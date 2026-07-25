"""Raw and effective maturity assessor (Task 8.2).

This module turns the domain evaluators' direct evidence into per-domain
maturity, following the design's Maturity Assessor contract exactly:

* A domain's **raw score** is derived from *direct evidence only* on the ordinal
  0..5 rubric (Requirement 3.1, 3.2). The rungs are:

    - 0 -- no implementation evidence (Requirement 3.6, 10.6, 15.5);
    - 1 -- a narrow experimental implementation;
    - 2 -- a repeatable repository-local implementation;
    - 3 -- a candidate contract verified across supported hosts *with* migration
      and rollback evidence *and* release-review evidence;
    - 4 -- a supported production capability (score 3 conditions plus supported
      production evidence); and
    - 5 -- a mature independent ecosystem capability.

  Because rung 3 requires cross-supported-host candidate + migration/rollback +
  release-review evidence, *no* domain can exceed 2 without those three signals.
  For language/tooling domains this is the explicit Requirement 15.4 cap; the
  same gate applies to every domain (Requirement 3.2).

* A domain's **effective score** is computed in **topological order** over the
  Task 8.1 validated :class:`~tools.universe_os_gap_analysis.hard_gate_graph.HardGateGraph`
  as ``min(raw, every blocking dependency/gate score)`` (Requirement 3.4). Only
  *blocking* edges cap; non-blocking associations never reduce a score. Each
  gate's own effective score is propagated transitively so a deep prerequisite
  gap holds a downstream domain at its floor.

* No implementation evidence fixes **both** raw and effective at 0 regardless of
  plans, prerequisites, examples, or adjacent capabilities (Requirement 3.6,
  10.6, 15.5).

The assessor is ordinal-only: it never produces a percentage, average, sum, or
schedule estimate (Requirement 3.2, 3.7). It also does **not** decide
target-level achievement (Task 8.3); instead it exposes every gate's effective
score, every domain's effective score, and a per-domain capping trace so the
target-achievement stage can consume them.

The assessor never mutates evidence, never upgrades a status, and edits no
evaluator or product module. It builds on ``hard_gate_graph`` (imported by module
path) and fails closed with a ``MAT-*`` code on any structural problem.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from .hard_gate_graph import HardGateGraph
from .identifiers import ReferenceId, reference
from .models import (
    CapabilityAssessment,
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    MaturityScore,
)

# --------------------------------------------------------------------------- #
# MAT-* error codes (fail closed; the graph already ran GRF-* validation).    #
# --------------------------------------------------------------------------- #
MAT_UNKNOWN_GATE = "MAT-UNKNOWN-GATE"
MAT_DUPLICATE_DOMAIN = "MAT-DUPLICATE-DOMAIN"
MAT_MISSING_NEXT_GATE = "MAT-MISSING-NEXT-GATE"

# Evidence kinds that count as *direct implementation* of a current capability.
# Specification, RFC, release, workflow, example, and non-claim kinds never do.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Statuses that assert a present-tense/current implementation to some degree.
# Planned/Unsupported/Unknown are never implementation evidence.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Statuses whose evidence, when directly implemented, demonstrates a repeatable
# repository-local implementation (rung 2) rather than a narrow experiment.
_REPEATABLE_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
    }
)

# Evidence kinds that, on their own, show a capability is reproducible (rung 2).
_REPEATABLE_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)


class MaturityAssessmentError(ValueError):
    """A fail-closed maturity error carrying a ``MAT-*`` code and object refs."""

    def __init__(self, code: str, message: str, object_refs: Iterable[str] = ()) -> None:
        self.code = code
        self.object_refs = tuple(sorted({str(ref) for ref in object_refs}))
        detail = ", ".join(self.object_refs)
        suffix = f" [{detail}]" if detail else ""
        super().__init__(f"{code}: {message}{suffix}")


class DomainClass(str, Enum):
    """Whether a domain is subject to the named language/tooling maturity cap.

    Both classes are capped at 2 without score-3 evidence (Requirement 3.2);
    ``LANGUAGE_TOOLING`` additionally records the Requirement 15.4 limitation
    when that cap binds so the report can cite it explicitly.
    """

    LANGUAGE_TOOLING = "language_tooling"
    GENERAL = "general"


@dataclass(frozen=True, slots=True, kw_only=True)
class DomainMaturityInput:
    """The direct-evidence inputs the assessor needs for one capability domain.

    ``direct_evidence`` are the records the evaluator attributed to *this* domain;
    only direct implementation records (source/test-execution/artifact with an
    implemented status) can lift the raw score above 0. ``gate_id`` ties the
    domain to its representative node in the validated Hard-Gate graph; the
    *blocking* dependencies of that gate cap the domain's effective score.

    The score-3+ signals (``cross_host_candidate_contract``, ``migration_rollback``,
    ``release_review``, ``supported_production``, ``mature_ecosystem``) are the
    evaluator's judgement about evidence that is not reducible to a single record
    kind; they default to ``False`` so a domain stays capped at 2 until every
    score-3 condition is demonstrated.
    """

    domain_id: str
    gate_id: str
    direct_evidence: tuple[EvidenceRecord, ...] = ()
    domain_class: DomainClass = DomainClass.GENERAL
    confidence: ConfidenceRating = ConfidenceRating.LOW
    evidence_status: EvidenceStatus = EvidenceStatus.UNKNOWN
    limitations: tuple[str, ...] = ()
    rationale_note: str = ""
    next_hard_gate_id: str | None = None
    cross_host_candidate_contract: bool = False
    migration_rollback: bool = False
    release_review: bool = False
    supported_production: bool = False
    mature_ecosystem: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.domain_id, str) or not self.domain_id.strip():
            raise ValueError("domain_id must be a non-empty string")
        if not isinstance(self.gate_id, str) or not self.gate_id.strip():
            raise ValueError("gate_id must be a non-empty string")
        evidence = tuple(self.direct_evidence)
        if not all(isinstance(item, EvidenceRecord) for item in evidence):
            raise TypeError("direct_evidence must contain EvidenceRecord values")
        object.__setattr__(self, "direct_evidence", evidence)
        if not isinstance(self.domain_class, DomainClass):
            raise TypeError("domain_class must be a DomainClass")
        if not isinstance(self.confidence, ConfidenceRating):
            raise TypeError("confidence must be a ConfidenceRating")
        if not isinstance(self.evidence_status, EvidenceStatus):
            raise TypeError("evidence_status must be an EvidenceStatus")
        limitations = tuple(self.limitations)
        if not all(isinstance(item, str) and item.strip() for item in limitations):
            raise ValueError("limitations must be non-empty strings")
        object.__setattr__(self, "limitations", limitations)
        for name in (
            "cross_host_candidate_contract",
            "migration_rollback",
            "release_review",
            "supported_production",
            "mature_ecosystem",
        ):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")
        if self.next_hard_gate_id is not None and (
            not isinstance(self.next_hard_gate_id, str) or not self.next_hard_gate_id.strip()
        ):
            raise ValueError("next_hard_gate_id must be a non-empty string or None")

    @property
    def score_three_ready(self) -> bool:
        """Whether the three score-3 conditions are all demonstrated."""

        return (
            self.cross_host_candidate_contract
            and self.migration_rollback
            and self.release_review
        )


@dataclass(frozen=True, slots=True, kw_only=True)
class CapStep:
    """One blocking-dependency cap applied (or considered) for a domain.

    ``binding`` is ``True`` when this dependency's effective score is what holds
    the domain below its raw score (i.e., it is the tightest active cap).
    """

    dependency_gate_id: ReferenceId
    dependency_effective_score: MaturityScore
    cap_rationale: str
    binding: bool

    def __post_init__(self) -> None:
        object.__setattr__(self, "dependency_gate_id", reference(self.dependency_gate_id))
        if not isinstance(self.dependency_effective_score, MaturityScore):
            raise TypeError("dependency_effective_score must be a MaturityScore")
        if not isinstance(self.cap_rationale, str) or not self.cap_rationale.strip():
            raise ValueError("cap_rationale must be a non-empty string")
        if not isinstance(self.binding, bool):
            raise TypeError("binding must be a bool")


@dataclass(frozen=True, slots=True, kw_only=True)
class DomainMaturityResult:
    """A domain's assessment plus the raw/effective trace for Task 8.3."""

    assessment: CapabilityAssessment
    raw_score: MaturityScore
    effective_score: MaturityScore
    cap_trace: tuple[CapStep, ...]
    language_tooling_capped: bool

    def __post_init__(self) -> None:
        if not isinstance(self.assessment, CapabilityAssessment):
            raise TypeError("assessment must be a CapabilityAssessment")
        for name in ("raw_score", "effective_score"):
            if not isinstance(getattr(self, name), MaturityScore):
                raise TypeError(f"{name} must be a MaturityScore")
        if self.effective_score > self.raw_score:
            raise ValueError("effective_score must not exceed raw_score")
        cap_trace = tuple(self.cap_trace)
        if not all(isinstance(step, CapStep) for step in cap_trace):
            raise TypeError("cap_trace must contain CapStep values")
        object.__setattr__(
            self,
            "cap_trace",
            tuple(sorted(cap_trace, key=lambda step: str(step.dependency_gate_id))),
        )
        if not isinstance(self.language_tooling_capped, bool):
            raise TypeError("language_tooling_capped must be a bool")

    @property
    def domain_id(self) -> ReferenceId:
        return self.assessment.domain_id


