from __future__ import annotations

import json

import jsonschema
from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.identifiers import ReferenceId, StableId
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
    TargetLevel,
    VerificationState,
)
from tools.universe_os_gap_analysis.serialization import stable_json_dumps

# The stable-identifier grammar shared by object IDs and typed references. A
# schema-valid Evidence_Record must carry references matching this grammar so
# every relationship in the assessment model resolves deterministically.
_ID_PATTERN = r"^[A-Za-z][A-Za-z0-9_.:]*(?:-[A-Za-z0-9_.:]+)*$"

# A stable, repository-relative POSIX location: no backslash, no leading or
# trailing slash. This encodes the "stable source location" requirement.
_PATH_PATTERN = r"^[^/\\]([^\\]*[^/\\])?$"

_EVIDENCE_STATUS_VALUES = sorted(status.value for status in EvidenceStatus)
_LOCATION_KIND_VALUES = sorted(kind.value for kind in LocationKind)
_REVISION_ORIGIN_VALUES = sorted(origin.value for origin in RevisionOrigin)
_EVIDENCE_KIND_VALUES = sorted(kind.value for kind in EvidenceKind)
_CONFIDENCE_VALUES = sorted(rating.value for rating in ConfidenceRating)
_VERIFICATION_VALUES = sorted(state.value for state in VerificationState)
_TARGET_LEVEL_VALUES = sorted(level.value for level in TargetLevel)
_OWNERSHIP_VALUES = sorted(owner.value for owner in Ownership)

# Every glossary-defined Evidence_Record field, in canonical camelCase form.
_REQUIRED_FIELDS = (
    "id",
    "claimKey",
    "claim",
    "status",
    "sourcePath",
    "location",
    "revisionRef",
    "origin",
    "evidenceKind",
    "confidence",
    "scope",
    "limitations",
    "trustAssumptions",
    "verificationState",
    "relatedEvidenceIds",
)

# JSON Schema capturing Requirements 1.4 (all glossary fields), 4.1 (exactly one
# allowed Evidence_Status), and 4.2 (the closed Evidence_Status set), plus valid
# reference identifiers and a stable source location.
EVIDENCE_RECORD_SCHEMA: dict = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "type": "object",
    "additionalProperties": False,
    "required": list(_REQUIRED_FIELDS),
    "properties": {
        "id": {"type": "string", "pattern": _ID_PATTERN},
        "claimKey": {"type": "string", "minLength": 1},
        "claim": {"type": "string", "minLength": 1},
        # Exactly one status from the closed Evidence_Status set. `enum` admits a
        # single scalar value, never a list, so a record is singly classified.
        "status": {"type": "string", "enum": _EVIDENCE_STATUS_VALUES},
        "sourcePath": {"type": "string", "minLength": 1, "pattern": _PATH_PATTERN},
        "location": {
            "type": "object",
            "additionalProperties": False,
            "required": ["kind", "value"],
            "properties": {
                "kind": {"type": "string", "enum": _LOCATION_KIND_VALUES},
                "value": {"type": "string", "minLength": 1},
            },
        },
        "revisionRef": {"type": "string", "pattern": _ID_PATTERN},
        "origin": {"type": "string", "enum": _REVISION_ORIGIN_VALUES},
        "evidenceKind": {"type": "string", "enum": _EVIDENCE_KIND_VALUES},
        "confidence": {"type": "string", "enum": _CONFIDENCE_VALUES},
        "scope": {
            "type": "object",
            "additionalProperties": False,
            "required": ["capabilityIds", "targetLevels", "platforms", "ownership"],
            "properties": {
                "capabilityIds": {
                    "type": "array",
                    "items": {"type": "string", "pattern": _ID_PATTERN},
                },
                "targetLevels": {
                    "type": "array",
                    "items": {"type": "string", "enum": _TARGET_LEVEL_VALUES},
                },
                "platforms": {"type": "array", "items": {"type": "string"}},
                "ownership": {
                    "anyOf": [
                        {"type": "string", "enum": _OWNERSHIP_VALUES},
                        {"type": "null"},
                    ]
                },
            },
        },
        "limitations": {"type": "array", "items": {"type": "string", "minLength": 1}},
        "trustAssumptions": {
            "type": "array",
            "items": {"type": "string", "minLength": 1},
        },
        "verificationState": {"type": "string", "enum": _VERIFICATION_VALUES},
        "relatedEvidenceIds": {
            "type": "array",
            "items": {"type": "string", "pattern": _ID_PATTERN},
        },
    },
}

_EVIDENCE_RECORD_VALIDATOR = jsonschema.Draft202012Validator(EVIDENCE_RECORD_SCHEMA)


# Constrained generators keep every draw inside the accepted input space so the
# property exercises real Evidence_Record content rather than rejected garbage.
_stable_ids = st.from_regex(
    r"[a-z][a-z0-9]{0,10}(-[a-z0-9]{1,10}){0,3}", fullmatch=True
)
_relative_paths = st.lists(
    st.from_regex(r"[a-z0-9_]{1,8}", fullmatch=True), min_size=1, max_size=4
).map("/".join)
_printable_text = (
    st.text(
        alphabet=st.characters(min_codepoint=0x20, max_codepoint=0x7E),
        min_size=1,
        max_size=40,
    )
    .map(lambda value: value.strip())
    .filter(bool)
)


