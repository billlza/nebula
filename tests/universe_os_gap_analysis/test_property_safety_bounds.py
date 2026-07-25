"""Hypothesis Property 11 test (Task 5.4).

This module drives the *real* memory/ownership/concurrency/unsafe evaluator
(``evaluate_memory_concurrency_safety``) across randomized evidence bundles. It
never re-implements the classifier; every assertion checks the evaluator's own
output against the semantic meaning of the injected evidence, so a passing run
proves the production component (and the Claim Guard it depends on) behaves,
rather than restating a tautology.

The generators compose three independently varied aspects:

* an *ownership* record set (absent / borrow assistance / normative spec /
  normative current implementation / normative but non-current / assistance plus
  normative),
* a *concurrency* record set (absent / hosted cooperative async /
  scheduler-independent current implementation / scheduler-independent but
  non-current), and
* a disclosed *exclusion* set (any subset of opaque/dynamic/FFI/unsafe) attached
  to an unsafe/FFI record's ``limitations``.

Each aspect is drawn from concrete evidence templates whose classification is
known from Requirement 6, and the test asserts the evaluator reproduces that
classification. The status/kind/origin fields for the "current" and
"non-current" templates are themselves randomized within the sets that do and do
not license present-tense implementation claims, which exercises the Claim
Guard's gating (not just substring matching).
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.evaluators.memory_concurrency_safety import (
    ConcurrencyModelStrength,
    SafetyModelStrength,
    evaluate_memory_concurrency_safety,
)
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-property-11")

# Capability IDs under assessment (mirrors the MEMORY_SAFETY_CHECKLIST keys).
_OWNERSHIP = "capability-ownership-borrow-model"
_SCHED = "capability-scheduler-independent-concurrency"
_UNSAFE = "capability-unsafe-ffi-boundary"

# Statuses / kinds / origins that DO license a present-tense current
# implementation (must satisfy all three of the Claim Guard's conditions).
_IMPLEMENTED_STATUSES: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
    EvidenceStatus.INSTALLED_PREVIEW,
    EvidenceStatus.REPO_PREVIEW,
    EvidenceStatus.EXPERIMENTAL,
)
_DIRECT_KINDS: tuple[EvidenceKind, ...] = (
    EvidenceKind.SOURCE,
    EvidenceKind.TEST_EXECUTION,
    EvidenceKind.ARTIFACT,
)
_CURRENT_ORIGINS: tuple[RevisionOrigin, ...] = (
    RevisionOrigin.COMMITTED_REVISION,
    RevisionOrigin.CURRENT_WORKTREE,
)

# The disclosed opaque/dynamic/FFI/unsafe exclusions (Requirement 6.6). Each
# fragment contains the production evaluator's detection marker for its category
# and is mutually non-overlapping so a subset triggers exactly those categories.
_EXCLUSION_FRAGMENTS: dict[str, str] = {
    "opaque": "The safety guarantee excludes opaque values.",
    "dynamic": "The safety guarantee excludes dynamic dispatch.",
    "ffi": "The safety guarantee excludes ffi boundaries.",
    "unsafe": "The safety guarantee excludes unsafe blocks.",
}


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    evidence_kind: EvidenceKind,
    origin: RevisionOrigin,
    source_path: str,
    limitations: tuple[str, ...] = (),
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, evidence_kind.value, origin.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=limitations,
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key[record.claim_key] = by_claim_key.get(record.claim_key, ()) + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


# --------------------------------------------------------------------------- #
# Ownership aspect                                                            #
# --------------------------------------------------------------------------- #

_ASSISTANCE_CLAIM = (
    "Rep x Owner inference and conservative borrow assistance track ownership and "
    "exclusivity."
)
_NORMATIVE_CLAIM = (
    "A normative move, borrow, lifetime, and aliasing model with a borrow checker "
    "governs ownership."
)


@st.composite
def _ownership_aspect(draw: st.DrawFn) -> tuple[list[EvidenceRecord], SafetyModelStrength]:
    mode = draw(
        st.sampled_from(
            (
                "none",
                "assistance_only",
                "normative_spec",
                "normative_current",
                "normative_noncurrent",
                "assistance_and_normative_spec",
            )
        )
    )
    if mode == "none":
        return [], SafetyModelStrength.ABSENT

    if mode == "assistance_only":
        record = _record(
            claim_key="source:spec/rep_owner_model.md#assist",
            claim=_ASSISTANCE_CLAIM,
            status=EvidenceStatus.EXPERIMENTAL,
            evidence_kind=EvidenceKind.SOURCE,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path="spec/rep_owner_model.md",
        )
        return [record], SafetyModelStrength.ASSISTANCE_ONLY

    if mode == "normative_spec":
        record = _record(
            claim_key="source:spec/rep_owner_model.md#norm-spec",
            claim=_NORMATIVE_CLAIM,
            status=EvidenceStatus.EXPERIMENTAL,
            evidence_kind=EvidenceKind.SPECIFICATION,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path="spec/rep_owner_model.md",
        )
        return [record], SafetyModelStrength.NORMATIVE

    if mode == "normative_current":
        record = _record(
            claim_key="source:spec/rep_owner_model.md#norm-cur",
            claim=_NORMATIVE_CLAIM,
            status=draw(st.sampled_from(_IMPLEMENTED_STATUSES)),
            evidence_kind=draw(st.sampled_from(_DIRECT_KINDS)),
            origin=draw(st.sampled_from(_CURRENT_ORIGINS)),
            source_path="passes/borrow_checker.cpp",
        )
        return [record], SafetyModelStrength.NORMATIVE

    if mode == "normative_noncurrent":
        # Normative markers present, but the evidence does not license a
        # present-tense current implementation (planned, tagged, or a non-direct
        # kind) and is not a specification, so it must NOT count as normative.
        status, kind, origin = draw(
            st.sampled_from(
                (
                    (EvidenceStatus.PLANNED, EvidenceKind.SOURCE, RevisionOrigin.CURRENT_WORKTREE),
                    (EvidenceStatus.REPO_PREVIEW, EvidenceKind.SOURCE, RevisionOrigin.TAGGED_RELEASE),
                    (EvidenceStatus.REPO_PREVIEW, EvidenceKind.RFC, RevisionOrigin.CURRENT_WORKTREE),
                )
            )
        )
        record = _record(
            claim_key="source:spec/rep_owner_model.md#norm-noncur",
            claim=_NORMATIVE_CLAIM,
            status=status,
            evidence_kind=kind,
            origin=origin,
            source_path="spec/rep_owner_model.md",
        )
        return [record], SafetyModelStrength.ABSENT

    # assistance_and_normative_spec: a normative spec always wins over assistance.
    assist = _record(
        claim_key="source:spec/rep_owner_model.md#assist2",
        claim=_ASSISTANCE_CLAIM,
        status=EvidenceStatus.EXPERIMENTAL,
        evidence_kind=EvidenceKind.SOURCE,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        source_path="spec/rep_owner_model.md",
    )
    norm = _record(
        claim_key="source:spec/rep_owner_model.md#norm-spec2",
        claim=_NORMATIVE_CLAIM,
        status=EvidenceStatus.EXPERIMENTAL,
        evidence_kind=EvidenceKind.SPECIFICATION,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        source_path="spec/rep_owner_model.md",
    )
    return [assist, norm], SafetyModelStrength.NORMATIVE


# --------------------------------------------------------------------------- #
# Concurrency aspect                                                          #
# --------------------------------------------------------------------------- #

_HOSTED_ASYNC_CLAIM = (
    "Async is a hosted single-threaded cooperative runtime scheduler with await "
    "points."
)
_SCHED_INDEP_CLAIM = (
    "A scheduler-independent preemptive scheduler runs async tasks without the "
    "hosted runtime."
)


@st.composite
def _concurrency_aspect(
    draw: st.DrawFn,
) -> tuple[list[EvidenceRecord], ConcurrencyModelStrength]:
    mode = draw(
        st.sampled_from(
            (
                "none",
                "hosted_cooperative",
                "scheduler_independent_current",
                "scheduler_independent_noncurrent",
            )
        )
    )
    if mode == "none":
        return [], ConcurrencyModelStrength.ABSENT

    if mode == "hosted_cooperative":
        record = _record(
            claim_key="source:runtime/nebula_runtime.hpp#async",
            claim=_HOSTED_ASYNC_CLAIM,
            status=EvidenceStatus.EXPERIMENTAL,
            evidence_kind=EvidenceKind.SOURCE,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path="runtime/nebula_runtime.hpp",
        )
        return [record], ConcurrencyModelStrength.HOSTED_COOPERATIVE_ONLY

    if mode == "scheduler_independent_current":
        record = _record(
            claim_key="source:runtime/scheduler.cpp#sched",
            claim=_SCHED_INDEP_CLAIM,
            status=draw(st.sampled_from(_IMPLEMENTED_STATUSES)),
            evidence_kind=draw(st.sampled_from(_DIRECT_KINDS)),
            origin=draw(st.sampled_from(_CURRENT_ORIGINS)),
            source_path="runtime/scheduler.cpp",
        )
        return [record], ConcurrencyModelStrength.SCHEDULER_INDEPENDENT

    # scheduler_independent_noncurrent: scheduler-independent markers exist but
    # the evidence does not license a current implementation, so it cannot
    # satisfy the scheduler-independent concurrency capability.
    status, kind, origin = draw(
        st.sampled_from(
            (
                (EvidenceStatus.PLANNED, EvidenceKind.SOURCE, RevisionOrigin.CURRENT_WORKTREE),
                (EvidenceStatus.REPO_PREVIEW, EvidenceKind.SOURCE, RevisionOrigin.TAGGED_RELEASE),
                (EvidenceStatus.REPO_PREVIEW, EvidenceKind.SPECIFICATION, RevisionOrigin.CURRENT_WORKTREE),
            )
        )
    )
    record = _record(
        claim_key="source:runtime/scheduler.cpp#sched-noncur",
        claim=_SCHED_INDEP_CLAIM,
        status=status,
        evidence_kind=kind,
        origin=origin,
        source_path="runtime/scheduler.cpp",
    )
    return [record], ConcurrencyModelStrength.ABSENT


# --------------------------------------------------------------------------- #
# Exclusion aspect                                                            #
# --------------------------------------------------------------------------- #


@st.composite
def _exclusion_aspect(draw: st.DrawFn) -> tuple[EvidenceRecord, tuple[str, ...]]:
    categories = draw(
        st.lists(
            st.sampled_from(tuple(_EXCLUSION_FRAGMENTS)),
            unique=True,
            max_size=len(_EXCLUSION_FRAGMENTS),
        )
    )
    disclosed = tuple(_EXCLUSION_FRAGMENTS[cat] for cat in categories)
    record = _record(
        claim_key="source:spec/safety_contract.md#unsafe",
        claim="Unsafe blocks and FFI boundaries permit raw pointer access.",
        status=EvidenceStatus.EXPERIMENTAL,
        evidence_kind=EvidenceKind.SOURCE,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        source_path="spec/safety_contract.md",
        limitations=disclosed,
    )
    return record, disclosed


# Feature: nebula-universe-os-gap-analysis, Property 11: Safety assistance and
# hosted async stay bounded - Rep x Owner inference / borrow assistance without
# normative move/borrow/lifetime/alias rules cannot satisfy the normative safety
# capability; hosted cooperative async without a scheduler-independent
# implementation creates an Implementation_Gap; and every opaque/dynamic/FFI/
# unsafe exclusion disclosed on evidence is carried into the related gap.
# **Validates: Requirements 6.2, 6.4, 6.6**
@given(
    ownership=_ownership_aspect(),
    concurrency=_concurrency_aspect(),
    exclusion=_exclusion_aspect(),
)
@settings(max_examples=200, deadline=None, print_blob=True)
def test_safety_assistance_and_hosted_async_stay_bounded(
    ownership: tuple[list[EvidenceRecord], SafetyModelStrength],
    concurrency: tuple[list[EvidenceRecord], ConcurrencyModelStrength],
    exclusion: tuple[EvidenceRecord, tuple[str, ...]],
) -> None:
    ownership_records, expected_safety = ownership
    concurrency_records, expected_concurrency = concurrency
    unsafe_record, disclosed = exclusion

    bundle = _bundle([*ownership_records, *concurrency_records, unsafe_record])
    result = evaluate_memory_concurrency_safety(bundle)

    # -- Requirement 6.2 / Property 11: assistance never satisfies -------- #
    assert result.safety_model_strength == expected_safety
    ownership_draft = result.draft_for(_OWNERSHIP)
    assert ownership_draft is not None
    ownership_satisfied = expected_safety is SafetyModelStrength.NORMATIVE
    assert ownership_draft.satisfied == ownership_satisfied
    ownership_gap = result.gap_for(_OWNERSHIP)
    if ownership_satisfied:
        assert ownership_gap is None
    else:
        # Rep x Owner / borrow assistance (or absence) leaves a Language_Gap.
        assert ownership_gap is not None
        assert ownership_gap.primary_category is GapCategory.LANGUAGE

    # -- Requirement 6.4: hosted async is an Implementation_Gap ----------- #
    assert result.concurrency_model_strength == expected_concurrency
    sched_draft = result.draft_for(_SCHED)
    assert sched_draft is not None
    sched_satisfied = expected_concurrency is ConcurrencyModelStrength.SCHEDULER_INDEPENDENT
    assert sched_draft.satisfied == sched_satisfied
    sched_gap = result.gap_for(_SCHED)
    if sched_satisfied:
        assert sched_gap is None
    else:
        # Hosted cooperative async (or no implementation) yields an
        # Implementation_Gap at the freestanding-substrate target level.
        assert sched_gap is not None
        assert sched_gap.primary_category is GapCategory.IMPLEMENTATION
        assert sched_gap.target_level is TargetLevel.T2_FREESTANDING_SUBSTRATE

    # -- Requirement 6.6: disclosed exclusions are preserved -------------- #
    unsafe_gap = result.gap_for(_UNSAFE)
    assert unsafe_gap is not None  # unsafe/FFI boundary is never satisfied here
    unsafe_draft = result.draft_for(_UNSAFE)
    assert unsafe_draft is not None
    acceptance_text = "\n".join(unsafe_gap.acceptance_evidence)
    for exclusion_text in disclosed:
        assert exclusion_text in unsafe_draft.limitations
        assert exclusion_text in acceptance_text


# Feature: nebula-universe-os-gap-analysis, Property 11: Safety assistance and
# hosted async stay bounded - a focused check that at least one disclosed
# opaque/dynamic/FFI/unsafe exclusion is always propagated verbatim into the
# unsafe/FFI boundary gap's limitations and acceptance evidence.
# **Validates: Requirements 6.2, 6.4, 6.6**
@given(
    categories=st.lists(
        st.sampled_from(tuple(_EXCLUSION_FRAGMENTS)),
        unique=True,
        min_size=1,
        max_size=len(_EXCLUSION_FRAGMENTS),
    )
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_disclosed_exclusions_are_carried_into_the_unsafe_gap(
    categories: list[str],
) -> None:
    disclosed = tuple(_EXCLUSION_FRAGMENTS[cat] for cat in categories)
    unsafe_record = _record(
        claim_key="source:spec/safety_contract.md#unsafe-focus",
        claim="Unsafe blocks and FFI boundaries permit raw pointer access.",
        status=EvidenceStatus.EXPERIMENTAL,
        evidence_kind=EvidenceKind.SOURCE,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        source_path="spec/safety_contract.md",
        limitations=disclosed,
    )
    result = evaluate_memory_concurrency_safety(_bundle([unsafe_record]))

    gap = result.gap_for(_UNSAFE)
    assert gap is not None
    draft = result.draft_for(_UNSAFE)
    assert draft is not None
    acceptance_text = "\n".join(gap.acceptance_evidence)
    for exclusion_text in disclosed:
        assert exclusion_text in draft.limitations
        assert exclusion_text in acceptance_text
