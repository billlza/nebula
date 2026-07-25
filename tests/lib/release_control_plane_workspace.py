from __future__ import annotations

import hashlib
import http.client
import json
import math
import os
import shutil
import stat
import subprocess
import sys
import time
import tomllib
from pathlib import Path
from typing import NoReturn

from process_containment import (
    DEFAULT_MAX_CAPTURED_OUTPUT_BYTES,
    InvocationError,
    LongLivedProcess,
    ProcessContainmentPolicy,
    run_contained_command,
)
from service_probe import (
    ServiceProbeCleanupError,
    ServiceProbeOutput as ServiceOutput,
    ServiceProbeSetupError,
    await_listener_bound,
    finish_process_output,
    start_process as start_captured_process,
)


COMMAND_TIMEOUT_SECONDS = 180.0
CTL_COMMAND_TIMEOUT_SECONDS = 120.0
SERVICE_MAX_CAPTURED_OUTPUT_BYTES = 1024 * 1024
MAX_HTTP_RESPONSE_BYTES = 4 * 1024 * 1024
MAX_HEALTH_RESPONSE_BYTES = 64 * 1024
HTTP_READ_CHUNK_BYTES = 64 * 1024
MAX_ARTIFACT_METADATA_BYTES = 16 * 1024
ARTIFACT_METADATA_FIELDS = (
    "version",
    "build_inputs_sha256",
    "mode",
    "profile",
    "artifact_kind",
    "compiler_schema_version",
    "cache_schema_version",
    "strict_region",
    "warnings_as_errors",
    "no_std",
    "runtime_profile",
    "target",
    "panic_policy",
    "artifact_size",
    "artifact_sha256",
)


class WorkspaceCommandTimeout(RuntimeError):
    pass


class WorkspaceCommandExecutionError(RuntimeError):
    pass


class ServiceTerminationError(RuntimeError):
    def __init__(self, message: str, *, stdout: str = "", stderr: str = ""):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr


class ServiceStartupError(RuntimeError):
    pass


class HttpResponseLimitError(RuntimeError):
    pass


ServiceProcess = LongLivedProcess


def print_completed(result: subprocess.CompletedProcess[str]) -> None:
    print(result.stdout, end="")
    print(result.stderr, end="")


def require_success(result: subprocess.CompletedProcess[str], context: str) -> None:
    if result.returncode != 0:
        print_completed(result)
        raise SystemExit(f"{context} failed: rc={result.returncode}")


def _rewrite_path_dependencies(
    manifest_path: Path,
    replacements: dict[str, tuple[str, Path]],
) -> None:
    manifest_text = manifest_path.read_text(encoding="utf-8")
    expected_paths: dict[str, str] = {}
    for dependency, (relative_path, absolute_path) in replacements.items():
        canonical_line = f'{dependency} = {{ path = "{relative_path}" }}'
        occurrence_count = manifest_text.count(canonical_line)
        if occurrence_count != 1:
            raise WorkspaceCommandExecutionError(
                f"{manifest_path} must contain exactly one canonical {dependency!r} "
                f"dependency; found {occurrence_count}"
            )
        resolved_path = absolute_path.resolve(strict=True)
        encoded_path = json.dumps(str(resolved_path))
        replacement = f"{dependency} = {{ path = {encoded_path} }}"
        manifest_text = manifest_text.replace(canonical_line, replacement, 1)
        expected_paths[dependency] = str(resolved_path)

    parsed_manifest = tomllib.loads(manifest_text)
    parsed_dependencies = parsed_manifest.get("dependencies")
    if not isinstance(parsed_dependencies, dict):
        raise WorkspaceCommandExecutionError(
            f"{manifest_path} does not contain a dependency table"
        )
    for dependency, expected_path in expected_paths.items():
        entry = parsed_dependencies.get(dependency)
        if not isinstance(entry, dict) or entry.get("path") != expected_path:
            raise WorkspaceCommandExecutionError(
                f"{manifest_path} dependency {dependency!r} did not round-trip"
            )
    manifest_path.write_text(manifest_text, encoding="utf-8")


