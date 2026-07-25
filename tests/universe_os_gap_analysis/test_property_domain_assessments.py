"""Property-based test for domain-assessment completeness and ordinality.

This module owns the Hypothesis property test for design Property 6:

    Domain assessments are complete, ordinal, and non-additive.

It exercises the *real* Task 8.2 maturity assessor
(:func:`tools.universe_os_gap_analysis.maturity.assess_domains`) against the
*real* Task 8.1 validated Hard-Gate graph
(:func:`tools.universe_os_gap_analysis.hard_gate_graph.build_hard_gate_graph`).
No product code is edited and no mocks are used.

For every randomly generated capability-domain set the assessor must produce
exactly one :class:`CapabilityAssessment` per domain, each carrying a score,
confidence, status, evidence, limitations, next gate, and blocking dependencies;
every score must be an integer in ``0..5``; and the result must expose no
aggregate percentage, average, sum, or schedule estimate.

To keep the test from being a tautology it re-derives the effective score
independently from the assessor's own per-gate effective scores
(``effective = min(raw, blocking dependency effective scores)``), links every
assessment's evidence references back to the exact input records, and includes a
deterministic liveness check proving the assessor genuinely computes non-trivial
scores (a strong domain earns credit, a no-evidence domain stays at 0, and a
blocking dependency actually caps a domain).
"""

from __future__ import annotations

from hypothesis import given, settings
from hypothesis import strategies as st

