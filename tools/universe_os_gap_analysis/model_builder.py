"""Canonical model builder and all-or-nothing publish transaction (Task 10.2).

This module has two responsibilities and *only* these two:

1. **Canonical model assembly.** :func:`build_assessment_model` aggregates the
   independently produced assessment inputs -- the bound revision, source
   inventory, evidence records, conflicts, six-level target model, capability
   domains, capability assessments, gaps, Hard-Gates, trust assumptions,
   non-claims, observed conclusions, and recommendations -- into the *single*
   canonical :class:`~tools.universe_os_gap_analysis.models.AssessmentModel`.
   Every downstream artifact must be a lossless projection of this one model, so
   there is exactly one source of truth (Requirement 14.7).

2. **Fail-closed publish transaction.** :func:`publish_assessment` is the single
   publish gate that stands between a canonical model and its rendered
   artifacts. It runs the Task 10.1 validator *first* and refuses to emit *any*
   artifact when validation fails; it then renders every artifact into memory,
   enforces cross-artifact reference parity, and only commits the whole set to
   the output directory once *all* checks pass. If anything fails -- validation,
   a renderer raising, or a parity mismatch -- nothing is written and any prior
   valid assessment is left untouched (all-or-nothing; Requirement 9.7, 14.1).

This module reuses the Task 10.1 validator wholesale
(:func:`~tools.universe_os_gap_analysis.validator.validate_assessment_model`);
it never reimplements validation, mutates evidence, upgrades a status, or edits
any evaluator/product code. The concrete JSON/table/Markdown renderers are Task
11; here we only define the renderer *interface* (:class:`RenderedArtifact` and
the ``Renderer`` callable) and enforce the atomic, parity-checked publish
contract that those renderers must satisfy.

Fail-closed publish findings use the ``RPT-*`` error-code family (report/publish
concerns), mirroring the validator's conventions:

* ``RPT-PUBLISH-INVALID-MODEL`` -- the model failed validation, so no artifact
  may be published (the underlying validator findings are carried alongside).
* ``RPT-RENDER-FAILED`` -- a renderer raised while producing its artifact.
* ``RPT-DUPLICATE-ARTIFACT`` -- two renderers claimed the same artifact name.
* ``RPT-PARITY-FOREIGN-FACT`` -- an artifact references an identifier that does
  not exist in the canonical model (a renderer must never introduce facts that
  are not in the model; Requirement 14.7).
* ``RPT-PARITY-MISSING-PROJECTION`` -- an artifact that declared an exact
  required projection did not project it losslessly (Requirement 14.2, 14.3).
"""

from __future__ import annotations

import dataclasses
import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Callable, Iterable, Sequence

if TYPE_CHECKING:  # pragma: no cover - typing only, avoids an import cycle.
    from .manifest import ManifestBuilder

from .models import (
    AssessmentModel,
    AssessmentRevision,
    CapabilityAssessment,
    CapabilityDomain,
    EvidenceConflict,
    EvidenceRecord,
    FindingSeverity,
    GapEntry,
    HardGate,
    ObservedConclusion,
    Recommendation,
    SourceInventoryEntry,
    TargetLevel,
    ValidationFinding,
    ValidationResult,
)
from .validator import validate_assessment_model

# --------------------------------------------------------------------------- #
# RPT-* : fail-closed publish-transaction error codes.                         #
# --------------------------------------------------------------------------- #
RPT_PUBLISH_INVALID_MODEL = "RPT-PUBLISH-INVALID-MODEL"
RPT_RENDER_FAILED = "RPT-RENDER-FAILED"
RPT_DUPLICATE_ARTIFACT = "RPT-DUPLICATE-ARTIFACT"
RPT_PARITY_FOREIGN_FACT = "RPT-PARITY-FOREIGN-FACT"
RPT_PARITY_MISSING_PROJECTION = "RPT-PARITY-MISSING-PROJECTION"
RPT_MANIFEST_FAILED = "RPT-MANIFEST-FAILED"


