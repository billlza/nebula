from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.universe_os_gap_analysis.test_adapters import _write_adapter_fixture
from tests.universe_os_gap_analysis.test_inventory import _revision
from tools.universe_os_gap_analysis import (
    ClaimGuard,
    ClaimTense,
    EXTERNAL_HOST_COMPILER_NOTE,
    GuardedEvidence,
    MANDATORY_NON_CLAIM_TOPICS,
    PREREQUISITE_GATE_SCOPE_STATEMENT,
    PRIMITIVE_OBJECT_SCOPE_NOTE,
    PRIMITIVE_OBJECT_WORDING,
    adapt_repository_evidence,
    collect_evidence,
    guard_evidence,
)
from tools.universe_os_gap_analysis.claim_guard import (
    PRIMITIVE_OBJECT_FORBIDDEN_TERMS,
    GuardedClaim,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.inventory import discover_source_inventory
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)
from tools.universe_os_gap_analysis.serialization import stable_json_dumps

_REVISION_REF = reference("revision-claim-guard-test")


def _record(
    *,
    claim_key: str,
    status: EvidenceStatus,
    source_path: str = "spec/compiler_pipeline.md",
    claim: str | None = None,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.CURRENT_WORKTREE,
    scope: EvidenceScope | None = None,
    verification_state: VerificationState = VerificationState.NOT_RUN,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, status.value, evidence_kind.value, origin.value),
        claim_key=claim_key,
        claim=claim if claim is not None else f"{status.value} claim for {claim_key}.",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}"),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=scope if scope is not None else EvidenceScope(),
        limitations=(),
        trust_assumptions=(),
        verification_state=verification_state,
    )


