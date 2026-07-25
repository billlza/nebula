"""Unit tests for the canonical model builder and publish transaction (Task 10.2).

These tests exercise Requirements 9.7 and 14.1-14.7 against the real
:mod:`tools.universe_os_gap_analysis.model_builder` module and real canonical
model objects (no mocks). They confirm:

* :func:`build_assessment_model` aggregates every input into one
  :class:`AssessmentModel`, defaults the six-level target model, and attaches the
  Task 10.1 validation result;
* :func:`publish_assessment` validates *first* and refuses to emit any artifact
  when the model is invalid (all-or-nothing; Requirement 9.7);
* a renderer that raises, a duplicate artifact name, a foreign-fact reference, or
  a broken lossless projection each rejects the *whole* publish and writes
  nothing;
* a valid model with well-formed renderers commits every artifact atomically to
  the output directory; and
* a failed publish never clobbers a prior valid assessment on disk.
"""

from __future__ import annotations

import dataclasses
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.model_builder import (
    RPT_DUPLICATE_ARTIFACT,
    RPT_PARITY_FOREIGN_FACT,
    RPT_PARITY_MISSING_PROJECTION,
    RPT_PUBLISH_INVALID_MODEL,
    RPT_RENDER_FAILED,
    RenderedArtifact,
    build_assessment_model,
    canonical_reference_ids,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import AssessmentModel, TargetLevel

# Reuse the fully-populated, internally consistent model the validator tests
# build, so these tests aggregate/publish exactly what the pipeline produces.
from .test_validator import build_valid_model


def _codes(result) -> set[str]:
    return {finding.code for finding in result.findings}


class BuildAssessmentModelTests(unittest.TestCase):
    def test_aggregates_all_inputs_into_single_model(self) -> None:
        reference = build_valid_model()
        model = build_assessment_model(
            revision=reference.revision,
            source_inventory=reference.source_inventory,
            evidence_records=reference.evidence_records,
            conflicts=reference.conflicts,
            target_levels=reference.target_levels,
            domains=reference.domains,
            assessments=reference.assessments,
            gaps=reference.gaps,
            hard_gates=reference.hard_gates,
            assumptions=reference.assumptions,
            non_claims=reference.non_claims,
            observed_conclusions=reference.observed_conclusions,
            recommendations=reference.recommendations,
        )
        self.assertIsInstance(model, AssessmentModel)
        self.assertEqual(model.domains, reference.domains)
        self.assertEqual(model.evidence_records, reference.evidence_records)
        self.assertEqual(model.gaps, reference.gaps)
        self.assertEqual(model.hard_gates, reference.hard_gates)

    def test_defaults_to_the_six_level_target_model(self) -> None:
        reference = build_valid_model()
        model = build_assessment_model(
            revision=reference.revision,
            source_inventory=reference.source_inventory,
            evidence_records=reference.evidence_records,
            conflicts=reference.conflicts,
            domains=reference.domains,
            assessments=reference.assessments,
            gaps=reference.gaps,
            hard_gates=reference.hard_gates,
            assumptions=reference.assumptions,
            non_claims=reference.non_claims,
            observed_conclusions=reference.observed_conclusions,
            recommendations=reference.recommendations,
        )
        self.assertEqual(set(model.target_levels), set(TargetLevel))

    def test_attaches_validation_result_for_valid_model(self) -> None:
        reference = build_valid_model()
        model = build_assessment_model(
            revision=reference.revision,
            source_inventory=reference.source_inventory,
            evidence_records=reference.evidence_records,
            conflicts=reference.conflicts,
            domains=reference.domains,
            assessments=reference.assessments,
            gaps=reference.gaps,
            hard_gates=reference.hard_gates,
            assumptions=reference.assumptions,
            non_claims=reference.non_claims,
            observed_conclusions=reference.observed_conclusions,
            recommendations=reference.recommendations,
        )
        self.assertTrue(model.validation.valid, msg=f"{model.validation.findings}")

    def test_rejects_non_revision_input(self) -> None:
        with self.assertRaises(TypeError):
            build_assessment_model(revision=object())  # type: ignore[arg-type]


def _json_renderer(model: AssessmentModel) -> RenderedArtifact:
    """A minimal well-formed renderer that projects the full canonical id set."""

    ids = canonical_reference_ids(model)
    return RenderedArtifact(
        name="assessment.json",
        content=b"{}",
        projected_ids=ids,
    )


def _domain_row_ids(model: AssessmentModel) -> frozenset[str]:
    return frozenset(str(domain.id) for domain in model.domains)


def _matrix_renderer(model: AssessmentModel) -> RenderedArtifact:
    """A capability-matrix renderer declaring an exact lossless projection."""

    ids = _domain_row_ids(model)
    return RenderedArtifact(
        name="capability_matrix.csv",
        content=b"domain\n",
        projected_ids=ids,
        required_ids=ids,
    )


class PublishValidationGateTests(unittest.TestCase):
    def test_invalid_model_publishes_nothing(self) -> None:
        model = dataclasses.replace(build_valid_model(), non_claims=())
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model, (_json_renderer,), output_dir=tmp
            )
            self.assertFalse(result.published)
            self.assertIn(RPT_PUBLISH_INVALID_MODEL, _codes(result))
            self.assertFalse(result.validation.valid)
            self.assertEqual(result.written_paths, ())
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_valid_model_dry_run_buffers_without_writing(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, (_json_renderer, _matrix_renderer))
        self.assertTrue(result.published, msg=f"{result.findings}")
        self.assertEqual(result.written_paths, ())
        self.assertEqual(
            {artifact.name for artifact in result.artifacts},
            {"assessment.json", "capability_matrix.csv"},
        )


