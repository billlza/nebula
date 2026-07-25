"""Property 7: dependency validation precedes maturity capping.

This module holds the Hypothesis property tests for design Property 7. Every
test exercises the *real* Task 8.1 graph builder
(:func:`~tools.universe_os_gap_analysis.hard_gate_graph.build_hard_gate_graph`)
and the *real* Task 8.2 maturity assessor
(:func:`~tools.universe_os_gap_analysis.maturity.assess_domains`) with no mocks
and no reimplementation of the components under test.

Property 7 has three independent claims, each checked here against an oracle
recomputed from the requirement text rather than from the implementation, so the
assertions are not tautological:

* **Validation precedes capping (Requirements 3.4, 12.7).** Missing/out-of-range
  scores, unknown nodes, duplicate edges, self-edges, or cycles must make the
  builder fail closed with the correct ``GRF-*`` code *before* any maturity
  computation can run. Because ``build_hard_gate_graph`` raises, no
  ``HardGateGraph`` is produced and ``assess_domains`` cannot be reached.

* **Blocking-dependency cap (Requirements 3.4, 3.5).** For every valid DAG, each
  gate/domain effective score equals ``min(raw, every blocking dependency
  effective score)``, is never greater than its raw score, and is never greater
  than any blocking dependency's effective score. Non-blocking edges never cap.

* **Explicit join convergence (Requirement 12.7).** Independent parallel
  branches build only when they declare an explicit join gate; a join must be a
  genuine convergence point (two or more incoming dependency edges); and a
  parallel branch without a declared join fails closed.
"""

from __future__ import annotations

from hypothesis import HealthCheck, assume, given, settings, strategies as st

from tools.universe_os_gap_analysis.hard_gate_graph import (
    GRF_CYCLE,
    GRF_DUPLICATE_EDGE,
    GRF_ILLEGAL_BRANCH,
    GRF_ILLEGAL_JOIN,
    GRF_MISSING_SCORE,
    GRF_SCORE_RANGE,
    GRF_SELF_EDGE,
    GRF_UNKNOWN_NODE,
    GateDependencyEdge,
    HardGateGraph,
    HardGateGraphError,
    build_hard_gate_graph,
)
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.maturity import (
    DomainMaturityInput,
    assess_domains,
)
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    HardGate,
    LocationKind,
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_MIN_SCORE = 0
_MAX_SCORE = 5


# --------------------------------------------------------------------------- #
# Builders (thin wrappers over the real models; no component under test).     #
# --------------------------------------------------------------------------- #


def _gid(key: str) -> str:
    return str(stable_id("gate", "p7", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "p7", key))


def _gate(
    key: str,
    *,
    score: int = 0,
    dependencies: tuple[str, ...] = (),
    branch: str | None = None,
    joins: tuple[str, ...] = (),
) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=MaturityScore(score),
        dependency_ids=tuple(_gid(dep) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Test Owner",
        parallel_branch=branch,
        join_gate_ids=tuple(_gid(join) for join in joins),
    )


def _record(key: str, index: int, kind: EvidenceKind, status: EvidenceStatus) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", "p7", key, str(index), kind.value, status.value),
        claim_key=f"src:p7-{key}-{index}",
        claim=f"direct evidence {index} for {key}",
        status=status,
        source_path="tools/p7/evidence.py",
        location=SourceLocation(kind=LocationKind.SYMBOL, value=f"p7_{key}_{index}"),
        revision_ref="revision-property-7",
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.VALIDATED,
    )


def _domain_input(key: str, raw_target: int) -> DomainMaturityInput:
    """Build a domain whose real raw score equals ``raw_target`` (0..5).

    The rungs mirror the design rubric exactly (Requirement 3.1): no direct
    implementation evidence -> 0; a source+experimental record -> 1; adding a
    reproducible test-execution record -> 2; and the three score-3 signals plus
    supported-production / mature-ecosystem flags climb to 3, 4, and 5.
    """

    evidence: list[EvidenceRecord] = []
    if raw_target >= 1:
        evidence.append(_record(key, 1, EvidenceKind.SOURCE, EvidenceStatus.EXPERIMENTAL))
    if raw_target >= 2:
        evidence.append(
            _record(key, 2, EvidenceKind.TEST_EXECUTION, EvidenceStatus.REPO_PREVIEW)
        )
    return DomainMaturityInput(
        domain_id=_did(key),
        gate_id=_gid(key),
        direct_evidence=tuple(evidence),
        cross_host_candidate_contract=raw_target >= 3,
        migration_rollback=raw_target >= 3,
        release_review=raw_target >= 3,
        supported_production=raw_target >= 4,
        mature_ecosystem=raw_target >= 5,
    )


