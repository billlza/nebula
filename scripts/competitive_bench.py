#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tomllib
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


INT_FIELDS = {"warmup_iterations", "measure_iterations", "samples"}
FLOAT_FIELDS = {"p50_ms", "p90_ms", "p99_ms", "mean_ms", "stddev_ms", "throughput_ops_s"}
REQUIRED_METRIC_FIELDS = INT_FIELDS | FLOAT_FIELDS | {
    "clock",
    "platform",
    "perf_capability",
    "perf_counters",
    "perf_reason",
}
ALLOWED_REFERENCE_SUPPORT = {"crypto_ref_support"}
DEFAULT_LANGUAGES = ["nebula", "cpp", "rust", "swift"]


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def matrix_path() -> Path:
    return repo_root() / "benchmarks" / "backend_crypto" / "matrix.json"


def load_matrix() -> dict[str, Any]:
    with matrix_path().open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise SystemExit("benchmark matrix must be a JSON object")
    return data


def repo_version() -> str:
    return (repo_root() / "VERSION").read_text(encoding="utf-8").strip()


def benchmark_build_root() -> Path:
    root = repo_root() / "build" / "competitive_bench"
    root.mkdir(parents=True, exist_ok=True)
    return root


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


def _capture_version(path: str) -> str:
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
    resolved_nebula = str(Path(nebula_binary).resolve()) if nebula_binary else (shutil.which("nebula") or "")
    clangxx = shutil.which("clang++") or ""
    rustc = shutil.which("rustc") or ""
    swiftc = shutil.which("swiftc") or ""
    return {
        "nebula": {
            "available": bool(resolved_nebula),
            "path": resolved_nebula,
            "version": _capture_version(resolved_nebula) if resolved_nebula else "",
        },
        "clang++": {
            "available": bool(clangxx),
            "path": clangxx,
            "version": _capture_version(clangxx) if clangxx else "",
        },
        "rustc": {
            "available": bool(rustc),
            "path": rustc,
            "version": _capture_version(rustc) if rustc else "",
        },
        "swiftc": {
            "available": bool(swiftc),
            "path": swiftc,
            "version": _capture_version(swiftc) if swiftc else "",
        },
    }


def verify_matrix(data: dict[str, Any]) -> None:
    if data.get("schema_version") != 1:
        raise SystemExit("benchmark matrix schema_version must be 1")
    workloads = data.get("workloads")
    if not isinstance(workloads, list) or len(workloads) != 5:
        raise SystemExit("benchmark matrix must define exactly 5 workloads")
    seen: set[str] = set()
    for workload in workloads:
        if not isinstance(workload, dict):
            raise SystemExit("each workload must be a JSON object")
        workload_id = workload.get("id")
        if not isinstance(workload_id, str) or not workload_id:
            raise SystemExit("each workload must define a non-empty id")
        if workload_id in seen:
            raise SystemExit(f"duplicate workload id: {workload_id}")
        seen.add(workload_id)

        nebula_project = workload.get("nebula_project")
        if not isinstance(nebula_project, str) or not nebula_project:
            raise SystemExit(f"workload {workload_id} must define nebula_project")
        project_dir = repo_root() / nebula_project
        if not (project_dir / "nebula.toml").exists():
            raise SystemExit(f"workload {workload_id} is missing nebula.toml: {project_dir}")
        if not (project_dir / "src" / "main.nb").exists():
            raise SystemExit(f"workload {workload_id} is missing src/main.nb: {project_dir}")

        refs = workload.get("reference_languages")
        if not isinstance(refs, list) or sorted(refs) != ["cpp", "rust", "swift"]:
            raise SystemExit(f"workload {workload_id} must define cpp/rust/swift reference_languages")

        reference_paths = workload.get("references")
        if not isinstance(reference_paths, dict):
            raise SystemExit(f"workload {workload_id} must define references")
        for language in refs:
            rel = reference_paths.get(language)
            if not isinstance(rel, str) or not rel:
                raise SystemExit(f"workload {workload_id} missing reference path for {language}")
            if not (repo_root() / rel).exists():
                raise SystemExit(f"workload {workload_id} missing reference file for {language}: {rel}")

        support = workload.get("reference_support", [])
        if not isinstance(support, list):
            raise SystemExit(f"workload {workload_id} reference_support must be a list")
        unknown_support = sorted(set(str(item) for item in support) - ALLOWED_REFERENCE_SUPPORT)
        if unknown_support:
            raise SystemExit(f"workload {workload_id} has unknown reference_support: {', '.join(unknown_support)}")

        for key in ("timed_scope", "setup_scope", "input_profile", "correctness_contract"):
            if key not in workload:
                raise SystemExit(f"workload {workload_id} must define {key}")

    thresholds = data.get("victory_thresholds")
    if not isinstance(thresholds, dict):
        raise SystemExit("benchmark matrix must define victory_thresholds")
    required_keys = {
        "target_workload_count",
        "required_win_count",
        "min_lead_vs_cpp_rust_pct",
        "max_allowed_trail_pct",
        "swift_scope",
    }
    missing = sorted(required_keys - set(thresholds.keys()))
    if missing:
        raise SystemExit(f"benchmark matrix missing threshold keys: {', '.join(missing)}")


