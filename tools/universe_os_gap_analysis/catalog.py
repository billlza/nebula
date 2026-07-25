"""Immutable target, maturity, capability, and initial-conclusion catalogs."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable

from .identifiers import StableId
from .models import MaturityScore, TargetLevel

UNIVERSE_OS_DEFINITION = (
    "The complete Nebula-owned independent system platform: boot chain, "
    "freestanding runtime, system ABI, kernel resource management, hardware "
    "and driver abstractions, isolated userspace, system services, application "
    "lifecycle, security, observability, update, and recovery."
)

NON_ADDITIVE_MATURITY_STATEMENT = (
    "Capability maturity scores are non-additive ordinal values; they must not "
    "be summed, averaged, converted to percentages, or interpreted as schedule estimates."
)


class CapabilityBoundary(str, Enum):
    """The ownership boundary that prevents hosted evidence leaking into OS work."""

    HOSTED_ADJACENCY = "Hosted_Adjacency"
    OS_SUBSTRATE = "OS_Substrate"


@dataclass(frozen=True, slots=True)
class TargetLevelDefinition:
    level: TargetLevel
    order: int
    title: str
    definition: str
    boundary: CapabilityBoundary


@dataclass(frozen=True, slots=True)
class MaturityRubricEntry:
    score: MaturityScore
    meaning: str


@dataclass(frozen=True, slots=True)
class ChecklistItemDefinition:
    id: StableId
    capability_id: StableId
    title: str
    mandatory: bool = True

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        if not self.title.strip():
            raise ValueError("checklist title must not be empty")
        if not isinstance(self.mandatory, bool):
            raise TypeError("mandatory must be a bool")


@dataclass(frozen=True, slots=True)
class CapabilityDefinition:
    id: StableId
    name: str
    target_level: TargetLevel
    boundary: CapabilityBoundary
    mandatory_for_target: bool
    checklist: tuple[ChecklistItemDefinition, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        if not self.name.strip():
            raise ValueError("capability name must not be empty")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.boundary, CapabilityBoundary):
            raise TypeError("boundary must be a CapabilityBoundary")
        if not isinstance(self.mandatory_for_target, bool):
            raise TypeError("mandatory_for_target must be a bool")
        if not self.checklist:
            raise ValueError("each capability must define checklist metadata")
        if any(item.capability_id != self.id for item in self.checklist):
            raise ValueError("checklist item capability references must match their owner")


@dataclass(frozen=True, slots=True)
class InitialConclusion:
    id: StableId
    text: str
    requirement_ref: str
    overturning_evidence: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        for value in (self.text, self.requirement_ref, self.overturning_evidence):
            if not value.strip():
                raise ValueError("initial conclusion fields must not be empty")


@dataclass(frozen=True, slots=True)
class ConclusionEvidence:
    """A normalized repository-evidence decision gating snapshot conclusions.

    Defaults preserve the original constructor while making every qualification
    explicit for collectors that need to reject plans, adjacent scope, or
    otherwise unaccepted evidence.
    """

    conclusion_id: StableId
    contradicts: bool
    direct: bool
    verified: bool
    newer_than_snapshot: bool
    repository_evidence: bool = True
    accepted: bool = True
    same_scope: bool = True

    def __post_init__(self) -> None:
        object.__setattr__(self, "conclusion_id", StableId(self.conclusion_id))
        for field_name in (
            "contradicts",
            "direct",
            "verified",
            "newer_than_snapshot",
            "repository_evidence",
            "accepted",
            "same_scope",
        ):
            if not isinstance(getattr(self, field_name), bool):
                raise TypeError(f"{field_name} must be a bool")

    @property
    def overturns(self) -> bool:
        return all(
            (
                self.contradicts,
                self.direct,
                self.verified,
                self.newer_than_snapshot,
                self.repository_evidence,
                self.accepted,
                self.same_scope,
            )
        )


@dataclass(frozen=True, slots=True)
class EvidencePathNode:
    id: StableId
    title: str
    target_level: TargetLevel
    dependency_ids: tuple[StableId, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        object.__setattr__(
            self, "dependency_ids", tuple(StableId(value) for value in self.dependency_ids)
        )
        if not self.title.strip():
            raise ValueError("evidence path title must not be empty")


TARGET_LEVEL_DEFINITIONS = (
    TargetLevelDefinition(
        TargetLevel.T0_HOSTED_ADJACENCY, 0, "Hosted adjacency",
        "CLI tools, services, control planes, and thin-host application cores running on an existing host OS; this level reduces porting effort but does not complete OS substrate work.",
        CapabilityBoundary.HOSTED_ADJACENCY,
    ),
    TargetLevelDefinition(
        TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, 1, "Independent language platform",
        "Language, compiler, package, debugger, compatibility, and reproducible-backend independence sufficient for sustained development without a host language.",
        CapabilityBoundary.OS_SUBSTRATE,
    ),
    TargetLevelDefinition(
        TargetLevel.T2_FREESTANDING_SUBSTRATE, 2, "Freestanding substrate",
        "System ABI, freestanding core and runtime, target model, linker inputs, panic and allocation policy, and hardware-safe primitives without hosted runtime dependency.",
        CapabilityBoundary.OS_SUBSTRATE,
    ),
    TargetLevelDefinition(
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, 3, "Boot and kernel foundation",
        "Reproducible boot, memory management, interrupts, scheduling, syscalls, capabilities, drivers, storage, and networking foundations.",
        CapabilityBoundary.OS_SUBSTRATE,
    ),
    TargetLevelDefinition(
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM, 4, "Isolated userspace platform",
        "Process isolation, user runtime, system services, IPC, install and update, recovery, application APIs, and platform shell foundations.",
        CapabilityBoundary.OS_SUBSTRATE,
    ),
    TargetLevelDefinition(
        TargetLevel.T5_OPERABLE_UNIVERSE_OS, 5, "Operable Universe OS",
        "Supported hardware, security operations, observability, packaging, upgrades, recovery, application distribution, compatibility, and sustained ecosystem evidence.",
        CapabilityBoundary.OS_SUBSTRATE,
    ),
)

MATURITY_RUBRIC = (
    MaturityRubricEntry(MaturityScore.ABSENT, "No implementation evidence."),
    MaturityRubricEntry(MaturityScore.NARROW_EXPERIMENT, "Narrow experimental implementation."),
    MaturityRubricEntry(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION, "Repeatable repository-local implementation."),
    MaturityRubricEntry(MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT, "Candidate contract verified across supported hosts, with migration and rollback evidence."),
    MaturityRubricEntry(MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY, "Supported production capability."),
    MaturityRubricEntry(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM, "Mature independent ecosystem capability."),
)


def _capability(
    identifier: str,
    name: str,
    target_level: TargetLevel,
    checklist_rows: tuple[tuple[str, str], ...],
) -> CapabilityDefinition:
    capability_id = StableId(identifier)
    boundary = (
        CapabilityBoundary.HOSTED_ADJACENCY
        if target_level is TargetLevel.T0_HOSTED_ADJACENCY
        else CapabilityBoundary.OS_SUBSTRATE
    )
    return CapabilityDefinition(
        id=capability_id,
        name=name,
        target_level=target_level,
        boundary=boundary,
        mandatory_for_target=True,
        checklist=tuple(
            ChecklistItemDefinition(
                id=StableId(checklist_id), capability_id=capability_id, title=title
            )
            for checklist_id, title in checklist_rows
        ),
    )


CAPABILITY_DEFINITIONS = (
    _capability(
        "capability-t0-hosted-adjacency", "Hosted application adjacency",
        TargetLevel.T0_HOSTED_ADJACENCY,
        (
            ("check-t0-cli", "Hosted CLI tools"),
            ("check-t0-services", "Hosted backend services"),
            ("check-t0-control-plane", "Hosted control-plane applications"),
            ("check-t0-thin-host", "Thin-host application cores and bridges"),
        ),
    ),
    _capability(
        "capability-t1-language-platform", "Independent language platform",
        TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        (
            ("check-t1-language", "Stable language and low-level semantics"),
            ("check-t1-compiler", "Independent compiler and reproducible backend"),
            ("check-t1-package", "Package and dependency workflow"),
            ("check-t1-debugger", "Debugger and diagnostic integration"),
            ("check-t1-compatibility", "Compatibility governance"),
            ("check-t1-bootstrap", "Accepted independent bootstrap path"),
        ),
    ),
    _capability(
        "capability-t2-freestanding-substrate", "Freestanding substrate",
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        (
            ("check-t2-system-abi", "Freestanding system ABI"),
            ("check-t2-core", "Freestanding core library"),
            ("check-t2-runtime", "Freestanding startup and runtime"),
            ("check-t2-target", "Target model and linker inputs"),
            ("check-t2-panic-allocation", "Panic and allocation policy"),
            ("check-t2-hardware-primitives", "Hardware-safe low-level primitives"),
        ),
    ),
    _capability(
        "capability-t3-kernel-foundation", "Boot and kernel foundation",
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        (
            ("check-t3-boot", "Reproducible boot chain"),
            ("check-t3-memory", "Physical and virtual memory management"),
            ("check-t3-interrupts", "Interrupt and trap handling"),
            ("check-t3-scheduler", "Scheduling and context switching"),
            ("check-t3-syscalls", "Syscall and capability boundaries"),
            ("check-t3-drivers", "Driver and hardware abstractions"),
            ("check-t3-storage", "Storage foundation"),
            ("check-t3-networking", "Networking foundation"),
        ),
    ),
    _capability(
        "capability-t4-userspace-platform", "Isolated userspace platform",
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        (
            ("check-t4-isolation", "Process and address-space isolation"),
            ("check-t4-user-runtime", "Userspace runtime"),
            ("check-t4-services", "System services"),
            ("check-t4-ipc", "Inter-process communication"),
            ("check-t4-lifecycle", "Install, update, and recovery lifecycle"),
            ("check-t4-app-api", "Application APIs"),
            ("check-t4-shell", "Command and platform shell foundations"),
        ),
    ),
    _capability(
        "capability-t5-operable-os", "Operable Universe OS",
        TargetLevel.T5_OPERABLE_UNIVERSE_OS,
        (
            ("check-t5-hardware", "Qualified supported hardware"),
            ("check-t5-security", "Security operations"),
            ("check-t5-observability", "System-wide observability"),
            ("check-t5-packaging", "Packaging and application distribution"),
            ("check-t5-upgrade", "Upgrade, rollback, and recovery operations"),
            ("check-t5-compatibility", "Sustained compatibility"),
            ("check-t5-ecosystem", "Sustained independent ecosystem evidence"),
        ),
    ),
)

INITIAL_CONCLUSION_OVERTURN_RULE = (
    "An initial conclusion remains applicable unless newer evidence from the repository "
    "is accepted, contradicts it, and is direct and verified for the same scope; "
    "plans, adjacency, indirect evidence, unverified claims, rejected evidence, "
    "or evidence from another scope cannot overturn the snapshot contract."
)

INITIAL_CONCLUSIONS = (
    InitialConclusion(
        StableId("conclusion-15.1-hosted-foundation"),
        "Nebula is a promising hosted language, compiler/tooling, backend-service, and thin-host application-core foundation.",
        "15.1",
        "Newer direct verified evidence that the scoped hosted foundations are absent or no longer usable.",
    ),
    InitialConclusion(
        StableId("conclusion-15.2-t1-unachieved"),
        "T1_Independent_Language_Platform is materially unachieved because production compilation still depends on generated C++ and external host tooling.",
        "15.2",
        "Newer direct verified production evidence of an accepted independent backend/bootstrap path with no generated-C++ or external-host-tooling dependency.",
    ),
    InitialConclusion(
        StableId("conclusion-15.3-t2-t5-unachieved"),
        "T2_Freestanding_Substrate through T5_Operable_Universe_OS are unachieved under current evidence.",
        "15.3",
        "Newer direct verified evidence satisfying every mandatory domain and hard gate for each target level claimed achieved.",
    ),
    InitialConclusion(
        StableId("conclusion-15.4-language-tooling-cap"),
        "The strongest repository-local language/tooling capabilities have maturity no higher than 2 without cross-supported-host candidate evidence.",
        "15.4",
        "Newer direct verified cross-host candidate-contract, migration/rollback, and release-review evidence satisfying score 3.",
    ),
    InitialConclusion(
        StableId("conclusion-15.5-substrate-zero"),
        "Freestanding runtime, linked or bootable chain, kernel subsystems, and Universe OS userspace have maturity 0 without direct implementation evidence.",
        "15.5",
        "Newer direct verified implementation evidence for each specifically upgraded capability domain.",
    ),
    InitialConclusion(
        StableId("conclusion-15.6-adjacency-isolated"),
        "Hosted Adjacency can reduce future application-porting effort but remains separate from every OS Substrate critical-path dependency and hard gate.",
        "15.6",
        "A newer accepted target-model revision that directly and verifiably changes the ownership boundary without inferring substrate completion from hosted assets.",
    ),
    InitialConclusion(
        StableId("conclusion-15.7-shortest-path"),
        "The shortest evidence-backed path runs from low-level language soundness to system ABI, independent backend and freestanding runtime, then a complete boot toolchain and linked/bootable proof, followed by separately gated kernel and userspace subsystems.",
        "15.7",
        "Newer direct verified dependency evidence that removes or reorders a named hard gate while preserving explicit independent subsystem gates.",
    ),
)

SHORTEST_EVIDENCE_PATH_TEMPLATE = (
    EvidencePathNode(StableId("path-language-soundness"), "Low-level language soundness", TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM),
    EvidencePathNode(StableId("path-system-abi"), "Freestanding system ABI", TargetLevel.T2_FREESTANDING_SUBSTRATE, (StableId("path-language-soundness"),)),
    EvidencePathNode(StableId("path-independent-backend"), "Independent backend and bootstrap", TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, (StableId("path-system-abi"),)),
    EvidencePathNode(StableId("path-boot-toolchain"), "Complete boot toolchain", TargetLevel.T2_FREESTANDING_SUBSTRATE, (StableId("path-system-abi"),)),
    EvidencePathNode(StableId("path-freestanding-runtime"), "Freestanding core and runtime", TargetLevel.T2_FREESTANDING_SUBSTRATE, (StableId("path-independent-backend"),)),
    EvidencePathNode(StableId("path-primitive-object"), "Primitive ET_REL object proof", TargetLevel.T2_FREESTANDING_SUBSTRATE),
    EvidencePathNode(StableId("path-linked-elf"), "Deterministic linked ELF", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-freestanding-runtime"), StableId("path-boot-toolchain"), StableId("path-primitive-object"))),
    EvidencePathNode(StableId("path-boot-media"), "Boot media", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-linked-elf"),)),
    EvidencePathNode(StableId("path-qemu-execution"), "QEMU serial execution proof", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-boot-media"),)),
    EvidencePathNode(StableId("path-memory-management"), "Memory management and MMU", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-qemu-execution"),)),
    EvidencePathNode(StableId("path-interrupts"), "Interrupt and trap handling", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-qemu-execution"),)),
    EvidencePathNode(StableId("path-scheduler"), "Scheduler and context switching", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-memory-management"), StableId("path-interrupts"))),
    EvidencePathNode(StableId("path-syscall-capabilities"), "Syscall and capability boundary", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-scheduler"),)),
    EvidencePathNode(StableId("path-drivers-dma"), "Drivers and DMA safety", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-syscall-capabilities"),)),
    EvidencePathNode(StableId("path-storage"), "Storage stack", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-drivers-dma"),)),
    EvidencePathNode(StableId("path-networking"), "Networking stack", TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION, (StableId("path-drivers-dma"),)),
    EvidencePathNode(StableId("path-process-isolation"), "Process and address-space isolation", TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM, (StableId("path-syscall-capabilities"),)),
    EvidencePathNode(StableId("path-userspace"), "Universe OS userspace", TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM, (StableId("path-process-isolation"),)),
    EvidencePathNode(StableId("path-update-recovery"), "Update and recovery", TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM, (StableId("path-userspace"),)),
    EvidencePathNode(StableId("path-product-shell"), "Product shell", TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM, (StableId("path-userspace"),)),
    EvidencePathNode(StableId("path-operable-universe-os"), "Operable Universe OS", TargetLevel.T5_OPERABLE_UNIVERSE_OS, (StableId("path-storage"), StableId("path-networking"), StableId("path-update-recovery"), StableId("path-product-shell"))),
)


def applicable_initial_conclusions(
    evidence: Iterable[ConclusionEvidence],
) -> tuple[InitialConclusion, ...]:
    """Return snapshot conclusions not overturned by qualifying contrary evidence."""

    conclusion_ids = {item.id for item in INITIAL_CONCLUSIONS}
    decisions = tuple(evidence)
    unknown_ids = {item.conclusion_id for item in decisions} - conclusion_ids
    if unknown_ids:
        raise ValueError(f"unknown initial conclusion references: {sorted(unknown_ids)}")
    overturned = {item.conclusion_id for item in decisions if item.overturns}
    return tuple(item for item in INITIAL_CONCLUSIONS if item.id not in overturned)


def validate_catalog() -> None:
    """Fail closed if immutable Task 1.3 catalog metadata becomes inconsistent."""

    expected_levels = tuple(TargetLevel)
    if tuple(item.level for item in TARGET_LEVEL_DEFINITIONS) != expected_levels:
        raise ValueError("target definitions must contain ordered T0 through T5 exactly once")
    if tuple(item.order for item in TARGET_LEVEL_DEFINITIONS) != tuple(range(6)):
        raise ValueError("target definition order metadata must be 0 through 5")
    if tuple(item.score for item in MATURITY_RUBRIC) != tuple(MaturityScore):
        raise ValueError("maturity rubric must contain ordinal scores 0 through 5")

    target_boundaries = {item.level: item.boundary for item in TARGET_LEVEL_DEFINITIONS}
    capability_ids = [item.id for item in CAPABILITY_DEFINITIONS]
    checklist_ids = [check.id for item in CAPABILITY_DEFINITIONS for check in item.checklist]
    if len(capability_ids) != len(set(capability_ids)):
        raise ValueError("capability IDs must be unique")
    if len(checklist_ids) != len(set(checklist_ids)):
        raise ValueError("checklist IDs must be unique")
    for level in expected_levels:
        if not any(item.target_level is level and item.mandatory_for_target for item in CAPABILITY_DEFINITIONS):
            raise ValueError(f"target level {level.value} requires mandatory capability metadata")
    for item in CAPABILITY_DEFINITIONS:
        if item.boundary is not target_boundaries[item.target_level]:
            raise ValueError(f"capability {item.id} crosses its target boundary")
        if not all(check.mandatory for check in item.checklist):
            raise ValueError(f"capability {item.id} has a non-mandatory target checklist item")

    node_ids = {item.id for item in SHORTEST_EVIDENCE_PATH_TEMPLATE}
    if len(node_ids) != len(SHORTEST_EVIDENCE_PATH_TEMPLATE):
        raise ValueError("evidence-path node IDs must be unique")
    for item in SHORTEST_EVIDENCE_PATH_TEMPLATE:
        unknown = set(item.dependency_ids) - node_ids
        if unknown:
            raise ValueError(f"evidence-path node {item.id} has unknown dependencies")

    visiting: set[StableId] = set()
    visited: set[StableId] = set()
    by_id = {item.id: item for item in SHORTEST_EVIDENCE_PATH_TEMPLATE}

    def visit(node_id: StableId) -> None:
        if node_id in visiting:
            raise ValueError("shortest evidence path must be acyclic")
        if node_id in visited:
            return
        visiting.add(node_id)
        for dependency_id in by_id[node_id].dependency_ids:
            visit(dependency_id)
        visiting.remove(node_id)
        visited.add(node_id)

    for node_id in node_ids:
        visit(node_id)


validate_catalog()
