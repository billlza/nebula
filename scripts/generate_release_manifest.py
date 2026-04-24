#!/usr/bin/env python3
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path

from release_lib import (
    SUPPORTED_TARGETS,
    archive_name,
    backend_sdk_archive_name,
    github_release_base_url,
    read_release_repository,
    read_repo_version,
    repo_root_from,
    sha256_file,
    utc_now_iso,
    write_json,
)


def _render_manifest(payload: dict) -> str:
    return json.dumps(payload, indent=2, ensure_ascii=True) + "\n"


def _canonical_manifest_sha(payload: dict) -> str:
    clone = copy.deepcopy(payload)
    clone.setdefault("metadata", {}).setdefault("manifest", {})["sha256"] = ""
    text = _render_manifest(clone).encode("utf-8")
    return hashlib.sha256(text).hexdigest()


def _manifest_sha_placeholder() -> str:
    return "0" * 64


def _stabilize_manifest_size(payload: dict) -> None:
    size = -1
    while True:
        rendered = _render_manifest(payload)
        next_size = len(rendered.encode("utf-8"))
        if next_size == size:
            payload["metadata"]["manifest"]["size"] = next_size
            return
        payload["metadata"]["manifest"]["size"] = next_size
        size = next_size


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

    backend_sdks = []
    backend_sdk_name = backend_sdk_archive_name(version)
    backend_sdk_path = artifact_dir / backend_sdk_name
    if backend_sdk_path.exists():
        backend_sdks.append(
            {
                "name": backend_sdk_name,
                "target": "linux-x86_64",
                "platform": "linux",
                "arch": "x86_64",
                "kind": "backend-sdk",
                "archive": "tar.gz",
                "size": backend_sdk_path.stat().st_size,
                "sha256": sha256_file(backend_sdk_path),
                "url": f"{base_url.rstrip('/')}/{backend_sdk_name}",
            }
        )
    elif args.require_all == "on":
        missing.append(backend_sdk_name)

    if args.require_all == "on" and missing:
        raise SystemExit("missing release artifacts: " + ", ".join(sorted(missing)))

    sboms = []
    for item in [*assets, *backend_sdks]:
        sbom_name = item["name"].replace(".tar.gz", ".spdx.json").replace(".zip", ".spdx.json")
        sbom_path = artifact_dir / sbom_name
        if not sbom_path.exists():
            continue
        sboms.append(
            {
                "name": sbom_name,
                "subject": item["name"],
                "size": sbom_path.stat().st_size,
                "sha256": sha256_file(sbom_path),
                "url": f"{base_url.rstrip('/')}/{sbom_name}",
            }
        )

    attestations = []
    for path in sorted(artifact_dir.glob("*.intoto.jsonl")):
        attestations.append(
            {
                "name": path.name,
                "size": path.stat().st_size,
                "sha256": sha256_file(path),
                "url": f"{base_url.rstrip('/')}/{path.name}",
            }
        )

    notes_name = f"RELEASE_NOTES_v{version}.md"
    metadata = {
        "checksums": {
            "name": checksums_out.name,
            "url": f"{base_url.rstrip('/')}/{checksums_out.name}",
            "size": 0,
            "sha256": "",
        },
        "manifest": {
            "name": manifest_out.name,
            "url": f"{base_url.rstrip('/')}/{manifest_out.name}",
            "size": 0,
            "sha256": "",
        },
    }
    formula_path = artifact_dir / "nebula.rb"
    if formula_path.exists():
        metadata["homebrew_formula"] = {
            "name": "nebula.rb",
            "size": formula_path.stat().st_size,
            "sha256": sha256_file(formula_path),
            "url": f"{base_url.rstrip('/')}/nebula.rb",
        }
    notes_path = artifact_dir / notes_name
    if notes_path.exists():
        metadata["release_notes"] = {
            "name": notes_name,
            "size": notes_path.stat().st_size,
            "sha256": sha256_file(notes_path),
            "url": f"{base_url.rstrip('/')}/{notes_name}",
        }

    checksum_rows = []
    for item in assets:
        checksum_rows.append((item["name"], item["sha256"]))
    for item in backend_sdks:
        checksum_rows.append((item["name"], item["sha256"]))
    for item in sboms:
        checksum_rows.append((item["name"], item["sha256"]))
    if formula_path.exists():
        checksum_rows.append(("nebula.rb", sha256_file(formula_path)))
    if notes_path.exists():
        checksum_rows.append((notes_name, sha256_file(notes_path)))

    checksum_rows.sort(key=lambda item: item[0])
    checksums_out.write_text(
        "".join(f"{digest}  {name}\n" for name, digest in checksum_rows),
        encoding="utf-8",
    )

    metadata["checksums"]["size"] = checksums_out.stat().st_size
    metadata["checksums"]["sha256"] = sha256_file(checksums_out)

    payload = {
        "version": version,
        "repository": repository,
        "generated_at_utc": utc_now_iso(),
        "artifacts": assets,
        "backend_sdks": backend_sdks,
        "sboms": sboms,
        "attestations": attestations,
        "metadata": metadata,
        "checksums": checksums_out.name,
    }
    payload["metadata"]["manifest"]["sha256"] = _manifest_sha_placeholder()
    _stabilize_manifest_size(payload)
    payload["metadata"]["manifest"]["sha256"] = _canonical_manifest_sha(payload)
    manifest_out.write_text(_render_manifest(payload), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
