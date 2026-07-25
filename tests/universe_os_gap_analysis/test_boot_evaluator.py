"""Unit tests for the boot evaluator and pre-kernel gate generation (Task 6.3).

These tests exercise Requirements 7.7, 10.1, 12.5, 12.7, and 15.7 against the
real evidence and claim-guard layers (no mocks):

* the nine boot stages are modelled as separate evidence, separate domains, and
  separate Hard-Gate candidates that never merge (7.7);
* the candidate dependency chain runs low-level soundness -> system ABI ->
  (backend/bootstrap parallel to boot toolchain) -> linked-ELF join -> media ->
  QEMU (12.5, 12.7, 15.7); and
* a primitive ET_REL object proof never satisfies the later stages (7.7, 15.7).
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.boot import (
    BOOT_CHAIN_SPECS,
    BRANCH_BACKEND_BOOTSTRAP,
    BRANCH_BOOT_TOOLCHAIN,
    JOIN_GATE_KEY,
    BootEvaluator,
    BootStageKind,
    evaluate_boot,
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
    HardGate,
    LocationKind,
    MaturityScore,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-boot-test")

# The nine boot stages that must remain separate (Requirement 7.7).
_BOOT_STAGES = (
    "target-spec",
    "boot-protocol",
    "boot-entry",
    "linker-script-input",
    "relocation",
    "startup-object",
    "deterministic-linked-elf",
    "boot-media",
    "qemu-execution",
)


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.CURRENT_WORKTREE,
    source_path: str = "boot/boot.md",
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


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key[record.claim_key] = by_claim_key.get(record.claim_key, ()) + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class DecompositionTests(unittest.TestCase):
    def test_nine_boot_stages_have_separate_domains_and_gates(self) -> None:
        """Requirement 7.7: object/link/media/execute never merge."""

        result = evaluate_boot(_bundle())
        for key in _BOOT_STAGES:
            self.assertIsNotNone(result.domain_for(key), f"missing domain for {key}")
            self.assertIsNotNone(result.gate_for(key), f"missing gate for {key}")
            self.assertIsNotNone(result.gap_for(key), f"missing gap for {key}")
        # Exactly nine boot-stage domains, and each maps to a distinct gate.
        self.assertEqual(len(result.domains), len(_BOOT_STAGES))
        gate_ids = {str(g.id) for g in result.hard_gates}
        self.assertEqual(len(gate_ids), len(BOOT_CHAIN_SPECS))

    def test_upstream_candidates_are_gates_only(self) -> None:
        """Upstream chain nodes emit gate candidates but no domain/gap."""

        result = evaluate_boot(_bundle())
        for key in (
            "low-level-soundness",
            "system-abi",
            "independent-backend-bootstrap",
            "freestanding-runtime",
            "primitive-et-rel-object",
        ):
            self.assertIsNotNone(result.gate_for(key), f"missing gate for {key}")
            self.assertIsNone(result.domain_for(key))
            self.assertIsNone(result.gap_for(key))

    def test_all_gates_are_valid_hardgates(self) -> None:
        result = evaluate_boot(_bundle())
        self.assertTrue(all(isinstance(gate, HardGate) for gate in result.hard_gates))


class DependencyChainTests(unittest.TestCase):
    def test_chain_order_soundness_to_qemu(self) -> None:
        """Requirement 12.5/15.7: the sequenced pre-kernel dependency chain."""

        result = evaluate_boot(_bundle())
        edges = set(result.dependency_edges())
        # low-level soundness -> system ABI
        self.assertIn(("low-level-soundness", "system-abi"), edges)
        # system ABI branches into backend/bootstrap and the boot toolchain
        self.assertIn(("system-abi", "independent-backend-bootstrap"), edges)
        self.assertIn(("system-abi", "target-spec"), edges)
        # backend/bootstrap -> freestanding runtime
        self.assertIn(("independent-backend-bootstrap", "freestanding-runtime"), edges)
        # media follows the linked-ELF join; QEMU follows media
        self.assertIn((JOIN_GATE_KEY, "boot-media"), edges)
        self.assertIn(("boot-media", "qemu-execution"), edges)

    def test_linked_elf_join_consumes_both_branches_and_object(self) -> None:
        """Requirement 12.7: the join gate merges the two branches and the object input."""

        result = evaluate_boot(_bundle())
        join = result.gate_for(JOIN_GATE_KEY)
        assert join is not None
        dep_ids = {str(ref) for ref in join.dependency_ids}

        def gate_id(key: str) -> str:
            return str(stable_id("gate", "boot", key))

        # runtime branch predecessor + primitive object input + six toolchain stages
        self.assertIn(gate_id("freestanding-runtime"), dep_ids)
        self.assertIn(gate_id("primitive-et-rel-object"), dep_ids)
        for toolchain in (
            "target-spec",
            "boot-protocol",
            "boot-entry",
            "linker-script-input",
            "relocation",
            "startup-object",
        ):
            self.assertIn(gate_id(toolchain), dep_ids)

    def test_parallel_branches_declare_join(self) -> None:
        """Requirement 12.7: independent branches converge only through the join gate."""

        result = evaluate_boot(_bundle())
        join_ref = reference(stable_id("gate", "boot", JOIN_GATE_KEY))

        backend_branch = [
            g for g in result.hard_gates if g.parallel_branch == BRANCH_BACKEND_BOOTSTRAP
        ]
        toolchain_branch = [
            g for g in result.hard_gates if g.parallel_branch == BRANCH_BOOT_TOOLCHAIN
        ]
        self.assertTrue(backend_branch)
        self.assertEqual(len(toolchain_branch), 6)

        # The immediate predecessors of the join declare it as their join gate.
        runtime = result.gate_for("freestanding-runtime")
        assert runtime is not None
        self.assertIn(join_ref, runtime.join_gate_ids)
        for toolchain in (
            "target-spec",
            "boot-protocol",
            "boot-entry",
            "linker-script-input",
            "relocation",
            "startup-object",
        ):
            gate = result.gate_for(toolchain)
            assert gate is not None
            self.assertIn(join_ref, gate.join_gate_ids)

    def test_candidate_chain_is_acyclic_and_reaches_qemu(self) -> None:
        result = evaluate_boot(_bundle())
        edges = result.dependency_edges()
        # Topologically reachable: qemu depends (transitively) on soundness.
        adjacency: dict[str, set[str]] = {}
        for dep, dependent in edges:
            adjacency.setdefault(dependent, set()).add(dep)

        seen: set[str] = set()
        stack = ["qemu-execution"]
        while stack:
            node = stack.pop()
            if node in seen:
                continue
            seen.add(node)
            stack.extend(adjacency.get(node, set()))
        self.assertIn("low-level-soundness", seen)
        self.assertIn("system-abi", seen)
        self.assertIn(JOIN_GATE_KEY, seen)


class PrimitiveObjectIsolationTests(unittest.TestCase):
    def test_primitive_object_does_not_satisfy_later_stages(self) -> None:
        """Requirement 7.7/15.7: an ET_REL proof never satisfies later stages."""

        bundle = _bundle(
            _record(
                claim_key="source:codegen/freestanding",
                claim=(
                    "Clang-backed ELF64 relocatable object (ET_REL) emission for the "
                    "primitive freestanding type subset."
                ),
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="codegen/freestanding_cpp_emitter.cpp",
            )
        )
        result = evaluate_boot(bundle)

        # The primitive-object gate is recognized (matched, maturity >= narrow).
        primitive = result.assessment_for("primitive-et-rel-object")
        assert primitive is not None
        self.assertTrue(primitive.supporting_evidence_ids)
        self.assertEqual(primitive.maturity, MaturityScore.NARROW_EXPERIMENT)
        self.assertTrue(primitive.satisfied)

        # But the later stages remain unsatisfied at maturity 0 with no evidence.
        for later in ("deterministic-linked-elf", "boot-media", "qemu-execution"):
            assessment = result.assessment_for(later)
            assert assessment is not None
            self.assertFalse(assessment.satisfied, f"{later} must not be satisfied")
            self.assertEqual(assessment.maturity, MaturityScore.ABSENT)
            self.assertEqual(assessment.supporting_evidence_ids, ())
            gate = result.gate_for(later)
            assert gate is not None
            self.assertEqual(gate.maturity_score, MaturityScore.ABSENT)
            self.assertEqual(gate.status, EvidenceStatus.UNSUPPORTED)
            self.assertIsNotNone(result.gap_for(later))

    def test_join_gate_non_claim_rejects_primitive_object(self) -> None:
        result = evaluate_boot(_bundle())
        join = result.gate_for(JOIN_GATE_KEY)
        assert join is not None
        joined = "\n".join(join.non_claims).lower()
        self.assertIn("et_rel", joined)
        self.assertIn("does not satisfy", joined)

    def test_primitive_evidence_does_not_leak_into_relocation_stage(self) -> None:
        """ET_REL markers are disjoint from the relocation-support stage markers."""

        bundle = _bundle(
            _record(
                claim_key="source:codegen/freestanding",
                claim="Relocatable object (ET_REL) emission only.",
                source_path="codegen/freestanding_cpp_emitter.cpp",
            )
        )
        result = evaluate_boot(bundle)
        relocation = result.assessment_for("relocation")
        assert relocation is not None
        self.assertEqual(relocation.supporting_evidence_ids, ())
        self.assertFalse(relocation.satisfied)


class StageSatisfactionTests(unittest.TestCase):
    def test_boot_media_satisfied_only_by_direct_current_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:boot/media",
                claim="A bootable disk image is assembled from the linked ELF image.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="boot/media.py",
            )
        )
        result = evaluate_boot(bundle)
        media = result.assessment_for("boot-media")
        assert media is not None
        self.assertTrue(media.satisfied)
        self.assertEqual(media.maturity, MaturityScore.NARROW_EXPERIMENT)
        # A satisfied stage produces no gap.
        self.assertIsNone(result.gap_for("boot-media"))

    def test_specification_only_evidence_does_not_satisfy_stage(self) -> None:
        """Documented-but-unimplemented boot protocol is a gap, not a satisfied stage."""

        bundle = _bundle(
            _record(
                claim_key="source:docs/boot",
                claim="The boot protocol and firmware handoff are documented.",
                status=EvidenceStatus.EXPERIMENTAL,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/universeos/qemu_boot_hello.md",
            )
        )
        result = evaluate_boot(bundle)
        protocol = result.assessment_for("boot-protocol")
        assert protocol is not None
        self.assertFalse(protocol.satisfied)
        self.assertEqual(protocol.maturity, MaturityScore.ABSENT)
        gap = result.gap_for("boot-protocol")
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)


class GapAndDomainTests(unittest.TestCase):
    def test_boot_stage_gaps_are_high_severity_implementation_gaps(self) -> None:
        result = evaluate_boot(_bundle())
        for key in _BOOT_STAGES:
            gap = result.gap_for(key)
            assert gap is not None
            self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
            self.assertIn(gap.severity.value, {"High", "Critical"})

    def test_target_levels_split_toolchain_t2_and_chain_t3(self) -> None:
        result = evaluate_boot(_bundle())
        for toolchain in (
            "target-spec",
            "boot-protocol",
            "boot-entry",
            "linker-script-input",
            "relocation",
            "startup-object",
        ):
            domain = result.domain_for(toolchain)
            assert domain is not None
            self.assertEqual(domain.target_level, TargetLevel.T2_FREESTANDING_SUBSTRATE)
        for downstream in ("deterministic-linked-elf", "boot-media", "qemu-execution"):
            domain = result.domain_for(downstream)
            assert domain is not None
            self.assertEqual(
                domain.target_level, TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION
            )

    def test_qemu_gate_retains_kernel_non_claims(self) -> None:
        """Requirement 10.1/15.7: a serial hello does not imply the rest of the OS."""

        result = evaluate_boot(_bundle())
        qemu = result.gate_for("qemu-execution")
        assert qemu is not None
        text = "\n".join(qemu.non_claims).lower()
        for concept in ("drivers", "interrupts", "mmu", "scheduler", "syscalls"):
            self.assertIn(concept, text)


class DeterminismTests(unittest.TestCase):
    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="source:a",
                claim="Relocatable object (ET_REL) emission.",
                source_path="codegen/a.cpp",
            ),
            _record(
                claim_key="source:b",
                claim="Linker script controls section placement.",
                source_path="boot/link.ld",
            ),
            _record(
                claim_key="source:c",
                claim="QEMU serial output during boot execution.",
                source_path="boot/run.sh",
            ),
        )
        forward = evaluate_boot(_bundle(*records))
        reverse = evaluate_boot(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.hard_gates],
            [str(g.id) for g in reverse.hard_gates],
        )
        self.assertEqual(
            [str(g.id) for g in forward.gaps],
            [str(g.id) for g in reverse.gaps],
        )

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:boot",
                claim="Boot protocol handoff documented.",
                source_path="docs/universeos/qemu_boot_hello.md",
            )
        )
        guarded = guard_evidence(bundle)
        result = BootEvaluator().evaluate(bundle, guarded)
        self.assertEqual(len(result.domains), len(_BOOT_STAGES))

    def test_spec_kinds_partition_stages_and_upstream(self) -> None:
        boot_stages = [s for s in BOOT_CHAIN_SPECS if s.kind is BootStageKind.BOOT_STAGE]
        upstream = [
            s for s in BOOT_CHAIN_SPECS if s.kind is BootStageKind.UPSTREAM_CANDIDATE
        ]
        self.assertEqual(len(boot_stages), len(_BOOT_STAGES))
        self.assertEqual(len(upstream), 5)


if __name__ == "__main__":
    unittest.main()
