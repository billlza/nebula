from __future__ import annotations

import json
import math
import os
import signal
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, NoReturn, Protocol, Sequence

from process_containment import (
    BoundedOutputCollector,
    InvocationError,
    LongLivedProcess,
    ProcessContainmentPolicy,
    start_long_lived_process,
)


MAX_PENDING_OBSERVE_RECORD_BYTES = 1024 * 1024
OBSERVE_FILE_READ_CHUNK_BYTES = 64 * 1024
SERVICE_PROBE_MAX_CAPTURED_OUTPUT_BYTES = 1024 * 1024
SERVICE_PROBE_POLL_SECONDS = 0.05
ServiceWaitTimeoutMode = Literal["service-start", "exact"]


@dataclass
class _ServiceLaunchSlot:
    owner: LongLivedProcess | None = None
    pending_signal: int | None = None


class _ServiceLaunchInterrupted(Exception):
    def __init__(self, signum: int, original_handler: Any):
        super().__init__(f"service launch was interrupted by signal {signum}")
        self.signum = signum
        self.original_handler = original_handler


_active_service_owners: tuple[LongLivedProcess, ...] = ()
_service_launch_slot: _ServiceLaunchSlot | None = None
_original_service_signal_handlers: dict[int, Any] = {}
_service_signal_handlers_installed = False
_service_cancel_failures: dict[int, list[str]] = {}


def _require_service_owner_main_thread(operation: str) -> None:
    if threading.current_thread() is not threading.main_thread():
        raise ServiceProbeSetupError(
            f"service {operation} must run on the Python main thread"
        )


def _record_cancel_failure(owner: LongLivedProcess, detail: str) -> None:
    _service_cancel_failures.setdefault(id(owner), []).append(detail)


def _cancel_service_owner(owner: LongLivedProcess) -> None:
    try:
        owner.cancel()
    except BaseException as exc:
        _record_cancel_failure(owner, f"{type(exc).__name__}: {exc}")


def _cancel_registered_services() -> None:
    owners = list(reversed(_active_service_owners))
    slot = _service_launch_slot
    if slot is not None and slot.owner is not None and slot.owner not in owners:
        owners.append(slot.owner)
    for owner in owners:
        _cancel_service_owner(owner)


def _write_signal_cancel_failures() -> None:
    if not _service_cancel_failures:
        return
    details = [
        item
        for failures in _service_cancel_failures.values()
        for item in failures
    ]
    message = (
        "nebula: service cancellation failed during parent signal: "
        + "; ".join(details)
        + "\n"
    ).encode("utf-8", errors="replace")[:4096]
    try:
        os.write(2, message)
    except OSError:
        pass


def _deliver_original_service_signal(
    signum: int,
    frame: Any,
    original_handler: Any,
) -> None:
    if original_handler == signal.SIG_IGN:
        return
    if original_handler == signal.SIG_DFL:
        _write_signal_cancel_failures()
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)
        os._exit(128 + signum)
    if not callable(original_handler):
        raise ServiceProbeCleanupError(
            f"unsupported original signal handler for signal {signum}"
        )
    original_handler(signum, frame)


def _managed_service_signal(signum: int, frame: Any) -> None:
    slot = _service_launch_slot
    if slot is not None and slot.pending_signal is None:
        slot.pending_signal = signum
    _cancel_registered_services()
    if slot is not None:
        return
    original_handler = _original_service_signal_handlers.get(
        signum, signal.SIG_DFL
    )
    _deliver_original_service_signal(signum, frame, original_handler)


def _install_service_signal_handlers() -> None:
    global _service_signal_handlers_installed
    if os.name == "nt" or _service_signal_handlers_installed:
        return
    installed: list[int] = []
    try:
        for signum in (signal.SIGTERM, signal.SIGINT):
            _original_service_signal_handlers[signum] = signal.getsignal(signum)
            signal.signal(signum, _managed_service_signal)
            installed.append(signum)
    except BaseException:
        for signum in reversed(installed):
            signal.signal(
                signum, _original_service_signal_handlers[signum]
            )
        _original_service_signal_handlers.clear()
        raise
    _service_signal_handlers_installed = True


