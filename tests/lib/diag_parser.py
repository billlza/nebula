from __future__ import annotations

import json
from typing import Any


def _scan_json_array_segments(text: str) -> list[str]:
    out: list[str] = []
    i = 0
    n = len(text)

    while i < n:
        if text[i] != "[":
            i += 1
            continue

        start = i
        depth = 0
        in_str = False
        escaped = False
        j = i
        found = False

        while j < n:
            ch = text[j]
            if in_str:
                if escaped:
                    escaped = False
                elif ch == "\\":
                    escaped = True
                elif ch == '"':
                    in_str = False
            else:
                if ch == '"':
                    in_str = True
                elif ch == "[":
                    depth += 1
                elif ch == "]":
                    depth -= 1
                    if depth == 0:
                        out.append(text[start : j + 1])
                        i = j + 1
                        found = True
                        break
            j += 1

        if not found:
            i = start + 1

    return out


def extract_diagnostics(output: str) -> list[dict[str, Any]]:
    diags: list[dict[str, Any]] = []
    for seg in _scan_json_array_segments(output):
        try:
            parsed = json.loads(seg)
        except json.JSONDecodeError:
            continue

        if not isinstance(parsed, list):
            continue
        if not parsed:
            continue

        if not all(isinstance(item, dict) for item in parsed):
            continue
        if not any("code" in item and "severity" in item for item in parsed):
            continue

        diags.extend(parsed)
    return diags
