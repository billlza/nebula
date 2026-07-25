from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tests.universe_os_gap_analysis.test_inventory import _revision, _write_fixture
from tools.universe_os_gap_analysis import adapt_repository_evidence
from tools.universe_os_gap_analysis.inventory import InventoryError, discover_source_inventory
from tools.universe_os_gap_analysis.models import ExecutionState, SourceCategory


def _registry(*, evidence: str = "TST-001-sample", dependency: str | None = None) -> str:
    depends_on = [] if dependency is None else [dependency]
    document = {
        "gate_registry_version": 2,
        "gate_naming": {"UOS-DOC": "documentation gates"},
        "source_doc_mapping": [{
            "path": "docs/universeos/architecture.md",
            "maps_to": ["UOS-DOC-001"],
            "relationship": "Defines the fixture documentation boundary.",
        }],
        "gates": [{
            "id": "UOS-DOC-001",
            "title": "Fixture documentation gate",
            "status": "experimental",
            "owner_area": "docs/spec",
            "depends_on": depends_on,
            "required_evidence": ["A versioned documentation contract."],
            "evidence_cases": [evidence],
            "non_claim": "This definition does not prove current runtime execution.",
        }],
    }
    return "# Gate Registry\n\n```json\n" + json.dumps(document, indent=2) + "\n```\n"


def _write_adapter_fixture(root: Path) -> None:
    _write_fixture(root)
    (root / "docs/universeos/gate_registry.md").write_text(_registry(), encoding="utf-8")
    (root / ".github/workflows/ci.yml").write_text(
        "name: ci\njobs:\n  build:\n    runs-on: ubuntu-latest\n    steps:\n"
        "      - uses: actions/upload-artifact@v4\n        with:\n"
        "          name: fixture-reports\n          path: reports/*.json\n",
        encoding="utf-8",
    )
    (root / "artifacts/release-manifest.json").write_text(
        json.dumps({
            "version": "1.2.3",
            "repository": "example/fixture",
            "artifacts": [{"name": "nebula-v1.2.3-linux.tar.gz"}],
            "backend_sdks": [],
            "sboms": [{
                "name": "nebula-v1.2.3-linux.spdx.json",
                "subject": "nebula-v1.2.3-linux.tar.gz",
            }],
            "attestations": [],
            "metadata": {
                "checksums": {"name": "SHA256SUMS.txt"},
                "manifest": {"name": "release-manifest.json"},
            },
        }),
        encoding="utf-8",
    )


class AdapterFixtureTests(unittest.TestCase):
    def test_definitions_history_and_unavailable_assets_are_not_current_execution(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            bundle = adapt_repository_evidence(root, inventory)

        self.assertEqual(bundle.gate_registry_version, 2)
        self.assertEqual([gate.gate_id for gate in bundle.gates], ["UOS-DOC-001"])
        self.assertIs(bundle.gates[0].execution_state, ExecutionState.NOT_RUN)
        self.assertIs(bundle.cases[0].execution_state, ExecutionState.NOT_RUN)
        self.assertTrue(all(job.execution_state is ExecutionState.NOT_RUN for job in bundle.workflow_jobs))
        self.assertTrue(all(item.execution_state is ExecutionState.NOT_RUN for item in bundle.release_metadata))
        self.assertIn(
            "ReleaseManifest", {item.metadata_kind for item in bundle.artifact_metadata}
        )
        self.assertTrue(any(
            item.execution_state is ExecutionState.UNAVAILABLE
            and item.identity == "build:fixture-reports"
            for item in bundle.artifact_metadata
        ))
        self.assertTrue(all(
            "execut" in (gate.execution_detail or "").lower() for gate in bundle.gates
        ))

    def test_malformed_duplicate_and_unknown_gate_references_fail_closed(self) -> None:
        mutations = {
            "unknown evidence": (
                lambda text: text.replace("TST-001-sample", "TST-999-missing"),
                "INV-GATE-EVIDENCE",
            ),
            "unknown dependency": (
                lambda _text: _registry(dependency="UOS-DOC-999"),
                "INV-GATE-DEPENDENCY",
            ),
            "duplicate mapping": (
                lambda text: text.replace(
                    '"maps_to": [\n        "UOS-DOC-001"\n      ]',
                    '"maps_to": ["UOS-DOC-001", "UOS-DOC-001"]',
                ),
                "INV-GATE-DUPLICATE",
            ),
            "malformed status": (
                lambda text: text.replace('"status": "experimental"', '"status": "passing"'),
                "INV-GATE-SCHEMA",
            ),
        }
        for label, (mutate, expected_code) in mutations.items():
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                _write_adapter_fixture(root)
                registry = root / "docs/universeos/gate_registry.md"
                registry.write_text(mutate(registry.read_text(encoding="utf-8")), encoding="utf-8")
                inventory = discover_source_inventory(root, _revision(clean=False))
                with self.assertRaises(InventoryError) as captured:
                    adapt_repository_evidence(root, inventory)
                self.assertEqual(captured.exception.code, expected_code)

    def test_adapter_rejects_content_drift_after_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            registry = root / "docs/universeos/gate_registry.md"
            registry.write_text(registry.read_text(encoding="utf-8") + "\nchanged\n", encoding="utf-8")
            with self.assertRaises(InventoryError) as captured:
                adapt_repository_evidence(root, inventory)
        self.assertEqual(captured.exception.code, "INV-ADAPTER-CONTENT-DRIFT")

    def test_adapter_does_not_follow_manifest_replaced_by_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            _write_adapter_fixture(root)
            inventory = discover_source_inventory(root, _revision(clean=False))
            manifest = root / "tests/cases/test/TST-001-sample/case.toml"
            replacement = Path(outside) / "case.toml"
            replacement.write_bytes(manifest.read_bytes())
            manifest.unlink()
            try:
                manifest.symlink_to(replacement)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"symbolic links unavailable: {error}")
            with self.assertRaises(InventoryError) as captured:
                adapt_repository_evidence(root, inventory)
        self.assertEqual(captured.exception.code, "INV-ADAPTER-PATH")


class AdapterRepositoryDryRunTests(unittest.TestCase):
    def test_real_repository_adapter_dry_run_executes_no_commands(self) -> None:
        root = Path(__file__).resolve().parents[2]
        inventory = discover_source_inventory(root, _revision(clean=False))
        bundle = adapt_repository_evidence(root, inventory)

        self.assertEqual(bundle.gate_registry_version, 2)
        self.assertEqual(len(bundle.gates), 15)
        self.assertIn(
            "TST-329-universeos-gate-registry-docs-contract",
            {case.case_id for case in bundle.cases},
        )
        self.assertIn(
            (".github/workflows/release.yml", "build-assets"),
            {(job.workflow_path, job.job_id) for job in bundle.workflow_jobs},
        )
        self.assertTrue(all(gate.execution_state is ExecutionState.NOT_RUN for gate in bundle.gates))
        self.assertTrue(all(case.execution_state is ExecutionState.NOT_RUN for case in bundle.cases))
        self.assertTrue(all(job.execution_state is ExecutionState.NOT_RUN for job in bundle.workflow_jobs))
        self.assertTrue(any(
            item.execution_state is ExecutionState.UNAVAILABLE
            for item in bundle.artifact_metadata
        ))
        self.assertTrue(any(
            entry.category is SourceCategory.RELEASE_WORKFLOW for entry in inventory
        ))


if __name__ == "__main__":
    unittest.main()
