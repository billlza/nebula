#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from release_lib import (
    github_release_base_url,
    read_release_repository,
    read_repo_version,
    repo_root_from,
    sha256_file,
    utc_now_iso,
    write_json,
)


PACKAGE_SPDX_ID = "SPDXRef-Package-NebulaRelease"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate an SPDX SBOM for a staged Nebula release bundle")
    p.add_argument("--staged-root", required=True, help="installed release staging directory")
    p.add_argument("--artifact", required=True, help="archive produced from the staged directory")
    p.add_argument("--output", required=True, help="SPDX JSON output path")
    p.add_argument("--base-url", default="", help="release asset base URL")
    return p.parse_args()


def _spdx_file_id(index: int) -> str:
    return f"SPDXRef-File-{index:04d}"


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from(Path(__file__))
    version = read_repo_version(repo_root)
    repository = read_release_repository(repo_root)
    staged_root = Path(args.staged_root).resolve()
    artifact = Path(args.artifact).resolve()
    output = Path(args.output).resolve()

    if not staged_root.is_dir():
        raise SystemExit(f"staged release root not found: {staged_root}")
    if not artifact.is_file():
        raise SystemExit(f"release archive not found: {artifact}")

    base_url = args.base_url or github_release_base_url(repository, version)
    files = []
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": PACKAGE_SPDX_ID,
        }
    ]

    staged_files = sorted(path for path in staged_root.rglob("*") if path.is_file())
    for idx, path in enumerate(staged_files, start=1):
        rel = path.relative_to(staged_root).as_posix()
        spdx_id = _spdx_file_id(idx)
        files.append(
            {
                "fileName": f"./{rel}",
                "SPDXID": spdx_id,
                "checksums": [
                    {
                        "algorithm": "SHA256",
                        "checksumValue": sha256_file(path),
                    }
                ],
                "licenseConcluded": "NOASSERTION",
                "copyrightText": "NOASSERTION",
            }
        )
        relationships.append(
            {
                "spdxElementId": PACKAGE_SPDX_ID,
                "relationshipType": "CONTAINS",
                "relatedSpdxElement": spdx_id,
            }
        )

    payload = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"{artifact.name} SBOM",
        "documentNamespace": f"{base_url.rstrip('/')}/{output.name}",
        "creationInfo": {
            "created": utc_now_iso(),
            "creators": ["Tool: scripts/generate_release_sbom.py"],
        },
        "documentDescribes": [PACKAGE_SPDX_ID],
        "packages": [
            {
                "name": staged_root.name,
                "SPDXID": PACKAGE_SPDX_ID,
                "versionInfo": version,
                "downloadLocation": f"{base_url.rstrip('/')}/{artifact.name}",
                "filesAnalyzed": False,
                "licenseConcluded": "NOASSERTION",
                "licenseDeclared": "Apache-2.0",
                "copyrightText": "NOASSERTION",
                "summary": f"Nebula staged release bundle for {artifact.name}",
                "description": (
                    "Deterministic SPDX inventory for the staged Nebula release tree that is "
                    f"packaged into {artifact.name}."
                ),
            }
        ],
        "files": files,
        "relationships": relationships,
    }
    write_json(output, payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
