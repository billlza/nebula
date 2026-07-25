"""Unit tests for the fail-closed publish validator (Task 10.1).

These tests exercise Requirements 3.5, 9.6, 9.7, 13.4, and 14.1-14.7 against the
real :mod:`tools.universe_os_gap_analysis.validator` module and real canonical
model objects (no mocks). They confirm the validator:

* accepts a complete, internally consistent ``AssessmentModel``;
* fails closed with an ``EVD-*`` finding when a required ``Evidence_Record`` a
  domain/assessment/conclusion references does not resolve (Requirement 9.7);
* fails closed with an ``RPT-*`` finding when the capability-matrix/domain parity
  breaks, the six-level target model is malformed, or a mandatory section is
  missing (Requirement 14.1, 14.2);
* fails closed with an ``INV-*`` finding when a required source category is
  absent (Requirement 1.3, 14.1);
* fails closed with a ``GRF-*`` finding when the Hard-Gate graph is not a DAG
  (Requirement 3.5, 12.7);
* fails closed with a ``CLM-*`` finding when a trust assumption is unrecorded
  (Requirement 9.5, 9.6);
* fails closed with a ``CNF-*`` finding when a conflict references an unknown
  record (Requirement 1.5, 13.4); and
* fails closed with ``MAT-*``/``RPT-*`` findings when a domain without direct
  evidence carries a non-zero maturity score (Requirement 3.6, 10.6, 15.5).
"""

from __future__ import annotations

import dataclasses
import unittest
from datetime import datetime, timezone

from tools.universe_os_gap_analysis.identifiers import StableId
from tools.universe_os_gap_analysis.inventory import REQUIRED_SOURCE_CATEGORIES
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
    ExcludedPath,
    ExecutionState,
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
    ValidationResult,
    VerificationState,
)
from tools.universe_os_gap_analysis.validator import (
    AssessmentValidator,
    CNF_UNKNOWN_EVIDENCE,
    EVD_UNKNOWN_PATH,
    EVD_UNKNOWN_REFERENCE,
    INV_MISSING_CATEGORY,
    MAT_NO_EVIDENCE_NONZERO,
    RPT_DOMAIN_ASSESSMENT_PARITY,
    RPT_INITIAL_CONCLUSION,
    RPT_MISSING_SECTION,
    RPT_TARGET_LEVELS,
    RPT_UNKNOWN_DOMAIN,
    validate_assessment_model,
)

_README_PATH = "README.md"
_README_ANCHOR = "Current Boundary"
_SPEC_PATH = "spec/language_core.md"
_SPEC_ANCHOR = "Overview"


def _codes(result: ValidationResult) -> set[str]:
    return {finding.code for finding in result.findings}


def _revision() -> AssessmentRevision:
    return AssessmentRevision(
        schema_version="1",
        commit_id="0" * 40,
        branch="main",
        version="1.0.0",
        describe="v1.0.0",
        tags=(),
        worktree_clean=True,
        assessed_at_utc=datetime(2024, 1, 1, tzinfo=timezone.utc),
        fingerprint_algorithm="sha256-length-prefixed",
        worktree_fingerprint="f" * 64,
        tracked_diff_hash="0" * 64,
        untracked_path_set_hash="0" * 64,
        excluded_paths=(
            ExcludedPath(
                path="out/assessment.json",
                reason="assessment output directory",
                rule_version="1",
            ),
        ),
        repository_root_id=StableId("repo-root"),
    )


def _inventory() -> tuple[SourceInventoryEntry, ...]:
    """One inventory entry per required category, covering the evidence paths."""

    entries: list[SourceInventoryEntry] = []
    for index, category in enumerate(
        sorted(REQUIRED_SOURCE_CATEGORIES, key=lambda item: item.value)
    ):
        if category is SourceCategory.README:
            path, anchor = _README_PATH, _README_ANCHOR
        elif category is SourceCategory.SPECIFICATION:
            path, anchor = _SPEC_PATH, _SPEC_ANCHOR
        else:
            path, anchor = f"inv/entry_{index}.txt", f"anchor-{index}"
        entries.append(
            SourceInventoryEntry(
                id=StableId(f"inv-{index}"),
                category=category,
                path=path,
                revision_origin=RevisionOrigin.COMMITTED_REVISION,
                inspected=True,
                execution_state=ExecutionState.NOT_RUN,
                content_hash="a" * 64,
                stable_anchors=(anchor,),
            )
        )
    return tuple(entries)


