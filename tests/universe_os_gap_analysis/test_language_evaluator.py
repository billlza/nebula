from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis import (
    ASPECT_LANGUAGE_SEMANTICS,
    ASPECT_LOW_LEVEL_SEMANTICS,
    ASPECT_TYPE_SYSTEM,
    ChecklistEvaluator,
    ChecklistItem,
    DeclarativeChecklist,
    DomainDraft,
    EvidenceLayer,
    LANGUAGE_TYPE_SYSTEM_CHECKLIST,
    evaluate_language_type_system,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    CapabilityDomain,
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-language-evaluator-test")


def _record(
    *,
    claim_key: str,
    source_path: str,
    evidence_kind: EvidenceKind = EvidenceKind.SPECIFICATION,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    claim: str | None = None,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, evidence_kind.value, status.value),
        claim_key=claim_key,
        claim=claim if claim is not None else f"{claim_key} documented at {source_path}.",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}"),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


def _spec_only_bundle() -> EvidenceBundle:
    """Every distinct checklist specification file present as spec evidence only."""

    paths: set[str] = set()
    for item in LANGUAGE_TYPE_SYSTEM_CHECKLIST.items:
        paths.update(item.specification_paths)
    return _bundle(
        [
            _record(claim_key=f"source:{path}", source_path=path)
            for path in sorted(paths)
        ]
    )


class ChecklistCoverageTests(unittest.TestCase):
    """Requirements 5.1, 5.2, 5.5: the checklist covers every mandated feature."""

    def _keys_for_aspect(self, aspect: str) -> set[str]:
        return {
            item.key
            for item in LANGUAGE_TYPE_SYSTEM_CHECKLIST.items
            if item.aspect == aspect
        }

    def test_language_semantics_features_are_covered(self) -> None:
        expected = {
            "lexical-rules", "control-flow", "functions", "methods", "modules",
            "visibility", "generics", "traits-protocols", "closures", "patterns",
            "error-effects", "reflection", "macros", "metaprogramming",
        }
        self.assertEqual(self._keys_for_aspect(ASPECT_LANGUAGE_SEMANTICS), expected)

    def test_type_system_features_are_covered(self) -> None:
        expected = {
            "primitive-widths", "pointers", "references", "slices", "arrays",
            "collections", "nullable", "aggregates", "enums", "callable-types",
            "variance", "lifetimes", "constrained-generics", "dynamic-dispatch",
        }
        self.assertEqual(self._keys_for_aspect(ASPECT_TYPE_SYSTEM), expected)

    def test_low_level_prerequisites_are_covered(self) -> None:
        expected = {
            "target-layout", "initialization", "destruction", "aliasing",
            "syscall-boundaries",
        }
        self.assertEqual(self._keys_for_aspect(ASPECT_LOW_LEVEL_SEMANTICS), expected)

    def test_every_item_declares_an_authoritative_specification_path(self) -> None:
        for item in LANGUAGE_TYPE_SYSTEM_CHECKLIST.items:
            self.assertTrue(
                item.specification_paths,
                f"{item.key} must cite an authoritative specification source",
            )

    def test_domain_targets_the_independent_language_platform(self) -> None:
        self.assertIs(
            LANGUAGE_TYPE_SYSTEM_CHECKLIST.target_level,
            TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        )


class DomainDraftShapeTests(unittest.TestCase):
    def test_domain_is_built_with_checklist_and_gap_references(self) -> None:
        draft = evaluate_language_type_system(_spec_only_bundle())
        self.assertIsInstance(draft, DomainDraft)
        domain = draft.domain
        self.assertIsInstance(domain, CapabilityDomain)
        self.assertEqual(
            len(domain.checklist_ids), len(LANGUAGE_TYPE_SYSTEM_CHECKLIST.items)
        )
        self.assertEqual(len(draft.findings), len(LANGUAGE_TYPE_SYSTEM_CHECKLIST.items))
        gap_ids = {str(gap.id) for gap in draft.gaps}
        self.assertEqual(gap_ids, {str(ref) for ref in domain.gap_ids})

    def test_every_gap_has_exactly_one_primary_category(self) -> None:
        draft = evaluate_language_type_system(_spec_only_bundle())
        for gap in draft.gaps:
            self.assertIsInstance(gap, GapEntry)
            self.assertIn(
                gap.primary_category,
                {GapCategory.LANGUAGE, GapCategory.VERIFICATION},
            )
            self.assertNotIn(gap.primary_category, gap.secondary_categories)


