"""Unit tests for the capability-matrix / gap-register table renderer (Task 11.2).

These tests exercise Requirements 3.3, 12.3, 14.2, and 14.3 against the real
:mod:`tools.universe_os_gap_analysis.table_renderer` module and real canonical
model objects (no mocks). They confirm the renderer:

* emits exactly one capability-matrix row per :class:`CapabilityDomain` with the
  Requirement 14.2 columns (maturity score, confidence, evidence status,
  evidence references, next Hard-Gate, target level);
* emits exactly one gap-register row per :class:`GapEntry` carrying every
  Requirement 12.3 field (affected domains, current status, target level,
  severity, dependencies, acceptance evidence, recommended owner area) plus the
  one-primary / unique-secondary categories;
* is deterministic (identical models render byte-identical CSV and JSON);
* passes a bidirectional ID/reference parity check against the canonical model
  and fails closed when a row is missing, foreign, or cites a foreign fact; and
* integrates with the Task 10.2 ``publish_assessment`` dry run so the publish
  gate's own exact-projection parity is satisfied.
"""

from __future__ import annotations

import csv
import dataclasses
import io
import json
import unittest

from tools.universe_os_gap_analysis.model_builder import (
    canonical_reference_ids,
    publish_assessment,
)
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_COLUMNS,
    CAPABILITY_MATRIX_CSV,
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_COLUMNS,
    GAP_REGISTER_CSV,
    GAP_REGISTER_JSON,
    RPT_TABLE_FOREIGN_DOMAIN_ROW,
    RPT_TABLE_FOREIGN_REFERENCE,
    RPT_TABLE_MALFORMED,
    RPT_TABLE_MISSING_DOMAIN_ROW,
    TABLE_RENDERERS,
    capability_matrix_rows,
    check_table_parity,
    gap_register_rows,
    render_capability_matrix_csv,
    render_capability_matrix_json,
    render_gap_register_csv,
    render_gap_register_json,
    render_tables,
)

from .test_validator import build_valid_model


def _artifacts_by_name(model):
    return {artifact.name: artifact for artifact in render_tables(model)}


def _json_rows(artifact):
    document = json.loads(artifact.content.decode("utf-8"))
    return document["rows"]


def _csv_rows(artifact):
    reader = csv.reader(io.StringIO(artifact.content.decode("utf-8")))
    return list(reader)


class CapabilityMatrixTests(unittest.TestCase):
    def test_exactly_one_row_per_domain(self) -> None:
        model = build_valid_model()
        rows = capability_matrix_rows(model)
        self.assertEqual(len(rows), len(model.domains))
        row_ids = [row["domainId"] for row in rows]
        model_ids = [str(domain.id) for domain in model.domains]
        # One row per domain, no duplicates, same stable order as the model.
        self.assertEqual(row_ids, model_ids)
        self.assertEqual(len(set(row_ids)), len(model.domains))

    def test_required_columns_present_and_populated(self) -> None:
        model = build_valid_model()
        artifact = render_capability_matrix_json(model)
        rows = _json_rows(artifact)
        assessments = {str(a.domain_id): a for a in model.assessments}
        for row in rows:
            self.assertEqual(set(row.keys()), set(CAPABILITY_MATRIX_COLUMNS))
            assessment = assessments[row["domainId"]]
            # Requirement 14.2 columns projected verbatim from the model.
            self.assertEqual(row["effectiveScore"], int(assessment.effective_score))
            self.assertEqual(row["rawScore"], int(assessment.raw_score))
            self.assertEqual(row["confidence"], assessment.confidence.value)
            self.assertEqual(row["evidenceStatus"], assessment.evidence_status.value)
            self.assertEqual(
                row["evidenceRefs"], [str(ref) for ref in assessment.evidence_ids]
            )
            self.assertEqual(row["nextHardGate"], str(assessment.next_hard_gate_id))

    def test_csv_header_and_row_count_match_domains(self) -> None:
        model = build_valid_model()
        rows = _csv_rows(render_capability_matrix_csv(model))
        self.assertEqual(rows[0], list(CAPABILITY_MATRIX_COLUMNS))
        self.assertEqual(len(rows) - 1, len(model.domains))

    def test_no_aggregate_score_is_emitted(self) -> None:
        # Requirement 3.3: capability scores are non-additive ordinals; the table
        # must never introduce a total/average/percentage column.
        forbidden = {"total", "average", "sum", "percent", "percentage", "mean"}
        lowered = {column.lower() for column in CAPABILITY_MATRIX_COLUMNS}
        self.assertEqual(lowered & forbidden, set())


