"""Unit tests for the observability/security/reliability evaluator (Task 7.3).

These tests exercise Requirement 9.1-9.5 against the real evidence, claim-guard,
and trust-audit layers (no mocks):

* compiler diagnostics / hosted-service observability are distinguished from
  boot/kernel/driver/userspace observability (9.1, 9.2);
* the security and reliability capability groups are covered (9.3, 9.4);
* hosted / scoped-release evidence can never satisfy an OS-substrate capability
  (9.2 / Property 9);
* trust assumptions (trusted tools, cooperative descendants, caller-controlled
  directories, host security services) are surfaced as gap limitations (9.5).
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.observability_security_reliability import (
    OPERATIONS_CHECKLIST,
    AssessmentScope,
    ObservabilitySecurityReliabilityEvaluator,
    OperationsGroup,
    OperationsScopeStrength,
    evaluate_observability_security_reliability,
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
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-operations-test")

_COMPILER_DIAG = "capability-observability-compiler-diagnostics"
_HOSTED_TELEMETRY = "capability-observability-hosted-telemetry"
_KERNEL_OBS = "capability-observability-kernel"
_BOOT_OBS = "capability-observability-boot"
_USERSPACE_OBS = "capability-observability-userspace"
_CAP_SEC = "capability-security-capability-enforcement"
_SECURE_BOOT = "capability-security-secure-boot"
_SUPPLY_CHAIN = "capability-security-compiler-supply-chain"
_CRASH_CONSISTENCY = "capability-reliability-crash-consistency"
_DETERMINISTIC = "capability-reliability-deterministic-rebuild"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/universeos/architecture.md",
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
        scope=EvidenceScope(),
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


class ScopeDistinctionTests(unittest.TestCase):
    def test_compiler_diagnostics_satisfied_by_hosted_ga(self) -> None:
        """Requirement 9.1: hosted diagnostics tooling is satisfied by hosted GA."""

        bundle = _bundle(
            _record(
                claim_key="source:frontend/diagnostics.cpp",
                claim="Source diagnostics and LSP language server are implemented.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                evidence_kind=EvidenceKind.SOURCE,
                source_path="frontend/diagnostics.cpp",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        draft = result.draft_for(_COMPILER_DIAG)
        assert draft is not None
        self.assertTrue(draft.satisfied)
        self.assertEqual(draft.scope, AssessmentScope.COMPILER_HOSTED)
        self.assertIsNone(result.gap_for(_COMPILER_DIAG))

    def test_hosted_ga_does_not_satisfy_kernel_observability(self) -> None:
        """Requirement 9.2 / Property 9: hosted GA cannot satisfy OS observability."""

        bundle = _bundle(
            _record(
                claim_key="source:tooling/telemetry",
                claim=(
                    "Hosted service observability provides kernel log ingestion and "
                    "kernel metrics dashboards."
                ),
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.SOURCE,
                source_path="services/observe/main.nb",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        kernel = result.draft_for(_KERNEL_OBS)
        assert kernel is not None
        self.assertFalse(kernel.satisfied)
        self.assertEqual(kernel.maturity_score, MaturityScore.ABSENT)
        gap = result.gap_for(_KERNEL_OBS)
        assert gap is not None
        # Hosted implemented evidence touching an OS topic raises a claim-risk
        # verification concern.
        self.assertIn(GapCategory.VERIFICATION, gap.secondary_categories)
        self.assertGreaterEqual(gap.claim_risk, 1)

    def test_os_substrate_satisfied_only_by_non_hosted_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:kernel/observe.nb",
                claim="Kernel observability with kernel tracing and kernel metrics.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="kernel/observe.nb",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        kernel = result.draft_for(_KERNEL_OBS)
        assert kernel is not None
        self.assertTrue(kernel.satisfied)
        self.assertEqual(result.os_substrate_strength, OperationsScopeStrength.OS_SUBSTRATE)

    def test_hosted_only_strength_when_only_hosted_touches_os_topic(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="example:hosted-observe",
                claim="Hosted service observability visualizes kernel log streams.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.EXAMPLE,
                source_path="examples/observe/main.nb",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        self.assertEqual(
            result.os_substrate_strength, OperationsScopeStrength.COMPILER_HOSTED_ONLY
        )

    def test_absent_strength_without_os_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:frontend/diagnostics.cpp",
                claim="Source diagnostics are implemented.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="frontend/diagnostics.cpp",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        self.assertEqual(result.os_substrate_strength, OperationsScopeStrength.ABSENT)


class SecurityAndReliabilityTests(unittest.TestCase):
    def test_capability_enforcement_gap_is_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="nonclaim:kernel-boundary",
                claim="No capability enforcement or capability-based security exists.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
                source_path="docs/universeos/kernel_boundary.md",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        gap = result.gap_for(_CAP_SEC)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION)

    def test_secure_boot_absent_without_implementation(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="spec:secure-boot",
                claim="A secure boot / verified boot chain is planned.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/universeos/qemu_boot_hello.md",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        draft = result.draft_for(_SECURE_BOOT)
        assert draft is not None
        self.assertFalse(draft.satisfied)
        self.assertEqual(draft.maturity_score, MaturityScore.ABSENT)

    def test_crash_consistency_is_reliability_group(self) -> None:
        item = next(i for i in OPERATIONS_CHECKLIST if str(i.capability_id) == _CRASH_CONSISTENCY)
        self.assertEqual(item.group, OperationsGroup.RELIABILITY)
        self.assertEqual(item.scope, AssessmentScope.OS_SUBSTRATE)


class TrustAssumptionSurfacingTests(unittest.TestCase):
    def test_deterministic_rebuild_records_trust_assumptions(self) -> None:
        """Requirement 9.5: disclosed trust assumptions become gap limitations."""

        bundle = _bundle(
            _record(
                claim_key="test:deterministic-build",
                claim=(
                    "Deterministic rebuild is verified, but reproducibility assumes a "
                    "trusted toolchain and a caller-controlled directory."
                ),
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.TEST_DEFINITION,
                source_path="tests/README.md",
                limitations=(
                    "Reproducibility assumes a trusted toolchain.",
                    "Reproducibility writes into a caller-controlled directory.",
                ),
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        gap = result.gap_for(_DETERMINISTIC)
        assert gap is not None
        limitation_text = "\n".join(gap.acceptance_evidence).lower()
        self.assertIn("trusted toolchain", limitation_text)
        self.assertIn("caller-controlled directory", limitation_text)
        draft = result.draft_for(_DETERMINISTIC)
        assert draft is not None
        joined = "\n".join(draft.limitations).lower()
        self.assertIn("trusted toolchain", joined)

    def test_records_without_trust_assumptions_do_not_add_limitations(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:kernel/observe.nb",
                claim="Kernel observability with kernel tracing.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/universeos/kernel_boundary.md",
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        draft = result.draft_for(_KERNEL_OBS)
        assert draft is not None
        # Only the item's own non-claims are present, no extra trust text.
        joined = "\n".join(draft.limitations).lower()
        self.assertNotIn("trusted toolchain", joined)


class CoverageAndDeterminismTests(unittest.TestCase):
    def test_all_domains_drafted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="nonclaim:kernel-boundary",
                claim="No OS operations exist.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        drafted = {str(d.domain.id) for d in result.domain_drafts}
        expected = {str(item.capability_id) for item in OPERATIONS_CHECKLIST}
        self.assertEqual(drafted, expected)

    def test_required_keywords_are_covered(self) -> None:
        """Every Requirement 9.1/9.3/9.4 concept appears in the checklist markers."""

        required_keywords = (
            "diagnostic",
            "lsp",
            "formatter",
            "explain data",
            "debugger",
            "stack trace",
            "symbol",
            "crash dump",
            "profiling",
            "tracing",
            "metric",
            "logging",
            "log correlation",
            "kernel log",
            "boot",
            "driver",
            "userspace observability",
            "supply chain",
            "artifact integrity",
            "package trust",
            "unsafe audit",
            "capability",
            "process isolation",
            "privilege separation",
            "secure boot",
            "secret",
            "crypto",
            "update rollback",
            "incident response",
            "bounded execution",
            "containment",
            "transactional",
            "crash consistency",
            "power-loss",
            "deterministic rebuild",
            "recovery",
        )
        all_markers = "\n".join(
            marker for item in OPERATIONS_CHECKLIST for marker in item.markers
        ).lower()
        for keyword in required_keywords:
            self.assertIn(keyword, all_markers, msg=f"missing coverage for {keyword}")

    def test_three_gate_groups_are_distinct(self) -> None:
        groups = {item.group for item in OPERATIONS_CHECKLIST}
        self.assertEqual(
            groups,
            {
                OperationsGroup.OBSERVABILITY,
                OperationsGroup.SECURITY,
                OperationsGroup.RELIABILITY,
            },
        )
        gate_values = {str(gate_id(g)) for g in OperationsGroup}
        self.assertEqual(len(gate_values), 3)
        bundle = _bundle(
            _record(
                claim_key="nonclaim:kernel-boundary",
                claim="No OS operations exist.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            )
        )
        result = evaluate_observability_security_reliability(bundle)
        for draft in result.domain_drafts:
            expected = reference(gate_id(draft.group))
            self.assertIn(expected, draft.domain.dependency_gate_ids)

    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="nonclaim:kernel-boundary",
                claim="No kernel observability or secure boot exists.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            ),
            _record(
                claim_key="source:frontend/diagnostics.cpp",
                claim="Compiler diagnostics and LSP are implemented.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="frontend/diagnostics.cpp",
            ),
            _record(
                claim_key="test:deterministic-build",
                claim="Reproducible build assumes a trusted toolchain.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.TEST_DEFINITION,
                source_path="tests/README.md",
                limitations=("Assumes a trusted toolchain.",),
            ),
        )
        forward = evaluate_observability_security_reliability(_bundle(*records))
        reverse = evaluate_observability_security_reliability(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps], [str(g.id) for g in reverse.gaps]
        )
        self.assertEqual(forward.os_substrate_strength, reverse.os_substrate_strength)

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="nonclaim:kernel-boundary",
                claim="No OS operations exist.",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            )
        )
        guarded = guard_evidence(bundle)
        result = ObservabilitySecurityReliabilityEvaluator().evaluate(bundle, guarded)
        self.assertEqual(result.os_substrate_strength, OperationsScopeStrength.ABSENT)


if __name__ == "__main__":
    unittest.main()
