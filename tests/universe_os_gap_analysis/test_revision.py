from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

from tools.universe_os_gap_analysis import fingerprint as fingerprint_module
from tools.universe_os_gap_analysis.fingerprint import (
    FINGERPRINT_ALGORITHM,
    OUTPUT_EXCLUSION_REASON,
    OUTPUT_EXCLUSION_RULE_VERSION,
    WorktreeFingerprintProvider,
)
from tools.universe_os_gap_analysis.models import ExcludedPath, RevisionOrigin
from tools.universe_os_gap_analysis.revision import (
    FINGERPRINT_DEFERRED,
    FingerprintCapture,
    RevisionBinder,
    RevisionBindingError,
)


def _git(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ("git", *arguments),
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
        text=True,
    )
    return result.stdout.strip()


def _create_repository(root: Path) -> str:
    _git(root, "init", "-b", "main")
    _git(root, "config", "user.name", "Gap Analysis Tests")
    _git(root, "config", "user.email", "gap-tests@example.invalid")
    (root / "VERSION").write_text("1.2.3\n", encoding="utf-8")
    (root / "tracked.txt").write_text("baseline\n", encoding="utf-8")
    _git(root, "add", "VERSION", "tracked.txt")
    _git(root, "commit", "-m", "fixture baseline")
    return _git(root, "rev-parse", "HEAD")


class _FixedFingerprintProvider:
    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> FingerprintCapture:
        return FingerprintCapture(
            algorithm="test-fingerprint-v1",
            worktree_fingerprint="worktree-test-hash",
            tracked_diff_hash="diff-test-hash",
            untracked_path_set_hash="untracked-test-hash",
            excluded_paths=(
                ExcludedPath(
                    path="assessment/output",
                    reason="test output",
                    rule_version=OUTPUT_EXCLUSION_RULE_VERSION,
                ),
            ),
        )


class _DriftingFingerprintProvider:
    def capture(
        self, repo_root: Path, assessment_output_paths: tuple[Path, ...]
    ) -> FingerprintCapture:
        (repo_root / "VERSION").write_text("9.9.9\n", encoding="utf-8")
        return FingerprintCapture.deferred()


class RevisionBinderUnitTests(unittest.TestCase):
    def test_deferred_fingerprint_explicitly_reserves_task_2_2(self) -> None:
        capture = FingerprintCapture.deferred()

        self.assertEqual(capture.algorithm, FINGERPRINT_DEFERRED)
        self.assertEqual(capture.worktree_fingerprint, FINGERPRINT_DEFERRED)
        self.assertEqual(capture.excluded_paths, ())

    def test_tag_parser_handles_lightweight_and_annotated_tags(self) -> None:
        tags = RevisionBinder._parse_tags(
            b"lightweight\x00abc123\x00\nrelease\x00tag-object\x00def456\n"
        )

        self.assertEqual(
            tuple((tag.name, tag.peeled_commit) for tag in tags),
            (("lightweight", "abc123"), ("release", "def456")),
        )

    def test_tag_parser_returns_structured_rev_error_for_malformed_output(self) -> None:
        with self.assertRaises(RevisionBindingError) as raised:
            RevisionBinder._parse_tags(b"malformed-tag-record\n")

        self.assertEqual(raised.exception.code, "REV-TAGS-PARSE")
        self.assertEqual(raised.exception.operation, "git-tags")


