from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.claim_guard import (
    ClaimGuard,
    ClaimTense,
    guard_evidence,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle, decide_status
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

# --------------------------------------------------------------------------- #
# Independent oracles                                                         #
#                                                                             #
# These re-derive the expected behaviour from the *requirement text*, not     #
# from the implementation, so the property is not a tautology: the test's     #
# oracle and the Claim Guard are computed separately and then compared.       #
# --------------------------------------------------------------------------- #

_REVISION_REF = reference("revision-property-21")

# Statuses that assert a present-tense (current) implementation to some scoped
# degree. Anything outside this set can never license present tense.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Requirement 13.1: present tense needs *implementation evidence from the bound
# revision*. Only direct implementation/executable evidence kinds count.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# ...and only origins that belong to the bound Assessment_Revision's current
# state. A published tagged release describes an artifact, not current-revision
# implementation, so it never licenses present tense.
_CURRENT_REVISION_ORIGINS: frozenset[RevisionOrigin] = frozenset(
    {
        RevisionOrigin.COMMITTED_REVISION,
        RevisionOrigin.CURRENT_WORKTREE,
        RevisionOrigin.EXECUTION_ARTIFACT,
    }
)

# Requirement 13.3: examples and documentation-only contracts are labelled by
# their strongest directly supported status; being documentation, none of these
# kinds is direct implementation evidence, so present tense is never licensed.
_DOCS_AND_EXAMPLE_KINDS: frozenset[EvidenceKind] = frozenset(
    {
        EvidenceKind.EXAMPLE,
        EvidenceKind.SPECIFICATION,
        EvidenceKind.RFC,
        EvidenceKind.RELEASE,
        EvidenceKind.WORKFLOW,
        EvidenceKind.TEST_DEFINITION,
        EvidenceKind.NON_CLAIM,
    }
)

# Topics that must stay explicit non-claims until an accepted gate exists.
_MANDATORY_TOPICS: tuple[str, ...] = (
    "kernel",
    "driver",
    "interrupt",
    "mmu",
    "scheduler",
    "syscall-abi",
    "freestanding-runtime",
    "bootability",
    "backend-independence",
)


def _oracle_present_tense(
    status: EvidenceStatus, kind: EvidenceKind, origin: RevisionOrigin
) -> bool:
    """Requirement 13.1, derived independently from the implementation."""

    return (
        status in _IMPLEMENTED_STATUSES
        and kind in _DIRECT_IMPLEMENTATION_KINDS
        and origin in _CURRENT_REVISION_ORIGINS
    )


# --------------------------------------------------------------------------- #
# Generators                                                                  #
# --------------------------------------------------------------------------- #

_paths = st.lists(
    st.from_regex(r"[a-z0-9_]{1,8}", fullmatch=True), min_size=1, max_size=3
).map("/".join)
_text = (
    st.text(
        alphabet=st.characters(min_codepoint=0x20, max_codepoint=0x7E),
        min_size=1,
        max_size=40,
    )
    .map(str.strip)
    .filter(bool)
    # Avoid accidental primitive-object / host-compiler marker words so the
    # record keeps its ordinary wording path; those special wordings are
    # exercised by the Claim Guard unit tests, not by this property.
    .filter(lambda value: not any(
        marker in value.lower()
        for marker in (
            "primitive", "et_rel", "relocatable", "freestanding object",
            "clang", "host compiler", "host toolchain", "external",
        )
    ))
)


@st.composite
def _records(draw: st.DrawFn) -> EvidenceRecord:
    """Draw a valid Evidence_Record spanning every status/kind/origin."""

    status = draw(st.sampled_from(tuple(EvidenceStatus)))
    kind = draw(st.sampled_from(tuple(EvidenceKind)))
    origin = draw(st.sampled_from(tuple(RevisionOrigin)))
    claim_key = draw(
        st.from_regex(r"cap:[a-z0-9]{1,10}(-[a-z0-9]{1,10}){0,2}", fullmatch=True)
    )
    source_path = draw(_paths) + draw(st.sampled_from([".md", ".nb", ".py", ".toml"]))
    scope = EvidenceScope(
        target_levels=tuple(
            draw(
                st.lists(
                    st.sampled_from(tuple(TargetLevel)), max_size=3, unique=True
                )
            )
        )
    )
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, status.value, kind.value, origin.value),
        claim_key=claim_key,
        claim=draw(_text),
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}"),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=scope,
        limitations=(),
        trust_assumptions=(),
        verification_state=draw(st.sampled_from(tuple(VerificationState))),
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