@dataclass(frozen=True, slots=True)
class MaturityAssessment:
    """All per-domain results plus the propagated per-gate effective scores.

    This is the boundary the target-achievement stage (Task 8.3) consumes: it
    exposes each domain's effective score and capping trace, and every gate's
    effective score, without itself deciding whether any target level is met.
    """

    results: tuple[DomainMaturityResult, ...]
    gate_effective_scores: Mapping[str, MaturityScore]

    def __post_init__(self) -> None:
        results = tuple(self.results)
        if not all(isinstance(item, DomainMaturityResult) for item in results):
            raise TypeError("results must contain DomainMaturityResult values")
        object.__setattr__(
            self,
            "results",
            tuple(sorted(results, key=lambda item: str(item.domain_id))),
        )
        object.__setattr__(
            self,
            "gate_effective_scores",
            {str(k): v for k, v in dict(self.gate_effective_scores).items()},
        )

    @property
    def assessments(self) -> tuple[CapabilityAssessment, ...]:
        return tuple(result.assessment for result in self.results)

    def result_for(self, domain_id: str) -> DomainMaturityResult | None:
        target = str(domain_id)
        for result in self.results:
            if str(result.domain_id) == target:
                return result
        return None

    def effective_score_of(self, domain_id: str) -> MaturityScore:
        result = self.result_for(domain_id)
        if result is None:
            raise KeyError(f"unknown domain id: {domain_id!r}")
        return result.effective_score

    def gate_effective_score_of(self, gate_id: str) -> MaturityScore:
        key = str(gate_id)
        if key not in self.gate_effective_scores:
            raise KeyError(f"unknown gate id: {gate_id!r}")
        return self.gate_effective_scores[key]


