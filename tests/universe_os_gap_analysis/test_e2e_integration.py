"""End-to-end, fail-closed integration tests for the assessment pipeline (Task 12.3).

These tests drive the *full* deterministic pipeline through
:func:`tools.universe_os_gap_analysis.pipeline.run_pipeline` and the thin CLI
boundary :func:`tools.universe_os_gap_analysis.cli.main`, asserting the complete
publish contract end to end (Requirements 3.5, 9.6, 9.7, 14.1-14.7):

* A legal canonical model publishes with :data:`EXIT_OK`, every artifact is
  written, and the digest-bound artifact manifest describes exactly the bytes on
  disk -- committed atomically (no staging directory left behind).
* Every failure mode -- an invalid maturity score, a missing evidence record, an
  unknown gate dependency, a dependency cycle, an unrecorded trust assumption,
  worktree fingerprint drift, and a renderer mismatch/parity failure -- returns
  the correct non-zero ``EXIT_*`` code and writes *nothing*: no partial or
  "half-valid" report is ever left behind, and any prior valid assessment is
  preserved untouched.
* The default execution policy has the network disabled and external command
  execution disabled, both in the pipeline module and as surfaced through the
  CLI JSON summary.

The failure-mode models are injected through the pipeline's pluggable
``assembler`` argument (mirroring ``test_pipeline.py``), reusing
``build_valid_model`` and the model helpers from ``test_validator.py`` so the
pipeline's fail-closed exit-code contract is exercised independently of the
curated repository baseline (Task 14.2). Fingerprint drift is injected by
patching :meth:`RevisionBinder.bind`; renderer mismatch is injected through the
``renderers`` config. No product or existing test/implementation file is
modified, and every non-zero exit is asserted to have written nothing.
"""

from __future__ import annotations

import dataclasses
import io
import json
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

from tools.universe_os_gap_analysis import cli, pipeline
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
from tools.universe_os_gap_analysis.model_builder import RenderedArtifact
from tools.universe_os_gap_analysis.models import AssessmentModel, MaturityScore
from tools.universe_os_gap_analysis.pipeline import (
    EXIT_OK,
    EXIT_PIPELINE_ERROR,
    EXIT_REPOSITORY_DRIFT,
    EXIT_RENDER_PARITY_FAILED,
    EXIT_VALIDATION_FAILED,
    PipelineConfig,
    PipelineContext,
    run_pipeline,
)
from tools.universe_os_gap_analysis.revision import RevisionBindingError
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
)

from . import test_validator as tv
from .test_validator import build_valid_model

# The full set of artifacts a successful publish must commit to disk: every
# default renderer's output plus the digest-bound artifact manifest.
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

# The staging-directory prefix used by the atomic commit; no directory with this
# prefix may survive a run (success or failure).
_STAGING_PREFIX = ".assessment-staging-"


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


# Transient revision-drift codes: the live repository can mutate untracked
# working-tree files (e.g. ``__pycache__``/``.pytest_cache``/``.hypothesis``)
# while the binder is capturing its fingerprint during a full-suite run. That is
# a legitimate fail-closed outcome, but it is orthogonal to the model-driven
# behaviour these tests assert, so runs that hit it are retried to keep the
# model contract deterministic.
_TRANSIENT_DRIFT_CODES = frozenset(
    {"REV-DRIFT", "REV-ROOT-DRIFT", "REV-VERSION-DRIFT", "REV-FINGERPRINT-DRIFT"}
)
_MAX_DRIFT_RETRIES = 8


def _static_assembler(model: AssessmentModel):
    """Return an assembler that ignores its context and yields ``model``."""

    def assemble(_context: PipelineContext) -> AssessmentModel:
        return model

    return assemble