@st.composite
def _evidence_scopes(draw: st.DrawFn) -> EvidenceScope:
    return EvidenceScope(
        capability_ids=tuple(
            ReferenceId(value)
            for value in draw(st.lists(_stable_ids, max_size=3, unique=True))
        ),
        target_levels=tuple(
            draw(
                st.lists(
                    st.sampled_from(tuple(TargetLevel)), max_size=3, unique=True
                )
            )
        ),
        platforms=tuple(draw(st.lists(_printable_text, max_size=3, unique=True))),
        ownership=draw(st.one_of(st.none(), st.sampled_from(tuple(Ownership)))),
    )


@st.composite
def _accepted_evidence_records(draw: st.DrawFn) -> EvidenceRecord:
    """Draw a valid, accepted Evidence_Record spanning every Evidence_Status."""

    related = draw(st.lists(_stable_ids, max_size=4, unique=True))
    return EvidenceRecord(
        id=StableId(draw(_stable_ids)),
        claim_key=draw(_printable_text),
        claim=draw(_printable_text),
        status=draw(st.sampled_from(tuple(EvidenceStatus))),
        source_path=draw(_relative_paths),
        location=SourceLocation(
            kind=draw(st.sampled_from(tuple(LocationKind))),
            value=draw(_printable_text),
        ),
        revision_ref=ReferenceId(draw(_stable_ids)),
        origin=draw(st.sampled_from(tuple(RevisionOrigin))),
        evidence_kind=draw(st.sampled_from(tuple(EvidenceKind))),
        confidence=draw(st.sampled_from(tuple(ConfidenceRating))),
        scope=draw(_evidence_scopes()),
        limitations=tuple(draw(st.lists(_printable_text, max_size=3, unique=True))),
        trust_assumptions=tuple(
            draw(st.lists(_printable_text, max_size=3, unique=True))
        ),
        verification_state=draw(st.sampled_from(tuple(VerificationState))),
        related_evidence_ids=tuple(ReferenceId(value) for value in related),
    )


# Feature: nebula-universe-os-gap-analysis, Property 2: Accepted evidence is complete and singly classified
# **Validates: Requirements 1.4, 4.1, 4.2**
@given(record=_accepted_evidence_records())
@settings(max_examples=100, deadline=None, print_blob=True)
def test_accepted_evidence_is_complete_and_singly_classified(
    record: EvidenceRecord,
) -> None:
    serialized = stable_json_dumps(record)
    payload = json.loads(serialized)

    # Requirements 1.4 + 4.1/4.2: the projection is schema-valid, so it carries
    # every glossary field, a stable location, valid references, and one status.
    _EVIDENCE_RECORD_VALIDATOR.validate(payload)

    # Every required glossary field is present (schema `additionalProperties` is
    # False, so there are also no stray fields).
    assert set(payload) == set(_REQUIRED_FIELDS)

    # Singly classified: status is a single scalar from the closed set, never a
    # collection, and it round-trips the record's own status verbatim.
    assert isinstance(payload["status"], str)
    assert payload["status"] in _EVIDENCE_STATUS_VALUES
    assert payload["status"] == record.status.value

    # Valid references: the revision reference and every related evidence id obey
    # the stable-identifier grammar and are distinct (model-normalized).
    assert isinstance(payload["revisionRef"], str)
    related = payload["relatedEvidenceIds"]
    assert len(related) == len(set(related))
    assert all(isinstance(identifier, str) for identifier in related)

    # Stable source location: a closed-set kind plus a non-empty stable value.
    assert payload["location"]["kind"] in _LOCATION_KIND_VALUES
    assert payload["location"]["value"]

    # Serialization is deterministic, so the "stable" location is reproducible.
    assert stable_json_dumps(record) == serialized

    # Illegal classification is rejected: mutating the single status into a value
    # outside the closed Evidence_Status set fails schema validation, and so does
    # replacing it with two statuses (a record must be classified exactly once).
    off_set = dict(payload)
    off_set["status"] = "Definitely_Not_A_Status"
    assert not _EVIDENCE_RECORD_VALIDATOR.is_valid(off_set)

    doubly_classified = dict(payload)
    doubly_classified["status"] = [record.status.value, record.status.value]
    assert not _EVIDENCE_RECORD_VALIDATOR.is_valid(doubly_classified)

    # An incomplete record (any required glossary field removed) is also invalid,
    # confirming completeness is enforced field-by-field.
    for field_name in _REQUIRED_FIELDS:
        incomplete = dict(payload)
        del incomplete[field_name]
        assert not _EVIDENCE_RECORD_VALIDATOR.is_valid(incomplete), field_name


if __name__ == "__main__":
    # The verification virtualenv has no test runner, so make the property test
    # directly executable: Hypothesis drives the example generation on call.
    test_accepted_evidence_is_complete_and_singly_classified()
    print("Property 2 OK: accepted evidence is complete and singly classified")
