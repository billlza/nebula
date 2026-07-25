"""Property-based test for exclusive application-responsibility ownership.

This module owns the Hypothesis property test for design Property 19. It
exercises the *real* :func:`evaluate_application_ecosystem_release` evaluator
(Task 7.4) against adversarially generated evidence bundles; no product code is
edited and no mocks are used.

Property 19 has two halves, and both are checked here without restating the
implementation:

1. **Exclusivity + completeness.** Every application responsibility is drafted
   exactly once and is assigned exactly one of ``NebulaOwned | HostOwned |
   OperationsOwned``. The three ownership buckets partition the full
   responsibility set, and the assignment is order independent.
2. **Ownership does not imply maturity.** An ownership label is only a label:
   assigning an owner (even with the strongest GA status) to an application
   responsibility never satisfies an ecosystem/release capability domain. Those
   maturity domains stay gap'd unless they carry their own direct current
   implementation evidence.

The ownership-resolution oracle in :func:`_expected_owner` is re-derived from
the documented Requirement 11.2 contract ("declared ownership wins when
unanimous; otherwise unanimous textual ownership; otherwise the declared
default"), not copied from the evaluator, so the equality check is meaningful.
"""

from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    APPLICATION_RESPONSIBILITIES,
    ECOSYSTEM_CHECKLIST,
    RELEASE_CHECKLIST,
    evaluate_application_ecosystem_release,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    LocationKind,
    Ownership,
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)

# --------------------------------------------------------------------------- #
# Independent oracle constants (re-derived from Requirement 11.2, not code)   #
# --------------------------------------------------------------------------- #

_REVISION_REF = stable_id("revision", "property-19")

_ALL_OWNERS: tuple[Ownership, ...] = (
    Ownership.NEBULA_OWNED,
    Ownership.HOST_OWNED,
    Ownership.OPERATIONS_OWNED,
)

# Unambiguous single-owner text phrases. Each maps to exactly one owner and
# contains no other owner marker, so the oracle can reason about them in
# isolation. Host precedence mirrors the documented contract (a hosted asset can
# never be read as a Nebula-owned OS capability).
_OWNER_TEXT_PHRASE: dict[Ownership, str] = {
    Ownership.HOST_OWNED: "host-owned",
    Ownership.OPERATIONS_OWNED: "operations-owned",
    Ownership.NEBULA_OWNED: "nebula-owned",
}

# The CLI responsibility is a convenient probe: its declared default owner is
# NebulaOwned and its markers ("cli tool") contain no owner or ecosystem/release
# marker, so attaching owner signals to a "cli tool" claim controls ownership
# cleanly without collateral matches.
_CLI_RESPONSIBILITY_ID = "responsibility-app-cli-tools"
_CLI_MARKER = "cli tool"
_CLI_DEFAULT_OWNER = Ownership.NEBULA_OWNED

# Safe filler: alnum-only, so it can never accidentally form a multi-word marker.
_SAFE_FILLER = st.from_regex(r"[a-z0-9]{0,12}", fullmatch=True)
_UNIQUE = st.from_regex(r"[a-z0-9]{8}", fullmatch=True)

# A spread of distinct responsibility markers used to exercise matching in the
# general exclusivity test. None of these is a substring of another.
_PROBE_MARKERS: tuple[str, ...] = (
    "cli tool",
    "backend service",
    "authentication",
    "tls",
    "crypto",
    "renderer",
    "code signing",
    "installer",
    "crash reporting",
)


def _text_owner(text: str) -> Ownership | None:
    """Owner implied by unambiguous text markers (host precedence first)."""

    lowered = text.lower()
    if _OWNER_TEXT_PHRASE[Ownership.HOST_OWNED] in lowered:
        return Ownership.HOST_OWNED
    if _OWNER_TEXT_PHRASE[Ownership.OPERATIONS_OWNED] in lowered:
        return Ownership.OPERATIONS_OWNED
    if _OWNER_TEXT_PHRASE[Ownership.NEBULA_OWNED] in lowered:
        return Ownership.NEBULA_OWNED
    return None


def _expected_owner(
    matched: list[EvidenceRecord], default: Ownership
) -> Ownership:
    """Requirement 11.2 contract oracle for a single responsibility.

    Precedence, re-derived from the documented contract:
      1. If the declared ``scope.ownership`` values are unanimous, use them.
      2. Otherwise, if the textual owners are unanimous, use them.
      3. Otherwise fall back to the declared default owner.
    """

    declared = {
        record.scope.ownership
        for record in matched
        if record.scope.ownership is not None
    }
    if len(declared) == 1:
        return next(iter(declared))
    text_owners = {
        owner
        for owner in (_text_owner(record.claim) for record in matched)
        if owner is not None
    }
    if len(text_owners) == 1:
        return next(iter(text_owners))
    return default