def _run_stable(config: PipelineConfig, *, assembler):
    """Run the pipeline, retrying only on transient live-repository drift.

    Drift caused by the surrounding test run mutating untracked files is not the
    behaviour under test here, so a run that fails closed on a transient
    ``REV-*`` drift code is retried. Any other outcome (including the mocked
    fingerprint-drift test, which patches ``bind`` directly) is returned as-is.
    """

    result = run_pipeline(config, assembler=assembler)
    attempts = 0
    while (
        not result.ok
        and result.error_code in _TRANSIENT_DRIFT_CODES
        and attempts < _MAX_DRIFT_RETRIES
    ):
        attempts += 1
        result = run_pipeline(config, assembler=assembler)
    return result


def _no_staging_left(output_dir: Path) -> bool:
    """True when no private staging directory survives under ``output_dir``."""

    if not output_dir.exists():
        return True
    return not any(
        child.name.startswith(_STAGING_PREFIX) for child in output_dir.iterdir()
    )


# --------------------------------------------------------------------------- #
# Failure-mode model builders (all derived from the one valid model).          #
# --------------------------------------------------------------------------- #
def _model_invalid_score() -> AssessmentModel:
    """A substrate domain with no direct evidence but a non-zero maturity score.

    This is an invalid maturity score: a domain without direct implementation
    evidence must be fixed at 0 (Requirement 3.6, 10.6, 15.5). The validator
    fails closed with ``MAT-*``/``RPT-*`` findings.
    """

    model = build_valid_model()
    nonzero_kernel = tv._kernel_assessment(
        raw=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
        effective=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
        evidence_ids=(),
    )
    return dataclasses.replace(
        model, assessments=(tv._hosted_assessment(), nonzero_kernel)
    )


def _model_missing_record() -> AssessmentModel:
    """Drop every evidence record that other objects reference (Requirement 9.7)."""

    return dataclasses.replace(build_valid_model(), evidence_records=())


def _model_unknown_dependency() -> AssessmentModel:
    """A Hard-Gate whose dependency edge points at a gate that does not exist.

    The graph builder fails closed with ``GRF-UNKNOWN-NODE`` before any maturity
    is computed (Requirement 3.5, 12.7).
    """

    model = build_valid_model()
    dangling_gate = dataclasses.replace(
        tv._hosted_gate(), dependency_ids=("gate-ghost",)
    )
    return dataclasses.replace(
        model, hard_gates=(dangling_gate, tv._kernel_gate())
    )


def _model_cycle() -> AssessmentModel:
    """A dependency cycle between the hosted and kernel gates (Requirement 3.5)."""

    model = build_valid_model()
    cyclic_hosted = dataclasses.replace(
        tv._hosted_gate(), dependency_ids=("gate-kernel",)
    )
    return dataclasses.replace(
        model, hard_gates=(cyclic_hosted, tv._kernel_gate())
    )


def _model_missing_trust_assumption() -> AssessmentModel:
    """An evidence record with a trust assumption that is never recorded.

    The Claim Guard / trust audit fails closed with a ``CLM-*`` finding
    (Requirement 9.5, 9.6).
    """

    model = build_valid_model()
    risky = tv._record(
        record_id="ev-hosted",
        source_path=tv._README_PATH,
        anchor=tv._README_ANCHOR,
        claim="The build relies on a trusted toolchain to link artifacts.",
    )
    return dataclasses.replace(
        model, evidence_records=(risky, model.evidence_records[1])
    )


