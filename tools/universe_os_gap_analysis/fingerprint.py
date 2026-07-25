"""Deterministic, fail-closed fingerprinting of a Git worktree."""

from __future__ import annotations

import hashlib
import os
import stat
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .identifiers import RepositoryPath
from .models import ExcludedPath
from .revision import FingerprintCapture, RevisionBindingError

FINGERPRINT_ALGORITHM = "sha256-length-prefixed-worktree-v1"
OUTPUT_EXCLUSION_RULE_VERSION = "assessment-output-exclusion-v1"
OUTPUT_EXCLUSION_REASON = (
    "explicit assessment output excluded to prevent fingerprint self-reference"
)
_GIT_TIMEOUT_SECONDS = 10
_LENGTH_BYTES = 8
_MODE_BYTES = 4
_KIND_CODE = {"missing": 0, "file": 1, "symlink": 2}
_SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".java", ".js", ".jsx", ".nb", ".py", ".rs", ".ts", ".tsx",
}


@dataclass(frozen=True, slots=True)
class _GitState:
    tracked: tuple[bytes, ...]
    untracked: tuple[bytes, ...]
    tracked_diff: bytes


@dataclass(frozen=True, slots=True)
class _EntrySnapshot:
    path: bytes
    kind: str
    mode: int
    content_length: int
    content_digest: bytes
    identity: tuple[int, ...] | None

