from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis import (
    CLM_UNRECORDED_ASSUMPTION_CODE,
    AssumptionCategory,
    AssumptionKind,
    TrustAssumptionAuditError,
    TrustAssumptionAuditor,
    audit_trust_assumptions,
)
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
from tools.universe_os_gap_analysis.trust_audit import RecordAssumptionAudit

_REVISION_REF = reference("revision-trust-audit-test")


def _record(
    *,
    claim_key: str,
    claim: str,
    source_path: str = "spec/safety_contract.md",
    limitations: tuple[str, ...] = (),
    trust_assumptions: tuple[str, ...] = (),
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, claim),
        claim_key=claim_key,
        claim=claim,
        status=EvidenceStatus.EXPERIMENTAL,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=limitations,
        trust_assumptions=trust_assumptions,
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class ExclusionDetectionTests(unittest.TestCase):
    """Requirement 6.6: opaque/dynamic/FFI/unsafe exclusions must be recorded."""

    def test_unsafe_exclusion_recorded_in_limitations_is_complete(self) -> None:
        record = _record(
            claim_key="cap:safety",
            claim="The safety guarantee excludes unsafe blocks and FFI boundaries.",
            limitations=(
                "Guarantee does not cover unsafe code.",
                "FFI boundaries are outside the guarantee.",
            ),
        )
        report = TrustAssumptionAuditor().audit(_bundle([record]))
        audit = report.audit_for(record.id)
        assert audit is not None
        self.assertIn(AssumptionCategory.UNSAFE_EXCLUSION, audit.detected)
        self.assertIn(AssumptionCategory.FFI_EXCLUSION, audit.detected)
        self.assertTrue(audit.is_complete)
        self.assertTrue(report.is_complete)

    def test_unrecorded_unsafe_exclusion_fails_closed(self) -> None:
        record = _record(
            claim_key="cap:safety",
            claim="The safety guarantee excludes unsafe blocks.",
        )
        report = TrustAssumptionAuditor().audit(_bundle([record]))
        self.assertFalse(report.is_complete)
        audit = report.audit_for(record.id)
        self.assertEqual(audit.unrecorded, (AssumptionCategory.UNSAFE_EXCLUSION,))
        with self.assertRaises(TrustAssumptionAuditError) as ctx:
            report.enforce()
        self.assertEqual(ctx.exception.code, CLM_UNRECORDED_ASSUMPTION_CODE)
        self.assertIn(str(record.id), ctx.exception.evidence_refs)
        self.assertIn("6.6", ctx.exception.requirement_refs)
        self.assertIn("9.6", ctx.exception.requirement_refs)

    def test_opaque_and_dynamic_exclusions_detected(self) -> None:
        record = _record(
            claim_key="cap:analysis",
            claim="Aliasing analysis excludes opaque values and dynamic dispatch.",
        )
        audit = TrustAssumptionAuditor().audit([record]).audit_for(record.id)
        self.assertIn(AssumptionCategory.OPAQUE_EXCLUSION, audit.detected)
        self.assertIn(AssumptionCategory.DYNAMIC_EXCLUSION, audit.detected)


class TrustAssumptionDetectionTests(unittest.TestCase):
    """Requirement 9.5: trust assumptions must be recorded as limitations."""

    def test_all_four_trust_assumptions_detected(self) -> None:
        record = _record(
            claim_key="cap:build",
            claim=(
                "Reproducibility assumes a trusted toolchain, cooperative "
                "descendant processes, a caller-controlled directory, and the "
                "host security service."
            ),
        )
        audit = TrustAssumptionAuditor().audit([record]).audit_for(record.id)
        self.assertEqual(
            set(audit.detected),
            {
                AssumptionCategory.TRUSTED_TOOL,
                AssumptionCategory.COOPERATIVE_DESCENDANT,
                AssumptionCategory.CALLER_CONTROLLED_DIRECTORY,
                AssumptionCategory.HOST_SECURITY_SERVICE,
            },
        )

    def test_trust_assumption_recorded_in_trust_assumptions_field(self) -> None:
        record = _record(
            claim_key="cap:build",
            claim="Reproducibility assumes a trusted toolchain.",
            trust_assumptions=("Assumes a trusted toolchain is present.",),
        )
        report = audit_trust_assumptions(_bundle([record]))
        self.assertTrue(report.is_complete)

    def test_partial_recording_leaves_set_difference(self) -> None:
        record = _record(
            claim_key="cap:build",
            claim=(
                "Assumes a trusted tool and a caller-controlled directory."
            ),
            limitations=("Assumes a trusted tool.",),
        )
        audit = TrustAssumptionAuditor().audit([record]).audit_for(record.id)
        self.assertIn(AssumptionCategory.TRUSTED_TOOL, audit.recorded)
        self.assertEqual(
            audit.unrecorded, (AssumptionCategory.CALLER_CONTROLLED_DIRECTORY,)
        )

    def test_unrecorded_trust_assumption_fails_closed_with_9_5(self) -> None:
        record = _record(
            claim_key="cap:build",
            claim="Assumes the host security service enforces isolation.",
        )
        with self.assertRaises(TrustAssumptionAuditError) as ctx:
            audit_trust_assumptions([record])
        self.assertIn("9.5", ctx.exception.requirement_refs)
        self.assertIn("9.6", ctx.exception.requirement_refs)


