#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import tarfile
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from release_lib import (
    SUPPORTED_TARGETS,
    archive_name,
    backend_sdk_archive_name,
    backend_sdk_stage_name,
    read_release_repository,
    read_repo_version,
    release_notes_name,
    repo_root_from,
    sha256_file,
    staging_dir_name,
)


@dataclass
class CheckResult:
    name: str
    ok: bool
    detail: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate a Nebula release bundle and render sign-off artifacts")
    parser.add_argument("--artifact-dir", required=True, help="directory containing the assembled release bundle")
    parser.add_argument("--version", default="", help="override VERSION from the repo")
    parser.add_argument("--repo", default="", help="override release repository owner/name")
    parser.add_argument("--verify-attestations", action="store_true", help="run gh attestation verify against bundled attestations")
    parser.add_argument("--json-out", default="", help="optional JSON summary output path")
    parser.add_argument("--markdown-out", default="", help="optional Markdown summary output path")
    return parser.parse_args()


def _render_manifest(payload: dict[str, Any]) -> str:
    return json.dumps(payload, indent=2, ensure_ascii=True) + "\n"


def _canonical_manifest_sha(payload: dict[str, Any]) -> str:
    clone = json.loads(json.dumps(payload))
    clone.setdefault("metadata", {}).setdefault("manifest", {})["sha256"] = ""
    return sha256_text(_render_manifest(clone))


def sha256_text(text: str) -> str:
    import hashlib

    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _run_git(repo_root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo_root), *args], text=True).strip()


def _collect_git_state(repo_root: Path) -> dict[str, Any]:
    if not (repo_root / ".git").exists():
        return {"available": False}
    try:
        head = _run_git(repo_root, "rev-parse", "HEAD")
        branch = _run_git(repo_root, "rev-parse", "--abbrev-ref", "HEAD")
        dirty = bool(_run_git(repo_root, "status", "--short"))
        return {"available": True, "head": head, "branch": branch, "dirty": dirty}
    except Exception as exc:  # pragma: no cover - defensive
        return {"available": False, "error": str(exc)}


def _archive_members(path: Path) -> list[str]:
    if path.suffix == ".zip":
        with zipfile.ZipFile(path) as zf:
            return zf.namelist()
    with tarfile.open(path, "r:*") as tf:
        return tf.getnames()


def _expected_release_files(version: str) -> dict[str, Path]:
    expected: dict[str, Path] = {}
    for target_name, target in SUPPORTED_TARGETS.items():
        expected[f"archive:{target_name}"] = Path(archive_name(version, target))
        expected[f"sbom:{target_name}"] = Path(f"nebula-v{version}-{target_name}.spdx.json")
        expected[f"provenance:{target_name}"] = Path(f"nebula-v{version}-{target_name}.provenance.intoto.jsonl")
        expected[f"sbom-bundle:{target_name}"] = Path(f"nebula-v{version}-{target_name}.sbom.intoto.jsonl")
    expected["archive:backend-sdk"] = Path(backend_sdk_archive_name(version))
    expected["sbom:backend-sdk"] = Path(backend_sdk_archive_name(version).replace(".tar.gz", ".spdx.json"))
    expected["provenance:backend-sdk"] = Path(
        backend_sdk_archive_name(version).replace(".tar.gz", ".provenance.intoto.jsonl")
    )
    expected["sbom-bundle:backend-sdk"] = Path(
        backend_sdk_archive_name(version).replace(".tar.gz", ".sbom.intoto.jsonl")
    )
    expected["checksums"] = Path("SHA256SUMS.txt")
    expected["checksums-bundle"] = Path("SHA256SUMS.txt.intoto.jsonl")
    expected["manifest"] = Path("release-manifest.json")
    expected["homebrew"] = Path("nebula.rb")
    expected["notes"] = Path(release_notes_name(version))
    return expected


def _verify_required_files(artifact_dir: Path, version: str) -> list[CheckResult]:
    results: list[CheckResult] = []
    for label, rel in sorted(_expected_release_files(version).items()):
        path = artifact_dir / rel
        if path.is_file():
            results.append(CheckResult(f"required-file:{label}", True, rel.as_posix()))
        else:
            results.append(CheckResult(f"required-file:{label}", False, f"missing {rel.as_posix()}"))
    return results


