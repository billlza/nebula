"""Read-only repository evidence manifest for the bound assessment revision.

Task 14.1 collects a curated, deterministic snapshot of the repository evidence
that later baseline work (Task 14.2) builds on. The manifest is assembled by
composing the existing read-only components:

* :func:`tools.universe_os_gap_analysis.revision.bind` freezes the assessment
  revision identity (commit / branch / VERSION / tags / worktree cleanliness).
* :func:`tools.universe_os_gap_analysis.inventory.discover_source_inventory`
  enumerates every Requirement 1.3 source category with stable anchors and
  ``inspected`` / ``Validated`` / ``NotRun`` / ``Unavailable`` execution state.
* :func:`tools.universe_os_gap_analysis.adapters.adapt_repository_evidence`
  parses the UniverseOS gate registry and every ``case.toml`` so gate, case,
  dependency, and non-claim references can be confirmed to resolve.

The builder never executes commands, never follows symbolic links, and never
mutates product source or repository evidence files. It only reads the
repository and returns an immutable, stably ordered manifest that can be
serialized deterministically.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Iterable, Mapping, Sequence

from .adapters import AdapterBundle, adapt_repository_evidence
from .inventory import (
    REQUIRED_SOURCE_CATEGORIES,
    discover_source_inventory,
)
from .models import (
    AssessmentRevision,
    ExecutionState,
    RevisionOrigin,
    SourceCategory,
    SourceInventoryEntry,
)
from .revision import RevisionBinder

MANIFEST_SCHEMA_VERSION = "1.0"

# How many representative repository-relative paths and stable anchors are kept
# per category so the manifest stays a curated summary rather than a full dump.
_MAX_REPRESENTATIVE_PATHS = 5
_MAX_REPRESENTATIVE_ANCHORS = 8


@dataclass(frozen=True, slots=True)
class CategoryManifest:
    """Deterministic coverage summary for a single source category."""

    category: SourceCategory
    entry_count: int
    inspected_count: int
    validated_count: int
    not_run_count: int
    unavailable_count: int
    failed_count: int
    representative_paths: tuple[str, ...]
    representative_anchors: tuple[str, ...]

    @property
    def available(self) -> bool:
        """A category is available when at least one entry was inspected."""

        return self.inspected_count > 0

    def to_dict(self) -> dict[str, object]:
        return {
            "category": self.category.value,
            "available": self.available,
            "entryCount": self.entry_count,
            "inspectedCount": self.inspected_count,
            "executionStates": {
                ExecutionState.VALIDATED.value: self.validated_count,
                ExecutionState.NOT_RUN.value: self.not_run_count,
                ExecutionState.UNAVAILABLE.value: self.unavailable_count,
                ExecutionState.FAILED.value: self.failed_count,
            },
            "representativePaths": list(self.representative_paths),
            "representativeAnchors": list(self.representative_anchors),
        }


@dataclass(frozen=True, slots=True)
class GateReference:
    """A UniverseOS gate whose dependency / case / non-claim references resolved."""

    gate_id: str
    status: str
    owner_area: str
    dependency_ids: tuple[str, ...]
    evidence_case_ids: tuple[str, ...]
    non_claim: str
    source_path: str
    dependencies_resolved: bool
    evidence_cases_resolved: bool

    @property
    def resolved(self) -> bool:
        return self.dependencies_resolved and self.evidence_cases_resolved

    def to_dict(self) -> dict[str, object]:
        return {
            "gateId": self.gate_id,
            "status": self.status,
            "ownerArea": self.owner_area,
            "dependencyIds": list(self.dependency_ids),
            "evidenceCaseIds": list(self.evidence_case_ids),
            "nonClaim": self.non_claim,
            "sourcePath": self.source_path,
            "dependenciesResolved": self.dependencies_resolved,
            "evidenceCasesResolved": self.evidence_cases_resolved,
            "resolved": self.resolved,
        }


@dataclass(frozen=True, slots=True)
class RevisionOriginSummary:
    """The three non-interchangeable evidence axes of the bound revision."""

    commit_id: str
    branch: str
    version: str
    describe: str
    tags: tuple[str, ...]
    worktree_clean: bool
    inventory_origin: RevisionOrigin
    assessed_at_utc: str

    def to_dict(self) -> dict[str, object]:
        return {
            "committedRevision": {"commitId": self.commit_id, "branch": self.branch},
            "taggedRelease": {"describe": self.describe, "tags": list(self.tags)},
            "currentWorktree": {
                "baseCommitId": self.commit_id,
                "worktreeClean": self.worktree_clean,
                "inventoryOrigin": self.inventory_origin.value,
            },
            "version": self.version,
            "assessedAtUtc": self.assessed_at_utc,
        }


@dataclass(frozen=True, slots=True)
class RepositoryManifest:
    """Immutable, deterministically ordered repository evidence manifest."""

    schema_version: str
    revision_origin: RevisionOriginSummary
    categories: tuple[CategoryManifest, ...]
    missing_categories: tuple[SourceCategory, ...]
    total_entries: int
    total_stable_anchors: int
    execution_state_totals: Mapping[ExecutionState, int]
    origin_totals: Mapping[RevisionOrigin, int]
    gate_registry_version: int
    gates: tuple[GateReference, ...]
    case_ids: tuple[str, ...]
    non_claims: tuple[str, ...]
    unresolved_gate_ids: tuple[str, ...]

    @property
    def required_categories_covered(self) -> bool:
        """Every Requirement 1.3 category is represented (inspected)."""

        return not self.missing_categories

    @property
    def gate_references_resolved(self) -> bool:
        """All gate dependency and evidence-case references resolve."""

        return not self.unresolved_gate_ids

    def to_dict(self) -> dict[str, object]:
        return {
            "schemaVersion": self.schema_version,
            "revisionOrigin": self.revision_origin.to_dict(),
            "requiredCategoriesCovered": self.required_categories_covered,
            "missingCategories": [item.value for item in self.missing_categories],
            "totals": {
                "entries": self.total_entries,
                "stableAnchors": self.total_stable_anchors,
                "executionStates": {
                    state.value: self.execution_state_totals.get(state, 0)
                    for state in (
                        ExecutionState.VALIDATED,
                        ExecutionState.NOT_RUN,
                        ExecutionState.UNAVAILABLE,
                        ExecutionState.FAILED,
                    )
                },
                "origins": {
                    origin.value: self.origin_totals.get(origin, 0)
                    for origin in RevisionOrigin
                },
            },
            "categories": [item.to_dict() for item in self.categories],
            "gateRegistry": {
                "version": self.gate_registry_version,
                "referencesResolved": self.gate_references_resolved,
                "unresolvedGateIds": list(self.unresolved_gate_ids),
                "gates": [item.to_dict() for item in self.gates],
                "caseIds": list(self.case_ids),
                "nonClaims": list(self.non_claims),
            },
        }


def _summarize_categories(
    entries: Sequence[SourceInventoryEntry],
) -> tuple[CategoryManifest, ...]:
    grouped: dict[SourceCategory, list[SourceInventoryEntry]] = {}
    for entry in entries:
        grouped.setdefault(entry.category, []).append(entry)

    summaries: list[CategoryManifest] = []
    for category, category_entries in grouped.items():
        ordered = sorted(category_entries, key=lambda item: str(item.path))
        state_counter: Counter[ExecutionState] = Counter(
            entry.execution_state for entry in ordered
        )
        anchors = sorted(
            {anchor for entry in ordered for anchor in entry.stable_anchors}
        )
        summaries.append(
            CategoryManifest(
                category=category,
                entry_count=len(ordered),
                inspected_count=sum(1 for entry in ordered if entry.inspected),
                validated_count=state_counter.get(ExecutionState.VALIDATED, 0),
                not_run_count=state_counter.get(ExecutionState.NOT_RUN, 0),
                unavailable_count=state_counter.get(ExecutionState.UNAVAILABLE, 0),
                failed_count=state_counter.get(ExecutionState.FAILED, 0),
                representative_paths=tuple(
                    str(entry.path) for entry in ordered[:_MAX_REPRESENTATIVE_PATHS]
                ),
                representative_anchors=tuple(anchors[:_MAX_REPRESENTATIVE_ANCHORS]),
            )
        )
    return tuple(sorted(summaries, key=lambda item: item.category.value))


def _summarize_gates(bundle: AdapterBundle) -> tuple[tuple[GateReference, ...], tuple[str, ...]]:
    known_gate_ids = frozenset(gate.gate_id for gate in bundle.gates)
    known_case_ids = frozenset(case.case_id for case in bundle.cases)

    references: list[GateReference] = []
    unresolved: list[str] = []
    for gate in sorted(bundle.gates, key=lambda item: item.gate_id):
        dependencies_resolved = all(
            dependency in known_gate_ids for dependency in gate.dependency_ids
        )
        evidence_cases_resolved = all(
            case_id in known_case_ids for case_id in gate.evidence_case_ids
        )
        reference = GateReference(
            gate_id=gate.gate_id,
            status=gate.status,
            owner_area=gate.owner_area,
            dependency_ids=tuple(gate.dependency_ids),
            evidence_case_ids=tuple(gate.evidence_case_ids),
            non_claim=gate.non_claim,
            source_path=gate.source_path,
            dependencies_resolved=dependencies_resolved,
            evidence_cases_resolved=evidence_cases_resolved,
        )
        references.append(reference)
        if not reference.resolved:
            unresolved.append(gate.gate_id)
    return tuple(references), tuple(sorted(unresolved))


def build_repository_manifest(
    repo_root: Path,
    *,
    revision: AssessmentRevision | None = None,
    clock: Callable[[], datetime] | None = None,
    required_categories: Iterable[SourceCategory] = REQUIRED_SOURCE_CATEGORIES,
) -> RepositoryManifest:
    """Assemble a deterministic, read-only repository evidence manifest.

    When ``revision`` is omitted the manifest binds to the repository at
    ``repo_root`` via the read-only :class:`RevisionBinder`. The discovered
    inventory, gate registry, and ``case.toml`` references are summarised into a
    single immutable :class:`RepositoryManifest`. No commands are executed and no
    product or evidence files are modified.
    """

    root = Path(repo_root)
    bound = revision if revision is not None else RevisionBinder().bind(root, (), clock)

    entries = discover_source_inventory(root, bound, required_categories=required_categories)
    bundle = adapt_repository_evidence(root, entries)

    categories = _summarize_categories(entries)
    covered = {summary.category for summary in categories if summary.available}
    missing = tuple(
        sorted(
            (item for item in frozenset(required_categories) if item not in covered),
            key=lambda item: item.value,
        )
    )

    execution_state_totals: Counter[ExecutionState] = Counter(
        entry.execution_state for entry in entries
    )
    origin_totals: Counter[RevisionOrigin] = Counter(
        entry.revision_origin for entry in entries
    )
    total_stable_anchors = sum(len(entry.stable_anchors) for entry in entries)

    gates, unresolved_gate_ids = _summarize_gates(bundle)
    non_claims = tuple(sorted({gate.non_claim for gate in bundle.gates}))
    case_ids = tuple(sorted(case.case_id for case in bundle.cases))

    inventory_origin = (
        RevisionOrigin.COMMITTED_REVISION
        if bound.worktree_clean
        else RevisionOrigin.CURRENT_WORKTREE
    )
    revision_origin = RevisionOriginSummary(
        commit_id=bound.commit_id,
        branch=bound.branch,
        version=bound.version,
        describe=bound.describe,
        tags=tuple(bound.tags),
        worktree_clean=bound.worktree_clean,
        inventory_origin=inventory_origin,
        assessed_at_utc=bound.assessed_at_utc.isoformat(),
    )

    return RepositoryManifest(
        schema_version=MANIFEST_SCHEMA_VERSION,
        revision_origin=revision_origin,
        categories=categories,
        missing_categories=missing,
        total_entries=len(entries),
        total_stable_anchors=total_stable_anchors,
        execution_state_totals=dict(execution_state_totals),
        origin_totals=dict(origin_totals),
        gate_registry_version=bundle.gate_registry_version,
        gates=gates,
        case_ids=case_ids,
        non_claims=non_claims,
        unresolved_gate_ids=unresolved_gate_ids,
    )