# --------------------------------------------------------------------------- #
# Independent oracle for the capping recurrence (derived from Requirement 3.4).#
# --------------------------------------------------------------------------- #


def _blocking_effective_oracle(
    scores: dict[str, int], blocking_deps: dict[str, list[str]]
) -> dict[str, int]:
    """Recompute effective = min(raw, blocking deps' effective) over the DAG.

    This is written independently of ``maturity._gate_effective_scores`` so the
    comparison against the real assessor is a genuine cross-check, not a
    tautology. The input graph is a validated DAG, so the memoized recursion
    terminates and visits every blocking dependency before its dependent.
    """

    memo: dict[str, int] = {}

    def effective(node: str) -> int:
        if node in memo:
            return memo[node]
        floor = scores[node]
        for dep in blocking_deps.get(node, ()):  # only blocking edges cap
            floor = min(floor, effective(dep))
        memo[node] = floor
        return floor

    return {node: effective(node) for node in scores}


# --------------------------------------------------------------------------- #
# Generators                                                                  #
# --------------------------------------------------------------------------- #


@st.composite
def _valid_dags(draw: st.DrawFn) -> tuple[tuple[HardGate, ...], tuple[GateDependencyEdge, ...], dict[str, int]]:
    """Draw a random valid DAG: topologically indexed nodes with back-edges only.

    Edges always run from a higher-indexed dependent to a lower-indexed
    dependency, which guarantees acyclicity. Each edge is independently marked
    blocking or non-blocking so the capping invariant is exercised for both.
    """

    n = draw(st.integers(min_value=1, max_value=8))
    keys = [f"k{i}" for i in range(n)]
    scores = {
        _gid(keys[i]): draw(st.integers(min_value=_MIN_SCORE, max_value=_MAX_SCORE))
        for i in range(n)
    }
    raw_targets = {
        keys[i]: draw(st.integers(min_value=0, max_value=5)) for i in range(n)
    }

    gates = tuple(
        _gate(keys[i], score=scores[_gid(keys[i])]) for i in range(n)
    )

    edges: list[GateDependencyEdge] = []
    for i in range(n):
        # Dependencies are a subset of strictly earlier nodes (keeps it a DAG).
        candidates = list(range(i))
        chosen = draw(
            st.lists(
                st.sampled_from(candidates) if candidates else st.nothing(),
                max_size=min(3, i),
                unique=True,
            )
        ) if candidates else []
        for j in chosen:
            blocking = draw(st.booleans())
            edges.append(
                GateDependencyEdge(
                    dependent_id=_gid(keys[i]),
                    dependency_id=_gid(keys[j]),
                    blocking=blocking,
                    cap_rationale=f"{keys[i]} capping edge on {keys[j]}",
                )
            )

    # Stash the raw targets on the tuple by returning them alongside.
    return gates, tuple(edges), raw_targets  # type: ignore[return-value]