def build_assessment_model(
    *,
    revision: AssessmentRevision,
    source_inventory: Iterable[SourceInventoryEntry] = (),
    evidence_records: Iterable[EvidenceRecord] = (),
    conflicts: Iterable[EvidenceConflict] = (),
    target_levels: Iterable[TargetLevel] | None = None,
    domains: Iterable[CapabilityDomain] = (),
    assessments: Iterable[CapabilityAssessment] = (),
    gaps: Iterable[GapEntry] = (),
    hard_gates: Iterable[HardGate] = (),
    assumptions: Iterable[str] = (),
    non_claims: Iterable[str] = (),
    observed_conclusions: Iterable[ObservedConclusion] = (),
    recommendations: Iterable[Recommendation] = (),
    validate: bool = True,
) -> AssessmentModel:
    """Aggregate every assessment input into the single canonical model.

    All inputs are the outputs of the earlier pipeline stages (Revision Binder,
    Source Inventory, Evidence Collector/Claim Guard, evaluators, Hard-Gate
    graph, maturity assessor, gap register, roadmap). This function merges them
    into exactly one :class:`AssessmentModel` -- the one source of truth every
    renderer must project from (Requirement 14.7).

    When ``target_levels`` is omitted, the canonical six-level target model
    (``T0``-``T5``) is used, since a publishable report must always present the
    full hierarchy (Requirement 2.2, 14.1).

    When ``validate`` is true (the default), the Task 10.1 validator is run and
    its :class:`ValidationResult` is attached to ``model.validation`` so callers
    can inspect the fail-closed findings without a second pass. Attaching the
    result never changes what the validator sees: the validator ignores
    ``model.validation`` entirely.
    """

    if not isinstance(revision, AssessmentRevision):
        raise TypeError("revision must be an AssessmentRevision")

    levels: tuple[TargetLevel, ...]
    if target_levels is None:
        levels = tuple(TargetLevel)
    else:
        levels = tuple(target_levels)

    model = AssessmentModel(
        revision=revision,
        source_inventory=tuple(source_inventory),
        evidence_records=tuple(evidence_records),
        conflicts=tuple(conflicts),
        target_levels=levels,
        domains=tuple(domains),
        assessments=tuple(assessments),
        gaps=tuple(gaps),
        hard_gates=tuple(hard_gates),
        assumptions=tuple(assumptions),
        non_claims=tuple(non_claims),
        observed_conclusions=tuple(observed_conclusions),
        recommendations=tuple(recommendations),
    )

    if not validate:
        return model

    result = validate_assessment_model(model)
    return _with_validation(model, result)


def canonical_reference_ids(model: AssessmentModel) -> frozenset[str]:
    """Return every object identifier the canonical model actually defines.

    An artifact may only reference identifiers in this set; anything else is a
    "foreign fact" a renderer must never invent (Requirement 14.7). The set spans
    the revision root, inventory entries, evidence records, conflicts, domains,
    gaps, Hard-Gates, observed conclusions, and recommendations.
    """

    ids: set[str] = {str(model.revision.repository_root_id)}
    for entry in model.source_inventory:
        ids.add(str(entry.id))
    for record in model.evidence_records:
        ids.add(str(record.id))
    for conflict in model.conflicts:
        ids.add(str(conflict.id))
    for domain in model.domains:
        ids.add(str(domain.id))
    for gap in model.gaps:
        ids.add(str(gap.id))
    for gate in model.hard_gates:
        ids.add(str(gate.id))
    for conclusion in model.observed_conclusions:
        ids.add(str(conclusion.id))
    for recommendation in model.recommendations:
        ids.add(str(recommendation.id))
    return frozenset(ids)


