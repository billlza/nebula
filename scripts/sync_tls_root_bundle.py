#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


SOURCE_ASSET_RE = re.compile(r"(^\s*- pinned source asset: `)([^`]+)(`)$", re.MULTILINE)
PINNED_SHA_RE = re.compile(r"(^\s*- pinned SHA256: `)([0-9a-f]{64})(`)$", re.MULTILINE)
MOZILLA_UPDATED_RE = re.compile(
    r"^## Certificate data from Mozilla last updated on: (.+)$", re.MULTILINE
)


def repo_root_from(script_path: Path) -> Path:
    return script_path.resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sync and validate the vendored TLS default root bundle metadata/header"
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--check",
        action="store_true",
        help="verify the checked-in header and metadata match the vendored bundle (default)",
    )
    mode.add_argument(
        "--write",
        action="store_true",
        help="rewrite the generated header and pinned SHA256 metadata from the vendored bundle",
    )
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def extract_required(pattern: re.Pattern[str], text: str, label: str) -> str:
    match = pattern.search(text)
    if match is None:
        raise SystemExit(f"missing {label}")
    return match.group(2 if pattern is not MOZILLA_UPDATED_RE else 1)


def render_header(bundle_text: str, source_asset: str, bundle_sha256: str) -> str:
    return (
        "#pragma once\n\n"
        "#include <cstddef>\n\n"
        "// Generated from native/vendor/cacert.pem.\n"
        f"// Source: {source_asset}\n"
        f"// SHA256: {bundle_sha256}\n\n"
        "namespace nebula::rt {\n"
        'inline constexpr char kNebulaTlsDefaultRootsPem[] = R"TLSROOTS('
        + bundle_text
        + ')TLSROOTS";\n'
        "inline constexpr std::size_t kNebulaTlsDefaultRootsPemLen = sizeof(kNebulaTlsDefaultRootsPem) - 1;\n"
        "} // namespace nebula::rt\n"
    )


def rewrite_pinned_sha256(third_party_text: str, bundle_sha256: str) -> str:
    return PINNED_SHA_RE.sub(rf"\1{bundle_sha256}\3", third_party_text, count=1)


def main() -> int:
    args = parse_args()
    script_path = Path(__file__)
    repo_root = repo_root_from(script_path)
    bundle_path = repo_root / "official" / "nebula-tls" / "native" / "vendor" / "cacert.pem"
    header_path = (
        repo_root
        / "official"
        / "nebula-tls"
        / "native"
        / "include"
        / "nebula_tls_default_roots.hpp"
    )
    third_party_path = repo_root / "official" / "nebula-tls" / "THIRD_PARTY.md"

    bundle_text = bundle_path.read_text(encoding="utf-8")
    header_text = header_path.read_text(encoding="utf-8")
    third_party_text = third_party_path.read_text(encoding="utf-8")

    source_asset = extract_required(SOURCE_ASSET_RE, third_party_text, "pinned source asset")
    pinned_sha = extract_required(PINNED_SHA_RE, third_party_text, "pinned SHA256")
    mozilla_updated = extract_required(
        MOZILLA_UPDATED_RE, bundle_text, "Mozilla last-updated marker in cacert.pem"
    )
    bundle_sha = sha256_file(bundle_path)
    expected_header = render_header(bundle_text, source_asset, bundle_sha)
    updated_third_party = rewrite_pinned_sha256(third_party_text, bundle_sha)

    if args.write:
        if updated_third_party != third_party_text:
            third_party_path.write_text(updated_third_party, encoding="utf-8")
        if header_text != expected_header:
            header_path.write_text(expected_header, encoding="utf-8")
        print(
            f"tls-root-bundle-synced source={source_asset} sha256={bundle_sha} "
            f"mozilla_updated={mozilla_updated}"
        )
        return 0

    problems: list[str] = []
    if pinned_sha != bundle_sha:
        problems.append(
            "THIRD_PARTY.md pinned SHA256 does not match official/nebula-tls/native/vendor/cacert.pem"
        )
    if header_text != expected_header:
        problems.append(
            "nebula_tls_default_roots.hpp is out of sync with official/nebula-tls/native/vendor/cacert.pem"
        )
    if problems:
        detail = "\n".join(f"- {problem}" for problem in problems)
        raise SystemExit(
            detail
            + "\nRun `python3 scripts/sync_tls_root_bundle.py --write` after refreshing the vendored bundle."
        )

    print(
        f"tls-root-bundle-ok source={source_asset} sha256={bundle_sha} "
        f"mozilla_updated={mozilla_updated}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
