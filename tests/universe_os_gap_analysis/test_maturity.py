"""Unit and property tests for the raw/effective maturity assessor (Task 8.2).

These exercise Requirements 3.1-3.6, 10.6, 15.4, and 15.5 against the real
models and the Task 8.1 validated Hard-Gate graph (no mocks):

* raw scores come from direct evidence only, and no implementation evidence
  fixes both raw and effective at 0;
* the language/tooling (and general) cap holds a domain at 2 until the three
  score-3 conditions are demonstrated;
* effective score is ``min(raw, every blocking dependency/gate score)`` computed
  in topological order, with transitive propagation and blocking-only capping;
* every domain yields exactly one ordinal :class:`CapabilityAssessment`; and
* the assessor fails closed with a ``MAT-*`` code on unknown gates / duplicates
  and exposes gate/domain effective scores plus a capping trace for Task 8.3.
"""

from __future__ import annotations

import unittest

from hypothesis import given, settings
from hypothesis import strategies as st

from tools.universe_os_gap_analysis.hard_gate_graph import (
    GateDependencyEdge,
    build_hard_gate_graph,
)
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.maturity import (
    MAT_DUPLICATE_DOMAIN,
    MAT_UNKNOWN_GATE,
    CapabilityAssessment,
    DomainClass,
    DomainMaturityInput,
    MaturityAssessmentError,
    assess_domains,
    compute_raw_score,
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


def _gid(key: str) -> str:
    return str(stable_id("gate", "test", key))


def _did(key: str) -> str:
    return str(stable_id("domain", "test", key))


def _gate(
    key: str,
    *,
    dependencies: tuple[str, ...] = (),
    score: MaturityScore = MaturityScore.ABSENT,
) -> HardGate:
    return HardGate(
        id=_gid(key),
        title=f"Gate {key}",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
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
    kind: EvidenceKind = EvidenceKind.SOURCE,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
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


class RawScoreTests(unittest.TestCase):
    def test_no_direct_implementation_is_zero(self) -> None:
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("plan", kind=EvidenceKind.RFC, status=EvidenceStatus.PLANNED),
                _record("spec", kind=EvidenceKind.SPECIFICATION, status=EvidenceStatus.PLANNED),
                _record("example", kind=EvidenceKind.EXAMPLE, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
        self.assertEqual(compute_raw_score(inp), MaturityScore.ABSENT)

    def test_no_evidence_is_zero(self) -> None:
        inp = DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("g"))
        self.assertEqual(compute_raw_score(inp), MaturityScore.ABSENT)

    def test_experimental_source_is_narrow_experiment(self) -> None:
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
        self.assertEqual(compute_raw_score(inp), MaturityScore.NARROW_EXPERIMENT)

    def test_test_execution_is_repeatable(self) -> None:
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("t", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
        self.assertEqual(
            compute_raw_score(inp), MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
        )

    def test_preview_status_is_repeatable(self) -> None:
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        self.assertEqual(
            compute_raw_score(inp), MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
        )

    def test_capped_at_two_without_score_three_conditions(self) -> None:
        # Even GA + production + ecosystem signals cannot exceed 2 without the
        # three score-3 conditions.
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.COMPILER_TOOLING_GA),
            ),
            supported_production=True,
            mature_ecosystem=True,
        )
        self.assertEqual(
            compute_raw_score(inp), MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
        )

    def test_score_three_requires_all_three_conditions(self) -> None:
        base = dict(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        # Missing release_review -> still capped at 2.
        inp_missing = DomainMaturityInput(
            **base, cross_host_candidate_contract=True, migration_rollback=True
        )
        self.assertEqual(
            compute_raw_score(inp_missing),
            MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
        )
        # All three -> reaches 3.
        inp_ready = DomainMaturityInput(
            **base,
            cross_host_candidate_contract=True,
            migration_rollback=True,
            release_review=True,
        )
        self.assertEqual(
            compute_raw_score(inp_ready), MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT
        )

    def test_production_and_ecosystem_climb_to_four_and_five(self) -> None:
        base = dict(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.BACKEND_SDK_GA),
            ),
            cross_host_candidate_contract=True,
            migration_rollback=True,
            release_review=True,
        )
        self.assertEqual(
            compute_raw_score(DomainMaturityInput(**base, supported_production=True)),
            MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY,
        )
        self.assertEqual(
            compute_raw_score(
                DomainMaturityInput(
                    **base, supported_production=True, mature_ecosystem=True
                )
            ),
            MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM,
        )

    def test_ecosystem_without_production_stays_at_three(self) -> None:
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.REPO_PREVIEW),
            ),
            cross_host_candidate_contract=True,
            migration_rollback=True,
            release_review=True,
            mature_ecosystem=True,
        )
        self.assertEqual(
            compute_raw_score(inp), MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT
        )