def _restore_service_signal_handlers_if_idle() -> str:
    global _service_signal_handlers_installed
    if (
        os.name == "nt"
        or not _service_signal_handlers_installed
        or _active_service_owners
        or _service_launch_slot is not None
    ):
        return ""
    errors: list[str] = []
    restored_or_replaced: list[int] = []
    for signum, original_handler in tuple(
        _original_service_signal_handlers.items()
    ):
        current_handler = signal.getsignal(signum)
        if current_handler is not _managed_service_signal:
            errors.append(f"signal {signum} handler changed while services were active")
            restored_or_replaced.append(signum)
            continue
        try:
            signal.signal(signum, original_handler)
        except Exception as exc:
            errors.append(
                f"could not restore signal {signum} handler: "
                f"{type(exc).__name__}: {exc}"
            )
            continue
        restored_or_replaced.append(signum)
    for signum in restored_or_replaced:
        del _original_service_signal_handlers[signum]
    _service_signal_handlers_installed = bool(
        _original_service_signal_handlers
    )
    return "; ".join(errors)


def _begin_service_launch() -> _ServiceLaunchSlot | None:
    global _service_launch_slot
    if os.name == "nt":
        return None
    _require_service_owner_main_thread("launch")
    if _service_launch_slot is not None:
        raise ServiceProbeSetupError("a service launch transaction is already active")
    if _service_signal_handlers_installed and not _active_service_owners:
        restore_detail = _restore_service_signal_handlers_if_idle()
        if restore_detail:
            raise ServiceProbeSetupError(
                "previous service signal-handler restoration remains incomplete: "
                + restore_detail
            )
    _install_service_signal_handlers()
    slot = _ServiceLaunchSlot()
    _service_launch_slot = slot
    return slot


def _release_service_owner(owner: LongLivedProcess) -> str:
    global _active_service_owners
    if os.name == "nt":
        return ""
    _require_service_owner_main_thread("cleanup")
    if owner not in _active_service_owners:
        return "service owner was not registered for signal cleanup"
    _active_service_owners = tuple(
        active for active in _active_service_owners if active is not owner
    )
    failures = _service_cancel_failures.pop(id(owner), [])
    restore_detail = _restore_service_signal_handlers_if_idle()
    details = [*failures]
    if restore_detail:
        details.append(restore_detail)
    return "; ".join(details)


def _abort_service_launch(
    slot: _ServiceLaunchSlot | None,
) -> tuple[_ServiceLaunchInterrupted | None, str]:
    global _service_launch_slot
    if slot is None or _service_launch_slot is not slot:
        return None, ""
    _service_launch_slot = None
    pending_signal = slot.pending_signal
    original_handler = (
        _original_service_signal_handlers.get(pending_signal, signal.SIG_DFL)
        if pending_signal is not None
        else None
    )
    restore_detail = _restore_service_signal_handlers_if_idle()
    interruption = (
        _ServiceLaunchInterrupted(pending_signal, original_handler)
        if pending_signal is not None
        else None
    )
    return interruption, restore_detail


def _complete_service_launch(
    slot: _ServiceLaunchSlot | None,
    owner: LongLivedProcess,
) -> None:
    global _active_service_owners, _service_launch_slot
    if slot is None:
        return
    slot.owner = owner
    _active_service_owners = (*_active_service_owners, owner)
    _service_launch_slot = None
    pending_signal = slot.pending_signal
    if pending_signal is None:
        return
    original_handler = _original_service_signal_handlers.get(
        pending_signal, signal.SIG_DFL
    )
    raise _ServiceLaunchInterrupted(
        pending_signal,
        original_handler,
    )


class PollableProcess(Protocol):
    returncode: int | None

    def poll(self) -> int | None:
        ...


class ServiceProbeError(RuntimeError):
    pass


class ServiceProbeSetupError(ServiceProbeError):
    pass


class ServiceProbeWaitError(ServiceProbeError):
    pass


class ServiceProbeCleanupError(ServiceProbeError):
    def __init__(self, message: str, *, stdout: str = "", stderr: str = ""):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr


