from __future__ import annotations

import http.client
import json
import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from service_probe import fail_with_output, wait_for_listener_bound


def print_completed(result: subprocess.CompletedProcess[str]) -> None:
    print(result.stdout, end="")
    print(result.stderr, end="")


def require_success(result: subprocess.CompletedProcess[str], context: str) -> None:
    if result.returncode != 0:
        print_completed(result)
        raise SystemExit(f"{context} failed: rc={result.returncode}")


def copy_example(repo_root: Path, dest: Path) -> None:
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(repo_root / "examples" / "cli_service_workspace", dest)
    ctl_manifest = dest / "apps" / "ctl" / "nebula.toml"
    if ctl_manifest.exists():
        text = ctl_manifest.read_text(encoding="utf-8")
        text = text.replace(
            'tls = { path = "../../../../official/nebula-tls" }',
            f'tls = {{ path = "{repo_root / "official" / "nebula-tls"}" }}',
        )
        ctl_manifest.write_text(text, encoding="utf-8")
    lock_path = dest / "nebula.lock"
    if lock_path.exists():
        lock_path.unlink()


def nebula(binary: str,
           *args: str | Path,
           env: dict[str, str] | None = None,
           cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, *[str(arg) for arg in args]],
        capture_output=True,
        text=True,
        env=env,
        cwd=cwd,
    )


def fetch_workspace(binary: str, root: Path) -> None:
    require_success(nebula(binary, "fetch", root), "workspace fetch")


def build_service_binary(binary: str, root: Path) -> Path:
    out_dir = root / ".service-out"
    require_success(
        nebula(binary, "build", root / "apps" / "service", "--out-dir", out_dir),
        "service build",
    )
    return out_dir / "main.out"


def start_service(binary: str,
                  root: Path,
                  extra_env: dict[str, str] | None = None) -> tuple[subprocess.Popen[str], list[str], int]:
    service_binary = build_service_binary(binary, root)
    env = {
        **os.environ,
        "NEBULA_BIND_HOST": "127.0.0.1",
        "NEBULA_PORT": "0",
    }
    if extra_env is not None:
        env.update(extra_env)
    proc = subprocess.Popen(
        [str(service_binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    stderr_lines: list[str] = []
    _, port = wait_for_listener_bound(proc, stderr_lines)
    return proc, stderr_lines, port


def terminate_process(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def wait_until_health_ok(proc: subprocess.Popen[str],
                         stderr_lines: list[str],
                         port: int,
                         timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail_with_output(proc, stderr_lines, f"service exited before /healthz became ready: rc={proc.returncode}")
        try:
            status, body, _ = http_request("127.0.0.1", port, "GET", "/healthz")
            if status == 200:
                payload = json.loads(body)
                if payload.get("status") == "ok":
                    return
        except Exception:
            time.sleep(0.1)
    fail_with_output(proc, stderr_lines, "service did not become healthy in time")


def wait_until_ready_status(proc: subprocess.Popen[str],
                            stderr_lines: list[str],
                            port: int,
                            expected_status: int,
                            expected_state: str,
                            timeout: float = 10.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail_with_output(proc, stderr_lines, f"service exited before /readyz became {expected_status}: rc={proc.returncode}")
        try:
            status, body, _ = http_request("127.0.0.1", port, "GET", "/readyz")
            payload = json.loads(body)
            if status == expected_status and payload.get("status") == expected_state:
                return
        except Exception:
            time.sleep(0.1)
    fail_with_output(proc, stderr_lines, f"service did not reach /readyz={expected_status}/{expected_state}")


def http_request(host: str,
                 port: int,
                 method: str,
                 path: str,
                 body: bytes | None = None,
                 headers: dict[str, str] | None = None) -> tuple[int, str, list[tuple[str, str]]]:
    conn = http.client.HTTPConnection(host, port, timeout=5)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        payload = resp.read().decode("utf-8")
        return resp.status, payload, resp.getheaders()
    finally:
        conn.close()


def json_request(host: str,
                 port: int,
                 method: str,
                 path: str,
                 payload: Any | None = None,
                 headers: dict[str, str] | None = None) -> tuple[int, dict[str, Any]]:
    merged_headers = {"Accept": "application/json"}
    if headers:
        merged_headers.update(headers)
    body: bytes | None = None
    if payload is not None:
        merged_headers["Content-Type"] = "application/json; charset=utf-8"
        body = json.dumps(payload).encode("utf-8")
    status, raw_body, _ = http_request(host, port, method, path, body=body, headers=merged_headers)
    return status, json.loads(raw_body)


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
    return nebula(binary, *cmd, env=run_env)