def copy_example(repo_root: Path, dest: Path) -> None:
    if dest.is_symlink():
        dest.unlink()
    elif dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(repo_root / "examples" / "release_control_plane_workspace", dest)

    official_root = repo_root / "official"
    _rewrite_path_dependencies(
        dest / "apps" / "service" / "nebula.toml",
        {
            "db_sqlite": (
                "../../../../official/nebula-db-sqlite",
                official_root / "nebula-db-sqlite",
            ),
            "db_postgres": (
                "../../../../official/nebula-db-postgres",
                official_root / "nebula-db-postgres",
            ),
            "crypto": (
                "../../../../official/nebula-crypto",
                official_root / "nebula-crypto",
            ),
            "auth_pkg": (
                "../../../../official/nebula-auth",
                official_root / "nebula-auth",
            ),
            "app_config": (
                "../../../../official/nebula-config",
                official_root / "nebula-config",
            ),
            "jobs_pkg": (
                "../../../../official/nebula-jobs",
                official_root / "nebula-jobs",
            ),
        },
    )
    _rewrite_path_dependencies(
        dest / "packages" / "core" / "nebula.toml",
        {
            "jobs_pkg": (
                "../../../../official/nebula-jobs",
                official_root / "nebula-jobs",
            ),
            "app_config": (
                "../../../../official/nebula-config",
                official_root / "nebula-config",
            ),
        },
    )
    _rewrite_path_dependencies(
        dest / "apps" / "ctl" / "nebula.toml",
        {
            "tls": (
                "../../../../official/nebula-tls",
                official_root / "nebula-tls",
            ),
            "crypto": (
                "../../../../official/nebula-crypto",
                official_root / "nebula-crypto",
            ),
            "auth_pkg": (
                "../../../../official/nebula-auth",
                official_root / "nebula-auth",
            ),
        },
    )

    lock_path = dest / "nebula.lock"
    if lock_path.exists():
        lock_path.unlink()


def nebula(binary: str,
           *args: str | Path,
           env: dict[str, str] | None = None,
           cwd: Path | None = None,
           timeout_sec: float = COMMAND_TIMEOUT_SECONDS) -> subprocess.CompletedProcess[str]:
    command = [binary, *[str(arg) for arg in args]]
    try:
        result = run_contained_command(
            command,
            cwd=cwd,
            shell=False,
            env=os.environ.copy() if env is None else env,
            timeout_sec=timeout_sec,
            containment_policy=(
                ProcessContainmentPolicy.OS_ENFORCED_RECURSIVE
                if os.name == "nt"
                else ProcessContainmentPolicy.TRUSTED_COOPERATIVE
            ),
            combine_stderr=False,
            max_captured_bytes=DEFAULT_MAX_CAPTURED_OUTPUT_BYTES,
        )
    except InvocationError as exc:
        if exc.output:
            print(exc.output, end="" if exc.output.endswith("\n") else "\n", file=sys.stderr)
        raise WorkspaceCommandExecutionError(
            f"Nebula workspace command infrastructure failure: {exc}"
        ) from exc
    if result.timed_out:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise WorkspaceCommandTimeout(
            f"Nebula workspace command timed out after {timeout_sec:g}s"
        )
    return subprocess.CompletedProcess(
        command,
        result.returncode,
        result.stdout,
        result.stderr,
    )


def fetch_workspace(binary: str, root: Path, env: dict[str, str] | None = None) -> None:
    require_success(nebula(binary, "fetch", root, env=env), "workspace fetch")


