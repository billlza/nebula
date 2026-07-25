"""Task 3.4 inventory + adapter integration tests.

These integration cases wire the Task 3.1 source inventory (``inventory.py``) and
the Task 3.2 gate/case/workflow/release/artifact adapter (``adapters.py``)
together and exercise them against **both** a curated fixture repository and a
read-only dry run of this repository. They validate the Requirement 1.3 mandatory
source categories, the minimal stable anchors (Requirements 14.4-14.6), the
gate/case cross-reference integrity that never impersonates execution
(Requirement 7.7), the not-run/unavailable disclosure surface, and the ``INV-*``
fail-closed behaviour that triggers when a mandatory category is absent.

To avoid duplicating the per-component fixtures in ``test_inventory`` and
``test_adapters`` (or the dry-run cross-reference / missing-family checks already
in ``test_integration``), this module reuses the shared fixture helpers and
focuses on the combined inventory -> adapter pipeline invariants: every
Requirement 1.3 category present through a single fixture *and* the real
repository, the smallest-stable-anchor guarantee for every inspected source, and
the inventory-level mandatory-category fail-closed path that stops the adapter
from ever running.

_Requirements: 1.3, 7.7, 14.4, 14.5, 14.6_
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.universe_os_gap_analysis.test_adapters import (
    _registry,
    _write_adapter_fixture,
)
from tests.universe_os_gap_analysis.test_inventory import _revision, _write_fixture
from tools.universe_os_gap_analysis import adapt_repository_evidence
from tools.universe_os_gap_analysis.inventory import (
    REQUIRED_SOURCE_CATEGORIES,
    InventoryError,
    discover_source_inventory,
)
from tools.universe_os_gap_analysis.models import (
    ExecutionState,
    RevisionOrigin,
    SourceCategory,
)

# The smallest stable anchor kinds described by Requirement 14.5: heading,
# symbol, case ID, manifest key, and workflow job. Every inspected source must
# carry at least the repository-relative ``File:`` anchor (Requirement 14.4), and
# these finer anchors must appear where the corresponding evidence exists.
_MINIMAL_ANCHOR_KINDS = (
    "Heading:",
    "Symbol:",
    "CaseId:",
    "ManifestKey:",
    "WorkflowJob:",
)

# A mandatory category whose absence at the inventory stage must fail closed
# before the adapter is ever consulted (Requirement 1.3 / INV-* handling). Each
# entry maps a category to a fixture file whose omission removes that category.
_MANDATORY_CATEGORY_FIXTURES = {
    SourceCategory.ROADMAP: "ROADMAP.md",
    SourceCategory.CHANGELOG: "CHANGELOG.md",
    SourceCategory.RELEASE_NOTES: "RELEASE_NOTES_v1.2.3.md",
    SourceCategory.SPECIFICATION: "spec/language.md",
    SourceCategory.RFC: "rfcs/0001-feature.md",
}


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _anchor_kinds(anchors: tuple[str, ...]) -> set[str]:
    kinds: set[str] = set()
    for anchor in anchors:
        prefix, _, _ = anchor.partition(":")
        if prefix:
            kinds.add(prefix + ":")
    return kinds


class FixtureCategoryCoverageTests(unittest.TestCase):
    """A single fixture spans every Requirement 1.3 category and feeds the adapter."""

    def test_fixture_inventory_then_adapter_span_every_required_category(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            inspected = {entry.category for entry in inventory if entry.inspected}
            # Requirement 1.3: every mandatory source category is discovered.
            self.assertTrue(REQUIRED_SOURCE_CATEGORIES <= inspected)
            # The adapter only succeeds because the inventory already spans the
            # gate registry, cases, and workflows, tying the two components.
            bundle = adapt_repository_evidence(root, inventory)

        self.assertTrue(bundle.gates)
        self.assertTrue(bundle.cases)
        self.assertTrue(bundle.workflow_jobs)
        # Dirty fixture worktree evidence is labelled as current-worktree origin,
        # never a tagged release.
        self.assertTrue(
            all(entry.revision_origin is RevisionOrigin.CURRENT_WORKTREE for entry in inventory)
        )


class MinimalStableAnchorTests(unittest.TestCase):
    """Every inspected source cites a repo-relative path and a minimal stable anchor."""

    def test_fixture_sources_carry_repo_relative_path_and_minimal_anchors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))

        by_path = {str(entry.path): entry for entry in inventory}
        # Requirement 14.4: every inspected source has a repository-relative File anchor.
        for entry in inventory:
            if not entry.inspected:
                continue
            self.assertTrue(entry.stable_anchors, f"{entry.path} has no stable anchors")
            self.assertIn(f"File:{entry.path}", entry.stable_anchors)

        # Requirement 14.5: the smallest stable anchor kind is emitted per source
        # (heading, symbol, manifest key, case ID, workflow job).
        self.assertIn("Heading:Current Boundary", by_path["README.md"].stable_anchors)
        self.assertIn("Symbol:Compiler", by_path["frontend/compiler.cpp"].stable_anchors)
        self.assertIn(
            "ManifestKey:package.name",
            by_path["official/example/nebula.toml"].stable_anchors,
        )
        self.assertIn(
            "CaseId:TST-001-sample",
            by_path["tests/cases/test/TST-001-sample/case.toml"].stable_anchors,
        )
        self.assertIn(
            "WorkflowJob:build",
            by_path[".github/workflows/ci.yml"].stable_anchors,
        )

    def test_real_repository_dry_run_emits_every_minimal_anchor_kind(self) -> None:
        root = _real_repo_root()
        inventory = discover_source_inventory(root, _revision(clean=False))

        observed_kinds: set[str] = set()
        for entry in inventory:
            if not entry.inspected:
                continue
            # Requirement 14.4: repository-relative anchor is always present.
            self.assertIn(f"File:{entry.path}", entry.stable_anchors)
            observed_kinds |= _anchor_kinds(entry.stable_anchors)

        # Requirement 14.5: the real repository exercises every minimal anchor kind.
        for kind in _MINIMAL_ANCHOR_KINDS:
            self.assertIn(kind, observed_kinds, f"no {kind} anchor found in dry run")


class GateCaseCrossReferenceFixtureTests(unittest.TestCase):
    """Gate/case cross-references resolve without impersonating execution (Req 7.7)."""

    def test_fixture_gate_evidence_and_mappings_resolve_and_stay_unexecuted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            bundle = adapt_repository_evidence(root, inventory)

        inventory_paths = {str(entry.path) for entry in inventory}
        case_ids = {case.case_id for case in bundle.cases}
        gate_ids = {gate.gate_id for gate in bundle.gates}

        # Every non-planned gate carries evidence cases that resolve to a parsed
        # case manifest, and cross-references never assert current execution.
        referenced_cases: set[str] = set()
        for gate in bundle.gates:
            if gate.status != "planned":
                self.assertTrue(
                    gate.evidence_case_ids,
                    f"non-planned gate {gate.gate_id} lacks evidence cases",
                )
            referenced_cases.update(gate.evidence_case_ids)
            self.assertIs(gate.execution_state, ExecutionState.NOT_RUN)
        self.assertTrue(referenced_cases)
        self.assertTrue(referenced_cases <= case_ids)

        # Source-document mappings resolve to both inventoried paths and gates.
        self.assertTrue(bundle.source_mappings)
        for mapping in bundle.source_mappings:
            self.assertIn(mapping.path, inventory_paths)
            self.assertTrue(set(mapping.gate_ids) <= gate_ids)

        # Cases are parsed definitions, never presented as executed.
        self.assertTrue(all(case.execution_state is ExecutionState.NOT_RUN for case in bundle.cases))

    def test_fixture_gate_referencing_unknown_case_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            # Point the gate at an evidence case ID that no case manifest defines.
            registry = root / "docs/universeos/gate_registry.md"
            registry.write_text(_registry(evidence="TST-777-absent"), encoding="utf-8")
            inventory = discover_source_inventory(root, _revision(clean=False))
            with self.assertRaises(InventoryError) as captured:
                adapt_repository_evidence(root, inventory)

        self.assertEqual(captured.exception.code, "INV-GATE-EVIDENCE")


class NotRunDisclosureFixtureTests(unittest.TestCase):
    """Fixture definitions and remote assets disclose not-run / unavailable state."""

    def test_fixture_bundle_discloses_definitions_and_unavailable_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            bundle = adapt_repository_evidence(root, inventory)

        definition_records = (
            *bundle.gates,
            *bundle.cases,
            *bundle.workflow_jobs,
            *bundle.release_metadata,
        )
        self.assertTrue(definition_records)
        # Requirement 14.6: every parsed definition is disclosed as unexecuted with a reason.
        for record in definition_records:
            self.assertIs(record.execution_state, ExecutionState.NOT_RUN)
            self.assertTrue((record.execution_detail or "").strip())

        # The workflow declares an upload-artifact whose payload is unavailable at
        # analysis time; it must be disclosed as Unavailable, not a current result.
        artifact_states = {record.execution_state for record in bundle.artifact_metadata}
        self.assertTrue(artifact_states)
        self.assertTrue(
            artifact_states <= {ExecutionState.NOT_RUN, ExecutionState.UNAVAILABLE}
        )
        self.assertIn(ExecutionState.UNAVAILABLE, artifact_states)
        self.assertTrue(
            any(
                record.execution_state is ExecutionState.UNAVAILABLE
                and record.identity == "build:fixture-reports"
                for record in bundle.artifact_metadata
            )
        )
        for record in bundle.artifact_metadata:
            self.assertTrue((record.execution_detail or "").strip())

    def test_real_repository_release_and_gate_records_are_never_current_execution(self) -> None:
        root = _real_repo_root()
        inventory = discover_source_inventory(root, _revision(clean=False))
        bundle = adapt_repository_evidence(root, inventory)

        self.assertTrue(bundle.release_metadata)
        for record in bundle.release_metadata:
            self.assertIs(record.execution_state, ExecutionState.NOT_RUN)
            self.assertTrue((record.execution_detail or "").strip())
        self.assertTrue(
            all(gate.execution_state is ExecutionState.NOT_RUN for gate in bundle.gates)
        )


class MandatoryCategoryFailClosedTests(unittest.TestCase):
    """An absent mandatory category fails closed before the adapter can run."""

    def test_each_absent_mandatory_category_raises_structured_inv_error(self) -> None:
        for category, fixture_file in _MANDATORY_CATEGORY_FIXTURES.items():
            with self.subTest(category=category), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                _write_fixture(root, omit=fixture_file)
                with self.assertRaises(InventoryError) as captured:
                    discover_source_inventory(root, _revision(clean=False))

                self.assertEqual(captured.exception.code, "INV-REQUIRED-CATEGORY-MISSING")
                self.assertIn(category, captured.exception.missing_categories)
                # The structured error names the missing category so the failure
                # can be traced back to Requirement 1.3.
                self.assertIn(category.value, captured.exception.to_dict()["missingCategories"])

    def test_missing_category_stops_pipeline_before_adapter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            # Remove a mandatory category after building an otherwise-complete
            # adapter fixture, proving the inventory gate stops the adapter.
            (root / "CHANGELOG.md").unlink()
            with self.assertRaises(InventoryError) as captured:
                # The adapter is never reached: discovery fails closed first.
                adapt_repository_evidence(
                    root, discover_source_inventory(root, _revision(clean=False))
                )

        self.assertEqual(captured.exception.code, "INV-REQUIRED-CATEGORY-MISSING")
        self.assertIn(SourceCategory.CHANGELOG, captured.exception.missing_categories)


if __name__ == "__main__":
    unittest.main()
