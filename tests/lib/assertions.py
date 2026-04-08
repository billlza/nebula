from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from .diag_parser import extract_diagnostics


def _to_bool(v: Any) -> bool:
    if isinstance(v, bool):
        return v
    if isinstance(v, str):
        return v.lower() in {"1", "true", "yes", "on"}
    return bool(v)


def _diag_matches(diag: dict[str, Any], spec: dict[str, Any]) -> bool:
    for key, expected in spec.items():
        if key == "confidence_min":
            conf = diag.get("confidence")
            if conf is None or float(conf) < float(expected):
                return False
            continue
        if key == "confidence_max":
            conf = diag.get("confidence")
            if conf is None or float(conf) > float(expected):
                return False
            continue
        if key == "confidence_is_null":
            conf = diag.get("confidence")
            if _to_bool(expected) and conf is not None:
                return False
            if not _to_bool(expected) and conf is None:
                return False
            continue
        if key == "predictive":
            if _to_bool(diag.get("predictive")) != _to_bool(expected):
                return False
            continue

        if str(diag.get(key)) != str(expected):
            return False
    return True


def evaluate_step_assertions(step: dict[str, Any], step_result: dict[str, Any], sandbox_root: Path) -> dict[str, Any]:
    matched = 0
    failures: list[str] = []

    output = step_result["output"]
    rc = step_result["rc"]

    expect_rc = int(step.get("expect_rc", 0))
    if rc != expect_rc:
        failures.append(f"expected rc={expect_rc}, got rc={rc}")
    else:
        matched += 1

    for needle in step.get("expect_stdout_contains", []):
        if needle not in output:
            failures.append(f"expected output to contain: {needle!r}")
        else:
            matched += 1

    for needle in step.get("forbid_stdout_contains", []):
        if needle in output:
            failures.append(f"expected output not to contain: {needle!r}")
        else:
            matched += 1

    for pattern in step.get("expect_stdout_regex", []):
        if not re.search(pattern, output, re.MULTILINE):
            failures.append(f"expected output to match regex: {pattern!r}")
        else:
            matched += 1

    for rel in step.get("must_exist", []):
        p = sandbox_root / rel
        if not p.exists():
            failures.append(f"expected path to exist: {p}")
        else:
            matched += 1

    for rel in step.get("must_not_exist", []):
        p = sandbox_root / rel
        if p.exists():
            failures.append(f"expected path not to exist: {p}")
        else:
            matched += 1

    diags = extract_diagnostics(output)
    budget_warning_count = sum(1 for d in diags if d.get("code") == "NBL-PR001")

    required_keys = step.get("require_diag_keys", [])
    for key in required_keys:
        if not diags:
            failures.append("expected diagnostics but found none")
            break
        if not all(key in d for d in diags):
            failures.append(f"expected all diagnostics to contain key: {key}")
        else:
            matched += 1

    for spec in step.get("expect_diag", []):
        found = any(_diag_matches(d, spec) for d in diags)
        if not found:
            failures.append(f"expected diagnostic match not found: {spec}")
        else:
            matched += 1

    for spec in step.get("forbid_diag", []):
        found = any(_diag_matches(d, spec) for d in diags)
        if found:
            failures.append(f"forbidden diagnostic match found: {spec}")
        else:
            matched += 1

    return {
        "ok": len(failures) == 0,
        "matched_assertions": matched,
        "fail_reason": failures[0] if failures else "",
        "all_failures": failures,
        "diag_count": len(diags),
        "budget_warning_count": budget_warning_count,
    }
