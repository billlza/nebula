from __future__ import annotations

import ctypes
import errno
import math
import os
import platform
import secrets
import selectors
import signal
import stat
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import BinaryIO, Callable, Mapping, Sequence

if os.name != "nt":
    import fcntl


TIMEOUT_RETURN_CODE = 124
INFRASTRUCTURE_ERROR_RETURN_CODE = 125
PROCESS_TERMINATION_GRACE_SECONDS = 5.0
PROCESS_TERMINATION_POLL_SECONDS = 0.05
OUTPUT_DRAIN_TIMEOUT_SECONDS = 5.0
OUTPUT_DRAIN_ABORT_TIMEOUT_SECONDS = 2.0
OUTPUT_DRAIN_POLL_SECONDS = 0.05
WINDOWS_OUTPUT_DRAIN_POLL_SECONDS = 0.005
DEFAULT_MAX_CAPTURED_OUTPUT_BYTES = 8 * 1024 * 1024
DARWIN_PROCESS_AUDIT_MAX_PIDS = 256 * 1024
POSIX_DESCENDANT_AUDIT_INTERVAL_SECONDS = 0.1
POSIX_DESCENDANT_SIGNAL_POLL_SECONDS = 0.1
POSIX_EMPTY_AUDIT_CONFIRMATIONS = 2
POSIX_CONTAINMENT_TOKEN_ENV = "_NEBULA_POSIX_CONTAINMENT_TOKENS"
POSIX_CONTAINMENT_ANCHOR_FDS_ENV = "_NEBULA_POSIX_CONTAINMENT_ANCHOR_FDS"
POSIX_CONTAINMENT_TOKEN_HEX_CHARS = 64
POSIX_CONTAINMENT_TOKEN_STACK_MAX_DEPTH = 64
POSIX_CONTAINMENT_TOKEN_STACK_MAX_BYTES = 8192
POSIX_CONTAINMENT_ANCHOR_STACK_MAX_DEPTH = 64
POSIX_CONTAINMENT_ANCHOR_STACK_MAX_BYTES = 8192
POSIX_PROCESS_ENVIRONMENT_MAX_BYTES = 2 * 1024 * 1024
POSIX_EXEC_GATE_HANDSHAKE_SECONDS = 5.0
POSIX_EXEC_GATE_ERROR_MAX_BYTES = 4096
POSIX_EXEC_GATE_READY_STATUS = b"READY\n"
POSIX_EXEC_GATE_ERROR_PREFIX = b"ERROR "
DARWIN_PROCESS_INFO_AUDIT_ATTEMPTS = 3
DARWIN_PROCESS_FD_AUDIT_MAX_FDS = 64 * 1024


class InvocationError(RuntimeError):
    def __init__(self, message: str, *, output: str = "", timed_out: bool = False):
        super().__init__(message)
        self.output = output
        self.timed_out = timed_out


class _ProcessOwnershipError(InvocationError):
    pass


class _DescendantOwnershipError(InvocationError):
    """A descendant group is unsafe to signal; the unreaped leader remains owned."""


class _DarwinProcessInfoError(InvocationError):
    def __init__(self, pid: int, error_number: int):
        self.pid = pid
        self.error_number = error_number
        super().__init__(
            f"Darwin proc_pidinfo failed for pid {pid}: "
            f"[{error_number}] {os.strerror(error_number)}"
        )


class ProcessContainmentPolicy(str, Enum):
    """Strength requested from a process launch boundary."""

    TRUSTED_COOPERATIVE = "trusted-cooperative"
    OS_ENFORCED_RECURSIVE = "os-enforced-recursive"


@dataclass(frozen=True)
class _PosixProcessIdentity:
    pid: int
    start_token: tuple[int, int]


@dataclass(frozen=True)
class _PosixProcessRecord:
    identity: _PosixProcessIdentity
    parent_pid: int
    process_group_id: int
    session_id: int
    state: str

    @property
    def is_live(self) -> bool:
        return self.state != "Z"


@dataclass
class _PosixDescendantDomain:
    """Track one trusted-cooperative POSIX command domain.

    The outer command is launched as a new session leader.  Descendants may
    create additional process groups or sessions, so cleanup retains stable
    ``(pid, start_token)`` identities and only signals groups that still contain
    a freshly revalidated tracked member. Darwin native descendants are discoverable
    only when every repository-controlled spawn preserves the complete writer-FD
    anchor stack for the descendant's lifetime. Linux additionally uses the inherited
    token as supplemental discovery evidence. ``session_ids`` is historical audit
    context only; neither mechanism is an authorization boundary or hostile sandbox.
    """

    leader: _PosixProcessIdentity
    member_identities: set[_PosixProcessIdentity]
    session_ids: set[int]
    containment_token: str | None = None
    prelaunch_identities: set[_PosixProcessIdentity] = field(default_factory=set)
    anchor_read_fd: int = -1
    anchor_read_handle: int | None = None
    anchor_write_handle: int | None = None


@dataclass(frozen=True)
class ContainedCommandResult:
    """Completed command plus the strength and activity of its cleanup.

    ``cleanup_after_exit_performed`` reports that descendants or open output
    pipes required cleanup after the root command exited.  It is not a proof
    strength flag. ``containment_complete`` means the requested policy reached
    its terminal oracle: a Windows Job for OS-enforced recursive containment, or
    the explicit writer-lifetime contract for POSIX trusted-cooperative mode.
    """

    returncode: int
    stdout: str
    stderr: str
    timed_out: bool
    cleanup_after_exit_performed: bool
    containment_policy: ProcessContainmentPolicy
    containment_complete: bool


class BoundedOutputCollector:
    """Drain a binary pipe continuously while retaining a bounded head and tail."""

    def __init__(
        self,
        stream: BinaryIO,
        *,
        max_captured_bytes: int = DEFAULT_MAX_CAPTURED_OUTPUT_BYTES,
        thread_name: str = "nebula-output-drain",
        chunk_observer: Callable[[bytes], None] | None = None,
    ):
        if type(max_captured_bytes) is not int or max_captured_bytes < 1:
            raise ValueError("max_captured_bytes must be >= 1")
        self._stream = stream
        self._fd = stream.fileno()
        self._max_captured_bytes = max_captured_bytes
        self._chunk_observer = chunk_observer
        self._head_limit = (max_captured_bytes + 1) // 2
        self._tail_limit = max_captured_bytes - self._head_limit
        self._head = bytearray()
        self._tail = bytearray()
        self._total_bytes = 0
        self._read_error: BaseException | None = None
        self._close_errors: list[BaseException] = []
        self._lock = threading.Lock()
        self._stop_requested = threading.Event()
        self._finished = threading.Event()
        self._drain_entered = threading.Event()
        self._start_attempted = False
        self._started = False
        self._start_failed_before_thread = False
        if os.name != "nt":
            os.set_blocking(self._fd, False)
        self._thread = threading.Thread(
            target=self._drain,
            name=thread_name,
            daemon=True,
        )

    def start(self) -> BoundedOutputCollector:
        if self._start_attempted:
            raise RuntimeError("bounded output collector can only be started once")
        self._start_attempted = True
        try:
            self._thread.start()
        except BaseException as exc:
            self._stop_requested.set()
            if self._thread.ident is None:
                if isinstance(exc, Exception) or not self._drain_entered.wait(
                    OUTPUT_DRAIN_ABORT_TIMEOUT_SECONDS
                ):
                    self._start_failed_before_thread = True
            raise
        self._started = True
        return self

    def _retain(self, chunk: bytes) -> None:
        with self._lock:
            self._total_bytes += len(chunk)
            head_remaining = self._head_limit - len(self._head)
            if head_remaining > 0:
                head_chunk = chunk[:head_remaining]
                self._head.extend(head_chunk)
                chunk = chunk[len(head_chunk) :]
            if self._tail_limit == 0 or not chunk:
                return
            self._tail.extend(chunk)
            excess = len(self._tail) - self._tail_limit
            if excess > 0:
                del self._tail[:excess]

    def _drain(self) -> None:
        self._drain_entered.set()
        selector: selectors.BaseSelector | None = None
        try:
            if os.name != "nt":
                selector = selectors.DefaultSelector()
                selector.register(self._fd, selectors.EVENT_READ)
            while not self._stop_requested.is_set():
                if os.name == "nt":
                    chunk = _read_windows_pipe_chunk(self._fd)
                else:
                    assert selector is not None
                    if not selector.select(OUTPUT_DRAIN_POLL_SECONDS):
                        continue
                    try:
                        chunk = os.read(self._fd, 64 * 1024)
                    except BlockingIOError:
                        chunk = None
                if chunk is None:
                    self._stop_requested.wait(
                        WINDOWS_OUTPUT_DRAIN_POLL_SECONDS
                        if os.name == "nt"
                        else OUTPUT_DRAIN_POLL_SECONDS
                    )
                    continue
                if not chunk:
                    break
                self._retain(chunk)
                if self._chunk_observer is not None:
                    self._chunk_observer(chunk)
        except BaseException as exc:
            self._read_error = exc
        finally:
            if selector is not None:
                try:
                    selector.close()
                except BaseException as exc:
                    self._close_errors.append(exc)
            try:
                self._stream.close()
            except BaseException as exc:
                self._close_errors.append(exc)
            finally:
                self._finished.set()

    @property
    def total_bytes(self) -> int:
        with self._lock:
            return self._total_bytes

    @property
    def retained_bytes(self) -> int:
        with self._lock:
            return len(self._head) + len(self._tail)

    def is_finished(self) -> bool:
        return self._finished.is_set()

    def snapshot(self) -> str:
        with self._lock:
            head = bytes(self._head)
            tail = bytes(self._tail)
            total_bytes = self._total_bytes
        retained_bytes = len(head) + len(tail)
        if total_bytes == retained_bytes:
            return (head + tail).decode("utf-8", errors="replace")
        omitted_bytes = total_bytes - retained_bytes
        return (
            head.decode("utf-8", errors="replace")
            + "\n[nebula-output-truncated] omitted "
            + f"{omitted_bytes} of {total_bytes} bytes\n"
            + tail.decode("utf-8", errors="replace")
        )

    def abort(self) -> None:
        self._stop_requested.set()
        if not self._start_attempted or self._start_failed_before_thread:
            try:
                self._stream.close()
            except BaseException as exc:
                self._close_errors.append(exc)
            self._finished.set()
        if not self._finished.wait(OUTPUT_DRAIN_ABORT_TIMEOUT_SECONDS):
            raise InvocationError(
                "command output drain thread did not stop after cancellation",
                output=self.snapshot(),
            )
        if self._thread.ident is not None:
            self._thread.join()
        errors: list[str] = []
        if self._read_error is not None:
            errors.append(f"failed while draining command output: {self._read_error}")
        if self._close_errors:
            errors.append(
                "failed to close command output resources: "
                + "; ".join(str(exc) for exc in self._close_errors)
            )
        if errors:
            raise InvocationError(
                "; ".join(errors),
                output=self.snapshot(),
            )

    def finish(self, timeout_sec: float = OUTPUT_DRAIN_TIMEOUT_SECONDS) -> str:
        if (
            isinstance(timeout_sec, bool)
            or not isinstance(timeout_sec, (int, float))
            or not math.isfinite(timeout_sec)
            or timeout_sec <= 0
        ):
            raise ValueError("output drain timeout must be a finite positive number")
        if not self._started:
            raise InvocationError("command output collector was not started")
        if not self._finished.wait(timeout_sec):
            try:
                self.abort()
            except InvocationError as exc:
                raise InvocationError(
                    "command output pipe remained open after process termination; "
                    f"cancellation also failed: {exc}",
                    output=exc.output,
                ) from exc
            raise InvocationError(
                "command output pipe remained open after process termination",
                output=self.snapshot(),
            )
        self._thread.join()
        errors: list[str] = []
        if self._read_error is not None:
            errors.append(f"failed while draining command output: {self._read_error}")
        if self._close_errors:
            errors.append(
                "failed to close command output resources: "
                + "; ".join(str(exc) for exc in self._close_errors)
            )
        if errors:
            raise InvocationError(
                "; ".join(errors),
                output=self.snapshot(),
            )
        return self.snapshot()


class _CapturedStreams:
    def __init__(
        self,
        stdout: BoundedOutputCollector,
        stderr: BoundedOutputCollector | None,
    ):
        self.stdout = stdout
        self.stderr = stderr

    def all_finished(self) -> bool:
        return self.stdout.is_finished() and (
            self.stderr is None or self.stderr.is_finished()
        )

    def snapshot(self) -> tuple[str, str]:
        return (
            self.stdout.snapshot(),
            "" if self.stderr is None else self.stderr.snapshot(),
        )

    def combined_snapshot(self) -> str:
        stdout, stderr = self.snapshot()
        if not stderr:
            return stdout
        separator = "" if not stdout or stdout.endswith("\n") else "\n"
        return f"{stdout}{separator}[nebula-command-stderr]\n{stderr}"

    def finish(self) -> tuple[str, str]:
        errors: list[str] = []
        try:
            stdout = self.stdout.finish()
        except InvocationError as exc:
            stdout = exc.output
            errors.append(f"stdout: {exc}")
        stderr = ""
        if self.stderr is not None:
            try:
                stderr = self.stderr.finish()
            except InvocationError as exc:
                stderr = exc.output
                errors.append(f"stderr: {exc}")
        if errors:
            raise InvocationError(
                "; ".join(errors),
                output=self.combined_snapshot(),
            )
        return stdout, stderr

    def abort(self) -> None:
        errors: list[str] = []
        try:
            self.stdout.abort()
        except InvocationError as exc:
            errors.append(f"stdout: {exc}")
        if self.stderr is not None:
            try:
                self.stderr.abort()
            except InvocationError as exc:
                errors.append(f"stderr: {exc}")
        if errors:
            raise InvocationError(
                "; ".join(errors),
                output=self.combined_snapshot(),
            )


