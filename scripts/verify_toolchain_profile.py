#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path

from release_lib import release_doc_install_relpaths


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def fail(message: str) -> None:
    raise SystemExit(message)


def run_command(cmd: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)


def assert_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label} failed with rc={result.returncode}\nSTDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}")


def host_target() -> str:
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Darwin" and machine in ("arm64", "aarch64"):
        return "darwin-arm64"
    if system == "Darwin" and machine == "x86_64":
        return "darwin-x86_64"
    if system == "Linux" and machine == "x86_64":
        return "linux-x86_64"
    if system == "Windows" or system.startswith("MSYS") or system.startswith("MINGW"):
        return "windows-x86_64"
    fail(f"unsupported host for toolchain profile verification: {system}/{machine}")


def assert_doc_contract(root: Path) -> None:
    text = (root / "docs" / "toolchain_profile.md").read_text(encoding="utf-8")
    required = [
        "Core Profile",
        "bin/nebula",
        "share/nebula/std",
        "include/runtime",
        "share/nebula/registry",
        "share/doc/nebula",
        "docs/universeos",
        "spec",
        "rfcs",
        "External Prerequisites",
        "clang++",
        "Python 3.11+",
        "git-backed dependencies",
        "Lint And Check Surface",
        "nebula check",
        "Optional Backend SDK Profile",
        "installed-preview",
        "Target And Preview Boundaries",
        "does not currently download per-target standard libraries",
        "There is no `nebula toolchain update stable` command",
    ]
    missing = [item for item in required if item not in text]
    if missing:
        fail(f"toolchain profile doc missing markers: {missing!r}")


def package_release(root: Path, binary: Path, out_root: Path) -> Path:
    shutil.rmtree(out_root, ignore_errors=True)
    out_root.mkdir(parents=True, exist_ok=True)
    build = run_command(["cmake", "--build", str(binary.parent), "-j2"], cwd=root)
    assert_ok(build, "toolchain profile build refresh")
    target = host_target()
    result = run_command(
        [
            "python3",
            str(root / "scripts" / "package_release.py"),
            "--binary",
            str(binary),
            "--target",
            target,
            "--output-dir",
            str(out_root),
        ],
        cwd=root,
    )
    assert_ok(result, "toolchain profile package release")
    archives = sorted(out_root.glob("nebula-v*.zip" if target == "windows-x86_64" else "nebula-v*.tar.gz"))
    if len(archives) != 1:
        fail(f"expected one release archive, found: {[item.name for item in archives]}")
    return archives[0]


def tar_members(path: Path) -> set[str]:
    with tarfile.open(path, "r:gz") as tf:
        return {item.name for item in tf.getmembers()}


def zip_members(path: Path) -> set[str]:
    with zipfile.ZipFile(path) as zf:
        return set(zf.namelist())


def find_member(members: set[str], suffix: str) -> bool:
    suffix = suffix.replace(os.sep, "/")
    return any(item.endswith(suffix) for item in members)


def assert_archive_contract(archive: Path) -> None:
    members = zip_members(archive) if archive.suffix == ".zip" else tar_members(archive)
    required_suffixes = [
        "bin/nebula.exe" if archive.suffix == ".zip" else "bin/nebula",
        "include/runtime/nebula_runtime.hpp",
        "include/runtime/region_allocator.hpp",
        "share/nebula/std/fs.nb",
        "share/nebula/std/json.nb",
        "share/nebula/registry/client.py",
        "share/nebula/registry/README.md",
        "share/doc/nebula/toolchain_profile.md",
        "share/doc/nebula/support_matrix.md",
        "share/doc/nebula/install_lifecycle.md",
    ]
    required_suffixes.extend(release_doc_install_relpaths(repo_root()))
    missing = [suffix for suffix in required_suffixes if not find_member(members, suffix)]
    if missing:
        fail(f"release archive missing toolchain profile members: {missing!r}")
    preview_offenders = [
        item for item in members
        if "/official/" in item or item.endswith("/official") or item.startswith("official/")
    ]
    if preview_offenders:
        fail(f"core release archive leaked repo-local preview packages: {preview_offenders[:10]!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify the Nebula installable toolchain profile contract")
    parser.add_argument("--binary", default=str(repo_root() / "build" / "nebula"))
    parser.add_argument("--out-root", default=str(repo_root() / "work" / "toolchain_profile"))
    args = parser.parse_args()

    root = repo_root()
    binary = Path(args.binary).resolve()
    if not binary.exists():
        fail(f"nebula binary missing: {binary}")
    assert_doc_contract(root)
    archive = package_release(root, binary, Path(args.out_root).resolve())
    assert_archive_contract(archive)
    print(json.dumps({
        "schema": "nebula.toolchain-profile.verify.v1",
        "status": "ok",
        "target": host_target(),
        "archive": str(archive),
    }, sort_keys=True))
    print("nebula-toolchain-profile-ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