def _verify_archive_layout(artifact_dir: Path, version: str) -> list[CheckResult]:
    results: list[CheckResult] = []
    for target_name, target in SUPPORTED_TARGETS.items():
        archive_path = artifact_dir / archive_name(version, target)
        root = staging_dir_name(version, target)
        if not archive_path.is_file():
            results.append(CheckResult(f"archive-layout:{target_name}", False, "archive missing"))
            continue
        try:
            members = _archive_members(archive_path)
        except Exception as exc:
            results.append(CheckResult(f"archive-layout:{target_name}", False, str(exc)))
            continue
        offenders = [name for name in members if "official" in PurePosixPath(name).parts]
        if offenders:
            results.append(
                CheckResult(
                    f"archive-layout:{target_name}",
                    False,
                    "archive unexpectedly contains preview packages under official/",
                )
            )
            continue
        required = [
            f"{root}/bin/{target.binary_name}",
            f"{root}/include/runtime/nebula_runtime.hpp",
            f"{root}/include/runtime/region_allocator.hpp",
            f"{root}/share/nebula/std/task.nb",
            f"{root}/share/nebula/std/fs.nb",
            f"{root}/share/nebula/registry/client.py",
            f"{root}/share/nebula/registry/server.py",
            f"{root}/share/nebula/registry/README.md",
            f"{root}/share/doc/nebula/VERSION",
            f"{root}/share/doc/nebula/README.md",
            f"{root}/share/doc/nebula/{release_notes_name(version)}",
        ]
        missing = [item for item in required if item not in members]
        if missing:
            results.append(
                CheckResult(
                    f"archive-layout:{target_name}",
                    False,
                    "archive missing required members: " + ", ".join(missing),
                )
            )
            continue
        results.append(CheckResult(f"archive-layout:{target_name}", True, archive_path.name))

    backend_archive = artifact_dir / backend_sdk_archive_name(version)
    backend_root = backend_sdk_stage_name(version)
    if not backend_archive.is_file():
        results.append(CheckResult("archive-layout:backend-sdk", False, "archive missing"))
        return results
    try:
        members = _archive_members(backend_archive)
    except Exception as exc:
        results.append(CheckResult("archive-layout:backend-sdk", False, str(exc)))
        return results
    required_backend = [
        f"{backend_root}/share/nebula/sdk/backend/nebula-service/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/nebula-observe/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/nebula-auth/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/nebula-config/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/nebula-db-sqlite/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/nebula-observe/prometheus_bridge.py",
        f"{backend_root}/share/nebula/sdk/backend/examples/hello_api/nebula.toml",
        f"{backend_root}/share/nebula/sdk/backend/docs/service_profile.md",
        f"{backend_root}/share/nebula/sdk/backend/docs/reverse_proxy_deployment.md",
        f"{backend_root}/share/nebula/sdk/backend/docs/backend_operator_guide.md",
    ]
    missing_backend = [item for item in required_backend if item not in members]
    if missing_backend:
        results.append(
            CheckResult(
                "archive-layout:backend-sdk",
                False,
                "archive missing required members: " + ", ".join(missing_backend),
            )
        )
    else:
        results.append(CheckResult("archive-layout:backend-sdk", True, backend_archive.name))
    return results


def _verify_formula(artifact_dir: Path, version: str) -> list[CheckResult]:
    formula = artifact_dir / "nebula.rb"
    if not formula.is_file():
        return [CheckResult("homebrew-formula", False, "nebula.rb missing")]
    text = formula.read_text(encoding="utf-8")
    checks: list[CheckResult] = []
    if f'version "{version}"' in text:
        checks.append(CheckResult("homebrew-formula:version", True, version))
    else:
        checks.append(CheckResult("homebrew-formula:version", False, "version string missing from formula"))
    for target_name in ["darwin-x86_64", "darwin-arm64", "linux-x86_64"]:
        target = SUPPORTED_TARGETS[target_name]
        archive = archive_name(version, target)
        digest = sha256_file(artifact_dir / archive)
        ok = archive in text and digest in text
        checks.append(
            CheckResult(
                f"homebrew-formula:{target_name}",
                ok,
                archive if ok else f"formula does not reference {archive} with matching sha256",
            )
        )
    return checks


