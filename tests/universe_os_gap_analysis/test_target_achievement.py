"""Unit tests for target-level achievement and scoped blockers (Task 8.3).

These exercise Requirements 2.2, 2.3, 3.1-3.3, 7.4, 7.5, 15.2, 15.3, and 15.6
against the real models, the Task 8.1 validated Hard-Gate graph, and the Task
8.2 maturity assessor (no mocks):

* a Target_Level is achieved only when every mandatory domain reaches its
  threshold, every required Hard-Gate is satisfied, no blocking conflict touches
  a mandatory domain, no active scoped blocker applies, and validation succeeded;
* T0 hosted-adjacency evidence can only affect T0 and never changes T1-T5
  achievement, next gate, or blocking dependencies;
* a production generated-C++/host-tooling scoped blocker keeps T1 unachieved;
* the report is ordinal-only (no total score, average, percentage, or schedule)
  and reports next gate, blocking dependencies, and limitations; and
* the stage fails closed with a ``TGT-*`` code on unknown domains / duplicate
  mandatory requirements.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.hard_gate_graph import build_hard_gate_graph
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
from tools.universe_os_gap_analysis.target_achievement import (
    NON_AGGREGATE_STATEMENT,
    TGT_DUPLICATE_REQUIREMENT,
    TGT_UNKNOWN_DOMAIN,
    ConflictImpact,
    MandatoryDomainRequirement,
    ScopedBlocker,
    TargetAchievementError,
    TargetAchievementReport,
    evaluate_target_achievement,
    production_backend_blocker,
)


def _gid(key: str) -> str:
    return str(stable_id("gate", "test", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "test", key))


def _gate(
    key: str,
    *,
    level: TargetLevel,
    dependencies: tuple[str, ...] = (),
    score: MaturityScore = MaturityScore.ABSENT,
) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=level,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=score,
        dependency_ids=tuple(_gid(dep) for dep in dependencies),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=(f"Acceptance evidence for {key}.",),
        non_claims=(),
        owner_area="Test Owner",
    )


def _record(
    key: str,
    *,
    kind: EvidenceKind = EvidenceKind.TEST_EXECUTION,
    status: EvidenceStatus = EvidenceStatus.REPO_PREVIEW,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=str(stable_id("evidence", "test", key)),
        claim_key=f"claim.{key}",
        claim=f"Claim for {key}",
        status=status,
        source_path=f"src/{key}.nb",
        location=SourceLocation(kind=LocationKind.SYMBOL, value=key),
        revision_ref=str(stable_id("revision", "test")),
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _domain_input(
    domain_key: str, gate_key: str, raw: int
) -> DomainMaturityInput:
    """Build a domain input whose direct evidence yields the requested raw score."""

    common = dict(domain_id=_did(domain_key), gate_id=_gid(gate_key))
    if raw == 0:
        return DomainMaturityInput(**common)
    if raw == 1:
        return DomainMaturityInput(
            **common,
            direct_evidence=(
                _record(domain_key, kind=EvidenceKind.SOURCE, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
    evidence = (_record(domain_key),)
    if raw == 2:
        return DomainMaturityInput(**common, direct_evidence=evidence)
    score_three = dict(
        cross_host_candidate_contract=True,
        migration_rollback=True,
        release_review=True,
    )
    if raw == 3:
        return DomainMaturityInput(**common, direct_evidence=evidence, **score_three)
    if raw == 4:
        return DomainMaturityInput(
            **common, direct_evidence=evidence, supported_production=True, **score_three
        )
    return DomainMaturityInput(
        **common,
        direct_evidence=evidence,
        supported_production=True,
        mature_ecosystem=True,
        **score_three,
    )


class AchievementDecisionTests(unittest.TestCase):
    """A level is achieved only when every independent condition holds."""

    def _t1_scenario(self, raw: int = 3):
        gate = _gate("g1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "g1", raw),), graph)
        requirements = (
            MandatoryDomainRequirement(
                domain_id=_did("d1"),
                target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            ),
        )
        return assessment, graph, requirements

    def test_level_achieved_when_all_conditions_met(self) -> None:
        assessment, graph, requirements = self._t1_scenario(raw=3)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertTrue(result.achieved)
        self.assertEqual(result.unmet_mandatory_domains, ())
        self.assertEqual(result.unsatisfied_gate_ids, ())
        self.assertIsNone(result.next_hard_gate_id)
        self.assertTrue(report.is_achieved(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM))

    def test_level_unachieved_when_domain_below_threshold(self) -> None:
        # raw 2 domain cannot reach a required minimum of 3.
        assessment, graph, requirements = self._t1_scenario(raw=2)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertFalse(result.achieved)
        self.assertEqual(len(result.unmet_mandatory_domains), 1)
        self.assertEqual(str(result.unmet_mandatory_domains[0].domain_id), _did("d1"))
        self.assertTrue(result.limitations)

    def test_level_unachieved_when_gate_unsatisfied(self) -> None:
        # Gate score 1 is below the gate threshold of 3, so the required gate is
        # unsatisfied even though the domain (raw 1, effective capped to 1) is
        # trivially at its lowered threshold.
        gate = _gate("g1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.NARROW_EXPERIMENT)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "g1", 1),), graph)
        requirements = (
            MandatoryDomainRequirement(
                domain_id=_did("d1"),
                target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                minimum_maturity=MaturityScore.NARROW_EXPERIMENT,
            ),
        )
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertFalse(result.achieved)
        self.assertIn(_gid("g1"), (str(g) for g in result.unsatisfied_gate_ids))
        # The unsatisfied gate is on the frontier, so it is the next Hard-Gate.
        self.assertEqual(str(result.next_hard_gate_id), _gid("g1"))

    def test_level_unachieved_when_blocking_conflict_touches_mandatory_domain(self) -> None:
        assessment, graph, requirements = self._t1_scenario(raw=3)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            conflict_impacts=(
                ConflictImpact(
                    conflict_id="conflict-1",
                    blocking=True,
                    domain_ids=(_did("d1"),),
                ),
            ),
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertFalse(result.achieved)
        self.assertIn("conflict-1", (str(c) for c in result.blocking_conflict_ids))

    def test_non_blocking_conflict_does_not_block(self) -> None:
        assessment, graph, requirements = self._t1_scenario(raw=3)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            conflict_impacts=(
                ConflictImpact(
                    conflict_id="conflict-1",
                    blocking=False,
                    domain_ids=(_did("d1"),),
                ),
            ),
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertTrue(result.achieved)
        self.assertEqual(result.blocking_conflict_ids, ())

    def test_level_unachieved_when_validation_failed(self) -> None:
        assessment, graph, requirements = self._t1_scenario(raw=3)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            validation_ok=False,
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertFalse(result.achieved)
        self.assertFalse(result.validation_ok)
        self.assertTrue(any("validation" in lim.lower() for lim in result.limitations))


class HostedAdjacencyIsolationTests(unittest.TestCase):
    """T0 evidence can affect only T0 and never changes T1-T5 (Req 2.3, 15.6)."""

    def _substrate_scenario(self):
        gates = (
            _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.NARROW_EXPERIMENT),
            _gate("t2", level=TargetLevel.T2_FREESTANDING_SUBSTRATE, dependencies=("t1",), score=MaturityScore.ABSENT),
        )
        inputs = (
            _domain_input("d_t1", "t1", 1),
            _domain_input("d_t2", "t2", 0),
        )
        requirements = (
            MandatoryDomainRequirement(
                domain_id=_did("d_t1"),
                target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            ),
            MandatoryDomainRequirement(
                domain_id=_did("d_t2"),
                target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
                minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            ),
        )
        return gates, inputs, requirements

    def _project(self, report: TargetAchievementReport) -> dict:
        """Project the T1-T5 substrate decisions into a comparable snapshot."""

        snapshot = {}
        for result in report.results:
            if result.level is TargetLevel.T0_HOSTED_ADJACENCY:
                continue
            snapshot[result.level] = (
                result.achieved,
                None if result.next_hard_gate_id is None else str(result.next_hard_gate_id),
                tuple(str(d) for d in result.blocking_dependency_ids),
                tuple(str(g) for g in result.unsatisfied_gate_ids),
            )
        return snapshot

    def test_adding_hosted_adjacency_evidence_does_not_change_substrate(self) -> None:
        gates, inputs, requirements = self._substrate_scenario()

        base_graph = build_hard_gate_graph(gates)
        base_assessment = assess_domains(inputs, base_graph)
        base_report = evaluate_target_achievement(
            assessment=base_assessment,
            graph=base_graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )

        # Add a fully satisfied, disconnected hosted-adjacency gate and domain.
        t0_gate = _gate(
            "t0",
            level=TargetLevel.T0_HOSTED_ADJACENCY,
            score=MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM,
        )
        ext_graph = build_hard_gate_graph(gates + (t0_gate,))
        ext_assessment = assess_domains(inputs + (_domain_input("d_t0", "t0", 5),), ext_graph)
        ext_requirements = requirements + (
            MandatoryDomainRequirement(
                domain_id=_did("d_t0"),
                target_level=TargetLevel.T0_HOSTED_ADJACENCY,
                minimum_maturity=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
            ),
        )
        ext_report = evaluate_target_achievement(
            assessment=ext_assessment,
            graph=ext_graph,
            mandatory_requirements=ext_requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )

        # T0 becomes achieved in the extended report; T1-T5 are unchanged.
        self.assertTrue(ext_report.is_achieved(TargetLevel.T0_HOSTED_ADJACENCY))
        self.assertEqual(self._project(base_report), self._project(ext_report))

    def test_six_levels_present_and_ordered(self) -> None:
        gates, inputs, requirements = self._substrate_scenario()
        graph = build_hard_gate_graph(gates)
        assessment = assess_domains(inputs, graph)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
        )
        levels = tuple(result.level for result in report.results)
        self.assertEqual(levels, tuple(TargetLevel))
        self.assertEqual(
            tuple(result.order for result in report.results), tuple(range(6))
        )

    def test_t0_result_flagged_hosted_and_isolated_in_rationale(self) -> None:
        gates, inputs, requirements = self._substrate_scenario()
        graph = build_hard_gate_graph(gates)
        assessment = assess_domains(inputs, graph)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
        )
        t0 = report.result_for(TargetLevel.T0_HOSTED_ADJACENCY)
        self.assertTrue(t0.is_hosted_adjacency)
        self.assertIn("porting", t0.rationale.lower())


class ProductionBackendBlockerTests(unittest.TestCase):
    """A generated-C++/host-tooling blocker keeps T1 unachieved (Req 7.4/7.5/15.2)."""

    def _all_satisfied_t1(self):
        gate = _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "t1", 4),), graph)
        requirements = (
            MandatoryDomainRequirement(
                domain_id=_did("d1"),
                target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                minimum_maturity=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
            ),
        )
        return assessment, graph, requirements

    def test_active_blocker_keeps_t1_unachieved(self) -> None:
        assessment, graph, requirements = self._all_satisfied_t1()
        blocker = production_backend_blocker()
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
            scoped_blockers=(blocker,),
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        # Every other condition is met, but the scoped blocker holds T1 down.
        self.assertEqual(result.unmet_mandatory_domains, ())
        self.assertEqual(result.unsatisfied_gate_ids, ())
        self.assertFalse(result.achieved)
        self.assertEqual(len(result.active_blockers), 1)
        self.assertEqual(result.active_blockers[0].requirement_refs, ("7.4", "7.5", "15.2"))
        self.assertTrue(any("7.4" in lim for lim in result.limitations))

    def test_blocker_is_scoped_to_t1_only(self) -> None:
        assessment, graph, requirements = self._all_satisfied_t1()
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
            scoped_blockers=(production_backend_blocker(),),
        )
        for result in report.results:
            if result.level is TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM:
                self.assertEqual(len(result.active_blockers), 1)
            else:
                self.assertEqual(result.active_blockers, ())

    def test_inactive_blocker_does_not_block(self) -> None:
        assessment, graph, requirements = self._all_satisfied_t1()
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
            scoped_blockers=(production_backend_blocker(active=False),),
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        self.assertTrue(result.achieved)
        self.assertEqual(result.active_blockers, ())


class ReportingAndOrdinalTests(unittest.TestCase):
    """Reports next gate / blocking deps / limitations, and never aggregates."""

    def test_reports_next_gate_and_blocking_dependencies(self) -> None:
        # t2 depends on an unsatisfied t1 gate; T2's blocking dependency is t1
        # and its next actionable gate is t1 (t2's own prereq is unmet).
        gates = (
            _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.ABSENT),
            _gate("t2", level=TargetLevel.T2_FREESTANDING_SUBSTRATE, dependencies=("t1",), score=MaturityScore.ABSENT),
        )
        graph = build_hard_gate_graph(gates)
        assessment = assess_domains(
            (_domain_input("d_t2", "t2", 0),), graph
        )
        requirements = (
            MandatoryDomainRequirement(
                domain_id=_did("d_t2"),
                target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
                minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
            ),
        )
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=requirements,
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        t2 = report.result_for(TargetLevel.T2_FREESTANDING_SUBSTRATE)
        self.assertFalse(t2.achieved)
        # t1 is a lower-level prerequisite gate, so it is a blocking dependency.
        self.assertIn(_gid("t1"), (str(g) for g in t2.blocking_dependency_ids))
        # The frontier next gate is the root prerequisite t1.
        self.assertEqual(str(t2.next_hard_gate_id), _gid("t1"))

    def test_no_total_score_average_percentage_or_schedule(self) -> None:
        gate = _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "t1", 3),), graph)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=(
                MandatoryDomainRequirement(
                    domain_id=_did("d1"),
                    target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                    minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
                ),
            ),
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        result = report.result_for(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)
        # No aggregate numeric fields exist on the per-level result.
        forbidden = {"total", "score", "average", "percent", "percentage", "schedule", "duration"}
        for name in forbidden:
            self.assertFalse(hasattr(result, name), f"unexpected aggregate field {name!r}")
        # Achievement is a boolean gate; the rationale states non-aggregation.
        self.assertIsInstance(result.achieved, bool)
        self.assertIn(NON_AGGREGATE_STATEMENT, result.rationale)
        for word in ("%", "average", "schedule", "percent"):
            self.assertNotIn(word, result.rationale.replace(NON_AGGREGATE_STATEMENT, "").lower())

    def test_helper_accessors(self) -> None:
        gate = _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "t1", 3),), graph)
        report = evaluate_target_achievement(
            assessment=assessment,
            graph=graph,
            mandatory_requirements=(
                MandatoryDomainRequirement(
                    domain_id=_did("d1"),
                    target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                    minimum_maturity=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
                ),
            ),
            gate_threshold=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT,
        )
        self.assertIn(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, report.achieved_levels())
        # Every level resolves through result_for and is_achieved agrees with it.
        for level in TargetLevel:
            result = report.result_for(level)
            self.assertEqual(report.is_achieved(level), result.achieved)


class FailClosedTests(unittest.TestCase):
    def _graph_and_assessment(self):
        gate = _gate("t1", level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM, score=MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT)
        graph = build_hard_gate_graph((gate,))
        assessment = assess_domains((_domain_input("d1", "t1", 3),), graph)
        return graph, assessment

    def test_unknown_domain_fails_closed(self) -> None:
        graph, assessment = self._graph_and_assessment()
        with self.assertRaises(TargetAchievementError) as ctx:
            evaluate_target_achievement(
                assessment=assessment,
                graph=graph,
                mandatory_requirements=(
                    MandatoryDomainRequirement(
                        domain_id=_did("missing"),
                        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                    ),
                ),
            )
        self.assertEqual(ctx.exception.code, TGT_UNKNOWN_DOMAIN)

    def test_duplicate_requirement_fails_closed(self) -> None:
        graph, assessment = self._graph_and_assessment()
        with self.assertRaises(TargetAchievementError) as ctx:
            evaluate_target_achievement(
                assessment=assessment,
                graph=graph,
                mandatory_requirements=(
                    MandatoryDomainRequirement(
                        domain_id=_did("d1"),
                        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                    ),
                    MandatoryDomainRequirement(
                        domain_id=_did("d1"),
                        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                    ),
                ),
            )
        self.assertEqual(ctx.exception.code, TGT_DUPLICATE_REQUIREMENT)


if __name__ == "__main__":
    unittest.main()