class ObserveEventMonitor:
    """Incrementally retain one structured observe event from a byte stream."""

    def __init__(self, event_name: str):
        if not event_name:
            raise ValueError("observe event name must not be empty")
        self._event_name = event_name
        self._pending = bytearray()
        self._payload: dict[str, Any] | None = None
        self._failure = ""
        self._lock = threading.Lock()

    def feed(self, chunk: bytes) -> None:
        with self._lock:
            if self._payload is not None or self._failure:
                return
            self._pending.extend(chunk)
            while True:
                newline_index = self._pending.find(b"\n")
                if newline_index < 0:
                    break
                if newline_index > MAX_PENDING_OBSERVE_RECORD_BYTES:
                    self._failure = (
                        "observe record exceeded "
                        f"{MAX_PENDING_OBSERVE_RECORD_BYTES} bytes"
                    )
                    return
                record = bytes(self._pending[:newline_index])
                del self._pending[: newline_index + 1]
                for payload in collect_observe_events(
                    record.decode("utf-8", errors="replace")
                ):
                    if payload.get("event") == self._event_name:
                        self._payload = payload
                        return
            if len(self._pending) > MAX_PENDING_OBSERVE_RECORD_BYTES:
                self._failure = (
                    "observe record exceeded "
                    f"{MAX_PENDING_OBSERVE_RECORD_BYTES} bytes without a newline"
                )

    def payload(self) -> dict[str, Any] | None:
        with self._lock:
            return None if self._payload is None else dict(self._payload)

    def failure(self) -> str:
        with self._lock:
            return self._failure


@dataclass
class ServiceProbeOutput:
    """Bind an observe monitor to one group/Job-contained service and both pipes."""

    process: LongLivedProcess
    monitor: ObserveEventMonitor
    event_name: str
    _signal_registration_released: bool = False

    @property
    def stdout(self) -> BoundedOutputCollector:
        return self.process.stdout_collector

    @property
    def stderr(self) -> BoundedOutputCollector:
        return self.process.stderr_collector

    def snapshots(self) -> tuple[str, str]:
        return self.process.snapshots()

    def finish(self) -> tuple[str, str]:
        if os.name != "nt":
            _require_service_owner_main_thread("cleanup")
        primary_error: InvocationError | None = None
        try:
            result = self.process.finish()
        except InvocationError as exc:
            primary_error = exc
            result = self.snapshots()

        release_detail = ""
        if (
            not self._signal_registration_released
            and (primary_error is None or self.process.containment_sealed)
        ):
            try:
                release_detail = _release_service_owner(self.process)
            except ServiceProbeError as exc:
                release_detail = str(exc)
            else:
                self._signal_registration_released = True
        elif self._signal_registration_released and os.name != "nt":
            release_detail = _restore_service_signal_handlers_if_idle()
        if primary_error is None and not release_detail:
            return result

        details: list[str] = []
        if primary_error is not None:
            details.append(f"failed to finish contained service: {primary_error}")
        if release_detail:
            details.append(f"service signal-owner cleanup failed: {release_detail}")
        if primary_error is not None and not self.process.containment_sealed:
            details.append("service containment remains owned for an explicit cleanup retry")
        stdout, stderr = self.snapshots()
        raise ServiceProbeCleanupError(
            "; ".join(details),
            stdout=stdout,
            stderr=stderr,
        ) from primary_error


