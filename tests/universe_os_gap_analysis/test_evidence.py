from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from tests.universe_os_gap_analysis.test_adapters import _write_adapter_fixture
from tests.universe_os_gap_analysis.test_inventory import _revision
from tools.universe_os_gap_analysis import (
    ClaimInput,
    EvidenceBundle,
    EvidenceCollector,
    EvidenceConflictDetector,
    adapt_repository_evidence,
    collect_evidence,
    decide_status,
    detect_evidence_conflicts,
)
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.execution import (
    ExecutionArtifactReference,
    ExecutionEvidence,
    ExecutionOutcome,
    ExecutionRevisionSnapshot,
    ExecutionValidationState,
)
from tools.universe_os_gap_analysis.inventory import discover_source_inventory
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceConflict,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    ExecutionState,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)
from tools.universe_os_gap_analysis.serialization import stable_json_dumps


class StatusDecisionOrderTests(unittest.TestCase):
    def test_pathless_claim_is_unknown_even_when_plan_only(self) -> None:
        self.assertIs(
            decide_status(
                source_path=None,
                plan_only=True,
                negative_claim=False,
                audited_absence=False,
                proposed_status=EvidenceStatus.EXPERIMENTAL,
            ),
            EvidenceStatus.UNKNOWN,
        )

    def test_plan_only_text_is_planned(self) -> None:
        self.assertIs(
            decide_status(
                source_path="ROADMAP.md",
                plan_only=True,
                negative_claim=False,
                audited_absence=False,
                proposed_status=EvidenceStatus.EXPERIMENTAL,
            ),
            EvidenceStatus.PLANNED,
        )

    def test_unsupported_only_from_negative_or_audited_absence(self) -> None:
        for negative, audited in ((True, False), (False, True)):
            with self.subTest(negative=negative, audited=audited):
                self.assertIs(
                    decide_status(
                        source_path="docs/universeos/gate_registry.md",
                        plan_only=False,
                        negative_claim=negative,
                        audited_absence=audited,
                    ),
                    EvidenceStatus.UNSUPPORTED,
                )

    def test_bare_unsupported_proposal_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "Unsupported"):
            decide_status(
                source_path="README.md",
                plan_only=False,
                negative_claim=False,
                audited_absence=False,
                proposed_status=EvidenceStatus.UNSUPPORTED,
            )

    def test_proposed_status_passes_through_and_defaults_to_unknown(self) -> None:
        self.assertIs(
            decide_status(
                source_path="README.md",
                plan_only=False,
                negative_claim=False,
                audited_absence=False,
                proposed_status=EvidenceStatus.EXPERIMENTAL,
            ),
            EvidenceStatus.EXPERIMENTAL,
        )
        self.assertIs(
            decide_status(
                source_path="README.md",
                plan_only=False,
                negative_claim=False,
                audited_absence=False,
            ),
            EvidenceStatus.UNKNOWN,
        )


class ClaimInputTests(unittest.TestCase):
    def test_status_property_uses_decision_order(self) -> None:
        candidate = ClaimInput(
            claim_key="plan:ROADMAP.md",
            claim="Roadmap plan at ROADMAP.md.",
            evidence_kind=EvidenceKind.SPECIFICATION,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path="ROADMAP.md",
            plan_only=True,
        )
        self.assertIs(candidate.status, EvidenceStatus.PLANNED)

    def test_pathless_candidate_status_is_unknown(self) -> None:
        candidate = ClaimInput(
            claim_key="floating",
            claim="A claim without a verifiable path.",
            evidence_kind=EvidenceKind.SOURCE,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path=None,
        )
        self.assertIs(candidate.status, EvidenceStatus.UNKNOWN)