class WorktreeFingerprintProvider:
    """Hash tracked and non-ignored untracked files without following links."""

    def __init__(self, *, git_executable: str = "git") -> None:
        self._git_executable = git_executable

    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> FingerprintCapture:
        root = self._validate_root(repo_root)
        before = self._git_state(root)
        exclusions = self._normalize_exclusions(
            root, assessment_output_paths, before.tracked, before.untracked
        )
        prefixes = tuple(str(item.path).encode("utf-8") for item in exclusions)
        before = self._included_state(before, prefixes)
        paths = tuple(sorted((*before.tracked, *before.untracked)))
        if len(paths) != len(set(paths)):
            self._fail("REV-PATH-DUPLICATE", "Git returned duplicate worktree paths", "enumerate-paths")

        first = self._snapshot(root, paths, frozenset(before.tracked))
        middle = self._included_state(self._git_state(root), prefixes)
        if middle != before:
            self._drift("Git path or tracked-diff state changed during collection")
        second = self._snapshot(root, paths, frozenset(before.tracked))
        after = self._included_state(self._git_state(root), prefixes)
        if after != middle or second != first:
            self._drift("worktree file content or Git state changed during collection")

        return FingerprintCapture(
            algorithm=FINGERPRINT_ALGORITHM,
            worktree_fingerprint=self._worktree_hash(second),
            tracked_diff_hash=hashlib.sha256(after.tracked_diff).hexdigest(),
            untracked_path_set_hash=self._path_set_hash(after.untracked),
            excluded_paths=exclusions,
        )

    @staticmethod
    def _validate_root(repo_root: Path) -> Path:
        try:
            root = repo_root.resolve(strict=True)
        except OSError as error:
            raise RevisionBindingError(
                "REV-ROOT-READ", str(error), operation="fingerprint-root"
            ) from error
        if not root.is_dir():
            raise RevisionBindingError(
                "REV-ROOT-INVALID", "fingerprint root is not a directory",
                operation="fingerprint-root",
            )
        return root

    def _git_state(self, root: Path) -> _GitState:
        tracked = self._parse_paths(
            self._git(root, "ls-files", "-c", "-z", "--"), "tracked"
        )
        untracked = self._parse_paths(
            self._git(root, "ls-files", "-o", "--exclude-standard", "-z", "--"),
            "untracked",
        )
        diff = self._git(
            root, "diff", "--binary", "--no-ext-diff", "--no-textconv", "HEAD", "--"
        )
        return _GitState(tracked=tracked, untracked=untracked, tracked_diff=diff)

    @staticmethod
    def _parse_paths(output: bytes, label: str) -> tuple[bytes, ...]:
        if output and not output.endswith(b"\0"):
            raise RevisionBindingError(
                "REV-GIT-PATHS", f"Git returned malformed {label} path data",
                operation="enumerate-paths",
            )
        paths = output[:-1].split(b"\0") if output else []
        normalized: list[bytes] = []
        for raw in paths:
            try:
                text = raw.decode("utf-8", errors="strict")
                RepositoryPath(text)
            except (UnicodeError, TypeError, ValueError) as error:
                raise RevisionBindingError(
                    "REV-PATH-INVALID",
                    f"Git returned a non-UTF-8 or unsafe {label} path",
                    operation="enumerate-paths",
                ) from error
            if text.encode("utf-8") != raw:
                raise RevisionBindingError(
                    "REV-PATH-INVALID", f"Git returned a non-canonical {label} path",
                    operation="enumerate-paths",
                )
            normalized.append(raw)
        if len(normalized) != len(set(normalized)):
            raise RevisionBindingError(
                "REV-PATH-DUPLICATE", f"Git returned duplicate {label} paths",
                operation="enumerate-paths",
            )
        return tuple(sorted(normalized))

    def _normalize_exclusions(
        self,
        root: Path,
        output_paths: Sequence[Path],
        tracked: tuple[bytes, ...],
        untracked: tuple[bytes, ...],
    ) -> tuple[ExcludedPath, ...]:
        relative_paths: set[str] = set()
        for supplied in output_paths:
            candidate = supplied if supplied.is_absolute() else root / supplied
            try:
                # Canonicalize the parent (including platform aliases such as
                # /var -> /private/var) but deliberately do not resolve the
                # final component before deciding whether it started in-repo.
                lexical = candidate.parent.resolve(strict=False) / candidate.name
            except OSError as error:
                self._fail("REV-EXCLUSION-READ", str(error), "normalize-exclusions", error)
            try:
                lexical.relative_to(root)
                lexically_inside = True
            except ValueError:
                lexically_inside = False
            try:
                # Remember the unresolved final component. Checking ``resolved``
                # would make every symlink appear to be its target and could let
                # an output exclusion hide unrelated in-repository content.
                final_is_symlink = lexical.is_symlink()
                resolved = candidate.resolve(strict=False)
            except OSError as error:
                self._fail("REV-EXCLUSION-READ", str(error), "normalize-exclusions", error)
            try:
                relative = resolved.relative_to(root)
            except ValueError:
                if lexically_inside:
                    self._fail(
                        "REV-PATH-ESCAPE", "assessment output resolves outside repository",
                        "normalize-exclusions",
                    )
                continue
            if final_is_symlink:
                self._fail(
                    "REV-EXCLUSION-INVALID",
                    "assessment output path must not be a symbolic link",
                    "normalize-exclusions",
                )
            if relative == Path("."):
                self._fail(
                    "REV-EXCLUSION-ROOT", "repository root cannot be excluded",
                    "normalize-exclusions",
                )
            if resolved.exists() and not resolved.is_dir():
                self._fail(
                    "REV-EXCLUSION-INVALID",
                    "assessment output path must be a directory",
                    "normalize-exclusions",
                )
            relative_paths.add(relative.as_posix())

        encoded = tuple(path.encode("utf-8") for path in sorted(relative_paths))
        for prefix in encoded:
            if any(self._under(path, prefix) for path in tracked):
                self._fail(
                    "REV-EXCLUSION-PRODUCT-SOURCE",
                    "assessment output exclusion overlaps tracked repository content",
                    "normalize-exclusions",
                )
            excluded_untracked = (path for path in untracked if self._under(path, prefix))
            if any(Path(path.decode("utf-8")).suffix.lower() in _SOURCE_SUFFIXES for path in excluded_untracked):
                self._fail(
                    "REV-EXCLUSION-PRODUCT-SOURCE",
                    "assessment output exclusion contains source-like repository content",
                    "normalize-exclusions",
                )
        return tuple(
            ExcludedPath(
                path=path,
                reason=OUTPUT_EXCLUSION_REASON,
                rule_version=OUTPUT_EXCLUSION_RULE_VERSION,
            )
            for path in sorted(relative_paths)
        )

    @classmethod
    def _included_state(cls, state: _GitState, prefixes: tuple[bytes, ...]) -> _GitState:
        def included(path: bytes) -> bool:
            return not any(cls._under(path, prefix) for prefix in prefixes)

        return _GitState(
            tracked=tuple(path for path in state.tracked if included(path)),
            untracked=tuple(path for path in state.untracked if included(path)),
            tracked_diff=state.tracked_diff,
        )

    @staticmethod
    def _under(path: bytes, prefix: bytes) -> bool:
        return path == prefix or path.startswith(prefix + b"/")

    def _snapshot(
        self, root: Path, paths: tuple[bytes, ...], tracked: frozenset[bytes]
    ) -> tuple[_EntrySnapshot, ...]:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
        try:
            root_fd = os.open(root, flags | getattr(os, "O_DIRECTORY", 0))
        except OSError as error:
            self._fail("REV-FINGERPRINT-READ", str(error), "open-repository", error)
        try:
            return tuple(self._read_entry(root_fd, path, path in tracked) for path in paths)
        finally:
            os.close(root_fd)

    def _read_entry(self, root_fd: int, path: bytes, is_tracked: bool) -> _EntrySnapshot:
        components = path.split(b"/")
        parent_fd = os.dup(root_fd)
        try:
            directory_flags = (
                os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)
            )
            for component in components[:-1]:
                try:
                    next_fd = os.open(component, directory_flags, dir_fd=parent_fd)
                except OSError as error:
                    self._fail(
                        "REV-PATH-ESCAPE", f"unsafe parent for {path!r}: {error}",
                        "read-worktree-entry", error,
                    )
                os.close(parent_fd)
                parent_fd = next_fd
            name = components[-1]
            try:
                before = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
            except FileNotFoundError as error:
                if is_tracked:
                    return _EntrySnapshot(
                        path, "missing", 0, 0, hashlib.sha256(b"").digest(), None
                    )
                self._fail("REV-FINGERPRINT-DRIFT", str(error), "read-worktree-entry", error)
            except OSError as error:
                self._fail("REV-FINGERPRINT-READ", str(error), "read-worktree-entry", error)

            if stat.S_ISREG(before.st_mode):
                return self._read_regular(parent_fd, name, path, before)
            if stat.S_ISLNK(before.st_mode):
                return self._read_symlink(parent_fd, name, path, before)
            self._fail(
                "REV-FILE-KIND",
                f"unsupported worktree entry kind for {path.decode('utf-8')}",
                "read-worktree-entry",
            )
        finally:
            os.close(parent_fd)

    def _read_regular(
        self, parent_fd: int, name: bytes, path: bytes, before: os.stat_result
    ) -> _EntrySnapshot:
        flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(name, flags, dir_fd=parent_fd)
        except OSError as error:
            self._fail("REV-FINGERPRINT-READ", str(error), "read-file", error)
        try:
            opened = os.fstat(descriptor)
            digest = hashlib.sha256()
            length = 0
            while True:
                chunk = os.read(descriptor, 1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                length += len(chunk)
            after = os.fstat(descriptor)
            path_after = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError as error:
            self._fail("REV-FINGERPRINT-READ", str(error), "read-file", error)
        finally:
            os.close(descriptor)
        identities = tuple(self._identity(value) for value in (before, opened, after, path_after))
        if len(set(identities)) != 1 or length != after.st_size:
            self._drift(f"file changed while reading {path.decode('utf-8')}")
        return _EntrySnapshot(
            path, "file", stat.S_IMODE(after.st_mode), length, digest.digest(), identities[-1]
        )

    def _read_symlink(
        self, parent_fd: int, name: bytes, path: bytes, before: os.stat_result
    ) -> _EntrySnapshot:
        try:
            target = os.readlink(name, dir_fd=parent_fd)
            after = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        except OSError as error:
            self._fail("REV-FINGERPRINT-READ", str(error), "read-symlink", error)
        if not isinstance(target, bytes):
            self._fail(
                "REV-FINGERPRINT-READ", "symlink target was not returned as bytes",
                "read-symlink",
            )
        before_identity = self._identity(before)
        after_identity = self._identity(after)
        if before_identity != after_identity or len(target) != after.st_size:
            self._drift(f"symbolic link changed while reading {path.decode('utf-8')}")
        return _EntrySnapshot(
            path, "symlink", stat.S_IMODE(after.st_mode), len(target),
            hashlib.sha256(target).digest(), after_identity,
        )

    @staticmethod
    def _identity(value: os.stat_result) -> tuple[int, ...]:
        return (
            value.st_dev, value.st_ino, value.st_mode, value.st_size,
            value.st_mtime_ns, value.st_ctime_ns,
        )

    @staticmethod
    def _worktree_hash(entries: Iterable[_EntrySnapshot]) -> str:
        digest = hashlib.sha256()
        for entry in entries:
            path_length = len(entry.path).to_bytes(_LENGTH_BYTES, "big")
            content_length = entry.content_length.to_bytes(_LENGTH_BYTES, "big")
            mode = entry.mode.to_bytes(_MODE_BYTES, "big")
            digest.update(path_length)
            digest.update(entry.path)
            digest.update(bytes((_KIND_CODE[entry.kind],)))
            digest.update(mode)
            digest.update(content_length)
            digest.update(entry.content_digest)
        return digest.hexdigest()

    @staticmethod
    def _path_set_hash(paths: Iterable[bytes]) -> str:
        digest = hashlib.sha256()
        for path in paths:
            digest.update(len(path).to_bytes(_LENGTH_BYTES, "big"))
            digest.update(path)
        return digest.hexdigest()

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
                "GIT_OPTIONAL_LOCKS": "0", "GIT_TERMINAL_PROMPT": "0",
                "GIT_ASKPASS": "", "SSH_ASKPASS": "",
            }
        )
        try:
            result = subprocess.run(
                command, cwd=root, env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
                timeout=_GIT_TIMEOUT_SECONDS,
            )
        except FileNotFoundError as error:
            self._fail("REV-GIT-UNAVAILABLE", str(error), "fingerprint-git", error)
        except (OSError, subprocess.TimeoutExpired) as error:
            self._fail("REV-GIT-EXEC", str(error), "fingerprint-git", error)
        if result.returncode != 0:
            detail = result.stderr.decode("utf-8", errors="replace").strip()
            self._fail(
                "REV-GIT-COMMAND",
                detail or f"Git exited with status {result.returncode}",
                "fingerprint-git",
            )
        return result.stdout

    @staticmethod
    def _drift(message: str) -> None:
        raise RevisionBindingError(
            "REV-FINGERPRINT-DRIFT", message, operation="fingerprint-stability-check"
        )

    @staticmethod
    def _fail(
        code: str, message: str, operation: str, cause: BaseException | None = None
    ) -> None:
        error = RevisionBindingError(code, message, operation=operation)
        if cause is None:
            raise error
        raise error from cause