class LongLivedProcess:
    """Own a long-lived process containment domain and its drained output pipes.

    On POSIX, the unreaped leader, stable identities, and cooperative writer-FD stack
    retain ownership across ``setsid``/``setpgid`` only for repository-controlled
    spawns that preserve every anchor until exit. A descendant that deliberately
    closes or omits an anchor violates this contract and is not contained. On Windows,
    a kill-on-close Job Object provides a distinct OS-enforced recursive boundary.
    Callers must use this owner instead of waiting on the underlying ``Popen`` object.
    """

    def __init__(
        self,
        proc: subprocess.Popen[bytes],
        streams: _CapturedStreams,
        job: _WindowsJob | None,
        descendant_tracker: _PosixDescendantTracker | None,
        containment_policy: ProcessContainmentPolicy,
    ) -> None:
        self._proc = proc
        self._streams = streams
        self._job = job
        self._descendant_tracker = descendant_tracker
        self._containment_policy = containment_policy
        self._observed_returncode: int | None = None
        self._finished = False
        self._finish_result: tuple[str, str] | None = None
        self._cancel_requested = False
        self._containment_sealed = False

    @property
    def pid(self) -> int:
        return self._proc.pid

    @property
    def returncode(self) -> int | None:
        if self._proc.returncode is not None:
            return self._proc.returncode
        return self._observed_returncode

    @property
    def containment_sealed(self) -> bool:
        return self._containment_sealed

    @property
    def containment_policy(self) -> ProcessContainmentPolicy:
        return self._containment_policy

    @property
    def containment_complete(self) -> bool:
        return self._containment_sealed

    @property
    def stdout_collector(self) -> BoundedOutputCollector:
        return self._streams.stdout

    @property
    def stderr_collector(self) -> BoundedOutputCollector:
        if self._streams.stderr is None:
            raise InvocationError("long-lived process stderr collector is unavailable")
        return self._streams.stderr

    def snapshots(self) -> tuple[str, str]:
        return self._streams.snapshot()

    def poll(self) -> int | None:
        if self._finished:
            return self.returncode
        if os.name == "nt":
            self._observed_returncode = self._proc.poll()
            return self._observed_returncode
        tracker = self._descendant_tracker
        if tracker is None:
            raise InvocationError("long-lived POSIX process has no descendant tracker")
        tracker.raise_if_failed()
        status = _posix_leader_status(self._proc)
        if status is None:
            return None
        self._observed_returncode = _returncode_from_waitid(status)
        return self._observed_returncode

    def wait(self, timeout: float | None = None) -> int:
        if timeout is not None and (
            isinstance(timeout, bool)
            or not isinstance(timeout, (int, float))
            or not math.isfinite(timeout)
            or timeout < 0
        ):
            raise ValueError("process wait timeout must be a finite non-negative number")
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            returncode = self.poll()
            if returncode is not None:
                return returncode
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(
                    self._proc.args,
                    timeout,
                )
            time.sleep(PROCESS_TERMINATION_POLL_SECONDS)

    def cancel(self) -> None:
        """Immediately stop the owned group/Job without draining or reaping it.

        This is the signal-handler path for a parent that may itself be about to
        terminate. ``finish()`` remains responsible for the later resource
        convergence. Stable POSIX descendant identities remain owned until
        ``finish()`` confirms that the containment domain is sealed.
        """

        if self._finished or self._containment_sealed or self._cancel_requested:
            return
        self._cancel_requested = True
        if os.name == "nt":
            job = self._job
            if job is None:
                raise InvocationError("long-lived Windows process has no owned Job Object")
            job.terminate(INFRASTRUCTURE_ERROR_RETURN_CODE)
            return
        tracker = self._descendant_tracker
        if tracker is None:
            raise InvocationError("long-lived POSIX process has no descendant tracker")
        tracker.kill()

    def finish(self) -> tuple[str, str]:
        """Seal the owned group/Job, reap its leader, and finish both pipes."""

        if self._finished:
            assert self._finish_result is not None
            return self._finish_result
        if os.name == "nt":
            self._finish_result = _finish_long_lived_windows_process(self)
        else:
            self._finish_result = _finish_long_lived_posix_process(self)
        self._finished = True
        return self._finish_result


if os.name == "nt":
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
    _JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION_CLASS = 1
    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS = 9
    _TH32CS_SNAPTHREAD = 0x00000004
    _THREAD_SUSPEND_RESUME = 0x0002
    _ERROR_NO_MORE_FILES = 18
    _ERROR_BROKEN_PIPE = 109
    _ERROR_NO_DATA = 232
    _ERROR_PIPE_NOT_CONNECTED = 233
    _CREATE_SUSPENDED = 0x00000004
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    class _IoCounters(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_ulonglong),
            ("WriteOperationCount", ctypes.c_ulonglong),
            ("OtherOperationCount", ctypes.c_ulonglong),
            ("ReadTransferCount", ctypes.c_ulonglong),
            ("WriteTransferCount", ctypes.c_ulonglong),
            ("OtherTransferCount", ctypes.c_ulonglong),
        ]

    class _JobObjectBasicLimitInformation(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", ctypes.c_longlong),
            ("PerJobUserTimeLimit", ctypes.c_longlong),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.DWORD),
            ("SchedulingClass", wintypes.DWORD),
        ]

    class _JobObjectExtendedLimitInformation(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", _JobObjectBasicLimitInformation),
            ("IoInfo", _IoCounters),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    class _JobObjectBasicAccountingInformation(ctypes.Structure):
        _fields_ = [
            ("TotalUserTime", ctypes.c_longlong),
            ("TotalKernelTime", ctypes.c_longlong),
            ("ThisPeriodTotalUserTime", ctypes.c_longlong),
            ("ThisPeriodTotalKernelTime", ctypes.c_longlong),
            ("TotalPageFaultCount", wintypes.DWORD),
            ("TotalProcesses", wintypes.DWORD),
            ("ActiveProcesses", wintypes.DWORD),
            ("TotalTerminatedProcesses", wintypes.DWORD),
        ]

    class _ThreadEntry32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD),
            ("cntUsage", wintypes.DWORD),
            ("th32ThreadID", wintypes.DWORD),
            ("th32OwnerProcessID", wintypes.DWORD),
            ("tpBasePri", wintypes.LONG),
            ("tpDeltaPri", wintypes.LONG),
            ("dwFlags", wintypes.DWORD),
        ]

    _kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    _kernel32.SetInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
    ]
    _kernel32.SetInformationJobObject.restype = wintypes.BOOL
    _kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    _kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _kernel32.TerminateJobObject.restype = wintypes.BOOL
    _kernel32.QueryInformationJobObject.argtypes = [
        wintypes.HANDLE,
        ctypes.c_int,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.c_void_p,
    ]
    _kernel32.QueryInformationJobObject.restype = wintypes.BOOL
    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel32.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry32)]
    _kernel32.Thread32First.restype = wintypes.BOOL
    _kernel32.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(_ThreadEntry32)]
    _kernel32.Thread32Next.restype = wintypes.BOOL
    _kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.OpenThread.restype = wintypes.HANDLE
    _kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    _kernel32.ResumeThread.restype = wintypes.DWORD
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.PeekNamedPipe.argtypes = [
        wintypes.HANDLE,
        ctypes.c_void_p,
        wintypes.DWORD,
        ctypes.c_void_p,
        ctypes.POINTER(wintypes.DWORD),
        ctypes.c_void_p,
    ]
    _kernel32.PeekNamedPipe.restype = wintypes.BOOL

    def _read_windows_pipe_chunk(fd: int) -> bytes | None:
        available = wintypes.DWORD()
        handle = msvcrt.get_osfhandle(fd)
        if not _kernel32.PeekNamedPipe(
            handle,
            None,
            0,
            None,
            ctypes.byref(available),
            None,
        ):
            code = ctypes.get_last_error()
            if code in (_ERROR_BROKEN_PIPE, _ERROR_NO_DATA, _ERROR_PIPE_NOT_CONNECTED):
                return b""
            raise OSError(
                code,
                f"PeekNamedPipe failed: {ctypes.FormatError(code).strip()}",
            )
        if available.value == 0:
            return None
        return os.read(fd, min(int(available.value), 64 * 1024))

    def _windows_error(context: str) -> OSError:
        code = ctypes.get_last_error()
        return OSError(code, f"{context}: {ctypes.FormatError(code).strip()}")

    def _windows_error_from_code(context: str, code: int) -> OSError:
        return OSError(code, f"{context}: {ctypes.FormatError(code).strip()}")

    def _close_windows_handle(handle: int) -> None:
        if not _kernel32.CloseHandle(handle):
            raise _windows_error("CloseHandle failed")

    class _WindowsJob:
        def __init__(self) -> None:
            handle = _kernel32.CreateJobObjectW(None, None)
            if not handle:
                raise _windows_error("CreateJobObjectW failed")
            self._handle = handle
            self._closed = False
            try:
                limits = _JobObjectExtendedLimitInformation()
                limits.BasicLimitInformation.LimitFlags = (
                    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                )
                if not _kernel32.SetInformationJobObject(
                    handle,
                    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION_CLASS,
                    ctypes.byref(limits),
                    ctypes.sizeof(limits),
                ):
                    raise _windows_error("SetInformationJobObject failed")
            except BaseException as exc:
                try:
                    _close_windows_handle(handle)
                except OSError as close_error:
                    exc.add_note(
                        "Windows Job initialization handle cleanup failed: "
                        f"{close_error}"
                    )
                raise

        def assign(self, proc: subprocess.Popen[bytes]) -> None:
            process_handle = getattr(proc, "_handle", None)
            if process_handle is None:
                raise InvocationError("subprocess did not expose its Windows process handle")
            if not _kernel32.AssignProcessToJobObject(self._handle, int(process_handle)):
                raise _windows_error("AssignProcessToJobObject failed")

        def terminate(self, exit_code: int) -> None:
            if not _kernel32.TerminateJobObject(self._handle, exit_code):
                raise _windows_error("TerminateJobObject failed")

        def active_processes(self) -> int:
            accounting = _JobObjectBasicAccountingInformation()
            if not _kernel32.QueryInformationJobObject(
                self._handle,
                _JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION_CLASS,
                ctypes.byref(accounting),
                ctypes.sizeof(accounting),
                None,
            ):
                raise _windows_error("QueryInformationJobObject failed")
            return int(accounting.ActiveProcesses)

        def wait_until_empty(self, timeout_sec: float) -> None:
            deadline = time.monotonic() + timeout_sec
            while self.active_processes() != 0:
                if time.monotonic() >= deadline:
                    raise InvocationError(
                        f"Windows Job Object still had active processes after {timeout_sec:g}s"
                    )
                time.sleep(PROCESS_TERMINATION_POLL_SECONDS)

        def close(self) -> None:
            if self._closed:
                return
            _close_windows_handle(self._handle)
            self._closed = True

        @property
        def closed(self) -> bool:
            return self._closed

    def _resume_windows_process(proc: subprocess.Popen[bytes]) -> None:
        snapshot = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPTHREAD, 0)
        snapshot_value = ctypes.cast(snapshot, ctypes.c_void_p).value
        if snapshot_value == _INVALID_HANDLE_VALUE:
            raise _windows_error("CreateToolhelp32Snapshot failed")

        primary_error: BaseException | None = None
        thread_handle = None
        try:
            entry = _ThreadEntry32()
            entry.dwSize = ctypes.sizeof(entry)
            matching_thread_ids: list[int] = []
            ctypes.set_last_error(0)
            found = bool(_kernel32.Thread32First(snapshot, ctypes.byref(entry)))
            if not found:
                code = ctypes.get_last_error()
                if code != _ERROR_NO_MORE_FILES:
                    raise _windows_error_from_code("Thread32First failed", code)
            while found:
                if int(entry.th32OwnerProcessID) == proc.pid:
                    matching_thread_ids.append(int(entry.th32ThreadID))
                ctypes.set_last_error(0)
                if not _kernel32.Thread32Next(snapshot, ctypes.byref(entry)):
                    code = ctypes.get_last_error()
                    if code != _ERROR_NO_MORE_FILES:
                        raise _windows_error_from_code("Thread32Next failed", code)
                    found = False
            if len(matching_thread_ids) != 1:
                raise InvocationError(
                    "suspended Windows process did not have exactly one discoverable thread: "
                    f"found {len(matching_thread_ids)}"
                )
            thread_handle = _kernel32.OpenThread(
                _THREAD_SUSPEND_RESUME,
                False,
                matching_thread_ids[0],
            )
            if not thread_handle:
                raise _windows_error("OpenThread failed")
            previous_suspend_count = _kernel32.ResumeThread(thread_handle)
            if previous_suspend_count == 0xFFFFFFFF:
                raise _windows_error("ResumeThread failed")
            if previous_suspend_count != 1:
                raise InvocationError(
                    "suspended Windows process had an unexpected thread suspend count: "
                    f"{previous_suspend_count}"
                )
        except BaseException as exc:
            primary_error = exc

        close_errors: list[str] = []
        if thread_handle is not None:
            try:
                _close_windows_handle(thread_handle)
            except OSError as exc:
                close_errors.append(f"thread handle: {exc}")
        try:
            _close_windows_handle(snapshot)
        except OSError as exc:
            close_errors.append(f"snapshot handle: {exc}")

        if primary_error is not None:
            if close_errors:
                if not isinstance(primary_error, Exception):
                    primary_error.add_note(
                        "Windows suspended-thread handle cleanup failures: "
                        + "; ".join(close_errors)
                    )
                    raise primary_error
                raise InvocationError(
                    f"{primary_error}; handle cleanup failed: {'; '.join(close_errors)}"
                ) from primary_error
            raise primary_error
        if close_errors:
            raise InvocationError(
                f"Windows thread handle cleanup failed: {'; '.join(close_errors)}"
            )


def _require_posix_primitives() -> None:
    required = ("waitid", "P_PID", "WEXITED", "WNOHANG", "WNOWAIT")
    missing = [name for name in required if not hasattr(os, name)]
    if missing:
        raise InvocationError(
            "POSIX host lacks required non-reaping wait primitives: "
            + ", ".join(missing)
        )


def _require_containment_policy(policy: ProcessContainmentPolicy) -> None:
    if not isinstance(policy, ProcessContainmentPolicy):
        raise InvocationError(
            "process containment policy must be a ProcessContainmentPolicy value"
        )
    if (
        os.name != "nt"
        and policy is ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
    ):
        raise InvocationError(
            "OS-enforced recursive containment is unavailable on this POSIX host; "
            "use TRUSTED_COOPERATIVE only when every nested spawn preserves the "
            "containment anchor"
        )


