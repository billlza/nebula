"""Gap priority ranking and the parallel pre-kernel/post-boot roadmap (Task 9.2).

This module consumes the validated, one-primary-category gap register produced
by Task 9.1 (:mod:`~tools.universe_os_gap_analysis.gap_register`) and turns it
into the two artifacts the assessment report needs downstream (Task 11):

* a **deterministic priority ranking** of every gap, and
* the **parallel Hard-Gate roadmap** with its pre-kernel and post-boot lanes,
  explicit parallel workstreams, and explicit join gates.

Ranking (Requirement 12.4). Gaps are ordered by a *strict lexicographic*
comparison over the tuple::

    (dependency_criticality, safety_impact, claim_risk, target_unblock_value, stable_id)

The four heterogeneous priority dimensions are compared **independently, most
urgent first**; they are *never* summed, averaged, or otherwise combined into a
single scalar (there is no meaningful common unit across dependency criticality,
safety impact, claim risk, and target-unblock value). The stable identifier is
the final, always-distinct tie-breaker, so the ordering is a strict total order
that is completely independent of the order gaps were collected in.

Roadmap (Requirements 12.5-12.7, 15.7). The roadmap is expressed as two
independent gate *lanes*:

* the **pre-kernel** lane -- low-level language soundness, freestanding system
  ABI, independent backend/bootstrap, freestanding core/runtime, complete boot
  toolchain, the primitive ET_REL object gate, the deterministic linked ELF, the
  boot media, and the QEMU serial execution proof (Requirement 12.5); and
* the **post-boot** lane -- memory management/MMU, interrupts, scheduler with
  syscall/capability joins, drivers/DMA, process isolation with userspace,
  storage with networking, and update/recovery with the product shell, each held
  behind its own gate after the boot proof (Requirement 12.6).

Workstreams that can progress independently are marked as **parallel branches**
and every set of parallel branches converges on an **explicit join gate**
(Requirement 12.7): the deterministic linked ELF joins the backend/runtime,
boot-toolchain, and primitive-object branches; the scheduler/syscall gate joins
the memory and interrupt branches; and the update/recovery/shell gate joins the
storage/networking and isolation/userspace branches. The shortest
evidence-backed path (Requirement 15.7) is exactly this dependency ordering, not
a schedule or effort estimate.

Every workstream keeps its **observed current fact** separate from its
**recommendation** (Requirement 14.7); the roadmap exposes the two as distinct
projections and never blends them.

Scope boundaries. This module is read-only and additive: it ranks and arranges
gaps and gates but never mutates a gap, upgrades a status, assigns maturity
(Task 8), or edits any evaluator or product module. It fails closed with an
``RMP-*`` code on any structural problem so an inconsistent roadmap can never be
published.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Iterable, Mapping

from .gap_register import GapRegister
from .identifiers import ReferenceId, StableId, reference
from .models import GapEntry

# --------------------------------------------------------------------------- #
# RMP-* error codes (fail closed; the gap register was already validated).     #
# --------------------------------------------------------------------------- #
RMP_DUPLICATE_WORKSTREAM = "RMP-DUPLICATE-WORKSTREAM"
RMP_UNKNOWN_DEPENDENCY = "RMP-UNKNOWN-DEPENDENCY"
RMP_SELF_DEPENDENCY = "RMP-SELF-DEPENDENCY"
RMP_ILLEGAL_JOIN = "RMP-ILLEGAL-JOIN"
RMP_CYCLE = "RMP-CYCLE"
RMP_MISSING_NARRATIVE = "RMP-MISSING-NARRATIVE"
RMP_UNKNOWN_WORKSTREAM = "RMP-UNKNOWN-WORKSTREAM"

NON_AGGREGATE_STATEMENT = (
    "Gap priority is a strict lexicographic ordering over dependency "
    "criticality, safety impact, claim risk, and target-unblock value, then "
    "stable identifier; the heterogeneous dimensions are never summed, and the "
    "roadmap ordering is a dependency frontier, not a schedule or effort total."
)


class RoadmapError(ValueError):
    """A fail-closed roadmap error carrying an ``RMP-*`` code and object refs."""

    def __init__(self, code: str, message: str, object_refs: Iterable[str] = ()) -> None:
        self.code = code
        self.object_refs = tuple(sorted({str(ref) for ref in object_refs}))
        detail = ", ".join(self.object_refs)
        suffix = f" [{detail}]" if detail else ""
        super().__init__(f"{code}: {message}{suffix}")


# --------------------------------------------------------------------------- #
# Gap priority ranking (Requirement 12.4).                                     #
# --------------------------------------------------------------------------- #

# The heterogeneous priority dimensions, most-significant first. They are only
# ever *compared*, never summed; this tuple documents the comparison order.
PRIORITY_DIMENSIONS: tuple[str, ...] = (
    "dependency_criticality",
    "safety_impact",
    "claim_risk",
    "target_unblock_value",
)


def gap_priority_key(gap: GapEntry) -> tuple[int, int, int, int, str]:
    """Return the strict lexicographic sort key for ``gap`` (most urgent first).

    The four integer dimensions are negated so a *higher* value sorts *earlier*
    (more urgent), and the stable identifier is appended ascending as the final,
    always-distinct tie-breaker. The dimensions are compared independently and
    are never summed into a single scalar (Requirement 12.4).
    """

    if not isinstance(gap, GapEntry):
        raise TypeError("gap must be a GapEntry")
    return (
        -int(gap.dependency_criticality),
        -int(gap.safety_impact),
        -int(gap.claim_risk),
        -int(gap.target_unblock_value),
        str(gap.id),
    )


def rank_gaps(gaps: GapRegister | Iterable[GapEntry]) -> tuple[GapEntry, ...]:
    """Return the gaps in deterministic priority order (highest priority first).

    Accepts a :class:`~tools.universe_os_gap_analysis.gap_register.GapRegister`
    or any iterable of :class:`GapEntry`. The ordering is the strict
    lexicographic comparison documented by :func:`gap_priority_key`, so it is a
    strict total order independent of the input order.
    """

    entries = tuple(gaps.gaps) if isinstance(gaps, GapRegister) else tuple(gaps)
    if not all(isinstance(gap, GapEntry) for gap in entries):
        raise TypeError("gaps must contain GapEntry values")
    return tuple(sorted(entries, key=gap_priority_key))


@dataclass(frozen=True, slots=True)
class GapRanking:
    """A deterministic, strict lexicographic priority ranking of gaps."""

    ranked_gaps: tuple[GapEntry, ...] = ()

    def __post_init__(self) -> None:
        entries = tuple(self.ranked_gaps)
        if not all(isinstance(gap, GapEntry) for gap in entries):
            raise TypeError("ranked_gaps must contain GapEntry values")
        object.__setattr__(self, "ranked_gaps", tuple(sorted(entries, key=gap_priority_key)))

    def rank_of(self, gap_id: str) -> int:
        """Return the zero-based priority rank of ``gap_id`` (0 == most urgent)."""

        target = str(gap_id)
        for index, gap in enumerate(self.ranked_gaps):
            if str(gap.id) == target:
                return index
        raise KeyError(f"unknown gap id: {gap_id!r}")

    @property
    def gap_ids(self) -> tuple[ReferenceId, ...]:
        return tuple(reference(gap.id) for gap in self.ranked_gaps)

    def observed_facts(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return ``(gap id, observed fact)`` pairs in priority order."""

        return tuple((reference(gap.id), gap.observed_fact) for gap in self.ranked_gaps)

    def recommendations(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return ``(gap id, recommendation)`` pairs in priority order."""

        return tuple((reference(gap.id), gap.recommendation) for gap in self.ranked_gaps)

    def __len__(self) -> int:
        return len(self.ranked_gaps)


def rank_gap_register(register: GapRegister) -> GapRanking:
    """Build a :class:`GapRanking` from a validated gap register."""

    if not isinstance(register, GapRegister):
        raise TypeError("register must be a GapRegister")
    return GapRanking(ranked_gaps=register.gaps)


# --------------------------------------------------------------------------- #
# Parallel roadmap: pre-kernel and post-boot gate lanes (Req 12.5-12.7, 15.7). #
# --------------------------------------------------------------------------- #


class RoadmapLane(str, Enum):
    """The two independent gate lanes of the Universe OS roadmap."""

    PRE_KERNEL = "PreKernel"
    POST_BOOT = "PostBoot"


@dataclass(frozen=True, slots=True, kw_only=True)
class RoadmapWorkstream:
    """A single gate workstream on one roadmap lane.

    ``depends_on`` names the prerequisite workstreams whose gates must be
    satisfied first. ``parallel_group``, when set, labels a branch that can
    progress independently of its sibling branches; every parallel branch must
    ultimately converge on a workstream flagged ``is_join`` (Requirement 12.7).
    ``observed_fact`` records the current evidence-backed state and is kept
    strictly separate from ``recommendation`` (Requirement 14.7).
    """

    id: StableId
    title: str
    lane: RoadmapLane
    depends_on: tuple[ReferenceId, ...] = ()
    parallel_group: str | None = None
    is_join: bool = False
    observed_fact: str = ""
    recommendation: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        if not isinstance(self.title, str) or not self.title.strip():
            raise ValueError("title must be a non-empty string")
        if not isinstance(self.lane, RoadmapLane):
            raise TypeError("lane must be a RoadmapLane")
        object.__setattr__(
            self,
            "depends_on",
            tuple(sorted({reference(dep) for dep in self.depends_on}, key=str)),
        )
        if self.parallel_group is not None and (
            not isinstance(self.parallel_group, str) or not self.parallel_group.strip()
        ):
            raise ValueError("parallel_group must be a non-empty string or None")
        if not isinstance(self.is_join, bool):
            raise TypeError("is_join must be a bool")
        for name in ("observed_fact", "recommendation"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise RoadmapError(
                    RMP_MISSING_NARRATIVE,
                    f"workstream must record a non-empty {name}",
                    (self.id,),
                )


@dataclass(frozen=True, slots=True)
class ParallelRoadmap:
    """The validated, deterministic two-lane parallel roadmap.

    Instances are only produced by :func:`build_parallel_roadmap`, which runs all
    ``RMP-*`` validation (unknown/self/cyclic dependencies, illegal joins,
    missing narrative) before returning. The API is read-only and every
    collection is sorted for determinism.
    """

    workstreams: tuple[RoadmapWorkstream, ...]
    _by_id: Mapping[str, RoadmapWorkstream] = field(default_factory=dict)
    _dependents: Mapping[str, tuple[str, ...]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        streams = tuple(self.workstreams)
        if not all(isinstance(item, RoadmapWorkstream) for item in streams):
            raise TypeError("workstreams must contain RoadmapWorkstream values")
        object.__setattr__(
            self, "workstreams", tuple(sorted(streams, key=lambda item: str(item.id)))
        )

    # -- basic accessors -------------------------------------------------- #

    def workstream(self, workstream_id: str) -> RoadmapWorkstream:
        key = str(workstream_id)
        if key not in self._by_id:
            raise RoadmapError(
                RMP_UNKNOWN_WORKSTREAM, "unknown workstream id", (key,)
            )
        return self._by_id[key]

    def lane(self, lane: RoadmapLane) -> tuple[RoadmapWorkstream, ...]:
        """Return every workstream on ``lane`` in deterministic order."""

        if not isinstance(lane, RoadmapLane):
            raise TypeError("lane must be a RoadmapLane")
        return tuple(item for item in self.workstreams if item.lane is lane)

    @property
    def pre_kernel(self) -> tuple[RoadmapWorkstream, ...]:
        return self.lane(RoadmapLane.PRE_KERNEL)

    @property
    def post_boot(self) -> tuple[RoadmapWorkstream, ...]:
        return self.lane(RoadmapLane.POST_BOOT)

    # -- parallel branches and join gates (Requirement 12.7) -------------- #

    def parallel_branches(self) -> Mapping[str, tuple[str, ...]]:
        """Return ``branch label -> sorted workstream ids`` for every branch."""

        branches: dict[str, list[str]] = {}
        for item in self.workstreams:
            if item.parallel_group is not None:
                branches.setdefault(item.parallel_group, []).append(str(item.id))
        return {label: tuple(sorted(ids)) for label, ids in sorted(branches.items())}

    @property
    def join_workstream_ids(self) -> tuple[str, ...]:
        return tuple(sorted(str(item.id) for item in self.workstreams if item.is_join))

    def dependents_of(self, workstream_id: str) -> tuple[ReferenceId, ...]:
        key = str(workstream_id)
        if key not in self._by_id:
            raise RoadmapError(RMP_UNKNOWN_WORKSTREAM, "unknown workstream id", (key,))
        return tuple(reference(dep) for dep in self._dependents.get(key, ()))

    # -- ordering and frontier -------------------------------------------- #

    def dependency_ordered_path(self) -> tuple[str, ...]:
        """Return a deterministic topological order: prerequisites before gates."""

        indegree = {str(item.id): len(item.depends_on) for item in self.workstreams}
        ready = sorted(wid for wid, degree in indegree.items() if degree == 0)
        order: list[str] = []
        while ready:
            current = ready.pop(0)
            order.append(current)
            newly_ready: list[str] = []
            for dependent in self._dependents.get(current, ()):
                indegree[dependent] -= 1
                if indegree[dependent] == 0:
                    newly_ready.append(dependent)
            if newly_ready:
                ready = sorted(ready + newly_ready)
        return tuple(order)

    def gate_frontier(self, satisfied: Iterable[str] = ()) -> tuple[ReferenceId, ...]:
        """Return the actionable frontier: unmet gates whose deps are all satisfied.

        ``satisfied`` is the set of workstream ids a caller considers already
        complete. With an empty ``satisfied`` set this is exactly the roadmap's
        root workstreams (the low-level language soundness and primitive-object
        gates).
        """

        satisfied_ids = {str(wid) for wid in satisfied}
        unknown = satisfied_ids - set(self._by_id)
        if unknown:
            raise RoadmapError(
                RMP_UNKNOWN_WORKSTREAM, "unknown satisfied workstream id", unknown
            )
        frontier: list[str] = []
        for item in self.workstreams:
            wid = str(item.id)
            if wid in satisfied_ids:
                continue
            if all(str(dep) in satisfied_ids for dep in item.depends_on):
                frontier.append(wid)
        return tuple(reference(wid) for wid in sorted(frontier))

    # -- observed facts vs recommendations (Requirement 14.7) ------------- #

    def observed_facts(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return ``(workstream id, observed fact)`` pairs in dependency order."""

        return tuple(
            (reference(wid), self._by_id[wid].observed_fact)
            for wid in self.dependency_ordered_path()
        )

    def recommendations(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return ``(workstream id, recommendation)`` pairs in dependency order."""

        return tuple(
            (reference(wid), self._by_id[wid].recommendation)
            for wid in self.dependency_ordered_path()
        )

    def __len__(self) -> int:
        return len(self.workstreams)


def build_parallel_roadmap(
    workstreams: Iterable[RoadmapWorkstream] | None = None,
) -> ParallelRoadmap:
    """Build and fully validate the parallel pre-kernel/post-boot roadmap.

    When ``workstreams`` is omitted, the canonical Universe OS roadmap
    (Requirements 12.5-12.7, 15.7) is used. All ``RMP-*`` validation runs before
    returning, so an inconsistent roadmap can never be published:

    * duplicate workstream ids fail closed with ``RMP-DUPLICATE-WORKSTREAM``;
    * a dependency on an unknown workstream fails with ``RMP-UNKNOWN-DEPENDENCY``;
    * a self dependency fails with ``RMP-SELF-DEPENDENCY``;
    * a workstream flagged ``is_join`` that does not converge at least two
      prerequisite branches fails with ``RMP-ILLEGAL-JOIN``; and
    * any dependency cycle fails with ``RMP-CYCLE``.
    """

    streams = tuple(_CANONICAL_WORKSTREAMS if workstreams is None else workstreams)
    for item in streams:
        if not isinstance(item, RoadmapWorkstream):
            raise TypeError("workstreams must contain RoadmapWorkstream values")

    # 1) Unique ids.
    by_id: dict[str, RoadmapWorkstream] = {}
    duplicates: set[str] = set()
    for item in streams:
        wid = str(item.id)
        if wid in by_id:
            duplicates.add(wid)
        by_id[wid] = item
    if duplicates:
        raise RoadmapError(
            RMP_DUPLICATE_WORKSTREAM, "duplicate workstream id", duplicates
        )

    # 2) Dependency references: unknown node and self edge.
    dependents: dict[str, list[str]] = {wid: [] for wid in by_id}
    for item in streams:
        wid = str(item.id)
        for dep in item.depends_on:
            dep_id = str(dep)
            if dep_id == wid:
                raise RoadmapError(
                    RMP_SELF_DEPENDENCY, "a workstream cannot depend on itself", (wid,)
                )
            if dep_id not in by_id:
                raise RoadmapError(
                    RMP_UNKNOWN_DEPENDENCY,
                    "workstream depends on an unknown workstream",
                    (f"{wid}->{dep_id}",),
                )
            dependents[dep_id].append(wid)

    # 3) Illegal join validation (Requirement 12.7): a declared join must
    #    genuinely converge at least two prerequisite branches.
    for item in streams:
        if item.is_join and len(item.depends_on) < 2:
            raise RoadmapError(
                RMP_ILLEGAL_JOIN,
                "a join workstream must converge at least two prerequisite branches",
                (str(item.id),),
            )

    # 4) Cycle detection (must precede publishing an ordered roadmap).
    _reject_cycles(by_id, {wid: item.depends_on for wid, item in by_id.items()})

    return ParallelRoadmap(
        workstreams=streams,
        _by_id=dict(by_id),
        _dependents={wid: tuple(sorted(vals)) for wid, vals in dependents.items()},
    )


def _reject_cycles(
    nodes: Mapping[str, RoadmapWorkstream],
    dependencies: Mapping[str, tuple[ReferenceId, ...]],
) -> None:
    """Fail closed with ``RMP-CYCLE`` if the workstream graph is not a DAG."""

    WHITE, GRAY, BLACK = 0, 1, 2
    color: dict[str, int] = {wid: WHITE for wid in nodes}

    def visit(node_id: str, stack: list[str]) -> None:
        color[node_id] = GRAY
        stack.append(node_id)
        for dep in sorted(str(d) for d in dependencies.get(node_id, ())):
            if color[dep] == GRAY:
                cycle = stack[stack.index(dep):] + [dep]
                raise RoadmapError(RMP_CYCLE, "dependency cycle detected", cycle)
            if color[dep] == WHITE:
                visit(dep, stack)
        stack.pop()
        color[node_id] = BLACK

    for wid in sorted(nodes):
        if color[wid] == WHITE:
            visit(wid, [])


# --------------------------------------------------------------------------- #
# Combined gap roadmap.                                                        #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True, slots=True)
class GapRoadmap:
    """The ranked gap register paired with the parallel Hard-Gate roadmap.

    This is the single object the report renderer (Task 11) consumes: it exposes
    the deterministic gap priority ranking and the two-lane roadmap together,
    while keeping every observed fact strictly separate from every recommendation
    (Requirement 14.7).
    """

    ranking: GapRanking
    roadmap: ParallelRoadmap

    def __post_init__(self) -> None:
        if not isinstance(self.ranking, GapRanking):
            raise TypeError("ranking must be a GapRanking")
        if not isinstance(self.roadmap, ParallelRoadmap):
            raise TypeError("roadmap must be a ParallelRoadmap")

    def observed_facts(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return every observed fact (gaps first, then workstreams), no recs mixed in."""

        return self.ranking.observed_facts() + self.roadmap.observed_facts()

    def recommendations(self) -> tuple[tuple[ReferenceId, str], ...]:
        """Return every recommendation (gaps first, then workstreams), no facts mixed in."""

        return self.ranking.recommendations() + self.roadmap.recommendations()


def build_gap_roadmap(
    register: GapRegister,
    *,
    roadmap: ParallelRoadmap | None = None,
) -> GapRoadmap:
    """Combine a validated gap register with the parallel roadmap.

    The gaps are ranked by strict lexicographic priority (Requirement 12.4) and
    paired with the canonical pre-kernel/post-boot roadmap (Requirements
    12.5-12.7, 15.7) unless an explicit ``roadmap`` is supplied.
    """

    if not isinstance(register, GapRegister):
        raise TypeError("register must be a GapRegister")
    if roadmap is None:
        roadmap = build_parallel_roadmap()
    elif not isinstance(roadmap, ParallelRoadmap):
        raise TypeError("roadmap must be a ParallelRoadmap")
    return GapRoadmap(ranking=rank_gap_register(register), roadmap=roadmap)


# --------------------------------------------------------------------------- #
# Canonical Universe OS roadmap (Requirements 12.5-12.7, 15.7).                #
#                                                                              #
# The dependency structure mirrors the design's DAG frontier exactly:         #
#                                                                              #
#   L -> A -> {B -> R, BT}                                                     #
#   O  (independent primitive ET_REL object gate root)                        #
#   {R, BT, O} -> ELF -> M -> Q                     (pre-kernel lane)          #
#   Q -> {K1, K2} -> J -> {D -> S, U} -> {S, U} -> OPS   (post-boot lane)      #
#                                                                              #
# ELF, J, and OPS are the explicit join gates where parallel branches         #
# converge. Nothing here is summed or scheduled; it is a dependency ordering.  #
# --------------------------------------------------------------------------- #

# Pre-kernel workstream ids.
_WS_LANGUAGE = "pre-kernel-language-soundness"
_WS_SYSTEM_ABI = "pre-kernel-freestanding-system-abi"
_WS_BACKEND = "pre-kernel-independent-backend-bootstrap"
_WS_RUNTIME = "pre-kernel-freestanding-core-runtime"
_WS_BOOT_TOOLCHAIN = "pre-kernel-complete-boot-toolchain"
_WS_PRIMITIVE_OBJECT = "pre-kernel-primitive-et-rel-object"
_WS_LINKED_ELF = "pre-kernel-deterministic-linked-elf"
_WS_BOOT_MEDIA = "pre-kernel-boot-media"
_WS_QEMU_PROOF = "pre-kernel-qemu-serial-proof"

# Post-boot workstream ids.
_WS_MEMORY = "post-boot-memory-management-mmu"
_WS_INTERRUPTS = "post-boot-interrupts"
_WS_SCHEDULER_SYSCALL = "post-boot-scheduler-syscall-capability"
_WS_DRIVERS = "post-boot-drivers-dma"
_WS_ISOLATION_USERSPACE = "post-boot-process-isolation-userspace"
_WS_STORAGE_NETWORK = "post-boot-storage-networking"
_WS_UPDATE_RECOVERY_SHELL = "post-boot-update-recovery-product-shell"

# Parallel branch labels.
_BRANCH_BACKEND_RUNTIME = "backend-and-runtime"
_BRANCH_BOOT_TOOLCHAIN = "boot-toolchain"
_BRANCH_PRIMITIVE_OBJECT = "primitive-object"
_BRANCH_MEMORY = "memory-and-mmu"
_BRANCH_INTERRUPTS = "interrupts"
_BRANCH_DRIVERS = "drivers-and-storage"
_BRANCH_USERSPACE = "isolation-and-userspace"

_UNIMPLEMENTED = (
    "No direct implementation evidence for this capability exists at the bound "
    "revision, so its maturity is 0; plans and adjacent hosted assets grant no "
    "credit."
)

_CANONICAL_WORKSTREAMS: tuple[RoadmapWorkstream, ...] = (
    # -- pre-kernel lane ------------------------------------------------- #
    RoadmapWorkstream(
        id=_WS_LANGUAGE,
        title="Low-level language soundness",
        lane=RoadmapLane.PRE_KERNEL,
        observed_fact=(
            "Language and tooling are a hosted foundation whose strongest "
            "repository-local capabilities sit at maturity 2; freestanding-grade "
            "soundness is not yet demonstrated."
        ),
        recommendation=(
            "Establish low-level language soundness (memory/type/ABI-relevant "
            "guarantees) as the first pre-kernel gate before any substrate work."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_SYSTEM_ABI,
        title="Freestanding system ABI",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_LANGUAGE,),
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Define and verify a freestanding system ABI once language soundness "
            "holds; it gates both the independent backend and the boot toolchain."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_BACKEND,
        title="Independent backend / bootstrap",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_SYSTEM_ABI,),
        parallel_group=_BRANCH_BACKEND_RUNTIME,
        observed_fact=(
            "Production compilation still depends on generated C++ and external "
            "host tooling, so an independent backend/bootstrap is unachieved and "
            "T1 remains blocked."
        ),
        recommendation=(
            "Build an independent backend/bootstrap path; this branch runs in "
            "parallel with the boot-toolchain branch after the system ABI gate."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_RUNTIME,
        title="Freestanding core / runtime",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_BACKEND,),
        parallel_group=_BRANCH_BACKEND_RUNTIME,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Deliver a freestanding core/runtime on top of the independent "
            "backend; it feeds the deterministic linked ELF join gate."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_BOOT_TOOLCHAIN,
        title="Complete boot toolchain",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_SYSTEM_ABI,),
        parallel_group=_BRANCH_BOOT_TOOLCHAIN,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Close the boot toolchain (target spec, linker script/input, "
            "relocation, startup object); this branch progresses independently of "
            "the backend/runtime branch and feeds the linked ELF join gate."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_PRIMITIVE_OBJECT,
        title="Primitive ET_REL object gate",
        lane=RoadmapLane.PRE_KERNEL,
        parallel_group=_BRANCH_PRIMITIVE_OBJECT,
        observed_fact=(
            "Primitive ET_REL relocatable-object emission is a clang-backed, "
            "narrowly scoped gate; it proves only its named scope and does not "
            "imply any later boot stage."
        ),
        recommendation=(
            "Keep the primitive ET_REL object gate as an independent input that "
            "joins the deterministic linked ELF, never as proof of later stages."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_LINKED_ELF,
        title="Deterministic linked ELF",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_RUNTIME, _WS_BOOT_TOOLCHAIN, _WS_PRIMITIVE_OBJECT),
        is_join=True,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Produce a deterministic linked ELF: the explicit join gate where the "
            "backend/runtime, boot-toolchain, and primitive-object branches "
            "converge before boot media."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_BOOT_MEDIA,
        title="Boot media",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_LINKED_ELF,),
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Assemble bootable media from the deterministic linked ELF before any "
            "execution proof."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_QEMU_PROOF,
        title="QEMU serial execution proof",
        lane=RoadmapLane.PRE_KERNEL,
        depends_on=(_WS_BOOT_MEDIA,),
        observed_fact=(
            "No QEMU serial execution proof exists yet; when one is added it "
            "proves only that the image boots and does not imply drivers, "
            "interrupts, MMU, scheduling, syscalls, isolation, storage, "
            "networking, userspace, or operations."
        ),
        recommendation=(
            "Demonstrate a QEMU serial execution proof as the final pre-kernel "
            "gate; the post-boot lane starts only after this proof."
        ),
    ),
    # -- post-boot lane -------------------------------------------------- #
    RoadmapWorkstream(
        id=_WS_MEMORY,
        title="Memory management / MMU",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_QEMU_PROOF,),
        parallel_group=_BRANCH_MEMORY,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Gate memory management and the MMU separately after the boot proof; "
            "this branch runs in parallel with interrupts."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_INTERRUPTS,
        title="Interrupts",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_QEMU_PROOF,),
        parallel_group=_BRANCH_INTERRUPTS,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Gate interrupt handling separately after the boot proof; this branch "
            "runs in parallel with memory management and joins the scheduler gate."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_SCHEDULER_SYSCALL,
        title="Scheduler with syscall/capability",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_MEMORY, _WS_INTERRUPTS),
        is_join=True,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Deliver scheduling with the syscall/capability boundary as the "
            "explicit join gate where the memory and interrupt branches converge."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_DRIVERS,
        title="Drivers / DMA",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_SCHEDULER_SYSCALL,),
        parallel_group=_BRANCH_DRIVERS,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Gate drivers and DMA safety after the scheduler/syscall join; this "
            "branch runs in parallel with process isolation and userspace."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_ISOLATION_USERSPACE,
        title="Process isolation and userspace",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_SCHEDULER_SYSCALL,),
        parallel_group=_BRANCH_USERSPACE,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Gate process isolation and userspace after the scheduler/syscall "
            "join; this branch runs in parallel with drivers and joins operations."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_STORAGE_NETWORK,
        title="Storage and networking",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_DRIVERS,),
        parallel_group=_BRANCH_DRIVERS,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Gate storage and networking on top of drivers/DMA; it feeds the "
            "update/recovery/product-shell join gate."
        ),
    ),
    RoadmapWorkstream(
        id=_WS_UPDATE_RECOVERY_SHELL,
        title="Update/recovery and product shell",
        lane=RoadmapLane.POST_BOOT,
        depends_on=(_WS_STORAGE_NETWORK, _WS_ISOLATION_USERSPACE),
        is_join=True,
        observed_fact=_UNIMPLEMENTED,
        recommendation=(
            "Deliver update/recovery and the product shell as the explicit join "
            "gate where the storage/networking and isolation/userspace branches "
            "converge; this closes the post-boot lane."
        ),
    ),
)
