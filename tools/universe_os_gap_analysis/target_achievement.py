"""Target-level achievement and scoped blockers (Task 8.3).

This stage is the final consumer of the Task 8.1 validated Hard-Gate graph and
the Task 8.2 :class:`~tools.universe_os_gap_analysis.maturity.MaturityAssessment`.
It decides, per ordered ``Target_Level``, whether that level is *achieved* under
the current evidence, and — when it is not — reports the next Hard-Gate, the
blocking dependencies, the scoped blockers, and the limitations that keep it
unachieved. It never produces a total score, average, percentage, or schedule
estimate (Requirements 2.2, 2.3, 3.1-3.3, 7.4, 7.5, 15.2, 15.3, 15.6).

Achievement contract (design "Maturity Assessor"):

* A ``Target_Level`` is marked **achieved** only when **all** of the following
  hold together:

    1. every *mandatory* capability domain for that level reaches its required
       ordinal maturity threshold (per-domain, non-additive);
    2. every Hard-Gate required for that level (the level's own gates plus their
       transitive blocking prerequisites) is satisfied at the configured gate
       maturity threshold;
    3. no *blocking* evidence conflict touches a mandatory domain of the level;
    4. no active *scoped blocker* is declared for the level; and
    5. assessment validation succeeded.

  If any single condition fails, the level is **unachieved**. Nothing is summed:
  each condition is an independent gate on the level (Requirement 3.1, 3.2).

* **Hosted-adjacency isolation** (Requirements 2.3, 15.6): the hosted-adjacency
  level ``T0`` is evaluated only from its own hosted-adjacency domains and gates.
  Substrate levels ``T1``-``T5`` never count a ``T0`` gate as a required
  prerequisite, so adding hosted-adjacency evidence can only affect ``T0`` and
  can never change ``T1``-``T5`` achievement, next gate, or blocking
  dependencies.

* **Scoped blockers** (Requirements 7.4, 7.5, 15.2): a production
  generated-C++/host-tooling dependency is a scoped blocker on
  ``T1_Independent_Language_Platform``. While such a blocker is active, ``T1``
  stays unachieved regardless of any domain or gate score. The convenience
  factory :func:`production_backend_blocker` builds exactly this blocker.

The stage is ordinal-only and read-only: it mutates no evidence, upgrades no
status, and edits no evaluator or product module. It imports the graph and
maturity modules by module path and fails closed with a ``TGT-*`` code on any
structural problem (an unknown domain/gate reference or a duplicate mandatory
requirement).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping

from .hard_gate_graph import HardGateGraph
from .identifiers import ReferenceId, reference
from .maturity import MaturityAssessment
from .models import MaturityScore, TargetLevel

# --------------------------------------------------------------------------- #
# TGT-* error codes (fail closed; the graph and maturity stages already ran). #
# --------------------------------------------------------------------------- #
TGT_UNKNOWN_DOMAIN = "TGT-UNKNOWN-DOMAIN"
TGT_UNKNOWN_GATE = "TGT-UNKNOWN-GATE"
TGT_DUPLICATE_REQUIREMENT = "TGT-DUPLICATE-REQUIREMENT"

# The ordered target levels, T0 first through T5 last (Requirement 2.2).
_TARGET_ORDER: dict[TargetLevel, int] = {
    level: index for index, level in enumerate(TargetLevel)
}
HOSTED_ADJACENCY_LEVEL = TargetLevel.T0_HOSTED_ADJACENCY

# Default per-domain minimum maturity for a mandatory domain to count as met.
DEFAULT_DOMAIN_THRESHOLD = MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY
# Default maturity at which a Hard-Gate is considered satisfied as a dependency.
DEFAULT_GATE_THRESHOLD = MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT

NON_AGGREGATE_STATEMENT = (
    "Target achievement is an all-or-nothing per-level gate on mandatory "
    "domains, Hard-Gates, conflicts, and validation; it produces no total "
    "score, average, percentage, or schedule estimate."
)


class TargetAchievementError(ValueError):
    """A fail-closed target-achievement error carrying a ``TGT-*`` code and refs."""

    def __init__(self, code: str, message: str, object_refs: Iterable[str] = ()) -> None:
        self.code = code
        self.object_refs = tuple(sorted({str(ref) for ref in object_refs}))
        detail = ", ".join(self.object_refs)
        suffix = f" [{detail}]" if detail else ""
        super().__init__(f"{code}: {message}{suffix}")


@dataclass(frozen=True, slots=True, kw_only=True)
class MandatoryDomainRequirement:
    """A mandatory capability domain that a target level requires.

    ``minimum_maturity`` is the per-domain ordinal threshold the domain's
    *effective* score must reach; it is compared directly, never summed with any
    other domain's score.
    """

    domain_id: str
    target_level: TargetLevel
    minimum_maturity: MaturityScore = DEFAULT_DOMAIN_THRESHOLD

    def __post_init__(self) -> None:
        if not isinstance(self.domain_id, str) or not self.domain_id.strip():
            raise ValueError("domain_id must be a non-empty string")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.minimum_maturity, MaturityScore):
            raise TypeError("minimum_maturity must be a MaturityScore")


@dataclass(frozen=True, slots=True, kw_only=True)
class ScopedBlocker:
    """A scoped blocker that holds a single target level unachieved.

    The blocker is *scoped* to exactly one target level; it never propagates to
    any other level. A production generated-C++/host-tooling dependency is the
    canonical T1 blocker (Requirements 7.4, 7.5, 15.2).
    """

    id: str
    target_level: TargetLevel
    reason: str
    requirement_refs: tuple[str, ...] = ()
    active: bool = True

    def __post_init__(self) -> None:
        if not isinstance(self.id, str) or not self.id.strip():
            raise ValueError("id must be a non-empty string")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.reason, str) or not self.reason.strip():
            raise ValueError("reason must be a non-empty string")
        refs = tuple(self.requirement_refs)
        if not all(isinstance(item, str) and item.strip() for item in refs):
            raise ValueError("requirement_refs must be non-empty strings")
        object.__setattr__(self, "requirement_refs", refs)
        if not isinstance(self.active, bool):
            raise TypeError("active must be a bool")


@dataclass(frozen=True, slots=True, kw_only=True)
class ConflictImpact:
    """A blocking-evidence-conflict impact projected onto capability domains."""

    conflict_id: str
    blocking: bool
    domain_ids: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.conflict_id, str) or not self.conflict_id.strip():
            raise ValueError("conflict_id must be a non-empty string")
        if not isinstance(self.blocking, bool):
            raise TypeError("blocking must be a bool")
        domains = tuple(self.domain_ids)
        if not all(isinstance(item, str) and item.strip() for item in domains):
            raise ValueError("domain_ids must be non-empty strings")
        object.__setattr__(self, "domain_ids", domains)


@dataclass(frozen=True, slots=True, kw_only=True)
class UnmetDomain:
    """A mandatory domain whose effective maturity is below its threshold."""

    domain_id: ReferenceId
    effective_score: MaturityScore
    required_minimum: MaturityScore

    def __post_init__(self) -> None:
        object.__setattr__(self, "domain_id", reference(self.domain_id))
        if not isinstance(self.effective_score, MaturityScore):
            raise TypeError("effective_score must be a MaturityScore")
        if not isinstance(self.required_minimum, MaturityScore):
            raise TypeError("required_minimum must be a MaturityScore")


@dataclass(frozen=True, slots=True, kw_only=True)
class TargetLevelAchievement:
    """The achievement decision and rationale for one target level."""

    level: TargetLevel
    order: int
    is_hosted_adjacency: bool
    achieved: bool
    validation_ok: bool
    satisfied_mandatory_domain_ids: tuple[ReferenceId, ...]
    unmet_mandatory_domains: tuple[UnmetDomain, ...]
    satisfied_gate_ids: tuple[ReferenceId, ...]
    unsatisfied_gate_ids: tuple[ReferenceId, ...]
    next_hard_gate_id: ReferenceId | None
    blocking_dependency_ids: tuple[ReferenceId, ...]
    active_blockers: tuple[ScopedBlocker, ...]
    blocking_conflict_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    rationale: str

    def __post_init__(self) -> None:
        if not isinstance(self.level, TargetLevel):
            raise TypeError("level must be a TargetLevel")
        if not isinstance(self.order, int) or isinstance(self.order, bool):
            raise TypeError("order must be an int")
        for name in ("is_hosted_adjacency", "achieved", "validation_ok"):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")
        object.__setattr__(
            self,
            "satisfied_mandatory_domain_ids",
            tuple(sorted((reference(d) for d in self.satisfied_mandatory_domain_ids), key=str)),
        )
        unmet = tuple(self.unmet_mandatory_domains)
        if not all(isinstance(item, UnmetDomain) for item in unmet):
            raise TypeError("unmet_mandatory_domains must contain UnmetDomain values")
        object.__setattr__(
            self,
            "unmet_mandatory_domains",
            tuple(sorted(unmet, key=lambda item: str(item.domain_id))),
        )
        object.__setattr__(
            self,
            "satisfied_gate_ids",
            tuple(sorted((reference(g) for g in self.satisfied_gate_ids), key=str)),
        )
        object.__setattr__(
            self,
            "unsatisfied_gate_ids",
            tuple(sorted((reference(g) for g in self.unsatisfied_gate_ids), key=str)),
        )
        if self.next_hard_gate_id is not None:
            object.__setattr__(self, "next_hard_gate_id", reference(self.next_hard_gate_id))
        object.__setattr__(
            self,
            "blocking_dependency_ids",
            tuple(sorted((reference(g) for g in self.blocking_dependency_ids), key=str)),
        )
        blockers = tuple(self.active_blockers)
        if not all(isinstance(item, ScopedBlocker) for item in blockers):
            raise TypeError("active_blockers must contain ScopedBlocker values")
        object.__setattr__(
            self, "active_blockers", tuple(sorted(blockers, key=lambda item: item.id))
        )
        object.__setattr__(
            self,
            "blocking_conflict_ids",
            tuple(sorted((reference(c) for c in self.blocking_conflict_ids), key=str)),
        )
        limitations = tuple(self.limitations)
        if not all(isinstance(item, str) and item.strip() for item in limitations):
            raise ValueError("limitations must be non-empty strings")
        object.__setattr__(self, "limitations", limitations)
        if not isinstance(self.rationale, str) or not self.rationale.strip():
            raise ValueError("rationale must be a non-empty string")
        # An achieved level cannot carry any unmet condition (internal invariant).
        if self.achieved and (
            self.unmet_mandatory_domains
            or self.unsatisfied_gate_ids
            or self.active_blockers
            or self.blocking_conflict_ids
            or not self.validation_ok
        ):
            raise ValueError("an achieved level cannot carry an unmet condition")


@dataclass(frozen=True, slots=True)
class TargetAchievementReport:
    """All per-level achievement decisions, ordered T0 through T5."""

    results: tuple[TargetLevelAchievement, ...]

    def __post_init__(self) -> None:
        results = tuple(self.results)
        if not all(isinstance(item, TargetLevelAchievement) for item in results):
            raise TypeError("results must contain TargetLevelAchievement values")
        object.__setattr__(
            self, "results", tuple(sorted(results, key=lambda item: item.order))
        )

    def result_for(self, level: TargetLevel) -> TargetLevelAchievement:
        for result in self.results:
            if result.level is level:
                return result
        raise KeyError(f"no achievement result for {level!r}")

    def achieved_levels(self) -> tuple[TargetLevel, ...]:
        return tuple(result.level for result in self.results if result.achieved)

    def is_achieved(self, level: TargetLevel) -> bool:
        return self.result_for(level).achieved


def production_backend_blocker(
    *,
    reason: str = (
        "Production compilation still depends on generated C++ and external "
        "host tooling with no accepted independent bootstrap path, so "
        "T1_Independent_Language_Platform remains unachieved."
    ),
    active: bool = True,
) -> ScopedBlocker:
    """Build the canonical T1 generated-C++/host-tooling scoped blocker.

    Validates Requirements 7.4, 7.5, and 15.2: while generated C++ or external
    clang remains a production dependency (or the dependency inventory is
    incomplete / no independent bootstrap is accepted), T1 stays unachieved.
    """

    return ScopedBlocker(
        id="blocker-t1-generated-cpp-host-tooling",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        reason=reason,
        requirement_refs=("7.4", "7.5", "15.2"),
        active=active,
    )


def _transitive_blocking_prerequisites(
    graph: HardGateGraph, gate_ids: Iterable[str]
) -> set[str]:
    """Return every gate reachable through blocking dependency edges."""

    result: set[str] = set()
    stack = [str(gate_id) for gate_id in gate_ids]
    while stack:
        current = stack.pop()
        for dep in graph.blocking_dependencies_of(current):
            dep_id = str(dep)
            if dep_id not in result:
                result.add(dep_id)
                stack.append(dep_id)
    return result


def evaluate_target_achievement(
    *,
    assessment: MaturityAssessment,
    graph: HardGateGraph,
    mandatory_requirements: Iterable[MandatoryDomainRequirement],
    scoped_blockers: Iterable[ScopedBlocker] = (),
    conflict_impacts: Iterable[ConflictImpact] = (),
    validation_ok: bool = True,
    gate_threshold: MaturityScore = DEFAULT_GATE_THRESHOLD,
) -> TargetAchievementReport:
    """Decide per-level target achievement from maturity and the Hard-Gate graph.

    A level is achieved only when every mandatory domain reaches its threshold,
    every required Hard-Gate is satisfied, no blocking conflict touches a
    mandatory domain, no active scoped blocker is declared for the level, and
    validation succeeded. Hosted-adjacency (T0) gates never enter a substrate
    level's required set, so T0 evidence cannot change T1-T5.

    Fails closed with a ``TGT-*`` code on an unknown domain reference, an unknown
    gate reference, or a duplicate mandatory requirement.
    """

    if not isinstance(assessment, MaturityAssessment):
        raise TypeError("assessment must be a MaturityAssessment")
    if not isinstance(graph, HardGateGraph):
        raise TypeError("graph must be a HardGateGraph")
    if not isinstance(gate_threshold, MaturityScore):
        raise TypeError("gate_threshold must be a MaturityScore")
    if not isinstance(validation_ok, bool):
        raise TypeError("validation_ok must be a bool")

    requirements = tuple(mandatory_requirements)
    if not all(isinstance(item, MandatoryDomainRequirement) for item in requirements):
        raise TypeError("mandatory_requirements must contain MandatoryDomainRequirement values")
    blockers = tuple(scoped_blockers)
    if not all(isinstance(item, ScopedBlocker) for item in blockers):
        raise TypeError("scoped_blockers must contain ScopedBlocker values")
    impacts = tuple(conflict_impacts)
    if not all(isinstance(item, ConflictImpact) for item in impacts):
        raise TypeError("conflict_impacts must contain ConflictImpact values")

    # Fail closed on duplicate (level, domain) requirements.
    requirement_keys: set[tuple[str, str]] = set()
    duplicates: set[str] = set()
    for req in requirements:
        key = (req.target_level.value, req.domain_id)
        if key in requirement_keys:
            duplicates.add(req.domain_id)
        requirement_keys.add(key)
    if duplicates:
        raise TargetAchievementError(
            TGT_DUPLICATE_REQUIREMENT,
            "duplicate mandatory domain requirement for a target level",
            duplicates,
        )

    # Fail closed on unknown domain references.
    known_domains = {str(result.domain_id) for result in assessment.results}
    unknown_domains = {req.domain_id for req in requirements} - known_domains
    if unknown_domains:
        raise TargetAchievementError(
            TGT_UNKNOWN_DOMAIN,
            "mandatory requirement references a domain absent from the assessment",
            unknown_domains,
        )

    # Precompute per-gate satisfaction and a stable dependency order.
    known_gates = set(graph.gate_ids)
    gate_satisfied: dict[str, bool] = {
        gate_id: int(assessment.gate_effective_score_of(gate_id)) >= int(gate_threshold)
        for gate_id in known_gates
    }
    order_index = {gate_id: i for i, gate_id in enumerate(graph.dependency_ordered_path())}

    requirements_by_level: dict[TargetLevel, list[MandatoryDomainRequirement]] = {}
    for req in requirements:
        requirements_by_level.setdefault(req.target_level, []).append(req)

    results: list[TargetLevelAchievement] = []
    for level in TargetLevel:
        order = _TARGET_ORDER[level]
        is_hosted = level is HOSTED_ADJACENCY_LEVEL

        # 1) Mandatory domains for this level.
        level_reqs = requirements_by_level.get(level, [])
        satisfied_domains: list[str] = []
        unmet_domains: list[UnmetDomain] = []
        mandatory_domain_ids = {req.domain_id for req in level_reqs}
        for req in level_reqs:
            effective = assessment.effective_score_of(req.domain_id)
            if int(effective) >= int(req.minimum_maturity):
                satisfied_domains.append(req.domain_id)
            else:
                unmet_domains.append(
                    UnmetDomain(
                        domain_id=req.domain_id,
                        effective_score=effective,
                        required_minimum=req.minimum_maturity,
                    )
                )

        # 2) Required Hard-Gates: this level's own gates plus their transitive
        #    blocking prerequisites. For substrate levels, hosted-adjacency gates
        #    are never required prerequisites (Requirements 2.3, 15.6).
        level_gate_ids = {
            gate_id
            for gate_id in known_gates
            if graph.gate(gate_id).target_level is level
        }
        required_gate_ids = set(level_gate_ids)
        required_gate_ids |= _transitive_blocking_prerequisites(graph, level_gate_ids)
        if not is_hosted:
            required_gate_ids = {
                gate_id
                for gate_id in required_gate_ids
                if graph.gate(gate_id).target_level is not HOSTED_ADJACENCY_LEVEL
            }

        satisfied_gates = sorted(g for g in required_gate_ids if gate_satisfied[g])
        unsatisfied_gates = sorted(g for g in required_gate_ids if not gate_satisfied[g])

        # Blocking dependencies are the unsatisfied *prerequisite* gates that
        # belong to a strictly lower target level than this one.
        blocking_dependencies = sorted(
            g
            for g in unsatisfied_gates
            if _TARGET_ORDER[graph.gate(g).target_level] < order
        )

        # Next Hard-Gate: the earliest actionable unsatisfied gate on the
        # frontier (all of its blocking dependencies already satisfied).
        next_gate = _select_next_gate(
            graph, unsatisfied_gates, gate_satisfied, order_index
        )

        # 3) Blocking conflicts touching a mandatory domain of this level.
        blocking_conflicts = sorted(
            {
                impact.conflict_id
                for impact in impacts
                if impact.blocking
                and mandatory_domain_ids.intersection(impact.domain_ids)
            }
        )

        # 4) Active scoped blockers declared for exactly this level.
        active_blockers = tuple(
            b for b in blockers if b.active and b.target_level is level
        )

        achieved = (
            validation_ok
            and not unmet_domains
            and not unsatisfied_gates
            and not blocking_conflicts
            and not active_blockers
        )

        limitations = _build_limitations(
            level=level,
            validation_ok=validation_ok,
            unmet_domains=tuple(unmet_domains),
            unsatisfied_gates=tuple(unsatisfied_gates),
            blocking_conflicts=tuple(blocking_conflicts),
            active_blockers=active_blockers,
        )
        rationale = _build_rationale(
            level=level,
            achieved=achieved,
            is_hosted=is_hosted,
            unmet_domains=tuple(unmet_domains),
            unsatisfied_gates=tuple(unsatisfied_gates),
            next_gate=next_gate,
            active_blockers=active_blockers,
            blocking_conflicts=tuple(blocking_conflicts),
            validation_ok=validation_ok,
        )

        results.append(
            TargetLevelAchievement(
                level=level,
                order=order,
                is_hosted_adjacency=is_hosted,
                achieved=achieved,
                validation_ok=validation_ok,
                satisfied_mandatory_domain_ids=tuple(satisfied_domains),
                unmet_mandatory_domains=tuple(unmet_domains),
                satisfied_gate_ids=tuple(satisfied_gates),
                unsatisfied_gate_ids=tuple(unsatisfied_gates),
                next_hard_gate_id=next_gate,
                blocking_dependency_ids=tuple(blocking_dependencies),
                active_blockers=active_blockers,
                blocking_conflict_ids=tuple(blocking_conflicts),
                limitations=limitations,
                rationale=rationale,
            )
        )

    return TargetAchievementReport(results=tuple(results))


def _select_next_gate(
    graph: HardGateGraph,
    unsatisfied_gates: Iterable[str],
    gate_satisfied: Mapping[str, bool],
    order_index: Mapping[str, int],
) -> str | None:
    """Return the earliest unsatisfied gate whose blocking deps are all satisfied."""

    frontier: list[str] = []
    for gate_id in unsatisfied_gates:
        deps = graph.blocking_dependencies_of(gate_id)
        if all(gate_satisfied[str(dep)] for dep in deps):
            frontier.append(gate_id)
    if not frontier:
        return None
    return min(frontier, key=lambda gate_id: (order_index.get(gate_id, 0), gate_id))


def _build_limitations(
    *,
    level: TargetLevel,
    validation_ok: bool,
    unmet_domains: tuple[UnmetDomain, ...],
    unsatisfied_gates: tuple[str, ...],
    blocking_conflicts: tuple[str, ...],
    active_blockers: tuple[ScopedBlocker, ...],
) -> tuple[str, ...]:
    limitations: list[str] = []
    if not validation_ok:
        limitations.append(
            f"{level.value} is unachieved because assessment validation failed."
        )
    if unmet_domains:
        names = ", ".join(str(item.domain_id) for item in unmet_domains)
        limitations.append(
            f"{level.value} has {len(unmet_domains)} mandatory domain(s) below "
            f"their required maturity threshold: {names}."
        )
    if unsatisfied_gates:
        limitations.append(
            f"{level.value} has {len(unsatisfied_gates)} required Hard-Gate(s) not "
            "yet satisfied at the configured gate maturity threshold."
        )
    if blocking_conflicts:
        limitations.append(
            f"{level.value} is blocked by {len(blocking_conflicts)} unresolved "
            "evidence conflict(s) touching a mandatory domain."
        )
    for blocker in active_blockers:
        refs = ", ".join(blocker.requirement_refs)
        suffix = f" (Requirement {refs})" if refs else ""
        limitations.append(f"{blocker.reason}{suffix}")
    return tuple(limitations)


def _build_rationale(
    *,
    level: TargetLevel,
    achieved: bool,
    is_hosted: bool,
    unmet_domains: tuple[UnmetDomain, ...],
    unsatisfied_gates: tuple[str, ...],
    next_gate: str | None,
    active_blockers: tuple[ScopedBlocker, ...],
    blocking_conflicts: tuple[str, ...],
    validation_ok: bool,
) -> str:
    parts: list[str] = []
    if achieved:
        parts.append(
            f"{level.value} is achieved: every mandatory domain reached its "
            "threshold, every required Hard-Gate is satisfied, no blocking "
            "conflict or scoped blocker applies, and validation succeeded."
        )
    else:
        reasons: list[str] = []
        if not validation_ok:
            reasons.append("validation failed")
        if unmet_domains:
            reasons.append(f"{len(unmet_domains)} mandatory domain(s) below threshold")
        if unsatisfied_gates:
            reasons.append(f"{len(unsatisfied_gates)} required Hard-Gate(s) unsatisfied")
        if blocking_conflicts:
            reasons.append(f"{len(blocking_conflicts)} blocking conflict(s)")
        if active_blockers:
            reasons.append(f"{len(active_blockers)} active scoped blocker(s)")
        parts.append(
            f"{level.value} is unachieved: " + "; ".join(reasons) + "."
        )
        if next_gate is not None:
            parts.append(f"Next Hard-Gate on the frontier is {next_gate}.")
    if is_hosted:
        parts.append(
            "Hosted adjacency reduces future application-porting effort but is "
            "not on any OS-substrate critical path and does not affect T1-T5."
        )
    parts.append(NON_AGGREGATE_STATEMENT)
    return " ".join(parts)
