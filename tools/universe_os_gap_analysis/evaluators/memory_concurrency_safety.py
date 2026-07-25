"""Memory, ownership, concurrency, and unsafe/low-level evaluator (Task 5.2).

This declarative evaluator covers Requirement 6 (memory, ownership, concurrency,
and safety gaps). It builds on the Task 4.1 :class:`EvidenceBundle` and the Task
4.3 Claim Guard: the guard tells the evaluator which claims may be stated in the
present tense and which are hosted/scoped (so they cannot promote OS-substrate
maturity), while the bundle supplies the full claim text and the disclosed
``limitations`` that the evaluator surfaces on the gaps it emits.

The evaluator is intentionally self-contained. Task 5.1 (language & type-system
evaluator) runs in parallel and may introduce a shared evaluator base; this
module defines only module-local draft/result types so the two can coexist and a
later task can reconcile them without either duplicating or fighting over a base
class. Nothing here mutates evidence or edits sibling evaluator files.

Requirement 6 coverage:

* **6.1 storage semantics.** Stack/region/heap/static storage, promotion,
  initialization, destruction, allocator failure, raw memory, and
  resource-lifetime semantics are assessed as the memory storage-model domain.
* **6.2 ownership.** Current Rep x Owner inference and borrow *assistance* are
  distinguished from a *normative* move/borrow/lifetime/aliasing model. Assistance
  alone never satisfies the normative ownership capability (design Property 11).
* **6.3 concurrency.** Threads, tasks, actors, structured concurrency,
  interruption, atomics, memory ordering, data-race prevention, interrupt safety,
  and synchronization primitives are assessed as the concurrency-model domain.
* **6.4 hosted async.** When current async depends on the hosted cooperative
  runtime (and no scheduler-independent implementation exists), the
  scheduler-independent concurrency capability is an ``Implementation_Gap``.
* **6.5 unsafe / low level.** Unsafe blocks/functions, FFI boundaries, raw
  pointers, volatile access, MMIO, intrinsics, inline assembly, and privilege
  transitions are assessed across an unsafe/FFI boundary domain and a
  hardware-low-level-primitive domain.
* **6.6 exclusions.** Every opaque/dynamic/FFI/unsafe exclusion disclosed on a
  matched Evidence_Record is carried into the related gap's limitations.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from ..catalog import CapabilityBoundary
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
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent. The "normative" vocabulary is deliberately narrow: it names a
# formal/normative model rather than an implementation assistance heuristic, so
# borrow *assistance* can never be mistaken for a normative model (Requirement
# 6.2 / Property 11).

_NORMATIVE_MODEL_MARKERS: tuple[str, ...] = (
    "normative",
    "formal model",
    "formally specified",
    "formal semantics",
    "borrow checker",
    "move semantics model",
    "lifetime model",
    "aliasing model",
    "alias model",
    "memory model",
    "happens-before",
    "data-race freedom",
    "data race freedom",
)

_OWNERSHIP_ASSISTANCE_MARKERS: tuple[str, ...] = (
    "rep x owner",
    "rep × owner",
    "rep×owner",
    "rep-owner",
    "rep owner",
    "borrow assist",
    "borrow assistance",
    "ownership inference",
    "exclusivity assist",
    "conservative borrow",
    "conservative exclusivity",
)

_SCHEDULER_INDEPENDENT_MARKERS: tuple[str, ...] = (
    "scheduler-independent",
    "scheduler independent",
    "preemptive scheduler",
    "preemptive scheduling",
    "kernel scheduler",
    "independent scheduler",
    "native thread scheduler",
    "os thread scheduler",
)

_HOSTED_COOPERATIVE_MARKERS: tuple[str, ...] = (
    "hosted cooperative",
    "cooperative runtime",
    "cooperative async",
    "hosted async",
    "single-threaded cooperative",
    "single threaded cooperative",
    "cooperative scheduler",
    "hosted runtime async",
)

# Safety-guarantee exclusions (Requirement 6.6). These are surfaced onto gap
# limitations. Detection here is a light-weight complement to the Task 4.4 trust
# auditor: the auditor fails closed when a record leaves an exclusion undisclosed;
# this evaluator propagates the disclosed exclusions onto the affected gaps.
_EXCLUSION_MARKERS: Mapping[str, tuple[str, ...]] = {
    "opaque": ("opaque",),
    "dynamic": ("dynamic dispatch", "dynamic boundary", "dynamic"),
    "ffi": ("ffi", "foreign function", "foreign-function"),
    "unsafe": ("unsafe",),
}


class SafetyModelStrength(ClosedStrEnum):
    """How strong the ownership/borrow evidence is (Requirement 6.2)."""

    #: No move/borrow/ownership evidence at all.
    ABSENT = "Absent"
    #: Only Rep x Owner inference / borrow assistance; not a normative model.
    ASSISTANCE_ONLY = "AssistanceOnly"
    #: A normative move/borrow/lifetime/aliasing model with current evidence.
    NORMATIVE = "Normative"


class ConcurrencyModelStrength(ClosedStrEnum):
    """How strong the scheduler-independence evidence is (Requirement 6.4)."""

    #: No concurrency implementation evidence at all.
    ABSENT = "Absent"
    #: Async exists but depends on the hosted cooperative runtime only.
    HOSTED_COOPERATIVE_ONLY = "HostedCooperativeOnly"
    #: A scheduler-independent concurrency implementation exists.
    SCHEDULER_INDEPENDENT = "SchedulerIndependent"


class CapabilityKind(Enum):
    """The classification rule a checklist item applies."""

    #: Requires a normative memory/ownership model; assistance never satisfies.
    NORMATIVE_MODEL = "normative_model"
    #: Requires a scheduler-independent concurrency implementation.
    SCHEDULER_INDEPENDENT = "scheduler_independent"
    #: Requires normative/implementation evidence for a low-level boundary.
    LOW_LEVEL_BOUNDARY = "low_level_boundary"


@dataclass(frozen=True, slots=True)
class MemorySafetyChecklistItem:
    """One declarative capability check for the memory/concurrency/safety domain."""

    capability_id: StableId
    name: str
    target_level: TargetLevel
    parent_capability_id: StableId
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
        object.__setattr__(self, "parent_capability_id", StableId(self.parent_capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
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


@dataclass(frozen=True, slots=True, kw_only=True)
class MemorySafetyDomainDraft:
    """A per-capability draft: domain, observed evidence, and classification."""

    domain: CapabilityDomain
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    gap_id: ReferenceId | None
    safety_model_strength: SafetyModelStrength | None = None
    concurrency_model_strength: ConcurrencyModelStrength | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
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
class MemorySafetyEvaluation:
    """The evaluator output: domain drafts, gaps, and headline classifications."""

    domain_drafts: tuple[MemorySafetyDomainDraft, ...]
    gaps: tuple[GapEntry, ...]
    safety_model_strength: SafetyModelStrength
    concurrency_model_strength: ConcurrencyModelStrength

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(
            self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id)))
        )

    def draft_for(self, capability_id: str) -> MemorySafetyDomainDraft | None:
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


# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS``
# that these fine-grained domains roll up into, for later reconciliation.
_PARENT_T1 = StableId("capability-t1-language-platform")
_PARENT_T2 = StableId("capability-t2-freestanding-substrate")


MEMORY_SAFETY_CHECKLIST: tuple[MemorySafetyChecklistItem, ...] = (
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-memory-storage-model"),
        name="Memory storage, lifetime, and allocation-failure semantics",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.NORMATIVE_MODEL,
        markers=(
            "stack",
            "region",
            "heap",
            "static storage",
            "storage duration",
            "promotion",
            "initialization",
            "destruction",
            "allocator failure",
            "allocation failure",
            "raw memory",
            "resource lifetime",
            "resource-lifetime",
        ),
        authoritative_paths=("spec/region_semantics.md", "spec/language_core.md"),
        unsatisfied_category=GapCategory.LANGUAGE,
        requirement_refs=("6.1",),
        acceptance_evidence=(
            "A normative specification of stack/region/heap/static storage, "
            "promotion, initialization, destruction, allocator-failure, raw-memory, "
            "and resource-lifetime semantics with direct implementation evidence.",
        ),
        non_claims=(
            "Explicit region semantics do not yet constitute a normative, "
            "target-layout-complete storage and resource-lifetime model.",
        ),
        owner_area="Language & Safety",
    ),
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-ownership-borrow-model"),
        name="Normative move, borrow, lifetime, and aliasing model",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.NORMATIVE_MODEL,
        markers=(
            "move",
            "borrow",
            "lifetime",
            "aliasing",
            "alias",
            "ownership",
            "exclusivity",
            "rep x owner",
            "rep × owner",
        ),
        authoritative_paths=("spec/rep_owner_model.md", "spec/safety_contract.md"),
        unsatisfied_category=GapCategory.LANGUAGE,
        requirement_refs=("6.2",),
        acceptance_evidence=(
            "A normative move/borrow/lifetime/aliasing model, not merely Rep x Owner "
            "inference or conservative borrow assistance, with direct implementation "
            "evidence.",
        ),
        non_claims=(
            "Current Rep x Owner inference and borrow assistance do not constitute a "
            "normative move/borrow/lifetime/aliasing model.",
        ),
        owner_area="Language & Safety",
    ),
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-concurrency-model"),
        name="Concurrency, atomics, ordering, and data-race model",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.NORMATIVE_MODEL,
        markers=(
            "thread",
            "task",
            "actor",
            "structured concurrency",
            "interruption",
            "atomic",
            "memory ordering",
            "data race",
            "data-race",
            "interrupt safety",
            "synchronization",
            "mutex",
        ),
        authoritative_paths=("spec/language_core.md", "spec/safety_contract.md"),
        unsatisfied_category=GapCategory.LANGUAGE,
        requirement_refs=("6.3",),
        acceptance_evidence=(
            "A normative concurrency model covering threads, tasks, actors, "
            "structured concurrency, interruption, atomics, memory ordering, "
            "data-race freedom, interrupt safety, and synchronization primitives.",
        ),
        non_claims=(
            "No normative atomics, memory-ordering, or data-race-freedom model is "
            "defined; interrupt safety and synchronization primitives are unspecified.",
        ),
        owner_area="Language & Safety",
    ),
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-scheduler-independent-concurrency"),
        name="Scheduler-independent concurrency implementation",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=CapabilityKind.SCHEDULER_INDEPENDENT,
        markers=(
            "async",
            "await",
            "future",
            "task",
            "scheduler",
            "cooperative",
            "concurrency runtime",
        ),
        authoritative_paths=("runtime/nebula_runtime.hpp", "spec/language_core.md"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("6.4",),
        acceptance_evidence=(
            "A scheduler-independent concurrency implementation that does not depend "
            "on the hosted cooperative runtime.",
        ),
        non_claims=(
            "Current async depends on the hosted single-threaded cooperative runtime; "
            "there is no scheduler-independent concurrency implementation.",
        ),
        owner_area="Runtime & Concurrency",
    ),
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-unsafe-ffi-boundary"),
        name="Unsafe, FFI, and raw-pointer boundary semantics",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.LOW_LEVEL_BOUNDARY,
        markers=(
            "unsafe block",
            "unsafe function",
            "unsafe fn",
            "unsafe",
            "ffi",
            "foreign function",
            "raw pointer",
            "extern",
            "privilege transition",
        ),
        authoritative_paths=("spec/safety_contract.md", "spec/interop_c_abi.md"),
        unsatisfied_category=GapCategory.LANGUAGE,
        requirement_refs=("6.5", "6.6"),
        acceptance_evidence=(
            "Normative unsafe-block/function, FFI-boundary, raw-pointer, and "
            "privilege-transition semantics with direct implementation evidence, "
            "including every disclosed opaque/dynamic/FFI/unsafe exclusion.",
        ),
        non_claims=(
            "Unsafe and FFI boundaries lack a normative low-level semantic contract; "
            "raw-pointer and privilege-transition semantics are unspecified.",
        ),
        owner_area="Language & Safety",
    ),
    MemorySafetyChecklistItem(
        capability_id=StableId("capability-hardware-lowlevel-primitives"),
        name="Volatile, MMIO, intrinsic, and inline-assembly primitives",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=CapabilityKind.LOW_LEVEL_BOUNDARY,
        markers=(
            "volatile",
            "mmio",
            "memory-mapped",
            "memory mapped",
            "intrinsic",
            "inline assembly",
            "inline asm",
            "privileged instruction",
            "privilege level",
        ),
        authoritative_paths=("spec/abi_layout.md", "spec/safety_contract.md"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("6.5",),
        acceptance_evidence=(
            "Implemented volatile access, MMIO, intrinsics, inline assembly, and "
            "privilege-transition primitives suitable for hardware-safe code.",
        ),
        non_claims=(
            "Volatile access, MMIO, intrinsics, inline assembly, and privilege "
            "transitions are documented gaps with no implementation evidence.",
        ),
        owner_area="Freestanding Runtime",
    ),
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

# Evidence kinds that count as direct implementation for a "current" model.
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


def _has_current_implementation(record: EvidenceRecord) -> bool:
    return (
        record.status in _IMPLEMENTED_STATUSES
        and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
    )


class MemoryConcurrencySafetyEvaluator:
    """Evaluate Requirement 6 memory, ownership, concurrency, and safety gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> MemorySafetyEvaluation:
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

        safety_strength = self._safety_model_strength(records, present_permitted)
        concurrency_strength = self._concurrency_model_strength(records, present_permitted)

        drafts: list[MemorySafetyDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in MEMORY_SAFETY_CHECKLIST:
            matched = tuple(
                record
                for record in records
                if _contains(_record_text(record), item.markers)
            )
            draft, gap = self._assess_item(
                item,
                matched,
                present_permitted,
                safety_strength,
                concurrency_strength,
            )
            drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        return MemorySafetyEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
            safety_model_strength=safety_strength,
            concurrency_model_strength=concurrency_strength,
        )

    # -- headline classifications ---------------------------------------- #

    def _safety_model_strength(
        self,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> SafetyModelStrength:
        """Distinguish a normative model from Rep x Owner / borrow assistance."""

        has_normative = False
        has_assistance = False
        for record in records:
            text = _record_text(record)
            ownership_related = _contains(
                text, ("ownership", "borrow", "move", "lifetime", "alias", "rep")
            )
            if not ownership_related:
                continue
            if _contains(text, _OWNERSHIP_ASSISTANCE_MARKERS):
                has_assistance = True
            if _contains(text, _NORMATIVE_MODEL_MARKERS) and self._current_or_specified(
                record, present_permitted
            ):
                has_normative = True
        if has_normative:
            return SafetyModelStrength.NORMATIVE
        if has_assistance:
            return SafetyModelStrength.ASSISTANCE_ONLY
        return SafetyModelStrength.ABSENT

    def _concurrency_model_strength(
        self,
        records: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> ConcurrencyModelStrength:
        """Detect hosted cooperative async vs a scheduler-independent implementation."""

        has_scheduler_independent = False
        has_hosted_cooperative = False
        for record in records:
            text = _record_text(record)
            if _contains(text, _SCHEDULER_INDEPENDENT_MARKERS) and _has_current_implementation(
                record
            ) and present_permitted.get(str(record.id), False):
                has_scheduler_independent = True
            if _contains(text, _HOSTED_COOPERATIVE_MARKERS):
                has_hosted_cooperative = True
        if has_scheduler_independent:
            return ConcurrencyModelStrength.SCHEDULER_INDEPENDENT
        if has_hosted_cooperative:
            return ConcurrencyModelStrength.HOSTED_COOPERATIVE_ONLY
        return ConcurrencyModelStrength.ABSENT

    @staticmethod
    def _current_or_specified(
        record: EvidenceRecord, present_permitted: Mapping[str, bool]
    ) -> bool:
        """A normative model counts when it is current implementation or a spec."""

        if record.evidence_kind is EvidenceKind.SPECIFICATION:
            return True
        return _has_current_implementation(record) and present_permitted.get(
            str(record.id), False
        )

    # -- per-capability assessment --------------------------------------- #

    def _assess_item(
        self,
        item: MemorySafetyChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
        safety_strength: SafetyModelStrength,
        concurrency_strength: ConcurrencyModelStrength,
    ) -> tuple[MemorySafetyDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)
        # Requirement 6.6: carry every disclosed exclusion into the gap limitations.
        exclusion_limitations = self._exclusion_limitations(matched)

        satisfied, safety_flag, concurrency_flag = self._is_satisfied(
            item, matched, present_permitted, safety_strength, concurrency_strength
        )

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} capability assessed by "
                "the memory/ownership/concurrency/unsafe evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(item.parent_capability_id),
            evidence_ids=supporting_ids,
        )

        gap: GapEntry | None = None
        if not satisfied:
            gap = self._build_gap(
                item, matched, observed_status, exclusion_limitations, concurrency_strength
            )
            domain = CapabilityDomain(
                id=domain.id,
                name=domain.name,
                target_level=domain.target_level,
                description=domain.description,
                mandatory_for_target=domain.mandatory_for_target,
                parent_id=item.parent_capability_id,
                evidence_ids=supporting_ids,
                gap_ids=(reference(gap.id),),
            )

        draft = MemorySafetyDomainDraft(
            domain=domain,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            limitations=tuple(sorted(set(item.non_claims) | set(exclusion_limitations))),
            satisfied=satisfied,
            gap_id=reference(gap.id) if gap is not None else None,
            safety_model_strength=safety_flag,
            concurrency_model_strength=concurrency_flag,
        )
        return draft, gap

    def _is_satisfied(
        self,
        item: MemorySafetyChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
        safety_strength: SafetyModelStrength,
        concurrency_strength: ConcurrencyModelStrength,
    ) -> tuple[bool, SafetyModelStrength | None, ConcurrencyModelStrength | None]:
        if item.kind is CapabilityKind.SCHEDULER_INDEPENDENT:
            # Requirement 6.4: only a scheduler-independent implementation
            # satisfies; hosted cooperative async never does.
            satisfied = concurrency_strength is ConcurrencyModelStrength.SCHEDULER_INDEPENDENT
            return satisfied, None, concurrency_strength
        if item.kind is CapabilityKind.NORMATIVE_MODEL:
            if item.capability_id == StableId("capability-ownership-borrow-model"):
                # Requirement 6.2 / Property 11: assistance alone never satisfies.
                satisfied = safety_strength is SafetyModelStrength.NORMATIVE
                return satisfied, safety_strength, None
            # Other normative-model capabilities require a normative spec or a
            # current normative implementation among matched records.
            satisfied = any(
                _contains(_record_text(record), _NORMATIVE_MODEL_MARKERS)
                and self._current_or_specified(record, present_permitted)
                for record in matched
            )
            return satisfied, None, None
        # LOW_LEVEL_BOUNDARY: satisfied only with a normative-and-implemented
        # boundary contract. Documented-but-unimplemented boundaries are gaps.
        satisfied = any(
            _contains(_record_text(record), _NORMATIVE_MODEL_MARKERS)
            and _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in matched
        )
        return satisfied, None, None

    @staticmethod
    def _exclusion_limitations(
        matched: tuple[EvidenceRecord, ...],
    ) -> tuple[str, ...]:
        """Collect disclosed opaque/dynamic/FFI/unsafe exclusions (Requirement 6.6)."""

        limitations: set[str] = set()
        for record in matched:
            disclosed = "\n".join((*record.limitations, *record.trust_assumptions)).lower()
            for category, markers in _EXCLUSION_MARKERS.items():
                if _contains(disclosed, markers):
                    # Preserve the record's own disclosed limitation text verbatim
                    # so the gap carries the actual exclusion wording.
                    for limitation in record.limitations:
                        if _contains(limitation.lower(), markers):
                            limitations.add(limitation)
        return tuple(sorted(limitations))

    def _build_gap(
        self,
        item: MemorySafetyChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
        exclusion_limitations: tuple[str, ...],
        concurrency_strength: ConcurrencyModelStrength,
    ) -> GapEntry:
        primary = item.unsatisfied_category
        secondary: list[GapCategory] = []
        # A capability with implementation/assistance evidence but no normative
        # contract also carries semantic-stability verification risk.
        if primary is not GapCategory.VERIFICATION and observed_status in _IMPLEMENTED_STATUSES:
            secondary.append(GapCategory.VERIFICATION)

        observed_fact, recommendation = self._narrative(
            item, observed_status, concurrency_strength
        )
        acceptance = tuple(item.acceptance_evidence)
        if exclusion_limitations:
            acceptance = acceptance + (
                "Disclosed exclusions must remain recorded: "
                + "; ".join(exclusion_limitations),
            )

        return GapEntry(
            id=stable_id("gap", "memory-safety", str(item.capability_id)),
            title=f"{item.name} gap",
            primary_category=primary,
            secondary_categories=tuple(secondary),
            domain_ids=(reference(item.capability_id),),
            current_status=observed_status,
            target_level=item.target_level,
            severity=self._severity(item),
            dependencies=(),
            acceptance_evidence=acceptance,
            recommended_owner_area=item.owner_area,
            dependency_criticality=self._dependency_criticality(item),
            safety_impact=self._safety_impact(item),
            claim_risk=1 if observed_status in _IMPLEMENTED_STATUSES else 0,
            target_unblock_value=self._dependency_criticality(item),
            observed_fact=observed_fact,
            recommendation=recommendation,
        )

    @staticmethod
    def _narrative(
        item: MemorySafetyChecklistItem,
        observed_status: EvidenceStatus,
        concurrency_strength: ConcurrencyModelStrength,
    ) -> tuple[str, str]:
        source = item.authoritative_paths[0]
        if item.capability_id == StableId("capability-ownership-borrow-model"):
            observed = (
                "Current evidence provides Rep x Owner inference and conservative "
                "borrow assistance, not a normative move/borrow/lifetime/aliasing "
                f"model (authoritative source {source})."
            )
            recommendation = (
                "Specify and implement a normative move/borrow/lifetime/aliasing "
                "model; borrow assistance cannot satisfy the normative safety capability."
            )
            return observed, recommendation
        if item.kind is CapabilityKind.SCHEDULER_INDEPENDENT:
            if concurrency_strength is ConcurrencyModelStrength.HOSTED_COOPERATIVE_ONLY:
                observed = (
                    "Current async depends on the hosted single-threaded cooperative "
                    f"runtime (authoritative source {source}); no scheduler-independent "
                    "concurrency implementation exists."
                )
            else:
                observed = (
                    "No scheduler-independent concurrency implementation exists "
                    f"(authoritative source {source})."
                )
            recommendation = (
                "Implement scheduler-independent concurrency that does not depend on "
                "the hosted cooperative runtime."
            )
            return observed, recommendation
        observed = (
            f"No normative, implemented contract was found for this capability; "
            f"strongest observed evidence status is {observed_status.value} "
            f"(authoritative source {source})."
        )
        recommendation = (
            "Define the normative semantics and provide direct implementation "
            "evidence before relying on this capability for system-level code."
        )
        return observed, recommendation

    @staticmethod
    def _severity(item: MemorySafetyChecklistItem) -> Severity:
        if item.kind is CapabilityKind.SCHEDULER_INDEPENDENT:
            return Severity.HIGH
        if item.capability_id in {
            StableId("capability-ownership-borrow-model"),
            StableId("capability-unsafe-ffi-boundary"),
        }:
            return Severity.HIGH
        return Severity.MEDIUM

    @staticmethod
    def _dependency_criticality(item: MemorySafetyChecklistItem) -> int:
        # OS-substrate (T2+) low-level capabilities gate more downstream work.
        return 3 if item.target_level is not TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM else 2

    @staticmethod
    def _safety_impact(item: MemorySafetyChecklistItem) -> int:
        safety_critical = {
            StableId("capability-ownership-borrow-model"),
            StableId("capability-unsafe-ffi-boundary"),
            StableId("capability-concurrency-model"),
            StableId("capability-hardware-lowlevel-primitives"),
        }
        return 3 if item.capability_id in safety_critical else 2


def evaluate_memory_concurrency_safety(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> MemorySafetyEvaluation:
    """Convenience API for the memory/ownership/concurrency/unsafe evaluator."""

    return MemoryConcurrencySafetyEvaluator().evaluate(bundle, guarded)
