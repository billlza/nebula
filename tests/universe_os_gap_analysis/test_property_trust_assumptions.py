from __future__ import annotations

from hypothesis import given, settings, strategies as st

from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    FindingSeverity,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    VerificationState,
)
from tools.universe_os_gap_analysis.trust_audit import (
    CLM_UNRECORDED_ASSUMPTION_CODE,
    AssumptionCategory,
    TrustAssumptionAuditError,
    TrustAssumptionAuditor,
    audit_trust_assumptions,
)

# Every detectable exclusion/trust-assumption category paired with a real marker
# fragment that the production trust_audit detector recognizes. Detection runs on
# claim text; recording runs on limitation/trust-assumption text. Both fragments
# are chosen from the production marker vocabulary so this test drives the real
# component, and the fragments are mutually non-overlapping so a text built from
# one category's fragment triggers exactly that category and no other.
_CATEGORY_FRAGMENTS: dict[AssumptionCategory, str] = {
    # -- opaque/dynamic/FFI/unsafe safety exclusions (Requirement 6.6) -------- #
    AssumptionCategory.OPAQUE_EXCLUSION: "the guarantee excludes opaque values",
    AssumptionCategory.DYNAMIC_EXCLUSION: "the guarantee excludes dynamic dispatch",
    AssumptionCategory.FFI_EXCLUSION: "the guarantee excludes ffi boundaries",
    AssumptionCategory.UNSAFE_EXCLUSION: "the guarantee excludes unsafe blocks",
    # -- trusted-tool / cooperative-descendant / caller-controlled-directory /
    #    host-security-service trust assumptions (Requirement 9.5) ------------ #
    AssumptionCategory.TRUSTED_TOOL: "reproducibility assumes a trusted toolchain",
    AssumptionCategory.COOPERATIVE_DESCENDANT: "relies on a cooperative descendant process",
    AssumptionCategory.CALLER_CONTROLLED_DIRECTORY: "writes into a caller-controlled directory",
    AssumptionCategory.HOST_SECURITY_SERVICE: "delegates isolation to the host security service",
}

# The trust-assumption categories that Requirement 9.5 enumerates explicitly.
_TRUST_CATEGORIES: frozenset[AssumptionCategory] = frozenset(
    {
        AssumptionCategory.TRUSTED_TOOL,
        AssumptionCategory.COOPERATIVE_DESCENDANT,
        AssumptionCategory.CALLER_CONTROLLED_DIRECTORY,
        AssumptionCategory.HOST_SECURITY_SERVICE,
    }
)

_ALL_CATEGORIES: tuple[AssumptionCategory, ...] = tuple(_CATEGORY_FRAGMENTS)

_REVISION_REF = reference("revision-property-15")


def _category_sets() -> st.SearchStrategy[frozenset[AssumptionCategory]]:
    """Non-empty subsets of the detectable exclusion/trust-assumption categories."""

    return st.sets(st.sampled_from(_ALL_CATEGORIES), min_size=1, max_size=len(_ALL_CATEGORIES)).map(
        frozenset
    )


def _build_claim(detected: frozenset[AssumptionCategory]) -> str:
    """A claim whose text implies exactly ``detected`` (order-independent)."""

    fragments = [_CATEGORY_FRAGMENTS[category] for category in _ALL_CATEGORIES if category in detected]
    return "The capability holds, but " + "; ".join(fragments) + "."


