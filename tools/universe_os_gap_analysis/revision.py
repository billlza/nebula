"""Fail-closed binding of an assessment to local repository identity."""

from __future__ import annotations

import os
import stat
import subprocess
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Protocol, Sequence

from .identifiers import StableId, stable_id
from .models import (
    AssessmentRevision,
    CommittedRevisionAxis,
    CurrentWorktreeAxis,
    ExcludedPath,
    RevisionEvidenceAxes,
    TagBinding,
    TaggedReleaseAxis,
)

REVISION_SCHEMA_VERSION = "1.0"
FINGERPRINT_DEFERRED = "pending-task-2.2"
_GIT_TIMEOUT_SECONDS = 10


class RevisionBindingError(RuntimeError):
    """Structured REV-* failure; no partial revision is exposed."""

    def __init__(self, code: str, message: str, *, operation: str) -> None:
        if not code.startswith("REV-"):
            raise ValueError("revision error codes must start with REV-")
        super().__init__(message)
        self.code = code
        self.operation = operation
        self.message = message

    def to_dict(self) -> dict[str, str]:
        return {"code": self.code, "message": self.message, "operation": self.operation}


@dataclass(frozen=True, slots=True)
class FingerprintCapture:
    """Complete deterministic worktree fingerprint captured by a provider."""

    algorithm: str
    worktree_fingerprint: str
    tracked_diff_hash: str
    untracked_path_set_hash: str
    excluded_paths: tuple[ExcludedPath, ...] = ()

    def __post_init__(self) -> None:
        for name in (
            "algorithm",
            "worktree_fingerprint",
            "tracked_diff_hash",
            "untracked_path_set_hash",
        ):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{name} must be non-empty text")
        if not all(isinstance(item, ExcludedPath) for item in self.excluded_paths):
            raise TypeError("excluded_paths must contain ExcludedPath values")
        object.__setattr__(
            self,
            "excluded_paths",
            tuple(sorted(self.excluded_paths, key=lambda item: item.path)),
        )

    @classmethod
    def deferred(cls) -> "FingerprintCapture":
        return cls(
            algorithm=FINGERPRINT_DEFERRED,
            worktree_fingerprint=FINGERPRINT_DEFERRED,
            tracked_diff_hash=FINGERPRINT_DEFERRED,
            untracked_path_set_hash=FINGERPRINT_DEFERRED,
        )


class FingerprintProvider(Protocol):
    """Read-only interface for a worktree fingerprint implementation."""

    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> FingerprintCapture: ...


@dataclass(frozen=True, slots=True)
class _VersionRead:
    value: str
    identity: tuple[int, int, int, int]


@dataclass(frozen=True, slots=True)
class _RepositoryState:
    commit_id: str
    branch: str
    describe: str
    tags: tuple[TagBinding, ...]
    status: bytes
    version: _VersionRead


