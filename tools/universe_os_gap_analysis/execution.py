"""Explicit, local-only execution policy and fail-closed evidence capture."""

from __future__ import annotations

import hashlib
import math
import os
import platform
import signal
import stat
import subprocess
import threading
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Protocol, Sequence

from .identifiers import StableId, stable_id
from .models import AssessmentRevision, ClosedStrEnum, ExecutionState
from .revision import RevisionBinder, RevisionBindingError

_REDACTED = "<redacted>"
_READ_CHUNK_BYTES = 64 * 1024
_READER_JOIN_SECONDS = 5.0
_DEFAULT_ARTIFACT_LIMIT = 1024 * 1024
_MAX_ARTIFACT_LIMIT = 16 * 1024 * 1024


class ExecutionOutcome(ClosedStrEnum):
    DISABLED = "Disabled"
    NOT_ALLOWLISTED = "NotAllowlisted"
    SUCCEEDED = "Succeeded"
    NONZERO_EXIT = "NonzeroExit"
    TIMED_OUT = "TimedOut"
    EXECUTABLE_UNAVAILABLE = "ExecutableUnavailable"
    PLATFORM_UNAVAILABLE = "PlatformUnavailable"
    EXECUTION_FAILED = "ExecutionFailed"
    SNAPSHOT_FAILED = "SnapshotFailed"
    ARTIFACT_FAILED = "ArtifactFailed"
    FINGERPRINT_DRIFT = "FingerprintDrift"


class ExecutionValidationState(ClosedStrEnum):
    NOT_RUN = "NotRun"
    VALIDATED = "Validated"
    FAILED = "Failed"
    INVALIDATED = "Invalidated"


@dataclass(frozen=True, slots=True, kw_only=True)
class AllowlistedCommand:
    """One immutable argv template; callers cannot append or replace arguments."""

    command_id: StableId
    executable: Path
    arguments: tuple[str, ...]
    supported_platforms: tuple[str, ...]
    timeout_seconds: float
    visible_argument_indexes: tuple[int, ...] = ()

    def __post_init__(self) -> None:
        object.__setattr__(self, "command_id", StableId(self.command_id))
        executable = Path(self.executable)
        if not executable.is_absolute():
            raise ValueError("allowlisted executable must be an absolute local path")
        object.__setattr__(self, "executable", executable)
        arguments = tuple(self.arguments)
        for argument in arguments:
            _text("argument", argument, allow_empty=True)
            _utf8(argument, "argument")
            if "\x00" in argument:
                raise ValueError("command arguments must not contain NUL")
        object.__setattr__(self, "arguments", arguments)
        platforms = tuple(sorted(set(self.supported_platforms)))
        if not platforms:
            raise ValueError("supported_platforms must be explicit and non-empty")
        for value in platforms:
            _text("supported platform", value)
        object.__setattr__(self, "supported_platforms", platforms)
        if (
            isinstance(self.timeout_seconds, bool)
            or not isinstance(self.timeout_seconds, (int, float))
            or not math.isfinite(self.timeout_seconds)
            or self.timeout_seconds <= 0
        ):
            raise ValueError("timeout_seconds must be a positive finite number")
        visible = tuple(sorted(set(self.visible_argument_indexes)))
        if any(isinstance(index, bool) or not isinstance(index, int) for index in visible):
            raise TypeError("visible argument indexes must be integers")
        if any(index < 0 or index >= len(arguments) for index in visible):
            raise ValueError("visible argument index is outside configured argv")
        object.__setattr__(self, "visible_argument_indexes", visible)

    def redacted_command(self, executable: Path) -> tuple[str, ...]:
        visible = frozenset(self.visible_argument_indexes)
        return (
            executable.name,
            *(value if index in visible else _REDACTED for index, value in enumerate(self.arguments)),
        )


