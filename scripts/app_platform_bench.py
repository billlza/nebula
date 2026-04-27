#!/usr/bin/env python3
from __future__ import annotations

import argparse
import http.client
import json
import math
import os
import platform
import selectors
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


EXPECTED_WORKLOAD_IDS = [
    "cli_cold_start",
    "service_json_db_crud",
    "thin_host_bridge_roundtrip",
    "state_sync_latency",
    "resident_memory",
    "nebula_ui_startup",
    "ui_action_roundtrip",
    "ui_snapshot_render",
    "ui_layout_pass",
    "ui_render_list_build",
    "ui_hit_test_dispatch",
    "ui_patch_apply",
    "ui_gpu_submit_smoke",
]
EXPECTED_REFERENCE_STACKS = ["cpp", "rust", "swift"]
EXPECTED_COMPARISON_LAYERS = ["nebula_owned", "host_owned", "ops_owned"]
EXPECTED_NEBULA_RUNNERS = ["bench", "resident_memory"]
EXPECTED_REFERENCE_RUNNERS = ["cpp_bench"]
COMMON_TEXT_FIELDS = {
    "clock",
    "platform",
    "perf_capability",
    "perf_counters",
    "perf_reason",
}
LATENCY_INT_FIELDS = {"warmup_iterations", "measure_iterations", "samples"}
LATENCY_FLOAT_FIELDS = {"p50_ms", "p90_ms", "p99_ms", "mean_ms", "stddev_ms", "throughput_ops_s"}
LATENCY_REQUIRED_METRIC_FIELDS = LATENCY_INT_FIELDS | LATENCY_FLOAT_FIELDS | COMMON_TEXT_FIELDS
MEMORY_INT_FIELDS = {"samples"}
MEMORY_FLOAT_FIELDS = {"p50_rss_mib", "p90_rss_mib", "p99_rss_mib", "mean_rss_mib", "max_rss_mib", "steady_state_seconds"}
MEMORY_REQUIRED_METRIC_FIELDS = MEMORY_INT_FIELDS | MEMORY_FLOAT_FIELDS | COMMON_TEXT_FIELDS | {"metric_kind", "unit"}


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def matrix_path() -> Path:
    return repo_root() / "benchmarks" / "app_platform" / "matrix.json"


def load_matrix() -> dict[str, Any]:
    with matrix_path().open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise SystemExit("app-platform matrix must be a JSON object")
    return data


def repo_version() -> str:
    return (repo_root() / "VERSION").read_text(encoding="utf-8").strip()


def host_platform_tag() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        arch = "x64"
    elif machine in {"arm64", "aarch64"}:
        arch = "arm64"
    else:
        arch = machine or "unknown"
    if system == "darwin":
        os_name = "macos"
    elif system == "linux":
        os_name = "linux"
    elif system == "windows":
        os_name = "windows"
    else:
        os_name = system or "unknown"
    return f"{os_name}-{arch}"


def machine_info() -> dict[str, Any]:
    return {
        "platform": platform.platform(),
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": sys.version.split()[0],
        "cpu_count": os.cpu_count(),
        "host_platform_tag": host_platform_tag(),
    }


