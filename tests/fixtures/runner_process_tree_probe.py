from __future__ import annotations

import errno
import json
import os
import platform
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import NoReturn


HEARTBEAT_INTERVAL_SECONDS = 0.05
READY_TIMEOUT_SECONDS = 4.0
VERIFY_INTERVAL_SECONDS = 0.5


def _path(prefix: str, suffix: str) -> Path:
    return Path("work") / f"{prefix}-{suffix}.txt"


def _cooperative_spawn_pass_fds() -> tuple[int, ...]:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from process_containment import cooperative_posix_spawn_pass_fds

    return cooperative_posix_spawn_pass_fds()


def _heartbeat_child(prefix: str) -> NoReturn:
    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    heartbeat.write_text("0", encoding="utf-8")
    ready.write_text("ready", encoding="utf-8")
    counter = 1
    while True:
        heartbeat.write_text(str(counter), encoding="utf-8")
        counter += 1
        time.sleep(HEARTBEAT_INTERVAL_SECONDS)


def _contained_heartbeat_child(prefix: str) -> NoReturn:
    if hasattr(signal, "pthread_sigmask"):
        blocked = signal.pthread_sigmask(signal.SIG_BLOCK, [])
        if signal.SIGTERM in blocked or signal.SIGINT in blocked:
            raise RuntimeError("service child inherited a blocked termination signal")
    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    heartbeat.write_text("0", encoding="utf-8")
    ready.write_text("ready", encoding="utf-8")
    print(
        json.dumps({"event": "contained_heartbeat_ready"}, separators=(",", ":")),
        file=sys.stderr,
        flush=True,
    )
    counter = 1
    while True:
        heartbeat.write_text(str(counter), encoding="utf-8")
        counter += 1
        time.sleep(HEARTBEAT_INTERVAL_SECONDS)


def _nested_session_parent(prefix: str) -> NoReturn:
    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    child = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "heartbeat-child", prefix],
        stdin=subprocess.DEVNULL,
        start_new_session=True,
        pass_fds=_cooperative_spawn_pass_fds(),
    )
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline and not ready.exists():
        if child.poll() is not None:
            raise RuntimeError(
                f"{prefix} nested-session heartbeat exited with {child.returncode}"
            )
        time.sleep(0.01)
    if not ready.exists():
        raise RuntimeError(f"{prefix} nested-session heartbeat did not become ready")
    _path(prefix, "domain").write_text(
        json.dumps(
            {
                "parent_pid": os.getpid(),
                "parent_pgid": os.getpgrp(),
                "parent_sid": os.getsid(0),
                "child_pid": child.pid,
                "child_pgid": os.getpgid(child.pid),
                "child_sid": os.getsid(child.pid),
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    while True:
        time.sleep(30)


def _start_nested_session_timeout(prefix: str) -> int:
    if os.name == "nt":
        return _start(prefix, sleep_after_ready=True, return_code=0)

    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    domain_path = _path(prefix, "domain")
    heartbeat.parent.mkdir(parents=True, exist_ok=True)
    for path in (heartbeat, ready, domain_path):
        if path.exists():
            path.unlink()

    parent = subprocess.Popen(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "nested-session-parent",
            prefix,
        ],
        stdin=subprocess.DEVNULL,
        start_new_session=True,
        pass_fds=_cooperative_spawn_pass_fds(),
    )
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline and not domain_path.exists():
        if parent.poll() is not None:
            raise RuntimeError(
                f"{prefix} nested-session parent exited with {parent.returncode}"
            )
        time.sleep(0.01)
    if not domain_path.exists():
        raise RuntimeError(f"{prefix} nested-session domain did not become ready")

    domain = json.loads(domain_path.read_text(encoding="utf-8"))
    outer_session = os.getsid(0)
    parent_session = domain.get("parent_sid")
    child_session = domain.get("child_sid")
    if (
        type(parent_session) is not int
        or type(child_session) is not int
        or len({outer_session, parent_session, child_session}) != 3
        or domain.get("parent_pid") != domain.get("parent_pgid")
        or domain.get("child_pid") != domain.get("child_pgid")
    ):
        raise RuntimeError(
            f"{prefix} did not create two independently nested sessions: {domain!r}"
        )
    print(f"{prefix}-cleanup-child-started", flush=True)
    time.sleep(30)
    return 0


def _start_nested_session_exit(prefix: str, return_code: int) -> int:
    if os.name == "nt":
        return _start(prefix, sleep_after_ready=False, return_code=return_code)

    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    domain_path = _path(prefix, "domain")
    heartbeat.parent.mkdir(parents=True, exist_ok=True)
    for path in (heartbeat, ready, domain_path):
        if path.exists():
            path.unlink()

    child = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "heartbeat-child", prefix],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        pass_fds=_cooperative_spawn_pass_fds(),
    )
    handed_to_containment = False
    try:
        child_session = os.getsid(child.pid)
        if child_session == os.getsid(0) or child_session != child.pid:
            raise RuntimeError(
                f"{prefix} child did not detach into its own session: "
                f"pid={child.pid} sid={child_session}"
            )
        repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
        sys.path.insert(0, str(repo_root / "tests" / "lib"))
        import process_containment as containment_module

        child_record = containment_module._read_posix_process_record(child.pid)
        if child_record is None:
            raise RuntimeError(f"{prefix} child disappeared before identity capture")
        domain_path.write_text(
            json.dumps(
                {
                    "child_pid": child.pid,
                    "child_pgid": os.getpgid(child.pid),
                    "child_sid": child_session,
                    "child_start_token": list(child_record.identity.start_token),
                },
                sort_keys=True,
            ),
            encoding="utf-8",
        )
        print(f"{prefix}-cleanup-child-started", flush=True)
        handed_to_containment = True
        return return_code
    finally:
        if not handed_to_containment and child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=READY_TIMEOUT_SECONDS)


def _wait_for_child_ready_fd(
    ready_fd: int,
    child: subprocess.Popen[bytes],
    label: str,
) -> None:
    with selectors.DefaultSelector() as selector:
        selector.register(ready_fd, selectors.EVENT_READ)
        events = selector.select(READY_TIMEOUT_SECONDS)
    if not events:
        return_code = child.poll()
        detail = (
            f"; child exited with {return_code}"
            if return_code is not None
            else ""
        )
        raise RuntimeError(f"{label} child readiness timed out{detail}")
    ready = os.read(ready_fd, 1)
    if ready != b"R":
        raise RuntimeError(
            f"{label} child readiness channel closed with {ready!r}"
        )