@dataclass(frozen=True, slots=True)
class RenderedArtifact:
    """A single artifact a renderer produced entirely in memory.

    ``content`` is the exact bytes to write. ``projected_ids`` is the set of
    canonical object identifiers the artifact references; the publish gate
    verifies these are all present in the canonical model (no foreign facts).
    When ``required_ids`` is not ``None`` the artifact declares that it is an
    *exact* lossless projection of that identifier set, and the gate verifies
    ``projected_ids == required_ids`` (Requirement 14.2, 14.3).
    """

    name: str
    content: bytes
    projected_ids: frozenset[str] = frozenset()
    required_ids: frozenset[str] | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.name, str) or not self.name.strip():
            raise ValueError("artifact name must be a non-empty string")
        if not isinstance(self.content, (bytes, bytearray)):
            raise TypeError("artifact content must be bytes")
        object.__setattr__(self, "content", bytes(self.content))
        object.__setattr__(
            self, "projected_ids", frozenset(str(ref) for ref in self.projected_ids)
        )
        if self.required_ids is not None:
            object.__setattr__(
                self,
                "required_ids",
                frozenset(str(ref) for ref in self.required_ids),
            )


# A renderer turns the canonical model into exactly one in-memory artifact.
Renderer = Callable[[AssessmentModel], RenderedArtifact]


@dataclass(frozen=True, slots=True)
class PublishResult:
    """The outcome of a publish transaction.

    ``published`` is true only when validation, rendering, and parity all passed
    *and* the artifacts were committed (or buffered, in a dry run). ``validation``
    is the Task 10.1 result. ``findings`` carries every fail-closed publish
    finding (empty on success). ``artifacts`` are the in-memory rendered
    artifacts (always populated when rendering succeeded, even on parity
    failure, so callers can inspect them). ``written_paths`` lists the files
    actually committed to disk (empty on a dry run or any failure).
    """

    published: bool
    validation: ValidationResult
    findings: tuple[ValidationFinding, ...] = ()
    artifacts: tuple[RenderedArtifact, ...] = ()
    written_paths: tuple[str, ...] = ()


