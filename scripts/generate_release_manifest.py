#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from release_lib import (
    SUPPORTED_TARGETS,
    archive_name,
    github_release_base_url,
    read_release_repository,
    read_repo_version,
    repo_root_from,
    sha256_file,
    utc_now_iso,
    write_json,
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate Nebula release checksums and manifest")
    p.add_argument("--artifact-dir", required=True)
    p.add_argument("--checksums-out", required=True)
    p.add_argument("--manifest-out", required=True)
    p.add_argument("--base-url", default="")
    p.add_argument("--require-all", choices=["on", "off"], default="off")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from(Path(__file__))
    version = read_repo_version(repo_root)
    repository = read_release_repository(repo_root)
    artifact_dir = Path(args.artifact_dir).resolve()
    checksums_out = Path(args.checksums_out).resolve()
    manifest_out = Path(args.manifest_out).resolve()
    base_url = args.base_url or github_release_base_url(repository, version)

    assets = []
    missing = []
    for target_name, target in SUPPORTED_TARGETS.items():
        name = archive_name(version, target)
        path = artifact_dir / name
        if not path.exists():
            missing.append(name)
            continue
        assets.append(
            {
                "name": name,
                "target": target_name,
                "platform": target.platform,
                "arch": target.arch,
                "archive": target.archive_kind,
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
                "url": f"{base_url.rstrip('/')}/{name}",
            }
        )

    if args.require_all == "on" and missing:
        raise SystemExit("missing release artifacts: " + ", ".join(sorted(missing)))

    extra_names = ["nebula.rb", f"RELEASE_NOTES_v{version}.md"]
    checksum_rows = []
    for item in assets:
        checksum_rows.append((item["name"], item["sha256"]))
    for name in extra_names:
        path = artifact_dir / name
        if path.exists():
            checksum_rows.append((name, sha256_file(path)))

    checksum_rows.sort(key=lambda item: item[0])
    checksums_out.write_text(
        "".join(f"{digest}  {name}\n" for name, digest in checksum_rows),
        encoding="utf-8",
    )

    write_json(
        manifest_out,
        {
            "version": version,
            "repository": repository,
            "generated_at_utc": utc_now_iso(),
            "artifacts": assets,
            "checksums": checksums_out.name,
        },
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
