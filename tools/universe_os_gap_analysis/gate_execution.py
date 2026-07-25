"""Opt-in, allowlisted fast docs/gate contract runner (Task 14.3).

The default gap-analysis pipeline is read-only: it runs no commands, touches no
network, and treats every UniverseOS gate contract as *test-definition evidence*
only (``NotRun``). Task 14.3 adds an **opt-in** capability to run a small, fixed
allowlist of fast documentation/gate contracts and bind their genuine execution
evidence (command, environment, revision fingerprint, and produced artifacts)
into the canonical model.

This module never changes the default. Concretely:

* Without an explicit, *enabled* :class:`ExecutionPolicy` that allowlists a
  gate's command, every fast gate yields ``NotRun`` test-definition evidence and
  **no command is executed** (the injected command runner short-circuits on the
  disabled/absent policy path).
* The build gates ``BLD-017`` .. ``BLD-020`` are *always* kept as
  test-definition-only evidence. They are never sent to the command runner here,
  regardless of policy, because they are not fast docs contracts.
* A gate is reported as passed (``ExecutionState.VALIDATED``) *only* when the
  underlying :class:`ExecutionEvidence` is a genuine success
  (:attr:`ExecutionEvidence.gate_pass`). A failure, non-zero exit, timeout,
  missing executable, unsupported platform, snapshot/artifact error, or
  fingerprint drift is recorded with its real state and is **never** presented as
  a pass.

The runner takes an injected command runner (dependency injection) so tests can
exercise it hermetically with a fake runner, and so real gate execution stays
behind the explicit opt-in policy. :class:`LocalExecutionRunner` already matches
the :class:`GateCommandRunner` protocol, so production opt-in wiring simply
passes a real runner.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Protocol

from .execution import (
    ExecutionEvidence,
    ExecutionPolicy,
)
from .identifiers import StableId
from .models import ClosedStrEnum, ExecutionState


class GateEvidenceKind(ClosedStrEnum):
    """How a gate's evidence was produced for this assessment.

    ``TestDefinition`` means the gate is only known to be *defined* in the
    repository (its ``case.toml`` exists); no command was executed this run.
    ``Execution`` means the allowlisted command was actually attempted and its
    fail-closed :class:`ExecutionEvidence` is bound to the result.
    """

    TEST_DEFINITION = "TestDefinition"
    EXECUTION = "Execution"


@dataclass(frozen=True, slots=True, kw_only=True)
class GateContract:
    """One UniverseOS gate contract known to the assessment.

    ``command_id`` is the identifier used to look the gate's argv template up in
    an :class:`ExecutionPolicy` allowlist. It equals the gate/case ID so an
    opt-in policy binds unambiguously.
    """

    gate_id: StableId
    command_id: StableId
    description: str
    fast_executable: bool

    def __post_init__(self) -> None:
        object.__setattr__(self, "gate_id", StableId(self.gate_id))
        object.__setattr__(self, "command_id", StableId(self.command_id))
        if not isinstance(self.description, str) or not self.description.strip():
            raise ValueError("gate description must be a non-empty string")
        if not isinstance(self.fast_executable, bool):
            raise TypeError("fast_executable must be a bool")


def _contract(gate_id: str, description: str, *, fast_executable: bool) -> GateContract:
    return GateContract(
        gate_id=StableId(gate_id),
        command_id=StableId(gate_id),
        description=description,
        fast_executable=fast_executable,
    )


# The fixed allowlist of fast documentation/gate contracts that MAY be executed
# when the user explicitly opts in. These are pure repository-text contracts
# (see ``tests/cases/test/<id>/case.toml``) that are cheap and hermetic. Kept
# sorted by gate ID for deterministic reporting.
ALLOWLISTED_FAST_GATES: tuple[GateContract, ...] = (
    _contract(
        "TST-280-universeos-convergence-docs-contract",
        "UniverseOS convergence documentation contract.",
        fast_executable=True,
    ),
    _contract(
        "TST-282-system-profile-docs-contract",
        "System profile documentation contract.",
        fast_executable=True,
    ),
    _contract(
        "TST-329-universeos-gate-registry-docs-contract",
        "UniverseOS gate registry documentation contract.",
        fast_executable=True,
    ),
    _contract(
        "TST-331-backend-interface-docs-contract",
        "Backend interface documentation contract.",
        fast_executable=True,
    ),
)


# Build gates for the experimental primitive freestanding object slice. These
# are NOT fast docs contracts and are always retained as test-definition-only
# evidence here; they are never executed by this opt-in runner.
TEST_DEFINITION_ONLY_GATES: tuple[GateContract, ...] = (
    _contract(
        "BLD-017-freestanding-object-elf-contract",
        "Freestanding ELF64 relocatable-object contract.",
        fast_executable=False,
    ),
    _contract(
        "BLD-018-freestanding-request-state-machine",
        "Freestanding request state machine contract.",
        fast_executable=False,
    ),
    _contract(
        "BLD-019-freestanding-nir-allowlist",
        "Freestanding NIR allowlist contract.",
        fast_executable=False,
    ),
    _contract(
        "BLD-020-freestanding-transaction-and-toolchain",
        "Freestanding transaction and toolchain contract.",
        fast_executable=False,
    ),
)


@dataclass(frozen=True, slots=True, kw_only=True)
class GateExecutionResult:
    """The outcome for one gate: executed evidence or test-definition-only.

    The class enforces the "never present a failure/timeout/unavailable as a
    pass" invariant structurally: ``passed`` can only be ``True`` for a genuine,
    validated :class:`ExecutionEvidence`.
    """

    gate_id: StableId
    command_id: StableId
    evidence_kind: GateEvidenceKind
    execution_state: ExecutionState
    passed: bool
    evidence: ExecutionEvidence | None
    detail: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "gate_id", StableId(self.gate_id))
        object.__setattr__(self, "command_id", StableId(self.command_id))
        if not isinstance(self.evidence_kind, GateEvidenceKind):
            raise TypeError("evidence_kind must be a GateEvidenceKind")
        if not isinstance(self.execution_state, ExecutionState):
            raise TypeError("execution_state must be an ExecutionState")
        if not isinstance(self.passed, bool):
            raise TypeError("passed must be a bool")
        if self.evidence is not None and not isinstance(self.evidence, ExecutionEvidence):
            raise TypeError("evidence must be an ExecutionEvidence or None")
        if not isinstance(self.detail, str) or not self.detail.strip():
            raise ValueError("detail must be a non-empty string")

        if self.passed:
            # A pass is only ever derived from a genuine, validated execution.
            if self.evidence_kind is not GateEvidenceKind.EXECUTION:
                raise ValueError("a passed gate must carry execution evidence")
            if self.execution_state is not ExecutionState.VALIDATED:
                raise ValueError("a passed gate must be in the Validated state")
            if self.evidence is None or not self.evidence.gate_pass:
                raise ValueError("a passed gate requires validated execution evidence")

        if self.evidence_kind is GateEvidenceKind.TEST_DEFINITION:
            # Test-definition evidence never ran a command and never passes.
            if self.passed:
                raise ValueError("test-definition evidence cannot be a pass")
            if self.execution_state is not ExecutionState.NOT_RUN:
                raise ValueError("test-definition evidence must be NotRun")
            if self.evidence is not None and self.evidence.gate_pass:
                raise ValueError("test-definition evidence cannot wrap a passing execution")

        if (
            self.evidence_kind is GateEvidenceKind.EXECUTION
            and self.evidence is None
        ):
            raise ValueError("execution evidence kind requires an ExecutionEvidence")

    @property
    def executed(self) -> bool:
        """True when a command was actually attempted for this gate."""

        return self.evidence_kind is GateEvidenceKind.EXECUTION


@dataclass(frozen=True, slots=True, kw_only=True)
class GateExecutionReport:
    """The complete opt-in gate execution report for one assessment run."""

    fast_gate_results: tuple[GateExecutionResult, ...]
    definition_only_results: tuple[GateExecutionResult, ...]

    def __post_init__(self) -> None:
        fast = tuple(self.fast_gate_results)
        definition = tuple(self.definition_only_results)
        for item in (*fast, *definition):
            if not isinstance(item, GateExecutionResult):
                raise TypeError("results must be GateExecutionResult values")
        # Build gates are always test-definition-only, never executed here.
        for item in definition:
            if item.evidence_kind is not GateEvidenceKind.TEST_DEFINITION:
                raise ValueError("build gates must remain test-definition-only")
        object.__setattr__(
            self, "fast_gate_results", tuple(sorted(fast, key=lambda item: item.gate_id))
        )
        object.__setattr__(
            self,
            "definition_only_results",
            tuple(sorted(definition, key=lambda item: item.gate_id)),
        )

    @property
    def results(self) -> tuple[GateExecutionResult, ...]:
        """All gate results, fast gates first, then build gates, sorted by ID."""

        return (*self.fast_gate_results, *self.definition_only_results)

    @property
    def passed_gate_ids(self) -> tuple[StableId, ...]:
        return tuple(item.gate_id for item in self.results if item.passed)

    @property
    def execution_evidence(self) -> tuple[ExecutionEvidence, ...]:
        """Every genuinely attempted execution's evidence, sorted by evidence ID.

        The default (no opt-in) path attempts no commands, so this is empty and
        the pipeline's execution-evidence input is unchanged.
        """

        attempted = [
            item.evidence
            for item in self.results
            if item.executed and item.evidence is not None
        ]
        return tuple(sorted(attempted, key=lambda evidence: str(evidence.id)))


class GateCommandRunner(Protocol):
    """A runner that maps an allowlisted command ID to fail-closed evidence.

    :class:`~tools.universe_os_gap_analysis.execution.LocalExecutionRunner`
    satisfies this protocol; tests inject a fake to stay hermetic.
    """

    def execute(self, policy: ExecutionPolicy, command_id: str) -> ExecutionEvidence: ...


class AllowlistedFastGateRunner:
    """Run only the allowlisted fast gates through an injected command runner."""

    def __init__(self, command_runner: GateCommandRunner) -> None:
        if not hasattr(command_runner, "execute"):
            raise TypeError("command_runner must expose an execute(policy, command_id) method")
        self._runner = command_runner

    def run(self, policy: ExecutionPolicy) -> GateExecutionReport:
        if not isinstance(policy, ExecutionPolicy):
            raise TypeError("policy must be an ExecutionPolicy")
        fast_results = tuple(
            self._run_fast_gate(policy, gate) for gate in ALLOWLISTED_FAST_GATES
        )
        # Build gates are never executed here: no command is sent to the runner.
        definition_results = tuple(
            self._definition_only(gate) for gate in TEST_DEFINITION_ONLY_GATES
        )
        return GateExecutionReport(
            fast_gate_results=fast_results,
            definition_only_results=definition_results,
        )

    def _run_fast_gate(
        self, policy: ExecutionPolicy, gate: GateContract
    ) -> GateExecutionResult:
        evidence = self._runner.execute(policy, gate.command_id)
        if not isinstance(evidence, ExecutionEvidence):
            raise TypeError("command runner must return an ExecutionEvidence")
        if str(evidence.command_id) != str(gate.command_id):
            raise ValueError(
                "command runner returned evidence for a different command ID"
            )

        if evidence.gate_pass:
            # Genuine success: bound command/env/fingerprint/artifacts are valid.
            return GateExecutionResult(
                gate_id=gate.gate_id,
                command_id=gate.command_id,
                evidence_kind=GateEvidenceKind.EXECUTION,
                execution_state=ExecutionState.VALIDATED,
                passed=True,
                evidence=evidence,
                detail=f"{gate.gate_id} passed via bound local execution.",
            )

        if evidence.execution_state is ExecutionState.NOT_RUN:
            # Disabled or non-allowlisted policy: no command ran. This stays
            # test-definition-only evidence, exactly like the default path.
            return GateExecutionResult(
                gate_id=gate.gate_id,
                command_id=gate.command_id,
                evidence_kind=GateEvidenceKind.TEST_DEFINITION,
                execution_state=ExecutionState.NOT_RUN,
                passed=False,
                evidence=evidence,
                detail=(
                    f"{gate.gate_id} was not executed "
                    f"({evidence.outcome.value}); retained as test-definition evidence."
                ),
            )

        # Failed, timed out, or unavailable: record the real state; never a pass.
        return GateExecutionResult(
            gate_id=gate.gate_id,
            command_id=gate.command_id,
            evidence_kind=GateEvidenceKind.EXECUTION,
            execution_state=evidence.execution_state,
            passed=False,
            evidence=evidence,
            detail=(
                f"{gate.gate_id} did not pass "
                f"(outcome={evidence.outcome.value}, state={evidence.execution_state.value})."
            ),
        )

    @staticmethod
    def _definition_only(gate: GateContract) -> GateExecutionResult:
        return GateExecutionResult(
            gate_id=gate.gate_id,
            command_id=gate.command_id,
            evidence_kind=GateEvidenceKind.TEST_DEFINITION,
            execution_state=ExecutionState.NOT_RUN,
            passed=False,
            evidence=None,
            detail=(
                f"{gate.gate_id} is retained as test-definition evidence only "
                "and is never executed by the fast-gate runner."
            ),
        )


def run_allowlisted_fast_gates(
    command_runner: GateCommandRunner,
    policy: ExecutionPolicy | None = None,
) -> GateExecutionReport:
    """Convenience API for the opt-in allowlisted fast gate runner.

    With no policy (or a disabled policy) this attempts no commands and yields
    only ``NotRun`` test-definition evidence, preserving the read-only default.
    """

    return AllowlistedFastGateRunner(command_runner).run(policy or ExecutionPolicy())
