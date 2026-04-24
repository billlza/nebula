from __future__ import annotations

import fnmatch
from pathlib import Path
from typing import Any

try:
    import tomllib  # type: ignore[attr-defined]
except ModuleNotFoundError:  # pragma: no cover
    tomllib = None


class CaseLoadError(RuntimeError):
    pass


def _read_toml(path: Path) -> dict[str, Any]:
    if tomllib is None:
        raise CaseLoadError(
            "Python 3.11+ is required for tomllib. "
            "Install Python 3.11+ to run tests/run.py"
        )
    with path.open("rb") as f:
        data = tomllib.load(f)
    if not isinstance(data, dict):
        raise CaseLoadError(f"{path}: expected table at top-level")
    return data


def _as_list(value: Any, default: list[Any] | None = None) -> list[Any]:
    if value is None:
        return [] if default is None else list(default)
    if isinstance(value, list):
        return value
    raise CaseLoadError(f"expected list, got: {type(value).__name__}")


def _normalize_step(raw: dict[str, Any], case_default: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    out["kind"] = str(raw.get("kind", "nebula"))

    for key in ("command", "source"):
        if key in raw:
            out[key] = raw[key]
        elif key in case_default:
            out[key] = case_default[key]

    out["args"] = [str(x) for x in _as_list(raw.get("args", case_default.get("args", [])))]
    out["expect_rc"] = int(raw.get("expect_rc", case_default.get("expect_rc", 0)))
    timeout_value = raw.get("timeout", case_default.get("timeout"))
    if timeout_value is not None:
        out["timeout"] = int(timeout_value)

    out["expect_stdout_contains"] = [
        str(x)
        for x in _as_list(raw.get("expect_stdout_contains", case_default.get("expect_stdout_contains", [])))
    ]
    out["forbid_stdout_contains"] = [
        str(x)
        for x in _as_list(raw.get("forbid_stdout_contains", case_default.get("forbid_stdout_contains", [])))
    ]
    out["expect_stdout_regex"] = [
        str(x)
        for x in _as_list(raw.get("expect_stdout_regex", case_default.get("expect_stdout_regex", [])))
    ]

    out["expect_diag"] = [
        dict(x) for x in _as_list(raw.get("expect_diag", case_default.get("expect_diag", [])))
    ]
    out["forbid_diag"] = [
        dict(x) for x in _as_list(raw.get("forbid_diag", case_default.get("forbid_diag", [])))
    ]
    out["require_diag_keys"] = [
        str(x)
        for x in _as_list(raw.get("require_diag_keys", case_default.get("require_diag_keys", [])))
    ]

    out["must_exist"] = [
        str(x) for x in _as_list(raw.get("must_exist", case_default.get("must_exist", [])))
    ]
    out["must_not_exist"] = [
        str(x)
        for x in _as_list(raw.get("must_not_exist", case_default.get("must_not_exist", [])))
    ]

    if out["kind"] == "shell":
        run = raw.get("run")
        if not isinstance(run, str) or not run.strip():
            raise CaseLoadError("shell step requires non-empty `run`")
        out["run"] = run
    elif out["kind"] == "nebula":
        cmd = out.get("command")
        if not isinstance(cmd, str) or not cmd.strip():
            raise CaseLoadError("nebula step requires `command`")
        if cmd in {"check", "build", "run"}:
            src = out.get("source")
            if not isinstance(src, str) or not src.strip():
                raise CaseLoadError(f"nebula {cmd} step requires `source`")
    else:
        raise CaseLoadError(f"unsupported step kind: {out['kind']}")

    return out


def _normalize_case(path: Path, raw: dict[str, Any]) -> dict[str, Any]:
    case_id = str(raw.get("id", path.parent.name))
    suite = str(raw.get("suite", path.parent.parent.name))

    case_default = {
        "command": raw.get("command"),
        "source": raw.get("source"),
        "args": _as_list(raw.get("args", [])),
        "expect_rc": int(raw.get("expect_rc", 0)),
        "timeout": raw.get("timeout"),
        "expect_stdout_contains": _as_list(raw.get("expect_stdout_contains", [])),
        "forbid_stdout_contains": _as_list(raw.get("forbid_stdout_contains", [])),
        "expect_stdout_regex": _as_list(raw.get("expect_stdout_regex", [])),
        "expect_diag": _as_list(raw.get("expect_diag", [])),
        "forbid_diag": _as_list(raw.get("forbid_diag", [])),
        "require_diag_keys": _as_list(raw.get("require_diag_keys", [])),
        "must_exist": _as_list(raw.get("must_exist", [])),
        "must_not_exist": _as_list(raw.get("must_not_exist", [])),
    }

    steps_raw = raw.get("steps")
    if steps_raw is None:
        steps = [_normalize_step({}, case_default)]
    else:
        if not isinstance(steps_raw, list) or not steps_raw:
            raise CaseLoadError("`steps` must be a non-empty list")
        steps = [_normalize_step(dict(step), case_default) for step in steps_raw]

    return {
        "id": case_id,
        "suite": suite,
        "path": str(path),
        "steps": steps,
    }


def load_cases(cases_root: Path, suite: str = "all", filter_glob: str = "*") -> list[dict[str, Any]]:
    if not cases_root.exists():
        raise CaseLoadError(f"cases root does not exist: {cases_root}")

    out: list[dict[str, Any]] = []
    for case_file in sorted(cases_root.glob("**/case.toml")):
        raw = _read_toml(case_file)
        case = _normalize_case(case_file, raw)

        if suite != "all" and case["suite"] != suite:
            continue
        if not fnmatch.fnmatch(case["id"], filter_glob):
            continue

        out.append(case)

    out.sort(key=lambda x: x["id"])
    return out
