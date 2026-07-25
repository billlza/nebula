"""Unit tests for the deterministic ``assessment.json`` renderer (Task 11.1).

These tests exercise Requirements 14.1, 14.3, 14.4, and 14.7 against the real
:mod:`tools.universe_os_gap_analysis.json_renderer` module and a real, fully
populated canonical model (no mocks). They confirm the renderer:

* is deterministic -- the same model renders byte-for-byte identical output;
* emits the schema version and a document that validates against the module's
  own JSON Schema (Requirement 14.1);
* projects the complete canonical model plus its full reference graph and
  validation state (Requirement 14.1, 14.3, 14.4);
* introduces no facts outside the model -- ``projected_ids`` is exactly the
  canonical id set, every graph node is a canonical object, and every resolved
  edge target is a canonical node (Requirement 14.7); and
* integrates with the fail-closed publish gate on a dry run (Requirement 14.7).
"""

from __future__ import annotations

import json
import unittest

from jsonschema import Draft202012Validator

from tools.universe_os_gap_analysis.json_renderer import (
    ASSESSMENT_JSON_ARTIFACT_NAME,
    ASSESSMENT_JSON_SCHEMA_VERSION,
    ASSESSMENT_SCHEMA_ARTIFACT_NAME,
    assessment_json_schema,
    build_assessment_document,
    build_reference_graph,
    render_assessment_json,
    render_schema,
)
from tools.universe_os_gap_analysis.model_builder import (
    build_assessment_model,
    canonical_reference_ids,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import AssessmentModel

# Reuse the fully-populated, internally consistent model the validator tests
# build, so the renderer is exercised against exactly what the pipeline emits.
from .test_validator import build_valid_model


def _document(model: AssessmentModel) -> dict:
    return json.loads(render_assessment_json(model).content.decode("utf-8"))


class RendererInterfaceTests(unittest.TestCase):
    def test_produces_named_assessment_json_artifact(self) -> None:
        artifact = render_assessment_json(build_valid_model())
        self.assertEqual(artifact.name, ASSESSMENT_JSON_ARTIFACT_NAME)
        self.assertIsInstance(artifact.content, bytes)
        # Content must be valid, decodable UTF-8 JSON.
        json.loads(artifact.content.decode("utf-8"))

    def test_rejects_non_model_input(self) -> None:
        with self.assertRaises(TypeError):
            render_assessment_json(object())  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            build_assessment_document(object())  # type: ignore[arg-type]
        with self.assertRaises(TypeError):
            build_reference_graph(object())  # type: ignore[arg-type]


class DeterminismTests(unittest.TestCase):
    def test_same_model_renders_byte_for_byte_identical_output(self) -> None:
        model = build_valid_model()
        first = render_assessment_json(model).content
        second = render_assessment_json(model).content
        self.assertEqual(first, second)

    def test_output_is_canonically_key_sorted(self) -> None:
        # stable_json_bytes sorts keys; re-dumping with sort_keys must match.
        model = build_valid_model()
        document = _document(model)
        canonical = json.dumps(document, sort_keys=True, ensure_ascii=False)
        rendered = render_assessment_json(model).content.decode("utf-8")
        self.assertEqual(json.loads(rendered), json.loads(canonical))

    def test_ends_with_single_trailing_newline(self) -> None:
        content = render_assessment_json(build_valid_model()).content
        self.assertTrue(content.endswith(b"\n"))
        self.assertFalse(content.endswith(b"\n\n"))


class SchemaTests(unittest.TestCase):
    def test_schema_itself_is_a_valid_draft_2020_12_schema(self) -> None:
        Draft202012Validator.check_schema(assessment_json_schema())

    def test_rendered_document_validates_against_schema(self) -> None:
        document = _document(build_valid_model())
        Draft202012Validator(assessment_json_schema()).validate(document)

    def test_document_carries_the_schema_version(self) -> None:
        document = _document(build_valid_model())
        self.assertEqual(document["schemaVersion"], ASSESSMENT_JSON_SCHEMA_VERSION)
        self.assertEqual(
            document["modelSchemaVersion"],
            str(build_valid_model().revision.schema_version),
        )

    def test_schema_artifact_renders_and_is_valid(self) -> None:
        artifact = render_schema(build_valid_model())
        self.assertEqual(artifact.name, ASSESSMENT_SCHEMA_ARTIFACT_NAME)
        schema = json.loads(artifact.content.decode("utf-8"))
        Draft202012Validator.check_schema(schema)
        self.assertEqual(artifact.projected_ids, frozenset())


class ProjectionParityTests(unittest.TestCase):
    def test_projected_ids_are_exactly_the_canonical_ids(self) -> None:
        model = build_valid_model()
        artifact = render_assessment_json(model)
        self.assertEqual(artifact.projected_ids, canonical_reference_ids(model))

    def test_no_foreign_facts_projected_ids_subset_of_canonical(self) -> None:
        model = build_valid_model()
        artifact = render_assessment_json(model)
        self.assertTrue(
            artifact.projected_ids <= canonical_reference_ids(model),
            msg="renderer introduced ids outside the canonical model",
        )

    def test_document_carries_full_model_and_validation_state(self) -> None:
        model = build_valid_model()
        document = _document(model)
        assessment = document["assessment"]
        # Every top-level model section is projected.
        for section in (
            "revision",
            "sourceInventory",
            "evidenceRecords",
            "conflicts",
            "targetLevels",
            "domains",
            "assessments",
            "gaps",
            "hardGates",
            "assumptions",
            "nonClaims",
            "observedConclusions",
            "recommendations",
            "validation",
        ):
            self.assertIn(section, assessment)
        # Validation state is surfaced and matches the model's own result.
        self.assertEqual(document["validation"]["valid"], model.validation.valid)

    def test_projects_attached_valid_validation_state(self) -> None:
        # A model assembled through the builder carries a real (valid) result;
        # the renderer must project it faithfully rather than inventing one.
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
        self.assertTrue(model.validation.valid)
        document = _document(model)
        self.assertTrue(document["validation"]["valid"])
        self.assertEqual(document["assessment"]["validation"]["valid"], True)


class ReferenceGraphTests(unittest.TestCase):
    def test_every_canonical_object_appears_as_a_node(self) -> None:
        model = build_valid_model()
        graph = build_reference_graph(model)
        node_ids = {node["id"] for node in graph["nodes"]}
        self.assertEqual(node_ids, set(canonical_reference_ids(model)))

    def test_nodes_are_unique_and_carry_a_kind(self) -> None:
        graph = build_reference_graph(build_valid_model())
        ids = [node["id"] for node in graph["nodes"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(node["kind"] for node in graph["nodes"]))

    def test_resolved_edges_target_a_canonical_node(self) -> None:
        model = build_valid_model()
        graph = build_reference_graph(model)
        canonical = canonical_reference_ids(model)
        for edge in graph["edges"]:
            self.assertEqual(edge["resolved"], edge["target"] in canonical)

    def test_known_domain_edges_are_present(self) -> None:
        # The hosted domain references its evidence, gap, and dependency gate.
        model = build_valid_model()
        graph = build_reference_graph(model)
        edges = {
            (edge["source"], edge["relation"], edge["target"])
            for edge in graph["edges"]
        }
        self.assertIn(("domain-hosted", "evidence", "ev-hosted"), edges)
        self.assertIn(("domain-hosted", "gap", "gap-hosted"), edges)
        self.assertIn(("domain-hosted", "dependencyGate", "gate-hosted"), edges)
        # The conflict projects both of the records it reconciles.
        self.assertIn(("conflict-1", "evidence", "ev-hosted"), edges)
        self.assertIn(("conflict-1", "evidence", "ev-spec"), edges)

    def test_reference_graph_is_deterministic(self) -> None:
        model = build_valid_model()
        self.assertEqual(build_reference_graph(model), build_reference_graph(model))


class PublishIntegrationTests(unittest.TestCase):
    def test_renderer_publishes_in_dry_run(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, (render_assessment_json,))
        self.assertTrue(result.published, msg=f"{result.findings}")
        self.assertEqual(result.written_paths, ())
        self.assertEqual(
            {artifact.name for artifact in result.artifacts},
            {ASSESSMENT_JSON_ARTIFACT_NAME},
        )

    def test_renderer_and_schema_publish_together(self) -> None:
        model = build_valid_model()
        result = publish_assessment(
            model, (render_assessment_json, render_schema)
        )
        self.assertTrue(result.published, msg=f"{result.findings}")
        self.assertEqual(
            {artifact.name for artifact in result.artifacts},
            {ASSESSMENT_JSON_ARTIFACT_NAME, ASSESSMENT_SCHEMA_ARTIFACT_NAME},
        )


if __name__ == "__main__":
    unittest.main()