@st.composite
def _record_specs(draw: st.DrawFn) -> tuple[EvidenceRecord, frozenset[AssumptionCategory], frozenset[AssumptionCategory]]:
    """Draw one record plus its injected detected/recorded category sets.

    ``recorded`` is drawn independently of ``detected`` so that the set
    difference ``detected - recorded`` spans complete, partial, and empty
    disclosure. Recorded fragments are split across the ``limitations`` and
    ``trust_assumptions`` fields to exercise both disclosure channels.
    """

    detected = draw(_category_sets())
    recorded = frozenset(
        draw(st.sets(st.sampled_from(_ALL_CATEGORIES), max_size=len(_ALL_CATEGORIES)))
    )
    index = draw(st.integers(min_value=0, max_value=999999))
    claim_key = f"cap:prop15-{index:06d}"
    claim = _build_claim(detected)

    limitations: list[str] = []
    trust_assumptions: list[str] = []
    for category in _ALL_CATEGORIES:
        if category not in recorded:
            continue
        fragment = _CATEGORY_FRAGMENTS[category]
        if draw(st.booleans()):
            limitations.append(f"Recorded limitation: {fragment}.")
        else:
            trust_assumptions.append(f"Recorded assumption: {fragment}.")

    record = EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, str(index)),
        claim_key=claim_key,
        claim=claim,
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="spec/safety_contract.md",
        location=SourceLocation(kind=LocationKind.HEADING, value="Safety Contract"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=tuple(limitations),
        trust_assumptions=tuple(trust_assumptions),
        verification_state=VerificationState.NOT_RUN,
    )
    return record, detected, recorded


# Feature: nebula-universe-os-gap-analysis, Property 15: Trust assumptions are
# complete or validation fails - every detected trusted-tool / cooperative-
# descendant / caller-controlled-directory / host-security-service (and safety
# exclusion) assumption must appear in the record's disclosure, and any
# detected-minus-recorded set difference invalidates the assessment and cites the
# affected records.
# **Validates: Requirements 9.5, 9.6**
@given(specs=st.lists(_record_specs(), min_size=1, max_size=6, unique_by=lambda spec: spec[0].id))
@settings(max_examples=200, deadline=None, print_blob=True)
def test_trust_assumptions_are_complete_or_validation_fails(
    specs: list[tuple[EvidenceRecord, frozenset[AssumptionCategory], frozenset[AssumptionCategory]]],
) -> None:
    """Exercise the real TrustAssumptionAuditor over injected disclosure gaps.

    The detected and recorded category sets are injected via the production
    marker vocabulary, so the assertions confirm the real detector (not a
    tautology): detection must equal the injected detected set, recording must
    equal the injected recorded set, the unrecorded set must be exactly their
    difference, and any non-empty difference must fail closed with a structured
    CLM-* finding that cites the affected evidence records.
    """

    records = [record for record, _, _ in specs]
    report = TrustAssumptionAuditor().audit(records)

    affected_refs: set[str] = set()
    exclusion_missing = False
    trust_missing = False

    for record, detected, recorded in specs:
        audit = report.audit_for(record.id)
        assert audit is not None

        # The real detector recovers exactly the injected categories from the
        # claim text and from the recorded disclosure text.
        assert set(audit.detected) == set(detected)
        assert set(audit.recorded) == set(recorded)

        # The invalidating condition is precisely the set difference: every
        # detected assumption not recorded in limitations/trust_assumptions.
        expected_unrecorded = detected - recorded
        assert set(audit.unrecorded) == expected_unrecorded
        assert audit.is_complete == (not expected_unrecorded)

        # When complete, every detected trust assumption appears in disclosure.
        if audit.is_complete:
            for category in detected & _TRUST_CATEGORIES:
                assert category in set(audit.recorded)

        if expected_unrecorded:
            affected_refs.add(str(record.id))
            if any(cat in _TRUST_CATEGORIES for cat in expected_unrecorded):
                trust_missing = True
            if any(cat not in _TRUST_CATEGORIES for cat in expected_unrecorded):
                exclusion_missing = True

    if affected_refs:
        # Any set difference invalidates the whole assessment (fail closed).
        assert not report.is_complete
        assert set(report.unrecorded_evidence_refs) == affected_refs

        findings = report.validation_findings()
        assert findings
        finding_refs = {str(ref) for finding in findings for ref in finding.object_refs}
        # Every affected record is cited by at least one finding.
        assert affected_refs <= finding_refs
        for finding in findings:
            assert finding.severity is FindingSeverity.ERROR
            assert finding.code == CLM_UNRECORDED_ASSUMPTION_CODE
            # Requirement 9.6 (fail closed) is always cited; the governing
            # disclosure requirement (9.5 trust / 6.6 exclusion) accompanies it.
            assert "9.6" in finding.requirement_refs
        finding_requirements = {ref for finding in findings for ref in finding.requirement_refs}
        if trust_missing:
            assert "9.5" in finding_requirements
        if exclusion_missing:
            assert "6.6" in finding_requirements

        # enforce() and the convenience entry point both fail closed, citing the
        # affected records on the raised structured error.
        try:
            report.enforce()
        except TrustAssumptionAuditError as error:
            assert set(error.evidence_refs) == affected_refs
            assert "9.6" in error.requirement_refs
        else:  # pragma: no cover - the branch above must trigger
            raise AssertionError("enforce() must fail closed on unrecorded assumptions")

        try:
            audit_trust_assumptions(records)
        except TrustAssumptionAuditError as error:
            assert set(error.evidence_refs) == affected_refs
        else:  # pragma: no cover
            raise AssertionError("audit_trust_assumptions must fail closed")
    else:
        # No set difference anywhere: the assessment is valid and stays valid.
        assert report.is_complete
        assert report.unrecorded_evidence_refs == ()
        assert report.validation_findings() == ()
        returned = audit_trust_assumptions(records)
        assert returned.is_complete


