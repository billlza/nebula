"""Task 3.4 inventory + adapter integration tests.

These cases exercise the Task 3.1/3.2 inventory and adapter components together,
against both a curated fixture and a read-only dry run of the real repository.
They cover the cross-reference integrity, unexecuted disclosure, and structural
fail-closed behaviour that the per-component fixture tests in ``test_inventory``
and ``test_adapters`` do not assert end to end. Duplicated coverage is avoided by
reusing the shared fixture helpers from those modules.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.universe_os_gap_analysis.test_adapters import _revision, _write_adapter_fixture
from tools.universe_os_gap_analysis import adapt_repository_evidence
from tools.universe_os_gap_analysis.inventory import (
    REQUIRED_SOURCE_CATEGORIES,
    InventoryError,
    discover_source_inventory,
)
from tools.universe_os_gap_analysis.models import ExecutionState, SourceCategory


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


class InventoryAdapterCrossReferenceTests(unittest.TestCase):
    """Gate, case, mapping, and inventory references must resolve end to end."""

    def test_real_repository_gate_case_and_mapping_references_resolve(self) -> None:
        root = _real_repo_root()
        inventory = discover_source_inventory(root, _revision(clean=False))
        bundle = adapt_repository_evidence(root, inventory)

        inventory_paths = {str(entry.path) for entry in inventory}
        inventory_case_anchors = {
            anchor.split("CaseId:", 1)[1]
            for entry in inventory
            for anchor in entry.stable_anchors
            if anchor.startswith("CaseId:")
        }
        case_ids = {case.case_id for case in bundle.cases}
        gate_ids = {gate.gate_id for gate in bundle.gates}

        # Every discovered adapter case is anchored in the Task 3.1 inventory.
        self.assertTrue(case_ids)
        self.assertTrue(case_ids <= inventory_case_anchors)

        # Every gate evidence reference resolves to a discovered case, and every
        # non-planned gate actually carries at least one evidence case.
        referenced_cases: set[str] = set()
        for gate in bundle.gates:
            referenced_cases.update(gate.evidence_case_ids)
            if gate.status != "planned":
                self.assertTrue(
                    gate.evidence_case_ids,
                    f"non-planned gate {gate.gate_id} lacks evidence cases",
                )
        self.assertTrue(referenced_cases)
        self.assertTrue(referenced_cases <= case_ids)

        # Source-document mappings resolve to both inventoried paths and known gates.
        self.assertTrue(bundle.source_mappings)
        for mapping in bundle.source_mappings:
            self.assertIn(mapping.path, inventory_paths)
            self.assertTrue(set(mapping.gate_ids) <= gate_ids)

    def test_real_repository_dry_run_covers_required_categories_for_adapter(self) -> None:
        root = _real_repo_root()
        inventory = discover_source_inventory(root, _revision(clean=False))
        # The adapter only succeeds when the inventory that feeds it already spans
        # every Requirement 1.3 category, tying the two components together.
        inspected = {entry.category for entry in inventory if entry.inspected}
        self.assertTrue(REQUIRED_SOURCE_CATEGORIES <= inspected)
        bundle = adapt_repository_evidence(root, inventory)
        self.assertTrue(bundle.gates)
        self.assertTrue(bundle.cases)
        self.assertTrue(bundle.workflow_jobs)


class UnexecutedDisclosureIntegrationTests(unittest.TestCase):
    """Definitions and history are disclosed as unexecuted across the whole bundle."""

    def test_real_repository_bundle_discloses_every_record_as_unexecuted(self) -> None:
        root = _real_repo_root()
        inventory = discover_source_inventory(root, _revision(clean=False))
        bundle = adapt_repository_evidence(root, inventory)

        definition_records = (
            *bundle.gates,
            *bundle.cases,
            *bundle.workflow_jobs,
            *bundle.release_metadata,
        )
        self.assertTrue(definition_records)
        for record in definition_records:
            self.assertIs(record.execution_state, ExecutionState.NOT_RUN)
            self.assertTrue((record.execution_detail or "").strip())

        # Parsed artifact metadata is never presented as a current execution result:
        # it is either hash-bound-but-unrun or an unavailable remote asset.
        artifact_states = {record.execution_state for record in bundle.artifact_metadata}
        self.assertTrue(artifact_states)
        self.assertTrue(
            artifact_states <= {ExecutionState.NOT_RUN, ExecutionState.UNAVAILABLE}
        )
        self.assertIn(ExecutionState.UNAVAILABLE, artifact_states)
        for record in bundle.artifact_metadata:
            self.assertTrue((record.execution_detail or "").strip())


class AdapterMissingSourceFamilyTests(unittest.TestCase):
    """Absent required source families fail closed before any evidence is produced."""

    def _adapt_without(self, predicate) -> str:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            filtered = tuple(entry for entry in inventory if not predicate(entry))
            with self.assertRaises(InventoryError) as captured:
                adapt_repository_evidence(root, filtered)
            return captured.exception.code

    def test_absent_gate_registry_fails_closed(self) -> None:
        self.assertEqual(
            self._adapt_without(lambda entry: entry.category is SourceCategory.GATE_REGISTRY),
            "INV-GATE-MISSING",
        )

    def test_absent_case_manifests_fail_closed(self) -> None:
        self.assertEqual(
            self._adapt_without(lambda entry: entry.category is SourceCategory.TEST),
            "INV-CASE-MISSING",
        )

    def test_absent_workflows_fail_closed(self) -> None:
        self.assertEqual(
            self._adapt_without(
                lambda entry: entry.category
                in {SourceCategory.CI_WORKFLOW, SourceCategory.RELEASE_WORKFLOW}
            ),
            "INV-WORKFLOW-MISSING",
        )


if __name__ == "__main__":
    unittest.main()