# --------------------------------------------------------------------------- #
# Property 21                                                                 #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 21: Present-tense claims require direct current evidence - present-tense implementation wording is permitted only with implementation evidence from the bound revision; docs/examples cannot exceed their strongest direct scoped status; pathless claims are Unknown; and explicit non-claims persist until their own accepted gate exists.
# **Validates: Requirements 13.1, 13.3, 13.5, 13.6**
@given(records=st.lists(_records(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=150, deadline=None, print_blob=True)
def test_present_tense_only_for_direct_current_revision_evidence(
    records: list[EvidenceRecord],
) -> None:
    """Requirement 13.1 + 13.3: exercise the real Claim Guard wording gate.

    Present tense is permitted iff the record carries an implemented status via a
    direct implementation/executable kind from a current-revision origin. The
    guarded status must always equal the record's own status verbatim (never an
    upgrade), and documentation/example evidence can never claim present tense.
    """

    guarded = guard_evidence(_bundle(records))

    for record in records:
        claim = guarded.claim_for(record.id)
        assert claim is not None

        expected = _oracle_present_tense(
            record.status, record.evidence_kind, record.origin
        )
        # 13.1: the real guard agrees with the independently derived oracle.
        assert claim.present_tense_permitted is expected
        # Present tense is emitted only when permitted (never fabricated).
        assert (claim.tense is ClaimTense.PRESENT) == expected

        # 13.3 (no upgrade): the guarded status is exactly the record's status;
        # documentation/example evidence is never asserted in present tense and
        # therefore cannot exceed its strongest directly supported status.
        assert claim.status is record.status
        if record.evidence_kind in _DOCS_AND_EXAMPLE_KINDS:
            assert claim.present_tense_permitted is False
            assert claim.tense is not ClaimTense.PRESENT
        # An example is documentation-only: it can only ever describe hosted
        # adjacency and never raises OS-substrate maturity.
        if record.evidence_kind is EvidenceKind.EXAMPLE:
            assert claim.substrate_promotion_blocked is True


# Feature: nebula-universe-os-gap-analysis, Property 21: Present-tense claims require direct current evidence - a claim without a verifiable source path is classified Unknown regardless of any proposed status or plan/negative/audited signal.
# **Validates: Requirements 13.1, 13.3, 13.5, 13.6**
@given(
    proposed_status=st.one_of(
        st.none(),
        st.sampled_from(
            [status for status in EvidenceStatus if status is not EvidenceStatus.UNSUPPORTED]
        ),
    ),
    plan_only=st.booleans(),
)
@settings(max_examples=150, deadline=None, print_blob=True)
def test_pathless_claims_are_unknown(
    proposed_status: EvidenceStatus | None, plan_only: bool
) -> None:
    """Requirement 13.5: exercise the real classifier on pathless claims.

    Without a verifiable source path the classification is Unknown, no matter
    what status a caller proposes or whether the prose is plan-only. This is
    non-tautological: the proposed status ranges over every implemented and
    planned tier and the real decision order overrides all of them.
    """

    resolved = decide_status(
        source_path=None,
        plan_only=plan_only,
        negative_claim=False,
        audited_absence=False,
        proposed_status=proposed_status,
    )
    assert resolved is EvidenceStatus.UNKNOWN

    # A path-bearing counterpart with the same signals must *not* collapse to
    # Unknown for an implemented proposal, proving the pathless rule is what
    # forces Unknown rather than some unrelated default.
    with_path = decide_status(
        source_path="spec/example.md",
        plan_only=plan_only,
        negative_claim=False,
        audited_absence=False,
        proposed_status=proposed_status,
    )
    if plan_only:
        assert with_path is EvidenceStatus.PLANNED
    elif proposed_status is not None:
        assert with_path is proposed_status


# Feature: nebula-universe-os-gap-analysis, Property 21: Present-tense claims require direct current evidence - explicit OS-substrate non-claims persist until their own accepted gate exists, and accepting one topic releases only that topic.
# **Validates: Requirements 13.1, 13.3, 13.5, 13.6**
@given(
    accepted=st.lists(st.sampled_from(_MANDATORY_TOPICS), unique=True),
)
@settings(max_examples=150, deadline=None, print_blob=True)
def test_non_claims_persist_until_their_own_accepted_gate(
    accepted: list[str],
) -> None:
    """Requirement 13.6: exercise the real Claim Guard non-claim persistence.

    Every mandatory OS-substrate topic without an accepted gate must remain an
    explicit non-claim; accepting a topic releases exactly that topic and no
    other. The accepted set is drawn adversarially over all subsets.
    """

    record = EvidenceRecord(
        id=stable_id("evidence", "cap:anchor"),
        claim_key="cap:anchor",
        claim="An unrelated experimental capability.",
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="spec/anchor.md",
        location=SourceLocation(kind=LocationKind.HEADING, value="File:spec/anchor.md"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )

    guarded = ClaimGuard().guard(
        _bundle([record]), accepted_gate_topics=tuple(accepted)
    )

    accepted_set = set(accepted)
    persisting = {item.topic for item in guarded.non_claims}
    released = set(guarded.released_non_claim_topics)

    # Persisting non-claims are exactly the mandatory topics without a gate.
    assert persisting == set(_MANDATORY_TOPICS) - accepted_set
    # Only accepted topics are released, and never anything outside the set.
    assert released == accepted_set
    # Persisting and released partition the mandatory topics with no overlap.
    assert persisting.isdisjoint(released)
    assert persisting | released == set(_MANDATORY_TOPICS)
    # Each persisting non-claim states it holds until an accepted gate exists.
    for item in guarded.non_claims:
        assert "explicit non-claim" in item.statement
        assert item.topic in item.statement


if __name__ == "__main__":
    # The verification virtualenv has no test runner installed by default, so
    # make the property tests directly executable as a fallback.
    test_present_tense_only_for_direct_current_revision_evidence()
    test_pathless_claims_are_unknown()
    test_non_claims_persist_until_their_own_accepted_gate()
    print("Property 21 OK: present-tense claims require direct current evidence")
