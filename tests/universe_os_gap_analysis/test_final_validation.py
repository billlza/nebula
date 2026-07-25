"""Final published-artifact-set validation tests (Task 15.2).

These tests drive the real
:func:`tools.universe_os_gap_analysis.final_validation.validate_final_report`
(no mocks) against the *committed* deliverable at
``tools/universe_os_gap_analysis/artifacts/`` and against deliberately tampered
copies. They assert two things (Requirements 9.6, 9.7, 13.7, 14.1-14.7):

* (a) the real committed artifact set validates **clean** -- schema, table
  parity, markdown references, digests, revision binding, status/wording, trust
  assumptions, and all fifteen requirements coverage all pass; and
* (b) a tampered artifact -- a wrong digest, a dropped matrix row, corrupted
  JSON, or a markdown claim that expands a prerequisite gate into an OS claim --
  fails **closed** with the right finding code and requirement/object refs.

The committed deliverable is never mutated: every tamper case first copies the
whole artifact set into a throwaway temporary directory and mutates the copy.
"""

from __future__ import annotations

import json
import shutil
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.final_validation import (
    FV_ARTIFACT_CORRUPT,
    FV_DIGEST_MISMATCH,
    FV_MD_OS_CLAIM_EXPANSION,
    FV_MD_STATEMENT_MISSING,
    FV_REVISION_MISMATCH,
    FV_SCHEMA_VALIDATION,
    FV_TABLE_COUNT,
    FV_TABLE_MISSING_ROW,
    REQUIRED_ARTIFACTS,
    validate_final_report,
)
from tools.universe_os_gap_analysis.manifest import MANIFEST_ARTIFACT_NAME
from tools.universe_os_gap_analysis.json_renderer import ASSESSMENT_JSON_ARTIFACT_NAME
from tools.universe_os_gap_analysis.markdown_renderer import (
    ARTIFACT_NAME as MARKDOWN_ARTIFACT_NAME,
)
from tools.universe_os_gap_analysis.table_renderer import CAPABILITY_MATRIX_JSON


def _committed_artifacts_dir() -> Path:
    return (
        Path(__file__).resolve().parents[2]
        / "tools"
        / "universe_os_gap_analysis"
        / "artifacts"
    )


def _copy_artifacts(destination: Path) -> Path:
    """Copy the committed deliverable into ``destination`` and return the dir."""

    destination.mkdir(parents=True, exist_ok=True)
    source = _committed_artifacts_dir()
    for name in REQUIRED_ARTIFACTS:
        shutil.copy2(source / name, destination / name)
    return destination


def _codes(result) -> set[str]:
    return {finding.code for finding in result.findings}


class CommittedArtifactSetTests(unittest.TestCase):
    """(a) The real committed deliverable validates clean."""

    def test_committed_artifact_set_is_valid(self) -> None:
        result = validate_final_report(_committed_artifacts_dir())
        self.assertTrue(
            result.valid,
            msg=f"unexpected findings: {[(f.code, f.detail) for f in result.findings]}",
        )
        self.assertEqual(result.findings, ())

    def test_copy_of_committed_set_is_valid(self) -> None:
        # The tamper tests copy the set first; confirm an untouched copy is clean.
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            result = validate_final_report(directory)
            self.assertTrue(
                result.valid,
                msg=f"{[(f.code, f.detail) for f in result.findings]}",
            )


