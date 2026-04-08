from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


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
