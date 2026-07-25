"""Machine-readable capability-matrix and gap-register table renderer (Task 11.2).

This module projects the single canonical
:class:`~tools.universe_os_gap_analysis.models.AssessmentModel` into stable,
machine-readable tables:

* a **capability matrix** with *exactly one row per*
  :class:`~tools.universe_os_gap_analysis.models.CapabilityDomain`, carrying the
  domain's maturity score, confidence, evidence status, referenced
  ``Evidence_Record`` ids, next Hard-Gate, and target level (Requirement 14.2);
  and
* a **gap register** with *exactly one row per*
  :class:`~tools.universe_os_gap_analysis.models.GapEntry`, carrying every field
  Requirement 12.3 mandates -- affected domains, current status, target level,
  severity, dependencies, acceptance evidence, recommended owner area -- plus the
  one primary / unique-secondary categories (Requirement 12.1, 12.2) and the
  observed-fact / recommendation split (Requirement 14.7).

Each table is emitted in two stable, deterministic encodings: a flat CSV view and
a lossless JSON view. Both conform to the Task 10.2 renderer contract
(``Renderer = Callable[[AssessmentModel], RenderedArtifact]``): every renderer
returns exactly one :class:`RenderedArtifact` whose ``projected_ids`` /
``required_ids`` declare an *exact lossless projection* of the row-key id set
(the domain ids for the matrix, the gap ids for the register), so the publish
gate can enforce one-row-per-object parity for free.

Renderers never introduce facts outside the canonical model. Every value in every
row is copied straight from the model; nothing is inferred, aggregated, or
scored here. Because capability scores are non-additive ordinals, no total,
average, or percentage is ever computed (Requirement 3.3).

In addition to the publish gate's row-key parity, :func:`check_table_parity`
performs a full **bidirectional ID/reference parity** check against the canonical
model: it re-parses the rendered JSON tables and verifies that (a) every model
domain and gap appears as exactly one row and vice-versa, and (b) every
referenced identifier in every row (evidence records, Hard-Gates, dependencies,
affected domains) resolves to an object the canonical model actually defines
(Requirement 14.2, 14.3).
"""

from __future__ import annotations

import csv
import io
import json
from enum import Enum
from typing import Any, Iterable, Mapping, Sequence

from .model_builder import RenderedArtifact, canonical_reference_ids
from .models import (
    AssessmentModel,
    CapabilityAssessment,
    FindingSeverity,
    ValidationFinding,
)
from .serialization import stable_json_bytes

# --------------------------------------------------------------------------- #
# Stable artifact names and the table schema version.                          #
# --------------------------------------------------------------------------- #
TABLE_SCHEMA_VERSION = "1"

CAPABILITY_MATRIX_CSV = "capability_matrix.csv"
CAPABILITY_MATRIX_JSON = "capability_matrix.json"
GAP_REGISTER_CSV = "gap_register.csv"
GAP_REGISTER_JSON = "gap_register.json"

# --------------------------------------------------------------------------- #
# RPT-* : bidirectional table-parity finding codes.                            #
# --------------------------------------------------------------------------- #
RPT_TABLE_MISSING_DOMAIN_ROW = "RPT-TABLE-MISSING-DOMAIN-ROW"
RPT_TABLE_FOREIGN_DOMAIN_ROW = "RPT-TABLE-FOREIGN-DOMAIN-ROW"
RPT_TABLE_MISSING_GAP_ROW = "RPT-TABLE-MISSING-GAP-ROW"
RPT_TABLE_FOREIGN_GAP_ROW = "RPT-TABLE-FOREIGN-GAP-ROW"
RPT_TABLE_FOREIGN_REFERENCE = "RPT-TABLE-FOREIGN-REFERENCE"
RPT_TABLE_MALFORMED = "RPT-TABLE-MALFORMED"

# --------------------------------------------------------------------------- #
# Column order (stable) for each table.                                        #
# --------------------------------------------------------------------------- #
CAPABILITY_MATRIX_COLUMNS: tuple[str, ...] = (
    "domainId",
    "name",
    "targetLevel",
    "mandatoryForTarget",
    "rawScore",
    "effectiveScore",
    "confidence",
    "evidenceStatus",
    "evidenceRefs",
    "nextHardGate",
    "blockingDependencies",
)

