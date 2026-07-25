"""Durable generation smoke/parity test for the final assessment (Task 15.1).

This test drives the durable generation entrypoint
:func:`tools.universe_os_gap_analysis.generate.generate_assessment` against the
*real* Nebula repository, writing into a throwaway temporary directory (so the
committed deliverable under ``tools/universe_os_gap_analysis/artifacts/`` is never
disturbed by the test run). It asserts the fail-closed publish contract end to
end (Requirements 3.3, 3.7, 12.3-12.7, 14.1-14.7, 15.1-15.7):

* generation exits :data:`EXIT_OK` and publishes;
* all eight versioned artifacts are written to disk;
* the digest-bound manifest describes exactly the bytes on disk;
* the narrative ``assessment.md`` states the correct initial evidence-backed
  conclusions -- T1 unachieved, T2-T5 unachieved, language/tooling maturity <= 2,
  freestanding/boot/kernel/userspace maturity 0, and Hosted Adjacency isolated
  from the OS Substrate critical path.

Generation itself retries only on transient live-repository drift (untracked
``__pycache__``/``.pytest_cache``/``.hypothesis`` churn), mirroring the pattern in
``test_e2e_integration.py``, so the model contract stays deterministic without
masking real failures.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.generate import generate_assessment
from tools.universe_os_gap_analysis.json_renderer import (
    ASSESSMENT_JSON_ARTIFACT_NAME,
    ASSESSMENT_SCHEMA_ARTIFACT_NAME,
)
from tools.universe_os_gap_analysis.manifest import (
    MANIFEST_ARTIFACT_NAME,
    artifact_digest,
)
from tools.universe_os_gap_analysis.markdown_renderer import (
    ARTIFACT_NAME as MARKDOWN_ARTIFACT_NAME,
)
from tools.universe_os_gap_analysis.pipeline import EXIT_OK
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
)

# The full versioned deliverable set a successful generation must publish.
EXPECTED_ARTIFACTS = frozenset(
    {
        ASSESSMENT_JSON_ARTIFACT_NAME,
        ASSESSMENT_SCHEMA_ARTIFACT_NAME,
        CAPABILITY_MATRIX_CSV,
        CAPABILITY_MATRIX_JSON,
        GAP_REGISTER_CSV,
        GAP_REGISTER_JSON,
        MARKDOWN_ARTIFACT_NAME,
        MANIFEST_ARTIFACT_NAME,
    }
)

# Substrings that must appear in the narrative report, asserting the initial
# evidence-backed distance conclusion (Requirement 15.1-15.7). Each entry is a
# lowercase fragment matched case-insensitively so minor phrasing edits do not
# make the test brittle while still pinning the substantive claim.
_INITIAL_CONCLUSION_FRAGMENTS = (
    # T1 unachieved because production compilation depends on generated C++ /
    # external host tooling.
    ("t1_independent_language_platform", "unachieved"),
    ("generated c++", "external host tooling"),
    # T2-T5 unachieved.
    ("t2_freestanding_substrate", "t5_operable_universe_os", "unachieved"),
    # Language/tooling capped at maturity 2 without candidate evidence.
    ("language/tooling", "no higher than 2"),
    # Freestanding/boot/kernel/userspace at maturity 0.
    (
        "freestanding runtime",
        "kernel subsystems",
        "universe os userspace",
        "maturity 0",
    ),
    # Hosted Adjacency isolated from the OS Substrate critical path.
    ("hosted adjacency", "os substrate", "critical-path"),
)


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


class DurableGenerationTests(unittest.TestCase):
    """Generation against the real repository publishes the full deliverable."""

    def test_generation_publishes_all_artifacts_with_exit_ok(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "artifacts"
            result = generate_assessment(_real_repo_root(), output)

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.detail)
            self.assertTrue(result.published)
            self.assertEqual(result.findings, ())

            on_disk = {child.name for child in output.iterdir()}
            self.assertEqual(on_disk, set(EXPECTED_ARTIFACTS))
            self.assertEqual(
                {Path(path).name for path in result.written_paths},
                set(EXPECTED_ARTIFACTS),
            )

    def test_manifest_digests_match_committed_bytes(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "artifacts"
            result = generate_assessment(_real_repo_root(), output)
            self.assertEqual(result.exit_code, EXIT_OK, msg=result.detail)

            manifest = json.loads((output / MANIFEST_ARTIFACT_NAME).read_text())
            for entry in manifest["artifacts"]:
                path = output / entry["name"]
                self.assertTrue(path.exists(), msg=entry["name"])
                content = path.read_bytes()
                self.assertEqual(entry["sha256"], artifact_digest(content))
                self.assertEqual(entry["sizeBytes"], len(content))
            names = {entry["name"] for entry in manifest["artifacts"]}
            self.assertNotIn(MANIFEST_ARTIFACT_NAME, names)
            self.assertEqual(
                manifest["revision"]["worktreeFingerprint"],
                result.model.revision.worktree_fingerprint,
            )

    def test_report_states_initial_evidence_backed_conclusions(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "artifacts"
            result = generate_assessment(_real_repo_root(), output)
            self.assertEqual(result.exit_code, EXIT_OK, msg=result.detail)

            report = (output / MARKDOWN_ARTIFACT_NAME).read_text().lower()
            for fragments in _INITIAL_CONCLUSION_FRAGMENTS:
                for fragment in fragments:
                    self.assertIn(
                        fragment,
                        report,
                        msg=f"missing initial-conclusion fragment: {fragment!r}",
                    )

    def test_dry_run_writes_nothing(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "artifacts"
            result = generate_assessment(_real_repo_root(), output, dry_run=True)

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.detail)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