class EvidenceCollectorFixtureTests(unittest.TestCase):
    def _collect(self, clean: bool = False) -> EvidenceBundle:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            revision = _revision(clean=clean)
            inventory = discover_source_inventory(root, revision)
            bundle = adapt_repository_evidence(root, inventory)
            return collect_evidence(revision, inventory, bundle)

    def test_every_record_is_complete_and_singly_classified(self) -> None:
        result = self._collect()
        self.assertTrue(result.records)
        for record in result.records:
            self.assertIsInstance(record, EvidenceRecord)
            self.assertIsInstance(record.status, EvidenceStatus)
            self.assertTrue(str(record.source_path))
            self.assertTrue(record.claim_key)
            self.assertTrue(record.claim)
            self.assertNotIn(record.id, record.related_evidence_ids)

    def test_gate_and_mapping_share_claim_key_and_are_cross_linked(self) -> None:
        result = self._collect()
        group = result.records_for("gate:UOS-DOC-001")
        # Gate definition + source-doc mapping are preserved as distinct sources.
        self.assertEqual(len(group), 2)
        source_paths = {str(record.source_path) for record in group}
        self.assertEqual(
            source_paths,
            {"docs/universeos/gate_registry.md", "docs/universeos/architecture.md"},
        )
        # Each sibling references the other, losslessly preserving the evidence set.
        ids = {record.id for record in group}
        for record in group:
            self.assertEqual(set(record.related_evidence_ids), ids - {record.id})
        gate_record = next(
            r for r in group if str(r.source_path) == "docs/universeos/gate_registry.md"
        )
        self.assertIs(gate_record.status, EvidenceStatus.EXPERIMENTAL)

    def test_gate_non_claim_is_unsupported(self) -> None:
        result = self._collect()
        group = result.records_for("non-claim:gate:UOS-DOC-001")
        self.assertEqual(len(group), 1)
        self.assertIs(group[0].status, EvidenceStatus.UNSUPPORTED)
        self.assertIs(group[0].evidence_kind, EvidenceKind.NON_CLAIM)

    def test_roadmap_and_rfc_are_planned_with_paths(self) -> None:
        result = self._collect()
        planned = [r for r in result.records if r.status is EvidenceStatus.PLANNED]
        planned_paths = {str(r.source_path) for r in planned}
        self.assertIn("ROADMAP.md", planned_paths)
        self.assertIn("rfcs/0001-feature.md", planned_paths)

    def test_example_evidence_is_hosted_adjacency(self) -> None:
        result = self._collect()
        examples = [r for r in result.records if r.evidence_kind is EvidenceKind.EXAMPLE]
        self.assertTrue(examples)
        for record in examples:
            self.assertEqual(record.scope.target_levels, (TargetLevel.T0_HOSTED_ADJACENCY,))

    def test_release_workflow_test_and_artifact_records_are_present(self) -> None:
        result = self._collect()
        kinds = {record.evidence_kind for record in result.records}
        self.assertIn(EvidenceKind.RELEASE, kinds)
        self.assertIn(EvidenceKind.WORKFLOW, kinds)
        self.assertIn(EvidenceKind.TEST_DEFINITION, kinds)
        self.assertIn(EvidenceKind.ARTIFACT, kinds)

    def test_current_worktree_origin_is_preserved(self) -> None:
        result = self._collect(clean=False)
        origins = {record.origin for record in result.records}
        self.assertIn(RevisionOrigin.CURRENT_WORKTREE, origins)
        # No record fabricated a tagged-release origin from worktree evidence.
        self.assertNotIn(RevisionOrigin.TAGGED_RELEASE, origins)

    def test_collection_is_deterministic(self) -> None:
        first = stable_json_dumps(self._collect().records)
        second = stable_json_dumps(self._collect().records)
        self.assertEqual(first, second)


def _execution_evidence(*, with_artifact: bool) -> ExecutionEvidence:
    snapshot = ExecutionRevisionSnapshot(
        commit_id="c" * 40,
        branch="main",
        version="1.0.0",
        repository_root_id="root-abc",
        fingerprint_algorithm="sha256",
        worktree_fingerprint="f" * 64,
        tracked_diff_hash="a" * 64,
        untracked_path_set_hash="b" * 64,
    )
    empty = hashlib.sha256(b"").hexdigest()
    if with_artifact:
        artifact = ExecutionArtifactReference(
            path="executions/execution-evidence-xyz.stdout",
            sha256=empty,
            observed_stream_sha256=empty,
            byte_count=0,
            observed_byte_count=0,
            truncated=False,
        )
        return ExecutionEvidence(
            id="execution-evidence-xyz",
            command_id="TST-280",
            argv_digest=empty,
            redacted_command=("bin",),
            platform=None,
            environment=None,
            outcome=ExecutionOutcome.SUCCEEDED,
            execution_state=ExecutionState.VALIDATED,
            validation_state=ExecutionValidationState.VALIDATED,
            exit_status=0,
            stdout_artifact=artifact,
            stderr_artifact=None,
            before_revision=snapshot,
            after_revision=snapshot,
            detail="ok",
        )
    return ExecutionEvidence(
        id="execution-evidence-disabled",
        command_id="TST-280",
        argv_digest=None,
        redacted_command=(),
        platform=None,
        environment=None,
        outcome=ExecutionOutcome.DISABLED,
        execution_state=ExecutionState.NOT_RUN,
        validation_state=ExecutionValidationState.NOT_RUN,
        exit_status=None,
        stdout_artifact=None,
        stderr_artifact=None,
        before_revision=None,
        after_revision=None,
        detail="local execution is disabled",
    )