def _record(
    *,
    record_id: str,
    source_path: str,
    anchor: str,
    claim: str = "The hosted CLI builds and runs on supported hosts.",
) -> EvidenceRecord:
    return EvidenceRecord(
        id=StableId(record_id),
        claim_key=record_id,
        claim=claim,
        status=EvidenceStatus.COMPILER_TOOLING_GA,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=anchor),
        revision_ref="revision-test",
        origin=RevisionOrigin.COMMITTED_REVISION,
        evidence_kind=EvidenceKind.SOURCE,
        confidence=ConfidenceRating.HIGH,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _hosted_domain() -> CapabilityDomain:
    return CapabilityDomain(
        id=StableId("domain-hosted"),
        name="Hosted CLI",
        target_level=TargetLevel.T0_HOSTED_ADJACENCY,
        description="Hosted CLI tooling on a host OS.",
        mandatory_for_target=True,
        evidence_ids=("ev-hosted",),
        gap_ids=("gap-hosted",),
        dependency_gate_ids=("gate-hosted",),
    )


def _kernel_domain() -> CapabilityDomain:
    return CapabilityDomain(
        id=StableId("domain-kernel"),
        name="Kernel scheduler",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        description="Kernel scheduler with no direct implementation evidence.",
        mandatory_for_target=True,
    )


def _hosted_assessment(
    *, raw: MaturityScore = MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
    effective: MaturityScore = MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
    evidence_ids: tuple[str, ...] = ("ev-hosted",),
) -> CapabilityAssessment:
    return CapabilityAssessment(
        domain_id="domain-hosted",
        raw_score=raw,
        effective_score=effective,
        confidence=ConfidenceRating.HIGH,
        evidence_status=EvidenceStatus.COMPILER_TOOLING_GA,
        evidence_ids=evidence_ids,
        limitations=(),
        next_hard_gate_id="gate-hosted",
        blocking_dependency_ids=(),
        rationale="Repeatable repository-local hosted CLI implementation.",
    )


def _kernel_assessment(
    *, raw: MaturityScore = MaturityScore.ABSENT,
    effective: MaturityScore = MaturityScore.ABSENT,
    evidence_ids: tuple[str, ...] = (),
) -> CapabilityAssessment:
    return CapabilityAssessment(
        domain_id="domain-kernel",
        raw_score=raw,
        effective_score=effective,
        confidence=ConfidenceRating.LOW,
        evidence_status=EvidenceStatus.UNSUPPORTED,
        evidence_ids=evidence_ids,
        limitations=("No direct implementation evidence.",),
        next_hard_gate_id="gate-kernel",
        blocking_dependency_ids=(),
        rationale="No direct implementation evidence, fixed at maturity 0.",
    )


def _hosted_gate() -> HardGate:
    return HardGate(
        id=StableId("gate-hosted"),
        title="Hosted CLI gate",
        target_level=TargetLevel.T0_HOSTED_ADJACENCY,
        status=EvidenceStatus.COMPILER_TOOLING_GA,
        maturity_score=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
        dependency_ids=(),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=("Hosted CLI builds on supported hosts.",),
        non_claims=(),
        owner_area="Tooling",
    )


def _kernel_gate() -> HardGate:
    return HardGate(
        id=StableId("gate-kernel"),
        title="Kernel scheduler gate",
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        status=EvidenceStatus.UNSUPPORTED,
        maturity_score=MaturityScore.ABSENT,
        dependency_ids=("gate-hosted",),
        blocking_domain_ids=(),
        evidence_ids=(),
        acceptance_evidence=("A reproducible kernel scheduler.",),
        non_claims=("No kernel scheduler exists.",),
        owner_area="Kernel",
    )