class RevisionBinder:
    """Collect one coherent local Git/version snapshot without network access."""

    def __init__(
        self,
        *,
        git_executable: str = "git",
        fingerprint_provider: FingerprintProvider | None = None,
    ) -> None:
        self._git_executable = git_executable
        if fingerprint_provider is None:
            # Imported lazily so the concrete provider can reuse the public
            # FingerprintCapture and RevisionBindingError types without a cycle.
            from .fingerprint import WorktreeFingerprintProvider

            fingerprint_provider = WorktreeFingerprintProvider(
                git_executable=git_executable
            )
        self._fingerprint_provider = fingerprint_provider

    def bind(
        self,
        repo_root: Path,
        assessment_output_paths: Sequence[Path] = (),
        clock: Callable[[], datetime] | None = None,
    ) -> AssessmentRevision:
        root = self._discover_root(repo_root)
        first_root_id = self._repository_root_id(root)
        first = self._capture_state(root)
        fingerprint = self._capture_fingerprint(root, tuple(assessment_output_paths))
        second = self._capture_state(root)
        second_root_id = self._repository_root_id(root)
        if first != second:
            raise RevisionBindingError(
                "REV-DRIFT",
                "repository Git or VERSION state changed during revision binding",
                operation="stability-check",
            )
        if first_root_id != second_root_id:
            raise RevisionBindingError(
                "REV-ROOT-DRIFT",
                "repository root identity changed during revision binding",
                operation="stability-check",
            )
        assessed_at = self._utc_timestamp(clock or (lambda: datetime.now(timezone.utc)))
        clean = not first.status
        tags = tuple(item.name for item in first.tags)
        try:
            axes = RevisionEvidenceAxes(
                tagged_release=TaggedReleaseAxis(describe=first.describe, tags=first.tags),
                committed_revision=CommittedRevisionAxis(
                    commit_id=first.commit_id, branch=first.branch
                ),
                current_worktree=CurrentWorktreeAxis(
                    base_commit_id=first.commit_id, worktree_clean=clean
                ),
            )
            return AssessmentRevision(
                schema_version=REVISION_SCHEMA_VERSION,
                commit_id=first.commit_id,
                branch=first.branch,
                version=first.version.value,
                describe=first.describe,
                tags=tags,
                worktree_clean=clean,
                assessed_at_utc=assessed_at,
                fingerprint_algorithm=fingerprint.algorithm,
                worktree_fingerprint=fingerprint.worktree_fingerprint,
                tracked_diff_hash=fingerprint.tracked_diff_hash,
                untracked_path_set_hash=fingerprint.untracked_path_set_hash,
                excluded_paths=fingerprint.excluded_paths,
                repository_root_id=second_root_id,
                evidence_axes=axes,
            )
        except (TypeError, ValueError) as error:
            raise RevisionBindingError(
                "REV-SNAPSHOT-INVALID",
                str(error),
                operation="construct-assessment-revision",
            ) from error

    @staticmethod
    def _repository_root_id(root: Path) -> StableId:
        try:
            root_stat = root.stat()
            return stable_id(
                "repository-root", str(root), root_stat.st_dev, root_stat.st_ino
            )
        except OSError as error:
            raise RevisionBindingError(
                "REV-ROOT-READ", str(error), operation="repository-root-identity"
            ) from error

    def _discover_root(self, candidate: Path) -> Path:
        try:
            path = candidate.expanduser().resolve(strict=True)
        except OSError as error:
            raise RevisionBindingError(
                "REV-ROOT-READ", str(error), operation="resolve-repository-root"
            ) from error
        if not path.is_dir():
            raise RevisionBindingError(
                "REV-ROOT-INVALID",
                f"repository location is not a directory: {path}",
                operation="resolve-repository-root",
            )
        raw_root = self._git(path, "rev-parse", "--show-toplevel")
        try:
            root_text = raw_root.decode("utf-8", errors="strict").strip()
            root = Path(root_text).resolve(strict=True)
        except (OSError, UnicodeError) as error:
            raise RevisionBindingError(
                "REV-ROOT-READ",
                "Git returned an unreadable repository root",
                operation="git-show-toplevel",
            ) from error
        if not root.is_dir():
            raise RevisionBindingError(
                "REV-ROOT-INVALID",
                "Git repository root is not a directory",
                operation="git-show-toplevel",
            )
        return root

    def _capture_state(self, root: Path) -> _RepositoryState:
        commit_id = self._git_text(root, "rev-parse", "--verify", "HEAD^{commit}")
        branch = self._git_text(root, "rev-parse", "--abbrev-ref", "HEAD")
        describe = self._git_text(root, "describe", "--tags", "--always")
        tag_output = self._git(
            root,
            "for-each-ref",
            "--points-at=HEAD",
            "--format=%(refname:strip=2)%00%(objectname)%00%(*objectname)",
            "refs/tags",
        )
        status = self._git(root, "status", "--porcelain=v1", "-z", "--untracked-files=all")
        return _RepositoryState(
            commit_id=commit_id,
            branch=branch,
            describe=describe,
            tags=self._parse_tags(tag_output),
            status=status,
            version=self._read_version(root),
        )

    def _capture_fingerprint(
        self, root: Path, output_paths: tuple[Path, ...]
    ) -> FingerprintCapture:
        if self._fingerprint_provider is None:
            return FingerprintCapture.deferred()
        try:
            capture = self._fingerprint_provider.capture(root, output_paths)
        except RevisionBindingError:
            raise
        except Exception as error:
            raise RevisionBindingError(
                "REV-FINGERPRINT-READ", str(error), operation="fingerprint-provider"
            ) from error
        if not isinstance(capture, FingerprintCapture):
            raise RevisionBindingError(
                "REV-FINGERPRINT-INVALID",
                "fingerprint provider returned an invalid capture",
                operation="fingerprint-provider",
            )
        return capture

    @staticmethod
    def _utc_timestamp(clock: Callable[[], datetime]) -> datetime:
        try:
            value = clock()
        except Exception as error:
            raise RevisionBindingError(
                "REV-TIMESTAMP-READ", str(error), operation="assessment-clock"
            ) from error
        if not isinstance(value, datetime) or value.tzinfo is None or value.utcoffset() is None:
            raise RevisionBindingError(
                "REV-TIMESTAMP-INVALID",
                "assessment clock must return a timezone-aware datetime",
                operation="assessment-clock",
            )
        return value.astimezone(timezone.utc)

    def _read_version(self, root: Path) -> _VersionRead:
        path = root / "VERSION"
        descriptor = -1
        try:
            path_before = path.lstat()
            if not stat.S_ISREG(path_before.st_mode):
                raise OSError("VERSION must be a regular file and not a symbolic link")
            flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
            descriptor = os.open(path, flags)
            before = os.fstat(descriptor)
            if not stat.S_ISREG(before.st_mode):
                raise OSError("VERSION must be a regular file and not a symbolic link")
            with os.fdopen(descriptor, "rb", closefd=False) as version_file:
                data = version_file.read()
            after = os.fstat(descriptor)
            path_after = path.lstat()
        except OSError as error:
            raise RevisionBindingError(
                "REV-VERSION-READ", str(error), operation="read-version"
            ) from error
        finally:
            if descriptor >= 0:
                os.close(descriptor)
        path_before_identity = (
            path_before.st_dev,
            path_before.st_ino,
            path_before.st_size,
            path_before.st_mtime_ns,
        )
        before_identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
        after_identity = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        path_identity = (
            path_after.st_dev,
            path_after.st_ino,
            path_after.st_size,
            path_after.st_mtime_ns,
        )
        if (
            path_before_identity != before_identity
            or before_identity != after_identity
            or after_identity != path_identity
            or len(data) != after.st_size
        ):
            raise RevisionBindingError(
                "REV-VERSION-DRIFT",
                "VERSION changed while it was being read",
                operation="read-version",
            )
        try:
            value = data.decode("utf-8", errors="strict").strip()
        except UnicodeError as error:
            raise RevisionBindingError(
                "REV-VERSION-DECODE",
                "VERSION is not valid UTF-8",
                operation="read-version",
            ) from error
        if not value or len(value.splitlines()) != 1 or "\x00" in value:
            raise RevisionBindingError(
                "REV-VERSION-INVALID",
                "VERSION must contain one non-empty text line",
                operation="read-version",
            )
        return _VersionRead(value=value, identity=after_identity)

    @staticmethod
    def _parse_tags(output: bytes) -> tuple[TagBinding, ...]:
        tags: list[TagBinding] = []
        try:
            text = output.decode("utf-8", errors="strict")
            for line in text.splitlines():
                if not line:
                    continue
                name, object_id, peeled_id = line.split("\x00")
                tags.append(
                    TagBinding(name=name, peeled_commit=peeled_id or object_id)
                )
        except (UnicodeError, ValueError) as error:
            raise RevisionBindingError(
                "REV-TAGS-PARSE",
                "Git returned malformed tag identity data",
                operation="git-tags",
            ) from error
        return tuple(sorted(tags, key=lambda item: item.name))

    def _git_text(self, root: Path, *arguments: str) -> str:
        output = self._git(root, *arguments)
        try:
            value = output.decode("utf-8", errors="strict").strip()
        except UnicodeError as error:
            raise RevisionBindingError(
                "REV-GIT-DECODE",
                "Git identity output is not valid UTF-8",
                operation="git-" + arguments[0],
            ) from error
        if not value:
            raise RevisionBindingError(
                "REV-GIT-EMPTY",
                "Git identity command returned empty output",
                operation="git-" + arguments[0],
            )
        return value

    def _git(self, root: Path, *arguments: str) -> bytes:
        command = (
            self._git_executable,
            "-c", "core.hooksPath=/dev/null",
            "-c", "core.fsmonitor=false",
            "-c", "credential.helper=",
            *arguments,
        )
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_OPTIONAL_LOCKS": "0",
                "GIT_TERMINAL_PROMPT": "0",
                "GIT_ASKPASS": "",
                "SSH_ASKPASS": "",
            }
        )
        try:
            result = subprocess.run(
                command,
                cwd=root,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=_GIT_TIMEOUT_SECONDS,
            )
        except FileNotFoundError as error:
            raise RevisionBindingError(
                "REV-GIT-UNAVAILABLE", str(error), operation="git-exec"
            ) from error
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RevisionBindingError(
                "REV-GIT-EXEC", str(error), operation="git-exec"
            ) from error
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            raise RevisionBindingError(
                "REV-GIT-COMMAND",
                detail or f"Git exited with status {result.returncode}",
                operation="git-" + arguments[0],
            )
        return result.stdout


def bind(
    repo_root: Path,
    assessment_output_paths: Sequence[Path] = (),
    clock: Callable[[], datetime] | None = None,
    *,
    fingerprint_provider: FingerprintProvider | None = None,
) -> AssessmentRevision:
    """Convenience API matching the design's Revision Binder interface."""

    return RevisionBinder(fingerprint_provider=fingerprint_provider).bind(
        repo_root, assessment_output_paths, clock
    )