# Feature: nebula-universe-os-gap-analysis, Property 15: Trust assumptions are
# complete or validation fails - a single unrecorded trust assumption on any
# record in an otherwise-complete bundle fails the whole assessment and cites
# exactly that record, proving the check is over all evidence records.
# **Validates: Requirements 9.5, 9.6**
@given(
    missing_category=st.sampled_from(tuple(_TRUST_CATEGORIES)),
    companion_count=st.integers(min_value=0, max_value=4),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_single_unrecorded_trust_assumption_invalidates_and_cites_record(
    missing_category: AssumptionCategory,
    companion_count: int,
) -> None:
    """One undisclosed trust assumption must invalidate the bundle and name it."""

    fragment = _CATEGORY_FRAGMENTS[missing_category]
    culprit = EvidenceRecord(
        id=stable_id("evidence", "culprit", fragment),
        claim_key="cap:culprit",
        claim=f"The capability holds, but {fragment}.",
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="spec/safety_contract.md",
        location=SourceLocation(kind=LocationKind.HEADING, value="Safety Contract"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )

    # Companions carry no exclusion/trust markers, so they never contribute an
    # unrecorded assumption and cannot be the cited record.
    companions = [
        EvidenceRecord(
            id=stable_id("evidence", "companion", str(index)),
            claim_key=f"cap:companion-{index}",
            claim="The parser accepts the documented grammar.",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="spec/language_core.md",
            location=SourceLocation(kind=LocationKind.HEADING, value="Grammar"),
            revision_ref=_REVISION_REF,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            evidence_kind=EvidenceKind.SOURCE,
            confidence=ConfidenceRating.MEDIUM,
            scope=EvidenceScope(),
            limitations=(),
            trust_assumptions=(),
            verification_state=VerificationState.NOT_RUN,
        )
        for index in range(companion_count)
    ]

    records = [culprit, *companions]
    bundle = EvidenceBundle(
        records=tuple(records),
        by_claim_key={record.claim_key: (record,) for record in records},
    )

    report = TrustAssumptionAuditor().audit(bundle)
    assert not report.is_complete
    assert report.unrecorded_evidence_refs == (str(culprit.id),)

    with_missing = report.audit_for(culprit.id)
    assert with_missing is not None
    assert set(with_missing.unrecorded) == {missing_category}

    try:
        audit_trust_assumptions(bundle)
    except TrustAssumptionAuditError as error:
        assert set(error.evidence_refs) == {str(culprit.id)}
        assert "9.5" in error.requirement_refs
        assert "9.6" in error.requirement_refs
    else:  # pragma: no cover
        raise AssertionError("a single unrecorded trust assumption must fail closed")