def publish_assessment(
    model: AssessmentModel,
    renderers: Sequence[Renderer] = (),
    *,
    output_dir: str | os.PathLike[str] | None = None,
    manifest_builder: "ManifestBuilder | None" = None,
) -> PublishResult:
    """Validate, render, parity-check, and atomically publish a model.

    This is the all-or-nothing publish gate (Requirement 9.7, 14.1). The order is
    strict and fail-closed at every step:

    1. **Validate first.** Run the Task 10.1 validator. If the model is invalid,
       emit *nothing* and return ``published=False`` with the validator findings
       plus an ``RPT-PUBLISH-INVALID-MODEL`` finding. Only a valid model is ever
       allowed to reach a renderer.
    2. **Render into memory.** Invoke each renderer and buffer its artifact. A
       renderer that raises produces an ``RPT-RENDER-FAILED`` finding and aborts
       the publish; duplicate artifact names produce ``RPT-DUPLICATE-ARTIFACT``.
    3. **Parity-check.** Reject any artifact that references an identifier absent
       from the canonical model (``RPT-PARITY-FOREIGN-FACT``) or that fails to
       project its declared required identifier set losslessly
       (``RPT-PARITY-MISSING-PROJECTION``).
    3b. **Build the artifact manifest (optional).** When ``manifest_builder`` is
       given, it runs *inside* the staging window over the fully rendered,
       parity-checked artifact set, digesting their bytes and binding them to the
       revision fingerprint. The manifest joins the same atomic commit, so its
       digests always describe exactly the bytes written (Requirement 14.7). A
       manifest builder that raises, produces a non-artifact, collides with an
       existing name, or references a foreign identifier fails the whole publish
       (``RPT-MANIFEST-FAILED`` / ``RPT-DUPLICATE-ARTIFACT`` /
       ``RPT-PARITY-FOREIGN-FACT``) and nothing is written.
    4. **Commit atomically.** Only once every check passes are the buffered bytes
       written. When ``output_dir`` is given, artifacts are staged in a private
       temporary directory and then moved into place, so a partially written or
       "half-valid" report can never be left behind. When ``output_dir`` is
       ``None`` the transaction is a dry run: artifacts are returned in memory and
       nothing is written.

    On any failure the function returns ``published=False`` and writes nothing;
    it never raises for a model/rendering/parity problem (every problem becomes a
    finding) so the caller sees the complete set of publish blockers at once.
    """

    if not isinstance(model, AssessmentModel):
        raise TypeError("model must be an AssessmentModel")

    # -- Step 1: validate first; a valid model is required to reach renderers. -
    validation = validate_assessment_model(model)
    if not validation.valid:
        finding = _finding(
            RPT_PUBLISH_INVALID_MODEL,
            ("9.7", "14.1"),
            (str(model.revision.repository_root_id),),
        )
        return PublishResult(
            published=False,
            validation=validation,
            findings=(finding, *validation.findings),
        )

    # -- Step 2: render every artifact into memory (buffer, do not write). ----
    artifacts: list[RenderedArtifact] = []
    seen_names: set[str] = set()
    render_findings: list[ValidationFinding] = []
    for renderer in renderers:
        try:
            artifact = renderer(model)
        except Exception:  # noqa: BLE001 - any renderer failure fails closed.
            render_findings.append(
                _finding(RPT_RENDER_FAILED, ("14.1",), (_renderer_name(renderer),))
            )
            # A renderer that raised leaves the transaction unpublishable.
            return PublishResult(
                published=False,
                validation=validation,
                findings=tuple(render_findings),
                artifacts=tuple(artifacts),
            )
        if not isinstance(artifact, RenderedArtifact):
            render_findings.append(
                _finding(RPT_RENDER_FAILED, ("14.1",), (_renderer_name(renderer),))
            )
            return PublishResult(
                published=False,
                validation=validation,
                findings=tuple(render_findings),
                artifacts=tuple(artifacts),
            )
        if artifact.name in seen_names:
            render_findings.append(
                _finding(RPT_DUPLICATE_ARTIFACT, ("14.1",), (artifact.name,))
            )
        seen_names.add(artifact.name)
        artifacts.append(artifact)

    # -- Step 3: parity-check every buffered artifact against the model. ------
    canonical = canonical_reference_ids(model)
    parity_findings = list(render_findings)
    for artifact in artifacts:
        foreign = sorted(artifact.projected_ids - canonical)
        if foreign:
            parity_findings.append(
                _finding(
                    RPT_PARITY_FOREIGN_FACT,
                    ("14.7",),
                    (artifact.name, *foreign),
                )
            )
        if artifact.required_ids is not None and (
            artifact.projected_ids != artifact.required_ids
        ):
            missing = sorted(artifact.required_ids - artifact.projected_ids)
            extra = sorted(artifact.projected_ids - artifact.required_ids)
            parity_findings.append(
                _finding(
                    RPT_PARITY_MISSING_PROJECTION,
                    ("14.2", "14.3"),
                    (artifact.name, *missing, *extra),
                )
            )

    if parity_findings:
        return PublishResult(
            published=False,
            validation=validation,
            findings=tuple(parity_findings),
            artifacts=tuple(artifacts),
        )

    # -- Step 3b: build the digest-bound artifact manifest, if requested. -----
    # The manifest digests the *other* artifacts, so it can only be built once
    # every artifact has been rendered and parity-checked -- but before commit,
    # so it joins the same all-or-nothing transaction.
    if manifest_builder is not None:
        try:
            manifest = manifest_builder(model, tuple(artifacts))
        except Exception:  # noqa: BLE001 - a manifest failure fails closed.
            return PublishResult(
                published=False,
                validation=validation,
                findings=(_finding(RPT_MANIFEST_FAILED, ("14.1", "14.7"),
                                   (str(model.revision.repository_root_id),)),),
                artifacts=tuple(artifacts),
            )
        manifest_findings: list[ValidationFinding] = []
        if not isinstance(manifest, RenderedArtifact):
            manifest_findings.append(
                _finding(RPT_MANIFEST_FAILED, ("14.1", "14.7"),
                         (str(model.revision.repository_root_id),))
            )
        else:
            if manifest.name in seen_names:
                manifest_findings.append(
                    _finding(RPT_DUPLICATE_ARTIFACT, ("14.1",), (manifest.name,))
                )
            foreign = sorted(manifest.projected_ids - canonical)
            if foreign:
                manifest_findings.append(
                    _finding(
                        RPT_PARITY_FOREIGN_FACT,
                        ("14.7",),
                        (manifest.name, *foreign),
                    )
                )
        if manifest_findings:
            return PublishResult(
                published=False,
                validation=validation,
                findings=tuple(manifest_findings),
                artifacts=tuple(artifacts),
            )
        seen_names.add(manifest.name)
        artifacts.append(manifest)

    # -- Step 4: commit atomically (or return in memory on a dry run). --------
    written: tuple[str, ...] = ()
    if output_dir is not None:
        written = _atomic_commit(output_dir, artifacts)

    return PublishResult(
        published=True,
        validation=validation,
        findings=(),
        artifacts=tuple(artifacts),
        written_paths=written,
    )


