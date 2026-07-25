"""Evidence Collector: normalize and deduplicate repository evidence.

Task 4.1 converts source, test, release, workflow, artifact, example, plan, and
non-claim inputs from the source inventory, repository adapters, and execution
evidence into complete :class:`EvidenceRecord` values, then deduplicates them by
stable claim key while preserving every distinct source and revision origin.

Scope boundaries (intentional seams for later tasks):

* Conflict detection between incompatible records for the same claim key is
  **Task 4.2**. This module groups records by claim key (see
  :attr:`EvidenceBundle.by_claim_key`) but never selects a winner or forces a
  confidence downgrade from a conflict.
* Claim Guard status/wording/scope promotion (GA/preview tiers, present-tense
  gating, primitive-object wording) is **Task 4.3**. This module only applies the
  base status decision order below and leaves tier promotion to the guard.
* Trust-assumption and exclusion auditing is **Task 4.4**. This module passes any
  caller-provided limitations/trust assumptions through unchanged and never
  fails closed on a missing assumption.

Status decision order implemented here (fail-closed base classification):

1. A claim without a verifiable source path is ``Unknown``.
2. Plan-only prose (roadmap, RFC, planned gate) is ``Planned``.
3. ``Unsupported`` is produced *only* from an explicit non-claim/negative gate or
   a fully-audited absence; a bare proposed ``Unsupported`` is rejected.
4. Otherwise the caller's proposed status is used, defaulting to ``Unknown`` when
   the evidence cannot yet be classified (deferred to the Claim Guard).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping

from .adapters import (
    AdapterBundle,
    CaseManifest,
    GateDefinition,
    MetadataRecord,
    SourceDocumentMapping,
    WorkflowJobDefinition,
)
from .execution import ExecutionEvidence
from .identifiers import ReferenceId, RepositoryPath, StableId, reference, stable_id
from .models import (
    AssessmentRevision,
    ConfidenceRating,
    EvidenceConflict,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    ExecutionState,
    LocationKind,
    RevisionOrigin,
    SourceCategory,
    SourceInventoryEntry,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

# Conservative base mapping from parsed gate status to evidence status. Gate
# acceptance without a bound current execution is not GA/preview proof, so every
# non-planned gate is Experimental at this layer; the Claim Guard (Task 4.3) may
# promote specific gates within their declared scope.
_GATE_STATUS_TO_EVIDENCE_STATUS: dict[str, EvidenceStatus] = {
    "experimental": EvidenceStatus.EXPERIMENTAL,
    "candidate": EvidenceStatus.EXPERIMENTAL,
    "accepted": EvidenceStatus.EXPERIMENTAL,
}

# Location kind selection order for whole-file source evidence, preferring the
# smallest stable sub-file anchor (Requirement 14.5).
_ANCHOR_KIND_PRIORITY: tuple[tuple[str, LocationKind], ...] = (
    ("Heading:", LocationKind.HEADING),
    ("Symbol:", LocationKind.SYMBOL),
    ("ManifestKey:", LocationKind.MANIFEST_KEY),
    ("CaseId:", LocationKind.CASE_ID),
    ("WorkflowJob:", LocationKind.WORKFLOW_JOB),
)

_EXECUTION_STATE_TO_VERIFICATION: dict[ExecutionState, VerificationState] = {
    ExecutionState.VALIDATED: VerificationState.VALIDATED,
    ExecutionState.NOT_RUN: VerificationState.NOT_RUN,
    ExecutionState.UNAVAILABLE: VerificationState.UNAVAILABLE,
    ExecutionState.FAILED: VerificationState.FAILED,
}


def decide_status(
    *,
    source_path: str | None,
    plan_only: bool,
    negative_claim: bool,
    audited_absence: bool,
    proposed_status: EvidenceStatus | None = None,
) -> EvidenceStatus:
    """Apply the Task 4.1 status decision order.

    ``Unsupported`` is reachable only through ``negative_claim`` or
    ``audited_absence``; a proposed ``Unsupported`` without one of those signals
    is rejected so that "not found" can never masquerade as a negative result.
    """

    if proposed_status is not None and not isinstance(proposed_status, EvidenceStatus):
        raise TypeError("proposed_status must be an EvidenceStatus or None")
    if (
        proposed_status is EvidenceStatus.UNSUPPORTED
        and not (negative_claim or audited_absence)
    ):
        raise ValueError(
            "Unsupported requires an explicit non-claim/negative gate or audited absence"
        )
    if source_path is None:
        return EvidenceStatus.UNKNOWN
    if plan_only:
        return EvidenceStatus.PLANNED
    if negative_claim or audited_absence:
        return EvidenceStatus.UNSUPPORTED
    if proposed_status is not None:
        return proposed_status
    return EvidenceStatus.UNKNOWN


@dataclass(frozen=True, slots=True, kw_only=True)
class ClaimInput:
    """A normalized, pre-classification evidence candidate for one claim key."""

    claim_key: str
    claim: str
    evidence_kind: EvidenceKind
    origin: RevisionOrigin
    source_path: str | None
    location: SourceLocation | None = None
    scope: EvidenceScope = field(default_factory=EvidenceScope)
    proposed_status: EvidenceStatus | None = None
    plan_only: bool = False
    negative_claim: bool = False
    audited_absence: bool = False
    verification_state: VerificationState = VerificationState.NOT_RUN
    limitations: tuple[str, ...] = ()
    trust_assumptions: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.claim_key, str) or not self.claim_key.strip():
            raise ValueError("claim_key must be a non-empty string")
        if not isinstance(self.claim, str) or not self.claim.strip():
            raise ValueError("claim must be a non-empty string")
        if not isinstance(self.evidence_kind, EvidenceKind):
            raise TypeError("evidence_kind must be an EvidenceKind")
        if not isinstance(self.origin, RevisionOrigin):
            raise TypeError("origin must be a RevisionOrigin")
        if self.source_path is not None:
            object.__setattr__(self, "source_path", RepositoryPath(self.source_path))
        if self.location is not None and not isinstance(self.location, SourceLocation):
            raise TypeError("location must be a SourceLocation or None")
        if not isinstance(self.scope, EvidenceScope):
            raise TypeError("scope must be an EvidenceScope")
        if not isinstance(self.verification_state, VerificationState):
            raise TypeError("verification_state must be a VerificationState")
        for name in ("plan_only", "negative_claim", "audited_absence"):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")

    @property
    def status(self) -> EvidenceStatus:
        return decide_status(
            source_path=self.source_path,
            plan_only=self.plan_only,
            negative_claim=self.negative_claim,
            audited_absence=self.audited_absence,
            proposed_status=self.proposed_status,
        )


@dataclass(frozen=True, slots=True)
class EvidenceBundle:
    """Deduplicated evidence records grouped by stable claim key."""

    records: tuple[EvidenceRecord, ...]
    by_claim_key: Mapping[str, tuple[EvidenceRecord, ...]]

    def __post_init__(self) -> None:
        records = tuple(self.records)
        if not all(isinstance(item, EvidenceRecord) for item in records):
            raise TypeError("records must contain EvidenceRecord values")
        object.__setattr__(self, "records", records)
        object.__setattr__(self, "by_claim_key", dict(self.by_claim_key))

    @property
    def claim_keys(self) -> tuple[str, ...]:
        return tuple(sorted(self.by_claim_key))

    def records_for(self, claim_key: str) -> tuple[EvidenceRecord, ...]:
        return self.by_claim_key.get(claim_key, ())


class EvidenceCollector:
    """Turn inventory, adapter, and execution inputs into canonical evidence."""

    def collect(
        self,
        revision: AssessmentRevision,
        inventory: Iterable[SourceInventoryEntry],
        bundle: AdapterBundle,
        execution_evidence: Iterable[ExecutionEvidence] = (),
    ) -> EvidenceBundle:
        if not isinstance(revision, AssessmentRevision):
            raise TypeError("revision must be an AssessmentRevision")
        if not isinstance(bundle, AdapterBundle):
            raise TypeError("bundle must be an AdapterBundle")
        entries = tuple(inventory)
        if not all(isinstance(item, SourceInventoryEntry) for item in entries):
            raise TypeError("inventory must contain SourceInventoryEntry values")
        executions = tuple(execution_evidence)
        if not all(isinstance(item, ExecutionEvidence) for item in executions):
            raise TypeError("execution_evidence must contain ExecutionEvidence values")

        origin_by_path = {str(entry.path): entry.revision_origin for entry in entries}
        default_origin = (
            RevisionOrigin.COMMITTED_REVISION
            if revision.worktree_clean
            else RevisionOrigin.CURRENT_WORKTREE
        )
        revision_ref = reference(
            stable_id("revision", revision.commit_id, revision.worktree_fingerprint)
        )

        inputs: list[ClaimInput] = []
        inputs.extend(self._inventory_inputs(entries))
        inputs.extend(self._gate_inputs(bundle.gates, origin_by_path, default_origin))
        inputs.extend(
            self._mapping_inputs(bundle.source_mappings, origin_by_path, default_origin)
        )
        inputs.extend(self._case_inputs(bundle.cases, origin_by_path, default_origin))
        inputs.extend(self._workflow_inputs(bundle.workflow_jobs, origin_by_path, default_origin))
        inputs.extend(
            self._metadata_inputs(
                bundle.release_metadata, EvidenceKind.RELEASE, "release", origin_by_path, default_origin
            )
        )
        inputs.extend(
            self._metadata_inputs(
                bundle.artifact_metadata, EvidenceKind.ARTIFACT, "artifact", origin_by_path, default_origin
            )
        )
        inputs.extend(self._execution_inputs(executions))

        return self._normalize(inputs, revision_ref)

    # -- input builders ---------------------------------------------------

    def _inventory_inputs(
        self, entries: tuple[SourceInventoryEntry, ...]
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for entry in entries:
            if not entry.inspected:
                continue
            kind, plan_only, scope, prefix, description = self._source_role(entry.category)
            if kind is None:
                continue
            path = str(entry.path)
            results.append(
                ClaimInput(
                    claim_key=f"{prefix}:{path}",
                    claim=f"{description} at {path}.",
                    evidence_kind=kind,
                    origin=entry.revision_origin,
                    source_path=path,
                    location=self._anchor_location(entry.stable_anchors, path),
                    scope=scope,
                    plan_only=plan_only,
                    verification_state=_EXECUTION_STATE_TO_VERIFICATION[entry.execution_state],
                )
            )
        return results

    @staticmethod
    def _source_role(
        category: SourceCategory,
    ) -> tuple[EvidenceKind | None, bool, EvidenceScope, str, str]:
        """Return (kind, plan_only, scope, claim-key prefix, description)."""

        if category is SourceCategory.EXAMPLE:
            return (
                EvidenceKind.EXAMPLE,
                False,
                EvidenceScope(target_levels=(TargetLevel.T0_HOSTED_ADJACENCY,)),
                "example",
                "Hosted example workflow",
            )
        if category is SourceCategory.ROADMAP:
            return (EvidenceKind.SPECIFICATION, True, EvidenceScope(), "plan", "Roadmap plan")
        if category is SourceCategory.RFC:
            return (EvidenceKind.RFC, True, EvidenceScope(), "plan", "RFC proposal")
        if category in {
            SourceCategory.SPECIFICATION,
            SourceCategory.UNIVERSE_OS_DOCUMENT,
            SourceCategory.GATE_REGISTRY,
        }:
            return (EvidenceKind.SPECIFICATION, False, EvidenceScope(), "source", "Specification evidence")
        if category in {
            SourceCategory.SOURCE_CODE,
            SourceCategory.README,
            SourceCategory.CHANGELOG,
            SourceCategory.RUNTIME,
            SourceCategory.STANDARD_LIBRARY,
            SourceCategory.OFFICIAL_PACKAGE,
            SourceCategory.BUILD_CONFIGURATION,
        }:
            return (EvidenceKind.SOURCE, False, EvidenceScope(), "source", "Source evidence")
        # Test, workflow, release-notes, and artifact categories are represented
        # through their structured adapter outputs to avoid double counting.
        return (None, False, EvidenceScope(), "source", "Source evidence")

    def _gate_inputs(
        self,
        gates: tuple[GateDefinition, ...],
        origin_by_path: Mapping[str, RevisionOrigin],
        default_origin: RevisionOrigin,
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for gate in gates:
            origin = origin_by_path.get(gate.source_path, default_origin)
            planned = gate.status == "planned"
            results.append(
                ClaimInput(
                    claim_key=f"gate:{gate.gate_id}",
                    claim=f"Gate {gate.gate_id}: {gate.title}",
                    evidence_kind=EvidenceKind.SPECIFICATION,
                    origin=origin,
                    source_path=gate.source_path,
                    location=SourceLocation(kind=LocationKind.MANIFEST_KEY, value=gate.gate_id),
                    proposed_status=None if planned else _GATE_STATUS_TO_EVIDENCE_STATUS[gate.status],
                    plan_only=planned,
                )
            )
            results.append(
                ClaimInput(
                    claim_key=f"non-claim:gate:{gate.gate_id}",
                    claim=gate.non_claim,
                    evidence_kind=EvidenceKind.NON_CLAIM,
                    origin=origin,
                    source_path=gate.source_path,
                    location=SourceLocation(
                        kind=LocationKind.MANIFEST_KEY, value=f"{gate.gate_id}.non_claim"
                    ),
                    negative_claim=True,
                )
            )
        return results

    def _mapping_inputs(
        self,
        mappings: tuple[SourceDocumentMapping, ...],
        origin_by_path: Mapping[str, RevisionOrigin],
        default_origin: RevisionOrigin,
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for mapping in mappings:
            origin = origin_by_path.get(mapping.path, default_origin)
            for gate_id in mapping.gate_ids:
                results.append(
                    ClaimInput(
                        claim_key=f"gate:{gate_id}",
                        claim=f"{mapping.relationship} ({mapping.path} -> {gate_id})",
                        evidence_kind=EvidenceKind.SOURCE,
                        origin=origin,
                        source_path=mapping.path,
                        location=SourceLocation(
                            kind=LocationKind.MANIFEST_KEY, value=gate_id
                        ),
                    )
                )
        return results

    def _case_inputs(
        self,
        cases: tuple[CaseManifest, ...],
        origin_by_path: Mapping[str, RevisionOrigin],
        default_origin: RevisionOrigin,
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for case in cases:
            results.append(
                ClaimInput(
                    claim_key=f"test:{case.case_id}",
                    claim=f"Test case {case.case_id} is defined in suite {case.suite}.",
                    evidence_kind=EvidenceKind.TEST_DEFINITION,
                    origin=origin_by_path.get(case.path, default_origin),
                    source_path=case.path,
                    location=SourceLocation(kind=LocationKind.CASE_ID, value=case.case_id),
                    verification_state=_EXECUTION_STATE_TO_VERIFICATION[case.execution_state],
                )
            )
        return results

    def _workflow_inputs(
        self,
        jobs: tuple[WorkflowJobDefinition, ...],
        origin_by_path: Mapping[str, RevisionOrigin],
        default_origin: RevisionOrigin,
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for job in jobs:
            results.append(
                ClaimInput(
                    claim_key=f"workflow:{job.workflow_path}#{job.job_id}",
                    claim=f"Workflow job {job.job_id} is defined in {job.workflow_path}.",
                    evidence_kind=EvidenceKind.WORKFLOW,
                    origin=origin_by_path.get(job.workflow_path, default_origin),
                    source_path=job.workflow_path,
                    location=SourceLocation(kind=LocationKind.WORKFLOW_JOB, value=job.job_id),
                    verification_state=_EXECUTION_STATE_TO_VERIFICATION[job.execution_state],
                )
            )
        return results

    def _metadata_inputs(
        self,
        records: tuple[MetadataRecord, ...],
        kind: EvidenceKind,
        prefix: str,
        origin_by_path: Mapping[str, RevisionOrigin],
        default_origin: RevisionOrigin,
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for record in records:
            results.append(
                ClaimInput(
                    claim_key=f"{prefix}:{record.metadata_kind}:{record.identity}",
                    claim=(
                        f"{record.metadata_kind} metadata {record.identity} "
                        f"from {record.source_path}."
                    ),
                    evidence_kind=kind,
                    origin=origin_by_path.get(record.source_path, default_origin),
                    source_path=record.source_path,
                    location=SourceLocation(
                        kind=LocationKind.MANIFEST_KEY,
                        value=f"{record.metadata_kind}:{record.identity}",
                    ),
                    verification_state=_EXECUTION_STATE_TO_VERIFICATION[record.execution_state],
                )
            )
        return results

    def _execution_inputs(
        self, executions: tuple[ExecutionEvidence, ...]
    ) -> list[ClaimInput]:
        results: list[ClaimInput] = []
        for evidence in executions:
            artifact = evidence.stdout_artifact or evidence.stderr_artifact
            if artifact is None:
                # A disabled or not-run command produces no positive execution
                # artifact; it is not evidence of any capability. Skip it.
                continue
            results.append(
                ClaimInput(
                    claim_key=f"execution:{evidence.command_id}",
                    claim=(
                        f"Local command {evidence.command_id} produced execution "
                        f"outcome {evidence.outcome.value}."
                    ),
                    evidence_kind=EvidenceKind.TEST_EXECUTION,
                    origin=RevisionOrigin.EXECUTION_ARTIFACT,
                    source_path=artifact.path,
                    location=SourceLocation(
                        kind=LocationKind.MANIFEST_KEY, value=str(evidence.id)
                    ),
                    verification_state=_EXECUTION_STATE_TO_VERIFICATION[evidence.execution_state],
                )
            )
        return results

    # -- normalization ----------------------------------------------------

    def _normalize(
        self, inputs: list[ClaimInput], revision_ref: ReferenceId
    ) -> EvidenceBundle:
        # Pass 1: assign a stable id per input and collapse exact duplicates
        # (same claim key, source path, location, origin, and kind).
        canonical: dict[StableId, ClaimInput] = {}
        keys_by_id: dict[StableId, str] = {}
        ids_by_key: dict[str, list[StableId]] = {}
        for candidate in inputs:
            record_id = self._record_id(candidate)
            if record_id in canonical:
                continue
            canonical[record_id] = candidate
            keys_by_id[record_id] = candidate.claim_key
            ids_by_key.setdefault(candidate.claim_key, []).append(record_id)

        # Pass 2: build records, cross-linking every sibling under a claim key so
        # the full evidence set (all sources and origins) is preserved losslessly.
        records: list[EvidenceRecord] = []
        for record_id, candidate in canonical.items():
            siblings = tuple(
                reference(other)
                for other in ids_by_key[candidate.claim_key]
                if other != record_id
            )
            records.append(
                self._build_record(record_id, candidate, revision_ref, siblings)
            )

        records.sort(key=lambda item: str(item.id))
        by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
        for claim_key in sorted(ids_by_key):
            group = tuple(
                record for record in records if keys_by_id[record.id] == claim_key
            )
            by_claim_key[claim_key] = group
        return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)

    @staticmethod
    def _record_id(candidate: ClaimInput) -> StableId:
        location = candidate.location
        return stable_id(
            "evidence",
            candidate.claim_key,
            candidate.source_path or "",
            candidate.evidence_kind.value,
            candidate.origin.value,
            location.kind.value if location is not None else "",
            location.value if location is not None else "",
        )

    def _build_record(
        self,
        record_id: StableId,
        candidate: ClaimInput,
        revision_ref: ReferenceId,
        related: tuple[ReferenceId, ...],
    ) -> EvidenceRecord:
        source_path = candidate.source_path
        if source_path is None:
            # A pathless candidate is Unknown by the decision order but cannot be
            # serialized as a schema-valid EvidenceRecord (which requires a
            # repository path). Such candidates are surfaced only through
            # decide_status and are never emitted as records.
            raise ValueError(
                f"pathless claim {candidate.claim_key!r} cannot become an EvidenceRecord"
            )
        location = candidate.location or self._anchor_location((), source_path)
        status = candidate.status
        return EvidenceRecord(
            id=record_id,
            claim_key=candidate.claim_key,
            claim=candidate.claim,
            status=status,
            source_path=source_path,
            location=location,
            revision_ref=revision_ref,
            origin=candidate.origin,
            evidence_kind=candidate.evidence_kind,
            confidence=self._base_confidence(status, candidate.verification_state),
            scope=candidate.scope,
            limitations=candidate.limitations,
            trust_assumptions=candidate.trust_assumptions,
            verification_state=candidate.verification_state,
            related_evidence_ids=related,
        )

    @staticmethod
    def _base_confidence(
        status: EvidenceStatus, verification_state: VerificationState
    ) -> ConfidenceRating:
        if status is EvidenceStatus.UNKNOWN:
            return ConfidenceRating.LOW
        if verification_state is VerificationState.VALIDATED:
            return ConfidenceRating.HIGH
        return ConfidenceRating.MEDIUM

    @staticmethod
    def _anchor_location(
        anchors: tuple[str, ...], source_path: str
    ) -> SourceLocation:
        for prefix, kind in _ANCHOR_KIND_PRIORITY:
            candidates = sorted(
                anchor for anchor in anchors if anchor.startswith(prefix)
            )
            if candidates:
                return SourceLocation(kind=kind, value=candidates[0][len(prefix):])
        # No sub-file anchor is available; fall back to a stable, file-level
        # anchor so the location remains reproducible and traceable.
        return SourceLocation(kind=LocationKind.HEADING, value=f"File:{source_path}")


def collect_evidence(
    revision: AssessmentRevision,
    inventory: Iterable[SourceInventoryEntry],
    bundle: AdapterBundle,
    execution_evidence: Iterable[ExecutionEvidence] = (),
) -> EvidenceBundle:
    """Convenience API for the Evidence Collector, normalizer, and deduplicator."""

    return EvidenceCollector().collect(revision, inventory, bundle, execution_evidence)


# --------------------------------------------------------------------------- #
# Task 4.2: lossless evidence conflict detection                              #
# --------------------------------------------------------------------------- #

# Present-tense implementation claims: an Evidence_Record with one of these
# statuses asserts that a capability exists now, to some scoped degree. A
# conflict that touches any of these defaults to blocking (Error-handling table,
# ``CNF-*``) because an unresolved implementation claim cannot be trusted.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)


def _claim_polarity(status: EvidenceStatus) -> str | None:
    """Return the assertive polarity of a status, or ``None`` when neutral.

    Two records for the same claim key are incompatible only when they assert
    *different* things about a capability's existence. The polarity buckets are:

    * ``"implemented"`` -- any present-tense/current status (GA, preview,
      experimental). Distinct implemented tiers (e.g. repo preview vs installed
      preview of the same package) are the *same* polarity and never conflict
      with one another; they are scope-differentiated by design.
    * ``"planned"`` -- future-only work.
    * ``"unsupported"`` -- an explicit non-claim / audited absence.

    ``Unknown`` asserts nothing (the collector could not classify the evidence),
    so it is neutral and can never create a conflict.
    """

    if status in _IMPLEMENTED_STATUSES:
        return "implemented"
    if status is EvidenceStatus.PLANNED:
        return "planned"
    if status is EvidenceStatus.UNSUPPORTED:
        return "unsupported"
    return None


class EvidenceConflictDetector:
    """Detect incompatible claims for a shared claim key without picking a winner.

    Detection consumes the Task 4.1 :class:`EvidenceBundle` grouping and is
    order-independent: it iterates each claim-key group, and every emitted
    :class:`EvidenceConflict` is built from the full assertive record set, so
    permuting the collector's inputs cannot change which records or locations a
    conflict contains, nor select a "more favorable" source as a winner. The
    :class:`EvidenceConflict` model fixes ``winner = None`` and
    ``confidence = Low`` structurally.
    """

    def detect(self, bundle: EvidenceBundle) -> tuple[EvidenceConflict, ...]:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        conflicts: list[EvidenceConflict] = []
        for claim_key in sorted(bundle.by_claim_key):
            records = bundle.records_for(claim_key)
            # Only records that make an assertive claim can disagree; Unknown
            # records are neutral and are never treated as conflicting.
            assertive = tuple(
                record
                for record in records
                if _claim_polarity(record.status) is not None
            )
            polarities = {_claim_polarity(record.status) for record in assertive}
            if len(polarities) < 2:
                continue
            conflicts.append(self._build_conflict(claim_key, assertive))
        conflicts.sort(key=lambda conflict: str(conflict.id))
        return tuple(conflicts)

    @staticmethod
    def _build_conflict(
        claim_key: str, records: tuple[EvidenceRecord, ...]
    ) -> EvidenceConflict:
        # Losslessly include every assertive record: all IDs, all reported
        # statuses, and all source locations. The model deduplicates and sorts
        # these deterministically, so the conflict is symmetric and stable
        # across input permutations.
        blocking = any(record.status in _IMPLEMENTED_STATUSES for record in records)
        return EvidenceConflict(
            id=stable_id("conflict", claim_key),
            claim_key=claim_key,
            evidence_ids=tuple(reference(record.id) for record in records),
            incompatible_values=tuple(record.status.value for record in records),
            locations=tuple(record.location for record in records),
            blocking=blocking,
        )


def detect_evidence_conflicts(bundle: EvidenceBundle) -> tuple[EvidenceConflict, ...]:
    """Convenience API for lossless, order-independent conflict detection."""

    return EvidenceConflictDetector().detect(bundle)