# --------------------------------------------------------------------------- #
# Shared record building                                                      #
# --------------------------------------------------------------------------- #


def _make_record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    kind: EvidenceKind,
    origin: RevisionOrigin,
    ownership: Ownership | None,
    unique: str,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, unique),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path="docs/support_matrix.md",
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(ownership=ownership),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


# --------------------------------------------------------------------------- #
# Part 1: exclusivity, completeness, and order independence                   #
# --------------------------------------------------------------------------- #


@st.composite
def _arbitrary_record(draw: st.DrawFn) -> EvidenceRecord:
    """A varied record that may match responsibilities and carry owner signals."""

    marker = draw(st.sampled_from(_PROBE_MARKERS))
    owner_text = draw(st.sampled_from((None, *_ALL_OWNERS)))
    text_fragment = "" if owner_text is None else f" {_OWNER_TEXT_PHRASE[owner_text]}"
    filler = draw(_SAFE_FILLER)
    claim = f"The {marker} note{text_fragment}. {filler}".strip()
    unique = draw(_UNIQUE)
    return _make_record(
        claim_key=f"source:docs/support_matrix.md#{unique}",
        claim=claim,
        status=draw(st.sampled_from(tuple(EvidenceStatus))),
        kind=draw(st.sampled_from(tuple(EvidenceKind))),
        origin=draw(st.sampled_from(tuple(RevisionOrigin))),
        ownership=draw(st.sampled_from((None, *_ALL_OWNERS))),
        unique=unique,
    )


# Feature: nebula-universe-os-gap-analysis, Property 19: Application responsibility ownership is exclusive - every responsibility is drafted once and assigned exactly one owner, the three owners partition the full set, and the assignment is order independent.
# **Validates: Requirements 11.2**
@given(records=st.lists(_arbitrary_record(), max_size=8, unique_by=lambda r: str(r.id)))
@settings(max_examples=150, deadline=None, print_blob=True)
def test_ownership_is_exclusive_complete_and_order_independent(
    records: list[EvidenceRecord],
) -> None:
    result = evaluate_application_ecosystem_release(_bundle(records))

    # Completeness: exactly the full responsibility set is drafted, once each.
    drafted_ids = [str(d.responsibility_id) for d in result.responsibility_drafts]
    expected_ids = {str(item.responsibility_id) for item in APPLICATION_RESPONSIBILITIES}
    assert len(drafted_ids) == len(set(drafted_ids))  # no duplicates
    assert set(drafted_ids) == expected_ids

    # Exclusivity: each draft carries exactly one of the three allowed owners.
    for draft in result.responsibility_drafts:
        assert draft.ownership in _ALL_OWNERS

    # The three ownership buckets partition the responsibility set (each
    # responsibility appears in exactly one bucket).
    partition: list[str] = []
    for owner in _ALL_OWNERS:
        partition.extend(
            str(d.responsibility_id)
            for d in result.responsibilities_owned_by(owner)
        )
    assert len(partition) == len(expected_ids)
    assert set(partition) == expected_ids

    # Order independence: reversing the input yields identical assignments.
    reversed_result = evaluate_application_ecosystem_release(_bundle(list(reversed(records))))
    assert {
        str(d.responsibility_id): d.ownership for d in result.responsibility_drafts
    } == {
        str(d.responsibility_id): d.ownership
        for d in reversed_result.responsibility_drafts
    }


# --------------------------------------------------------------------------- #
# Part 2: the resolved owner matches the Requirement 11.2 contract oracle      #
# --------------------------------------------------------------------------- #

_SIGNAL_SCOPE = "scope"
_SIGNAL_TEXT = "text"
_SIGNAL_NEUTRAL = "neutral"


