# Feature: nebula-universe-os-gap-analysis, Property 22: Structured and narrative outputs are lossless projections
# **Validates: Requirements 14.2, 14.3, 14.4, 14.5, 14.6, 14.7**
"""Hypothesis property test for Property 22.

Property 22 (design.md): *For all valid canonical assessment models, the
capability table has exactly one matching row per domain, the gap register
exactly one matching row per gap, every material conclusion has a
repository-relative stable anchor, every unexecuted source is disclosed, and
observed facts and recommendations remain in separate typed sections.*

This module drives Hypothesis over **valid** structural variations of the
canonical model that :func:`build_valid_model` (the validator test helper)
produces: it adds extra capability domains (each with a matching assessment),
extra gaps, extra observed conclusions, extra recommendations, extra
assumptions/non-claims, and it toggles whether the evidence conflict is present.
Every variation is assembled to remain internally consistent, and each generated
model is asserted to pass the real publish validator before the projection
invariants are checked -- so the properties below are only ever evaluated against
legal canonical models.

For every such model the test asserts the Property 22 invariants against the
real JSON, table, and Markdown renderers (no mocks):

* (a) exactly one capability-matrix row per domain and one gap-register row per
      gap, one-to-one in both directions (Requirement 14.2, 14.3);
* (b) the JSON renderer's ``projected_ids`` equals the canonical id set
      (Requirement 14.7);
* (c) cross-format parity: the domain/gap ids exposed by the JSON structured
      output, the machine-readable tables, and the Markdown narrative agree
      (Requirement 14.2, 14.3);
* (d) the Markdown narrative cites a repository-relative path plus a stable
      anchor for every material conclusion and discloses every unexecuted source
      (Requirement 14.4, 14.5, 14.6);
* (e) observed current facts and recommendations are kept in separate, typed
      sections (Requirement 14.7); and
* (f) no renderer introduces a foreign fact -- every artifact's ``projected_ids``
      is a subset of the canonical id set, and the all-or-nothing publish gate
      accepts the full renderer set (Requirement 14.7).
"""

from __future__ import annotations