class LanguageGapTests(unittest.TestCase):
    """Requirement 5.3: documented features yield authoritative Language_Gaps."""

    def test_documented_feature_creates_language_gap_with_source_and_impl(self) -> None:
        records = [
            _record(claim_key="source:spec/language_core.md", source_path="spec/language_core.md"),
            _record(claim_key="source:spec/grammar.ebnf", source_path="spec/grammar.ebnf"),
            _record(
                claim_key="source:frontend/parser.cpp",
                source_path="frontend/parser.cpp",
                evidence_kind=EvidenceKind.SOURCE,
            ),
        ]
        draft = evaluate_language_type_system(_bundle(records))
        language_gaps = draft.gaps_by_category(GapCategory.LANGUAGE)
        # One Language_Gap per documented checklist item.
        self.assertEqual(len(language_gaps), len(LANGUAGE_TYPE_SYSTEM_CHECKLIST.items))
        control_flow_gap = next(
            gap for gap in language_gaps if "Control flow" in gap.title
        )
        self.assertIn("spec/language_core.md", control_flow_gap.observed_fact)
        # Direct implementation evidence is referenced in the observed fact.
        impl_id = stable_id(
            "evidence",
            "source:frontend/parser.cpp",
            "frontend/parser.cpp",
            EvidenceKind.SOURCE.value,
            EvidenceStatus.EXPERIMENTAL.value,
        )
        self.assertIn(str(impl_id), control_flow_gap.observed_fact)

    def test_language_gap_references_the_domain(self) -> None:
        draft = evaluate_language_type_system(_spec_only_bundle())
        domain_ref = reference(draft.domain.id)
        for gap in draft.gaps_by_category(GapCategory.LANGUAGE):
            self.assertIn(domain_ref, gap.domain_ids)


class VerificationGapTests(unittest.TestCase):
    """Requirement 5.4: implementation without a compatibility policy is unstable."""

    def test_parser_support_without_compat_policy_creates_verification_gap(self) -> None:
        records = [
            _record(claim_key="source:spec/language_core.md", source_path="spec/language_core.md"),
            _record(
                claim_key="source:frontend/parser.cpp",
                source_path="frontend/parser.cpp",
                evidence_kind=EvidenceKind.SOURCE,
            ),
        ]
        draft = evaluate_language_type_system(_bundle(records))
        finding = draft.finding_for("control-flow")
        self.assertTrue(finding.has_parser_typechecker)
        self.assertFalse(finding.has_compatibility_policy)
        verification_gaps = draft.gaps_by_category(GapCategory.VERIFICATION)
        self.assertTrue(
            any("Control flow" in gap.title for gap in verification_gaps)
        )

    def test_compatibility_policy_evidence_suppresses_verification_gap(self) -> None:
        records = [
            _record(claim_key="source:spec/grammar.ebnf", source_path="spec/grammar.ebnf"),
            _record(claim_key="source:spec/language_core.md", source_path="spec/language_core.md"),
            _record(
                claim_key="source:frontend/lexer.cpp",
                source_path="frontend/lexer.cpp",
                evidence_kind=EvidenceKind.SOURCE,
            ),
            _record(
                claim_key="source:docs/stability_policy.md",
                source_path="docs/stability_policy.md",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
            ),
        ]
        draft = evaluate_language_type_system(_bundle(records))
        finding = draft.finding_for("lexical-rules")
        self.assertTrue(finding.has_specification)
        self.assertTrue(finding.has_parser_typechecker)
        self.assertTrue(finding.has_compatibility_policy)
        lexical_verification = [
            gap
            for gap in draft.gaps_by_category(GapCategory.VERIFICATION)
            if "Lexical rules" in gap.title
        ]
        self.assertEqual(lexical_verification, [])

    def test_documented_only_feature_has_no_verification_gap(self) -> None:
        # reflection is documented but has no implementation entries at all.
        draft = evaluate_language_type_system(_spec_only_bundle())
        finding = draft.finding_for("reflection")
        self.assertTrue(finding.has_specification)
        self.assertFalse(finding.has_parser_typechecker)
        self.assertEqual(
            [gap for gap in draft.gaps if "Reflection" in gap.title and gap.primary_category is GapCategory.VERIFICATION],
            [],
        )


class CurrentStatusTests(unittest.TestCase):
    def test_status_is_unknown_without_matching_evidence(self) -> None:
        draft = evaluate_language_type_system(_bundle([]))
        finding = draft.finding_for("functions")
        self.assertIs(finding.current_status, EvidenceStatus.UNKNOWN)

    def test_status_uses_strongest_matched_status(self) -> None:
        records = [
            _record(
                claim_key="source:spec/type_system.md",
                source_path="spec/type_system.md",
                status=EvidenceStatus.PLANNED,
            ),
            _record(
                claim_key="source:spec/abi_layout.md",
                source_path="spec/abi_layout.md",
                status=EvidenceStatus.EXPERIMENTAL,
            ),
        ]
        draft = evaluate_language_type_system(_bundle(records))
        finding = draft.finding_for("primitive-widths")
        self.assertIs(finding.current_status, EvidenceStatus.EXPERIMENTAL)