# --------------------------------------------------------------------------- #
# Property 7 -- valid-DAG capping invariant (complex DAGs at 300 examples).   #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 7: Dependency validation precedes maturity capping - for every valid DAG each effective score is no greater than its raw score or any blocking dependency score, and non-blocking edges never cap.
# **Validates: Requirements 3.4, 3.5, 12.7**
@given(dag=_valid_dags())
@settings(
    max_examples=300,
    deadline=None,
    print_blob=True,
    suppress_health_check=[HealthCheck.too_slow],
)
def test_effective_score_is_capped_by_blocking_dependencies(dag) -> None:
    gates, edges, raw_targets = dag

    graph = build_hard_gate_graph(gates, edges=edges)
    assert isinstance(graph, HardGateGraph)

    # One domain per gate, keyed by the same "kN" key so we can tie a domain
    # result back to its gate id deterministically.
    keys = [f"k{i}" for i in range(len(gates))]
    gate_id_by_domain = {_did(key): _gid(key) for key in keys}
    inputs = tuple(_domain_input(key, raw_targets[key]) for key in keys)
    assessment = assess_domains(inputs, graph)

    # Independent oracle for the per-gate effective scores.
    blocking_deps: dict[str, list[str]] = {str(g.id): [] for g in gates}
    for edge in edges:
        if edge.blocking:
            blocking_deps[str(edge.dependent_id)].append(str(edge.dependency_id))
    gate_scores = {str(g.id): int(g.maturity_score) for g in gates}
    oracle = _blocking_effective_oracle(gate_scores, blocking_deps)

    # 1) The real per-gate effective scores match the independent oracle, and
    #    every gate effective score is capped by its blocking dependencies.
    for gate in gates:
        gid = str(gate.id)
        real = int(assessment.gate_effective_score_of(gid))
        assert real == oracle[gid]
        assert real <= gate_scores[gid]
        for dep in blocking_deps[gid]:
            assert real <= oracle[dep]

    # 2) Non-blocking edges never appear among the capping dependencies.
    non_blocking = {
        (str(e.dependent_id), str(e.dependency_id)) for e in edges if not e.blocking
    }
    for dependent, dependency in non_blocking:
        blocking_ids = {str(x) for x in graph.blocking_dependencies_of(dependent)}
        assert dependency not in blocking_ids

    # 3) Each domain's effective score equals min(raw, blocking deps effective),
    #    is never above its raw score, and never above any blocking dependency.
    for result in assessment.results:
        gate_id = gate_id_by_domain[str(result.domain_id)]
        raw = int(result.raw_score)
        eff = int(result.effective_score)
        assert eff <= raw
        dep_effectives = [oracle[dep] for dep in blocking_deps[gate_id]]
        for dep_eff in dep_effectives:
            assert eff <= dep_eff
        expected = min([raw, *dep_effectives]) if dep_effectives else raw
        assert eff == expected

    # 4) Raw scores were driven to their intended rung (sanity on the generator).
    for key in raw_targets:
        result = assessment.result_for(_did(key))
        assert result is not None
        assert int(result.raw_score) == raw_targets[key]


# --------------------------------------------------------------------------- #
# Property 7 -- validation precedes capping (fail closed before assessment).  #
# --------------------------------------------------------------------------- #


@st.composite
def _invalid_graphs(draw: st.DrawFn) -> tuple[str, tuple[HardGate, ...], dict]:
    """Draw an otherwise-buildable graph with exactly one injected defect.

    Returns ``(expected_code, gates, build_kwargs)``. Each defect corresponds to
    one of the structural failures Property 7 requires to be rejected before any
    maturity is computed.
    """

    defect = draw(
        st.sampled_from(
            [
                "score_range",
                "missing_score",
                "unknown_node",
                "self_edge",
                "duplicate_edge",
                "cycle",
            ]
        )
    )

    if defect == "score_range":
        gates = (_gate("a"), _gate("b", dependencies=("a",)))
        bad = draw(
            st.one_of(
                st.integers(min_value=-20, max_value=-1),
                st.integers(min_value=6, max_value=40),
            )
        )
        scores = {_gid("a"): 1, _gid("b"): bad}
        return GRF_SCORE_RANGE, gates, {"gate_scores": scores}

    if defect == "missing_score":
        gates = (_gate("a"), _gate("b"))
        # Provide a score for only one of the two gates (dict is non-empty but
        # incomplete), forcing the missing-score failure.
        scores = {_gid("a"): draw(st.integers(min_value=0, max_value=5))}
        return GRF_MISSING_SCORE, gates, {"gate_scores": scores}

    if defect == "unknown_node":
        gates = (_gate("a"),)
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("a"),
                dependency_id=_gid("ghost"),
                cap_rationale="edge to a missing node",
            ),
        )
        return GRF_UNKNOWN_NODE, gates, {"edges": edges}

    if defect == "self_edge":
        gates = (_gate("a"), _gate("b"))
        target = draw(st.sampled_from(["a", "b"]))
        edges = (
            GateDependencyEdge(
                dependent_id=_gid(target),
                dependency_id=_gid(target),
                cap_rationale="self dependency",
            ),
        )
        return GRF_SELF_EDGE, gates, {"edges": edges}

    if defect == "duplicate_edge":
        gates = (_gate("a"), _gate("b"))
        edge = GateDependencyEdge(
            dependent_id=_gid("b"),
            dependency_id=_gid("a"),
            cap_rationale="duplicated edge",
        )
        return GRF_DUPLICATE_EDGE, gates, {"edges": (edge, edge)}

    # cycle
    length = draw(st.integers(min_value=2, max_value=5))
    keys = [f"c{i}" for i in range(length)]
    gates = tuple(_gate(k) for k in keys)
    edges = tuple(
        GateDependencyEdge(
            dependent_id=_gid(keys[i]),
            dependency_id=_gid(keys[(i + 1) % length]),
            cap_rationale=f"cycle edge {i}",
        )
        for i in range(length)
    )
    return GRF_CYCLE, gates, {"edges": edges}


