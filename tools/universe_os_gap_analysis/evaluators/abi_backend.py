"""FFI/ABI, compilation, linking, and backend evaluator (Task 6.1).

This declarative evaluator covers Requirement 7 acceptance criteria 7.1-7.6
(FFI/ABI, compilation, linking, and backend gaps). Boot-toolchain gate
decomposition (7.7) is owned by the sibling boot evaluator (Task 6.3); this
module focuses on ABI scope isolation, compiler-pipeline completeness, the
T1 independence blockers, and the primitive-object wording bound.

Like the memory/concurrency/safety evaluator (Task 5.2), this module is
intentionally self-contained: it defines module-local draft/result types and
never mutates evidence or edits sibling evaluator files. It reuses the shared
Task 1.x models, the Task 4.1 :class:`EvidenceBundle`, the Task 4.3 Claim Guard,
and the Claim Guard's canonical primitive-object wording so the wording bound
stays consistent across components.

Requirement 7 coverage:

* **7.1 hosted C ABI surface.** Imported extern contracts, exported C ABI types,
  calling conventions, symbol rules, aggregate layout, enum layout, alignment,
  versioning, and cross-language fixtures are assessed as the hosted C ABI
  capability domain.
* **7.2 ABI scope isolation.** Hosted C ABI evidence is kept strictly separate
  from freestanding compiler ABI, runtime ABI, boot ABI, syscall ABI, driver
  ABI, and package ABI. Each freestanding ABI domain matches only its own scope
  markers, so hosted C ABI evidence can never satisfy a freestanding ABI domain
  (design Property 12).
* **7.3 compiler pipeline.** Frontend completeness, NIR/CFG, analyses,
  optimization, incremental compilation, debug information, native code
  generation, assembler integration, linker integration, and bootstrap
  reproducibility are each assessed as a pipeline-stage domain. Native
  code generation, assembler, linker, and bootstrap are *independence* stages
  that clang-backed or generated-C++ evidence can never satisfy.
* **7.4 incomplete inventory blocks T1.** While the production compiler
  dependency inventory is incomplete, ``T1_Independent_Language_Platform`` is
  classified unachieved.
* **7.5 generated C++/external clang blocks T1.** While generated C++ or
  external clang remains a production dependency without an accepted independent
  bootstrap path, ``T1_Independent_Language_Platform`` is classified unachieved.
* **7.6 primitive-object wording bound.** A passing primitive object gate is
  described only as clang-backed ELF relocatable-object emission, never as
  direct backend, linked-image, runtime, or boot evidence.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from ..claim_guard import (
    PRIMITIVE_OBJECT_FORBIDDEN_TERMS,
    PRIMITIVE_OBJECT_SCOPE_NOTE,
    PRIMITIVE_OBJECT_WORDING,
    GuardedEvidence,
    guard_evidence,
)
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
    Severity,
    TargetLevel,
    VerificationState,
)

# --------------------------------------------------------------------------- #
# ABI scopes and capability kinds                                             #
# --------------------------------------------------------------------------- #


class AbiScope(ClosedStrEnum):
    """The seven distinct ABI scopes assessed by Requirement 7.2.

    Keeping these scopes distinct is what prevents hosted C ABI evidence from
    ever being counted as freestanding compiler, runtime, boot, syscall, driver,
    or package ABI evidence (design Property 12).
    """

    HOSTED_C_ABI = "HostedCAbi"
    COMPILER_ABI = "CompilerAbi"
    RUNTIME_ABI = "RuntimeAbi"
    BOOT_ABI = "BootAbi"
    SYSCALL_ABI = "SyscallAbi"
    DRIVER_ABI = "DriverAbi"
    PACKAGE_ABI = "PackageAbi"


class AbiBackendCapabilityKind(Enum):
    """The classification rule a checklist item applies."""

    #: Hosted C ABI surface: satisfied by a normative spec or current impl.
    ABI_HOSTED = "abi_hosted"
    #: Freestanding ABI scope: satisfied only by current in-scope implementation.
    ABI_FREESTANDING = "abi_freestanding"
    #: Pipeline stage that hosted/generated-C++ evidence can satisfy.
    PIPELINE_IMPLEMENTED = "pipeline_implemented"
    #: Backend-independence stage that clang-backed/primitive evidence cannot satisfy.
    PIPELINE_INDEPENDENCE = "pipeline_independence"


# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent. Each freestanding ABI scope owns a distinct, explicit marker so
# a hosted C ABI claim (which never contains "runtime abi", "boot abi", etc.)
# cannot leak into a freestanding ABI domain.

_HOSTED_C_ABI_MARKERS: tuple[str, ...] = (
    "hosted c abi",
    "c abi",
    "c-abi",
    'extern "c"',
    "extern c",
    "export c abi",
    "exported c abi",
    "calling convention",
    "symbol rule",
    "symbol naming",
    "aggregate layout",
    "enum layout",
    "cross-language fixture",
    "cross language fixture",
    "interop_c_abi",
    "abi_layout",
)

# Distinct, explicit freestanding-scope markers (Requirement 7.2).
_COMPILER_ABI_MARKERS: tuple[str, ...] = (
    "compiler abi",
    "compiler-abi",
    "freestanding compiler abi",
    "internal compiler abi",
)
_RUNTIME_ABI_MARKERS: tuple[str, ...] = ("runtime abi", "runtime-abi")
_BOOT_ABI_MARKERS: tuple[str, ...] = ("boot abi", "boot-abi")
_SYSCALL_ABI_MARKERS: tuple[str, ...] = (
    "syscall abi",
    "syscall-abi",
    "system call abi",
    "system-call abi",
)
_DRIVER_ABI_MARKERS: tuple[str, ...] = ("driver abi", "driver-abi")
_PACKAGE_ABI_MARKERS: tuple[str, ...] = ("package abi", "package-abi")

# Compiler pipeline stage markers (Requirement 7.3).
_FRONTEND_MARKERS: tuple[str, ...] = (
    "frontend",
    "front-end",
    "parser",
    "typechecker",
    "type checker",
    "lexer",
    "typed ast",
    "abstract syntax tree",
)
_NIR_CFG_MARKERS: tuple[str, ...] = (
    "nir",
    "control flow graph",
    "control-flow graph",
    "cfg lowering",
    "basic block",
)
_ANALYSES_MARKERS: tuple[str, ...] = (
    "analysis pass",
    "analyses",
    "dataflow analysis",
    "data-flow analysis",
    "borrow analysis",
    "exclusivity analysis",
)
_OPTIMIZATION_MARKERS: tuple[str, ...] = (
    "optimization",
    "optimisation",
    "optimizer",
    "inlining pass",
    "constant folding",
)
_INCREMENTAL_MARKERS: tuple[str, ...] = (
    "incremental compilation",
    "incremental build",
    "incremental rebuild",
)
_DEBUG_INFO_MARKERS: tuple[str, ...] = (
    "debug info",
    "debug information",
    "debuginfo",
    "dwarf",
    "source-level debug",
)
_NATIVE_CODEGEN_MARKERS: tuple[str, ...] = (
    "native code generation",
    "native codegen",
    "native backend",
    "direct backend",
    "machine code emission",
    "instruction selection",
)
_ASSEMBLER_MARKERS: tuple[str, ...] = (
    "assembler integration",
    "assembler",
    "assembly emission",
    "emit assembly",
)
_LINKER_MARKERS: tuple[str, ...] = (
    "linker integration",
    "linker",
    "linking stage",
    "link step",
)
_BOOTSTRAP_MARKERS: tuple[str, ...] = (
    "bootstrap reproducibility",
    "self-hosting",
    "self host",
    "self-host",
    "compiler bootstrap",
    "independent bootstrap",
)

# Production-dependency detection for the T1 independence assessment.
_GENERATED_CPP_MARKERS: tuple[str, ...] = (
    "generated c++",
    "generated cpp",
    "c++23",
    "cpp emitter",
    "c++ emitter",
    "transpile",
    "transpiler",
    "-> c++",
    "to c++",
)
_EXTERNAL_CLANG_MARKERS: tuple[str, ...] = (
    "external clang",
    "clang++",
    "host compiler",
    "host c++ compiler",
    "host toolchain",
    "external host compiler",
)
_INVENTORY_COMPLETE_MARKERS: tuple[str, ...] = (
    "dependency inventory complete",
    "complete dependency inventory",
    "complete production dependency inventory",
    "production dependency inventory is complete",
)
_ACCEPTED_BOOTSTRAP_MARKERS: tuple[str, ...] = (
    "accepted independent bootstrap",
    "independent bootstrap path",
    "independent bootstrap accepted",
    "reproducible independent bootstrap",
)

# Primitive freestanding object markers (Requirement 7.6). Reused shape from the
# Claim Guard so the wording bound stays consistent across components.
_PRIMITIVE_OBJECT_MARKERS: tuple[str, ...] = (
    "primitive freestanding",
    "primitive object",
    "et_rel",
    "relocatable object",
    "relocatable-object",
    "freestanding object",
)

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

# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS``.
_PARENT_T1 = StableId("capability-t1-language-platform")
_PARENT_T2 = StableId("capability-t2-freestanding-substrate")
_PARENT_T3 = StableId("capability-t3-kernel-foundation")


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


def _is_clang_or_primitive_backed(record: EvidenceRecord) -> bool:
    """True when a record only proves clang-backed / generated-C++ / primitive work."""

    text = _record_text(record)
    return _contains(text, _PRIMITIVE_OBJECT_MARKERS) or _contains(
        text, _GENERATED_CPP_MARKERS + _EXTERNAL_CLANG_MARKERS
    )


# --------------------------------------------------------------------------- #
# Checklist item and result types                                             #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True, slots=True)
class AbiBackendChecklistItem:
    """One declarative capability check for the ABI/backend domain."""

    capability_id: StableId
    name: str
    target_level: TargetLevel
    parent_capability_id: StableId
    kind: AbiBackendCapabilityKind
    scope: AbiScope | None
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    unsatisfied_category: GapCategory
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        object.__setattr__(self, "parent_capability_id", StableId(self.parent_capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.kind, AbiBackendCapabilityKind):
            raise TypeError("kind must be an AbiBackendCapabilityKind")
        if self.scope is not None and not isinstance(self.scope, AbiScope):
            raise TypeError("scope must be an AbiScope or None")
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
class AbiBackendDomainDraft:
    """A per-capability draft: domain, observed evidence, and classification."""

    domain: CapabilityDomain
    scope: AbiScope | None
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    gap_id: ReferenceId | None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        if self.scope is not None and not isinstance(self.scope, AbiScope):
            raise TypeError("scope must be an AbiScope or None")
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
class T1IndependenceAssessment:
    """Requirement 7.4 / 7.5: whether T1 independence is blocked, and why."""

    generated_cpp_dependency: bool
    external_clang_dependency: bool
    dependency_inventory_complete: bool
    accepted_independent_bootstrap: bool
    achieved: bool
    blocking_reasons: tuple[str, ...]

    def __post_init__(self) -> None:
        for name in (
            "generated_cpp_dependency",
            "external_clang_dependency",
            "dependency_inventory_complete",
            "accepted_independent_bootstrap",
            "achieved",
        ):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")
        object.__setattr__(self, "blocking_reasons", tuple(self.blocking_reasons))
        if self.achieved and self.blocking_reasons:
            raise ValueError("an achieved T1 assessment cannot carry blocking reasons")
        if not self.achieved and not self.blocking_reasons:
            raise ValueError("an unachieved T1 assessment must record blocking reasons")


@dataclass(frozen=True, slots=True)
class PrimitiveObjectFinding:
    """Requirement 7.6: the fixed wording bound for the primitive object gate."""

    gate_passed: bool
    wording: str = PRIMITIVE_OBJECT_WORDING
    scope_note: str = PRIMITIVE_OBJECT_SCOPE_NOTE
    forbidden_terms: tuple[str, ...] = PRIMITIVE_OBJECT_FORBIDDEN_TERMS

    def __post_init__(self) -> None:
        if not isinstance(self.gate_passed, bool):
            raise TypeError("gate_passed must be a bool")
        object.__setattr__(self, "forbidden_terms", tuple(self.forbidden_terms))

    def wording_asserts_forbidden(self, text: str) -> tuple[str, ...]:
        """Return any forbidden terms present in ``text`` (empty when compliant)."""

        lowered = text.lower()
        return tuple(term for term in self.forbidden_terms if term in lowered)


@dataclass(frozen=True, slots=True)
class AbiBackendEvaluation:
    """The evaluator output: domain drafts, gaps, and headline classifications."""

    domain_drafts: tuple[AbiBackendDomainDraft, ...]
    gaps: tuple[GapEntry, ...]
    t1_independence: T1IndependenceAssessment
    primitive_object: PrimitiveObjectFinding

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id))))
        if not isinstance(self.t1_independence, T1IndependenceAssessment):
            raise TypeError("t1_independence must be a T1IndependenceAssessment")
        if not isinstance(self.primitive_object, PrimitiveObjectFinding):
            raise TypeError("primitive_object must be a PrimitiveObjectFinding")

    def draft_for(self, capability_id: str) -> AbiBackendDomainDraft | None:
        target = str(capability_id)
        for draft in self.domain_drafts:
            if str(draft.domain.id) == target:
                return draft
        return None

    def draft_for_scope(self, scope: AbiScope) -> AbiBackendDomainDraft | None:
        for draft in self.domain_drafts:
            if draft.scope is scope:
                return draft
        return None

    def gap_for(self, capability_id: str) -> GapEntry | None:
        target = str(capability_id)
        for gap in self.gaps:
            if target in {str(ref) for ref in gap.domain_ids}:
                return gap
        return None

    @property
    def t1_achieved(self) -> bool:
        return self.t1_independence.achieved


# --------------------------------------------------------------------------- #
# The declarative checklist                                                   #
# --------------------------------------------------------------------------- #

ABI_BACKEND_CHECKLIST: tuple[AbiBackendChecklistItem, ...] = (
    # -- Requirement 7.1: hosted C ABI surface ------------------------------ #
    AbiBackendChecklistItem(
        capability_id=StableId("capability-hosted-c-abi"),
        name="Hosted C ABI surface (extern/export, calling convention, layout, versioning)",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.ABI_HOSTED,
        scope=AbiScope.HOSTED_C_ABI,
        markers=_HOSTED_C_ABI_MARKERS,
        authoritative_paths=("spec/interop_c_abi.md", "spec/abi_layout.md"),
        unsatisfied_category=GapCategory.LANGUAGE,
        requirement_refs=("7.1",),
        acceptance_evidence=(
            "A normative hosted C ABI contract covering imported extern contracts, "
            "exported C ABI types, calling conventions, symbol rules, aggregate and "
            "enum layout, alignment, versioning, and cross-language fixtures.",
        ),
        non_claims=(
            "The hosted C ABI is a host-interop surface; it is not a freestanding "
            "compiler, runtime, boot, syscall, driver, or package ABI.",
        ),
        owner_area="Compiler & ABI",
    ),
    # -- Requirement 7.2: freestanding ABI scopes --------------------------- #
    AbiBackendChecklistItem(
        capability_id=StableId("capability-compiler-abi"),
        name="Freestanding compiler ABI",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.COMPILER_ABI,
        markers=_COMPILER_ABI_MARKERS,
        authoritative_paths=("spec/abi_layout.md", "spec/compiler_pipeline.md"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=(
            "A freestanding compiler ABI defined and implemented independently of "
            "the hosted C ABI and the host C++ toolchain.",
        ),
        non_claims=(
            "No freestanding compiler ABI exists; hosted C ABI evidence does not "
            "establish it.",
        ),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-runtime-abi"),
        name="Runtime ABI",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.RUNTIME_ABI,
        markers=_RUNTIME_ABI_MARKERS,
        authoritative_paths=("spec/abi_layout.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=(
            "A freestanding runtime ABI defined and implemented without a hosted "
            "runtime dependency.",
        ),
        non_claims=("No freestanding runtime ABI exists.",),
        owner_area="Freestanding Runtime",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-boot-abi"),
        name="Boot ABI",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.BOOT_ABI,
        markers=_BOOT_ABI_MARKERS,
        authoritative_paths=("docs/universeos/qemu_boot_hello.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=(
            "A boot ABI defined and implemented for a reproducible boot handoff.",
        ),
        non_claims=("No boot ABI exists.",),
        owner_area="Boot & Toolchain",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-syscall-abi"),
        name="Syscall ABI",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        parent_capability_id=_PARENT_T3,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.SYSCALL_ABI,
        markers=_SYSCALL_ABI_MARKERS,
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=(
            "A syscall ABI defined and implemented with a capability boundary.",
        ),
        non_claims=("No syscall ABI exists.",),
        owner_area="Kernel",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-driver-abi"),
        name="Driver ABI",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        parent_capability_id=_PARENT_T3,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.DRIVER_ABI,
        markers=_DRIVER_ABI_MARKERS,
        authoritative_paths=("docs/universeos/kernel_boundary.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=("A driver ABI defined and implemented for a driver model.",),
        non_claims=("No driver ABI exists.",),
        owner_area="Drivers & Hardware",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-package-abi"),
        name="Package ABI",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.ABI_FREESTANDING,
        scope=AbiScope.PACKAGE_ABI,
        markers=_PACKAGE_ABI_MARKERS,
        authoritative_paths=("spec/abi_layout.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.2",),
        acceptance_evidence=(
            "A package ABI defined and implemented for cross-package binary "
            "compatibility.",
        ),
        non_claims=("No package ABI exists.",),
        owner_area="Compiler & ABI",
    ),
    # -- Requirement 7.3: compiler pipeline stages -------------------------- #
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-frontend"),
        name="Frontend completeness",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_FRONTEND_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md", "frontend/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=(
            "Frontend (lexer/parser/typechecker) implementation evidence for the "
            "supported language surface.",
        ),
        non_claims=(),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-nir-cfg"),
        name="NIR and control-flow graph",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_NIR_CFG_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md", "nir/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=("NIR and CFG lowering implementation evidence.",),
        non_claims=(),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-analyses"),
        name="Analyses",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_ANALYSES_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md", "passes/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=("Analysis-pass implementation evidence.",),
        non_claims=(),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-optimization"),
        name="Optimization",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_OPTIMIZATION_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md", "passes/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=("Optimization-pass implementation evidence.",),
        non_claims=(),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-incremental"),
        name="Incremental compilation",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_INCREMENTAL_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=("Incremental compilation implementation evidence.",),
        non_claims=("Incremental compilation is not established as implemented.",),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-debug-info"),
        name="Debug information",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED,
        scope=None,
        markers=_DEBUG_INFO_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=("Debug-information generation implementation evidence.",),
        non_claims=(),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-native-codegen"),
        name="Native code generation (independent backend)",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE,
        scope=None,
        markers=_NATIVE_CODEGEN_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md", "codegen/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=(
            "A Nebula-owned native code generator that does not depend on generated "
            "C++ or an external clang toolchain.",
        ),
        non_claims=(
            "No direct native backend exists; production code generation targets "
            "generated C++ compiled by an external host toolchain.",
        ),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-assembler"),
        name="Assembler integration",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE,
        scope=None,
        markers=_ASSEMBLER_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=(
            "Assembler integration that emits machine code without a host toolchain "
            "dependency.",
        ),
        non_claims=("No independent assembler integration exists.",),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-linker"),
        name="Linker integration",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE,
        scope=None,
        markers=_LINKER_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=(
            "Linker integration that produces linked images without a host toolchain "
            "dependency.",
        ),
        non_claims=(
            "No independent linker integration exists; only clang-backed relocatable "
            "objects are emitted.",
        ),
        owner_area="Compiler & ABI",
    ),
    AbiBackendChecklistItem(
        capability_id=StableId("capability-pipeline-bootstrap"),
        name="Bootstrap reproducibility",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE,
        scope=None,
        markers=_BOOTSTRAP_MARKERS,
        authoritative_paths=("spec/compiler_pipeline.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("7.3",),
        acceptance_evidence=(
            "An accepted, reproducible independent bootstrap path with no generated "
            "C++ or external clang dependency.",
        ),
        non_claims=(
            "No accepted independent bootstrap path exists; the compiler does not "
            "self-host without the host toolchain.",
        ),
        owner_area="Compiler & ABI",
    ),
)


# --------------------------------------------------------------------------- #
# The evaluator                                                               #
# --------------------------------------------------------------------------- #


class AbiBackendEvaluator:
    """Evaluate Requirement 7.1-7.6 FFI/ABI, compilation, linking, and backend gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> AbiBackendEvaluation:
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

        drafts: list[AbiBackendDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in ABI_BACKEND_CHECKLIST:
            matched = tuple(
                record for record in records if _contains(_record_text(record), item.markers)
            )
            draft, gap = self._assess_item(item, matched, present_permitted)
            drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        t1 = self._assess_t1_independence(records, present_permitted)
        primitive = self._assess_primitive_object(records)
        if not t1.achieved:
            gaps.append(self._build_t1_gap(t1))

        return AbiBackendEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
            t1_independence=t1,
            primitive_object=primitive,
        )

    # -- per-capability assessment --------------------------------------- #

    def _assess_item(
        self,
        item: AbiBackendChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> tuple[AbiBackendDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)
        satisfied = any(
            self._record_satisfies(item, record, present_permitted) for record in matched
        )

        description = (
            f"Requirement {', '.join(item.requirement_refs)} capability assessed by "
            "the ABI/backend evaluator."
        )
        gap: GapEntry | None = None
        if not satisfied:
            gap = self._build_gap(item, observed_status)

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=description,
            mandatory_for_target=True,
            parent_id=reference(item.parent_capability_id),
            evidence_ids=supporting_ids,
            gap_ids=(reference(gap.id),) if gap is not None else (),
        )

        draft = AbiBackendDomainDraft(
            domain=domain,
            scope=item.scope,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            limitations=tuple(item.non_claims),
            satisfied=satisfied,
            gap_id=reference(gap.id) if gap is not None else None,
        )
        return draft, gap

    @staticmethod
    def _record_satisfies(
        item: AbiBackendChecklistItem,
        record: EvidenceRecord,
        present_permitted: Mapping[str, bool],
    ) -> bool:
        is_spec = record.evidence_kind is EvidenceKind.SPECIFICATION
        current = _has_current_implementation(record) and present_permitted.get(
            str(record.id), False
        )
        if item.kind is AbiBackendCapabilityKind.ABI_HOSTED:
            # Requirement 7.1: a hosted C ABI is satisfied by a normative spec or a
            # current implementation of the hosted interop surface.
            return is_spec or current
        if item.kind is AbiBackendCapabilityKind.ABI_FREESTANDING:
            # Requirement 7.2: a freestanding ABI needs a current in-scope
            # implementation; documentation alone (or hosted evidence, which never
            # matches these markers) is insufficient.
            return current
        if item.kind is AbiBackendCapabilityKind.PIPELINE_IMPLEMENTED:
            return is_spec or current
        # PIPELINE_INDEPENDENCE (7.3): clang-backed / generated-C++ / primitive
        # relocatable-object evidence can never satisfy an independence stage.
        return current and not _is_clang_or_primitive_backed(record)

    def _build_gap(
        self, item: AbiBackendChecklistItem, observed_status: EvidenceStatus
    ) -> GapEntry:
        primary = item.unsatisfied_category
        secondary: list[GapCategory] = []
        if primary is not GapCategory.VERIFICATION and observed_status in _IMPLEMENTED_STATUSES:
            secondary.append(GapCategory.VERIFICATION)

        source = item.authoritative_paths[0]
        observed_fact = (
            f"No normative, in-scope, independent implementation was found for "
            f"{item.name}; strongest observed evidence status is "
            f"{observed_status.value} (authoritative source {source})."
        )
        recommendation = (
            f"Define and implement {item.name} within its own scope; hosted C ABI, "
            "generated C++, clang-backed, or primitive relocatable-object evidence "
            "cannot satisfy it."
        )
        return GapEntry(
            id=stable_id("gap", "abi-backend", str(item.capability_id)),
            title=f"{item.name} gap",
            primary_category=primary,
            secondary_categories=tuple(secondary),
            domain_ids=(reference(item.capability_id),),
            current_status=observed_status,
            target_level=item.target_level,
            severity=self._severity(item),
            dependencies=(),
            acceptance_evidence=tuple(item.acceptance_evidence),
            recommended_owner_area=item.owner_area,
            dependency_criticality=self._dependency_criticality(item),
            safety_impact=0,
            claim_risk=2 if item.kind is AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE else 1,
            target_unblock_value=self._dependency_criticality(item),
            observed_fact=observed_fact,
            recommendation=recommendation,
        )

    @staticmethod
    def _severity(item: AbiBackendChecklistItem) -> Severity:
        if item.kind in {
            AbiBackendCapabilityKind.ABI_FREESTANDING,
            AbiBackendCapabilityKind.PIPELINE_INDEPENDENCE,
        }:
            return Severity.HIGH
        return Severity.MEDIUM

    @staticmethod
    def _dependency_criticality(item: AbiBackendChecklistItem) -> int:
        return 3 if item.target_level is not TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM else 2

    # -- T1 independence (Requirements 7.4, 7.5) ------------------------- #

    def _assess_t1_independence(
        self,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> T1IndependenceAssessment:
        generated_cpp = any(
            _contains(_record_text(record), _GENERATED_CPP_MARKERS) for record in records
        )
        external_clang = any(
            _contains(_record_text(record), _EXTERNAL_CLANG_MARKERS) for record in records
        )
        inventory_complete = any(
            _contains(_record_text(record), _INVENTORY_COMPLETE_MARKERS)
            and _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in records
        )
        # An accepted independent bootstrap is asserted by its own explicit
        # markers plus current implementation evidence. A record may also mention
        # a legacy generated-C++ path without disqualifying the bootstrap claim;
        # the generated-C++/clang dependency is tracked separately above.
        accepted_bootstrap = any(
            _contains(_record_text(record), _ACCEPTED_BOOTSTRAP_MARKERS)
            and _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in records
        )

        reasons: list[str] = []
        # Requirement 7.4: an incomplete production dependency inventory blocks T1.
        if not inventory_complete:
            reasons.append(
                "The production compiler dependency inventory is incomplete, so "
                "T1_Independent_Language_Platform is unachieved (Requirement 7.4)."
            )
        # Requirement 7.5: generated C++ or external clang as a production
        # dependency without an accepted independent bootstrap blocks T1.
        if (generated_cpp or external_clang) and not accepted_bootstrap:
            reasons.append(
                "Generated C++ or external clang remains a production dependency "
                "without an accepted independent bootstrap path, so "
                "T1_Independent_Language_Platform is unachieved (Requirement 7.5)."
            )

        return T1IndependenceAssessment(
            generated_cpp_dependency=generated_cpp,
            external_clang_dependency=external_clang,
            dependency_inventory_complete=inventory_complete,
            accepted_independent_bootstrap=accepted_bootstrap,
            achieved=not reasons,
            blocking_reasons=tuple(reasons),
        )

    @staticmethod
    def _build_t1_gap(t1: T1IndependenceAssessment) -> GapEntry:
        observed_fact = " ".join(t1.blocking_reasons)
        return GapEntry(
            id=stable_id("gap", "abi-backend", "t1-independence"),
            title="T1 independent language platform is unachieved",
            primary_category=GapCategory.IMPLEMENTATION,
            secondary_categories=(),
            domain_ids=(reference(StableId("capability-t1-language-platform")),),
            current_status=EvidenceStatus.EXPERIMENTAL,
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            severity=Severity.CRITICAL,
            dependencies=(),
            acceptance_evidence=(
                "A complete production compiler dependency inventory and an accepted "
                "independent backend/bootstrap path with no generated-C++ or "
                "external-clang production dependency.",
            ),
            recommended_owner_area="Compiler & ABI",
            dependency_criticality=3,
            safety_impact=0,
            claim_risk=3,
            target_unblock_value=3,
            observed_fact=observed_fact,
            recommendation=(
                "Complete the production dependency inventory and land an accepted "
                "independent backend/bootstrap path before claiming T1 independence."
            ),
        )

    # -- Primitive object wording (Requirement 7.6) --------------------- #

    @staticmethod
    def _assess_primitive_object(
        records: tuple[EvidenceRecord, ...],
    ) -> PrimitiveObjectFinding:
        gate_passed = any(
            _contains(_record_text(record), _PRIMITIVE_OBJECT_MARKERS)
            and (
                record.verification_state is VerificationState.VALIDATED
                or record.evidence_kind is EvidenceKind.TEST_EXECUTION
            )
            for record in records
        )
        return PrimitiveObjectFinding(gate_passed=gate_passed)


def evaluate_abi_backend(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> AbiBackendEvaluation:
    """Convenience API for the ABI/backend evaluator (Requirement 7.1-7.6)."""

    return AbiBackendEvaluator().evaluate(bundle, guarded)