def normalize_metrics(metrics: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in REQUIRED_METRIC_FIELDS:
        if key not in metrics:
            raise SystemExit(f"missing benchmark metric field: {key}")
        value = metrics[key]
        if key in INT_FIELDS:
            out[key] = int(value)
        elif key in FLOAT_FIELDS:
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
    return normalize_metrics(payload)


def parse_bench_output(text: str) -> dict[str, Any]:
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith("NEBULA_BENCH ") or line.startswith("BACKEND_CRYPTO_BENCH "):
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
                return normalize_metrics(payload)
            except Exception:
                continue
    raise SystemExit("failed to parse benchmark metrics from output")


def select_workloads(data: dict[str, Any], selected: list[str]) -> list[dict[str, Any]]:
    workloads = data["workloads"]
    if not selected:
        return list(workloads)
    wanted = set(selected)
    chosen = [workload for workload in workloads if workload["id"] in wanted]
    missing = sorted(wanted - {workload["id"] for workload in chosen})
    if missing:
        raise SystemExit(f"unknown workload ids: {', '.join(missing)}")
    return chosen


def write_json(path: str, payload: dict[str, Any]) -> None:
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_command(cmd: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)


def path_if_exists(executable: str) -> str:
    return shutil.which(executable) or ""


def c_compiler_path() -> str:
    return path_if_exists("clang") or path_if_exists("cc")


def static_archiver_path() -> str:
    return path_if_exists("ar") or path_if_exists("llvm-ar")


def crypto_support_inputs() -> tuple[list[Path], list[Path]]:
    package_root = repo_root() / "official" / "nebula-crypto"
    with package_root.joinpath("nebula.toml").open("rb") as handle:
        manifest = tomllib.load(handle)
    native = manifest.get("native", {})
    c_sources = native.get("c_sources", [])
    include_dirs = native.get("include_dirs", [])
    sources = [repo_root() / "benchmarks" / "backend_crypto" / "ref_support" / "crypto_ref_support.c"]
    for rel in c_sources:
        sources.append(package_root / str(rel))
    includes = [repo_root() / "benchmarks" / "backend_crypto" / "ref_support"]
    for rel in include_dirs:
        includes.append(package_root / str(rel))
    return sources, includes


def ensure_crypto_support_library(build_root: Path) -> tuple[Path, Path]:
    cc = c_compiler_path()
    if not cc:
        raise SystemExit("unable to locate a C compiler for crypto_ref_support")
    archiver = static_archiver_path()
    if not archiver:
        raise SystemExit("unable to locate a static archiver for crypto_ref_support")

    support_root = build_root / "crypto_ref_support"
    obj_root = support_root / "obj"
    lib_path = support_root / "libbackend_crypto_refsupport.a"
    module_root = support_root / "swift_module"
    module_root.mkdir(parents=True, exist_ok=True)
    obj_root.mkdir(parents=True, exist_ok=True)

    sources, include_dirs = crypto_support_inputs()
    objects: list[Path] = []
    include_flags: list[str] = []
    for include_dir in include_dirs:
        include_flags.extend(["-I", str(include_dir)])
    for source in sources:
        rel_name = source.relative_to(repo_root()).as_posix().replace("/", "_")
        obj = obj_root / (rel_name + ".o")
        cmd = [
            cc,
            "-std=c11",
            "-O2",
            "-DNDEBUG",
            *include_flags,
            "-c",
            str(source),
            "-o",
            str(obj),
        ]
        result = run_command(cmd, repo_root())
        if result.returncode != 0:
            raise SystemExit(
                "failed to build crypto_ref_support object:\n"
                + result.stdout
                + result.stderr
            )
        objects.append(obj)

    result = run_command([archiver, "rcs", str(lib_path), *[str(obj) for obj in objects]], repo_root())
    if result.returncode != 0:
        raise SystemExit("failed to archive crypto_ref_support library:\n" + result.stdout + result.stderr)

    modulemap = module_root / "module.modulemap"
    header_path = (repo_root() / "benchmarks" / "backend_crypto" / "ref_support" / "crypto_ref_support.h").resolve()
    modulemap.write_text(
        f'module CryptoRefSupport [system] {{\n  header "{header_path}"\n  export *\n}}\n',
        encoding="utf-8",
    )
    return lib_path, module_root


def language_allowed(language: str, workload: dict[str, Any], toolchains: dict[str, dict[str, Any]]) -> tuple[bool, str]:
    if language == "swift" and platform.system() != "Darwin":
        return False, "swift_scope_macos_only"
    if language == "cpp" and not toolchains["clang++"]["available"]:
        return False, "toolchain_missing"
    if language == "rust" and not toolchains["rustc"]["available"]:
        return False, "toolchain_missing"
    if language == "swift" and not toolchains["swiftc"]["available"]:
        return False, "toolchain_missing"
    return True, ""


def record_skip(language: str, reason: str, source_path: str) -> dict[str, Any]:
    return {
        "language": language,
        "status": f"skipped_{reason}",
        "source_path": source_path,
        "returncode": 0,
        "metrics": {},
        "stdout": "",
        "stderr": "",
    }


def compile_cpp_reference(source_path: Path, out_path: Path, needs_crypto_support: bool, lib_path: Path | None) -> subprocess.CompletedProcess[str]:
    clangxx = shutil.which("clang++")
    assert clangxx is not None
    include_flags = [
        "-I",
        str((repo_root() / "benchmarks" / "backend_crypto" / "reference" / "cpp").resolve()),
    ]
    if needs_crypto_support:
        include_flags.extend(
            [
                "-I",
                str((repo_root() / "benchmarks" / "backend_crypto" / "ref_support").resolve()),
                "-I",
                str((repo_root() / "official" / "nebula-crypto" / "native" / "include").resolve()),
            ]
        )
    cmd = [clangxx, "-std=c++23", "-O2", "-DNDEBUG", *include_flags, str(source_path), "-o", str(out_path)]
    if needs_crypto_support and lib_path is not None:
        cmd.append(str(lib_path))
    return run_command(cmd, repo_root())


def compile_rust_reference(source_path: Path, out_path: Path, needs_crypto_support: bool, lib_path: Path | None) -> subprocess.CompletedProcess[str]:
    rustc = shutil.which("rustc")
    assert rustc is not None
    cmd = [rustc, str(source_path), "--edition=2021", "-C", "opt-level=2", "-C", "debuginfo=0", "-o", str(out_path)]
    if needs_crypto_support and lib_path is not None:
        cmd.extend(["-L", f"native={lib_path.parent}", "-l", "static=backend_crypto_refsupport"])
    return run_command(cmd, repo_root())


def compile_swift_reference(source_path: Path,
                            out_path: Path,
                            needs_crypto_support: bool,
                            lib_path: Path | None,
                            module_root: Path | None) -> subprocess.CompletedProcess[str]:
    swiftc = shutil.which("swiftc")
    assert swiftc is not None
    runtime_source = repo_root() / "benchmarks" / "backend_crypto" / "reference" / "swift" / "BenchRuntime.swift"
    cmd = [swiftc, "-O", "-whole-module-optimization", str(runtime_source), str(source_path), "-o", str(out_path)]
    if needs_crypto_support and lib_path is not None and module_root is not None:
        official_include = (repo_root() / "official" / "nebula-crypto" / "native" / "include").resolve()
        cmd.extend(
            [
                "-I",
                str(module_root),
                "-Xcc",
                "-I",
                "-Xcc",
                str(official_include),
                "-L",
                str(lib_path.parent),
                "-lbackend_crypto_refsupport",
            ]
        )
    return run_command(cmd, repo_root())


def run_reference(workload: dict[str, Any],
                  language: str,
                  build_root: Path,
                  toolchains: dict[str, dict[str, Any]]) -> dict[str, Any]:
    source_rel = workload["references"][language]
    source_path = (repo_root() / source_rel).resolve()
    allowed, reason = language_allowed(language, workload, toolchains)
    if not allowed:
        return record_skip(language, reason, str(source_path))

    workload_build = build_root / language / workload["id"]
    workload_build.mkdir(parents=True, exist_ok=True)
    out_path = workload_build / f"{workload['id']}-{language}"
    needs_crypto_support = "crypto_ref_support" in workload.get("reference_support", [])
    lib_path: Path | None = None
    module_root: Path | None = None
    if needs_crypto_support:
        lib_path, module_root = ensure_crypto_support_library(build_root)

    if language == "cpp":
        compiled = compile_cpp_reference(source_path, out_path, needs_crypto_support, lib_path)
    elif language == "rust":
        compiled = compile_rust_reference(source_path, out_path, needs_crypto_support, lib_path)
    elif language == "swift":
        compiled = compile_swift_reference(source_path, out_path, needs_crypto_support, lib_path, module_root)
    else:
        raise SystemExit(f"unsupported reference language: {language}")

    if compiled.returncode != 0:
        return {
            "language": language,
            "status": "compile_failed",
            "source_path": str(source_path),
            "returncode": compiled.returncode,
            "metrics": {},
            "stdout": compiled.stdout,
            "stderr": compiled.stderr,
        }

    executed = run_command([str(out_path)], workload_build)
    if executed.returncode != 0:
        return {
            "language": language,
            "status": "run_failed",
            "source_path": str(source_path),
            "returncode": executed.returncode,
            "metrics": {},
            "stdout": executed.stdout,
            "stderr": executed.stderr,
        }
    try:
        metrics = parse_bench_output(executed.stdout + "\n" + executed.stderr)
    except SystemExit as exc:
        return {
            "language": language,
            "status": "parse_failed",
            "source_path": str(source_path),
            "returncode": executed.returncode,
            "metrics": {},
            "stdout": executed.stdout,
            "stderr": executed.stderr + f"\n{exc}",
        }
    return {
        "language": language,
        "status": "ok",
        "source_path": str(source_path),
        "returncode": executed.returncode,
        "metrics": metrics,
        "stdout": executed.stdout,
        "stderr": executed.stderr,
    }


def prepare_nebula_project(binary: Path, project_dir: Path) -> subprocess.CompletedProcess[str]:
    return run_command([str(binary), "fetch", str(project_dir)], repo_root())


def run_nebula_workload(binary: Path, workload: dict[str, Any], build_root: Path) -> dict[str, Any]:
    project_dir = (repo_root() / workload["nebula_project"]).resolve()
    fetch = prepare_nebula_project(binary, project_dir)
    if fetch.returncode != 0:
        return {
            "language": "nebula",
            "status": "fetch_failed",
            "project_dir": str(project_dir),
            "returncode": fetch.returncode,
            "metrics": {},
            "stdout": fetch.stdout,
            "stderr": fetch.stderr,
        }

    run_dir = build_root / "nebula" / workload["id"]
    run_dir.mkdir(parents=True, exist_ok=True)
    bench = run_command([str(binary), "bench", "--dir", str(project_dir)], run_dir)
    if bench.returncode != 0:
        return {
            "language": "nebula",
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
            "language": "nebula",
            "status": "parse_failed",
            "project_dir": str(project_dir),
            "returncode": bench.returncode,
            "metrics": {},
            "stdout": bench.stdout,
            "stderr": bench.stderr + f"\n{exc}",
        }
    return {
        "language": "nebula",
        "status": "ok",
        "project_dir": str(project_dir),
        "returncode": bench.returncode,
        "metrics": metrics,
        "stdout": bench.stdout,
        "stderr": bench.stderr,
    }


def delta_pct(current: float, baseline: float) -> float | None:
    if baseline <= 0.0:
        return None
    return ((current - baseline) / baseline) * 100.0


def classify_delta(delta: float | None, thresholds: dict[str, Any]) -> str:
    if delta is None:
        return "unknown"
    if delta >= float(thresholds["min_lead_vs_cpp_rust_pct"]):
        return "hard_win"
    if delta <= float(thresholds["max_allowed_trail_pct"]):
        return "hard_trail"
    return "within_band"


def compute_language_comparison(nebula_result: dict[str, Any],
                                ref_result: dict[str, Any],
                                thresholds: dict[str, Any]) -> dict[str, Any]:
    if nebula_result["status"] != "ok":
        return {"status": "nebula_not_ok"}
    if ref_result["status"] != "ok":
        return {"status": ref_result["status"]}
    nebula_metrics = nebula_result["metrics"]
    ref_metrics = ref_result["metrics"]
    throughput_delta = delta_pct(
        float(nebula_metrics["throughput_ops_s"]), float(ref_metrics["throughput_ops_s"])
    )
    mean_latency_delta = delta_pct(
        float(ref_metrics["mean_ms"]), float(nebula_metrics["mean_ms"])
    )
    return {
        "status": classify_delta(throughput_delta, thresholds),
        "throughput_delta_pct": throughput_delta,
        "mean_latency_delta_pct": mean_latency_delta,
        "reference_throughput_ops_s": ref_metrics["throughput_ops_s"],
        "nebula_throughput_ops_s": nebula_metrics["throughput_ops_s"],
        "reference_mean_ms": ref_metrics["mean_ms"],
        "nebula_mean_ms": nebula_metrics["mean_ms"],
    }


def aggregate_cpp_rust(workload_results: dict[str, dict[str, Any]], thresholds: dict[str, Any]) -> dict[str, Any]:
    nebula_result = workload_results["nebula"]
    available = [
        workload_results[language]
        for language in ("cpp", "rust")
        if language in workload_results and workload_results[language]["status"] == "ok"
    ]
    if nebula_result["status"] != "ok":
        return {"status": "nebula_not_ok"}
    if not available:
        return {"status": "reference_missing"}
    best = max(available, key=lambda result: float(result["metrics"]["throughput_ops_s"]))
    throughput_delta = delta_pct(
        float(nebula_result["metrics"]["throughput_ops_s"]),
        float(best["metrics"]["throughput_ops_s"]),
    )
    mean_latency_delta = delta_pct(
        float(best["metrics"]["mean_ms"]),
        float(nebula_result["metrics"]["mean_ms"]),
    )
    return {
        "status": classify_delta(throughput_delta, thresholds),
        "best_reference_language": best["language"],
        "throughput_delta_pct": throughput_delta,
        "mean_latency_delta_pct": mean_latency_delta,
    }


def command_list(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    for workload in data["workloads"]:
        print(workload["id"])
    return 0


def command_verify(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    print("competitive-bench-matrix-ok")
    return 0


def command_plan(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    out = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "repo_version": repo_version(),
        "matrix": data,
        "machine": machine_info(),
        "toolchains": detect_toolchains(args.binary),
    }
    if args.format == "json":
        print(json.dumps(out, indent=2, sort_keys=True))
    else:
        print(f"matrix={data['name']}")
        print(f"machine={out['machine']['system']}/{out['machine']['machine']}")
        print("workloads=" + ",".join(workload["id"] for workload in data["workloads"]))
    return 0


def command_run_nebula(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    binary = Path(args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"nebula binary not found: {binary}")
    build_root = benchmark_build_root() / "nebula_only"
    results: list[dict[str, Any]] = []
    for workload in select_workloads(data, args.workload):
        result = run_nebula_workload(binary, workload, build_root)
        results.append(
            {
                "id": workload["id"],
                "project_dir": result.get("project_dir", str((repo_root() / workload["nebula_project"]).resolve())),
                "status": result["status"],
                "returncode": result["returncode"],
                "metrics": result.get("metrics", {}),
                "stdout": result["stdout"],
                "stderr": result["stderr"],
            }
        )
    out = {
        "schema_version": 1,
        "kind": "backend_crypto_nebula_results",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "matrix": data["name"],
        "machine": machine_info(),
        "toolchains": detect_toolchains(str(binary)),
        "results": results,
    }
    if args.json_out:
        write_json(args.json_out, out)
    print(json.dumps(out, indent=2, sort_keys=True))
    failed = [result for result in results if result["status"] != "ok"]
    return 1 if failed else 0


def command_run_reference(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    build_root = benchmark_build_root() / "references_only"
    toolchains = detect_toolchains()
    language = args.language
    rendered_results: list[dict[str, Any]] = []
    for workload in select_workloads(data, args.workload):
        if language not in workload["reference_languages"]:
            rendered_results.append(record_skip(language, "language_not_declared", ""))
            continue
        rendered_results.append(run_reference(workload, language, build_root, toolchains))
    out = {
        "schema_version": 1,
        "kind": "backend_crypto_reference_results",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "repo_version": repo_version(),
        "matrix": data["name"],
        "machine": machine_info(),
        "toolchains": toolchains,
        "language": language,
        "results": rendered_results,
    }
    if args.json_out:
        write_json(args.json_out, out)
    print(json.dumps(out, indent=2, sort_keys=True))
    failed = [result for result in rendered_results if result["status"] not in {"ok"} and not result["status"].startswith("skipped_")]
    return 1 if failed else 0


def command_run_matrix(args: argparse.Namespace) -> int:
    data = load_matrix()
    verify_matrix(data)
    binary = Path(args.binary).resolve()
    if not binary.exists():
        raise SystemExit(f"nebula binary not found: {binary}")

    selected_languages = args.language if args.language else list(DEFAULT_LANGUAGES)
    unknown = sorted(set(selected_languages) - set(DEFAULT_LANGUAGES))
    if unknown:
        raise SystemExit(f"unknown languages requested: {', '.join(unknown)}")

    toolchains = detect_toolchains(str(binary))
    build_root = benchmark_build_root() / "matrix"
    workload_reports: list[dict[str, Any]] = []
    thresholds = data["victory_thresholds"]

    for workload in select_workloads(data, args.workload):
        language_results: dict[str, dict[str, Any]] = {}
        if "nebula" in selected_languages:
            language_results["nebula"] = run_nebula_workload(binary, workload, build_root)
        for language in ("cpp", "rust", "swift"):
            if language in selected_languages:
                language_results[language] = run_reference(workload, language, build_root, toolchains)

        comparisons: dict[str, Any] = {}
        if "nebula" in language_results:
            nebula_result = language_results["nebula"]
            for language in ("cpp", "rust", "swift"):
                if language in language_results:
                    comparisons[language] = compute_language_comparison(
                        nebula_result, language_results[language], thresholds
                    )
            comparisons["cpp_rust_best"] = aggregate_cpp_rust(language_results, thresholds)

        workload_reports.append(
            {
                "id": workload["id"],
                "category": workload["category"],
                "description": workload["description"],
                "timed_scope": workload["timed_scope"],
                "setup_scope": workload["setup_scope"],
                "input_profile": workload["input_profile"],
                "correctness_contract": workload["correctness_contract"],
                "results": language_results,
                "comparisons": comparisons,
            }
        )

    summary = {
        "cpp_rust_hard_win_count": 0,
        "cpp_rust_hard_trail_count": 0,
        "cpp_rust_within_band_count": 0,
        "reference_missing_count": 0,
    }
    for workload in workload_reports:
        status = workload.get("comparisons", {}).get("cpp_rust_best", {}).get("status")
        if status == "hard_win":
            summary["cpp_rust_hard_win_count"] += 1
        elif status == "hard_trail":
            summary["cpp_rust_hard_trail_count"] += 1
        elif status == "within_band":
            summary["cpp_rust_within_band_count"] += 1
        elif status == "reference_missing":
            summary["reference_missing_count"] += 1

    out = {
        "schema_version": 1,
        "kind": "backend_crypto_competitive_results",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "repo_version": repo_version(),
        "matrix": data["name"],
        "machine": machine_info(),
        "toolchains": toolchains,
        "thresholds": thresholds,
        "workloads": workload_reports,
        "summary": summary,
    }
    if args.json_out:
        write_json(args.json_out, out)
    print(json.dumps(out, indent=2, sort_keys=True))
    failures = []
    for workload in workload_reports:
        for result in workload["results"].values():
            status = result["status"]
            if status == "ok" or status.startswith("skipped_"):
                continue
            failures.append((workload["id"], result["language"], status))
    return 1 if failures else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Nebula Backend+Crypto competitive benchmark helper")
    sub = parser.add_subparsers(dest="command", required=True)

    list_parser = sub.add_parser("list", help="list workload ids")
    list_parser.set_defaults(func=command_list)

    verify_parser = sub.add_parser("verify", help="validate matrix and workload layout")
    verify_parser.set_defaults(func=command_verify)

    plan_parser = sub.add_parser("plan", help="emit machine/toolchain-aware benchmark plan")
    plan_parser.add_argument("--format", choices=["json", "text"], default="text")
    plan_parser.add_argument("--binary", default="", help="optional nebula binary path for toolchain reporting")
    plan_parser.set_defaults(func=command_plan)

    run_nebula_parser = sub.add_parser("run-nebula", help="run the committed Nebula workload implementations")
    run_nebula_parser.add_argument("--binary", required=True, help="path to nebula binary")
    run_nebula_parser.add_argument("--workload", action="append", default=[], help="workload id to run; repeat to narrow the set")
    run_nebula_parser.add_argument("--json-out", default="", help="optional path to write JSON results")
    run_nebula_parser.set_defaults(func=command_run_nebula)

    run_reference_parser = sub.add_parser("run-reference", help="run one external reference language across selected workloads")
    run_reference_parser.add_argument("--language", choices=["cpp", "rust", "swift"], required=True)
    run_reference_parser.add_argument("--workload", action="append", default=[], help="workload id to run; repeat to narrow the set")
    run_reference_parser.add_argument("--json-out", default="", help="optional path to write JSON results")
    run_reference_parser.set_defaults(func=command_run_reference)

    run_matrix_parser = sub.add_parser("run-matrix", help="run Nebula plus available external reference implementations and compute diffs")
    run_matrix_parser.add_argument("--binary", required=True, help="path to nebula binary")
    run_matrix_parser.add_argument("--workload", action="append", default=[], help="workload id to run; repeat to narrow the set")
    run_matrix_parser.add_argument("--language", action="append", default=[], help="language to include; defaults to nebula,cpp,rust,swift")
    run_matrix_parser.add_argument("--json-out", default="", help="optional path to write JSON results")
    run_matrix_parser.set_defaults(func=command_run_matrix)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
