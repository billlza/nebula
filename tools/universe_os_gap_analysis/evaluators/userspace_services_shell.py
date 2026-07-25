"""Userspace, system-service, and product-shell evaluator (Task 7.2).

This declarative evaluator covers Requirement 10.4-10.6 and 12.6 for the
``T4_Isolated_Userspace_Platform`` layer. It assesses the userspace substrate --
filesystems, storage, networking, service manager, identity, policy, time,
configuration, install/update/rollback/backup/recovery lifecycle -- together with
the user runtime, command environment, application model, GUI/accessibility
shell, sandboxing, application distribution, and developer SDK.

It builds on the Task 4.1 :class:`EvidenceBundle` and the Task 4.3 Claim Guard:
the guard tells the evaluator which claims may be asserted in the present tense
(so host-owned/thin-host adjacency cannot be mistaken for a Nebula-owned OS
capability), while the bundle supplies the full claim text and disclosed
``limitations`` the evaluator surfaces on the gaps it emits.

The governing rule (design "Userspace"; ``docs/universeos/kernel_boundary.md``
"Userspace Responsibilities" and "Scheduler Assumptions") is:

    Host-owned services and thin-host bridges belong only to ``T0`` adjacency.
    Without a **Nebula-owned process/syscall boundary**, every ``T4`` domain
    stays ``Maturity_Score`` 0 regardless of how mature the hosted adjacency is.

Accordingly this evaluator classifies the process/syscall boundary once
(:class:`ProcessBoundaryStrength`) and forces every T4 domain's ``maturity_score``
to ``0`` until that boundary exists with direct current implementation evidence.
The evaluator keeps the ``isolation``, ``userspace``, ``update-recovery``, and
``shell`` workstreams as independent gate groups (:class:`UserspaceGateGroup`) so
the Task 8 Hard-Gate DAG never folds them into one gate (Requirement 12.6).

Requirement coverage:

* **10.4 system services / lifecycle.** filesystems, storage stack, network
  stack, service manager, identity, policy, time, configuration, package
  installation, updates, rollback, backup, and recovery.
* **10.5 userspace and shell.** user runtime, command environment, application
  model, GUI/accessibility shell, sandboxing, application distribution, and
  developer SDK.
* **10.6 no implementation means zero.** Any listed OS-substrate capability
  without direct implementation evidence is ``Maturity_Score`` 0; this evaluator
  additionally caps every T4 domain at 0 while the Nebula-owned process/syscall
  boundary is absent.
* **12.6 independent post-boot gates.** isolation, userspace, update/recovery,
  and product shell remain separate gate groups.

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
    Ownership,
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent.

# A Nebula-owned process/syscall boundary. These name a Nebula-owned OS process
# supervisor / syscall path, not a hosted control plane. To count, a record must
# also be a direct current implementation and must not be host-owned (below).
_NEBULA_BOUNDARY_MARKERS: tuple[str, ...] = (
    "nebula-owned process",
    "nebula owned process",
    "nebula process supervisor",
    "nebula-owned syscall",
    "nebula owned syscall",
    "process isolation",
    "address-space isolation",
    "address space isolation",
    "syscall boundary",
    "capability-checked handle",
    "user process boundary",
    "process supervisor",
)

# Host-owned / thin-host adjacency. Any of these marks the evidence as hosted
# (T0) adjacency; it can never be a Nebula-owned OS boundary (design Property 9,
# Requirement 4.6). Kept broad because hosted control planes describe themselves
# in these terms.
_HOST_OWNED_MARKERS: tuple[str, ...] = (
    "host-owned",
    "host owned",
    "hosted control plane",
    "thin-host",
    "thin host",
    "runs on the host os",
    "runs on the host operating system",
    "host owns",
    "hosted service manager",
    "hosted service-manager",
    "hosted example",
    "host os service",
    "host operating system",
)

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


class ProcessBoundaryStrength(ClosedStrEnum):
    """How strong the Nebula-owned process/syscall boundary evidence is."""

    #: No process/syscall boundary evidence at all.
    ABSENT = "Absent"
    #: Only host-owned services / thin-host bridges (T0 adjacency), no boundary.
    HOST_OWNED_ONLY = "HostOwnedOnly"
    #: A Nebula-owned process/syscall boundary with direct current evidence.
    NEBULA_OWNED = "NebulaOwned"


class UserspaceGateGroup(ClosedStrEnum):
    """The independent post-boot gate a T4 capability belongs to (Requirement 12.6)."""

    ISOLATION = "isolation"
    USERSPACE = "userspace"
    UPDATE_RECOVERY = "update-recovery"
    SHELL = "shell"


class CapabilityKind(Enum):
    """The classification rule a checklist item applies."""

    #: The Nebula-owned process/syscall boundary itself (isolation foundation).
    PROCESS_BOUNDARY = "process_boundary"
    #: A T4 substrate/service/shell capability that depends on the boundary.
    USERSPACE_CAPABILITY = "userspace_capability"


@dataclass(frozen=True, slots=True)
class UserspaceChecklistItem:
    """One declarative capability check for the userspace/service/shell domain."""

    capability_id: StableId
    name: str
    gate_group: UserspaceGateGroup
    kind: CapabilityKind
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
        if not isinstance(self.gate_group, UserspaceGateGroup):
            raise TypeError("gate_group must be a UserspaceGateGroup")
        if not isinstance(self.kind, CapabilityKind):
            raise TypeError("kind must be a CapabilityKind")
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

    @property
    def target_level(self) -> TargetLevel:
        # Every capability in this evaluator is a T4 isolated-userspace capability.
        return TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM


@dataclass(frozen=True, slots=True, kw_only=True)
class UserspaceDomainDraft:
    """A per-capability draft: domain, observed evidence, and maturity classification."""

    domain: CapabilityDomain
    gate_group: UserspaceGateGroup
    observed_status: EvidenceStatus
    maturity_score: MaturityScore
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    nebula_owned: bool
    gap_id: ReferenceId | None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        if not isinstance(self.gate_group, UserspaceGateGroup):
            raise TypeError("gate_group must be a UserspaceGateGroup")
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
        if not isinstance(self.nebula_owned, bool):
            raise TypeError("nebula_owned must be a bool")
        if self.gap_id is not None:
            object.__setattr__(self, "gap_id", reference(self.gap_id))


@dataclass(frozen=True, slots=True)
class UserspaceEvaluation:
    """The evaluator output: domain drafts, gaps, and the boundary classification."""

    domain_drafts: tuple[UserspaceDomainDraft, ...]
    gaps: tuple[GapEntry, ...]
    process_boundary_strength: ProcessBoundaryStrength

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(
            self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id)))
        )
        if not isinstance(self.process_boundary_strength, ProcessBoundaryStrength):
            raise TypeError("process_boundary_strength must be a ProcessBoundaryStrength")

    def draft_for(self, capability_id: str) -> UserspaceDomainDraft | None:
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

    def drafts_for_gate(self, gate_group: UserspaceGateGroup) -> tuple[UserspaceDomainDraft, ...]:
        return tuple(draft for draft in self.domain_drafts if draft.gate_group is gate_group)

    def gate_ids(self) -> Mapping[UserspaceGateGroup, ReferenceId]:
        """Independent post-boot gate identifiers, one per gate group (Requirement 12.6)."""

        return {group: reference(gate_id(group)) for group in UserspaceGateGroup}


def gate_id(gate_group: UserspaceGateGroup) -> StableId:
    """Stable identifier for an independent userspace gate group."""

    if not isinstance(gate_group, UserspaceGateGroup):
        raise TypeError("gate_group must be a UserspaceGateGroup")
    return stable_id("gate", "t4", gate_group.value)


# Parent (T-level umbrella) capability from ``catalog.CAPABILITY_DEFINITIONS``.
_PARENT_T4 = StableId("capability-t4-userspace-platform")


USERSPACE_CHECKLIST: tuple[UserspaceChecklistItem, ...] = (
    # -- ISOLATION gate group ------------------------------------------- #
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-process-isolation"),
        name="Nebula-owned process and address-space isolation",
        gate_group=UserspaceGateGroup.ISOLATION,
        kind=CapabilityKind.PROCESS_BOUNDARY,
        markers=(
            "process isolation",
            "address-space isolation",
            "address space isolation",
            "process supervisor",
            "process model",
            "privilege separation",
            "syscall boundary",
            "nebula-owned process",
            "user process boundary",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned process/syscall boundary with address-space isolation "
            "and direct current implementation evidence, not a hosted control plane.",
        ),
        non_claims=(
            "No Nebula-owned process/syscall boundary exists; hosted service-manager "
            "examples are not an OS process supervisor.",
        ),
        owner_area="Userspace & Isolation",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-sandbox"),
        name="Application sandboxing and confinement",
        gate_group=UserspaceGateGroup.ISOLATION,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=("sandbox", "sandboxing", "confinement", "seccomp", "capability confinement"),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "An implemented sandbox/confinement mechanism enforced by the "
            "Nebula-owned process/capability boundary.",
        ),
        non_claims=("No application sandboxing or confinement mechanism exists.",),
        owner_area="Userspace & Isolation",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-ipc"),
        name="Inter-process communication",
        gate_group=UserspaceGateGroup.ISOLATION,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "ipc",
            "inter-process communication",
            "inter process communication",
            "message passing",
            "channel",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "An implemented IPC mechanism mediated by the Nebula-owned process "
            "and capability boundary.",
        ),
        non_claims=("No Nebula-owned inter-process communication mechanism exists.",),
        owner_area="Userspace & Isolation",
    ),
    # -- USERSPACE gate group (services + runtime + app model) ---------- #
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-filesystem-storage"),
        name="Filesystem and storage stack",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=("filesystem", "file system", "storage stack", "storage service", "block storage"),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned filesystem and storage stack with direct implementation "
            "evidence.",
        ),
        non_claims=("No Nebula-owned filesystem or storage stack exists.",),
        owner_area="System Services",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-network-stack"),
        name="Network stack",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=("network stack", "networking stack", "network service", "tcp/ip stack", "socket layer"),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned network stack with direct implementation evidence.",
        ),
        non_claims=("No Nebula-owned network stack exists.",),
        owner_area="System Services",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-service-manager"),
        name="Service manager and supervision",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "service manager",
            "service supervision",
            "init system",
            "service isolation",
            "daemon supervision",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned service manager that supervises isolated OS services, "
            "not a hosted service-manager example.",
        ),
        non_claims=(
            "Hosted service-manager examples are userspace control-plane examples, "
            "not an OS process supervisor or service isolation manager.",
        ),
        owner_area="System Services",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-identity-policy-time-config"),
        name="Identity, policy, time, and configuration services",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "identity service",
            "identity",
            "policy service",
            "policy engine",
            "time service",
            "timekeeping",
            "configuration service",
            "system configuration",
        ),
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "Nebula-owned identity, policy, time, and configuration services with "
            "direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned identity, policy, time, or configuration services exist.",
        ),
        owner_area="System Services",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-user-runtime-command"),
        name="User runtime and command environment",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "user runtime",
            "userspace runtime",
            "command environment",
            "command-line environment",
            "process runtime",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned user runtime and command environment running above the "
            "process/syscall boundary with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned user runtime or command environment exists; current CLI "
            "tools run on the host OS.",
        ),
        owner_area="Userspace Runtime",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-app-model-sdk"),
        name="Application model and developer SDK",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "application model",
            "app model",
            "developer sdk",
            "application sdk",
            "application api",
            "app api",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned application model and developer SDK targeting the "
            "isolated userspace platform with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned application model or developer SDK for an isolated "
            "userspace platform exists.",
        ),
        owner_area="Userspace Runtime",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-app-distribution"),
        name="Application distribution",
        gate_group=UserspaceGateGroup.USERSPACE,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "application distribution",
            "app distribution",
            "app store",
            "package distribution",
            "software distribution",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned application distribution mechanism for the isolated "
            "userspace platform with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned application distribution mechanism for an isolated "
            "userspace platform exists.",
        ),
        owner_area="Ecosystem",
    ),
    # -- UPDATE-RECOVERY gate group ------------------------------------- #
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-install-update-rollback"),
        name="Package installation, updates, and rollback",
        gate_group=UserspaceGateGroup.UPDATE_RECOVERY,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "package installation",
            "system install",
            "os update",
            "system update",
            "update service",
            "rollback",
            "atomic update",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned install/update/rollback mechanism for the OS platform "
            "with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned OS install, update, or rollback mechanism exists.",
        ),
        owner_area="Update & Recovery",
    ),
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-backup-recovery"),
        name="Backup and recovery",
        gate_group=UserspaceGateGroup.UPDATE_RECOVERY,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "backup",
            "system recovery",
            "disaster recovery",
            "recovery mode",
            "restore",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.4", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned backup and recovery mechanism for the OS platform with "
            "direct implementation evidence.",
        ),
        non_claims=("No Nebula-owned backup or recovery mechanism exists.",),
        owner_area="Update & Recovery",
    ),
    # -- SHELL gate group ----------------------------------------------- #
    UserspaceChecklistItem(
        capability_id=StableId("capability-userspace-shell"),
        name="GUI, accessibility, and command shell foundations",
        gate_group=UserspaceGateGroup.SHELL,
        kind=CapabilityKind.USERSPACE_CAPABILITY,
        markers=(
            "gui shell",
            "graphical shell",
            "platform shell",
            "command shell",
            "accessibility shell",
            "accessibility",
            "desktop shell",
        ),
        authoritative_paths=("docs/universeos/architecture.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("10.5", "10.6", "12.6"),
        acceptance_evidence=(
            "A Nebula-owned GUI/accessibility/command shell for the isolated "
            "userspace platform with direct implementation evidence.",
        ),
        non_claims=(
            "No Nebula-owned GUI, accessibility, or command shell for an isolated "
            "userspace platform exists.",
        ),
        owner_area="Product Shell",
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


def _is_host_owned(record: EvidenceRecord) -> bool:
    if record.scope.ownership is Ownership.HOST_OWNED:
        return True
    return _contains(_record_text(record), _HOST_OWNED_MARKERS)


class UserspaceServicesShellEvaluator:
    """Evaluate Requirement 10.4-10.6 / 12.6 userspace, service, and shell gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> UserspaceEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")

        records = bundle.records
        # Claims the guard forbids from present-tense assertion cannot count as a
        # current implementation, even if their status looks implemented.
        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted
            for claim in guarded.claims
        }

        boundary_strength = self._process_boundary_strength(records, present_permitted)

        drafts: list[UserspaceDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in USERSPACE_CHECKLIST:
            matched = tuple(
                record
                for record in records
                if _contains(_record_text(record), item.markers)
            )
            draft, gap = self._assess_item(
                item, matched, present_permitted, boundary_strength
            )
            drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        return UserspaceEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
            process_boundary_strength=boundary_strength,
        )

    # -- headline classification ----------------------------------------- #

    def _process_boundary_strength(
        self,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> ProcessBoundaryStrength:
        """Classify the Nebula-owned process/syscall boundary once.

        A boundary counts as Nebula-owned only with direct current implementation
        evidence that is *not* host-owned adjacency; otherwise, if any host-owned
        service evidence exists it is ``HostOwnedOnly`` (T0 adjacency), and with no
        boundary evidence at all it is ``Absent``.
        """

        has_nebula_owned = False
        has_host_owned = False
        for record in records:
            if _is_host_owned(record):
                has_host_owned = True
                # Host-owned adjacency can never be a Nebula-owned boundary.
                continue
            text = _record_text(record)
            if (
                _contains(text, _NEBULA_BOUNDARY_MARKERS)
                and record.scope.ownership is not Ownership.HOST_OWNED
                and _has_current_implementation(record)
                and present_permitted.get(str(record.id), False)
            ):
                has_nebula_owned = True
        if has_nebula_owned:
            return ProcessBoundaryStrength.NEBULA_OWNED
        if has_host_owned:
            return ProcessBoundaryStrength.HOST_OWNED_ONLY
        return ProcessBoundaryStrength.ABSENT

    # -- per-capability assessment --------------------------------------- #

    def _assess_item(
        self,
        item: UserspaceChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
        boundary_strength: ProcessBoundaryStrength,
    ) -> tuple[UserspaceDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)

        # A capability has direct current implementation evidence only from
        # non-host-owned, present-tense-permitted implementation records.
        has_direct_impl = any(
            not _is_host_owned(record)
            and _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in matched
        )

        nebula_owned = boundary_strength is ProcessBoundaryStrength.NEBULA_OWNED

        # Requirement 10.6 / design "Userspace": without a Nebula-owned process/
        # syscall boundary, every T4 domain stays Maturity_Score 0. Even with the
        # boundary, a capability with no direct implementation evidence is still 0.
        if nebula_owned and has_direct_impl:
            satisfied = True
            maturity = MaturityScore.NARROW_EXPERIMENT
        else:
            satisfied = False
            maturity = MaturityScore.ABSENT

        # The process-boundary domain is the boundary itself.
        if item.kind is CapabilityKind.PROCESS_BOUNDARY:
            satisfied = nebula_owned
            maturity = MaturityScore.NARROW_EXPERIMENT if nebula_owned else MaturityScore.ABSENT

        limitations = self._limitations(item, matched, boundary_strength)

        gap: GapEntry | None = None
        gap_ids: tuple[ReferenceId, ...] = ()
        if not satisfied:
            gap = self._build_gap(item, matched, observed_status, boundary_strength, limitations)
            gap_ids = (reference(gap.id),)

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} T4 userspace capability "
                f"in the {item.gate_group.value} gate group, assessed by the "
                "userspace/system-service/product-shell evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(_PARENT_T4),
            evidence_ids=supporting_ids,
            gap_ids=gap_ids,
            dependency_gate_ids=(reference(gate_id(item.gate_group)),),
        )

        draft = UserspaceDomainDraft(
            domain=domain,
            gate_group=item.gate_group,
            observed_status=observed_status,
            maturity_score=maturity,
            supporting_evidence_ids=supporting_ids,
            limitations=limitations,
            satisfied=satisfied,
            nebula_owned=nebula_owned,
            gap_id=reference(gap.id) if gap is not None else None,
        )
        return draft, gap

    def _build_gap(
        self,
        item: UserspaceChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
        boundary_strength: ProcessBoundaryStrength,
        limitations: tuple[str, ...],
    ) -> GapEntry:
        primary = item.unsatisfied_category
        secondary: list[GapCategory] = []
        # Host-owned adjacency with an implemented status is a claim-risk /
        # semantic-stability concern: a GA/preview hosted asset must not be read
        # as the Nebula-owned OS capability.
        host_owned_implemented = any(
            _is_host_owned(record) and record.status in _IMPLEMENTED_STATUSES
            for record in matched
        )
        if primary is not GapCategory.VERIFICATION and host_owned_implemented:
            secondary.append(GapCategory.VERIFICATION)

        observed_fact, recommendation = self._narrative(item, boundary_strength, matched)
        acceptance = tuple(item.acceptance_evidence)
        if limitations:
            acceptance = acceptance + (
                "Disclosed limitations must remain recorded: " + "; ".join(limitations),
            )

        return GapEntry(
            id=stable_id("gap", "userspace", str(item.capability_id)),
            title=f"{item.name} gap",
            primary_category=primary,
            secondary_categories=tuple(secondary),
            domain_ids=(reference(item.capability_id),),
            current_status=observed_status,
            target_level=item.target_level,
            severity=self._severity(item),
            dependencies=(reference(gate_id(item.gate_group)),),
            acceptance_evidence=acceptance,
            recommended_owner_area=item.owner_area,
            dependency_criticality=self._dependency_criticality(item),
            safety_impact=self._safety_impact(item),
            claim_risk=1 if host_owned_implemented else 0,
            target_unblock_value=self._dependency_criticality(item),
            observed_fact=observed_fact,
            recommendation=recommendation,
        )

    # -- narrative and helpers ------------------------------------------- #

    @staticmethod
    def _narrative(
        item: UserspaceChecklistItem,
        boundary_strength: ProcessBoundaryStrength,
        matched: tuple[EvidenceRecord, ...],
    ) -> tuple[str, str]:
        source = item.authoritative_paths[0]
        if boundary_strength is ProcessBoundaryStrength.HOST_OWNED_ONLY:
            boundary_note = (
                "only host-owned / thin-host adjacency exists (T0), so this T4 "
                "capability stays Maturity_Score 0"
            )
        elif boundary_strength is ProcessBoundaryStrength.NEBULA_OWNED:
            boundary_note = (
                "a Nebula-owned process/syscall boundary exists but this capability "
                "has no direct implementation evidence, so it stays Maturity_Score 0"
            )
        else:
            boundary_note = (
                "no Nebula-owned process/syscall boundary exists, so this T4 "
                "capability stays Maturity_Score 0"
            )
        observed = (
            f"{item.name}: {boundary_note} (authoritative source {source})."
        )
        if item.kind is CapabilityKind.PROCESS_BOUNDARY:
            recommendation = (
                "Establish a Nebula-owned process/syscall boundary with address-space "
                "isolation before any T4 userspace capability can exceed Maturity_Score 0."
            )
        else:
            recommendation = (
                f"Implement {item.name} above a Nebula-owned process/syscall boundary; "
                "host-owned adjacency cannot raise this T4 capability's maturity."
            )
        return observed, recommendation

    def _limitations(
        self,
        item: UserspaceChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        boundary_strength: ProcessBoundaryStrength,
    ) -> tuple[str, ...]:
        limitations: set[str] = set(item.non_claims)
        if boundary_strength is ProcessBoundaryStrength.HOST_OWNED_ONLY:
            limitations.add(
                "Host-owned / thin-host adjacency belongs to T0 and cannot raise "
                "this T4 capability's maturity above 0."
            )
        # Carry disclosed limitations/trust assumptions from host-owned records so
        # the T0-vs-T4 boundary stays explicit on the emitted gap.
        for record in matched:
            if _is_host_owned(record):
                for limitation in record.limitations:
                    limitations.add(limitation)
        return tuple(sorted(limitations))

    @staticmethod
    def _severity(item: UserspaceChecklistItem) -> Severity:
        if item.kind is CapabilityKind.PROCESS_BOUNDARY:
            return Severity.CRITICAL
        if item.gate_group in {UserspaceGateGroup.ISOLATION, UserspaceGateGroup.UPDATE_RECOVERY}:
            return Severity.HIGH
        return Severity.MEDIUM

    @staticmethod
    def _dependency_criticality(item: UserspaceChecklistItem) -> int:
        # The isolation boundary gates every other userspace capability.
        if item.kind is CapabilityKind.PROCESS_BOUNDARY:
            return 4
        if item.gate_group is UserspaceGateGroup.ISOLATION:
            return 3
        return 2

    @staticmethod
    def _safety_impact(item: UserspaceChecklistItem) -> int:
        if item.gate_group is UserspaceGateGroup.ISOLATION:
            return 3
        if item.gate_group is UserspaceGateGroup.UPDATE_RECOVERY:
            return 2
        return 1


def evaluate_userspace_services_shell(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> UserspaceEvaluation:
    """Convenience API for the userspace/system-service/product-shell evaluator."""

    return UserspaceServicesShellEvaluator().evaluate(bundle, guarded)