def _require_prebuilt_executable(executable: Path, context: str) -> Path:
    metadata = executable.parent / f"{executable.name}.nebmeta"
    for path, label in ((executable, "executable"), (metadata, "metadata")):
        try:
            file_stat = path.lstat()
        except OSError as exc:
            raise WorkspaceCommandExecutionError(
                f"{context} {label} is unavailable: {path}: {exc}"
            ) from exc
        if not stat.S_ISREG(file_stat.st_mode):
            raise WorkspaceCommandExecutionError(
                f"{context} {label} is not a regular file: {path}"
            )
        if file_stat.st_size == 0:
            raise WorkspaceCommandExecutionError(
                f"{context} {label} is empty: {path}"
            )
    if not os.access(executable, os.X_OK):
        raise WorkspaceCommandExecutionError(
            f"{context} executable is not executable: {executable}"
        )
    try:
        metadata_bytes = metadata.read_bytes()
    except OSError as exc:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata could not be read: {metadata}: {exc}"
        ) from exc
    if len(metadata_bytes) > MAX_ARTIFACT_METADATA_BYTES:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata exceeds {MAX_ARTIFACT_METADATA_BYTES} bytes: {metadata}"
        )
    if not metadata_bytes.endswith(b"\n") or b"\r" in metadata_bytes:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata does not use canonical LF-terminated records: {metadata}"
        )
    try:
        metadata_lines = metadata_bytes[:-1].decode("ascii").split("\n")
    except UnicodeDecodeError as exc:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata is not canonical ASCII: {metadata}"
        ) from exc
    parsed_fields: dict[str, str] = {}
    observed_field_order: list[str] = []
    for line in metadata_lines:
        if "=" not in line:
            raise WorkspaceCommandExecutionError(
                f"{context} metadata contains a malformed field: {metadata}"
            )
        key, value = line.split("=", 1)
        if key in parsed_fields:
            raise WorkspaceCommandExecutionError(
                f"{context} metadata repeats field {key!r}: {metadata}"
            )
        parsed_fields[key] = value
        observed_field_order.append(key)
    if tuple(observed_field_order) != ARTIFACT_METADATA_FIELDS:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata is not canonical v6 field order: {metadata}"
        )
    if parsed_fields["version"] != "6":
        raise WorkspaceCommandExecutionError(
            f"{context} metadata version is not 6: {metadata}"
        )
    if parsed_fields["artifact_kind"] != "executable":
        raise WorkspaceCommandExecutionError(
            f"{context} metadata artifact_kind is not executable: {metadata}"
        )
    declared_size = parsed_fields["artifact_size"]
    if (
        not declared_size.isascii()
        or not declared_size.isdecimal()
        or declared_size.startswith("0")
        or len(declared_size) > 20
    ):
        raise WorkspaceCommandExecutionError(
            f"{context} metadata artifact_size is not a canonical positive integer: {metadata}"
        )
    if int(declared_size) != executable.lstat().st_size:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata artifact_size does not match the executable: {metadata}"
        )
    declared_digest = parsed_fields["artifact_sha256"]
    if (
        len(declared_digest) != 64
        or declared_digest.lower() != declared_digest
        or any(character not in "0123456789abcdef" for character in declared_digest)
    ):
        raise WorkspaceCommandExecutionError(
            f"{context} metadata artifact_sha256 is not canonical: {metadata}"
        )
    digest = hashlib.sha256()
    try:
        with executable.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise WorkspaceCommandExecutionError(
            f"{context} executable could not be hashed: {executable}: {exc}"
        ) from exc
    if digest.hexdigest() != declared_digest:
        raise WorkspaceCommandExecutionError(
            f"{context} metadata artifact_sha256 does not match the executable: {metadata}"
        )
    return executable


def build_service_binary(binary: str, root: Path, env: dict[str, str] | None = None) -> Path:
    out_dir = root / ".service-out"
    require_success(
        nebula(binary, "build", root / "apps" / "service", "--out-dir", out_dir, env=env),
        "service build",
    )
    return _require_prebuilt_executable(out_dir / "main.out", "service build")


def build_ctl_binary(binary: str,
                     root: Path,
                     out_dir: Path,
                     env: dict[str, str] | None = None) -> Path:
    require_success(
        nebula(binary, "build", root / "apps" / "ctl", "--out-dir", out_dir, env=env),
        "ctl build",
    )
    return _require_prebuilt_executable(out_dir / "main.out", "ctl build")


