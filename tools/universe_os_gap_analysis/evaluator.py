"""Shared declarative capability-evaluator framework (Task 5.x foundation).

The Domain Evaluators described in the design consume the Task 4.1
:class:`~tools.universe_os_gap_analysis.evidence.EvidenceBundle` and the Task 4.3
:class:`~tools.universe_os_gap_analysis.claim_guard.GuardedEvidence` layer and
emit a :class:`DomainDraft`: one :class:`~tools.universe_os_gap_analysis.models.CapabilityDomain`
plus the :class:`~tools.universe_os_gap_analysis.models.GapEntry` values that the
Maturity Assessor (Task 8) and gap register (Task 9) later consume.

This module introduces the *reusable* declarative machinery so that the
language/type-system evaluator (Task 5.1), the memory/concurrency/safety
evaluator (Task 5.2), and the ABI/backend/runtime/kernel/userspace evaluators
(Tasks 6.x, 7.x) all describe their domains as data (a :class:`DeclarativeChecklist`)
and share one deterministic evaluation engine (:class:`ChecklistEvaluator`).

Design intent (from design.md "Domain Evaluators"):

* Every checklist item carries a *capability key*, a *target level*, the
  *allowed evidence kinds*, its *specification*, *parser/typechecker
  implementation entries*, *compatibility-policy* sources, *test/gate*
  references, known *non-claims*, a *gap-classification rule*, and an
  *acceptance-evidence* template.
* The three semantic layers -- specification, parser/typechecker
  implementation, and compatibility policy -- are kept **separate** so the
  engine can distinguish "documented", "implemented", and "stabilized" and
  produce the correct gap kind.
* The engine never mutates evidence, never upgrades a status, and is
  order-independent: matching an item against evidence is a pure function of the
  evidence source paths, kinds, and claim keys.

Scope boundaries (owned by later tasks): the engine assigns raw scores or caps
nothing (Maturity Assessor, Task 8), does not rank gaps (Task 9), and does not
render anything (Task 11). It only classifies evidence into layers per item and
emits the gap entries mandated by each item's classification rule.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping

from .claim_guard import GuardedEvidence
from .evidence import EvidenceBundle
from .identifiers import ReferenceId, RepositoryPath, StableId, reference, stable_id
from .models import (
    CapabilityDomain,
    ClosedStrEnum,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    Severity,
    TargetLevel,
)


class EvidenceLayer(ClosedStrEnum):
    """The three separated semantic layers a checklist item is evaluated against.

    Keeping these layers distinct is what lets an evaluator tell a *documented*
    feature (specification only) from an *implemented* one (parser/typechecker)
    and from a *stabilized* one (compatibility policy), which in turn selects the
    correct gap kind (Requirements 5.3, 5.4).
    """

    #: Normative/authoritative specification prose (the feature is documented).
    SPECIFICATION = "Specification"
    #: Parser or typechecker implementation entries (the feature is implemented).
    PARSER_TYPECHECKER = "ParserTypechecker"
    #: Compatibility / stability policy (the feature's semantics are governed).
    COMPATIBILITY_POLICY = "CompatibilityPolicy"
    #: Executable test or gate references (the feature is verified).
    TEST_GATE = "TestGate"


# Evidence kinds that count as direct parser/typechecker *implementation*
# evidence for a language/type-system feature. Specification, RFC, release,
# example, and non-claim kinds are never implementation evidence.
_IMPLEMENTATION_EVIDENCE_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Descending "implemented strength" of every status. Higher means more present;
# this ordering is only used to summarize a documented feature's current status,
# never to upgrade or aggregate a score.
_STATUS_STRENGTH: Mapping[EvidenceStatus, int] = {
    EvidenceStatus.COMPILER_TOOLING_GA: 7,
    EvidenceStatus.BACKEND_SDK_GA: 6,
    EvidenceStatus.INSTALLED_PREVIEW: 5,
    EvidenceStatus.REPO_PREVIEW: 4,
    EvidenceStatus.EXPERIMENTAL: 3,
    EvidenceStatus.PLANNED: 2,
    EvidenceStatus.UNSUPPORTED: 1,
    EvidenceStatus.UNKNOWN: 0,
}


def _path_entries(values: Iterable[str]) -> tuple[str, ...]:
    """Validate exact file paths and ``dir/`` directory-prefix entries.

    An entry ending in ``/`` denotes a directory prefix; the portion before the
    slash must still be a portable repository-relative path. Any other entry
    must be an exact :class:`RepositoryPath`.
    """

    seen: set[str] = set()
    for value in values:
        if not isinstance(value, str) or not value:
            raise ValueError("path entries must be non-empty strings")
        if value.endswith("/"):
            RepositoryPath(value[:-1])  # validate the directory portion
            seen.add(value)
        else:
            seen.add(str(RepositoryPath(value)))
    return tuple(sorted(seen))


def _strings(values: Iterable[str]) -> tuple[str, ...]:
    seen: set[str] = set()
    for value in values:
        if not isinstance(value, str) or not value.strip():
            raise ValueError("expected non-empty strings")
        seen.add(value)
    return tuple(sorted(seen))


def _path_matches(source_path: str, entry: str) -> bool:
    """Match a record path against an item entry (exact file or directory prefix).

    A trailing ``/`` marks a directory prefix so an entry like ``frontend/``
    matches every file beneath it; otherwise the match is an exact file path.
    """

    if entry.endswith("/"):
        return source_path == entry[:-1] or source_path.startswith(entry)
    return source_path == entry


@dataclass(frozen=True, slots=True, kw_only=True)
class ChecklistItem:
    """A single declarative capability check with its three separated layers."""

    key: str
    title: str
    aspect: str
    specification_paths: tuple[str, ...] = ()
    implementation_entries: tuple[str, ...] = ()
    compatibility_policy_paths: tuple[str, ...] = ()
    test_gate_refs: tuple[str, ...] = ()
    allowed_evidence_kinds: tuple[EvidenceKind, ...] = ()
    known_non_claims: tuple[str, ...] = ()
    acceptance_evidence: tuple[str, ...] = ()
    recommended_owner_area: str = "Language & type system"
    dependency_criticality: int = 0
    safety_impact: int = 0
    claim_risk: int = 0
    target_unblock_value: int = 0

    def __post_init__(self) -> None:
        if not isinstance(self.key, str) or not self.key.strip():
            raise ValueError("checklist item key must be a non-empty string")
        for name in ("title", "aspect", "recommended_owner_area"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{name} must be a non-empty string")
        object.__setattr__(self, "specification_paths", _path_entries(self.specification_paths))
        object.__setattr__(self, "implementation_entries", _path_entries(self.implementation_entries))
        object.__setattr__(
            self, "compatibility_policy_paths", _path_entries(self.compatibility_policy_paths)
        )
        object.__setattr__(self, "test_gate_refs", _strings(self.test_gate_refs))
        object.__setattr__(self, "known_non_claims", _strings(self.known_non_claims))
        object.__setattr__(self, "acceptance_evidence", _strings(self.acceptance_evidence))
        kinds = tuple(self.allowed_evidence_kinds)
        if not all(isinstance(kind, EvidenceKind) for kind in kinds):
            raise TypeError("allowed_evidence_kinds must contain EvidenceKind values")
        object.__setattr__(
            self,
            "allowed_evidence_kinds",
            tuple(sorted(set(kinds), key=lambda kind: kind.value)),
        )
        if not self.acceptance_evidence:
            raise ValueError("each checklist item must define acceptance evidence")
        for name in (
            "dependency_criticality",
            "safety_impact",
            "claim_risk",
            "target_unblock_value",
        ):
            value = getattr(self, name)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError(f"{name} must be a non-negative integer")

    @property
    def item_id(self) -> StableId:
        return stable_id("checklist", self.key)


@dataclass(frozen=True, slots=True, kw_only=True)
class DeclarativeChecklist:
    """A domain-level checklist: metadata plus its ordered checklist items."""

    domain_key: str
    name: str
    target_level: TargetLevel
    description: str
    mandatory_for_target: bool = True
    items: tuple[ChecklistItem, ...] = ()

    def __post_init__(self) -> None:
        for name in ("domain_key", "name", "description"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{name} must be a non-empty string")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.mandatory_for_target, bool):
            raise TypeError("mandatory_for_target must be a bool")
        items = tuple(self.items)
        if not items:
            raise ValueError("a checklist must define at least one item")
        if not all(isinstance(item, ChecklistItem) for item in items):
            raise TypeError("items must contain ChecklistItem values")
        keys = [item.key for item in items]
        if len(keys) != len(set(keys)):
            raise ValueError("checklist item keys must be unique")
        object.__setattr__(self, "items", tuple(sorted(items, key=lambda item: item.key)))

    @property
    def domain_id(self) -> StableId:
        return stable_id("domain", self.domain_key)


@dataclass(frozen=True, slots=True, kw_only=True)
class ChecklistItemFinding:
    """Per-item classification: which layers matched and which gaps were emitted."""

    item_key: str
    item_id: ReferenceId
    aspect: str
    has_specification: bool
    has_parser_typechecker: bool
    has_compatibility_policy: bool
    has_test_gate: bool
    current_status: EvidenceStatus
    evidence_by_layer: Mapping[EvidenceLayer, tuple[ReferenceId, ...]]
    gap_ids: tuple[ReferenceId, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "item_id", reference(self.item_id))
        if not isinstance(self.current_status, EvidenceStatus):
            raise TypeError("current_status must be an EvidenceStatus")
        object.__setattr__(
            self,
            "evidence_by_layer",
            {layer: tuple(ids) for layer, ids in self.evidence_by_layer.items()},
        )
        object.__setattr__(self, "gap_ids", tuple(self.gap_ids))

    @property
    def matched_evidence_ids(self) -> tuple[ReferenceId, ...]:
        seen: dict[str, ReferenceId] = {}
        for ids in self.evidence_by_layer.values():
            for ref in ids:
                seen[str(ref)] = reference(ref)
        return tuple(seen[key] for key in sorted(seen))


@dataclass(frozen=True, slots=True)
class DomainDraft:
    """An evaluator's output: one capability domain plus its gaps and findings."""

    domain: CapabilityDomain
    gaps: tuple[GapEntry, ...]
    findings: tuple[ChecklistItemFinding, ...]

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        gaps = tuple(self.gaps)
        if not all(isinstance(gap, GapEntry) for gap in gaps):
            raise TypeError("gaps must contain GapEntry values")
        findings = tuple(self.findings)
        if not all(isinstance(item, ChecklistItemFinding) for item in findings):
            raise TypeError("findings must contain ChecklistItemFinding values")
        object.__setattr__(self, "gaps", tuple(sorted(gaps, key=lambda gap: str(gap.id))))
        object.__setattr__(
            self, "findings", tuple(sorted(findings, key=lambda item: item.item_key))
        )

    def gaps_by_category(self, category: GapCategory) -> tuple[GapEntry, ...]:
        return tuple(gap for gap in self.gaps if gap.primary_category is category)

    def finding_for(self, item_key: str) -> ChecklistItemFinding | None:
        for finding in self.findings:
            if finding.item_key == item_key:
                return finding
        return None