import dataclasses
import json

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.json_renderer import (
    render_assessment_json,
    render_schema,
)
from tools.universe_os_gap_analysis.markdown_renderer import (
    render_markdown,
    markdown_report,
)
from tools.universe_os_gap_analysis.model_builder import (
    canonical_reference_ids,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import (
    AssessmentModel,
    CapabilityAssessment,
    CapabilityDomain,
    ConfidenceRating,
    EvidenceStatus,
    ExecutionState,
    GapCategory,
    GapEntry,
    MaturityScore,
    ObservedConclusion,
    Recommendation,
    Severity,
    TargetLevel,
)
from tools.universe_os_gap_analysis.table_renderer import (
    CAPABILITY_MATRIX_JSON,
    GAP_REGISTER_JSON,
    TABLE_RENDERERS,
    capability_matrix_rows,
    check_table_parity,
    gap_register_rows,
    render_tables,
)
from tools.universe_os_gap_analysis.validator import validate_assessment_model

from .test_validator import build_valid_model

# The extra objects added by the strategy always attach to these base objects,
# which build_valid_model() is guaranteed to define.
_BASE_DOMAIN = "domain-hosted"
_BASE_GAP = "gap-hosted"
_BASE_GATE = "gate-hosted"
_BASE_EVIDENCE = "ev-hosted"


def _extra_domain_and_assessment(
    index: int,
    *,
    raw: MaturityScore,
    effective: MaturityScore,
) -> tuple[CapabilityDomain, CapabilityAssessment]:
    """A self-consistent T0 domain plus its single matching assessment.

    T0 (hosted adjacency) is not an OS-substrate level, so a non-zero maturity is
    legal as long as the assessment cites direct evidence -- it references the
    base evidence record, and its next Hard-Gate is the base gate, so every
    reference resolves and the domain/assessment parity stays one-to-one.
    """

    domain = CapabilityDomain(
        id=StableId(f"domain-extra-{index}"),
        name=f"Extra hosted domain {index}",
        target_level=TargetLevel.T0_HOSTED_ADJACENCY,
        description=f"Extra hosted-adjacency capability domain {index}.",
        mandatory_for_target=False,
        evidence_ids=(_BASE_EVIDENCE,),
    )
    assessment = CapabilityAssessment(
        domain_id=f"domain-extra-{index}",
        raw_score=raw,
        effective_score=effective,
        confidence=ConfidenceRating.MEDIUM,
        evidence_status=EvidenceStatus.COMPILER_TOOLING_GA,
        evidence_ids=(_BASE_EVIDENCE,),
        limitations=(),
        next_hard_gate_id=_BASE_GATE,
        blocking_dependency_ids=(),
        rationale=f"Extra hosted domain {index} backed by base evidence.",
    )
    return domain, assessment


def _extra_gap(
    index: int,
    *,
    primary: GapCategory,
    secondary: tuple[GapCategory, ...],
    current_status: EvidenceStatus,
    target_level: TargetLevel,
    severity: Severity,
    priority: tuple[int, int, int, int],
) -> GapEntry:
    """A self-consistent gap that references only the base domain (no foreign refs)."""

    return GapEntry(
        id=StableId(f"gap-extra-{index}"),
        title=f"Extra gap {index}",
        primary_category=primary,
        secondary_categories=secondary,
        domain_ids=(_BASE_DOMAIN,),
        current_status=current_status,
        target_level=target_level,
        severity=severity,
        dependencies=(),
        acceptance_evidence=(f"Acceptance evidence for extra gap {index}.",),
        recommended_owner_area=f"Owner area {index}",
        dependency_criticality=priority[0],
        safety_impact=priority[1],
        claim_risk=priority[2],
        target_unblock_value=priority[3],
        observed_fact=f"Observed fact for extra gap {index}.",
        recommendation=f"Recommendation for extra gap {index}.",
    )


@st.composite
def valid_models(draw: st.DrawFn) -> AssessmentModel:
    """Generate legal canonical models as valid structural variations of the base.

    Starts from :func:`build_valid_model` and layers additive, internally
    consistent variation on top: extra domains (with matching assessments), extra
    gaps, extra observed conclusions/recommendations, extra assumption/non-claim
    text, and an optional toggle of the evidence conflict. Every reference the
    extras introduce resolves to a base object, so the result stays publishable.
    """

    base = build_valid_model()

    # --- Extra domains, each with its one matching capability assessment. ----
    n_domains = draw(st.integers(min_value=0, max_value=2))
    extra_domains: list[CapabilityDomain] = []
    extra_assessments: list[CapabilityAssessment] = []
    for index in range(n_domains):
        raw = draw(st.sampled_from(list(MaturityScore)))
        effective = draw(
            st.sampled_from([s for s in MaturityScore if int(s) <= int(raw)])
        )
        domain, assessment = _extra_domain_and_assessment(
            index, raw=raw, effective=effective
        )
        extra_domains.append(domain)
        extra_assessments.append(assessment)

    # --- Extra gaps referencing the base domain. -----------------------------
    n_gaps = draw(st.integers(min_value=0, max_value=3))
    extra_gaps: list[GapEntry] = []
    for index in range(n_gaps):
        primary = draw(st.sampled_from(list(GapCategory)))
        secondary = tuple(
            draw(
                st.sets(
                    st.sampled_from([c for c in GapCategory if c is not primary]),
                    max_size=len(GapCategory) - 1,
                )
            )
        )
        priority = (
            draw(st.integers(min_value=0, max_value=3)),
            draw(st.integers(min_value=0, max_value=3)),
            draw(st.integers(min_value=0, max_value=3)),
            draw(st.integers(min_value=0, max_value=3)),
        )
        extra_gaps.append(
            _extra_gap(
                index,
                primary=primary,
                secondary=secondary,
                current_status=draw(st.sampled_from(list(EvidenceStatus))),
                target_level=draw(st.sampled_from(list(TargetLevel))),
                severity=draw(st.sampled_from(list(Severity))),
                priority=priority,
            )
        )

    # --- Extra observed conclusions and recommendations. ---------------------
    n_conclusions = draw(st.integers(min_value=0, max_value=2))
    extra_conclusions = [
        ObservedConclusion(
            id=StableId(f"conclusion-extra-{index}"),
            text=f"Observed conclusion {index} backed by base evidence.",
            evidence_ids=(_BASE_EVIDENCE,),
        )
        for index in range(n_conclusions)
    ]
    n_recs = draw(st.integers(min_value=0, max_value=2))
    extra_recs = [
        Recommendation(
            id=StableId(f"rec-extra-{index}"),
            text=f"Extra recommendation {index}.",
            related_gap_ids=(_BASE_GAP,),
        )
        for index in range(n_recs)
    ]

    # --- Extra free-text assumptions / non-claims. ---------------------------
    n_assumptions = draw(st.integers(min_value=0, max_value=2))
    extra_assumptions = tuple(
        f"Extra trust assumption {index}." for index in range(n_assumptions)
    )
    n_non_claims = draw(st.integers(min_value=0, max_value=2))
    extra_non_claims = tuple(
        f"Extra non-claim {index}." for index in range(n_non_claims)
    )

    # --- Optionally drop the (optional) evidence conflict. -------------------
    keep_conflict = draw(st.booleans())
    conflicts = base.conflicts if keep_conflict else ()

    return dataclasses.replace(
        base,
        conflicts=conflicts,
        domains=tuple(base.domains) + tuple(extra_domains),
        assessments=tuple(base.assessments) + tuple(extra_assessments),
        gaps=tuple(base.gaps) + tuple(extra_gaps),
        assumptions=tuple(base.assumptions) + extra_assumptions,
        non_claims=tuple(base.non_claims) + extra_non_claims,
        observed_conclusions=tuple(base.observed_conclusions) + tuple(extra_conclusions),
        recommendations=tuple(base.recommendations) + tuple(extra_recs),
    )


def _json_row_ids(artifact_content: bytes, key_column: str) -> list[str]:
    document = json.loads(artifact_content.decode("utf-8"))
    return [str(row[key_column]) for row in document["rows"]]


# **Validates: Requirements 14.2, 14.3, 14.4, 14.5, 14.6, 14.7**
@given(valid_models())
@settings(max_examples=150, deadline=None, print_blob=True)
def test_structured_and_narrative_outputs_are_lossless_projections(
    model: AssessmentModel,
) -> None:
    # Precondition: the strategy only ever yields legal canonical models.
    validation = validate_assessment_model(model)
    assert validation.valid, f"generated an invalid model: {validation.findings}"

    canonical = canonical_reference_ids(model)
    model_domain_ids = {str(domain.id) for domain in model.domains}
    model_gap_ids = {str(gap.id) for gap in model.gaps}

    # ------------------------------------------------------------------ #
    # (a) One-to-one row/object correspondence, both directions.         #
    # ------------------------------------------------------------------ #
    matrix_rows = capability_matrix_rows(model)
    register_rows = gap_register_rows(model)

    matrix_ids = [str(row["domainId"]) for row in matrix_rows]
    register_ids = [str(row["gapId"]) for row in register_rows]

    # Exactly one row per object (no missing, no foreign, no duplicate rows).
    assert len(matrix_ids) == len(model_domain_ids)
    assert set(matrix_ids) == model_domain_ids
    assert len(matrix_ids) == len(set(matrix_ids))

    assert len(register_ids) == len(model_gap_ids)
    assert set(register_ids) == model_gap_ids
    assert len(register_ids) == len(set(register_ids))

    # The rendered JSON tables agree with the in-memory rows (and re-parsing the
    # published artifacts confirms bidirectional parity + no foreign references).
    table_artifacts = render_tables(model)
    by_name = {artifact.name: artifact for artifact in table_artifacts}
    table_domain_ids = _json_row_ids(by_name[CAPABILITY_MATRIX_JSON].content, "domainId")
    table_gap_ids = _json_row_ids(by_name[GAP_REGISTER_JSON].content, "gapId")
    assert set(table_domain_ids) == model_domain_ids
    assert set(table_gap_ids) == model_gap_ids
    assert check_table_parity(model, table_artifacts) == ()

    # ------------------------------------------------------------------ #
    # (b) JSON renderer projects exactly the canonical id set.           #
    # ------------------------------------------------------------------ #
    json_artifact = render_assessment_json(model)
    assert json_artifact.projected_ids == canonical

    # ------------------------------------------------------------------ #
    # (c) Cross-format parity: domain/gap ids agree across all formats.  #
    # ------------------------------------------------------------------ #
    json_document = json.loads(json_artifact.content.decode("utf-8"))
    nodes = json_document["referenceGraph"]["nodes"]
    json_domain_ids = {n["id"] for n in nodes if n["kind"] == "CapabilityDomain"}
    json_gap_ids = {n["id"] for n in nodes if n["kind"] == "GapEntry"}
    assert json_domain_ids == model_domain_ids
    assert json_gap_ids == model_gap_ids

    markdown_artifact = render_markdown(model)
    md_projected = markdown_artifact.projected_ids
    # The narrative cites every domain and every gap (its ids are a superset that
    # includes all evidence/gate/etc. references it also cites).
    assert model_domain_ids <= md_projected
    assert model_gap_ids <= md_projected
    # All three formats expose the same domain/gap id sets.
    assert set(table_domain_ids) == json_domain_ids == (model_domain_ids & md_projected)
    assert set(table_gap_ids) == json_gap_ids == (model_gap_ids & md_projected)

    # ------------------------------------------------------------------ #
    # (d) Material anchors + unexecuted-source disclosure in narrative.  #
    # ------------------------------------------------------------------ #
    report = markdown_report(model)
    # Every material conclusion cites a repository-relative path plus its
    # smallest stable anchor (Requirement 14.4, 14.5).
    for record in model.evidence_records:
        assert record.source_path in report
        assert " ".join(str(record.location.value).split()) in report

    # Every source inspected but not validated by execution is disclosed in the
    # dedicated section (Requirement 14.6).
    assert "## 14. Unvalidated / Unexecuted Evidence" in report
    for entry in model.source_inventory:
        if entry.execution_state is not ExecutionState.VALIDATED:
            assert str(entry.id) in report
            assert entry.path in report

    # ------------------------------------------------------------------ #
    # (e) Observed facts and recommendations stay in separate sections.  #
    # ------------------------------------------------------------------ #
    assert "### Observed facts" in report
    assert "### Recommendations" in report
    # Each observed-facts heading precedes its paired recommendations heading.
    facts_index = report.index("### Observed facts")
    recs_index = report.index("### Recommendations")
    assert facts_index < recs_index
    # The structured gap register keeps the two concerns in distinct fields.
    for row in register_rows:
        assert "observedFact" in row
        assert "recommendation" in row

    # ------------------------------------------------------------------ #
    # (f) No renderer introduces a foreign fact.                         #
    # ------------------------------------------------------------------ #
    renderers = (
        render_assessment_json,
        render_schema,
        *TABLE_RENDERERS,
        render_markdown,
    )
    for renderer in renderers:
        artifact = renderer(model)
        assert artifact.projected_ids <= canonical

    # The all-or-nothing publish gate accepts the full renderer set, confirming
    # every artifact is a lossless, foreign-fact-free projection of the model.
    published = publish_assessment(model, renderers)
    assert published.published, f"publish blocked: {published.findings}"
    assert published.findings == ()
