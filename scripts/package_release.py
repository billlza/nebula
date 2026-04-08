#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import stat
import tarfile
import zipfile
from pathlib import Path

from release_lib import archive_name, ensure_supported_target, read_repo_version, repo_root_from, staging_dir_name


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Package a Nebula release asset")
    p.add_argument("--binary", required=True, help="path to built nebula binary")
    p.add_argument("--target", required=True, help="release target, e.g. darwin-arm64")
    p.add_argument("--output-dir", required=True, help="directory for packaged artifacts")
    return p.parse_args()


def make_executable(path: Path) -> None:
    mode = path.stat().st_mode
    path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def main() -> int:
    args = parse_args()
    script_path = Path(__file__)
    repo_root = repo_root_from(script_path)
    version = read_repo_version(repo_root)
    target = ensure_supported_target(args.target)
    binary = Path(args.binary).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    stage_root = output_dir / staging_dir_name(version, target)
    if stage_root.exists():
        shutil.rmtree(stage_root)
    (stage_root / "bin").mkdir(parents=True, exist_ok=True)

    staged_binary = stage_root / "bin" / target.binary_name
    shutil.copy2(binary, staged_binary)
    if target.platform != "windows":
        make_executable(staged_binary)

    for rel_path in ["LICENSE", "README.md", "CHANGELOG.md", "VERSION"]:
        shutil.copy2(repo_root / rel_path, stage_root / rel_path)

    archive_path = output_dir / archive_name(version, target)
    if archive_path.exists():
        archive_path.unlink()

    if target.archive_kind == "tar.gz":
        with tarfile.open(archive_path, "w:gz") as tf:
            tf.add(stage_root, arcname=stage_root.name)
    else:
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(stage_root.rglob("*")):
                if path.is_dir():
                    continue
                zf.write(path, arcname=str(path.relative_to(output_dir)))

    print(str(archive_path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
