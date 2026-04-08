from __future__ import annotations

import datetime as dt
import json
import re
from xml.etree import ElementTree as ET
from typing import Any


def render_text(results: list[dict[str, Any]]) -> str:
    total = len(results)
    passed = sum(1 for r in results if r["status"] == "passed")
    failed = total - passed
    budget_warn_total = sum(int(r.get("budget_warning_count", 0)) for r in results)

    lines: list[str] = []
    lines.append(
        f"cases: {total} passed: {passed} failed: {failed} budget_warnings: {budget_warn_total}"
    )
    for r in results:
        prefix = "PASS" if r["status"] == "passed" else "FAIL"
        msg = f"{prefix} {r['id']} ({r['duration_ms']}ms)"
        if r.get("fail_reason"):
            msg += f" :: {r['fail_reason']}"
        if r.get("budget_warning_count"):
            msg += f" :: budget_warnings={r['budget_warning_count']}"
        if r.get("sandbox"):
            msg += f" :: sandbox={r['sandbox']}"
        lines.append(msg)
    return "\n".join(lines)


def render_json(results: list[dict[str, Any]]) -> str:
    return json.dumps(results, indent=2, ensure_ascii=True)


def render_junit(results: list[dict[str, Any]]) -> str:
    suite = ET.Element("testsuite")
    suite.set("name", "nebula-contract-tests")
    suite.set("tests", str(len(results)))
    suite.set("failures", str(sum(1 for r in results if r["status"] != "passed")))

    for r in results:
        tc = ET.SubElement(suite, "testcase")
        tc.set("name", r["id"])
        tc.set("time", f"{r['duration_ms'] / 1000.0:.3f}")
        if r["status"] != "passed":
            fail = ET.SubElement(tc, "failure")
            fail.set("message", r.get("fail_reason") or "assertion failed")
            fail.text = r.get("output", "")

    return ET.tostring(suite, encoding="unicode")


