"""Unit tests for the preview-security ecosystem obligation generator (Task 7.5).

These tests exercise Requirement 9.8 / design Property 17 against the real
Task 7.4 evaluator, evidence layer, and Claim Guard (no mocks):

* every security-sensitive package/capability whose observed status is a preview
  tier (``Installed_Preview`` / ``Repo_Preview``) accrues the four obligation
  Ecosystem_Gaps: maintenance, certification, deployment, vulnerability response;
* non-preview security-sensitive subjects accrue no obligation gaps here;
* an obligation is subtracted only when direct GA-tier, present-tense-permitted
  evidence independently closes it, and closing one obligation never closes
  another;
* generation is order independent and every emitted gap is a well-formed
  Ecosystem_Gap referencing its subject.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    evaluate_application_ecosystem_release,
)
from tools.universe_os_gap_analysis.evaluators.preview_security_obligations import (
    OBLIGATION_SPECS,
    PreviewSecurityObligationGenerator,
    SecurityObligation,
    SubjectKind,
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
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-preview-security-test")

# Security-sensitive capability / responsibility subject ids from Task 7.4.
_SECURITY_MAINT = "capability-ecosystem-security-maintenance"
_SBOM = "capability-release-sbom"
_CRYPTO = "responsibility-app-crypto"
_TLS = "responsibility-app-tls"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.REPO_PREVIEW,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/official_package_tiering.md",
    limitations: tuple[str, ...] = (),
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, origin.value),
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


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


# A bundle with no preview security evidence: security-sensitive subjects exist
# in the Task 7.4 checklist but their observed status is Unknown, not preview.
_EMPTY_BUNDLE = _bundle(
    _record(
        claim_key="source:spec/grammar.ebnf",
        claim="Lexical grammar for identifiers.",
        status=EvidenceStatus.EXPERIMENTAL,
        source_path="spec/grammar.ebnf",
    )
)

_ALL_OBLIGATIONS = {spec.obligation for spec in OBLIGATION_SPECS}


class NoPreviewSecuritySubjectTests(unittest.TestCase):
    def test_no_preview_security_evidence_yields_no_gaps(self) -> None:
        result = evaluate_preview_security_obligations(_EMPTY_BUNDLE)
        self.assertEqual(result.subjects, ())
        self.assertEqual(result.obligation_gaps, ())

    def test_ga_security_subject_is_not_preview(self) -> None:
        """A security-sensitive capability at GA maturity accrues no obligations."""

        bundle = _bundle(
            _record(
                claim_key="release:sbom",
                claim="SBOM software bill of materials artifacts are published.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            )
        )
        result = evaluate_preview_security_obligations(bundle)
        self.assertIsNone(result.subject_for(_SBOM))
        self.assertEqual(result.gaps_for_subject(_SBOM), ())


class PreviewSubjectObligationTests(unittest.TestCase):
    def test_preview_security_capability_accrues_four_obligations(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance for the crypto package is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
            )
        )
        result = evaluate_preview_security_obligations(bundle)
        subject = result.subject_for(_SECURITY_MAINT)
        assert subject is not None
        self.assertEqual(subject.kind, SubjectKind.CAPABILITY)
        self.assertEqual(subject.observed_status, EvidenceStatus.REPO_PREVIEW)
        self.assertEqual(set(subject.open_obligations), _ALL_OBLIGATIONS)
        self.assertEqual(subject.closed_obligations, ())

        gaps = result.gaps_for_subject(_SECURITY_MAINT)
        self.assertEqual(len(gaps), 4)
        for gap in gaps:
            self.assertEqual(gap.primary_category, GapCategory.ECOSYSTEM)
            self.assertEqual(gap.secondary_categories, ())
            self.assertEqual(gap.severity, Severity.HIGH)
            self.assertEqual(gap.current_status, EvidenceStatus.REPO_PREVIEW)
            self.assertEqual(gap.recommended_owner_area, "Security")
            self.assertEqual(
                {str(ref) for ref in gap.domain_ids}, {_SECURITY_MAINT}
            )

    def test_preview_security_responsibility_accrues_obligations(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="package:crypto",
                claim="The cryptography package is installed preview.",
                status=EvidenceStatus.INSTALLED_PREVIEW,
                source_path="docs/support_matrix.md",
            )
        )
        result = evaluate_preview_security_obligations(bundle)
        subject = result.subject_for(_CRYPTO)
        assert subject is not None
        self.assertEqual(subject.kind, SubjectKind.RESPONSIBILITY)
        self.assertEqual(subject.target_level, TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertEqual(len(result.gaps_for_subject(_CRYPTO)), 4)

    def test_each_obligation_has_a_distinct_gap(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
            )
        )
        result = evaluate_preview_security_obligations(bundle)
        for obligation in SecurityObligation:
            gap = result.obligation_gap(_SECURITY_MAINT, obligation)
            self.assertIsNotNone(gap, msg=f"missing obligation gap: {obligation}")
        ids = {str(g.id) for g in result.gaps_for_subject(_SECURITY_MAINT)}
        self.assertEqual(len(ids), 4)


class ObligationClosingTests(unittest.TestCase):
    def _crypto_preview(self) -> EvidenceRecord:
        return _record(
            claim_key="package:crypto",
            claim="The cryptography package is installed preview.",
            status=EvidenceStatus.INSTALLED_PREVIEW,
            source_path="docs/support_matrix.md",
        )

    def test_ga_evidence_closes_one_obligation(self) -> None:
        """GA-tier, present-tense-permitted evidence closes only its obligation."""

        bundle = _bundle(
            self._crypto_preview(),
            _record(
                claim_key="process:vuln-response",
                claim="A coordinated disclosure and vulnerability response process is operated.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        subject = result.subject_for(_CRYPTO)
        assert subject is not None
        self.assertIn(SecurityObligation.VULNERABILITY_RESPONSE, subject.closed_obligations)
        self.assertNotIn(SecurityObligation.VULNERABILITY_RESPONSE, subject.open_obligations)
        # The closed obligation has no gap; the other three remain.
        self.assertIsNone(
            result.obligation_gap(_CRYPTO, SecurityObligation.VULNERABILITY_RESPONSE)
        )
        self.assertEqual(len(result.gaps_for_subject(_CRYPTO)), 3)
        for obligation in (
            SecurityObligation.MAINTENANCE,
            SecurityObligation.CERTIFICATION,
            SecurityObligation.DEPLOYMENT,
        ):
            self.assertIsNotNone(result.obligation_gap(_CRYPTO, obligation))

    def test_preview_evidence_does_not_close_obligation(self) -> None:
        bundle = _bundle(
            self._crypto_preview(),
            _record(
                claim_key="process:vuln-response",
                claim="A vulnerability response process is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path="docs/official_package_tiering.md",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        self.assertEqual(len(result.gaps_for_subject(_CRYPTO)), 4)

    def test_experimental_evidence_does_not_close_obligation(self) -> None:
        bundle = _bundle(
            self._crypto_preview(),
            _record(
                claim_key="process:vuln-response",
                claim="A vulnerability response process exists.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path="docs/official_package_tiering.md",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        self.assertEqual(len(result.gaps_for_subject(_CRYPTO)), 4)

    def test_ga_evidence_without_present_tense_does_not_close(self) -> None:
        """GA evidence that is not direct current implementation cannot close."""

        bundle = _bundle(
            self._crypto_preview(),
            _record(
                claim_key="process:certification",
                claim="Security certification is documented.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                # Specification is not a direct implementation kind -> the Claim
                # Guard does not permit present tense -> cannot close.
                evidence_kind=EvidenceKind.SPECIFICATION,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path="docs/official_package_tiering.md",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        self.assertEqual(len(result.gaps_for_subject(_CRYPTO)), 4)

    def test_all_four_obligations_closed_yields_no_gaps(self) -> None:
        bundle = _bundle(
            self._crypto_preview(),
            _record(
                claim_key="process:maintenance",
                claim="A sustained security maintenance process is operated in production.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            ),
            _record(
                claim_key="process:certification",
                claim="Security certification compliance is attested.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            ),
            _record(
                claim_key="process:deployment",
                claim="A supported production deployment path is operated.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            ),
            _record(
                claim_key="process:vuln-response",
                claim="A coordinated vulnerability response process is operated.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        subject = result.subject_for(_CRYPTO)
        assert subject is not None
        self.assertEqual(set(subject.closed_obligations), _ALL_OBLIGATIONS)
        self.assertEqual(result.gaps_for_subject(_CRYPTO), ())


class DeterminismAndReuseTests(unittest.TestCase):
    def test_generation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
            ),
            _record(
                claim_key="package:crypto",
                claim="The cryptography package is installed preview.",
                status=EvidenceStatus.INSTALLED_PREVIEW,
                source_path="docs/support_matrix.md",
            ),
        )
        forward = evaluate_preview_security_obligations(_bundle(*records))
        reverse = evaluate_preview_security_obligations(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.obligation_gaps],
            [str(g.id) for g in reverse.obligation_gaps],
        )
        self.assertEqual(
            [str(s.subject_id) for s in forward.subjects],
            [str(s.subject_id) for s in reverse.subjects],
        )

    def test_consumes_provided_task_74_evaluation(self) -> None:
        """The generator reuses a caller-provided Task 7.4 evaluation."""

        bundle = _bundle(
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
            )
        )
        evaluation = evaluate_application_ecosystem_release(bundle)
        result = PreviewSecurityObligationGenerator().generate(bundle, evaluation)
        self.assertIsNotNone(result.subject_for(_SECURITY_MAINT))
        self.assertEqual(len(result.gaps_for_subject(_SECURITY_MAINT)), 4)

    def test_gap_ids_are_unique_across_subjects(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance is repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
            ),
            _record(
                claim_key="package:crypto",
                claim="The cryptography package is installed preview.",
                status=EvidenceStatus.INSTALLED_PREVIEW,
                source_path="docs/support_matrix.md",
            ),
        )
        result = evaluate_preview_security_obligations(bundle)
        ids = [str(g.id) for g in result.obligation_gaps]
        self.assertEqual(len(ids), len(set(ids)))
        # Two preview security subjects, four obligations each.
        self.assertEqual(len(ids), 8)


if __name__ == "__main__":
    unittest.main()
