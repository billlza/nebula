#!/usr/bin/env python3
from __future__ import annotations

import argparse
import http.client
import json
import os
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Verify the release-control-plane publishable internal-app standard")
    p.add_argument("--binary", required=True, help="path to the nebula compiler binary")
    p.add_argument("--work-dir", default="", help="workspace scratch directory; defaults to a temp dir")
    p.add_argument("--keep-work-dir", action="store_true", help="do not delete the scratch directory")
    return p.parse_args()


def repo_root_from(script_path: Path) -> Path:
    return script_path.resolve().parents[1]


def run_checked(cmd: list[str], *, env: dict[str, str] | None = None, cwd: Path | None = None, context: str) -> subprocess.CompletedProcess[str]:
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cwd)
    if proc.returncode != 0:
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        raise SystemExit(f"{context} failed: rc={proc.returncode}")
    return proc


def copy_example(repo_root: Path, dest: Path) -> None:
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(
        repo_root / "examples" / "release_control_plane_workspace",
        dest,
        ignore=shutil.ignore_patterns(".service-out", ".ctl-out", "state", "tmp", "nebula.lock"),
    )
    rewrites = {
        'db_sqlite = { path = "../../../../official/nebula-db-sqlite" }':
            f'db_sqlite = {{ path = "{repo_root / "official" / "nebula-db-sqlite"}" }}',
        'db_postgres = { path = "../../../../official/nebula-db-postgres" }':
            f'db_postgres = {{ path = "{repo_root / "official" / "nebula-db-postgres"}" }}',
        'crypto = { path = "../../../../official/nebula-crypto" }':
            f'crypto = {{ path = "{repo_root / "official" / "nebula-crypto"}" }}',
        'auth_pkg = { path = "../../../../official/nebula-auth" }':
            f'auth_pkg = {{ path = "{repo_root / "official" / "nebula-auth"}" }}',
        'app_config = { path = "../../../../official/nebula-config" }':
            f'app_config = {{ path = "{repo_root / "official" / "nebula-config"}" }}',
        'jobs_pkg = { path = "../../../../official/nebula-jobs" }':
            f'jobs_pkg = {{ path = "{repo_root / "official" / "nebula-jobs"}" }}',
        'tls = { path = "../../../../official/nebula-tls" }':
            f'tls = {{ path = "{repo_root / "official" / "nebula-tls"}" }}',
    }
    for manifest in [
        dest / "apps" / "service" / "nebula.toml",
        dest / "apps" / "ctl" / "nebula.toml",
        dest / "packages" / "core" / "nebula.toml",
    ]:
        if not manifest.exists():
            continue
        text = manifest.read_text(encoding="utf-8")
        for old, new in rewrites.items():
            text = text.replace(old, new)
        manifest.write_text(text, encoding="utf-8")