class GapRegisterTests(unittest.TestCase):
    def test_exactly_one_row_per_gap(self) -> None:
        model = build_valid_model()
        rows = gap_register_rows(model)
        self.assertEqual(len(rows), len(model.gaps))
        row_ids = [row["gapId"] for row in rows]
        self.assertEqual(row_ids, [str(gap.id) for gap in model.gaps])
        self.assertEqual(len(set(row_ids)), len(model.gaps))

    def test_requirement_12_3_fields_present(self) -> None:
        model = build_valid_model()
        rows = _json_rows(render_gap_register_json(model))
        gaps = {str(gap.id): gap for gap in model.gaps}
        # Every Requirement 12.3 field must be a column.
        required_12_3 = {
            "domainIds",
            "currentStatus",
            "targetLevel",
            "severity",
            "dependencies",
            "acceptanceEvidence",
            "recommendedOwnerArea",
        }
        self.assertTrue(required_12_3.issubset(set(GAP_REGISTER_COLUMNS)))
        for row in rows:
            self.assertEqual(set(row.keys()), set(GAP_REGISTER_COLUMNS))
            gap = gaps[row["gapId"]]
            self.assertEqual(row["primaryCategory"], gap.primary_category.value)
            # Exactly one primary; secondary categories exclude the primary.
            self.assertNotIn(gap.primary_category.value, row["secondaryCategories"])
            self.assertEqual(
                row["domainIds"], [str(ref) for ref in gap.domain_ids]
            )
            self.assertEqual(row["severity"], gap.severity.value)
            self.assertEqual(row["currentStatus"], gap.current_status.value)
            self.assertEqual(row["recommendedOwnerArea"], gap.recommended_owner_area)
            # Observed facts stay separate from recommendations (Req 14.7).
            self.assertEqual(row["observedFact"], gap.observed_fact)
            self.assertEqual(row["recommendation"], gap.recommendation)

    def test_csv_header_and_row_count_match_gaps(self) -> None:
        model = build_valid_model()
        rows = _csv_rows(render_gap_register_csv(model))
        self.assertEqual(rows[0], list(GAP_REGISTER_COLUMNS))
        self.assertEqual(len(rows) - 1, len(model.gaps))


class DeterminismTests(unittest.TestCase):
    def test_identical_models_render_byte_identical_artifacts(self) -> None:
        first = _artifacts_by_name(build_valid_model())
        second = _artifacts_by_name(build_valid_model())
        self.assertEqual(set(first), set(second))
        for name, artifact in first.items():
            self.assertEqual(artifact.content, second[name].content)

    def test_all_four_table_artifacts_are_emitted(self) -> None:
        names = set(_artifacts_by_name(build_valid_model()))
        self.assertEqual(
            names,
            {
                CAPABILITY_MATRIX_CSV,
                CAPABILITY_MATRIX_JSON,
                GAP_REGISTER_CSV,
                GAP_REGISTER_JSON,
            },
        )


