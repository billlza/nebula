"""Unit tests for gap priority ranking and the parallel roadmap (Task 9.2).

These tests exercise, against the real
:mod:`tools.universe_os_gap_analysis.roadmap` module (no mocks):

* strict lexicographic gap ranking over
  ``(dependency_criticality, safety_impact, claim_risk, target_unblock_value,
  stable_id)`` with no heterogeneous summation and stable-id tie-breaking, order
  independent of input (Requirement 12.4);
* the two independent gate lanes -- pre-kernel and post-boot -- covering the
  Requirement 12.5 pre-kernel sequence and the Requirement 12.6 post-boot gates;
* explicit parallel branches converging on explicit join gates
  (Requirement 12.7) and the shortest evidence-backed dependency ordering
  (Requirement 15.7);
* the gate frontier and dependency-ordered path; and
* the strict separation of observed facts from recommendations
  (Requirement 14.7), plus fail-closed ``RMP-*`` validation.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.gap_register import build_gap_register
from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.models import (
    EvidenceStatus,
    GapCategory,
    GapEntry,
    Severity,
    TargetLevel,
)
from tools.universe_os_gap_analysis.roadmap import (
    RMP_CYCLE,
    RMP_DUPLICATE_WORKSTREAM,
    RMP_ILLEGAL_JOIN,
    RMP_SELF_DEPENDENCY,
    RMP_UNKNOWN_DEPENDENCY,
    GapRanking,
    GapRoadmap,
    ParallelRoadmap,
    RoadmapError,
    RoadmapLane,
    RoadmapWorkstream,
    build_gap_roadmap,
    build_parallel_roadmap,
    gap_priority_key,
    rank_gap_register,
    rank_gaps,
)


def _gap(
    *,
    gap_id: str,
    dependency_criticality: int = 1,
    safety_impact: int = 1,
    claim_risk: int = 1,
    target_unblock_value: int = 1,
    primary: GapCategory = GapCategory.IMPLEMENTATION,
) -> GapEntry:
    return GapEntry(
        id=StableId(gap_id),
        title="Example gap",
        primary_category=primary,
        secondary_categories=(),
        domain_ids=("domain-example",),
        current_status=EvidenceStatus.UNSUPPORTED,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        severity=Severity.HIGH,
        dependencies=(),
        acceptance_evidence=("Direct implementation evidence closing the gap.",),
        recommended_owner_area="Kernel",
        dependency_criticality=dependency_criticality,
        safety_impact=safety_impact,
        claim_risk=claim_risk,
        target_unblock_value=target_unblock_value,
        observed_fact="The capability has no direct implementation evidence.",
        recommendation="Implement and verify the capability before depending on it.",
    )


class GapPriorityRankingTests(unittest.TestCase):
    def test_higher_dimension_ranks_first_lexicographically(self) -> None:
        low = _gap(gap_id="gap-low", dependency_criticality=1)
        high = _gap(gap_id="gap-high", dependency_criticality=5)
        ranked = rank_gaps([low, high])
        self.assertEqual([str(g.id) for g in ranked], ["gap-high", "gap-low"])

    def test_dimensions_are_compared_in_order_not_summed(self) -> None:
        # A gap with a huge secondary sum but lower primary dimension must still
        # rank below a gap with a higher primary dimension (no summation).
        primary_wins = _gap(
            gap_id="gap-primary",
            dependency_criticality=3,
            safety_impact=0,
            claim_risk=0,
            target_unblock_value=0,
        )
        big_sum = _gap(
            gap_id="gap-sum",
            dependency_criticality=2,
            safety_impact=9,
            claim_risk=9,
            target_unblock_value=9,
        )
        ranked = rank_gaps([big_sum, primary_wins])
        self.assertEqual([str(g.id) for g in ranked], ["gap-primary", "gap-sum"])

    def test_ties_broken_by_stable_id_ascending(self) -> None:
        a = _gap(gap_id="gap-aaa")
        b = _gap(gap_id="gap-bbb")
        c = _gap(gap_id="gap-ccc")
        ranked = rank_gaps([c, a, b])
        self.assertEqual([str(g.id) for g in ranked], ["gap-aaa", "gap-bbb", "gap-ccc"])

    def test_ranking_is_independent_of_input_order(self) -> None:
        gaps = [
            _gap(gap_id="gap-1", dependency_criticality=2, safety_impact=3),
            _gap(gap_id="gap-2", dependency_criticality=2, safety_impact=1),
            _gap(gap_id="gap-3", dependency_criticality=4, safety_impact=0),
        ]
        forward = [str(g.id) for g in rank_gaps(gaps)]
        reverse = [str(g.id) for g in rank_gaps(list(reversed(gaps)))]
        self.assertEqual(forward, reverse)

    def test_priority_key_shape_and_negation(self) -> None:
        gap = _gap(
            gap_id="gap-key",
            dependency_criticality=2,
            safety_impact=3,
            claim_risk=4,
            target_unblock_value=5,
        )
        self.assertEqual(gap_priority_key(gap), (-2, -3, -4, -5, "gap-key"))

    def test_rank_gap_register_and_rank_of(self) -> None:
        register = build_gap_register(
            _gap(gap_id="gap-a", dependency_criticality=1),
            _gap(gap_id="gap-b", dependency_criticality=5),
        )
        ranking = rank_gap_register(register)
        self.assertIsInstance(ranking, GapRanking)
        self.assertEqual(ranking.rank_of("gap-b"), 0)
        self.assertEqual(ranking.rank_of("gap-a"), 1)
        with self.assertRaises(KeyError):
            ranking.rank_of("gap-missing")

    def test_ranking_facts_and_recommendations_are_separate(self) -> None:
        ranking = GapRanking(ranked_gaps=(_gap(gap_id="gap-x"),))
        facts = dict(ranking.observed_facts())
        recs = dict(ranking.recommendations())
        self.assertIn("gap-x", {str(k) for k in facts})
        self.assertNotEqual(facts, recs)


class ParallelRoadmapStructureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.roadmap = build_parallel_roadmap()

    def test_two_independent_lanes_are_populated(self) -> None:
        self.assertTrue(self.roadmap.pre_kernel)
        self.assertTrue(self.roadmap.post_boot)
        pre_ids = {str(w.id) for w in self.roadmap.pre_kernel}
        post_ids = {str(w.id) for w in self.roadmap.post_boot}
        self.assertFalse(pre_ids & post_ids)
        for w in self.roadmap.pre_kernel:
            self.assertIs(w.lane, RoadmapLane.PRE_KERNEL)
        for w in self.roadmap.post_boot:
            self.assertIs(w.lane, RoadmapLane.POST_BOOT)

    def test_pre_kernel_sequence_present(self) -> None:
        pre_ids = {str(w.id) for w in self.roadmap.pre_kernel}
        for expected in (
            "pre-kernel-language-soundness",
            "pre-kernel-freestanding-system-abi",
            "pre-kernel-independent-backend-bootstrap",
            "pre-kernel-freestanding-core-runtime",
            "pre-kernel-complete-boot-toolchain",
            "pre-kernel-deterministic-linked-elf",
            "pre-kernel-boot-media",
            "pre-kernel-qemu-serial-proof",
        ):
            self.assertIn(expected, pre_ids)

    def test_post_boot_gates_present(self) -> None:
        post_ids = {str(w.id) for w in self.roadmap.post_boot}
        for expected in (
            "post-boot-memory-management-mmu",
            "post-boot-interrupts",
            "post-boot-scheduler-syscall-capability",
            "post-boot-drivers-dma",
            "post-boot-process-isolation-userspace",
            "post-boot-storage-networking",
            "post-boot-update-recovery-product-shell",
        ):
            self.assertIn(expected, post_ids)

    def test_explicit_join_gates_converge_multiple_branches(self) -> None:
        joins = set(self.roadmap.join_workstream_ids)
        self.assertEqual(
            joins,
            {
                "pre-kernel-deterministic-linked-elf",
                "post-boot-scheduler-syscall-capability",
                "post-boot-update-recovery-product-shell",
            },
        )
        for join_id in joins:
            self.assertGreaterEqual(len(self.roadmap.workstream(join_id).depends_on), 2)

    def test_parallel_branches_declared(self) -> None:
        branches = self.roadmap.parallel_branches()
        # The backend/runtime, boot-toolchain, and primitive-object branches
        # converge at the linked ELF join.
        self.assertIn("backend-and-runtime", branches)
        self.assertIn("boot-toolchain", branches)
        # Memory and interrupt branches converge at the scheduler join.
        self.assertIn("memory-and-mmu", branches)
        self.assertIn("interrupts", branches)

    def test_dependency_order_places_prerequisites_first(self) -> None:
        order = self.roadmap.dependency_ordered_path()
        pos = {wid: i for i, wid in enumerate(order)}
        # Language soundness precedes the system ABI which precedes the backend.
        self.assertLess(pos["pre-kernel-language-soundness"], pos["pre-kernel-freestanding-system-abi"])
        self.assertLess(pos["pre-kernel-freestanding-system-abi"], pos["pre-kernel-independent-backend-bootstrap"])
        # The linked ELF join comes after all three of its branches.
        self.assertLess(pos["pre-kernel-freestanding-core-runtime"], pos["pre-kernel-deterministic-linked-elf"])
        self.assertLess(pos["pre-kernel-complete-boot-toolchain"], pos["pre-kernel-deterministic-linked-elf"])
        self.assertLess(pos["pre-kernel-primitive-et-rel-object"], pos["pre-kernel-deterministic-linked-elf"])
        # The QEMU proof precedes every post-boot gate.
        self.assertLess(pos["pre-kernel-qemu-serial-proof"], pos["post-boot-memory-management-mmu"])
        self.assertLess(pos["pre-kernel-qemu-serial-proof"], pos["post-boot-interrupts"])

    def test_gate_frontier_starts_at_roots(self) -> None:
        frontier = {str(w) for w in self.roadmap.gate_frontier()}
        # With nothing satisfied, only the two dependency-free roots are actionable.
        self.assertEqual(
            frontier,
            {"pre-kernel-language-soundness", "pre-kernel-primitive-et-rel-object"},
        )

    def test_gate_frontier_advances_as_prerequisites_are_satisfied(self) -> None:
        frontier = {
            str(w)
            for w in self.roadmap.gate_frontier(
                satisfied=("pre-kernel-language-soundness",)
            )
        }
        self.assertIn("pre-kernel-freestanding-system-abi", frontier)
        self.assertNotIn("pre-kernel-language-soundness", frontier)

    def test_gate_frontier_rejects_unknown_satisfied_id(self) -> None:
        with self.assertRaises(RoadmapError):
            self.roadmap.gate_frontier(satisfied=("nope",))

    def test_observed_facts_and_recommendations_are_separate(self) -> None:
        facts = self.roadmap.observed_facts()
        recs = self.roadmap.recommendations()
        fact_ids = [str(k) for k, _ in facts]
        rec_ids = [str(k) for k, _ in recs]
        self.assertEqual(fact_ids, rec_ids)  # same workstreams, same order
        # But the text payloads differ (facts describe current state; recs advise).
        fact_text = {str(k): v for k, v in facts}
        rec_text = {str(k): v for k, v in recs}
        for wid in fact_ids:
            self.assertNotEqual(fact_text[wid], rec_text[wid])


class RoadmapFailClosedTests(unittest.TestCase):
    def _ws(self, wid: str, **kw: object) -> RoadmapWorkstream:
        defaults: dict[str, object] = dict(
            id=wid,
            title=f"Gate {wid}",
            lane=RoadmapLane.PRE_KERNEL,
            observed_fact="Observed current state.",
            recommendation="Recommended action.",
        )
        defaults.update(kw)
        return RoadmapWorkstream(**defaults)  # type: ignore[arg-type]

    def test_duplicate_workstream_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError) as ctx:
            build_parallel_roadmap([self._ws("gate-a"), self._ws("gate-a")])
        self.assertEqual(ctx.exception.code, RMP_DUPLICATE_WORKSTREAM)

    def test_unknown_dependency_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError) as ctx:
            build_parallel_roadmap([self._ws("gate-a", depends_on=("gate-missing",))])
        self.assertEqual(ctx.exception.code, RMP_UNKNOWN_DEPENDENCY)

    def test_self_dependency_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError) as ctx:
            build_parallel_roadmap([self._ws("gate-a", depends_on=("gate-a",))])
        self.assertEqual(ctx.exception.code, RMP_SELF_DEPENDENCY)

    def test_illegal_join_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError) as ctx:
            build_parallel_roadmap(
                [
                    self._ws("gate-a"),
                    self._ws("gate-b", depends_on=("gate-a",), is_join=True),
                ]
            )
        self.assertEqual(ctx.exception.code, RMP_ILLEGAL_JOIN)

    def test_cycle_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError) as ctx:
            build_parallel_roadmap(
                [
                    self._ws("gate-a", depends_on=("gate-b",)),
                    self._ws("gate-b", depends_on=("gate-a",)),
                ]
            )
        self.assertEqual(ctx.exception.code, RMP_CYCLE)

    def test_missing_narrative_fails_closed(self) -> None:
        with self.assertRaises(RoadmapError):
            RoadmapWorkstream(
                id="gate-x",
                title="Gate x",
                lane=RoadmapLane.PRE_KERNEL,
                observed_fact="   ",
                recommendation="Recommended action.",
            )


class GapRoadmapTests(unittest.TestCase):
    def test_build_gap_roadmap_combines_ranking_and_roadmap(self) -> None:
        register = build_gap_register(
            _gap(gap_id="gap-a", dependency_criticality=1),
            _gap(gap_id="gap-b", dependency_criticality=5),
        )
        gap_roadmap = build_gap_roadmap(register)
        self.assertIsInstance(gap_roadmap, GapRoadmap)
        self.assertIsInstance(gap_roadmap.roadmap, ParallelRoadmap)
        # Ranking is applied.
        self.assertEqual(str(gap_roadmap.ranking.ranked_gaps[0].id), "gap-b")
        # Facts and recommendations cover both gaps and workstreams, kept apart.
        facts = gap_roadmap.observed_facts()
        recs = gap_roadmap.recommendations()
        self.assertEqual(len(facts), len(recs))
        self.assertGreater(len(facts), len(register))  # includes workstreams too


if __name__ == "__main__":
    unittest.main()
