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
    shutil.copytree(repo_root / "examples" / "release_control_plane_workspace", dest)

    service_manifest = dest / "apps" / "service" / "nebula.toml"
    if service_manifest.exists():
        text = service_manifest.read_text(encoding="utf-8")
        text = text.replace(
            'db_sqlite = { path = "../../../../official/nebula-db-sqlite" }',
            f'db_sqlite = {{ path = "{repo_root / "official" / "nebula-db-sqlite"}" }}',
        )
        text = text.replace(
            'db_postgres = { path = "../../../../official/nebula-db-postgres" }',
            f'db_postgres = {{ path = "{repo_root / "official" / "nebula-db-postgres"}" }}',
        )
        text = text.replace(
            'crypto = { path = "../../../../official/nebula-crypto" }',
            f'crypto = {{ path = "{repo_root / "official" / "nebula-crypto"}" }}',
        )
        text = text.replace(
            'auth_pkg = { path = "../../../../official/nebula-auth" }',
            f'auth_pkg = {{ path = "{repo_root / "official" / "nebula-auth"}" }}',
        )
        text = text.replace(
            'app_config = { path = "../../../../official/nebula-config" }',
            f'app_config = {{ path = "{repo_root / "official" / "nebula-config"}" }}',
        )
        text = text.replace(
            'jobs_pkg = { path = "../../../../official/nebula-jobs" }',
            f'jobs_pkg = {{ path = "{repo_root / "official" / "nebula-jobs"}" }}',
        )
        service_manifest.write_text(text, encoding="utf-8")

    core_manifest = dest / "packages" / "core" / "nebula.toml"
    if core_manifest.exists():
        text = core_manifest.read_text(encoding="utf-8")
        text = text.replace(
            'jobs_pkg = { path = "../../../../official/nebula-jobs" }',
            f'jobs_pkg = {{ path = "{repo_root / "official" / "nebula-jobs"}" }}',
        )
        text = text.replace(
            'app_config = { path = "../../../../official/nebula-config" }',
            f'app_config = {{ path = "{repo_root / "official" / "nebula-config"}" }}',
        )
        core_manifest.write_text(text, encoding="utf-8")

    ctl_manifest = dest / "apps" / "ctl" / "nebula.toml"
    if ctl_manifest.exists():
        text = ctl_manifest.read_text(encoding="utf-8")
        text = text.replace(
            'tls = { path = "../../../../official/nebula-tls" }',
            f'tls = {{ path = "{repo_root / "official" / "nebula-tls"}" }}',
        )
        text = text.replace(
            'crypto = { path = "../../../../official/nebula-crypto" }',
            f'crypto = {{ path = "{repo_root / "official" / "nebula-crypto"}" }}',
        )
        text = text.replace(
            'auth_pkg = { path = "../../../../official/nebula-auth" }',
            f'auth_pkg = {{ path = "{repo_root / "official" / "nebula-auth"}" }}',
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


def fetch_workspace(binary: str, root: Path, env: dict[str, str] | None = None) -> None:
    require_success(nebula(binary, "fetch", root, env=env), "workspace fetch")


def build_service_binary(binary: str, root: Path, env: dict[str, str] | None = None) -> Path:
    out_dir = root / ".service-out"
    require_success(
        nebula(binary, "build", root / "apps" / "service", "--out-dir", out_dir, env=env),
        "service build",
    )
    return out_dir / "main.out"


def start_service(binary: str,
                  root: Path,
                  extra_env: dict[str, str] | None = None,
                  build_env: dict[str, str] | None = None) -> tuple[subprocess.Popen[str], list[str], int]:
    service_binary = build_service_binary(binary, root, env=build_env)
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
