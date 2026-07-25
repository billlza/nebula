"""Property-based test for target hierarchy / hosted-adjacency isolation.

This module owns the Hypothesis property test for design Property 5:

    Target hierarchy and hosted-adjacency isolation.

It exercises the *real* Task 8.3 target-achievement stage
(:func:`evaluate_target_achievement`) built on the *real* Task 8.1 validated
Hard-Gate graph (:func:`build_hard_gate_graph`) and the *real* Task 8.2 maturity
assessor (:func:`assess_domains`), together with the immutable Task 1.3 catalog
target definitions (:data:`TARGET_LEVEL_DEFINITIONS`). No product code, other
test file, or task metadata is edited, and no mocks are used.

The property has three parts, all proven against the real pipeline:

1. *Exactly six ordered target levels.* Every achievement report -- and the
   catalog target model it is derived from -- contains the six ``TargetLevel``
   values exactly once, ordered T0 (order 0) through T5 (order 5).

2. *Hosted evidence maps to T0_Hosted_Adjacency.* The catalog places the hosted
   level on the ``Hosted_Adjacency`` ownership boundary and every substrate
   level on ``OS_Substrate``; and a freshly added hosted-adjacency domain shows
   up only in the T0 achievement result, never in any T1-T5 result.

3. *Adding hosted-adjacency evidence leaves T1-T5 unchanged.* Adding a fully
   independent hosted-adjacency gate, domain, mandatory requirement, scoped
   blocker, and evidence conflict never changes any T1-T5 substrate
   achievement decision (achieved flag, next Hard-Gate, blocking dependencies,
   unsatisfied/satisfied gates, blocking conflicts, active blockers, unmet
   domains) and never changes any substrate critical-path score (per-gate and
   per-domain effective maturity from the assessor).

To keep the test from being a tautology it (a) drives the *real*
``evaluate_target_achievement`` and ``assess_domains`` over randomly generated
substrate scenarios rather than re-deriving decisions, (b) grounds the "hosted"
premise in the catalog's ``Hosted_Adjacency`` boundary rather than assuming it,
and (c) is accompanied by a deterministic liveness check proving the substrate
projection is genuinely sensitive to a substrate change (so an unchanged
projection is meaningful evidence of isolation, not vacuity).
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.catalog import (
    TARGET_LEVEL_DEFINITIONS,
    CapabilityBoundary,
)
from tools.universe_os_gap_analysis.hard_gate_graph import build_hard_gate_graph
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.maturity import (
    DomainMaturityInput,
    assess_domains,
)
from tools.universe_os_gap_analysis.models import (
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
from tools.universe_os_gap_analysis.target_achievement import (
    ConflictImpact,
    MandatoryDomainRequirement,
    ScopedBlocker,
    TargetAchievementReport,
    evaluate_target_achievement,
)

HOSTED_LEVEL = TargetLevel.T0_HOSTED_ADJACENCY
_SUBSTRATE_LEVELS: tuple[TargetLevel, ...] = tuple(
    level for level in TargetLevel if level is not HOSTED_LEVEL
)
_ALL_SCORES: tuple[MaturityScore, ...] = tuple(MaturityScore)


# --------------------------------------------------------------------------- #
# Shared builders (mirror the Task 8.3 unit tests; no product code touched).  #
# --------------------------------------------------------------------------- #


def _gid(key: str) -> str:
    return str(stable_id("gate", "prop5", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "prop5", key))


def _gate(
    key: str,
    *,
    level: TargetLevel,
    dependencies: tuple[str, ...] = (),
    score: MaturityScore = MaturityScore.ABSENT,
) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=level,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=score,
        dependency_ids=tuple(_gid(dep) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Property 5 Owner",
    )


def _record(
    key: str,
    *,
    kind: EvidenceKind = EvidenceKind.TEST_EXECUTION,
    status: EvidenceStatus = EvidenceStatus.REPO_PREVIEW,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=str(stable_id("evidence", "prop5", key)),
        claim_key=f"claim.{key}",
        claim=f"Claim for {key}",
        status=status,
        source_path=f"src/{key}.nb",
        location=SourceLocation(kind=LocationKind.SYMBOL, value=key),
        revision_ref=str(stable_id("revision", "prop5")),
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _domain_input(domain_key: str, gate_key: str, raw: int) -> DomainMaturityInput:
    """Build a domain input whose direct evidence yields the requested raw score."""

    common = dict(domain_id=_did(domain_key), gate_id=_gid(gate_key))
    if raw == 0:
        return DomainMaturityInput(**common)
    if raw == 1:
        return DomainMaturityInput(
            **common,
            direct_evidence=(
                _record(domain_key, kind=EvidenceKind.SOURCE, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
    evidence = (_record(domain_key),)
    if raw == 2:
        return DomainMaturityInput(**common, direct_evidence=evidence)
    score_three = dict(
        cross_host_candidate_contract=True,
        migration_rollback=True,
        release_review=True,
    )
    if raw == 3:
        return DomainMaturityInput(**common, direct_evidence=evidence, **score_three)
    if raw == 4:
        return DomainMaturityInput(
            **common, direct_evidence=evidence, supported_production=True, **score_three
        )
    return DomainMaturityInput(
        **common,
        direct_evidence=evidence,
        supported_production=True,
        mature_ecosystem=True,
        **score_three,
    )


# --------------------------------------------------------------------------- #
# Scenario generator (substrate-only; T0 additions are applied separately).   #
# --------------------------------------------------------------------------- #


@st.composite
def _substrate_scenario(draw: st.DrawFn) -> dict:
    """A freely varied T1-T5 substrate scenario for the real pipeline.

    One gate and one mandatory domain per included substrate level, chained in
    ascending order so the graph is a valid DAG; each gate/domain carries a
    randomly chosen score, and blocking evidence conflicts, scoped blockers, a
    gate threshold, and the validation flag are all varied.
    """

    included = draw(
        st.lists(st.sampled_from(_SUBSTRATE_LEVELS), min_size=1, max_size=5, unique=True)
    )
    included = sorted(included, key=lambda level: list(TargetLevel).index(level))

    gates: list[HardGate] = []
    inputs: list[DomainMaturityInput] = []
    requirements: list[MandatoryDomainRequirement] = []
    domain_ids: list[str] = []
    prev_gate_key: str | None = None

    for level in included:
        gate_key = f"g_{level.name}"
        domain_key = f"d_{level.name}"
        score = draw(st.sampled_from(_ALL_SCORES))
        # Optionally depend on the previous (strictly lower) included gate.
        deps = (prev_gate_key,) if prev_gate_key is not None and draw(st.booleans()) else ()
        gates.append(_gate(gate_key, level=level, dependencies=deps, score=score))

        raw = draw(st.integers(min_value=0, max_value=5))
        inputs.append(_domain_input(domain_key, gate_key, raw))
        requirements.append(
            MandatoryDomainRequirement(
                domain_id=_did(domain_key),
                target_level=level,
                minimum_maturity=draw(st.sampled_from(_ALL_SCORES)),
            )
        )
        domain_ids.append(_did(domain_key))
        prev_gate_key = gate_key

    # Blocking / non-blocking evidence conflicts touching substrate domains.
    conflict_impacts: list[ConflictImpact] = []
    for index in range(draw(st.integers(min_value=0, max_value=3))):
        touched = draw(
            st.lists(st.sampled_from(domain_ids), min_size=1, max_size=len(domain_ids), unique=True)
        )
        conflict_impacts.append(
            ConflictImpact(
                conflict_id=f"conflict-{index}",
                blocking=draw(st.booleans()),
                domain_ids=tuple(touched),
            )
        )

    # Scoped blockers on included substrate levels.
    scoped_blockers: list[ScopedBlocker] = []
    for index in range(draw(st.integers(min_value=0, max_value=2))):
        level = draw(st.sampled_from(included))
        scoped_blockers.append(
            ScopedBlocker(
                id=f"substrate-blocker-{index}",
                target_level=level,
                reason=f"Substrate blocker {index} on {level.value}.",
                requirement_refs=("15.3",),
                active=draw(st.booleans()),
            )
        )

    return {
        "gates": tuple(gates),
        "inputs": tuple(inputs),
        "requirements": tuple(requirements),
        "conflict_impacts": tuple(conflict_impacts),
        "scoped_blockers": tuple(scoped_blockers),
        "validation_ok": draw(st.booleans()),
        "gate_threshold": draw(st.sampled_from(_ALL_SCORES)),
    }


def _substrate_projection(report: TargetAchievementReport) -> dict:
    """Project every T1-T5 decision into a fully comparable snapshot."""

    snapshot: dict = {}
    for result in report.results:
        if result.level is HOSTED_LEVEL:
            continue
        snapshot[result.level] = (
            result.achieved,
            result.validation_ok,
            None if result.next_hard_gate_id is None else str(result.next_hard_gate_id),
            tuple(str(g) for g in result.blocking_dependency_ids),
            tuple(str(g) for g in result.unsatisfied_gate_ids),
            tuple(str(g) for g in result.satisfied_gate_ids),
            tuple(str(c) for c in result.blocking_conflict_ids),
            tuple(b.id for b in result.active_blockers),
            tuple(str(d.domain_id) for d in result.unmet_mandatory_domains),
            tuple(str(d) for d in result.satisfied_mandatory_domain_ids),
        )
    return snapshot


def _assert_six_ordered_levels(report: TargetAchievementReport) -> None:
    levels = tuple(result.level for result in report.results)
    assert levels == tuple(TargetLevel), levels
    assert tuple(result.order for result in report.results) == tuple(range(6))


# Feature: nebula-universe-os-gap-analysis, Property 5: Target hierarchy and hosted-adjacency isolation - the model has exactly the six ordered target levels, hosted evidence maps to T0_Hosted_Adjacency, and adding hosted-adjacency evidence leaves T1-T5 substrate achievement and critical-path scores unchanged.
# **Validates: Requirements 2.2, 2.3, 15.6**
@given(
    scenario=_substrate_scenario(),
    t0_score=st.sampled_from(_ALL_SCORES),
    t0_raw=st.integers(min_value=0, max_value=5),
    t0_min=st.sampled_from(_ALL_SCORES),
    t0_blocker_active=st.booleans(),
    t0_conflict_blocking=st.booleans(),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_hosted_adjacency_addition_does_not_change_substrate(
    scenario: dict,
    t0_score: MaturityScore,
    t0_raw: int,
    t0_min: MaturityScore,
    t0_blocker_active: bool,
    t0_conflict_blocking: bool,
) -> None:
    """Requirement 2.2/2.3/15.6: T0 hosted evidence cannot move T1-T5."""

    gates = scenario["gates"]
    inputs = scenario["inputs"]
    requirements = scenario["requirements"]
    conflict_impacts = scenario["conflict_impacts"]
    scoped_blockers = scenario["scoped_blockers"]
    validation_ok = scenario["validation_ok"]
    gate_threshold = scenario["gate_threshold"]

    # Baseline: substrate only, driven through the real graph + assessor + stage.
    base_graph = build_hard_gate_graph(gates)
    base_assessment = assess_domains(inputs, base_graph)
    base_report = evaluate_target_achievement(
        assessment=base_assessment,
        graph=base_graph,
        mandatory_requirements=requirements,
        scoped_blockers=scoped_blockers,
        conflict_impacts=conflict_impacts,
        validation_ok=validation_ok,
        gate_threshold=gate_threshold,
    )

    # Grounding: the catalog places T0 on the hosted-adjacency boundary and every
    # substrate level on the OS-substrate boundary. The hosted-adjacency gate and
    # domain we add therefore genuinely represent hosted evidence.
    boundaries = {definition.level: definition.boundary for definition in TARGET_LEVEL_DEFINITIONS}
    assert tuple(definition.level for definition in TARGET_LEVEL_DEFINITIONS) == tuple(TargetLevel)
    assert boundaries[HOSTED_LEVEL] is CapabilityBoundary.HOSTED_ADJACENCY
    for level in _SUBSTRATE_LEVELS:
        assert boundaries[level] is CapabilityBoundary.OS_SUBSTRATE

    # Extend with a fully independent hosted-adjacency gate/domain/requirement,
    # plus a T0-scoped blocker and a T0-touching conflict.
    hosted_domain_id = _did("t0")
    hosted_gate = _gate("t0", level=HOSTED_LEVEL, score=t0_score)
    ext_graph = build_hard_gate_graph(gates + (hosted_gate,))
    ext_assessment = assess_domains(
        inputs + (_domain_input("t0", "t0", t0_raw),), ext_graph
    )
    ext_requirements = requirements + (
        MandatoryDomainRequirement(
            domain_id=hosted_domain_id,
            target_level=HOSTED_LEVEL,
            minimum_maturity=t0_min,
        ),
    )
    ext_conflicts = conflict_impacts + (
        ConflictImpact(
            conflict_id="conflict-hosted",
            blocking=t0_conflict_blocking,
            domain_ids=(hosted_domain_id,),
        ),
    )
    ext_blockers = scoped_blockers + (
        ScopedBlocker(
            id="hosted-blocker",
            target_level=HOSTED_LEVEL,
            reason="Hosted-adjacency scoped blocker.",
            requirement_refs=("15.6",),
            active=t0_blocker_active,
        ),
    )
    ext_report = evaluate_target_achievement(
        assessment=ext_assessment,
        graph=ext_graph,
        mandatory_requirements=ext_requirements,
        scoped_blockers=ext_blockers,
        conflict_impacts=ext_conflicts,
        validation_ok=validation_ok,
        gate_threshold=gate_threshold,
    )

    # 1) Exactly six ordered target levels in both reports.
    _assert_six_ordered_levels(base_report)
    _assert_six_ordered_levels(ext_report)

    # 2) The T1-T5 substrate decisions are byte-for-byte identical.
    assert _substrate_projection(base_report) == _substrate_projection(ext_report)

    # 3) Substrate critical-path scores are unchanged: every substrate gate's
    #    effective score and every substrate domain's effective score is equal.
    substrate_gate_ids = {gate.id for gate in gates}
    for gate_id in substrate_gate_ids:
        assert base_assessment.gate_effective_score_of(gate_id) == (
            ext_assessment.gate_effective_score_of(gate_id)
        ), gate_id
    for inp in inputs:
        assert base_assessment.effective_score_of(inp.domain_id) == (
            ext_assessment.effective_score_of(inp.domain_id)
        ), inp.domain_id

    # 4) Hosted evidence maps to T0 only: the added hosted domain appears in the
    #    T0 result and in no substrate result.
    t0_result = ext_report.result_for(HOSTED_LEVEL)
    t0_domains = {str(d) for d in t0_result.satisfied_mandatory_domain_ids} | {
        str(d.domain_id) for d in t0_result.unmet_mandatory_domains
    }
    assert hosted_domain_id in t0_domains
    assert t0_result.is_hosted_adjacency is True
    for result in ext_report.results:
        if result.level is HOSTED_LEVEL:
            continue
        substrate_domains = {str(d) for d in result.satisfied_mandatory_domain_ids} | {
            str(d.domain_id) for d in result.unmet_mandatory_domains
        }
        assert hosted_domain_id not in substrate_domains, result.level


# Feature: nebula-universe-os-gap-analysis, Property 5: Target hierarchy and hosted-adjacency isolation - deterministic liveness proving the substrate projection is sensitive to a real substrate change, so isolation is non-vacuous.
# **Validates: Requirements 2.2, 2.3, 15.6**
def test_substrate_projection_is_sensitive_to_substrate_change() -> None:
    """A substrate gate change DOES move the substrate projection (non-tautology)."""

    def build(t1_score: MaturityScore) -> TargetAchievementReport:
        gates = (
            _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=t1_score),
            _gate(
                "t2",
                level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
                dependencies=("t1",),
                score=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            ),
        )
        graph = build_hard_gate_graph(gates)
        assessment = assess_domains(
            (_domain_input("d_t2", "t2", 3),), graph
        )
        return evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=(
                MandatoryDomainRequirement(
                    domain_id=_did("d_t2"),
                    target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
                    minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
                ),
            ),
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )

    # With T1 absent, T2's prerequisite gate is unsatisfied; with T1 satisfied it
    # flips. The projection must therefore differ between the two worlds.
    absent = _substrate_projection(build(MaturityScore.ABSENT))
    satisfied = _substrate_projection(build(MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY))
    assert absent != satisfied

    # And T2 is genuinely achieved once its prerequisite is satisfied.
    achieved_report = build(MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY)
    assert achieved_report.is_achieved(TargetLevel.T2_FREESTANDING_SUBSTRATE) is True


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # test to run directly as a fallback.
    test_hosted_adjacency_addition_does_not_change_substrate()
    test_substrate_projection_is_sensitive_to_substrate_change()
    print("Property 5 OK: hosted adjacency stays isolated from the T1-T5 substrate")
