from __future__ import annotations

import shlex
import subprocess
import time
import os
from pathlib import Path
from typing import Any


class InvocationError(RuntimeError):
    pass


def _stringify_cmd(cmd: list[str]) -> str:
    return " ".join(shlex.quote(x) for x in cmd)


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
        proc = subprocess.run(
            cmd[0],
            cwd=str(cwd),
            shell=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=timeout_sec,
        )
    else:
        proc = subprocess.run(
            cmd,
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=timeout_sec,
        )
    duration_ms = int((time.perf_counter() - t0) * 1000)

    return {
        "kind": kind,
        "cmd": cmd,
        "cmd_str": _stringify_cmd(cmd),
        "rc": proc.returncode,
        "output": proc.stdout,
        "duration_ms": duration_ms,
    }