def _immediate_detached_child(
    prefix: str,
    ready_fd: int,
    parent_session: int,
) -> NoReturn:
    pid = os.getpid()
    process_group = os.getpgrp()
    session = os.getsid(0)
    if process_group != pid or session != pid or session == parent_session:
        raise RuntimeError(
            f"{prefix} child did not enter a distinct session: "
            f"pid={pid} pgid={process_group} sid={session} parent_sid={parent_session}"
        )
    heartbeat = _path(prefix, "heartbeat")
    _path(prefix, "domain").write_text(
        json.dumps(
            {
                "child_pid": pid,
                "child_pgid": process_group,
                "child_sid": session,
                "parent_sid": parent_session,
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    heartbeat.write_text("0", encoding="utf-8")
    os.write(ready_fd, b"R")
    os.close(ready_fd)
    counter = 1
    while True:
        heartbeat.write_text(str(counter), encoding="utf-8")
        counter += 1
        time.sleep(HEARTBEAT_INTERVAL_SECONDS)


def _start_immediate_detached_exit(prefix: str, return_code: int) -> int:
    if os.name == "nt":
        return _start(prefix, sleep_after_ready=False, return_code=return_code)

    ready_read_fd, ready_write_fd = os.pipe()
    child: subprocess.Popen[bytes] | None = None
    handed_to_containment = False
    try:
        child = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "immediate-detached-child",
                prefix,
                str(ready_write_fd),
                str(os.getsid(0)),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            pass_fds=(*_cooperative_spawn_pass_fds(), ready_write_fd),
        )
        os.close(ready_write_fd)
        ready_write_fd = -1
        _wait_for_child_ready_fd(ready_read_fd, child, prefix)
        # Return immediately after the independently-sessioned child proves it
        # has started.  There is deliberately no process-table import, identity
        # serialization, output flush, or settle delay on this parent path.
        handed_to_containment = True
        return return_code
    finally:
        for descriptor in (ready_read_fd, ready_write_fd):
            if descriptor < 0:
                continue
            try:
                os.close(descriptor)
            except OSError as exc:
                if handed_to_containment:
                    raise RuntimeError(
                        f"{prefix} readiness descriptor cleanup failed: {exc}"
                    ) from exc
        if (
            not handed_to_containment
            and child is not None
            and child.poll() is None
        ):
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=READY_TIMEOUT_SECONDS)


def _prepare_immediate_probe(prefix: str) -> None:
    heartbeat = _path(prefix, "heartbeat")
    domain = _path(prefix, "domain")
    heartbeat.parent.mkdir(parents=True, exist_ok=True)
    for path in (heartbeat, domain):
        if path.exists():
            path.unlink()


def _assert_immediate_probe_stopped(prefix: str) -> None:
    domain_path = _path(prefix, "domain")
    heartbeat = _path(prefix, "heartbeat")
    if not domain_path.is_file() or not heartbeat.is_file():
        raise RuntimeError(f"{prefix} detached child did not publish its proof files")
    domain = json.loads(domain_path.read_text(encoding="utf-8"))
    child_pid = domain.get("child_pid")
    child_pgid = domain.get("child_pgid")
    child_sid = domain.get("child_sid")
    parent_sid = domain.get("parent_sid")
    if (
        type(child_pid) is not int
        or child_pid <= 1
        or child_pgid != child_pid
        or child_sid != child_pid
        or type(parent_sid) is not int
        or parent_sid <= 1
        or parent_sid == child_sid
    ):
        raise RuntimeError(f"{prefix} detached-session proof drifted: {domain!r}")
    first = heartbeat.read_text(encoding="utf-8")
    time.sleep(VERIFY_INTERVAL_SECONDS)
    second = heartbeat.read_text(encoding="utf-8")
    if first != second:
        raise RuntimeError(
            f"{prefix} detached child survived containment cleanup: "
            f"{first!r} -> {second!r}"
        )


def _run_immediate_detached_oracle() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from process_containment import ProcessContainmentPolicy, run_contained_command

    fixture = str(Path(__file__).resolve())
    for return_code in (0, 7):
        prefix = f"immediate-detached-rc{return_code}"
        _prepare_immediate_probe(prefix)
        result = run_contained_command(
            [
                sys.executable,
                fixture,
                "immediate-detached-exit",
                prefix,
                str(return_code),
            ],
            cwd=Path.cwd(),
            shell=False,
            env=dict(os.environ),
            timeout_sec=5.0,
            containment_policy=ProcessContainmentPolicy.TRUSTED_COOPERATIVE,
        )
        if (
            result.returncode != return_code
            or result.timed_out
            or not result.cleanup_after_exit_performed
        ):
            raise RuntimeError(
                f"{prefix} containment result drifted: {result!r}"
            )
        _assert_immediate_probe_stopped(prefix)
    print("immediate-detached-exit-containment-ok")
    return 0


def _start_native_anchor_detached_exit(prefix: str) -> int:
    anchor_fds = _cooperative_spawn_pass_fds()
    pid_path = _path(prefix, "pid")
    ack_path = _path(prefix, "ack")
    child: subprocess.Popen[bytes] | None = None
    handed_to_containment = False
    try:
        child = subprocess.Popen(
            ["/bin/sleep", "30"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env={},
            start_new_session=True,
            pass_fds=anchor_fds,
        )
        pending_pid_path = pid_path.with_name(pid_path.name + ".pending")
        pending_pid_path.write_text(str(child.pid), encoding="ascii")
        pending_pid_path.replace(pid_path)
        deadline = time.monotonic() + READY_TIMEOUT_SECONDS
        while not ack_path.is_file() and time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(
                    f"native anchor child exited with {child.returncode} before handoff"
                )
            time.sleep(0.001)
        if not ack_path.is_file():
            raise RuntimeError("native anchor child identity was not acknowledged")
        handed_to_containment = True
        return 0
    finally:
        if (
            not handed_to_containment
            and child is not None
            and child.poll() is None
        ):
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=READY_TIMEOUT_SECONDS)


def _run_native_anchor_oracle() -> int:
    if platform.system() != "Darwin":
        print("native-apple-anchor-containment-ok")
        return 0

    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    import process_containment as containment_module

    prefix = "native-apple-anchor"
    pid_path = _path(prefix, "pid")
    ack_path = _path(prefix, "ack")
    pending_pid_path = pid_path.with_name(pid_path.name + ".pending")
    pid_path.parent.mkdir(parents=True, exist_ok=True)
    for path in (pid_path, ack_path, pending_pid_path):
        if path.exists():
            path.unlink()

    observed: dict[str, object] = {}

    def capture_native_identity() -> None:
        try:
            deadline = time.monotonic() + READY_TIMEOUT_SECONDS
            while not pid_path.is_file() and time.monotonic() < deadline:
                time.sleep(0.001)
            if not pid_path.is_file():
                raise RuntimeError("native anchor child pid was not published")
            child_pid = int(pid_path.read_text(encoding="ascii"))
            record = containment_module._read_posix_process_record(child_pid)
            if record is None:
                raise RuntimeError("native anchor child disappeared before identity capture")
            if (
                record.process_group_id != child_pid
                or record.session_id != child_pid
            ):
                raise RuntimeError(
                    f"native anchor child did not detach: {record!r}"
                )
            observed["identity"] = record.identity
            observed["process_group_id"] = record.process_group_id
            ack_path.write_text("ack", encoding="ascii")
        except BaseException as exc:
            observed["error"] = exc

    observer = threading.Thread(
        target=capture_native_identity,
        name="native-anchor-identity-observer",
    )
    original_audit_interval = (
        containment_module.POSIX_DESCENDANT_AUDIT_INTERVAL_SECONDS
    )
    observer.start()
    try:
        # Force final pipe-handle discovery to carry the proof instead of the
        # periodic parent/session tracker observing the child first.
        containment_module.POSIX_DESCENDANT_AUDIT_INTERVAL_SECONDS = 60.0
        result = containment_module.run_contained_command(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "native-anchor-detached-exit",
                prefix,
            ],
            cwd=Path.cwd(),
            shell=False,
            env=dict(os.environ),
            timeout_sec=5.0,
            containment_policy=(
                containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
            ),
        )
    finally:
        containment_module.POSIX_DESCENDANT_AUDIT_INTERVAL_SECONDS = (
            original_audit_interval
        )
        observer.join(timeout=READY_TIMEOUT_SECONDS)
        identity = observed.get("identity")
        group_id = observed.get("process_group_id")
        if isinstance(identity, containment_module._PosixProcessIdentity):
            current = containment_module._read_posix_process_record(identity.pid)
            if (
                current is not None
                and current.identity == identity
                and current.process_group_id == group_id
            ):
                os.killpg(current.process_group_id, signal.SIGKILL)
    if observer.is_alive():
        raise RuntimeError("native anchor identity observer did not stop")
    if "error" in observed:
        raise RuntimeError(f"native anchor identity capture failed: {observed['error']}")
    if (
        result.returncode != 0
        or result.timed_out
        or not result.cleanup_after_exit_performed
        or not result.containment_complete
    ):
        raise RuntimeError(f"native anchor containment result drifted: {result!r}")
    identity = observed.get("identity")
    if not isinstance(identity, containment_module._PosixProcessIdentity):
        raise RuntimeError("native anchor identity was not retained")
    current = containment_module._read_posix_process_record(identity.pid)
    if current is not None and current.identity == identity:
        raise RuntimeError(
            f"native Apple executable survived cooperative containment: {current!r}"
        )
    print("native-apple-anchor-containment-ok")
    return 0


