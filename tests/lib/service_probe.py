from __future__ import annotations

import json
import selectors
import subprocess
import time
from pathlib import Path
from typing import Any


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


def finish_process_output(proc: subprocess.Popen[str], stderr_chunks: list[str]) -> tuple[str, str]:
    if proc.poll() is None:
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
    else:
        stdout, stderr = proc.communicate(timeout=5)
    stderr_chunks.append(stderr)
    return stdout, "".join(stderr_chunks)


def fail_with_output(proc: subprocess.Popen[str], stderr_chunks: list[str], message: str) -> None:
    stdout, stderr = finish_process_output(proc, stderr_chunks)
    print(stdout, end="")
    print(stderr, end="")
    raise SystemExit(message)


def wait_for_observe_event(proc: subprocess.Popen[str],
                           stderr_chunks: list[str],
                           event_name: str,
                           timeout: float) -> dict[str, Any]:
    if proc.stderr is None:
        raise ValueError("wait_for_observe_event requires proc.stderr=PIPE")
    selector = selectors.DefaultSelector()
    selector.register(proc.stderr, selectors.EVENT_READ)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                fail_with_output(proc, stderr_chunks, f"service exited early while waiting for {event_name}: rc={proc.returncode}")
            for key, _ in selector.select(timeout=0.05):
                line = key.fileobj.readline()
                if not line:
                    continue
                stderr_chunks.append(line)
                for payload in collect_observe_events(line):
                    if payload.get("event") == event_name:
                        return payload
    finally:
        selector.close()
    fail_with_output(proc, stderr_chunks, f"timed out waiting for {event_name}")


def wait_for_listener_bound(proc: subprocess.Popen[str],
                            stderr_chunks: list[str],
                            timeout: float = 10.0) -> tuple[dict[str, Any], int]:
    payload = wait_for_observe_event(proc, stderr_chunks, "listener_bound", timeout)
    port = payload.get("port")
    if not isinstance(port, int) or port <= 0:
        fail_with_output(proc, stderr_chunks, f"invalid listener_bound port payload: {payload!r}")
    return payload, port


def wait_for_observe_event_in_file(log_path: Path,
                                   event_name: str,
                                   timeout: float,
                                   proc: subprocess.Popen[str] | None = None) -> dict[str, Any]:
    deadline = time.time() + timeout
    offset = 0
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None and not log_path.exists():
            raise SystemExit(f"process exited early before writing {event_name}: rc={proc.returncode}")
        if log_path.exists():
            with log_path.open("r", encoding="utf-8", errors="replace") as handle:
                handle.seek(offset)
                chunk = handle.read()
                offset = handle.tell()
            for payload in collect_observe_events(chunk):
                if payload.get("event") == event_name:
                    return payload
        time.sleep(0.05)
    if proc is not None and proc.poll() is not None:
        raise SystemExit(f"process exited before {event_name} appeared: rc={proc.returncode}")
    raise SystemExit(f"timed out waiting for {event_name} in {log_path}")