@dataclass(frozen=True, slots=True, kw_only=True)
class ExecutionPolicy:
    """Disabled-by-default policy containing the complete command allowlist."""

    enabled: bool = False
    commands: tuple[AllowlistedCommand, ...] = ()
    max_artifact_bytes: int = _DEFAULT_ARTIFACT_LIMIT

    def __post_init__(self) -> None:
        if not isinstance(self.enabled, bool):
            raise TypeError("enabled must be a bool")
        commands = tuple(self.commands)
        if not all(isinstance(item, AllowlistedCommand) for item in commands):
            raise TypeError("commands must contain AllowlistedCommand values")
        ids = [str(item.command_id) for item in commands]
        if len(ids) != len(set(ids)):
            raise ValueError("allowlisted command IDs must be unique")
        object.__setattr__(self, "commands", tuple(sorted(commands, key=lambda item: item.command_id)))
        limit = self.max_artifact_bytes
        if (
            isinstance(limit, bool)
            or not isinstance(limit, int)
            or limit < 0
            or limit > _MAX_ARTIFACT_LIMIT
        ):
            raise ValueError(f"max_artifact_bytes must be in 0..{_MAX_ARTIFACT_LIMIT}")

    def command(self, command_id: str) -> AllowlistedCommand | None:
        return next((item for item in self.commands if item.command_id == command_id), None)


@dataclass(frozen=True, slots=True, kw_only=True)
class PlatformSummary:
    system: str
    release: str
    machine: str
    python_implementation: str

    def __post_init__(self) -> None:
        for name in ("system", "release", "machine", "python_implementation"):
            _text(name, getattr(self, name))


@dataclass(frozen=True, slots=True, kw_only=True)
class EnvironmentSummary:
    variable_names: tuple[str, ...]
    inherited_variable_names: tuple[str, ...]
    environment_digest: str
    shell_enabled: bool = field(default=False, init=False)
    network_access_requested: bool = field(default=False, init=False)
    interactive_input_inherited: bool = field(default=False, init=False)

    def __post_init__(self) -> None:
        object.__setattr__(self, "variable_names", _unique_texts(self.variable_names))
        object.__setattr__(
            self, "inherited_variable_names", _unique_texts(self.inherited_variable_names)
        )
        _digest("environment_digest", self.environment_digest)
        if not set(self.inherited_variable_names).issubset(self.variable_names):
            raise ValueError("inherited environment names must be included in variable_names")


@dataclass(frozen=True, slots=True, kw_only=True)
class ExecutionRevisionSnapshot:
    commit_id: str
    branch: str
    version: str
    repository_root_id: StableId
    fingerprint_algorithm: str
    worktree_fingerprint: str
    tracked_diff_hash: str
    untracked_path_set_hash: str

    def __post_init__(self) -> None:
        for name in (
            "commit_id", "branch", "version", "fingerprint_algorithm",
            "worktree_fingerprint", "tracked_diff_hash", "untracked_path_set_hash",
        ):
            _text(name, getattr(self, name))
        object.__setattr__(self, "repository_root_id", StableId(self.repository_root_id))

    @classmethod
    def from_revision(cls, revision: AssessmentRevision) -> "ExecutionRevisionSnapshot":
        return cls(
            commit_id=revision.commit_id,
            branch=revision.branch,
            version=revision.version,
            repository_root_id=revision.repository_root_id,
            fingerprint_algorithm=revision.fingerprint_algorithm,
            worktree_fingerprint=revision.worktree_fingerprint,
            tracked_diff_hash=revision.tracked_diff_hash,
            untracked_path_set_hash=revision.untracked_path_set_hash,
        )