def start_prebuilt_service(
    service_binary: Path,
    extra_env: dict[str, str] | None = None,
) -> tuple[ServiceProcess, ServiceOutput, int]:
    verified_binary = _require_prebuilt_executable(
        service_binary,
        "prebuilt service",
    )
    return _start_service_binary(verified_binary, extra_env)


def start_service(binary: str,
                  root: Path,
                  extra_env: dict[str, str] | None = None,
                  build_env: dict[str, str] | None = None) -> tuple[ServiceProcess, ServiceOutput, int]:
    service_binary = build_service_binary(binary, root, env=build_env)
    return _start_service_binary(service_binary, extra_env)


def _start_service_binary(
    service_binary: Path,
    extra_env: dict[str, str] | None,
) -> tuple[ServiceProcess, ServiceOutput, int]:
    startup_timeout = 10.0
    env = {
        **os.environ,
        "NEBULA_BIND_HOST": "127.0.0.1",
        "NEBULA_PORT": "0",
    }
    if extra_env is not None:
        env.update(extra_env)
    try:
        proc, output = start_captured_process(
            [str(service_binary)],
            "listener_bound",
            env=env,
            max_captured_bytes=SERVICE_MAX_CAPTURED_OUTPUT_BYTES,
            thread_name_prefix="nebula-service",
        )
    except ServiceProbeSetupError as exc:
        raise WorkspaceCommandExecutionError(str(exc)) from exc

    try:
        _, port = await_listener_bound(proc, output, startup_timeout)
        return proc, output, port
    except BaseException as exc:
        cleanup_error = _terminate_process_impl(proc, output)
        _print_service_output(output)
        if cleanup_error is not None:
            cleanup_context = f"service startup cleanup failed: {cleanup_error}"
            if not isinstance(exc, Exception):
                exc.add_note(cleanup_context)
                raise
        if not isinstance(exc, Exception):
            raise
        message = f"service startup failed: {type(exc).__name__}: {exc}"
        if cleanup_error is not None:
            message += f"; cleanup failure: {cleanup_error}"
        raise ServiceStartupError(message) from exc


def _terminate_process_impl(
    proc: ServiceProcess,
    output: ServiceOutput,
) -> ServiceTerminationError | None:
    try:
        finish_process_output(proc, output)
    except ServiceProbeCleanupError as exc:
        return ServiceTerminationError(
            str(exc),
            stdout=exc.stdout,
            stderr=exc.stderr,
        )
    return None


def terminate_process(proc: ServiceProcess, output: ServiceOutput) -> None:
    cleanup_error = _terminate_process_impl(proc, output)
    if cleanup_error is None:
        return
    active_exception = sys.exception()
    if active_exception is not None:
        active_exception.add_note(f"service cleanup failed: {cleanup_error}")
        _print_service_capture(cleanup_error.stdout, cleanup_error.stderr)
        return
    raise cleanup_error


def _print_service_capture(stdout: str, stderr: str) -> None:
    print("[nebula-service-stdout]")
    print(stdout, end="" if stdout.endswith("\n") or not stdout else "\n")
    print("[nebula-service-stderr]", file=sys.stderr)
    print(
        stderr,
        end="" if stderr.endswith("\n") or not stderr else "\n",
        file=sys.stderr,
    )


def _print_service_output(output: ServiceOutput) -> None:
    stdout, stderr = output.snapshots()
    _print_service_capture(stdout, stderr)


def _fail_with_service_output(proc: ServiceProcess,
                              output: ServiceOutput,
                              message: str) -> NoReturn:
    termination_error = _terminate_process_impl(proc, output)
    _print_service_output(output)
    if termination_error is not None:
        message += f"; service cleanup failed: {termination_error}"
    raise SystemExit(message)


