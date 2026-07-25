from __future__ import annotations

import os
import json
import signal
import socket
import subprocess
import sys
import threading
import time
import tracemalloc
from pathlib import Path


BOUNDED_STREAM_BYTES = 16 * 1024 * 1024
BOUNDED_CAPTURE_BYTES = 4096
PIPE_STREAM_BYTES = 4 * 1024 * 1024


def _write_split_event(path: Path) -> int:
    record = b'prefix {"event":"listener_bound","port":43210}\n'
    split = record.index(b"port") + 2
    with path.open("wb", buffering=0) as handle:
        handle.write(record[:split])
        os.fsync(handle.fileno())
        time.sleep(0.2)
        handle.write(record[split:])
        os.fsync(handle.fileno())
    return 0


def _write_bounded_output() -> int:
    chunk = b"x" * (64 * 1024)
    sys.stdout.buffer.write(b"stdout-head\n")
    sys.stderr.buffer.write(b"stderr-head\n")
    for _ in range(BOUNDED_STREAM_BYTES // len(chunk)):
        sys.stdout.buffer.write(chunk)
        sys.stderr.buffer.write(chunk)
    sys.stdout.buffer.write(b"\nstdout-tail\n")
    sys.stderr.buffer.write(b"\nstderr-tail\n")
    sys.stdout.buffer.flush()
    sys.stderr.buffer.flush()
    return 0


def _write_partial_pipe() -> int:
    sys.stderr.buffer.write(b'prefix {"event":"listener_bound","port":')
    sys.stderr.buffer.flush()
    time.sleep(2.0)
    return 0


def _write_oversized_pipe() -> int:
    chunk = b"x" * (64 * 1024)
    for _ in range(20):
        sys.stderr.buffer.write(chunk)
        sys.stderr.buffer.flush()
    time.sleep(2.0)
    return 0


def _write_bounded_pipe_event() -> int:
    stdout_chunk = b"s" * (64 * 1024)
    stderr_noise = b"noise-record\n" * 4096
    for _ in range(PIPE_STREAM_BYTES // len(stdout_chunk)):
        sys.stdout.buffer.write(stdout_chunk)
    sys.stdout.buffer.flush()
    for _ in range(12):
        sys.stderr.buffer.write(stderr_noise)
    sys.stderr.buffer.write(b'{"event":"listener_')
    sys.stderr.buffer.flush()
    time.sleep(0.05)
    sys.stderr.buffer.write(b'bound","port":43213}\n')
    for _ in range(12):
        sys.stderr.buffer.write(stderr_noise)
    sys.stderr.buffer.flush()
    time.sleep(0.5)
    return 0


def _write_port_event(port_token: str) -> int:
    if port_token not in ("true", "0", "65536"):
        raise RuntimeError(f"invalid port token: {port_token!r}")
    sys.stderr.buffer.write(
        b'{"event":"listener_bound","port":'
        + port_token.encode("ascii")
        + b"}\n"
    )
    sys.stderr.buffer.flush()
    time.sleep(2.0)
    return 0


def _write_listener_then_exit() -> int:
    sys.stderr.buffer.write(b'{"event":"listener_bound","port":43214}\n')
    sys.stderr.buffer.flush()
    return 0


def _write_noise_then_exit() -> int:
    sys.stderr.buffer.write(b"unterminated-noise")
    sys.stderr.buffer.flush()
    return 0


def _ignore_termination_request() -> None:
    if os.name != "nt":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)


def _write_process_tree_descendant(parent_pid: int, parent_port: int) -> int:
    _ignore_termination_request()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        payload = {
            "event": "listener_bound",
            "port": listener.getsockname()[1],
            "parent_pid": parent_pid,
            "descendant_pid": os.getpid(),
            "parent_port": parent_port,
        }
        sys.stderr.write(json.dumps(payload, separators=(",", ":")) + "\n")
        sys.stderr.flush()
        while True:
            time.sleep(1.0)


def _write_process_tree() -> int:
    _ignore_termination_request()
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        descendant = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "write-process-tree-descendant",
                str(os.getpid()),
                str(listener.getsockname()[1]),
            ],
            stdin=subprocess.DEVNULL,
        )
        try:
            while True:
                time.sleep(1.0)
        finally:
            if descendant.poll() is None:
                descendant.kill()
            descendant.wait(timeout=5)