GAP_REGISTER_COLUMNS: tuple[str, ...] = (
    "gapId",
    "title",
    "primaryCategory",
    "secondaryCategories",
    "domainIds",
    "currentStatus",
    "targetLevel",
    "severity",
    "dependencies",
    "acceptanceEvidence",
    "recommendedOwnerArea",
    "dependencyCriticality",
    "safetyImpact",
    "claimRisk",
    "targetUnblockValue",
    "observedFact",
    "recommendation",
)

# Columns whose values are id references into the canonical model. Used by the
# bidirectional parity check to confirm no row invents a foreign identifier.
_CAPABILITY_REFERENCE_COLUMNS = ("evidenceRefs", "nextHardGate", "blockingDependencies")
_GAP_REFERENCE_COLUMNS = ("domainIds", "dependencies")


# --------------------------------------------------------------------------- #
# Row construction (primitive projections of the canonical model).             #
# --------------------------------------------------------------------------- #
def _primitive(value: Any) -> Any:
    """Project a model value into a deterministic JSON primitive.

    Enums collapse to their declared value (ints for maturity scores, strings for
    the closed string enums); already-sorted model tuples become lists in the
    same stable order; scalars pass through unchanged.
    """

    if isinstance(value, tuple):
        return [_primitive(item) for item in value]
    if isinstance(value, Enum):
        return value.value
    return value


def _assessment_by_domain(model: AssessmentModel) -> Mapping[str, CapabilityAssessment]:
    return {str(assessment.domain_id): assessment for assessment in model.assessments}


def capability_matrix_rows(model: AssessmentModel) -> tuple[dict[str, Any], ...]:
    """Return one primitive row per capability domain, in stable id order.

    ``model.domains`` is already sorted by stable id, so the row order is
    deterministic. Each row pairs the domain with its single assessment; a domain
    without an assessment is a model-integrity error the validator rejects, so we
    fail loudly here rather than emit an incomplete row.
    """

    assessments = _assessment_by_domain(model)
    rows: list[dict[str, Any]] = []
    for domain in model.domains:
        assessment = assessments.get(str(domain.id))
        if assessment is None:
            raise ValueError(f"domain {domain.id!r} has no capability assessment")
        rows.append(
            {
                "domainId": _primitive(domain.id),
                "name": domain.name,
                "targetLevel": _primitive(domain.target_level),
                "mandatoryForTarget": bool(domain.mandatory_for_target),
                "rawScore": _primitive(assessment.raw_score),
                "effectiveScore": _primitive(assessment.effective_score),
                "confidence": _primitive(assessment.confidence),
                "evidenceStatus": _primitive(assessment.evidence_status),
                "evidenceRefs": _primitive(assessment.evidence_ids),
                "nextHardGate": _primitive(assessment.next_hard_gate_id),
                "blockingDependencies": _primitive(assessment.blocking_dependency_ids),
            }
        )
    return tuple(rows)


def gap_register_rows(model: AssessmentModel) -> tuple[dict[str, Any], ...]:
    """Return one primitive row per gap, in stable id order.

    ``model.gaps`` is already sorted by stable id. Every Requirement 12.3 field is
    projected verbatim, along with the one-primary / unique-secondary categories
    and the observed-fact / recommendation split.
    """

    rows: list[dict[str, Any]] = []
    for gap in model.gaps:
        rows.append(
            {
                "gapId": _primitive(gap.id),
                "title": gap.title,
                "primaryCategory": _primitive(gap.primary_category),
                "secondaryCategories": _primitive(gap.secondary_categories),
                "domainIds": _primitive(gap.domain_ids),
                "currentStatus": _primitive(gap.current_status),
                "targetLevel": _primitive(gap.target_level),
                "severity": _primitive(gap.severity),
                "dependencies": _primitive(gap.dependencies),
                "acceptanceEvidence": _primitive(gap.acceptance_evidence),
                "recommendedOwnerArea": gap.recommended_owner_area,
                "dependencyCriticality": gap.dependency_criticality,
                "safetyImpact": gap.safety_impact,
                "claimRisk": gap.claim_risk,
                "targetUnblockValue": gap.target_unblock_value,
                "observedFact": gap.observed_fact,
                "recommendation": gap.recommendation,
            }
        )
    return tuple(rows)


# --------------------------------------------------------------------------- #
# Encoding: flat CSV and lossless JSON.                                        #
# --------------------------------------------------------------------------- #
def _csv_cell(value: Any) -> str:
    """Render a primitive row value as a single, unambiguous CSV cell.

    List-valued cells (multi-reference / multi-value columns) are encoded as
    compact JSON arrays so embedded separators are never ambiguous and the output
    stays lossless; booleans use the canonical ``true``/``false`` spelling.
    """

    if isinstance(value, list):
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return ""
    return str(value)