def _gap() -> GapEntry:
    return GapEntry(
        id=StableId("gap-hosted"),
        title="Hosted CLI compatibility governance gap",
        primary_category=GapCategory.VERIFICATION,
        secondary_categories=(),
        domain_ids=("domain-hosted",),
        current_status=EvidenceStatus.COMPILER_TOOLING_GA,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        severity=Severity.MEDIUM,
        dependencies=(),
        acceptance_evidence=("A published compatibility policy.",),
        recommended_owner_area="Tooling",
        dependency_criticality=1,
        safety_impact=0,
        claim_risk=1,
        target_unblock_value=1,
        observed_fact="The hosted CLI has no compatibility policy.",
        recommendation="Publish and verify a compatibility policy.",
    )


def build_valid_model() -> AssessmentModel:
    """Assemble a complete, internally consistent model that must validate."""

    ev_hosted = _record(
        record_id="ev-hosted", source_path=_README_PATH, anchor=_README_ANCHOR
    )
    ev_spec = _record(
        record_id="ev-spec",
        source_path=_SPEC_PATH,
        anchor=_SPEC_ANCHOR,
        claim="The language core specification documents the current pipeline.",
    )
    conflict = EvidenceConflict(
        id=StableId("conflict-1"),
        claim_key="pipeline description",
        evidence_ids=("ev-hosted", "ev-spec"),
        incompatible_values=("hosted", "freestanding"),
        locations=(
            SourceLocation(kind=LocationKind.HEADING, value=_README_ANCHOR),
            SourceLocation(kind=LocationKind.HEADING, value=_SPEC_ANCHOR),
        ),
        blocking=False,
    )
    return AssessmentModel(
        revision=_revision(),
        source_inventory=_inventory(),
        evidence_records=(ev_hosted, ev_spec),
        conflicts=(conflict,),
        target_levels=tuple(TargetLevel),
        domains=(_hosted_domain(), _kernel_domain()),
        assessments=(_hosted_assessment(), _kernel_assessment()),
        gaps=(_gap(),),
        hard_gates=(_hosted_gate(), _kernel_gate()),
        assumptions=("The host toolchain is a production dependency.",),
        non_claims=("No kernel, driver, or freestanding runtime exists.",),
        observed_conclusions=(
            ObservedConclusion(
                id=StableId("conclusion-1"),
                text="Nebula is a hosted language and tooling foundation.",
                evidence_ids=("ev-hosted",),
            ),
        ),
        recommendations=(
            Recommendation(
                id=StableId("rec-1"),
                text="Publish a compatibility policy before depending on the CLI.",
                related_gap_ids=("gap-hosted",),
            ),
        ),
    )


class ValidAssessmentModelTests(unittest.TestCase):
    def test_complete_model_is_valid_with_no_findings(self) -> None:
        result = validate_assessment_model(build_valid_model())
        self.assertTrue(result.valid, msg=f"unexpected findings: {result.findings}")
        self.assertEqual(result.findings, ())

    def test_convenience_and_class_entry_points_agree(self) -> None:
        model = build_valid_model()
        self.assertEqual(
            validate_assessment_model(model),
            AssessmentValidator().validate(model),
        )

    def test_rejects_non_model_input(self) -> None:
        with self.assertRaises(TypeError):
            validate_assessment_model(object())  # type: ignore[arg-type]


