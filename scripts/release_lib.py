from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ReleaseTarget:
    platform: str
    arch: str
    archive_kind: str

    @property
    def target(self) -> str:
        return f"{self.platform}-{self.arch}"

    @property
    def binary_name(self) -> str:
        return "nebula.exe" if self.platform == "windows" else "nebula"

    @property
    def archive_name_suffix(self) -> str:
        return self.target

    @property
    def archive_extension(self) -> str:
        return ".zip" if self.archive_kind == "zip" else ".tar.gz"


SUPPORTED_TARGETS = {
    "darwin-x86_64": ReleaseTarget(platform="darwin", arch="x86_64", archive_kind="tar.gz"),
    "darwin-arm64": ReleaseTarget(platform="darwin", arch="arm64", archive_kind="tar.gz"),
    "linux-x86_64": ReleaseTarget(platform="linux", arch="x86_64", archive_kind="tar.gz"),
    "windows-x86_64": ReleaseTarget(platform="windows", arch="x86_64", archive_kind="zip"),
}


def repo_root_from(path: Path) -> Path:
    return path.resolve().parents[1]


def read_repo_version(repo_root: Path) -> str:
    return (repo_root / "VERSION").read_text(encoding="utf-8").strip()


def read_release_repository(repo_root: Path) -> str:
    repo_file = repo_root / "RELEASE_REPOSITORY"
    if repo_file.exists():
        return repo_file.read_text(encoding="utf-8").strip()
    return "billlza/nebula"


def release_notes_name(version: str) -> str:
    return f"RELEASE_NOTES_v{version}.md"


def release_notes_path(repo_root: Path, version: str) -> Path:
    return repo_root / release_notes_name(version)


def install_doc_sources(repo_root: Path, version: str) -> list[Path]:
    docs = [
        repo_root / "LICENSE",
        repo_root / "VERSION",
        repo_root / "README.md",
        repo_root / "CHANGELOG.md",
        repo_root / "RELEASE_PROCESS.md",
    ]
    notes = release_notes_path(repo_root, version)
    if notes.exists():
        docs.append(notes)
    return docs


def github_release_base_url(repository: str, version: str) -> str:
    return f"https://github.com/{repository}/releases/download/v{version}"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def staging_dir_name(version: str, target: ReleaseTarget) -> str:
    return f"nebula-v{version}-{target.archive_name_suffix}"


def archive_name(version: str, target: ReleaseTarget) -> str:
    return staging_dir_name(version, target) + target.archive_extension


def ensure_supported_target(target_name: str) -> ReleaseTarget:
    if target_name not in SUPPORTED_TARGETS:
        raise SystemExit(f"unsupported release target: {target_name}")
    return SUPPORTED_TARGETS[target_name]


def write_json(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def find_cmake_build_dir(binary: Path) -> Path:
    for candidate in [binary.parent, *binary.parents]:
        if (candidate / "CMakeCache.txt").exists():
            return candidate
    raise SystemExit(f"could not locate CMake build directory for binary: {binary}")


def _run_checked(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, check=True, capture_output=True, text=True)


def _normalize_windows_dependency_path(raw_path: str) -> Path:
    raw = raw_path.strip()
    if os.name == "nt" and raw.startswith("/"):
        cygpath = shutil.which("cygpath")
        if cygpath is not None:
            proc = _run_checked([cygpath, "-w", raw])
            raw = proc.stdout.strip()
    return Path(raw)


def _iter_windows_runtime_deps(binary: Path) -> Iterable[Path]:
    ldd = shutil.which("ldd")
    if ldd is None:
        raise SystemExit("windows release packaging requires `ldd` to discover runtime DLLs")
    proc = subprocess.run([ldd, str(binary)], check=True, capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        text = line.strip()
        if "=>" not in text:
            continue
        name, rhs = text.split("=>", 1)
        location = rhs.split("(", 1)[0].strip()
        if not location:
            continue
        if location.lower() == "not found":
            raise SystemExit(f"missing runtime dependency for {binary.name}: {name.strip()}")
        dep = _normalize_windows_dependency_path(location)
        if dep.suffix.lower() != ".dll":
            continue
        yield dep


def bundle_windows_runtime_deps(binary: Path, dest_dir: Path) -> list[Path]:
    if os.name != "nt":
        return []
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows")).resolve()
    copied: list[Path] = []
    seen: set[Path] = set()
    for dep in _iter_windows_runtime_deps(binary):
        resolved = dep.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)
        dep_lower = str(resolved).lower()
        if dep_lower.startswith(str(system_root).lower()):
            continue
        if not resolved.exists():
            raise SystemExit(f"runtime dependency not found on disk: {resolved}")
        target = dest_dir / resolved.name
        shutil.copy2(resolved, target)
        copied.append(target)
    return copied