class EndToEndSuccessTests(unittest.TestCase):
    """A legal model publishes every artifact atomically with EXIT_OK."""

    def test_valid_fixture_publishes_all_artifacts_atomically(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
            self.assertTrue(result.published)
            self.assertEqual(result.findings, ())

            on_disk = {child.name for child in output.iterdir()}
            # Every renderer's artifact plus the manifest is present.
            self.assertEqual(on_disk, set(EXPECTED_ARTIFACTS))
            # The written-paths report matches exactly what is on disk.
            self.assertEqual(
                {Path(path).name for path in result.written_paths},
                set(EXPECTED_ARTIFACTS),
            )
            # The atomic commit cleaned up its private staging directory.
            self.assertTrue(_no_staging_left(output))

    def test_manifest_digests_match_the_committed_bytes(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )
            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)

            manifest = json.loads((output / MANIFEST_ARTIFACT_NAME).read_text())
            # Every manifest entry's digest matches the bytes actually on disk.
            for entry in manifest["artifacts"]:
                artifact_path = output / entry["name"]
                self.assertTrue(artifact_path.exists(), msg=entry["name"])
                content = artifact_path.read_bytes()
                self.assertEqual(entry["sha256"], artifact_digest(content))
                self.assertEqual(entry["sizeBytes"], len(content))
            # The manifest never digests itself.
            names = {entry["name"] for entry in manifest["artifacts"]}
            self.assertNotIn(MANIFEST_ARTIFACT_NAME, names)
            # The manifest is bound to the published revision fingerprint.
            self.assertEqual(
                manifest["revision"]["worktreeFingerprint"],
                result.model.revision.worktree_fingerprint,
            )

    def test_dry_run_validates_and_renders_but_writes_nothing(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=True
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )

            self.assertEqual(result.exit_code, EXIT_OK)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())


class EndToEndFailClosedTests(unittest.TestCase):
    """Every failure mode returns the right non-zero code and writes nothing."""

    def _assert_fails_closed(
        self, model_or_kwargs, expected_exit: int
    ) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            result = _run_stable(
                config, assembler=_static_assembler(model_or_kwargs)
            )

            self.assertEqual(result.exit_code, expected_exit, msg=result.detail)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            # Nothing was written: no output directory, no partial report.
            self.assertFalse(output.exists())

    def test_invalid_score_fails_validation_and_writes_nothing(self) -> None:
        self._assert_fails_closed(_model_invalid_score(), EXIT_VALIDATION_FAILED)

    def test_missing_record_fails_validation_and_writes_nothing(self) -> None:
        self._assert_fails_closed(_model_missing_record(), EXIT_VALIDATION_FAILED)

    def test_unknown_dependency_fails_validation_and_writes_nothing(self) -> None:
        self._assert_fails_closed(
            _model_unknown_dependency(), EXIT_VALIDATION_FAILED
        )

    def test_dependency_cycle_fails_validation_and_writes_nothing(self) -> None:
        self._assert_fails_closed(_model_cycle(), EXIT_VALIDATION_FAILED)

    def test_missing_trust_assumption_fails_validation_and_writes_nothing(self) -> None:
        self._assert_fails_closed(
            _model_missing_trust_assumption(), EXIT_VALIDATION_FAILED
        )

    def test_validation_failure_carries_findings(self) -> None:
        # The fail-closed contract also surfaces validator findings for the
        # caller, without publishing anything.
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            result = _run_stable(
                config, assembler=_static_assembler(_model_missing_record())
            )
            self.assertEqual(result.exit_code, EXIT_VALIDATION_FAILED)
            self.assertTrue(result.publish_result is not None)
            self.assertFalse(result.publish_result.validation.valid)
            self.assertFalse(output.exists())

    def test_fingerprint_drift_fails_closed_before_any_analysis(self) -> None:
        drift = RevisionBindingError(
            "REV-DRIFT",
            "worktree fingerprint changed during binding",
            operation="stability-check",
        )
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(repo_root=_real_repo_root(), output_dir=output)
            with mock.patch.object(
                pipeline.RevisionBinder, "bind", side_effect=drift
            ):
                result = run_pipeline(
                    config, assembler=_static_assembler(build_valid_model())
                )

            self.assertEqual(result.exit_code, EXIT_REPOSITORY_DRIFT)
            self.assertEqual(result.error_code, "REV-DRIFT")
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())

    def test_renderer_that_raises_fails_closed(self) -> None:
        def boom(_model: AssessmentModel) -> RenderedArtifact:
            raise RuntimeError("renderer blew up")

        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=False,
                renderers=(boom,),
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )

            self.assertEqual(result.exit_code, EXIT_RENDER_PARITY_FAILED)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            # The model validated; the failure is strictly at render time.
            self.assertTrue(result.publish_result.validation.valid)
            self.assertFalse(output.exists())

    def test_renderer_emitting_a_foreign_fact_fails_parity(self) -> None:
        def foreign_fact(_model: AssessmentModel) -> RenderedArtifact:
            # References an identifier that does not exist in the canonical
            # model -> cross-artifact parity must reject it (Requirement 14.7).
            return RenderedArtifact(
                name="rogue.json",
                content=b"{}",
                projected_ids=frozenset({"not-a-real-object"}),
            )

        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=False,
                renderers=(foreign_fact,),
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )

            self.assertEqual(result.exit_code, EXIT_RENDER_PARITY_FAILED)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertTrue(result.publish_result.validation.valid)
            self.assertFalse(output.exists())

    def test_assembler_failure_fails_closed(self) -> None:
        def broken(_context: PipelineContext) -> AssessmentModel:
            raise RuntimeError("assembly failed")

        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(repo_root=_real_repo_root(), output_dir=output)
            result = run_pipeline(config, assembler=broken)

            self.assertEqual(result.exit_code, EXIT_PIPELINE_ERROR)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())


