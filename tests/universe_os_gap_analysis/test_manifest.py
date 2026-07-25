"""Unit tests for the artifact manifest and its atomic publish (Task 12.2).

These tests exercise Requirements 1.1, 9.7, and 14.1-14.7 against the real
:mod:`tools.universe_os_gap_analysis.manifest` builder and the real
:func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment`
transaction (no mocks). They confirm:

* the manifest lists *every* published artifact with its correct SHA-256 digest
  and byte size, and never digests itself;
* the manifest binds the artifact set to the bound revision fingerprint;
* an atomic publish writes the manifest together with every other artifact, and
  the recorded digests match the bytes actually written to disk;
* a manifest builder that fails (raises / collides / cites a foreign fact) fails
  the whole publish closed; and
* a failed publish leaves a prior valid assessment (and its manifest) intact and
  cleans up staging.
"""

from __future__ import annotations

import hashlib
import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.manifest import (
    DIGEST_ALGORITHM,
    MANIFEST_ARTIFACT_NAME,
    MANIFEST_SCHEMA_VERSION,
    artifact_digest,
    build_artifact_manifest,
    build_manifest_document,
)
from tools.universe_os_gap_analysis.model_builder import (
    RPT_DUPLICATE_ARTIFACT,
    RPT_MANIFEST_FAILED,
    RPT_PARITY_FOREIGN_FACT,
    RenderedArtifact,
    canonical_reference_ids,
    publish_assessment,
)
from tools.universe_os_gap_analysis.models import AssessmentModel

from .test_validator import build_valid_model


def _codes(result) -> set[str]:
    return {finding.code for finding in result.findings}


def _json_renderer(model: AssessmentModel) -> RenderedArtifact:
    return RenderedArtifact(
        name="assessment.json",
        content=b'{"hello": "world"}',
        projected_ids=canonical_reference_ids(model),
    )


def _matrix_renderer(model: AssessmentModel) -> RenderedArtifact:
    ids = frozenset(str(domain.id) for domain in model.domains)
    return RenderedArtifact(
        name="capability_matrix.csv",
        content=b"domainId\n",
        projected_ids=ids,
        required_ids=ids,
    )


def _read_manifest(result) -> dict:
    for artifact in result.artifacts:
        if artifact.name == MANIFEST_ARTIFACT_NAME:
            return json.loads(artifact.content.decode("utf-8"))
    raise AssertionError("no manifest artifact was produced")


class ManifestDocumentTests(unittest.TestCase):
    def test_lists_all_artifacts_with_correct_digests(self) -> None:
        model = build_valid_model()
        artifacts = (_json_renderer(model), _matrix_renderer(model))
        document = build_manifest_document(model, artifacts)

        by_name = {entry["name"]: entry for entry in document["artifacts"]}
        self.assertEqual(
            set(by_name), {"assessment.json", "capability_matrix.csv"}
        )
        for artifact in artifacts:
            entry = by_name[artifact.name]
            self.assertEqual(
                entry["sha256"],
                hashlib.sha256(artifact.content).hexdigest(),
            )
            self.assertEqual(entry["sizeBytes"], len(artifact.content))
        self.assertEqual(document["digestAlgorithm"], DIGEST_ALGORITHM)
        self.assertEqual(document["schemaVersion"], MANIFEST_SCHEMA_VERSION)

    def test_binds_revision_fingerprint(self) -> None:
        model = build_valid_model()
        document = build_manifest_document(model, (_json_renderer(model),))
        revision = model.revision
        bound = document["revision"]
        self.assertEqual(
            bound["repositoryRootId"], str(revision.repository_root_id)
        )
        self.assertEqual(bound["commitId"], revision.commit_id)
        self.assertEqual(
            bound["fingerprintAlgorithm"], revision.fingerprint_algorithm
        )
        self.assertEqual(
            bound["worktreeFingerprint"], revision.worktree_fingerprint
        )
        self.assertEqual(bound["trackedDiffHash"], revision.tracked_diff_hash)
        self.assertEqual(
            bound["untrackedPathSetHash"], revision.untracked_path_set_hash
        )

    def test_manifest_never_digests_itself(self) -> None:
        model = build_valid_model()
        # Feed a prior manifest artifact into the builder; it must be skipped.
        prior = build_artifact_manifest(model, (_json_renderer(model),))
        document = build_manifest_document(model, (_json_renderer(model), prior))
        names = {entry["name"] for entry in document["artifacts"]}
        self.assertNotIn(MANIFEST_ARTIFACT_NAME, names)

    def test_entries_are_sorted_by_name(self) -> None:
        model = build_valid_model()
        artifacts = (_matrix_renderer(model), _json_renderer(model))
        document = build_manifest_document(model, artifacts)
        names = [entry["name"] for entry in document["artifacts"]]
        self.assertEqual(names, sorted(names))

    def test_artifact_digest_matches_hashlib(self) -> None:
        self.assertEqual(
            artifact_digest(b"abc"), hashlib.sha256(b"abc").hexdigest()
        )

    def test_builder_projects_only_the_repository_root(self) -> None:
        model = build_valid_model()
        manifest = build_artifact_manifest(model, (_json_renderer(model),))
        self.assertEqual(
            manifest.projected_ids,
            frozenset({str(model.revision.repository_root_id)}),
        )
        self.assertLessEqual(
            manifest.projected_ids, canonical_reference_ids(model)
        )


