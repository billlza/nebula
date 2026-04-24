from __future__ import annotations

import time
from pathlib import Path
from typing import Any

from .assertions import evaluate_step_assertions
from .fs_sandbox import cleanup_case_sandbox, make_case_sandbox
from .nebula_invoker import run_step


class RunnerConfig:
    def __init__(self, binary: Path, tests_root: Path, keep_temp: bool, timeout_sec: int = 120):
        self.binary = binary
        self.tests_root = tests_root
        self.keep_temp = keep_temp
        self.timeout_sec = timeout_sec


def run_cases(cases: list[dict[str, Any]], cfg: RunnerConfig) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []

    for case in cases:
        t0 = time.perf_counter()
        sandbox = make_case_sandbox(case["id"], cfg.tests_root)
        status = "passed"
        fail_reason = ""
        matched_assertions = 0
        budget_warning_count = 0
        case_rc = 0
        all_output_parts: list[str] = []

        step_reports: list[dict[str, Any]] = []

        try:
            for idx, step in enumerate(case["steps"], start=1):
                step_timeout = int(step.get("timeout", cfg.timeout_sec))
                step_result = run_step(
                    step,
                    cfg.binary,
                    sandbox,
                    timeout_sec=step_timeout,
                    extra_env={
                        "NEBULA_BINARY": str(cfg.binary),
                        "NEBULA_REPO_ROOT": str(cfg.tests_root.parent),
                        "NEBULA_TESTS_ROOT": str(cfg.tests_root),
                    },
                )
                all_output_parts.append(
                    f"[step {idx}] {step_result['cmd_str']}\n{step_result['output']}"
                )

                assertion = evaluate_step_assertions(step, step_result, sandbox)
                case_rc = int(step_result["rc"])
                matched_assertions += assertion["matched_assertions"]
                budget_warning_count += assertion["budget_warning_count"]

                step_reports.append(
                    {
                        "index": idx,
                        "cmd": step_result["cmd_str"],
                        "rc": step_result["rc"],
                        "duration_ms": step_result["duration_ms"],
                        "timed_out": step_result.get("timed_out", False),
                        "ok": assertion["ok"],
                        "fail_reason": assertion["fail_reason"],
                        "diag_count": assertion["diag_count"],
                        "budget_warning_count": assertion["budget_warning_count"],
                    }
                )

                if not assertion["ok"]:
                    status = "failed"
                    fail_reason = f"step {idx}: {assertion['fail_reason']}"
                    break
        except Exception as exc:  # pragma: no cover
            status = "failed"
            fail_reason = f"exception: {exc}"

        duration_ms = int((time.perf_counter() - t0) * 1000)

        result = {
            "id": case["id"],
            "suite": case["suite"],
            "status": status,
            "duration_ms": duration_ms,
            "rc": case_rc,
            "matched_assertions": matched_assertions,
            "budget_warning_count": budget_warning_count,
            "fail_reason": fail_reason,
            "sandbox": str(sandbox) if cfg.keep_temp or status == "failed" else "",
            "steps": step_reports,
            "output": "\n".join(all_output_parts),
        }
        results.append(result)

        if not cfg.keep_temp and status == "passed":
            cleanup_case_sandbox(sandbox)

    return results