class PublishRenderFailureTests(unittest.TestCase):
    def test_renderer_that_raises_fails_closed(self) -> None:
        def boom(_model: AssessmentModel) -> RenderedArtifact:
            raise RuntimeError("renderer blew up")

        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model, (_json_renderer, boom), output_dir=tmp
            )
            self.assertFalse(result.published)
            self.assertIn(RPT_RENDER_FAILED, _codes(result))
            self.assertEqual(result.written_paths, ())
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_duplicate_artifact_name_fails_closed(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, (_json_renderer, _json_renderer))
        self.assertFalse(result.published)
        self.assertIn(RPT_DUPLICATE_ARTIFACT, _codes(result))


class PublishParityTests(unittest.TestCase):
    def test_foreign_fact_reference_fails_closed(self) -> None:
        def foreign(_model: AssessmentModel) -> RenderedArtifact:
            return RenderedArtifact(
                name="assessment.json",
                content=b"{}",
                projected_ids=frozenset({"totally-made-up-id"}),
            )

        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(model, (foreign,), output_dir=tmp)
            self.assertFalse(result.published)
            self.assertIn(RPT_PARITY_FOREIGN_FACT, _codes(result))
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_incomplete_projection_fails_closed(self) -> None:
        def lossy_matrix(model: AssessmentModel) -> RenderedArtifact:
            required = _domain_row_ids(model)
            # Drop one required domain row -> not a lossless projection.
            projected = frozenset(sorted(required)[1:])
            return RenderedArtifact(
                name="capability_matrix.csv",
                content=b"domain\n",
                projected_ids=projected,
                required_ids=required,
            )

        model = build_valid_model()
        result = publish_assessment(model, (lossy_matrix,))
        self.assertFalse(result.published)
        self.assertIn(RPT_PARITY_MISSING_PROJECTION, _codes(result))


class PublishAtomicCommitTests(unittest.TestCase):
    def test_valid_publish_commits_all_artifacts(self) -> None:
        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model, (_json_renderer, _matrix_renderer), output_dir=tmp
            )
            self.assertTrue(result.published, msg=f"{result.findings}")
            written = {Path(path).name for path in result.written_paths}
            self.assertEqual(written, {"assessment.json", "capability_matrix.csv"})
            on_disk = {child.name for child in Path(tmp).iterdir()}
            self.assertEqual(on_disk, {"assessment.json", "capability_matrix.csv"})
            self.assertEqual(
                (Path(tmp) / "assessment.json").read_bytes(), b"{}"
            )

    def test_failed_publish_preserves_prior_valid_assessment(self) -> None:
        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            # Commit a first, good assessment.
            first = publish_assessment(model, (_json_renderer,), output_dir=tmp)
            self.assertTrue(first.published)
            prior = (Path(tmp) / "assessment.json").read_bytes()

            # A later publish fails during rendering; the prior file must remain.
            def boom(_model: AssessmentModel) -> RenderedArtifact:
                raise RuntimeError("later render failed")

            second = publish_assessment(model, (boom,), output_dir=tmp)
            self.assertFalse(second.published)
            self.assertEqual(second.written_paths, ())
            self.assertTrue((Path(tmp) / "assessment.json").exists())
            self.assertEqual(
                (Path(tmp) / "assessment.json").read_bytes(), prior
            )
            # No stray staging directories were left behind.
            leftovers = [
                child.name
                for child in Path(tmp).iterdir()
                if child.name.startswith(".assessment-staging-")
            ]
            self.assertEqual(leftovers, [])


if __name__ == "__main__":
    unittest.main()
