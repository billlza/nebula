"""Property-based test for Property 17 (Task 7.7).

Feature: nebula-universe-os-gap-analysis, Property 17: Preview security packages
create ecosystem obligations - for every security-sensitive package/capability
whose observed status is a preview tier (``Installed_Preview`` / ``Repo_Preview``)
the gap register contains ecosystem gaps covering maintenance, certification,
deployment, and vulnerability response, *unless direct evidence independently
closes each obligation*.

These tests drive the *real* ``evaluate_preview_security_obligations`` generator
(which itself consumes the real Task 7.4 application/ecosystem/release evaluator
and the real Claim Guard) over Hypothesis-generated evidence bundles. Nothing is
mocked and no result is asserted against a re-implementation of the obligation
rule, so the checks are non-tautological: the generator computes which
obligations are open/closed and the property constrains the outcome to be a
complete, disjoint partition of the four obligations whose *open* half is exactly
the set of well-formed Ecosystem_Gaps that were emitted.
"""

from __future__ import annotations

from hypothesis import assume, given, settings, strategies as st

from tools.universe_os_gap_analysis.evaluators.preview_security_obligations import (
    OBLIGATION_SPECS,
    SecurityObligation,
    evaluate_preview_security_obligations,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
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
    Severity,
    SourceLocation,
    VerificationState,
)

_REVISION_REF = reference("revision-property-17")

# The two preview tiers that make a security-sensitive subject accrue the four
# Requirement 9.8 obligations.
_PREVIEW_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {EvidenceStatus.INSTALLED_PREVIEW, EvidenceStatus.REPO_PREVIEW}
)

# GA-tier statuses. Only these can *close* an obligation, because the obligation
# is created precisely by preview (non-GA) maturity.
_GA_STATUSES: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
)

# Statuses that can never close an obligation (everything that is not GA-tier).
_NON_GA_STATUSES: tuple[EvidenceStatus, ...] = tuple(
    status for status in EvidenceStatus if status not in _GA_STATUSES
)

# The complete set of the four obligations, taken straight from the generator's
# own vocabulary so the test tracks the product definition.
_ALL_OBLIGATIONS: frozenset[SecurityObligation] = frozenset(
    spec.obligation for spec in OBLIGATION_SPECS
)

# Preview claims that each contain a marker for one security-sensitive subject
# (three security responsibilities + four security ecosystem/release capabilities).
# The ``crypto`` marker set is intentionally disjoint from every obligation
# marker so a crypto subject stays preview even when GA obligation evidence is
# added (used by the full-closing property below).
_SECURITY_SUBJECT_CLAIMS: tuple[str, ...] = (
    "The cryptography package cipher suite is provided as a preview package.",
    "The TLS transport layer security stack is a preview package.",
    "Authentication login credential handling is a preview package.",
    "Code signing signature production is a preview package.",
    "Notarization of release artifacts is a preview package.",
    "SBOM software bill of materials publishing is a preview package.",
    "Build provenance supply-chain provenance is a preview package.",
    "Signed attestation artifact attestation is a preview package.",
)

# A crypto preview claim whose markers do not collide with any obligation marker.
_CRYPTO_PREVIEW_CLAIM = _SECURITY_SUBJECT_CLAIMS[0]

# Free-text claims that carry each obligation's markers, used as adversarial
# closing candidates. The status/kind is chosen by the strategy, not baked in.
_OBLIGATION_MARKER_CLAIMS: tuple[str, ...] = (
    "A sustained security maintenance process is actively maintained.",
    "Security certification compliance certification is attested.",
    "A supported production deployment path deployment pipeline is operated.",
    "A coordinated disclosure vulnerability response incident response process runs.",
    "Some unrelated observation about tooling.",
)


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/official_package_tiering.md",
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id(
            "evidence", claim_key, claim, status.value, evidence_kind.value, origin.value
        ),
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
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: tuple[EvidenceRecord, ...]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key[record.claim_key] = by_claim_key.get(record.claim_key, ()) + (record,)
    return EvidenceBundle(records=records, by_claim_key=by_claim_key)