def start_process(
    cmd: Sequence[str | os.PathLike[str]],
    event_name: str,
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    max_captured_bytes: int = SERVICE_PROBE_MAX_CAPTURED_OUTPUT_BYTES,
    thread_name_prefix: str = "nebula-service-probe",
) -> tuple[LongLivedProcess, ServiceProbeOutput]:
    """Launch a service whose containment domain and binary pipes have one owner."""

    slot = _begin_service_launch()
    process: LongLivedProcess | None = None
    try:
        monitor = ObserveEventMonitor(event_name)
        process = start_long_lived_process(
            cmd,
            containment_policy=(
                ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
                if os.name == "nt"
                else ProcessContainmentPolicy.TRUSTED_COOPERATIVE
            ),
            cwd=cwd,
            env=env,
            max_captured_bytes=max_captured_bytes,
            thread_name_prefix=thread_name_prefix,
            stderr_chunk_observer=monitor.feed,
        )
        if slot is not None:
            slot.owner = process
        output = ServiceProbeOutput(
            process=process,
            monitor=monitor,
            event_name=event_name,
        )
        _complete_service_launch(slot, process)
        return process, output
    except BaseException as exc:
        interrupted = (
            exc if isinstance(exc, _ServiceLaunchInterrupted) else None
        )
        rollback_errors: list[str] = []
        if process is not None:
            try:
                process.cancel()
            except BaseException as cleanup_exc:
                rollback_errors.append(
                    "service process cancellation failed: "
                    f"{type(cleanup_exc).__name__}: {cleanup_exc}"
                )
            try:
                process.finish()
            except BaseException as cleanup_exc:
                rollback_errors.append(
                    "service process convergence failed: "
                    f"{type(cleanup_exc).__name__}: {cleanup_exc}"
                )

            if process.containment_sealed and process in _active_service_owners:
                try:
                    release_detail = _release_service_owner(process)
                except BaseException as cleanup_exc:
                    rollback_errors.append(
                        "service signal-owner rollback failed: "
                        f"{type(cleanup_exc).__name__}: {cleanup_exc}"
                    )
                else:
                    if release_detail:
                        rollback_errors.append(
                            "service signal-owner rollback failed: "
                            + release_detail
                        )

        ownership_retained = (
            process is not None and not process.containment_sealed
        )
        if ownership_retained:
            rollback_errors.append(
                "service containment remains owned by the failed launch transaction"
            )
        else:
            try:
                abort_interruption, abort_detail = _abort_service_launch(slot)
            except BaseException as cleanup_exc:
                rollback_errors.append(
                    "service launch rollback failed: "
                    f"{type(cleanup_exc).__name__}: {cleanup_exc}"
                )
            else:
                if abort_detail:
                    rollback_errors.append(
                        "service launch signal-handler restoration failed: "
                        + abort_detail
                    )
                if abort_interruption is not None:
                    if interrupted is not None:
                        rollback_errors.append(
                            "service launch observed multiple pending signal states"
                        )
                    else:
                        interrupted = abort_interruption
        if interrupted is not None:
            if ownership_retained:
                rollback_errors.append(
                    "original signal delivery was suppressed because service "
                    "containment did not converge"
                )
            else:
                if rollback_errors and interrupted.original_handler == signal.SIG_DFL:
                    try:
                        os.write(
                            2,
                            (
                                "nebula: interrupted service launch cleanup failed: "
                                + "; ".join(rollback_errors)
                                + "\n"
                            ).encode("utf-8", errors="replace")[:4096],
                        )
                    except OSError:
                        pass
                try:
                    _deliver_original_service_signal(
                        interrupted.signum,
                        None,
                        interrupted.original_handler,
                    )
                except BaseException as signal_exc:
                    replay_context = (
                        "service launch state before original signal delivery: "
                        f"{type(exc).__name__}: {exc}"
                    )
                    if rollback_errors:
                        replay_context += "; cleanup failed: " + "; ".join(
                            rollback_errors
                        )
                    signal_exc.add_note(replay_context)
                    raise
        if not isinstance(exc, Exception):
            if rollback_errors:
                exc.add_note("; ".join(rollback_errors))
            raise
        if isinstance(exc, ServiceProbeError) and not rollback_errors:
            raise
        detail = f"failed to start contained service: {exc}"
        if rollback_errors:
            detail += "; " + "; ".join(rollback_errors)
        raise ServiceProbeSetupError(
            detail
        ) from exc