def _bundle(records: list[EvidenceRecord]) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] += (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class PresentTenseGatingTests(unittest.TestCase):
    """Requirement 13.1: present tense requires direct current-revision evidence."""

    def test_direct_current_source_implementation_permits_present_tense(self) -> None:
        record = _record(
            claim_key="cap:frontend",
            status=EvidenceStatus.EXPERIMENTAL,
            evidence_kind=EvidenceKind.SOURCE,
            origin=RevisionOrigin.CURRENT_WORKTREE,
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        assert claim is not None
        self.assertTrue(claim.present_tense_permitted)
        self.assertIs(claim.tense, ClaimTense.PRESENT)
        self.assertTrue(claim.guarded_wording.startswith("Implemented"))

    def test_execution_artifact_permits_present_tense(self) -> None:
        record = _record(
            claim_key="cap:gate-run",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="executions/exec.stdout",
            evidence_kind=EvidenceKind.TEST_EXECUTION,
            origin=RevisionOrigin.EXECUTION_ARTIFACT,
            verification_state=VerificationState.VALIDATED,
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        assert claim is not None
        self.assertTrue(claim.present_tense_permitted)
        self.assertIs(claim.tense, ClaimTense.PRESENT)

    def test_tagged_release_ga_does_not_license_present_tense(self) -> None:
        # A GA release note is release evidence, not current-revision
        # implementation evidence, so it cannot claim present tense.
        record = _record(
            claim_key="release:compiler",
            status=EvidenceStatus.COMPILER_TOOLING_GA,
            source_path="RELEASE_NOTES_v1.0.0.md",
            evidence_kind=EvidenceKind.RELEASE,
            origin=RevisionOrigin.TAGGED_RELEASE,
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        assert claim is not None
        self.assertFalse(claim.present_tense_permitted)
        self.assertIs(claim.tense, ClaimTense.NEUTRAL)

    def test_specification_evidence_does_not_license_present_tense(self) -> None:
        record = _record(
            claim_key="gate:UOS-DOC-001",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="docs/universeos/gate_registry.md",
            evidence_kind=EvidenceKind.SPECIFICATION,
            origin=RevisionOrigin.CURRENT_WORKTREE,
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        assert claim is not None
        self.assertFalse(claim.present_tense_permitted)
        self.assertIs(claim.tense, ClaimTense.NEUTRAL)

    def test_planned_is_future_and_unsupported_is_absent(self) -> None:
        planned = _record(
            claim_key="plan:roadmap",
            status=EvidenceStatus.PLANNED,
            source_path="ROADMAP.md",
            evidence_kind=EvidenceKind.SPECIFICATION,
        )
        unsupported = _record(
            claim_key="non-claim:kernel",
            status=EvidenceStatus.UNSUPPORTED,
            source_path="docs/universeos/kernel_boundary.md",
            evidence_kind=EvidenceKind.NON_CLAIM,
        )
        guarded = guard_evidence(_bundle([planned, unsupported]))
        self.assertIs(guarded.claim_for(planned.id).tense, ClaimTense.FUTURE)
        self.assertFalse(guarded.claim_for(planned.id).present_tense_permitted)
        self.assertIs(guarded.claim_for(unsupported.id).tense, ClaimTense.ABSENT)


class StatusPreservationTests(unittest.TestCase):
    """Requirements 4.3, 8.6, 11.6: statuses are preserved, never upgraded."""

    def test_every_status_is_preserved_verbatim(self) -> None:
        records = [
            _record(claim_key=f"cap:{status.value}", status=status)
            for status in EvidenceStatus
            if status is not EvidenceStatus.UNSUPPORTED
        ]
        # Unsupported requires a negative record; add one explicitly.
        records.append(
            _record(
                claim_key="cap:unsupported",
                status=EvidenceStatus.UNSUPPORTED,
                evidence_kind=EvidenceKind.NON_CLAIM,
            )
        )
        guarded = guard_evidence(_bundle(records))
        for record in records:
            self.assertIs(guarded.claim_for(record.id).status, record.status)

    def test_preview_statuses_block_os_substrate_promotion(self) -> None:
        for status in (EvidenceStatus.INSTALLED_PREVIEW, EvidenceStatus.REPO_PREVIEW):
            with self.subTest(status=status):
                record = _record(claim_key=f"package:{status.value}", status=status)
                claim = guard_evidence(_bundle([record])).claim_for(record.id)
                self.assertIs(claim.status, status)
                self.assertTrue(claim.substrate_promotion_blocked)

    def test_scoped_release_evidence_blocks_substrate_promotion(self) -> None:
        for status in (EvidenceStatus.COMPILER_TOOLING_GA, EvidenceStatus.BACKEND_SDK_GA):
            with self.subTest(status=status):
                record = _record(claim_key=f"release:{status.value}", status=status)
                claim = guard_evidence(_bundle([record])).claim_for(record.id)
                self.assertTrue(claim.substrate_promotion_blocked)

    def test_hosted_example_blocks_substrate_promotion(self) -> None:
        record = _record(
            claim_key="example:hosted",
            status=EvidenceStatus.UNKNOWN,
            source_path="examples/hello/main.nb",
            evidence_kind=EvidenceKind.EXAMPLE,
            scope=EvidenceScope(target_levels=(TargetLevel.T0_HOSTED_ADJACENCY,)),
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        self.assertTrue(claim.substrate_promotion_blocked)
        # An example is documentation-only: it never claims present tense.
        self.assertFalse(claim.present_tense_permitted)


class PrimitiveObjectWordingTests(unittest.TestCase):
    """Requirement 7.6: primitive object path is relocatable-object emission only."""

    def test_primitive_object_wording_is_fixed_and_excludes_forbidden_terms(self) -> None:
        record = _record(
            claim_key="gate:UOS-BOOT-001",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="spec/abi_layout.md",
            claim="Experimental primitive freestanding ET_REL object slice emission.",
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        assert claim is not None
        self.assertTrue(claim.is_primitive_object)
        self.assertEqual(claim.guarded_wording, PRIMITIVE_OBJECT_WORDING)
        self.assertEqual(claim.scope_note, PRIMITIVE_OBJECT_SCOPE_NOTE)
        lowered = claim.guarded_wording.lower()
        for term in PRIMITIVE_OBJECT_FORBIDDEN_TERMS:
            self.assertNotIn(term, lowered)

    def test_primitive_object_detected_by_explicit_claim_key(self) -> None:
        record = _record(
            claim_key="gate:UOS-BOOT-777",
            status=EvidenceStatus.EXPERIMENTAL,
            claim="Some object gate without keyword markers.",
        )
        guarded = guard_evidence(
            _bundle([record]), primitive_object_claim_keys=("gate:UOS-BOOT-777",)
        )
        claim = guarded.claim_for(record.id)
        self.assertTrue(claim.is_primitive_object)
        self.assertEqual(claim.guarded_wording, PRIMITIVE_OBJECT_WORDING)

    def test_non_primitive_record_keeps_normal_wording(self) -> None:
        record = _record(claim_key="cap:frontend", status=EvidenceStatus.EXPERIMENTAL)
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        self.assertFalse(claim.is_primitive_object)
        self.assertNotEqual(claim.guarded_wording, PRIMITIVE_OBJECT_WORDING)


class HostCompilerAnnotationTests(unittest.TestCase):
    """Requirement 4.3: external host compiler is an external production dependency."""

    def test_host_compiler_record_is_annotated(self) -> None:
        record = _record(
            claim_key="dependency:host-compiler",
            status=EvidenceStatus.COMPILER_TOOLING_GA,
            source_path="README.md",
            claim="The production pipeline invokes the external host clang++ compiler.",
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        self.assertEqual(claim.production_dependency_note, EXTERNAL_HOST_COMPILER_NOTE)

    def test_unrelated_record_has_no_production_note(self) -> None:
        record = _record(claim_key="cap:frontend", status=EvidenceStatus.EXPERIMENTAL)
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        self.assertIsNone(claim.production_dependency_note)


class NonClaimAndScopeTests(unittest.TestCase):
    """Requirements 13.6, 13.7."""

    def test_all_mandatory_non_claims_persist_by_default(self) -> None:
        guarded = guard_evidence(_bundle([_record(claim_key="cap:x", status=EvidenceStatus.EXPERIMENTAL)]))
        topics = {item.topic for item in guarded.non_claims}
        self.assertEqual(topics, {topic for topic, _ in MANDATORY_NON_CLAIM_TOPICS})
        self.assertEqual(guarded.released_non_claim_topics, ())

    def test_accepted_gate_topic_releases_only_that_non_claim(self) -> None:
        guarded = guard_evidence(
            _bundle([_record(claim_key="cap:x", status=EvidenceStatus.EXPERIMENTAL)]),
            accepted_gate_topics=("kernel",),
        )
        topics = {item.topic for item in guarded.non_claims}
        self.assertNotIn("kernel", topics)
        self.assertIn("scheduler", topics)
        self.assertEqual(guarded.released_non_claim_topics, ("kernel",))

    def test_unknown_accepted_topic_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown non-claim topics"):
            guard_evidence(
                _bundle([_record(claim_key="cap:x", status=EvidenceStatus.EXPERIMENTAL)]),
                accepted_gate_topics=("not-a-topic",),
            )

    def test_prerequisite_gate_scope_statement_is_emitted(self) -> None:
        guarded = guard_evidence(_bundle([_record(claim_key="cap:x", status=EvidenceStatus.EXPERIMENTAL)]))
        self.assertEqual(
            guarded.prerequisite_gate_scope_statement, PREREQUISITE_GATE_SCOPE_STATEMENT
        )

    def test_gate_record_gets_named_scope_note(self) -> None:
        record = _record(
            claim_key="gate:UOS-DOC-001",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="docs/universeos/gate_registry.md",
            evidence_kind=EvidenceKind.SPECIFICATION,
        )
        claim = guard_evidence(_bundle([record])).claim_for(record.id)
        self.assertIsNotNone(claim.scope_note)
        self.assertIn("UOS-DOC-001", claim.scope_note)


class GuardIntegrationTests(unittest.TestCase):
    def _guard(self) -> GuardedEvidence:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            revision = _revision(clean=False)
            inventory = discover_source_inventory(root, revision)
            bundle = adapt_repository_evidence(root, inventory)
            collected = collect_evidence(revision, inventory, bundle)
            return guard_evidence(collected)

    def test_guarding_the_fixture_bundle_covers_every_record(self) -> None:
        guarded = self._guard()
        self.assertTrue(guarded.claims)
        # Every guarded claim is well-formed and preserves its status contract.
        for claim in guarded.claims:
            self.assertIsInstance(claim, GuardedClaim)
            if claim.tense is ClaimTense.PRESENT:
                self.assertTrue(claim.present_tense_permitted)

    def test_guard_output_is_deterministic(self) -> None:
        first = stable_json_dumps(self._guard())
        second = stable_json_dumps(self._guard())
        self.assertEqual(first, second)

    def test_guard_rejects_non_bundle_input(self) -> None:
        with self.assertRaises(TypeError):
            ClaimGuard().guard(object())  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
