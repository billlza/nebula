"""Tests for the opt-in allowlisted fast gate runner (Task 14.3).

These tests are hermetic: they never touch the network and never mutate the
Nebula repository. Real command execution is exercised only against a harmless
allowlisted command inside a temporary Git fixture, and all other outcomes are
driven through a fake, injected command runner (dependency injection).
"""

from __future__ import annotations

import platform
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.universe_os_gap_analysis import (
    ALLOWLISTED_FAST_GATES,
    TEST_DEFINITION_ONLY_GATES,
    AllowlistedCommand,
    AllowlistedFastGateRunner,
    ExecutionEvidence,
    ExecutionOutcome,
    ExecutionPolicy,
    ExecutionValidationState,
    GateEvidenceKind,
    LocalExecutionRunner,
    run_allowlisted_fast_gates,
)
from tools.universe_os_gap_analysis.identifiers import stable_id
from tools.universe_os_gap_analysis.models import ExecutionState

_FAST_GATE_IDS = tuple(str(gate.command_id) for gate in ALLOWLISTED_FAST_GATES)
_BUILD_GATE_IDS = tuple(str(gate.command_id) for gate in TEST_DEFINITION_ONLY_GATES)


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


def _command(command_id: str, code: str, *, timeout: float = 5.0) -> AllowlistedCommand:
    return AllowlistedCommand(
        command_id=command_id,
        executable=Path(sys.executable).resolve(),
        arguments=("-c", code),
        supported_platforms=(platform.system(),),
        timeout_seconds=timeout,
        visible_argument_indexes=(0,),
    )


def _fake_evidence(
    command_id: str,
    outcome: ExecutionOutcome,
    state: ExecutionState,
    validation: ExecutionValidationState,
    *,
    exit_status: int | None = None,
) -> ExecutionEvidence:
    """A minimal, schema-valid ExecutionEvidence that never reports gate_pass."""

    return ExecutionEvidence(
        id=stable_id("execution-evidence", command_id, outcome.value),
        command_id=command_id,
        argv_digest=None,
        redacted_command=(),
        platform=None,
        environment=None,
        outcome=outcome,
        execution_state=state,
        validation_state=validation,
        exit_status=exit_status,
        stdout_artifact=None,
        stderr_artifact=None,
        before_revision=None,
        after_revision=None,
        detail=f"fake evidence for {command_id}",
    )


class _FakeGateRunner:
    """Injected command runner recording calls and returning canned evidence."""

    def __init__(self, evidence_by_id: dict[str, ExecutionEvidence]) -> None:
        self._evidence_by_id = dict(evidence_by_id)
        self.requested_command_ids: list[str] = []

    def execute(self, policy: ExecutionPolicy, command_id: str) -> ExecutionEvidence:
        self.requested_command_ids.append(command_id)
        if command_id in self._evidence_by_id:
            return self._evidence_by_id[command_id]
        # Default to a disabled/not-run marker for unmapped IDs.
        return _fake_evidence(
            command_id,
            ExecutionOutcome.DISABLED,
            ExecutionState.NOT_RUN,
            ExecutionValidationState.NOT_RUN,
        )