def _run_long_lived_immediate_detached_oracle() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from process_containment import (
        ProcessContainmentPolicy,
        start_long_lived_process,
    )

    fixture = str(Path(__file__).resolve())
    for return_code, cancel_after_exit in ((0, False), (7, True)):
        prefix = f"long-lived-immediate-rc{return_code}"
        _prepare_immediate_probe(prefix)
        owner = start_long_lived_process(
            [
                sys.executable,
                fixture,
                "immediate-detached-exit",
                prefix,
                str(return_code),
            ],
            cwd=Path.cwd(),
            env=dict(os.environ),
            containment_policy=ProcessContainmentPolicy.TRUSTED_COOPERATIVE,
            thread_name_prefix=f"{prefix}-output",
        )
        observed_return_code = owner.wait(timeout=5.0)
        if cancel_after_exit:
            owner.cancel()
        owner.finish()
        if (
            observed_return_code != return_code
            or owner.returncode != return_code
            or not owner.containment_sealed
        ):
            raise RuntimeError(
                f"{prefix} owner result drifted: "
                f"observed={observed_return_code} returncode={owner.returncode} "
                f"sealed={owner.containment_sealed}"
            )
        _assert_immediate_probe_stopped(prefix)
    print("long-lived-immediate-detached-containment-ok")
    return 0


def _run_long_lived_retry_oracle() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    import process_containment as containment_module

    owner = containment_module.start_long_lived_process(
        [sys.executable, "-c", "raise SystemExit(0)"],
        containment_policy=(
            containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
        ),
        cwd=Path.cwd(),
        env=dict(os.environ),
        thread_name_prefix="long-lived-retry",
    )
    if owner.wait(timeout=5.0) != 0 or owner._proc.returncode is not None:
        raise RuntimeError(
            "long-lived retry probe reaped its normally exited leader early"
        )
    original_terminate = containment_module._terminate_posix_descendant_domain
    original_kill = containment_module._kill_posix_descendant_domain

    def fail_convergence(*args: object, **kwargs: object) -> None:
        del args, kwargs
        raise containment_module.InvocationError(
            "injected descendant convergence failure"
        )

    containment_module._terminate_posix_descendant_domain = fail_convergence
    containment_module._kill_posix_descendant_domain = fail_convergence
    first_failure = ""
    try:
        try:
            owner.finish()
        except containment_module.InvocationError as exc:
            first_failure = str(exc)
        else:
            raise RuntimeError("injected long-lived convergence failure was accepted")
    finally:
        containment_module._terminate_posix_descendant_domain = original_terminate
        containment_module._kill_posix_descendant_domain = original_kill

    tracker = owner._descendant_tracker
    if (
        "injected descendant convergence failure" not in first_failure
        or owner.containment_sealed
        or tracker is None
        or tracker._domain.anchor_read_fd < 0
        or owner._proc.returncode is not None
    ):
        if not owner.containment_sealed:
            owner.cancel()
            owner.finish()
        raise RuntimeError(
            "long-lived retry state was not retained after cleanup failure: "
            f"failure={first_failure!r} sealed={owner.containment_sealed} "
            f"returncode={owner._proc.returncode}"
        )

    owner.finish()
    if (
        not owner.containment_sealed
        or not owner.containment_complete
        or tracker._domain.anchor_read_fd >= 0
        or owner.returncode is None
    ):
        raise RuntimeError(
            "long-lived retry did not converge: "
            f"sealed={owner.containment_sealed} returncode={owner.returncode}"
        )
    print("long-lived-containment-retry-ok")
    return 0


def _run_sigterm_late_spawn_target(prefix: str) -> NoReturn:
    spawned = False

    def spawn_detached_child(signum: int, frame: object) -> NoReturn:
        del signum, frame
        nonlocal spawned
        if spawned:
            raise RuntimeError(f"{prefix} received SIGTERM more than once")
        spawned = True
        ready_read_fd, ready_write_fd = os.pipe()
        child: subprocess.Popen[bytes] | None = None
        handed_to_containment = False
        try:
            child = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(__file__).resolve()),
                    "immediate-detached-child",
                    prefix,
                    str(ready_write_fd),
                    str(os.getsid(0)),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
                pass_fds=(*_cooperative_spawn_pass_fds(), ready_write_fd),
            )
            os.close(ready_write_fd)
            ready_write_fd = -1
            _wait_for_child_ready_fd(ready_read_fd, child, prefix)
            handed_to_containment = True
        finally:
            for descriptor in (ready_read_fd, ready_write_fd):
                if descriptor >= 0:
                    os.close(descriptor)
            if (
                not handed_to_containment
                and child is not None
                and child.poll() is None
            ):
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=READY_TIMEOUT_SECONDS)
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, spawn_detached_child)
    print("sigterm-late-spawn-handler-ready", flush=True)
    while True:
        signal.pause()


def _run_sigterm_late_spawn_oracle() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from process_containment import ProcessContainmentPolicy, run_contained_command

    prefix = "sigterm-late-spawn"
    _prepare_immediate_probe(prefix)
    result = run_contained_command(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "sigterm-late-spawn-target",
            prefix,
        ],
        cwd=Path.cwd(),
        shell=False,
        env=dict(os.environ),
        timeout_sec=1.0,
        containment_policy=ProcessContainmentPolicy.TRUSTED_COOPERATIVE,
    )
    if (
        result.returncode != 124
        or not result.timed_out
        or "sigterm-late-spawn-handler-ready" not in result.stdout
    ):
        raise RuntimeError(f"SIGTERM late-spawn result drifted: {result!r}")
    _assert_immediate_probe_stopped(prefix)
    print("sigterm-late-spawn-containment-ok")
    return 0


def _start_contained_service_timeout(prefix: str) -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    from service_probe import start_process, wait_for_observe_event

    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    heartbeat.parent.mkdir(parents=True, exist_ok=True)
    for path in (heartbeat, ready):
        if path.exists():
            path.unlink()
    process, output = start_process(
        [
            sys.executable,
            str(Path(__file__).resolve()),
            "contained-heartbeat-child",
            prefix,
        ],
        "contained_heartbeat_ready",
    )
    wait_for_observe_event(
        process,
        output,
        "contained_heartbeat_ready",
        READY_TIMEOUT_SECONDS,
    )
    print(f"{prefix}-cleanup-child-started", flush=True)
    time.sleep(30)
    return 0


def _start(prefix: str, *, sleep_after_ready: bool, return_code: int) -> int:
    heartbeat = _path(prefix, "heartbeat")
    ready = _path(prefix, "ready")
    heartbeat.parent.mkdir(parents=True, exist_ok=True)
    for path in (heartbeat, ready):
        if path.exists():
            path.unlink()

    subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "heartbeat-child", prefix],
        stdin=subprocess.DEVNULL,
        pass_fds=_cooperative_spawn_pass_fds(),
    )
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline and not ready.exists():
        time.sleep(0.01)
    if not ready.exists():
        raise RuntimeError(f"{prefix} child did not become ready")
    print(f"{prefix}-cleanup-child-started", flush=True)
    if sleep_after_ready:
        time.sleep(30)
    return return_code