# --------------------------------------------------------------------------- #
# Raw-score computation (direct evidence only).                                #
# --------------------------------------------------------------------------- #


def _direct_implementation_records(
    records: Iterable[EvidenceRecord],
) -> tuple[EvidenceRecord, ...]:
    return tuple(
        record
        for record in records
        if record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
        and record.status in _IMPLEMENTED_STATUSES
    )


def _is_repeatable(records: Iterable[EvidenceRecord]) -> bool:
    for record in records:
        if record.evidence_kind in _REPEATABLE_KINDS:
            return True
        if record.status in _REPEATABLE_STATUSES:
            return True
    return False


def compute_raw_score(inp: DomainMaturityInput) -> MaturityScore:
    """Compute a domain's raw ordinal score from direct evidence only.

    No direct implementation evidence fixes the raw score at 0 (Requirement 3.6,
    10.6, 15.5). Above 0 the score climbs the rubric ladder; rung 3 and above
    require the three score-3 conditions, so any domain lacking cross-host
    candidate + migration/rollback + release-review evidence is capped at 2
    (Requirement 15.4).
    """

    direct = _direct_implementation_records(inp.direct_evidence)
    if not direct:
        return MaturityScore.ABSENT

    score = MaturityScore.NARROW_EXPERIMENT
    if _is_repeatable(direct):
        score = MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION

    # Rung 3+ is gated on the three score-3 conditions. Without them, a domain
    # cannot exceed a repeatable repository-local implementation (rung 2).
    if score >= MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION and inp.score_three_ready:
        score = MaturityScore.CROSS_HOST_CANDIDATE_CONTRACT
        if inp.supported_production:
            score = MaturityScore.SUPPORTED_PRODUCTION_CAPABILITY
            if inp.mature_ecosystem:
                score = MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM

    return score


