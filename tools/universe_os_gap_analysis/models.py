"""Closed enums and typed canonical models for Universe OS gap analysis."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum, IntEnum
from typing import Any, Iterable, TypeVar

from .identifiers import ReferenceId, RepositoryPath, StableId, reference


class ClosedStrEnum(str, Enum):
    """String enum whose serialized representation is its declared value."""


class EvidenceStatus(ClosedStrEnum):
    COMPILER_TOOLING_GA = "Compiler_Tooling_GA"
    BACKEND_SDK_GA = "Backend_SDK_GA"
    INSTALLED_PREVIEW = "Installed_Preview"
    REPO_PREVIEW = "Repo_Preview"
    EXPERIMENTAL = "Experimental"
    PLANNED = "Planned"
    UNSUPPORTED = "Unsupported"
    UNKNOWN = "Unknown"


class TargetLevel(ClosedStrEnum):
    T0_HOSTED_ADJACENCY = "T0_Hosted_Adjacency"
    T1_INDEPENDENT_LANGUAGE_PLATFORM = "T1_Independent_Language_Platform"
    T2_FREESTANDING_SUBSTRATE = "T2_Freestanding_Substrate"
    T3_BOOT_AND_KERNEL_FOUNDATION = "T3_Boot_And_Kernel_Foundation"
    T4_ISOLATED_USERSPACE_PLATFORM = "T4_Isolated_Userspace_Platform"
    T5_OPERABLE_UNIVERSE_OS = "T5_Operable_Universe_OS"


class MaturityScore(IntEnum):
    ABSENT = 0
    NARROW_EXPERIMENT = 1
    REPEATABLE_REPOSITORY_IMPLEMENTATION = 2
    CROSS_HOST_CANDIDATE_CONTRACT = 3
    SUPPORTED_PRODUCTION_CAPABILITY = 4
    MATURE_INDEPENDENT_ECOSYSTEM = 5


class ConfidenceRating(ClosedStrEnum):
    HIGH = "High"
    MEDIUM = "Medium"
    LOW = "Low"


class GapCategory(ClosedStrEnum):
    LANGUAGE = "Language_Gap"
    IMPLEMENTATION = "Implementation_Gap"
    VERIFICATION = "Verification_Gap"
    ECOSYSTEM = "Ecosystem_Gap"


class ExecutionState(ClosedStrEnum):
    VALIDATED = "Validated"
    NOT_RUN = "NotRun"
    UNAVAILABLE = "Unavailable"
    FAILED = "Failed"


class VerificationState(ClosedStrEnum):
    VALIDATED = "Validated"
    NOT_RUN = "NotRun"
    UNAVAILABLE = "Unavailable"
    FAILED = "Failed"


class LocationKind(ClosedStrEnum):
    LINE_RANGE = "LineRange"
    HEADING = "Heading"
    SYMBOL = "Symbol"
    CASE_ID = "CaseId"
    MANIFEST_KEY = "ManifestKey"
    WORKFLOW_JOB = "WorkflowJob"


class RevisionOrigin(ClosedStrEnum):
    TAGGED_RELEASE = "TaggedRelease"
    COMMITTED_REVISION = "CommittedRevision"
    CURRENT_WORKTREE = "CurrentWorktree"
    EXECUTION_ARTIFACT = "ExecutionArtifact"


@dataclass(frozen=True, slots=True, kw_only=True)
class TagBinding:
    name: str
    peeled_commit: str

    def __post_init__(self) -> None:
        _require_text("name", self.name)
        _require_text("peeled_commit", self.peeled_commit)


@dataclass(frozen=True, slots=True, kw_only=True)
class TaggedReleaseAxis:
    describe: str
    tags: tuple[TagBinding, ...] = ()
    origin: RevisionOrigin = field(default=RevisionOrigin.TAGGED_RELEASE, init=False)

    def __post_init__(self) -> None:
        _require_text("describe", self.describe)
        tags = tuple(self.tags)
        if not all(isinstance(item, TagBinding) for item in tags):
            raise TypeError("tags must contain TagBinding values")
        normalized = tuple(sorted(tags, key=lambda item: item.name))
        if len({item.name for item in normalized}) != len(normalized):
            raise ValueError("tag names must be unique")
        object.__setattr__(self, "tags", normalized)


@dataclass(frozen=True, slots=True, kw_only=True)
class CommittedRevisionAxis:
    commit_id: str
    branch: str
    origin: RevisionOrigin = field(default=RevisionOrigin.COMMITTED_REVISION, init=False)

    def __post_init__(self) -> None:
        _require_text("commit_id", self.commit_id)
        _require_text("branch", self.branch)


@dataclass(frozen=True, slots=True, kw_only=True)
class CurrentWorktreeAxis:
    base_commit_id: str
    worktree_clean: bool
    origin: RevisionOrigin = field(default=RevisionOrigin.CURRENT_WORKTREE, init=False)

    def __post_init__(self) -> None:
        _require_text("base_commit_id", self.base_commit_id)
        if not isinstance(self.worktree_clean, bool):
            raise TypeError("worktree_clean must be a bool")


@dataclass(frozen=True, slots=True, kw_only=True)
class RevisionEvidenceAxes:
    tagged_release: TaggedReleaseAxis
    committed_revision: CommittedRevisionAxis
    current_worktree: CurrentWorktreeAxis

    def __post_init__(self) -> None:
        if not isinstance(self.tagged_release, TaggedReleaseAxis):
            raise TypeError("tagged_release must be a TaggedReleaseAxis")
        if not isinstance(self.committed_revision, CommittedRevisionAxis):
            raise TypeError("committed_revision must be a CommittedRevisionAxis")
        if not isinstance(self.current_worktree, CurrentWorktreeAxis):
            raise TypeError("current_worktree must be a CurrentWorktreeAxis")


class EvidenceKind(ClosedStrEnum):
    SOURCE = "Source"
    TEST_DEFINITION = "TestDefinition"
    TEST_EXECUTION = "TestExecution"
    SPECIFICATION = "Specification"
    RFC = "RFC"
    RELEASE = "Release"
    WORKFLOW = "Workflow"
    ARTIFACT = "Artifact"
    EXAMPLE = "Example"
    NON_CLAIM = "NonClaim"


class Ownership(ClosedStrEnum):
    NEBULA_OWNED = "NebulaOwned"
    HOST_OWNED = "HostOwned"
    OPERATIONS_OWNED = "OperationsOwned"


class Severity(ClosedStrEnum):
    CRITICAL = "Critical"
    HIGH = "High"
    MEDIUM = "Medium"
    LOW = "Low"


class FindingSeverity(ClosedStrEnum):
    ERROR = "Error"
    WARNING = "Warning"
    INFO = "Info"


class SourceCategory(ClosedStrEnum):
    SOURCE_CODE = "SourceCode"
    README = "README"
    ROADMAP = "Roadmap"
    CHANGELOG = "Changelog"
    RELEASE_NOTES = "ReleaseNotes"
    SPECIFICATION = "Specification"
    RFC = "RFC"
    TEST = "Test"
    BUILD_CONFIGURATION = "BuildConfiguration"
    CI_WORKFLOW = "CIWorkflow"
    RELEASE_WORKFLOW = "ReleaseWorkflow"
    RUNTIME = "Runtime"
    STANDARD_LIBRARY = "StandardLibrary"
    OFFICIAL_PACKAGE = "OfficialPackage"
    EXAMPLE = "Example"
    UNIVERSE_OS_DOCUMENT = "UniverseOSDocument"
    GATE_REGISTRY = "GateRegistry"
    ARTIFACT = "Artifact"


@dataclass(frozen=True, slots=True, kw_only=True)
class ExcludedPath:
    path: RepositoryPath
    reason: str
    rule_version: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "path", RepositoryPath(self.path))
        _require_text("reason", self.reason)
        _require_text("rule_version", self.rule_version)


@dataclass(frozen=True, slots=True, kw_only=True)
class SourceLocation:
    kind: LocationKind
    value: str

    def __post_init__(self) -> None:
        _require_enum("kind", self.kind, LocationKind)
        _require_text("value", self.value)


@dataclass(frozen=True, slots=True, kw_only=True)
class EvidenceScope:
    capability_ids: tuple[ReferenceId, ...] = ()
    target_levels: tuple[TargetLevel, ...] = ()
    platforms: tuple[str, ...] = ()
    ownership: Ownership | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_ids", _references(self.capability_ids))
        object.__setattr__(self, "target_levels", _enum_values(self.target_levels, TargetLevel))
        object.__setattr__(self, "platforms", _strings(self.platforms))
        if self.ownership is not None:
            _require_enum("ownership", self.ownership, Ownership)


@dataclass(frozen=True, slots=True, kw_only=True)
class AssessmentRevision:
    schema_version: str
    commit_id: str
    branch: str
    version: str
    describe: str
    tags: tuple[str, ...]
    worktree_clean: bool
    assessed_at_utc: datetime
    fingerprint_algorithm: str
    worktree_fingerprint: str
    tracked_diff_hash: str
    untracked_path_set_hash: str
    excluded_paths: tuple[ExcludedPath, ...]
    repository_root_id: StableId
    evidence_axes: RevisionEvidenceAxes | None = None

    def __post_init__(self) -> None:
        for name in (
            "schema_version", "commit_id", "branch", "version", "describe",
            "fingerprint_algorithm", "worktree_fingerprint", "tracked_diff_hash",
            "untracked_path_set_hash",
        ):
            _require_text(name, getattr(self, name))
        if not isinstance(self.worktree_clean, bool):
            raise TypeError("worktree_clean must be a bool")
        if not isinstance(self.assessed_at_utc, datetime):
            raise TypeError("assessed_at_utc must be a datetime")
        if self.assessed_at_utc.tzinfo is None or self.assessed_at_utc.utcoffset() is None:
            raise ValueError("assessed_at_utc must be timezone-aware")
        object.__setattr__(self, "repository_root_id", StableId(self.repository_root_id))
        object.__setattr__(self, "tags", _strings(self.tags))
        excluded_paths = tuple(self.excluded_paths)
        if not all(isinstance(item, ExcludedPath) for item in excluded_paths):
            raise TypeError("excluded_paths must contain ExcludedPath values")
        object.__setattr__(self, "excluded_paths", _sorted_models(excluded_paths, "path"))
        axes = self.evidence_axes
        if axes is None:
            axes = RevisionEvidenceAxes(
                tagged_release=TaggedReleaseAxis(
                    describe=self.describe,
                    tags=tuple(
                        TagBinding(name=tag, peeled_commit=self.commit_id)
                        for tag in self.tags
                    ),
                ),
                committed_revision=CommittedRevisionAxis(
                    commit_id=self.commit_id, branch=self.branch
                ),
                current_worktree=CurrentWorktreeAxis(
                    base_commit_id=self.commit_id, worktree_clean=self.worktree_clean
                ),
            )
            object.__setattr__(self, "evidence_axes", axes)
        if not isinstance(axes, RevisionEvidenceAxes):
            raise TypeError("evidence_axes must be RevisionEvidenceAxes")
        if axes.committed_revision.commit_id != self.commit_id:
            raise ValueError("committed revision axis must match commit_id")
        if axes.committed_revision.branch != self.branch:
            raise ValueError("committed revision axis must match branch")
        if axes.current_worktree.base_commit_id != self.commit_id:
            raise ValueError("current worktree axis must reference commit_id")
        if axes.current_worktree.worktree_clean != self.worktree_clean:
            raise ValueError("current worktree axis must match cleanliness")
        if axes.tagged_release.describe != self.describe:
            raise ValueError("tagged release axis must match describe")
        if tuple(item.name for item in axes.tagged_release.tags) != self.tags:
            raise ValueError("tagged release axis must match tags")
        if any(item.peeled_commit != self.commit_id for item in axes.tagged_release.tags):
            raise ValueError("tagged release tags must peel to commit_id")


@dataclass(frozen=True, slots=True, kw_only=True)
class SourceInventoryEntry:
    id: StableId
    category: SourceCategory
    path: RepositoryPath
    revision_origin: RevisionOrigin
    inspected: bool
    execution_state: ExecutionState
    content_hash: str
    stable_anchors: tuple[str, ...]
    execution_detail: str | None = None

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_enum("category", self.category, SourceCategory)
        _require_enum("revision_origin", self.revision_origin, RevisionOrigin)
        _require_enum("execution_state", self.execution_state, ExecutionState)
        object.__setattr__(self, "path", RepositoryPath(self.path))
        object.__setattr__(self, "stable_anchors", _strings(self.stable_anchors))
        _require_text("content_hash", self.content_hash)
        if not isinstance(self.inspected, bool):
            raise TypeError("inspected must be a bool")
        if self.execution_detail is not None:
            _require_text("execution_detail", self.execution_detail)


@dataclass(frozen=True, slots=True, kw_only=True)
class EvidenceRecord:
    id: StableId
    claim_key: str
    claim: str
    status: EvidenceStatus
    source_path: RepositoryPath
    location: SourceLocation
    revision_ref: ReferenceId
    origin: RevisionOrigin
    evidence_kind: EvidenceKind
    confidence: ConfidenceRating
    scope: EvidenceScope
    limitations: tuple[str, ...]
    trust_assumptions: tuple[str, ...]
    verification_state: VerificationState
    related_evidence_ids: tuple[ReferenceId, ...] = ()

    def __post_init__(self) -> None:
        _normalize_id(self)
        for name, enum_type in (
            ("status", EvidenceStatus), ("origin", RevisionOrigin),
            ("evidence_kind", EvidenceKind), ("confidence", ConfidenceRating),
            ("verification_state", VerificationState),
        ):
            _require_enum(name, getattr(self, name), enum_type)
        _require_text("claim_key", self.claim_key)
        _require_text("claim", self.claim)
        object.__setattr__(self, "source_path", RepositoryPath(self.source_path))
        object.__setattr__(self, "revision_ref", reference(self.revision_ref))
        object.__setattr__(self, "limitations", _strings(self.limitations))
        object.__setattr__(self, "trust_assumptions", _strings(self.trust_assumptions))
        object.__setattr__(self, "related_evidence_ids", _references(self.related_evidence_ids))
        if not isinstance(self.location, SourceLocation):
            raise TypeError("location must be a SourceLocation")
        if not isinstance(self.scope, EvidenceScope):
            raise TypeError("scope must be an EvidenceScope")


@dataclass(frozen=True, slots=True, kw_only=True)
class EvidenceConflict:
    id: StableId
    claim_key: str
    evidence_ids: tuple[ReferenceId, ...]
    incompatible_values: tuple[str, ...]
    locations: tuple[SourceLocation, ...]
    blocking: bool
    winner: None = field(default=None, init=False)
    confidence: ConfidenceRating = field(default=ConfidenceRating.LOW, init=False)

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("claim_key", self.claim_key)
        object.__setattr__(self, "evidence_ids", _references(self.evidence_ids, minimum=2))
        object.__setattr__(self, "incompatible_values", _strings(self.incompatible_values, minimum=2))
        object.__setattr__(self, "locations", _sorted_locations(self.locations, minimum=2))
        if not isinstance(self.blocking, bool):
            raise TypeError("blocking must be a bool")


@dataclass(frozen=True, slots=True, kw_only=True)
class CapabilityDomain:
    id: StableId
    name: str
    target_level: TargetLevel
    description: str
    mandatory_for_target: bool
    parent_id: ReferenceId | None = None
    checklist_ids: tuple[ReferenceId, ...] = ()
    evidence_ids: tuple[ReferenceId, ...] = ()
    gap_ids: tuple[ReferenceId, ...] = ()
    dependency_gate_ids: tuple[ReferenceId, ...] = ()

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("name", self.name)
        _require_text("description", self.description)
        _require_enum("target_level", self.target_level, TargetLevel)
        if not isinstance(self.mandatory_for_target, bool):
            raise TypeError("mandatory_for_target must be a bool")
        if self.parent_id is not None:
            object.__setattr__(self, "parent_id", reference(self.parent_id))
        for name in ("checklist_ids", "evidence_ids", "gap_ids", "dependency_gate_ids"):
            object.__setattr__(self, name, _references(getattr(self, name)))


@dataclass(frozen=True, slots=True, kw_only=True)
class CapabilityAssessment:
    domain_id: ReferenceId
    raw_score: MaturityScore
    effective_score: MaturityScore
    confidence: ConfidenceRating
    evidence_status: EvidenceStatus
    evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    next_hard_gate_id: ReferenceId
    blocking_dependency_ids: tuple[ReferenceId, ...]
    rationale: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "domain_id", reference(self.domain_id))
        object.__setattr__(self, "raw_score", _score(self.raw_score))
        object.__setattr__(self, "effective_score", _score(self.effective_score))
        if self.effective_score > self.raw_score:
            raise ValueError("effective_score must not exceed raw_score")
        _require_enum("confidence", self.confidence, ConfidenceRating)
        _require_enum("evidence_status", self.evidence_status, EvidenceStatus)
        object.__setattr__(self, "evidence_ids", _references(self.evidence_ids))
        object.__setattr__(self, "limitations", _strings(self.limitations))
        object.__setattr__(self, "next_hard_gate_id", reference(self.next_hard_gate_id))
        object.__setattr__(self, "blocking_dependency_ids", _references(self.blocking_dependency_ids))
        _require_text("rationale", self.rationale)


@dataclass(frozen=True, slots=True, kw_only=True)
class GapEntry:
    id: StableId
    title: str
    primary_category: GapCategory
    secondary_categories: tuple[GapCategory, ...]
    domain_ids: tuple[ReferenceId, ...]
    current_status: EvidenceStatus
    target_level: TargetLevel
    severity: Severity
    dependencies: tuple[ReferenceId, ...]
    acceptance_evidence: tuple[str, ...]
    recommended_owner_area: str
    dependency_criticality: int
    safety_impact: int
    claim_risk: int
    target_unblock_value: int
    observed_fact: str
    recommendation: str

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("title", self.title)
        _require_enum("primary_category", self.primary_category, GapCategory)
        secondary = _enum_values(self.secondary_categories, GapCategory)
        if self.primary_category in secondary:
            raise ValueError("secondary_categories must not repeat primary_category")
        object.__setattr__(self, "secondary_categories", secondary)
        object.__setattr__(self, "domain_ids", _references(self.domain_ids, minimum=1))
        _require_enum("current_status", self.current_status, EvidenceStatus)
        _require_enum("target_level", self.target_level, TargetLevel)
        _require_enum("severity", self.severity, Severity)
        object.__setattr__(self, "dependencies", _references(self.dependencies))
        object.__setattr__(
            self,
            "acceptance_evidence",
            _strings(self.acceptance_evidence, minimum=1),
        )
        for name in ("recommended_owner_area", "observed_fact", "recommendation"):
            _require_text(name, getattr(self, name))
        for name in (
            "dependency_criticality", "safety_impact", "claim_risk", "target_unblock_value"
        ):
            value = getattr(self, name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")


@dataclass(frozen=True, slots=True, kw_only=True)
class HardGate:
    id: StableId
    title: str
    target_level: TargetLevel
    status: EvidenceStatus
    maturity_score: MaturityScore
    dependency_ids: tuple[ReferenceId, ...]
    blocking_domain_ids: tuple[ReferenceId, ...]
    evidence_ids: tuple[ReferenceId, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str
    parallel_branch: str | None = None
    join_gate_ids: tuple[ReferenceId, ...] = ()

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("title", self.title)
        _require_enum("target_level", self.target_level, TargetLevel)
        _require_enum("status", self.status, EvidenceStatus)
        object.__setattr__(self, "maturity_score", _score(self.maturity_score))
        for name in ("dependency_ids", "blocking_domain_ids", "evidence_ids", "join_gate_ids"):
            object.__setattr__(self, name, _references(getattr(self, name)))
        object.__setattr__(
            self,
            "acceptance_evidence",
            _strings(self.acceptance_evidence, minimum=1),
        )
        object.__setattr__(self, "non_claims", _strings(self.non_claims))
        _require_text("owner_area", self.owner_area)
        if self.parallel_branch is not None:
            _require_text("parallel_branch", self.parallel_branch)


@dataclass(frozen=True, slots=True, kw_only=True)
class ObservedConclusion:
    id: StableId
    text: str
    evidence_ids: tuple[ReferenceId, ...]

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("text", self.text)
        object.__setattr__(self, "evidence_ids", _references(self.evidence_ids, minimum=1))


@dataclass(frozen=True, slots=True, kw_only=True)
class Recommendation:
    id: StableId
    text: str
    related_gap_ids: tuple[ReferenceId, ...]

    def __post_init__(self) -> None:
        _normalize_id(self)
        _require_text("text", self.text)
        object.__setattr__(
            self,
            "related_gap_ids",
            _references(self.related_gap_ids, minimum=1),
        )


@dataclass(frozen=True, slots=True, kw_only=True)
class ValidationFinding:
    severity: FindingSeverity
    code: str
    requirement_refs: tuple[str, ...]
    object_refs: tuple[ReferenceId, ...]

    def __post_init__(self) -> None:
        _require_enum("severity", self.severity, FindingSeverity)
        _require_text("code", self.code)
        object.__setattr__(self, "requirement_refs", _strings(self.requirement_refs))
        object.__setattr__(self, "object_refs", _references(self.object_refs))


@dataclass(frozen=True, slots=True, kw_only=True)
class ValidationResult:
    valid: bool
    findings: tuple[ValidationFinding, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.valid, bool):
            raise TypeError("valid must be a bool")
        findings = tuple(self.findings)
        if not all(isinstance(item, ValidationFinding) for item in findings):
            raise TypeError("findings must contain ValidationFinding values")
        findings = tuple(
            sorted(
                findings,
                key=lambda item: (
                    item.severity.value,
                    item.code,
                    tuple(item.requirement_refs),
                    tuple(str(ref) for ref in item.object_refs),
                ),
            )
        )
        object.__setattr__(self, "findings", findings)
        if self.valid and any(item.severity is FindingSeverity.ERROR for item in findings):
            raise ValueError("valid result cannot contain error findings")


@dataclass(frozen=True, slots=True, kw_only=True)
class AssessmentModel:
    revision: AssessmentRevision
    source_inventory: tuple[SourceInventoryEntry, ...] = ()
    evidence_records: tuple[EvidenceRecord, ...] = ()
    conflicts: tuple[EvidenceConflict, ...] = ()
    target_levels: tuple[TargetLevel, ...] = ()
    domains: tuple[CapabilityDomain, ...] = ()
    assessments: tuple[CapabilityAssessment, ...] = ()
    gaps: tuple[GapEntry, ...] = ()
    hard_gates: tuple[HardGate, ...] = ()
    assumptions: tuple[str, ...] = ()
    non_claims: tuple[str, ...] = ()
    observed_conclusions: tuple[ObservedConclusion, ...] = ()
    recommendations: tuple[Recommendation, ...] = ()
    validation: ValidationResult = field(default_factory=lambda: ValidationResult(valid=False))

    def __post_init__(self) -> None:
        if not isinstance(self.revision, AssessmentRevision):
            raise TypeError("revision must be an AssessmentRevision")
        model_collections = (
            ("source_inventory", SourceInventoryEntry),
            ("evidence_records", EvidenceRecord),
            ("conflicts", EvidenceConflict),
            ("domains", CapabilityDomain),
            ("gaps", GapEntry),
            ("hard_gates", HardGate),
            ("observed_conclusions", ObservedConclusion),
            ("recommendations", Recommendation),
        )
        for name, model_type in model_collections:
            values = tuple(getattr(self, name))
            if not all(isinstance(value, model_type) for value in values):
                raise TypeError(f"{name} must contain {model_type.__name__} values")
            object.__setattr__(self, name, _sorted_models(values, "id"))
        assessments = tuple(self.assessments)
        if not all(isinstance(value, CapabilityAssessment) for value in assessments):
            raise TypeError("assessments must contain CapabilityAssessment values")
        object.__setattr__(self, "assessments", _sorted_models(assessments, "domain_id"))
        object.__setattr__(self, "target_levels", _enum_values(self.target_levels, TargetLevel))
        object.__setattr__(self, "assumptions", _strings(self.assumptions))
        object.__setattr__(self, "non_claims", _strings(self.non_claims))
        if not isinstance(self.validation, ValidationResult):
            raise TypeError("validation must be a ValidationResult")


TEnum = TypeVar("TEnum", bound=Enum)


def _normalize_id(instance: Any) -> None:
    object.__setattr__(instance, "id", StableId(instance.id))


def _require_text(name: str, value: Any) -> None:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{name} must be a non-empty string")


def _require_enum(name: str, value: Any, enum_type: type[TEnum]) -> None:
    if not isinstance(value, enum_type):
        raise TypeError(f"{name} must be {enum_type.__name__}")


def _score(value: int | MaturityScore) -> MaturityScore:
    if isinstance(value, bool):
        raise ValueError("maturity score must be an integer from 0 through 5")
    try:
        return MaturityScore(value)
    except (TypeError, ValueError) as error:
        raise ValueError("maturity score must be an integer from 0 through 5") from error


def _references(values: Iterable[str | StableId], *, minimum: int = 0) -> tuple[ReferenceId, ...]:
    normalized = tuple(sorted({reference(value) for value in values}, key=str))
    if len(normalized) < minimum:
        raise ValueError(f"expected at least {minimum} distinct references")
    return normalized


def _strings(values: Iterable[str], *, minimum: int = 0) -> tuple[str, ...]:
    normalized_values: set[str] = set()
    for value in values:
        _require_text("collection item", value)
        normalized_values.add(value)
    normalized = tuple(sorted(normalized_values))
    if len(normalized) < minimum:
        raise ValueError(f"expected at least {minimum} distinct values")
    return normalized


def _enum_values(values: Iterable[TEnum], enum_type: type[TEnum]) -> tuple[TEnum, ...]:
    normalized: set[TEnum] = set()
    for value in values:
        _require_enum("collection item", value, enum_type)
        normalized.add(value)
    return tuple(sorted(normalized, key=lambda item: str(item.value)))


def _sorted_models(values: Iterable[Any], key_name: str) -> tuple[Any, ...]:
    normalized = tuple(values)
    keys = [str(getattr(value, key_name)) for value in normalized]
    if len(keys) != len(set(keys)):
        raise ValueError(f"duplicate {key_name} in model collection")
    return tuple(sorted(normalized, key=lambda value: str(getattr(value, key_name))))


def _sorted_locations(
    values: Iterable[SourceLocation], *, minimum: int = 0
) -> tuple[SourceLocation, ...]:
    normalized = tuple(values)
    if not all(isinstance(value, SourceLocation) for value in normalized):
        raise TypeError("locations must contain SourceLocation values")
    distinct = {(value.kind.value, value.value): value for value in normalized}
    result = tuple(distinct[key] for key in sorted(distinct))
    if len(result) < minimum:
        raise ValueError(f"expected at least {minimum} distinct locations")
    return result
