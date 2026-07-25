"""Unit tests for the ABI/backend evaluator (Task 6.1, Requirement 7.1-7.6).

These tests exercise the ABI/backend evaluator against the real evidence and
claim-guard layers (no mocks): hosted-vs-freestanding ABI scope isolation, the
compiler-pipeline stages, the T1 independence blockers, and the primitive-object
wording bound.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import (
    PRIMITIVE_OBJECT_WORDING,
    guard_evidence,
)
from tools.universe_os_gap_analysis.evaluators.abi_backend import (
    ABI_BACKEND_CHECKLIST,
    AbiBackendEvaluator,
    AbiScope,
    evaluate_abi_backend,
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

_REVISION_REF = reference("revision-abi-backend-test")

_HOSTED = "capability-hosted-c-abi"
_COMPILER = "capability-compiler-abi"
_RUNTIME = "capability-runtime-abi"
_BOOT = "capability-boot-abi"
_SYSCALL = "capability-syscall-abi"
_DRIVER = "capability-driver-abi"
_PACKAGE = "capability-package-abi"
_NATIVE_CODEGEN = "capability-pipeline-native-codegen"
_FRONTEND = "capability-pipeline-frontend"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "spec/abi_layout.md",
    verification_state: VerificationState = VerificationState.NOT_RUN,
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
        verification_state=verification_state,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


def _hosted_c_abi_record() -> EvidenceRecord:
    return _record(
        claim_key="source:spec/interop_c_abi.md",
        claim=(
            "The hosted C ABI covers extern C imports, exported C ABI types, the "
            "calling convention, symbol rules, aggregate layout, enum layout, "
            "alignment, versioning, and cross-language fixtures."
        ),
        evidence_kind=EvidenceKind.SPECIFICATION,
        source_path="spec/interop_c_abi.md",
    )


class ScopeIsolationTests(unittest.TestCase):
    def test_hosted_c_abi_evidence_satisfies_only_hosted_domain(self) -> None:
        """Requirement 7.2 / Property 12: hosted C ABI evidence does not leak scopes."""

        result = evaluate_abi_backend(_bundle(_hosted_c_abi_record()))

        hosted = result.draft_for(_HOSTED)
        assert hosted is not None
        self.assertTrue(hosted.satisfied)
        self.assertIsNone(result.gap_for(_HOSTED))

        for capability_id in (_COMPILER, _RUNTIME, _BOOT, _SYSCALL, _DRIVER, _PACKAGE):
            draft = result.draft_for(capability_id)
            assert draft is not None, capability_id
            self.assertFalse(draft.satisfied, capability_id)
            self.assertEqual(draft.supporting_evidence_ids, ())
            gap = result.gap_for(capability_id)
            assert gap is not None, capability_id
            self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)

    def test_freestanding_abi_needs_current_in_scope_implementation(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:runtime/runtime_abi.hpp",
                    claim="The freestanding runtime ABI defines the runtime ABI calling layout.",
                    status=EvidenceStatus.REPO_PREVIEW,
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="runtime/runtime_abi.hpp",
                )
            )
        )
        draft = result.draft_for(_RUNTIME)
        assert draft is not None
        self.assertTrue(draft.satisfied)
        self.assertIsNone(result.gap_for(_RUNTIME))

    def test_documentation_only_freestanding_abi_is_a_gap(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="spec:docs/universeos/kernel_boundary.md",
                    claim="A syscall ABI is planned but not implemented.",
                    evidence_kind=EvidenceKind.SPECIFICATION,
                    status=EvidenceStatus.PLANNED,
                    source_path="docs/universeos/kernel_boundary.md",
                )
            )
        )
        draft = result.draft_for(_SYSCALL)
        assert draft is not None
        self.assertFalse(draft.satisfied)

    def test_scope_lookup_returns_expected_domain(self) -> None:
        result = evaluate_abi_backend(_bundle(_hosted_c_abi_record()))
        draft = result.draft_for_scope(AbiScope.HOSTED_C_ABI)
        assert draft is not None
        self.assertEqual(str(draft.domain.id), _HOSTED)


class PipelineStageTests(unittest.TestCase):
    def test_frontend_implementation_satisfies(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:frontend/parser.cpp",
                    claim="The frontend parser and typechecker lower source to a typed AST.",
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="frontend/parser.cpp",
                )
            )
        )
        draft = result.draft_for(_FRONTEND)
        assert draft is not None
        self.assertTrue(draft.satisfied)

    def test_native_codegen_not_satisfied_by_primitive_object_evidence(self) -> None:
        """Requirement 7.3/7.6: primitive relocatable-object work is not a native backend."""

        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:codegen/freestanding_cpp_emitter.cpp",
                    claim=(
                        "Native code generation for the experimental primitive object "
                        "path emits a clang-backed ELF relocatable object."
                    ),
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="codegen/freestanding_cpp_emitter.cpp",
                )
            )
        )
        draft = result.draft_for(_NATIVE_CODEGEN)
        assert draft is not None
        self.assertFalse(draft.satisfied)
        gap = result.gap_for(_NATIVE_CODEGEN)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)

    def test_native_codegen_satisfied_by_independent_backend(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:codegen/native_backend.cpp",
                    claim="A Nebula-owned native backend performs instruction selection and machine code emission.",
                    status=EvidenceStatus.REPO_PREVIEW,
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="codegen/native_backend.cpp",
                )
            )
        )
        draft = result.draft_for(_NATIVE_CODEGEN)
        assert draft is not None
        self.assertTrue(draft.satisfied)


class T1IndependenceTests(unittest.TestCase):
    def test_generated_cpp_and_clang_block_t1(self) -> None:
        """Requirement 7.5: generated C++/external clang without bootstrap blocks T1."""

        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:spec/compiler_pipeline.md",
                    claim="Production compilation lowers to generated C++ compiled by external clang++ (host compiler).",
                    source_path="spec/compiler_pipeline.md",
                )
            )
        )
        t1 = result.t1_independence
        self.assertTrue(t1.generated_cpp_dependency)
        self.assertTrue(t1.external_clang_dependency)
        self.assertFalse(t1.achieved)
        self.assertTrue(t1.blocking_reasons)
        gap = result.gap_for("capability-t1-language-platform")
        assert gap is not None
        self.assertEqual(gap.target_level, TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)

    def test_incomplete_inventory_blocks_t1_even_without_cpp_or_clang(self) -> None:
        """Requirement 7.4: an incomplete dependency inventory alone blocks T1."""

        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:frontend/parser.cpp",
                    claim="The frontend parser lowers source to a typed AST.",
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="frontend/parser.cpp",
                )
            )
        )
        t1 = result.t1_independence
        self.assertFalse(t1.generated_cpp_dependency)
        self.assertFalse(t1.external_clang_dependency)
        self.assertFalse(t1.dependency_inventory_complete)
        self.assertFalse(t1.achieved)

    def test_complete_inventory_and_accepted_bootstrap_achieves_t1(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:docs/compiler_dependency_inventory.md",
                    claim=(
                        "The production dependency inventory is complete and an "
                        "accepted independent bootstrap path self-hosts the compiler."
                    ),
                    status=EvidenceStatus.REPO_PREVIEW,
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="docs/compiler_dependency_inventory.md",
                )
            )
        )
        t1 = result.t1_independence
        self.assertTrue(t1.dependency_inventory_complete)
        self.assertTrue(t1.accepted_independent_bootstrap)
        self.assertTrue(t1.achieved)
        self.assertEqual(t1.blocking_reasons, ())
        self.assertIsNone(result.gap_for("capability-t1-language-platform"))

    def test_generated_cpp_with_accepted_bootstrap_does_not_block_t1(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="source:docs/bootstrap.md",
                    claim=(
                        "Although a legacy generated C++ path exists, the production "
                        "dependency inventory is complete and an accepted independent "
                        "bootstrap path is the production build."
                    ),
                    status=EvidenceStatus.REPO_PREVIEW,
                    origin=RevisionOrigin.CURRENT_WORKTREE,
                    source_path="docs/bootstrap.md",
                )
            )
        )
        t1 = result.t1_independence
        self.assertTrue(t1.generated_cpp_dependency)
        self.assertTrue(t1.accepted_independent_bootstrap)
        self.assertTrue(t1.achieved)


class PrimitiveObjectWordingTests(unittest.TestCase):
    def test_validated_primitive_object_gate_marks_passed(self) -> None:
        result = evaluate_abi_backend(
            _bundle(
                _record(
                    claim_key="execution:BLD-017",
                    claim="Primitive freestanding object emission produced an ELF relocatable object.",
                    evidence_kind=EvidenceKind.TEST_EXECUTION,
                    origin=RevisionOrigin.EXECUTION_ARTIFACT,
                    source_path="tests/artifacts/bld-017.log",
                    verification_state=VerificationState.VALIDATED,
                )
            )
        )
        finding = result.primitive_object
        self.assertTrue(finding.gate_passed)
        self.assertEqual(finding.wording, PRIMITIVE_OBJECT_WORDING)

    def test_primitive_wording_flags_forbidden_terms(self) -> None:
        finding = evaluate_abi_backend(_bundle(_hosted_c_abi_record())).primitive_object
        self.assertFalse(finding.gate_passed)
        offending = finding.wording_asserts_forbidden(
            "This proves a direct backend and a linked ELF bootable image."
        )
        self.assertIn("direct backend", offending)
        self.assertIn("linked elf", offending)
        # The canonical wording itself asserts none of the forbidden terms.
        self.assertEqual(finding.wording_asserts_forbidden(finding.wording), ())


class CoverageAndDeterminismTests(unittest.TestCase):
    def test_every_checklist_capability_is_drafted(self) -> None:
        result = evaluate_abi_backend(_bundle(_hosted_c_abi_record()))
        drafted = {str(draft.domain.id) for draft in result.domain_drafts}
        expected = {str(item.capability_id) for item in ABI_BACKEND_CHECKLIST}
        self.assertEqual(drafted, expected)

    def test_all_seven_abi_scopes_are_present(self) -> None:
        result = evaluate_abi_backend(_bundle(_hosted_c_abi_record()))
        scopes = {draft.scope for draft in result.domain_drafts if draft.scope is not None}
        self.assertEqual(scopes, set(AbiScope))

    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _hosted_c_abi_record(),
            _record(
                claim_key="source:spec/compiler_pipeline.md",
                claim="Production compilation lowers to generated C++ compiled by external clang++.",
                source_path="spec/compiler_pipeline.md",
            ),
            _record(
                claim_key="source:frontend/parser.cpp",
                claim="The frontend parser lowers source to a typed AST.",
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="frontend/parser.cpp",
            ),
        )
        forward = evaluate_abi_backend(_bundle(*records))
        reverse = evaluate_abi_backend(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps],
            [str(g.id) for g in reverse.gaps],
        )
        self.assertEqual(forward.t1_achieved, reverse.t1_achieved)

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(_hosted_c_abi_record())
        guarded = guard_evidence(bundle)
        result = AbiBackendEvaluator().evaluate(bundle, guarded)
        hosted = result.draft_for(_HOSTED)
        assert hosted is not None
        self.assertTrue(hosted.satisfied)


if __name__ == "__main__":
    unittest.main()
