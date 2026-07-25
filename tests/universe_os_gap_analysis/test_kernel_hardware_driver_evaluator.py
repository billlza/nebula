"""Unit tests for the kernel/hardware/driver evaluator (Task 7.1).

These tests exercise Requirement 10.1-10.3, 10.6, and 12.6 against the real
evidence and claim-guard layers (no mocks): every OS-substrate subsystem is an
independent domain/gap, and any subsystem lacking direct implementation evidence
scores 0 (absent). A future QEMU boot hello or prerequisite gate does not lift a
subsystem.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.kernel_hardware_driver import (
    KERNEL_HARDWARE_DRIVER_CHECKLIST,
    CapabilityAspect,
    KernelHardwareDriverEvaluator,
    evaluate_kernel_hardware_driver,
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
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-kernel-hw-driver-test")

# A handful of representative subsystem IDs across the three aspects.
_HW_MMU = "capability-hw-mmu"
_HW_INTERRUPTS = "capability-hw-interrupts"
_KERNEL_SCHEDULER = "capability-kernel-scheduler"
_KERNEL_SYSCALL = "capability-kernel-syscall-dispatch"
_KERNEL_ISOLATION = "capability-kernel-address-space-isolation"
_DRIVER_DMA_SAFETY = "capability-driver-dma-safety"
_DRIVER_STORAGE = "capability-driver-storage-devices"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SPECIFICATION,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/universeos/kernel_boundary.md",
    verification_state: VerificationState = VerificationState.NOT_RUN,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, evidence_kind.value),
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
        verification_state=verification_state,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class CoverageAndIndependenceTests(unittest.TestCase):
    def test_every_requirement_10_subsystem_is_independent(self) -> None:
        """Requirement 10.1-10.3 / 12.6: each subsystem is its own domain and gap."""

        result = evaluate_kernel_hardware_driver(_bundle())
        # 14 hardware/firmware + 12 kernel + 12 driver subsystems.
        self.assertEqual(len(KERNEL_HARDWARE_DRIVER_CHECKLIST), 38)
        self.assertEqual(len(result.domain_drafts), 38)
        self.assertEqual(len(result.gaps), 38)

        drafted_ids = {str(draft.domain.id) for draft in result.domain_drafts}
        expected = {str(item.capability_id) for item in KERNEL_HARDWARE_DRIVER_CHECKLIST}
        self.assertEqual(drafted_ids, expected)

        # Each domain has exactly one dedicated gap and each gap references
        # exactly one domain (independence, Requirement 12.6).
        for draft in result.domain_drafts:
            gap = result.gap_for(str(draft.domain.id))
            assert gap is not None
            self.assertEqual(len(gap.domain_ids), 1)
            self.assertEqual(str(gap.domain_ids[0]), str(draft.domain.id))

        # Gap IDs are unique across all subsystems.
        gap_ids = [str(gap.id) for gap in result.gaps]
        self.assertEqual(len(gap_ids), len(set(gap_ids)))

    def test_three_aspects_are_all_present(self) -> None:
        result = evaluate_kernel_hardware_driver(_bundle())
        self.assertEqual(len(result.drafts_for_aspect(CapabilityAspect.HARDWARE)), 14)
        self.assertEqual(len(result.drafts_for_aspect(CapabilityAspect.KERNEL)), 12)
        self.assertEqual(len(result.drafts_for_aspect(CapabilityAspect.DRIVER)), 12)

    def test_all_domains_target_t3(self) -> None:
        result = evaluate_kernel_hardware_driver(_bundle())
        for draft in result.domain_drafts:
            self.assertEqual(
                draft.domain.target_level, TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION
            )


class NoImplementationMeansZeroTests(unittest.TestCase):
    def test_empty_evidence_scores_every_subsystem_zero(self) -> None:
        """Requirement 10.6: absent direct implementation means Maturity_Score 0."""

        result = evaluate_kernel_hardware_driver(_bundle())
        for draft in result.domain_drafts:
            self.assertFalse(draft.has_direct_implementation)
            self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)
            self.assertEqual(int(draft.raw_maturity_score), 0)

    def test_boundary_document_specification_does_not_count_as_implementation(self) -> None:
        """A boundary/spec document about MMU is not implementation evidence (10.6)."""

        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No MMU implementation exists; the MMU model is future work.",
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        result = evaluate_kernel_hardware_driver(bundle)
        draft = result.draft_for(_HW_MMU)
        assert draft is not None
        self.assertFalse(draft.has_direct_implementation)
        self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)
        gap = result.gap_for(_HW_MMU)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)

    def test_planned_gate_evidence_does_not_count_as_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="gate:UOS-SCHED-001",
                claim="Planned scheduler gate for kernel scheduler work.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        result = evaluate_kernel_hardware_driver(bundle)
        draft = result.draft_for(_KERNEL_SCHEDULER)
        assert draft is not None
        self.assertFalse(draft.has_direct_implementation)
        self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)


class QemuHelloDoesNotLiftSubsystemsTests(unittest.TestCase):
    def test_boot_hello_execution_does_not_lift_kernel_or_driver_subsystems(self) -> None:
        """Requirement 10.6/10.7: a boot serial-hello does not imply kernel/driver support."""

        bundle = _bundle(
            _record(
                claim_key="execution:UOS-BOOT-005",
                claim="QEMU serial hello boot smoke produced serial output.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.TEST_EXECUTION,
                origin=RevisionOrigin.EXECUTION_ARTIFACT,
                source_path="docs/universeos/qemu_boot_hello.md",
                verification_state=VerificationState.VALIDATED,
            )
        )
        result = evaluate_kernel_hardware_driver(bundle)
        # The boot-hello claim mentions none of the kernel/driver markers, so no
        # subsystem gains direct implementation evidence.
        for capability_id in (
            _KERNEL_SCHEDULER,
            _KERNEL_SYSCALL,
            _KERNEL_ISOLATION,
            _HW_MMU,
            _HW_INTERRUPTS,
            _DRIVER_DMA_SAFETY,
            _DRIVER_STORAGE,
        ):
            draft = result.draft_for(capability_id)
            assert draft is not None
            self.assertFalse(draft.has_direct_implementation)
            self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)


class DirectImplementationEvidenceTests(unittest.TestCase):
    def test_direct_implementation_evidence_raises_only_its_own_subsystem(self) -> None:
        """A direct source implementation of one subsystem must not lift the others."""

        bundle = _bundle(
            _record(
                claim_key="source:kernel/mmu.nb",
                claim="MMU configuration and address translation control implemented.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/mmu.nb",
            )
        )
        result = evaluate_kernel_hardware_driver(bundle)

        mmu = result.draft_for(_HW_MMU)
        assert mmu is not None
        self.assertTrue(mmu.has_direct_implementation)
        self.assertEqual(mmu.raw_maturity_score, MaturityScore.NARROW_EXPERIMENT)
        self.assertGreater(int(mmu.raw_maturity_score), 0)

        # Every other subsystem remains absent (independence + Requirement 10.6).
        for draft in result.domain_drafts:
            if str(draft.domain.id) == _HW_MMU:
                continue
            self.assertFalse(draft.has_direct_implementation)
            self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)

    def test_specification_kind_with_implemented_status_is_not_implementation(self) -> None:
        """Only source/execution/artifact kinds count as direct implementation."""

        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="Kernel scheduler responsibilities are described.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        result = evaluate_kernel_hardware_driver(bundle)
        draft = result.draft_for(_KERNEL_SCHEDULER)
        assert draft is not None
        self.assertFalse(draft.has_direct_implementation)
        self.assertEqual(draft.raw_maturity_score, MaturityScore.ABSENT)


class DeterminismTests(unittest.TestCase):
    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="source:kernel/mmu.nb",
                claim="MMU address translation implemented.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/mmu.nb",
            ),
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="Scheduler and driver responsibilities documented as gaps.",
                evidence_kind=EvidenceKind.SPECIFICATION,
            ),
            _record(
                claim_key="source:kernel/timer.nb",
                claim="Hardware timer clock source driver implemented.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/timer.nb",
            ),
        )
        forward = evaluate_kernel_hardware_driver(_bundle(*records))
        reverse = evaluate_kernel_hardware_driver(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps],
            [str(g.id) for g in reverse.gaps],
        )
        self.assertEqual(
            [(str(d.domain.id), d.has_direct_implementation) for d in forward.domain_drafts],
            [(str(d.domain.id), d.has_direct_implementation) for d in reverse.domain_drafts],
        )

    def test_accepts_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="MMU is documented as future work only.",
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        guarded = guard_evidence(bundle)
        result = KernelHardwareDriverEvaluator().evaluate(bundle, guarded)
        draft = result.draft_for(_HW_MMU)
        assert draft is not None
        self.assertFalse(draft.has_direct_implementation)


class GapContentTests(unittest.TestCase):
    def test_absent_subsystem_gap_documents_zero_and_owner(self) -> None:
        result = evaluate_kernel_hardware_driver(_bundle())
        gap = result.gap_for(_KERNEL_SYSCALL)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION)
        self.assertIn("Maturity_Score is 0", gap.observed_fact)
        self.assertTrue(gap.acceptance_evidence)
        self.assertEqual(gap.recommended_owner_area, "Kernel")

    def test_gaps_carry_requirement_10_domains_only(self) -> None:
        result = evaluate_kernel_hardware_driver(_bundle())
        expected_domains = {str(item.capability_id) for item in KERNEL_HARDWARE_DRIVER_CHECKLIST}
        for gap in result.gaps:
            self.assertTrue(
                {str(ref) for ref in gap.domain_ids}.issubset(expected_domains)
            )


if __name__ == "__main__":
    unittest.main()
