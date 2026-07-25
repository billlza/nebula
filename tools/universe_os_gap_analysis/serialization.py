"""Deterministic serialization shared by assessment components."""

from __future__ import annotations

import json
import math
from dataclasses import fields, is_dataclass
from datetime import datetime, timezone
from enum import Enum
from typing import Any, Mapping


def to_primitive(value: Any) -> Any:
    """Convert typed model values to deterministic JSON-compatible values."""

    if isinstance(value, Enum):
        return to_primitive(value.value)
    if isinstance(value, datetime):
        if value.tzinfo is None or value.utcoffset() is None:
            raise ValueError("serialized datetime must be timezone-aware")
        utc_value = value.astimezone(timezone.utc)
        return utc_value.isoformat(timespec="microseconds").replace("+00:00", "Z")
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("serialized float must be finite")
        return value
    if is_dataclass(value) and not isinstance(value, type):
        result: dict[str, Any] = {}
        for model_field in fields(value):
            key = model_field.metadata.get("json_name", _snake_to_camel(model_field.name))
            result[key] = to_primitive(getattr(value, model_field.name))
        return result
    if isinstance(value, Mapping):
        if not all(isinstance(key, str) for key in value):
            raise TypeError("serialized mapping keys must be strings")
        return {key: to_primitive(value[key]) for key in sorted(value)}
    if isinstance(value, (set, frozenset)):
        primitive_values = [to_primitive(item) for item in value]
        return sorted(primitive_values, key=_canonical_sort_key)
    if isinstance(value, (tuple, list)):
        return [to_primitive(item) for item in value]
    raise TypeError(f"unsupported serialization type: {type(value).__name__}")


def stable_json_dumps(value: Any, *, indent: int | None = None) -> str:
    """Serialize a value as canonical UTF-8 JSON with one trailing newline."""

    if indent is not None and (not isinstance(indent, int) or isinstance(indent, bool) or indent < 0):
        raise ValueError("indent must be a non-negative integer or None")
    separators = (",", ":") if indent is None else None
    return (
        json.dumps(
            to_primitive(value),
            allow_nan=False,
            ensure_ascii=False,
            indent=indent,
            separators=separators,
            sort_keys=True,
        )
        + "\n"
    )


def stable_json_bytes(value: Any, *, indent: int | None = None) -> bytes:
    """Return the stable JSON representation encoded as UTF-8."""

    return stable_json_dumps(value, indent=indent).encode("utf-8")


def _canonical_sort_key(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _snake_to_camel(value: str) -> str:
    head, *tail = value.split("_")
    return head + "".join(part[:1].upper() + part[1:] for part in tail)