def _verify_stopped(prefix: str) -> int:
    heartbeat = _path(prefix, "heartbeat")
    if not heartbeat.exists():
        raise RuntimeError(f"{prefix} heartbeat was never created")
    first = heartbeat.read_text(encoding="utf-8")
    time.sleep(VERIFY_INTERVAL_SECONDS)
    second = heartbeat.read_text(encoding="utf-8")
    if first != second:
        raise RuntimeError(
            f"{prefix} descendant remained active after containment cleanup: "
            f"{first!r} -> {second!r}"
        )
    print(f"{prefix}-cleanup-ok")
    return 0


def _verify_detached_stopped(prefix: str) -> int:
    if os.name == "nt":
        _verify_stopped(prefix)
        print(f"{prefix}-detached-cleanup-ok")
        return 0
    domain = json.loads(_path(prefix, "domain").read_text(encoding="utf-8"))
    child_pid = domain.get("child_pid")
    child_pgid = domain.get("child_pgid")
    start_token = domain.get("child_start_token")
    if (
        type(child_pid) is not int
        or child_pid <= 1
        or type(child_pgid) is not int
        or child_pgid <= 1
        or not isinstance(start_token, list)
        or len(start_token) != 2
        or any(type(value) is not int for value in start_token)
    ):
        raise RuntimeError(f"{prefix} recorded an invalid child identity")
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests" / "lib"))
    import process_containment as containment_module

    expected_identity = containment_module._PosixProcessIdentity(
        pid=child_pid,
        start_token=tuple(start_token),
    )
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        current = containment_module._read_posix_process_record(child_pid)
        if current is None or current.identity != expected_identity:
            print(f"{prefix}-detached-cleanup-ok")
            return 0
        time.sleep(0.05)
    current = containment_module._read_posix_process_record(child_pid)
    if (
        current is not None
        and current.identity == expected_identity
        and current.process_group_id == child_pgid
    ):
        os.killpg(child_pgid, signal.SIGKILL)
    raise RuntimeError(
        f"{prefix} detached descendant remained alive after containment cleanup: "
        f"pid={child_pid}"
    )