def _to_int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _extract_cache_reports(output: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for line in output.splitlines():
        start = line.find('{"cache_report":{')
        if start < 0:
            continue
        payload = line[start:].strip()
        try:
            loaded = json.loads(payload)
        except json.JSONDecodeError:
            continue
        if not isinstance(loaded, dict):
            continue
        obj = loaded
        cache = obj.get("cache_report")
        if isinstance(cache, dict):
            out.append(cache)
    return out


_GROUPING_JSON_RE = re.compile(
    r'"grouping_total_ms"\s*:\s*(\d+)'
    r'.*"grouping_index_ms"\s*:\s*(\d+)'
    r'.*"grouping_rank_ms"\s*:\s*(\d+)'
    r'.*"grouping_root_cause_v2_ms"\s*:\s*(\d+)'
    r'.*"grouping_emit_ms"\s*:\s*(\d+)'
    r'.*"grouping_budget_fallback"\s*:\s*(true|false)'
)
_GROUPING_TEXT_RE = re.compile(
    r"grouping-perf:\s+total-ms=(\d+)\s+index-ms=(\d+)\s+rank-ms=(\d+)\s+"
    r"root-cause-v2-ms=(\d+)\s+emit-ms=(\d+)\s+budget-fallback=(on|off)"
)


def _extract_grouping_reports(output: str) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for line in output.splitlines():
        m_json = _GROUPING_JSON_RE.search(line)
        if m_json:
            out.append(
                {
                    "grouping_total_ms": _to_int(m_json.group(1)),
                    "grouping_index_ms": _to_int(m_json.group(2)),
                    "grouping_rank_ms": _to_int(m_json.group(3)),
                    "grouping_root_cause_v2_ms": _to_int(m_json.group(4)),
                    "grouping_emit_ms": _to_int(m_json.group(5)),
                    "grouping_budget_fallback": m_json.group(6) == "true",
                }
            )
            continue

        m_text = _GROUPING_TEXT_RE.search(line)
        if m_text:
            out.append(
                {
                    "grouping_total_ms": _to_int(m_text.group(1)),
                    "grouping_index_ms": _to_int(m_text.group(2)),
                    "grouping_rank_ms": _to_int(m_text.group(3)),
                    "grouping_root_cause_v2_ms": _to_int(m_text.group(4)),
                    "grouping_emit_ms": _to_int(m_text.group(5)),
                    "grouping_budget_fallback": m_text.group(6) == "on",
                }
            )
    return out


def render_perf_json(results: list[dict[str, Any]], top_n: int = 10) -> str:
    total = len(results)
    passed = sum(1 for r in results if r.get("status") == "passed")
    failed = total - passed
    total_duration_ms = sum(_to_int(r.get("duration_ms")) for r in results)
    mean_duration_ms = round(total_duration_ms / total, 3) if total else 0.0

    by_suite_map: dict[str, dict[str, Any]] = {}
    for r in results:
        suite = str(r.get("suite", "unknown"))
        bucket = by_suite_map.setdefault(
            suite,
            {
                "suite": suite,
                "cases": 0,
                "passed": 0,
                "failed": 0,
                "total_duration_ms": 0,
            },
        )
        bucket["cases"] += 1
        if r.get("status") == "passed":
            bucket["passed"] += 1
        else:
            bucket["failed"] += 1
        bucket["total_duration_ms"] += _to_int(r.get("duration_ms"))

    by_suite: list[dict[str, Any]] = []
    for suite in sorted(by_suite_map.keys()):
        row = by_suite_map[suite]
        cases = int(row["cases"])
        row["mean_duration_ms"] = round(row["total_duration_ms"] / cases, 3) if cases else 0.0
        by_suite.append(row)

    sorted_cases = sorted(results, key=lambda r: _to_int(r.get("duration_ms")), reverse=True)
    slowest_cases = [
        {
            "id": str(r.get("id")),
            "suite": str(r.get("suite")),
            "status": str(r.get("status")),
            "duration_ms": _to_int(r.get("duration_ms")),
        }
        for r in sorted_cases[: max(0, top_n)]
    ]

    cross_stage_by_case: list[dict[str, Any]] = []
    for r in results:
        reports = _extract_cache_reports(str(r.get("output", "")))
        if not reports:
            continue
        candidates = sum(_to_int(c.get("cross_stage_candidates")) for c in reports)
        reused = sum(_to_int(c.get("cross_stage_reused")) for c in reports)
        saved = sum(_to_int(c.get("cross_stage_saved_ms_estimate")) for c in reports)
        disk_hits = sum(_to_int(c.get("disk_hits")) for c in reports)
        disk_misses = sum(_to_int(c.get("disk_misses")) for c in reports)
        disk_writes = sum(_to_int(c.get("disk_writes")) for c in reports)
        disk_expired = sum(_to_int(c.get("disk_expired")) for c in reports)
        disk_evictions = sum(_to_int(c.get("disk_evictions")) for c in reports)
        disk_entries_peak = max((_to_int(c.get("disk_entries")) for c in reports), default=0)
        cross_stage_by_case.append(
            {
                "id": str(r.get("id")),
                "suite": str(r.get("suite")),
                "cache_report_samples": len(reports),
                "cross_stage_candidates": candidates,
                "cross_stage_reused": reused,
                "cross_stage_saved_ms_estimate": saved,
                "disk_hits": disk_hits,
                "disk_misses": disk_misses,
                "disk_writes": disk_writes,
                "disk_expired": disk_expired,
                "disk_evictions": disk_evictions,
                "disk_entries_peak": disk_entries_peak,
            }
        )

    cross_stage_by_case.sort(
        key=lambda x: (x["cross_stage_reused"], x["cross_stage_candidates"], x["id"]),
        reverse=True,
    )
    cross_stage_candidates_total = sum(int(x["cross_stage_candidates"]) for x in cross_stage_by_case)
    cross_stage_reused_total = sum(int(x["cross_stage_reused"]) for x in cross_stage_by_case)
    cross_stage_saved_ms_total = sum(int(x["cross_stage_saved_ms_estimate"]) for x in cross_stage_by_case)
    disk_hits_total = sum(int(x["disk_hits"]) for x in cross_stage_by_case)
    disk_misses_total = sum(int(x["disk_misses"]) for x in cross_stage_by_case)
    disk_writes_total = sum(int(x["disk_writes"]) for x in cross_stage_by_case)
    disk_expired_total = sum(int(x["disk_expired"]) for x in cross_stage_by_case)
    disk_evictions_total = sum(int(x["disk_evictions"]) for x in cross_stage_by_case)
    disk_entries_peak = max((int(x["disk_entries_peak"]) for x in cross_stage_by_case), default=0)

    grouped_by_case: list[dict[str, Any]] = []
    for r in results:
        samples = _extract_grouping_reports(str(r.get("output", "")))
        if not samples:
            continue
        grouped_by_case.append(
            {
                "id": str(r.get("id")),
                "suite": str(r.get("suite")),
                "grouping_samples": len(samples),
                "grouping_total_ms": sum(_to_int(s.get("grouping_total_ms")) for s in samples),
                "grouping_index_ms": sum(_to_int(s.get("grouping_index_ms")) for s in samples),
                "grouping_rank_ms": sum(_to_int(s.get("grouping_rank_ms")) for s in samples),
                "grouping_root_cause_v2_ms": sum(
                    _to_int(s.get("grouping_root_cause_v2_ms")) for s in samples
                ),
                "grouping_emit_ms": sum(_to_int(s.get("grouping_emit_ms")) for s in samples),
                "grouping_budget_fallback_count": sum(
                    1 for s in samples if bool(s.get("grouping_budget_fallback"))
                ),
            }
        )
    grouped_by_case.sort(
        key=lambda x: (x["grouping_total_ms"], x["grouping_samples"], x["id"]),
        reverse=True,
    )
    grouping_samples_total = sum(int(x["grouping_samples"]) for x in grouped_by_case)
    grouping_total_ms_total = sum(int(x["grouping_total_ms"]) for x in grouped_by_case)
    grouping_index_ms_total = sum(int(x["grouping_index_ms"]) for x in grouped_by_case)
    grouping_rank_ms_total = sum(int(x["grouping_rank_ms"]) for x in grouped_by_case)
    grouping_root_cause_v2_ms_total = sum(int(x["grouping_root_cause_v2_ms"]) for x in grouped_by_case)
    grouping_emit_ms_total = sum(int(x["grouping_emit_ms"]) for x in grouped_by_case)
    grouping_budget_fallback_total = sum(int(x["grouping_budget_fallback_count"]) for x in grouped_by_case)

    report = {
        "schema_version": 1,
        "generated_at_utc": dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "non_gating": True,
        "summary": {
            "cases": total,
            "passed": passed,
            "failed": failed,
            "total_duration_ms": total_duration_ms,
            "mean_duration_ms": mean_duration_ms,
        },
        "by_suite": by_suite,
        "slowest_cases": slowest_cases,
        "cross_stage_signals": {
            "cases_with_cache_report": len(cross_stage_by_case),
            "cross_stage_candidates_total": cross_stage_candidates_total,
            "cross_stage_reused_total": cross_stage_reused_total,
            "cross_stage_saved_ms_estimate_total": cross_stage_saved_ms_total,
            "cases": cross_stage_by_case,
        },
        "disk_cache_signals": {
            "cases_with_cache_report": len(cross_stage_by_case),
            "disk_hits_total": disk_hits_total,
            "disk_misses_total": disk_misses_total,
            "disk_writes_total": disk_writes_total,
            "disk_expired_total": disk_expired_total,
            "disk_evictions_total": disk_evictions_total,
            "disk_entries_peak": disk_entries_peak,
            "cases": [
                {
                    "id": row["id"],
                    "suite": row["suite"],
                    "cache_report_samples": row["cache_report_samples"],
                    "disk_hits": row["disk_hits"],
                    "disk_misses": row["disk_misses"],
                    "disk_writes": row["disk_writes"],
                    "disk_expired": row["disk_expired"],
                    "disk_evictions": row["disk_evictions"],
                    "disk_entries_peak": row["disk_entries_peak"],
                }
                for row in cross_stage_by_case
            ],
        },
        "grouped_diagnostic_signals": {
            "cases_with_grouping_summary": len(grouped_by_case),
            "grouping_summary_samples_total": grouping_samples_total,
            "grouping_total_ms_total": grouping_total_ms_total,
            "grouping_index_ms_total": grouping_index_ms_total,
            "grouping_rank_ms_total": grouping_rank_ms_total,
            "grouping_root_cause_v2_ms_total": grouping_root_cause_v2_ms_total,
            "grouping_emit_ms_total": grouping_emit_ms_total,
            "grouping_budget_fallback_total": grouping_budget_fallback_total,
            "cases": grouped_by_case,
        },
    }
    return json.dumps(report, indent=2, ensure_ascii=True)
