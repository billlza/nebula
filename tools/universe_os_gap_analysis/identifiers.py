"""Stable identifiers and repository-relative paths for assessment models."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import PurePosixPath
from typing import Any

_ID_PATTERN = re.compile(
    r"^[A-Za-z][A-Za-z0-9_.:]*(?:-[A-Za-z0-9_.:]+)*$"
)
_PREFIX_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")
_WINDOWS_DRIVE_PATTERN = re.compile(r"^[A-Za-z]:$")


class StableId(str):
    """Validated stable object identifier with non-empty hyphen-separated segments."""

    def __new__(cls, value: str) -> "StableId":
        if not isinstance(value, str):
            raise TypeError("stable ID must be a string")
        if not _ID_PATTERN.fullmatch(value):
            raise ValueError(f"invalid stable ID: {value!r}")
        return str.__new__(cls, value)


class ReferenceId(StableId):
    """A typed reference to another stable assessment object."""


def repository_path(value: str) -> "RepositoryPath":
    """Construct a validated repository-relative POSIX path."""

    return RepositoryPath(value)


class RepositoryPath(str):
    """Normalized portable POSIX path below, but never equal to, the repository root."""

    def __new__(cls, value: str) -> "RepositoryPath":
        if not isinstance(value, str):
            raise TypeError("repository path must be a string")
        if not value or value in {".", ".."}:
            raise ValueError("repository path must identify an entry below the root")
        if "\\" in value or any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError(f"repository path is not portable POSIX syntax: {value!r}")
        path = PurePosixPath(value)
        if (
            path.is_absolute()
            or any(part in {"", ".", ".."} for part in path.parts)
            or _WINDOWS_DRIVE_PATTERN.fullmatch(path.parts[0]) is not None
        ):
            raise ValueError(f"repository path must be normalized and relative: {value!r}")
        if path.as_posix() != value or value.endswith("/"):
            raise ValueError(f"repository path must be normalized: {value!r}")
        return str.__new__(cls, value)


def stable_id(prefix: str, *components: Any) -> StableId:
    """Derive a stable ID from canonical, JSON-compatible components."""

    if not _PREFIX_PATTERN.fullmatch(prefix):
        raise ValueError(f"invalid stable ID prefix: {prefix!r}")
    payload = json.dumps(
        components,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
        default=_unsupported_component,
    ).encode("utf-8")
    digest = hashlib.sha256(payload).hexdigest()[:20]
    return StableId(f"{prefix}-{digest}")


def reference(value: str | StableId) -> ReferenceId:
    """Convert an object ID into a typed reference ID."""

    if not isinstance(value, str):
        raise TypeError("reference ID must be a string or StableId")
    return ReferenceId(value)


def _unsupported_component(value: Any) -> Any:
    raise TypeError(f"stable ID component is not JSON-compatible: {type(value).__name__}")