def _verify_pid_reuse_guard() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests"))
    from lib import process_containment as containment_module

    linux_stat_fields = ["R", "100", "200", "300"] + (["0"] * 15) + ["987654"]
    linux_record = containment_module._parse_linux_process_record(
        123,
        "123 (worker)name) " + " ".join(linux_stat_fields),
    )
    if (
        linux_record.parent_pid != 100
        or linux_record.process_group_id != 200
        or linux_record.session_id != 300
        or linux_record.identity.start_token != (0, 987654)
    ):
        raise RuntimeError(f"Linux /proc stat identity parsing drifted: {linux_record!r}")

    if os.name == "nt":
        print("pid-reuse-guard-ok")
        return 0

    expected_identity = containment_module._PosixProcessIdentity(
        pid=41001,
        start_token=(11, 22),
    )
    reused_identity = containment_module._PosixProcessIdentity(
        pid=41001,
        start_token=(33, 44),
    )
    expected_record = containment_module._PosixProcessRecord(
        identity=expected_identity,
        parent_pid=41000,
        process_group_id=41001,
        session_id=41001,
        state="R",
    )
    reused_record = containment_module._PosixProcessRecord(
        identity=reused_identity,
        parent_pid=1,
        process_group_id=41001,
        session_id=41001,
        state="R",
    )
    domain = containment_module._PosixDescendantDomain(
        leader=expected_identity,
        member_identities={expected_identity},
        session_ids={41001},
    )
    original_reader = containment_module._read_posix_process_record
    original_killpg = containment_module.os.killpg
    kill_calls: list[tuple[int, signal.Signals]] = []

    def reused_reader(pid: int) -> object:
        if pid != expected_identity.pid:
            raise RuntimeError(f"unexpected PID revalidation: {pid}")
        return reused_record

    def record_killpg(group_id: int, sig: signal.Signals) -> None:
        kill_calls.append((group_id, sig))

    containment_module._read_posix_process_record = reused_reader
    containment_module.os.killpg = record_killpg
    try:
        try:
            containment_module._signal_posix_descendant_groups(
                domain,
                (expected_record,),
                signal.SIGKILL,
                set(),
            )
        except containment_module._DescendantOwnershipError as exc:
            if "PID identity reuse" not in str(exc):
                raise RuntimeError(f"PID reuse diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("PID reuse did not fail closed")
    finally:
        containment_module._read_posix_process_record = original_reader
        containment_module.os.killpg = original_killpg
    if kill_calls:
        raise RuntimeError(f"PID reuse signaled an unowned group: {kill_calls!r}")

    runner_group_record = containment_module._PosixProcessRecord(
        identity=containment_module._PosixProcessIdentity(
            pid=41002,
            start_token=(55, 66),
        ),
        parent_pid=expected_identity.pid,
        process_group_id=os.getpgrp(),
        session_id=41001,
        state="R",
    )
    try:
        containment_module._extend_posix_descendant_domain(
            domain,
            {
                expected_identity.pid: expected_record,
                runner_group_record.identity.pid: runner_group_record,
            },
            require_live_leader=True,
        )
    except containment_module._DescendantOwnershipError as exc:
        if "runner's process group" not in str(exc):
            raise RuntimeError(f"runner PGID diagnostic drifted: {exc}") from exc
    else:
        raise RuntimeError("descendant entry into the runner PGID was accepted")

    _verify_cooperative_capability_contract(containment_module, repo_root)
    _verify_isolated_gate_environment(containment_module, repo_root)
    _verify_gate_error_protocol(repo_root)
    _verify_terminal_anchor_close_failure(containment_module)

    fast_result = containment_module.run_contained_command(
        ["/usr/bin/true"],
        cwd=Path.cwd(),
        shell=False,
        env=dict(os.environ),
        timeout_sec=2,
        containment_policy=(
            containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
        ),
    )
    if (
        fast_result.returncode != 0
        or fast_result.timed_out
        or fast_result.stdout
        or fast_result.stderr
    ):
        raise RuntimeError(f"fast exec-gate path drifted: {fast_result!r}")

    normal_result = containment_module.run_contained_command(
        [sys.executable, "-c", "print('normal-path-tracked')"],
        cwd=Path.cwd(),
        shell=False,
        env=dict(os.environ),
        timeout_sec=2,
        containment_policy=(
            containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
        ),
    )
    if (
        normal_result.returncode != 0
        or normal_result.timed_out
        or normal_result.stdout.strip() != "normal-path-tracked"
    ):
        raise RuntimeError(f"normal containment path drifted: {normal_result!r}")
    print("pid-reuse-guard-ok")
    return 0


def _verify_cooperative_capability_contract(
    containment_module: object,
    repo_root: Path,
) -> None:
    if os.name == "nt":
        return
    anchor_key = containment_module.POSIX_CONTAINMENT_ANCHOR_FDS_ENV
    original_anchor_stack = os.environ.get(anchor_key)
    outer_anchor_fds = containment_module.cooperative_posix_spawn_pass_fds()
    if not outer_anchor_fds:
        raise RuntimeError("outer cooperative anchor stack was empty")

    def expect_adapter_failure(raw_value: str | None, expected: str) -> None:
        if raw_value is None:
            os.environ.pop(anchor_key, None)
        else:
            os.environ[anchor_key] = raw_value
        try:
            containment_module.cooperative_posix_spawn_pass_fds()
        except containment_module.InvocationError as exc:
            if expected not in str(exc):
                raise RuntimeError(
                    f"cooperative adapter diagnostic drifted: {exc}"
                ) from exc
        else:
            raise RuntimeError(
                f"cooperative adapter accepted invalid stack {raw_value!r}"
            )

    try:
        expect_adapter_failure(None, "missing")
        expect_adapter_failure("03", "malformed")
        expect_adapter_failure(
            f"{outer_anchor_fds[0]}:{outer_anchor_fds[0]}",
            "duplicates",
        )
        expect_adapter_failure("2", "unsafe")

        with tempfile.TemporaryFile() as non_pipe:
            os.set_inheritable(non_pipe.fileno(), True)
            expect_adapter_failure(str(non_pipe.fileno()), "not a pipe")

        read_fd, write_fd = os.pipe()
        try:
            os.set_inheritable(read_fd, True)
            expect_adapter_failure(str(read_fd), "not write-only")
        finally:
            os.close(read_fd)
            os.close(write_fd)
    finally:
        if original_anchor_stack is None:
            os.environ.pop(anchor_key, None)
        else:
            os.environ[anchor_key] = original_anchor_stack

    unsupported_sentinel = _path("os-enforced-policy", "sentinel")
    if unsupported_sentinel.exists():
        unsupported_sentinel.unlink()
    try:
        containment_module.run_contained_command(
            [
                sys.executable,
                "-c",
                f"from pathlib import Path; Path({str(unsupported_sentinel)!r}).write_text('ran')",
            ],
            cwd=Path.cwd(),
            shell=False,
            env=dict(os.environ),
            timeout_sec=2.0,
            containment_policy=(
                containment_module.ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
            ),
        )
    except containment_module.InvocationError as exc:
        if "unavailable on this POSIX host" not in str(exc):
            raise RuntimeError(f"OS-enforced policy diagnostic drifted: {exc}") from exc
    else:
        raise RuntimeError("unsupported OS-enforced POSIX policy launched a command")
    if unsupported_sentinel.exists():
        raise RuntimeError("unsupported OS-enforced policy executed its target")

    target_library = repo_root / "tests" / "lib"
    nested_code = (
        "import sys; "
        f"sys.path.insert(0, {str(target_library)!r}); "
        "import process_containment as p; "
        "print(len(p.cooperative_posix_spawn_pass_fds()))"
    )
    nested_result = containment_module.run_contained_command(
        [sys.executable, "-c", nested_code],
        cwd=Path.cwd(),
        shell=False,
        env={},
        timeout_sec=5.0,
        containment_policy=(
            containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
        ),
    )
    if (
        nested_result.returncode != 0
        or nested_result.stdout.strip() != str(len(outer_anchor_fds) + 1)
        or not nested_result.containment_complete
    ):
        raise RuntimeError(
            f"nested cooperative anchor stack drifted: {nested_result!r}"
        )


def _verify_isolated_gate_environment(
    containment_module: object,
    repo_root: Path,
) -> None:
    site_root = Path(
        tempfile.mkdtemp(prefix="nebula-gate-site-", dir=Path("work"))
    ).resolve()
    sentinel = site_root / "sitecustomize-ran.txt"
    (site_root / "sitecustomize.py").write_text(
        "from pathlib import Path\n"
        f"Path({str(sentinel)!r}).write_text('target', encoding='utf-8')\n",
        encoding="utf-8",
    )
    tracker_type = containment_module._PosixDescendantTracker
    original_start_descriptor = tracker_type.__dict__["start"]
    capture_observed = False
    sentinel_observed = False

    def capture_without_sitecustomize(
        cls: object,
        proc: object,
        containment_token: str,
        anchor: tuple[int, int | None, int | None],
    ):
        nonlocal capture_observed
        if sentinel.exists():
            raise RuntimeError("sitecustomize ran before exec-gate domain capture")
        capture_observed = True
        return original_start_descriptor.__func__(
            cls,
            proc,
            containment_token,
            anchor,
        )

    tracker_type.start = classmethod(capture_without_sitecustomize)
    try:
        target_env = dict(os.environ)
        target_env["PYTHONPATH"] = str(site_root)
        result = containment_module.run_contained_command(
            [sys.executable, "-c", "print('isolated-gate-target-ok')"],
            cwd=repo_root,
            shell=False,
            env=target_env,
            timeout_sec=5.0,
            containment_policy=(
                containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
            ),
        )
    finally:
        sentinel_observed = sentinel.is_file()
        tracker_type.start = original_start_descriptor
        shutil.rmtree(site_root)
    if (
        not capture_observed
        or result.returncode != 0
        or result.stdout.strip() != "isolated-gate-target-ok"
        or not sentinel_observed
    ):
        raise RuntimeError(
            "isolated exec-gate environment contract drifted: "
            f"capture={capture_observed} result={result!r} "
            f"sentinel={sentinel_observed}"
        )


def _verify_gate_error_protocol(repo_root: Path) -> None:
    sys.path.insert(0, str(repo_root / "tests"))
    from lib import posix_exec_gate as gate_module

    gate_read_fd, gate_write_fd = os.pipe()
    error_read_fd, error_write_fd = os.pipe()
    anchor_read_fd, anchor_write_fd = os.pipe()
    anchor_key = gate_module.ANCHOR_FDS_ENV
    original_anchor_stack = os.environ.get(anchor_key)
    original_argv = sys.argv
    original_set_inheritable = gate_module.os.set_inheritable

    def fail_anchor_inheritance(fd: int, inheritable: bool) -> None:
        if fd == anchor_write_fd and inheritable:
            raise OSError(errno.EIO, "injected anchor inheritance failure")
        original_set_inheritable(fd, inheritable)

    try:
        original_set_inheritable(anchor_write_fd, True)
        os.environ[anchor_key] = str(anchor_write_fd)
        sys.argv = [
            str(repo_root / "tests" / "lib" / "posix_exec_gate.py"),
            str(gate_read_fd),
            str(error_write_fd),
            str(anchor_write_fd),
            "/usr/bin/true",
        ]
        gate_module.os.set_inheritable = fail_anchor_inheritance
        return_code = gate_module.main()
        os.close(error_write_fd)
        error_write_fd = -1
        diagnostic = os.read(error_read_fd, gate_module.ERROR_MESSAGE_MAX_BYTES)
    finally:
        gate_module.os.set_inheritable = original_set_inheritable
        sys.argv = original_argv
        if original_anchor_stack is None:
            os.environ.pop(anchor_key, None)
        else:
            os.environ[anchor_key] = original_anchor_stack
        for fd in (
            gate_read_fd,
            gate_write_fd,
            error_read_fd,
            error_write_fd,
            anchor_read_fd,
            anchor_write_fd,
        ):
            if fd >= 0:
                os.close(fd)
    if (
        return_code != 125
        or not diagnostic.startswith(gate_module.ERROR_STATUS_PREFIX)
        or b"injected anchor inheritance failure" not in diagnostic
    ):
        raise RuntimeError(
            "exec-gate error protocol lost its diagnostic: "
            f"rc={return_code} diagnostic={diagnostic!r}"
        )


def _verify_terminal_anchor_close_failure(containment_module: object) -> None:
    original_close_anchor = containment_module._close_posix_anchor
    observed_domains: list[object] = []
    close_calls = 0

    def fail_first_anchor_close(domain: object) -> None:
        nonlocal close_calls
        close_calls += 1
        observed_domains.append(domain)
        if close_calls == 1:
            raise containment_module.InvocationError(
                "injected terminal anchor close failure"
            )
        original_close_anchor(domain)

    containment_module._close_posix_anchor = fail_first_anchor_close
    failure = ""
    try:
        try:
            containment_module.run_contained_command(
                ["/usr/bin/true"],
                cwd=Path.cwd(),
                shell=False,
                env=dict(os.environ),
                timeout_sec=2.0,
                containment_policy=(
                    containment_module.ProcessContainmentPolicy.TRUSTED_COOPERATIVE
                ),
            )
        except containment_module.InvocationError as exc:
            failure = str(exc)
        else:
            raise RuntimeError("terminal anchor close failure was silently accepted")
    finally:
        containment_module._close_posix_anchor = original_close_anchor
        for domain in observed_domains:
            if domain.anchor_read_fd >= 0:
                original_close_anchor(domain)
    if (
        "injected terminal anchor close failure" not in failure
        or close_calls < 2
        or not observed_domains
        or any(domain.anchor_read_fd >= 0 for domain in observed_domains)
    ):
        raise RuntimeError(
            "terminal anchor close failure did not converge: "
            f"failure={failure!r} calls={close_calls}"
        )


def _verify_structured_infrastructure_failure() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests"))
    from lib import fs_sandbox as fs_sandbox_module
    from lib import nebula_invoker as invoker_module
    from lib import process_containment as containment_module
    from lib.fs_sandbox import cleanup_case_sandbox
    from lib import runner as runner_module
    from lib.process_containment import InvocationError

    calls: list[str] = []
    original_run_step = runner_module.run_step

    def fake_run_step(*args: object, **kwargs: object) -> dict[str, object]:
        calls.append("called")
        return {
            "kind": "shell",
            "cmd": ["ignored"],
            "cmd_str": "ignored",
            "rc": 125,
            "output": "partial-output\n",
            "duration_ms": 1,
            "timed_out": True,
            "infrastructure_error": "injected cleanup failure",
        }

    cases = [
        {
            "id": "nested-infrastructure-failure",
            "suite": "test",
            "steps": [{"kind": "shell", "run": "ignored", "expect_rc": 125}],
        },
        {
            "id": "must-not-run-after-infrastructure-failure",
            "suite": "test",
            "steps": [{"kind": "shell", "run": "ignored"}],
        },
    ]
    config = runner_module.RunnerConfig(
        binary=Path(sys.executable),
        tests_root=repo_root / "tests",
        keep_temp=False,
    )
    results: list[dict[str, object]] = []
    runner_module.run_step = fake_run_step
    try:
        results = runner_module.run_cases(cases, config)
    finally:
        runner_module.run_step = original_run_step
        for result in results:
            sandbox = str(result.get("sandbox", ""))
            if sandbox:
                cleanup_case_sandbox(Path(sandbox))

    if len(calls) != 1 or len(results) != 1:
        raise RuntimeError("runner continued after an injected infrastructure failure")
    result = results[0]
    if result.get("status") != "failed" or result.get("rc") != 125:
        raise RuntimeError(f"infrastructure failure result was not forced to fail: {result!r}")
    if result.get("infrastructure_error") != "injected cleanup failure":
        raise RuntimeError(f"infrastructure failure context was lost: {result!r}")
    if "partial-output" not in str(result.get("output", "")):
        raise RuntimeError(f"partial infrastructure output was lost: {result!r}")
    steps = result.get("steps")
    if not isinstance(steps, list) or len(steps) != 1:
        raise RuntimeError(f"infrastructure step report was not retained: {result!r}")
    step = steps[0]
    if not isinstance(step, dict) or not step.get("timed_out"):
        raise RuntimeError(f"infrastructure timeout state was not retained: {result!r}")

    class OwnershipLostProcess:
        returncode = None
        kill_called = False
        wait_called = False

        def kill(self) -> None:
            self.kill_called = True
            raise RuntimeError("ownership-lost cleanup attempted a direct kill")

        def wait(self, timeout: float) -> int:
            self.wait_called = True
            raise RuntimeError("ownership-lost cleanup attempted a reap")

    class FinishedStreams:
        finish_called = False

        def finish(self) -> tuple[str, str]:
            self.finish_called = True
            return "", ""

        def abort(self) -> None:
            raise RuntimeError("ownership-lost cleanup unexpectedly aborted output")

    ownership_lost_process = OwnershipLostProcess()
    finished_streams = FinishedStreams()
    cleanup_errors = containment_module._force_posix_cleanup(
        ownership_lost_process,
        finished_streams,
        safe_to_signal_group=False,
        release_unsealed_anchor=True,
    )
    if (
        cleanup_errors
        or ownership_lost_process.kill_called
        or ownership_lost_process.wait_called
        or not finished_streams.finish_called
    ):
        raise RuntimeError(
            "ownership-lost cleanup signaled an unowned PID or lost output cleanup: "
            f"errors={cleanup_errors!r}"
        )

    original_run_contained_command = invoker_module.run_contained_command

    def fail_contained_command(*args: object, **kwargs: object) -> object:
        raise InvocationError(
            "injected invocation ownership failure",
            output="invocation-partial-output\n",
            timed_out=True,
        )

    invoker_module.run_contained_command = fail_contained_command
    try:
        invocation_result = invoker_module.run_step(
            {"kind": "shell", "run": "ignored"},
            Path(sys.executable),
            Path.cwd(),
        )
    finally:
        invoker_module.run_contained_command = original_run_contained_command
    if (
        invocation_result.get("rc") != 125
        or invocation_result.get("timed_out") is not True
        or invocation_result.get("infrastructure_error")
        != "injected invocation ownership failure"
        or "invocation-partial-output" not in str(invocation_result.get("output", ""))
        or "[nebula-test-infrastructure]" not in str(invocation_result.get("output", ""))
    ):
        raise RuntimeError(
            "InvocationError was not preserved as structured rc=125 output: "
            f"{invocation_result!r}"
        )

    original_mkdtemp = fs_sandbox_module.tempfile.mkdtemp
    original_copytree = fs_sandbox_module.shutil.copytree
    original_rmtree = fs_sandbox_module.shutil.rmtree
    allocated_paths: list[Path] = []

    def recording_mkdtemp(*args: object, **kwargs: object) -> str:
        path = original_mkdtemp(*args, **kwargs)
        allocated_paths.append(Path(path))
        return path

    def fail_copytree(*args: object, **kwargs: object) -> None:
        raise PermissionError("injected fixture copy failure")

    fs_sandbox_module.tempfile.mkdtemp = recording_mkdtemp
    fs_sandbox_module.shutil.copytree = fail_copytree
    try:
        try:
            fs_sandbox_module.make_case_sandbox(
                "transactional-sandbox-probe",
                repo_root / "tests",
            )
        except fs_sandbox_module.SandboxCreationError as exc:
            if "failed to initialize case sandbox" not in str(exc):
                raise RuntimeError(f"sandbox initialization diagnostic drifted: {exc}") from exc
        else:
            raise RuntimeError("fixture copy failure did not fail sandbox initialization")
    finally:
        fs_sandbox_module.tempfile.mkdtemp = original_mkdtemp
        fs_sandbox_module.shutil.copytree = original_copytree
    if not allocated_paths or allocated_paths[-1].exists():
        raise RuntimeError(
            f"partial sandbox was not transactionally removed: {allocated_paths!r}"
        )

    def fail_rmtree(*args: object, **kwargs: object) -> None:
        raise PermissionError("injected partial sandbox cleanup failure")

    allocated_paths.clear()
    fs_sandbox_module.tempfile.mkdtemp = recording_mkdtemp
    fs_sandbox_module.shutil.copytree = fail_copytree
    fs_sandbox_module.shutil.rmtree = fail_rmtree
    partial_path: Path | None = None
    try:
        try:
            fs_sandbox_module.make_case_sandbox(
                "sandbox-cleanup-failure-probe",
                repo_root / "tests",
            )
        except fs_sandbox_module.SandboxCreationError as exc:
            partial_path = exc.partial_path
            if partial_path is None or "failed to remove" not in str(exc):
                raise RuntimeError(f"partial sandbox ownership was lost: {exc}") from exc
        else:
            raise RuntimeError("partial sandbox cleanup failure was silently accepted")
    finally:
        fs_sandbox_module.tempfile.mkdtemp = original_mkdtemp
        fs_sandbox_module.shutil.copytree = original_copytree
        fs_sandbox_module.shutil.rmtree = original_rmtree
        if partial_path is not None and partial_path.exists():
            cleanup_case_sandbox(partial_path)

    original_make_case_sandbox = runner_module.make_case_sandbox
    original_python_shim_dir = runner_module._python_shim_dir
    original_cleanup_case_sandbox = runner_module.cleanup_case_sandbox

    def verify_boundary_failure(
        boundary: str,
        *,
        expected_run_calls: int,
        expected_step_count: int,
        expected_sandbox: bool,
    ) -> None:
        boundary_calls: list[str] = []
        boundary_cases = [
            {
                "id": f"nested-{boundary.replace(' ', '-')}-failure",
                "suite": "test",
                "steps": [{"kind": "shell", "run": "ignored", "expect_rc": 0}],
            },
            {
                "id": "must-not-run-after-boundary-failure",
                "suite": "test",
                "steps": [{"kind": "shell", "run": "ignored", "expect_rc": 0}],
            },
        ]

        def successful_run_step(*args: object, **kwargs: object) -> dict[str, object]:
            boundary_calls.append("called")
            return {
                "kind": "shell",
                "cmd": ["ignored"],
                "cmd_str": "ignored",
                "rc": 0,
                "output": "nested-step-ok\n",
                "duration_ms": 1,
                "timed_out": False,
                "infrastructure_error": "",
            }

        boundary_results: list[dict[str, object]] = []
        preserved_sandbox_existed = False
        runner_module.run_step = successful_run_step
        if boundary == "sandbox setup":
            def fail_sandbox(*args: object, **kwargs: object) -> Path:
                raise PermissionError("injected sandbox setup failure")

            runner_module.make_case_sandbox = fail_sandbox
        elif boundary == "partial sandbox setup":
            partial_sandbox = Path(
                fs_sandbox_module.tempfile.mkdtemp(
                    prefix="nebula-partial-runner-boundary-"
                )
            )

            def fail_partial_sandbox(*args: object, **kwargs: object) -> Path:
                raise fs_sandbox_module.SandboxCreationError(
                    "injected partial sandbox setup failure",
                    partial_path=partial_sandbox,
                )

            runner_module.make_case_sandbox = fail_partial_sandbox
        elif boundary == "python shim setup":
            def fail_shim(*args: object, **kwargs: object) -> Path:
                raise PermissionError("injected python shim setup failure")

            runner_module._python_shim_dir = fail_shim
        elif boundary == "sandbox cleanup":
            def fail_cleanup(*args: object, **kwargs: object) -> None:
                raise PermissionError("injected sandbox cleanup failure")

            runner_module.cleanup_case_sandbox = fail_cleanup
        else:
            raise RuntimeError(f"unknown injected runner boundary: {boundary}")

        try:
            boundary_results = runner_module.run_cases(boundary_cases, config)
        finally:
            runner_module.run_step = original_run_step
            runner_module.make_case_sandbox = original_make_case_sandbox
            runner_module._python_shim_dir = original_python_shim_dir
            runner_module.cleanup_case_sandbox = original_cleanup_case_sandbox
            for boundary_result in boundary_results:
                sandbox = str(boundary_result.get("sandbox", ""))
                if sandbox and Path(sandbox).exists():
                    preserved_sandbox_existed = True
                    cleanup_case_sandbox(Path(sandbox))

        if len(boundary_calls) != expected_run_calls or len(boundary_results) != 1:
            raise RuntimeError(
                f"runner did not abort after {boundary}: "
                f"calls={len(boundary_calls)} results={boundary_results!r}"
            )
        boundary_result = boundary_results[0]
        if (
            boundary_result.get("status") != "failed"
            or boundary_result.get("rc") != 125
            or boundary not in str(boundary_result.get("infrastructure_error", ""))
        ):
            raise RuntimeError(
                f"{boundary} was not recorded as rc=125: {boundary_result!r}"
            )
        boundary_steps = boundary_result.get("steps")
        if not isinstance(boundary_steps, list) or len(boundary_steps) != expected_step_count:
            raise RuntimeError(
                f"{boundary} retained an invalid step record: {boundary_result!r}"
            )
        if preserved_sandbox_existed is not expected_sandbox:
            raise RuntimeError(
                f"{boundary} sandbox preservation drifted: "
                f"expected={expected_sandbox} actual={preserved_sandbox_existed}"
            )

    verify_boundary_failure(
        "sandbox setup",
        expected_run_calls=0,
        expected_step_count=0,
        expected_sandbox=False,
    )
    verify_boundary_failure(
        "partial sandbox setup",
        expected_run_calls=0,
        expected_step_count=0,
        expected_sandbox=True,
    )
    verify_boundary_failure(
        "python shim setup",
        expected_run_calls=0,
        expected_step_count=0,
        expected_sandbox=True,
    )
    verify_boundary_failure(
        "sandbox cleanup",
        expected_run_calls=1,
        expected_step_count=1,
        expected_sandbox=True,
    )
    print("structured-infrastructure-failure-ok")
    return 0


def _verify_windows_shell_adapter() -> int:
    repo_root = Path(os.environ["NEBULA_REPO_ROOT"])
    sys.path.insert(0, str(repo_root / "tests"))
    from lib import nebula_invoker as invoker_module
    from lib.process_containment import InvocationError

    if (
        invoker_module._windows_interoperable_path(
            r"C:\nebula repo\build\nebula.exe"
        )
        != "C:/nebula repo/build/nebula.exe"
    ):
        raise RuntimeError("Windows interoperable path normalization drifted")
    pure_command = invoker_module._windows_shell_command(
        Path(r"C:\msys64\usr\bin\bash.exe"),
        Path(r"C:\sandbox\.nebula-shell-step-test.sh"),
    )
    if pure_command != [
        "C:/msys64/usr/bin/bash.exe",
        "--noprofile",
        "--norc",
        "C:/sandbox/.nebula-shell-step-test.sh",
    ]:
        raise RuntimeError(f"Windows shell argv construction drifted: {pure_command!r}")

    root = Path(tempfile.mkdtemp(prefix="nebula-windows-shell-adapter-", dir="work"))
    fake_bash = (root / "bash.exe").resolve()
    fake_bash.write_bytes(b"simulated-bash")
    original_is_windows_host = invoker_module._is_windows_host
    original_resolve_windows_bash = invoker_module._resolve_windows_bash
    original_run_contained_command = invoker_module.run_contained_command
    observed_scripts: list[Path] = []

    class SimulatedResult:
        returncode = 0
        stdout = "windows-shell-simulated-ok\n"
        timed_out = False

    large_source = "printf 'windows-shell-simulated-ok\\n'\n" + ("#" * 40000)
    path_environment = {
        "NEBULA_BINARY": r"C:\repo\build\nebula.exe",
        "NEBULA_REPO_ROOT": r"C:\repo",
        "NEBULA_TESTS_ROOT": r"C:\repo\tests",
        "NEBULA_TEST_PYTHON": r"C:\msys64\clang64\bin\python.exe",
        "PYTHON": r"C:\msys64\clang64\bin\python.exe",
    }

    def simulate_contained_command(
        command: object,
        *,
        cwd: Path,
        shell: bool,
        env: dict[str, str],
        timeout_sec: int,
        containment_policy: object,
    ) -> SimulatedResult:
        if not isinstance(command, list) or len(command) != 4:
            raise RuntimeError(f"Windows shell command was not an argv: {command!r}")
        if shell:
            raise RuntimeError("Windows shell adapter bypassed direct process containment")
        expected_bash = invoker_module._windows_interoperable_path(str(fake_bash))
        if command[:3] != [expected_bash, "--noprofile", "--norc"]:
            raise RuntimeError(f"Windows Bash boundary drifted: {command!r}")
        if timeout_sec != 17 or cwd.resolve() != root.resolve():
            raise RuntimeError("Windows shell containment arguments drifted")
        if (
            containment_policy
            is not invoker_module.ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
        ):
            raise RuntimeError("Windows shell did not request OS-enforced containment")
        if any(large_source in argument for argument in command):
            raise RuntimeError("large Windows shell source leaked into CreateProcess argv")
        script = Path(command[3])
        observed_scripts.append(script)
        if not script.is_file() or script.parent != root.resolve():
            raise RuntimeError(f"temporary shell script escaped its sandbox: {script}")
        if script.read_text(encoding="utf-8") != large_source:
            raise RuntimeError("temporary Windows shell script content drifted")
        expected_environment = {
            key: value.replace("\\", "/") for key, value in path_environment.items()
        }
        if any(env.get(key) != value for key, value in expected_environment.items()):
            raise RuntimeError("Windows shell path environment was not C:/ normalized")
        return SimulatedResult()

    def fail_contained_command(*args: object, **kwargs: object) -> object:
        command = args[0]
        if not isinstance(command, list):
            raise RuntimeError("failing Windows shell command was not an argv")
        observed_scripts.append(Path(command[-1]))
        raise InvocationError(
            "simulated Windows containment failure",
            output="windows-shell-partial-output\n",
            timed_out=True,
        )

    try:
        invoker_module._is_windows_host = lambda: True
        invoker_module._resolve_windows_bash = lambda: fake_bash
        invoker_module.run_contained_command = simulate_contained_command
        success = invoker_module.run_step(
            {"kind": "shell", "run": large_source},
            Path(sys.executable),
            root,
            timeout_sec=17,
            extra_env=path_environment,
        )
        if (
            success.get("rc") != 0
            or success.get("infrastructure_error")
            or success.get("output") != "windows-shell-simulated-ok\n"
        ):
            raise RuntimeError(f"simulated Windows shell success drifted: {success!r}")
        if not observed_scripts or any(path.exists() for path in observed_scripts):
            raise RuntimeError("successful Windows shell step leaked its temporary script")

        observed_scripts.clear()
        invoker_module.run_contained_command = fail_contained_command
        failure = invoker_module.run_step(
            {"kind": "shell", "run": "exit 9"},
            Path(sys.executable),
            root,
            timeout_sec=17,
            extra_env=path_environment,
        )
        if (
            failure.get("rc") != 125
            or failure.get("timed_out") is not True
            or failure.get("infrastructure_error")
            != "simulated Windows containment failure"
            or "windows-shell-partial-output" not in str(failure.get("output", ""))
        ):
            raise RuntimeError(f"simulated Windows shell failure drifted: {failure!r}")
        if not observed_scripts or any(path.exists() for path in observed_scripts):
            raise RuntimeError("failing Windows shell step leaked its temporary script")

        original_which = invoker_module.shutil.which
        invoker_module.shutil.which = lambda command: None
        try:
            try:
                original_resolve_windows_bash()
            except InvocationError as exc:
                if "require an absolute bash.exe" not in str(exc):
                    raise RuntimeError(f"missing Bash diagnostic drifted: {exc}") from exc
            else:
                raise RuntimeError("missing Windows bash.exe did not fail fast")
        finally:
            invoker_module.shutil.which = original_which
    finally:
        invoker_module._is_windows_host = original_is_windows_host
        invoker_module._resolve_windows_bash = original_resolve_windows_bash
        invoker_module.run_contained_command = original_run_contained_command
        shutil.rmtree(root)

    print("windows-shell-adapter-ok")
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        raise RuntimeError("expected one runner process-tree probe mode")
    mode = sys.argv[1]
    if mode == "heartbeat-child":
        raise RuntimeError("heartbeat-child requires a prefix")
    if mode == "timeout-start":
        return _start("timeout", sleep_after_ready=True, return_code=0)
    if mode == "timeout-verify":
        return _verify_stopped("timeout")
    if mode == "nested-session-timeout-start":
        return _start_nested_session_timeout("nested-session")
    if mode == "nested-session-timeout-verify":
        return _verify_stopped("nested-session")
    if mode == "nested-session-success-start":
        return _start_nested_session_exit("nested-session-success", 0)
    if mode == "nested-session-success-verify":
        return _verify_detached_stopped("nested-session-success")
    if mode == "nested-session-nonzero-start":
        return _start_nested_session_exit("nested-session-nonzero", 7)
    if mode == "nested-session-nonzero-verify":
        return _verify_detached_stopped("nested-session-nonzero")
    if mode == "immediate-detached-oracle":
        return _run_immediate_detached_oracle()
    if mode == "native-anchor-oracle":
        return _run_native_anchor_oracle()
    if mode == "long-lived-immediate-detached-oracle":
        return _run_long_lived_immediate_detached_oracle()
    if mode == "long-lived-retry-oracle":
        return _run_long_lived_retry_oracle()
    if mode == "sigterm-late-spawn-oracle":
        return _run_sigterm_late_spawn_oracle()
    if mode == "pid-reuse-guard":
        return _verify_pid_reuse_guard()
    if mode == "service-timeout-start":
        return _start_contained_service_timeout("service-timeout")
    if mode == "service-timeout-verify":
        return _verify_stopped("service-timeout")
    if mode == "success-start":
        return _start("success", sleep_after_ready=False, return_code=0)
    if mode == "success-verify":
        return _verify_stopped("success")
    if mode == "nonzero-start":
        return _start("nonzero", sleep_after_ready=False, return_code=7)
    if mode == "nonzero-verify":
        return _verify_stopped("nonzero")
    if mode == "structured-infrastructure-failure":
        return _verify_structured_infrastructure_failure()
    if mode == "windows-shell-adapter":
        return _verify_windows_shell_adapter()
    raise RuntimeError(f"unknown runner process-tree probe mode: {mode}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "heartbeat-child":
        raise SystemExit(_heartbeat_child(sys.argv[2]))
    if len(sys.argv) == 3 and sys.argv[1] == "contained-heartbeat-child":
        raise SystemExit(_contained_heartbeat_child(sys.argv[2]))
    if len(sys.argv) == 3 and sys.argv[1] == "nested-session-parent":
        raise SystemExit(_nested_session_parent(sys.argv[2]))
    if len(sys.argv) == 5 and sys.argv[1] == "immediate-detached-child":
        raise SystemExit(
            _immediate_detached_child(
                sys.argv[2],
                int(sys.argv[3]),
                int(sys.argv[4]),
            )
        )
    if len(sys.argv) == 4 and sys.argv[1] == "immediate-detached-exit":
        raise SystemExit(
            _start_immediate_detached_exit(sys.argv[2], int(sys.argv[3]))
        )
    if len(sys.argv) == 3 and sys.argv[1] == "native-anchor-detached-exit":
        raise SystemExit(_start_native_anchor_detached_exit(sys.argv[2]))
    if len(sys.argv) == 3 and sys.argv[1] == "sigterm-late-spawn-target":
        raise SystemExit(_run_sigterm_late_spawn_target(sys.argv[2]))
    raise SystemExit(main())