def _render_csv(columns: Sequence[str], rows: Iterable[Mapping[str, Any]]) -> bytes:
    """Serialize rows to deterministic UTF-8 CSV with ``\\n`` line endings."""

    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n")
    writer.writerow(list(columns))
    for row in rows:
        writer.writerow([_csv_cell(row[column]) for column in columns])
    return buffer.getvalue().encode("utf-8")


def _render_json(
    table: str, columns: Sequence[str], rows: Iterable[Mapping[str, Any]]
) -> bytes:
    """Serialize rows to canonical JSON: schema version, columns, and rows."""

    document = {
        "schemaVersion": TABLE_SCHEMA_VERSION,
        "table": table,
        "columns": list(columns),
        "rows": [dict(row) for row in rows],
    }
    return stable_json_bytes(document, indent=2)


# --------------------------------------------------------------------------- #
# Renderer callables (Renderer = Callable[[AssessmentModel], RenderedArtifact]).#
# --------------------------------------------------------------------------- #
def _domain_ids(model: AssessmentModel) -> frozenset[str]:
    return frozenset(str(domain.id) for domain in model.domains)


def _gap_ids(model: AssessmentModel) -> frozenset[str]:
    return frozenset(str(gap.id) for gap in model.gaps)


def render_capability_matrix_csv(model: AssessmentModel) -> RenderedArtifact:
    """Render the capability matrix as CSV (exact one-row-per-domain projection)."""

    rows = capability_matrix_rows(model)
    keys = _domain_ids(model)
    return RenderedArtifact(
        name=CAPABILITY_MATRIX_CSV,
        content=_render_csv(CAPABILITY_MATRIX_COLUMNS, rows),
        projected_ids=keys,
        required_ids=keys,
    )


def render_capability_matrix_json(model: AssessmentModel) -> RenderedArtifact:
    """Render the capability matrix as JSON (exact one-row-per-domain projection)."""

    rows = capability_matrix_rows(model)
    keys = _domain_ids(model)
    return RenderedArtifact(
        name=CAPABILITY_MATRIX_JSON,
        content=_render_json("capabilityMatrix", CAPABILITY_MATRIX_COLUMNS, rows),
        projected_ids=keys,
        required_ids=keys,
    )


def render_gap_register_csv(model: AssessmentModel) -> RenderedArtifact:
    """Render the gap register as CSV (exact one-row-per-gap projection)."""

    rows = gap_register_rows(model)
    keys = _gap_ids(model)
    return RenderedArtifact(
        name=GAP_REGISTER_CSV,
        content=_render_csv(GAP_REGISTER_COLUMNS, rows),
        projected_ids=keys,
        required_ids=keys,
    )


def render_gap_register_json(model: AssessmentModel) -> RenderedArtifact:
    """Render the gap register as JSON (exact one-row-per-gap projection)."""

    rows = gap_register_rows(model)
    keys = _gap_ids(model)
    return RenderedArtifact(
        name=GAP_REGISTER_JSON,
        content=_render_json("gapRegister", GAP_REGISTER_COLUMNS, rows),
        projected_ids=keys,
        required_ids=keys,
    )


# The full, stable set of table renderers for use with ``publish_assessment``.
TABLE_RENDERERS: tuple = (
    render_capability_matrix_csv,
    render_capability_matrix_json,
    render_gap_register_csv,
    render_gap_register_json,
)


def render_tables(model: AssessmentModel) -> tuple[RenderedArtifact, ...]:
    """Render every capability/gap table artifact for the model."""

    return tuple(renderer(model) for renderer in TABLE_RENDERERS)


# --------------------------------------------------------------------------- #
# Bidirectional ID/reference parity against the canonical model.               #
# --------------------------------------------------------------------------- #
def _finding(code: str, requirement_refs: Sequence[str], object_refs: Sequence[str]) -> ValidationFinding:
    return ValidationFinding(
        severity=FindingSeverity.ERROR,
        code=code,
        requirement_refs=tuple(requirement_refs),
        object_refs=tuple(object_refs),
    )


def _parse_json_table(artifact: RenderedArtifact) -> list[dict[str, Any]]:
    document = json.loads(artifact.content.decode("utf-8"))
    rows = document.get("rows")
    if not isinstance(rows, list):
        raise ValueError("table document has no rows array")
    return rows