def _verify_manifest(artifact_dir: Path, version: str, repository: str) -> list[CheckResult]:
    manifest_path = artifact_dir / "release-manifest.json"
    if not manifest_path.is_file():
        return [CheckResult("release-manifest", False, "release-manifest.json missing")]
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:
        return [CheckResult("release-manifest", False, f"invalid JSON: {exc}")]

    results: list[CheckResult] = []
    results.append(CheckResult("manifest:version", payload.get("version") == version, str(payload.get("version"))))
    results.append(
        CheckResult("manifest:repository", payload.get("repository") == repository, str(payload.get("repository")))
    )

    expected_archives = {
        archive_name(version, target) for target in SUPPORTED_TARGETS.values()
    }
    actual_archives = {item.get("name", "") for item in payload.get("artifacts", [])}
    results.append(
        CheckResult(
            "manifest:artifacts",
            actual_archives == expected_archives,
            ", ".join(sorted(actual_archives)),
        )
    )

    expected_backend_sdks = {backend_sdk_archive_name(version)}
    actual_backend_sdks = {item.get("name", "") for item in payload.get("backend_sdks", [])}
    results.append(
        CheckResult(
            "manifest:backend-sdks",
            actual_backend_sdks == expected_backend_sdks,
            ", ".join(sorted(actual_backend_sdks)),
        )
    )

    expected_sboms = {f"nebula-v{version}-{name}.spdx.json" for name in SUPPORTED_TARGETS}
    expected_sboms.add(backend_sdk_archive_name(version).replace(".tar.gz", ".spdx.json"))
    actual_sboms = {item.get("name", "") for item in payload.get("sboms", [])}
    results.append(CheckResult("manifest:sboms", actual_sboms == expected_sboms, ", ".join(sorted(actual_sboms))))

    expected_attestations = {"SHA256SUMS.txt.intoto.jsonl"}
    for name in SUPPORTED_TARGETS:
        expected_attestations.add(f"nebula-v{version}-{name}.provenance.intoto.jsonl")
        expected_attestations.add(f"nebula-v{version}-{name}.sbom.intoto.jsonl")
    expected_attestations.add(
        backend_sdk_archive_name(version).replace(".tar.gz", ".provenance.intoto.jsonl")
    )
    expected_attestations.add(
        backend_sdk_archive_name(version).replace(".tar.gz", ".sbom.intoto.jsonl")
    )
    actual_attestations = {item.get("name", "") for item in payload.get("attestations", [])}
    results.append(
        CheckResult(
            "manifest:attestations",
            actual_attestations == expected_attestations,
            ", ".join(sorted(actual_attestations)),
        )
    )

    for section_name in ["artifacts", "backend_sdks", "sboms", "attestations"]:
        for item in payload.get(section_name, []):
            name = item.get("name", "")
            path = artifact_dir / name
            if not path.is_file():
                results.append(CheckResult(f"manifest:file:{name}", False, "referenced file is missing"))
                continue
            size_ok = item.get("size") == path.stat().st_size
            sha_ok = item.get("sha256") == sha256_file(path)
            results.append(CheckResult(f"manifest:size:{name}", size_ok, str(item.get("size"))))
            results.append(CheckResult(f"manifest:sha256:{name}", sha_ok, str(item.get("sha256"))))

    metadata = payload.get("metadata", {})
    checksums_meta = metadata.get("checksums", {})
    manifest_meta = metadata.get("manifest", {})
    checksums_path = artifact_dir / "SHA256SUMS.txt"
    results.append(
        CheckResult(
            "manifest:checksums-meta",
            checksums_meta.get("size") == checksums_path.stat().st_size
            and checksums_meta.get("sha256") == sha256_file(checksums_path),
            checksums_path.name,
        )
    )
    actual_manifest_size = manifest_path.stat().st_size
    actual_manifest_sha = _canonical_manifest_sha(payload)
    results.append(
        CheckResult(
            "manifest:self-size",
            manifest_meta.get("size") == actual_manifest_size,
            str(manifest_meta.get("size")),
        )
    )
    results.append(
        CheckResult(
            "manifest:self-sha",
            manifest_meta.get("sha256") == actual_manifest_sha,
            str(manifest_meta.get("sha256")),
        )
    )
    return results