# --------------------------------------------------------------------------- #
# Effective-score computation (topological cap over the validated graph).      #
# --------------------------------------------------------------------------- #


def _gate_effective_scores(graph: HardGateGraph) -> dict[str, MaturityScore]:
    """Propagate each gate's effective score in dependency order.

    ``gate_effective[g] = min(score_of(g), gate_effective[d] for blocking d)``.
    The graph is a validated DAG, so the dependency-ordered path visits every
    dependency before its dependents and the recurrence is well defined.
    """

    effective: dict[str, MaturityScore] = {}
    for gate_id in graph.dependency_ordered_path():
        raw = graph.score_of(gate_id)
        floor = int(raw)
        for dep in graph.blocking_dependencies_of(gate_id):
            dep_effective = effective[str(dep)]
            floor = min(floor, int(dep_effective))
        effective[gate_id] = MaturityScore(floor)
    return effective


def assess_domains(
    inputs: Iterable[DomainMaturityInput],
    graph: HardGateGraph,
) -> MaturityAssessment:
    """Assess raw and effective maturity for every domain against ``graph``.

    Fails closed with a ``MAT-*`` code on a duplicate domain, an unknown gate
    reference, or a missing next-gate reference. Every returned assessment is
    ordinal only; no aggregate, percentage, average, or schedule is produced.
    """

    if not isinstance(graph, HardGateGraph):
        raise TypeError("graph must be a HardGateGraph")

    materialized = tuple(inputs)
    if not all(isinstance(item, DomainMaturityInput) for item in materialized):
        raise TypeError("inputs must contain DomainMaturityInput values")

    seen: set[str] = set()
    duplicates: set[str] = set()
    for inp in materialized:
        if inp.domain_id in seen:
            duplicates.add(inp.domain_id)
        seen.add(inp.domain_id)
    if duplicates:
        raise MaturityAssessmentError(
            MAT_DUPLICATE_DOMAIN, "duplicate domain assessment inputs", duplicates
        )

    known_gates = set(graph.gate_ids)
    gate_effective = _gate_effective_scores(graph)

    results: list[DomainMaturityResult] = []
    for inp in materialized:
        if inp.gate_id not in known_gates:
            raise MaturityAssessmentError(
                MAT_UNKNOWN_GATE,
                "domain references a gate absent from the validated graph",
                (inp.domain_id, inp.gate_id),
            )
        next_gate = inp.next_hard_gate_id or inp.gate_id
        if next_gate not in known_gates:
            raise MaturityAssessmentError(
                MAT_MISSING_NEXT_GATE,
                "domain next hard gate is absent from the validated graph",
                (inp.domain_id, next_gate),
            )

        raw = compute_raw_score(inp)

        # Effective score: min(raw, every blocking dependency/gate score) using
        # the transitively propagated per-gate effective scores.
        blocking_deps = graph.blocking_dependencies_of(inp.gate_id)
        running = int(raw)
        cap_steps: list[CapStep] = []
        for dep in blocking_deps:
            dep_effective = gate_effective[str(dep)]
            binding = int(dep_effective) < running
            cap_steps.append(
                CapStep(
                    dependency_gate_id=dep,
                    dependency_effective_score=dep_effective,
                    cap_rationale=graph.cap_rationale(inp.gate_id, str(dep)),
                    binding=binding,
                )
            )
            running = min(running, int(dep_effective))
        effective = MaturityScore(running)

        language_capped = (
            inp.domain_class is DomainClass.LANGUAGE_TOOLING
            and not inp.score_three_ready
            and int(raw) == int(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)
        )

        limitations = _build_limitations(inp, raw, language_capped)
        rationale = _build_rationale(inp, raw, effective, cap_steps, language_capped)
        evidence_ids = tuple(
            reference(record.id) for record in inp.direct_evidence
        )
        blocking_dependency_ids = tuple(reference(dep) for dep in blocking_deps)

        assessment = CapabilityAssessment(
            domain_id=inp.domain_id,
            raw_score=raw,
            effective_score=effective,
            confidence=inp.confidence,
            evidence_status=inp.evidence_status,
            evidence_ids=evidence_ids,
            limitations=limitations,
            next_hard_gate_id=next_gate,
            blocking_dependency_ids=blocking_dependency_ids,
            rationale=rationale,
        )
        results.append(
            DomainMaturityResult(
                assessment=assessment,
                raw_score=raw,
                effective_score=effective,
                cap_trace=tuple(cap_steps),
                language_tooling_capped=language_capped,
            )
        )

    return MaturityAssessment(
        results=tuple(results),
        gate_effective_scores=gate_effective,
    )