def _atomic_commit(
    output_dir: str | os.PathLike[str], artifacts: Sequence[RenderedArtifact]
) -> tuple[str, ...]:
    """Write every artifact via a private staging directory, then move in place.

    All artifacts are written to a temporary staging directory first; only after
    every staged write succeeds are the files moved into ``output_dir``. If any
    staged write fails, the staging directory is removed and ``output_dir`` is
    left exactly as it was, so a prior valid assessment is never clobbered by a
    partial one (Requirement 9.7, 14.1).
    """

    destination = Path(output_dir)
    destination.mkdir(parents=True, exist_ok=True)

    staging = Path(tempfile.mkdtemp(prefix=".assessment-staging-", dir=destination))
    try:
        for artifact in artifacts:
            staged_path = staging / artifact.name
            staged_path.parent.mkdir(parents=True, exist_ok=True)
            staged_path.write_bytes(artifact.content)

        written: list[str] = []
        for artifact in artifacts:
            staged_path = staging / artifact.name
            final_path = destination / artifact.name
            final_path.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staged_path, final_path)
            written.append(str(final_path))
    except OSError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    return tuple(sorted(written))


def _with_validation(
    model: AssessmentModel, result: ValidationResult
) -> AssessmentModel:
    """Return a copy of ``model`` carrying ``result`` in its validation field."""

    return dataclasses.replace(model, validation=result)


def _finding(
    code: str,
    requirement_refs: Iterable[str],
    object_refs: Iterable[str],
) -> ValidationFinding:
    return ValidationFinding(
        severity=FindingSeverity.ERROR,
        code=code,
        requirement_refs=tuple(str(ref) for ref in requirement_refs),
        object_refs=tuple(_ref_token(ref) for ref in object_refs),
    )


_REF_DISALLOWED = re.compile(r"[^A-Za-z0-9_.:-]")


def _ref_token(value: object) -> str:
    """Coerce an arbitrary label into a valid stable-reference token.

    Object references on a :class:`ValidationFinding` must be valid stable
    identifiers. Canonical object IDs already are; artifact names or renderer
    names may contain characters (like ``/``) that are not, so those are mapped
    into the allowed alphabet rather than crashing the fail-closed publish path.
    """

    text = str(value)
    sanitized = _REF_DISALLOWED.sub("_", text)
    if not sanitized or not sanitized[0].isalpha():
        sanitized = f"artifact.{sanitized}" if sanitized else "artifact"
    return sanitized


def _renderer_name(renderer: Renderer) -> str:
    """Best-effort stable name for a renderer, for fail-closed findings."""

    name = getattr(renderer, "__name__", None)
    if isinstance(name, str) and name.strip():
        return name
    return "renderer"