def capture_version(path: str) -> str:
    try:
        result = subprocess.run(
            [path, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except Exception:
        return ""
    text = (result.stdout or result.stderr).strip()
    return text.splitlines()[0] if text else ""


def detect_toolchains(nebula_binary: str | None = None) -> dict[str, dict[str, Any]]:
    tools = {
        "nebula": str(Path(nebula_binary).resolve()) if nebula_binary else (shutil.which("nebula") or ""),
        "clang++": shutil.which("clang++") or "",
        "rustc": shutil.which("rustc") or "",
        "swiftc": shutil.which("swiftc") or "",
        "cmake": shutil.which("cmake") or "",
        "python3": shutil.which("python3") or "",
    }
    return {
        name: {
            "available": bool(path),
            "path": path,
            "version": capture_version(path) if path else "",
        }
        for name, path in tools.items()
    }


def run_command(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)


def benchmark_build_root() -> Path:
    root = repo_root() / "build" / "app_platform_bench"
    root.mkdir(parents=True, exist_ok=True)
    return root


def normalize_latency_metrics(metrics: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in LATENCY_REQUIRED_METRIC_FIELDS:
        if key not in metrics:
            raise SystemExit(f"missing benchmark metric field: {key}")
        value = metrics[key]
        if key in LATENCY_INT_FIELDS:
            out[key] = int(value)
        elif key in LATENCY_FLOAT_FIELDS:
            out[key] = float(value)
        else:
            out[key] = str(value)
    return out


def normalize_memory_metrics(metrics: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in MEMORY_REQUIRED_METRIC_FIELDS:
        if key not in metrics:
            raise SystemExit(f"missing resident-memory metric field: {key}")
        value = metrics[key]
        if key in MEMORY_INT_FIELDS:
            out[key] = int(value)
        elif key in MEMORY_FLOAT_FIELDS:
            out[key] = float(value)
        else:
            out[key] = str(value)
    return out


def parse_bench_kv_line(line: str) -> dict[str, Any]:
    payload: dict[str, Any] = {}
    parts = line.split()[1:]
    for token in parts:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        payload[key] = value
    return normalize_latency_metrics(payload)


def parse_bench_output(text: str) -> dict[str, Any]:
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("NEBULA_BENCH "):
            return parse_bench_kv_line(line)
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line.startswith("{"):
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict):
            try:
                return normalize_latency_metrics(payload)
            except Exception:
                continue
    raise SystemExit("failed to parse benchmark metrics from output")


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
        raise SystemExit("resident-memory runner requires stderr=PIPE")
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


def http_request(host: str, port: int, path: str) -> tuple[int, str]:
    conn = http.client.HTTPConnection(host, port, timeout=3)
    try:
        conn.request("GET", path, body=None, headers={})
        response = conn.getresponse()
        return response.status, response.read().decode("utf-8", errors="replace")
    finally:
        conn.close()


def wait_for_http_status(proc: subprocess.Popen[str],
                         stderr_chunks: list[str],
                         host: str,
                         port: int,
                         path: str,
                         expected_status: int,
                         timeout: float) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            fail_with_output(proc, stderr_chunks, f"service exited before {path} became ready: rc={proc.returncode}")
        try:
            status, _ = http_request(host, port, path)
            if status == expected_status:
                return
        except Exception:
            pass
        time.sleep(0.1)
    fail_with_output(proc, stderr_chunks, f"timed out waiting for {path}={expected_status}")


def percentile(sorted_samples: list[float], fraction: float) -> float:
    if not sorted_samples:
        raise SystemExit("cannot compute percentile of an empty sample set")
    if len(sorted_samples) == 1:
        return sorted_samples[0]
    index = int(round((len(sorted_samples) - 1) * fraction))
    if index < 0:
        index = 0
    if index >= len(sorted_samples):
        index = len(sorted_samples) - 1
    return sorted_samples[index]


def rss_kib(pid: int) -> int:
    if platform.system().lower() not in {"darwin", "linux"}:
        raise SystemExit(f"resident-memory runner is unsupported on host: {platform.system()}")
    result = subprocess.run(
        ["ps", "-o", "rss=", "-p", str(pid)],
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"ps rss sample failed for pid={pid}: {result.stderr.strip()}")
    text = result.stdout.strip()
    if not text:
        raise SystemExit(f"ps rss sample returned empty output for pid={pid}")
    return int(text)


def resident_memory_metrics(samples_kib: list[int], steady_state_seconds: float) -> dict[str, Any]:
    samples_mib = [sample / 1024.0 for sample in samples_kib]
    ordered = sorted(samples_mib)
    mean = sum(samples_mib) / len(samples_mib)
    return normalize_memory_metrics(
        {
            "metric_kind": "resident_memory",
            "unit": "MiB",
            "samples": len(samples_mib),
            "p50_rss_mib": percentile(ordered, 0.50),
            "p90_rss_mib": percentile(ordered, 0.90),
            "p99_rss_mib": percentile(ordered, 0.99),
            "mean_rss_mib": mean,
            "max_rss_mib": max(samples_mib),
            "steady_state_seconds": steady_state_seconds,
            "clock": "steady_state_process",
            "platform": host_platform_tag(),
            "perf_capability": "ps_rss",
            "perf_counters": "ps_rss",
            "perf_reason": "steady_state_sample",
        }
    )


def write_json(path: str, payload: dict[str, Any]) -> None:
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def bounded_output(text: str, limit: int = 20000) -> str:
    if len(text) <= limit:
        return text
    tail = text[-limit:]
    return f"[output truncated to last {limit} bytes]\n{tail}"


def verify_matrix(data: dict[str, Any]) -> None:
    if data.get("schema_version") != 1:
        raise SystemExit("app-platform matrix schema_version must be 1")
    if data.get("name") != "app_platform_convergence_matrix":
        raise SystemExit("app-platform matrix name must be app_platform_convergence_matrix")

    comparison_layers = data.get("comparison_layers")
    if comparison_layers != EXPECTED_COMPARISON_LAYERS:
        raise SystemExit(f"unexpected comparison_layers: {comparison_layers!r}")

    reference_stacks = data.get("reference_stacks")
    if not isinstance(reference_stacks, dict):
        raise SystemExit("app-platform matrix must define reference_stacks")
    reference_keys = sorted(reference_stacks.keys())
    if reference_keys != EXPECTED_REFERENCE_STACKS:
        raise SystemExit(f"unexpected reference stacks: {reference_keys!r}")
    for key, stack in reference_stacks.items():
        if not isinstance(stack, dict):
            raise SystemExit(f"reference stack {key} must be an object")
        for required in ("toolchain", "backend", "data", "ui"):
            if not isinstance(stack.get(required), str) or not stack[required]:
                raise SystemExit(f"reference stack {key} missing {required}")

    defaults = data.get("measurement_defaults")
    if not isinstance(defaults, dict):
        raise SystemExit("app-platform matrix must define measurement_defaults")
    if defaults.get("split_by_layer") is not True:
        raise SystemExit("measurement_defaults.split_by_layer must be true")
    if defaults.get("treat_host_renderer_as_nebula_runtime") is not False:
        raise SystemExit("measurement_defaults.treat_host_renderer_as_nebula_runtime must be false")
    blockers = defaults.get("public_claim_blocked_until")
    if blockers != ["internal_app_standard", "thin_host_contract"]:
        raise SystemExit(f"unexpected public-claim blockers: {blockers!r}")

    workloads = data.get("workloads")
    if not isinstance(workloads, list) or len(workloads) != len(EXPECTED_WORKLOAD_IDS):
        raise SystemExit("app-platform matrix must define exactly the expected workloads")
    ids = [workload.get("id") for workload in workloads]
    if ids != EXPECTED_WORKLOAD_IDS:
        raise SystemExit(f"unexpected workload ids: {ids!r}")
    for workload in workloads:
        if not isinstance(workload, dict):
            raise SystemExit("each app-platform workload must be an object")
        for key in ("id", "category", "description", "timed_scope", "comparison_layer"):
            if not isinstance(workload.get(key), str) or not workload[key]:
                raise SystemExit(f"workload missing {key}: {workload!r}")
        if workload["comparison_layer"] not in EXPECTED_COMPARISON_LAYERS:
            raise SystemExit(f"workload has unknown comparison_layer: {workload!r}")
        correctness = workload.get("correctness_contract")
        if not isinstance(correctness, list) or not correctness or not all(isinstance(item, str) and item for item in correctness):
            raise SystemExit(f"workload correctness_contract must be a non-empty string list: {workload!r}")
        nebula_paths = workload.get("nebula_paths")
        if not isinstance(nebula_paths, list) or not nebula_paths or not all(isinstance(item, str) and item for item in nebula_paths):
            raise SystemExit(f"workload nebula_paths must be a non-empty string list: {workload!r}")
        for rel in nebula_paths:
            if not (repo_root() / rel).exists():
                raise SystemExit(f"missing app-platform Nebula path: {rel}")
        nebula_runner = workload.get("nebula_runner")
        if nebula_runner is not None and nebula_runner not in EXPECTED_NEBULA_RUNNERS:
            raise SystemExit(f"unexpected nebula_runner: {workload!r}")
        nebula_project = workload.get("nebula_project")
        if nebula_project is not None:
            if not isinstance(nebula_project, str) or not nebula_project:
                raise SystemExit(f"workload nebula_project must be a non-empty string when present: {workload!r}")
            project_root = repo_root() / nebula_project
            if not (project_root / "nebula.toml").exists():
                raise SystemExit(f"missing app-platform Nebula project manifest: {nebula_project}")
            if not (project_root / "src" / "main.nb").exists():
                raise SystemExit(f"missing app-platform Nebula project entry source: {nebula_project}")
        reference_projects = workload.get("reference_projects")
        if reference_projects is not None:
            if not isinstance(reference_projects, dict):
                raise SystemExit(f"workload reference_projects must be an object when present: {workload!r}")
            for stack, reference_project in reference_projects.items():
                if stack not in EXPECTED_REFERENCE_STACKS:
                    raise SystemExit(f"workload has unknown reference stack: {workload!r}")
                if not isinstance(reference_project, str) or not reference_project:
                    raise SystemExit(f"workload reference project must be a non-empty string: {workload!r}")
                reference_root = repo_root() / reference_project
                if not (reference_root / "main.cpp").exists():
                    raise SystemExit(f"missing app-platform {stack} reference source: {reference_project}/main.cpp")
        reference_stacks = workload.get("reference_stacks")
        if reference_stacks is not None:
            if not isinstance(reference_stacks, list) or not all(isinstance(item, str) and item for item in reference_stacks):
                raise SystemExit(f"workload reference_stacks must be a string list when present: {workload!r}")
            unknown = sorted(set(reference_stacks) - set(EXPECTED_REFERENCE_STACKS))
            if unknown:
                raise SystemExit(f"workload has unknown reference_stacks: {workload!r}")
            project_keys = sorted((reference_projects or {}).keys())
            if sorted(reference_stacks) != project_keys:
                raise SystemExit(f"workload reference_stacks must match reference_projects keys: {workload!r}")
        reference_runner = workload.get("reference_runner")
        if reference_runner is not None and reference_runner not in EXPECTED_REFERENCE_RUNNERS:
            raise SystemExit(f"unexpected reference_runner: {workload!r}")


def build_plan_payload(nebula_binary: str | None = None) -> dict[str, Any]:
    data = load_matrix()
    verify_matrix(data)
    return {
        "schema_version": 1,
        "kind": "app_platform_benchmark_plan",
        "repo_version": repo_version(),
        "machine": machine_info(),
        "toolchains": detect_toolchains(nebula_binary),
        "matrix": data,
    }


def select_workloads(data: dict[str, Any], selected: list[str]) -> list[dict[str, Any]]:
    workloads = data["workloads"]
    if not selected:
        return [workload for workload in workloads if "nebula_project" in workload]
    wanted = set(selected)
    chosen = [workload for workload in workloads if workload["id"] in wanted]
    missing = sorted(wanted - {workload["id"] for workload in chosen})
    if missing:
        raise SystemExit(f"unknown workload ids: {', '.join(missing)}")
    non_runnable = sorted(workload["id"] for workload in chosen if "nebula_project" not in workload)
    if non_runnable:
        raise SystemExit(
            "selected app-platform workloads do not have runnable Nebula projects yet: "
            + ", ".join(non_runnable)
        )
    return chosen


def select_reference_workloads(data: dict[str, Any], selected: list[str], stack: str) -> list[dict[str, Any]]:
    if stack not in EXPECTED_REFERENCE_STACKS:
        raise SystemExit(f"unknown reference stack: {stack}")
    workloads = data["workloads"]
    if not selected:
        return [
            workload
            for workload in workloads
            if stack in workload.get("reference_projects", {})
        ]
    wanted = set(selected)
    chosen = [workload for workload in workloads if workload["id"] in wanted]
    missing = sorted(wanted - {workload["id"] for workload in chosen})
    if missing:
        raise SystemExit(f"unknown workload ids: {', '.join(missing)}")
    non_runnable = sorted(
        workload["id"]
        for workload in chosen
        if stack not in workload.get("reference_projects", {})
    )
    if non_runnable:
        raise SystemExit(
            f"selected app-platform workloads do not have runnable {stack} reference projects yet: "
            + ", ".join(non_runnable)
        )
    return chosen


def run_nebula_workload(binary: Path, workload: dict[str, Any], build_root: Path) -> dict[str, Any]:
    project_dir = (repo_root() / workload["nebula_project"]).resolve()
    workload_root = build_root / workload["id"]
    workload_root.mkdir(parents=True, exist_ok=True)

    fetch = run_command([str(binary), "fetch", str(project_dir)], workload_root)
    if fetch.returncode != 0:
        return {
            "workload": workload["id"],
            "status": "fetch_failed",
            "project_dir": str(project_dir),
            "returncode": fetch.returncode,
            "metrics": {},
            "stdout": fetch.stdout,
            "stderr": fetch.stderr,
        }

    runner = workload.get("nebula_runner", "bench")
    if runner == "resident_memory":
        return run_nebula_resident_memory_workload(binary, workload, build_root)

    bench = run_command([str(binary), "bench", "--dir", str(project_dir)], workload_root)
    if bench.returncode != 0:
        return {
            "workload": workload["id"],
            "status": "bench_failed",
            "project_dir": str(project_dir),
            "returncode": bench.returncode,
            "metrics": {},
            "stdout": bench.stdout,
            "stderr": bench.stderr,
        }
    try:
        metrics = parse_bench_output(bench.stdout + "\n" + bench.stderr)
    except SystemExit as exc:
        return {
            "workload": workload["id"],
            "status": "parse_failed",
            "project_dir": str(project_dir),
            "returncode": bench.returncode,
            "metrics": {},
            "stdout": bench.stdout,
            "stderr": bench.stderr + f"\n{exc}",
        }
    return {
        "workload": workload["id"],
        "status": "ok",
        "project_dir": str(project_dir),
        "returncode": bench.returncode,
        "metrics": metrics,
        "stdout": bench.stdout,
        "stderr": bench.stderr,
    }


def run_nebula_resident_memory_workload(binary: Path, workload: dict[str, Any], build_root: Path) -> dict[str, Any]:
    project_dir = (repo_root() / workload["nebula_project"]).resolve()
    workload_root = build_root / workload["id"]
    workload_root.mkdir(parents=True, exist_ok=True)

    out_dir = workload_root / "resident_out"
    out_dir.mkdir(parents=True, exist_ok=True)
    build = run_command([str(binary), "build", str(project_dir), "--mode", "release", "--out-dir", str(out_dir)], workload_root)
    if build.returncode != 0:
        return {
            "workload": workload["id"],
            "status": "build_failed",
            "project_dir": str(project_dir),
            "returncode": build.returncode,
            "metrics": {},
            "stdout": build.stdout,
            "stderr": build.stderr,
        }

    binary_name = "main.exe" if platform.system().lower() == "windows" else "main.out"
    service_binary = out_dir / binary_name
    if not service_binary.exists():
        return {
            "workload": workload["id"],
            "status": "build_output_missing",
            "project_dir": str(project_dir),
            "returncode": 1,
            "metrics": {},
            "stdout": build.stdout,
            "stderr": build.stderr + f"\nmissing built service binary: {service_binary}",
        }

    env = dict(os.environ)
    env["NEBULA_BIND_HOST"] = "127.0.0.1"
    env["NEBULA_PORT"] = "0"
    env["APP_STATE_DIR"] = str((workload_root / "state").resolve())
    proc = subprocess.Popen(
        [str(service_binary)],
        cwd=workload_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    stderr_chunks: list[str] = []
    try:
        _, port = wait_for_listener_bound(proc, stderr_chunks, timeout=20.0)
        wait_for_http_status(proc, stderr_chunks, "127.0.0.1", port, "/healthz", 200, timeout=20.0)
        time.sleep(0.25)
        steady_state_seconds = 1.0
        sample_interval_seconds = 0.2
        sample_count = int(round(steady_state_seconds / sample_interval_seconds))
        if sample_count < 3:
            sample_count = 3
        samples_kib: list[int] = []
        for _ in range(sample_count):
            if proc.poll() is not None:
                fail_with_output(proc, stderr_chunks, f"service exited during resident-memory sampling: rc={proc.returncode}")
            samples_kib.append(rss_kib(proc.pid))
            time.sleep(sample_interval_seconds)
        metrics = resident_memory_metrics(samples_kib, steady_state_seconds)
        stdout, stderr = finish_process_output(proc, stderr_chunks)
        return {
            "workload": workload["id"],
            "status": "ok",
            "project_dir": str(project_dir),
            "returncode": 0,
            "metrics": metrics,
            "stdout": build.stdout + stdout,
            "stderr": build.stderr + stderr,
        }
    except SystemExit as exc:
        stdout, stderr = finish_process_output(proc, stderr_chunks)
        return {
            "workload": workload["id"],
            "status": "resident_memory_failed",
            "project_dir": str(project_dir),
            "returncode": 1,
            "metrics": {},
            "stdout": build.stdout + stdout,
            "stderr": build.stderr + stderr + f"\n{exc}",
        }


def run_nebula_payload(binary: str, selected: list[str]) -> dict[str, Any]:
    resolved_binary = Path(binary).resolve()
    data = load_matrix()
    verify_matrix(data)
    workloads = select_workloads(data, selected)
    results = []
    build_root = benchmark_build_root()
    for workload in workloads:
        if "nebula_project" not in workload:
            continue
        results.append(run_nebula_workload(resolved_binary, workload, build_root))
    return {
        "schema_version": 1,
        "kind": "app_platform_nebula_results",
        "repo_version": repo_version(),
        "machine": machine_info(),
        "toolchains": detect_toolchains(str(resolved_binary)),
        "results": results,
    }


def run_cpp_reference_workload(workload: dict[str, Any], build_root: Path) -> dict[str, Any]:
    clang = shutil.which("clang++")
    if not clang:
        return {
            "workload": workload["id"],
            "status": "toolchain_missing",
            "stack": "cpp",
            "comparison_layer": workload["comparison_layer"],
            "project_dir": "",
            "source_path": "",
            "returncode": 1,
            "metrics": {},
            "stdout": "",
            "stderr": "clang++ not found",
        }
    reference_project = workload.get("reference_projects", {}).get("cpp")
    project_dir = (repo_root() / reference_project).resolve()
    source = project_dir / "main.cpp"
    workload_root = build_root / "reference" / "cpp" / workload["id"]
    workload_root.mkdir(parents=True, exist_ok=True)
    output_name = "main.exe" if platform.system().lower() == "windows" else "main.out"
    output = workload_root / output_name
    link_flags = ["-lsqlite3"] if workload["id"] == "service_json_db_crud" else []
    compile_cmd = [
        clang,
        "-std=c++23",
        "-O2",
        "-DNDEBUG",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(source),
        "-o",
        str(output),
    ] + link_flags
    build = run_command(compile_cmd, workload_root)
    build_stderr = "[cmd] " + " ".join(compile_cmd) + "\n" + build.stderr
    if build.returncode != 0:
        return {
            "workload": workload["id"],
            "status": "build_failed",
            "stack": "cpp",
            "comparison_layer": workload["comparison_layer"],
            "project_dir": str(project_dir),
            "source_path": str(source),
            "returncode": build.returncode,
            "metrics": {},
            "stdout": bounded_output(build.stdout),
            "stderr": bounded_output(build_stderr),
        }
    run = run_command([str(output)], workload_root)
    run_stderr = build_stderr + "[cmd] " + str(output) + "\n" + run.stderr
    if run.returncode != 0:
        return {
            "workload": workload["id"],
            "status": "run_failed",
            "stack": "cpp",
            "comparison_layer": workload["comparison_layer"],
            "project_dir": str(project_dir),
            "source_path": str(source),
            "returncode": run.returncode,
            "metrics": {},
            "stdout": bounded_output(build.stdout + run.stdout),
            "stderr": bounded_output(run_stderr),
        }
    try:
        metrics = parse_bench_output(run.stdout + "\n" + run.stderr)
    except SystemExit as exc:
        return {
            "workload": workload["id"],
            "status": "parse_failed",
            "stack": "cpp",
            "comparison_layer": workload["comparison_layer"],
            "project_dir": str(project_dir),
            "source_path": str(source),
            "returncode": run.returncode,
            "metrics": {},
            "stdout": bounded_output(build.stdout + run.stdout),
            "stderr": bounded_output(run_stderr + f"\n{exc}"),
        }
    return {
        "workload": workload["id"],
        "status": "ok",
        "stack": "cpp",
        "comparison_layer": workload["comparison_layer"],
        "project_dir": str(project_dir),
        "source_path": str(source),
        "returncode": run.returncode,
        "metrics": metrics,
        "stdout": bounded_output(build.stdout + run.stdout),
        "stderr": bounded_output(run_stderr),
    }


def run_reference_payload(stack: str, selected: list[str]) -> dict[str, Any]:
    if stack != "cpp":
        raise SystemExit(f"runnable reference stack is not implemented yet: {stack}")
    data = load_matrix()
    verify_matrix(data)
    workloads = select_reference_workloads(data, selected, stack)
    build_root = benchmark_build_root()
    results = [run_cpp_reference_workload(workload, build_root) for workload in workloads]
    return {
        "schema_version": 1,
        "kind": "app_platform_reference_results",
        "matrix": "app_platform_convergence_matrix",
        "stack": stack,
        "repo_version": repo_version(),
        "machine": machine_info(),
        "toolchains": detect_toolchains(None),
        "results": results,
    }


def load_json_payload(path: str) -> dict[str, Any]:
    with Path(path).open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise SystemExit(f"JSON payload must be an object: {path}")
    return payload


def positive_metric(metrics: Any, key: str) -> float | None:
    if not isinstance(metrics, dict):
        return None
    try:
        value = float(metrics[key])
    except (KeyError, TypeError, ValueError):
        return None
    if not math.isfinite(value) or value <= 0.0:
        return None
    return value


def compare_payload(stack: str, nebula_json: str, reference_json: str) -> dict[str, Any]:
    nebula = load_json_payload(nebula_json)
    reference = load_json_payload(reference_json)
    if nebula.get("kind") != "app_platform_nebula_results":
        raise SystemExit(f"unexpected Nebula payload kind: {nebula.get('kind')!r}")
    if reference.get("kind") != "app_platform_reference_results" or reference.get("stack") != stack:
        raise SystemExit(f"unexpected reference payload: kind={reference.get('kind')!r} stack={reference.get('stack')!r}")

    reference_results = {item.get("workload"): item for item in reference.get("results", []) if isinstance(item, dict)}
    results: list[dict[str, Any]] = []
    for nebula_item in nebula.get("results", []):
        if not isinstance(nebula_item, dict):
            continue
        workload = nebula_item.get("workload")
        reference_item = reference_results.get(workload)
        if reference_item is None:
            results.append({
                "workload": workload,
                "status": "missing_reference",
                "nebula_status": nebula_item.get("status", ""),
                "reference_status": "",
                "nebula_metrics": nebula_item.get("metrics", {}),
                "reference_metrics": {},
                "ratios": {},
            })
            continue
        nebula_status = str(nebula_item.get("status", ""))
        reference_status = str(reference_item.get("status", ""))
        nebula_metrics = nebula_item.get("metrics", {})
        reference_metrics = reference_item.get("metrics", {})
        status = "ok" if nebula_status == "ok" and reference_status == "ok" else "not_comparable"
        ratios: dict[str, float] = {}
        comparison_error = ""
        if status == "ok":
            metric_values = {
                "nebula.mean_ms": positive_metric(nebula_metrics, "mean_ms"),
                "reference.mean_ms": positive_metric(reference_metrics, "mean_ms"),
                "nebula.p99_ms": positive_metric(nebula_metrics, "p99_ms"),
                "reference.p99_ms": positive_metric(reference_metrics, "p99_ms"),
                "nebula.throughput_ops_s": positive_metric(nebula_metrics, "throughput_ops_s"),
                "reference.throughput_ops_s": positive_metric(reference_metrics, "throughput_ops_s"),
            }
            invalid_metrics = sorted(key for key, value in metric_values.items() if value is None)
            if invalid_metrics:
                status = "invalid_metrics"
                comparison_error = "missing, non-finite, or non-positive metric(s): " + ", ".join(invalid_metrics)
            else:
                ratios = {
                    "nebula_to_reference_mean_ms": metric_values["nebula.mean_ms"] / metric_values["reference.mean_ms"],
                    "nebula_to_reference_p99_ms": metric_values["nebula.p99_ms"] / metric_values["reference.p99_ms"],
                    "nebula_to_reference_throughput_ops_s": (
                        metric_values["nebula.throughput_ops_s"] / metric_values["reference.throughput_ops_s"]
                    ),
                }
        result = {
            "workload": workload,
            "status": status,
            "nebula_status": nebula_status,
            "reference_status": reference_status,
            "nebula_metrics": nebula_metrics,
            "reference_metrics": reference_metrics,
            "ratios": ratios,
        }
        if comparison_error:
            result["comparison_error"] = comparison_error
        results.append(result)

    return {
        "schema_version": 1,
        "kind": "app_platform_comparison_results",
        "matrix": "app_platform_convergence_matrix",
        "stack": stack,
        "repo_version": repo_version(),
        "machine": machine_info(),
        "toolchains": {
            "nebula": nebula.get("toolchains", {}),
            "reference": reference.get("toolchains", {}),
        },
        "claim_policy": "measurement_only_no_threshold",
        "results": results,
    }


def render_text(payload: dict[str, Any]) -> str:
    lines = [
        "App platform convergence matrix",
        f"repo_version: {payload['repo_version']}",
        f"host_platform: {payload['machine']['host_platform_tag']}",
        "workloads:",
    ]
    for workload in payload["matrix"]["workloads"]:
        reference_projects = workload.get("reference_projects", {})
        reference_text = ""
        if reference_projects:
            reference_text = " refs=" + ",".join(f"{key}:{value}" for key, value in sorted(reference_projects.items()))
        lines.append(
            f"- {workload['id']}: {workload['category']} [{workload['comparison_layer']}] -> "
            + ", ".join(workload["nebula_paths"])
            + reference_text
        )
    lines.append("reference_stacks:")
    for key, stack in payload["matrix"]["reference_stacks"].items():
        lines.append(
            f"- {key}: toolchain={stack['toolchain']}; backend={stack['backend']}; "
            f"data={stack['data']}; ui={stack['ui']}"
        )
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Nebula app-platform convergence benchmark helper")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("verify", help="verify the fixed app-platform benchmark matrix")

    plan_parser = subparsers.add_parser("plan", help="emit the app-platform benchmark plan")
    plan_parser.add_argument("--format", choices=["text", "json"], default="text")

    run_parser = subparsers.add_parser("run-nebula", help="run Nebula-backed app-platform workloads")
    run_parser.add_argument("--binary", required=True)
    run_parser.add_argument("--workload", action="append", default=[])
    run_parser.add_argument("--json-out", default="")

    reference_parser = subparsers.add_parser("run-reference", help="run reference app-platform workloads")
    reference_parser.add_argument("--stack", choices=["cpp"], required=True)
    reference_parser.add_argument("--workload", action="append", default=[])
    reference_parser.add_argument("--json-out", default="")

    compare_parser = subparsers.add_parser("compare", help="compare Nebula and reference app-platform results")
    compare_parser.add_argument("--stack", choices=["cpp"], required=True)
    compare_parser.add_argument("--nebula-json", required=True)
    compare_parser.add_argument("--reference-json", required=True)
    compare_parser.add_argument("--json-out", default="")

    args = parser.parse_args(argv)
    if args.command == "verify":
        verify_matrix(load_matrix())
        print("app-platform-benchmark-matrix-ok")
        return 0

    if args.command == "plan":
        payload = build_plan_payload()
        if args.format == "json":
            print(json.dumps(payload, indent=2, sort_keys=True))
        else:
            print(render_text(payload))
        return 0

    if args.command == "run-nebula":
        payload = run_nebula_payload(args.binary, args.workload)
        if args.json_out:
            write_json(args.json_out, payload)
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    if args.command == "run-reference":
        payload = run_reference_payload(args.stack, args.workload)
        if args.json_out:
            write_json(args.json_out, payload)
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0 if all(item.get("status") == "ok" for item in payload["results"]) else 1

    if args.command == "compare":
        payload = compare_payload(args.stack, args.nebula_json, args.reference_json)
        if args.json_out:
            write_json(args.json_out, payload)
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0 if all(item.get("status") == "ok" for item in payload["results"]) else 1

    raise SystemExit(f"unsupported command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
