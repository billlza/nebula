"""Kernel, hardware/firmware, and driver evaluator (Task 7.1).

This declarative evaluator covers Requirement 10.1-10.3, 10.6, and 12.6: the
hardware/firmware substrate, the kernel resource-management subsystems, and the
driver/hardware-abstraction subsystems. Every listed subsystem is assessed as an
**independent** :class:`~tools.universe_os_gap_analysis.models.CapabilityDomain`
with its own :class:`~tools.universe_os_gap_analysis.models.GapEntry`, so a missing
foundation always remains individually visible and can never be hidden behind an
adjacent capability (Requirement 12.6).

The controlling rule is Requirement 10.6 / design "Kernel" and "Drivers &
Hardware" methods: *no direct implementation evidence means Maturity_Score 0*.
The repository's ``docs/universeos/kernel_boundary.md`` is a boundary document --
it documents future responsibilities and explicit non-claims, not present-tense
implementations -- so it never counts as implementation evidence. A future QEMU
serial hello (or any boot/prerequisite gate) likewise cannot lift any kernel,
hardware, or driver subsystem above 0; only direct implementation evidence for
*that* subsystem can.

This evaluator is intentionally self-contained (mirroring the sibling Task 5.2
``memory_concurrency_safety`` evaluator). It builds on the Task 4.1
:class:`~tools.universe_os_gap_analysis.evidence.EvidenceBundle` and the Task 4.3
:class:`~tools.universe_os_gap_analysis.claim_guard.GuardedEvidence` layer, and it
never mutates evidence, upgrades a status, or edits sibling evaluator files.

Requirement coverage:

* **10.1 hardware/firmware.** target discovery, firmware, boot protocol, early
  console, timers, interrupts, traps, CPU state, MMU, page tables, physical
  memory, virtual memory, DMA, IOMMU, and power management.
* **10.2 drivers & hardware.** device discovery, bus abstractions, driver
  lifecycle, driver isolation, interrupt routing, DMA safety, storage devices,
  network devices, input, display, audio, and hardware qualification.
* **10.3 kernel.** kernel entry, panic, synchronization, scheduler, context
  switching, syscall dispatch, capability enforcement, IPC, process model,
  thread model, address-space isolation, and resource accounting.
* **10.6 / 12.6.** each subsystem is an independent domain/gap; any subsystem
  with no direct implementation evidence yields Maturity_Score 0 (absent).
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
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    MaturityScore,
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS`` #
# --------------------------------------------------------------------------- #
# Every domain here rolls up into the T3 boot-and-kernel-foundation umbrella so a
# later reconciliation step can attach these fine-grained domains to the target
# model without this evaluator having to own that catalog.
_PARENT_T3 = StableId("capability-t3-kernel-foundation")

# The three semantic aspects this evaluator partitions its checklist into.
ASPECT_HARDWARE = "hardware-firmware"
ASPECT_KERNEL = "kernel"
ASPECT_DRIVER = "driver-hardware-abstraction"

# Authoritative boundary/readiness documents. These are non-claim / boundary
# sources: they describe *future* responsibilities and record explicit
# non-support, so they never satisfy a present-tense implementation claim.
_DOC_KERNEL_BOUNDARY = "docs/universeos/kernel_boundary.md"
_DOC_READINESS = "docs/universeos/readiness_assessment.md"


class CapabilityAspect(Enum):
    """Which of the three Requirement-10 aspects a subsystem belongs to."""

    HARDWARE = ASPECT_HARDWARE
    KERNEL = ASPECT_KERNEL
    DRIVER = ASPECT_DRIVER


@dataclass(frozen=True, slots=True)
class SubsystemChecklistItem:
    """One declarative, independently-assessed OS-substrate subsystem."""

    capability_id: StableId
    name: str
    aspect: CapabilityAspect
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str
    dependency_criticality: int = 0
    safety_impact: int = 0
    claim_risk: int = 0
    target_unblock_value: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.aspect, CapabilityAspect):
            raise TypeError("aspect must be a CapabilityAspect")
        for name in ("markers", "authoritative_paths", "requirement_refs", "acceptance_evidence"):
            value = tuple(getattr(self, name))
            if not value:
                raise ValueError(f"{name} must not be empty")
            object.__setattr__(self, name, value)
        object.__setattr__(self, "non_claims", tuple(self.non_claims))
        if not self.owner_area.strip():
            raise ValueError("owner_area must not be empty")
        for name in (
            "dependency_criticality",
            "safety_impact",
            "claim_risk",
            "target_unblock_value",
        ):
            value = getattr(self, name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")


@dataclass(frozen=True, slots=True, kw_only=True)
class SubsystemDomainDraft:
    """A per-subsystem draft: domain, direct-implementation flag, and score."""

    domain: CapabilityDomain
    aspect: CapabilityAspect
    has_direct_implementation: bool
    raw_maturity_score: MaturityScore
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    gap_id: ReferenceId

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        if not isinstance(self.aspect, CapabilityAspect):
            raise TypeError("aspect must be a CapabilityAspect")
        if not isinstance(self.has_direct_implementation, bool):
            raise TypeError("has_direct_implementation must be a bool")
        if not isinstance(self.raw_maturity_score, MaturityScore):
            raise TypeError("raw_maturity_score must be a MaturityScore")
        if not isinstance(self.observed_status, EvidenceStatus):
            raise TypeError("observed_status must be an EvidenceStatus")
        # Requirement 10.6: no direct implementation evidence means score 0.
        if not self.has_direct_implementation and self.raw_maturity_score != MaturityScore.ABSENT:
            raise ValueError("a subsystem without direct implementation must score 0")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(value) for value in self.supporting_evidence_ids}, key=str)),
        )
        object.__setattr__(self, "limitations", tuple(self.limitations))
        object.__setattr__(self, "gap_id", reference(self.gap_id))


@dataclass(frozen=True, slots=True)
class KernelHardwareDriverEvaluation:
    """The evaluator output: independent subsystem drafts plus their gaps."""

    domain_drafts: tuple[SubsystemDomainDraft, ...]
    gaps: tuple[GapEntry, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id))))

    def draft_for(self, capability_id: str) -> SubsystemDomainDraft | None:
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

    def drafts_for_aspect(self, aspect: CapabilityAspect) -> tuple[SubsystemDomainDraft, ...]:
        return tuple(draft for draft in self.domain_drafts if draft.aspect is aspect)


# --------------------------------------------------------------------------- #
# Requirement 10.1: hardware / firmware substrate                             #
# --------------------------------------------------------------------------- #

_HARDWARE_ITEMS: tuple[SubsystemChecklistItem, ...] = (
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-target-firmware"),
        name="Target discovery and firmware",
        aspect=CapabilityAspect.HARDWARE,
        markers=("target discovery", "firmware", "uefi", "acpi", "device tree", "platform init"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence for target discovery and firmware "
            "handoff parsing (boot memory map, platform tables) in a freestanding "
            "artifact, not a boundary document.",
        ),
        non_claims=(
            "No target-discovery or firmware-handoff implementation exists; the "
            "boundary document only describes future responsibilities.",
        ),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-boot-protocol"),
        name="Boot protocol",
        aspect=CapabilityAspect.HARDWARE,
        markers=("boot protocol", "limine", "multiboot", "boot handoff", "entry protocol"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence that a pinned boot protocol/ABI is "
            "honored by a linked, bootable kernel artifact.",
        ),
        non_claims=(
            "A boot protocol/ABI candidate is documented but no implementation "
            "honors it; UOS-BOOT-001 remains planned.",
        ),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=1,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-early-console"),
        name="Early console",
        aspect=CapabilityAspect.HARDWARE,
        markers=("early console", "serial console", "uart", "debug console", "serial output"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of an early console (e.g. serial/UART) "
            "driven by kernel code, proven by execution.",
        ),
        non_claims=("No early console implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-timers"),
        name="Timers",
        aspect=CapabilityAspect.HARDWARE,
        markers=("timer", "clock source", "tick", "hpet", "apic timer", "monotonic clock"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a hardware timer/clock-source driver "
            "usable for scheduling and timekeeping.",
        ),
        non_claims=("No timer or clock-source implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-interrupts"),
        name="Interrupts",
        aspect=CapabilityAspect.HARDWARE,
        markers=("interrupt controller", "interrupt", "idt", "gic", "apic", "irq"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of interrupt-controller programming and "
            "an interrupt entry/dispatch path.",
        ),
        non_claims=("No interrupt model or interrupt-controller implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-traps"),
        name="Traps and exceptions",
        aspect=CapabilityAspect.HARDWARE,
        markers=("trap", "exception vector", "fault handler", "cpu exception", "synchronous exception"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a trap/exception vector table with "
            "fault handlers and defined return contracts.",
        ),
        non_claims=("No trap/exception handling implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-cpu-state"),
        name="CPU state management",
        aspect=CapabilityAspect.HARDWARE,
        markers=("cpu state", "register state", "privilege level", "cpu mode", "control register"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of CPU state and privilege-level "
            "management (control registers, mode transitions).",
        ),
        non_claims=("No CPU-state or privilege-level management implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-mmu"),
        name="MMU",
        aspect=CapabilityAspect.HARDWARE,
        markers=("mmu", "memory management unit", "address translation", "tlb"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of MMU configuration and address "
            "translation control.",
        ),
        non_claims=("No MMU implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-page-tables"),
        name="Page tables",
        aspect=CapabilityAspect.HARDWARE,
        markers=("page table", "paging", "page mapping", "page directory", "pte"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of page-table construction and mapping "
            "management.",
        ),
        non_claims=("No page-table implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-physical-memory"),
        name="Physical memory management",
        aspect=CapabilityAspect.HARDWARE,
        markers=("physical memory", "frame allocator", "physical frame", "memory map", "phys mem"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a physical frame allocator over a "
            "parsed boot memory map.",
        ),
        non_claims=("No physical memory manager or frame allocator implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-virtual-memory"),
        name="Virtual memory management",
        aspect=CapabilityAspect.HARDWARE,
        markers=("virtual memory", "virtual address space", "vm mapping", "address space", "virt mem"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a virtual memory manager governing "
            "address-space mappings.",
        ),
        non_claims=("No virtual memory manager implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-dma"),
        name="DMA",
        aspect=CapabilityAspect.HARDWARE,
        markers=("dma", "direct memory access", "dma buffer", "dma engine"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of DMA programming and DMA-safe memory "
            "policy.",
        ),
        non_claims=("No DMA implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-iommu"),
        name="IOMMU",
        aspect=CapabilityAspect.HARDWARE,
        markers=("iommu", "io mmu", "device address translation", "smmu"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of IOMMU configuration for device "
            "address isolation.",
        ),
        non_claims=("No IOMMU implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-hw-power-management"),
        name="Power management",
        aspect=CapabilityAspect.HARDWARE,
        markers=("power management", "power state", "cpu idle", "sleep state", "acpi power"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.1", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of power-state and CPU idle management.",
        ),
        non_claims=("No power-management implementation exists.",),
        owner_area="Kernel & Hardware",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
)


# --------------------------------------------------------------------------- #
# Requirement 10.3: kernel resource management                                #
# --------------------------------------------------------------------------- #

_KERNEL_ITEMS: tuple[SubsystemChecklistItem, ...] = (
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-entry"),
        name="Kernel entry",
        aspect=CapabilityAspect.KERNEL,
        markers=("kernel entry", "kernel main", "boot handoff", "kernel start", "kmain"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY, _DOC_READINESS),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a kernel entry path that receives "
            "control and establishes early runtime invariants.",
        ),
        non_claims=("No kernel entry path exists.",),
        owner_area="Kernel",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-panic"),
        name="Kernel panic path",
        aspect=CapabilityAspect.KERNEL,
        markers=("kernel panic", "panic path", "abort", "trap handler panic", "halt on panic"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a freestanding panic/abort path "
            "without hosted unwind or C++ runtime dependencies.",
        ),
        non_claims=("No freestanding kernel panic path exists.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-synchronization"),
        name="Kernel synchronization",
        aspect=CapabilityAspect.KERNEL,
        markers=("kernel synchronization", "spinlock", "kernel lock", "kernel mutex", "critical section"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of kernel synchronization primitives "
            "(spinlocks/locks) safe for interrupt and multiprocessor contexts.",
        ),
        non_claims=("No kernel synchronization primitives exist.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-scheduler"),
        name="Scheduler",
        aspect=CapabilityAspect.KERNEL,
        markers=("kernel scheduler", "scheduler", "run queue", "preemptive scheduling", "task scheduling"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a kernel scheduler selecting "
            "execution contexts, backed by timer and context-switch support.",
        ),
        non_claims=(
            "No kernel scheduler exists; hosted service-manager examples are not "
            "scheduler evidence.",
        ),
        owner_area="Kernel",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-context-switch"),
        name="Context switching",
        aspect=CapabilityAspect.KERNEL,
        markers=("context switch", "context frame", "register save", "task switch", "save/restore"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of context-frame save/restore and "
            "context switching between execution contexts.",
        ),
        non_claims=("No context-switching implementation exists.",),
        owner_area="Kernel",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-syscall-dispatch"),
        name="Syscall dispatch",
        aspect=CapabilityAspect.KERNEL,
        markers=("syscall dispatch", "syscall", "system call", "syscall number", "syscall entry"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a syscall dispatch path validating "
            "syscall numbers, ABI version, and argument layout.",
        ),
        non_claims=("No syscall ABI or dispatch implementation exists.",),
        owner_area="Kernel",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-capability-enforcement"),
        name="Capability enforcement",
        aspect=CapabilityAspect.KERNEL,
        markers=("capability enforcement", "capability handle", "capability check", "ambient authority", "privileged operation"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of capability handles mediating access "
            "to kernel objects and privileged operations.",
        ),
        non_claims=("No kernel capability model or enforcement exists.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-ipc"),
        name="IPC",
        aspect=CapabilityAspect.KERNEL,
        markers=("ipc", "inter-process communication", "message passing", "kernel channel", "endpoint"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a kernel IPC mechanism between "
            "isolated contexts.",
        ),
        non_claims=("No kernel IPC implementation exists.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-process-model"),
        name="Process model",
        aspect=CapabilityAspect.KERNEL,
        markers=("process model", "process table", "kernel process", "process creation", "process control block"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a kernel process model with process "
            "lifecycle management.",
        ),
        non_claims=("No kernel process model exists.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-thread-model"),
        name="Thread model",
        aspect=CapabilityAspect.KERNEL,
        markers=("thread model", "kernel thread", "thread control block", "thread creation", "kthread"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a kernel thread model with thread "
            "lifecycle management.",
        ),
        non_claims=("No kernel thread model exists.",),
        owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-address-space-isolation"),
        name="Address-space isolation",
        aspect=CapabilityAspect.KERNEL,
        markers=("address-space isolation", "address space isolation", "process isolation", "memory isolation", "user/kernel separation"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of enforced address-space isolation "
            "between kernel and user contexts.",
        ),
        non_claims=("No address-space isolation implementation exists.",),
        owner_area="Kernel",
        dependency_criticality=3,
        safety_impact=2,
        claim_risk=2,
        target_unblock_value=2,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-kernel-resource-accounting"),
        name="Resource accounting",
        aspect=CapabilityAspect.KERNEL,
        markers=("resource accounting", "resource quota", "memory accounting", "cpu accounting", "usage tracking"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.3", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of kernel resource accounting and "
            "quota enforcement.",
        ),
        non_claims=("No kernel resource accounting implementation exists.",),
        owner_area="Kernel",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
)


# --------------------------------------------------------------------------- #
# Requirement 10.2: drivers and hardware abstraction                          #
# --------------------------------------------------------------------------- #

_DRIVER_ITEMS: tuple[SubsystemChecklistItem, ...] = (
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-device-discovery"),
        name="Device discovery",
        aspect=CapabilityAspect.DRIVER,
        markers=("device discovery", "device enumeration", "device probe", "hardware discovery"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of device discovery/enumeration in a "
            "kernel-owned driver framework.",
        ),
        non_claims=("No device-discovery implementation exists; the gate registry has no driver gate.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-bus-abstraction"),
        name="Bus abstractions",
        aspect=CapabilityAspect.DRIVER,
        markers=("bus abstraction", "pci", "usb bus", "bus driver", "system bus"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a bus abstraction (e.g. PCI/USB) "
            "used by drivers.",
        ),
        non_claims=("No bus-abstraction implementation exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-lifecycle"),
        name="Driver lifecycle",
        aspect=CapabilityAspect.DRIVER,
        markers=("driver lifecycle", "driver registration", "driver load", "driver bind", "driver unload"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a driver lifecycle (registration, "
            "bind, unload) in a kernel-owned framework.",
        ),
        non_claims=("No driver lifecycle implementation exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-isolation"),
        name="Driver isolation",
        aspect=CapabilityAspect.DRIVER,
        markers=("driver isolation", "driver sandbox", "isolated driver", "driver containment", "userspace driver"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of driver isolation/containment with "
            "capability-checked hardware access.",
        ),
        non_claims=("No driver isolation implementation exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-interrupt-routing"),
        name="Interrupt routing",
        aspect=CapabilityAspect.DRIVER,
        markers=("interrupt routing", "irq routing", "interrupt affinity", "irq handler registration", "device interrupt"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of device interrupt routing to driver "
            "handlers.",
        ),
        non_claims=("No interrupt-routing implementation exists; it cannot be inferred from boot evidence.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-dma-safety"),
        name="DMA safety",
        aspect=CapabilityAspect.DRIVER,
        markers=("dma safety", "dma-safe", "dma policy", "safe dma", "dma mapping"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of DMA-safe memory policy enforced for "
            "drivers.",
        ),
        non_claims=("No DMA-safety implementation exists; it cannot be inferred from boot evidence.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=2,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-storage-devices"),
        name="Storage devices",
        aspect=CapabilityAspect.DRIVER,
        markers=("storage device", "block device", "nvme", "ahci", "disk driver", "sata"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a storage/block device driver.",
        ),
        non_claims=("No storage device driver exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-network-devices"),
        name="Network devices",
        aspect=CapabilityAspect.DRIVER,
        markers=("network device", "nic", "network driver", "ethernet driver", "network interface"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a network device driver.",
        ),
        non_claims=("No network device driver exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-input"),
        name="Input devices",
        aspect=CapabilityAspect.DRIVER,
        markers=("input device", "keyboard driver", "mouse driver", "hid", "input driver"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of an input device driver.",
        ),
        non_claims=("No input device driver exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-display"),
        name="Display devices",
        aspect=CapabilityAspect.DRIVER,
        markers=("display device", "framebuffer", "gpu driver", "display driver", "graphics device"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of a display/framebuffer device driver.",
        ),
        non_claims=("No display device driver exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-audio"),
        name="Audio devices",
        aspect=CapabilityAspect.DRIVER,
        markers=("audio device", "sound driver", "audio driver", "codec driver"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation evidence of an audio device driver.",
        ),
        non_claims=("No audio device driver exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
    SubsystemChecklistItem(
        capability_id=StableId("capability-driver-hardware-qualification"),
        name="Hardware qualification",
        aspect=CapabilityAspect.DRIVER,
        markers=("hardware qualification", "hardware certification", "device qualification", "supported hardware", "hardware compatibility list"),
        authoritative_paths=(_DOC_KERNEL_BOUNDARY,),
        requirement_refs=("10.2", "10.6"),
        acceptance_evidence=(
            "Direct implementation and process evidence qualifying supported "
            "hardware for the driver framework.",
        ),
        non_claims=("No hardware qualification process or evidence exists.",),
        owner_area="Drivers & Hardware",
        dependency_criticality=1,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
    ),
)


KERNEL_HARDWARE_DRIVER_CHECKLIST: tuple[SubsystemChecklistItem, ...] = (
    _HARDWARE_ITEMS + _KERNEL_ITEMS + _DRIVER_ITEMS
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

# Evidence kinds that count as direct implementation for a "current" subsystem.
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


class KernelHardwareDriverEvaluator:
    """Evaluate Requirement 10.1-10.3/10.6/12.6 kernel, hardware, and driver gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> KernelHardwareDriverEvaluation:
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

        drafts: list[SubsystemDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in KERNEL_HARDWARE_DRIVER_CHECKLIST:
            matched = tuple(
                record
                for record in records
                if _contains(_record_text(record), item.markers)
            )
            draft, gap = self._assess_item(item, matched, present_permitted)
            drafts.append(draft)
            gaps.append(gap)

        return KernelHardwareDriverEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
        )

    # -- per-subsystem assessment ---------------------------------------- #

    def _assess_item(
        self,
        item: SubsystemChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> tuple[SubsystemDomainDraft, GapEntry]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)

        has_impl = any(
            self._has_direct_implementation(record, present_permitted)
            for record in matched
        )
        # Requirement 10.6: no direct implementation evidence means score 0.
        raw_score = (
            MaturityScore.NARROW_EXPERIMENT if has_impl else MaturityScore.ABSENT
        )

        gap = self._build_gap(item, matched, observed_status, has_impl)

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} OS-substrate "
                f"subsystem ({item.aspect.value}) assessed independently by the "
                "kernel/hardware/driver evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(_PARENT_T3),
            evidence_ids=supporting_ids,
            gap_ids=(reference(gap.id),),
        )

        draft = SubsystemDomainDraft(
            domain=domain,
            aspect=item.aspect,
            has_direct_implementation=has_impl,
            raw_maturity_score=raw_score,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            limitations=tuple(sorted(set(item.non_claims))),
            gap_id=reference(gap.id),
        )
        return draft, gap

    @staticmethod
    def _has_direct_implementation(
        record: EvidenceRecord, present_permitted: Mapping[str, bool]
    ) -> bool:
        return (
            record.status in _IMPLEMENTED_STATUSES
            and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
            and present_permitted.get(str(record.id), True)
        )

    @staticmethod
    def _build_gap(
        item: SubsystemChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
        has_impl: bool,
    ) -> GapEntry:
        domain_ref = reference(item.capability_id)
        matched_note = (
            f"matched evidence {', '.join(sorted(str(reference(r.id)) for r in matched))}"
            if matched
            else "no matching evidence"
        )
        if has_impl:
            observed_fact = (
                f"{item.name} has matching direct implementation evidence "
                f"({matched_note}); its maturity is assessed separately and is not "
                "assumed to be complete."
            )
        else:
            observed_fact = (
                f"{item.name} has no direct implementation evidence ({matched_note}); "
                f"per Requirement 10.6 its Maturity_Score is 0 (absent). "
                + " ".join(item.non_claims)
            )
        return GapEntry(
            id=stable_id("gap", "kernel-hardware-driver", str(item.capability_id)),
            title=f"OS-substrate subsystem: {item.name}",
            primary_category=GapCategory.IMPLEMENTATION,
            secondary_categories=(),
            domain_ids=(domain_ref,),
            current_status=observed_status,
            target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
            severity=KernelHardwareDriverEvaluator._severity(item),
            dependencies=(),
            acceptance_evidence=item.acceptance_evidence,
            recommended_owner_area=item.owner_area,
            dependency_criticality=item.dependency_criticality,
            safety_impact=item.safety_impact,
            claim_risk=item.claim_risk,
            target_unblock_value=item.target_unblock_value,
            observed_fact=observed_fact,
            recommendation=(
                f"Register a positive implementation gate for {item.name} and "
                "produce direct implementation evidence before claiming any "
                "maturity; boot or prerequisite gates do not lift this subsystem."
            ),
        )

    @staticmethod
    def _severity(item: SubsystemChecklistItem) -> Severity:
        if item.safety_impact >= 2:
            return Severity.CRITICAL
        if item.safety_impact == 1 or item.dependency_criticality >= 2:
            return Severity.HIGH
        if item.dependency_criticality == 1:
            return Severity.MEDIUM
        return Severity.LOW


def evaluate_kernel_hardware_driver(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> KernelHardwareDriverEvaluation:
    """Evaluate kernel, hardware/firmware, and driver gaps (Requirements 10.1-10.3, 10.6, 12.6)."""

    return KernelHardwareDriverEvaluator().evaluate(bundle, guarded)