class EffectiveScoreTests(unittest.TestCase):
    def test_no_implementation_fixes_effective_at_zero(self) -> None:
        graph = build_hard_gate_graph((_gate("g"),))
        inp = DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("g"))
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None
        self.assertEqual(result.raw_score, MaturityScore.ABSENT)
        self.assertEqual(result.effective_score, MaturityScore.ABSENT)

    def test_effective_capped_by_blocking_dependency(self) -> None:
        # gate "dep" has score 1; gate "g" depends on it. A repeatable (raw 2)
        # domain on "g" is capped to 1 by its blocking dependency.
        gates = (
            _gate("dep", score=MaturityScore.NARROW_EXPERIMENT),
            _gate("g", dependencies=("dep",)),
        )
        graph = build_hard_gate_graph(gates)
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("t", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None
        self.assertEqual(result.raw_score, MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)
        self.assertEqual(result.effective_score, MaturityScore.NARROW_EXPERIMENT)
        binding = [step for step in result.cap_trace if step.binding]
        self.assertEqual(len(binding), 1)
        self.assertEqual(str(binding[0].dependency_gate_id), _gid("dep"))

    def test_non_blocking_edge_does_not_cap(self) -> None:
        gates = (
            _gate("dep", score=MaturityScore.ABSENT),
            _gate("g"),
        )
        edges = (
            GateDependencyEdge(
                dependent_id=_gid("g"),
                dependency_id=_gid("dep"),
                blocking=False,
                cap_rationale="Associated only; does not cap maturity.",
            ),
        )
        graph = build_hard_gate_graph(gates, edges=edges)
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("t", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None
        self.assertEqual(result.effective_score, MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)
        self.assertEqual(result.cap_trace, ())

    def test_transitive_cap_propagates_through_chain(self) -> None:
        # root(0) -> mid(5) -> g. gate effective of mid is min(5, 0) = 0, so a
        # repeatable domain on g is held at 0 transitively.
        gates = (
            _gate("root", score=MaturityScore.ABSENT),
            _gate("mid", dependencies=("root",), score=MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM),
            _gate("g", dependencies=("mid",)),
        )
        graph = build_hard_gate_graph(gates)
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            direct_evidence=(
                _record("t", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        assessment = assess_domains((inp,), graph)
        self.assertEqual(
            assessment.gate_effective_score_of(_gid("mid")), MaturityScore.ABSENT
        )
        result = assessment.result_for(_did("d"))
        assert result is not None
        self.assertEqual(result.effective_score, MaturityScore.ABSENT)


class AssessmentShapeTests(unittest.TestCase):
    def test_one_ordinal_assessment_per_domain_with_all_fields(self) -> None:
        gates = (_gate("g1"), _gate("g2"))
        graph = build_hard_gate_graph(gates)
        inputs = (
            DomainMaturityInput(
                domain_id=_did("d1"),
                gate_id=_gid("g1"),
                direct_evidence=(_record("a", status=EvidenceStatus.EXPERIMENTAL),),
                confidence=ConfidenceRating.MEDIUM,
                evidence_status=EvidenceStatus.EXPERIMENTAL,
            ),
            DomainMaturityInput(domain_id=_did("d2"), gate_id=_gid("g2")),
        )
        assessment = assess_domains(inputs, graph)
        self.assertEqual(len(assessment.assessments), 2)
        for capability in assessment.assessments:
            self.assertIsInstance(capability, CapabilityAssessment)
            self.assertIn(int(capability.raw_score), range(0, 6))
            self.assertIn(int(capability.effective_score), range(0, 6))
            self.assertLessEqual(int(capability.effective_score), int(capability.raw_score))
            self.assertTrue(capability.rationale.strip())
            self.assertTrue(str(capability.next_hard_gate_id).strip())
        # Non-additive / no-percentage statement is present in the rationale.
        self.assertIn(
            "non-additive", assessment.assessments[0].rationale.lower()
        )

    def test_language_tooling_cap_recorded_in_limitations(self) -> None:
        graph = build_hard_gate_graph((_gate("g"),))
        inp = DomainMaturityInput(
            domain_id=_did("d"),
            gate_id=_gid("g"),
            domain_class=DomainClass.LANGUAGE_TOOLING,
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.REPO_PREVIEW),
            ),
        )
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None
        self.assertTrue(result.language_tooling_capped)
        self.assertEqual(result.effective_score, MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)
        self.assertTrue(
            any("15.4" in limitation for limitation in result.assessment.limitations)
        )

    def test_next_hard_gate_defaults_to_domain_gate(self) -> None:
        graph = build_hard_gate_graph((_gate("g"),))
        inp = DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("g"))
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None
        self.assertEqual(str(result.assessment.next_hard_gate_id), _gid("g"))


class FailClosedTests(unittest.TestCase):
    def test_unknown_gate_fails_closed(self) -> None:
        graph = build_hard_gate_graph((_gate("g"),))
        inp = DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("missing"))
        with self.assertRaises(MaturityAssessmentError) as ctx:
            assess_domains((inp,), graph)
        self.assertEqual(ctx.exception.code, MAT_UNKNOWN_GATE)

    def test_duplicate_domain_fails_closed(self) -> None:
        graph = build_hard_gate_graph((_gate("g"),))
        inputs = (
            DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("g")),
            DomainMaturityInput(domain_id=_did("d"), gate_id=_gid("g")),
        )
        with self.assertRaises(MaturityAssessmentError) as ctx:
            assess_domains(inputs, graph)
        self.assertEqual(ctx.exception.code, MAT_DUPLICATE_DOMAIN)