class RevisionBinderIntegrationTests(unittest.TestCase):
    def test_binds_clean_tagged_repository_and_separates_evidence_axes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commit_id = _create_repository(root)
            _git(root, "tag", "-a", "v1.2.3", "-m", "release")
            timestamp = datetime(
                2025, 2, 3, 4, 5, 6,
                tzinfo=timezone(timedelta(hours=8)),
            )

            revision = RevisionBinder().bind(root / ".", clock=lambda: timestamp)

            self.assertEqual(revision.commit_id, commit_id)
            self.assertEqual(revision.branch, "main")
            self.assertEqual(revision.version, "1.2.3")
            self.assertEqual(revision.describe, "v1.2.3")
            self.assertEqual(revision.tags, ("v1.2.3",))
            self.assertTrue(revision.worktree_clean)
            self.assertEqual(
                revision.assessed_at_utc,
                datetime(2025, 2, 2, 20, 5, 6, tzinfo=timezone.utc),
            )
            self.assertEqual(revision.fingerprint_algorithm, FINGERPRINT_ALGORITHM)
            self.assertEqual(len(revision.worktree_fingerprint), 64)
            self.assertEqual(len(revision.tracked_diff_hash), 64)
            self.assertEqual(len(revision.untracked_path_set_hash), 64)

            axes = revision.evidence_axes
            self.assertIsNotNone(axes)
            assert axes is not None
            self.assertIs(axes.tagged_release.origin, RevisionOrigin.TAGGED_RELEASE)
            self.assertEqual(axes.tagged_release.tags[0].peeled_commit, commit_id)
            self.assertIs(
                axes.committed_revision.origin, RevisionOrigin.COMMITTED_REVISION
            )
            self.assertEqual(axes.committed_revision.commit_id, commit_id)
            self.assertIs(axes.current_worktree.origin, RevisionOrigin.CURRENT_WORKTREE)
            self.assertTrue(axes.current_worktree.worktree_clean)

    def test_commits_after_tag_keep_tagged_and_committed_axes_distinct(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tagged_commit = _create_repository(root)
            _git(root, "tag", "-a", "v1.2.3", "-m", "release")
            (root / "tracked.txt").write_text("second commit\n", encoding="utf-8")
            _git(root, "add", "tracked.txt")
            _git(root, "commit", "-m", "post-release change")
            head_commit = _git(root, "rev-parse", "HEAD")

            revision = RevisionBinder().bind(root)

            # HEAD advanced past the tagged release: the committed-revision axis
            # must point at the new commit while the tagged-release axis still
            # peels to the original release commit.
            self.assertNotEqual(head_commit, tagged_commit)
            self.assertEqual(revision.commit_id, head_commit)
            self.assertTrue(revision.describe.startswith("v1.2.3-"))
            self.assertIn(head_commit[:7], revision.describe)
            assert revision.evidence_axes is not None
            self.assertEqual(
                revision.evidence_axes.committed_revision.commit_id, head_commit
            )
            # No tag points at the advanced HEAD, so the tagged-release axis
            # carries no HEAD tag binding even though describe still names the
            # base release. This keeps release evidence from silently attaching
            # to a later commit.
            self.assertEqual(revision.tags, ())
            self.assertEqual(revision.evidence_axes.tagged_release.tags, ())
            self.assertEqual(
                revision.evidence_axes.tagged_release.describe, revision.describe
            )
            self.assertTrue(revision.worktree_clean)

    def test_dirty_tracked_and_untracked_files_only_mark_current_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commit_id = _create_repository(root)
            _git(root, "tag", "v1.2.3")
            (root / "tracked.txt").write_text("changed\n", encoding="utf-8")
            (root / "untracked.txt").write_text("local\n", encoding="utf-8")

            revision = RevisionBinder().bind(root)

            self.assertFalse(revision.worktree_clean)
            self.assertEqual(revision.describe, "v1.2.3")
            assert revision.evidence_axes is not None
            self.assertEqual(revision.evidence_axes.tagged_release.describe, "v1.2.3")
            self.assertEqual(
                revision.evidence_axes.tagged_release.tags[0].peeled_commit,
                commit_id,
            )
            self.assertEqual(
                revision.evidence_axes.committed_revision.commit_id, commit_id
            )
            self.assertFalse(
                revision.evidence_axes.current_worktree.worktree_clean
            )

    def test_assessment_timestamp_does_not_affect_content_fingerprints(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            earlier = datetime(2025, 1, 1, tzinfo=timezone.utc)
            later = datetime(2025, 6, 1, tzinfo=timezone.utc)

            first = RevisionBinder().bind(root, clock=lambda: earlier)
            second = RevisionBinder().bind(root, clock=lambda: later)

            self.assertNotEqual(first.assessed_at_utc, second.assessed_at_utc)
            self.assertEqual(first.worktree_fingerprint, second.worktree_fingerprint)
            self.assertEqual(first.tracked_diff_hash, second.tracked_diff_hash)
            self.assertEqual(
                first.untracked_path_set_hash,
                second.untracked_path_set_hash,
            )

    def test_repository_root_identity_read_failure_is_structured(self) -> None:
        with mock.patch.object(Path, "stat", side_effect=PermissionError("denied")):
            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder._repository_root_id(Path("/unreadable/repository"))

        self.assertEqual(raised.exception.code, "REV-ROOT-READ")
        self.assertEqual(raised.exception.operation, "repository-root-identity")

    def test_injected_fingerprint_provider_is_preserved(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            revision = RevisionBinder(
                fingerprint_provider=_FixedFingerprintProvider()
            ).bind(root, (root / "assessment" / "output",))

            self.assertEqual(revision.fingerprint_algorithm, "test-fingerprint-v1")
            self.assertEqual(revision.worktree_fingerprint, "worktree-test-hash")
            self.assertEqual(revision.tracked_diff_hash, "diff-test-hash")
            self.assertEqual(revision.untracked_path_set_hash, "untracked-test-hash")
            self.assertEqual(revision.excluded_paths[0].path, "assessment/output")

    def test_version_or_git_drift_fails_closed_without_partial_revision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            binder = RevisionBinder(
                fingerprint_provider=_DriftingFingerprintProvider()
            )

            with self.assertRaises(RevisionBindingError) as raised:
                binder.bind(root)

            self.assertEqual(raised.exception.code, "REV-DRIFT")
            self.assertEqual(
                raised.exception.to_dict(),
                {
                    "code": "REV-DRIFT",
                    "message": (
                        "repository Git or VERSION state changed during revision binding"
                    ),
                    "operation": "stability-check",
                },
            )

    def test_version_symlink_is_rejected_without_following_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            external = root.parent / f"{root.name}-external-version"
            external.write_text("7.7.7\n", encoding="utf-8")
            (root / "VERSION").unlink()
            try:
                (root / "VERSION").symlink_to(external)
            except OSError as error:
                external.unlink(missing_ok=True)
                self.skipTest(f"symbolic links unavailable: {error}")
            try:
                with self.assertRaises(RevisionBindingError) as raised:
                    RevisionBinder().bind(root)
                self.assertEqual(raised.exception.code, "REV-VERSION-READ")
            finally:
                external.unlink(missing_ok=True)

    def test_missing_version_returns_structured_rev_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            (root / "VERSION").unlink()

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder().bind(root)

            self.assertEqual(raised.exception.code, "REV-VERSION-READ")
            self.assertEqual(raised.exception.operation, "read-version")

    def test_missing_git_executable_returns_structured_rev_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder(git_executable="definitely-not-a-git-binary").bind(root)

            self.assertEqual(raised.exception.code, "REV-GIT-UNAVAILABLE")
            self.assertEqual(raised.exception.operation, "git-exec")

    def test_repository_root_identity_is_stable_per_root_and_differs_across_roots(self) -> None:
        with tempfile.TemporaryDirectory() as left_dir, tempfile.TemporaryDirectory() as right_dir:
            left = Path(left_dir)
            right = Path(right_dir)
            _create_repository(left)
            _create_repository(right)
            binder = RevisionBinder()

            first = binder.bind(left)
            second = binder.bind(left)
            other = binder.bind(right)

            self.assertEqual(first.repository_root_id, second.repository_root_id)
            self.assertNotEqual(first.repository_root_id, other.repository_root_id)


class WorktreeFingerprintIntegrationTests(unittest.TestCase):
    def test_tracked_and_untracked_hashes_are_independent(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            (root / "local.txt").write_text("one\n", encoding="utf-8")
            first = RevisionBinder().bind(root)

            (root / "local.txt").write_text("two\n", encoding="utf-8")
            second = RevisionBinder().bind(root)
            self.assertNotEqual(first.worktree_fingerprint, second.worktree_fingerprint)
            self.assertEqual(first.tracked_diff_hash, second.tracked_diff_hash)
            self.assertEqual(first.untracked_path_set_hash, second.untracked_path_set_hash)

            (root / "tracked.txt").write_text("changed\n", encoding="utf-8")
            third = RevisionBinder().bind(root)
            self.assertNotEqual(second.tracked_diff_hash, third.tracked_diff_hash)
            self.assertEqual(second.untracked_path_set_hash, third.untracked_path_set_hash)

    def test_untracked_path_set_uses_utf8_byte_order_and_length_prefixes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            names = ("é.txt", "z.txt", "alpha.txt")
            for name in names:
                (root / name).write_text(name, encoding="utf-8")

            revision = RevisionBinder().bind(root)
            digest = hashlib.sha256()
            for path in sorted(name.encode("utf-8") for name in names):
                digest.update(len(path).to_bytes(8, "big"))
                digest.update(path)
            self.assertEqual(revision.untracked_path_set_hash, digest.hexdigest())

    def test_explicit_output_is_excluded_and_records_rule_path_and_reason(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            output = root / "assessment"
            output.mkdir()
            artifact = output / "assessment.json"
            artifact.write_text('{"generation":1}\n', encoding="utf-8")

            first = RevisionBinder().bind(root, (output,))
            artifact.write_text('{"generation":2}\n', encoding="utf-8")
            second = RevisionBinder().bind(root, (output,))

            self.assertEqual(first.worktree_fingerprint, second.worktree_fingerprint)
            self.assertEqual(first.untracked_path_set_hash, second.untracked_path_set_hash)
            self.assertEqual(
                first.excluded_paths,
                (
                    ExcludedPath(
                        path="assessment",
                        reason=OUTPUT_EXCLUSION_REASON,
                        rule_version=OUTPUT_EXCLUSION_RULE_VERSION,
                    ),
                ),
            )

    def test_output_exclusion_cannot_hide_tracked_product_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            output = root / "assessment"
            output.mkdir()
            (output / "product.txt").write_text("tracked\n", encoding="utf-8")
            _git(root, "add", "assessment/product.txt")
            _git(root, "commit", "-m", "tracked product content")

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder().bind(root, (output,))
            self.assertEqual(raised.exception.code, "REV-EXCLUSION-PRODUCT-SOURCE")

    def test_output_exclusion_cannot_hide_untracked_source_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            output = root / "assessment"
            output.mkdir()
            # An untracked, source-like file inside a proposed output exclusion
            # must not be silently dropped from the fingerprint.
            (output / "smuggled.py").write_text("print('hi')\n", encoding="utf-8")

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder().bind(root, (output,))
            self.assertEqual(raised.exception.code, "REV-EXCLUSION-PRODUCT-SOURCE")

    def test_file_kind_participates_independently_of_content_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            # A regular file and a symbolic link that carry the *same* bytes must
            # still fingerprint differently because the file kind is encoded.
            (root / "regular").write_text("payload", encoding="utf-8")
            with_regular = RevisionBinder().bind(root)

            (root / "regular").unlink()
            try:
                (root / "regular").symlink_to("payload")
            except OSError as error:
                self.skipTest(f"symbolic links unavailable: {error}")
            with_symlink = RevisionBinder().bind(root)

            self.assertNotEqual(
                with_regular.worktree_fingerprint,
                with_symlink.worktree_fingerprint,
            )

    def test_output_symlink_escape_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            _create_repository(root)
            output = root / "assessment"
            try:
                output.symlink_to(Path(outside), target_is_directory=True)
            except OSError as error:
                self.skipTest(f"symbolic links unavailable: {error}")

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder().bind(root, (output,))
            self.assertEqual(raised.exception.code, "REV-PATH-ESCAPE")

    def test_output_symlink_cannot_hide_in_repository_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            target = root / "generated-assessment"
            target.mkdir()
            (target / "assessment.json").write_text("{}\n", encoding="utf-8")
            output = root / "assessment"
            try:
                output.symlink_to(target, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"symbolic links unavailable: {error}")

            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder().bind(root, (output,))
            self.assertEqual(raised.exception.code, "REV-EXCLUSION-INVALID")

    def test_symlink_hashes_link_bytes_without_following_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory() as outside:
            root = Path(directory)
            _create_repository(root)
            target = Path(outside) / "target.txt"
            target.write_text("first\n", encoding="utf-8")
            link = root / "external-link"
            try:
                link.symlink_to(target)
            except OSError as error:
                self.skipTest(f"symbolic links unavailable: {error}")

            first = RevisionBinder().bind(root)
            target.write_text("second and different\n", encoding="utf-8")
            second = RevisionBinder().bind(root)
            self.assertEqual(first.worktree_fingerprint, second.worktree_fingerprint)

    def test_mode_bits_participate_in_worktree_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            first = RevisionBinder().bind(root)
            tracked = root / "tracked.txt"
            tracked.chmod(tracked.stat().st_mode | 0o100)
            second = RevisionBinder().bind(root)
            self.assertNotEqual(first.worktree_fingerprint, second.worktree_fingerprint)

    def test_file_read_failure_is_a_structured_rev_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            original_open = os.open

            def fail_tracked(path: object, *args: object, **kwargs: object) -> int:
                if path == b"tracked.txt" and "dir_fd" in kwargs:
                    raise PermissionError("injected read denial")
                return original_open(path, *args, **kwargs)  # type: ignore[arg-type]

            with mock.patch.object(fingerprint_module.os, "open", side_effect=fail_tracked):
                with self.assertRaises(RevisionBindingError) as raised:
                    RevisionBinder().bind(root)
            self.assertEqual(raised.exception.code, "REV-FINGERPRINT-READ")

    def test_collection_drift_fails_closed(self) -> None:
        class MutatingProvider(WorktreeFingerprintProvider):
            def __init__(self) -> None:
                super().__init__()
                self.snapshots = 0

            def _snapshot(self, root: Path, paths: tuple[bytes, ...], tracked: frozenset[bytes]):  # type: ignore[no-untyped-def]
                result = super()._snapshot(root, paths, tracked)
                self.snapshots += 1
                if self.snapshots == 1:
                    (root / "tracked.txt").write_text("drifted\n", encoding="utf-8")
                return result

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _create_repository(root)
            with self.assertRaises(RevisionBindingError) as raised:
                RevisionBinder(fingerprint_provider=MutatingProvider()).bind(root)
            self.assertEqual(raised.exception.code, "REV-FINGERPRINT-DRIFT")


if __name__ == "__main__":
    unittest.main()
