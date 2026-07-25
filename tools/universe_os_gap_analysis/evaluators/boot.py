"""Boot evaluator and pre-kernel Hard-Gate candidate generation (Task 6.3).

This declarative evaluator covers the boot slice of the capability model
(Requirements 7.7, 10.1, 12.5, 12.7, 15.7). It decomposes the path from a
freestanding system ABI to a QEMU serial-hello proof into *separate* pieces of
evidence and *separate* Hard-Gate candidates, and it wires those candidates into
the pre-kernel dependency chain the design's shortest-evidence path prescribes:

    low-level soundness -> freestanding system ABI
                             |-> independent backend / bootstrap -> freestanding runtime   (branch A)
                             |-> closed boot toolchain (target/protocol/entry/linker/         (branch B)
                             |   relocation/startup objects)
    (branch A + branch B + a primitive ET_REL object proof)
                             -> deterministic linked ELF (join gate)
                             -> boot media
                             -> QEMU serial execution

Two invariants are enforced structurally so a later stage can never be "proved"
by an earlier, weaker artifact:

* **Object / link / media / execute never merge (Requirement 7.7).** Each of the
  nine boot stages -- target specification, boot protocol, entry, linker
  script/input, relocation, startup object, deterministic linked ELF, boot media,
  and QEMU execution -- is a distinct :class:`CapabilityDomain`, a distinct
  :class:`GapEntry`, and a distinct :class:`HardGate` candidate with no fallback.
* **A primitive ET_REL object proof does not satisfy later stages
  (Requirements 7.7, 15.7).** The primitive relocatable-object gate is only one
  *input* to the deterministic-linked-ELF join. Its evidence markers are disjoint
  from the linked-ELF/media/QEMU markers, so primitive-object evidence can raise
  only the primitive-object gate and never lifts the linked-image, media, or
  execution gates out of maturity 0.

Scope boundaries (owned by other, parallel tasks): the coarse upstream nodes
(low-level soundness, system ABI, independent backend/bootstrap, freestanding
runtime, and the primitive ET_REL object gate) are emitted here only as chain
*candidates* so the dependency graph is expressible; their domains, gaps, raw
scores, and final capping are produced by the language/ABI/runtime evaluators and
the Maturity Assessor (Tasks 5.x, 6.1, 6.2, 8.x). This module never mutates
evidence, never upgrades a status, and edits no sibling evaluator file. It reads
the shared ``catalog.SHORTEST_EVIDENCE_PATH_TEMPLATE`` read-only to keep its
decomposition consistent with the design's canonical chain.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from ..catalog import SHORTEST_EVIDENCE_PATH_TEMPLATE
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
    HardGate,
    MaturityScore,
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS``
# --------------------------------------------------------------------------- #
_PARENT_T2 = StableId("capability-t2-freestanding-substrate")
_PARENT_T3 = StableId("capability-t3-kernel-foundation")

#: Parallel-branch labels for the two independent pre-join workstreams.
BRANCH_BACKEND_BOOTSTRAP = "backend-bootstrap"
BRANCH_BOOT_TOOLCHAIN = "boot-toolchain"

#: The join gate key where the two parallel branches (and the primitive object
#: input) converge.
JOIN_GATE_KEY = "deterministic-linked-elf"

# Statuses that assert a present-tense/current implementation to some degree.
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


class BootStageKind(Enum):
    """Whether a chain node is a boot stage (domain+gap+gate) or an upstream candidate."""

    #: A boot stage: emits a CapabilityDomain, a GapEntry, and a HardGate candidate.
    BOOT_STAGE = "boot_stage"
    #: An upstream chain candidate: emits only a HardGate candidate (owned elsewhere).
    UPSTREAM_CANDIDATE = "upstream_candidate"


@dataclass(frozen=True, slots=True, kw_only=True)
class BootGateSpec:
    """A declarative specification of one node in the pre-kernel candidate chain."""

    key: str
    title: str
    target_level: TargetLevel
    kind: BootStageKind
    dependency_keys: tuple[str, ...]
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str
    markers: tuple[str, ...] = ()
    parent_capability_id: StableId | None = None
    parallel_branch: str | None = None
    is_join_gate: bool = False
    is_primitive_object: bool = False
    gap_category: GapCategory = GapCategory.IMPLEMENTATION

    def __post_init__(self) -> None:
        if not self.key.strip():
            raise ValueError("gate key must not be empty")
        if not self.title.strip():
            raise ValueError("gate title must not be empty")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.kind, BootStageKind):
            raise TypeError("kind must be a BootStageKind")
        if not isinstance(self.gap_category, GapCategory):
            raise TypeError("gap_category must be a GapCategory")
        for name in ("requirement_refs", "acceptance_evidence"):
            if not tuple(getattr(self, name)):
                raise ValueError(f"{name} must not be empty")
        if self.parent_capability_id is not None:
            object.__setattr__(
                self, "parent_capability_id", StableId(self.parent_capability_id)
            )
        if self.kind is BootStageKind.BOOT_STAGE and self.parent_capability_id is None:
            raise ValueError("a boot stage must declare a parent capability")

    @property
    def gate_id(self) -> StableId:
        return stable_id("gate", "boot", self.key)

    @property
    def domain_id(self) -> StableId:
        return stable_id("domain", "boot", self.key)

    @property
    def gap_id(self) -> StableId:
        return stable_id("gap", "boot", self.key)


# --------------------------------------------------------------------------- #
# The pre-kernel candidate chain (Requirement 12.5 sequence; 12.7 parallelism). #
# --------------------------------------------------------------------------- #
#
# Upstream candidates first (owned by other evaluators), then the two parallel
# branches, then the join gate and downstream media/execution stages. Keys are
# stable; dependency edges reference other keys and are resolved to gate IDs when
# the HardGate objects are built.

BOOT_CHAIN_SPECS: tuple[BootGateSpec, ...] = (
    # -- upstream candidates (chain scaffolding, owned by other evaluators) -- #
    BootGateSpec(
        key="low-level-soundness",
        title="Low-level language soundness",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        kind=BootStageKind.UPSTREAM_CANDIDATE,
        dependency_keys=(),
        requirement_refs=("12.5", "15.7"),
        acceptance_evidence=(
            "Normative low-level language semantics (layout, initialization, "
            "destruction, aliasing, system-call boundaries) with direct evidence.",
        ),
        non_claims=(
            "This candidate is owned by the language/safety evaluators; the boot "
            "evaluator only positions it as the chain root.",
        ),
        owner_area="Language & Safety",
    ),
    BootGateSpec(
        key="system-abi",
        title="Freestanding system ABI",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.UPSTREAM_CANDIDATE,
        dependency_keys=("low-level-soundness",),
        requirement_refs=("12.5", "15.7"),
        acceptance_evidence=(
            "A freestanding system ABI (calling convention, symbol, layout, and "
            "alignment rules) independent of the hosted runtime.",
        ),
        non_claims=(
            "This candidate is owned by the ABI/backend evaluator; the boot chain "
            "consumes it as the common predecessor of both parallel branches.",
        ),
        owner_area="ABI & Backend",
    ),
    BootGateSpec(
        key="independent-backend-bootstrap",
        title="Independent backend / bootstrap",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        kind=BootStageKind.UPSTREAM_CANDIDATE,
        dependency_keys=("system-abi",),
        requirement_refs=("12.5", "12.7", "15.7"),
        acceptance_evidence=(
            "An accepted independent backend/bootstrap path with no generated-C++ "
            "or external host-toolchain production dependency.",
        ),
        non_claims=(
            "Owned by the ABI/backend evaluator; positioned here as the head of the "
            "backend/bootstrap parallel branch.",
        ),
        owner_area="ABI & Backend",
        parallel_branch=BRANCH_BACKEND_BOOTSTRAP,
    ),
    BootGateSpec(
        key="freestanding-runtime",
        title="Freestanding core / runtime",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.UPSTREAM_CANDIDATE,
        dependency_keys=("independent-backend-bootstrap",),
        requirement_refs=("12.5", "12.7", "15.7"),
        acceptance_evidence=(
            "A freestanding core/runtime (startup, static init, panic, allocation, "
            "termination) with no hosted-runtime dependency.",
        ),
        non_claims=(
            "Owned by the freestanding-runtime evaluator; positioned here as the "
            "backend/bootstrap-branch predecessor of the linked-ELF join.",
        ),
        owner_area="Freestanding Runtime",
        parallel_branch=BRANCH_BACKEND_BOOTSTRAP,
    ),
    BootGateSpec(
        key="primitive-et-rel-object",
        title="Primitive ET_REL relocatable-object proof",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.UPSTREAM_CANDIDATE,
        dependency_keys=(),
        requirement_refs=("7.7", "15.7"),
        acceptance_evidence=(
            "Clang-backed ELF64 relocatable-object (ET_REL) emission for the "
            "primitive freestanding type subset.",
        ),
        non_claims=(
            "A primitive ET_REL relocatable-object proof is only an input to the "
            "deterministic-linked-ELF join; it does not satisfy deterministic "
            "linking, boot media, or boot execution, nor does it prove a direct "
            "backend, linked image, runtime, or boot capability.",
        ),
        owner_area="ABI & Backend",
        markers=(
            "et_rel",
            "relocatable object",
            "relocatable-object",
            "primitive freestanding",
            "primitive object",
            "freestanding object",
        ),
        is_primitive_object=True,
    ),
    # -- branch B: the closed boot toolchain (six independent stages) -------- #
    BootGateSpec(
        key="target-spec",
        title="Boot target specification",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "10.1", "12.5"),
        acceptance_evidence=(
            "A reproducible target specification (architecture, data layout, and "
            "code model) with direct implementation evidence.",
        ),
        non_claims=(
            "No reproducible boot target specification is implemented; a documented "
            "target description is not target-specification evidence.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "target specification",
            "target spec",
            "target triple",
            "target model",
            "target descriptor",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    BootGateSpec(
        key="boot-protocol",
        title="Boot protocol and firmware handoff",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "10.1", "12.5"),
        acceptance_evidence=(
            "An implemented boot protocol / firmware handoff contract (entry state, "
            "memory map, and hand-off registers) for a supported target.",
        ),
        non_claims=(
            "No boot protocol or firmware handoff is implemented; this remains an "
            "explicit non-claim.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "boot protocol",
            "multiboot",
            "uefi",
            "bios boot",
            "bootloader handoff",
            "firmware handoff",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    BootGateSpec(
        key="boot-entry",
        title="Boot entry point",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "10.1", "12.5"),
        acceptance_evidence=(
            "An implemented freestanding entry point that establishes an initial "
            "execution environment before runtime startup.",
        ),
        non_claims=(
            "No freestanding boot entry point is implemented; this remains an "
            "explicit non-claim.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "boot entry",
            "entry point",
            "_start symbol",
            "kernel entry point",
            "reset vector",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    BootGateSpec(
        key="linker-script-input",
        title="Linker script / linker inputs",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "12.5"),
        acceptance_evidence=(
            "Implemented linker scripts and linker inputs that place sections and "
            "symbols deterministically for a freestanding image.",
        ),
        non_claims=(
            "No linker script or linker-input control is implemented; this remains "
            "an explicit non-claim.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "linker script",
            "linker input",
            "link script",
            "ld script",
            "linker.ld",
            "section placement",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    BootGateSpec(
        key="relocation",
        title="Relocation support",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "12.5"),
        acceptance_evidence=(
            "Implemented relocation processing that resolves relocation entries "
            "during linking for a freestanding image.",
        ),
        non_claims=(
            "No relocation processing is implemented; a primitive relocatable-object "
            "emission is not relocation-support evidence.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "relocation processing",
            "relocation support",
            "apply relocation",
            "resolve relocation",
            "relocation entries",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    BootGateSpec(
        key="startup-object",
        title="Startup object",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("system-abi",),
        requirement_refs=("7.7", "12.5"),
        acceptance_evidence=(
            "An implemented startup object (crt-equivalent) that prepares the "
            "environment and transfers control to the freestanding entry point.",
        ),
        non_claims=(
            "No startup object is implemented; this remains an explicit non-claim.",
        ),
        owner_area="Boot Toolchain",
        markers=(
            "startup object",
            "crt0",
            "crt object",
            "startup code",
            "start object",
        ),
        parent_capability_id=_PARENT_T2,
        parallel_branch=BRANCH_BOOT_TOOLCHAIN,
    ),
    # -- the join gate: deterministic linked ELF ----------------------------- #
    BootGateSpec(
        key=JOIN_GATE_KEY,
        title="Deterministic linked ELF",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=(
            "freestanding-runtime",
            "primitive-et-rel-object",
            "target-spec",
            "boot-protocol",
            "boot-entry",
            "linker-script-input",
            "relocation",
            "startup-object",
        ),
        requirement_refs=("7.7", "12.5", "12.7", "15.7"),
        acceptance_evidence=(
            "A deterministic, fully linked ELF image produced from the freestanding "
            "runtime and the complete boot toolchain, reproducible across builds.",
        ),
        non_claims=(
            "No deterministic linked ELF image exists; a primitive ET_REL "
            "relocatable-object proof does not satisfy this join gate.",
        ),
        owner_area="Boot Chain",
        markers=(
            "linked elf",
            "linked image",
            "deterministic link",
            "linked executable",
            "fully linked",
        ),
        parent_capability_id=_PARENT_T3,
        is_join_gate=True,
    ),
    # -- downstream: boot media then QEMU execution -------------------------- #
    BootGateSpec(
        key="boot-media",
        title="Boot media assembly",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=(JOIN_GATE_KEY,),
        requirement_refs=("7.7", "10.1", "12.5", "15.7"),
        acceptance_evidence=(
            "A bootable media artifact assembled from the deterministic linked ELF "
            "image (disk/ISO image) suitable for a supported target.",
        ),
        non_claims=(
            "No boot media assembly exists; boot media requires a deterministic "
            "linked ELF and cannot be produced from a relocatable object alone.",
        ),
        owner_area="Boot Chain",
        markers=(
            "boot media",
            "disk image",
            "bootable image",
            "iso image",
            "boot disk",
            "media assembly",
        ),
        parent_capability_id=_PARENT_T3,
    ),
    BootGateSpec(
        key="qemu-execution",
        title="QEMU serial execution proof",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        kind=BootStageKind.BOOT_STAGE,
        dependency_keys=("boot-media",),
        requirement_refs=("7.7", "10.1", "12.5", "15.7"),
        acceptance_evidence=(
            "A reproducible QEMU boot that executes the boot media and emits the "
            "expected serial output for a supported target.",
        ),
        non_claims=(
            "No QEMU boot execution exists; a passing serial-hello would prove only "
            "boot execution and would not imply drivers, interrupts, MMU, "
            "scheduler, syscalls, isolation, storage, networking, or userspace.",
        ),
        owner_area="Boot Chain",
        markers=(
            "qemu",
            "serial hello",
            "serial output",
            "boot execution",
            "emulator boot",
        ),
        parent_capability_id=_PARENT_T3,
    ),
)


@dataclass(frozen=True, slots=True, kw_only=True)
class BootStageAssessment:
    """Per-node assessment: observed status, maturity, evidence, and satisfaction."""

    key: str
    gate_id: ReferenceId
    kind: BootStageKind
    observed_status: EvidenceStatus
    maturity: MaturityScore
    supporting_evidence_ids: tuple[ReferenceId, ...]
    satisfied: bool
    parallel_branch: str | None
    is_primitive_object: bool

    def __post_init__(self) -> None:
        object.__setattr__(self, "gate_id", reference(self.gate_id))
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(v) for v in self.supporting_evidence_ids}, key=str)),
        )


@dataclass(frozen=True, slots=True)
class BootEvaluation:
    """The boot evaluator output: boot-stage domains, gaps, and chain gates."""

    domains: tuple[CapabilityDomain, ...]
    gaps: tuple[GapEntry, ...]
    hard_gates: tuple[HardGate, ...]
    assessments: tuple[BootStageAssessment, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "domains", tuple(sorted(self.domains, key=lambda d: str(d.id)))
        )
        object.__setattr__(
            self, "gaps", tuple(sorted(self.gaps, key=lambda g: str(g.id)))
        )
        object.__setattr__(
            self, "hard_gates", tuple(sorted(self.hard_gates, key=lambda g: str(g.id)))
        )
        object.__setattr__(
            self, "assessments", tuple(sorted(self.assessments, key=lambda a: a.key))
        )

    def gate_for(self, key: str) -> HardGate | None:
        target = str(stable_id("gate", "boot", key))
        for gate in self.hard_gates:
            if str(gate.id) == target:
                return gate
        return None

    def gap_for(self, key: str) -> GapEntry | None:
        target = str(stable_id("gap", "boot", key))
        for gap in self.gaps:
            if str(gap.id) == target:
                return gap
        return None

    def domain_for(self, key: str) -> CapabilityDomain | None:
        target = str(stable_id("domain", "boot", key))
        for domain in self.domains:
            if str(domain.id) == target:
                return domain
        return None

    def assessment_for(self, key: str) -> BootStageAssessment | None:
        for assessment in self.assessments:
            if assessment.key == key:
                return assessment
        return None

    def dependency_edges(self) -> tuple[tuple[str, str], ...]:
        """Return (dependency_key, dependent_key) edges over the candidate chain."""

        edges: list[tuple[str, str]] = []
        for spec in BOOT_CHAIN_SPECS:
            for dep in spec.dependency_keys:
                edges.append((dep, spec.key))
        return tuple(sorted(edges))


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


class BootEvaluator:
    """Evaluate the boot slice and generate pre-kernel Hard-Gate candidates."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> BootEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")

        records = bundle.records
        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted for claim in guarded.claims
        }

        gate_id_by_key = {spec.key: spec.gate_id for spec in BOOT_CHAIN_SPECS}

        assessments: list[BootStageAssessment] = []
        domains: list[CapabilityDomain] = []
        gaps: list[GapEntry] = []
        hard_gates: list[HardGate] = []

        for spec in BOOT_CHAIN_SPECS:
            assessment = self._assess(spec, records, present_permitted)
            assessments.append(assessment)

            gap: GapEntry | None = None
            if spec.kind is BootStageKind.BOOT_STAGE and not assessment.satisfied:
                gap = self._build_gap(spec, assessment)
                gaps.append(gap)
            if spec.kind is BootStageKind.BOOT_STAGE:
                domains.append(self._build_domain(spec, assessment, gap))

            hard_gates.append(self._build_gate(spec, assessment, gate_id_by_key))

        return BootEvaluation(
            domains=tuple(domains),
            gaps=tuple(gaps),
            hard_gates=tuple(hard_gates),
            assessments=tuple(assessments),
        )

    # -- per-node assessment --------------------------------------------- #

    def _assess(
        self,
        spec: BootGateSpec,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> BootStageAssessment:
        matched = tuple(
            record
            for record in records
            if spec.markers and _contains(_record_text(record), spec.markers)
        )
        supporting_ids = tuple(reference(record.id) for record in matched)

        # A stage is satisfied only with direct, present-permitted current
        # implementation evidence for its own markers. Documented, planned,
        # hosted, or upstream evidence never satisfies a boot stage, and the
        # primitive-object markers are disjoint from every later stage so a
        # relocatable-object proof cannot satisfy linking, media, or execution.
        implemented = tuple(
            record
            for record in matched
            if record.status in _IMPLEMENTED_STATUSES
            and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
            and present_permitted.get(str(record.id), False)
        )
        satisfied = bool(implemented)

        if satisfied:
            observed_status = _strongest_status(implemented)
            maturity = MaturityScore.NARROW_EXPERIMENT
        elif matched:
            observed_status = _strongest_status(matched)
            maturity = MaturityScore.ABSENT
        elif spec.kind is BootStageKind.UPSTREAM_CANDIDATE:
            # Upstream candidates are resolved by their owning evaluators; here
            # they are only chain scaffolding with no boot-specific evidence.
            observed_status = EvidenceStatus.UNKNOWN
            maturity = MaturityScore.ABSENT
        else:
            # A boot stage with no evidence is an explicit non-claim in the repo.
            observed_status = EvidenceStatus.UNSUPPORTED
            maturity = MaturityScore.ABSENT

        return BootStageAssessment(
            key=spec.key,
            gate_id=spec.gate_id,
            kind=spec.kind,
            observed_status=observed_status,
            maturity=maturity,
            supporting_evidence_ids=supporting_ids,
            satisfied=satisfied,
            parallel_branch=spec.parallel_branch,
            is_primitive_object=spec.is_primitive_object,
        )

    # -- builders --------------------------------------------------------- #

    def _build_domain(
        self,
        spec: BootGateSpec,
        assessment: BootStageAssessment,
        gap: GapEntry | None,
    ) -> CapabilityDomain:
        assert spec.parent_capability_id is not None  # guaranteed for boot stages
        return CapabilityDomain(
            id=spec.domain_id,
            name=spec.title,
            target_level=spec.target_level,
            description=(
                f"Requirement {', '.join(spec.requirement_refs)} boot-chain stage "
                "assessed by the boot evaluator as a separate piece of evidence and "
                "a separate Hard-Gate candidate."
            ),
            mandatory_for_target=True,
            parent_id=reference(spec.parent_capability_id),
            evidence_ids=assessment.supporting_evidence_ids,
            gap_ids=(reference(gap.id),) if gap is not None else (),
            dependency_gate_ids=(reference(spec.gate_id),),
        )

    def _build_gap(
        self, spec: BootGateSpec, assessment: BootStageAssessment
    ) -> GapEntry:
        is_downstream = spec.target_level is TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION
        dependency_criticality = 3 if is_downstream else 2
        observed = (
            f"{spec.title} has no direct implementation evidence; strongest observed "
            f"status is {assessment.observed_status.value}. It remains a separate, "
            "unmerged pre-kernel Hard-Gate candidate."
        )
        recommendation = (
            f"Implement {spec.title} as an independent gate with direct evidence; "
            "it must not be satisfied by upstream or primitive relocatable-object proofs."
        )
        return GapEntry(
            id=spec.gap_id,
            title=f"{spec.title} gap",
            primary_category=spec.gap_category,
            secondary_categories=(GapCategory.VERIFICATION,),
            domain_ids=(reference(spec.domain_id),),
            current_status=assessment.observed_status,
            target_level=spec.target_level,
            severity=Severity.HIGH,
            dependencies=(),
            acceptance_evidence=spec.acceptance_evidence,
            recommended_owner_area=spec.owner_area,
            dependency_criticality=dependency_criticality,
            safety_impact=1,
            claim_risk=2 if spec.is_join_gate else 1,
            target_unblock_value=dependency_criticality,
            observed_fact=observed,
            recommendation=recommendation,
        )

    def _build_gate(
        self,
        spec: BootGateSpec,
        assessment: BootStageAssessment,
        gate_id_by_key: Mapping[str, StableId],
    ) -> HardGate:
        dependency_ids = tuple(
            reference(gate_id_by_key[dep]) for dep in spec.dependency_keys
        )
        blocking_domain_ids = (
            (reference(spec.domain_id),) if spec.kind is BootStageKind.BOOT_STAGE else ()
        )
        # Branch members and the primitive-object input converge at the join gate.
        join_gate_ids: tuple[ReferenceId, ...] = ()
        if JOIN_GATE_KEY in spec.dependency_keys or (
            spec.parallel_branch is not None and not spec.is_join_gate
        ):
            # The immediate predecessors of the join declare it as their join gate.
            if spec.key in {
                "freestanding-runtime",
                "primitive-et-rel-object",
                "target-spec",
                "boot-protocol",
                "boot-entry",
                "linker-script-input",
                "relocation",
                "startup-object",
            }:
                join_gate_ids = (reference(gate_id_by_key[JOIN_GATE_KEY]),)
        # The primitive object is also an immediate predecessor of the join even
        # though it carries no branch label.
        if spec.is_primitive_object:
            join_gate_ids = (reference(gate_id_by_key[JOIN_GATE_KEY]),)

        return HardGate(
            id=spec.gate_id,
            title=spec.title,
            target_level=spec.target_level,
            status=assessment.observed_status,
            maturity_score=assessment.maturity,
            dependency_ids=dependency_ids,
            blocking_domain_ids=blocking_domain_ids,
            evidence_ids=assessment.supporting_evidence_ids,
            acceptance_evidence=spec.acceptance_evidence,
            non_claims=spec.non_claims,
            owner_area=spec.owner_area,
            parallel_branch=spec.parallel_branch,
            join_gate_ids=join_gate_ids,
        )


def evaluate_boot(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> BootEvaluation:
    """Convenience API for the boot evaluator and pre-kernel gate generation."""

    return BootEvaluator().evaluate(bundle, guarded)


def _validate_chain_against_template() -> None:
    """Fail closed if our decomposition diverges from the canonical shortest path.

    This is a read-only consistency check against
    ``catalog.SHORTEST_EVIDENCE_PATH_TEMPLATE`` so the boot evaluator's finer
    decomposition stays faithful to the design's canonical chain: the linked-ELF
    join consumes the freestanding runtime, the boot toolchain, and the primitive
    ET_REL object; media follows the linked ELF; and QEMU follows media.
    """

    by_id = {node.id: node for node in SHORTEST_EVIDENCE_PATH_TEMPLATE}

    def deps(node_id: str) -> set[StableId]:
        return set(by_id[StableId(node_id)].dependency_ids)

    linked_elf_deps = deps("path-linked-elf")
    required = {
        StableId("path-freestanding-runtime"),
        StableId("path-boot-toolchain"),
        StableId("path-primitive-object"),
    }
    if not required <= linked_elf_deps:
        raise ValueError("linked-ELF join must depend on runtime, boot toolchain, and object")
    if StableId("path-linked-elf") not in deps("path-boot-media"):
        raise ValueError("boot media must depend on the deterministic linked ELF")
    if StableId("path-boot-media") not in deps("path-qemu-execution"):
        raise ValueError("QEMU execution must depend on boot media")
    if deps("path-primitive-object"):
        raise ValueError("the primitive ET_REL object gate must be an independent input")


def _validate_specs() -> None:
    """Fail closed on internal inconsistencies in the candidate-chain definition."""

    keys = [spec.key for spec in BOOT_CHAIN_SPECS]
    if len(keys) != len(set(keys)):
        raise ValueError("boot chain keys must be unique")
    known = set(keys)
    ids = {spec.gate_id for spec in BOOT_CHAIN_SPECS}
    if len(ids) != len(BOOT_CHAIN_SPECS):
        raise ValueError("boot gate IDs must be unique")
    for spec in BOOT_CHAIN_SPECS:
        unknown = set(spec.dependency_keys) - known
        if unknown:
            raise ValueError(f"gate {spec.key} references unknown dependencies: {unknown}")
        if spec.key in spec.dependency_keys:
            raise ValueError(f"gate {spec.key} cannot depend on itself")

    # Acyclicity of the candidate DAG.
    by_key = {spec.key: spec for spec in BOOT_CHAIN_SPECS}
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(key: str) -> None:
        if key in visiting:
            raise ValueError("boot candidate chain must be acyclic")
        if key in visited:
            return
        visiting.add(key)
        for dep in by_key[key].dependency_keys:
            visit(dep)
        visiting.remove(key)
        visited.add(key)

    for key in known:
        visit(key)


_validate_chain_against_template()
_validate_specs()
