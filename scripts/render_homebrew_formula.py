#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from release_lib import SUPPORTED_TARGETS, archive_name, read_release_repository, read_repo_version, repo_root_from, sha256_file


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render the Nebula Homebrew formula")
    p.add_argument("--artifact-dir", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--url-base", required=True)
    p.add_argument("--template", default="")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from(Path(__file__))
    version = read_repo_version(repo_root)
    repository = read_release_repository(repo_root)
    artifact_dir = Path(args.artifact_dir).resolve()
    template_path = (
        Path(args.template).resolve()
        if args.template
        else repo_root / "packaging" / "homebrew" / "nebula.rb.in"
    )
    output_path = Path(args.output).resolve()

    replacements = {
        "@VERSION@": version,
        "@REPOSITORY@": repository,
    }
    for key, target_name in [
        ("DARWIN_X86_64", "darwin-x86_64"),
        ("DARWIN_ARM64", "darwin-arm64"),
        ("LINUX_X86_64", "linux-x86_64"),
    ]:
        asset_name = archive_name(version, SUPPORTED_TARGETS[target_name])
        asset_path = artifact_dir / asset_name
        replacements[f"@{key}_URL@"] = f"{args.url_base.rstrip('/')}/{asset_name}"
        replacements[f"@{key}_SHA256@"] = sha256_file(asset_path)

    text = template_path.read_text(encoding="utf-8")
    for needle, value in replacements.items():
        text = text.replace(needle, value)
    output_path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