class GateExecutionDefaultPathTests(unittest.TestCase):
    def test_default_no_opt_in_yields_test_definition_not_run_and_runs_no_commands(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _repository(root)
            markers = {
                gate_id: root / f"marker-{index}"
                for index, gate_id in enumerate(_FAST_GATE_IDS)
            }
            commands = tuple(
                _command(
                    gate_id,
                    f"from pathlib import Path; Path({str(markers[gate_id])!r}).touch()",
                )
                for gate_id in _FAST_GATE_IDS
            )
            # Even with commands present, a *disabled* policy is the default and
            # must run nothing.
            disabled_policy = ExecutionPolicy(enabled=False, commands=commands)
            runner = AllowlistedFastGateRunner(
                LocalExecutionRunner(root, root / "assessment-output")
            )
            report = runner.run(disabled_policy)

        self.assertEqual(report.passed_gate_ids, ())
        self.assertEqual(report.execution_evidence, ())
        for result in report.fast_gate_results:
            self.assertIs(result.evidence_kind, GateEvidenceKind.TEST_DEFINITION)
            self.assertIs(result.execution_state, ExecutionState.NOT_RUN)
            self.assertFalse(result.passed)
            self.assertFalse(result.executed)
        # No allowlisted command was executed, so no marker file exists.
        for marker in markers.values():
            self.assertFalse(marker.exists())

    def test_missing_policy_defaults_to_read_only_test_definition_evidence(self) -> None:
        report = run_allowlisted_fast_gates(
            _FakeGateRunner({}), policy=None
        )
        self.assertEqual(report.passed_gate_ids, ())
        self.assertEqual(len(report.fast_gate_results), len(ALLOWLISTED_FAST_GATES))
        for result in report.fast_gate_results:
            self.assertIs(result.evidence_kind, GateEvidenceKind.TEST_DEFINITION)
            self.assertIs(result.execution_state, ExecutionState.NOT_RUN)
            self.assertFalse(result.passed)


class GateExecutionOptInTests(unittest.TestCase):
    def test_opt_in_passing_gate_binds_command_environment_fingerprint_and_artifacts(self) -> None:
        target_gate = _FAST_GATE_IDS[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _repository(root)
            output = root / "assessment-output"
            command = _command(
                target_gate,
                "import sys; print('gate-ok'); print('note', file=sys.stderr)",
            )
            policy = ExecutionPolicy(enabled=True, commands=(command,))
            runner = AllowlistedFastGateRunner(LocalExecutionRunner(root, output))
            report = runner.run(policy)

            results = {str(item.gate_id): item for item in report.fast_gate_results}
            passing = results[target_gate]

            self.assertTrue(passing.passed)
            self.assertIs(passing.evidence_kind, GateEvidenceKind.EXECUTION)
            self.assertIs(passing.execution_state, ExecutionState.VALIDATED)
            evidence = passing.evidence
            assert evidence is not None
            # Command binding (redacted argv), environment, and fingerprint.
            self.assertTrue(evidence.gate_pass)
            self.assertEqual(evidence.exit_status, 0)
            self.assertIn(Path(sys.executable).resolve().name, evidence.redacted_command)
            self.assertIsNotNone(evidence.environment)
            self.assertFalse(evidence.environment.network_access_requested)
            self.assertIsNotNone(evidence.before_revision)
            self.assertEqual(evidence.before_revision, evidence.after_revision)
            # Produced artifacts are bound and persisted under the output dir.
            assert evidence.stdout_artifact is not None
            stdout = (output / evidence.stdout_artifact.path).read_bytes()
            self.assertEqual(stdout, b"gate-ok\n")

            # The gates that were not allowlisted stay test-definition-only.
            for gate_id in _FAST_GATE_IDS[1:]:
                other = results[gate_id]
                self.assertIs(other.evidence_kind, GateEvidenceKind.TEST_DEFINITION)
                self.assertIs(other.execution_state, ExecutionState.NOT_RUN)
                self.assertFalse(other.passed)

            self.assertEqual(report.passed_gate_ids, (passing.gate_id,))
            self.assertEqual(report.execution_evidence, (evidence,))

    def test_failing_timeout_and_unavailable_gates_are_never_presented_as_pass(self) -> None:
        failing_id, timeout_id, unavailable_id, not_run_id = _FAST_GATE_IDS
        canned = {
            failing_id: _fake_evidence(
                failing_id,
                ExecutionOutcome.NONZERO_EXIT,
                ExecutionState.FAILED,
                ExecutionValidationState.FAILED,
                exit_status=7,
            ),
            timeout_id: _fake_evidence(
                timeout_id,
                ExecutionOutcome.TIMED_OUT,
                ExecutionState.FAILED,
                ExecutionValidationState.FAILED,
            ),
            unavailable_id: _fake_evidence(
                unavailable_id,
                ExecutionOutcome.EXECUTABLE_UNAVAILABLE,
                ExecutionState.UNAVAILABLE,
                ExecutionValidationState.FAILED,
            ),
            not_run_id: _fake_evidence(
                not_run_id,
                ExecutionOutcome.DISABLED,
                ExecutionState.NOT_RUN,
                ExecutionValidationState.NOT_RUN,
            ),
        }
        fake = _FakeGateRunner(canned)
        report = run_allowlisted_fast_gates(
            fake, policy=ExecutionPolicy(enabled=True)
        )
        results = {str(item.gate_id): item for item in report.fast_gate_results}

        # Nothing passes and no failure/timeout/unavailable becomes a pass.
        self.assertEqual(report.passed_gate_ids, ())
        for gate_id in (failing_id, timeout_id, unavailable_id, not_run_id):
            self.assertFalse(results[gate_id].passed)

        self.assertIs(results[failing_id].execution_state, ExecutionState.FAILED)
        self.assertIs(results[failing_id].evidence_kind, GateEvidenceKind.EXECUTION)
        self.assertIs(results[timeout_id].execution_state, ExecutionState.FAILED)
        self.assertIs(results[unavailable_id].execution_state, ExecutionState.UNAVAILABLE)
        self.assertIs(results[unavailable_id].evidence_kind, GateEvidenceKind.EXECUTION)
        # A NotRun outcome remains test-definition evidence, not an execution.
        self.assertIs(results[not_run_id].evidence_kind, GateEvidenceKind.TEST_DEFINITION)
        self.assertIs(results[not_run_id].execution_state, ExecutionState.NOT_RUN)

    def test_build_gates_stay_test_definition_only_and_are_never_executed(self) -> None:
        fake = _FakeGateRunner({})
        report = run_allowlisted_fast_gates(
            fake, policy=ExecutionPolicy(enabled=True)
        )

        # Only fast gates may reach the command runner; build gates never do.
        for command_id in fake.requested_command_ids:
            self.assertIn(command_id, _FAST_GATE_IDS)
        self.assertTrue(set(fake.requested_command_ids).isdisjoint(_BUILD_GATE_IDS))

        self.assertEqual(len(report.definition_only_results), len(TEST_DEFINITION_ONLY_GATES))
        reported_ids = {str(item.gate_id) for item in report.definition_only_results}
        self.assertEqual(reported_ids, set(_BUILD_GATE_IDS))
        for result in report.definition_only_results:
            self.assertIs(result.evidence_kind, GateEvidenceKind.TEST_DEFINITION)
            self.assertIs(result.execution_state, ExecutionState.NOT_RUN)
            self.assertFalse(result.passed)
            self.assertIsNone(result.evidence)


if __name__ == "__main__":
    unittest.main()
