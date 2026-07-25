"""Read-only source inventory discovery and stable-anchor adapters."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable, Iterator

from .identifiers import RepositoryPath, stable_id
from .models import (
    AssessmentRevision,
    ExecutionState,
    RevisionOrigin,
    SourceCategory,
    SourceInventoryEntry,
)

MAX_ADAPTER_BYTES = 2 * 1024 * 1024

REQUIRED_SOURCE_CATEGORIES = frozenset(
    {
        SourceCategory.SOURCE_CODE,
        SourceCategory.README,
        SourceCategory.ROADMAP,
        SourceCategory.CHANGELOG,
        SourceCategory.RELEASE_NOTES,
        SourceCategory.SPECIFICATION,
        SourceCategory.RFC,
        SourceCategory.TEST,
        SourceCategory.BUILD_CONFIGURATION,
        SourceCategory.CI_WORKFLOW,
        SourceCategory.RELEASE_WORKFLOW,
        SourceCategory.RUNTIME,
        SourceCategory.STANDARD_LIBRARY,
        SourceCategory.OFFICIAL_PACKAGE,
        SourceCategory.EXAMPLE,
        SourceCategory.UNIVERSE_OS_DOCUMENT,
        SourceCategory.GATE_REGISTRY,
    }
)

_SOURCE_SUFFIXES = frozenset(
    {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
        ".java", ".js", ".jsx", ".nb", ".py", ".rs", ".sh", ".ts",
        ".tsx",
    }
)
_TEXT_SUFFIXES = _SOURCE_SUFFIXES | frozenset(
    {".md", ".markdown", ".toml", ".yml", ".yaml", ".json", ".cmake", ".ebnf"}
)
_RELEASE_NOTES = re.compile(r"^release[_-]notes(?:[_-].+)?\.md$", re.IGNORECASE)
_CASE_ID = re.compile(r"(?:^|/)((?:ABI|BEN|BLD|CHK|RUN|SAF|TST)-\d+(?:-[A-Za-z0-9-]+)?)(?:/|$)")
_HEADING = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
_SETEXT = re.compile(r"^\s*(?:=+|-+)\s*$")
_DECLARATION = re.compile(
    r"\b(?:class|struct|enum(?:\s+class)?|namespace|interface|trait|protocol|fn|function|def)\s+([A-Za-z_][A-Za-z0-9_:]*)"
)
_C_LIKE_FUNCTION = re.compile(
    r"^\s*"
    r"(?P<prefix>(?:(?:const|static|inline|constexpr|consteval|virtual|explicit|friend|extern|auto)\s+)*"
    r"[A-Za-z_][A-Za-z0-9_:<>.,]*(?:\s*[*&]+)?\s+)"
    r"(?P<name>[A-Za-z_~][A-Za-z0-9_:~]*)\s*"
    r"\([^;{}]*\)\s*"
    r"(?:const\s*)?(?:noexcept(?:\s*\([^)]*\))?\s*)?"
    r"(?:->\s*[^;{]+\s*)?(?:\{|;)",
    re.MULTILINE,
)
_CMAKE_SYMBOL = re.compile(
    r"^\s*(?:add_executable|add_library|function|macro)\s*\(\s*([^\s)]+)",
    re.IGNORECASE,
)
_MANIFEST_SECTION = re.compile(r"^\s*\[\[?([^\]]+)\]\]?\s*(?:#.*)?$")
_MANIFEST_KEY = re.compile(r"^\s*([A-Za-z0-9_.-]+)\s*[:=]")
_YAML_KEY = re.compile(r"^(\s*)([A-Za-z0-9_.-]+):(?:\s|$)")
_IDENTITY_METADATA_KEYS = frozenset(
    {
        "artifact", "artifact_kind", "artifact_name", "cache_schema_version",
        "compiler_schema_version", "id", "kind", "mode", "name", "predicateType",
        "profile", "runtime_profile", "schema_version", "subject", "target", "version",
    }
)

_IGNORED_DIRECTORY_NAMES = frozenset(
    {".git", ".kiro", ".mypy_cache", ".pytest_cache", ".venv", "__pycache__", "node_modules"}
)
_IGNORED_TOP_LEVEL_PREFIXES = (
    "benchmark_results", "build", "dist", "generated_cpp", "tmp-", "work"
)


class InventoryError(RuntimeError):
    """Structured INV-* failure; partial inventories are never returned."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        operation: str,
        path: str | None = None,
        missing_categories: Iterable[SourceCategory] = (),
    ) -> None:
        if not code.startswith("INV-"):
            raise ValueError("inventory error codes must start with INV-")
        super().__init__(message)
        self.code = code
        self.message = message
        self.operation = operation
        self.path = path
        self.missing_categories = tuple(
            sorted(set(missing_categories), key=lambda item: item.value)
        )

    def to_dict(self) -> dict[str, object]:
        result: dict[str, object] = {
            "code": self.code,
            "message": self.message,
            "operation": self.operation,
        }
        if self.path is not None:
            result["path"] = self.path
        if self.missing_categories:
            result["missingCategories"] = [
                item.value for item in self.missing_categories
            ]
        return result


