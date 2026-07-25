"""CLI-level tests for the wired assessment pipeline (Task 12.1).

These complement ``test_cli.py`` (which covers the argument/path contract) by
exercising ``cli.main`` driving the full deterministic pipeline: the read-only
policy surfaced in the JSON summary, the fail-closed exit codes, and the
guarantee that a dry run and any failure write nothing.
"""

from __future__ import annotations

import io
import json
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis import cli
from tools.universe_os_gap_analysis.pipeline import EXIT_OK


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _run(argv: list[str]) -> tuple[int, dict]:
    stdout = io.StringIO()
    with redirect_stdout(stdout):
        code = cli.main(argv)
    return code, json.loads(stdout.getvalue())


class CliPolicySummaryTests(unittest.TestCase):
    def test_summary_reports_read_only_policy(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            _, payload = _run(
                [
                    "--repo-root",
                    str(_real_repo_root()),
                    "--output-dir",
                    str(output),
                    "--dry-run",
                ]
            )
            self.assertTrue(payload["dryRun"])
            self.assertFalse(payload["networkEnabled"])
            self.assertFalse(payload["externalCommandsEnabled"])
            self.assertEqual(payload["writtenPaths"], [])


class CliFailClosedTests(unittest.TestCase):
    def test_dry_run_against_real_repo_is_deterministic_and_writes_nothing(self) -> None:
        with TemporaryDirectory() as tmp:
            output = Path(tmp) / "assessment-output"
            code, payload = _run(
                [
                    "--repo-root",
                    str(_real_repo_root()),
                    "--output-dir",
                    str(output),
                    "--dry-run",
                ]
            )
            # The curated baseline (Task 14.2) is now the default assembler, so
            # the real-repository model validates and the pipeline reaches
            # EXIT_OK. A dry run still writes nothing.
            self.assertEqual(code, EXIT_OK)
            self.assertEqual(payload["exitCode"], EXIT_OK)
            self.assertFalse(payload["published"])
            self.assertFalse(output.exists())
            # The upstream stages still ran and are reported in order.
            stage_names = [stage["name"] for stage in payload["stages"]]
            self.assertEqual(stage_names[0], "revision-binder")
            self.assertEqual(stage_names[-1], "validate-and-publish")

    def test_unbindable_repository_returns_nonzero_without_writing(self) -> None:
        with TemporaryDirectory() as tmp:
            repo = Path(tmp) / "plain-dir"
            repo.mkdir()
            output = Path(tmp) / "assessment-output"
            code, payload = _run(
                ["--repo-root", str(repo), "--output-dir", str(output)]
            )
            self.assertNotEqual(code, EXIT_OK)
            self.assertFalse(payload["published"])
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