class NoAssumptionTests(unittest.TestCase):
    def test_record_without_assumptions_is_complete(self) -> None:
        record = _record(
            claim_key="cap:frontend",
            claim="The parser accepts the documented grammar.",
        )
        audit = TrustAssumptionAuditor().audit([record]).audit_for(record.id)
        self.assertEqual(audit.detected, ())
        self.assertEqual(audit.unrecorded, ())
        self.assertTrue(audit.is_complete)

    def test_empty_bundle_is_complete(self) -> None:
        report = TrustAssumptionAuditor().audit([])
        self.assertTrue(report.is_complete)
        self.assertEqual(report.validation_findings(), ())


class ValidationFindingTests(unittest.TestCase):
    """Requirement 9.6: findings cite affected records and requirement refs."""

    def test_findings_are_emitted_per_unrecorded_category(self) -> None:
        record = _record(
            claim_key="cap:safety",
            claim="Excludes unsafe boundaries and assumes a trusted tool.",
        )
        report = TrustAssumptionAuditor().audit([record])
        findings = report.validation_findings()
        self.assertEqual(len(findings), 2)
        for finding in findings:
            self.assertIs(finding.severity, FindingSeverity.ERROR)
            self.assertEqual(finding.code, CLM_UNRECORDED_ASSUMPTION_CODE)
            self.assertIn("9.6", finding.requirement_refs)
            self.assertIn(reference(record.id), finding.object_refs)

    def test_complete_report_has_no_findings(self) -> None:
        record = _record(
            claim_key="cap:safety",
            claim="Excludes unsafe boundaries.",
            limitations=("The guarantee excludes unsafe code paths.",),
        )
        report = TrustAssumptionAuditor().audit([record])
        self.assertEqual(report.validation_findings(), ())


class SetDifferenceInvariantTests(unittest.TestCase):
    def test_unrecorded_must_equal_detected_minus_recorded(self) -> None:
        with self.assertRaisesRegex(ValueError, "detected minus recorded"):
            RecordAssumptionAudit(
                evidence_id=reference("e1"),
                claim_key="cap:x",
                detected=(AssumptionCategory.UNSAFE_EXCLUSION,),
                recorded=(),
                unrecorded=(),
            )

    def test_order_independent_result(self) -> None:
        records = [
            _record(claim_key="cap:a", claim="Excludes unsafe boundaries."),
            _record(claim_key="cap:b", claim="Assumes a trusted tool."),
        ]
        forward = TrustAssumptionAuditor().audit(records)
        backward = TrustAssumptionAuditor().audit(list(reversed(records)))
        self.assertEqual(
            [str(a.evidence_id) for a in forward.audits],
            [str(a.evidence_id) for a in backward.audits],
        )
        self.assertEqual(forward.unrecorded_evidence_refs, backward.unrecorded_evidence_refs)


class InputTypeTests(unittest.TestCase):
    def test_non_record_iterable_is_rejected(self) -> None:
        with self.assertRaises(TypeError):
            TrustAssumptionAuditor().audit([object()])  # type: ignore[list-item]


if __name__ == "__main__":
    unittest.main()
