from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import os
import shlex
import shutil
import tempfile
import time
from pathlib import Path
from typing import Any

from .process_containment import (
    INFRASTRUCTURE_ERROR_RETURN_CODE,
    InvocationError,
    ProcessContainmentPolicy,
    run_contained_command,
)


_WINDOWS_SHELL_PATH_ENVIRONMENT_KEYS = (
    "NEBULA_BINARY",
    "NEBULA_REPO_ROOT",
    "NEBULA_TESTS_ROOT",
    "NEBULA_TEST_PYTHON",
    "PYTHON",
)


def _stringify_cmd(cmd: list[str]) -> str:
    return " ".join(shlex.quote(x) for x in cmd)


def _output_with_marker(output: str, marker: str) -> str:
    separator = "" if not output or output.endswith("\n") else "\n"
    return f"{output}{separator}{marker}\n"


def _is_windows_host() -> bool:
    return os.name == "nt"


def _windows_interoperable_path(value: str) -> str:
    """Render a native Windows path for both MSYS Bash and native programs."""
    return value.replace("\\", "/")


def _normalize_windows_shell_environment(environment: dict[str, str]) -> None:
    for key in _WINDOWS_SHELL_PATH_ENVIRONMENT_KEYS:
        value = environment.get(key)
        if value:
            environment[key] = _windows_interoperable_path(value)


def _resolve_windows_bash() -> Path:
    candidate = shutil.which("bash.exe")
    if candidate is None:
        raise InvocationError(
            "Windows contract shell steps require an absolute bash.exe on PATH"
        )
    try:
        resolved = Path(candidate).resolve(strict=True)
    except OSError as exc:
        raise InvocationError(f"failed to resolve Windows bash.exe: {exc}") from exc
    if not resolved.is_absolute() or not resolved.is_file():
        raise InvocationError(
            "Windows contract shell steps require bash.exe to resolve to an absolute file"
        )
    return resolved


def _windows_shell_command(bash: Path, script: Path) -> list[str]:
    return [
        _windows_interoperable_path(str(bash)),
        "--noprofile",
        "--norc",
        _windows_interoperable_path(str(script)),
    ]


def _remove_windows_shell_script(script: Path) -> str:
    try:
        script.unlink()
    except FileNotFoundError:
        return ""
    except OSError as exc:
        return f"failed to remove temporary Windows shell script {script}: {exc}"
    return ""


def _create_windows_shell_script(cwd: Path, source: str) -> Path:
    try:
        sandbox = cwd.resolve(strict=True)
    except OSError as exc:
        raise InvocationError(f"failed to resolve Windows shell sandbox: {exc}") from exc
    if not sandbox.is_dir():
        raise InvocationError("Windows shell sandbox is not a directory")

    descriptor = -1
    script: Path | None = None
    try:
        # mkstemp uses O_CREAT|O_EXCL and keeps the script inside the owned case
        # sandbox. The file is closed before Bash is launched on Windows.
        descriptor, name = tempfile.mkstemp(
            prefix=".nebula-shell-step-",
            suffix=".sh",
            dir=sandbox,
        )
        script = Path(name)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            descriptor = -1
            stream.write(source)
        return script
    except BaseException as exc:
        cleanup_errors: list[str] = []
        if descriptor >= 0:
            try:
                os.close(descriptor)
            except OSError as close_exc:
                cleanup_errors.append(f"descriptor close failed: {close_exc}")
        if script is not None:
            cleanup_error = _remove_windows_shell_script(script)
            if cleanup_error:
                cleanup_errors.append(cleanup_error)
        cleanup_detail = ""
        if cleanup_errors:
            cleanup_detail = "; cleanup failures: " + "; ".join(cleanup_errors)
        if not isinstance(exc, Exception):
            if cleanup_detail:
                exc.add_note(cleanup_detail.removeprefix("; "))
            raise
        raise InvocationError(
            f"failed to create temporary Windows shell script: {exc}{cleanup_detail}"
        ) from exc


@contextmanager
def _temporary_windows_shell_script(cwd: Path, source: str) -> Iterator[Path]:
    script = _create_windows_shell_script(cwd, source)
    try:
        yield script
    except InvocationError as exc:
        cleanup_error = _remove_windows_shell_script(script)
        if cleanup_error:
            raise InvocationError(
                f"{exc}; {cleanup_error}",
                output=exc.output,
                timed_out=exc.timed_out,
            ) from exc
        raise
    except BaseException as exc:
        cleanup_error = _remove_windows_shell_script(script)
        if cleanup_error:
            if not isinstance(exc, Exception):
                exc.add_note(cleanup_error)
                raise
            raise InvocationError(
                "Windows shell execution and temporary-script cleanup failed: "
                f"execution={type(exc).__name__}: {exc}; cleanup={cleanup_error}"
            ) from exc
        raise
    else:
        cleanup_error = _remove_windows_shell_script(script)
        if cleanup_error:
            raise InvocationError(cleanup_error)


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
        shell_source: str | None = None
    elif kind == "shell":
        shell_source = str(step["run"])
        cmd = [] if _is_windows_host() else [shell_source]
        shell = not _is_windows_host()
    else:
        raise InvocationError(f"unsupported step kind: {kind}")

    t0 = time.perf_counter()
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    if shell_source is not None and _is_windows_host():
        _normalize_windows_shell_environment(env)

    infrastructure_error = ""
    try:
        if shell_source is not None and _is_windows_host():
            bash = _resolve_windows_bash()
            with _temporary_windows_shell_script(cwd, shell_source) as script:
                cmd = _windows_shell_command(bash, script)
                result = run_contained_command(
                    cmd,
                    cwd=cwd,
                    shell=False,
                    env=env,
                    timeout_sec=timeout_sec,
                    containment_policy=(
                        ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
                        if _is_windows_host()
                        else ProcessContainmentPolicy.TRUSTED_COOPERATIVE
                    ),
                )
        else:
            result = run_contained_command(
                cmd[0] if shell else cmd,
                cwd=cwd,
                shell=shell,
                env=env,
                timeout_sec=timeout_sec,
                containment_policy=(
                    ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
                    if _is_windows_host()
                    else ProcessContainmentPolicy.TRUSTED_COOPERATIVE
                ),
            )
        rc = result.returncode
        output = result.stdout
        timed_out = result.timed_out
    except InvocationError as exc:
        rc = INFRASTRUCTURE_ERROR_RETURN_CODE
        timed_out = exc.timed_out
        infrastructure_error = str(exc)
        output = _output_with_marker(
            exc.output,
            f"[nebula-test-infrastructure] {infrastructure_error}",
        )
    duration_ms = int((time.perf_counter() - t0) * 1000)

    return {
        "kind": kind,
        "cmd": cmd,
        "cmd_str": _stringify_cmd(cmd),
        "rc": rc,
        "output": output,
        "duration_ms": duration_ms,
        "timed_out": timed_out,
        "infrastructure_error": infrastructure_error,
    }