class ExecutionEvidenceCollectionTests(unittest.TestCase):
    def test_execution_artifact_becomes_a_complete_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            revision = _revision(clean=False)
            inventory = discover_source_inventory(root, revision)
            bundle = adapt_repository_evidence(root, inventory)
            result = collect_evidence(
                revision, inventory, bundle, (_execution_evidence(with_artifact=True),)
            )
        group = result.records_for("execution:TST-280")
        self.assertEqual(len(group), 1)
        record = group[0]
        self.assertIs(record.evidence_kind, EvidenceKind.TEST_EXECUTION)
        # Execution artifacts always carry an execution-artifact origin, never a
        # fabricated tagged-release or worktree origin.
        self.assertIs(record.origin, RevisionOrigin.EXECUTION_ARTIFACT)
        self.assertIs(record.verification_state, VerificationState.VALIDATED)
        self.assertEqual(
            str(record.source_path), "executions/execution-evidence-xyz.stdout"
        )

    def test_disabled_or_not_run_execution_produces_no_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            revision = _revision(clean=False)
            inventory = discover_source_inventory(root, revision)
            bundle = adapt_repository_evidence(root, inventory)
            without = collect_evidence(revision, inventory, bundle)
            disabled = collect_evidence(
                revision, inventory, bundle, (_execution_evidence(with_artifact=False),)
            )
        # A disabled/not-run command yields no positive artifact, so it is not
        # evidence of any capability and must not appear as a record.
        self.assertEqual(
            {str(r.id) for r in disabled.records},
            {str(r.id) for r in without.records},
        )


class EvidenceDeduplicationTests(unittest.TestCase):
    def _bundle(self, inputs: list[ClaimInput]) -> EvidenceBundle:
        collector = EvidenceCollector()
        revision_ref = collector  # placeholder; use internal normalize via a stub
        return collector._normalize(inputs, __import__(
            "tools.universe_os_gap_analysis.identifiers", fromlist=["reference"]
        ).reference("revision-test"))

    def test_exact_duplicates_collapse_but_distinct_sources_are_linked(self) -> None:
        shared = dict(
            claim_key="gate:UOS-DOC-001",
            claim="Gate UOS-DOC-001.",
            evidence_kind=EvidenceKind.SPECIFICATION,
            origin=RevisionOrigin.CURRENT_WORKTREE,
        )
        duplicate_a = ClaimInput(source_path="docs/universeos/gate_registry.md", **shared)
        duplicate_b = ClaimInput(source_path="docs/universeos/gate_registry.md", **shared)
        distinct = ClaimInput(
            claim_key="gate:UOS-DOC-001",
            claim="Mapping source.",
            evidence_kind=EvidenceKind.SOURCE,
            origin=RevisionOrigin.CURRENT_WORKTREE,
            source_path="docs/universeos/architecture.md",
        )
        result = self._bundle([duplicate_a, duplicate_b, distinct])
        group = result.records_for("gate:UOS-DOC-001")
        # The exact duplicate collapses; the distinct source is preserved.
        self.assertEqual(len(group), 2)
        ids = {record.id for record in group}
        for record in group:
            self.assertEqual(set(record.related_evidence_ids), ids - {record.id})


_REVISION_REF = reference("revision-conflict-test")


def _record(
    *,
    claim_key: str,
    status: EvidenceStatus,
    source_path: str,
    location_value: str | None = None,
) -> EvidenceRecord:
    """Build a minimal, schema-valid EvidenceRecord for conflict tests."""

    anchor = location_value if location_value is not None else f"File:{source_path}"
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, source_path, status.value),
        claim_key=claim_key,
        claim=f"{status.value} claim for {claim_key} at {source_path}.",
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=anchor),
        revision_ref=_REVISION_REF,
        origin=RevisionOrigin.CURRENT_WORKTREE,
        evidence_kind=EvidenceKind.SOURCE,
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