# Feature: nebula-universe-os-gap-analysis, Property 7: Dependency validation precedes maturity capping - missing/out-of-range scores, unknown nodes, duplicate edges, self-edges, or cycles invalidate the assessment before dependent scores are computed.
# **Validates: Requirements 3.4, 3.5, 12.7**
@given(spec=_invalid_graphs())
@settings(max_examples=300, deadline=None, print_blob=True)
def test_structural_defects_fail_closed_before_capping(spec) -> None:
    expected_code, gates, build_kwargs = spec

    graph = None
    try:
        graph = build_hard_gate_graph(gates, **build_kwargs)
    except HardGateGraphError as error:
        # The builder failed closed with the expected GRF-* code, so no graph
        # exists and maturity capping is never reached.
        assert error.code == expected_code
        assert graph is None
        return

    # Reaching here would mean an invalid graph was accepted -- fail the test.
    raise AssertionError(
        f"expected {expected_code} but build_hard_gate_graph accepted the graph"
    )


# --------------------------------------------------------------------------- #
# Property 7 -- explicit join convergence for independent branches (Req 12.7). #
# --------------------------------------------------------------------------- #


# Feature: nebula-universe-os-gap-analysis, Property 7: Dependency validation precedes maturity capping - independent branches converge only through explicit join gates.
# **Validates: Requirements 3.4, 3.5, 12.7**
@given(
    branch_count=st.integers(min_value=2, max_value=5),
    scores=st.lists(st.integers(min_value=0, max_value=5), min_size=2, max_size=5),
)
@settings(max_examples=150, deadline=None, print_blob=True)
def test_parallel_branches_converge_only_through_join(branch_count, scores) -> None:
    assume(len(scores) >= branch_count)

    branch_keys = [f"branch{i}" for i in range(branch_count)]
    gates = [_gate("root", score=5)]
    for i, key in enumerate(branch_keys):
        gates.append(
            _gate(
                key,
                score=scores[i],
                dependencies=("root",),
                branch=f"lane-{i}",
                joins=("join",),
            )
        )
    # The join is a genuine convergence point: it depends on every branch tip.
    gates.append(_gate("join", score=5, dependencies=tuple(branch_keys)))

    graph = build_hard_gate_graph(tuple(gates))

    # The declared join is reported as a join gate and converges every branch.
    assert graph.is_join_gate(_gid("join"))
    assert _gid("join") in graph.join_gate_ids
    join_deps = {str(x) for x in graph.dependencies_of(_gid("join"))}
    assert join_deps == {_gid(k) for k in branch_keys}

    # Each branch is a declared parallel branch, and no branch depends on another
    # branch -- they converge only at the explicit join gate.
    branches = graph.parallel_branches
    assert len(branches) == branch_count
    for i, key in enumerate(branch_keys):
        assert graph.branch_of(_gid(key)) == f"lane-{i}"
        deps = {str(x) for x in graph.dependencies_of(_gid(key))}
        assert deps == {_gid("root")}
        for other in branch_keys:
            if other != key:
                assert _gid(other) not in deps


# Feature: nebula-universe-os-gap-analysis, Property 7: Dependency validation precedes maturity capping - a parallel branch without an explicit join gate, or a join that is not a genuine convergence point, is rejected before capping.
# **Validates: Requirements 3.4, 3.5, 12.7**
@given(defect=st.sampled_from(["branch_without_join", "join_single_dependency"]))
@settings(max_examples=100, deadline=None, print_blob=True)
def test_illegal_branch_or_join_fails_closed(defect) -> None:
    if defect == "branch_without_join":
        gates = (
            _gate("root", score=3),
            _gate("branch-x", score=2, dependencies=("root",), branch="x"),
        )
        expected = GRF_ILLEGAL_BRANCH
    else:
        # A declared join with only a single incoming dependency edge is not a
        # real convergence point.
        gates = (
            _gate("p", score=2, joins=("join",)),
            _gate("join", score=2, dependencies=("p",)),
        )
        expected = GRF_ILLEGAL_JOIN

    try:
        build_hard_gate_graph(gates)
    except HardGateGraphError as error:
        assert error.code == expected
        return
    raise AssertionError(f"expected {expected} but the graph was accepted")


if __name__ == "__main__":
    # The verification virtualenv may lack a test runner; allow direct execution.
    test_effective_score_is_capped_by_blocking_dependencies()
    test_structural_defects_fail_closed_before_capping()
    test_parallel_branches_converge_only_through_join()
    test_illegal_branch_or_join_fails_closed()
    print("Property 7 OK: dependency validation precedes maturity capping")