@dataclass(frozen=True, slots=True, kw_only=True)
class ExecutionArtifactReference:
    path: str
    sha256: str
    observed_stream_sha256: str
    byte_count: int
    observed_byte_count: int
    truncated: bool

    def __post_init__(self) -> None:
        path = PurePosixPath(self.path)
        if (
            not self.path
            or path.is_absolute()
            or ".." in path.parts
            or path.as_posix() != self.path
        ):
            raise ValueError("artifact reference must be a normalized relative path")
        _digest("sha256", self.sha256)
        _digest("observed_stream_sha256", self.observed_stream_sha256)
        for name in ("byte_count", "observed_byte_count"):
            value = getattr(self, name)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")
        if self.byte_count > self.observed_byte_count:
            raise ValueError("stored artifact cannot exceed observed stream length")
        if not isinstance(self.truncated, bool):
            raise TypeError("truncated must be a bool")
        if self.truncated != (self.byte_count < self.observed_byte_count):
            raise ValueError("truncated must describe the stored/observed byte counts")


@dataclass(frozen=True, slots=True, kw_only=True)
class ExecutionEvidence:
    id: StableId
    command_id: str
    argv_digest: str | None
    redacted_command: tuple[str, ...]
    platform: PlatformSummary | None
    environment: EnvironmentSummary | None
    outcome: ExecutionOutcome
    execution_state: ExecutionState
    validation_state: ExecutionValidationState
    exit_status: int | None
    stdout_artifact: ExecutionArtifactReference | None
    stderr_artifact: ExecutionArtifactReference | None
    before_revision: ExecutionRevisionSnapshot | None
    after_revision: ExecutionRevisionSnapshot | None
    detail: str
    gate_pass: bool = field(default=False, init=False)

    def __post_init__(self) -> None:
        object.__setattr__(self, "id", StableId(self.id))
        _text("command_id", self.command_id)
        if self.argv_digest is not None:
            _digest("argv_digest", self.argv_digest)
        command = tuple(self.redacted_command)
        for value in command:
            _text("redacted command item", value, allow_empty=True)
        object.__setattr__(self, "redacted_command", command)
        if not isinstance(self.outcome, ExecutionOutcome):
            raise TypeError("outcome must be ExecutionOutcome")
        if not isinstance(self.execution_state, ExecutionState):
            raise TypeError("execution_state must be ExecutionState")
        if not isinstance(self.validation_state, ExecutionValidationState):
            raise TypeError("validation_state must be ExecutionValidationState")
        if self.exit_status is not None and (
            isinstance(self.exit_status, bool) or not isinstance(self.exit_status, int)
        ):
            raise TypeError("exit_status must be an integer or None")
        for value, model_type, name in (
            (self.platform, PlatformSummary, "platform"),
            (self.environment, EnvironmentSummary, "environment"),
            (self.stdout_artifact, ExecutionArtifactReference, "stdout_artifact"),
            (self.stderr_artifact, ExecutionArtifactReference, "stderr_artifact"),
            (self.before_revision, ExecutionRevisionSnapshot, "before_revision"),
            (self.after_revision, ExecutionRevisionSnapshot, "after_revision"),
        ):
            if value is not None and not isinstance(value, model_type):
                raise TypeError(f"{name} must be {model_type.__name__} or None")
        _text("detail", self.detail)
        passed = (
            self.outcome is ExecutionOutcome.SUCCEEDED
            and self.execution_state is ExecutionState.VALIDATED
            and self.validation_state is ExecutionValidationState.VALIDATED
            and self.exit_status == 0
            and self.before_revision == self.after_revision
            and self.before_revision is not None
        )
        object.__setattr__(self, "gate_pass", passed)


class ExecutionSnapshotProvider(Protocol):
    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> ExecutionRevisionSnapshot: ...


class RevisionExecutionSnapshotProvider:
    """Adapt the Revision Binder to the execution snapshot interface."""

    def __init__(self, binder: RevisionBinder | None = None) -> None:
        self._binder = binder or RevisionBinder()

    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> ExecutionRevisionSnapshot:
        return ExecutionRevisionSnapshot.from_revision(
            self._binder.bind(repo_root, assessment_output_paths)
        )


@dataclass(slots=True)
class _CapturedStream:
    stored: bytes
    observed_count: int
    observed_digest: str
    error: BaseException | None = None