def _collect_references(
    rows: Sequence[Mapping[str, Any]], key_column: str, reference_columns: Sequence[str]
) -> tuple[list[str], list[str]]:
    """Return (row-key ids, referenced ids) drawn from a parsed table."""

    keys: list[str] = []
    references: list[str] = []
    for row in rows:
        keys.append(str(row[key_column]))
        for column in reference_columns:
            value = row.get(column)
            if isinstance(value, list):
                references.extend(str(item) for item in value)
            elif value is not None:
                references.append(str(value))
    return keys, references


def check_table_parity(
    model: AssessmentModel, artifacts: Sequence[RenderedArtifact]
) -> tuple[ValidationFinding, ...]:
    """Verify bidirectional ID/reference parity between tables and the model.

    Re-parses the rendered JSON capability-matrix and gap-register artifacts and
    confirms, in *both* directions, that:

    * every canonical domain appears as exactly one matrix row and every matrix
      row corresponds to a canonical domain (no missing / no foreign rows);
    * every canonical gap appears as exactly one register row and vice-versa; and
    * every identifier a row references (evidence records, Hard-Gates,
      dependencies, affected domains) resolves to an object the canonical model
      defines -- a renderer must never introduce a foreign fact (Requirement
      14.2, 14.3, 14.7).

    Returns an empty tuple when parity holds, or one ``RPT-TABLE-*`` finding per
    discrepancy (with the offending object ids) otherwise. The tables are located
    by their stable artifact names; a missing or malformed table is itself a
    fail-closed finding.
    """

    by_name = {artifact.name: artifact for artifact in artifacts}
    findings: list[ValidationFinding] = []
    canonical = canonical_reference_ids(model)

    findings.extend(
        _check_one_table(
            by_name.get(CAPABILITY_MATRIX_JSON),
            table_name=CAPABILITY_MATRIX_JSON,
            key_column="domainId",
            reference_columns=_CAPABILITY_REFERENCE_COLUMNS,
            expected_keys=_domain_ids(model),
            canonical=canonical,
            missing_code=RPT_TABLE_MISSING_DOMAIN_ROW,
            foreign_code=RPT_TABLE_FOREIGN_DOMAIN_ROW,
        )
    )
    findings.extend(
        _check_one_table(
            by_name.get(GAP_REGISTER_JSON),
            table_name=GAP_REGISTER_JSON,
            key_column="gapId",
            reference_columns=_GAP_REFERENCE_COLUMNS,
            expected_keys=_gap_ids(model),
            canonical=canonical,
            missing_code=RPT_TABLE_MISSING_GAP_ROW,
            foreign_code=RPT_TABLE_FOREIGN_GAP_ROW,
        )
    )
    return tuple(findings)


def _check_one_table(
    artifact: RenderedArtifact | None,
    *,
    table_name: str,
    key_column: str,
    reference_columns: Sequence[str],
    expected_keys: frozenset[str],
    canonical: frozenset[str],
    missing_code: str,
    foreign_code: str,
) -> list[ValidationFinding]:
    if artifact is None:
        return [_finding(RPT_TABLE_MALFORMED, ("14.2", "14.3"), (table_name,))]
    try:
        rows = _parse_json_table(artifact)
    except (ValueError, json.JSONDecodeError):
        return [_finding(RPT_TABLE_MALFORMED, ("14.2", "14.3"), (table_name,))]

    keys, references = _collect_references(rows, key_column, reference_columns)
    key_set = set(keys)
    findings: list[ValidationFinding] = []

    # Forward: every canonical row-key object must have a row.
    missing = sorted(expected_keys - key_set)
    if missing:
        findings.append(_finding(missing_code, ("14.2", "14.3"), missing))

    # Backward: every row must correspond to a canonical row-key object, and
    # each object appears exactly once (no duplicate rows).
    foreign = sorted(key_set - expected_keys)
    duplicates = sorted({key for key in keys if keys.count(key) > 1})
    if foreign or duplicates:
        findings.append(
            _finding(foreign_code, ("14.2", "14.3"), sorted(set(foreign) | set(duplicates)))
        )

    # Reference parity: no row may cite an identifier outside the canonical model.
    foreign_refs = sorted({ref for ref in references if ref not in canonical})
    if foreign_refs:
        findings.append(
            _finding(RPT_TABLE_FOREIGN_REFERENCE, ("14.7",), (table_name, *foreign_refs))
        )
    return findings
