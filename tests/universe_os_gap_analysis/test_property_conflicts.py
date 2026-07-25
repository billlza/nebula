from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evidence import (
    EvidenceBundle,
    detect_evidence_conflicts,
)
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
    VerificationState,
)
from tools.universe_os_gap_analysis.serialization import stable_json_dumps

_REVISION_REF = reference("revision-property-3")

# Present-tense/current statuses share the "implemented" polarity; distinct
# tiers within it are scope-differentiated and never disagree with one another.
_IMPLEMENTED_STATUSES = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
    EvidenceStatus.INSTALLED_PREVIEW,
    EvidenceStatus.REPO_PREVIEW,
    EvidenceStatus.EXPERIMENTAL,
)

# The three assertive polarity buckets. Two records disagree only when they
# belong to different buckets; Unknown is neutral and never conflicts.
_STATUS_BY_POLARITY: dict[str, tuple[EvidenceStatus, ...]] = {
    "implemented": _IMPLEMENTED_STATUSES,
    "planned": (EvidenceStatus.PLANNED,),
    "unsupported": (EvidenceStatus.UNSUPPORTED,),
}


def _polarity(status: EvidenceStatus) -> str | None:
    """Mirror the detector's polarity buckets for building test expectations."""

    if status in _IMPLEMENTED_STATUSES:
        return "implemented"
    if status is EvidenceStatus.PLANNED:
        return "planned"
    if status is EvidenceStatus.UNSUPPORTED:
        return "unsupported"
    return None


def _record(*, claim_key: str, status: EvidenceStatus, index: int) -> EvidenceRecord:
    """Build a minimal, schema-valid EvidenceRecord with a unique source location."""

    source_path = f"docs/evidence/record-{index}.md"
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, status.value),
        claim_key=claim_key,
        claim=f"{status.value} claim for {claim_key} at record {index}.",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"Anchor:{index}"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
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


@st.composite
def _incompatible_records_and_permutation(
    draw: st.DrawFn,
) -> tuple[list[EvidenceRecord], list[EvidenceRecord]]:
    """Generate an incompatible evidence set plus a permutation of its input order.

    Every claim key is forced to span at least two distinct assertive polarities,
    so each key is guaranteed to produce exactly one conflict. Arbitrary extra
    statuses (including neutral Unknown records) are added as noise, and each
    record receives a unique source location so nothing collapses on dedup.
    """

    all_statuses = tuple(EvidenceStatus)
    key_count = draw(st.integers(min_value=1, max_value=3))
    records: list[EvidenceRecord] = []
    for key_index in range(key_count):
        claim_key = f"cap:domain-{key_index}"
        polarities = draw(
            st.lists(
                st.sampled_from(("implemented", "planned", "unsupported")),
                min_size=2,
                max_size=3,
                unique=True,
            )
        )
        statuses = [draw(st.sampled_from(_STATUS_BY_POLARITY[p])) for p in polarities]
        statuses += draw(st.lists(st.sampled_from(all_statuses), max_size=4))
        for status in statuses:
            records.append(
                _record(claim_key=claim_key, status=status, index=len(records))
            )
    permutation = draw(st.permutations(records))
    return records, list(permutation)


# Feature: nebula-universe-os-gap-analysis, Property 3: Conflicts are symmetric,
# lossless, and winner-free -- for all incompatible evidence sets and all input-
# order permutations, conflict detection preserves every conflicting record and
# source location, assigns no inferred winner, and forces Confidence_Rating=Low.
# **Validates: Requirements 1.5, 13.4**
@given(_incompatible_records_and_permutation())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_conflicts_are_symmetric_lossless_and_winner_free(
    records_and_permutation: tuple[list[EvidenceRecord], list[EvidenceRecord]],
) -> None:
    records, permutation = records_and_permutation

    conflicts = detect_evidence_conflicts(_bundle(records))

    # Compute the expected assertive record set for every claim key that spans
    # more than one polarity: these are exactly the records a conflict must
    # losslessly preserve.
    expected_by_key: dict[str, list[EvidenceRecord]] = {}
    for record in records:
        if _polarity(record.status) is None:
            continue
        expected_by_key.setdefault(record.claim_key, []).append(record)
    conflicting_keys = {
        key: group
        for key, group in expected_by_key.items()
        if len({_polarity(r.status) for r in group}) >= 2
    }

    # Exactly one conflict per incompatible claim key, and no spurious conflicts.
    assert len(conflicts) == len(conflicting_keys)
    assert {conflict.claim_key for conflict in conflicts} == set(conflicting_keys)

    for conflict in conflicts:
        expected = conflicting_keys[conflict.claim_key]

        # Winner-free and Low confidence hold for every conflict.
        assert conflict.winner is None
        assert conflict.confidence is ConfidenceRating.LOW

        # Lossless: every conflicting record id is preserved, none invented.
        assert set(conflict.evidence_ids) == {reference(r.id) for r in expected}

        # Lossless: every conflicting source location is preserved.
        expected_locations = {(r.location.kind.value, r.location.value) for r in expected}
        actual_locations = {(loc.kind.value, loc.value) for loc in conflict.locations}
        assert actual_locations == expected_locations

        # Every disagreeing status value is retained among the incompatibles.
        assert set(conflict.incompatible_values) == {r.status.value for r in expected}

    # Symmetric / order-independent: permuting the input order yields an
    # identical set of conflicts down to their canonical serialization.
    permuted_conflicts = detect_evidence_conflicts(_bundle(permutation))
    assert stable_json_dumps(permuted_conflicts) == stable_json_dumps(conflicts)