class LocalExecutionRunner:
    """Run only configured local argv templates and produce fail-closed evidence."""

    def __init__(
        self,
        repo_root: Path,
        artifact_directory: Path,
        *,
        snapshot_provider: ExecutionSnapshotProvider | None = None,
    ) -> None:
        root = repo_root.expanduser().resolve(strict=True)
        if not root.is_dir():
            raise ValueError("repository root must be a directory")
        artifact = artifact_directory.expanduser()
        if not artifact.is_absolute():
            artifact = root / artifact
        if artifact == root:
            raise ValueError("execution artifact directory must not be the repository root")
        if artifact.is_symlink():
            raise ValueError("execution artifact directory must not be a symbolic link")
        self._root = root
        self._artifact_directory = artifact
        self._snapshot_provider = snapshot_provider or RevisionExecutionSnapshotProvider()

    def execute(self, policy: ExecutionPolicy, command_id: str) -> ExecutionEvidence:
        if not isinstance(policy, ExecutionPolicy):
            raise TypeError("policy must be ExecutionPolicy")
        _text("command_id", command_id)
        if not policy.enabled:
            return self._not_run(command_id, ExecutionOutcome.DISABLED, "local execution is disabled")
        command = policy.command(command_id)
        if command is None:
            return self._not_run(
                command_id, ExecutionOutcome.NOT_ALLOWLISTED,
                "command ID is not present in the explicit local allowlist",
            )

        platform_summary = _platform_summary()
        environment, environment_summary = _controlled_environment()
        executable, executable_error = self._resolve_executable(command.executable)
        argv = (str(executable or command.executable), *command.arguments)
        argv_digest = _sequence_digest(argv)
        redacted = command.redacted_command(executable or command.executable)
        before, snapshot_error = self._snapshot()
        if snapshot_error is not None:
            return self._result(
                command_id=command_id, argv_digest=argv_digest, redacted=redacted,
                platform_summary=platform_summary, environment_summary=environment_summary,
                outcome=ExecutionOutcome.SNAPSHOT_FAILED, state=ExecutionState.FAILED,
                validation=ExecutionValidationState.INVALIDATED, detail=snapshot_error,
            )

        capture_stdout: _CapturedStream | None = None
        capture_stderr: _CapturedStream | None = None
        exit_status: int | None = None
        if platform_summary.system not in command.supported_platforms:
            outcome = ExecutionOutcome.PLATFORM_UNAVAILABLE
            state = ExecutionState.UNAVAILABLE
            detail = f"platform {platform_summary.system!r} is not configured for this command"
        elif executable_error is not None:
            outcome, state, detail = executable_error
        else:
            assert executable is not None
            outcome, exit_status, capture_stdout, capture_stderr, detail = self._run(
                executable, command.arguments, environment, command.timeout_seconds,
                policy.max_artifact_bytes,
            )
            state = (
                ExecutionState.VALIDATED
                if outcome is ExecutionOutcome.SUCCEEDED
                else ExecutionState.FAILED
            )

        after, after_error = self._snapshot()
        validation = (
            ExecutionValidationState.VALIDATED
            if outcome is ExecutionOutcome.SUCCEEDED
            else ExecutionValidationState.FAILED
        )
        if after_error is not None:
            outcome = ExecutionOutcome.SNAPSHOT_FAILED
            state = ExecutionState.FAILED
            validation = ExecutionValidationState.INVALIDATED
            detail = after_error
        elif before != after:
            outcome = ExecutionOutcome.FINGERPRINT_DRIFT
            state = ExecutionState.FAILED
            validation = ExecutionValidationState.INVALIDATED
            detail = "repository revision or worktree fingerprint changed during execution"

        evidence = self._result(
            command_id=command_id, argv_digest=argv_digest, redacted=redacted,
            platform_summary=platform_summary, environment_summary=environment_summary,
            outcome=outcome, state=state, validation=validation, exit_status=exit_status,
            before=before, after=after, stdout_capture=capture_stdout,
            stderr_capture=capture_stderr, detail=detail,
        )
        if capture_stdout is None or capture_stderr is None:
            return evidence
        try:
            self._persist_artifacts(evidence, capture_stdout.stored, capture_stderr.stored)
        except OSError as error:
            return self._result(
                command_id=command_id, argv_digest=argv_digest, redacted=redacted,
                platform_summary=platform_summary, environment_summary=environment_summary,
                outcome=ExecutionOutcome.ARTIFACT_FAILED, state=ExecutionState.FAILED,
                validation=ExecutionValidationState.INVALIDATED, exit_status=exit_status,
                before=before, after=after,
                detail=f"execution artifacts could not be persisted: {error}",
            )
        return evidence

    def _snapshot(self) -> tuple[ExecutionRevisionSnapshot | None, str | None]:
        try:
            value = self._snapshot_provider.capture(
                self._root, (self._artifact_directory,)
            )
            if not isinstance(value, ExecutionRevisionSnapshot):
                raise TypeError("snapshot provider returned an invalid model")
            return value, None
        except RevisionBindingError as error:
            return None, f"{error.code}: {error.message}"
        except (OSError, TypeError, ValueError) as error:
            return None, f"execution snapshot failed: {error}"

    @staticmethod
    def _resolve_executable(
        configured: Path,
    ) -> tuple[Path | None, tuple[ExecutionOutcome, ExecutionState, str] | None]:
        try:
            executable = configured.resolve(strict=True)
            metadata = executable.stat()
        except FileNotFoundError:
            return None, (
                ExecutionOutcome.EXECUTABLE_UNAVAILABLE, ExecutionState.UNAVAILABLE,
                "configured executable is unavailable",
            )
        except OSError as error:
            return None, (
                ExecutionOutcome.EXECUTION_FAILED, ExecutionState.FAILED,
                f"configured executable could not be inspected: {error}",
            )
        if not stat.S_ISREG(metadata.st_mode) or not os.access(executable, os.X_OK):
            return None, (
                ExecutionOutcome.EXECUTION_FAILED, ExecutionState.FAILED,
                "configured executable is not an executable regular file",
            )
        return executable, None


    def _run(
        self,
        executable: Path,
        arguments: tuple[str, ...],
        environment: dict[str, str],
        timeout_seconds: float,
        artifact_limit: int,
    ) -> tuple[
        ExecutionOutcome, int | None, _CapturedStream | None, _CapturedStream | None, str
    ]:
        creation_flags = 0
        if os.name == "nt":
            creation_flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        try:
            process = subprocess.Popen(
                (str(executable), *arguments),
                cwd=self._root,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                shell=False,
                close_fds=True,
                start_new_session=(os.name == "posix"),
                creationflags=creation_flags,
            )
        except FileNotFoundError:
            return (
                ExecutionOutcome.EXECUTABLE_UNAVAILABLE, None, None, None,
                "configured executable became unavailable before process creation",
            )
        except OSError as error:
            return ExecutionOutcome.EXECUTION_FAILED, None, None, None, f"process creation failed: {error}"

        assert process.stdout is not None and process.stderr is not None
        stdout_box = [_CapturedStream(b"", 0, hashlib.sha256().hexdigest())]
        stderr_box = [_CapturedStream(b"", 0, hashlib.sha256().hexdigest())]
        readers = (
            threading.Thread(
                target=_drain_stream,
                args=(process.stdout, artifact_limit, stdout_box),
                name="gap-analysis-stdout",
                daemon=True,
            ),
            threading.Thread(
                target=_drain_stream,
                args=(process.stderr, artifact_limit, stderr_box),
                name="gap-analysis-stderr",
                daemon=True,
            ),
        )
        for reader in readers:
            reader.start()
        timed_out = False
        try:
            process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            self._terminate(process)
        finally:
            for reader in readers:
                reader.join(_READER_JOIN_SECONDS)
            for stream in (process.stdout, process.stderr):
                if not stream.closed:
                    stream.close()
            for reader in readers:
                reader.join(_READER_JOIN_SECONDS)

        stdout_capture, stderr_capture = stdout_box[0], stderr_box[0]
        if any(reader.is_alive() for reader in readers):
            return (
                ExecutionOutcome.EXECUTION_FAILED, process.returncode,
                stdout_capture, stderr_capture,
                "output capture did not terminate after the local process ended",
            )
        stream_error = stdout_capture.error or stderr_capture.error
        if stream_error is not None:
            return (
                ExecutionOutcome.EXECUTION_FAILED, process.returncode,
                stdout_capture, stderr_capture, f"output capture failed: {stream_error}",
            )
        if timed_out:
            return (
                ExecutionOutcome.TIMED_OUT, process.returncode,
                stdout_capture, stderr_capture,
                f"local command exceeded its {timeout_seconds:g} second timeout",
            )
        if process.returncode == 0:
            return (
                ExecutionOutcome.SUCCEEDED, 0, stdout_capture, stderr_capture,
                "allowlisted local command completed successfully",
            )
        return (
            ExecutionOutcome.NONZERO_EXIT, process.returncode,
            stdout_capture, stderr_capture,
            f"allowlisted local command exited with status {process.returncode}",
        )

    @staticmethod
    def _terminate(process: subprocess.Popen[bytes]) -> None:
        try:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=_READER_JOIN_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def _not_run(
        self, command_id: str, outcome: ExecutionOutcome, detail: str
    ) -> ExecutionEvidence:
        return self._result(
            command_id=command_id, argv_digest=None, redacted=(),
            platform_summary=None, environment_summary=None, outcome=outcome,
            state=ExecutionState.NOT_RUN,
            validation=ExecutionValidationState.NOT_RUN, detail=detail,
        )

    def _result(
        self,
        *,
        command_id: str,
        argv_digest: str | None,
        redacted: tuple[str, ...],
        platform_summary: PlatformSummary | None,
        environment_summary: EnvironmentSummary | None,
        outcome: ExecutionOutcome,
        state: ExecutionState,
        validation: ExecutionValidationState,
        detail: str,
        exit_status: int | None = None,
        before: ExecutionRevisionSnapshot | None = None,
        after: ExecutionRevisionSnapshot | None = None,
        stdout_capture: _CapturedStream | None = None,
        stderr_capture: _CapturedStream | None = None,
    ) -> ExecutionEvidence:
        identity = stable_id(
            "execution-evidence", command_id, argv_digest or "", outcome.value,
            exit_status, before.worktree_fingerprint if before else "",
            after.worktree_fingerprint if after else "",
            stdout_capture.observed_digest if stdout_capture else "",
            stderr_capture.observed_digest if stderr_capture else "",
        )
        stdout_ref = _artifact_reference(identity, "stdout", stdout_capture)
        stderr_ref = _artifact_reference(identity, "stderr", stderr_capture)
        return ExecutionEvidence(
            id=identity, command_id=command_id, argv_digest=argv_digest,
            redacted_command=redacted, platform=platform_summary,
            environment=environment_summary, outcome=outcome, execution_state=state,
            validation_state=validation, exit_status=exit_status,
            stdout_artifact=stdout_ref, stderr_artifact=stderr_ref,
            before_revision=before, after_revision=after, detail=detail,
        )

    def _persist_artifacts(
        self, evidence: ExecutionEvidence, stdout: bytes, stderr: bytes
    ) -> None:
        assert evidence.stdout_artifact is not None and evidence.stderr_artifact is not None
        execution_directory = self._artifact_directory / "executions"
        execution_directory.mkdir(parents=True, exist_ok=True)
        if execution_directory.is_symlink() or not execution_directory.is_dir():
            raise OSError("execution artifact location is not a real directory")
        written: list[Path] = []
        try:
            for reference, content in (
                (evidence.stdout_artifact, stdout), (evidence.stderr_artifact, stderr)
            ):
                destination = self._artifact_directory / reference.path
                temporary = destination.with_name(f".{destination.name}.{os.getpid()}.tmp")
                with temporary.open("xb") as output:
                    output.write(content)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, destination)
                written.append(destination)
        except OSError:
            for path in written:
                try:
                    path.unlink()
                except OSError:
                    pass
            raise


