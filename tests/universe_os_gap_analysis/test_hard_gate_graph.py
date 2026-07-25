"""Unit tests for the Hard-Gate graph builder and pre-maturity validation (Task 8.1).

These exercise Requirements 3.3, 3.4, 3.5, 12.5, 12.6, and 12.7 against the real
models (no mocks):

* every ``GRF-*`` structural failure (unknown node, duplicate/self edge, missing
  or out-of-range score, cycle, illegal branch/join) is rejected *before* any
  maturity computation; and
* a validated graph exposes blocking-only capping edges with cap rationale,
  explicit parallel branches, join gates, the unmet-gate frontier, and a
  dependency-ordered path.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.evaluators.boot import JOIN_GATE_KEY, evaluate_boot
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.hard_gate_graph import (
    GRF_CYCLE,
    GRF_DUPLICATE_EDGE,
    GRF_DUPLICATE_NODE,
    GRF_ILLEGAL_BRANCH,
    GRF_ILLEGAL_JOIN,
    GRF_MISSING_RATIONALE,
    GRF_MISSING_SCORE,
    GRF_SCORE_RANGE,
    GRF_SELF_EDGE,
    GRF_UNKNOWN_NODE,
    GateDependencyEdge,
    HardGateGraphError,
    build_hard_gate_graph,
)
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.models import (
    EvidenceStatus,
    HardGate,
    MaturityScore,
    TargetLevel,
)


def _gate(
    key: str,
    *,
    dependencies: tuple[str, ...] = (),
    branch: str | None = None,
    joins: tuple[str, ...] = (),
    score: MaturityScore = MaturityScore.ABSENT,
    target_level: TargetLevel = TargetLevel.T2_FREESTANDING_SUBSTRATE,
) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=target_level,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=score,
        dependency_ids=tuple(_gid(dep) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Test Owner",
        parallel_branch=branch,
        join_gate_ids=tuple(_gid(join) for join in joins),
    )


def _gid(key: str) -> str:
    return str(stable_id("gate", "test", key))


class BuildValidGraphTests(unittest.TestCase):
    def _linear_gates(self) -> tuple[HardGate, ...]:
        return (
            _gate("a", score=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION),
            _gate("b", dependencies=("a",), score=MaturityScore.NARROW_EXPERIMENT),
            _gate("c", dependencies=("b",)),
        )

    def test_dependency_ordered_path_places_dependencies_first(self) -> None:
        graph = build_hard_gate_graph(self._linear_gates())
        order = graph.dependency_ordered_path()
        self.assertEqual(order, (_gid("a"), _gid("b"), _gid("c")))
        self.assertEqual(set(order), set(graph.gate_ids))

    def test_blocking_dependencies_and_cap_rationale(self) -> None:
        graph = build_hard_gate_graph(self._linear_gates())
        blocking = graph.blocking_dependencies_of(_gid("b"))
        self.assertEqual(tuple(str(x) for x in blocking), (_gid("a"),))
        rationale = graph.cap_rationale(_gid("b"), _gid("a"))
        self.assertTrue(rationale.strip())
        self.assertIn("capping", rationale)

    def test_only_blocking_edges_participate_in_capping(self) -> None:
        gates = (_gate("a"), _gate("b", dependencies=("a",)))
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("b"),
                dependency_id=_gid("a"),
                blocking=False,
                cap_rationale="Associated only; does not cap maturity.",
            ),
        )
        graph = build_hard_gate_graph(gates, edges=edges)
        self.assertEqual(graph.blocking_dependencies_of(_gid("b")), ())
        # Non-blocking edge still appears among all dependencies.
        self.assertEqual(
            tuple(str(x) for x in graph.dependencies_of(_gid("b"))), (_gid("a"),)
        )

    def test_unmet_gate_frontier_advances_with_satisfied_set(self) -> None:
        graph = build_hard_gate_graph(self._linear_gates())
        # With nothing satisfied, only the root gate is on the frontier.
        self.assertEqual(
            tuple(str(x) for x in graph.unmet_gate_frontier()), (_gid("a"),)
        )
        # Satisfying the root advances the frontier to its dependent.
        self.assertEqual(
            tuple(str(x) for x in graph.unmet_gate_frontier(satisfied=(_gid("a"),))),
            (_gid("b"),),
        )

    def test_frontier_rejects_unknown_satisfied_id(self) -> None:
        graph = build_hard_gate_graph(self._linear_gates())
        with self.assertRaises(KeyError):
            graph.unmet_gate_frontier(satisfied=("gate-does-not-exist",))

    def test_parallel_branches_and_join_gate_reported(self) -> None:
        gates = (
            _gate("root"),
            _gate("branch-x", dependencies=("root",), branch="x", joins=("join",)),
            _gate("branch-y", dependencies=("root",), branch="y", joins=("join",)),
            _gate("join", dependencies=("branch-x", "branch-y")),
        )
        graph = build_hard_gate_graph(gates)
        branches = graph.parallel_branches
        self.assertEqual(set(branches), {"x", "y"})
        self.assertEqual(branches["x"], (_gid("branch-x"),))
        self.assertEqual(graph.join_gate_ids, (_gid("join"),))
        self.assertTrue(graph.is_join_gate(_gid("join")))
        self.assertEqual(graph.branch_of(_gid("branch-x")), "x")


class ScoreValidationTests(unittest.TestCase):
    def test_scores_default_from_gate_maturity(self) -> None:
        gates = (_gate("a", score=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY),)
        graph = build_hard_gate_graph(gates)
        self.assertEqual(
            graph.score_of(_gid("a")), MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY
        )

    def test_missing_score_override_fails_closed(self) -> None:
        gates = (_gate("a"), _gate("b", dependencies=("a",)))
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, gate_scores={_gid("a"): 2})
        self.assertEqual(ctx.exception.code, GRF_MISSING_SCORE)

    def test_out_of_range_score_fails_closed(self) -> None:
        gates = (_gate("a"),)
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, gate_scores={_gid("a"): 9})
        self.assertEqual(ctx.exception.code, GRF_SCORE_RANGE)

    def test_score_override_for_unknown_gate_fails_closed(self) -> None:
        gates = (_gate("a"),)
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, gate_scores={_gid("a"): 1, "gate-unknown": 2})
        self.assertEqual(ctx.exception.code, GRF_UNKNOWN_NODE)


class EdgeValidationTests(unittest.TestCase):
    def test_duplicate_node_fails_closed(self) -> None:
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph((_gate("a"), _gate("a")))
        self.assertEqual(ctx.exception.code, GRF_DUPLICATE_NODE)

    def test_unknown_dependency_node_fails_closed(self) -> None:
        gates = (_gate("a"),)
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("a"),
                dependency_id="gate-missing",
                cap_rationale="x",
            ),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, edges=edges)
        self.assertEqual(ctx.exception.code, GRF_UNKNOWN_NODE)

    def test_self_edge_fails_closed(self) -> None:
        gates = (_gate("a"),)
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("a"), dependency_id=_gid("a"), cap_rationale="x"
            ),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, edges=edges)
        self.assertEqual(ctx.exception.code, GRF_SELF_EDGE)

    def test_duplicate_edge_fails_closed(self) -> None:
        gates = (_gate("a"), _gate("b"))
        edge = GateDependencyEdge(
            dependent_id=_gid("b"), dependency_id=_gid("a"), cap_rationale="x"
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, edges=(edge, edge))
        self.assertEqual(ctx.exception.code, GRF_DUPLICATE_EDGE)

    def test_missing_cap_rationale_fails_closed(self) -> None:
        gates = (_gate("a"), _gate("b"))
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("b"), dependency_id=_gid("a"), cap_rationale="   "
            ),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, edges=edges)
        self.assertEqual(ctx.exception.code, GRF_MISSING_RATIONALE)


class CycleValidationTests(unittest.TestCase):
    def test_two_node_cycle_fails_closed(self) -> None:
        gates = (_gate("a"), _gate("b"))
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("a"), dependency_id=_gid("b"), cap_rationale="x"
            ),
            GateDependencyEdge(
                dependent_id=_gid("b"), dependency_id=_gid("a"), cap_rationale="y"
            ),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates, edges=edges)
        self.assertEqual(ctx.exception.code, GRF_CYCLE)

    def test_self_dependency_via_model_is_a_cycle_free_dag(self) -> None:
        # A legitimate diamond DAG builds successfully.
        gates = (
            _gate("root"),
            _gate("left", dependencies=("root",)),
            _gate("right", dependencies=("root",)),
            _gate("sink", dependencies=("left", "right")),
        )
        graph = build_hard_gate_graph(gates)
        order = graph.dependency_ordered_path()
        self.assertLess(order.index(_gid("root")), order.index(_gid("sink")))


class BranchJoinValidationTests(unittest.TestCase):
    def test_join_must_depend_on_declaring_gate(self) -> None:
        # branch-x declares the join, but join does not depend on branch-x.
        gates = (
            _gate("root"),
            _gate("branch-x", dependencies=("root",), branch="x", joins=("join",)),
            _gate("branch-y", dependencies=("root",), branch="y", joins=("join",)),
            _gate("join", dependencies=("branch-y", "root")),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates)
        self.assertEqual(ctx.exception.code, GRF_ILLEGAL_JOIN)

    def test_self_join_fails_closed(self) -> None:
        gates = (
            _gate("root"),
            _gate("j", dependencies=("root",), joins=("j",)),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates)
        self.assertEqual(ctx.exception.code, GRF_ILLEGAL_JOIN)

    def test_join_reference_to_unknown_node_fails_closed(self) -> None:
        gate = HardGate(
            id=_gid("a"),
            title="Gate a",
            target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
            status=EvidenceStatus.UNSUPPORTED,
            maturity_score=MaturityScore.ABSENT,
            dependency_ids=(),
            blocking_domain_ids=(),
            evidence_ids=(),
            acceptance_evidence=("Acceptance evidence for a.",),
            non_claims=(),
            owner_area="Test Owner",
            parallel_branch=None,
            join_gate_ids=("gate-unknown-join",),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph((gate,))
        self.assertEqual(ctx.exception.code, GRF_ILLEGAL_JOIN)

    def test_join_with_single_dependency_is_illegal(self) -> None:
        gates = (
            _gate("p", joins=("join",)),
            _gate("join", dependencies=("p",)),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates)
        self.assertEqual(ctx.exception.code, GRF_ILLEGAL_JOIN)

    def test_parallel_branch_without_join_is_illegal(self) -> None:
        gates = (
            _gate("root"),
            _gate("branch-x", dependencies=("root",), branch="x"),
        )
        with self.assertRaises(HardGateGraphError) as ctx:
            build_hard_gate_graph(gates)
        self.assertEqual(ctx.exception.code, GRF_ILLEGAL_BRANCH)


class RealBootGraphTests(unittest.TestCase):
    """The graph builder must accept the real boot evaluator's Hard-Gate output."""

    def test_boot_hard_gates_form_a_valid_graph(self) -> None:
        evaluation = evaluate_boot(EvidenceBundle(records=(), by_claim_key={}))
        graph = build_hard_gate_graph(evaluation.hard_gates)

        # The deterministic-linked-ELF gate is the explicit join point.
        join_id = str(stable_id("gate", "boot", JOIN_GATE_KEY))
        self.assertIn(join_id, graph.join_gate_ids)
        self.assertTrue(graph.is_join_gate(join_id))

        # Two parallel branches are reported.
        self.assertEqual(
            set(graph.parallel_branches), {"backend-bootstrap", "boot-toolchain"}
        )

        # The ordering places the join before the downstream boot media/QEMU.
        order = graph.dependency_ordered_path()
        media_id = str(stable_id("gate", "boot", "boot-media"))
        qemu_id = str(stable_id("gate", "boot", "qemu-execution"))
        self.assertLess(order.index(join_id), order.index(media_id))
        self.assertLess(order.index(media_id), order.index(qemu_id))

        # With nothing satisfied, the frontier is the set of root gates.
        frontier = {str(x) for x in graph.unmet_gate_frontier()}
        self.assertIn(str(stable_id("gate", "boot", "low-level-soundness")), frontier)
        self.assertIn(str(stable_id("gate", "boot", "primitive-et-rel-object")), frontier)


if __name__ == "__main__":
    unittest.main()
