from __future__ import annotations

import json
from datetime import datetime, timezone

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.identifiers import StableId, reference
from tools.universe_os_gap_analysis.models import (
    AssessmentModel,
    AssessmentRevision,
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


def _revision(worktree_clean: bool) -> AssessmentRevision:
    return AssessmentRevision(
        schema_version="1.0",
        commit_id="abc123",
        branch="property/revision-origin",
        version="1.0.0",
        describe="v1.0.0",
        tags=("v1.0.0",),
        worktree_clean=worktree_clean,
        assessed_at_utc=datetime(2025, 1, 1, tzinfo=timezone.utc),
        fingerprint_algorithm="sha256-length-prefixed-v1",
        worktree_fingerprint="worktree-hash",
        tracked_diff_hash="diff-hash",
        untracked_path_set_hash="paths-hash",
        excluded_paths=(),
        repository_root_id=StableId("repo-nebula"),
    )


def _evidence(index: int, origin: RevisionOrigin, status: EvidenceStatus) -> EvidenceRecord:
    return EvidenceRecord(
        id=StableId(f"evidence-origin-{index}"),
        claim_key=f"origin.claim.{index}",
        claim=f"Evidence claim {index}.",
        status=status,
        source_path=f"evidence/origin-{index}.md",
        location=SourceLocation(kind=LocationKind.HEADING, value=f"Claim {index}"),
        revision_ref=reference(f"revision-{origin.value}"),
        origin=origin,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


# Feature: nebula-universe-os-gap-analysis, Property 1: Revision-origin isolation
# **Validates: Requirements 1.2**
@given(
    worktree_clean=st.booleans(),
    generated=st.lists(
        st.tuples(st.sampled_from(tuple(RevisionOrigin)), st.sampled_from(tuple(EvidenceStatus))),
        max_size=10,
    ),
    current_status=st.sampled_from(tuple(EvidenceStatus)),
    tagged_status=st.sampled_from(tuple(EvidenceStatus)),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_current_worktree_never_serializes_as_tagged_release(
    worktree_clean: bool,
    generated: list[tuple[RevisionOrigin, EvidenceStatus]],
    current_status: EvidenceStatus,
    tagged_status: EvidenceStatus,
) -> None:
    combinations = [
        (RevisionOrigin.CURRENT_WORKTREE, current_status),
        (RevisionOrigin.TAGGED_RELEASE, tagged_status),
        *generated,
    ]
    records = tuple(
        _evidence(index, origin, status)
        for index, (origin, status) in enumerate(combinations)
    )
    expected_origins = {str(record.id): record.origin.value for record in records}

    payload = json.loads(
        stable_json_dumps(AssessmentModel(revision=_revision(worktree_clean), evidence_records=records))
    )
    serialized = {record["id"]: record for record in payload["evidenceRecords"]}

    # Serialization must preserve every origin verbatim: no record is silently
    # reclassified while projecting the model to its canonical JSON form.
    assert {identifier: record["origin"] for identifier, record in serialized.items()} == expected_origins

    current_worktree_value = RevisionOrigin.CURRENT_WORKTREE.value
    tagged_release_value = RevisionOrigin.TAGGED_RELEASE.value
    assert current_worktree_value != tagged_release_value

    current_ids = {
        identifier
        for identifier, origin in expected_origins.items()
        if origin == current_worktree_value
    }
    tagged_ids = {
        identifier
        for identifier, record in serialized.items()
        if record["origin"] == tagged_release_value
    }
    # Distinguishability at the value level: a Current_Worktree record can never
    # carry the tagged-release origin, so the two id sets never overlap and the
    # guaranteed seed records land on opposite sides of the partition.
    assert current_ids.isdisjoint(tagged_ids)
    assert str(records[0].id) in current_ids
    assert str(records[1].id) in tagged_ids
    # Every Current_Worktree record must still render as Current_Worktree, never
    # as tagged-release evidence, and must retain its worktree revision binding.
    for identifier in current_ids:
        assert serialized[identifier]["origin"] == current_worktree_value
        assert serialized[identifier]["origin"] != tagged_release_value
        assert serialized[identifier]["revisionRef"] == "revision-CurrentWorktree"

    axes = payload["revision"]["evidenceAxes"]
    assert payload["revision"]["worktreeClean"] is worktree_clean
    assert axes["currentWorktree"]["origin"] == RevisionOrigin.CURRENT_WORKTREE.value
    assert axes["taggedRelease"]["origin"] == RevisionOrigin.TAGGED_RELEASE.value
    assert axes["currentWorktree"] != axes["taggedRelease"]
