from __future__ import annotations

import json
import unittest
from datetime import datetime, timezone

from tools.universe_os_gap_analysis.identifiers import (
    ReferenceId,
    RepositoryPath,
    StableId,
    reference,
    stable_id,
)
from tools.universe_os_gap_analysis.models import (
    AssessmentModel,
    AssessmentRevision,
    CapabilityAssessment,
    CapabilityDomain,
    ConfidenceRating,
    EvidenceConflict,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    ExecutionState,
    ExcludedPath,
    FindingSeverity,
    GapCategory,
    GapEntry,
    HardGate,
    LocationKind,
    MaturityScore,
    ObservedConclusion,
    Recommendation,
    RevisionOrigin,
    Severity,
    SourceCategory,
    SourceInventoryEntry,
    SourceLocation,
    TargetLevel,
    ValidationFinding,
    ValidationResult,
    VerificationState,
)
from tools.universe_os_gap_analysis.serialization import stable_json_dumps, to_primitive


class IdentifierTests(unittest.TestCase):
    def test_stable_id_is_order_independent_for_mapping_components(self) -> None:
        left = stable_id("evidence", {"path": "README.md", "anchor": "Boundary"})
        right = stable_id("evidence", {"anchor": "Boundary", "path": "README.md"})
        different = stable_id("evidence", {"path": "README.md", "anchor": "Scope"})

        self.assertIsInstance(left, StableId)
        self.assertEqual(left, right)
        self.assertNotEqual(left, different)
        self.assertIsInstance(reference(left), ReferenceId)

    def test_ids_reject_empty_segments_and_non_string_references(self) -> None:
        for invalid in ("object-", "object--child", "-object"):
            with self.subTest(identifier=invalid), self.assertRaises(ValueError):
                StableId(invalid)
        with self.assertRaises(ValueError):
            stable_id("invalid-", "component")
        with self.assertRaises(TypeError):
            reference(123)  # type: ignore[arg-type]

    def test_repository_path_accepts_only_normalized_relative_posix_paths(self) -> None:
        self.assertEqual(RepositoryPath("docs/universeos/gates.md"), "docs/universeos/gates.md")
        for invalid in (
            "", ".", "..", "/README.md", "../README.md", "a/../b", "a/./b",
            "a//b", "a/", "C:\\repo\\file", "C:/repo/file", "bad\npath",
        ):
            with self.subTest(path=invalid), self.assertRaises((TypeError, ValueError)):
                RepositoryPath(invalid)


class ClosedEnumTests(unittest.TestCase):
    def test_evidence_status_is_the_required_closed_set(self) -> None:
        self.assertEqual(
            {status.value for status in EvidenceStatus},
            {
                "Compiler_Tooling_GA",
                "Backend_SDK_GA",
                "Installed_Preview",
                "Repo_Preview",
                "Experimental",
                "Planned",
                "Unsupported",
                "Unknown",
            },
        )

    def test_target_levels_and_maturity_scores_are_closed_and_ordered(self) -> None:
        self.assertEqual([level.value[:2] for level in TargetLevel], ["T0", "T1", "T2", "T3", "T4", "T5"])
        self.assertEqual([score.value for score in MaturityScore], list(range(6)))