def _assert_partition_and_gaps(result) -> None:
    """Every preview security subject partitions the four obligations exactly.

    For each preview subject the open obligations and the closed obligations are
    disjoint, together cover all four obligations, and the open half is exactly
    the set of obligations for which a well-formed Ecosystem_Gap was emitted.
    """

    for subject in result.subjects:
        subject_id = str(subject.subject_id)
        # Subjects reported here are, by construction, preview + security.
        assert subject.observed_status in _PREVIEW_STATUSES

        open_obligations = set(subject.open_obligations)
        closed_obligations = set(subject.closed_obligations)

        # Disjoint, complete partition of the four obligations.
        assert open_obligations.isdisjoint(closed_obligations)
        assert open_obligations | closed_obligations == _ALL_OBLIGATIONS

        # The open obligations are exactly those with an emitted gap.
        gapped = {
            obligation
            for obligation in _ALL_OBLIGATIONS
            if result.obligation_gap(subject_id, obligation) is not None
        }
        assert gapped == open_obligations

        subject_gaps = result.gaps_for_subject(subject_id)
        assert len(subject_gaps) == len(open_obligations)

        # Every emitted gap is a well-formed Ecosystem_Gap for this subject.
        for gap in subject_gaps:
            assert gap.primary_category is GapCategory.ECOSYSTEM
            assert gap.secondary_categories == ()
            assert gap.severity is Severity.HIGH
            assert gap.current_status is subject.observed_status
            assert {str(ref) for ref in gap.domain_ids} == {subject_id}


@st.composite
def _preview_security_bundles(draw: st.DrawFn) -> tuple[EvidenceRecord, ...]:
    """A bundle with >=1 preview security subject plus adversarial noise.

    At least one preview record for a security-sensitive subject guarantees the
    generator has a subject to reason about; extra records span the full
    status/kind space (including GA obligation-marker candidates) so the closing
    logic is genuinely exercised rather than assumed away.
    """

    preview_claims = draw(
        st.lists(
            st.sampled_from(_SECURITY_SUBJECT_CLAIMS), min_size=1, max_size=4
        )
    )
    records: list[EvidenceRecord] = []
    for index, claim in enumerate(preview_claims):
        records.append(
            _record(
                claim_key=f"preview:{index}",
                claim=claim,
                status=draw(st.sampled_from(tuple(_PREVIEW_STATUSES))),
            )
        )

    extra = draw(
        st.lists(
            st.tuples(
                st.sampled_from(_OBLIGATION_MARKER_CLAIMS),
                st.sampled_from(tuple(EvidenceStatus)),
                st.sampled_from(tuple(EvidenceKind)),
                st.sampled_from(tuple(RevisionOrigin)),
            ),
            min_size=0,
            max_size=6,
        )
    )
    for index, (claim, status, kind, origin) in enumerate(extra):
        records.append(
            _record(
                claim_key=f"extra:{index}",
                claim=claim,
                status=status,
                evidence_kind=kind,
                origin=origin,
            )
        )
    return tuple(records)


# Feature: nebula-universe-os-gap-analysis, Property 17: Preview security
# packages create ecosystem obligations - each preview security subject's four
# obligations are partitioned into open (gapped) and closed (evidence-satisfied)
# with no obligation lost, and every open obligation is a well-formed
# Ecosystem_Gap referencing the subject.
# **Validates: Requirements 9.8**
@given(records=_preview_security_bundles())
@settings(max_examples=200, deadline=None, print_blob=True)
def test_preview_security_obligations_are_complete_partition(
    records: tuple[EvidenceRecord, ...],
) -> None:
    """The real generator covers all four obligations for each preview subject."""

    result = evaluate_preview_security_obligations(_bundle(records))
    # The guaranteed preview record(s) must yield at least one preview subject.
    assume(len(result.subjects) >= 1)
    _assert_partition_and_gaps(result)