def _verify_attestations(artifact_dir: Path, version: str, repository: str) -> list[CheckResult]:
    gh = shutil.which("gh")
    if gh is None:
        return [CheckResult("attestations:gh", False, "gh is not on PATH")]
    signer = f"{repository}/.github/workflows/release.yml"
    checks: list[CheckResult] = []

    def run_verify(label: str, subject: Path, bundle: Path, predicate_type: str = "") -> None:
        cmd = [gh, "attestation", "verify", str(subject), "--repo", repository, "--signer-workflow", signer, "--bundle", str(bundle)]
        if predicate_type:
            cmd.extend(["--predicate-type", predicate_type])
        proc = subprocess.run(cmd, capture_output=True, text=True)
        detail = proc.stdout.strip() or proc.stderr.strip() or "ok"
        checks.append(CheckResult(label, proc.returncode == 0, detail))

    for target_name, target in SUPPORTED_TARGETS.items():
        archive = artifact_dir / archive_name(version, target)
        provenance = artifact_dir / f"nebula-v{version}-{target_name}.provenance.intoto.jsonl"
        sbom_bundle = artifact_dir / f"nebula-v{version}-{target_name}.sbom.intoto.jsonl"
        run_verify(f"attestations:provenance:{target_name}", archive, provenance)
        run_verify(
            f"attestations:sbom:{target_name}",
            archive,
            sbom_bundle,
            "https://spdx.dev/Document/v2.3",
        )
    backend_archive = artifact_dir / backend_sdk_archive_name(version)
    run_verify(
        "attestations:provenance:backend-sdk",
        backend_archive,
        artifact_dir / backend_sdk_archive_name(version).replace(".tar.gz", ".provenance.intoto.jsonl"),
    )
    run_verify(
        "attestations:sbom:backend-sdk",
        backend_archive,
        artifact_dir / backend_sdk_archive_name(version).replace(".tar.gz", ".sbom.intoto.jsonl"),
        "https://spdx.dev/Document/v2.3",
    )
    run_verify(
        "attestations:checksums",
        artifact_dir / "SHA256SUMS.txt",
        artifact_dir / "SHA256SUMS.txt.intoto.jsonl",
    )
    return checks


def _render_markdown(
    version: str,
    repository: str,
    artifact_dir: Path,
    checks: list[CheckResult],
    git_state: dict[str, Any],
) -> str:
    passed = [item for item in checks if item.ok]
    failed = [item for item in checks if not item.ok]
    head = git_state.get("head", "<rc-commit>")
    branch = git_state.get("branch", "<current-branch>")
    return "\n".join(
        [
            f"# Nebula v{version} Release Sign-Off",
            "",
            f"- repository: `{repository}`",
            f"- artifact dir: `{artifact_dir}`",
            f"- passed checks: `{len(passed)}`",
            f"- failed checks: `{len(failed)}`",
            "",
            "## Local Verification Summary",
            "",
            *[f"- [x] `{item.name}`: {item.detail}" for item in passed],
            *[f"- [ ] `{item.name}`: {item.detail}" for item in failed],
            "",
            "## Candidate Git State",
            "",
            f"- current branch: `{branch}`",
            f"- current head: `{head}`",
            f"- worktree dirty: `{git_state.get('dirty', 'unknown')}`",
            "",
            "## GitHub-Only Follow-Up",
            "",
            "Run these from a clean RC checkout once you have chosen the exact candidate commit:",
            "",
            "```bash",
            f"git switch -c release/1.0 {head}",
            "git push -u origin release/1.0",
            "gh workflow run contract-tests.yml --ref release/1.0",
            "gh workflow run release.yml --ref release/1.0",
            "gh run list --workflow release.yml --branch release/1.0 --event workflow_dispatch --limit 1",
            "gh run download <run-id> --name release-bundle --dir work/release-bundle",
            "python3 scripts/release_signoff.py --artifact-dir work/release-bundle --verify-attestations \\",
            "  --markdown-out work/release-signoff.md --json-out work/release-signoff.json",
            f"git tag v{version} {head}",
            f"git push origin v{version}",
            "```",
            "",
        ]
    ) + "\n"


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from(Path(__file__))
    version = args.version or read_repo_version(repo_root)
    repository = args.repo or read_release_repository(repo_root)
    artifact_dir = Path(args.artifact_dir).resolve()
    if not artifact_dir.is_dir():
        raise SystemExit(f"artifact dir not found: {artifact_dir}")

    checks: list[CheckResult] = []
    checks.extend(_verify_required_files(artifact_dir, version))
    checks.extend(_verify_archive_layout(artifact_dir, version))
    checks.extend(_verify_formula(artifact_dir, version))
    checks.extend(_verify_manifest(artifact_dir, version, repository))
    if args.verify_attestations:
        checks.extend(_verify_attestations(artifact_dir, version, repository))

    git_state = _collect_git_state(repo_root)
    payload = {
        "version": version,
        "repository": repository,
        "artifact_dir": str(artifact_dir),
        "all_passed": all(item.ok for item in checks),
        "checks": [asdict(item) for item in checks],
        "git": git_state,
    }
    markdown = _render_markdown(version, repository, artifact_dir, checks, git_state)

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(payload, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    if args.markdown_out:
        Path(args.markdown_out).write_text(markdown, encoding="utf-8")

    print(markdown, end="")
    return 0 if payload["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