from tools.universe_os_gap_analysis.hard_gate_graph import build_hard_gate_graph
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.maturity import (
    DomainClass,
    DomainMaturityInput,
    assess_domains,
    compute_raw_score,
)
from tools.universe_os_gap_analysis.models import (
    CapabilityAssessment,
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    HardGate,
    LocationKind,
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_MIN_SCORE = int(MaturityScore.ABSENT)
_MAX_SCORE = int(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM)

# Attribute names that would indicate a forbidden aggregate / percentage /
# average / schedule output. Property 6 requires the assessor to be ordinal and
# non-additive, so none of these may appear on the result or any assessment.
_FORBIDDEN_AGGREGATE_ATTRS: tuple[str, ...] = (
    "percentage",
    "percent",
    "average",
    "mean",
    "total",
    "sum",
    "aggregate",
    "overall",
    "overall_score",
    "completion",
    "completion_percent",
    "schedule",
    "estimate",
    "eta",
    "duration",
    "timeline",
)


def _gid(key: str) -> str:
    return str(stable_id("gate", "prop6", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "prop6", key))


def _gate(key: str, *, dependencies: tuple[str, ...], score: int) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=MaturityScore(score),
        dependency_ids=tuple(_gid(dep) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Test Owner",
    )


def _record(
    unique: str,
    *,
    kind: EvidenceKind,
    status: EvidenceStatus,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=str(stable_id("evidence", "prop6", unique)),
        claim_key=f"claim.{unique}",
        claim=f"Claim for {unique}",
        status=status,
        source_path=f"src/{unique}.nb",
        location=SourceLocation(kind=LocationKind.SYMBOL, value=unique),
        revision_ref=str(stable_id("revision", "prop6")),
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


# --------------------------------------------------------------------------- #
# Generators                                                                  #
# --------------------------------------------------------------------------- #


@st.composite
def _evidence_records(draw: st.DrawFn, prefix: str) -> tuple[EvidenceRecord, ...]:
    """Generate a small, freely varied set of direct-evidence records."""

    count = draw(st.integers(min_value=0, max_value=3))
    records: list[EvidenceRecord] = []
    for i in range(count):
        kind = draw(st.sampled_from(tuple(EvidenceKind)))
        status = draw(st.sampled_from(tuple(EvidenceStatus)))
        records.append(
            _record(f"{prefix}-{i}", kind=kind, status=status)
        )
    return tuple(records)


@st.composite
def _graph_and_domains(
    draw: st.DrawFn,
) -> tuple[tuple[HardGate, ...], tuple[DomainMaturityInput, ...]]:
    """Build a random valid Hard-Gate DAG plus a set of capability domains.

    Gate ``i`` may only depend on strictly lower-indexed gates, which guarantees
    an acyclic graph with no self edges or duplicate edges, so the real graph
    builder accepts it and the real assessor runs end to end.
    """

    n_gates = draw(st.integers(min_value=1, max_value=5))
    gates: list[HardGate] = []
    for i in range(n_gates):
        if i == 0:
            deps: tuple[str, ...] = ()
        else:
            dep_indices = draw(
                st.sets(st.integers(min_value=0, max_value=i - 1), max_size=i)
            )
            deps = tuple(f"g{j}" for j in sorted(dep_indices))
        score = draw(st.integers(min_value=_MIN_SCORE, max_value=_MAX_SCORE))
        gates.append(_gate(f"g{i}", dependencies=deps, score=score))

    gate_keys = [f"g{i}" for i in range(n_gates)]

    n_domains = draw(st.integers(min_value=1, max_value=6))
    inputs: list[DomainMaturityInput] = []
    for d in range(n_domains):
        gate_key = draw(st.sampled_from(gate_keys))
        next_gate_key = draw(st.sampled_from([None] + gate_keys))
        inputs.append(
            DomainMaturityInput(
                domain_id=_did(f"d{d}"),
                gate_id=_gid(gate_key),
                direct_evidence=draw(_evidence_records(f"d{d}")),
                domain_class=draw(st.sampled_from(tuple(DomainClass))),
                confidence=draw(st.sampled_from(tuple(ConfidenceRating))),
                evidence_status=draw(st.sampled_from(tuple(EvidenceStatus))),
                limitations=draw(
                    st.lists(
                        st.sampled_from(
                            ("known limitation A", "known limitation B")
                        ),
                        max_size=2,
                        unique=True,
                    ).map(tuple)
                ),
                cross_host_candidate_contract=draw(st.booleans()),
                migration_rollback=draw(st.booleans()),
                release_review=draw(st.booleans()),
                supported_production=draw(st.booleans()),
                mature_ecosystem=draw(st.booleans()),
                next_hard_gate_id=None if next_gate_key is None else _gid(next_gate_key),
            )
        )

    return tuple(gates), tuple(inputs)


def _assert_no_aggregate(obj: object) -> None:
    for attr in _FORBIDDEN_AGGREGATE_ATTRS:
        assert not hasattr(obj, attr), f"unexpected aggregate attribute: {attr!r}"


# Feature: nebula-universe-os-gap-analysis, Property 6: Domain assessments are complete, ordinal, and non-additive - for every capability-domain set there is exactly one assessment per domain containing score, confidence, status, evidence, limitations, next gate, and dependencies; every score is an integer in 0..5; and no aggregate percentage, average, or schedule estimate is produced.
# **Validates: Requirements 3.1, 3.2, 3.7**
@given(_graph_and_domains())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_domain_assessments_are_complete_ordinal_and_non_additive(
    graph_and_domains: tuple[tuple[HardGate, ...], tuple[DomainMaturityInput, ...]],
) -> None:
    """Requirement 3.1/3.2/3.7: complete, ordinal, non-additive per-domain output."""

    gates, inputs = graph_and_domains
    graph = build_hard_gate_graph(gates)

    assessment = assess_domains(inputs, graph)
    assessments = assessment.assessments

    # -- Exactly one assessment per domain (Requirement 3.1). ------------- #
    input_ids = [str(inp.domain_id) for inp in inputs]
    output_ids = [str(a.domain_id) for a in assessments]
    assert len(output_ids) == len(input_ids)
    assert len(set(output_ids)) == len(output_ids)  # no duplicates
    assert set(output_ids) == set(input_ids)  # bijection, order-independent

    # -- The result object exposes no aggregate / schedule output (3.7). -- #
    _assert_no_aggregate(assessment)

    inputs_by_id = {str(inp.domain_id): inp for inp in inputs}

    for capability in assessments:
        assert isinstance(capability, CapabilityAssessment)
        domain_id = str(capability.domain_id)
        inp = inputs_by_id[domain_id]

        # -- Every required field is present (Requirement 3.1). ----------- #
        assert domain_id.strip()
        assert isinstance(capability.raw_score, MaturityScore)
        assert isinstance(capability.effective_score, MaturityScore)
        assert isinstance(capability.confidence, ConfidenceRating)
        assert isinstance(capability.evidence_status, EvidenceStatus)
        assert isinstance(capability.evidence_ids, tuple)
        assert isinstance(capability.limitations, tuple)
        assert str(capability.next_hard_gate_id).strip()
        assert isinstance(capability.blocking_dependency_ids, tuple)
        assert capability.rationale.strip()

        # Evidence references trace back exactly to the input's direct
        # evidence -- the assessor genuinely carries them through (non-trivial).
        assert len(capability.evidence_ids) == len(inp.direct_evidence)
        expected_evidence = {str(rec.id) for rec in inp.direct_evidence}
        assert {str(ref) for ref in capability.evidence_ids} == expected_evidence

        # The confidence / status / next-gate fields echo the input contract.
        assert capability.confidence is inp.confidence
        assert capability.evidence_status is inp.evidence_status
        expected_next = inp.next_hard_gate_id or inp.gate_id
        assert str(capability.next_hard_gate_id) == str(expected_next)

        # -- Every score is an integer in 0..5, ordinal, effective<=raw. -- #
        for score in (capability.raw_score, capability.effective_score):
            assert isinstance(score, int)  # IntEnum member, never a float/percent
            assert not isinstance(score, bool)
            assert _MIN_SCORE <= int(score) <= _MAX_SCORE
        assert int(capability.effective_score) <= int(capability.raw_score)

        # Scores are ordinal, not additive: no assessment field is a float
        # percentage/average, and no forbidden aggregate attribute exists.
        _assert_no_aggregate(capability)

        # -- Raw score matches the real raw-score computation. ------------ #
        assert capability.raw_score == compute_raw_score(inp)

        # -- Effective score is exactly min(raw, blocking-dependency
        #    effective scores), re-derived from the assessor's own per-gate
        #    effective scores rather than assumed (non-tautology). --------- #
        floor = int(capability.raw_score)
        blocking = set()
        for dep in capability.blocking_dependency_ids:
            dep_id = str(dep)
            blocking.add(dep_id)
            floor = min(floor, int(assessment.gate_effective_score_of(dep_id)))
        assert int(capability.effective_score) == floor
        # blocking_dependency_ids must be exactly the gate's blocking deps.
        graph_blocking = {
            str(dep) for dep in graph.blocking_dependencies_of(str(inp.gate_id))
        }
        assert blocking == graph_blocking

    # -- Every gate effective score is itself an ordinal 0..5. ------------ #
    for gate_id in graph.gate_ids:
        eff = assessment.gate_effective_score_of(gate_id)
        assert isinstance(eff, int) and not isinstance(eff, bool)
        assert _MIN_SCORE <= int(eff) <= _MAX_SCORE


# Feature: nebula-universe-os-gap-analysis, Property 6: Domain assessments are complete, ordinal, and non-additive - deterministic liveness proving the assessor computes real, non-trivial ordinal scores (strong domain earns credit, no-evidence domain stays 0, blocking dependency caps).
# **Validates: Requirements 3.1, 3.2, 3.7**
def test_assessor_is_live_and_non_tautological() -> None:
    """A deterministic liveness check proving the assessor is not vacuous."""

    gates = (
        _gate("dep", dependencies=(), score=int(MaturityScore.NARROW_EXPERIMENT)),
        _gate("open", dependencies=(), score=int(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM)),
        _gate("blocked", dependencies=("dep",), score=int(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM)),
    )
    graph = build_hard_gate_graph(gates)

    strong_evidence = (
        _record("strong", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.REPO_PREVIEW),
    )
    inputs = (
        # No evidence -> raw and effective 0 regardless of gate scores.
        DomainMaturityInput(domain_id=_did("empty"), gate_id=_gid("open")),
        # Strong evidence on an unblocked gate -> earns repeatable maturity 2.
        DomainMaturityInput(
            domain_id=_did("strong"),
            gate_id=_gid("open"),
            direct_evidence=strong_evidence,
        ),
        # Same strong evidence, but a blocking dependency at score 1 caps it.
        DomainMaturityInput(
            domain_id=_did("capped"),
            gate_id=_gid("blocked"),
            direct_evidence=strong_evidence,
        ),
    )

    assessment = assess_domains(inputs, graph)

    empty = assessment.result_for(_did("empty"))
    strong = assessment.result_for(_did("strong"))
    capped = assessment.result_for(_did("capped"))
    assert empty is not None and strong is not None and capped is not None

    assert empty.raw_score == MaturityScore.ABSENT
    assert empty.effective_score == MaturityScore.ABSENT

    assert strong.raw_score == MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
    assert strong.effective_score == MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION

    assert capped.raw_score == MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
    assert capped.effective_score == MaturityScore.NARROW_EXPERIMENT

    # Non-additive: the assessor exposes no percentage/average/schedule output,
    # and its rationale states the non-additive/no-schedule guarantee.
    _assert_no_aggregate(assessment)
    assert "non-additive" in strong.assessment.rationale.lower()
    assert "percentage" in strong.assessment.rationale.lower()


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # tests to be executed directly as a fallback.
    test_domain_assessments_are_complete_ordinal_and_non_additive()
    test_assessor_is_live_and_non_tautological()
    print("Property 6 OK: domain assessments are complete, ordinal, and non-additive")
