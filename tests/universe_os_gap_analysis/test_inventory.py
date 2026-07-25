from __future__ import annotations

import hashlib
import os
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from tools.universe_os_gap_analysis.inventory import (
    REQUIRED_SOURCE_CATEGORIES,
    InventoryError,
    classify_source_path,
    discover_source_inventory,
)
from tools.universe_os_gap_analysis.models import (
    AssessmentRevision,
    ExecutionState,
    RevisionOrigin,
    SourceCategory,
)


_FIXTURE_FILES = {
    "README.md": "# Fixture Repository\n\n## Current Boundary\n",
    "ROADMAP.md": "# Roadmap\n",
    "CHANGELOG.md": "# Changelog\n",
    "RELEASE_NOTES_v1.2.3.md": "# Release 1.2.3\n",
    "spec/language.md": "# Language\n",
    "rfcs/0001-feature.md": "# RFC 0001\n",
    "tests/cases/test/TST-001-sample/case.toml": (
        'id = "TST-001-sample"\nsuite = "test"\n\n[[steps]]\nkind = "check"\n'
    ),
    "CMakeLists.txt": "cmake_minimum_required(VERSION 3.20)\nadd_executable(nebula main.cpp)\n",
    ".github/workflows/ci.yml": "name: ci\njobs:\n    build:\n        runs-on: ubuntu-latest\n",
    ".github/workflows/release.yml": "name: release\njobs:\n  publish:\n    runs-on: ubuntu-latest\n",
    "runtime/runtime.hpp": "struct RuntimeState {};\n",
    "std/io.nb": "fn print_line() -> Void {}\n",
    "official/example/nebula.toml": (
        'schema_version = 1\n[package]\nname = "example"\nentry = "src/lib.nb"\n'
    ),
    "official/example/src/lib.nb": "fn package_value() -> Int { return 1 }\n",
    "examples/demo/main.nb": "fn main() -> Void {}\n",
    "docs/universeos/architecture.md": "# UniverseOS Architecture\n",
    "docs/universeos/gate_registry.md": "# Gate Registry\n",
    "frontend/compiler.cpp": "class Compiler {};\nint compile_module() { return 0; }\n",
    "artifacts/release.json": '{"kind":"release","schema_version":1,"subject":[]}\n',
    "artifacts/compiler.out.nebmeta": (
        "version=3\nartifact_kind=executable\nmode=debug\nprofile=fast\n"
    ),
}


def _revision(*, clean: bool) -> AssessmentRevision:
    return AssessmentRevision(
        schema_version="1.0",
        commit_id="0123456789abcdef",
        branch="fixture",
        version="1.2.3",
        describe="fixture",
        tags=(),
        worktree_clean=clean,
        assessed_at_utc=datetime(2025, 1, 1, tzinfo=timezone.utc),
        fingerprint_algorithm="fixture",
        worktree_fingerprint="fixture-worktree",
        tracked_diff_hash="fixture-diff",
        untracked_path_set_hash="fixture-untracked",
        excluded_paths=(),
        repository_root_id="repository-fixture",
    )


def _write_fixture(root: Path, *, omit: str | None = None) -> None:
    (root / "VERSION").write_text("1.2.3\n", encoding="utf-8")
    for relative, content in _FIXTURE_FILES.items():
        if relative == omit:
            continue
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


