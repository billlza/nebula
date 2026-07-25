"""Task 14.1 repository evidence manifest tests.

These cases exercise :func:`build_repository_manifest`, the read-only Task 14.1
component that freezes a curated repository evidence manifest for the bound
assessment revision. They assert that:

* every Requirement 1.3 source category is represented (or explicitly recorded
  as unavailable / missing),
* the manifest binds to the current revision and records the current-worktree
  vs release/commit origin,
* UniverseOS gate, case, dependency, and non-claim references resolve, and
* collection is read-only and deterministic.

At least one case drives the real ``discover_source_inventory`` and adapter
pipeline against the real repository root for a smoke-level assertion, matching
the design's read-only inventory dry run.
"""

from __future__ import annotations

import hashlib
import os
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from tests.universe_os_gap_analysis.test_adapters import _write_adapter_fixture
from tests.universe_os_gap_analysis.test_inventory import _revision
from tools.universe_os_gap_analysis.inventory import REQUIRED_SOURCE_CATEGORIES
from tools.universe_os_gap_analysis.models import (
    ExecutionState,
    RevisionOrigin,
    SourceCategory,
)
from tools.universe_os_gap_analysis.repository_manifest import (
    MANIFEST_SCHEMA_VERSION,
    RepositoryManifest,
    build_repository_manifest,
)


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _fixed_clock():
    return lambda: datetime(2025, 1, 1, tzinfo=timezone.utc)


def _snapshot_tree(root: Path) -> dict[str, str]:
    """Hash every regular file under ``root`` for read-only verification."""

    snapshot: dict[str, str] = {}
    for current, _dirs, files in os.walk(root):
        for name in files:
            path = Path(current) / name
            relative = path.relative_to(root).as_posix()
            snapshot[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


class RealRepositoryManifestTests(unittest.TestCase):
    """Smoke-level dry run of the manifest against the real repository."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = _real_repo_root()
        cls.manifest = build_repository_manifest(cls.root, clock=_fixed_clock())

    def test_manifest_covers_every_required_category(self) -> None:
        covered = {
            summary.category for summary in self.manifest.categories if summary.available
        }
        self.assertTrue(REQUIRED_SOURCE_CATEGORIES <= covered)
        # Any category not represented must be explicitly recorded as missing.
        self.assertEqual(self.manifest.required_categories_covered, not self.manifest.missing_categories)
        for missing in self.manifest.missing_categories:
            self.assertIsInstance(missing, SourceCategory)
            self.assertNotIn(missing, covered)
        self.assertEqual(self.manifest.schema_version, MANIFEST_SCHEMA_VERSION)

    def test_manifest_binds_to_current_revision_with_worktree_origin(self) -> None:
        origin = self.manifest.revision_origin
        self.assertTrue(origin.commit_id.strip())
        self.assertTrue(origin.branch.strip())
        self.assertTrue(origin.version.strip())
        # A dirty worktree must be attributed to the current worktree; a clean
        # one to the committed revision. Either way the axis is explicit.
        expected = (
            RevisionOrigin.COMMITTED_REVISION
            if origin.worktree_clean
            else RevisionOrigin.CURRENT_WORKTREE
        )
        self.assertIs(origin.inventory_origin, expected)
        self.assertEqual(origin.assessed_at_utc, "2025-01-01T00:00:00+00:00")

    def test_gate_case_dependency_and_non_claim_references_resolve(self) -> None:
        self.assertTrue(self.manifest.gates)
        self.assertTrue(self.manifest.case_ids)
        self.assertTrue(self.manifest.gate_references_resolved)
        self.assertEqual(self.manifest.unresolved_gate_ids, ())
        known_gate_ids = {gate.gate_id for gate in self.manifest.gates}
        known_case_ids = set(self.manifest.case_ids)
        for gate in self.manifest.gates:
            self.assertTrue(gate.dependencies_resolved)
            self.assertTrue(gate.evidence_cases_resolved)
            self.assertTrue(set(gate.dependency_ids) <= known_gate_ids)
            self.assertTrue(set(gate.evidence_case_ids) <= known_case_ids)
            # Every gate preserves an explicit non-claim.
            self.assertTrue(gate.non_claim.strip())
        self.assertTrue(self.manifest.non_claims)

    def test_execution_states_are_read_only_and_disclosed(self) -> None:
        totals = self.manifest.execution_state_totals
        # Inventory adapters never execute commands: nothing is Failed.
        self.assertEqual(totals.get(ExecutionState.FAILED, 0), 0)
        self.assertGreater(totals.get(ExecutionState.VALIDATED, 0), 0)
        self.assertEqual(sum(totals.values()), self.manifest.total_entries)


class ManifestDeterminismAndIsolationTests(unittest.TestCase):
    """The manifest is deterministic and never mutates repository files."""

    def test_manifest_is_deterministic_and_read_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            before = _snapshot_tree(root)

            first = build_repository_manifest(root, revision=_revision(clean=False))
            second = build_repository_manifest(root, revision=_revision(clean=False))
            after = _snapshot_tree(root)

        # Read-only: no file was created, deleted, or modified.
        self.assertEqual(before, after)
        # Deterministic: identical inputs produce identical serialized manifests.
        self.assertEqual(first.to_dict(), second.to_dict())

    def test_fixture_manifest_records_worktree_origin_and_resolves_gates(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            manifest = build_repository_manifest(root, revision=_revision(clean=False))

        self.assertIsInstance(manifest, RepositoryManifest)
        self.assertTrue(manifest.required_categories_covered)
        self.assertEqual(manifest.missing_categories, ())
        self.assertIs(
            manifest.revision_origin.inventory_origin, RevisionOrigin.CURRENT_WORKTREE
        )
        self.assertFalse(manifest.revision_origin.worktree_clean)
        self.assertTrue(manifest.gate_references_resolved)
        self.assertIn("UOS-DOC-001", {gate.gate_id for gate in manifest.gates})
        self.assertIn("TST-001-sample", manifest.case_ids)

    def test_clean_revision_attributes_committed_origin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            manifest = build_repository_manifest(root, revision=_revision(clean=True))

        self.assertTrue(manifest.revision_origin.worktree_clean)
        self.assertIs(
            manifest.revision_origin.inventory_origin,
            RevisionOrigin.COMMITTED_REVISION,
        )
        origin_totals = manifest.origin_totals
        self.assertEqual(
            origin_totals.get(RevisionOrigin.CURRENT_WORKTREE, 0), 0
        )
        self.assertGreater(
            origin_totals.get(RevisionOrigin.COMMITTED_REVISION, 0), 0
        )


if __name__ == "__main__":
    unittest.main()
