from __future__ import annotations

import shlex
import signal
import subprocess
import time
import os
from pathlib import Path
from typing import Any


class InvocationError(RuntimeError):
    pass


TIMEOUT_RETURN_CODE = 124


def _stringify_cmd(cmd: list[str]) -> str:
    return " ".join(shlex.quote(x) for x in cmd)


def _terminate_process_tree(proc: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
                check=False,
            )
            return
        except Exception:
            proc.kill()
            return

    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except Exception:
        if proc.poll() is None:
            proc.terminate()

    try:
        proc.wait(timeout=5)
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except Exception:
        if proc.poll() is None:
            proc.kill()


def _run_command(
    cmd: list[str] | str,
    cwd: Path,
    shell: bool,
    env: dict[str, str],
    timeout_sec: int,
) -> tuple[int, str, bool]:
    creationflags = 0
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)

    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        shell=shell,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        start_new_session=(os.name != "nt"),
        creationflags=creationflags,
    )
    try:
        stdout, _ = proc.communicate(timeout=timeout_sec)
        return proc.returncode, stdout or "", False
    except subprocess.TimeoutExpired:
        _terminate_process_tree(proc)
        try:
            stdout, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            _terminate_process_tree(proc)
            stdout, _ = proc.communicate(timeout=5)
        output = stdout or ""
        output += (
            f"\n[nebula-test-timeout] command timed out after {timeout_sec}s; "
            "terminated process group\n"
        )
        return TIMEOUT_RETURN_CODE, output, True


def run_step(
    step: dict[str, Any],
    binary: Path,
    cwd: Path,
    timeout_sec: int = 120,
    extra_env: dict[str, str] | None = None,
) -> dict[str, Any]:
    kind = step["kind"]

    if kind == "nebula":
        cmd: list[str] = [str(binary), str(step["command"])]
        source = step.get("source")
        if isinstance(source, str) and source:
            cmd.append(source)
        cmd.extend(str(x) for x in step.get("args", []))
        shell = False
    elif kind == "shell":
        cmd = [step["run"]]
        shell = True
    else:
        raise InvocationError(f"unsupported step kind: {kind}")

    t0 = time.perf_counter()
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)

    if shell:
        rc, output, timed_out = _run_command(cmd[0], cwd, True, env, timeout_sec)
    else:
        rc, output, timed_out = _run_command(cmd, cwd, False, env, timeout_sec)
    duration_ms = int((time.perf_counter() - t0) * 1000)

    return {
        "kind": kind,
        "cmd": cmd,
        "cmd_str": _stringify_cmd(cmd),
        "rc": rc,
        "output": output,
        "duration_ms": duration_ms,
        "timed_out": timed_out,
    }