class SourceClassificationTests(unittest.TestCase):
    def test_required_path_classes_are_exclusive_and_explicit(self) -> None:
        examples = {
            "frontend/compiler.cpp": SourceCategory.SOURCE_CODE,
            "README.md": SourceCategory.README,
            "ROADMAP.md": SourceCategory.ROADMAP,
            "CHANGELOG.md": SourceCategory.CHANGELOG,
            "RELEASE_NOTES_v1.0.0.md": SourceCategory.RELEASE_NOTES,
            "spec/language.md": SourceCategory.SPECIFICATION,
            "rfcs/0001-feature.md": SourceCategory.RFC,
            "tests/cases/test/TST-001-sample/case.toml": SourceCategory.TEST,
            "CMakeLists.txt": SourceCategory.BUILD_CONFIGURATION,
            ".github/workflows/contract-tests.yml": SourceCategory.CI_WORKFLOW,
            ".github/workflows/release.yml": SourceCategory.RELEASE_WORKFLOW,
            "runtime/runtime.hpp": SourceCategory.RUNTIME,
            "std/io.nb": SourceCategory.STANDARD_LIBRARY,
            "official/pkg/nebula.toml": SourceCategory.OFFICIAL_PACKAGE,
            "examples/demo/main.nb": SourceCategory.EXAMPLE,
            "docs/universeos/architecture.md": SourceCategory.UNIVERSE_OS_DOCUMENT,
            "docs/universeos/gate_registry.md": SourceCategory.GATE_REGISTRY,
            "artifacts/build.json": SourceCategory.ARTIFACT,
        }
        for path, expected in examples.items():
            with self.subTest(path=path):
                self.assertIs(classify_source_path(path), expected)


class InventoryFixtureTests(unittest.TestCase):
    def test_fixture_discovers_all_required_categories_and_stable_anchor_types(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root)
            entries = discover_source_inventory(root, _revision(clean=False))

        self.assertTrue(REQUIRED_SOURCE_CATEGORIES <= {item.category for item in entries})
        self.assertTrue(all(item.inspected for item in entries))
        self.assertTrue(all(item.execution_state is ExecutionState.VALIDATED for item in entries))
        self.assertTrue(all(item.revision_origin is RevisionOrigin.CURRENT_WORKTREE for item in entries))
        by_path = {str(item.path): item for item in entries}
        self.assertIn("Heading:Current Boundary", by_path["README.md"].stable_anchors)
        self.assertIn("Symbol:Compiler", by_path["frontend/compiler.cpp"].stable_anchors)
        self.assertIn("Symbol:compile_module", by_path["frontend/compiler.cpp"].stable_anchors)
        self.assertIn(
            "ManifestKey:package.name",
            by_path["official/example/nebula.toml"].stable_anchors,
        )
        self.assertIn(
            "CaseId:TST-001-sample",
            by_path["tests/cases/test/TST-001-sample/case.toml"].stable_anchors,
        )
        self.assertIn("WorkflowJob:build", by_path[".github/workflows/ci.yml"].stable_anchors)
        self.assertIn(
            "ManifestKey:jobs.build.runs-on",
            by_path[".github/workflows/ci.yml"].stable_anchors,
        )
        self.assertIn(
            "ArtifactMetadata:kind=release",
            by_path["artifacts/release.json"].stable_anchors,
        )
        self.assertIn(
            "ArtifactMetadata:artifact_kind=executable",
            by_path["artifacts/compiler.out.nebmeta"].stable_anchors,
        )
        self.assertEqual(
            by_path["README.md"].content_hash,
            hashlib.sha256(_FIXTURE_FILES["README.md"].encode()).hexdigest(),
        )

    def test_anchor_collection_is_lossless_for_large_anchor_sets(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root)
            headings = "\n".join(f"## Section {index:03d}" for index in range(300))
            (root / "README.md").write_text(f"# Fixture\n{headings}\n", encoding="utf-8")
            entries = discover_source_inventory(root, _revision(clean=False))

        readme = next(item for item in entries if str(item.path) == "README.md")
        self.assertIn("Heading:Section 299", readme.stable_anchors)
        self.assertGreaterEqual(len(readme.stable_anchors), 301)

    def test_invalid_native_artifact_metadata_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root)
            (root / "artifacts" / "compiler.out.nebmeta").write_text(
                "version=3\nversion=4\n", encoding="utf-8"
            )
            with self.assertRaises(InventoryError) as captured:
                discover_source_inventory(root, _revision(clean=False))

        self.assertEqual(captured.exception.code, "INV-ARTIFACT-METADATA")
        self.assertEqual(captured.exception.path, "artifacts/compiler.out.nebmeta")

    def test_classified_special_file_fails_closed(self) -> None:
        if not hasattr(os, "mkfifo"):
            self.skipTest("FIFO creation is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root, omit="ROADMAP.md")
            try:
                os.mkfifo(root / "ROADMAP.md")
            except OSError as error:
                self.skipTest(f"FIFO creation unavailable: {error}")
            with self.assertRaises(InventoryError) as captured:
                discover_source_inventory(root, _revision(clean=False))

        self.assertEqual(captured.exception.code, "INV-PATH-UNSUPPORTED")
        self.assertEqual(captured.exception.path, "ROADMAP.md")

    def test_clean_fixture_uses_committed_revision_origin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root)
            entries = discover_source_inventory(root, _revision(clean=True))
        self.assertTrue(
            all(item.revision_origin is RevisionOrigin.COMMITTED_REVISION for item in entries)
        )

    def test_missing_required_category_fails_closed_with_structured_inv_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root, omit="ROADMAP.md")
            with self.assertRaises(InventoryError) as captured:
                discover_source_inventory(root, _revision(clean=False))
        self.assertEqual(captured.exception.code, "INV-REQUIRED-CATEGORY-MISSING")
        self.assertEqual(captured.exception.missing_categories, (SourceCategory.ROADMAP,))
        self.assertEqual(
            captured.exception.to_dict()["missingCategories"], ["Roadmap"]
        )

    def test_symlink_is_hashed_but_not_followed_and_directory_escape_is_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            outside_root = Path(outside)
            _write_fixture(root)
            secret = outside_root / "secret.nb"
            secret.write_text("fn outside_secret() -> Void {}\n", encoding="utf-8")
            link = root / "examples" / "outside-link"
            try:
                link.symlink_to(outside_root, target_is_directory=True)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"symbolic links unavailable: {error}")
            entries = discover_source_inventory(root, _revision(clean=False))

        by_path = {str(item.path): item for item in entries}
        linked = by_path["examples/outside-link"]
        self.assertFalse(linked.inspected)
        self.assertIs(linked.execution_state, ExecutionState.NOT_RUN)
        self.assertIn("without following", linked.execution_detail or "")
        self.assertEqual(
            linked.content_hash, hashlib.sha256(str(outside_root).encode()).hexdigest()
        )
        self.assertFalse(any("secret.nb" in str(item.path) for item in entries))
        self.assertFalse(
            any("outside_secret" in anchor for item in entries for anchor in item.stable_anchors)
        )

    def test_inventory_ids_and_order_are_repeatable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_fixture(root)
            first = discover_source_inventory(root, _revision(clean=False))
            second = discover_source_inventory(root, _revision(clean=False))
        self.assertEqual(first, second)
        self.assertEqual(
            list(first),
            sorted(first, key=lambda item: (item.category.value, str(item.path))),
        )


