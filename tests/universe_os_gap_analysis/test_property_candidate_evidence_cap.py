"""Property-based test for the repository-local maturity-2 cap (design Property 23).

This module owns the Hypothesis property test for design Property 23:

    Candidate evidence is required to exceed repository-local maturity 2.

It exercises the *real* Task 8.2 maturity assessor -- both
:func:`compute_raw_score` and :func:`assess_domains` -- against the Task 8.1
validated Hard-Gate graph. No product code is edited and no mocks are used.

The property, restated from the design body: for all language/tooling (and, per
Requirement 3.2, every) assessments lacking the three score-3 conditions --
a cross-supported-host candidate contract, migration/rollback evidence, and
release-review evidence -- maturity cannot exceed 2. Only evidence satisfying
*all three* score-3 conditions may remove that cap.

To avoid a tautology the test drives the real assessor with adversarially
generated direct evidence (arbitrary evidence kind / status combinations) and
arbitrary score-3+ signal flags, and then checks the invariant against the
assessor's genuine output:

* the *cap* direction -- ``raw_score > 2`` can only happen when all three
  score-3 conditions are demonstrated (contrapositive of "lacking any of them
  caps at 2"); this is asserted over every generated case;
* the *liveness / non-tautology* direction -- with genuinely repeatable
  direct implementation evidence, supplying all three conditions really does
  lift the score above 2, and removing any single condition drops it back to
  the cap; and
* the effective score computed through the validated graph never exceeds the
  raw score, and the Requirement 15.4 language/tooling limitation is recorded
  exactly when the cap binds a repeatable language/tooling domain.
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

_REPEATABLE_CAP = int(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)  # 2


def _gid(key: str) -> str:
    return str(stable_id("gate", "property-23", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "property-23", key))


def _gate(key: str) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=MaturityScore.ABSENT,
        dependency_ids=(),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Property 23 Owner",
    )


def _record(
    unique: str,
    *,
    kind: EvidenceKind,
    status: EvidenceStatus,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=str(stable_id("evidence", "property-23", unique, kind.value, status.value)),
        claim_key=f"claim.{unique}",
        claim=f"Claim for {unique}",
        status=status,
        source_path=f"src/{unique}.nb",
        location=SourceLocation(kind=LocationKind.SYMBOL, value=unique),
        revision_ref=str(stable_id("revision", "property-23")),
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
def _evidence_record(draw: st.DrawFn) -> EvidenceRecord:
    """A freely varied evidence record (any kind, any status)."""

    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    unique = draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))
    return _record(unique, kind=kind, status=status)


@st.composite
def _domain_input(draw: st.DrawFn) -> DomainMaturityInput:
    """A domain input with arbitrary evidence and arbitrary score-3+ signals."""

    evidence = tuple(
        draw(
            st.lists(
                _evidence_record(),
                max_size=5,
                unique_by=lambda r: str(r.id),
            )
        )
    )
    domain_class = draw(st.sampled_from(tuple(DomainClass)))
    return DomainMaturityInput(
        domain_id=_did(draw(st.from_regex(r"[a-z0-9]{6}", fullmatch=True))),
        gate_id=_gid("g"),
        direct_evidence=evidence,
        domain_class=domain_class,
        cross_host_candidate_contract=draw(st.booleans()),
        migration_rollback=draw(st.booleans()),
        release_review=draw(st.booleans()),
        supported_production=draw(st.booleans()),
        mature_ecosystem=draw(st.booleans()),
    )


# --------------------------------------------------------------------------- #
# Property 23                                                                 #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 23: Candidate evidence is required to exceed repository-local maturity 2 - lacking a cross-supported-host candidate contract, migration/rollback, and release-review evidence, maturity cannot exceed 2, and only evidence satisfying all three score-3 conditions may remove that cap.
# **Validates: Requirements 15.4**
@given(inp=_domain_input())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_maturity_cannot_exceed_two_without_all_score_three_conditions(
    inp: DomainMaturityInput,
) -> None:
    """Requirement 15.4: no score-3 conditions => maturity is capped at 2."""

    raw = compute_raw_score(inp)

    # Cap direction: without all three score-3 conditions, the real assessor
    # never returns a score above the repeatable repository-local rung (2),
    # regardless of evidence strength, supported-production, or ecosystem flags.
    if not inp.score_three_ready:
        assert int(raw) <= _REPEATABLE_CAP

    # Equivalent contrapositive stated directly: a score above 2 can *only* be
    # produced when all three score-3 conditions are demonstrated. This is the
    # "only ... may remove that cap" half of the property.
    if int(raw) > _REPEATABLE_CAP:
        assert inp.score_three_ready

    # The effective score computed through the validated Hard-Gate graph is
    # ordinal and never exceeds the raw score, so the cap propagates.
    graph = build_hard_gate_graph((_gate("g"),))
    result = assess_domains((inp,), graph).result_for(inp.domain_id)
    assert result is not None
    assert result.raw_score == raw
    assert int(result.effective_score) <= int(result.raw_score)
    if not inp.score_three_ready:
        assert int(result.effective_score) <= _REPEATABLE_CAP

    # The named language/tooling cap (Requirement 15.4) is flagged exactly when
    # a language/tooling domain is held at rung 2 for want of the score-3
    # conditions, and the limitation cites the requirement.
    expect_language_cap = (
        inp.domain_class is DomainClass.LANGUAGE_TOOLING
        and not inp.score_three_ready
        and int(raw) == _REPEATABLE_CAP
    )
    assert result.language_tooling_capped == expect_language_cap
    if expect_language_cap:
        assert any("15.4" in limitation for limitation in result.assessment.limitations)


# Feature: nebula-universe-os-gap-analysis, Property 23: Candidate evidence is required to exceed repository-local maturity 2 - liveness: with genuinely repeatable direct implementation evidence, supplying all three score-3 conditions lifts the score above 2, and removing any single condition restores the cap (proving the assessor is not vacuous).
# **Validates: Requirements 15.4**
def test_all_three_conditions_remove_the_cap_and_any_removal_restores_it() -> None:
    """Deterministic non-tautology check on the cap-removal semantics."""

    # Genuinely repeatable, current-worktree direct implementation evidence:
    # a test-execution record on its own establishes the repeatable rung (2).
    repeatable = (
        _record(
            "repeatable",
            kind=EvidenceKind.TEST_EXECUTION,
            status=EvidenceStatus.REPO_PREVIEW,
        ),
    )
    graph = build_hard_gate_graph((_gate("g"),))

    def _score(**flags: bool) -> int:
        inp = DomainMaturityInput(
            domain_id=_did("live"),
            gate_id=_gid("g"),
            direct_evidence=repeatable,
            domain_class=DomainClass.LANGUAGE_TOOLING,
            **flags,
        )
        raw = int(compute_raw_score(inp))
        result = assess_domains((inp,), graph).result_for(_did("live"))
        assert result is not None
        # No blocking dependencies on this gate, so effective == raw.
        assert int(result.effective_score) == raw
        return raw

    # Repeatable evidence alone is capped at exactly 2.
    assert _score() == _REPEATABLE_CAP

    # All three score-3 conditions remove the cap: the score rises above 2.
    all_three = dict(
        cross_host_candidate_contract=True,
        migration_rollback=True,
        release_review=True,
    )
    assert _score(**all_three) > _REPEATABLE_CAP
    assert _score(**all_three) == int(MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT)

    # Removing any single condition restores the cap at 2 -- proving all three
    # are jointly required and none is individually sufficient.
    for missing in all_three:
        partial = {name: value for name, value in all_three.items() if name != missing}
        assert _score(**partial) == _REPEATABLE_CAP


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner; allow direct execution.
    test_maturity_cannot_exceed_two_without_all_score_three_conditions()
    test_all_three_conditions_remove_the_cap_and_any_removal_restores_it()
    print("Property 23 OK: candidate evidence is required to exceed maturity 2")