def wait_until_health_ok(proc: ServiceProcess,
                         output: ServiceOutput,
                         port: int,
                         timeout: float = 20.0) -> None:
    if type(port) is not int or port < 1 or port > 65535:
        raise ValueError("health-check port must be an integer in the range 1..65535")
    if (
        isinstance(timeout, bool)
        or not isinstance(timeout, (int, float))
        or not math.isfinite(timeout)
        or timeout <= 0
    ):
        raise ValueError("health-check timeout must be a finite positive number")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            _fail_with_service_output(
                proc,
                output,
                f"service exited before /healthz became ready: rc={proc.returncode}",
            )
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            status, body, _ = http_request(
                "127.0.0.1",
                port,
                "GET",
                "/healthz",
                timeout=min(5.0, remaining),
                max_response_bytes=MAX_HEALTH_RESPONSE_BYTES,
            )
        except HttpResponseLimitError as exc:
            _fail_with_service_output(
                proc,
                output,
                f"service /healthz response exceeded its size limit: {exc}",
            )
        except (OSError, http.client.HTTPException):
            time.sleep(0.1)
            continue
        if status == 200:
            try:
                payload = json.loads(body)
            except json.JSONDecodeError as exc:
                _fail_with_service_output(
                    proc,
                    output,
                    f"service /healthz returned invalid JSON: {exc}",
                )
            if not isinstance(payload, dict):
                _fail_with_service_output(
                    proc,
                    output,
                    "service /healthz returned a non-object JSON payload",
                )
            if payload.get("status") == "ok":
                return
        time.sleep(0.1)
    _fail_with_service_output(proc, output, "service did not become healthy in time")


def http_request(host: str,
                 port: int,
                 method: str,
                 path: str,
                 body: bytes | None = None,
                 headers: dict[str, str] | None = None,
                 *,
                 timeout: float = 5.0,
                 max_response_bytes: int = MAX_HTTP_RESPONSE_BYTES) -> tuple[int, str, list[tuple[str, str]]]:
    if (
        isinstance(timeout, bool)
        or not isinstance(timeout, (int, float))
        or not math.isfinite(timeout)
        or timeout <= 0
    ):
        raise ValueError("HTTP timeout must be a finite positive number")
    if type(max_response_bytes) is not int or max_response_bytes < 1:
        raise ValueError("HTTP response byte limit must be a positive integer")
    deadline = time.monotonic() + timeout
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                f"HTTP request exceeded the {timeout:g}s total deadline"
            )
        if conn.sock is not None:
            conn.sock.settimeout(remaining)
        resp = conn.getresponse()
        payload = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"HTTP response exceeded the {timeout:g}s total deadline"
                )
            if conn.sock is not None:
                conn.sock.settimeout(remaining)
            chunk = resp.read(
                min(
                    HTTP_READ_CHUNK_BYTES,
                    max_response_bytes + 1 - len(payload),
                )
            )
            if not chunk:
                break
            payload.extend(chunk)
            if len(payload) > max_response_bytes:
                raise HttpResponseLimitError(
                    f"limit={max_response_bytes} bytes"
                )
        decoded_payload = payload.decode("utf-8")
        return resp.status, decoded_payload, resp.getheaders()
    finally:
        conn.close()


def run_ctl(binary: str,
            root: Path,
            *args: str,
            env: dict[str, str] | None = None,
            out_dir: Path | None = None,
            no_build: bool = False) -> subprocess.CompletedProcess[str]:
    run_env = dict(os.environ)
    if env is not None:
        run_env.update(env)
    cmd: list[str | Path] = ["run", root / "apps" / "ctl", "--run-gate", "none"]
    if out_dir is not None:
        cmd.extend(["--out-dir", out_dir])
    if no_build:
        cmd.append("--no-build")
    cmd.extend(["--", *args])
    return nebula(
        binary,
        *cmd,
        env=run_env,
        timeout_sec=(CTL_COMMAND_TIMEOUT_SECONDS if no_build else COMMAND_TIMEOUT_SECONDS),
    )