class EvidenceConflictDetectionTests(unittest.TestCase):
    def test_implemented_versus_unsupported_is_a_symmetric_conflict(self) -> None:
        implemented = _record(
            claim_key="cap:backend",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="spec/compiler_pipeline.md",
        )
        unsupported = _record(
            claim_key="cap:backend",
            status=EvidenceStatus.UNSUPPORTED,
            source_path="docs/universeos/kernel_boundary.md",
        )
        conflicts = detect_evidence_conflicts(_bundle([implemented, unsupported]))
        self.assertEqual(len(conflicts), 1)
        conflict = conflicts[0]
        self.assertIsInstance(conflict, EvidenceConflict)
        self.assertEqual(conflict.claim_key, "cap:backend")
        # No inferred winner and confidence forced Low (structural on the model).
        self.assertIsNone(conflict.winner)
        self.assertIs(conflict.confidence, ConfidenceRating.LOW)
        # Every conflicting record id and location is preserved (lossless).
        self.assertEqual(
            set(conflict.evidence_ids),
            {reference(implemented.id), reference(unsupported.id)},
        )
        self.assertEqual(
            set(conflict.incompatible_values),
            {EvidenceStatus.EXPERIMENTAL.value, EvidenceStatus.UNSUPPORTED.value},
        )
        self.assertEqual(len(conflict.locations), 2)
        # A present-tense implementation claim is involved, so the conflict blocks.
        self.assertTrue(conflict.blocking)

    def test_planned_versus_unsupported_conflict_is_not_blocking(self) -> None:
        planned = _record(
            claim_key="cap:userspace",
            status=EvidenceStatus.PLANNED,
            source_path="ROADMAP.md",
        )
        unsupported = _record(
            claim_key="cap:userspace",
            status=EvidenceStatus.UNSUPPORTED,
            source_path="docs/universeos/kernel_boundary.md",
        )
        conflicts = detect_evidence_conflicts(_bundle([planned, unsupported]))
        self.assertEqual(len(conflicts), 1)
        # No implemented/current claim participates, so it is not blocking.
        self.assertFalse(conflicts[0].blocking)

    def test_same_polarity_tiers_do_not_conflict(self) -> None:
        repo_preview = _record(
            claim_key="package:widgets",
            status=EvidenceStatus.REPO_PREVIEW,
            source_path="official/widgets/nebula.toml",
        )
        installed_preview = _record(
            claim_key="package:widgets",
            status=EvidenceStatus.INSTALLED_PREVIEW,
            source_path="docs/support_matrix.md",
        )
        conflicts = detect_evidence_conflicts(_bundle([repo_preview, installed_preview]))
        # Distinct implemented tiers are scope-differentiated, not incompatible.
        self.assertEqual(conflicts, ())

    def test_unknown_is_neutral_and_never_conflicts(self) -> None:
        implemented = _record(
            claim_key="gate:UOS-DOC-001",
            status=EvidenceStatus.EXPERIMENTAL,
            source_path="docs/universeos/gate_registry.md",
        )
        unknown = _record(
            claim_key="gate:UOS-DOC-001",
            status=EvidenceStatus.UNKNOWN,
            source_path="docs/universeos/architecture.md",
        )
        conflicts = detect_evidence_conflicts(_bundle([implemented, unknown]))
        self.assertEqual(conflicts, ())

    def test_single_assertive_record_is_not_a_conflict(self) -> None:
        implemented = _record(
            claim_key="cap:solo",
            status=EvidenceStatus.COMPILER_TOOLING_GA,
            source_path="README.md",
        )
        conflicts = detect_evidence_conflicts(_bundle([implemented]))
        self.assertEqual(conflicts, ())

    def test_three_way_disagreement_is_lossless(self) -> None:
        records = [
            _record(
                claim_key="cap:triple",
                status=EvidenceStatus.EXPERIMENTAL,
                source_path="spec/abi_layout.md",
            ),
            _record(
                claim_key="cap:triple",
                status=EvidenceStatus.PLANNED,
                source_path="ROADMAP.md",
            ),
            _record(
                claim_key="cap:triple",
                status=EvidenceStatus.UNSUPPORTED,
                source_path="docs/universeos/kernel_boundary.md",
            ),
        ]
        conflicts = detect_evidence_conflicts(_bundle(records))
        self.assertEqual(len(conflicts), 1)
        conflict = conflicts[0]
        self.assertEqual(
            set(conflict.evidence_ids), {reference(r.id) for r in records}
        )
        self.assertEqual(len(conflict.locations), 3)

    def test_detection_is_order_independent_across_permutations(self) -> None:
        import itertools

        records = [
            _record(
                claim_key="cap:perm",
                status=EvidenceStatus.EXPERIMENTAL,
                source_path="spec/compiler_pipeline.md",
            ),
            _record(
                claim_key="cap:perm",
                status=EvidenceStatus.UNSUPPORTED,
                source_path="docs/universeos/kernel_boundary.md",
            ),
            _record(
                claim_key="other:cap",
                status=EvidenceStatus.PLANNED,
                source_path="ROADMAP.md",
            ),
            _record(
                claim_key="other:cap",
                status=EvidenceStatus.BACKEND_SDK_GA,
                source_path="RELEASE_NOTES_v1.0.0.md",
            ),
        ]
        baseline = detect_evidence_conflicts(_bundle(records))
        self.assertEqual(len(baseline), 2)
        baseline_repr = stable_json_dumps(baseline)
        for permutation in itertools.permutations(records):
            result = detect_evidence_conflicts(_bundle(list(permutation)))
            self.assertEqual(stable_json_dumps(result), baseline_repr)

    def test_detector_rejects_non_bundle_input(self) -> None:
        with self.assertRaises(TypeError):
            EvidenceConflictDetector().detect(object())  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