def collect_observe_events(text: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for raw_line in text.splitlines():
        json_start = raw_line.find("{")
        if json_start < 0:
            continue
        try:
            payload = json.loads(raw_line[json_start:])
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            events.append(payload)
    return events


def finish_process(proc: subprocess.Popen[str], stderr_chunks: list[str]) -> tuple[str, str]:
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


def fail_with_process(proc: subprocess.Popen[str], stderr_chunks: list[str], message: str) -> None:
    stdout, stderr = finish_process(proc, stderr_chunks)
    print(stdout, end="")
    print(stderr, end="", file=sys.stderr)
    raise SystemExit(message)


def wait_for_listener_bound(proc: subprocess.Popen[str], stderr_chunks: list[str], timeout: float = 15.0) -> int:
    if proc.stderr is None:
        raise RuntimeError("service stderr must be captured")
    selector = selectors.DefaultSelector()
    selector.register(proc.stderr, selectors.EVENT_READ)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                fail_with_process(proc, stderr_chunks, f"service exited before listener_bound: rc={proc.returncode}")
            for key, _ in selector.select(timeout=0.05):
                line = key.fileobj.readline()
                if not line:
                    continue
                stderr_chunks.append(line)
                for payload in collect_observe_events(line):
                    if payload.get("event") == "listener_bound":
                        port = payload.get("port")
                        if not isinstance(port, int) or port <= 0:
                            fail_with_process(proc, stderr_chunks, f"invalid listener_bound payload: {payload!r}")
                        return port
    finally:
        selector.close()
    fail_with_process(proc, stderr_chunks, "timed out waiting for listener_bound")
    raise AssertionError("unreachable")


def http_request(port: int, method: str, path: str, *, body: bytes | None = None, headers: dict[str, str] | None = None) -> tuple[int, str]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        return resp.status, resp.read().decode("utf-8")
    finally:
        conn.close()


def wait_until_health_ok(proc: subprocess.Popen[str], stderr_chunks: list[str], port: int, timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail_with_process(proc, stderr_chunks, f"service exited before health check: rc={proc.returncode}")
        try:
            status, body = http_request(port, "GET", "/healthz")
            if status == 200 and json.loads(body).get("status") == "ok":
                return
        except Exception:
            time.sleep(0.1)
    fail_with_process(proc, stderr_chunks, "service did not become healthy")


def wait_until_ready_ok(proc: subprocess.Popen[str], stderr_chunks: list[str], port: int, timeout: float = 20.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail_with_process(proc, stderr_chunks, f"service exited before readiness check: rc={proc.returncode}")
        try:
            status, body = http_request(port, "GET", "/readyz")
            if status == 200 and json.loads(body).get("status") == "ready":
                return
        except Exception:
            time.sleep(0.1)
    fail_with_process(proc, stderr_chunks, "service did not become ready")


def start_service(binary: Path, root: Path, env: dict[str, str]) -> tuple[subprocess.Popen[str], list[str], int]:
    out_dir = root / ".service-out"
    run_checked(
        [str(binary), "build", str(root / "apps" / "service"), "--out-dir", str(out_dir)],
        context="service build",
    )
    service_binary = out_dir / "main.out"
    service_env = {
        **os.environ,
        "NEBULA_BIND_HOST": "127.0.0.1",
        "NEBULA_PORT": "0",
        **env,
    }
    proc = subprocess.Popen(
        [str(service_binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=service_env,
    )
    stderr_chunks: list[str] = []
    port = wait_for_listener_bound(proc, stderr_chunks)
    wait_until_health_ok(proc, stderr_chunks, port)
    wait_until_ready_ok(proc, stderr_chunks, port)
    return proc, stderr_chunks, port


def ctl(binary: Path, root: Path, *args: str, out_dir: Path, no_build: bool = True) -> subprocess.CompletedProcess[str]:
    cmd = [
        str(binary),
        "run",
        str(root / "apps" / "ctl"),
        "--run-gate",
        "none",
        "--out-dir",
        str(out_dir),
    ]
    if no_build:
        cmd.append("--no-build")
    cmd.extend(["--", *args])
    return run_checked(cmd, context="ctl " + " ".join(args[:3]))


def ctl_json(binary: Path, root: Path, *args: str, out_dir: Path, no_build: bool = True) -> dict[str, Any]:
    proc = ctl(binary, root, *args, "--json", out_dir=out_dir, no_build=no_build)
    return json.loads(proc.stdout)


def require_static_asset_contract(root: Path) -> None:
    checks = {
        root / "deploy" / "README.md": [
            "operations/",
            "workflow event submit",
            "worker claim/heartbeat/complete",
        ],
        root / "deploy" / "operations" / "README.md": [
            "## Publishable Internal-App Verification",
            "scripts/verify_release_control_plane_standard.py",
            "SQLite Backup",
            "Postgres Backup",
            "Upgrade And Migration",
            "Drain, Health, And Logs",
            "Common Failure Triage",
            "/var/log/nebula/release-control-plane.observe.ndjson",
        ],
        root / "deploy" / "systemd" / "release-control-plane.service": [
            "EnvironmentFile=/etc/nebula/release-control-plane.env",
            "ExecStart=/srv/release-control-plane/.service-out/main.out",
            "StandardError=append:/var/log/nebula/release-control-plane.observe.ndjson",
            "NoNewPrivileges=true",
        ],
        root / "deploy" / "systemd" / "release-control-plane.env.example": [
            "APP_DATA_BACKEND=sqlite",
            "APP_AUTH_REQUIRED=1",
            "NEBULA_DRAIN_FILE=/run/release-control-plane/drain",
            "APP_READ_TOKEN_FILE=/run/release-control-plane-secrets/read.token",
            "APP_WORKER_TOKEN_FILE=/run/release-control-plane-secrets/worker.token",
            "# APP_POSTGRES_PREVIEW=1",
        ],
        root / "deploy" / "logrotate" / "release-control-plane.observe": [
            "/var/log/nebula/release-control-plane.observe.ndjson",
            "copytruncate",
            "rotate 8",
        ],
        root / "deploy" / "secrets" / "README.md": [
            "read.token",
            "write.token",
            "admin.token",
            "worker.token",
            "startup-only",
            "Do not commit real values.",
        ],
        root / "deploy" / "container" / "README.md": [
            "deploy/operations/README.md",
            "APP_DATA_BACKEND=sqlite",
            "APP_POSTGRES_DSN_FILE",
        ],
        root / "deploy" / "k8s" / "README.md": [
            "backup-before-upgrade flow",
            "APP_POSTGRES_DSN_FILE",
            "worker lane",
        ],
    }
    for path, fragments in checks.items():
        text = path.read_text(encoding="utf-8")
        for fragment in fragments:
            if fragment not in text:
                raise SystemExit(f"{path.relative_to(root)} missing required fragment: {fragment!r}")


def verify_publishable_standard(binary: Path, root: Path) -> None:
    require_static_asset_contract(root)
    run_checked([str(binary), "fetch", str(root)], context="workspace fetch")

    state_dir = root / "state"
    ctl_out = root / ".ctl-out"
    admin_token = "admin-standard-token"
    writer_token = "writer-standard-token"
    reader_token = "reader-standard-token"
    worker_token = "worker-standard-token"
    service_env = {
        "APP_STATE_DIR": str(state_dir),
        "APP_EXTERNAL_BROKER_PREVIEW": "1",
        "APP_EXTERNAL_BROKER_URL": "http://127.0.0.1:1",
        "APP_AUTH_REQUIRED": "1",
        "APP_ADMIN_TOKEN": admin_token,
        "APP_WRITE_TOKEN": writer_token,
        "APP_READ_TOKEN": reader_token,
        "APP_WORKER_TOKEN": worker_token,
    }

    proc, stderr_chunks, port = start_service(binary, root, service_env)
    try:
        base_url = f"http://127.0.0.1:{port}"
        ctl(binary, root, "control-plane-validate", "--url", base_url, out_dir=ctl_out, no_build=False)

        status = ctl(binary, root, "status", "--url", base_url, "--token", reader_token, out_dir=ctl_out)
        if "status=ok" not in status.stdout:
            raise SystemExit(f"ctl status did not report ok: {status.stdout!r}")

        whoami = ctl_json(binary, root, "auth", "whoami", "--url", base_url, "--token", admin_token, out_dir=ctl_out)
        if whoami.get("actor") != "admin" or whoami.get("role") != "admin":
            raise SystemExit(f"unexpected whoami payload: {whoami!r}")

        config_put = ctl_json(
            binary, root,
            "config", "put",
            "--app", "control-plane",
            "--channel", "prod",
            "--key", "feature.flag",
            "--bool", "true",
            "--description", "publishable internal app standard",
            "--url", base_url,
            "--token", writer_token,
            out_dir=ctl_out,
        )
        config_revision = config_put.get("entry", {}).get("revision_id")
        if config_revision != 1:
            raise SystemExit(f"unexpected config revision: {config_put!r}")

        config_get = ctl_json(
            binary, root,
            "config", "get",
            "--app", "control-plane",
            "--channel", "prod",
            "--key", "feature.flag",
            "--url", base_url,
            "--token", reader_token,
            out_dir=ctl_out,
        )
        config_value = config_get.get("entry", {}).get("value", {})
        if config_value.get("kind") != "bool" or config_value.get("bool_value") is not True:
            raise SystemExit(f"unexpected config get payload: {config_get!r}")

        release_put = ctl_json(
            binary, root,
            "release", "put",
            "--app", "control-plane",
            "--channel", "prod",
            "--version", "2026.04.24",
            "--config-revision", str(config_revision),
            "--description", "publishable desired state",
            "--url", base_url,
            "--token", writer_token,
            out_dir=ctl_out,
        )
        if release_put.get("entry", {}).get("revision_id") != 1:
            raise SystemExit(f"unexpected release put payload: {release_put!r}")

        release_get = ctl_json(
            binary, root,
            "release", "get",
            "--app", "control-plane",
            "--channel", "prod",
            "--url", base_url,
            "--token", reader_token,
            out_dir=ctl_out,
        )
        if release_get.get("entry", {}).get("version") != "2026.04.24":
            raise SystemExit(f"unexpected release get payload: {release_get!r}")

        approval = ctl_json(
            binary, root,
            "approval", "decide",
            "--app", "control-plane",
            "--channel", "prod",
            "--if-match", "1",
            "--decision", "approved",
            "--reason", "publishable standard gate",
            "--url", base_url,
            "--token", admin_token,
            out_dir=ctl_out,
        )
        if approval.get("decision", {}).get("decision") != "approved":
            raise SystemExit(f"unexpected approval payload: {approval!r}")

        apply_run = ctl_json(
            binary, root,
            "release", "apply",
            "--app", "control-plane",
            "--channel", "prod",
            "--if-match", "1",
            "--url", base_url,
            "--token", writer_token,
            out_dir=ctl_out,
        )
        run_id = apply_run.get("run", {}).get("run_id")
        if not isinstance(run_id, int) or run_id <= 0:
            raise SystemExit(f"release apply did not create a workflow run: {apply_run!r}")

        broker_claim = ctl_json(
            binary, root,
            "broker", "outbox", "claim",
            "--worker-id", "standard-broker",
            "--topic", "workflow.run.created",
            "--now-unix-ms", "1000",
            "--url", base_url,
            "--token", worker_token,
            out_dir=ctl_out,
        )
        message = broker_claim.get("message", {})
        if broker_claim.get("outcome") != "claimed" or message.get("topic") != "workflow.run.created":
            raise SystemExit(f"broker outbox did not claim workflow.run.created: {broker_claim!r}")

        broker_done = ctl_json(
            binary, root,
            "broker", "outbox", "complete",
            "--worker-id", "standard-broker",
            "--message-id", str(message.get("message_id")),
            "--lease-id", str(message.get("meta", {}).get("lease_id")),
            "--status", "published",
            "--message", "standard published",
            "--url", base_url,
            "--token", worker_token,
            out_dir=ctl_out,
        )
        if broker_done.get("message", {}).get("status") != "published":
            raise SystemExit(f"broker outbox did not publish: {broker_done!r}")

        worker_claim = ctl_json(
            binary, root,
            "worker", "claim",
            "--worker-id", "standard-worker",
            "--task-kind", "apply_release",
            "--url", base_url,
            "--token", worker_token,
            out_dir=ctl_out,
        )
        lease = worker_claim.get("lease", {})
        if worker_claim.get("outcome") != "claimed" or lease.get("task_kind") != "apply_release":
            raise SystemExit(f"worker did not claim apply_release lease: {worker_claim!r}")

        worker_heartbeat = ctl_json(
            binary, root,
            "worker", "heartbeat",
            "--worker-id", "standard-worker",
            "--lease-id", str(lease.get("lease_id")),
            "--url", base_url,
            "--token", worker_token,
            out_dir=ctl_out,
        )
        if worker_heartbeat.get("lease", {}).get("lease_id") != lease.get("lease_id"):
            raise SystemExit(f"worker heartbeat did not preserve the claimed lease: {worker_heartbeat!r}")

        worker_done = ctl_json(
            binary, root,
            "worker", "complete",
            "--worker-id", "standard-worker",
            "--lease-id", str(lease.get("lease_id")),
            "--status", "succeeded",
            "--message", "standard apply complete",
            "--url", base_url,
            "--token", worker_token,
            out_dir=ctl_out,
        )
        run_status = worker_done.get("run", {}).get("meta", {}).get("status")
        if run_status not in {"completed", "applied"}:
            raise SystemExit(f"worker completion left unexpected run status: {worker_done!r}")

        workflow_view = ctl_json(
            binary, root,
            "workflow", "run",
            "--app", "control-plane",
            "--channel", "prod",
            "--run-id", str(run_id),
            "--url", base_url,
            "--token", reader_token,
            out_dir=ctl_out,
        )
        if workflow_view.get("run", {}).get("meta", {}).get("status") not in {"completed", "applied"}:
            raise SystemExit(f"workflow run did not complete: {workflow_view!r}")

        audit = ctl_json(
            binary, root,
            "audit", "list",
            "--app", "control-plane",
            "--channel", "prod",
            "--url", base_url,
            "--token", reader_token,
            out_dir=ctl_out,
        )
        event_kinds = {event.get("event_kind") for event in audit.get("events", [])}
        for expected in [
            "config_upserted",
            "release_upserted",
            "approval_decided",
            "release_apply_requested",
            "release_apply_completed",
        ]:
            if expected not in event_kinds:
                raise SystemExit(f"missing audit event {expected!r}: {sorted(event_kinds)!r}")
    finally:
        finish_process(proc, stderr_chunks)


def main() -> int:
    args = parse_args()
    repo_root = repo_root_from(Path(__file__))
    binary = Path(args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"nebula binary not found: {binary}")

    temp_root: tempfile.TemporaryDirectory[str] | None = None
    if args.work_dir:
        work_root = Path(args.work_dir).resolve()
        shutil.rmtree(work_root, ignore_errors=True)
        work_root.mkdir(parents=True, exist_ok=True)
    else:
        temp_root = tempfile.TemporaryDirectory(prefix="nebula-rcp-standard.")
        work_root = Path(temp_root.name)

    workspace = work_root / "release_control_plane_workspace"
    try:
        copy_example(repo_root, workspace)
        verify_publishable_standard(binary, workspace)
        print("release-control-plane-publishable-standard-ok")
    finally:
        if temp_root is not None and not args.keep_work_dir:
            temp_root.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