@dataclass(frozen=True, slots=True)
class _ReadResult:
    content_hash: str
    payload: bytes | None
    inspected: bool
    execution_state: ExecutionState
    execution_detail: str


class SourceInventoryDiscoverer:
    """Discover required repository evidence without commands or link traversal."""

    def discover(
        self,
        repo_root: Path,
        revision: AssessmentRevision,
        *,
        required_categories: Iterable[SourceCategory] = REQUIRED_SOURCE_CATEGORIES,
    ) -> tuple[SourceInventoryEntry, ...]:
        if not isinstance(revision, AssessmentRevision):
            raise TypeError("revision must be an AssessmentRevision")
        root = self._validate_root(repo_root)
        required = frozenset(required_categories)
        if not all(isinstance(item, SourceCategory) for item in required):
            raise TypeError("required_categories must contain SourceCategory values")
        excluded = tuple(str(item.path) for item in revision.excluded_paths)
        origin = (
            RevisionOrigin.COMMITTED_REVISION
            if revision.worktree_clean
            else RevisionOrigin.CURRENT_WORKTREE
        )
        entries: list[SourceInventoryEntry] = []
        root_fd = self._open_root(root)
        try:
            for relative, kind in self._walk(root_fd, "", excluded):
                category = classify_source_path(relative)
                if category is None:
                    continue
                read = self._read(root_fd, relative, kind)
                anchors = self._anchors(relative, category, read.payload)
                entries.append(
                    SourceInventoryEntry(
                        id=stable_id("inventory", category.value, relative),
                        category=category,
                        path=relative,
                        revision_origin=origin,
                        inspected=read.inspected,
                        execution_state=read.execution_state,
                        execution_detail=read.execution_detail,
                        content_hash=read.content_hash,
                        stable_anchors=anchors,
                    )
                )
        finally:
            os.close(root_fd)

        covered = {entry.category for entry in entries if entry.inspected}
        missing = required - covered
        if missing:
            names = ", ".join(item.value for item in sorted(missing, key=lambda item: item.value))
            raise InventoryError(
                "INV-REQUIRED-CATEGORY-MISSING",
                f"required source categories are missing or uninspectable: {names}",
                operation="validate-required-categories",
                missing_categories=missing,
            )
        return tuple(sorted(entries, key=lambda item: (item.category.value, str(item.path))))

    @staticmethod
    def _validate_root(repo_root: Path) -> Path:
        try:
            root = repo_root.expanduser().resolve(strict=True)
        except OSError as error:
            raise InventoryError(
                "INV-ROOT-READ", str(error), operation="resolve-repository-root"
            ) from error
        if not root.is_dir():
            raise InventoryError(
                "INV-ROOT-INVALID",
                f"inventory root is not a directory: {root}",
                operation="resolve-repository-root",
            )
        return root

    @staticmethod
    def _open_root(root: Path) -> int:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0)
        try:
            return os.open(root, flags)
        except OSError as error:
            raise InventoryError(
                "INV-ROOT-READ", str(error), operation="open-repository-root"
            ) from error

    def _walk(
        self,
        directory_fd: int,
        prefix: str,
        excluded: tuple[str, ...],
    ) -> Iterator[tuple[str, str]]:
        try:
            with os.scandir(directory_fd) as iterator:
                directory_entries = sorted(iterator, key=lambda item: os.fsencode(item.name))
        except OSError as error:
            raise InventoryError(
                "INV-DIRECTORY-READ",
                str(error),
                operation="enumerate-directory",
                path=prefix or None,
            ) from error

        directory_flags = (
            os.O_RDONLY
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        for entry in directory_entries:
            relative = entry.name if not prefix else f"{prefix}/{entry.name}"
            self._validate_relative(relative)
            if self._is_excluded(relative, excluded):
                continue
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as error:
                raise InventoryError(
                    "INV-PATH-READ",
                    str(error),
                    operation="inspect-directory-entry",
                    path=relative,
                ) from error
            if stat.S_ISLNK(metadata.st_mode):
                yield relative, "symlink"
                continue
            if stat.S_ISDIR(metadata.st_mode):
                if self._skip_directory(prefix, entry.name):
                    continue
                try:
                    child_fd = os.open(entry.name, directory_flags, dir_fd=directory_fd)
                except OSError as error:
                    raise InventoryError(
                        "INV-PATH-UNSAFE",
                        str(error),
                        operation="open-directory-without-following-links",
                        path=relative,
                    ) from error
                try:
                    yield from self._walk(child_fd, relative, excluded)
                finally:
                    os.close(child_fd)
                continue
            if stat.S_ISREG(metadata.st_mode):
                yield relative, "file"
                continue
            if classify_source_path(relative) is not None:
                raise InventoryError(
                    "INV-PATH-UNSUPPORTED",
                    "classified evidence path is neither a regular file nor a symbolic link",
                    operation="inspect-directory-entry",
                    path=relative,
                )

    @staticmethod
    def _validate_relative(relative: str) -> None:
        try:
            relative.encode("utf-8", errors="strict")
            RepositoryPath(PurePosixPath(relative).as_posix())
        except (UnicodeError, TypeError, ValueError) as error:
            raise InventoryError(
                "INV-PATH-INVALID",
                "repository entry is not a safe canonical UTF-8 relative path",
                operation="validate-repository-path",
                path=relative,
            ) from error

    @staticmethod
    def _is_excluded(relative: str, excluded: tuple[str, ...]) -> bool:
        return any(relative == item or relative.startswith(item + "/") for item in excluded)

    @staticmethod
    def _skip_directory(prefix: str, name: str) -> bool:
        if name in _IGNORED_DIRECTORY_NAMES:
            return True
        if prefix:
            return False
        return any(
            name.startswith(item)
            if item.endswith("-")
            else name == item or name.startswith(item + "-")
            for item in _IGNORED_TOP_LEVEL_PREFIXES
        )

    def _read(self, root_fd: int, relative: str, kind: str) -> _ReadResult:
        components = relative.split("/")
        parent_fd = os.dup(root_fd)
        directory_flags = (
            os.O_RDONLY
            | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        try:
            for component in components[:-1]:
                try:
                    child_fd = os.open(component, directory_flags, dir_fd=parent_fd)
                except OSError as error:
                    raise InventoryError(
                        "INV-PATH-UNSAFE",
                        str(error),
                        operation="open-parent-without-following-links",
                        path=relative,
                    ) from error
                os.close(parent_fd)
                parent_fd = child_fd
            if kind == "symlink":
                return self._read_symlink(parent_fd, components[-1], relative)
            return self._read_regular(parent_fd, components[-1], relative)
        finally:
            os.close(parent_fd)

    @staticmethod
    def _read_symlink(parent_fd: int, name: str, relative: str) -> _ReadResult:
        try:
            before = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            target = os.readlink(name, dir_fd=parent_fd)
            after = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError as error:
            raise InventoryError(
                "INV-SYMLINK-READ",
                str(error),
                operation="hash-symbolic-link-without-following",
                path=relative,
            ) from error
        if not stat.S_ISLNK(before.st_mode) or _identity(before) != _identity(after):
            raise InventoryError(
                "INV-CONTENT-DRIFT",
                "symbolic link changed while inventory was collected",
                operation="hash-symbolic-link-without-following",
                path=relative,
            )
        target_bytes = os.fsencode(target)
        return _ReadResult(
            content_hash=hashlib.sha256(target_bytes).hexdigest(),
            payload=None,
            inspected=False,
            execution_state=ExecutionState.NOT_RUN,
            execution_detail="skipped: symbolic link target bytes hashed without following the link",
        )

    @staticmethod
    def _read_regular(parent_fd: int, name: str, relative: str) -> _ReadResult:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            before = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            descriptor = os.open(name, flags, dir_fd=parent_fd)
        except OSError as error:
            raise InventoryError(
                "INV-FILE-READ", str(error), operation="open-file", path=relative
            ) from error
        digest = hashlib.sha256()
        payload = bytearray()
        length = 0
        try:
            opened = os.fstat(descriptor)
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                length += len(chunk)
                if len(payload) <= MAX_ADAPTER_BYTES:
                    remaining = MAX_ADAPTER_BYTES + 1 - len(payload)
                    payload.extend(chunk[:remaining])
            after = os.fstat(descriptor)
            path_after = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError as error:
            raise InventoryError(
                "INV-FILE-READ", str(error), operation="read-file", path=relative
            ) from error
        finally:
            os.close(descriptor)
        if (
            not stat.S_ISREG(before.st_mode)
            or len({_identity(item) for item in (before, opened, after, path_after)}) != 1
            or length != after.st_size
        ):
            raise InventoryError(
                "INV-CONTENT-DRIFT",
                "file changed while inventory was collected",
                operation="read-file",
                path=relative,
            )
        if len(payload) > MAX_ADAPTER_BYTES:
            return _ReadResult(
                content_hash=digest.hexdigest(),
                payload=None,
                inspected=True,
                execution_state=ExecutionState.NOT_RUN,
                execution_detail=(
                    f"content hash inspected; stable-anchor adapter skipped above {MAX_ADAPTER_BYTES} bytes"
                ),
            )
        return _ReadResult(
            content_hash=digest.hexdigest(),
            payload=bytes(payload),
            inspected=True,
            execution_state=ExecutionState.VALIDATED,
            execution_detail="content hash and stable anchors validated; no command was executed",
        )

    def _anchors(
        self, relative: str, category: SourceCategory, payload: bytes | None
    ) -> tuple[str, ...]:
        anchors = {f"File:{relative}"}
        if payload is None:
            return tuple(sorted(anchors))
        suffix = PurePosixPath(relative).suffix.lower()
        if suffix in _TEXT_SUFFIXES or PurePosixPath(relative).name == "CMakeLists.txt":
            try:
                text = payload.decode("utf-8", errors="strict")
            except UnicodeError as error:
                raise InventoryError(
                    "INV-ADAPTER-DECODE",
                    "text evidence is not valid UTF-8",
                    operation="extract-stable-anchors",
                    path=relative,
                ) from error
            anchors.update(_markdown_anchors(text) if suffix in {".md", ".markdown"} else ())
            anchors.update(_symbol_anchors(text, relative))
            anchors.update(_manifest_anchors(text, relative))
            anchors.update(_case_anchors(text, relative))
            if category in {SourceCategory.CI_WORKFLOW, SourceCategory.RELEASE_WORKFLOW}:
                anchors.update(_workflow_job_anchors(text))
        if category is SourceCategory.ARTIFACT and suffix in {".json", ".nebmeta"}:
            anchors.update(_artifact_metadata_anchors(payload, relative))
        return tuple(sorted(anchors))


def classify_source_path(relative: str) -> SourceCategory | None:
    """Classify one normalized path; classification is exclusive and deterministic."""

    path = PurePosixPath(relative)
    parts = path.parts
    lower_parts = tuple(part.lower() for part in parts)
    name = path.name
    lower_name = name.lower()

    if relative == "docs/universeos/gate_registry.md":
        return SourceCategory.GATE_REGISTRY
    if relative in {"docs/support_matrix.md", "docs/system_profile.md"}:
        return SourceCategory.UNIVERSE_OS_DOCUMENT
    if (
        len(parts) >= 2
        and lower_parts[0] == "docs"
        and (
            lower_parts[1] == "universeos"
            or lower_name.startswith("universeos")
        )
    ):
        return SourceCategory.UNIVERSE_OS_DOCUMENT
    if len(parts) >= 3 and parts[:2] == (".github", "workflows"):
        return (
            SourceCategory.RELEASE_WORKFLOW
            if "release" in lower_name
            else SourceCategory.CI_WORKFLOW
        )
    if _RELEASE_NOTES.fullmatch(name):
        return SourceCategory.RELEASE_NOTES
    if lower_name.startswith("readme"):
        return SourceCategory.README
    if lower_name.startswith("roadmap"):
        return SourceCategory.ROADMAP
    if lower_name.startswith("changelog"):
        return SourceCategory.CHANGELOG
    if lower_parts and lower_parts[0] in {"rfc", "rfcs"}:
        return SourceCategory.RFC
    if lower_parts and lower_parts[0] == "spec":
        return SourceCategory.SPECIFICATION
    if name == "CMakeLists.txt" or path.suffix.lower() == ".cmake":
        return SourceCategory.BUILD_CONFIGURATION
    if lower_parts and lower_parts[0] == "tests":
        return SourceCategory.TEST
    if lower_parts and lower_parts[0] == "runtime":
        return SourceCategory.RUNTIME
    if lower_parts and lower_parts[0] == "std":
        return SourceCategory.STANDARD_LIBRARY
    if lower_parts and lower_parts[0] == "official":
        return SourceCategory.OFFICIAL_PACKAGE
    if lower_parts and lower_parts[0] == "examples":
        return SourceCategory.EXAMPLE
    if lower_parts and lower_parts[0] == "artifacts" and path.suffix.lower() in {".json", ".nebmeta"}:
        return SourceCategory.ARTIFACT
    if path.suffix.lower() in _SOURCE_SUFFIXES:
        return SourceCategory.SOURCE_CODE
    return None


def _markdown_anchors(text: str) -> tuple[str, ...]:
    lines = text.splitlines()
    headings: set[str] = set()
    for index, line in enumerate(lines):
        match = _HEADING.match(line)
        if match:
            headings.add(f"Heading:{match.group(1).strip()}")
        elif index and line.strip() and _SETEXT.match(line):
            title = lines[index - 1].strip()
            if title:
                headings.add(f"Heading:{title}")
    return tuple(sorted(headings))


def _symbol_anchors(text: str, relative: str) -> tuple[str, ...]:
    suffix = PurePosixPath(relative).suffix.lower()
    if suffix not in _SOURCE_SUFFIXES and PurePosixPath(relative).name != "CMakeLists.txt" and suffix != ".cmake":
        return ()
    symbols: set[str] = set()
    for line in text.splitlines():
        for match in _DECLARATION.finditer(line):
            symbols.add(f"Symbol:{match.group(1)}")
        cmake = _CMAKE_SYMBOL.match(line)
        if cmake:
            symbols.add(f"Symbol:{cmake.group(1)}")
    if suffix in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}:
        for match in _C_LIKE_FUNCTION.finditer(text):
            symbols.add(f"Symbol:{match.group('name')}")
    return tuple(sorted(symbols))


def _manifest_anchors(text: str, relative: str) -> tuple[str, ...]:
    suffix = PurePosixPath(relative).suffix.lower()
    if suffix not in {".toml", ".yml", ".yaml"}:
        return ()
    keys: set[str] = set()
    if suffix == ".toml":
        section = ""
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            section_match = _MANIFEST_SECTION.match(line)
            if section_match:
                section = section_match.group(1).strip()
                keys.add(f"ManifestKey:{section}")
                continue
            key_match = _MANIFEST_KEY.match(line)
            if key_match:
                key = key_match.group(1)
                keys.add(f"ManifestKey:{section + '.' if section else ''}{key}")
        return tuple(sorted(keys))

    parents: list[tuple[int, str]] = []
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = _YAML_KEY.match(line)
        if not match:
            continue
        indent = len(match.group(1).replace("\t", "        "))
        key = match.group(2)
        while parents and indent <= parents[-1][0]:
            parents.pop()
        qualified = ".".join([*(item[1] for item in parents), key])
        keys.add(f"ManifestKey:{qualified}")
        parents.append((indent, key))
    return tuple(sorted(keys))


def _case_anchors(text: str, relative: str) -> tuple[str, ...]:
    identifiers = {match.group(1) for match in _CASE_ID.finditer(relative)}
    if PurePosixPath(relative).name == "case.toml":
        for line in text.splitlines():
            match = re.match(r'^\s*id\s*=\s*["\']([^"\']+)["\']\s*$', line)
            if match:
                identifiers.add(match.group(1))
    return tuple(f"CaseId:{item}" for item in sorted(identifiers))


def _workflow_job_anchors(text: str) -> tuple[str, ...]:
    jobs_indent: int | None = None
    job_indent: int | None = None
    jobs: set[str] = set()
    for line in text.splitlines():
        match = _YAML_KEY.match(line)
        if not match:
            continue
        indent = len(match.group(1).replace("\t", "        "))
        key = match.group(2)
        if key == "jobs":
            jobs_indent = indent
            job_indent = None
            continue
        if jobs_indent is not None:
            if indent <= jobs_indent:
                jobs_indent = None
                job_indent = None
            else:
                if job_indent is None:
                    job_indent = indent
                if indent == job_indent:
                    jobs.add(f"WorkflowJob:{key}")
    return tuple(sorted(jobs))


def _artifact_metadata_anchors(payload: bytes, relative: str) -> tuple[str, ...]:
    suffix = PurePosixPath(relative).suffix.lower()
    if suffix == ".nebmeta":
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeError as error:
            raise InventoryError(
                "INV-ARTIFACT-METADATA",
                "artifact metadata is not valid UTF-8",
                operation="extract-artifact-metadata-anchors",
                path=relative,
            ) from error
        anchors: set[str] = set()
        seen: set[str] = set()
        for line_number, line in enumerate(text.splitlines(), start=1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            match = re.fullmatch(r"([A-Za-z0-9_.-]+)=(.*)", stripped)
            if match is None or match.group(1) in seen:
                raise InventoryError(
                    "INV-ARTIFACT-METADATA",
                    f"artifact metadata has an invalid or duplicate field at line {line_number}",
                    operation="extract-artifact-metadata-anchors",
                    path=relative,
                )
            key, value = match.groups()
            seen.add(key)
            anchors.add(f"ArtifactMetadataKey:{key}")
            if key in _IDENTITY_METADATA_KEYS:
                anchors.add(f"ArtifactMetadata:{key}={value[:160]}")
        return tuple(sorted(anchors))

    try:
        document = json.loads(payload)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise InventoryError(
            "INV-ARTIFACT-METADATA",
            "artifact metadata is not valid UTF-8 JSON",
            operation="extract-artifact-metadata-anchors",
            path=relative,
        ) from error
    anchors = set()
    if isinstance(document, dict):
        for key in sorted(document):
            anchors.add(f"ArtifactMetadataKey:{key}")
            value = document[key]
            if key in _IDENTITY_METADATA_KEYS and isinstance(value, (str, int, float, bool)):
                rendered = str(value).replace("\n", " ")[:160]
                anchors.add(f"ArtifactMetadata:{key}={rendered}")
    return tuple(sorted(anchors))


def _identity(value: os.stat_result) -> tuple[int, ...]:
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def discover_source_inventory(
    repo_root: Path,
    revision: AssessmentRevision,
    *,
    required_categories: Iterable[SourceCategory] = REQUIRED_SOURCE_CATEGORIES,
) -> tuple[SourceInventoryEntry, ...]:
    """Convenience API for deterministic, fail-closed inventory discovery."""

    return SourceInventoryDiscoverer().discover(
        repo_root, revision, required_categories=required_categories
    )