class TamperedArtifactSetTests(unittest.TestCase):
    """(b) Tampered artifacts fail closed with the correct finding + refs."""

    def test_wrong_digest_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Corrupt the manifest's recorded digest for one artifact, leaving
            # the on-disk bytes untouched -> digest mismatch (fail closed).
            manifest_path = directory / MANIFEST_ARTIFACT_NAME
            manifest = json.loads(manifest_path.read_text())
            target = manifest["artifacts"][0]["name"]
            manifest["artifacts"][0]["sha256"] = "0" * 64
            manifest_path.write_text(json.dumps(manifest))

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_DIGEST_MISMATCH, _codes(result))
            digest_finding = next(
                f for f in result.findings if f.code == FV_DIGEST_MISMATCH
            )
            self.assertIn(target, digest_finding.object_refs)
            self.assertIn("9.7", digest_finding.requirement_refs)

    def test_mutated_on_disk_bytes_fail_digest_check(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Flip real bytes without updating the manifest -> digest mismatch.
            md_path = directory / MARKDOWN_ARTIFACT_NAME
            md_path.write_bytes(md_path.read_bytes() + b"\n<!-- tampered -->\n")

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_DIGEST_MISMATCH, _codes(result))

    def test_dropped_matrix_row_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Drop one capability-matrix row and re-hash so the digest still
            # matches -> the missing-row parity check must catch it anyway.
            matrix_path = directory / CAPABILITY_MATRIX_JSON
            matrix = json.loads(matrix_path.read_text())
            dropped = matrix["rows"].pop()
            matrix_path.write_text(json.dumps(matrix, indent=2) + "\n")
            _rehash_manifest(directory)

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            codes = _codes(result)
            self.assertIn(FV_TABLE_MISSING_ROW, codes)
            self.assertIn(FV_TABLE_COUNT, codes)
            missing = next(
                f for f in result.findings if f.code == FV_TABLE_MISSING_ROW
            )
            self.assertIn(str(dropped["domainId"]), missing.object_refs)
            self.assertIn("14.2", missing.requirement_refs)

    def test_corrupted_json_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Write malformed JSON into assessment.json (re-hash so the digest
            # check is not what trips) -> parse failure fails closed.
            json_path = directory / ASSESSMENT_JSON_ARTIFACT_NAME
            json_path.write_bytes(b'{"assessment": {this is not valid json,,}')
            _rehash_manifest(directory)

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_ARTIFACT_CORRUPT, _codes(result))

    def test_markdown_gate_expanded_into_os_claim_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Inject a claim that expands a prerequisite gate into an OS claim.
            md_path = directory / MARKDOWN_ARTIFACT_NAME
            text = md_path.read_text()
            text += (
                "\n\nThe primitive ET_REL object gate proves a bootable kernel "
                "image and a freestanding runtime.\n"
            )
            md_path.write_text(text)
            _rehash_manifest(directory)

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_MD_OS_CLAIM_EXPANSION, _codes(result))
            finding = next(
                f for f in result.findings if f.code == FV_MD_OS_CLAIM_EXPANSION
            )
            self.assertIn("13.7", finding.requirement_refs)

    def test_missing_non_additive_statement_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            md_path = directory / MARKDOWN_ARTIFACT_NAME
            # Remove the non-additive / prerequisite-gate scope wording.
            text = md_path.read_text()
            text = text.replace("non-additive", "SUMMABLE")
            text = text.replace("passing prerequisite gate proves only", "gates prove everything")
            md_path.write_text(text)
            _rehash_manifest(directory)

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_MD_STATEMENT_MISSING, _codes(result))

    def test_revision_fingerprint_mismatch_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Mutate the manifest's bound worktree fingerprint so it no longer
            # matches assessment.json's embedded revision -> fail closed.
            manifest_path = directory / MANIFEST_ARTIFACT_NAME
            manifest = json.loads(manifest_path.read_text())
            manifest["revision"]["worktreeFingerprint"] = "f" * 64
            manifest_path.write_text(json.dumps(manifest))

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_REVISION_MISMATCH, _codes(result))

    def test_missing_artifact_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            (directory / CAPABILITY_MATRIX_JSON).unlink()

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            # Missing artifact is reported and no false "valid" decision is made.
            self.assertFalse(result.valid)

    def test_schema_violation_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            directory = _copy_artifacts(Path(tmp) / "artifacts")
            # Remove a schema-required top-level key from assessment.json.
            json_path = directory / ASSESSMENT_JSON_ARTIFACT_NAME
            document = json.loads(json_path.read_text())
            document.pop("referenceGraph", None)
            json_path.write_text(json.dumps(document))
            _rehash_manifest(directory)

            result = validate_final_report(directory)
            self.assertFalse(result.valid)
            self.assertIn(FV_SCHEMA_VALIDATION, _codes(result))


def _rehash_manifest(directory: Path) -> None:
    """Recompute the manifest digests for the current on-disk bytes.

    Used by tamper cases that intentionally target a *non-digest* failure mode
    (dropped row, corrupted JSON, markdown claim), so the digest check does not
    mask the specific finding under test.
    """

    import hashlib

    manifest_path = directory / MANIFEST_ARTIFACT_NAME
    manifest = json.loads(manifest_path.read_text())
    for entry in manifest["artifacts"]:
        content = (directory / entry["name"]).read_bytes()
        entry["sha256"] = hashlib.sha256(content).hexdigest()
        entry["sizeBytes"] = len(content)
    manifest_path.write_text(json.dumps(manifest))


if __name__ == "__main__":
    unittest.main()