@dataclass(frozen=True, slots=True)
class _MatchedLayers:
    """Evidence references grouped by the layer they satisfy for one item."""

    by_layer: dict[EvidenceLayer, list[ReferenceId]] = field(default_factory=dict)

    def add(self, layer: EvidenceLayer, record_id: ReferenceId) -> None:
        self.by_layer.setdefault(layer, [])
        if record_id not in self.by_layer[layer]:
            self.by_layer[layer].append(record_id)

    def layer_ids(self, layer: EvidenceLayer) -> tuple[ReferenceId, ...]:
        return tuple(sorted(self.by_layer.get(layer, []), key=str))

    def frozen(self) -> Mapping[EvidenceLayer, tuple[ReferenceId, ...]]:
        return {layer: self.layer_ids(layer) for layer in EvidenceLayer if layer in self.by_layer}

    @property
    def all_ids(self) -> tuple[ReferenceId, ...]:
        seen: dict[str, ReferenceId] = {}
        for ids in self.by_layer.values():
            for ref in ids:
                seen[str(ref)] = ref
        return tuple(seen[key] for key in sorted(seen))


class ChecklistEvaluator:
    """Deterministically evaluate a declarative checklist against evidence.

    The engine matches each :class:`ChecklistItem` against the evidence bundle by
    source path (specification, implementation, compatibility policy) and by
    claim key (test/gate references), summarizes the item's current status from
    the Claim Guard, and emits gap entries per the shared classification rule:

    * every documented feature yields a ``Language_Gap`` referencing its
      authoritative source and any direct implementation evidence
      (Requirement 5.3); and
    * a feature with parser/typechecker implementation but no compatibility
      policy yields an additional semantic-stability ``Verification_Gap``
      (Requirement 5.4).

    Subclasses may override :meth:`classify_item_gaps` to add domain-specific
    gap rules; the base rule already satisfies the language/type-system needs.
    """

    def evaluate(
        self,
        checklist: DeclarativeChecklist,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> DomainDraft:
        if not isinstance(checklist, DeclarativeChecklist):
            raise TypeError("checklist must be a DeclarativeChecklist")
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is not None and not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence or None")

        records = bundle.records
        findings: list[ChecklistItemFinding] = []
        gaps: list[GapEntry] = []
        domain_evidence: set[ReferenceId] = set()
        domain_id = reference(checklist.domain_id)

        for item in checklist.items:
            matched = self._match_item(item, records)
            status = self._current_status(matched, records, guarded)
            item_gaps = self.classify_item_gaps(
                checklist=checklist,
                item=item,
                matched=matched,
                current_status=status,
                domain_id=domain_id,
            )
            gaps.extend(item_gaps)
            gap_ids = tuple(reference(gap.id) for gap in item_gaps)
            domain_evidence.update(matched.all_ids)
            findings.append(
                ChecklistItemFinding(
                    item_key=item.key,
                    item_id=reference(item.item_id),
                    aspect=item.aspect,
                    has_specification=bool(matched.by_layer.get(EvidenceLayer.SPECIFICATION)),
                    has_parser_typechecker=bool(
                        matched.by_layer.get(EvidenceLayer.PARSER_TYPECHECKER)
                    ),
                    has_compatibility_policy=bool(
                        matched.by_layer.get(EvidenceLayer.COMPATIBILITY_POLICY)
                    ),
                    has_test_gate=bool(matched.by_layer.get(EvidenceLayer.TEST_GATE)),
                    current_status=status,
                    evidence_by_layer=matched.frozen(),
                    gap_ids=gap_ids,
                )
            )

        domain = CapabilityDomain(
            id=checklist.domain_id,
            name=checklist.name,
            target_level=checklist.target_level,
            description=checklist.description,
            mandatory_for_target=checklist.mandatory_for_target,
            checklist_ids=tuple(reference(item.item_id) for item in checklist.items),
            evidence_ids=tuple(sorted(domain_evidence, key=str)),
            gap_ids=tuple(reference(gap.id) for gap in gaps),
        )
        return DomainDraft(domain=domain, gaps=tuple(gaps), findings=tuple(findings))

    # -- classification --------------------------------------------------- #

    def classify_item_gaps(
        self,
        *,
        checklist: DeclarativeChecklist,
        item: ChecklistItem,
        matched: "_MatchedLayers",
        current_status: EvidenceStatus,
        domain_id: ReferenceId,
    ) -> tuple[GapEntry, ...]:
        """Emit the gap entries for one checklist item (shared base rule).

        The base rule implements Requirements 5.3 and 5.4. Documented features
        always produce a ``Language_Gap``; features that have implementation but
        no compatibility policy additionally produce a semantic-stability
        ``Verification_Gap``. Both gaps keep exactly one primary category.
        """

        gaps: list[GapEntry] = []
        impl_ids = matched.layer_ids(EvidenceLayer.PARSER_TYPECHECKER)
        spec_ids = matched.layer_ids(EvidenceLayer.SPECIFICATION)

        # Requirement 5.3: a documented language feature yields a Language_Gap
        # that references the authoritative source path and any direct
        # implementation evidence.
        observed = self._language_observed_fact(item, spec_ids, impl_ids, current_status)
        gaps.append(
            GapEntry(
                id=stable_id("gap", "language", item.key),
                title=f"Normative language contract: {item.title}",
                primary_category=GapCategory.LANGUAGE,
                secondary_categories=(),
                domain_ids=(domain_id,),
                current_status=current_status,
                target_level=checklist.target_level,
                severity=self._severity(item),
                dependencies=(),
                acceptance_evidence=item.acceptance_evidence,
                recommended_owner_area=item.recommended_owner_area,
                dependency_criticality=item.dependency_criticality,
                safety_impact=item.safety_impact,
                claim_risk=item.claim_risk,
                target_unblock_value=item.target_unblock_value,
                observed_fact=observed,
                recommendation=(
                    f"Publish and freeze the normative semantics for {item.title} "
                    "with a versioned compatibility contract before depending on it "
                    "for system-level work."
                ),
            )
        )

        # Requirement 5.4: parser/typechecker support without a compatibility
        # policy is a semantic-stability Verification_Gap.
        if impl_ids and not matched.layer_ids(EvidenceLayer.COMPATIBILITY_POLICY):
            gaps.append(
                GapEntry(
                    id=stable_id("gap", "verification", item.key),
                    title=f"Semantic stability: {item.title}",
                    primary_category=GapCategory.VERIFICATION,
                    secondary_categories=(),
                    domain_ids=(domain_id,),
                    current_status=current_status,
                    target_level=checklist.target_level,
                    severity=self._severity(item),
                    dependencies=(),
                    acceptance_evidence=(
                        f"A published compatibility/stability policy governing {item.title} "
                        "with cross-revision guarantees and a negative-change process.",
                    ),
                    recommended_owner_area=item.recommended_owner_area,
                    dependency_criticality=item.dependency_criticality,
                    safety_impact=item.safety_impact,
                    claim_risk=item.claim_risk + 1,
                    target_unblock_value=item.target_unblock_value,
                    observed_fact=(
                        f"{item.title} has parser/typechecker implementation evidence "
                        f"({', '.join(str(ref) for ref in impl_ids)}) but no compatibility "
                        "policy, so its semantics are not yet stable across revisions."
                    ),
                    recommendation=(
                        f"Define a compatibility policy for {item.title} so implemented "
                        "behavior cannot silently change between revisions."
                    ),
                )
            )
        return tuple(gaps)

    # -- helpers ---------------------------------------------------------- #

    @staticmethod
    def _match_item(item: ChecklistItem, records: tuple[EvidenceRecord, ...]) -> "_MatchedLayers":
        matched = _MatchedLayers()
        for record in records:
            source_path = str(record.source_path)
            ref = reference(record.id)
            if any(_path_matches(source_path, entry) for entry in item.specification_paths):
                matched.add(EvidenceLayer.SPECIFICATION, ref)
            if (
                record.evidence_kind in _IMPLEMENTATION_EVIDENCE_KINDS
                and any(_path_matches(source_path, entry) for entry in item.implementation_entries)
            ):
                matched.add(EvidenceLayer.PARSER_TYPECHECKER, ref)
            if any(
                _path_matches(source_path, entry) for entry in item.compatibility_policy_paths
            ):
                matched.add(EvidenceLayer.COMPATIBILITY_POLICY, ref)
            if any(gate_ref in record.claim_key for gate_ref in item.test_gate_refs):
                matched.add(EvidenceLayer.TEST_GATE, ref)
        return matched

    @staticmethod
    def _current_status(
        matched: "_MatchedLayers",
        records: tuple[EvidenceRecord, ...],
        guarded: GuardedEvidence | None,
    ) -> EvidenceStatus:
        record_by_id = {str(record.id): record for record in records}
        statuses: list[EvidenceStatus] = []
        for ref in matched.all_ids:
            key = str(ref)
            if guarded is not None:
                claim = guarded.claim_for(key)
                if claim is not None:
                    statuses.append(claim.status)
                    continue
            record = record_by_id.get(key)
            if record is not None:
                statuses.append(record.status)
        if not statuses:
            return EvidenceStatus.UNKNOWN
        return max(statuses, key=lambda status: _STATUS_STRENGTH[status])

    @staticmethod
    def _language_observed_fact(
        item: ChecklistItem,
        spec_ids: tuple[ReferenceId, ...],
        impl_ids: tuple[ReferenceId, ...],
        current_status: EvidenceStatus,
    ) -> str:
        spec_paths = ", ".join(str(path) for path in item.specification_paths) or "(none)"
        impl_note = (
            f"direct implementation evidence {', '.join(str(ref) for ref in impl_ids)}"
            if impl_ids
            else "no direct implementation evidence"
        )
        return (
            f"{item.title} is documented in {spec_paths} with {impl_note}; "
            f"current classified status is {current_status.value}."
        )

    @staticmethod
    def _severity(item: ChecklistItem) -> Severity:
        if item.safety_impact >= 2:
            return Severity.CRITICAL
        if item.safety_impact == 1 or item.dependency_criticality >= 2:
            return Severity.HIGH
        if item.dependency_criticality == 1:
            return Severity.MEDIUM
        return Severity.LOW