@st.composite
def _non_ga_bundles(draw: st.DrawFn) -> tuple[EvidenceRecord, ...]:
    """A bundle with >=1 preview security subject and only non-GA evidence.

    Without any GA-tier record no obligation can be closed, so every preview
    security subject must accrue all four obligation gaps.
    """

    preview_claims = draw(
        st.lists(
            st.sampled_from(_SECURITY_SUBJECT_CLAIMS), min_size=1, max_size=4
        )
    )
    records: list[EvidenceRecord] = []
    for index, claim in enumerate(preview_claims):
        records.append(
            _record(
                claim_key=f"preview:{index}",
                claim=claim,
                status=draw(st.sampled_from(tuple(_PREVIEW_STATUSES))),
            )
        )

    extra = draw(
        st.lists(
            st.tuples(
                st.sampled_from(_OBLIGATION_MARKER_CLAIMS),
                st.sampled_from(_NON_GA_STATUSES),
                st.sampled_from(tuple(EvidenceKind)),
            ),
            min_size=0,
            max_size=6,
        )
    )
    for index, (claim, status, kind) in enumerate(extra):
        records.append(
            _record(
                claim_key=f"extra:{index}",
                claim=claim,
                status=status,
                evidence_kind=kind,
            )
        )
    return tuple(records)


# Feature: nebula-universe-os-gap-analysis, Property 17: Preview security
# packages create ecosystem obligations - preview/experimental/planned (non-GA)
# evidence never closes an obligation, so with no GA evidence every preview
# security subject accrues all four obligation gaps.
# **Validates: Requirements 9.8**
@given(records=_non_ga_bundles())
@settings(max_examples=100, deadline=None, print_blob=True)
def test_non_ga_evidence_never_closes_obligations(
    records: tuple[EvidenceRecord, ...],
) -> None:
    """Absent GA evidence, all four obligations stay open for every subject."""

    result = evaluate_preview_security_obligations(_bundle(records))
    assume(len(result.subjects) >= 1)
    for subject in result.subjects:
        assert set(subject.open_obligations) == _ALL_OBLIGATIONS
        assert subject.closed_obligations == ()
        assert len(result.gaps_for_subject(str(subject.subject_id))) == 4
    # The overarching partition invariant still holds.
    _assert_partition_and_gaps(result)


# Feature: nebula-universe-os-gap-analysis, Property 17: Preview security
# packages create ecosystem obligations - when direct GA-tier, present-tense
# implementation evidence independently closes all four obligations, a preview
# security subject accrues no obligation gaps at all.
# **Validates: Requirements 9.8**
@given(
    maintenance_status=st.sampled_from(_GA_STATUSES),
    certification_status=st.sampled_from(_GA_STATUSES),
    deployment_status=st.sampled_from(_GA_STATUSES),
    vulnerability_status=st.sampled_from(_GA_STATUSES),
)
@settings(max_examples=100, deadline=None, print_blob=True)
def test_direct_ga_evidence_closes_every_obligation(
    maintenance_status: EvidenceStatus,
    certification_status: EvidenceStatus,
    deployment_status: EvidenceStatus,
    vulnerability_status: EvidenceStatus,
) -> None:
    """Direct GA evidence for all four obligations removes every gap."""

    closing = (
        ("close:maintenance", _OBLIGATION_MARKER_CLAIMS[0], maintenance_status),
        ("close:certification", _OBLIGATION_MARKER_CLAIMS[1], certification_status),
        ("close:deployment", _OBLIGATION_MARKER_CLAIMS[2], deployment_status),
        ("close:vuln", _OBLIGATION_MARKER_CLAIMS[3], vulnerability_status),
    )
    records = [
        # A crypto preview subject whose markers never collide with obligation
        # markers, so the GA closing records cannot promote it out of preview.
        _record(
            claim_key="preview:crypto",
            claim=_CRYPTO_PREVIEW_CLAIM,
            status=EvidenceStatus.INSTALLED_PREVIEW,
        )
    ]
    for claim_key, claim, status in closing:
        records.append(
            _record(
                claim_key=claim_key,
                claim=claim,
                status=status,
                # Direct current-revision implementation evidence: the Claim
                # Guard permits present tense, which is required to close.
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            )
        )

    result = evaluate_preview_security_obligations(_bundle(tuple(records)))
    assume(len(result.subjects) >= 1)
    for subject in result.subjects:
        assert set(subject.closed_obligations) == _ALL_OBLIGATIONS
        assert subject.open_obligations == ()
        assert result.gaps_for_subject(str(subject.subject_id)) == ()
    _assert_partition_and_gaps(result)


if __name__ == "__main__":  # pragma: no cover
    import pytest

    raise SystemExit(pytest.main([__file__, "-q"]))
