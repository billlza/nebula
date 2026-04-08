#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path
from typing import Any


class PerfDiffError(RuntimeError):
    pass


def _read_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise PerfDiffError(f"file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise PerfDiffError(f"invalid json at {path}: {exc}") from exc

    if not isinstance(data, dict):
        raise PerfDiffError(f"expected object at top-level: {path}")
    return data


def _to_int(value: Any, *, ctx: str) -> int:
    if isinstance(value, bool):
        raise PerfDiffError(f"{ctx}: expected integer, got bool")
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    raise PerfDiffError(f"{ctx}: expected integer, got {type(value).__name__}")


def _to_nonneg_int(value: Any, *, ctx: str) -> int:
    out = _to_int(value, ctx=ctx)
    if out < 0:
        raise PerfDiffError(f"{ctx}: expected >= 0, got {out}")
    return out


def _ensure_dict(value: Any, *, ctx: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PerfDiffError(f"{ctx}: expected object")
    return value


def _ensure_list(value: Any, *, ctx: str) -> list[Any]:
    if not isinstance(value, list):
        raise PerfDiffError(f"{ctx}: expected array")
    return value


def _signal_int(obj: dict[str, Any], key: str, *, ctx: str) -> int:
    if key not in obj:
        return 0
    return _to_nonneg_int(obj.get(key), ctx=f"{ctx}.{key}")


def _validate_report(obj: dict[str, Any], *, label: str) -> dict[str, Any]:
    schema_version = obj.get("schema_version")
    if _to_int(schema_version, ctx=f"{label}.schema_version") != 1:
        raise PerfDiffError(f"{label}.schema_version: only schema_version=1 is supported")

    summary = _ensure_dict(obj.get("summary"), ctx=f"{label}.summary")
    total_duration_ms = _to_nonneg_int(
        summary.get("total_duration_ms"),
        ctx=f"{label}.summary.total_duration_ms",
    )

    by_suite_raw = _ensure_list(obj.get("by_suite"), ctx=f"{label}.by_suite")
    by_suite: dict[str, int] = {}
    for idx, row_raw in enumerate(by_suite_raw):
        row = _ensure_dict(row_raw, ctx=f"{label}.by_suite[{idx}]")
        suite = row.get("suite")
        if not isinstance(suite, str) or not suite:
            raise PerfDiffError(f"{label}.by_suite[{idx}].suite: expected non-empty string")
        if suite in by_suite:
            raise PerfDiffError(f"{label}.by_suite[{idx}].suite: duplicate suite '{suite}'")
        by_suite[suite] = _to_nonneg_int(
            row.get("total_duration_ms"),
            ctx=f"{label}.by_suite[{idx}].total_duration_ms",
        )

    slowest_raw = _ensure_list(obj.get("slowest_cases"), ctx=f"{label}.slowest_cases")
    slowest_cases: dict[str, dict[str, Any]] = {}
    for idx, row_raw in enumerate(slowest_raw):
        row = _ensure_dict(row_raw, ctx=f"{label}.slowest_cases[{idx}]")
        case_id = row.get("id")
        suite = row.get("suite")
        if not isinstance(case_id, str) or not case_id:
            raise PerfDiffError(f"{label}.slowest_cases[{idx}].id: expected non-empty string")
        if not isinstance(suite, str) or not suite:
            raise PerfDiffError(f"{label}.slowest_cases[{idx}].suite: expected non-empty string")
        if case_id in slowest_cases:
            raise PerfDiffError(f"{label}.slowest_cases[{idx}].id: duplicate case id '{case_id}'")
        slowest_cases[case_id] = {
            "id": case_id,
            "suite": suite,
            "duration_ms": _to_nonneg_int(
                row.get("duration_ms"),
                ctx=f"{label}.slowest_cases[{idx}].duration_ms",
            ),
        }

    cross_stage_raw = obj.get("cross_stage_signals")
    if cross_stage_raw is None:
        cross_stage_signals = {
            "cases_with_cache_report": 0,
            "cross_stage_candidates_total": 0,
            "cross_stage_reused_total": 0,
            "cross_stage_saved_ms_estimate_total": 0,
        }
    else:
        cross_stage_obj = _ensure_dict(cross_stage_raw, ctx=f"{label}.cross_stage_signals")
        cross_stage_signals = {
            "cases_with_cache_report": _signal_int(
                cross_stage_obj,
                "cases_with_cache_report",
                ctx=f"{label}.cross_stage_signals",
            ),
            "cross_stage_candidates_total": _signal_int(
                cross_stage_obj,
                "cross_stage_candidates_total",
                ctx=f"{label}.cross_stage_signals",
            ),
            "cross_stage_reused_total": _signal_int(
                cross_stage_obj,
                "cross_stage_reused_total",
                ctx=f"{label}.cross_stage_signals",
            ),
            "cross_stage_saved_ms_estimate_total": _signal_int(
                cross_stage_obj,
                "cross_stage_saved_ms_estimate_total",
                ctx=f"{label}.cross_stage_signals",
            ),
        }

    disk_raw = obj.get("disk_cache_signals")
    if disk_raw is None:
        disk_cache_signals = {
            "cases_with_cache_report": 0,
            "disk_hits_total": 0,
            "disk_misses_total": 0,
            "disk_writes_total": 0,
            "disk_expired_total": 0,
            "disk_evictions_total": 0,
            "disk_entries_peak": 0,
        }
    else:
        disk_obj = _ensure_dict(disk_raw, ctx=f"{label}.disk_cache_signals")
        disk_cache_signals = {
            "cases_with_cache_report": _signal_int(
                disk_obj,
                "cases_with_cache_report",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_hits_total": _signal_int(
                disk_obj,
                "disk_hits_total",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_misses_total": _signal_int(
                disk_obj,
                "disk_misses_total",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_writes_total": _signal_int(
                disk_obj,
                "disk_writes_total",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_expired_total": _signal_int(
                disk_obj,
                "disk_expired_total",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_evictions_total": _signal_int(
                disk_obj,
                "disk_evictions_total",
                ctx=f"{label}.disk_cache_signals",
            ),
            "disk_entries_peak": _signal_int(
                disk_obj,
                "disk_entries_peak",
                ctx=f"{label}.disk_cache_signals",
            ),
        }
        if "disk_entries_peak" not in disk_obj and "disk_entries_max" in disk_obj:
            disk_cache_signals["disk_entries_peak"] = _to_nonneg_int(
                disk_obj.get("disk_entries_max"),
                ctx=f"{label}.disk_cache_signals.disk_entries_max",
            )

    grouped_raw = obj.get("grouped_diagnostic_signals")
    if grouped_raw is None:
        grouped_diagnostic_signals = {
            "cases_with_grouping_summary": 0,
            "grouping_summary_samples_total": 0,
            "grouping_total_ms_total": 0,
            "grouping_index_ms_total": 0,
            "grouping_rank_ms_total": 0,
            "grouping_root_cause_v2_ms_total": 0,
            "grouping_emit_ms_total": 0,
            "grouping_budget_fallback_total": 0,
        }
    else:
        grouped_obj = _ensure_dict(grouped_raw, ctx=f"{label}.grouped_diagnostic_signals")
        grouped_diagnostic_signals = {
            "cases_with_grouping_summary": _signal_int(
                grouped_obj,
                "cases_with_grouping_summary",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_summary_samples_total": _signal_int(
                grouped_obj,
                "grouping_summary_samples_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_total_ms_total": _signal_int(
                grouped_obj,
                "grouping_total_ms_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_index_ms_total": _signal_int(
                grouped_obj,
                "grouping_index_ms_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_rank_ms_total": _signal_int(
                grouped_obj,
                "grouping_rank_ms_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_root_cause_v2_ms_total": _signal_int(
                grouped_obj,
                "grouping_root_cause_v2_ms_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_emit_ms_total": _signal_int(
                grouped_obj,
                "grouping_emit_ms_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
            "grouping_budget_fallback_total": _signal_int(
                grouped_obj,
                "grouping_budget_fallback_total",
                ctx=f"{label}.grouped_diagnostic_signals",
            ),
        }

    return {
        "schema_version": 1,
        "total_duration_ms": total_duration_ms,
        "by_suite": by_suite,
        "slowest_cases": slowest_cases,
        "cross_stage_signals": cross_stage_signals,
        "disk_cache_signals": disk_cache_signals,
        "grouped_diagnostic_signals": grouped_diagnostic_signals,
    }


def _pct_delta(current: int, baseline: int) -> float | None:
    if baseline == 0:
        return None
    return round(((current - baseline) * 100.0) / baseline, 3)


def _mode_on(value: str) -> bool:
    return value.strip().lower() == "on"


def _write_text(path: str, content: str) -> None:
    out_path = Path(path)
    if out_path.parent != Path("."):
        out_path.parent.mkdir(parents=True, exist_ok=True)
    text = content if content.endswith("\n") else content + "\n"
    out_path.write_text(text, encoding="utf-8")


def _format_pct(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.3f}%"


def _render_markdown(report: dict[str, Any]) -> str:
    summary = _ensure_dict(report["summary"], ctx="render.summary")
    judgement = _ensure_dict(report["judgement"], ctx="render.judgement")
    cross = _ensure_dict(report["cross_stage_signal_deltas"], ctx="render.cross_stage_signal_deltas")
    disk = _ensure_dict(report["disk_cache_signal_deltas"], ctx="render.disk_cache_signal_deltas")
    grouped = _ensure_dict(
        report["grouped_diagnostic_signal_deltas"],
        ctx="render.grouped_diagnostic_signal_deltas",
    )

    lines: list[str] = []
    lines.append("# Nebula Perf Baseline Diff")
    lines.append("")
    lines.append(f"- status: `{judgement['status']}`")
    lines.append(f"- generated_at_utc: `{report['generated_at_utc']}`")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(f"- baseline_total_duration_ms: `{summary['baseline_total_duration_ms']}`")
    lines.append(f"- current_total_duration_ms: `{summary['current_total_duration_ms']}`")
    lines.append(f"- total_duration_delta_ms: `{summary['total_duration_delta_ms']}`")
    lines.append(f"- total_duration_delta_pct: `{_format_pct(summary['total_duration_delta_pct'])}`")
    lines.append("")
    lines.append("## Cache Signals")
    lines.append("")
    lines.append(
        "- cross_stage_reused_total: `{}` -> `{}` (delta `{}`)".format(
            cross["baseline"]["cross_stage_reused_total"],
            cross["current"]["cross_stage_reused_total"],
            cross["delta"]["cross_stage_reused_total"],
        )
    )
    lines.append(
        "- cross_stage_saved_ms_estimate_total: `{}` -> `{}` (delta `{}`)".format(
            cross["baseline"]["cross_stage_saved_ms_estimate_total"],
            cross["current"]["cross_stage_saved_ms_estimate_total"],
            cross["delta"]["cross_stage_saved_ms_estimate_total"],
        )
    )
    lines.append(
        "- disk_hits_total: `{}` -> `{}` (delta `{}`)".format(
            disk["baseline"]["disk_hits_total"],
            disk["current"]["disk_hits_total"],
            disk["delta"]["disk_hits_total"],
        )
    )
    lines.append(
        "- disk_misses_total: `{}` -> `{}` (delta `{}`)".format(
            disk["baseline"]["disk_misses_total"],
            disk["current"]["disk_misses_total"],
            disk["delta"]["disk_misses_total"],
        )
    )
    lines.append(
        "- disk_evictions_total: `{}` -> `{}` (delta `{}`)".format(
            disk["baseline"]["disk_evictions_total"],
            disk["current"]["disk_evictions_total"],
            disk["delta"]["disk_evictions_total"],
        )
    )
    lines.append("")
    lines.append("## Grouped Diagnostic Signals")
    lines.append("")
    lines.append(
        "- grouping_total_ms_total: `{}` -> `{}` (delta `{}`)".format(
            grouped["baseline"]["grouping_total_ms_total"],
            grouped["current"]["grouping_total_ms_total"],
            grouped["delta"]["grouping_total_ms_total"],
        )
    )
    lines.append(
        "- grouping_budget_fallback_total: `{}` -> `{}` (delta `{}`)".format(
            grouped["baseline"]["grouping_budget_fallback_total"],
            grouped["current"]["grouping_budget_fallback_total"],
            grouped["delta"]["grouping_budget_fallback_total"],
        )
    )
    lines.append("")

    reasons = judgement.get("reasons", [])
    lines.append("## Judgement")
    lines.append("")
    if reasons:
        for reason in reasons:
            lines.append(f"- {reason}")
    else:
        lines.append("- no regression threshold exceeded")

    lines.append("")
    lines.append("## Top Regressions")
    lines.append("")
    lines.append("| id | suite | baseline_ms | current_ms | delta_ms | delta_pct |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: |")
    for row in report.get("case_regressions", []):
        lines.append(
            "| {id} | {suite} | {baseline_duration_ms} | {current_duration_ms} | {delta_ms} | {delta_pct} |".format(
                id=row["id"],
                suite=row["suite"],
                baseline_duration_ms=row["baseline_duration_ms"],
                current_duration_ms=row["current_duration_ms"],
                delta_ms=row["delta_ms"],
                delta_pct=_format_pct(row["delta_pct"]),
            )
        )
    if not report.get("case_regressions"):
        lines.append("| (none) | - | - | - | - | - |")

    lines.append("")
    lines.append("## Top Improvements")
    lines.append("")
    lines.append("| id | suite | baseline_ms | current_ms | delta_ms | delta_pct |")
    lines.append("| --- | --- | ---: | ---: | ---: | ---: |")
    for row in report.get("case_improvements", []):
        lines.append(
            "| {id} | {suite} | {baseline_duration_ms} | {current_duration_ms} | {delta_ms} | {delta_pct} |".format(
                id=row["id"],
                suite=row["suite"],
                baseline_duration_ms=row["baseline_duration_ms"],
                current_duration_ms=row["current_duration_ms"],
                delta_ms=row["delta_ms"],
                delta_pct=_format_pct(row["delta_pct"]),
            )
        )
    if not report.get("case_improvements"):
        lines.append("| (none) | - | - | - | - | - |")

    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Nebula performance baseline diff")
    p.add_argument("--current", required=True, help="current perf JSON generated by tests/run.py --perf-json-out")
    p.add_argument("--baseline", required=True, help="baseline perf JSON for comparison")
    p.add_argument("--out-json", default="", help="optional path to write machine-readable diff JSON")
    p.add_argument("--out-md", default="", help="optional path to write markdown summary")
    p.add_argument("--fail-on-regression", choices=["on", "off"], default="off")
    p.add_argument("--max-total-regression-pct", type=float, default=15.0)
    p.add_argument("--max-suite-regression-pct", type=float, default=20.0)
    p.add_argument("--max-case-regression-ms", type=int, default=250)
    p.add_argument("--min-case-duration-ms", type=int, default=200)
    p.add_argument("--top-regressions", type=int, default=10)
    p.add_argument("--top-improvements", type=int, default=10)
    p.add_argument("--max-cross-stage-reused-drop", type=int, default=2)
    p.add_argument("--max-cross-stage-saved-ms-drop", type=int, default=200)
    p.add_argument("--max-disk-hit-drop", type=int, default=2)
    p.add_argument("--max-disk-miss-increase", type=int, default=10)
    p.add_argument("--max-disk-eviction-increase", type=int, default=5)
    p.add_argument("--max-grouping-total-ms-increase", type=int, default=400)
    p.add_argument("--max-grouping-budget-fallback-increase", type=int, default=0)
    return p.parse_args()


def main() -> int:
    args = _parse_args()

    if args.max_total_regression_pct < 0:
        sys.stderr.write("error: --max-total-regression-pct must be >= 0\n")
        return 2
    if args.max_suite_regression_pct < 0:
        sys.stderr.write("error: --max-suite-regression-pct must be >= 0\n")
        return 2
    if args.max_case_regression_ms < 0:
        sys.stderr.write("error: --max-case-regression-ms must be >= 0\n")
        return 2
    if args.min_case_duration_ms < 0:
        sys.stderr.write("error: --min-case-duration-ms must be >= 0\n")
        return 2
    if args.top_regressions < 0:
        sys.stderr.write("error: --top-regressions must be >= 0\n")
        return 2
    if args.top_improvements < 0:
        sys.stderr.write("error: --top-improvements must be >= 0\n")
        return 2
    if args.max_cross_stage_reused_drop < 0:
        sys.stderr.write("error: --max-cross-stage-reused-drop must be >= 0\n")
        return 2
    if args.max_cross_stage_saved_ms_drop < 0:
        sys.stderr.write("error: --max-cross-stage-saved-ms-drop must be >= 0\n")
        return 2
    if args.max_disk_hit_drop < 0:
        sys.stderr.write("error: --max-disk-hit-drop must be >= 0\n")
        return 2
    if args.max_disk_miss_increase < 0:
        sys.stderr.write("error: --max-disk-miss-increase must be >= 0\n")
        return 2
    if args.max_disk_eviction_increase < 0:
        sys.stderr.write("error: --max-disk-eviction-increase must be >= 0\n")
        return 2
    if args.max_grouping_total_ms_increase < 0:
        sys.stderr.write("error: --max-grouping-total-ms-increase must be >= 0\n")
        return 2
    if args.max_grouping_budget_fallback_increase < 0:
        sys.stderr.write("error: --max-grouping-budget-fallback-increase must be >= 0\n")
        return 2

    try:
        baseline_obj = _read_json(Path(args.baseline))
        current_obj = _read_json(Path(args.current))
        baseline = _validate_report(baseline_obj, label="baseline")
        current = _validate_report(current_obj, label="current")
    except PerfDiffError as exc:
        sys.stderr.write(f"error: invalid perf report: {exc}\n")
        return 2

    baseline_total = int(baseline["total_duration_ms"])
    current_total = int(current["total_duration_ms"])
    total_delta_ms = current_total - baseline_total
    total_delta_pct = _pct_delta(current_total, baseline_total)

    suites = sorted(set(baseline["by_suite"].keys()) | set(current["by_suite"].keys()))
    suite_deltas: list[dict[str, Any]] = []
    for suite in suites:
        b = int(baseline["by_suite"].get(suite, 0))
        c = int(current["by_suite"].get(suite, 0))
        suite_deltas.append(
            {
                "suite": suite,
                "baseline_total_duration_ms": b,
                "current_total_duration_ms": c,
                "delta_ms": c - b,
                "delta_pct": _pct_delta(c, b),
            }
        )

    baseline_cases = baseline["slowest_cases"]
    current_cases = current["slowest_cases"]
    shared_ids = sorted(set(baseline_cases.keys()) & set(current_cases.keys()))

    case_deltas: list[dict[str, Any]] = []
    for cid in shared_ids:
        b_row = baseline_cases[cid]
        c_row = current_cases[cid]
        b = int(b_row["duration_ms"])
        c = int(c_row["duration_ms"])
        case_deltas.append(
            {
                "id": cid,
                "suite": str(c_row.get("suite") or b_row.get("suite") or "unknown"),
                "baseline_duration_ms": b,
                "current_duration_ms": c,
                "delta_ms": c - b,
                "delta_pct": _pct_delta(c, b),
                "eligible": b >= args.min_case_duration_ms,
            }
        )

    case_regressions_all = [x for x in case_deltas if x["eligible"] and x["delta_ms"] > 0]
    case_improvements_all = [x for x in case_deltas if x["eligible"] and x["delta_ms"] < 0]

    case_regressions_all.sort(key=lambda x: (x["delta_ms"], x["id"]), reverse=True)
    case_improvements_all.sort(key=lambda x: (x["delta_ms"], x["id"]))

    case_regressions = [dict(x) for x in case_regressions_all[: args.top_regressions]]
    case_improvements = [dict(x) for x in case_improvements_all[: args.top_improvements]]
    for rows in (case_regressions, case_improvements):
        for row in rows:
            row.pop("eligible", None)

    b_cross = baseline["cross_stage_signals"]
    c_cross = current["cross_stage_signals"]
    cross_delta = {
        "cases_with_cache_report": int(c_cross["cases_with_cache_report"]) - int(b_cross["cases_with_cache_report"]),
        "cross_stage_candidates_total": int(c_cross["cross_stage_candidates_total"]) - int(b_cross["cross_stage_candidates_total"]),
        "cross_stage_reused_total": int(c_cross["cross_stage_reused_total"]) - int(b_cross["cross_stage_reused_total"]),
        "cross_stage_saved_ms_estimate_total": int(c_cross["cross_stage_saved_ms_estimate_total"])
        - int(b_cross["cross_stage_saved_ms_estimate_total"]),
    }

    b_disk = baseline["disk_cache_signals"]
    c_disk = current["disk_cache_signals"]
    disk_delta = {
        "cases_with_cache_report": int(c_disk["cases_with_cache_report"]) - int(b_disk["cases_with_cache_report"]),
        "disk_hits_total": int(c_disk["disk_hits_total"]) - int(b_disk["disk_hits_total"]),
        "disk_misses_total": int(c_disk["disk_misses_total"]) - int(b_disk["disk_misses_total"]),
        "disk_writes_total": int(c_disk["disk_writes_total"]) - int(b_disk["disk_writes_total"]),
        "disk_expired_total": int(c_disk["disk_expired_total"]) - int(b_disk["disk_expired_total"]),
        "disk_evictions_total": int(c_disk["disk_evictions_total"]) - int(b_disk["disk_evictions_total"]),
        "disk_entries_peak": int(c_disk["disk_entries_peak"]) - int(b_disk["disk_entries_peak"]),
    }

    b_grouped = baseline["grouped_diagnostic_signals"]
    c_grouped = current["grouped_diagnostic_signals"]
    grouped_delta = {
        "cases_with_grouping_summary": int(c_grouped["cases_with_grouping_summary"])
        - int(b_grouped["cases_with_grouping_summary"]),
        "grouping_summary_samples_total": int(c_grouped["grouping_summary_samples_total"])
        - int(b_grouped["grouping_summary_samples_total"]),
        "grouping_total_ms_total": int(c_grouped["grouping_total_ms_total"])
        - int(b_grouped["grouping_total_ms_total"]),
        "grouping_index_ms_total": int(c_grouped["grouping_index_ms_total"])
        - int(b_grouped["grouping_index_ms_total"]),
        "grouping_rank_ms_total": int(c_grouped["grouping_rank_ms_total"])
        - int(b_grouped["grouping_rank_ms_total"]),
        "grouping_root_cause_v2_ms_total": int(c_grouped["grouping_root_cause_v2_ms_total"])
        - int(b_grouped["grouping_root_cause_v2_ms_total"]),
        "grouping_emit_ms_total": int(c_grouped["grouping_emit_ms_total"])
        - int(b_grouped["grouping_emit_ms_total"]),
        "grouping_budget_fallback_total": int(c_grouped["grouping_budget_fallback_total"])
        - int(b_grouped["grouping_budget_fallback_total"]),
    }

    reasons: list[str] = []
    if total_delta_pct is not None and total_delta_pct > args.max_total_regression_pct:
        reasons.append(
            "total_duration_regression_pct {:.3f}% exceeds threshold {:.3f}%".format(
                total_delta_pct,
                args.max_total_regression_pct,
            )
        )

    suite_regressions_over_threshold = [
        s
        for s in suite_deltas
        if s["delta_ms"] > 0
        and s["delta_pct"] is not None
        and float(s["delta_pct"]) > args.max_suite_regression_pct
    ]
    for row in suite_regressions_over_threshold:
        reasons.append(
            "suite '{}' regression {:.3f}% exceeds threshold {:.3f}%".format(
                row["suite"],
                float(row["delta_pct"]),
                args.max_suite_regression_pct,
            )
        )

    case_regressions_over_threshold = [x for x in case_regressions_all if x["delta_ms"] > args.max_case_regression_ms]
    for row in case_regressions_over_threshold:
        reasons.append(
            "case '{}' regression {}ms exceeds threshold {}ms".format(
                row["id"],
                row["delta_ms"],
                args.max_case_regression_ms,
            )
        )

    cross_reused_drop = max(0, int(b_cross["cross_stage_reused_total"]) - int(c_cross["cross_stage_reused_total"]))
    if cross_reused_drop > args.max_cross_stage_reused_drop:
        reasons.append(
            "cross_stage_reused_drop {} exceeds threshold {}".format(
                cross_reused_drop,
                args.max_cross_stage_reused_drop,
            )
        )

    cross_saved_drop = max(
        0,
        int(b_cross["cross_stage_saved_ms_estimate_total"]) - int(c_cross["cross_stage_saved_ms_estimate_total"]),
    )
    if cross_saved_drop > args.max_cross_stage_saved_ms_drop:
        reasons.append(
            "cross_stage_saved_ms_drop {} exceeds threshold {}".format(
                cross_saved_drop,
                args.max_cross_stage_saved_ms_drop,
            )
        )

    disk_hit_drop = max(0, int(b_disk["disk_hits_total"]) - int(c_disk["disk_hits_total"]))
    if disk_hit_drop > args.max_disk_hit_drop:
        reasons.append(
            "disk_hit_drop {} exceeds threshold {}".format(
                disk_hit_drop,
                args.max_disk_hit_drop,
            )
        )

    disk_miss_increase = max(0, int(c_disk["disk_misses_total"]) - int(b_disk["disk_misses_total"]))
    if disk_miss_increase > args.max_disk_miss_increase:
        reasons.append(
            "disk_miss_increase {} exceeds threshold {}".format(
                disk_miss_increase,
                args.max_disk_miss_increase,
            )
        )

    disk_eviction_increase = max(0, int(c_disk["disk_evictions_total"]) - int(b_disk["disk_evictions_total"]))
    if disk_eviction_increase > args.max_disk_eviction_increase:
        reasons.append(
            "disk_eviction_increase {} exceeds threshold {}".format(
                disk_eviction_increase,
                args.max_disk_eviction_increase,
            )
        )

    grouping_total_ms_increase = max(
        0,
        int(c_grouped["grouping_total_ms_total"]) - int(b_grouped["grouping_total_ms_total"]),
    )
    if grouping_total_ms_increase > args.max_grouping_total_ms_increase:
        reasons.append(
            "grouping_total_ms_increase {} exceeds threshold {}".format(
                grouping_total_ms_increase,
                args.max_grouping_total_ms_increase,
            )
        )

    grouping_budget_fallback_increase = max(
        0,
        int(c_grouped["grouping_budget_fallback_total"]) - int(b_grouped["grouping_budget_fallback_total"]),
    )
    if grouping_budget_fallback_increase > args.max_grouping_budget_fallback_increase:
        reasons.append(
            "grouping_budget_fallback_increase {} exceeds threshold {}".format(
                grouping_budget_fallback_increase,
                args.max_grouping_budget_fallback_increase,
            )
        )

    fail_on_regression = _mode_on(args.fail_on_regression)
    if reasons:
        status = "fail" if fail_on_regression else "warn"
    else:
        status = "pass"

    report: dict[str, Any] = {
        "schema_version": 1,
        "generated_at_utc": dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "current": str(Path(args.current)),
        "baseline": str(Path(args.baseline)),
        "thresholds": {
            "fail_on_regression": fail_on_regression,
            "max_total_regression_pct": args.max_total_regression_pct,
            "max_suite_regression_pct": args.max_suite_regression_pct,
            "max_case_regression_ms": args.max_case_regression_ms,
            "min_case_duration_ms": args.min_case_duration_ms,
            "top_regressions": args.top_regressions,
            "top_improvements": args.top_improvements,
            "max_cross_stage_reused_drop": args.max_cross_stage_reused_drop,
            "max_cross_stage_saved_ms_drop": args.max_cross_stage_saved_ms_drop,
            "max_disk_hit_drop": args.max_disk_hit_drop,
            "max_disk_miss_increase": args.max_disk_miss_increase,
            "max_disk_eviction_increase": args.max_disk_eviction_increase,
            "max_grouping_total_ms_increase": args.max_grouping_total_ms_increase,
            "max_grouping_budget_fallback_increase": args.max_grouping_budget_fallback_increase,
        },
        "summary": {
            "baseline_total_duration_ms": baseline_total,
            "current_total_duration_ms": current_total,
            "total_duration_delta_ms": total_delta_ms,
            "total_duration_delta_pct": total_delta_pct,
            "suite_count_baseline": len(baseline["by_suite"]),
            "suite_count_current": len(current["by_suite"]),
            "shared_case_count": len(shared_ids),
        },
        "suite_deltas": suite_deltas,
        "case_regressions": case_regressions,
        "case_improvements": case_improvements,
        "cross_stage_signal_deltas": {
            "baseline": b_cross,
            "current": c_cross,
            "delta": cross_delta,
        },
        "disk_cache_signal_deltas": {
            "baseline": b_disk,
            "current": c_disk,
            "delta": disk_delta,
        },
        "grouped_diagnostic_signal_deltas": {
            "baseline": b_grouped,
            "current": c_grouped,
            "delta": grouped_delta,
        },
        "judgement": {
            "status": status,
            "reasons": reasons,
        },
    }

    print(
        "perf-baseline-diff: status={status} fail_on_regression={fail} shared_cases={cases}".format(
            status=status,
            fail="on" if fail_on_regression else "off",
            cases=len(shared_ids),
        )
    )
    print(
        "summary: baseline_total_ms={b} current_total_ms={c} delta_ms={d} delta_pct={p}".format(
            b=baseline_total,
            c=current_total,
            d=total_delta_ms,
            p=_format_pct(total_delta_pct),
        )
    )
    print(
        "cross-stage: baseline_reused={br} current_reused={cr} baseline_saved_ms={bs} current_saved_ms={cs}".format(
            br=b_cross["cross_stage_reused_total"],
            cr=c_cross["cross_stage_reused_total"],
            bs=b_cross["cross_stage_saved_ms_estimate_total"],
            cs=c_cross["cross_stage_saved_ms_estimate_total"],
        )
    )
    print(
        "disk-cache: baseline_hits={bh} current_hits={ch} baseline_misses={bm} current_misses={cm} baseline_evictions={be} current_evictions={ce}".format(
            bh=b_disk["disk_hits_total"],
            ch=c_disk["disk_hits_total"],
            bm=b_disk["disk_misses_total"],
            cm=c_disk["disk_misses_total"],
            be=b_disk["disk_evictions_total"],
            ce=c_disk["disk_evictions_total"],
        )
    )
    print(
        "grouped-diag: baseline_total_ms={bt} current_total_ms={ct} baseline_budget_fallbacks={bb} current_budget_fallbacks={cb}".format(
            bt=b_grouped["grouping_total_ms_total"],
            ct=c_grouped["grouping_total_ms_total"],
            bb=b_grouped["grouping_budget_fallback_total"],
            cb=c_grouped["grouping_budget_fallback_total"],
        )
    )
    if reasons:
        print("regression-reasons:")
        for reason in reasons:
            print(f"- {reason}")
    else:
        print("regression-reasons: none")

    if args.out_json:
        _write_text(args.out_json, json.dumps(report, indent=2, ensure_ascii=True))
        print(f"wrote-json: {args.out_json}")

    if args.out_md:
        _write_text(args.out_md, _render_markdown(report))
        print(f"wrote-md: {args.out_md}")

    if status == "fail":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