def _parse_posix_containment_token_stack(value: str) -> tuple[str, ...]:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise InvocationError(
            "inherited POSIX containment token stack is not ASCII"
        ) from exc
    if len(encoded) > POSIX_CONTAINMENT_TOKEN_STACK_MAX_BYTES:
        raise InvocationError("inherited POSIX containment token stack is too large")
    tokens = tuple(value.split(":")) if value else ()
    if len(tokens) > POSIX_CONTAINMENT_TOKEN_STACK_MAX_DEPTH:
        raise InvocationError("inherited POSIX containment token stack is too deep")
    for token in tokens:
        if len(token) != POSIX_CONTAINMENT_TOKEN_HEX_CHARS or any(
            character not in "0123456789abcdef" for character in token
        ):
            raise InvocationError("inherited POSIX containment token stack is malformed")
    return tokens


def _prepare_posix_containment_environment(
    env: dict[str, str] | None,
) -> tuple[dict[str, str], str, tuple[int, ...]]:
    source = os.environ if env is None else env
    inherited_tokens = _parse_posix_containment_token_stack(
        os.environ.get(POSIX_CONTAINMENT_TOKEN_ENV, "")
    )
    inherited_anchor_fds = _validated_posix_anchor_fd_stack(os.environ)
    if len(inherited_tokens) >= POSIX_CONTAINMENT_TOKEN_STACK_MAX_DEPTH:
        raise InvocationError("POSIX containment token stack reached its depth limit")
    token = secrets.token_hex(POSIX_CONTAINMENT_TOKEN_HEX_CHARS // 2)
    token_stack = ":".join((*inherited_tokens, token))
    if len(token_stack.encode("ascii")) > POSIX_CONTAINMENT_TOKEN_STACK_MAX_BYTES:
        raise InvocationError("POSIX containment token stack reached its size limit")

    # Keep the anchor first in the exec environment. Linux can then prove it
    # within a strict bounded read even when a child has a very large environment.
    prepared = {POSIX_CONTAINMENT_TOKEN_ENV: token_stack}
    prepared.update(
        (key, value)
        for key, value in source.items()
        if key
        not in (POSIX_CONTAINMENT_TOKEN_ENV, POSIX_CONTAINMENT_ANCHOR_FDS_ENV)
    )
    return prepared, token, inherited_anchor_fds


def _validated_posix_anchor_fd_stack(
    source: Mapping[str, str],
) -> tuple[int, ...]:
    raw_stack = source.get(POSIX_CONTAINMENT_ANCHOR_FDS_ENV, "")
    try:
        encoded = raw_stack.encode("ascii")
    except UnicodeEncodeError as exc:
        raise InvocationError(
            "inherited POSIX containment anchor stack is not ASCII"
        ) from exc
    if len(encoded) > POSIX_CONTAINMENT_ANCHOR_STACK_MAX_BYTES:
        raise InvocationError("inherited POSIX containment anchor stack is too large")
    raw_fds = tuple(raw_stack.split(":")) if raw_stack else ()
    if len(raw_fds) > POSIX_CONTAINMENT_ANCHOR_STACK_MAX_DEPTH:
        raise InvocationError("inherited POSIX containment anchor stack is too deep")
    anchor_fds: list[int] = []
    for raw_fd in raw_fds:
        if (
            not raw_fd.isascii()
            or not raw_fd.isdecimal()
            or raw_fd != str(int(raw_fd))
        ):
            raise InvocationError(
                "inherited POSIX containment anchor stack is malformed"
            )
        anchor_fd = int(raw_fd)
        if anchor_fd < 3:
            raise InvocationError(
                "inherited POSIX containment anchor descriptor is unsafe"
            )
        if anchor_fd in anchor_fds:
            raise InvocationError(
                "inherited POSIX containment anchor stack contains duplicates"
            )
        try:
            status = os.fstat(anchor_fd)
            descriptor_flags = fcntl.fcntl(anchor_fd, fcntl.F_GETFL)
        except OSError as exc:
            raise InvocationError(
                "inherited POSIX containment anchor descriptor is not open: "
                f"fd={anchor_fd}: {exc}"
            ) from exc
        if not stat.S_ISFIFO(status.st_mode):
            raise InvocationError(
                "inherited POSIX containment anchor descriptor is not a pipe: "
                f"fd={anchor_fd}"
            )
        if (descriptor_flags & os.O_ACCMODE) != os.O_WRONLY:
            raise InvocationError(
                "inherited POSIX containment anchor descriptor is not write-only: "
                f"fd={anchor_fd}"
            )
        if not os.get_inheritable(anchor_fd):
            raise InvocationError(
                "inherited POSIX containment anchor descriptor is close-on-exec: "
                f"fd={anchor_fd}"
            )
        anchor_fds.append(anchor_fd)
    return tuple(anchor_fds)


def cooperative_posix_spawn_pass_fds() -> tuple[int, ...]:
    """Validate and return every inherited cooperative containment anchor.

    Repository-controlled POSIX spawn sites must add the returned descriptor to
    ``subprocess.Popen(pass_fds=...)``.  Nested containment produces a bounded
    stack of write-only pipe ends; preserving all of them keeps each outer owner
    able to identify native descendants without relying on environment visibility.
    This is a trusted-spawn contract, not an OS-enforced sandbox boundary.
    """

    if os.name == "nt":
        return ()
    anchor_fds = _validated_posix_anchor_fd_stack(os.environ)
    if not anchor_fds:
        raise InvocationError(
            "cooperative POSIX spawn is missing its containment anchor stack"
        )
    return anchor_fds


def _posix_target_argv(
    cmd: Sequence[str | os.PathLike[str]] | str,
    *,
    shell: bool,
) -> list[str]:
    if isinstance(cmd, str):
        arguments = [cmd]
    else:
        arguments = [os.fspath(argument) for argument in cmd]
    if not arguments or any(not isinstance(argument, str) for argument in arguments):
        raise InvocationError("POSIX command must contain at least one string argument")
    if shell:
        return ["/bin/sh", "-c", arguments[0], *arguments[1:]]
    return arguments


class _PosixExecGate:
    """Hold a repository-owned session leader until its domain is captured."""

    def __init__(self, target_argv: list[str]) -> None:
        gate_script = Path(__file__).with_name("posix_exec_gate.py").resolve()
        if not gate_script.is_file():
            raise InvocationError(f"POSIX exec gate is missing: {gate_script}")
        self._gate_read_fd = -1
        self._gate_write_fd = -1
        self._error_read_fd = -1
        self._error_write_fd = -1
        self._anchor_read_fd = -1
        self._anchor_write_fd = -1
        self._anchor_read_handle: int | None = None
        self._anchor_write_handle: int | None = None
        try:
            self._gate_read_fd, self._gate_write_fd = os.pipe()
            self._error_read_fd, self._error_write_fd = os.pipe()
            self._anchor_read_fd, self._anchor_write_fd = os.pipe()
            os.set_blocking(self._anchor_read_fd, False)
            os.set_inheritable(self._anchor_read_fd, False)
            os.set_inheritable(self._anchor_write_fd, True)
            if platform.system() == "Darwin":
                (
                    self._anchor_read_handle,
                    self._anchor_write_handle,
                ) = _darwin_pipe_end_handles(self._anchor_read_fd)
        except BaseException as exc:
            cleanup_errors = self.close()
            if cleanup_errors:
                if not isinstance(exc, Exception):
                    exc.add_note("exec-gate pipe cleanup failed: " + "; ".join(cleanup_errors))
                    raise
                raise InvocationError(
                    f"failed to create POSIX exec gate: {exc}; cleanup failures: "
                    + "; ".join(cleanup_errors)
                ) from exc
            raise
        self.command = [
            sys.executable,
            "-I",
            "-S",
            str(gate_script),
            str(self._gate_read_fd),
            str(self._error_write_fd),
            str(self._anchor_write_fd),
            *target_argv,
        ]
        self.pass_fds = (
            self._gate_read_fd,
            self._error_write_fd,
            *((self._anchor_write_fd,) if self._anchor_write_fd >= 0 else ()),
        )

    @property
    def cooperative_anchor_fd(self) -> int | None:
        return self._anchor_write_fd if self._anchor_write_fd >= 0 else None

    def detach_posix_anchor(self) -> tuple[int, int | None, int | None]:
        if self._anchor_read_fd < 0:
            raise InvocationError("POSIX cooperative containment anchor is unavailable")
        if platform.system() == "Darwin" and (
            self._anchor_read_handle is None or self._anchor_write_handle is None
        ):
            raise InvocationError("Darwin cooperative containment anchor is incomplete")
        anchor = (
            self._anchor_read_fd,
            self._anchor_read_handle,
            self._anchor_write_handle,
        )
        self._anchor_read_fd = -1
        self._anchor_read_handle = None
        self._anchor_write_handle = None
        return anchor

    def _close_fd(self, attribute: str) -> str:
        fd = getattr(self, attribute)
        if fd < 0:
            return ""
        setattr(self, attribute, -1)
        try:
            os.close(fd)
        except OSError as exc:
            return f"fd {fd}: {exc}"
        return ""

    def close(self) -> list[str]:
        return [
            error
            for error in (
                self._close_fd("_gate_read_fd"),
                self._close_fd("_gate_write_fd"),
                self._close_fd("_error_read_fd"),
                self._close_fd("_error_write_fd"),
                self._close_fd("_anchor_read_fd"),
                self._close_fd("_anchor_write_fd"),
            )
            if error
        ]

    def parent_after_launch(self) -> None:
        errors = [
            error
            for error in (
                self._close_fd("_gate_read_fd"),
                self._close_fd("_error_write_fd"),
                self._close_fd("_anchor_write_fd"),
            )
            if error
        ]
        if errors:
            raise InvocationError(
                "failed to transfer POSIX exec-gate descriptors: "
                + "; ".join(errors)
            )

    def release_and_verify_exec(self) -> None:
        payload = bytearray()
        primary_error: InvocationError | None = None
        try:
            if self._gate_write_fd < 0 or self._error_read_fd < 0:
                raise InvocationError("POSIX exec gate lost its parent descriptors")
            try:
                written = os.write(self._gate_write_fd, b"\x01")
            except OSError as exc:
                raise InvocationError(
                    f"failed to release POSIX exec gate: {exc}"
                ) from exc
            if written != 1:
                raise InvocationError("POSIX exec gate release write was incomplete")
            close_error = self._close_fd("_gate_write_fd")
            if close_error:
                raise InvocationError(
                    "failed to close POSIX exec-gate release descriptor: "
                    + close_error
                )

            os.set_blocking(self._error_read_fd, False)
            deadline = time.monotonic() + POSIX_EXEC_GATE_HANDSHAKE_SECONDS
            with selectors.DefaultSelector() as selector:
                selector.register(self._error_read_fd, selectors.EVENT_READ)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        raise InvocationError(
                            "POSIX exec gate did not complete its exec handshake"
                        )
                    try:
                        chunk = os.read(self._error_read_fd, 4096)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        break
                    payload.extend(chunk)
                    if len(payload) > POSIX_EXEC_GATE_ERROR_MAX_BYTES:
                        raise InvocationError(
                            "POSIX exec gate exceeded its bounded error channel"
                        )
        except InvocationError as exc:
            primary_error = exc
        cleanup_errors = self.close()
        if primary_error is not None:
            message = str(primary_error)
            if cleanup_errors:
                message += "; descriptor cleanup failures: " + "; ".join(
                    cleanup_errors
                )
            raise InvocationError(message) from primary_error
        if cleanup_errors:
            raise InvocationError(
                "POSIX exec-gate descriptor cleanup failed: "
                + "; ".join(cleanup_errors)
            )
        if payload == POSIX_EXEC_GATE_READY_STATUS:
            return
        if not payload:
            raise InvocationError(
                "POSIX exec gate terminated before its readiness handshake"
            )
        detail = bytes(payload)
        if detail.startswith(POSIX_EXEC_GATE_READY_STATUS):
            detail = detail[len(POSIX_EXEC_GATE_READY_STATUS) :]
        if detail.startswith(POSIX_EXEC_GATE_ERROR_PREFIX):
            detail = detail[len(POSIX_EXEC_GATE_ERROR_PREFIX) :]
        diagnostic = detail.decode("utf-8", errors="replace").strip()
        if not diagnostic:
            diagnostic = "POSIX exec gate returned an invalid status protocol"
        raise InvocationError(diagnostic)


def _posix_leader_status(proc: subprocess.Popen[bytes]) -> object | None:
    try:
        return os.waitid(
            os.P_PID,
            proc.pid,
            os.WEXITED | os.WNOHANG | os.WNOWAIT,
        )
    except ChildProcessError as exc:
        raise _ProcessOwnershipError(
            f"lost ownership of process-group leader {proc.pid} before cleanup"
        ) from exc
    except OSError as exc:
        raise InvocationError(
            f"failed to observe process-group leader {proc.pid}: {exc}"
        ) from exc


def _posix_leader_exit_observed(proc: subprocess.Popen[bytes]) -> bool:
    return _posix_leader_status(proc) is not None


def _returncode_from_waitid(status: object) -> int:
    code = getattr(status, "si_code", None)
    value = getattr(status, "si_status", None)
    if type(value) is not int:
        raise InvocationError("POSIX wait status did not contain an integer exit status")
    if code == getattr(os, "CLD_EXITED", object()):
        return value
    if code in (
        getattr(os, "CLD_KILLED", object()),
        getattr(os, "CLD_DUMPED", object()),
    ):
        return -value
    raise InvocationError(f"POSIX wait returned unexpected child status code: {code!r}")


def _wait_for_posix_leader_exit(proc: subprocess.Popen[bytes], timeout_sec: float) -> bool:
    deadline = time.monotonic() + timeout_sec
    while True:
        if _posix_leader_exit_observed(proc):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(PROCESS_TERMINATION_POLL_SECONDS)


_DARWIN_PROC_ALL_PIDS = 1
_DARWIN_PROC_PGRP_ONLY = 2
_DARWIN_PROC_PIDTBSDINFO = 3
_DARWIN_ZOMBIE_STATUS = 5


class _DarwinProcBsdInfo(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("xstatus", ctypes.c_uint32),
        ("pid", ctypes.c_uint32),
        ("ppid", ctypes.c_uint32),
        ("uid", ctypes.c_uint32),
        ("gid", ctypes.c_uint32),
        ("ruid", ctypes.c_uint32),
        ("rgid", ctypes.c_uint32),
        ("svuid", ctypes.c_uint32),
        ("svgid", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("command", ctypes.c_char * 16),
        ("name", ctypes.c_char * 32),
        ("open_files", ctypes.c_uint32),
        ("process_group", ctypes.c_uint32),
        ("job_control_count", ctypes.c_uint32),
        ("controlling_device", ctypes.c_uint32),
        ("foreground_group", ctypes.c_uint32),
        ("nice", ctypes.c_int32),
        ("start_seconds", ctypes.c_uint64),
        ("start_microseconds", ctypes.c_uint64),
    ]


class _DarwinProcFdInfo(ctypes.Structure):
    _fields_ = [
        ("fd", ctypes.c_int32),
        ("fd_type", ctypes.c_uint32),
    ]


class _DarwinProcFileInfo(ctypes.Structure):
    _fields_ = [
        ("open_flags", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("offset", ctypes.c_int64),
        ("fd_type", ctypes.c_int32),
        ("guard_flags", ctypes.c_uint32),
    ]


class _DarwinVinfoStat(ctypes.Structure):
    _fields_ = [
        ("device", ctypes.c_uint32),
        ("mode", ctypes.c_uint16),
        ("link_count", ctypes.c_uint16),
        ("inode", ctypes.c_uint64),
        ("uid", ctypes.c_uint32),
        ("gid", ctypes.c_uint32),
        ("atime", ctypes.c_int64),
        ("atime_ns", ctypes.c_int64),
        ("mtime", ctypes.c_int64),
        ("mtime_ns", ctypes.c_int64),
        ("ctime", ctypes.c_int64),
        ("ctime_ns", ctypes.c_int64),
        ("birthtime", ctypes.c_int64),
        ("birthtime_ns", ctypes.c_int64),
        ("size", ctypes.c_int64),
        ("blocks", ctypes.c_int64),
        ("block_size", ctypes.c_int32),
        ("flags", ctypes.c_uint32),
        ("generation", ctypes.c_uint32),
        ("raw_device", ctypes.c_uint32),
        ("spare", ctypes.c_int64 * 2),
    ]


class _DarwinPipeInfo(ctypes.Structure):
    _fields_ = [
        ("stat", _DarwinVinfoStat),
        ("handle", ctypes.c_uint64),
        ("peer_handle", ctypes.c_uint64),
        ("status", ctypes.c_int32),
        ("reserved", ctypes.c_int32),
    ]


class _DarwinPipeFdInfo(ctypes.Structure):
    _fields_ = [
        ("file_info", _DarwinProcFileInfo),
        ("pipe_info", _DarwinPipeInfo),
    ]


_DARWIN_PROC_PIDLISTFDS = 1
_DARWIN_PROC_PIDFDPIPEINFO = 6
_DARWIN_PROX_FDTYPE_PIPE = 6


def _darwin_libproc() -> ctypes.CDLL:
    try:
        libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    except OSError as exc:
        raise InvocationError(f"failed to load Darwin libproc: {exc}") from exc
    libproc.proc_listpids.argtypes = [
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_listpids.restype = ctypes.c_int
    libproc.proc_pidinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_pidinfo.restype = ctypes.c_int
    libproc.proc_pidfdinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    libproc.proc_pidfdinfo.restype = ctypes.c_int
    return libproc


def _require_darwin_pipe_layout() -> None:
    expected_sizes = {
        "proc_fdinfo": 8,
        "proc_fileinfo": 24,
        "vinfo_stat": 136,
        "pipe_info": 160,
        "pipe_fdinfo": 184,
    }
    observed_sizes = {
        "proc_fdinfo": ctypes.sizeof(_DarwinProcFdInfo),
        "proc_fileinfo": ctypes.sizeof(_DarwinProcFileInfo),
        "vinfo_stat": ctypes.sizeof(_DarwinVinfoStat),
        "pipe_info": ctypes.sizeof(_DarwinPipeInfo),
        "pipe_fdinfo": ctypes.sizeof(_DarwinPipeFdInfo),
    }
    if observed_sizes != expected_sizes:
        raise InvocationError(
            "Darwin libproc pipe structure layout is unsupported: "
            f"expected={expected_sizes!r} observed={observed_sizes!r}"
        )


def _darwin_pipe_fd_info(
    libproc: ctypes.CDLL,
    pid: int,
    fd: int,
) -> _DarwinPipeFdInfo | None:
    info = _DarwinPipeFdInfo()
    ctypes.set_errno(0)
    info_bytes = libproc.proc_pidfdinfo(
        pid,
        fd,
        _DARWIN_PROC_PIDFDPIPEINFO,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if info_bytes == 0:
        error = ctypes.get_errno()
        if error in (0, errno.EBADF, errno.ENOENT, errno.ESRCH):
            return None
        raise OSError(error, os.strerror(error))
    if info_bytes != ctypes.sizeof(info):
        raise InvocationError(
            f"Darwin proc_pidfdinfo returned {info_bytes} bytes for "
            f"pid {pid} fd {fd}; expected {ctypes.sizeof(info)}"
        )
    return info


def _darwin_pipe_end_handles(fd: int) -> tuple[int, int]:
    _require_darwin_pipe_layout()
    libproc = _darwin_libproc()
    try:
        info = _darwin_pipe_fd_info(libproc, os.getpid(), fd)
    except OSError as exc:
        raise InvocationError(
            f"failed to inspect Darwin containment pipe fd {fd}: {exc}"
        ) from exc
    if info is None:
        raise InvocationError(
            f"Darwin containment pipe fd {fd} disappeared during setup"
        )
    handle = int(info.pipe_info.handle)
    peer_handle = int(info.pipe_info.peer_handle)
    if handle == 0 or peer_handle == 0 or handle == peer_handle:
        raise InvocationError(
            "Darwin containment pipe returned invalid endpoint handles"
        )
    return handle, peer_handle


def _darwin_list_process_ids(
    libproc: ctypes.CDLL,
    list_type: int,
    type_info: int,
) -> tuple[int, ...]:
    pids = (ctypes.c_int * DARWIN_PROCESS_AUDIT_MAX_PIDS)()
    buffer_bytes = ctypes.sizeof(pids)
    ctypes.set_errno(0)
    listed_bytes = libproc.proc_listpids(
        list_type,
        type_info,
        ctypes.byref(pids),
        buffer_bytes,
    )
    if listed_bytes < 0:
        error = ctypes.get_errno()
        raise InvocationError(
            "Darwin proc_listpids failed: "
            f"[{error}] {os.strerror(error)}"
        )
    if listed_bytes >= buffer_bytes:
        raise InvocationError("Darwin process audit exceeded the PID buffer bound")
    count = listed_bytes // ctypes.sizeof(ctypes.c_int)
    return tuple(pid for pid in pids[:count] if pid > 0)


def _darwin_read_bsd_info(
    libproc: ctypes.CDLL,
    pid: int,
    *,
    allow_permission_denied: bool = False,
) -> _DarwinProcBsdInfo | None:
    info = _DarwinProcBsdInfo()
    ctypes.set_errno(0)
    info_bytes = libproc.proc_pidinfo(
        pid,
        _DARWIN_PROC_PIDTBSDINFO,
        0,
        ctypes.byref(info),
        ctypes.sizeof(info),
    )
    if info_bytes == 0:
        error = ctypes.get_errno()
        if error in (0, errno.ESRCH):
            return None
        if allow_permission_denied and error in (errno.EACCES, errno.EPERM):
            return None
        raise _DarwinProcessInfoError(pid, error)
    if info_bytes != ctypes.sizeof(info):
        raise InvocationError(
            f"Darwin proc_pidinfo returned {info_bytes} bytes for pid {pid}; "
            f"expected {ctypes.sizeof(info)}"
        )
    if info.pid != pid:
        raise InvocationError(
            f"Darwin proc_pidinfo returned pid {info.pid} while auditing pid {pid}"
        )
    return info


def _darwin_info_identity(info: _DarwinProcBsdInfo) -> _PosixProcessIdentity:
    return _PosixProcessIdentity(
        pid=int(info.pid),
        start_token=(int(info.start_seconds), int(info.start_microseconds)),
    )


def _darwin_read_process_record(
    pid: int,
    *,
    libproc: ctypes.CDLL | None = None,
    allow_permission_denied: bool = False,
) -> _PosixProcessRecord | None:
    active_libproc = _darwin_libproc() if libproc is None else libproc
    # proc_pidinfo and getsid are separate syscalls.  Re-read BSD identity after
    # getsid so PID reuse or concurrent exec/exit cannot splice two processes
    # into one trusted record.
    for _ in range(2):
        before = _darwin_read_bsd_info(
            active_libproc,
            pid,
            allow_permission_denied=allow_permission_denied,
        )
        if before is None:
            return None
        try:
            session_id = os.getsid(pid)
        except ProcessLookupError:
            return None
        except OSError as exc:
            raise InvocationError(
                f"failed to read Darwin session for pid {pid}: {exc}"
            ) from exc
        after = _darwin_read_bsd_info(
            active_libproc,
            pid,
            allow_permission_denied=allow_permission_denied,
        )
        if after is None:
            return None
        if (
            _darwin_info_identity(before) != _darwin_info_identity(after)
            or before.ppid != after.ppid
            or before.process_group != after.process_group
        ):
            continue
        return _PosixProcessRecord(
            identity=_darwin_info_identity(after),
            parent_pid=int(after.ppid),
            process_group_id=int(after.process_group),
            session_id=session_id,
            state="Z" if after.status == _DARWIN_ZOMBIE_STATUS else "R",
        )
    return None


def _darwin_process_table() -> dict[int, _PosixProcessRecord]:
    libproc = _darwin_libproc()
    records: dict[int, _PosixProcessRecord] = {}
    for pid in _darwin_list_process_ids(
        libproc,
        _DARWIN_PROC_ALL_PIDS,
        0,
    ):
        record = _darwin_read_process_record(
            pid,
            libproc=libproc,
            allow_permission_denied=True,
        )
        if record is not None:
            records[pid] = record
    return records


def _darwin_process_group_state(process_group_id: int) -> tuple[bool, bool]:
    """Return ``(has_members, has_live_members)`` via Darwin libproc."""

    libproc = _darwin_libproc()
    pids = _darwin_list_process_ids(
        libproc,
        _DARWIN_PROC_PGRP_ONLY,
        process_group_id,
    )

    has_members = False
    for pid in pids:
        info = _darwin_read_bsd_info(libproc, pid)
        if info is None:
            continue
        has_members = True
        if info.status != _DARWIN_ZOMBIE_STATUS:
            return True, True
    return has_members, False


def _darwin_group_has_only_zombies(process_group_id: int) -> bool:
    _, has_live_members = _darwin_process_group_state(process_group_id)
    # libproc omits a WNOWAIT-retained zombie leader from PROC_PGRP_ONLY. This
    # predicate is only used after that leader's exit has been observed, so an
    # empty listing also proves that no signalable member remains.
    return not has_live_members


def _parse_linux_process_record(
    pid: int,
    stat_text: str,
) -> _PosixProcessRecord:
    command_start = stat_text.find("(")
    command_end = stat_text.rfind(")")
    if command_start < 1 or command_end < command_start:
        raise InvocationError(f"Linux process {pid} returned malformed /proc stat data")
    try:
        recorded_pid = int(stat_text[:command_start].strip())
    except ValueError as exc:
        raise InvocationError(
            f"Linux process {pid} returned a non-numeric /proc stat pid"
        ) from exc
    if recorded_pid != pid:
        raise InvocationError(
            f"Linux /proc/{pid}/stat reported a different pid {recorded_pid}"
        )
    fields = stat_text[command_end + 1 :].split()
    if len(fields) < 20:
        raise InvocationError(f"Linux process {pid} returned incomplete /proc stat data")
    try:
        parent_pid = int(fields[1])
        process_group_id = int(fields[2])
        session_id = int(fields[3])
        start_ticks = int(fields[19])
    except ValueError as exc:
        raise InvocationError(
            f"Linux process {pid} returned non-numeric process identity data"
        ) from exc
    return _PosixProcessRecord(
        identity=_PosixProcessIdentity(pid=pid, start_token=(0, start_ticks)),
        parent_pid=parent_pid,
        process_group_id=process_group_id,
        session_id=session_id,
        state=fields[0],
    )


def _linux_read_process_record(
    pid: int,
    *,
    allow_permission_denied: bool = False,
) -> _PosixProcessRecord | None:
    try:
        stat_text = (Path("/proc") / str(pid) / "stat").read_text(
            encoding="utf-8",
            errors="replace",
        )
    except (FileNotFoundError, ProcessLookupError):
        return None
    except PermissionError as exc:
        if allow_permission_denied:
            return None
        raise InvocationError(
            f"permission denied while auditing Linux process {pid}"
        ) from exc
    except OSError as exc:
        raise InvocationError(f"failed to audit Linux process {pid}: {exc}") from exc
    return _parse_linux_process_record(pid, stat_text)


def _linux_process_table() -> dict[int, _PosixProcessRecord]:
    proc_root = Path("/proc")
    try:
        entries = tuple(proc_root.iterdir())
    except OSError as exc:
        raise InvocationError(f"failed to enumerate Linux /proc: {exc}") from exc
    records: dict[int, _PosixProcessRecord] = {}
    for entry in entries:
        if not entry.name.isdecimal():
            continue
        pid = int(entry.name)
        record = _linux_read_process_record(pid, allow_permission_denied=True)
        if record is not None:
            records[pid] = record
    return records


def _linux_process_group_has_live_members(process_group_id: int) -> bool:
    return any(
        record.process_group_id == process_group_id and record.is_live
        for record in _linux_process_table().values()
    )


def _environment_records_have_containment_token(
    records: Sequence[bytes],
    token: str,
) -> bool:
    prefix = POSIX_CONTAINMENT_TOKEN_ENV.encode("ascii") + b"="
    expected = token.encode("ascii")
    for record in records:
        if not record.startswith(prefix):
            continue
        value = record[len(prefix) :]
        return expected in value.split(b":")
    return False


def _linux_process_has_containment_token(
    expected_record: _PosixProcessRecord,
    token: str,
) -> bool:
    pid = expected_record.identity.pid
    before = _linux_read_process_record(pid)
    if before is None or before.identity != expected_record.identity:
        return False
    proc_dir = Path("/proc") / str(pid)
    try:
        owner_uid = proc_dir.stat().st_uid
    except (FileNotFoundError, ProcessLookupError):
        return False
    except OSError as exc:
        raise InvocationError(
            f"failed to identify Linux process {pid} while scanning containment anchors: {exc}"
        ) from exc
    if owner_uid != os.geteuid():
        return False
    try:
        with (proc_dir / "environ").open("rb", buffering=0) as handle:
            environment = handle.read(POSIX_PROCESS_ENVIRONMENT_MAX_BYTES + 1)
    except (FileNotFoundError, ProcessLookupError):
        return False
    except PermissionError as exc:
        raise InvocationError(
            f"permission denied while scanning same-uid Linux process {pid} environment"
        ) from exc
    except OSError as exc:
        raise InvocationError(
            f"failed to scan Linux process {pid} environment: {exc}"
        ) from exc
    if len(environment) > POSIX_PROCESS_ENVIRONMENT_MAX_BYTES:
        raise InvocationError(
            f"Linux process {pid} environment exceeded the containment audit bound"
        )
    after = _linux_read_process_record(pid)
    if after is None or after.identity != expected_record.identity:
        return False
    return _environment_records_have_containment_token(
        environment.split(b"\0"),
        token,
    )


def _darwin_read_bsd_info_for_stable_audit(
    libproc: ctypes.CDLL,
    pid: int,
) -> _DarwinProcBsdInfo | None:
    for attempt in range(DARWIN_PROCESS_INFO_AUDIT_ATTEMPTS):
        try:
            return _darwin_read_bsd_info(libproc, pid)
        except _DarwinProcessInfoError as exc:
            if (
                exc.error_number
                in (errno.EACCES, errno.EPERM, errno.EIO, errno.EINVAL)
                and attempt + 1 < DARWIN_PROCESS_INFO_AUDIT_ATTEMPTS
            ):
                continue
            raise
    raise AssertionError("Darwin process-info audit retry loop did not return")


def _darwin_process_has_pipe_anchor(
    expected_record: _PosixProcessRecord,
    *,
    anchor_read_handle: int,
    anchor_write_handle: int,
    libproc: ctypes.CDLL,
    fd_buffer: ctypes.Array[_DarwinProcFdInfo],
    deadline: float,
) -> bool:
    pid = expected_record.identity.pid
    try:
        before = _darwin_read_bsd_info_for_stable_audit(libproc, pid)
    except _DarwinProcessInfoError as exc:
        if exc.error_number in (errno.EACCES, errno.EPERM):
            return False
        raise
    if before is None or _darwin_info_identity(before) != expected_record.identity:
        return False
    expected_uid = os.geteuid()
    if int(before.uid) != expected_uid or before.status == _DARWIN_ZOMBIE_STATUS:
        return False
    if int(before.open_files) > DARWIN_PROCESS_FD_AUDIT_MAX_FDS:
        raise InvocationError(
            f"Darwin process {pid} exceeded the containment fd audit bound"
        )
    if time.monotonic() >= deadline:
        raise InvocationError("Darwin containment pipe scan exceeded its deadline")

    ctypes.set_errno(0)
    listed_bytes = libproc.proc_pidinfo(
        pid,
        _DARWIN_PROC_PIDLISTFDS,
        0,
        ctypes.byref(fd_buffer),
        ctypes.sizeof(fd_buffer),
    )
    if listed_bytes <= 0:
        error = ctypes.get_errno()
        current = _darwin_read_bsd_info_for_stable_audit(libproc, pid)
        if (
            current is None
            or _darwin_info_identity(current) != expected_record.identity
            or current.status == _DARWIN_ZOMBIE_STATUS
        ):
            return False
        if listed_bytes == 0 and error == 0 and int(current.open_files) == 0:
            return False
        if error in (errno.EACCES, errno.EPERM):
            return False
        raise InvocationError(
            f"Darwin fd listing failed for stable live pid {pid}: "
            f"[{error}] {os.strerror(error)}"
        )
    if listed_bytes >= ctypes.sizeof(fd_buffer):
        raise InvocationError(
            f"Darwin process {pid} filled the bounded containment fd buffer"
        )
    if listed_bytes % ctypes.sizeof(_DarwinProcFdInfo) != 0:
        raise InvocationError(
            f"Darwin process {pid} returned a partial fd-list record"
        )

    count = listed_bytes // ctypes.sizeof(_DarwinProcFdInfo)
    matched = False
    for index in range(count):
        if index % 64 == 0 and time.monotonic() >= deadline:
            raise InvocationError("Darwin containment pipe scan exceeded its deadline")
        descriptor = fd_buffer[index]
        if descriptor.fd_type != _DARWIN_PROX_FDTYPE_PIPE:
            continue
        try:
            pipe_info = _darwin_pipe_fd_info(libproc, pid, int(descriptor.fd))
        except PermissionError:
            # An unrelated protected process may be unreadable. If it actually
            # holds this unique endpoint, the final read-end EOF oracle remains
            # open and converts the ambiguity into an explicit failure.
            return False
        if pipe_info is None:
            continue
        if (
            int(pipe_info.pipe_info.handle) == anchor_write_handle
            and int(pipe_info.pipe_info.peer_handle) == anchor_read_handle
        ):
            matched = True
            break

    try:
        after = _darwin_read_bsd_info_for_stable_audit(libproc, pid)
    except _DarwinProcessInfoError as exc:
        if matched:
            raise InvocationError(
                f"lost Darwin identity audit access after matching pid {pid} "
                "to the containment pipe"
            ) from exc
        if exc.error_number in (errno.EACCES, errno.EPERM):
            return False
        raise
    if after is None or _darwin_info_identity(after) != expected_record.identity:
        return False
    if int(after.uid) != expected_uid:
        if matched:
            raise InvocationError(
                f"Darwin process {pid} changed uid after matching the containment pipe"
            )
        return False
    if after.status == _DARWIN_ZOMBIE_STATUS:
        return False
    return matched


def _posix_cooperative_member_identities(
    domain: _PosixDescendantDomain,
    process_table: dict[int, _PosixProcessRecord],
    *,
    deadline: float,
) -> set[_PosixProcessIdentity]:
    token = domain.containment_token
    if token is None:
        return set()
    system = platform.system()
    members: set[_PosixProcessIdentity] = set()
    darwin_resources: tuple[
        ctypes.CDLL,
        ctypes.Array[_DarwinProcFdInfo],
    ] | None = None
    if system == "Darwin":
        if domain.anchor_read_handle is None or domain.anchor_write_handle is None:
            raise InvocationError("Darwin containment domain has no pipe-handle oracle")
        _require_darwin_pipe_layout()
        darwin_resources = (
            _darwin_libproc(),
            (_DarwinProcFdInfo * DARWIN_PROCESS_FD_AUDIT_MAX_FDS)(),
        )
    for record in process_table.values():
        # Discover identities that escaped the recorded parent/session closure.
        # Darwin matches the cooperative pipe endpoint; Linux uses the inherited
        # token as supplemental evidence. Stable pre-launch identities cannot
        # carry this launch's newly created anchor.
        if (
            not record.is_live
            or record.identity in domain.prelaunch_identities
        ):
            continue
        if system == "Darwin":
            assert darwin_resources is not None
            libproc, fd_buffer = darwin_resources
            assert domain.anchor_read_handle is not None
            assert domain.anchor_write_handle is not None
            matched = _darwin_process_has_pipe_anchor(
                record,
                anchor_read_handle=domain.anchor_read_handle,
                anchor_write_handle=domain.anchor_write_handle,
                libproc=libproc,
                fd_buffer=fd_buffer,
                deadline=deadline,
            )
        elif system == "Linux":
            if record.identity in domain.member_identities:
                continue
            matched = _linux_process_has_containment_token(record, token)
        else:
            raise InvocationError(
                f"containment token audit is unsupported on POSIX host {system!r}"
            )
        if matched:
            members.add(record.identity)
    return members


def _read_posix_process_record(pid: int) -> _PosixProcessRecord | None:
    system = platform.system()
    if system == "Darwin":
        return _darwin_read_process_record(pid)
    if system == "Linux":
        return _linux_read_process_record(pid)
    raise InvocationError(
        f"descendant process audit is unsupported on POSIX host {system!r}"
    )


def _snapshot_posix_processes() -> dict[int, _PosixProcessRecord]:
    system = platform.system()
    if system == "Darwin":
        return _darwin_process_table()
    if system == "Linux":
        return _linux_process_table()
    raise InvocationError(
        f"descendant process audit is unsupported on POSIX host {system!r}"
    )


def _posix_anchor_has_writers(domain: _PosixDescendantDomain) -> bool:
    if domain.anchor_read_fd < 0:
        raise InvocationError("cooperative POSIX containment anchor is not owned")
    try:
        payload = os.read(domain.anchor_read_fd, 1)
    except BlockingIOError:
        return True
    except OSError as exc:
        raise InvocationError(
            f"failed to audit cooperative POSIX containment anchor: {exc}"
        ) from exc
    if payload:
        raise InvocationError(
            "cooperative POSIX containment anchor carried unexpected data"
        )
    return False


def _close_posix_anchor(domain: _PosixDescendantDomain) -> None:
    if domain.anchor_read_fd < 0:
        return
    anchor_fd = domain.anchor_read_fd
    domain.anchor_read_fd = -1
    try:
        os.close(anchor_fd)
    except OSError as exc:
        raise InvocationError(
            f"failed to close cooperative POSIX containment anchor fd {anchor_fd}: {exc}"
        ) from exc


def _extend_posix_descendant_domain(
    domain: _PosixDescendantDomain,
    process_table: dict[int, _PosixProcessRecord],
    *,
    require_live_leader: bool,
    seed_identities: set[_PosixProcessIdentity] | None = None,
) -> tuple[_PosixProcessRecord, ...]:
    leader_record = process_table.get(domain.leader.pid)
    if require_live_leader and (
        leader_record is None
        or leader_record.identity != domain.leader
        or not leader_record.is_live
    ):
        raise _DescendantOwnershipError(
            "process-group leader stopped anchoring descendant discovery before cleanup"
        )

    selected: dict[_PosixProcessIdentity, _PosixProcessRecord] = {}
    for identity in domain.member_identities | (seed_identities or set()):
        record = process_table.get(identity.pid)
        if record is not None and record.identity == identity:
            selected[identity] = record
    selected_pids = {record.identity.pid for record in selected.values()}
    selected_sessions = {record.session_id for record in selected.values()}

    changed = True
    while changed:
        changed = False
        for record in process_table.values():
            if record.identity in selected:
                continue
            if (
                record.parent_pid not in selected_pids
                and record.session_id not in selected_sessions
            ):
                continue
            selected[record.identity] = record
            selected_pids.add(record.identity.pid)
            selected_sessions.add(record.session_id)
            changed = True

    current_process_group = os.getpgrp()
    for record in selected.values():
        if record.process_group_id <= 1 or record.session_id <= 1:
            raise _DescendantOwnershipError(
                "descendant process entered an unsafe process group or session: "
                f"pid={record.identity.pid} pgid={record.process_group_id} "
                f"sid={record.session_id}"
            )
        if record.process_group_id == current_process_group:
            raise _DescendantOwnershipError(
                "descendant process joined the containment runner's process group: "
                f"pid={record.identity.pid} pgid={record.process_group_id}"
            )

    domain.member_identities.update(selected.keys())
    domain.session_ids.update(selected_sessions)
    return tuple(record for record in selected.values() if record.is_live)


def _capture_posix_descendant_domain(
    proc: subprocess.Popen[bytes],
    containment_token: str | None = None,
    anchor: tuple[int, int | None, int | None] | None = None,
) -> tuple[_PosixDescendantDomain, tuple[_PosixProcessRecord, ...]]:
    process_table = _snapshot_posix_processes()
    leader_record = process_table.get(proc.pid)
    if leader_record is None:
        raise _DescendantOwnershipError(
            f"process-group leader {proc.pid} disappeared before descendant closure was captured"
        )
    if (
        leader_record.process_group_id != proc.pid
        or leader_record.session_id != proc.pid
    ):
        raise _DescendantOwnershipError(
            "contained command was not its own process-group and session leader: "
            f"pid={proc.pid} pgid={leader_record.process_group_id} "
            f"sid={leader_record.session_id}"
        )
    domain = _PosixDescendantDomain(
        leader=leader_record.identity,
        member_identities={leader_record.identity},
        session_ids={leader_record.session_id},
        containment_token=containment_token,
        # The exec gate has not released the target yet, so no process other
        # than the owned leader can carry this launch's fresh random token.
        # Retaining stable pre-launch identities avoids auditing unrelated
        # same-uid processes while still allowing a reused PID to be scanned.
        prelaunch_identities={
            record.identity
            for record in process_table.values()
            if record.identity != leader_record.identity
        },
        anchor_read_fd=-1 if anchor is None else anchor[0],
        anchor_read_handle=None if anchor is None else anchor[1],
        anchor_write_handle=None if anchor is None else anchor[2],
    )
    records = _extend_posix_descendant_domain(
        domain,
        process_table,
        # A fast command may already be a WNOWAIT-retained zombie when the first
        # process-table snapshot completes.  Its stable identity and original
        # session still provide a safe discovery anchor for remaining members.
        require_live_leader=leader_record.is_live,
    )
    return domain, records


def _refresh_posix_descendant_domain(
    domain: _PosixDescendantDomain,
    *,
    discover_token_members: bool = False,
    audit_deadline: float | None = None,
) -> tuple[_PosixProcessRecord, ...]:
    process_table = _snapshot_posix_processes()
    # Whole-system snapshots skip unrelated protected records.  A previously
    # tracked member must instead be queried strictly: losing audit permission
    # for an owned descendant is an infrastructure failure, not evidence that
    # the process exited.
    for identity in domain.member_identities:
        if identity.pid in process_table:
            continue
        record = _read_posix_process_record(identity.pid)
        if record is not None:
            process_table[identity.pid] = record
    if discover_token_members and audit_deadline is None:
        audit_deadline = time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS
    records = _extend_posix_descendant_domain(
        domain,
        process_table,
        require_live_leader=False,
        seed_identities=(
            _posix_cooperative_member_identities(
                domain,
                process_table,
                deadline=audit_deadline,
            )
            if discover_token_members
            else None
        ),
    )
    if (
        discover_token_members
        and not records
        and _posix_anchor_has_writers(domain)
    ):
        raise InvocationError(
            "cooperative POSIX containment anchor still has writers, but no "
            "stable auditable holder was found"
        )
    return records


class _PosixDescendantTracker:
    """Continuously retain stable descendant identities until explicit sealing."""

    def __init__(
        self,
        proc: subprocess.Popen[bytes],
        domain: _PosixDescendantDomain,
        records: tuple[_PosixProcessRecord, ...],
    ) -> None:
        self._proc = proc
        self._domain = domain
        self._records = records
        self._failure: BaseException | None = None
        self._sealed = False
        # Python signal handlers can interrupt the owner thread while it is inside
        # a tracker method.  An RLock keeps cancel() re-entrant for that owner while
        # still serializing it against the background audit thread.
        self._lock = threading.RLock()
        self._stop_requested = threading.Event()
        self._finished = threading.Event()
        self._started = False
        self._thread = threading.Thread(
            target=self._audit_loop,
            name=f"nebula-posix-descendants-{proc.pid}",
            daemon=True,
        )

    @classmethod
    def start(
        cls,
        proc: subprocess.Popen[bytes],
        containment_token: str,
        anchor: tuple[int, int | None, int | None],
    ) -> _PosixDescendantTracker:
        try:
            domain, records = _capture_posix_descendant_domain(
                proc,
                containment_token,
                anchor,
            )
        except BaseException as exc:
            close_error = ""
            try:
                os.close(anchor[0])
            except OSError as close_exc:
                close_error = str(close_exc)
            if close_error:
                if not isinstance(exc, Exception):
                    exc.add_note(
                        "POSIX anchor cleanup after capture failure failed: "
                        + close_error
                    )
                    raise
                raise InvocationError(
                    f"{exc}; POSIX anchor cleanup after capture failure failed: "
                    f"{close_error}"
                ) from exc
            raise
        tracker = cls(proc, domain, records)
        try:
            tracker._thread.start()
        except BaseException as exc:
            tracker._stop_requested.set()
            try:
                _close_posix_anchor(domain)
            except InvocationError as close_exc:
                if not isinstance(exc, Exception):
                    exc.add_note(
                        "POSIX anchor cleanup after tracker start failure failed: "
                        + str(close_exc)
                    )
                    raise
                raise InvocationError(
                    f"{exc}; POSIX anchor cleanup after tracker start failure failed: "
                    f"{close_exc}"
                ) from exc
            raise
        tracker._started = True
        return tracker

    def _raise_if_failed_locked(self) -> None:
        if self._failure is None:
            return
        failure = self._failure
        if isinstance(failure, InvocationError):
            raise InvocationError(
                f"POSIX descendant audit failed: {failure}"
            ) from failure
        if not isinstance(failure, Exception):
            raise failure
        raise InvocationError(
            "POSIX descendant audit failed: "
            f"{type(failure).__name__}: {failure}"
        ) from failure

    def _refresh_locked(
        self,
        *,
        discover_token_members: bool = False,
        audit_deadline: float | None = None,
    ) -> tuple[_PosixProcessRecord, ...]:
        self._records = _refresh_posix_descendant_domain(
            self._domain,
            discover_token_members=discover_token_members,
            audit_deadline=audit_deadline,
        )
        return self._records

    def _audit_loop(self) -> None:
        try:
            while not self._stop_requested.wait(
                POSIX_DESCENDANT_AUDIT_INTERVAL_SECONDS
            ):
                with self._lock:
                    self._refresh_locked()
        except BaseException as exc:
            with self._lock:
                self._failure = exc
            self._stop_requested.set()
        finally:
            self._finished.set()

    def raise_if_failed(self) -> None:
        with self._lock:
            self._raise_if_failed_locked()

    @property
    def sealed(self) -> bool:
        with self._lock:
            return self._sealed

    def stop_and_audit(self) -> tuple[_PosixProcessRecord, ...]:
        self._stop_requested.set()
        if self._started:
            self._thread.join(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
            if self._thread.is_alive():
                raise InvocationError(
                    "POSIX descendant audit thread did not stop before cleanup"
                )
        with self._lock:
            self._raise_if_failed_locked()
            return self._refresh_locked(
                discover_token_members=True,
                audit_deadline=(
                    time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS
                ),
            )

    def kill(
        self,
        initial_records: tuple[_PosixProcessRecord, ...] | None = None,
    ) -> None:
        if self.sealed:
            return
        records = self.stop_and_audit() if initial_records is None else initial_records
        _kill_posix_descendant_domain(self._domain, records)
        _close_posix_anchor(self._domain)
        with self._lock:
            self._sealed = True

    def terminate(
        self,
        initial_records: tuple[_PosixProcessRecord, ...] | None = None,
    ) -> None:
        if self.sealed:
            return
        records = self.stop_and_audit() if initial_records is None else initial_records
        _terminate_posix_descendant_domain(self._domain, records)
        _close_posix_anchor(self._domain)
        with self._lock:
            self._sealed = True


def _signal_posix_descendant_groups(
    domain: _PosixDescendantDomain,
    records: tuple[_PosixProcessRecord, ...],
    sig: signal.Signals,
    signaled_members: set[tuple[int, _PosixProcessIdentity]],
) -> None:
    groups: dict[int, list[_PosixProcessRecord]] = {}
    for record in records:
        groups.setdefault(record.process_group_id, []).append(record)

    # Stop every non-root group before the anchored root group.  This narrows the
    # window in which a descendant can fork again after its root parent exits.
    ordered_group_ids = sorted(
        groups,
        key=lambda group_id: (group_id == domain.leader.pid, group_id),
    )
    for group_id in ordered_group_ids:
        if group_id <= 1 or group_id == os.getpgrp():
            raise _DescendantOwnershipError(
                f"refused to signal unsafe descendant process group {group_id}"
            )
        candidates = groups[group_id]
        candidate_keys = {
            (group_id, candidate.identity) for candidate in candidates
        }
        if candidate_keys.issubset(signaled_members):
            continue

        valid_anchor = False
        reused_identities: list[tuple[_PosixProcessIdentity, _PosixProcessIdentity]] = []
        for candidate in candidates:
            current = _read_posix_process_record(candidate.identity.pid)
            if current is None:
                continue
            if current.identity != candidate.identity:
                reused_identities.append((candidate.identity, current.identity))
                continue
            if current.process_group_id == group_id:
                valid_anchor = True
                break
        if not valid_anchor:
            if reused_identities:
                expected, observed = reused_identities[0]
                raise _DescendantOwnershipError(
                    "refused to signal a process group after PID identity reuse: "
                    f"pid={expected.pid} expected_start={expected.start_token!r} "
                    f"observed_start={observed.start_token!r} pgid={group_id}"
                )
            continue

        try:
            os.killpg(group_id, sig)
        except ProcessLookupError:
            pass
        except PermissionError as exc:
            raise InvocationError(
                f"permission denied while signaling descendant process group "
                f"{group_id} with {sig.name}"
            ) from exc
        except OSError as exc:
            raise InvocationError(
                f"failed to signal descendant process group {group_id} "
                f"with {sig.name}: {exc}"
            ) from exc
        signaled_members.update(candidate_keys)


def _converge_posix_descendant_signal(
    domain: _PosixDescendantDomain,
    initial_records: tuple[_PosixProcessRecord, ...],
    sig: signal.Signals,
    deadline: float,
) -> tuple[_PosixProcessRecord, ...]:
    signaled_members: set[tuple[int, _PosixProcessIdentity]] = set()
    records = initial_records
    empty_audits = 1 if not records else 0
    while empty_audits < POSIX_EMPTY_AUDIT_CONFIRMATIONS:
        if records:
            _signal_posix_descendant_groups(
                domain,
                records,
                sig,
                signaled_members,
            )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if not records:
                raise InvocationError(
                    "POSIX descendant cleanup deadline expired before the "
                    "empty containment domain was independently confirmed"
                )
            return records
        if records:
            time.sleep(min(POSIX_DESCENDANT_SIGNAL_POLL_SECONDS, remaining))
        if time.monotonic() >= deadline:
            return records
        records = _refresh_posix_descendant_domain(
            domain,
            discover_token_members=True,
            audit_deadline=deadline,
        )
        empty_audits = empty_audits + 1 if not records else 0
    return records


def _kill_posix_descendant_domain(
    domain: _PosixDescendantDomain,
    initial_records: tuple[_PosixProcessRecord, ...] | None = None,
) -> None:
    records = (
        _refresh_posix_descendant_domain(
            domain,
            discover_token_members=True,
            audit_deadline=(
                time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS
            ),
        )
        if initial_records is None
        else initial_records
    )
    records = _converge_posix_descendant_signal(
        domain,
        records,
        signal.SIGKILL,
        time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS,
    )
    if records:
        live_pids = ", ".join(str(record.identity.pid) for record in records)
        raise InvocationError(
            "descendant processes survived SIGKILL: " + live_pids
        )


def _terminate_posix_descendant_domain(
    domain: _PosixDescendantDomain,
    initial_records: tuple[_PosixProcessRecord, ...],
) -> None:
    records = _converge_posix_descendant_signal(
        domain,
        initial_records,
        signal.SIGTERM,
        time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS,
    )
    if records:
        _kill_posix_descendant_domain(domain, records)


def _posix_process_group_has_live_members(process_group_id: int) -> bool:
    system = platform.system()
    if system == "Linux":
        return _linux_process_group_has_live_members(process_group_id)
    if system == "Darwin":
        return _darwin_process_group_state(process_group_id)[1]
    raise InvocationError(
        f"long-lived process-group audit is unsupported on POSIX host {system!r}"
    )


def _signal_posix_process_group(
    proc: subprocess.Popen[bytes],
    sig: signal.Signals,
) -> bool:
    try:
        os.killpg(proc.pid, sig)
    except ProcessLookupError:
        return False
    except PermissionError as exc:
        leader_exited = _posix_leader_exit_observed(proc)
        if (
            leader_exited
            and platform.system() == "Darwin"
            and _darwin_group_has_only_zombies(proc.pid)
        ):
            return False
        raise InvocationError(
            f"permission denied while signaling process group {proc.pid} with {sig.name}"
        ) from exc
    except OSError as exc:
        raise InvocationError(
            f"failed to signal process group {proc.pid} with {sig.name}: {exc}"
        ) from exc
    return True


def _finish_reaped_process(
    proc: subprocess.Popen[bytes],
    streams: _CapturedStreams,
    *,
    timed_out: bool,
) -> tuple[int, str, str]:
    try:
        return_code = proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired as exc:
        raise InvocationError(
            f"process-group leader {proc.pid} survived final termination",
            output=streams.combined_snapshot(),
            timed_out=timed_out,
        ) from exc
    try:
        stdout, stderr = streams.finish()
    except InvocationError as exc:
        raise InvocationError(
            str(exc),
            output=exc.output,
            timed_out=timed_out,
        ) from exc
    return return_code, stdout, stderr


def _force_posix_cleanup(
    proc: subprocess.Popen[bytes],
    streams: _CapturedStreams,
    *,
    safe_to_signal_group: bool,
    descendant_domain: _PosixDescendantDomain | None = None,
    descendant_tracker: _PosixDescendantTracker | None = None,
    capture_descendants: bool = False,
    release_unsealed_anchor: bool,
) -> list[str]:
    errors: list[str] = []
    if descendant_tracker is not None and not descendant_tracker.sealed:
        try:
            # Stable identity revalidation remains safe even if another caller
            # incorrectly reaped the leader and destroyed the numeric PGID anchor.
            descendant_tracker.kill()
        except InvocationError as exc:
            errors.append(f"descendant tracker SIGKILL failed: {exc}")
    if (
        release_unsealed_anchor
        and descendant_tracker is not None
        and not descendant_tracker.sealed
    ):
        try:
            _close_posix_anchor(descendant_tracker._domain)
        except InvocationError as exc:
            errors.append(f"descendant tracker anchor release failed: {exc}")
    if proc.returncode is None and safe_to_signal_group:
        domain = descendant_domain
        initial_records: tuple[_PosixProcessRecord, ...] | None = None
        if descendant_tracker is None and domain is None and capture_descendants:
            try:
                domain, initial_records = _capture_posix_descendant_domain(proc)
            except InvocationError as exc:
                errors.append(f"descendant closure capture failed: {exc}")
        if descendant_tracker is None and domain is not None:
            try:
                _kill_posix_descendant_domain(domain, initial_records)
            except InvocationError as exc:
                errors.append(f"descendant-domain SIGKILL failed: {exc}")
        # The WNOWAIT-owned leader still anchors its original group even when a
        # broader descendant audit failed.  Retain this narrow cleanup without
        # presenting it as proof that escaped nested sessions were sealed.
        try:
            _signal_posix_process_group(proc, signal.SIGKILL)
        except InvocationError as exc:
            errors.append(f"final process-group SIGKILL failed: {exc}")
    if proc.returncode is None and safe_to_signal_group:
        retain_unreaped_leader = (
            not release_unsealed_anchor
            and descendant_tracker is not None
            and not descendant_tracker.sealed
        )
        if retain_unreaped_leader:
            try:
                if _posix_leader_status(proc) is None:
                    os.kill(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except (OSError, InvocationError) as exc:
                errors.append(f"direct leader kill failed: {exc}")
            try:
                if not _wait_for_posix_leader_exit(
                    proc,
                    PROCESS_TERMINATION_GRACE_SECONDS,
                ):
                    errors.append("direct leader termination observation timed out")
            except InvocationError as exc:
                errors.append(f"direct leader termination observation failed: {exc}")
        else:
            try:
                proc.kill()
            except OSError as exc:
                errors.append(f"direct leader kill failed: {exc}")
            try:
                proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
            except subprocess.TimeoutExpired:
                errors.append("direct leader reap timed out")
            except OSError as exc:
                errors.append(f"direct leader reap failed: {exc}")
    try:
        streams.finish()
    except InvocationError as exc:
        errors.append(f"output cleanup failed: {exc}")
        try:
            streams.abort()
        except InvocationError as abort_exc:
            errors.append(f"output cancellation failed: {abort_exc}")
    return errors


def _finish_long_lived_posix_process(
    owner: LongLivedProcess,
) -> tuple[str, str]:
    proc = owner._proc
    streams = owner._streams
    tracker = owner._descendant_tracker
    try:
        if tracker is None:
            raise InvocationError("long-lived POSIX process has no descendant tracker")
        # Observing with WNOWAIT proves that the unreaped leader still anchors the
        # root identity before any tracked descendant group is signaled.
        _posix_leader_status(proc)
        if owner._cancel_requested:
            tracker.kill()
        else:
            tracker.terminate()

        owner._containment_sealed = True

        if not _wait_for_posix_leader_exit(
            proc,
            PROCESS_TERMINATION_GRACE_SECONDS,
        ):
            raise InvocationError(
                f"process-group leader {proc.pid} survived final termination",
                output=streams.combined_snapshot(),
            )
        owner._observed_returncode = proc.wait(
            timeout=PROCESS_TERMINATION_GRACE_SECONDS
        )
        return streams.finish()
    except BaseException as exc:
        cleanup_errors = _force_posix_cleanup(
            proc,
            streams,
            safe_to_signal_group=not isinstance(exc, _ProcessOwnershipError),
            descendant_tracker=tracker,
            release_unsealed_anchor=False,
        )
        owner._containment_sealed = tracker is not None and tracker.sealed
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "long-lived POSIX process cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = (
            str(exc)
            if isinstance(exc, InvocationError)
            else f"{type(exc).__name__}: {exc}"
        )
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(
            f"long-lived POSIX process containment failed: {message}",
            output=(
                exc.output
                if isinstance(exc, InvocationError) and exc.output
                else streams.combined_snapshot()
            ),
        ) from exc


def _finish_long_lived_windows_process(
    owner: LongLivedProcess,
) -> tuple[str, str]:
    proc = owner._proc
    streams = owner._streams
    job = owner._job
    assert job is not None
    try:
        if not owner._cancel_requested and proc.poll() is None:
            proc.terminate()
        grace_deadline = time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS
        while job.active_processes() != 0 and time.monotonic() < grace_deadline:
            time.sleep(PROCESS_TERMINATION_POLL_SECONDS)
        if job.active_processes() != 0:
            job.terminate(INFRASTRUCTURE_ERROR_RETURN_CODE)
        job.wait_until_empty(PROCESS_TERMINATION_GRACE_SECONDS)
        owner._containment_sealed = True
        owner._observed_returncode = proc.wait(
            timeout=PROCESS_TERMINATION_GRACE_SECONDS
        )
        stdout, stderr = streams.finish()
        job.close()
        return stdout, stderr
    except BaseException as exc:
        output, cleanup_errors = _cleanup_windows_failure(
            proc,
            streams,
            job,
            assigned=True,
        )
        if job.closed:
            owner._containment_sealed = True
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "long-lived Windows process cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = _describe_windows_failure(exc)
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(
            f"long-lived Windows process containment failed: {message}",
            output=(
                exc.output
                if isinstance(exc, InvocationError) and exc.output
                else output
            ),
        ) from exc


def _run_posix_process(
    proc: subprocess.Popen[bytes],
    streams: _CapturedStreams,
    descendant_tracker: _PosixDescendantTracker,
    timeout_sec: float,
    containment_policy: ProcessContainmentPolicy,
) -> ContainedCommandResult:
    timed_out = False
    try:
        deadline = time.monotonic() + timeout_sec
        leader_exited = False
        while True:
            descendant_tracker.raise_if_failed()
            if _posix_leader_exit_observed(proc):
                leader_exited = True
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(PROCESS_TERMINATION_POLL_SECONDS, remaining))

        initial_records = descendant_tracker.stop_and_audit()
        if leader_exited:
            output_was_open = not streams.all_finished()
            had_live_descendants = any(
                record.identity != descendant_tracker._domain.leader
                for record in initial_records
            )
            descendant_tracker.kill(initial_records)
            return_code, stdout, stderr = _finish_reaped_process(
                proc,
                streams,
                timed_out=False,
            )
            return ContainedCommandResult(
                returncode=return_code,
                stdout=stdout,
                stderr=stderr,
                timed_out=False,
                cleanup_after_exit_performed=(
                    output_was_open or had_live_descendants
                ),
                containment_policy=containment_policy,
                containment_complete=True,
            )

        timed_out = True
        descendant_tracker.terminate(initial_records)
        _, stdout, stderr = _finish_reaped_process(
            proc,
            streams,
            timed_out=True,
        )
        return ContainedCommandResult(
            returncode=TIMEOUT_RETURN_CODE,
            stdout=stdout,
            stderr=stderr,
            timed_out=True,
            cleanup_after_exit_performed=False,
            containment_policy=containment_policy,
            containment_complete=True,
        )
    except BaseException as exc:
        cleanup_errors = _force_posix_cleanup(
            proc,
            streams,
            safe_to_signal_group=not isinstance(exc, _ProcessOwnershipError),
            descendant_tracker=descendant_tracker,
            release_unsealed_anchor=True,
        )
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "POSIX process containment cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = (
            str(exc)
            if isinstance(exc, InvocationError)
            else f"{type(exc).__name__}: {exc}"
        )
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        command_output = streams.combined_snapshot()
        if (
            isinstance(exc, InvocationError)
            and exc.output
            and exc.output != command_output
        ):
            separator = "" if not command_output or command_output.endswith("\n") else "\n"
            command_output += (
                separator
                + "[nebula-containment-diagnostic]\n"
                + exc.output
            )
        raise InvocationError(
            message,
            output=command_output,
            timed_out=(
                (exc.timed_out or timed_out)
                if isinstance(exc, InvocationError)
                else timed_out
            ),
        ) from exc


def _run_windows_process(
    proc: subprocess.Popen[bytes],
    streams: _CapturedStreams,
    job: _WindowsJob,
    timeout_sec: float,
    containment_policy: ProcessContainmentPolicy,
) -> ContainedCommandResult:
    timed_out = False
    try:
        deadline = time.monotonic() + timeout_sec
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(PROCESS_TERMINATION_POLL_SECONDS)

        timed_out = proc.returncode is None
        had_active_processes = job.active_processes() != 0
        if had_active_processes:
            job.terminate(
                TIMEOUT_RETURN_CODE if timed_out else INFRASTRUCTURE_ERROR_RETURN_CODE
            )
        return_code = proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
        job.wait_until_empty(PROCESS_TERMINATION_GRACE_SECONDS)
        stdout, stderr = streams.finish()
        return ContainedCommandResult(
            returncode=TIMEOUT_RETURN_CODE if timed_out else return_code,
            stdout=stdout,
            stderr=stderr,
            timed_out=timed_out,
            cleanup_after_exit_performed=(
                had_active_processes and not timed_out
            ),
            containment_policy=containment_policy,
            containment_complete=True,
        )
    except BaseException as exc:
        if not isinstance(exc, Exception):
            raise
        message = (
            str(exc)
            if isinstance(exc, InvocationError)
            else f"{type(exc).__name__}: {exc}"
        )
        raise InvocationError(
            message,
            output=streams.combined_snapshot(),
            timed_out=(
                (exc.timed_out or timed_out)
                if isinstance(exc, InvocationError)
                else timed_out
            ),
        ) from exc


def _describe_windows_failure(exc: Exception) -> str:
    if isinstance(exc, InvocationError):
        return str(exc)
    if isinstance(exc, subprocess.TimeoutExpired):
        return "Windows process cleanup deadline expired"
    return f"{type(exc).__name__}: {exc}"


def _cleanup_windows_failure(
    proc: subprocess.Popen[bytes],
    streams: _CapturedStreams,
    job: _WindowsJob,
    *,
    assigned: bool,
) -> tuple[str, list[str]]:
    errors: list[str] = []
    job_closed = False
    if assigned:
        try:
            job.terminate(INFRASTRUCTURE_ERROR_RETURN_CODE)
        except OSError as exc:
            errors.append(f"TerminateJobObject failed: {exc}")
            try:
                job.close()
                job_closed = True
            except OSError as close_exc:
                errors.append(f"kill-on-close Job handle close failed: {close_exc}")
                try:
                    proc.kill()
                except OSError as kill_exc:
                    errors.append(f"direct process fallback kill failed: {kill_exc}")
    else:
        try:
            proc.kill()
        except OSError as exc:
            errors.append(f"unassigned suspended process kill failed: {exc}")

    try:
        proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        errors.append("Windows process reap timed out")
        try:
            proc.kill()
        except OSError as exc:
            errors.append(f"process kill after reap timeout failed: {exc}")
        try:
            proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            errors.append("Windows process reap timed out after fallback kill")
        except OSError as exc:
            errors.append(f"Windows process reap after fallback kill failed: {exc}")
    except OSError as exc:
        errors.append(f"Windows process reap failed: {exc}")

    if assigned and not job_closed:
        try:
            job.wait_until_empty(PROCESS_TERMINATION_GRACE_SECONDS)
        except (OSError, InvocationError) as exc:
            errors.append(f"Windows Job drain failed: {exc}")

    if not job_closed:
        try:
            job.close()
            job_closed = True
        except OSError as exc:
            errors.append(f"Job handle close failed: {exc}")

    try:
        streams.finish()
    except InvocationError as exc:
        errors.append(f"output cleanup failed: {exc}")
        try:
            streams.abort()
        except InvocationError as abort_exc:
            errors.append(f"output cancellation failed: {abort_exc}")

    return streams.combined_snapshot(), errors


def _output_with_marker(output: str, marker: str) -> str:
    separator = "" if not output or output.endswith("\n") else "\n"
    return f"{output}{separator}{marker}\n"


def _cleanup_partial_launch(
    proc: subprocess.Popen[bytes],
    stdout_collector: BoundedOutputCollector | None,
    stderr_collector: BoundedOutputCollector | None,
    job: _WindowsJob | None,
    *,
    assigned_to_job: bool,
    descendant_tracker: _PosixDescendantTracker | None = None,
) -> tuple[str, list[str]]:
    errors: list[str] = []
    captured_output: dict[str, str] = {"stdout": "", "stderr": ""}
    job_closed = False

    if os.name == "nt":
        assert job is not None
        if assigned_to_job:
            try:
                job.terminate(INFRASTRUCTURE_ERROR_RETURN_CODE)
            except OSError as exc:
                errors.append(f"partial-launch Job termination failed: {exc}")
            try:
                job.wait_until_empty(PROCESS_TERMINATION_GRACE_SECONDS)
            except (OSError, InvocationError) as exc:
                errors.append(f"partial-launch Job drain failed: {exc}")
        else:
            try:
                proc.kill()
            except OSError as exc:
                errors.append(f"partial-launch suspended process kill failed: {exc}")
        try:
            job.close()
            job_closed = True
        except OSError as exc:
            errors.append(f"partial-launch Job handle close failed: {exc}")
            try:
                proc.kill()
            except OSError as kill_exc:
                errors.append(f"partial-launch process fallback kill failed: {kill_exc}")
    elif proc.returncode is None:
        if descendant_tracker is not None:
            try:
                descendant_tracker.kill()
            except InvocationError as exc:
                errors.append(f"partial-launch descendant cleanup failed: {exc}")
        if descendant_tracker is None or not descendant_tracker.sealed:
            try:
                _signal_posix_process_group(proc, signal.SIGTERM)
            except InvocationError as exc:
                errors.append(f"partial-launch process-group terminate failed: {exc}")
            grace_deadline = time.monotonic() + PROCESS_TERMINATION_GRACE_SECONDS
            try:
                while (
                    _posix_process_group_has_live_members(proc.pid)
                    and time.monotonic() < grace_deadline
                ):
                    time.sleep(PROCESS_TERMINATION_POLL_SECONDS)
            except InvocationError as exc:
                errors.append(f"partial-launch process-group grace audit failed: {exc}")
            try:
                _signal_posix_process_group(proc, signal.SIGKILL)
            except InvocationError as exc:
                errors.append(f"partial-launch process-group kill failed: {exc}")
            try:
                proc.kill()
            except ProcessLookupError:
                pass
            except OSError as leader_exc:
                errors.append(f"partial-launch leader kill failed: {leader_exc}")

    if (
        os.name != "nt"
        and descendant_tracker is not None
        and not descendant_tracker.sealed
    ):
        try:
            _close_posix_anchor(descendant_tracker._domain)
        except InvocationError as exc:
            errors.append(f"partial-launch anchor release failed: {exc}")

    if proc.returncode is None:
        try:
            proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
        except subprocess.TimeoutExpired:
            errors.append("partial-launch process reap timed out")
            try:
                proc.kill()
            except OSError as exc:
                errors.append(f"partial-launch process fallback kill failed: {exc}")
            try:
                proc.wait(timeout=PROCESS_TERMINATION_GRACE_SECONDS)
            except subprocess.TimeoutExpired:
                errors.append("partial-launch process reap timed out after fallback kill")
            except OSError as exc:
                errors.append(
                    f"partial-launch process reap after fallback kill failed: {exc}"
                )
        except OSError as exc:
            errors.append(f"partial-launch process reap failed: {exc}")

    for label, collector, stream in (
        ("stdout", stdout_collector, proc.stdout),
        ("stderr", stderr_collector, proc.stderr),
    ):
        if collector is not None:
            try:
                captured_output[label] = collector.finish()
            except InvocationError as exc:
                captured_output[label] = exc.output or collector.snapshot()
                errors.append(f"partial-launch {label} drain cleanup failed: {exc}")
                try:
                    collector.abort()
                except InvocationError as abort_exc:
                    errors.append(
                        f"partial-launch {label} drain cancellation failed: {abort_exc}"
                    )
        elif stream is not None:
            try:
                stream.close()
            except OSError as exc:
                errors.append(f"partial-launch {label} pipe close failed: {exc}")

    if os.name == "nt" and not job_closed:
        errors.append("partial-launch Windows Job handle ownership was not released")
    stdout = captured_output["stdout"]
    stderr = captured_output["stderr"]
    if stderr:
        separator = "" if not stdout or stdout.endswith("\n") else "\n"
        stdout += f"{separator}[nebula-command-stderr]\n{stderr}"
    return stdout, errors


def _prepare_posix_exec_gate(
    target_argv: list[str],
    launch_env: dict[str, str],
    inherited_anchor_fds: tuple[int, ...],
) -> tuple[_PosixExecGate, tuple[int, ...]]:
    exec_gate = _PosixExecGate(target_argv)
    try:
        new_anchor_fd = exec_gate.cooperative_anchor_fd
        if new_anchor_fd is None:
            raise InvocationError("POSIX launch did not create a cooperative anchor")
        anchor_stack = (*inherited_anchor_fds, new_anchor_fd)
        if len(anchor_stack) > POSIX_CONTAINMENT_ANCHOR_STACK_MAX_DEPTH:
            raise InvocationError("POSIX containment anchor stack reached its depth limit")
        serialized_anchors = ":".join(str(fd) for fd in anchor_stack)
        if (
            len(serialized_anchors.encode("ascii"))
            > POSIX_CONTAINMENT_ANCHOR_STACK_MAX_BYTES
        ):
            raise InvocationError("POSIX containment anchor stack reached its size limit")
        launch_env[POSIX_CONTAINMENT_ANCHOR_FDS_ENV] = serialized_anchors
        pass_fds = tuple(
            dict.fromkeys((*exec_gate.pass_fds, *inherited_anchor_fds))
        )
        return exec_gate, pass_fds
    except BaseException as exc:
        cleanup_errors = exec_gate.close()
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "POSIX exec-gate setup cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = str(exc)
        if cleanup_errors:
            message += "; descriptor cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(message) from exc


def start_long_lived_process(
    cmd: Sequence[str | os.PathLike[str]],
    *,
    containment_policy: ProcessContainmentPolicy,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    max_captured_bytes: int = DEFAULT_MAX_CAPTURED_OUTPUT_BYTES,
    thread_name_prefix: str = "nebula-long-lived",
    stdout_chunk_observer: Callable[[bytes], None] | None = None,
    stderr_chunk_observer: Callable[[bytes], None] | None = None,
) -> LongLivedProcess:
    """Launch under an explicit cooperative-POSIX or OS-enforced-Windows policy.

    The returned owner is the only supported process handle. In particular,
    callers must not wait on the internal ``Popen`` object, because POSIX group
    identity remains safe only while its leader is retained as an unreaped child.
    A POSIX nested spawn that may detach or outlive its parent must pass every fd
    returned by ``cooperative_posix_spawn_pass_fds()`` and retain those descriptors
    for its lifetime. POSIX explicitly rejects ``OS_ENFORCED_RECURSIVE``; token
    discovery is supplemental evidence, not authentication or sandboxing. Windows
    descendants remain constrained by the assigned Job Object.
    """

    if isinstance(cmd, (str, bytes)) or not cmd:
        raise InvocationError(
            "long-lived process command must be a non-empty argument sequence"
        )
    if type(max_captured_bytes) is not int or max_captured_bytes < 1:
        raise InvocationError("long-lived output bound must be at least one byte")
    if not thread_name_prefix:
        raise InvocationError("long-lived output thread prefix must not be empty")
    _require_containment_policy(containment_policy)
    if os.name != "nt":
        _require_posix_primitives()
        if platform.system() not in ("Darwin", "Linux"):
            raise InvocationError(
                "long-lived process containment requires Darwin, Linux, or Windows"
            )

    job = None
    creationflags = 0
    launch_env = env
    containment_token: str | None = None
    inherited_anchor_fds: tuple[int, ...] = ()
    if os.name == "nt":
        try:
            job = _WindowsJob()
        except OSError as exc:
            raise InvocationError(
                f"failed to create long-lived Windows Job Object: {exc}"
            ) from exc
        creationflags = _CREATE_SUSPENDED | getattr(
            subprocess,
            "CREATE_NEW_PROCESS_GROUP",
            0,
        )
    else:
        (
            launch_env,
            containment_token,
            inherited_anchor_fds,
        ) = _prepare_posix_containment_environment(env)

    exec_gate: _PosixExecGate | None = None
    launch_command: Sequence[str | os.PathLike[str]] = cmd
    pass_fds: tuple[int, ...] = ()
    if os.name != "nt":
        exec_gate, pass_fds = _prepare_posix_exec_gate(
            _posix_target_argv(cmd, shell=False),
            launch_env,
            inherited_anchor_fds,
        )
        launch_command = exec_gate.command

    try:
        proc = subprocess.Popen(
            list(launch_command),
            cwd=None if cwd is None else str(cwd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=launch_env,
            start_new_session=(os.name != "nt"),
            creationflags=creationflags,
            pass_fds=pass_fds,
        )
    except BaseException as exc:
        cleanup_errors: list[str] = []
        if exec_gate is not None:
            cleanup_errors.extend(exec_gate.close())
        if job is not None:
            try:
                job.close()
            except OSError as close_exc:
                cleanup_errors.append(f"Job handle close failed: {close_exc}")
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "long-lived process launch cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = f"failed to start long-lived process: {type(exc).__name__}: {exc}"
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(message) from exc

    assigned_to_job = False
    descendant_tracker: _PosixDescendantTracker | None = None
    stdout_collector: BoundedOutputCollector | None = None
    stderr_collector: BoundedOutputCollector | None = None
    try:
        if exec_gate is not None:
            exec_gate.parent_after_launch()
        if proc.stdout is None or proc.stderr is None:
            raise InvocationError("long-lived process output pipes were not created")
        if os.name == "nt":
            assert job is not None
            job.assign(proc)
            assigned_to_job = True
        else:
            if containment_token is None:
                raise InvocationError("POSIX launch did not create a containment token")
            descendant_tracker = _PosixDescendantTracker.start(
                proc,
                containment_token,
                exec_gate.detach_posix_anchor(),
            )
        stdout_collector = BoundedOutputCollector(
            proc.stdout,
            max_captured_bytes=max_captured_bytes,
            thread_name=f"{thread_name_prefix}-stdout",
            chunk_observer=stdout_chunk_observer,
        )
        stderr_collector = BoundedOutputCollector(
            proc.stderr,
            max_captured_bytes=max_captured_bytes,
            thread_name=f"{thread_name_prefix}-stderr",
            chunk_observer=stderr_chunk_observer,
        )
        stdout_collector.start()
        stderr_collector.start()
        if exec_gate is not None:
            exec_gate.release_and_verify_exec()
        streams = _CapturedStreams(stdout_collector, stderr_collector)
        owner = LongLivedProcess(
            proc,
            streams,
            job,
            descendant_tracker,
            containment_policy,
        )
        if os.name == "nt":
            _resume_windows_process(proc)
        return owner
    except BaseException as exc:
        gate_cleanup_errors = [] if exec_gate is None else exec_gate.close()
        cleanup_output, cleanup_errors = _cleanup_partial_launch(
            proc,
            stdout_collector,
            stderr_collector,
            job,
            assigned_to_job=assigned_to_job,
            descendant_tracker=descendant_tracker,
        )
        cleanup_errors = [*gate_cleanup_errors, *cleanup_errors]
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "long-lived partial launch cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = (
            "failed to initialize long-lived process containment: "
            f"{type(exc).__name__}: {exc}"
        )
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(message, output=cleanup_output) from exc


def run_contained_command(
    cmd: list[str] | str,
    *,
    cwd: Path | None,
    shell: bool,
    env: dict[str, str],
    timeout_sec: float,
    containment_policy: ProcessContainmentPolicy,
    combine_stderr: bool = True,
    max_captured_bytes: int = DEFAULT_MAX_CAPTURED_OUTPUT_BYTES,
) -> ContainedCommandResult:
    if (
        isinstance(timeout_sec, bool)
        or not isinstance(timeout_sec, (int, float))
        or not math.isfinite(timeout_sec)
        or timeout_sec <= 0
    ):
        raise InvocationError("command timeout must be a finite positive number")
    if type(max_captured_bytes) is not int or max_captured_bytes < 1:
        raise InvocationError("command output bound must be at least one byte")
    _require_containment_policy(containment_policy)
    if os.name != "nt":
        _require_posix_primitives()

    job = None
    creationflags = 0
    launch_env = env
    containment_token: str | None = None
    inherited_anchor_fds: tuple[int, ...] = ()
    if os.name == "nt":
        try:
            job = _WindowsJob()
        except OSError as exc:
            raise InvocationError(f"failed to create Windows Job Object: {exc}") from exc
        creationflags = _CREATE_SUSPENDED | getattr(
            subprocess,
            "CREATE_NEW_PROCESS_GROUP",
            0,
        )
    else:
        (
            launch_env,
            containment_token,
            inherited_anchor_fds,
        ) = _prepare_posix_containment_environment(env)

    exec_gate: _PosixExecGate | None = None
    launch_command: list[str] | str = cmd
    launch_shell = shell
    pass_fds: tuple[int, ...] = ()
    if os.name != "nt":
        exec_gate, pass_fds = _prepare_posix_exec_gate(
            _posix_target_argv(cmd, shell=shell),
            launch_env,
            inherited_anchor_fds,
        )
        launch_command = exec_gate.command
        launch_shell = False

    stderr_target: int = subprocess.STDOUT if combine_stderr else subprocess.PIPE
    try:
        proc = subprocess.Popen(
            launch_command,
            cwd=None if cwd is None else str(cwd),
            shell=launch_shell,
            stdout=subprocess.PIPE,
            stderr=stderr_target,
            env=launch_env,
            start_new_session=(os.name != "nt"),
            creationflags=creationflags,
            pass_fds=pass_fds,
        )
    except BaseException as exc:
        cleanup_errors: list[str] = []
        if exec_gate is not None:
            cleanup_errors.extend(exec_gate.close())
        if job is not None:
            try:
                job.close()
            except OSError as close_exc:
                cleanup_errors.append(f"Job handle close failed: {close_exc}")
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note("process launch cleanup failures: " + "; ".join(cleanup_errors))
            raise
        message = f"failed to start command: {type(exc).__name__}: {exc}"
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(message) from exc

    assigned_to_job = False
    descendant_tracker: _PosixDescendantTracker | None = None
    stdout_collector: BoundedOutputCollector | None = None
    stderr_collector: BoundedOutputCollector | None = None
    try:
        if exec_gate is not None:
            exec_gate.parent_after_launch()
        if proc.stdout is None or (not combine_stderr and proc.stderr is None):
            raise InvocationError("subprocess output pipes were not created")
        if os.name == "nt":
            assert job is not None
            job.assign(proc)
            assigned_to_job = True
        else:
            if containment_token is None:
                raise InvocationError("POSIX launch did not create a containment token")
            descendant_tracker = _PosixDescendantTracker.start(
                proc,
                containment_token,
                exec_gate.detach_posix_anchor(),
            )
        stdout_collector = BoundedOutputCollector(
            proc.stdout,
            max_captured_bytes=max_captured_bytes,
            thread_name="nebula-command-stdout",
        )
        stdout_collector.start()
        if not combine_stderr:
            assert proc.stderr is not None
            stderr_collector = BoundedOutputCollector(
                proc.stderr,
                max_captured_bytes=max_captured_bytes,
                thread_name="nebula-command-stderr",
            )
            stderr_collector.start()
        if exec_gate is not None:
            exec_gate.release_and_verify_exec()
        streams = _CapturedStreams(stdout_collector, stderr_collector)
    except BaseException as exc:
        gate_cleanup_errors = [] if exec_gate is None else exec_gate.close()
        cleanup_output, cleanup_errors = _cleanup_partial_launch(
            proc,
            stdout_collector,
            stderr_collector,
            job,
            assigned_to_job=assigned_to_job,
            descendant_tracker=descendant_tracker,
        )
        cleanup_errors = [*gate_cleanup_errors, *cleanup_errors]
        if not isinstance(exc, Exception):
            if cleanup_errors:
                exc.add_note(
                    "partial process launch cleanup failures: "
                    + "; ".join(cleanup_errors)
                )
            raise
        message = f"failed to initialize command containment: {type(exc).__name__}: {exc}"
        if cleanup_errors:
            message += "; cleanup failures: " + "; ".join(cleanup_errors)
        raise InvocationError(message, output=cleanup_output) from exc

    if os.name != "nt":
        if descendant_tracker is None:
            raise InvocationError("POSIX command has no descendant tracker")
        result = _run_posix_process(
            proc,
            streams,
            descendant_tracker,
            timeout_sec,
            containment_policy,
        )
    else:
        assert job is not None
        try:
            _resume_windows_process(proc)
            result = _run_windows_process(
                proc,
                streams,
                job,
                timeout_sec,
                containment_policy,
            )
        except BaseException as exc:
            output, cleanup_errors = _cleanup_windows_failure(
                proc,
                streams,
                job,
                assigned=assigned_to_job,
            )
            if not isinstance(exc, Exception):
                if cleanup_errors:
                    exc.add_note(
                        "Windows process containment cleanup failures: "
                        + "; ".join(cleanup_errors)
                    )
                raise
            message = _describe_windows_failure(exc)
            if cleanup_errors:
                message += "; cleanup failures: " + "; ".join(cleanup_errors)
            raise InvocationError(
                f"Windows process containment failed: {message}",
                output=(exc.output if isinstance(exc, InvocationError) and exc.output else output),
                timed_out=(exc.timed_out if isinstance(exc, InvocationError) else False),
            ) from exc
        try:
            job.close()
        except OSError as exc:
            raise InvocationError(
                f"failed to close completed Windows Job Object: {exc}",
                output=result.stdout + result.stderr,
                timed_out=result.timed_out,
            ) from exc

    marker = ""
    if result.timed_out:
        marker = (
            f"[nebula-test-timeout] command timed out after {timeout_sec:g}s; "
            + ("terminated Windows Job Object" if os.name == "nt" else "sealed process group")
        )
    elif result.cleanup_after_exit_performed:
        marker = (
            "[nebula-test-cleanup] sealed Windows Job Object after command exit"
            if os.name == "nt"
            else (
                "[nebula-test-cleanup] sealed POSIX descendant domain after command exit"
            )
        )
    if not marker:
        return result
    if combine_stderr:
        return ContainedCommandResult(
            result.returncode,
            _output_with_marker(result.stdout, marker),
            result.stderr,
            result.timed_out,
            result.cleanup_after_exit_performed,
            result.containment_policy,
            result.containment_complete,
        )
    return ContainedCommandResult(
        result.returncode,
        result.stdout,
        _output_with_marker(result.stderr, marker),
        result.timed_out,
        result.cleanup_after_exit_performed,
        result.containment_policy,
        result.containment_complete,
    )
