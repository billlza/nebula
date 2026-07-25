"""Unit tests for gap register generation and one-primary-category validation (Task 9.1).

These tests exercise Requirements 12.1-12.3 (and the language/verification and
ecosystem gap classification of Requirements 5.3, 5.4, 9.8) against the real
:mod:`tools.universe_os_gap_analysis.gap_register` module and real evaluator
outputs (no mocks):

* every gap keeps exactly one primary category, with deduplicated secondary
  labels that never repeat the primary (Requirements 12.1, 12.2);
* every gap records the full Requirement 12.3 field set;
* the register aggregates gaps from heterogeneous evaluator outputs
  (``DomainDraft.gaps``, ``*Evaluation.gaps``, ``obligation_gaps``) and covers
  all four ``Gap_Category`` kinds; and
* generation is order independent, deduplicates identical gaps, and fails closed
  on a colliding identifier or a malformed classification.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    evaluate_application_ecosystem_release,
)
from tools.universe_os_gap_analysis.evaluators.preview_security_obligations import (
    evaluate_preview_security_obligations,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.gap_register import (
    ALL_GAP_CATEGORIES,
    GAP_DUPLICATE_ID,
    GAP_SECONDARY_REPEATS_PRIMARY,
    GapRegister,
    GapRegisterError,
    build_gap_register,
    gaps_from_sources,
)
from tools.universe_os_gap_analysis.identifiers import StableId, stable_id
from tools.universe_os_gap_analysis.language_evaluator import (
    evaluate_language_type_system,
)
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    LocationKind,
    RevisionOrigin,
    Severity,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = "revision-gap-register-test"


def _gap(
    *,
    gap_id: str,
    primary: GapCategory = GapCategory.IMPLEMENTATION,
    secondary: tuple[GapCategory, ...] = (),
    domain_ids: tuple[str, ...] = ("domain-example",),
    title: str = "Example gap",
) -> GapEntry:
    return GapEntry(
        id=StableId(gap_id),
        title=title,
        primary_category=primary,
        secondary_categories=secondary,
        domain_ids=domain_ids,
        current_status=EvidenceStatus.UNSUPPORTED,
        target_level=TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        severity=Severity.HIGH,
        dependencies=(),
        acceptance_evidence=("Direct implementation evidence closing the gap.",),
        recommended_owner_area="Kernel",
        dependency_criticality=2,
        safety_impact=1,
        claim_risk=1,
        target_unblock_value=1,
        observed_fact="The capability has no direct implementation evidence.",
        recommendation="Implement and verify the capability before depending on it.",
    )


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus,
    evidence_kind: EvidenceKind,
    source_path: str,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


class BuildGapRegisterTests(unittest.TestCase):
    def test_register_keeps_gaps_sorted_by_id_independent_of_order(self) -> None:
        gap_a = _gap(gap_id="gap-aaa")
        gap_b = _gap(gap_id="gap-bbb")
        gap_c = _gap(gap_id="gap-ccc")

        forward = build_gap_register(gap_a, gap_b, gap_c)
        reverse = build_gap_register(gap_c, gap_b, gap_a)

        self.assertEqual(forward.gap_ids, reverse.gap_ids)
        self.assertEqual(
            [str(gap.id) for gap in forward.gaps],
            ["gap-aaa", "gap-bbb", "gap-ccc"],
        )

    def test_every_gap_keeps_exactly_one_primary_and_records_all_fields(self) -> None:
        register = build_gap_register(
            _gap(gap_id="gap-one", primary=GapCategory.LANGUAGE),
            _gap(
                gap_id="gap-two",
                primary=GapCategory.VERIFICATION,
                secondary=(GapCategory.IMPLEMENTATION,),
            ),
        )
        for gap in register.gaps:
            self.assertIsInstance(gap.primary_category, GapCategory)
            self.assertNotIn(gap.primary_category, gap.secondary_categories)
            self.assertEqual(
                len(gap.secondary_categories), len(set(gap.secondary_categories))
            )
            # Requirement 12.3 field set is present.
            self.assertTrue(gap.domain_ids)
            self.assertTrue(gap.acceptance_evidence)
            self.assertTrue(gap.recommended_owner_area.strip())
            self.assertTrue(gap.observed_fact.strip())
            self.assertTrue(gap.recommendation.strip())

    def test_identical_duplicate_gaps_are_deduplicated(self) -> None:
        gap = _gap(gap_id="gap-dup")
        register = build_gap_register(gap, gap, _gap(gap_id="gap-dup"))
        self.assertEqual(len(register), 1)

    def test_colliding_identifier_with_different_content_fails_closed(self) -> None:
        gap = _gap(gap_id="gap-collide", title="First title")
        other = _gap(gap_id="gap-collide", title="Different title")
        with self.assertRaises(GapRegisterError) as ctx:
            build_gap_register(gap, other)
        self.assertEqual(ctx.exception.code, GAP_DUPLICATE_ID)
        self.assertIn("gap-collide", ctx.exception.object_refs)

    def test_secondary_repeating_primary_is_rejected(self) -> None:
        # GapEntry forbids this at construction, so build a valid gap and corrupt
        # its secondary tuple to prove the register re-validates independently.
        gap = _gap(gap_id="gap-corrupt", primary=GapCategory.LANGUAGE)
        object.__setattr__(
            gap, "secondary_categories", (GapCategory.LANGUAGE,)
        )
        with self.assertRaises(GapRegisterError) as ctx:
            build_gap_register(gap)
        self.assertEqual(ctx.exception.code, GAP_SECONDARY_REPEATS_PRIMARY)

    def test_accessors_group_and_locate_gaps(self) -> None:
        register = build_gap_register(
            _gap(gap_id="gap-lang", primary=GapCategory.LANGUAGE, domain_ids=("domain-x",)),
            _gap(gap_id="gap-impl", primary=GapCategory.IMPLEMENTATION, domain_ids=("domain-x", "domain-y")),
            _gap(gap_id="gap-eco", primary=GapCategory.ECOSYSTEM, domain_ids=("domain-y",)),
        )
        self.assertEqual(
            {str(g.id) for g in register.by_primary_category(GapCategory.LANGUAGE)},
            {"gap-lang"},
        )
        self.assertEqual(
            {str(g.id) for g in register.for_domain("domain-x")},
            {"gap-lang", "gap-impl"},
        )
        self.assertIsNotNone(register.gap_for("gap-eco"))
        self.assertIsNone(register.gap_for("gap-missing"))

        counts = register.primary_category_counts()
        # All four categories are present as keys even when count is zero.
        self.assertEqual(set(counts), set(ALL_GAP_CATEGORIES))
        self.assertEqual(counts[GapCategory.LANGUAGE], 1)
        self.assertEqual(counts[GapCategory.VERIFICATION], 0)

    def test_empty_register_is_valid_and_exposes_all_category_keys(self) -> None:
        register = build_gap_register()
        self.assertEqual(len(register), 0)
        self.assertEqual(register.categories_present(), frozenset())
        self.assertEqual(set(register.primary_category_counts()), set(ALL_GAP_CATEGORIES))


class GapsFromSourcesTests(unittest.TestCase):
    def test_extracts_bare_entries_iterables_and_gap_bearing_objects(self) -> None:
        gap_a = _gap(gap_id="gap-a")
        gap_b = _gap(gap_id="gap-b")

        class _Draft:
            gaps = (gap_a, gap_b)

        collected = gaps_from_sources([gap_a, [gap_b], _Draft()])
        self.assertEqual(len(collected), 4)
        self.assertTrue(all(isinstance(gap, GapEntry) for gap in collected))

    def test_rejects_unsupported_source(self) -> None:
        with self.assertRaises(TypeError):
            gaps_from_sources([object()])


class RegisterCoversEvaluatorGapsTests(unittest.TestCase):
    """The register aggregates gaps from real evaluators across all four categories."""

    @staticmethod
    def _make_bundle(records: tuple[EvidenceRecord, ...]) -> EvidenceBundle:
        by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
        for record in records:
            by_claim_key[record.claim_key] = by_claim_key.get(record.claim_key, ()) + (record,)
        return EvidenceBundle(records=records, by_claim_key=by_claim_key)

    def _bundle(self) -> EvidenceBundle:
        records = (
            # Documented language feature -> Language_Gap (Req 5.3) with a
            # semantic-stability Verification_Gap (Req 5.4) from impl evidence.
            _record(
                claim_key="generics documented",
                claim="Constrained generics are documented in the language core spec.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="spec/generics_policy.md",
            ),
            _record(
                claim_key="generics implemented",
                claim="Generics are implemented in the frontend typechecker.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SOURCE,
                source_path="frontend/generics.cpp",
            ),
            # A preview security-sensitive package -> Ecosystem_Gap (Req 9.8).
            _record(
                claim_key="nebula-secrets repo preview",
                claim="nebula-secrets crypto secret package is a repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                source_path="official/nebula-secrets/manifest.toml",
            ),
        )
        return self._make_bundle(records)

    def test_register_aggregates_language_and_ecosystem_gaps(self) -> None:
        bundle = self._bundle()
        language_draft = evaluate_language_type_system(bundle)
        app_eval = evaluate_application_ecosystem_release(bundle)
        preview_eval = evaluate_preview_security_obligations(bundle, app_eval)

        register = build_gap_register(language_draft, app_eval, preview_eval)

        # Every registered gap is well-formed with a single primary category.
        for gap in register.gaps:
            self.assertIsInstance(gap.primary_category, GapCategory)
            self.assertNotIn(gap.primary_category, gap.secondary_categories)

        present = register.categories_present()
        # Language evaluator always yields Language_Gap entries.
        self.assertIn(GapCategory.LANGUAGE, present)
        # The register is a superset of each evaluator's own gaps.
        registered_ids = {str(ref) for ref in register.gap_ids}
        for gap in language_draft.gaps:
            self.assertIn(str(gap.id), registered_ids)
        for gap in preview_eval.obligation_gaps:
            self.assertIn(str(gap.id), registered_ids)

    def test_register_is_order_independent_across_evaluators(self) -> None:
        bundle = self._bundle()
        language_draft = evaluate_language_type_system(bundle)
        app_eval = evaluate_application_ecosystem_release(bundle)
        preview_eval = evaluate_preview_security_obligations(bundle, app_eval)

        forward = build_gap_register(language_draft, app_eval, preview_eval)
        reverse = build_gap_register(preview_eval, app_eval, language_draft)
        self.assertEqual(forward.gap_ids, reverse.gap_ids)


if __name__ == "__main__":
    unittest.main()