def _build_limitations(
    inp: DomainMaturityInput, raw: MaturityScore, language_capped: bool
) -> tuple[str, ...]:
    limitations = list(inp.limitations)
    if raw == MaturityScore.ABSENT:
        limitations.append(
            "No direct implementation evidence: raw and effective maturity are "
            "fixed at 0 regardless of plans, prerequisites, examples, or adjacent "
            "capabilities."
        )
    elif not inp.score_three_ready and int(raw) == int(
        MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION
    ):
        limitations.append(
            "Capped at maturity 2: no cross-supported-host candidate contract, "
            "migration/rollback, and release-review evidence to satisfy the "
            "score-3 conditions."
        )
    if language_capped:
        limitations.append(
            "Language/tooling capability held at maturity 2 (Requirement 15.4) "
            "until cross-supported-host candidate evidence is demonstrated."
        )
    # De-duplicate while keeping order stable and deterministic.
    seen: set[str] = set()
    ordered: list[str] = []
    for item in limitations:
        if item not in seen:
            seen.add(item)
            ordered.append(item)
    return tuple(ordered)


def _build_rationale(
    inp: DomainMaturityInput,
    raw: MaturityScore,
    effective: MaturityScore,
    cap_steps: tuple[CapStep, ...] | list[CapStep],
    language_capped: bool,
) -> str:
    parts: list[str] = []
    if inp.rationale_note.strip():
        parts.append(inp.rationale_note.strip())
    parts.append(
        f"Raw ordinal maturity {int(raw)} derived from direct evidence only; "
        f"effective ordinal maturity {int(effective)} after topological capping "
        "by blocking Hard-Gate dependencies."
    )
    binding = [step for step in cap_steps if step.binding]
    if binding:
        tightest = min(binding, key=lambda step: int(step.dependency_effective_score))
        parts.append(
            f"Effective score is held down by blocking dependency "
            f"{tightest.dependency_gate_id} (effective "
            f"{int(tightest.dependency_effective_score)}): {tightest.cap_rationale}"
        )
    elif cap_steps:
        parts.append(
            "No blocking dependency reduced the raw score; every prerequisite "
            "gate is at least as mature as this domain."
        )
    else:
        parts.append("This domain has no blocking Hard-Gate dependencies.")
    if language_capped:
        parts.append(
            "Language/tooling cap (Requirement 15.4) applies: no cross-host "
            "candidate contract, migration/rollback, and release-review evidence."
        )
    parts.append(
        "Scores are non-additive ordinal values and do not represent "
        "percentages, averages, or schedule estimates."
    )
    return " ".join(parts)
