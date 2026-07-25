"""Artifact manifest builder for the atomic assessment publish (Task 12.2).

The published assessment is a *set* of artifacts -- ``assessment.json``, the
JSON Schema, the capability matrix (CSV/JSON), the gap register (CSV/JSON), and
the narrative ``assessment.md`` -- all projected losslessly from the single
canonical :class:`~tools.universe_os_gap_analysis.models.AssessmentModel`. This
module adds the final member of that set: an **artifact manifest** that lists
every published artifact together with its content digest and binds the whole
set to the bound revision's worktree fingerprint (Requirement 1.1, 9.7,
14.1-14.7).

Unlike every other renderer, the manifest cannot conform to the plain
``Renderer = Callable[[AssessmentModel], RenderedArtifact]`` contract, because it
must digest the *bytes of the other artifacts* -- information a renderer that
only sees the model does not have. Instead this module exposes a
:data:`ManifestBuilder` (``Callable[[AssessmentModel, Sequence[RenderedArtifact]],
RenderedArtifact]``) that
:func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment` invokes
*inside* the staging window: after every other artifact has been rendered and
parity-checked, but before anything is committed. The manifest it produces joins
the same all-or-nothing commit, so the digests it records always describe the
exact bytes that land on disk (or nothing lands at all).

The manifest is a deterministic JSON document:

* ``schemaVersion`` -- the manifest document contract version;
* ``revision`` -- the bound revision fingerprint fields (repository root id,
  commit, fingerprint algorithm, worktree fingerprint, tracked diff hash,
  untracked path-set hash), so the artifact set is bound to exactly the revision
  it was produced from;
* ``digestAlgorithm`` -- ``sha256``;
* ``artifacts`` -- one entry per published artifact ``{name, sha256, sizeBytes}``,
  sorted by name for byte-for-byte reproducibility.

The manifest never digests itself (that would be circular); it describes only the
other artifacts in the publish set.
"""

from __future__ import annotations

import hashlib
from typing import Any, Callable, Sequence

from .model_builder import RenderedArtifact
from .models import AssessmentModel
from .serialization import stable_json_bytes

# Stable file name and versioned contract of the manifest document.
MANIFEST_ARTIFACT_NAME = "assessment.manifest.json"
MANIFEST_SCHEMA_VERSION = "1.0.0"

# The digest algorithm used for every per-artifact content digest.
DIGEST_ALGORITHM = "sha256"

# A manifest builder digests the already-rendered artifacts and binds them to the
# canonical model's revision fingerprint. It runs inside the publish transaction,
# so it sees the exact bytes that will be committed.
ManifestBuilder = Callable[[AssessmentModel, Sequence[RenderedArtifact]], RenderedArtifact]


def artifact_digest(content: bytes) -> str:
    """Return the hex SHA-256 digest of ``content``.

    The digest is computed over the exact artifact bytes, so it pins precisely
    what is (or will be) written to disk.
    """

    if not isinstance(content, (bytes, bytearray)):
        raise TypeError("content must be bytes")
    return hashlib.sha256(bytes(content)).hexdigest()


def build_manifest_document(
    model: AssessmentModel, artifacts: Sequence[RenderedArtifact]
) -> dict[str, Any]:
    """Build the JSON-primitive manifest document for a rendered artifact set.

    Every artifact except the manifest itself is listed with its content digest
    and byte size, sorted by name. The document is bound to the model's revision
    fingerprint so the artifact set is traceable to exactly one revision
    (Requirement 1.1, 14.7).
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    revision = model.revision
    entries: list[dict[str, Any]] = []
    for artifact in artifacts:
        if not isinstance(artifact, RenderedArtifact):
            raise TypeError("artifacts must be RenderedArtifact values")
        if artifact.name == MANIFEST_ARTIFACT_NAME:
            # Defensive: the manifest never digests itself.
            continue
        entries.append(
            {
                "name": artifact.name,
                "sha256": artifact_digest(artifact.content),
                "sizeBytes": len(artifact.content),
            }
        )
    entries.sort(key=lambda entry: entry["name"])

    return {
        "schemaVersion": MANIFEST_SCHEMA_VERSION,
        "digestAlgorithm": DIGEST_ALGORITHM,
        "revision": {
            "repositoryRootId": str(revision.repository_root_id),
            "commitId": revision.commit_id,
            "fingerprintAlgorithm": revision.fingerprint_algorithm,
            "worktreeFingerprint": revision.worktree_fingerprint,
            "trackedDiffHash": revision.tracked_diff_hash,
            "untrackedPathSetHash": revision.untracked_path_set_hash,
        },
        "artifacts": entries,
    }


def build_artifact_manifest(
    model: AssessmentModel, artifacts: Sequence[RenderedArtifact]
) -> RenderedArtifact:
    """Render the artifact manifest as a :class:`RenderedArtifact`.

    This is the :data:`ManifestBuilder` wired into
    :func:`~tools.universe_os_gap_analysis.model_builder.publish_assessment`. It
    digests every other artifact in the publish set and binds the set to the
    revision fingerprint. ``projected_ids`` is exactly the repository-root id --
    the one canonical object the manifest references -- so the publish gate's
    parity check confirms the manifest introduces no foreign facts (Requirement
    14.7).
    """

    document = build_manifest_document(model, artifacts)
    content = stable_json_bytes(document, indent=2)
    return RenderedArtifact(
        name=MANIFEST_ARTIFACT_NAME,
        content=content,
        projected_ids=frozenset({str(model.revision.repository_root_id)}),
    )