class DeterminismTests(unittest.TestCase):
    def test_result_is_independent_of_evidence_order(self) -> None:
        records = [
            _record(claim_key="source:spec/language_core.md", source_path="spec/language_core.md"),
            _record(claim_key="source:spec/type_system.md", source_path="spec/type_system.md"),
            _record(
                claim_key="source:frontend/parser.cpp",
                source_path="frontend/parser.cpp",
                evidence_kind=EvidenceKind.SOURCE,
            ),
        ]
        forward = evaluate_language_type_system(_bundle(records))
        backward = evaluate_language_type_system(_bundle(list(reversed(records))))
        self.assertEqual(
            [str(gap.id) for gap in forward.gaps],
            [str(gap.id) for gap in backward.gaps],
        )
        self.assertEqual(str(forward.domain.id), str(backward.domain.id))


class SharedFrameworkReuseTests(unittest.TestCase):
    """The declarative framework is reusable by later evaluators (5.2, 6.x, 7.x)."""

    def test_custom_checklist_produces_language_and_verification_gaps(self) -> None:
        checklist = DeclarativeChecklist(
            domain_key="demo-domain",
            name="Demo domain",
            target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
            description="A minimal reusable checklist for framework validation.",
            items=(
                ChecklistItem(
                    key="demo-feature",
                    title="Demo feature",
                    aspect="demo",
                    specification_paths=("spec/abi_layout.md",),
                    implementation_entries=("nir/",),
                    acceptance_evidence=("A normative demo contract.",),
                ),
            ),
        )
        records = [
            _record(claim_key="source:spec/abi_layout.md", source_path="spec/abi_layout.md"),
            _record(
                claim_key="source:nir/lower.cpp",
                source_path="nir/lower.cpp",
                evidence_kind=EvidenceKind.SOURCE,
            ),
        ]
        draft = ChecklistEvaluator().evaluate(checklist, _bundle(records))
        self.assertIs(draft.domain.target_level, TargetLevel.T2_FREESTANDING_SUBSTRATE)
        self.assertEqual(len(draft.gaps_by_category(GapCategory.LANGUAGE)), 1)
        self.assertEqual(len(draft.gaps_by_category(GapCategory.VERIFICATION)), 1)
        finding = draft.finding_for("demo-feature")
        self.assertEqual(
            finding.evidence_by_layer[EvidenceLayer.PARSER_TYPECHECKER],
            (reference(stable_id(
                "evidence",
                "source:nir/lower.cpp",
                "nir/lower.cpp",
                EvidenceKind.SOURCE.value,
                EvidenceStatus.EXPERIMENTAL.value,
            )),),
        )

    def test_directory_prefix_only_matches_source_kinds(self) -> None:
        checklist = DeclarativeChecklist(
            domain_key="demo-domain-2",
            name="Demo domain 2",
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            description="Directory-prefix matching should ignore non-implementation kinds.",
            items=(
                ChecklistItem(
                    key="demo",
                    title="Demo",
                    aspect="demo",
                    specification_paths=("spec/type_system.md",),
                    implementation_entries=("passes/",),
                    acceptance_evidence=("A normative demo contract.",),
                ),
            ),
        )
        # A specification-kind record under passes/ must not count as implementation.
        records = [
            _record(claim_key="source:spec/type_system.md", source_path="spec/type_system.md"),
            _record(
                claim_key="source:passes/typecheck.md",
                source_path="passes/typecheck.md",
                evidence_kind=EvidenceKind.SPECIFICATION,
            ),
        ]
        draft = ChecklistEvaluator().evaluate(checklist, _bundle(records))
        finding = draft.finding_for("demo")
        self.assertFalse(finding.has_parser_typechecker)
        self.assertEqual(draft.gaps_by_category(GapCategory.VERIFICATION), ())


class ChecklistValidationTests(unittest.TestCase):
    def test_duplicate_item_keys_are_rejected(self) -> None:
        item = ChecklistItem(
            key="dup",
            title="Dup",
            aspect="demo",
            specification_paths=("spec/type_system.md",),
            acceptance_evidence=("A contract.",),
        )
        with self.assertRaises(ValueError):
            DeclarativeChecklist(
                domain_key="d",
                name="D",
                target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
                description="dup keys",
                items=(item, item),
            )

    def test_directory_prefix_entry_is_validated(self) -> None:
        with self.assertRaises(ValueError):
            ChecklistItem(
                key="bad",
                title="Bad",
                aspect="demo",
                specification_paths=("../escape/",),
                acceptance_evidence=("A contract.",),
            )


if __name__ == "__main__":
    unittest.main()