class InventoryRepositoryDryRunTests(unittest.TestCase):
    def test_real_repository_dry_run_covers_every_required_category_without_commands(self) -> None:
        root = Path(__file__).resolve().parents[2]
        entries = discover_source_inventory(root, _revision(clean=False))
        categories = {item.category for item in entries if item.inspected}
        self.assertTrue(REQUIRED_SOURCE_CATEGORIES <= categories)

        release_notes = {
            str(item.path)
            for item in entries
            if item.category is SourceCategory.RELEASE_NOTES
        }
        expected_release_notes = {
            path.relative_to(root).as_posix()
            for path in root.glob("RELEASE_NOTES*.md")
        }
        self.assertEqual(release_notes, expected_release_notes)
        self.assertTrue(
            any(
                item.path == "docs/universeos/gate_registry.md"
                and "Heading:UniverseOS Gate Registry" in item.stable_anchors
                for item in entries
            )
        )
        self.assertTrue(
            any(
                item.path == ".github/workflows/release.yml"
                and "WorkflowJob:build-assets" in item.stable_anchors
                for item in entries
            )
        )
        self.assertTrue(
            any(
                item.path.endswith("TST-329-universeos-gate-registry-docs-contract/case.toml")
                and "CaseId:TST-329-universeos-gate-registry-docs-contract"
                in item.stable_anchors
                for item in entries
            )
        )
        self.assertFalse(any(str(item.path).startswith("build-") for item in entries))
        self.assertTrue(
            all(item.revision_origin is RevisionOrigin.CURRENT_WORKTREE for item in entries)
        )


if __name__ == "__main__":
    unittest.main()
