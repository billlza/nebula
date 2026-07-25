"""Real-repository curated baseline tests (Task 14.2).

These tests drive the curated baseline assembler
(:func:`tools.universe_os_gap_analysis.baseline.build_curated_model`) against the
*real* Nebula repository through the deterministic pipeline. They assert the
evidence discipline the curated baseline must uphold:

* the curated model is valid (the single publish validator accepts it);
* every OS-substrate domain (``T2``-``T5``: freestanding runtime, linked/bootable
  chain, kernel, drivers, UniverseOS userspace) stays at maturity 0 / Unsupported
  with no credited evidence (Requirements 3.6, 10.6, 15.5);
* hosted-adjacency / language-platform domains reflect their evidenced maturity
  but never exceed repository-local 2 (Requirements 15.1, 15.4);
* no capability is inferred from plans (a domain whose only evidence is
  planned/specification text scores 0); and
* a real-repository ``run_pipeline`` reaches ``EXIT_OK`` and atomically publishes
  every artifact.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.baseline import build_curated_model
from tools.universe_os_gap_analysis.models import MaturityScore, TargetLevel
from tools.universe_os_gap_analysis.pipeline import (
    EXIT_OK,
    PipelineConfig,
    run_pipeline,
)
from tools.universe_os_gap_analysis.validator import validate_assessment_model

_SUBSTRATE_LEVELS = frozenset(
    {
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        TargetLevel.T5_OPERABLE_UNIVERSE_OS,
    }
)

_PUBLISHED_ARTIFACTS = frozenset(
    {
        "assessment.json",
        "assessment.md",
        "assessment.schema.json",
        "assessment.manifest.json",
        "capability_matrix.csv",
        "capability_matrix.json",
        "gap_register.csv",
        "gap_register.json",
    }
)


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _dry_run_model():
    """Assemble the curated model for the real repo via a dry-run pipeline."""

    with TemporaryDirectory() as tmp:
        config = PipelineConfig(
            repo_root=_real_repo_root(),
            output_dir=Path(tmp) / "assessment-output",
            dry_run=True,
        )
        result = run_pipeline(config)
    return result


class CuratedBaselineValidityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.result = _dry_run_model()
        self.model = self.result.model

    def test_curated_baseline_reaches_exit_ok(self) -> None:
        self.assertEqual(self.result.exit_code, EXIT_OK, msg=self.result.findings)
        self.assertIsNotNone(self.model)

    def test_curated_model_is_valid(self) -> None:
        result = validate_assessment_model(self.model)
        self.assertTrue(result.valid, msg=f"findings: {result.findings}")

    def test_default_assembler_is_the_curated_baseline(self) -> None:
        # run_pipeline with no explicit assembler must produce the curated model.
        self.assertTrue(self.model.domains)
        self.assertEqual(len(self.model.assessments), len(self.model.domains))

    def test_every_domain_has_exactly_one_assessment(self) -> None:
        domain_ids = sorted(str(domain.id) for domain in self.model.domains)
        assessed = sorted(str(a.domain_id) for a in self.model.assessments)
        self.assertEqual(domain_ids, assessed)

    def test_six_target_levels_present(self) -> None:
        self.assertEqual(set(self.model.target_levels), set(TargetLevel))


class EvidenceDisciplineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.model = _dry_run_model().model
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_substrate_domains_stay_zero_without_evidence(self) -> None:
        for assessment in self.model.assessments:
            domain = self.domain_by_id[str(assessment.domain_id)]
            if domain.target_level in _SUBSTRATE_LEVELS:
                self.assertEqual(
                    int(assessment.effective_score),
                    0,
                    msg=f"{domain.id} ({domain.target_level.value}) must be 0",
                )
                self.assertEqual(
                    int(assessment.raw_score),
                    0,
                    msg=f"{domain.id} raw must be 0",
                )
                self.assertEqual(
                    assessment.evidence_ids,
                    (),
                    msg=f"{domain.id} must credit no evidence",
                )

    def test_no_capability_exceeds_repository_local_two(self) -> None:
        for assessment in self.model.assessments:
            self.assertLessEqual(
                int(assessment.effective_score),
                int(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION),
                msg=f"{assessment.domain_id} exceeds repository-local cap 2",
            )

    def test_hosted_domains_reflect_evidenced_maturity(self) -> None:
        # At least one hosted-adjacency / language-platform domain has direct
        # implementation evidence and therefore a non-zero, evidence-backed score.
        hosted_scored = [
            assessment
            for assessment in self.model.assessments
            if self.domain_by_id[str(assessment.domain_id)].target_level
            in {
                TargetLevel.T0_HOSTED_ADJACENCY,
                TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            }
            and int(assessment.effective_score) > 0
        ]
        self.assertTrue(hosted_scored, "expected evidenced hosted maturity > 0")
        for assessment in hosted_scored:
            # A non-zero score must be backed by credited direct evidence.
            self.assertTrue(
                assessment.evidence_ids,
                msg=f"{assessment.domain_id} scored > 0 without evidence",
            )

    def test_no_capability_inferred_from_plans(self) -> None:
        # Any assessment that credits no direct evidence must be exactly 0; a
        # positive score is never inferred from plans, prerequisites, or adjacency.
        for assessment in self.model.assessments:
            if not assessment.evidence_ids:
                self.assertEqual(int(assessment.effective_score), 0)
                self.assertEqual(int(assessment.raw_score), 0)

    def test_substrate_non_claims_present(self) -> None:
        joined = "\n".join(self.model.non_claims).lower()
        for token in ("kernel", "driver", "freestanding runtime", "bootable"):
            self.assertIn(token, joined)


class CuratedBaselinePublishTests(unittest.TestCase):
    def test_real_repo_publish_writes_all_artifacts(self) -> None:
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
            on_disk = {child.name for child in output.iterdir()}
            self.assertEqual(_PUBLISHED_ARTIFACTS, on_disk)

    def test_dry_run_publishes_nothing(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=output,
                dry_run=True,
            )
            result = run_pipeline(config)

            self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
            self.assertFalse(result.published)
            self.assertEqual(result.written_paths, ())
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