class DefaultExecutionPolicyTests(unittest.TestCase):
    """The default policy disables the network and external command execution."""

    def test_pipeline_module_disables_network_and_commands(self) -> None:
        self.assertFalse(pipeline.NETWORK_ENABLED)
        self.assertFalse(pipeline.EXTERNAL_COMMANDS_ENABLED)

    def test_default_config_policy_is_disabled_with_no_allowlist(self) -> None:
        config = PipelineConfig(
            repo_root=_real_repo_root(), output_dir=Path("/nonexistent-output")
        )
        self.assertFalse(config.execution_policy.enabled)
        self.assertEqual(config.execution_policy.commands, ())

    def test_cli_summary_reports_read_only_policy(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                cli.main(
                    [
                        "--repo-root",
                        str(_real_repo_root()),
                        "--output-dir",
                        str(output),
                        "--dry-run",
                    ]
                )
            payload = json.loads(stdout.getvalue())
            self.assertFalse(payload["networkEnabled"])
            self.assertFalse(payload["externalCommandsEnabled"])
            self.assertEqual(payload["writtenPaths"], [])


class AtomicPublishTests(unittest.TestCase):
    """Publishing is atomic: prior assessments are preserved on any failure."""

    def test_failed_publish_preserves_prior_assessment(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            output.mkdir(parents=True)
            # A prior, valid assessment already lives in the output directory.
            prior = output / ASSESSMENT_JSON_ARTIFACT_NAME
            prior_bytes = b'{"prior": "assessment"}'
            prior.write_bytes(prior_bytes)

            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            # This run fails validation, so it must publish nothing and must not
            # clobber the prior assessment.
            result = _run_stable(
                config, assembler=_static_assembler(_model_missing_record())
            )

            self.assertEqual(result.exit_code, EXIT_VALIDATION_FAILED)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            # The prior assessment is byte-for-byte intact.
            self.assertEqual(prior.read_bytes(), prior_bytes)
            # No staging directory leaked into the output directory.
            self.assertTrue(_no_staging_left(output))

    def test_failed_render_preserves_prior_assessment(self) -> None:
        def boom(_model: AssessmentModel) -> RenderedArtifact:
            raise RuntimeError("renderer blew up")

        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            output.mkdir(parents=True)
            prior = output / ASSESSMENT_JSON_ARTIFACT_NAME
            prior_bytes = b'{"prior": "assessment"}'
            prior.write_bytes(prior_bytes)

            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=False,
                renderers=(boom,),
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )

            self.assertEqual(result.exit_code, EXIT_RENDER_PARITY_FAILED)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertEqual(prior.read_bytes(), prior_bytes)
            self.assertTrue(_no_staging_left(output))

    def test_successful_publish_leaves_no_staging_directory(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(), output_dir=output, dry_run=False
            )
            result = _run_stable(
                config, assembler=_static_assembler(build_valid_model())
            )
            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
            self.assertTrue(result.published)
            self.assertTrue(_no_staging_left(output))


if __name__ == "__main__":
    unittest.main()