class AtomicPublishWithManifestTests(unittest.TestCase):
    def test_publish_writes_manifest_and_all_artifacts(self) -> None:
        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model,
                (_json_renderer, _matrix_renderer),
                output_dir=tmp,
                manifest_builder=build_artifact_manifest,
            )
            self.assertTrue(result.published, msg=f"{result.findings}")
            on_disk = {child.name for child in Path(tmp).iterdir()}
            self.assertEqual(
                on_disk,
                {
                    "assessment.json",
                    "capability_matrix.csv",
                    MANIFEST_ARTIFACT_NAME,
                },
            )
            # The recorded digests describe exactly the bytes on disk.
            manifest = json.loads(
                (Path(tmp) / MANIFEST_ARTIFACT_NAME).read_bytes().decode("utf-8")
            )
            for entry in manifest["artifacts"]:
                written = (Path(tmp) / entry["name"]).read_bytes()
                self.assertEqual(
                    entry["sha256"], hashlib.sha256(written).hexdigest()
                )

    def test_manifest_absent_when_no_builder_supplied(self) -> None:
        model = build_valid_model()
        result = publish_assessment(model, (_json_renderer,))
        names = {artifact.name for artifact in result.artifacts}
        self.assertNotIn(MANIFEST_ARTIFACT_NAME, names)

    def test_dry_run_buffers_manifest_without_writing(self) -> None:
        model = build_valid_model()
        result = publish_assessment(
            model,
            (_json_renderer,),
            manifest_builder=build_artifact_manifest,
        )
        self.assertTrue(result.published, msg=f"{result.findings}")
        self.assertEqual(result.written_paths, ())
        names = {artifact.name for artifact in result.artifacts}
        self.assertIn(MANIFEST_ARTIFACT_NAME, names)


class ManifestFailClosedTests(unittest.TestCase):
    def test_manifest_builder_that_raises_fails_closed(self) -> None:
        def boom(_model, _artifacts) -> RenderedArtifact:
            raise RuntimeError("manifest blew up")

        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model, (_json_renderer,), output_dir=tmp, manifest_builder=boom
            )
            self.assertFalse(result.published)
            self.assertIn(RPT_MANIFEST_FAILED, _codes(result))
            self.assertEqual(result.written_paths, ())
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_manifest_name_collision_fails_closed(self) -> None:
        def collide(model, _artifacts) -> RenderedArtifact:
            return RenderedArtifact(
                name="assessment.json",
                content=b"{}",
                projected_ids=frozenset(
                    {str(model.revision.repository_root_id)}
                ),
            )

        model = build_valid_model()
        result = publish_assessment(
            model, (_json_renderer,), manifest_builder=collide
        )
        self.assertFalse(result.published)
        self.assertIn(RPT_DUPLICATE_ARTIFACT, _codes(result))

    def test_manifest_foreign_fact_fails_closed(self) -> None:
        def foreign(_model, _artifacts) -> RenderedArtifact:
            return RenderedArtifact(
                name=MANIFEST_ARTIFACT_NAME,
                content=b"{}",
                projected_ids=frozenset({"totally-made-up-id"}),
            )

        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            result = publish_assessment(
                model, (_json_renderer,), output_dir=tmp, manifest_builder=foreign
            )
            self.assertFalse(result.published)
            self.assertIn(RPT_PARITY_FOREIGN_FACT, _codes(result))
            self.assertEqual(list(Path(tmp).iterdir()), [])

    def test_failed_publish_preserves_prior_manifest(self) -> None:
        model = build_valid_model()
        with TemporaryDirectory() as tmp:
            first = publish_assessment(
                model,
                (_json_renderer,),
                output_dir=tmp,
                manifest_builder=build_artifact_manifest,
            )
            self.assertTrue(first.published)
            prior_manifest = (Path(tmp) / MANIFEST_ARTIFACT_NAME).read_bytes()

            def boom(_model, _artifacts) -> RenderedArtifact:
                raise RuntimeError("later manifest failed")

            second = publish_assessment(
                model, (_json_renderer,), output_dir=tmp, manifest_builder=boom
            )
            self.assertFalse(second.published)
            self.assertEqual(second.written_paths, ())
            self.assertEqual(
                (Path(tmp) / MANIFEST_ARTIFACT_NAME).read_bytes(), prior_manifest
            )
            leftovers = [
                child.name
                for child in Path(tmp).iterdir()
                if child.name.startswith(".assessment-staging-")
            ]
            self.assertEqual(leftovers, [])


if __name__ == "__main__":
    unittest.main()
