#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from lib.case_loader import CaseLoadError, load_cases
from lib.report import render_json, render_junit, render_perf_json, render_text
from lib.runner import RunnerConfig, run_cases


def repo_version() -> str:
    tests_root = Path(__file__).resolve().parent
    version_file = tests_root.parent / "VERSION"
    return version_file.read_text(encoding="utf-8").strip()


def write_report(path: str, content: str) -> None:
    out_path = Path(path)
    if out_path.parent != Path("."):
        out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(content + ("\n" if content and not content.endswith("\n") else ""), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=f"Nebula v{repo_version()} contract test harness")
    p.add_argument(
        "--suite",
        choices=["all", "check", "build", "run", "test", "bench", "safety"],
        default="all",
        help="case suite to run",
    )
    p.add_argument("--filter", default="*", help="glob pattern for case id")
    p.add_argument(
        "--report",
        choices=["text", "json", "junit"],
        default="text",
        help="output report format",
    )
    p.add_argument("--keep-temp", action="store_true", help="preserve per-case temp sandboxes")
    p.add_argument("--timeout", type=int, default=120, help="per-step timeout seconds")
    p.add_argument("--binary", default="", help="path to nebula binary (default: repo/build/nebula)")
    p.add_argument("--text-out", default="", help="optional path to write text report")
    p.add_argument("--json-out", default="", help="optional path to write JSON report")
    p.add_argument("--junit-out", default="", help="optional path to write JUnit XML report")
    p.add_argument(
        "--perf-json-out",
        default="",
        help="optional path to write non-gating performance summary JSON",
    )
    p.add_argument(
        "--perf-top",
        type=int,
        default=10,
        help="number of slowest cases to include in perf report",
    )
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if args.perf_top < 0:
        sys.stderr.write("error: --perf-top must be >= 0\n")
        return 2

    tests_root = Path(__file__).resolve().parent
    repo_root = tests_root.parent
    binary = Path(args.binary).resolve() if args.binary else (repo_root / "build" / "nebula")

    if not binary.exists():
        sys.stderr.write(
            f"error: nebula binary not found at {binary}\n"
            f"hint: run `cmake -S {repo_root} -B {repo_root / 'build'}` and `cmake --build {repo_root / 'build'} -j`\n"
        )
        return 2

    try:
        cases = load_cases(tests_root / "cases", suite=args.suite, filter_glob=args.filter)
    except CaseLoadError as exc:
        sys.stderr.write(f"error: failed to load cases: {exc}\n")
        return 2

    if not cases:
        sys.stderr.write("error: no cases matched selected suite/filter\n")
        return 2

    cfg = RunnerConfig(binary=binary, tests_root=tests_root, keep_temp=args.keep_temp, timeout_sec=args.timeout)
    results = run_cases(cases, cfg)

    text_report = render_text(results)
    json_report = render_json(results)
    junit_report = render_junit(results)
    perf_report = render_perf_json(results, top_n=args.perf_top)

    if args.text_out:
        write_report(args.text_out, text_report)
    if args.json_out:
        write_report(args.json_out, json_report)
    if args.junit_out:
        write_report(args.junit_out, junit_report)
    if args.perf_json_out:
        write_report(args.perf_json_out, perf_report)

    if args.report == "text":
        print(text_report)
    elif args.report == "json":
        print(json_report)
    else:
        print(junit_report)

    failed = sum(1 for r in results if r["status"] != "passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