def collect_observe_events(text: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        json_start = line.find("{")
        if json_start < 0:
            continue
        try:
            payload = json.loads(line[json_start:])
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            events.append(payload)
    return events


def finish_process_output(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
) -> tuple[str, str]:
    """Seal/reap the owned service group or Job, then finish both pipe drains."""

    if output.process is not proc:
        raise ServiceProbeCleanupError(
            "service process/output ownership mismatch",
            stdout=output.snapshots()[0],
            stderr=output.snapshots()[1],
        )
    return output.finish()


def _print_process_output(stdout: str, stderr: str) -> None:
    print(stdout, end="" if not stdout or stdout.endswith("\n") else "\n")
    print(stderr, end="" if not stderr or stderr.endswith("\n") else "\n")


def terminate_process(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
) -> None:
    """Finish a captured service, preserving an active primary exception."""

    active_exception = sys.exception()
    try:
        finish_process_output(proc, output)
    except ServiceProbeCleanupError as exc:
        if active_exception is None:
            raise
        active_exception.add_note(f"service cleanup failed: {exc}")
        _print_process_output(exc.stdout, exc.stderr)


def fail_with_output(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
    message: str,
) -> NoReturn:
    try:
        stdout, stderr = finish_process_output(proc, output)
    except ServiceProbeCleanupError as exc:
        stdout = exc.stdout
        stderr = exc.stderr
        message += f"; service cleanup failed: {exc}"
    _print_process_output(stdout, stderr)
    raise SystemExit(message)


def _raise_finished_collector_failure(
    collector: BoundedOutputCollector,
    label: str,
) -> None:
    if not collector.is_finished():
        return
    try:
        collector.finish()
    except InvocationError as exc:
        raise ServiceProbeWaitError(
            f"service {label} output drain failed: {exc}"
        ) from exc


def _validated_service_timeout(requested: float) -> float:
    if (
        isinstance(requested, bool)
        or not isinstance(requested, (int, float))
        or not math.isfinite(requested)
        or requested <= 0
    ):
        raise ValueError("requested service timeout must be a finite positive number")
    return float(requested)


def _service_wait_timeout(
    requested: float,
    timeout_mode: ServiceWaitTimeoutMode,
) -> float:
    if timeout_mode == "service-start":
        return service_start_timeout(requested)
    if timeout_mode == "exact":
        return _validated_service_timeout(requested)
    raise ValueError("service wait timeout_mode must be 'service-start' or 'exact'")


def await_observe_event(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
    event_name: str,
    timeout: float,
    *,
    timeout_mode: ServiceWaitTimeoutMode = "service-start",
) -> dict[str, Any]:
    """Wait without stealing an owned pipe or mutating process state.

    ``service-start`` preserves the configurable CI startup floor. ``exact``
    uses the caller's finite positive deadline verbatim for already-built
    processes whose startup budget must not be widened.
    """

    if output.event_name != event_name:
        raise ValueError(
            "service output capture observes "
            f"{output.event_name!r}, not requested event {event_name!r}"
        )
    deadline = time.monotonic() + _service_wait_timeout(timeout, timeout_mode)
    while True:
        monitor_failure = output.monitor.failure()
        if monitor_failure:
            raise ServiceProbeWaitError(
                f"invalid {event_name} observe stream: {monitor_failure}"
            )

        _raise_finished_collector_failure(output.stdout, "stdout")
        stderr_finished = output.stderr.is_finished()
        if stderr_finished:
            _raise_finished_collector_failure(output.stderr, "stderr")

        payload = output.monitor.payload()
        if payload is not None:
            if proc.poll() is not None:
                raise ServiceProbeWaitError(
                    f"service exited while publishing {event_name}: "
                    f"rc={proc.returncode}"
                )
            return payload

        if stderr_finished:
            raise ServiceProbeWaitError(
                f"service stderr closed before {event_name} appeared: "
                f"rc={proc.poll()}"
            )

        if proc.poll() is not None:
            raise ServiceProbeWaitError(
                f"service exited early while waiting for {event_name}: "
                f"rc={proc.returncode}"
            )

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ServiceProbeWaitError(f"timed out waiting for {event_name}")
        time.sleep(min(SERVICE_PROBE_POLL_SECONDS, remaining))


def wait_for_observe_event(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
    event_name: str,
    timeout: float,
    *,
    timeout_mode: ServiceWaitTimeoutMode = "service-start",
) -> dict[str, Any]:
    try:
        return await_observe_event(
            proc,
            output,
            event_name,
            timeout,
            timeout_mode=timeout_mode,
        )
    except ServiceProbeWaitError as exc:
        fail_with_output(proc, output, str(exc))


def await_listener_bound(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
    timeout: float = 10.0,
    *,
    timeout_mode: ServiceWaitTimeoutMode = "service-start",
) -> tuple[dict[str, Any], int]:
    payload = await_observe_event(
        proc,
        output,
        "listener_bound",
        timeout,
        timeout_mode=timeout_mode,
    )
    port = payload.get("port")
    if type(port) is not int or port < 1 or port > 65535:
        raise ServiceProbeWaitError(
            f"invalid listener_bound port payload: {payload!r}"
        )
    return payload, port


def wait_for_listener_bound(
    proc: LongLivedProcess,
    output: ServiceProbeOutput,
    timeout: float = 10.0,
    *,
    timeout_mode: ServiceWaitTimeoutMode = "service-start",
) -> tuple[dict[str, Any], int]:
    try:
        return await_listener_bound(
            proc,
            output,
            timeout,
            timeout_mode=timeout_mode,
        )
    except ServiceProbeWaitError as exc:
        fail_with_output(proc, output, str(exc))


def wait_for_observe_event_in_file(log_path: Path,
                                   event_name: str,
                                   timeout: float,
                                   proc: PollableProcess | None = None) -> dict[str, Any]:
    deadline = time.monotonic() + service_start_timeout(timeout)
    offset = 0
    pending = b""
    while time.monotonic() < deadline:
        process_exited = proc is not None and proc.poll() is not None
        drained_to_eof_after_exit = False
        if process_exited and not log_path.exists():
            raise SystemExit(f"process exited early before writing {event_name}: rc={proc.returncode}")
        if log_path.exists():
            with log_path.open("rb") as handle:
                handle.seek(offset)
                while True:
                    chunk = handle.read(OBSERVE_FILE_READ_CHUNK_BYTES)
                    offset = handle.tell()
                    if not chunk:
                        drained_to_eof_after_exit = process_exited
                        break
                    pending += chunk
                    while True:
                        newline_index = pending.find(b"\n")
                        if newline_index < 0:
                            break
                        record = pending[:newline_index]
                        pending = pending[newline_index + 1 :]
                        if len(record) > MAX_PENDING_OBSERVE_RECORD_BYTES:
                            raise SystemExit(
                                "observe record exceeded "
                                f"{MAX_PENDING_OBSERVE_RECORD_BYTES} bytes in {log_path}"
                            )
                        for payload in collect_observe_events(
                            record.decode("utf-8", errors="replace")
                        ):
                            if payload.get("event") == event_name:
                                return payload
                    if len(pending) > MAX_PENDING_OBSERVE_RECORD_BYTES:
                        raise SystemExit(
                            f"observe record exceeded {MAX_PENDING_OBSERVE_RECORD_BYTES} "
                            f"bytes without a newline in {log_path}"
                        )
                    if proc is None:
                        break
                    process_exited = proc.poll() is not None
                    if not process_exited:
                        break
        if proc is not None and (process_exited or proc.poll() is not None):
            if not drained_to_eof_after_exit:
                continue
            raise SystemExit(f"process exited before {event_name} appeared: rc={proc.returncode}")
        time.sleep(0.05)
    raise SystemExit(f"timed out waiting for {event_name} in {log_path}")


def service_start_timeout(requested: float) -> float:
    requested_seconds = _validated_service_timeout(requested)
    override = os.environ.get("NEBULA_SERVICE_START_TIMEOUT")
    if override:
        try:
            parsed_override = float(override)
        except ValueError as exc:
            raise ValueError(
                "NEBULA_SERVICE_START_TIMEOUT must be a finite positive number"
            ) from exc
        if not math.isfinite(parsed_override) or parsed_override <= 0:
            raise ValueError(
                "NEBULA_SERVICE_START_TIMEOUT must be a finite positive number"
            )
        return max(requested_seconds, parsed_override)
    # CI runners are far slower to build and bind a service than dev machines.
    # Linux already had a generous floor; macOS CI (e.g. macos-14) is just as slow
    # to first-build + bind, so server smokes there were flaky (the bare 10s
    # requested timeout fired before the listener came up). Apply the same floor
    # on every platform; it is a max, so fast machines that bind quickly are
    # unaffected.
    return max(requested_seconds, 90.0)