class FailClosedReferenceTests(unittest.TestCase):
    def test_missing_referenced_evidence_record_fails_closed(self) -> None:
        # Property 16 spirit: removing a required Evidence_Record that other
        # objects reference must fail with the affected object IDs (Req 9.7).
        model = build_valid_model()
        broken = dataclasses.replace(model, evidence_records=())
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(EVD_UNKNOWN_REFERENCE, _codes(result))
        # The offending references are surfaced with the missing record id.
        offending = {
            ref
            for finding in result.findings
            if finding.code == EVD_UNKNOWN_REFERENCE
            for ref in finding.object_refs
        }
        self.assertIn("ev-hosted", offending)

    def test_assessment_for_unknown_domain_fails_closed(self) -> None:
        model = build_valid_model()
        stray = dataclasses.replace(
            _kernel_assessment(), domain_id="domain-missing"
        )
        broken = dataclasses.replace(
            model, assessments=(_hosted_assessment(), stray)
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        codes = _codes(result)
        self.assertIn(RPT_UNKNOWN_DOMAIN, codes)
        # The real kernel domain now has no assessment -> parity failure.
        self.assertIn(RPT_DOMAIN_ASSESSMENT_PARITY, codes)

    def test_conflict_referencing_unknown_evidence_fails_closed(self) -> None:
        model = build_valid_model()
        bad_conflict = EvidenceConflict(
            id=StableId("conflict-bad"),
            claim_key="dangling",
            evidence_ids=("ev-hosted", "ev-ghost"),
            incompatible_values=("a", "b"),
            locations=(
                SourceLocation(kind=LocationKind.HEADING, value=_README_ANCHOR),
                SourceLocation(kind=LocationKind.HEADING, value=_SPEC_ANCHOR),
            ),
            blocking=True,
        )
        broken = dataclasses.replace(model, conflicts=(bad_conflict,))
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(CNF_UNKNOWN_EVIDENCE, _codes(result))

    def test_evidence_path_absent_from_inventory_fails_closed(self) -> None:
        model = build_valid_model()
        stray = _record(
            record_id="ev-stray",
            source_path="not/in/inventory.md",
            anchor="Ghost",
        )
        broken = dataclasses.replace(
            model, evidence_records=(*model.evidence_records, stray)
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(EVD_UNKNOWN_PATH, _codes(result))


class FailClosedCoverageTests(unittest.TestCase):
    def test_missing_required_source_category_fails_closed(self) -> None:
        model = build_valid_model()
        # Drop every README inventory entry to remove that required category.
        trimmed = tuple(
            entry
            for entry in model.source_inventory
            if entry.category is not SourceCategory.RUNTIME
        )
        broken = dataclasses.replace(model, source_inventory=trimmed)
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(INV_MISSING_CATEGORY, _codes(result))

    def test_incomplete_target_model_fails_closed(self) -> None:
        model = build_valid_model()
        broken = dataclasses.replace(
            model,
            target_levels=(
                TargetLevel.T0_HOSTED_ADJACENCY,
                TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            ),
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(RPT_TARGET_LEVELS, _codes(result))

    def test_missing_mandatory_section_fails_closed(self) -> None:
        model = build_valid_model()
        broken = dataclasses.replace(model, non_claims=())
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertIn(RPT_MISSING_SECTION, _codes(result))


class FailClosedGraphTests(unittest.TestCase):
    def test_cyclic_hard_gate_graph_fails_closed(self) -> None:
        model = build_valid_model()
        hosted_gate = dataclasses.replace(
            _hosted_gate(), dependency_ids=("gate-kernel",)
        )
        broken = dataclasses.replace(
            model, hard_gates=(hosted_gate, _kernel_gate())
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertTrue(
            any(code.startswith("GRF-") for code in _codes(result)),
            msg=f"expected a GRF-* finding, got {_codes(result)}",
        )


class FailClosedTrustTests(unittest.TestCase):
    def test_unrecorded_trust_assumption_fails_closed(self) -> None:
        model = build_valid_model()
        risky = _record(
            record_id="ev-hosted",
            source_path=_README_PATH,
            anchor=_README_ANCHOR,
            claim="The build relies on a trusted toolchain to link artifacts.",
        )
        broken = dataclasses.replace(
            model, evidence_records=(risky, model.evidence_records[1])
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        self.assertTrue(
            any(code.startswith("CLM-") for code in _codes(result)),
            msg=f"expected a CLM-* finding, got {_codes(result)}",
        )


class FailClosedMaturityTests(unittest.TestCase):
    def test_substrate_domain_without_evidence_cannot_score_nonzero(self) -> None:
        model = build_valid_model()
        nonzero_kernel = _kernel_assessment(
            raw=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
            effective=MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION,
            evidence_ids=(),
        )
        broken = dataclasses.replace(
            model, assessments=(_hosted_assessment(), nonzero_kernel)
        )
        result = validate_assessment_model(broken)
        self.assertFalse(result.valid)
        codes = _codes(result)
        self.assertIn(MAT_NO_EVIDENCE_NONZERO, codes)
        self.assertIn(RPT_INITIAL_CONCLUSION, codes)


if __name__ == "__main__":
    unittest.main()
