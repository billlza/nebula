"""Unit tests for the deterministic assessment pipeline orchestration (Task 12.1).

These tests exercise the real
:mod:`tools.universe_os_gap_analysis.pipeline` orchestration against the real
repository (for stage wiring/ordering and the default read-only policy) and
against injected assemblers/renderers (for the fail-closed exit-code contract).
No product code is touched and, on every non-zero exit, no artifact is written.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

from tools.universe_os_gap_analysis import pipeline
from tools.universe_os_gap_analysis.model_builder import RenderedArtifact
from tools.universe_os_gap_analysis.models import AssessmentModel
from tools.universe_os_gap_analysis.pipeline import (
    EXIT_OK,
    EXIT_PIPELINE_ERROR,
    EXIT_REPOSITORY_DRIFT,
    EXIT_RENDER_PARITY_FAILED,
    EXIT_VALIDATION_FAILED,
    PIPELINE_STAGE_ORDER,
    PipelineConfig,
    PipelineContext,
    read_only_execution_policy,
    run_pipeline,
)
from tools.universe_os_gap_analysis.revision import RevisionBindingError

from .test_validator import build_valid_model


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _valid_assembler(model: AssessmentModel):
    def assemble(_context: PipelineContext) -> AssessmentModel:
        return model

    return assemble


class ExecutionPolicyTests(unittest.TestCase):
    """The default policy is explicit and read-only: no network, no commands."""

    def test_module_flags_disable_network_and_commands(self) -> None:
        self.assertFalse(pipeline.NETWORK_ENABLED)
        self.assertFalse(pipeline.EXTERNAL_COMMANDS_ENABLED)

    def test_read_only_policy_is_disabled_with_no_allowlist(self) -> None:
        policy = read_only_execution_policy()
        self.assertFalse(policy.enabled)
        self.assertEqual(policy.commands, ())


class StageWiringTests(unittest.TestCase):
    """The pipeline connects every stage in the fixed deterministic order."""

    def test_real_repository_dry_run_runs_every_stage_in_order(self) -> None:
        with TemporaryDirectory() as tmp:
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=Path(tmp) / "assessment-output",
                dry_run=True,
            )
            result = run_pipeline(config)

        names = [stage.name for stage in result.stages]
        # Stages appear in the canonical order, as a prefix of the full order.
        self.assertEqual(names, list(PIPELINE_STAGE_ORDER[: len(names)]))
        # The analytic stages up to and including model assembly all succeed on
        # the real repository (they are what Task 12.1 wires together).
        upstream = {
            stage.name: stage.status
            for stage in result.stages
            if stage.name != PIPELINE_STAGE_ORDER[-1]
        }
        for name in PIPELINE_STAGE_ORDER[:-1]:
            self.assertEqual(upstream.get(name), "ok", msg=name)

    def test_dry_run_uses_the_read_only_policy_by_default(self) -> None:
        config = PipelineConfig(
            repo_root=_real_repo_root(),
            output_dir=Path("/nonexistent-output"),
            dry_run=True,
        )
        self.assertFalse(config.execution_policy.enabled)


class DryRunTests(unittest.TestCase):
    """A dry run exercises validation/rendering but writes nothing."""

    def test_valid_model_dry_run_publishes_nothing(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=True,
            )
            result = run_pipeline(config, assembler=_valid_assembler(build_valid_model()))

            self.assertEqual(result.exit_code, EXIT_OK)
            self.assertTrue(result.ok)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            # Nothing was written to the explicit output directory.
            self.assertFalse(output.exists())
            # The publish transaction still rendered every artifact in memory.
            self.assertTrue(result.publish_result is not None)
            self.assertTrue(result.publish_result.published)


class PublishTests(unittest.TestCase):
    """A non-dry-run publish commits every artifact atomically."""

    def test_valid_model_publish_writes_all_artifacts(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=False,
            )
            result = run_pipeline(config, assembler=_valid_assembler(build_valid_model()))

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
            self.assertTrue(result.published)
            self.assertTrue(result.written_paths)
            on_disk = {child.name for child in output.iterdir()}
            self.assertIn("assessment.json", on_disk)
            self.assertIn("assessment.md", on_disk)


class FailClosedTests(unittest.TestCase):
    """Every failure path returns non-zero and writes nothing."""

    def test_default_curated_baseline_publishes_on_real_repo(self) -> None:
        # The default assembler is now the Task 14.2 curated baseline, which
        # assembles a complete, valid model for the real repository. The
        # real-repository run therefore reaches EXIT_OK and publishes atomically.
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=False,
            )
            result = run_pipeline(config)

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
            self.assertTrue(result.published)
            self.assertTrue(result.written_paths)
            self.assertTrue(result.publish_result.validation.valid)
            self.assertTrue(output.exists())

    def test_repository_drift_fails_closed(self) -> None:
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
                result = run_pipeline(config)

            self.assertEqual(result.exit_code, EXIT_REPOSITORY_DRIFT)
            self.assertFalse(result.published)
            self.assertEqual(result.error_code, "REV-DRIFT")
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())

    def test_unbindable_repository_fails_closed(self) -> None:
        with TemporaryDirectory() as tmp:
            # A bare temporary directory is not a Git repository, so binding
            # fails and the pipeline must fail closed before any analysis.
            repo = Path(tmp) / "not-a-repo"
            repo.mkdir()
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(repo_root=repo, output_dir=output)
            result = run_pipeline(config)

            self.assertNotEqual(result.exit_code, EXIT_OK)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())

    def test_render_failure_fails_closed(self) -> None:
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
            result = run_pipeline(config, assembler=_valid_assembler(build_valid_model()))

            self.assertEqual(result.exit_code, EXIT_RENDER_PARITY_FAILED)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertTrue(result.publish_result.validation.valid)
            self.assertFalse(output.exists())

    def test_assembler_exception_fails_closed(self) -> None:
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


if __name__ == "__main__":
    unittest.main()
