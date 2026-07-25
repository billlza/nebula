from __future__ import annotations

import hashlib
import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.universe_os_gap_analysis import (
    AllowlistedCommand,
    ExecutionOutcome,
    ExecutionPolicy,
    ExecutionValidationState,
    LocalExecutionRunner,
)
from tools.universe_os_gap_analysis.models import ExecutionState


def _git(root: Path, *arguments: str) -> None:
    subprocess.run(
        ("git", *arguments), cwd=root, check=True,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def _repository(root: Path) -> None:
    _git(root, "init", "-q")
    _git(root, "config", "user.email", "gap-analysis@example.invalid")
    _git(root, "config", "user.name", "Gap Analysis Tests")
    (root / "VERSION").write_text("1.0.0\n", encoding="utf-8")
    (root / "README.md").write_text("# Fixture\n", encoding="utf-8")
    _git(root, "add", "VERSION", "README.md")
    _git(root, "commit", "-q", "-m", "fixture")


def _command(
    command_id: str,
    code: str,
    *,
    timeout: float = 2.0,
    platforms: tuple[str, ...] | None = None,
) -> AllowlistedCommand:
    return AllowlistedCommand(
        command_id=command_id,
        executable=Path(sys.executable).resolve(),
        arguments=("-c", code),
        supported_platforms=platforms or (platform.system(),),
        timeout_seconds=timeout,
        visible_argument_indexes=(0,),
    )


class ExecutionPolicyUnitTests(unittest.TestCase):
    def test_default_policy_is_disabled_and_performs_no_repository_or_process_work(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "must-not-exist"
            command = _command(
                "TST-disabled", f"from pathlib import Path; Path({str(marker)!r}).touch()"
            )
            runner = LocalExecutionRunner(root, root / "artifacts")
            evidence = runner.execute(ExecutionPolicy(commands=(command,)), command.command_id)

        self.assertIs(evidence.outcome, ExecutionOutcome.DISABLED)
        self.assertIs(evidence.execution_state, ExecutionState.NOT_RUN)
        self.assertIs(evidence.validation_state, ExecutionValidationState.NOT_RUN)
        self.assertFalse(evidence.gate_pass)
        self.assertIsNone(evidence.before_revision)
        self.assertFalse(marker.exists())

    def test_enabled_policy_denies_non_allowlisted_command_id_without_running_any_command(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "must-not-exist"
            allowed = _command(
                "TST-allowed", f"from pathlib import Path; Path({str(marker)!r}).touch()"
            )
            evidence = LocalExecutionRunner(root, root / "artifacts").execute(
                ExecutionPolicy(enabled=True, commands=(allowed,)), "TST-not-allowed"
            )

        self.assertIs(evidence.outcome, ExecutionOutcome.NOT_ALLOWLISTED)
        self.assertIs(evidence.execution_state, ExecutionState.NOT_RUN)
        self.assertFalse(evidence.gate_pass)
        self.assertFalse(marker.exists())

    def test_allowlisted_commands_require_absolute_executable_and_fixed_platforms(self) -> None:
        with self.assertRaisesRegex(ValueError, "absolute local path"):
            AllowlistedCommand(
                command_id="TST-relative", executable=Path("python"), arguments=(),
                supported_platforms=(platform.system(),), timeout_seconds=1,
            )
        with self.assertRaisesRegex(ValueError, "explicit and non-empty"):
            AllowlistedCommand(
                command_id="TST-platform", executable=Path(sys.executable).resolve(),
                arguments=(), supported_platforms=(), timeout_seconds=1,
            )


class LocalExecutionIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self._temporary.cleanup)
        self.root = Path(self._temporary.name)
        _repository(self.root)
        self.output = self.root / "assessment-output"
        self.runner = LocalExecutionRunner(self.root, self.output)

    @staticmethod
    def _policy(command: AllowlistedCommand, *, limit: int = 1024 * 1024) -> ExecutionPolicy:
        return ExecutionPolicy(
            enabled=True, commands=(command,), max_artifact_bytes=limit
        )

    def _artifact(self, path: str) -> bytes:
        return (self.output / path).read_bytes()


    def test_success_records_sanitized_context_artifacts_and_equal_revision_snapshots(self) -> None:
        secret_code = "import sys; print('hello'); print('warning', file=sys.stderr)"
        command = _command("TST-success", secret_code)
        evidence = self.runner.execute(self._policy(command), command.command_id)

        self.assertIs(evidence.outcome, ExecutionOutcome.SUCCEEDED)
        self.assertEqual(evidence.exit_status, 0)
        self.assertTrue(evidence.gate_pass)
        self.assertEqual(evidence.before_revision, evidence.after_revision)
        self.assertEqual(
            evidence.redacted_command,
            (Path(sys.executable).resolve().name, "-c", "<redacted>"),
        )
        self.assertNotIn(secret_code, " ".join(evidence.redacted_command))
        self.assertEqual(len(evidence.argv_digest or ""), 64)
        self.assertFalse(evidence.environment.shell_enabled)
        self.assertFalse(evidence.environment.network_access_requested)
        self.assertFalse(evidence.environment.interactive_input_inherited)
        self.assertNotIn("SECRET", evidence.environment.variable_names)
        assert evidence.stdout_artifact is not None
        assert evidence.stderr_artifact is not None
        stdout = self._artifact(evidence.stdout_artifact.path)
        stderr = self._artifact(evidence.stderr_artifact.path)
        self.assertEqual(stdout, b"hello\n")
        self.assertEqual(stderr, b"warning\n")
        self.assertEqual(evidence.stdout_artifact.sha256, hashlib.sha256(stdout).hexdigest())

    def test_nonzero_exit_is_failed_evidence_and_never_gate_pass(self) -> None:
        command = _command(
            "TST-nonzero", "import sys; print('bad', file=sys.stderr); raise SystemExit(7)"
        )
        evidence = self.runner.execute(self._policy(command), command.command_id)

        self.assertIs(evidence.outcome, ExecutionOutcome.NONZERO_EXIT)
        self.assertEqual(evidence.exit_status, 7)
        self.assertIs(evidence.execution_state, ExecutionState.FAILED)
        self.assertIs(evidence.validation_state, ExecutionValidationState.FAILED)
        self.assertFalse(evidence.gate_pass)
        assert evidence.stderr_artifact is not None
        self.assertEqual(self._artifact(evidence.stderr_artifact.path), b"bad\n")

    def test_timeout_is_distinct_failed_evidence_with_partial_artifacts(self) -> None:
        # A 1.0s timeout still guarantees a timeout (the child sleeps 5s) while
        # giving the interpreter enough time to start and flush "started" before
        # the SIGKILL, so the partial-artifact assertion is not a startup race.
        command = _command(
            "TST-timeout",
            "import sys,time; print('started', flush=True); time.sleep(5)",
            timeout=1.0,
        )
        evidence = self.runner.execute(self._policy(command), command.command_id)

        self.assertIs(evidence.outcome, ExecutionOutcome.TIMED_OUT)
        self.assertIs(evidence.execution_state, ExecutionState.FAILED)
        self.assertFalse(evidence.gate_pass)
        assert evidence.stdout_artifact is not None
        self.assertEqual(self._artifact(evidence.stdout_artifact.path), b"started\n")

    def test_missing_executable_and_unsupported_platform_are_distinct_unavailable_states(self) -> None:
        missing = AllowlistedCommand(
            command_id="TST-missing", executable=(self.root / "missing-tool").resolve(),
            arguments=(), supported_platforms=(platform.system(),), timeout_seconds=1,
        )
        missing_evidence = self.runner.execute(self._policy(missing), missing.command_id)
        unsupported = _command(
            "TST-platform", "raise SystemExit('must not run')",
            platforms=("DefinitelyNotThisPlatform",),
        )
        platform_evidence = self.runner.execute(
            self._policy(unsupported), unsupported.command_id
        )

        self.assertIs(missing_evidence.outcome, ExecutionOutcome.EXECUTABLE_UNAVAILABLE)
        self.assertIs(platform_evidence.outcome, ExecutionOutcome.PLATFORM_UNAVAILABLE)
        for evidence in (missing_evidence, platform_evidence):
            self.assertIs(evidence.execution_state, ExecutionState.UNAVAILABLE)
            self.assertIs(evidence.validation_state, ExecutionValidationState.FAILED)
            self.assertFalse(evidence.gate_pass)
            self.assertEqual(evidence.before_revision, evidence.after_revision)
            self.assertIsNone(evidence.stdout_artifact)


    def test_artifacts_are_bounded_while_recording_the_complete_stream_digest(self) -> None:
        command = _command(
            "TST-bounded",
            "import sys; sys.stdout.write('x'*4096); sys.stderr.write('y'*2048)",
        )
        evidence = self.runner.execute(self._policy(command, limit=64), command.command_id)

        self.assertTrue(evidence.gate_pass)
        assert evidence.stdout_artifact is not None
        assert evidence.stderr_artifact is not None
        for reference, expected_byte, observed in (
            (evidence.stdout_artifact, b"x", 4096),
            (evidence.stderr_artifact, b"y", 2048),
        ):
            content = self._artifact(reference.path)
            self.assertEqual(content, expected_byte * 64)
            self.assertEqual(reference.byte_count, 64)
            self.assertEqual(reference.observed_byte_count, observed)
            self.assertTrue(reference.truncated)
            self.assertEqual(reference.sha256, hashlib.sha256(content).hexdigest())
            self.assertEqual(
                reference.observed_stream_sha256,
                hashlib.sha256(expected_byte * observed).hexdigest(),
            )

    def test_worktree_drift_invalidates_successful_process_evidence(self) -> None:
        command = _command(
            "TST-drift",
            "from pathlib import Path; print('ran'); Path('drift.txt').write_text('changed')",
        )
        evidence = self.runner.execute(self._policy(command), command.command_id)

        self.assertEqual(evidence.exit_status, 0)
        self.assertIs(evidence.outcome, ExecutionOutcome.FINGERPRINT_DRIFT)
        self.assertIs(evidence.validation_state, ExecutionValidationState.INVALIDATED)
        self.assertIs(evidence.execution_state, ExecutionState.FAILED)
        self.assertFalse(evidence.gate_pass)
        self.assertNotEqual(evidence.before_revision, evidence.after_revision)
        assert evidence.stdout_artifact is not None
        self.assertEqual(self._artifact(evidence.stdout_artifact.path), b"ran\n")


if __name__ == "__main__":
    unittest.main()
