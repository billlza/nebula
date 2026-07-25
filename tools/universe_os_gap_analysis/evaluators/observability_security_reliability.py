"""Debugging, observability, security, and reliability evaluator (Task 7.3).

This declarative evaluator covers Requirement 9.1-9.5 (the "Operations &
Ecosystem" operations slice of the design). It assesses three capability groups:

* **Observability (9.1, 9.2).** source diagnostics, LSP, formatter, explain
  data, debugger integration, stack traces, symbols, crash dumps, profiling,
  tracing, metrics, logs, and kernel/user log correlation.
* **Security (9.3).** compiler supply chain, artifact integrity, package trust,
  unsafe audit, capability security, process isolation, privilege separation,
  secure boot, secret lifecycle, crypto lifecycle, update rollback, and incident
  response.
* **Reliability (9.4).** bounded execution, containment, transactionality,
  crash consistency, power-loss durability, deterministic rebuilds, and recovery
  behavior.

The governing distinction (design "Operations & Ecosystem"; Requirement 9.2 and
Property 9) is:

    Compiler diagnostics and *hosted-service* observability are **not** boot,
    kernel, driver, or userspace observability. Hosted / scoped-release evidence
    can satisfy only its own hosted-tooling capability; it can never satisfy an
    OS-substrate observability, security, or reliability capability.

Accordingly each checklist item carries an :class:`AssessmentScope`. A
``COMPILER_HOSTED`` capability is satisfied by direct current hosted-tooling
implementation, while an ``OS_SUBSTRATE`` capability is satisfied only by direct
current implementation that the Claim Guard does **not** flag as
substrate-promotion-blocked (hosted example, compiler/tooling GA, backend SDK GA,
or a T0-only scope). This reuses the Task 4.3 Claim Guard's
``substrate_promotion_blocked`` flag so hosted observability can never be read as
OS observability.

Requirement 9.5 is surfaced by reusing the Task 4.4 trust auditor
(:class:`~tools.universe_os_gap_analysis.trust_audit.TrustAssumptionAuditor`):
every trusted-tool, cooperative-descendant, caller-controlled-directory, or
host-security-service assumption a matched record implies is recorded as a
limitation on the affected gap. Fail-closed enforcement of *unrecorded*
assumptions (Requirement 9.6) remains the trust auditor's job; this evaluator
only propagates the disclosed assumptions onto the relevant records/gaps.

Nothing here mutates evidence, assigns Hard-Gate scores (Task 8), ranks gaps
(Task 9), renders anything (Task 11), or edits sibling evaluator files.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from ..claim_guard import GuardedEvidence, guard_evidence
from ..evidence import EvidenceBundle
from ..identifiers import ReferenceId, StableId, reference, stable_id
from ..models import (
    CapabilityDomain,
    ClosedStrEnum,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    MaturityScore,
    Severity,
    TargetLevel,
)
from ..trust_audit import AssumptionCategory, TrustAssumptionAuditor

# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent.

# Statuses that assert a present-tense/current implementation to some scoped
# degree, mirroring the Claim Guard's implemented-status set.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Evidence kinds that count as direct implementation for a "current" capability.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Ordinal ranking of statuses, strongest first, for "strongest observed status".
_STATUS_STRENGTH: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
    EvidenceStatus.INSTALLED_PREVIEW,
    EvidenceStatus.REPO_PREVIEW,
    EvidenceStatus.EXPERIMENTAL,
    EvidenceStatus.PLANNED,
    EvidenceStatus.UNSUPPORTED,
    EvidenceStatus.UNKNOWN,
)

# The trust-assumption categories (Requirement 9.5) recognised by the Task 4.4
# auditor. Safety exclusions (6.6) are audited elsewhere and are not surfaced by
# this operations evaluator.
_TRUST_ASSUMPTION_CATEGORIES: frozenset[AssumptionCategory] = frozenset(
    {
        AssumptionCategory.TRUSTED_TOOL,
        AssumptionCategory.COOPERATIVE_DESCENDANT,
        AssumptionCategory.CALLER_CONTROLLED_DIRECTORY,
        AssumptionCategory.HOST_SECURITY_SERVICE,
    }
)


class AssessmentScope(ClosedStrEnum):
    """Whether a capability is compiler/hosted-service or OS-substrate (Requirement 9.2)."""

    #: Compiler diagnostics / hosted-service observability, hosted supply-chain
    #: tooling, or hosted reproducibility. Satisfiable by hosted evidence.
    COMPILER_HOSTED = "CompilerHosted"
    #: Boot / kernel / driver / userspace observability, OS security, or OS
    #: reliability. Never satisfiable by hosted / scoped-release evidence.
    OS_SUBSTRATE = "OsSubstrate"


class OperationsGroup(ClosedStrEnum):
    """The operations capability group a checklist item belongs to."""

    OBSERVABILITY = "observability"
    SECURITY = "security"
    RELIABILITY = "reliability"


class OperationsScopeStrength(ClosedStrEnum):
    """Headline classification of the strongest OS-substrate operations evidence."""

    #: No OS-substrate observability/security/reliability implementation exists.
    ABSENT = "Absent"
    #: Only compiler / hosted-service operations evidence exists (T0/T1 scope).
    COMPILER_HOSTED_ONLY = "CompilerHostedOnly"
    #: A direct, non-hosted OS-substrate operations implementation exists.
    OS_SUBSTRATE = "OsSubstrate"


# Map each target level to its parent (T-level umbrella) capability from
# ``catalog.CAPABILITY_DEFINITIONS`` for later reconciliation.
_PARENT_BY_TARGET: Mapping[TargetLevel, StableId] = {
    TargetLevel.T0_HOSTED_ADJACENCY: StableId("capability-t0-hosted-adjacency"),
    TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM: StableId("capability-t1-language-platform"),
    TargetLevel.T2_FREESTANDING_SUBSTRATE: StableId("capability-t2-freestanding-substrate"),
    TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION: StableId("capability-t3-kernel-foundation"),
    TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM: StableId("capability-t4-userspace-platform"),
    TargetLevel.T5_OPERABLE_UNIVERSE_OS: StableId("capability-t5-operable-os"),
}


@dataclass(frozen=True, slots=True)
class OperationsChecklistItem:
    """One declarative capability check for the operations domain."""

    capability_id: StableId
    name: str
    group: OperationsGroup
    scope: AssessmentScope
    target_level: TargetLevel
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    unsatisfied_category: GapCategory
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.group, OperationsGroup):
            raise TypeError("group must be an OperationsGroup")
        if not isinstance(self.scope, AssessmentScope):
            raise TypeError("scope must be an AssessmentScope")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.unsatisfied_category, GapCategory):
            raise TypeError("unsatisfied_category must be a GapCategory")
        for name in ("markers", "authoritative_paths", "requirement_refs", "acceptance_evidence"):
            value = tuple(getattr(self, name))
            if not value:
                raise ValueError(f"{name} must not be empty")
            object.__setattr__(self, name, value)
        object.__setattr__(self, "non_claims", tuple(self.non_claims))
        if not self.owner_area.strip():
            raise ValueError("owner_area must not be empty")


@dataclass(frozen=True, slots=True, kw_only=True)
class OperationsDomainDraft:
    """A per-capability draft: domain, observed evidence, and classification."""

    domain: CapabilityDomain
    group: OperationsGroup
    scope: AssessmentScope
    observed_status: EvidenceStatus
    maturity_score: MaturityScore
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    gap_id: ReferenceId | None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        if not isinstance(self.group, OperationsGroup):
            raise TypeError("group must be an OperationsGroup")
        if not isinstance(self.scope, AssessmentScope):
            raise TypeError("scope must be an AssessmentScope")
        if not isinstance(self.maturity_score, MaturityScore):
            raise TypeError("maturity_score must be a MaturityScore")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(value) for value in self.supporting_evidence_ids}, key=str)),
        )
        object.__setattr__(self, "limitations", tuple(self.limitations))
        if not isinstance(self.satisfied, bool):
            raise TypeError("satisfied must be a bool")
        if self.gap_id is not None:
            object.__setattr__(self, "gap_id", reference(self.gap_id))


@dataclass(frozen=True, slots=True)
class OperationsEvaluation:
    """The evaluator output: domain drafts, gaps, and the OS-substrate headline."""

    domain_drafts: tuple[OperationsDomainDraft, ...]
    gaps: tuple[GapEntry, ...]
    os_substrate_strength: OperationsScopeStrength

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(
            self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id)))
        )
        if not isinstance(self.os_substrate_strength, OperationsScopeStrength):
            raise TypeError("os_substrate_strength must be an OperationsScopeStrength")

    def draft_for(self, capability_id: str) -> OperationsDomainDraft | None:
        target = str(capability_id)
        for draft in self.domain_drafts:
            if str(draft.domain.id) == target:
                return draft
        return None

    def gap_for(self, capability_id: str) -> GapEntry | None:
        target = str(capability_id)
        for gap in self.gaps:
            if target in {str(ref) for ref in gap.domain_ids}:
                return gap
        return None

    def drafts_for_group(self, group: OperationsGroup) -> tuple[OperationsDomainDraft, ...]:
        return tuple(draft for draft in self.domain_drafts if draft.group is group)

    def gate_ids(self) -> Mapping[OperationsGroup, ReferenceId]:
        """Independent operations gate identifiers, one per group."""

        return {group: reference(gate_id(group)) for group in OperationsGroup}


def gate_id(group: OperationsGroup) -> StableId:
    """Stable identifier for an independent operations gate group."""

    if not isinstance(group, OperationsGroup):
        raise TypeError("group must be an OperationsGroup")
    return stable_id("gate", "operations", group.value)


OPERATIONS_CHECKLIST: tuple[OperationsChecklistItem, ...] = (
    # -- OBSERVABILITY: compiler / hosted-service scope (Requirement 9.1) --- #
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-compiler-diagnostics"),
        name="Compiler diagnostics, LSP, formatter, and explain data",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "source diagnostics",
            "compiler diagnostics",
            "diagnostic",
            "lsp",
            "language server",
            "formatter",
            "explain data",
            "explain output",
        ),
        authoritative_paths=("spec/compiler_pipeline.md", "tests/README.md"),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Compiler diagnostics, LSP, formatter, and explain-data tooling with "
            "direct current implementation evidence; scoped to hosted developer "
            "tooling, not OS observability.",
        ),
        non_claims=(
            "Compiler diagnostics and hosted developer tooling are not boot, "
            "kernel, driver, or userspace observability.",
        ),
        owner_area="Developer Tooling",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-debug-symbols"),
        name="Debugger integration, stack traces, symbols, and crash dumps",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "debugger",
            "debug integration",
            "stack trace",
            "backtrace",
            "symbol",
            "symbolication",
            "crash dump",
            "core dump",
            "debug info",
        ),
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Debugger integration, stack traces, symbol resolution, and crash-dump "
            "support with direct current implementation evidence in the hosted "
            "toolchain.",
        ),
        non_claims=(
            "Hosted debug tooling does not provide kernel or userspace crash "
            "observability for an OS substrate.",
        ),
        owner_area="Developer Tooling",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-hosted-telemetry"),
        name="Hosted-service profiling, tracing, metrics, and logs",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T0_HOSTED_ADJACENCY,
        markers=(
            "profiling",
            "profiler",
            "tracing",
            "trace span",
            "metrics",
            "metric",
            "logging",
            "log output",
            "hosted observability",
            "service observability",
        ),
        authoritative_paths=("docs/support_matrix.md",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Hosted-service profiling, tracing, metrics, and log tooling with "
            "direct current implementation evidence; scoped to hosted adjacency.",
        ),
        non_claims=(
            "Hosted-service observability runs on the host OS and is not OS-substrate "
            "observability.",
        ),
        owner_area="Backend & Observability",
    ),
    # -- OBSERVABILITY: OS-substrate scope (Requirement 9.2) ---------------- #
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-boot"),
        name="Boot observability",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        markers=(
            "boot observability",
            "early boot log",
            "boot diagnostics",
            "boot trace",
            "boot console log",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Boot-stage observability owned by the Nebula boot chain with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula-owned boot observability exists.",),
        owner_area="Boot & Kernel",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-kernel"),
        name="Kernel observability",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        markers=(
            "kernel observability",
            "kernel log",
            "kernel trace",
            "kernel metrics",
            "kernel diagnostics",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Kernel observability (logging, tracing, metrics) owned by a Nebula "
            "kernel with direct implementation evidence.",
        ),
        non_claims=("No Nebula kernel exists, so no kernel observability exists.",),
        owner_area="Boot & Kernel",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-driver"),
        name="Driver observability",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        markers=(
            "driver observability",
            "driver log",
            "driver trace",
            "driver diagnostics",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Driver observability owned by a Nebula driver model with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula driver model exists, so no driver observability exists.",),
        owner_area="Drivers & Hardware",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-userspace"),
        name="Userspace observability",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "userspace observability",
            "process observability",
            "userspace tracing",
            "userspace metrics",
            "userspace logging",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Userspace observability owned by a Nebula isolated-userspace platform "
            "with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned isolated userspace exists, so no userspace "
            "observability exists.",
        ),
        owner_area="Userspace & Isolation",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-observability-kernel-user-correlation"),
        name="Kernel/user log and event correlation",
        group=OperationsGroup.OBSERVABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        markers=(
            "kernel/user correlation",
            "kernel-user correlation",
            "kernel user correlation",
            "log correlation",
            "cross-layer correlation",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.1", "9.2"),
        acceptance_evidence=(
            "Correlated kernel and userspace observability across an operable "
            "Universe OS with direct implementation evidence.",
        ),
        non_claims=(
            "No kernel or userspace layers exist, so no kernel/user observability "
            "correlation exists.",
        ),
        owner_area="Operations",
    ),
    # -- SECURITY: compiler / hosted scope (Requirement 9.3) ---------------- #
    OperationsChecklistItem(
        capability_id=StableId("capability-security-compiler-supply-chain"),
        name="Compiler supply chain and artifact integrity",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "supply chain",
            "supply-chain",
            "artifact integrity",
            "sbom",
            "provenance",
            "attestation",
        ),
        authoritative_paths=(".github/workflows/release.yml", "tests/README.md"),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "Compiler supply-chain controls and artifact-integrity evidence (SBOM, "
            "provenance, attestation) with direct current implementation evidence.",
        ),
        non_claims=(
            "Compiler supply-chain evidence is scoped to hosted tooling releases and "
            "cannot raise OS-substrate security maturity.",
        ),
        owner_area="Release Engineering",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-package-trust"),
        name="Package trust and signing",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "package trust",
            "package signing",
            "signed package",
            "signature verification",
            "package provenance",
        ),
        authoritative_paths=("docs/official_package_tiering.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "Package trust and signature-verification controls with direct current "
            "implementation evidence.",
        ),
        non_claims=(
            "Package trust controls are scoped to the hosted package ecosystem.",
        ),
        owner_area="Ecosystem",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-unsafe-audit"),
        name="Unsafe-usage audit",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "unsafe audit",
            "unsafe review",
            "unsafe usage audit",
            "audit of unsafe",
            "unsafe accounting",
        ),
        authoritative_paths=("spec/safety_contract.md",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "An unsafe-usage audit that enumerates and reviews unsafe boundaries "
            "with direct current implementation evidence.",
        ),
        non_claims=(
            "No systematic unsafe-usage audit is demonstrated for the toolchain.",
        ),
        owner_area="Language & Safety",
    ),
    # -- SECURITY: OS-substrate scope (Requirement 9.3) --------------------- #
    OperationsChecklistItem(
        capability_id=StableId("capability-security-capability-enforcement"),
        name="Capability-based security enforcement",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        markers=(
            "capability security",
            "capability enforcement",
            "capability-based security",
            "capability model",
            "capability boundary",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "A Nebula-owned capability-based security enforcement mechanism with "
            "direct implementation evidence.",
        ),
        non_claims=("No Nebula-owned capability enforcement mechanism exists.",),
        owner_area="Boot & Kernel",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-isolation-privilege"),
        name="Process isolation and privilege separation",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "process isolation",
            "privilege separation",
            "privilege boundary",
            "least privilege",
            "privilege isolation",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "Nebula-owned process isolation and privilege separation enforced by an "
            "OS boundary with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned process isolation or privilege-separation boundary "
            "exists.",
        ),
        owner_area="Userspace & Isolation",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-secure-boot"),
        name="Secure / verified boot",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        markers=(
            "secure boot",
            "verified boot",
            "measured boot",
            "boot attestation",
            "trusted boot",
        ),
        authoritative_paths=("docs/universeos/qemu_boot_hello.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "A secure/verified boot chain owned by Nebula with direct implementation "
            "evidence.",
        ),
        non_claims=(
            "No linked, bootable image exists, so no secure/verified boot exists.",
        ),
        owner_area="Boot & Kernel",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-secret-crypto-lifecycle"),
        name="Secret and cryptographic key lifecycle",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        markers=(
            "secret lifecycle",
            "secret management",
            "key management",
            "crypto lifecycle",
            "cryptographic lifecycle",
            "key rotation",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "A Nebula-owned secret and cryptographic key lifecycle (provisioning, "
            "rotation, revocation) with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned OS secret or cryptographic key lifecycle exists.",
        ),
        owner_area="Security Operations",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-update-rollback"),
        name="Secure update and rollback protection",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        markers=(
            "secure update",
            "update rollback",
            "signed update",
            "rollback protection",
            "anti-rollback",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "A Nebula-owned secure update mechanism with rollback protection and "
            "direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned OS secure-update or rollback-protection mechanism "
            "exists.",
        ),
        owner_area="Security Operations",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-security-incident-response"),
        name="Incident response",
        group=OperationsGroup.SECURITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        markers=(
            "incident response",
            "incident handling",
            "security response",
            "vulnerability response",
            "security incident",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("9.3",),
        acceptance_evidence=(
            "An operable incident-response capability for the OS platform with "
            "direct implementation/process evidence.",
        ),
        non_claims=(
            "No OS-platform incident-response capability is demonstrated for a "
            "Nebula-owned substrate.",
        ),
        owner_area="Security Operations",
    ),
    # -- RELIABILITY (Requirement 9.4) -------------------------------------- #
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-bounded-execution"),
        name="Bounded execution",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        markers=(
            "bounded execution",
            "bounded time",
            "execution bound",
            "resource bound",
            "worst-case execution",
        ),
        authoritative_paths=("docs/universeos/no_std_runtime.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "A bounded-execution guarantee for the freestanding substrate with "
            "direct implementation evidence.",
        ),
        non_claims=("No bounded-execution guarantee exists for a freestanding substrate.",),
        owner_area="Freestanding Runtime",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-containment"),
        name="Fault containment",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "fault containment",
            "failure containment",
            "containment",
            "blast radius",
            "fault isolation",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "A fault-containment mechanism enforced by an OS isolation boundary with "
            "direct implementation evidence.",
        ),
        non_claims=("No OS-enforced fault-containment mechanism exists.",),
        owner_area="Userspace & Isolation",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-transactionality"),
        name="Transactionality",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "transactionality",
            "transactional",
            "atomic transaction",
            "transaction rollback",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "Transactional state updates for OS storage/state with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula-owned transactional state mechanism exists.",),
        owner_area="System Services",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-crash-consistency"),
        name="Crash consistency",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "crash consistency",
            "crash-consistent",
            "crash recovery",
            "journaling",
            "write-ahead log",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "A crash-consistency guarantee for OS storage/state with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula-owned crash-consistency guarantee exists.",),
        owner_area="System Services",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-power-loss"),
        name="Power-loss durability",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        markers=(
            "power-loss",
            "power loss",
            "power failure",
            "durability across power",
            "sudden power",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "A power-loss durability guarantee for OS storage with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula-owned power-loss durability guarantee exists.",),
        owner_area="System Services",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-deterministic-rebuild"),
        name="Deterministic / reproducible rebuild",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.COMPILER_HOSTED,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=(
            "deterministic rebuild",
            "deterministic build",
            "reproducible build",
            "reproducible rebuild",
            "bit-for-bit",
        ),
        authoritative_paths=("tests/README.md", ".github/workflows/release.yml"),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("9.4", "9.5"),
        acceptance_evidence=(
            "A deterministic/reproducible rebuild with direct current implementation "
            "evidence, and every trust assumption (trusted tools, cooperative "
            "descendants, caller-controlled directories, host security services) "
            "recorded as a limitation.",
        ),
        non_claims=(
            "Reproducible-build evidence is scoped to the hosted toolchain and "
            "depends on its recorded trust assumptions.",
        ),
        owner_area="Release Engineering",
    ),
    OperationsChecklistItem(
        capability_id=StableId("capability-reliability-recovery"),
        name="Recovery behavior",
        group=OperationsGroup.RELIABILITY,
        scope=AssessmentScope.OS_SUBSTRATE,
        target_level=TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        markers=(
            "recovery behavior",
            "system recovery",
            "automated recovery",
            "self-healing",
            "recovery orchestration",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("9.4",),
        acceptance_evidence=(
            "Operable recovery behavior for the OS platform with direct "
            "implementation evidence.",
        ),
        non_claims=("No Nebula-owned OS recovery behavior exists.",),
        owner_area="Operations",
    ),
)


def _contains(text: str, markers: Iterable[str]) -> bool:
    return any(marker in text for marker in markers)


def _record_text(record: EvidenceRecord) -> str:
    return f"{record.claim_key}\n{record.claim}".lower()


def _strongest_status(records: Iterable[EvidenceRecord]) -> EvidenceStatus:
    present = {record.status for record in records}
    for status in _STATUS_STRENGTH:
        if status in present:
            return status
    return EvidenceStatus.UNKNOWN


def _has_current_implementation(record: EvidenceRecord) -> bool:
    return (
        record.status in _IMPLEMENTED_STATUSES
        and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
    )


class ObservabilitySecurityReliabilityEvaluator:
    """Evaluate Requirement 9.1-9.5 observability, security, and reliability gaps."""

    def __init__(self) -> None:
        self._trust_auditor = TrustAssumptionAuditor()

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> OperationsEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")

        records = bundle.records
        # The Claim Guard tells us which claims may be asserted in the present
        # tense and which cannot raise OS-substrate maturity (Property 9).
        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted
            for claim in guarded.claims
        }
        substrate_blocked = {
            str(claim.evidence_id): claim.substrate_promotion_blocked
            for claim in guarded.claims
        }

        # Requirement 9.5: reuse the Task 4.4 auditor to find every record that
        # implies a trust assumption, so we can surface the disclosed assumption
        # onto the affected gaps as a limitation.
        trust_report = self._trust_auditor.audit(records)
        trust_by_record: dict[str, tuple[AssumptionCategory, ...]] = {}
        for audit in trust_report.audits:
            trust = tuple(
                category
                for category in audit.detected
                if category in _TRUST_ASSUMPTION_CATEGORIES
            )
            if trust:
                trust_by_record[str(audit.evidence_id)] = trust

        os_substrate_strength = self._os_substrate_strength(
            records, present_permitted, substrate_blocked
        )

        drafts: list[OperationsDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in OPERATIONS_CHECKLIST:
            matched = tuple(
                record
                for record in records
                if _contains(_record_text(record), item.markers)
            )
            draft, gap = self._assess_item(
                item,
                matched,
                present_permitted,
                substrate_blocked,
                trust_by_record,
            )
            drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        return OperationsEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
            os_substrate_strength=os_substrate_strength,
        )

    # -- headline classification ----------------------------------------- #

    def _os_substrate_strength(
        self,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
        substrate_blocked: Mapping[str, bool],
    ) -> OperationsScopeStrength:
        """Classify the strongest OS-substrate operations evidence once.

        Hosted / scoped-release operations evidence (compiler diagnostics, hosted
        service observability, compiler/tooling GA, backend SDK GA) can never be an
        OS-substrate implementation (Requirement 9.2 / Property 9).
        """

        os_markers = tuple(
            marker
            for item in OPERATIONS_CHECKLIST
            if item.scope is AssessmentScope.OS_SUBSTRATE
            for marker in item.markers
        )
        has_os_substrate = False
        has_hosted = False
        for record in records:
            text = _record_text(record)
            if not _contains(text, os_markers):
                continue
            if self._is_os_substrate_impl(record, present_permitted, substrate_blocked):
                has_os_substrate = True
            elif record.status in _IMPLEMENTED_STATUSES and substrate_blocked.get(
                str(record.id), True
            ):
                # Hosted / scoped-release evidence touching an OS-substrate topic;
                # it cannot satisfy the OS capability (Requirement 9.2 / Property 9).
                has_hosted = True
        if has_os_substrate:
            return OperationsScopeStrength.OS_SUBSTRATE
        if has_hosted:
            return OperationsScopeStrength.COMPILER_HOSTED_ONLY
        return OperationsScopeStrength.ABSENT

    @staticmethod
    def _is_os_substrate_impl(
        record: EvidenceRecord,
        present_permitted: Mapping[str, bool],
        substrate_blocked: Mapping[str, bool],
    ) -> bool:
        """A record is a genuine OS-substrate implementation only when it is a
        direct current implementation, present-tense permitted, and the guard does
        not flag it as substrate-promotion-blocked."""

        record_id = str(record.id)
        return (
            _has_current_implementation(record)
            and present_permitted.get(record_id, False)
            and not substrate_blocked.get(record_id, True)
        )

    # -- per-capability assessment --------------------------------------- #

    def _assess_item(
        self,
        item: OperationsChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
        substrate_blocked: Mapping[str, bool],
        trust_by_record: Mapping[str, tuple[AssumptionCategory, ...]],
    ) -> tuple[OperationsDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)

        if item.scope is AssessmentScope.COMPILER_HOSTED:
            # Hosted tooling is satisfied by any direct current implementation
            # (present-tense permitted); hosted/scoped status is acceptable here.
            satisfied = any(
                _has_current_implementation(record)
                and present_permitted.get(str(record.id), False)
                for record in matched
            )
        else:
            # OS-substrate capabilities require a non-hosted direct implementation;
            # hosted / scoped-release evidence can never satisfy them.
            satisfied = any(
                self._is_os_substrate_impl(record, present_permitted, substrate_blocked)
                for record in matched
            )

        maturity = MaturityScore.NARROW_EXPERIMENT if satisfied else MaturityScore.ABSENT

        limitations = self._limitations(item, matched, trust_by_record)

        gap: GapEntry | None = None
        gap_ids: tuple[ReferenceId, ...] = ()
        if not satisfied:
            gap = self._build_gap(
                item, matched, observed_status, limitations, substrate_blocked
            )
            gap_ids = (reference(gap.id),)

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} {item.group.value} "
                f"capability ({item.scope.value} scope), assessed by the "
                "debugging/observability/security/reliability evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(_PARENT_BY_TARGET[item.target_level]),
            evidence_ids=supporting_ids,
            gap_ids=gap_ids,
            dependency_gate_ids=(reference(gate_id(item.group)),),
        )

        draft = OperationsDomainDraft(
            domain=domain,
            group=item.group,
            scope=item.scope,
            observed_status=observed_status,
            maturity_score=maturity,
            supporting_evidence_ids=supporting_ids,
            limitations=limitations,
            satisfied=satisfied,
            gap_id=reference(gap.id) if gap is not None else None,
        )
        return draft, gap

    def _build_gap(
        self,
        item: OperationsChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
        limitations: tuple[str, ...],
        substrate_blocked: Mapping[str, bool],
    ) -> GapEntry:
        primary = item.unsatisfied_category
        secondary: list[GapCategory] = []
        # For an OS-substrate capability, hosted/scoped implemented evidence that
        # touches the topic is a claim-risk / semantic-stability concern: it must
        # not be read as the OS-substrate capability (Requirement 9.2 / Property 9).
        hosted_implemented = item.scope is AssessmentScope.OS_SUBSTRATE and any(
            _has_current_implementation(record)
            and substrate_blocked.get(str(record.id), True)
            for record in matched
        )
        if primary is not GapCategory.VERIFICATION and hosted_implemented:
            secondary.append(GapCategory.VERIFICATION)

        observed_fact, recommendation = self._narrative(item, matched, hosted_implemented)
        acceptance = tuple(item.acceptance_evidence)
        if limitations:
            acceptance = acceptance + (
                "Disclosed limitations must remain recorded: " + "; ".join(limitations),
            )

        return GapEntry(
            id=stable_id("gap", "operations", str(item.capability_id)),
            title=f"{item.name} gap",
            primary_category=primary,
            secondary_categories=tuple(secondary),
            domain_ids=(reference(item.capability_id),),
            current_status=observed_status,
            target_level=item.target_level,
            severity=self._severity(item),
            dependencies=(reference(gate_id(item.group)),),
            acceptance_evidence=acceptance,
            recommended_owner_area=item.owner_area,
            dependency_criticality=self._dependency_criticality(item),
            safety_impact=self._safety_impact(item),
            claim_risk=1 if hosted_implemented else 0,
            target_unblock_value=self._dependency_criticality(item),
            observed_fact=observed_fact,
            recommendation=recommendation,
        )

    # -- narrative and helpers ------------------------------------------- #

    @staticmethod
    def _narrative(
        item: OperationsChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        hosted_implemented: bool,
    ) -> tuple[str, str]:
        source = item.authoritative_paths[0]
        if item.scope is AssessmentScope.COMPILER_HOSTED:
            observed = (
                f"{item.name}: no direct current implementation evidence for this "
                f"hosted tooling capability (authoritative source {source})."
            )
            recommendation = (
                f"Provide direct current implementation evidence for {item.name} in "
                "the hosted toolchain/service scope."
            )
            return observed, recommendation
        if hosted_implemented:
            boundary_note = (
                "only compiler / hosted-service operations evidence exists, which "
                "cannot satisfy this OS-substrate capability"
            )
        else:
            boundary_note = "no OS-substrate implementation evidence exists"
        observed = f"{item.name}: {boundary_note} (authoritative source {source})."
        recommendation = (
            f"Implement {item.name} in a Nebula-owned OS substrate; compiler or "
            "hosted-service operations evidence cannot raise this capability's maturity."
        )
        return observed, recommendation

    def _limitations(
        self,
        item: OperationsChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        trust_by_record: Mapping[str, tuple[AssumptionCategory, ...]],
    ) -> tuple[str, ...]:
        limitations: set[str] = set(item.non_claims)
        # Requirement 9.5: surface every disclosed trust assumption on the matched
        # records as a limitation on this capability's gap.
        for record in matched:
            if str(record.id) not in trust_by_record:
                continue
            for text in (*record.limitations, *record.trust_assumptions):
                if text.strip():
                    limitations.add(text)
        return tuple(sorted(limitations))

    @staticmethod
    def _severity(item: OperationsChecklistItem) -> Severity:
        if item.scope is AssessmentScope.COMPILER_HOSTED:
            return Severity.MEDIUM
        if item.group is OperationsGroup.SECURITY:
            return Severity.HIGH
        return Severity.MEDIUM

    @staticmethod
    def _dependency_criticality(item: OperationsChecklistItem) -> int:
        if item.scope is AssessmentScope.COMPILER_HOSTED:
            return 1
        if item.group is OperationsGroup.SECURITY:
            return 3
        return 2

    @staticmethod
    def _safety_impact(item: OperationsChecklistItem) -> int:
        if item.group is OperationsGroup.SECURITY:
            return 3
        if item.group is OperationsGroup.RELIABILITY:
            return 2
        return 1


def evaluate_observability_security_reliability(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> OperationsEvaluation:
    """Convenience API for the debugging/observability/security/reliability evaluator."""

    return ObservabilitySecurityReliabilityEvaluator().evaluate(bundle, guarded)
