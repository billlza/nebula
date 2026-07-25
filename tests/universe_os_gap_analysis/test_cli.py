from __future__ import annotations

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from tools.universe_os_gap_analysis import cli


class CliTests(unittest.TestCase):
    def test_default_policy_disables_network_and_external_commands(self) -> None:
        self.assertFalse(cli.NETWORK_ENABLED)
        self.assertFalse(cli.EXTERNAL_COMMANDS_ENABLED)

    def test_validated_paths_requires_distinct_explicit_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(ValueError, "must not be the repository root"):
                cli.validated_paths(root, root)

    def test_dry_run_reports_policy_without_creating_output(self) -> None:
        # Under Task 12.1 a dry run drives the full pipeline; a bare temporary
        # directory is not a Git repository, so the run fails closed. In every
        # case the read-only policy is reported and no output is written.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "assessment-output"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                result = cli.main(
                    [
                        "--repo-root",
                        str(root),
                        "--output-dir",
                        str(output),
                        "--dry-run",
                    ]
                )
            payload = json.loads(stdout.getvalue())
            self.assertNotEqual(result, 0)
            self.assertTrue(payload["dryRun"])
            self.assertFalse(payload["networkEnabled"])
            self.assertFalse(payload["externalCommandsEnabled"])
            self.assertFalse(output.exists())

    def test_output_directory_is_required(self) -> None:
        parser = cli.build_parser()
        with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            parser.parse_args([])


if __name__ == "__main__":
    unittest.main()