@st.composite
def _cli_signal_record(draw: st.DrawFn) -> EvidenceRecord:
    """A record matching the CLI responsibility carrying a controlled owner signal."""

    signal = draw(st.sampled_from((_SIGNAL_SCOPE, _SIGNAL_TEXT, _SIGNAL_NEUTRAL)))
    filler = draw(_SAFE_FILLER)
    unique = draw(_UNIQUE)
    ownership: Ownership | None = None
    if signal == _SIGNAL_SCOPE:
        ownership = draw(st.sampled_from(_ALL_OWNERS))
        claim = f"The {_CLI_MARKER} subsystem entry. {filler}".strip()
    elif signal == _SIGNAL_TEXT:
        owner = draw(st.sampled_from(_ALL_OWNERS))
        claim = f"The {_CLI_MARKER} is {_OWNER_TEXT_PHRASE[owner]} here. {filler}".strip()
    else:
        claim = f"The {_CLI_MARKER} subsystem entry. {filler}".strip()
    return _make_record(
        claim_key=f"source:cli/{unique}.nb",
        claim=claim,
        status=draw(st.sampled_from(tuple(EvidenceStatus))),
        kind=draw(st.sampled_from(tuple(EvidenceKind))),
        origin=draw(st.sampled_from(tuple(RevisionOrigin))),
        ownership=ownership,
        unique=unique,
    )


# Feature: nebula-universe-os-gap-analysis, Property 19: Application responsibility ownership is exclusive - the single resolved owner matches the Requirement 11.2 contract oracle (unanimous declared ownership, else unanimous textual ownership, else the declared default) regardless of input order.
# **Validates: Requirements 11.2**
@given(records=st.lists(_cli_signal_record(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=200, deadline=None, print_blob=True)
def test_ownership_resolution_matches_contract_oracle(
    records: list[EvidenceRecord],
) -> None:
    result = evaluate_application_ecosystem_release(_bundle(records))

    # Every generated record matches the CLI responsibility (all contain the
    # owner/ecosystem-neutral "cli tool" marker), so the whole bundle is the
    # matched set for the oracle.
    expected = _expected_owner(records, _CLI_DEFAULT_OWNER)

    cli = result.responsibility_for(_CLI_RESPONSIBILITY_ID)
    assert cli is not None
    assert cli.ownership == expected
    # Exactly one owner, drawn from the allowed set.
    assert cli.ownership in _ALL_OWNERS
    # owner_from_evidence is set precisely when the resolution came from evidence
    # rather than the declared default.
    assert cli.owner_from_evidence is _resolved_from_evidence(records)


def _resolved_from_evidence(records: list[EvidenceRecord]) -> bool:
    """Whether the contract oracle resolves from evidence (not the default)."""

    declared = {
        record.scope.ownership
        for record in records
        if record.scope.ownership is not None
    }
    if len(declared) == 1:
        return True
    text_owners = {
        owner
        for owner in (_text_owner(record.claim) for record in records)
        if owner is not None
    }
    return len(text_owners) == 1


# --------------------------------------------------------------------------- #
# Part 3: ownership does not imply maturity outside the owner's domain         #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 19: Application responsibility ownership is exclusive - assigning an owner (even with the strongest GA status) to an application responsibility never satisfies any ecosystem/release capability domain; maturity requires that domain's own direct current implementation evidence.
# **Validates: Requirements 11.2**
@given(records=st.lists(_cli_signal_record(), min_size=1, max_size=6, unique_by=lambda r: str(r.id)))
@settings(max_examples=150, deadline=None, print_blob=True)
def test_ownership_does_not_imply_capability_maturity(
    records: list[EvidenceRecord],
) -> None:
    result = evaluate_application_ecosystem_release(_bundle(records))

    # The CLI responsibility receives exactly one owner from this evidence...
    cli = result.responsibility_for(_CLI_RESPONSIBILITY_ID)
    assert cli is not None
    assert cli.ownership in _ALL_OWNERS

    # ...yet the ownership label (and any GA status it carries) grants no
    # maturity: none of these responsibility-scoped records match an
    # ecosystem/release capability marker, so every maturity domain stays
    # unsatisfied and produces a gap. Ownership never leaks into a capability
    # domain it does not own.
    expected_capability_ids = {
        str(item.capability_id)
        for item in (*ECOSYSTEM_CHECKLIST, *RELEASE_CHECKLIST)
    }
    seen: set[str] = set()
    for draft in result.maturity_drafts():
        seen.add(str(draft.domain.id))
        assert draft.satisfied is False
        assert draft.substrate_promotion_blocked is False
        assert result.gap_for(str(draft.domain.id)) is not None
    assert seen == expected_capability_ids


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner, so allow the property
    # tests to be executed directly as a fallback.
    test_ownership_is_exclusive_complete_and_order_independent()
    test_ownership_resolution_matches_contract_oracle()
    test_ownership_does_not_imply_capability_maturity()
    print("Property 19 OK: application responsibility ownership is exclusive")