def _run_bounded_output_probe() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from process_containment import (
        BoundedOutputCollector,
        InvocationError,
        ProcessContainmentPolicy,
        run_contained_command,
    )
    from service_probe import (
        ObserveEventMonitor,
        ServiceProbeCleanupError,
        ServiceProbeWaitError,
    )

    started = time.monotonic()
    result = run_contained_command(
        [sys.executable, str(Path(__file__).resolve()), "write-bounded-output"],
        cwd=Path.cwd(),
        shell=False,
        env=os.environ.copy(),
        timeout_sec=10.0,
        containment_policy=ProcessContainmentPolicy.TRUSTED_COOPERATIVE,
        combine_stderr=False,
        max_captured_bytes=BOUNDED_CAPTURE_BYTES,
    )
    elapsed = time.monotonic() - started
    if result.returncode != 0 or result.timed_out:
        raise RuntimeError(f"bounded output command failed: {result!r}")
    if elapsed >= 10.0:
        raise RuntimeError(f"bounded output drain approached its deadline: {elapsed:.3f}s")
    expected_fragments = (
        (result.stdout, "stdout-head", "stdout-tail"),
        (result.stderr, "stderr-head", "stderr-tail"),
    )
    for output, head, tail in expected_fragments:
        if (
            head not in output
            or tail not in output
            or "[nebula-output-truncated] omitted" not in output
        ):
            raise RuntimeError(f"bounded output lost its head/tail contract: {output!r}")
        if len(output.encode("utf-8")) > BOUNDED_CAPTURE_BYTES + 160:
            raise RuntimeError(
                "bounded output retained more than its configured capture plus metadata: "
                f"{len(output.encode('utf-8'))} bytes"
            )

    drain_threads = [
        thread.name
        for thread in threading.enumerate()
        if thread.name.startswith("nebula-command-")
    ]
    if drain_threads:
        raise RuntimeError(f"command output drain threads leaked: {drain_threads!r}")

    for invalid_bound in (1.5, True):
        try:
            run_contained_command(
                [sys.executable, "-c", "print('should-not-run')"],
                cwd=Path.cwd(),
                shell=False,
                env=os.environ.copy(),
                timeout_sec=1.0,
                containment_policy=ProcessContainmentPolicy.TRUSTED_COOPERATIVE,
                max_captured_bytes=invalid_bound,
            )
        except InvocationError as exc:
            if "output bound" not in str(exc):
                raise RuntimeError(f"invalid output-bound diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError(f"invalid output bound was accepted: {invalid_bound!r}")

    interrupted_read_fd, interrupted_write_fd = os.pipe()
    interrupted_stream = os.fdopen(interrupted_read_fd, "rb", buffering=0)
    interrupted_collector = BoundedOutputCollector(
        interrupted_stream,
        max_captured_bytes=128,
        thread_name="nebula-interrupted-start-drain",
    )
    original_thread_start = threading.Thread.start

    def interrupt_before_thread_start(thread: threading.Thread) -> None:
        raise KeyboardInterrupt("injected before native thread creation")

    threading.Thread.start = interrupt_before_thread_start
    try:
        try:
            interrupted_collector.start()
        except KeyboardInterrupt:
            pass
        else:
            raise RuntimeError("interrupted collector start unexpectedly succeeded")
    finally:
        threading.Thread.start = original_thread_start
    interrupted_collector.abort()
    os.close(interrupted_write_fd)
    if not interrupted_stream.closed:
        raise RuntimeError("interrupted collector start leaked its read pipe")

    failed_read_fd, failed_write_fd = os.pipe()
    failed_stream = os.fdopen(failed_read_fd, "rb", buffering=0)
    failed_collector = BoundedOutputCollector(
        failed_stream,
        max_captured_bytes=128,
        thread_name="nebula-failed-worker-drain",
    )

    def fail_retain(chunk: bytes) -> None:
        raise RuntimeError(f"injected retain failure for {len(chunk)} bytes")

    failed_collector._retain = fail_retain
    failed_collector.start()
    os.write(failed_write_fd, b"worker-failure")
    os.close(failed_write_fd)
    try:
        failed_collector.finish()
    except InvocationError as exc:
        if "injected retain failure" not in str(exc):
            raise RuntimeError(f"collector worker failure lost context: {exc}") from exc
    else:
        raise RuntimeError("collector worker exception was reported as success")

    monitor_read_fd, monitor_write_fd = os.pipe()
    monitor_stream = os.fdopen(monitor_read_fd, "rb", buffering=0)
    event_monitor = ObserveEventMonitor("listener_bound")
    monitor_collector = BoundedOutputCollector(
        monitor_stream,
        max_captured_bytes=4096,
        thread_name="nebula-observe-monitor-drain",
        chunk_observer=event_monitor.feed,
    )
    monitor_collector.start()

    def write_middle_event() -> None:
        noise = b"noise-record\n" * 5000
        for _ in range(24):
            os.write(monitor_write_fd, noise)
        os.write(
            monitor_write_fd,
            b'\n{"event":"listener_bound","port":43212}\n',
        )
        for _ in range(24):
            os.write(monitor_write_fd, noise)
        os.close(monitor_write_fd)

    monitor_writer = threading.Thread(
        target=write_middle_event,
        name="nebula-observe-monitor-writer",
    )
    monitor_writer.start()
    monitor_output = monitor_collector.finish()
    monitor_writer.join(timeout=5)
    if monitor_writer.is_alive():
        raise RuntimeError("observe monitor writer thread did not finish")
    monitored_payload = event_monitor.payload()
    if monitored_payload is None or monitored_payload.get("port") != 43212:
        raise RuntimeError(
            f"stream monitor lost an event outside diagnostic head/tail: {monitored_payload!r}"
        )
    if '"listener_bound"' in monitor_output:
        raise RuntimeError("observe monitor regression did not place the event outside head/tail")

    if os.name != "nt":
        import resource

        original_limit = resource.getrlimit(resource.RLIMIT_NOFILE)
        soft_limit, hard_limit = original_limit
        desired_soft_limit = max(soft_limit, 1152)
        if hard_limit != resource.RLIM_INFINITY:
            desired_soft_limit = min(desired_soft_limit, hard_limit)
        if desired_soft_limit > soft_limit:
            resource.setrlimit(
                resource.RLIMIT_NOFILE,
                (desired_soft_limit, hard_limit),
            )

        owned_fds: set[int] = set()
        target_read_fd = -1
        target_write_fd = -1
        target_stream = None
        high_fd_collector: BoundedOutputCollector | None = None
        try:
            while target_read_fd <= 1050 and len(owned_fds) + 2 < desired_soft_limit:
                read_fd, write_fd = os.pipe()
                owned_fds.update((read_fd, write_fd))
                target_read_fd = read_fd
                target_write_fd = write_fd
            if target_read_fd <= 1024:
                raise RuntimeError(
                    "host file-descriptor limit was too low for the high-fd collector contract: "
                    f"soft={desired_soft_limit} highest={target_read_fd}"
                )
            for fd in tuple(owned_fds):
                if fd not in (target_read_fd, target_write_fd):
                    os.close(fd)
                    owned_fds.remove(fd)
            target_stream = os.fdopen(target_read_fd, "rb", buffering=0)
            owned_fds.remove(target_read_fd)
            high_fd_collector = BoundedOutputCollector(
                target_stream,
                max_captured_bytes=128,
                thread_name="nebula-high-fd-drain",
            )
            high_fd_collector.start()
            os.write(target_write_fd, b"high-fd-ok")
            os.close(target_write_fd)
            owned_fds.remove(target_write_fd)
            if high_fd_collector.finish() != "high-fd-ok":
                raise RuntimeError("high-fd collector output drifted")
            target_stream = None
        finally:
            cleanup_errors: list[str] = []
            if high_fd_collector is not None and not high_fd_collector.is_finished():
                try:
                    high_fd_collector.abort()
                except InvocationError as exc:
                    cleanup_errors.append(f"collector: {exc}")
            if target_stream is not None:
                try:
                    target_stream.close()
                except OSError as exc:
                    cleanup_errors.append(f"target stream: {exc}")
            for fd in owned_fds:
                try:
                    os.close(fd)
                except OSError as exc:
                    cleanup_errors.append(f"fd {fd}: {exc}")
            resource.setrlimit(resource.RLIMIT_NOFILE, original_limit)
            if cleanup_errors:
                raise RuntimeError(
                    "high-fd collector probe cleanup failed: " + "; ".join(cleanup_errors)
                )

    import release_control_plane_workspace as workspace_module

    original_build_service_binary = workspace_module.build_service_binary
    original_workspace_start = workspace_module.start_captured_process
    original_await_listener_bound = workspace_module.await_listener_bound
    spawned_services: list[object] = []

    def fake_build_service_binary(*args: object, **kwargs: object) -> Path:
        return Path(sys.executable)

    def fail_await_listener_bound(*args: object, **kwargs: object):
        raise ServiceProbeWaitError("injected readiness observer failure")

    def fake_service_start(
        args: object,
        event_name: str,
        **kwargs: object,
    ):
        proc, output = original_workspace_start(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            event_name,
            **kwargs,
        )
        spawned_services.append(proc)
        return proc, output

    workspace_module.build_service_binary = fake_build_service_binary
    workspace_module.start_captured_process = fake_service_start
    workspace_module.await_listener_bound = fail_await_listener_bound
    try:
        try:
            workspace_module.start_service("ignored", Path.cwd())
        except workspace_module.ServiceStartupError as exc:
            if "injected readiness observer failure" not in str(exc):
                raise RuntimeError(f"service startup diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("service readiness observer failure was silently accepted")
    finally:
        workspace_module.build_service_binary = original_build_service_binary
        workspace_module.start_captured_process = original_workspace_start
        workspace_module.await_listener_bound = original_await_listener_bound
    if len(spawned_services) != 1 or spawned_services[0].poll() is None:
        raise RuntimeError("service startup failure leaked its child process")

    original_finish_process_output = workspace_module.finish_process_output

    def fail_finish_process_output(*args: object, **kwargs: object):
        raise ServiceProbeCleanupError(
            "injected cleanup failure",
            stdout="cleanup-stdout\n",
            stderr="cleanup-stderr\n",
        )

    workspace_module.finish_process_output = fail_finish_process_output
    try:
        try:
            raise RuntimeError("primary test failure")
        finally:
            workspace_module.terminate_process(
                object(),
                object(),
            )
    except RuntimeError as exc:
        notes = getattr(exc, "__notes__", [])
        if str(exc) != "primary test failure" or not any(
            "injected cleanup failure" in note for note in notes
        ):
            raise RuntimeError(
                f"service cleanup did not preserve and annotate the primary error: {exc!r}"
            ) from exc
    else:
        raise RuntimeError("service cleanup masked the primary error")
    finally:
        workspace_module.finish_process_output = original_finish_process_output

    original_http_connection = workspace_module.http.client.HTTPConnection

    class FakeHttpResponse:
        status = 200

        def __init__(self, payload: bytes, delay: float = 0.0):
            self._payload = payload
            self._offset = 0
            self._delay = delay

        def read(self, amount: int) -> bytes:
            if self._delay:
                time.sleep(self._delay)
            chunk = self._payload[self._offset : self._offset + amount]
            self._offset += len(chunk)
            return chunk

        def getheaders(self) -> list[tuple[str, str]]:
            return []

    class FakeHttpConnection:
        sock = None
        response_payload = b""
        response_delay = 0.0

        def __init__(self, host: str, port: int, timeout: float):
            self._response = FakeHttpResponse(
                self.response_payload,
                self.response_delay,
            )

        def request(
            self,
            method: str,
            path: str,
            body: bytes | None,
            headers: dict[str, str],
        ) -> None:
            return

        def getresponse(self) -> FakeHttpResponse:
            return self._response

        def close(self) -> None:
            return

    workspace_module.http.client.HTTPConnection = FakeHttpConnection
    try:
        FakeHttpConnection.response_payload = b"x" * 65
        FakeHttpConnection.response_delay = 0.0
        try:
            workspace_module.http_request(
                "127.0.0.1",
                1,
                "GET",
                "/",
                max_response_bytes=64,
            )
        except workspace_module.HttpResponseLimitError:
            pass
        else:
            raise RuntimeError("oversized HTTP response was accepted")

        FakeHttpConnection.response_payload = b"ok"
        FakeHttpConnection.response_delay = 0.02
        try:
            workspace_module.http_request(
                "127.0.0.1",
                1,
                "GET",
                "/",
                timeout=0.01,
            )
        except TimeoutError:
            pass
        else:
            raise RuntimeError("HTTP total deadline was not enforced")
    finally:
        workspace_module.http.client.HTTPConnection = original_http_connection

    leaked_drain_threads = [
        thread.name
        for thread in threading.enumerate()
        if thread.name.startswith("nebula-")
        and ("drain" in thread.name or thread.name.startswith("nebula-service-"))
    ]
    if leaked_drain_threads:
        raise RuntimeError(f"bounded output drain threads leaked: {leaked_drain_threads!r}")
    print("service-probe-bounded-output-ok")
    return 0


def _run_pipe_probe() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    import service_probe as probe_module

    fixture_path = str(Path(__file__).resolve())

    def spawn(
        mode: str,
        *args: str,
        max_captured_bytes: int = BOUNDED_CAPTURE_BYTES,
        thread_name_prefix: str,
    ):
        return probe_module.start_process(
            [sys.executable, fixture_path, mode, *args],
            "listener_bound",
            max_captured_bytes=max_captured_bytes,
            thread_name_prefix=thread_name_prefix,
        )

    partial_proc, partial_output = spawn(
        "write-partial-pipe",
        thread_name_prefix="nebula-pipe-partial",
    )
    original_timeout_override = os.environ.get("NEBULA_SERVICE_START_TIMEOUT")
    os.environ["NEBULA_SERVICE_START_TIMEOUT"] = "30"
    partial_started = time.monotonic()
    try:
        try:
            probe_module.wait_for_listener_bound(
                partial_proc,
                partial_output,
                timeout=0.2,
                timeout_mode="exact",
            )
        except SystemExit as exc:
            if "timed out waiting for listener_bound" not in str(exc):
                raise RuntimeError(f"partial-pipe timeout diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("unterminated pipe record unexpectedly became an event")
    finally:
        if original_timeout_override is None:
            del os.environ["NEBULA_SERVICE_START_TIMEOUT"]
        else:
            os.environ["NEBULA_SERVICE_START_TIMEOUT"] = original_timeout_override
        probe_module.terminate_process(partial_proc, partial_output)
    partial_elapsed = time.monotonic() - partial_started
    if partial_elapsed >= 1.0:
        raise RuntimeError(
            "partial pipe read crossed its observe deadline: "
            f"{partial_elapsed:.3f}s"
        )
    if partial_proc.returncode is None:
        raise RuntimeError("partial pipe timeout did not reap its child")
    if not partial_output.stdout.is_finished():
        raise RuntimeError("partial pipe timeout leaked stdout drain")
    if not partial_output.stderr.is_finished():
        raise RuntimeError("partial pipe timeout leaked stderr drain")

    bounded_proc, bounded_output = spawn(
        "write-bounded-pipe-event",
        thread_name_prefix="nebula-pipe-bounded",
    )
    bounded_payload = probe_module.wait_for_listener_bound(
        bounded_proc,
        bounded_output,
        timeout=5.0,
    )[0]
    if bounded_payload.get("port") != 43213:
        raise RuntimeError(f"split pipe event was not reconstructed: {bounded_payload!r}")
    if bounded_proc.wait(timeout=5) != 0:
        raise RuntimeError("bounded pipe writer failed")

    if hasattr(bounded_proc, "communicate"):
        raise RuntimeError("contained service owner exposed Popen.communicate")
    bounded_stdout, bounded_stderr = probe_module.finish_process_output(
        bounded_proc,
        bounded_output,
    )
    for label, collector, captured in (
        ("stdout", bounded_output.stdout, bounded_stdout),
        ("stderr", bounded_output.stderr, bounded_stderr),
    ):
        if collector.total_bytes <= BOUNDED_CAPTURE_BYTES:
            raise RuntimeError(f"{label} pipe probe did not exceed its capture bound")
        if collector.retained_bytes > BOUNDED_CAPTURE_BYTES:
            raise RuntimeError(f"{label} pipe collector retained too many bytes")
        if "[nebula-output-truncated] omitted" not in captured:
            raise RuntimeError(f"{label} pipe capture omitted no truncation marker")
        if len(captured.encode("utf-8")) > BOUNDED_CAPTURE_BYTES + 160:
            raise RuntimeError(f"{label} pipe diagnostic exceeded its bounded contract")
    if '"listener_bound"' in bounded_stderr:
        raise RuntimeError("pipe event was not placed outside diagnostic head/tail")

    oversized_proc, oversized_output = spawn(
        "write-oversized-pipe",
        thread_name_prefix="nebula-pipe-oversized",
    )
    oversized_started = time.monotonic()
    try:
        try:
            probe_module.wait_for_listener_bound(
                oversized_proc,
                oversized_output,
                timeout=5.0,
            )
        except SystemExit as exc:
            if "observe record exceeded" not in str(exc):
                raise RuntimeError(f"oversized pipe diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("oversized unterminated pipe record was accepted")
    finally:
        probe_module.terminate_process(oversized_proc, oversized_output)
    if time.monotonic() - oversized_started >= 2.0:
        raise RuntimeError("oversized pipe record did not fail fast")

    original_collector_start = probe_module.BoundedOutputCollector.start
    import process_containment as containment_module

    original_containment_popen = containment_module.subprocess.Popen
    setup_processes: list[subprocess.Popen[bytes]] = []
    start_calls = 0

    def fail_second_collector_start(self):
        nonlocal start_calls
        start_calls += 1
        if start_calls == 2:
            raise RuntimeError("injected second collector start failure")
        return original_collector_start(self)

    def recording_popen(*args: object, **kwargs: object):
        proc = original_containment_popen(*args, **kwargs)
        setup_processes.append(proc)
        return proc

    probe_module.BoundedOutputCollector.start = fail_second_collector_start
    containment_module.subprocess.Popen = recording_popen
    try:
        try:
            probe_module.start_process(
                [sys.executable, fixture_path, "write-partial-pipe"],
                "listener_bound",
                thread_name_prefix="nebula-pipe-setup-failure",
            )
        except probe_module.ServiceProbeSetupError as exc:
            if "injected second collector start failure" not in str(exc.__cause__):
                raise RuntimeError(f"capture setup failure lost its cause: {exc}") from exc
        else:
            raise RuntimeError("second collector start failure was silently accepted")
    finally:
        probe_module.BoundedOutputCollector.start = original_collector_start
        containment_module.subprocess.Popen = original_containment_popen
    if len(setup_processes) != 1:
        raise RuntimeError(f"capture setup launched unexpected processes: {len(setup_processes)}")
    setup_proc = setup_processes[0]
    if setup_proc.poll() is None:
        raise RuntimeError("capture setup failure leaked its child")
    if setup_proc.stdout is None or not setup_proc.stdout.closed:
        raise RuntimeError("capture setup failure leaked stdout")
    if setup_proc.stderr is None or not setup_proc.stderr.closed:
        raise RuntimeError("capture setup failure leaked stderr")

    try:
        probe_module.start_process(
            [sys.executable, fixture_path, "write-partial-pipe"],
            "listener_bound",
            thread_name_prefix="",
        )
    except probe_module.ServiceProbeSetupError as exc:
        if "thread prefix" not in str(exc.__cause__):
            raise RuntimeError(f"empty-prefix rejection diagnostic drifted: {exc}") from exc
    else:
        raise RuntimeError("empty service-output thread prefix was accepted")

    original_probe_finish = probe_module.finish_process_output

    def fail_probe_finish(*args: object, **kwargs: object):
        raise probe_module.ServiceProbeCleanupError(
            "injected direct cleanup failure",
            stdout="direct-cleanup-stdout\n",
            stderr="direct-cleanup-stderr\n",
        )

    probe_module.finish_process_output = fail_probe_finish
    try:
        try:
            raise RuntimeError("direct primary failure")
        finally:
            probe_module.terminate_process(
                object(),
                object(),
            )
    except RuntimeError as exc:
        notes = getattr(exc, "__notes__", [])
        if str(exc) != "direct primary failure" or not any(
            "injected direct cleanup failure" in note for note in notes
        ):
            raise RuntimeError(
                f"direct cleanup did not preserve and annotate the primary error: {exc!r}"
            ) from exc
    else:
        raise RuntimeError("direct service cleanup masked the primary error")
    finally:
        probe_module.finish_process_output = original_probe_finish

    for mode, expected_fragment in (
        ("write-listener-then-exit", "service exited"),
        ("write-noise-then-exit", "service"),
    ):
        exited_proc, exited_output = spawn(
            mode,
            thread_name_prefix=f"nebula-pipe-{mode}",
        )
        exited_proc.wait(timeout=5)
        try:
            probe_module.wait_for_listener_bound(
                exited_proc,
                exited_output,
                timeout=1.0,
            )
        except SystemExit as exc:
            if expected_fragment not in str(exc):
                raise RuntimeError(f"exited-service diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError(f"exited service was treated as ready: {mode}")
        finally:
            probe_module.terminate_process(exited_proc, exited_output)

    if os.name != "nt":
        ownership_proc, ownership_output = spawn(
            "write-noise-then-exit",
            thread_name_prefix="nebula-pipe-ownership-loss",
        )
        ownership_proc._proc.wait(timeout=5)
        try:
            probe_module.finish_process_output(ownership_proc, ownership_output)
        except probe_module.ServiceProbeCleanupError as exc:
            if "lost ownership of process-group leader" not in str(exc):
                raise RuntimeError(
                    f"process ownership-loss diagnostic drifted: {exc}"
                ) from exc
        else:
            raise RuntimeError("reaped process-group leader ownership loss was accepted")

    for port_token in ("true", "0", "65536"):
        port_proc, port_output = spawn(
            "write-port-event",
            port_token,
            thread_name_prefix=f"nebula-pipe-port-{port_token}",
        )
        try:
            probe_module.wait_for_listener_bound(
                port_proc,
                port_output,
                timeout=1.0,
            )
        except SystemExit as exc:
            if "invalid listener_bound port payload" not in str(exc):
                raise RuntimeError(f"invalid-port diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError(f"invalid listener port was accepted: {port_token}")
        finally:
            probe_module.terminate_process(port_proc, port_output)

    tree_proc, tree_output = spawn(
        "write-process-tree",
        thread_name_prefix="nebula-pipe-tree",
    )
    tree_payload = probe_module.wait_for_listener_bound(
        tree_proc,
        tree_output,
        timeout=5.0,
    )[0]
    parent_pid = tree_payload.get("parent_pid")
    descendant_pid = tree_payload.get("descendant_pid")
    parent_port = tree_payload.get("parent_port")
    descendant_port = tree_payload.get("port")
    if parent_pid != tree_proc.pid:
        raise RuntimeError(f"process-tree parent identity drifted: {tree_payload!r}")
    for label, value in (
        ("descendant_pid", descendant_pid),
        ("parent_port", parent_port),
        ("descendant_port", descendant_port),
    ):
        if type(value) is not int or value <= 0:
            raise RuntimeError(f"invalid process-tree {label}: {tree_payload!r}")
    assert isinstance(parent_port, int)
    assert isinstance(descendant_port, int)
    for port in (parent_port, descendant_port):
        with socket.create_connection(("127.0.0.1", port), timeout=1.0):
            pass
    probe_module.terminate_process(tree_proc, tree_output)

    def process_exists(pid: int) -> bool:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    assert isinstance(descendant_pid, int)
    disappearance_deadline = time.monotonic() + 5.0
    while (
        process_exists(parent_pid)
        or process_exists(descendant_pid)
    ) and time.monotonic() < disappearance_deadline:
        time.sleep(0.05)
    if process_exists(parent_pid) or process_exists(descendant_pid):
        raise RuntimeError(
            "contained process tree retained leader or descendant: "
            f"parent={parent_pid} descendant={descendant_pid}"
        )
    for port in (parent_port, descendant_port):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as rebound:
            rebound.bind(("127.0.0.1", port))
    if not tree_output.stdout.is_finished() or not tree_output.stderr.is_finished():
        raise RuntimeError("process-tree cleanup leaked an output drain")

    leaked_threads = [
        thread.name
        for thread in threading.enumerate()
        if thread.name.startswith("nebula-pipe-")
    ]
    if leaked_threads:
        raise RuntimeError(f"service probe pipe threads leaked: {leaked_threads!r}")

    print("service-probe-pipe-contract-ok")
    return 0


def _run_signal_owner_probe() -> int:
    if os.name == "nt":
        print("service-probe-signal-owner-skip-windows")
        return 0

    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    import service_probe as probe_module

    fixture_path = str(Path(__file__).resolve())
    original_term = signal.getsignal(signal.SIGTERM)
    original_int = signal.getsignal(signal.SIGINT)
    worker_errors: list[BaseException] = []

    def launch_from_worker() -> None:
        try:
            probe_module.start_process(
                [sys.executable, fixture_path, "write-partial-pipe"],
                "listener_bound",
            )
        except BaseException as exc:
            worker_errors.append(exc)

    worker = threading.Thread(target=launch_from_worker)
    worker.start()
    worker.join(timeout=5)
    if worker.is_alive():
        raise RuntimeError("worker-thread service launch did not fail fast")
    if (
        len(worker_errors) != 1
        or not isinstance(worker_errors[0], probe_module.ServiceProbeSetupError)
        or "main thread" not in str(worker_errors[0])
    ):
        raise RuntimeError(
            f"worker-thread service launch contract drifted: {worker_errors!r}"
        )

    worker_cleanup_process, worker_cleanup_output = probe_module.start_process(
        [sys.executable, "-c", "import time; time.sleep(30)"],
        "listener_bound",
        thread_name_prefix="nebula-worker-cleanup",
    )
    worker_cleanup_errors: list[BaseException] = []

    def cleanup_from_worker() -> None:
        try:
            worker_cleanup_output.finish()
        except BaseException as exc:
            worker_cleanup_errors.append(exc)

    cleanup_worker = threading.Thread(target=cleanup_from_worker)
    cleanup_worker.start()
    cleanup_worker.join(timeout=5)
    if cleanup_worker.is_alive():
        raise RuntimeError("worker-thread service cleanup did not fail fast")
    if (
        len(worker_cleanup_errors) != 1
        or not isinstance(
            worker_cleanup_errors[0], probe_module.ServiceProbeSetupError
        )
        or "main thread" not in str(worker_cleanup_errors[0])
    ):
        raise RuntimeError(
            "worker-thread service cleanup contract drifted: "
            f"{worker_cleanup_errors!r}"
        )
    if worker_cleanup_process.poll() is not None:
        raise RuntimeError("worker-thread cleanup mutated the service process")
    if worker_cleanup_output._signal_registration_released:
        raise RuntimeError("worker-thread cleanup falsely released signal ownership")
    if worker_cleanup_process not in probe_module._active_service_owners:
        raise RuntimeError("worker-thread cleanup lost the registered service owner")
    probe_module.finish_process_output(
        worker_cleanup_process, worker_cleanup_output
    )
    if signal.getsignal(signal.SIGTERM) != original_term:
        raise RuntimeError("main-thread cleanup did not restore SIGTERM")
    if signal.getsignal(signal.SIGINT) != original_int:
        raise RuntimeError("main-thread cleanup did not restore SIGINT")

    original_output_type = probe_module.ServiceProbeOutput
    original_start = probe_module.start_long_lived_process
    rollback_owners: list[object] = []

    def capture_started_owner(*args: object, **kwargs: object):
        owner = original_start(*args, **kwargs)
        rollback_owners.append(owner)
        return owner

    def fail_output_construction(*args: object, **kwargs: object):
        del args, kwargs
        raise MemoryError("injected service output construction failure")

    probe_module.start_long_lived_process = capture_started_owner
    probe_module.ServiceProbeOutput = fail_output_construction
    try:
        try:
            probe_module.start_process(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                "listener_bound",
                thread_name_prefix="nebula-output-construction-rollback",
            )
        except probe_module.ServiceProbeSetupError as exc:
            if "injected service output construction failure" not in str(exc):
                raise RuntimeError(
                    f"output-construction rollback diagnostic drifted: {exc}"
                ) from exc
        else:
            raise RuntimeError("output-construction failure returned a service owner")
    finally:
        probe_module.ServiceProbeOutput = original_output_type
        probe_module.start_long_lived_process = original_start
        for rollback_owner in rollback_owners:
            if not rollback_owner.containment_sealed:
                rollback_owner.cancel()
                rollback_owner.finish()
    if len(rollback_owners) != 1:
        raise RuntimeError("output-construction rollback did not launch one owner")
    rollback_owner = rollback_owners[0]
    if (
        not rollback_owner.containment_sealed
        or rollback_owner.returncode is None
        or not rollback_owner.stdout_collector.is_finished()
        or not rollback_owner.stderr_collector.is_finished()
    ):
        raise RuntimeError("output-construction rollback leaked process resources")
    if (
        probe_module._active_service_owners
        or probe_module._service_launch_slot is not None
        or probe_module._service_signal_handlers_installed
        or probe_module._original_service_signal_handlers
    ):
        raise RuntimeError("output-construction rollback leaked signal ownership")
    if signal.getsignal(signal.SIGTERM) != original_term:
        raise RuntimeError("output-construction rollback did not restore SIGTERM")
    if signal.getsignal(signal.SIGINT) != original_int:
        raise RuntimeError("output-construction rollback did not restore SIGINT")

    class ReplayOwner:
        def __init__(self) -> None:
            self.cancel_calls = 0
            self.finish_calls = 0
            self.containment_sealed = False

        def cancel(self) -> None:
            self.cancel_calls += 1

        def finish(self) -> tuple[str, str]:
            self.finish_calls += 1
            self.containment_sealed = True
            return "", ""

    replay_owner = ReplayOwner()
    original_output_type = probe_module.ServiceProbeOutput
    original_start = probe_module.start_long_lived_process

    def raising_original_handler(signum: int, frame: object) -> None:
        del signum, frame
        raise KeyboardInterrupt("injected original signal handler control flow")

    def start_with_pending_signal(*args: object, **kwargs: object):
        del args, kwargs
        replay_slot = probe_module._service_launch_slot
        if replay_slot is None:
            raise RuntimeError("signal replay probe has no launch slot")
        replay_slot.owner = replay_owner
        probe_module._managed_service_signal(signal.SIGTERM, None)
        return replay_owner

    def fail_replay_output_construction(*args: object, **kwargs: object):
        del args, kwargs
        raise RuntimeError("injected pre-complete launch failure")

    signal.signal(signal.SIGTERM, raising_original_handler)
    probe_module.start_long_lived_process = start_with_pending_signal
    probe_module.ServiceProbeOutput = fail_replay_output_construction
    try:
        try:
            probe_module.start_process(
                [sys.executable, "-c", "raise SystemExit(99)"],
                "listener_bound",
            )
        except KeyboardInterrupt as exc:
            replay_notes = getattr(exc, "__notes__", [])
            if not any(
                "injected pre-complete launch failure" in note
                for note in replay_notes
            ):
                raise RuntimeError(
                    "original signal control flow lost the launch failure context: "
                    f"{replay_notes!r}"
                ) from exc
        else:
            raise RuntimeError("original signal handler control flow was swallowed")
    finally:
        probe_module.ServiceProbeOutput = original_output_type
        probe_module.start_long_lived_process = original_start
        signal.signal(signal.SIGTERM, original_term)
    if (
        replay_owner.cancel_calls < 1
        or replay_owner.finish_calls != 1
        or not replay_owner.containment_sealed
        or probe_module._active_service_owners
        or probe_module._service_launch_slot is not None
        or probe_module._service_signal_handlers_installed
        or probe_module._original_service_signal_handlers
    ):
        raise RuntimeError("original signal replay rollback did not converge")
    if signal.getsignal(signal.SIGINT) != original_int:
        raise RuntimeError("original signal replay did not restore SIGINT")

    delivered: list[int] = []

    def custom_term(signum: int, frame: object) -> None:
        del frame
        delivered.append(signum)

    signal.signal(signal.SIGTERM, custom_term)
    services: list[tuple[object, object]] = []
    try:
        import inspect

        class TraceOwner:
            def __init__(self) -> None:
                self.cancel_calls = 0

            def cancel(self) -> None:
                self.cancel_calls += 1

            def finish(self) -> tuple[str, str]:
                return "", ""

        trace_slot = probe_module._begin_service_launch()
        trace_owner = TraceOwner()
        source_lines, source_start = inspect.getsourcelines(
            probe_module._complete_service_launch
        )
        clear_line = next(
            source_start + offset
            for offset, line in enumerate(source_lines)
            if line.strip() == "_service_launch_slot = None"
        )
        pending_line = next(
            source_start + offset
            for offset, line in enumerate(source_lines)
            if line.strip() == "pending_signal = slot.pending_signal"
        )
        trace_target = max(clear_line, pending_line)
        trace_fired = False

        def inject_signal_between_launch_handoff_lines(
            frame: object,
            event: str,
            arg: object,
        ):
            del arg
            nonlocal trace_fired
            if (
                event == "line"
                and frame.f_code is probe_module._complete_service_launch.__code__
                and frame.f_lineno == trace_target
            ):
                trace_fired = True
                sys.settrace(None)
                probe_module._managed_service_signal(signal.SIGTERM, None)
                return None
            return inject_signal_between_launch_handoff_lines

        sys.settrace(inject_signal_between_launch_handoff_lines)
        try:
            probe_module._complete_service_launch(trace_slot, trace_owner)
        finally:
            sys.settrace(None)
        trace_release_detail = probe_module._release_service_owner(trace_owner)
        if trace_release_detail:
            raise RuntimeError(
                "launch-handoff trace owner release failed: "
                + trace_release_detail
            )
        if not trace_fired:
            raise RuntimeError("launch-handoff signal trace did not reach its target")
        if trace_owner.cancel_calls != 1:
            raise RuntimeError(
                "launch-handoff signal did not cancel exactly one owner: "
                f"{trace_owner.cancel_calls}"
            )
        if delivered != [signal.SIGTERM]:
            raise RuntimeError(
                "launch-handoff signal did not chain the original handler: "
                f"{delivered!r}"
            )
        delivered.clear()

        class FailedLaunchOwner:
            def __init__(self) -> None:
                self.cancel_calls = 0
                self.finish_calls = 0
                self.allow_convergence = False
                self.containment_sealed = False

            def cancel(self) -> None:
                self.cancel_calls += 1

            def finish(self) -> tuple[str, str]:
                self.finish_calls += 1
                if not self.allow_convergence:
                    raise probe_module.InvocationError(
                        "injected launch rollback convergence failure"
                    )
                self.containment_sealed = True
                return "", ""

        failed_launch_owner = FailedLaunchOwner()
        original_start_for_failed_launch = (
            probe_module.start_long_lived_process
        )

        def start_then_interrupt(*args: object, **kwargs: object):
            del args, kwargs
            launch_slot = probe_module._service_launch_slot
            if launch_slot is None:
                raise RuntimeError("failed-launch probe has no launch slot")
            launch_slot.owner = failed_launch_owner
            probe_module._managed_service_signal(signal.SIGTERM, None)
            return failed_launch_owner

        probe_module.start_long_lived_process = start_then_interrupt
        try:
            try:
                probe_module.start_process(
                    [sys.executable, "-c", "raise SystemExit(99)"],
                    "listener_bound",
                )
            except probe_module.ServiceProbeSetupError as exc:
                failed_launch_detail = str(exc)
                if (
                    "injected launch rollback convergence failure"
                    not in failed_launch_detail
                    or "remains owned" not in failed_launch_detail
                    or "signal delivery was suppressed" not in failed_launch_detail
                ):
                    raise RuntimeError(
                        "failed launch containment diagnostic drifted: "
                        + failed_launch_detail
                    ) from exc
            else:
                raise RuntimeError("failed launch containment returned an owner")
        finally:
            probe_module.start_long_lived_process = (
                original_start_for_failed_launch
            )
        if (
            probe_module._active_service_owners
            != (failed_launch_owner,)
            or probe_module._service_launch_slot is not None
            or not probe_module._service_signal_handlers_installed
            or signal.getsignal(signal.SIGTERM)
            is not probe_module._managed_service_signal
        ):
            raise RuntimeError(
                "failed launch containment lost globally reachable ownership"
            )
        if delivered:
            raise RuntimeError(
                "failed launch containment replayed the original signal before sealing"
            )
        failed_launch_owner.allow_convergence = True
        failed_launch_owner.cancel()
        failed_launch_owner.finish()
        failed_launch_release = probe_module._release_service_owner(
            failed_launch_owner
        )
        if failed_launch_release:
            raise RuntimeError(
                "failed launch containment recovery did not converge: "
                + failed_launch_release
            )
        if (
            probe_module._active_service_owners
            or probe_module._service_signal_handlers_installed
            or signal.getsignal(signal.SIGTERM) is not custom_term
        ):
            raise RuntimeError(
                "failed launch containment recovery leaked ownership"
            )

        class RestoreOwner:
            def cancel(self) -> None:
                return None

            def finish(self) -> tuple[str, str]:
                return "", ""

        restore_slot = probe_module._begin_service_launch()
        restore_owner = RestoreOwner()
        probe_module._complete_service_launch(restore_slot, restore_owner)
        original_signal_function = probe_module.signal.signal
        restoration_failure_enabled = True

        def fail_sigint_restoration(signum: int, handler: object):
            if (
                restoration_failure_enabled
                and signum == signal.SIGINT
                and handler == original_int
            ):
                raise OSError("injected SIGINT restoration failure")
            return original_signal_function(signum, handler)

        probe_module.signal.signal = fail_sigint_restoration
        try:
            restore_detail = probe_module._release_service_owner(restore_owner)
            if "injected SIGINT restoration failure" not in restore_detail:
                raise RuntimeError(
                    f"signal restoration failure lost context: {restore_detail!r}"
                )
            if (
                not probe_module._service_signal_handlers_installed
                or set(probe_module._original_service_signal_handlers)
                != {signal.SIGINT}
                or signal.getsignal(signal.SIGINT)
                is not probe_module._managed_service_signal
            ):
                raise RuntimeError("signal restoration failure lost its retry ledger")
            try:
                probe_module._begin_service_launch()
            except probe_module.ServiceProbeSetupError as exc:
                if "restoration remains incomplete" not in str(exc):
                    raise RuntimeError(
                        f"incomplete restoration diagnostic drifted: {exc}"
                    ) from exc
            else:
                raise RuntimeError("service launch bypassed incomplete signal restoration")
        finally:
            restoration_failure_enabled = False
            probe_module.signal.signal = original_signal_function
        retry_detail = probe_module._restore_service_signal_handlers_if_idle()
        if retry_detail:
            raise RuntimeError(
                f"signal restoration retry unexpectedly failed: {retry_detail}"
            )
        if (
            probe_module._service_signal_handlers_installed
            or probe_module._original_service_signal_handlers
            or signal.getsignal(signal.SIGTERM) is not custom_term
            or signal.getsignal(signal.SIGINT) != original_int
        ):
            raise RuntimeError("signal restoration retry did not converge")

        original_start = probe_module.start_long_lived_process
        launch_window_owners: list[object] = []

        def signal_after_launch(*args: object, **kwargs: object):
            owner = original_start(*args, **kwargs)
            launch_window_owners.append(owner)
            os.kill(os.getpid(), signal.SIGTERM)
            return owner

        probe_module.start_long_lived_process = signal_after_launch
        try:
            try:
                probe_module.start_process(
                    [sys.executable, fixture_path, "write-process-tree"],
                    "listener_bound",
                    thread_name_prefix="nebula-signal-launch-window",
                )
            except probe_module.ServiceProbeSetupError as exc:
                if "interrupted by signal" not in str(exc):
                    raise RuntimeError(
                        f"launch-window signal diagnostic drifted: {exc}"
                    ) from exc
            else:
                raise RuntimeError("launch-window signal returned a live service owner")
        finally:
            probe_module.start_long_lived_process = original_start
        if len(launch_window_owners) != 1:
            raise RuntimeError("launch-window probe did not create exactly one owner")
        launch_window_owner = launch_window_owners[0]
        launch_window_owner.cancel()
        launch_window_owner.cancel()
        launch_window_owner.finish()
        if launch_window_owner.returncode is None:
            raise RuntimeError("launch-window signal did not reap its service")
        if delivered != [signal.SIGTERM]:
            raise RuntimeError(
                f"launch-window signal did not chain the original handler: {delivered!r}"
            )
        if signal.getsignal(signal.SIGTERM) is not custom_term:
            raise RuntimeError("launch-window cleanup did not restore SIGTERM handler")
        delivered.clear()

        for index in range(2):
            process, output = probe_module.start_process(
                [sys.executable, fixture_path, "write-process-tree"],
                "listener_bound",
                thread_name_prefix=f"nebula-signal-owner-{index}",
            )
            probe_module.wait_for_listener_bound(process, output, timeout=5)
            services.append((process, output))

        if signal.getsignal(signal.SIGTERM) is custom_term:
            raise RuntimeError("service owner did not install its SIGTERM manager")
        os.kill(os.getpid(), signal.SIGTERM)
        if delivered != [signal.SIGTERM]:
            raise RuntimeError(f"original SIGTERM handler was not chained once: {delivered!r}")

        first_process, first_output = services[0]
        probe_module.finish_process_output(first_process, first_output)
        if signal.getsignal(signal.SIGTERM) is custom_term:
            raise RuntimeError("signal manager restored while a second service remained active")

        second_process, second_output = services[1]
        probe_module.finish_process_output(second_process, second_output)
        if signal.getsignal(signal.SIGTERM) is not custom_term:
            raise RuntimeError("original SIGTERM handler was not restored after final cleanup")
        if signal.getsignal(signal.SIGINT) != original_int:
            raise RuntimeError("original SIGINT handler was not restored after final cleanup")
    finally:
        for process, output in reversed(services):
            probe_module.terminate_process(process, output)
        signal.signal(signal.SIGTERM, original_term)
        signal.signal(signal.SIGINT, original_int)

    print("service-probe-signal-owner-ok")
    return 0


def _run_probe() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from service_probe import (
        MAX_PENDING_OBSERVE_RECORD_BYTES,
        wait_for_observe_event_in_file,
    )

    log_path = Path("work/service-probe-split.log")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if log_path.exists():
        log_path.unlink()
    writer = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "write", str(log_path)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    payload = wait_for_observe_event_in_file(
        log_path,
        "listener_bound",
        2.0,
        writer,
    )
    return_code = writer.wait(timeout=5)
    if return_code != 0 or payload.get("port") != 43210:
        raise RuntimeError(f"split observe event was not reconstructed: {payload!r}")

    oversized_path = Path("work/service-probe-oversized.log")
    oversized_path.write_bytes(b"x" * (MAX_PENDING_OBSERVE_RECORD_BYTES + 1))
    try:
        wait_for_observe_event_in_file(
            oversized_path,
            "listener_bound",
            0.1,
        )
    except SystemExit as exc:
        if "observe record exceeded" not in str(exc):
            raise RuntimeError(f"unexpected oversized-record failure: {exc}") from exc
    else:
        raise RuntimeError("oversized unterminated observe record was accepted")

    oversized_complete_path = Path("work/service-probe-oversized-complete.log")
    oversized_complete_path.write_bytes(
        b"x" * (MAX_PENDING_OBSERVE_RECORD_BYTES + 1) + b"\n"
    )
    try:
        wait_for_observe_event_in_file(
            oversized_complete_path,
            "listener_bound",
            0.1,
        )
    except SystemExit as exc:
        if "observe record exceeded" not in str(exc):
            raise RuntimeError(f"unexpected complete oversized-record failure: {exc}") from exc
    else:
        raise RuntimeError("oversized newline-terminated observe record was accepted")

    sparse_path = Path("work/service-probe-sparse-oversized.log")
    with sparse_path.open("wb") as handle:
        handle.truncate(32 * 1024 * 1024)
    tracemalloc.start()
    try:
        try:
            wait_for_observe_event_in_file(
                sparse_path,
                "listener_bound",
                0.1,
            )
        except SystemExit as exc:
            if "observe record exceeded" not in str(exc):
                raise RuntimeError(f"unexpected sparse oversized-record failure: {exc}") from exc
        else:
            raise RuntimeError("sparse oversized observe record was accepted")
        _, peak_bytes = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    if peak_bytes > 8 * 1024 * 1024:
        raise RuntimeError(
            f"incremental observe reader used excessive traced memory: {peak_bytes} bytes"
        )

    many_records_path = Path("work/service-probe-many-records.log")
    noise_chunk = b"noise-record\n" * 4096
    with many_records_path.open("wb") as handle:
        for _ in range(40):
            handle.write(noise_chunk)
        handle.write(b'{"event":"listener_bound","port":43211}\n')
    class ExitedProcess:
        returncode = 0

        def poll(self) -> int:
            return 0

    many_records_payload = wait_for_observe_event_in_file(
        many_records_path,
        "listener_bound",
        5.0,
        ExitedProcess(),
    )
    if many_records_payload.get("port") != 43211:
        raise RuntimeError(
            f"event after many complete records was not found: {many_records_payload!r}"
        )

    original_timeout = os.environ.get("NEBULA_SERVICE_START_TIMEOUT")
    os.environ["NEBULA_SERVICE_START_TIMEOUT"] = "invalid"
    try:
        from service_probe import service_start_timeout

        try:
            service_start_timeout(1.0)
        except ValueError as exc:
            if "finite positive number" not in str(exc):
                raise RuntimeError(f"invalid timeout diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("invalid service timeout override was silently accepted")
    finally:
        if original_timeout is None:
            del os.environ["NEBULA_SERVICE_START_TIMEOUT"]
        else:
            os.environ["NEBULA_SERVICE_START_TIMEOUT"] = original_timeout

    print("service-probe-partial-record-ok")
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "write":
        return _write_split_event(Path(sys.argv[2]))
    if len(sys.argv) == 2 and sys.argv[1] == "write-bounded-output":
        return _write_bounded_output()
    if len(sys.argv) == 2 and sys.argv[1] == "write-partial-pipe":
        return _write_partial_pipe()
    if len(sys.argv) == 2 and sys.argv[1] == "write-oversized-pipe":
        return _write_oversized_pipe()
    if len(sys.argv) == 2 and sys.argv[1] == "write-bounded-pipe-event":
        return _write_bounded_pipe_event()
    if len(sys.argv) == 3 and sys.argv[1] == "write-port-event":
        return _write_port_event(sys.argv[2])
    if len(sys.argv) == 2 and sys.argv[1] == "write-listener-then-exit":
        return _write_listener_then_exit()
    if len(sys.argv) == 2 and sys.argv[1] == "write-noise-then-exit":
        return _write_noise_then_exit()
    if len(sys.argv) == 2 and sys.argv[1] == "write-process-tree":
        return _write_process_tree()
    if len(sys.argv) == 4 and sys.argv[1] == "write-process-tree-descendant":
        return _write_process_tree_descendant(int(sys.argv[2]), int(sys.argv[3]))
    if len(sys.argv) == 2 and sys.argv[1] == "bounded-output-probe":
        return _run_bounded_output_probe()
    if len(sys.argv) == 2 and sys.argv[1] == "pipe-probe":
        return _run_pipe_probe()
    if len(sys.argv) == 2 and sys.argv[1] == "signal-owner-probe":
        return _run_signal_owner_probe()
    if len(sys.argv) == 2 and sys.argv[1] == "probe":
        return _run_probe()
    raise RuntimeError("invalid service probe fixture arguments")


if __name__ == "__main__":
    raise SystemExit(main())