def execute_local_command(
    repo_root: Path,
    artifact_directory: Path,
    policy: ExecutionPolicy,
    command_id: str,
    *,
    snapshot_provider: ExecutionSnapshotProvider | None = None,
) -> ExecutionEvidence:
    """Convenience API for one explicitly configured local command."""

    return LocalExecutionRunner(
        repo_root, artifact_directory, snapshot_provider=snapshot_provider
    ).execute(policy, command_id)


def _drain_stream(
    stream: BinaryIO, limit: int, result_box: list[_CapturedStream]
) -> None:
    digest = hashlib.sha256()
    stored = bytearray()
    observed = 0
    error: BaseException | None = None
    try:
        while True:
            chunk = stream.read(_READ_CHUNK_BYTES)
            if not chunk:
                break
            digest.update(chunk)
            observed += len(chunk)
            remaining = limit - len(stored)
            if remaining > 0:
                stored.extend(chunk[:remaining])
    except (OSError, ValueError) as caught:
        error = caught
    finally:
        try:
            stream.close()
        except OSError as caught:
            error = error or caught
        result_box[0] = _CapturedStream(bytes(stored), observed, digest.hexdigest(), error)


def _artifact_reference(
    identity: StableId, stream: str, capture: _CapturedStream | None
) -> ExecutionArtifactReference | None:
    if capture is None:
        return None
    stored_digest = hashlib.sha256(capture.stored).hexdigest()
    return ExecutionArtifactReference(
        path=f"executions/{identity}.{stream}",
        sha256=stored_digest,
        observed_stream_sha256=capture.observed_digest,
        byte_count=len(capture.stored),
        observed_byte_count=capture.observed_count,
        truncated=len(capture.stored) < capture.observed_count,
    )


