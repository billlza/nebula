"""Unit tests for the userspace/system-service/product-shell evaluator (Task 7.2).

These tests exercise Requirement 10.4-10.6 and 12.6 against the real evidence and
claim-guard layers (no mocks): the Nebula-owned process/syscall boundary rule
(all T4 domains stay Maturity_Score 0 without it), host-owned/thin-host adjacency
staying at T0, and the independence of the isolation/userspace/update-recovery/
shell gate groups.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.userspace_services_shell import (
    USERSPACE_CHECKLIST,
    ProcessBoundaryStrength,
    UserspaceGateGroup,
    UserspaceServicesShellEvaluator,
    evaluate_userspace_services_shell,
    gate_id,
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
    Ownership,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-userspace-test")

_ISOLATION = "capability-userspace-process-isolation"
_SANDBOX = "capability-userspace-sandbox"
_IPC = "capability-userspace-ipc"
_FS = "capability-userspace-filesystem-storage"
_NET = "capability-userspace-network-stack"
_SVC = "capability-userspace-service-manager"
_IDENTITY = "capability-userspace-identity-policy-time-config"
_RUNTIME = "capability-userspace-user-runtime-command"
_APP = "capability-userspace-app-model-sdk"
_DIST = "capability-userspace-app-distribution"
_UPDATE = "capability-userspace-install-update-rollback"
_BACKUP = "capability-userspace-backup-recovery"
_SHELL = "capability-userspace-shell"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/universeos/kernel_boundary.md",
    ownership: Ownership | None = None,
    limitations: tuple[str, ...] = (),
    trust_assumptions: tuple[str, ...] = (),
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(ownership=ownership),
        limitations=limitations,
        trust_assumptions=trust_assumptions,
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class ProcessBoundaryClassificationTests(unittest.TestCase):
    def test_no_evidence_is_absent_boundary(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/grammar.ebnf",
                claim="Lexical grammar for identifiers.",
                source_path="spec/grammar.ebnf",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.ABSENT
        )

    def test_host_owned_service_manager_is_host_owned_only(self) -> None:
        """Requirement 4.6/10.6: hosted service manager is T0 adjacency, not a boundary."""

        bundle = _bundle(
            _record(
                claim_key="example:hosted-service-manager",
                claim=(
                    "A hosted service manager runs on the host OS and supervises "
                    "example services."
                ),
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.EXAMPLE,
                ownership=Ownership.HOST_OWNED,
                source_path="examples/service/main.nb",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.HOST_OWNED_ONLY
        )

    def test_nebula_owned_boundary_requires_direct_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:kernel/process.nb",
                claim=(
                    "A Nebula-owned process supervisor enforces address-space "
                    "isolation across user processes."
                ),
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/process.nb",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.NEBULA_OWNED
        )

    def test_boundary_marker_without_implementation_stays_absent(self) -> None:
        """A planned/spec-only boundary is not a Nebula-owned boundary."""

        bundle = _bundle(
            _record(
                claim_key="spec:kernel-boundary",
                claim="A future process isolation and syscall boundary is planned.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.ABSENT
        )


class ZeroMaturityWithoutBoundaryTests(unittest.TestCase):
    def test_all_t4_domains_zero_without_boundary(self) -> None:
        """Requirement 10.6: no Nebula-owned boundary means every T4 domain is 0."""

        bundle = _bundle(
            _record(
                claim_key="nonclaim:docs/universeos/kernel_boundary.md",
                claim=(
                    "No kernel, driver, scheduler, process isolation, storage, or "
                    "network stack exists."
                ),
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        for draft in result.domain_drafts:
            self.assertEqual(
                draft.maturity_score, MaturityScore.ABSENT, msg=str(draft.domain.id)
            )
            self.assertFalse(draft.satisfied)
            self.assertEqual(draft.domain.target_level, TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM)

    def test_host_owned_ga_services_do_not_raise_t4_maturity(self) -> None:
        """Host-owned GA services stay T0; they cannot raise any T4 domain above 0."""

        bundle = _bundle(
            _record(
                claim_key="example:hosted-filesystem",
                claim=(
                    "A hosted filesystem and storage stack, network stack, and "
                    "service manager run on the host OS."
                ),
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.EXAMPLE,
                ownership=Ownership.HOST_OWNED,
                source_path="examples/services/main.nb",
                limitations=("The host OS owns filesystem and network I/O.",),
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.HOST_OWNED_ONLY
        )
        for draft in result.domain_drafts:
            self.assertEqual(draft.maturity_score, MaturityScore.ABSENT)
            self.assertFalse(draft.satisfied)
        # The filesystem gap should note the host-owned/T0 limitation.
        fs_draft = result.draft_for(_FS)
        assert fs_draft is not None
        limitation_text = "\n".join(fs_draft.limitations).lower()
        self.assertIn("t0", limitation_text)

    def test_capability_without_direct_impl_stays_zero_even_with_boundary(self) -> None:
        """With a boundary but no capability implementation, the domain stays 0."""

        bundle = _bundle(
            _record(
                claim_key="source:kernel/process.nb",
                claim=(
                    "A Nebula-owned process supervisor enforces address-space isolation."
                ),
                status=EvidenceStatus.REPO_PREVIEW,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/process.nb",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.NEBULA_OWNED
        )
        # The isolation boundary domain is satisfied...
        isolation_draft = result.draft_for(_ISOLATION)
        assert isolation_draft is not None
        self.assertTrue(isolation_draft.satisfied)
        self.assertTrue(isolation_draft.nebula_owned)
        # ...but the filesystem capability has no implementation, so it stays 0.
        fs_draft = result.draft_for(_FS)
        assert fs_draft is not None
        self.assertEqual(fs_draft.maturity_score, MaturityScore.ABSENT)
        self.assertFalse(fs_draft.satisfied)
        self.assertIsNotNone(result.gap_for(_FS))


class GateIndependenceTests(unittest.TestCase):
    def test_four_independent_gate_groups_present(self) -> None:
        """Requirement 12.6: isolation/userspace/update-recovery/shell are separate gates."""

        groups = {item.gate_group for item in USERSPACE_CHECKLIST}
        self.assertEqual(
            groups,
            {
                UserspaceGateGroup.ISOLATION,
                UserspaceGateGroup.USERSPACE,
                UserspaceGateGroup.UPDATE_RECOVERY,
                UserspaceGateGroup.SHELL,
            },
        )

    def test_gate_ids_are_distinct_per_group(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No userspace platform exists.",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        ids = result.gate_ids()
        self.assertEqual(len(set(str(v) for v in ids.values())), 4)
        # Each domain references exactly its own gate group's gate.
        for draft in result.domain_drafts:
            expected = reference(gate_id(draft.gate_group))
            self.assertIn(expected, draft.domain.dependency_gate_ids)

    def test_update_recovery_and_shell_gates_are_separate(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No userspace platform exists.",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        update_gate = str(gate_id(UserspaceGateGroup.UPDATE_RECOVERY))
        shell_gate = str(gate_id(UserspaceGateGroup.SHELL))
        isolation_gate = str(gate_id(UserspaceGateGroup.ISOLATION))
        userspace_gate = str(gate_id(UserspaceGateGroup.USERSPACE))
        self.assertEqual(
            len({update_gate, shell_gate, isolation_gate, userspace_gate}), 4
        )
        update_drafts = result.drafts_for_gate(UserspaceGateGroup.UPDATE_RECOVERY)
        shell_drafts = result.drafts_for_gate(UserspaceGateGroup.SHELL)
        self.assertTrue(update_drafts)
        self.assertTrue(shell_drafts)
        self.assertFalse(
            {str(d.domain.id) for d in update_drafts}
            & {str(d.domain.id) for d in shell_drafts}
        )


class GapClassificationTests(unittest.TestCase):
    def test_distribution_gap_is_ecosystem(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/architecture.md",
                claim="No application distribution mechanism exists.",
                source_path="docs/universeos/architecture.md",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        gap = result.gap_for(_DIST)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.ECOSYSTEM)

    def test_service_manager_gap_is_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No Nebula-owned service manager exists.",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        gap = result.gap_for(_SVC)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM)

    def test_host_owned_implemented_adds_verification_secondary(self) -> None:
        """Host-owned implemented adjacency raises claim-risk verification concern."""

        bundle = _bundle(
            _record(
                claim_key="example:hosted-service-manager",
                claim=(
                    "A hosted service manager runs on the host OS and supervises "
                    "services."
                ),
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.EXAMPLE,
                ownership=Ownership.HOST_OWNED,
                source_path="examples/service/main.nb",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        gap = result.gap_for(_SVC)
        assert gap is not None
        self.assertIn(GapCategory.VERIFICATION, gap.secondary_categories)
        self.assertNotEqual(gap.primary_category, GapCategory.VERIFICATION)
        self.assertGreaterEqual(gap.claim_risk, 1)


class CoverageAndDeterminismTests(unittest.TestCase):
    def test_all_thirteen_domains_are_drafted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No userspace platform exists.",
            )
        )
        result = evaluate_userspace_services_shell(bundle)
        drafted = {str(d.domain.id) for d in result.domain_drafts}
        expected = {str(item.capability_id) for item in USERSPACE_CHECKLIST}
        self.assertEqual(drafted, expected)
        self.assertEqual(len(drafted), len(USERSPACE_CHECKLIST))

    def test_required_domains_are_covered(self) -> None:
        """Every Requirement 10.4/10.5 concept appears in the checklist markers."""

        required_keywords = (
            "filesystem",
            "storage",
            "network",
            "service manager",
            "identity",
            "policy",
            "time",
            "configuration",
            "installation",
            "update",
            "rollback",
            "backup",
            "recovery",
            "user runtime",
            "command",
            "application model",
            "gui",
            "accessibility",
            "sandbox",
            "distribution",
            "sdk",
        )
        all_markers = "\n".join(
            marker for item in USERSPACE_CHECKLIST for marker in item.markers
        ).lower()
        for keyword in required_keywords:
            self.assertIn(keyword, all_markers, msg=f"missing coverage for {keyword}")

    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="nonclaim:docs/universeos/kernel_boundary.md",
                claim="No process isolation exists.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            ),
            _record(
                claim_key="example:hosted-filesystem",
                claim="A hosted filesystem runs on the host OS.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.EXAMPLE,
                ownership=Ownership.HOST_OWNED,
                source_path="examples/fs/main.nb",
            ),
            _record(
                claim_key="source:docs/universeos/architecture.md",
                claim="No GUI shell or accessibility shell exists.",
                source_path="docs/universeos/architecture.md",
            ),
        )
        forward = evaluate_userspace_services_shell(_bundle(*records))
        reverse = evaluate_userspace_services_shell(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps], [str(g.id) for g in reverse.gaps]
        )
        self.assertEqual(
            forward.process_boundary_strength, reverse.process_boundary_strength
        )

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/universeos/kernel_boundary.md",
                claim="No userspace platform exists.",
            )
        )
        guarded = guard_evidence(bundle)
        result = UserspaceServicesShellEvaluator().evaluate(bundle, guarded)
        self.assertEqual(
            result.process_boundary_strength, ProcessBoundaryStrength.ABSENT
        )


if __name__ == "__main__":
    unittest.main()