class MaturityPropertyTests(unittest.TestCase):
    """Property-style checks over random raw scores and blocking dependencies."""

    @settings(max_examples=200, deadline=None)
    @given(
        raw_choice=st.integers(min_value=0, max_value=5),
        dep_scores=st.lists(st.integers(min_value=0, max_value=5), min_size=0, max_size=6),
    )
    def test_effective_is_min_of_raw_and_blocking_dependencies(
        self, raw_choice: int, dep_scores: list[int]
    ) -> None:
        # Build a star: N dependency gates feed one dependent gate "g".
        dep_gates = tuple(
            _gate(f"dep{i}", score=MaturityScore(score))
            for i, score in enumerate(dep_scores)
        )
        g = _gate("g", dependencies=tuple(f"dep{i}" for i in range(len(dep_scores))))
        graph = build_hard_gate_graph(dep_gates + (g,))

        # Choose direct evidence that yields the requested raw score.
        inp = _input_for_raw(raw_choice)
        result = assess_domains((inp,), graph).result_for(_did("d"))
        assert result is not None

        expected = min([raw_choice] + dep_scores)
        self.assertEqual(int(result.raw_score), raw_choice)
        self.assertEqual(int(result.effective_score), expected)
        self.assertLessEqual(int(result.effective_score), int(result.raw_score))


def _input_for_raw(raw: int) -> DomainMaturityInput:
    """Construct a domain input whose direct evidence yields the given raw score."""

    common = dict(domain_id=_did("d"), gate_id=_gid("g"))
    if raw == 0:
        return DomainMaturityInput(**common)
    if raw == 1:
        return DomainMaturityInput(
            **common,
            direct_evidence=(
                _record("s", kind=EvidenceKind.SOURCE, status=EvidenceStatus.EXPERIMENTAL),
            ),
        )
    # raw >= 2 requires repeatable evidence.
    evidence = (
        _record("t", kind=EvidenceKind.TEST_EXECUTION, status=EvidenceStatus.REPO_PREVIEW),
    )
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


if __name__ == "__main__":
    unittest.main()
