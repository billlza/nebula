from __future__ import annotations

import os
import sys
import time
from pathlib import Path
from typing import Any

from .assertions import evaluate_step_assertions
from .fs_sandbox import SandboxCreationError, cleanup_case_sandbox, make_case_sandbox
from .nebula_invoker import INFRASTRUCTURE_ERROR_RETURN_CODE, run_step


class RunnerConfig:
    def __init__(self, binary: Path, tests_root: Path, keep_temp: bool, timeout_sec: int = 120):
        self.binary = binary
        self.tests_root = tests_root
        self.keep_temp = keep_temp
        self.timeout_sec = timeout_sec


def _python_shim_dir(sandbox: Path) -> Path:
    shim_dir = sandbox / ".nebula-test-python"
    shim_dir.mkdir(parents=True, exist_ok=True)
    python = Path(sys.executable).resolve()
    if os.name == "nt":
        for name in ("python.cmd", "python3.cmd"):
            (shim_dir / name).write_text(f'@"{python}" %*\r\n', encoding="utf-8")
        return shim_dir

    for name in ("python", "python3"):
        target = shim_dir / name
        if target.exists() or target.is_symlink():
            target.unlink()
        target.symlink_to(python)
    return shim_dir


def run_cases(cases: list[dict[str, Any]], cfg: RunnerConfig) -> list[dict[str, Any]]:
    results: list[dict[str, Any]] = []
    abort_suite = False

    for case in cases:
        t0 = time.perf_counter()
        sandbox: Path | None = None
        status = "passed"
        fail_reason = ""
        matched_assertions = 0
        budget_warning_count = 0
        case_rc = 0
        all_output_parts: list[str] = []
        infrastructure_error = ""

        step_reports: list[dict[str, Any]] = []

        try:
            sandbox = make_case_sandbox(case["id"], cfg.tests_root)
            python_shim = _python_shim_dir(sandbox)
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
                        "NEBULA_TEST_PYTHON": sys.executable,
                        "PYTHON": sys.executable,
                        "PATH": os.pathsep.join([str(python_shim), os.environ.get("PATH", "")]),
                    },
                )
                all_output_parts.append(
                    f"[step {idx}] {step_result['cmd_str']}\n{step_result['output']}"
                )

                case_rc = int(step_result["rc"])
                step_infrastructure_error = str(
                    step_result.get("infrastructure_error", "")
                )
                if step_infrastructure_error:
                    status = "failed"
                    infrastructure_error = step_infrastructure_error
                    fail_reason = (
                        f"step {idx}: test infrastructure failure: "
                        f"{step_infrastructure_error}"
                    )
                    step_reports.append(
                        {
                            "index": idx,
                            "cmd": step_result["cmd_str"],
                            "rc": step_result["rc"],
                            "duration_ms": step_result["duration_ms"],
                            "timed_out": step_result.get("timed_out", False),
                            "ok": False,
                            "fail_reason": fail_reason,
                            "infrastructure_error": step_infrastructure_error,
                            "diag_count": 0,
                            "budget_warning_count": 0,
                        }
                    )
                    abort_suite = True
                    break

                assertion = evaluate_step_assertions(step, step_result, sandbox)
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
                        "infrastructure_error": "",
                        "diag_count": assertion["diag_count"],
                        "budget_warning_count": assertion["budget_warning_count"],
                    }
                )

                if not assertion["ok"]:
                    status = "failed"
                    fail_reason = f"step {idx}: {assertion['fail_reason']}"
                    break
        except BaseException as exc:  # pragma: no cover - exercised through nested runner probes
            if not isinstance(exc, Exception):
                if sandbox is not None and not cfg.keep_temp:
                    try:
                        cleanup_case_sandbox(sandbox)
                    except OSError as cleanup_exc:
                        exc.add_note(
                            "case sandbox cleanup failed after cancellation: "
                            f"{cleanup_exc}; path={sandbox}"
                        )
                raise
            if (
                sandbox is None
                and isinstance(exc, SandboxCreationError)
                and exc.partial_path is not None
                and exc.partial_path.exists()
            ):
                sandbox = exc.partial_path
            status = "failed"
            case_rc = INFRASTRUCTURE_ERROR_RETURN_CODE
            infrastructure_error = f"{type(exc).__name__}: {exc}"
            fail_reason = f"test runner infrastructure exception: {infrastructure_error}"
            all_output_parts.append(
                f"[nebula-test-infrastructure] {infrastructure_error}\n"
            )
            abort_suite = True

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
            "infrastructure_error": infrastructure_error,
            "sandbox": (
                str(sandbox)
                if sandbox is not None and (cfg.keep_temp or status == "failed")
                else ""
            ),
            "steps": step_reports,
            "output": "\n".join(all_output_parts),
        }

        if not cfg.keep_temp and status == "passed" and sandbox is not None:
            try:
                cleanup_case_sandbox(sandbox)
            except OSError as exc:
                result["status"] = "failed"
                result["rc"] = INFRASTRUCTURE_ERROR_RETURN_CODE
                result["infrastructure_error"] = f"sandbox cleanup failed: {exc}"
                result["fail_reason"] = result["infrastructure_error"]
                result["sandbox"] = str(sandbox)
                abort_suite = True

        results.append(result)
        if abort_suite:
            break

    return results
