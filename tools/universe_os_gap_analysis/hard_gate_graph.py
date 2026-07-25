"""Hard-Gate dependency graph builder and pre-maturity validation (Task 8.1).

This module builds a validated Hard-Gate dependency graph and performs every
structural check *before* any maturity computation happens. It fails closed with
a ``GRF-*`` code on any of the following (Requirements 3.4, 3.5, 12.7):

* an edge referencing an **unknown node**;
* a **duplicate edge** (the same dependency appearing twice for one dependent);
* a **self edge** (a gate depending on itself);
* a **missing or out-of-range gate score** (0..5);
* a **cycle** in the dependency graph; and
* an **illegal branch/join** (a declared join that is not an actual convergence
  point, a join reference to an unknown or self node, or a parallel branch that
  never declares an explicit join gate).

Once validated, the graph exposes a read-only API for the Maturity Assessor
(Task 8.2) and target-achievement logic (Task 8.3):

* only **blocking** edges participate in capping, and *every* edge carries a
  human-readable **cap rationale** (Requirement 3.4);
* explicit **parallel branches** and **join gates** are reported (Requirement 12.7);
* the **unmet-gate frontier** and a **dependency-ordered path** are available for
  the capability matrix and roadmap (Requirement 3.3).

This module implements *neither* maturity capping (Task 8.2) nor target
achievement (Task 8.3); it only produces the validated graph they consume. It
does not mutate evidence, upgrade a status, or edit any evaluator module.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping

from .identifiers import ReferenceId, reference
from .models import HardGate, MaturityScore

# --------------------------------------------------------------------------- #
# GRF-* error codes (fail closed before maturity is computed).                #
# --------------------------------------------------------------------------- #
GRF_DUPLICATE_NODE = "GRF-DUPLICATE-NODE"
GRF_UNKNOWN_NODE = "GRF-UNKNOWN-NODE"
GRF_SELF_EDGE = "GRF-SELF-EDGE"
GRF_DUPLICATE_EDGE = "GRF-DUPLICATE-EDGE"
GRF_MISSING_RATIONALE = "GRF-MISSING-RATIONALE"
GRF_MISSING_SCORE = "GRF-MISSING-SCORE"
GRF_SCORE_RANGE = "GRF-SCORE-RANGE"
GRF_CYCLE = "GRF-CYCLE"
GRF_ILLEGAL_JOIN = "GRF-ILLEGAL-JOIN"
GRF_ILLEGAL_BRANCH = "GRF-ILLEGAL-BRANCH"

_MIN_SCORE = int(MaturityScore.ABSENT)
_MAX_SCORE = int(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM)


class HardGateGraphError(ValueError):
    """A fail-closed graph validation error carrying a ``GRF-*`` code and refs.

    ``object_refs`` names the affected gate/edge identifiers so the assessment
    validator can surface them alongside the requirement references.
    """

    def __init__(self, code: str, message: str, object_refs: Iterable[str] = ()) -> None:
        self.code = code
        self.object_refs = tuple(sorted({str(ref) for ref in object_refs}))
        detail = f", ".join(self.object_refs)
        suffix = f" [{detail}]" if detail else ""
        super().__init__(f"{code}: {message}{suffix}")


@dataclass(frozen=True, slots=True, kw_only=True)
class GateDependencyEdge:
    """A raw dependency edge input: ``dependent_id`` depends on ``dependency_id``.

    This raw form intentionally performs only light type checks so the graph
    builder is the single place that fails closed with ``GRF-*`` codes for
    structural problems (self edges, duplicates, unknown nodes, empty rationale).
    """

    dependent_id: str
    dependency_id: str
    blocking: bool = True
    cap_rationale: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.dependent_id, str) or not self.dependent_id:
            raise TypeError("dependent_id must be a non-empty string")
        if not isinstance(self.dependency_id, str) or not self.dependency_id:
            raise TypeError("dependency_id must be a non-empty string")
        if not isinstance(self.blocking, bool):
            raise TypeError("blocking must be a bool")
        if not isinstance(self.cap_rationale, str):
            raise TypeError("cap_rationale must be a string")


@dataclass(frozen=True, slots=True, kw_only=True)
class GateEdge:
    """A validated dependency edge with a mandatory, non-empty cap rationale."""

    dependent_id: ReferenceId
    dependency_id: ReferenceId
    blocking: bool
    cap_rationale: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "dependent_id", reference(self.dependent_id))
        object.__setattr__(self, "dependency_id", reference(self.dependency_id))
        if not isinstance(self.blocking, bool):
            raise TypeError("blocking must be a bool")
        if not isinstance(self.cap_rationale, str) or not self.cap_rationale.strip():
            raise ValueError("cap_rationale must be a non-empty string")

    @property
    def key(self) -> tuple[str, str]:
        return (str(self.dependent_id), str(self.dependency_id))


@dataclass(frozen=True, slots=True)
class HardGateGraph:
    """A validated Hard-Gate dependency graph ready for maturity capping.

    Instances are only produced by :func:`build_hard_gate_graph`, which runs all
    ``GRF-*`` validation first. The API here is read-only and deterministic
    (every collection is sorted by stable identifier).
    """

    nodes: Mapping[str, HardGate]
    edges: tuple[GateEdge, ...]
    scores: Mapping[str, MaturityScore]
    _dependents: Mapping[str, tuple[str, ...]] = field(default_factory=dict)
    _dependencies: Mapping[str, tuple[str, ...]] = field(default_factory=dict)
    _edge_by_key: Mapping[tuple[str, str], GateEdge] = field(default_factory=dict)

    # -- basic accessors -------------------------------------------------- #

    @property
    def gate_ids(self) -> tuple[str, ...]:
        return tuple(sorted(self.nodes))

    def gate(self, gate_id: str) -> HardGate:
        return self.nodes[str(gate_id)]

    def score_of(self, gate_id: str) -> MaturityScore:
        return self.scores[str(gate_id)]

    def dependencies_of(self, gate_id: str, *, blocking_only: bool = False) -> tuple[ReferenceId, ...]:
        """Return the prerequisite gate ids for ``gate_id`` (dependency edges)."""

        key = str(gate_id)
        if key not in self.nodes:
            raise KeyError(f"unknown gate id: {gate_id!r}")
        deps = self._dependencies.get(key, ())
        if blocking_only:
            deps = tuple(
                dep for dep in deps if self._edge_by_key[(key, dep)].blocking
            )
        return tuple(reference(dep) for dep in deps)

    def blocking_dependencies_of(self, gate_id: str) -> tuple[ReferenceId, ...]:
        """Return only the *blocking* prerequisite gate ids (participate in cap)."""

        return self.dependencies_of(gate_id, blocking_only=True)

    def dependents_of(self, gate_id: str) -> tuple[ReferenceId, ...]:
        key = str(gate_id)
        if key not in self.nodes:
            raise KeyError(f"unknown gate id: {gate_id!r}")
        return tuple(reference(dep) for dep in self._dependents.get(key, ()))

    def cap_rationale(self, dependent_id: str, dependency_id: str) -> str:
        """Return the recorded cap rationale for a specific edge."""

        return self._edge_by_key[(str(dependent_id), str(dependency_id))].cap_rationale

    def edge(self, dependent_id: str, dependency_id: str) -> GateEdge:
        return self._edge_by_key[(str(dependent_id), str(dependency_id))]

    # -- parallel branches and join gates (Requirement 12.7) -------------- #

    @property
    def parallel_branches(self) -> Mapping[str, tuple[str, ...]]:
        """Return ``branch label -> sorted gate ids`` for every declared branch."""

        branches: dict[str, list[str]] = {}
        for gate_id in sorted(self.nodes):
            label = self.nodes[gate_id].parallel_branch
            if label is not None:
                branches.setdefault(label, []).append(gate_id)
        return {label: tuple(sorted(ids)) for label, ids in sorted(branches.items())}

    @property
    def join_gate_ids(self) -> tuple[str, ...]:
        """Return the sorted set of gate ids that are declared convergence points."""

        joins: set[str] = set()
        for gate in self.nodes.values():
            for join_id in gate.join_gate_ids:
                joins.add(str(join_id))
        return tuple(sorted(joins))

    def is_join_gate(self, gate_id: str) -> bool:
        return str(gate_id) in set(self.join_gate_ids)

    def branch_of(self, gate_id: str) -> str | None:
        return self.nodes[str(gate_id)].parallel_branch

    # -- ordering and frontier (Requirement 3.3) -------------------------- #

    def dependency_ordered_path(self) -> tuple[str, ...]:
        """Return a deterministic topological order: dependencies before dependents."""

        indegree = {gate_id: len(self._dependencies.get(gate_id, ())) for gate_id in self.nodes}
        ready = sorted(gate_id for gate_id, degree in indegree.items() if degree == 0)
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
        # A validated graph is acyclic, so every node is emitted exactly once.
        return tuple(order)

    def unmet_gate_frontier(self, satisfied: Iterable[str] = ()) -> tuple[ReferenceId, ...]:
        """Return unmet gates whose dependencies are all satisfied.

        ``satisfied`` is the set of gate ids a caller (Task 8.2/8.3) considers
        already met. The frontier is the leading edge of remaining work: every
        gate that is not yet satisfied but all of whose prerequisites are. With
        an empty ``satisfied`` set this is exactly the set of root gates.
        """

        satisfied_ids = {str(gate_id) for gate_id in satisfied}
        unknown = satisfied_ids - set(self.nodes)
        if unknown:
            raise KeyError(f"unknown satisfied gate ids: {sorted(unknown)}")
        frontier: list[str] = []
        for gate_id in self.nodes:
            if gate_id in satisfied_ids:
                continue
            deps = self._dependencies.get(gate_id, ())
            if all(dep in satisfied_ids for dep in deps):
                frontier.append(gate_id)
        return tuple(reference(gate_id) for gate_id in sorted(frontier))


def _default_cap_rationale(dependent: HardGate, dependency: HardGate) -> str:
    return (
        f"{dependent.title} cannot exceed the maturity of its blocking dependency "
        f"{dependency.title} ({dependency.id}); this edge participates in capping."
    )


def _resolve_score(
    gate: HardGate, gate_scores: Mapping[str, int] | None
) -> MaturityScore:
    gate_id = str(gate.id)
    if gate_scores is None:
        raw: int = int(gate.maturity_score)
    else:
        if gate_id not in gate_scores:
            raise HardGateGraphError(
                GRF_MISSING_SCORE,
                "hard gate has no maturity score before capping",
                (gate_id,),
            )
        candidate = gate_scores[gate_id]
        if isinstance(candidate, bool) or not isinstance(candidate, int):
            raise HardGateGraphError(
                GRF_SCORE_RANGE,
                "gate score must be an integer from 0 through 5",
                (gate_id,),
            )
        raw = candidate
    if raw < _MIN_SCORE or raw > _MAX_SCORE:
        raise HardGateGraphError(
            GRF_SCORE_RANGE,
            f"gate score {raw} is outside the 0..5 maturity range",
            (gate_id,),
        )
    return MaturityScore(raw)


def build_hard_gate_graph(
    gates: Iterable[HardGate],
    *,
    edges: Iterable[GateDependencyEdge] | None = None,
    gate_scores: Mapping[str, int] | None = None,
) -> HardGateGraph:
    """Build and fully validate a Hard-Gate dependency graph.

    All ``GRF-*`` validation runs before returning, so no maturity capping (Task
    8.2) or target achievement (Task 8.3) can proceed on an invalid graph.

    Args:
        gates: the Hard-Gate nodes.
        edges: optional explicit dependency edges. When omitted, edges are
            derived from each gate's ``dependency_ids`` as *blocking* edges with
            a generated cap rationale.
        gate_scores: optional override of per-gate maturity scores keyed by gate
            id. When omitted, scores come from each gate's ``maturity_score``.

    Raises:
        HardGateGraphError: on any structural problem, with a ``GRF-*`` code.
    """

    node_list = tuple(gates)
    for node in node_list:
        if not isinstance(node, HardGate):
            raise TypeError("gates must contain HardGate values")

    # 1) Unique nodes.
    seen_ids: set[str] = set()
    duplicate_ids: set[str] = set()
    for node in node_list:
        gid = str(node.id)
        if gid in seen_ids:
            duplicate_ids.add(gid)
        seen_ids.add(gid)
    if duplicate_ids:
        raise HardGateGraphError(
            GRF_DUPLICATE_NODE, "duplicate hard gate ids", duplicate_ids
        )
    nodes: dict[str, HardGate] = {str(node.id): node for node in node_list}

    # 2) Scores present and in range (before any capping).
    if gate_scores is not None:
        unknown_scores = {str(k) for k in gate_scores} - set(nodes)
        if unknown_scores:
            raise HardGateGraphError(
                GRF_UNKNOWN_NODE,
                "gate score provided for unknown gate id",
                unknown_scores,
            )
    scores: dict[str, MaturityScore] = {
        gid: _resolve_score(node, gate_scores) for gid, node in nodes.items()
    }

    # 3) Edges: unknown node, self edge, duplicate edge, missing rationale.
    if edges is None:
        raw_edges: list[GateDependencyEdge] = []
        for node in node_list:
            dependent = str(node.id)
            for dep in node.dependency_ids:
                dependency = str(dep)
                rationale = ""
                if dependency in nodes:
                    rationale = _default_cap_rationale(node, nodes[dependency])
                raw_edges.append(
                    GateDependencyEdge(
                        dependent_id=dependent,
                        dependency_id=dependency,
                        blocking=True,
                        cap_rationale=rationale,
                    )
                )
    else:
        raw_edges = list(edges)
        for raw in raw_edges:
            if not isinstance(raw, GateDependencyEdge):
                raise TypeError("edges must contain GateDependencyEdge values")

    validated_edges: list[GateEdge] = []
    edge_keys: set[tuple[str, str]] = set()
    for raw in raw_edges:
        dependent = raw.dependent_id
        dependency = raw.dependency_id
        if dependent not in nodes:
            raise HardGateGraphError(
                GRF_UNKNOWN_NODE, "edge references unknown dependent gate", (dependent,)
            )
        if dependency not in nodes:
            raise HardGateGraphError(
                GRF_UNKNOWN_NODE, "edge references unknown dependency gate", (dependency,)
            )
        if dependent == dependency:
            raise HardGateGraphError(
                GRF_SELF_EDGE, "a gate cannot depend on itself", (dependent,)
            )
        key = (dependent, dependency)
        if key in edge_keys:
            raise HardGateGraphError(
                GRF_DUPLICATE_EDGE,
                "duplicate dependency edge",
                (f"{dependent}->{dependency}",),
            )
        edge_keys.add(key)
        if not raw.cap_rationale.strip():
            raise HardGateGraphError(
                GRF_MISSING_RATIONALE,
                "every dependency edge must carry a cap rationale",
                (f"{dependent}->{dependency}",),
            )
        validated_edges.append(
            GateEdge(
                dependent_id=dependent,
                dependency_id=dependency,
                blocking=raw.blocking,
                cap_rationale=raw.cap_rationale,
            )
        )

    # Build adjacency (sorted, deterministic).
    dependencies: dict[str, list[str]] = {gid: [] for gid in nodes}
    dependents: dict[str, list[str]] = {gid: [] for gid in nodes}
    edge_by_key: dict[tuple[str, str], GateEdge] = {}
    for validated in validated_edges:
        dependent = str(validated.dependent_id)
        dependency = str(validated.dependency_id)
        dependencies[dependent].append(dependency)
        dependents[dependency].append(dependent)
        edge_by_key[validated.key] = validated

    # 4) Illegal branch/join validation (Requirement 12.7).
    _validate_branches_and_joins(nodes, edge_by_key)

    # 5) Cycle detection (must precede any capping).
    _reject_cycles(nodes, dependencies)

    return HardGateGraph(
        nodes=dict(nodes),
        edges=tuple(sorted(validated_edges, key=lambda item: item.key)),
        scores=dict(scores),
        _dependents={gid: tuple(sorted(vals)) for gid, vals in dependents.items()},
        _dependencies={gid: tuple(sorted(vals)) for gid, vals in dependencies.items()},
        _edge_by_key=dict(edge_by_key),
    )


def _validate_branches_and_joins(
    nodes: Mapping[str, HardGate],
    edge_by_key: Mapping[tuple[str, str], GateEdge],
) -> None:
    """Reject illegal branch/join structure before maturity is computed."""

    # Count incoming dependency edges per gate (a join must converge 2+ inputs).
    indegree: dict[str, int] = {gid: 0 for gid in nodes}
    for dependent, _dependency in edge_by_key:
        indegree[dependent] += 1

    declared_joins: set[str] = set()
    for gate_id, gate in nodes.items():
        for join_ref in gate.join_gate_ids:
            join_id = str(join_ref)
            declared_joins.add(join_id)
            if join_id not in nodes:
                raise HardGateGraphError(
                    GRF_ILLEGAL_JOIN,
                    "join gate references an unknown node",
                    (gate_id, join_id),
                )
            if join_id == gate_id:
                raise HardGateGraphError(
                    GRF_ILLEGAL_JOIN, "a gate cannot join itself", (gate_id,)
                )
            # The declaring gate must be an actual dependency of the join gate.
            if (join_id, gate_id) not in edge_by_key:
                raise HardGateGraphError(
                    GRF_ILLEGAL_JOIN,
                    "join gate must depend on the gate that declares it",
                    (gate_id, join_id),
                )

    # A declared join must be a genuine convergence point (2+ dependencies).
    for join_id in sorted(declared_joins):
        if indegree.get(join_id, 0) < 2:
            raise HardGateGraphError(
                GRF_ILLEGAL_JOIN,
                "a join gate must converge at least two dependency edges",
                (join_id,),
            )

    # Every parallel branch must declare an explicit join gate (Requirement 12.7).
    branch_members: dict[str, list[str]] = {}
    for gate_id, gate in nodes.items():
        if gate.parallel_branch is not None:
            branch_members.setdefault(gate.parallel_branch, []).append(gate_id)
    for label, members in sorted(branch_members.items()):
        if not any(nodes[member].join_gate_ids for member in members):
            raise HardGateGraphError(
                GRF_ILLEGAL_BRANCH,
                f"parallel branch {label!r} declares no explicit join gate",
                members,
            )


def _reject_cycles(
    nodes: Mapping[str, HardGate], dependencies: Mapping[str, list[str]]
) -> None:
    """Fail closed with ``GRF-CYCLE`` if the dependency graph is not a DAG."""

    WHITE, GRAY, BLACK = 0, 1, 2
    color: dict[str, int] = {gid: WHITE for gid in nodes}

    def visit(node_id: str, stack: list[str]) -> None:
        color[node_id] = GRAY
        stack.append(node_id)
        for dependency in sorted(dependencies.get(node_id, ())):
            if color[dependency] == GRAY:
                cycle = stack[stack.index(dependency):] + [dependency]
                raise HardGateGraphError(
                    GRF_CYCLE, "dependency cycle detected", cycle
                )
            if color[dependency] == WHITE:
                visit(dependency, stack)
        stack.pop()
        color[node_id] = BLACK

    for gate_id in sorted(nodes):
        if color[gate_id] == WHITE:
            visit(gate_id, [])