def _controlled_environment() -> tuple[dict[str, str], EnvironmentSummary]:
    environment = {
        "HOME": "",
        "LANG": "C",
        "LC_ALL": "C",
        "PATH": "",
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONNOUSERSITE": "1",
        "TZ": "UTC",
    }
    inherited: list[str] = []
    if os.name == "nt":
        for name in ("SystemRoot", "WINDIR"):
            value = os.environ.get(name)
            if value:
                environment[name] = value
                inherited.append(name)
    flattened = tuple(f"{key}={environment[key]}" for key in sorted(environment))
    return environment, EnvironmentSummary(
        variable_names=tuple(environment),
        inherited_variable_names=tuple(inherited),
        environment_digest=_sequence_digest(flattened),
    )


def _platform_summary() -> PlatformSummary:
    return PlatformSummary(
        system=platform.system() or "Unknown",
        release=platform.release() or "Unknown",
        machine=platform.machine() or "Unknown",
        python_implementation=platform.python_implementation() or "Unknown",
    )


def _sequence_digest(values: Sequence[str]) -> str:
    digest = hashlib.sha256()
    for value in values:
        encoded = _utf8(value, "digest value")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


def _utf8(value: str, name: str) -> bytes:
    try:
        return value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError(f"{name} must be valid UTF-8 text") from error


def _text(name: str, value: object, *, allow_empty: bool = False) -> None:
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        qualifier = "text" if allow_empty else "non-empty text"
        raise ValueError(f"{name} must be {qualifier}")


def _unique_texts(values: Sequence[str]) -> tuple[str, ...]:
    normalized = tuple(sorted(set(values)))
    for value in normalized:
        _text("collection item", value)
    return normalized


def _digest(name: str, value: str) -> None:
    if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
        raise ValueError(f"{name} must be a lowercase SHA-256 digest")
