"""Unit tests for the memory/ownership/concurrency/unsafe evaluator (Task 5.2).

These tests exercise Requirement 6 behaviours against the real evidence and
claim-guard layers (no mocks): assistance vs normative safety, hosted cooperative
async vs scheduler-independent concurrency, and exclusion disclosure propagation.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.memory_concurrency_safety import (
    MEMORY_SAFETY_CHECKLIST,
    ConcurrencyModelStrength,
    MemoryConcurrencySafetyEvaluator,
    SafetyModelStrength,
    evaluate_memory_concurrency_safety,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-memory-safety-test")

_OWNERSHIP = "capability-ownership-borrow-model"
_MEMORY = "capability-memory-storage-model"
_CONCURRENCY = "capability-concurrency-model"
_SCHED = "capability-scheduler-independent-concurrency"
_UNSAFE = "capability-unsafe-ffi-boundary"
_HW = "capability-hardware-lowlevel-primitives"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "spec/safety_contract.md",
    limitations: tuple[str, ...] = (),
    trust_assumptions: tuple[str, ...] = (),
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
        limitations=limitations,
        trust_assumptions=trust_assumptions,
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class SafetyModelClassificationTests(unittest.TestCase):
    def test_assistance_only_does_not_satisfy_normative_safety(self) -> None:
        """Requirement 6.2 / Property 11: borrow assistance is not a normative model."""

        bundle = _bundle(
            _record(
                claim_key="source:spec/rep_owner_model.md",
                claim="Rep x Owner inference and conservative borrow assistance track ownership.",
                source_path="spec/rep_owner_model.md",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        self.assertEqual(result.safety_model_strength, SafetyModelStrength.ASSISTANCE_ONLY)
        draft = result.draft_for(_OWNERSHIP)
        assert draft is not None
        self.assertFalse(draft.satisfied)
        gap = result.gap_for(_OWNERSHIP)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.LANGUAGE)

    def test_normative_specification_satisfies_ownership(self) -> None:
        """A normative move/borrow/lifetime/aliasing model satisfies the capability."""

        bundle = _bundle(
            _record(
                claim_key="source:spec/rep_owner_model.md",
                claim=(
                    "A normative move, borrow, lifetime, and aliasing model with a "
                    "borrow checker governs ownership."
                ),
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="spec/rep_owner_model.md",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        self.assertEqual(result.safety_model_strength, SafetyModelStrength.NORMATIVE)
        draft = result.draft_for(_OWNERSHIP)
        assert draft is not None
        self.assertTrue(draft.satisfied)
        self.assertIsNone(result.gap_for(_OWNERSHIP))

    def test_absent_ownership_evidence_is_absent_strength(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/grammar.ebnf",
                claim="Lexical grammar for identifiers.",
                source_path="spec/grammar.ebnf",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        self.assertEqual(result.safety_model_strength, SafetyModelStrength.ABSENT)
        draft = result.draft_for(_OWNERSHIP)
        assert draft is not None
        self.assertFalse(draft.satisfied)


class ConcurrencyClassificationTests(unittest.TestCase):
    def test_hosted_cooperative_async_creates_implementation_gap(self) -> None:
        """Requirement 6.4: hosted cooperative async yields an Implementation_Gap."""

        bundle = _bundle(
            _record(
                claim_key="source:runtime/nebula_runtime.hpp",
                claim=(
                    "Async is a hosted single-threaded cooperative runtime scheduler "
                    "with await points."
                ),
                source_path="runtime/nebula_runtime.hpp",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        self.assertEqual(
            result.concurrency_model_strength,
            ConcurrencyModelStrength.HOSTED_COOPERATIVE_ONLY,
        )
        gap = result.gap_for(_SCHED)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T2_FREESTANDING_SUBSTRATE)

    def test_scheduler_independent_implementation_satisfies(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:runtime/scheduler.cpp",
                claim="A scheduler-independent preemptive scheduler runs tasks on native threads.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="runtime/scheduler.cpp",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        self.assertEqual(
            result.concurrency_model_strength,
            ConcurrencyModelStrength.SCHEDULER_INDEPENDENT,
        )
        draft = result.draft_for(_SCHED)
        assert draft is not None
        self.assertTrue(draft.satisfied)
        self.assertIsNone(result.gap_for(_SCHED))


class ExclusionDisclosureTests(unittest.TestCase):
    def test_unsafe_ffi_gap_carries_disclosed_exclusions(self) -> None:
        """Requirement 6.6: disclosed opaque/dynamic/FFI/unsafe exclusions surface on the gap."""

        bundle = _bundle(
            _record(
                claim_key="source:spec/safety_contract.md",
                claim="Unsafe blocks and FFI boundaries permit raw pointer access.",
                source_path="spec/safety_contract.md",
                limitations=(
                    "The safety guarantee excludes unsafe blocks.",
                    "The safety guarantee excludes ffi boundaries.",
                ),
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        gap = result.gap_for(_UNSAFE)
        assert gap is not None
        joined = "\n".join(gap.acceptance_evidence).lower()
        self.assertIn("unsafe", joined)
        self.assertIn("ffi", joined)
        draft = result.draft_for(_UNSAFE)
        assert draft is not None
        limitation_text = "\n".join(draft.limitations).lower()
        self.assertIn("excludes unsafe", limitation_text)
        self.assertIn("excludes ffi", limitation_text)


class CoverageAndDeterminismTests(unittest.TestCase):
    def test_all_six_domains_are_drafted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/language_core.md",
                claim="Documentation of storage, ownership, concurrency and unsafe topics.",
                source_path="spec/language_core.md",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        drafted_ids = {str(draft.domain.id) for draft in result.domain_drafts}
        expected = {str(item.capability_id) for item in MEMORY_SAFETY_CHECKLIST}
        self.assertEqual(drafted_ids, expected)
        self.assertEqual(len(drafted_ids), 6)

    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="source:spec/rep_owner_model.md",
                claim="Rep x Owner inference and borrow assistance.",
                source_path="spec/rep_owner_model.md",
            ),
            _record(
                claim_key="source:runtime/nebula_runtime.hpp",
                claim="Hosted cooperative async runtime.",
                source_path="runtime/nebula_runtime.hpp",
            ),
            _record(
                claim_key="source:spec/abi_layout.md",
                claim="Volatile and MMIO documented as gaps.",
                source_path="spec/abi_layout.md",
            ),
        )
        forward = evaluate_memory_concurrency_safety(_bundle(*records))
        reverse = evaluate_memory_concurrency_safety(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps],
            [str(g.id) for g in reverse.gaps],
        )
        self.assertEqual(forward.safety_model_strength, reverse.safety_model_strength)
        self.assertEqual(
            forward.concurrency_model_strength, reverse.concurrency_model_strength
        )

    def test_hardware_lowlevel_documented_gap_is_implementation_gap(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/abi_layout.md",
                claim="Volatile access, MMIO, intrinsics and inline assembly are documented gaps.",
                source_path="spec/abi_layout.md",
            )
        )
        result = evaluate_memory_concurrency_safety(bundle)
        gap = result.gap_for(_HW)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T2_FREESTANDING_SUBSTRATE)

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/rep_owner_model.md",
                claim="Rep x Owner inference and borrow assistance.",
                source_path="spec/rep_owner_model.md",
            )
        )
        guarded = guard_evidence(bundle)
        result = MemoryConcurrencySafetyEvaluator().evaluate(bundle, guarded)
        self.assertEqual(result.safety_model_strength, SafetyModelStrength.ASSISTANCE_ONLY)


if __name__ == "__main__":
    unittest.main()