class TypedModelTests(unittest.TestCase):
    def setUp(self) -> None:
        self.revision = AssessmentRevision(
            schema_version="1.0",
            commit_id="abc123",
            branch="feature/gap-analysis",
            version="1.0.0",
            describe="v1.0.0-dirty",
            tags=("v1.0.0",),
            worktree_clean=False,
            assessed_at_utc=datetime(2025, 1, 2, 3, 4, 5, tzinfo=timezone.utc),
            fingerprint_algorithm="sha256-length-prefixed-v1",
            worktree_fingerprint="worktree-hash",
            tracked_diff_hash="diff-hash",
            untracked_path_set_hash="paths-hash",
            excluded_paths=(
                ExcludedPath(
                    path="assessment/output",
                    reason="prevents self-reference",
                    rule_version="assessment-output-exclusion-v1",
                ),
            ),
            repository_root_id=StableId("repo-nebula"),
        )
        self.location = SourceLocation(kind=LocationKind.HEADING, value="Current Boundary")
        self.scope = EvidenceScope(
            capability_ids=(reference("domain-language"),),
            target_levels=(TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,),
            platforms=("linux", "macos", "linux"),
        )
        self.evidence = EvidenceRecord(
            id=StableId("evidence-current-boundary"),
            claim_key="compiler.pipeline",
            claim="Production compilation emits C++23.",
            status=EvidenceStatus.COMPILER_TOOLING_GA,
            source_path="README.md",
            location=self.location,
            revision_ref=reference("revision-current"),
            origin=RevisionOrigin.CURRENT_WORKTREE,
            evidence_kind=EvidenceKind.SOURCE,
            confidence=ConfidenceRating.MEDIUM,
            scope=self.scope,
            limitations=("external host compiler required",),
            trust_assumptions=("trusted host compiler",),
            verification_state=VerificationState.NOT_RUN,
        )

    def test_evidence_record_normalizes_references_and_set_like_fields(self) -> None:
        self.assertIsInstance(self.evidence.source_path, RepositoryPath)
        self.assertEqual(self.scope.platforms, ("linux", "macos"))
        self.assertEqual(self.evidence.status, EvidenceStatus.COMPILER_TOOLING_GA)
        with self.assertRaises(TypeError):
            EvidenceRecord(
                id=StableId("evidence-invalid-status"),
                claim_key="invalid",
                claim="Invalid status input.",
                status="Planned",  # type: ignore[arg-type]
                source_path="README.md",
                location=self.location,
                revision_ref=reference("revision-current"),
                origin=RevisionOrigin.CURRENT_WORKTREE,
                evidence_kind=EvidenceKind.SOURCE,
                confidence=ConfidenceRating.LOW,
                scope=self.scope,
                limitations=(),
                trust_assumptions=(),
                verification_state=VerificationState.NOT_RUN,
            )

    def test_conflict_forces_low_confidence_and_has_no_winner(self) -> None:
        conflict = EvidenceConflict(
            id=StableId("conflict-compiler-pipeline"),
            claim_key="compiler.pipeline",
            evidence_ids=(reference("evidence-b"), reference("evidence-a")),
            incompatible_values=("direct backend", "generated C++"),
            locations=(
                SourceLocation(kind=LocationKind.HEADING, value="Backend"),
                self.location,
            ),
            blocking=True,
        )
        self.assertIsNone(conflict.winner)
        self.assertIs(conflict.confidence, ConfidenceRating.LOW)
        self.assertEqual(conflict.evidence_ids, (reference("evidence-a"), reference("evidence-b")))

    def test_gap_category_constraints_and_maturity_range_are_enforced(self) -> None:
        gap = self._gap((GapCategory.VERIFICATION, GapCategory.VERIFICATION))
        self.assertEqual(gap.secondary_categories, (GapCategory.VERIFICATION,))
        with self.assertRaisesRegex(ValueError, "primary_category"):
            self._gap((GapCategory.LANGUAGE,))
        with self.assertRaisesRegex(ValueError, "0 through 5"):
            self._assessment(raw_score=6)  # type: ignore[arg-type]
        with self.assertRaisesRegex(ValueError, "effective_score"):
            self._assessment(raw_score=MaturityScore.ABSENT)

    def test_traceable_sections_require_acceptance_or_related_gap_references(self) -> None:
        with self.assertRaisesRegex(ValueError, "at least 1"):
            self._gap((), acceptance_evidence=())
        with self.assertRaisesRegex(ValueError, "at least 1"):
            self._gate(acceptance_evidence=())
        with self.assertRaisesRegex(ValueError, "at least 1"):
            Recommendation(
                id=StableId("recommendation-unlinked"),
                text="Define a gate.",
                related_gap_ids=(),
            )

    def test_validation_findings_are_typed_and_deterministically_sorted(self) -> None:
        warning = ValidationFinding(
            severity=FindingSeverity.WARNING,
            code="RPT-WARNING",
            requirement_refs=("14.7",),
            object_refs=(reference("gap-independent-backend"),),
        )
        info = ValidationFinding(
            severity=FindingSeverity.INFO,
            code="RPT-INFO",
            requirement_refs=("14.2",),
            object_refs=(reference("domain-language"),),
        )
        result = ValidationResult(valid=True, findings=(warning, info))
        self.assertEqual(result.findings, (info, warning))
        with self.assertRaisesRegex(TypeError, "ValidationFinding"):
            ValidationResult(valid=False, findings=("invalid",))  # type: ignore[arg-type]

    def test_assessment_model_rejects_untyped_section_members(self) -> None:
        with self.assertRaisesRegex(TypeError, "SourceInventoryEntry"):
            AssessmentModel(
                revision=self.revision,
                source_inventory=(self.evidence,),  # type: ignore[arg-type]
            )

    def test_assessment_model_sorts_id_collections_and_keeps_typed_sections(self) -> None:
        inventory_b = self._inventory("inventory-b", "spec/compiler_pipeline.md")
        inventory_a = self._inventory("inventory-a", "README.md")
        domain = self._domain()
        gap = self._gap(())
        gate = self._gate()
        assessment = self._assessment()
        conclusion = ObservedConclusion(
            id=StableId("conclusion-current"),
            text="The production compiler requires hosted tooling.",
            evidence_ids=(reference(self.evidence.id),),
        )
        recommendation = Recommendation(
            id=StableId("recommendation-backend"),
            text="Define an independent backend acceptance gate.",
            related_gap_ids=(reference(gap.id),),
        )
        model = AssessmentModel(
            revision=self.revision,
            source_inventory=(inventory_b, inventory_a),
            evidence_records=(self.evidence,),
            target_levels=(
                TargetLevel.T2_FREESTANDING_SUBSTRATE,
                TargetLevel.T0_HOSTED_ADJACENCY,
                TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            ),
            domains=(domain,),
            assessments=(assessment,),
            gaps=(gap,),
            hard_gates=(gate,),
            observed_conclusions=(conclusion,),
            recommendations=(recommendation,),
            validation=ValidationResult(valid=True),
        )

        self.assertEqual([entry.id for entry in model.source_inventory], ["inventory-a", "inventory-b"])
        self.assertEqual(
            model.target_levels,
            (
                TargetLevel.T0_HOSTED_ADJACENCY,
                TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                TargetLevel.T2_FREESTANDING_SUBSTRATE,
            ),
        )
        self.assertIsInstance(model.observed_conclusions[0], ObservedConclusion)
        self.assertIsInstance(model.recommendations[0], Recommendation)

        first = stable_json_dumps(model)
        second = stable_json_dumps(model)
        payload = json.loads(first)
        self.assertEqual(first, second)
        self.assertEqual(payload["revision"]["worktreeClean"], False)
        self.assertEqual(payload["evidenceRecords"][0]["status"], "Compiler_Tooling_GA")
        self.assertEqual(payload["observedConclusions"][0]["id"], "conclusion-current")
        self.assertTrue(first.endswith("\n"))

    def test_serializer_canonicalizes_mappings_and_unordered_sets(self) -> None:
        left = stable_json_dumps({"z": {"beta", "alpha"}, "a": 1})
        right = stable_json_dumps({"a": 1, "z": {"alpha", "beta"}})
        self.assertEqual(left, right)
        self.assertEqual(to_primitive(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION), 2)
        with self.assertRaisesRegex(ValueError, "finite"):
            stable_json_dumps({"invalid": float("nan")})
        with self.assertRaisesRegex(ValueError, "indent"):
            stable_json_dumps({}, indent=-1)

    def _inventory(self, identifier: str, path: str) -> SourceInventoryEntry:
        return SourceInventoryEntry(
            id=StableId(identifier),
            category=SourceCategory.SPECIFICATION if path.startswith("spec/") else SourceCategory.README,
            path=path,
            revision_origin=RevisionOrigin.CURRENT_WORKTREE,
            inspected=True,
            execution_state=ExecutionState.NOT_RUN,
            content_hash=f"hash-{identifier}",
            stable_anchors=("Current Boundary",),
        )

    def _domain(self) -> CapabilityDomain:
        return CapabilityDomain(
            id=StableId("domain-language"),
            name="Language platform",
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            description="Language and compiler independence.",
            mandatory_for_target=True,
            evidence_ids=(reference(self.evidence.id),),
            gap_ids=(reference("gap-independent-backend"),),
            dependency_gate_ids=(reference("gate-independent-backend"),),
        )

    def _assessment(self, raw_score: int | MaturityScore = MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION) -> CapabilityAssessment:
        return CapabilityAssessment(
            domain_id=reference("domain-language"),
            raw_score=raw_score,  # type: ignore[arg-type]
            effective_score=MaturityScore.NARROW_EXPERIMENT,
            confidence=ConfidenceRating.MEDIUM,
            evidence_status=EvidenceStatus.COMPILER_TOOLING_GA,
            evidence_ids=(reference(self.evidence.id),),
            limitations=("external host compiler required",),
            next_hard_gate_id=reference("gate-independent-backend"),
            blocking_dependency_ids=(reference("gate-system-abi"),),
            rationale="Direct evidence is repository-local and dependency-capped.",
        )

    def _gap(
        self,
        secondary: tuple[GapCategory, ...],
        *,
        acceptance_evidence: tuple[str, ...] = ("Cross-host backend contract",),
    ) -> GapEntry:
        return GapEntry(
            id=StableId("gap-independent-backend"),
            title="Independent backend is absent",
            primary_category=GapCategory.LANGUAGE,
            secondary_categories=secondary,
            domain_ids=(reference("domain-language"),),
            current_status=EvidenceStatus.UNSUPPORTED,
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            severity=Severity.CRITICAL,
            dependencies=(reference("gate-system-abi"),),
            acceptance_evidence=acceptance_evidence,
            recommended_owner_area="compiler",
            dependency_criticality=4,
            safety_impact=2,
            claim_risk=4,
            target_unblock_value=5,
            observed_fact="Generated C++ remains in the production path.",
            recommendation="Implement and verify an independent backend.",
        )

    def _gate(
        self,
        *,
        acceptance_evidence: tuple[str, ...] = ("Reproducible native output",),
    ) -> HardGate:
        return HardGate(
            id=StableId("gate-independent-backend"),
            title="Independent backend and bootstrap",
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            status=EvidenceStatus.PLANNED,
            maturity_score=MaturityScore.ABSENT,
            dependency_ids=(reference("gate-system-abi"),),
            blocking_domain_ids=(reference("domain-language"),),
            evidence_ids=(reference(self.evidence.id),),
            acceptance_evidence=acceptance_evidence,
            non_claims=("No backend independence is currently claimed",),
            owner_area="compiler",
        )


if __name__ == "__main__":
    unittest.main()