class BidirectionalParityTests(unittest.TestCase):
    def test_valid_model_tables_pass_parity(self) -> None:
        model = build_valid_model()
        findings = check_table_parity(model, render_tables(model))
        self.assertEqual(findings, ())

    def test_missing_domain_row_fails_closed(self) -> None:
        model = build_valid_model()
        artifacts = list(render_tables(model))
        # Drop the first data row from the capability matrix JSON table.
        matrix = next(a for a in artifacts if a.name == CAPABILITY_MATRIX_JSON)
        document = json.loads(matrix.content.decode("utf-8"))
        document["rows"] = document["rows"][1:]
        broken = dataclasses.replace(
            matrix, content=json.dumps(document).encode("utf-8")
        )
        artifacts = [broken if a.name == CAPABILITY_MATRIX_JSON else a for a in artifacts]
        codes = {f.code for f in check_table_parity(model, artifacts)}
        self.assertIn(RPT_TABLE_MISSING_DOMAIN_ROW, codes)

    def test_foreign_domain_row_fails_closed(self) -> None:
        model = build_valid_model()
        artifacts = list(render_tables(model))
        matrix = next(a for a in artifacts if a.name == CAPABILITY_MATRIX_JSON)
        document = json.loads(matrix.content.decode("utf-8"))
        stray = dict(document["rows"][0])
        stray["domainId"] = "domain-not-in-model"
        document["rows"].append(stray)
        broken = dataclasses.replace(
            matrix, content=json.dumps(document).encode("utf-8")
        )
        artifacts = [broken if a.name == CAPABILITY_MATRIX_JSON else a for a in artifacts]
        codes = {f.code for f in check_table_parity(model, artifacts)}
        self.assertIn(RPT_TABLE_FOREIGN_DOMAIN_ROW, codes)

    def test_foreign_reference_fails_closed(self) -> None:
        model = build_valid_model()
        artifacts = list(render_tables(model))
        matrix = next(a for a in artifacts if a.name == CAPABILITY_MATRIX_JSON)
        document = json.loads(matrix.content.decode("utf-8"))
        document["rows"][0]["evidenceRefs"] = ["ev-does-not-exist"]
        broken = dataclasses.replace(
            matrix, content=json.dumps(document).encode("utf-8")
        )
        artifacts = [broken if a.name == CAPABILITY_MATRIX_JSON else a for a in artifacts]
        codes = {f.code for f in check_table_parity(model, artifacts)}
        self.assertIn(RPT_TABLE_FOREIGN_REFERENCE, codes)

    def test_missing_table_artifact_fails_closed(self) -> None:
        model = build_valid_model()
        # Provide only the capability matrix -> the gap register is missing.
        artifacts = [render_capability_matrix_json(model)]
        codes = {f.code for f in check_table_parity(model, artifacts)}
        self.assertIn(RPT_TABLE_MALFORMED, codes)

    def test_all_referenced_ids_exist_in_canonical_model(self) -> None:
        model = build_valid_model()
        canonical = canonical_reference_ids(model)
        for row in capability_matrix_rows(model):
            self.assertIn(row["domainId"], canonical)
            self.assertIn(row["nextHardGate"], canonical)
            for ref in row["evidenceRefs"]:
                self.assertIn(ref, canonical)
        for row in gap_register_rows(model):
            self.assertIn(row["gapId"], canonical)
            for ref in row["domainIds"]:
                self.assertIn(ref, canonical)


class PublishIntegrationTests(unittest.TestCase):
    def test_publish_dry_run_accepts_table_renderers(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, TABLE_RENDERERS)
        self.assertTrue(result.published, msg=f"findings: {result.findings}")
        self.assertEqual(result.findings, ())
        names = {artifact.name for artifact in result.artifacts}
        self.assertEqual(
            names,
            {
                CAPABILITY_MATRIX_CSV,
                CAPABILITY_MATRIX_JSON,
                GAP_REGISTER_CSV,
                GAP_REGISTER_JSON,
            },
        )

    def test_publish_enforces_exact_domain_projection(self) -> None:
        # The capability matrix declares required_ids == the domain id set, so the
        # publish gate itself guarantees one row per domain.
        model = build_valid_model()
        artifact = render_capability_matrix_json(model)
        self.assertEqual(
            artifact.required_ids,
            frozenset(str(domain.id) for domain in model.domains),
        )
        self.assertEqual(artifact.projected_ids, artifact.required_ids)


if __name__ == "__main__":
    unittest.main()
