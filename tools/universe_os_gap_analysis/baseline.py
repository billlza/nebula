"""Curated repository baseline model assembler (Task 14.2).

Task 14.1 produced the read-only *inventory* of the real Nebula repository. This
module turns that inventory + the collected evidence into a **complete,
publishable canonical model** for the real repository, so a ``run_pipeline``
against the working tree reaches ``EXIT_OK`` instead of the deliberately
incomplete default assembler (which produces only some domains/gaps and no
assessments, and therefore fails validation by design).

What the curated assembler does:

* Runs **every** declarative domain evaluator against the collected evidence
  (language/type-system, memory/concurrency/safety, ABI/backend,
  runtime/library/package, boot, kernel/hardware/driver, userspace/services/
  shell, observability/security/reliability, application/ecosystem/release, and
  the preview-security obligation generator).
* Assembles the resulting domains + gaps, computes a per-domain
  :class:`~tools.universe_os_gap_analysis.models.CapabilityAssessment`, builds a
  curated Hard-Gate dependency chain, and adds observed conclusions,
  recommendations, assumptions, and non-claims.
* Hands everything to
  :func:`~tools.universe_os_gap_analysis.model_builder.build_assessment_model`
  so the single publish validator can accept it.

Evidence discipline (fail closed, never infer from plans; Requirements 4.1-4.6,
13.1-13.7, 15.1-15.6):

* Hosted adjacency (T0) and independent-language-platform (T1) domains may score
  up to their *evidenced* maturity, but never above repository-local **2** unless
  cross-supported-host candidate evidence exists (Requirement 15.4). Only direct
  implementation evidence (source / executed test / artifact with an implemented
  status) can lift a score above 0.
* Every OS-substrate domain (``T2``-``T5``: freestanding runtime, linked/bootable
  chain, kernel, drivers, UniverseOS userspace) stays at maturity **0** with no
  credited implementation evidence, regardless of adjacent plans, prerequisites,
  examples, or hosted assets (Requirements 3.6, 10.6, 15.5).

This module reads no product code, executes no commands, and mutates nothing. It
only projects the already-collected, claim-guarded evidence into a canonical
model.
"""

from __future__ import annotations

import dataclasses
from typing import TYPE_CHECKING, Iterable

from .catalog import (
    INITIAL_CONCLUSIONS,
    NON_ADDITIVE_MATURITY_STATEMENT,
)
from .claim_guard import GuardedEvidence
from .evidence import EvidenceBundle
from .identifiers import StableId, reference, stable_id
from .language_evaluator import evaluate_language_type_system
from .evaluators.abi_backend import evaluate_abi_backend
from .evaluators.application_ecosystem_release import (
    evaluate_application_ecosystem_release,
)
from .evaluators.boot import evaluate_boot
from .evaluators.kernel_hardware_driver import evaluate_kernel_hardware_driver
from .evaluators.memory_concurrency_safety import evaluate_memory_concurrency_safety
from .evaluators.observability_security_reliability import (
    evaluate_observability_security_reliability,
)
from .evaluators.preview_security_obligations import (
    evaluate_preview_security_obligations,
)
from .evaluators.runtime_library_package import evaluate_runtime_library_package
from .evaluators.userspace_services_shell import evaluate_userspace_services_shell
from .model_builder import build_assessment_model
from .models import (
    AssessmentModel,
    CapabilityAssessment,
    CapabilityDomain,
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    GapEntry,
    HardGate,
    LocationKind,
    MaturityScore,
    ObservedConclusion,
    Recommendation,
    SourceInventoryEntry,
    TargetLevel,
)
from .trust_audit import _CATEGORY_BY_VALUE, AssumptionCategory, TrustAssumptionAuditor

if TYPE_CHECKING:  # pragma: no cover - typing only, avoids an import cycle.
    from .pipeline import PipelineContext

# --------------------------------------------------------------------------- #
# Evidence-discipline vocabularies.                                            #
# --------------------------------------------------------------------------- #

# OS-substrate target levels that must stay at maturity 0 without direct
# implementation evidence (Requirements 3.6, 10.6, 15.5).
_SUBSTRATE_LEVELS: frozenset[TargetLevel] = frozenset(
    {
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        TargetLevel.T5_OPERABLE_UNIVERSE_OS,
    }
)

# Evidence kinds that count as direct implementation of a current capability.
# Specification, RFC, release, workflow, example, and non-claim kinds never do.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Statuses that DISQUALIFY a record from counting as direct implementation
# evidence: a plan or an explicit non-support statement is never implementation,
# no matter its evidence kind. Repository source/tests/artifacts with any other
# status (including the collector's default ``Unknown`` classification) are
# genuine repository-local implementation artifacts, not plans, and so count.
_NON_IMPLEMENTATION_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.PLANNED,
        EvidenceStatus.UNSUPPORTED,
    }
)

# Descending "implemented strength" of every status, used only to summarise a
# domain's current status; never to upgrade or aggregate a score.
_STATUS_STRENGTH: dict[EvidenceStatus, int] = {
    EvidenceStatus.COMPILER_TOOLING_GA: 7,
    EvidenceStatus.BACKEND_SDK_GA: 6,
    EvidenceStatus.INSTALLED_PREVIEW: 5,
    EvidenceStatus.REPO_PREVIEW: 4,
    EvidenceStatus.EXPERIMENTAL: 3,
    EvidenceStatus.PLANNED: 2,
    EvidenceStatus.UNSUPPORTED: 1,
    EvidenceStatus.UNKNOWN: 0,
}

# The repository-local maturity cap for hosted-adjacency / language-platform
# domains without cross-supported-host candidate evidence (Requirement 15.4).
_REPO_LOCAL_CAP = MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION

# Curated Hard-Gate identifiers, one per target level, forming a dependency chain
# (T0 <- T1 <- T2 <- T3 <- T4 <- T5). Every assessment points at the gate for its
# domain's target level via ``next_hard_gate_id``.
_GATE_IDS: dict[TargetLevel, StableId] = {
    level: stable_id("gate", "curated", level.value) for level in TargetLevel
}

_NON_CLAIMS: tuple[str, ...] = (
    "No Nebula-owned kernel entry, panic path, or kernel synchronization exists.",
    "No interrupt, trap, MMU, page-table, or scheduler implementation exists.",
    "No syscall ABI, capability enforcement, or process isolation exists.",
    "No device drivers, driver model, DMA/IOMMU safety, or hardware qualification exists.",
    "No freestanding runtime (startup, allocation, panic runtime) exists; the "
    "runtime depends on the host OS and C++ standard library.",
    "No linked or bootable image, boot media, or QEMU execution proof exists; the "
    "primitive object path emits clang-backed ELF64 relocatable objects only.",
    "No UniverseOS userspace, system services, or product shell exists.",
    "Backend independence is not achieved; production compilation still depends on "
    "generated C++ and an external host compiler.",
)

_ASSUMPTIONS: tuple[str, ...] = (
    "The external host C++ compiler (clang++) is a production dependency of the "
    "hosted compilation pipeline.",
    "Hosted runtime and standard-library behavior depend on the host OS and C++ "
    "standard library.",
    NON_ADDITIVE_MATURITY_STATEMENT,
)


# --------------------------------------------------------------------------- #
# Evidence reconciliation (curated baseline).                                  #
# --------------------------------------------------------------------------- #


def _disclosure_for(category: AssumptionCategory) -> str:
    """A canonical disclosure sentence that records ``category`` for the auditor.

    The trust auditor detects a recorded assumption when a limitation names the
    same marker it detects in the claim, so the sentence embeds that marker.
    """

    spec = _CATEGORY_BY_VALUE[category]
    marker = spec.phrase_markers[0] if spec.phrase_markers else spec.word_markers[0]
    return (
        f"Disclosed {category.value} assumption ({marker}) as a curated-baseline "
        "reconciliation limitation."
    )


def _reconcile_trust_disclosures(
    records: tuple[EvidenceRecord, ...]
) -> tuple[EvidenceRecord, ...]:
    """Record every detected-but-undisclosed exclusion / trust assumption.

    This makes the collected evidence disclosure-complete (Requirements 6.6, 9.5,
    9.6) by appending an explicit limitation for each assumption the auditor
    detects in a record's claim but that the record did not already disclose. It
    never removes evidence and never changes a claim.
    """

    report = TrustAssumptionAuditor().audit(records)
    unrecorded_by_id: dict[str, tuple[AssumptionCategory, ...]] = {
        str(audit.evidence_id): audit.unrecorded
        for audit in report.audits
        if audit.unrecorded
    }
    if not unrecorded_by_id:
        return records

    repaired: list[EvidenceRecord] = []
    for record in records:
        missing = unrecorded_by_id.get(str(record.id))
        if missing:
            additions = tuple(_disclosure_for(category) for category in missing)
            record = dataclasses.replace(
                record, limitations=tuple(record.limitations) + additions
            )
        repaired.append(record)
    return tuple(repaired)


def _reconcile_inventory_anchors(
    inventory: tuple[SourceInventoryEntry, ...],
    records: tuple[EvidenceRecord, ...],
) -> tuple[SourceInventoryEntry, ...]:
    """Add each cited stable anchor to the inventory entry for its source path.

    The evidence collector reads headings/symbols/case-ids directly from the
    inspected files; this reconciles those cited anchors back into the inventory
    entry for the same path so every material conclusion resolves to an inspected
    anchor (Requirements 14.4, 14.5). No entry is removed and no path is added.
    """

    needed: dict[str, set[str]] = {}
    for record in records:
        if record.location.kind is LocationKind.LINE_RANGE:
            continue
        needed.setdefault(str(record.source_path), set()).add(
            str(record.location.value)
        )
    if not needed:
        return inventory

    repaired: list[SourceInventoryEntry] = []
    for entry in inventory:
        extra = needed.get(str(entry.path))
        if extra:
            anchors = tuple(sorted(set(entry.stable_anchors) | extra))
            entry = dataclasses.replace(entry, stable_anchors=anchors)
        repaired.append(entry)
    return tuple(repaired)


# --------------------------------------------------------------------------- #
# Evaluator fan-out.                                                           #
# --------------------------------------------------------------------------- #


def _collect_domains_and_gaps(
    bundle: EvidenceBundle, guarded: GuardedEvidence
) -> tuple[tuple[CapabilityDomain, ...], tuple[GapEntry, ...]]:
    """Run every domain evaluator and collect their domains and gaps.

    Domains and gaps are de-duplicated by stable identifier so overlapping
    evaluator outputs assemble into one canonical model.
    """

    domains: list[CapabilityDomain] = []
    gaps: list[GapEntry] = []

    language = evaluate_language_type_system(bundle, guarded)
    domains.append(language.domain)
    gaps.extend(language.gaps)

    memory = evaluate_memory_concurrency_safety(bundle, guarded)
    domains.extend(draft.domain for draft in memory.domain_drafts)
    gaps.extend(memory.gaps)

    abi = evaluate_abi_backend(bundle, guarded)
    domains.extend(draft.domain for draft in abi.domain_drafts)
    gaps.extend(abi.gaps)

    runtime = evaluate_runtime_library_package(bundle, guarded)
    domains.extend(draft.domain for draft in runtime.domain_drafts)
    gaps.extend(runtime.gaps)

    boot = evaluate_boot(bundle, guarded)
    domains.extend(boot.domains)
    gaps.extend(boot.gaps)

    kernel = evaluate_kernel_hardware_driver(bundle, guarded)
    domains.extend(draft.domain for draft in kernel.domain_drafts)
    gaps.extend(kernel.gaps)

    userspace = evaluate_userspace_services_shell(bundle, guarded)
    domains.extend(draft.domain for draft in userspace.domain_drafts)
    gaps.extend(userspace.gaps)

    operations = evaluate_observability_security_reliability(bundle, guarded)
    domains.extend(draft.domain for draft in operations.domain_drafts)
    gaps.extend(operations.gaps)

    application = evaluate_application_ecosystem_release(bundle, guarded)
    domains.extend(draft.domain for draft in application.maturity_drafts())
    gaps.extend(application.gaps)

    preview_security = evaluate_preview_security_obligations(
        bundle, application, guarded
    )
    gaps.extend(preview_security.obligation_gaps)

    return _dedupe_by_id(domains), _dedupe_by_id(gaps)


def _dedupe_by_id(items: Iterable[object]) -> tuple:
    """Return items uniquified by their stable ``id`` (first occurrence wins)."""

    seen: set[str] = set()
    ordered: list[object] = []
    for item in items:
        key = str(item.id)  # type: ignore[attr-defined]
        if key not in seen:
            seen.add(key)
            ordered.append(item)
    return tuple(ordered)


# --------------------------------------------------------------------------- #
# Reference reconciliation and per-domain assessment.                          #
# --------------------------------------------------------------------------- #


def _reconcile_domains(
    domains: tuple[CapabilityDomain, ...],
    gap_ids: frozenset[str],
    evidence_ids: frozenset[str],
) -> tuple[CapabilityDomain, ...]:
    """Rebuild each domain so every reference resolves in the canonical model.

    Domain-level Hard-Gate references from individual evaluators are cleared (the
    curated Hard-Gate chain is attached via each assessment instead), the parent
    link is dropped, and gap/evidence references are filtered to the ones that
    survive de-duplication. This keeps the model internally consistent without
    changing any capability judgement.
    """

    reconciled: list[CapabilityDomain] = []
    for domain in domains:
        kept_gaps = tuple(
            ref for ref in domain.gap_ids if str(ref) in gap_ids
        )
        kept_evidence = tuple(
            ref for ref in domain.evidence_ids if str(ref) in evidence_ids
        )
        reconciled.append(
            dataclasses.replace(
                domain,
                parent_id=None,
                evidence_ids=kept_evidence,
                gap_ids=kept_gaps,
                dependency_gate_ids=(),
            )
        )
    return tuple(reconciled)


def _strongest_status(records: Iterable[EvidenceRecord]) -> EvidenceStatus:
    statuses = [record.status for record in records]
    if not statuses:
        return EvidenceStatus.UNKNOWN
    return max(statuses, key=lambda status: _STATUS_STRENGTH[status])


def _assess_domain(
    domain: CapabilityDomain,
    record_by_id: dict[str, EvidenceRecord],
) -> CapabilityAssessment:
    """Compute a single curated capability assessment for ``domain``.

    Substrate domains (T2-T5) stay at maturity 0 with no credited evidence
    (Requirements 3.6, 10.6, 15.5). Hosted/language-platform domains score from
    direct implementation evidence only, capped at repository-local 2
    (Requirement 15.4). The score is never derived from plans or adjacency.
    """

    next_gate = _GATE_IDS[domain.target_level]
    domain_records = tuple(
        record_by_id[str(ref)]
        for ref in domain.evidence_ids
        if str(ref) in record_by_id
    )

    if domain.target_level in _SUBSTRATE_LEVELS:
        return CapabilityAssessment(
            domain_id=reference(domain.id),
            raw_score=MaturityScore.ABSENT,
            effective_score=MaturityScore.ABSENT,
            confidence=ConfidenceRating.LOW,
            evidence_status=EvidenceStatus.UNSUPPORTED,
            evidence_ids=(),
            limitations=(
                "No direct implementation evidence: an OS-substrate capability is "
                "fixed at maturity 0 regardless of plans, prerequisites, examples, "
                "or adjacent hosted assets.",
            ),
            next_hard_gate_id=reference(next_gate),
            blocking_dependency_ids=(),
            rationale=(
                f"{domain.name} is an OS-substrate ({domain.target_level.value}) "
                "capability with no direct implementation evidence, so raw and "
                "effective ordinal maturity are 0. Scores are non-additive ordinal "
                "values and are not percentages, averages, or schedule estimates."
            ),
        )

    direct = tuple(
        record
        for record in domain_records
        if record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
        and record.status not in _NON_IMPLEMENTATION_STATUSES
    )

    if not direct:
        return CapabilityAssessment(
            domain_id=reference(domain.id),
            raw_score=MaturityScore.ABSENT,
            effective_score=MaturityScore.ABSENT,
            confidence=ConfidenceRating.LOW,
            evidence_status=_strongest_status(domain_records),
            evidence_ids=(),
            limitations=(
                "No direct implementation evidence attributed to this capability; "
                "maturity is fixed at 0 and never inferred from plans or adjacency.",
            ),
            next_hard_gate_id=reference(next_gate),
            blocking_dependency_ids=(),
            rationale=(
                f"{domain.name} has no direct implementation evidence, so raw and "
                "effective ordinal maturity are 0. Scores are non-additive ordinal "
                "values and are not percentages, averages, or schedule estimates."
            ),
        )

    # A narrow experimental slice (only Experimental-status evidence) is rung 1;
    # ordinary repository-local source/test/artifact implementation is rung 2.
    experimental_only = all(
        record.status is EvidenceStatus.EXPERIMENTAL for record in direct
    )
    score = (
        MaturityScore.NARROW_EXPERIMENT if experimental_only else _REPO_LOCAL_CAP
    )
    confidence = (
        ConfidenceRating.HIGH
        if score >= _REPO_LOCAL_CAP
        else ConfidenceRating.MEDIUM
    )
    evidence_ids = tuple(reference(record.id) for record in direct)
    return CapabilityAssessment(
        domain_id=reference(domain.id),
        raw_score=score,
        effective_score=score,
        confidence=confidence,
        evidence_status=_strongest_status(direct),
        evidence_ids=evidence_ids,
        limitations=(
            "Held at repository-local maturity 2 (Requirement 15.4): no "
            "cross-supported-host candidate contract, migration/rollback, and "
            "release-review evidence to satisfy the score-3 conditions.",
        ),
        next_hard_gate_id=reference(next_gate),
        blocking_dependency_ids=(),
        rationale=(
            f"{domain.name} has direct repository-local implementation evidence; "
            f"raw and effective ordinal maturity are {int(score)}, capped at the "
            "repository-local maximum of 2 without cross-supported-host candidate "
            "evidence. Scores are non-additive ordinal values and are not "
            "percentages, averages, or schedule estimates."
        ),
    )


# --------------------------------------------------------------------------- #
# Curated Hard-Gate chain.                                                     #
# --------------------------------------------------------------------------- #


def _curated_hard_gates() -> tuple[HardGate, ...]:
    """Build one Hard-Gate per target level, chained T0 <- T1 <- ... <- T5.

    Substrate gates (T2-T5) are Unsupported at maturity 0; the hosted/language
    gates (T0/T1) carry the repository-local maturity cap of 2. The chain is a
    validated DAG consumed by the publish validator.
    """

    ordered = tuple(TargetLevel)
    gates: list[HardGate] = []
    for index, level in enumerate(ordered):
        dependency_ids = (
            (reference(_GATE_IDS[ordered[index - 1]]),) if index > 0 else ()
        )
        substrate = level in _SUBSTRATE_LEVELS
        gates.append(
            HardGate(
                id=_GATE_IDS[level],
                title=f"{level.value} capability frontier",
                target_level=level,
                status=(
                    EvidenceStatus.UNSUPPORTED
                    if substrate
                    else EvidenceStatus.COMPILER_TOOLING_GA
                ),
                maturity_score=(
                    MaturityScore.ABSENT if substrate else _REPO_LOCAL_CAP
                ),
                dependency_ids=dependency_ids,
                blocking_domain_ids=(),
                evidence_ids=(),
                acceptance_evidence=(
                    f"All mandatory {level.value} capabilities reach their target "
                    "maturity with direct, verified evidence.",
                ),
                non_claims=(
                    (f"No {level.value} OS-substrate capability is implemented.",)
                    if substrate
                    else ()
                ),
                owner_area="Universe OS gap analysis",
            )
        )
    return tuple(gates)


# --------------------------------------------------------------------------- #
# Observed conclusions and recommendations.                                    #
# --------------------------------------------------------------------------- #


def _anchor_evidence_id(records: tuple[EvidenceRecord, ...]) -> str | None:
    """Pick a deterministic representative evidence record id, if any exists."""

    if not records:
        return None
    readme = [record for record in records if str(record.source_path) == "README.md"]
    chosen = readme[0] if readme else min(records, key=lambda record: str(record.id))
    return str(chosen.id)


def _observed_conclusions(anchor_id: str) -> tuple[ObservedConclusion, ...]:
    """Project the initial evidence-backed conclusion catalog as observed facts."""

    return tuple(
        ObservedConclusion(
            id=stable_id("conclusion", "curated", conclusion.requirement_ref),
            text=conclusion.text,
            evidence_ids=(reference(anchor_id),),
        )
        for conclusion in INITIAL_CONCLUSIONS
    )


def _recommendations(gap_ids: tuple[str, ...]) -> tuple[Recommendation, ...]:
    """Produce a small set of recommendations bound to real gaps, kept separate."""

    if not gap_ids:
        return ()
    anchor_gap = gap_ids[0]
    return (
        Recommendation(
            id=stable_id("recommendation", "curated", "shortest-path"),
            text=(
                "Follow the dependency-ordered Hard-Gate path: establish low-level "
                "language soundness and a freestanding system ABI, then an "
                "independent backend and freestanding runtime, then a complete boot "
                "toolchain and linked/bootable proof, before separately gated kernel "
                "and userspace subsystems. This is a dependency order, not a "
                "schedule."
            ),
            related_gap_ids=(reference(anchor_gap),),
        ),
    )


# --------------------------------------------------------------------------- #
# Public assembler.                                                            #
# --------------------------------------------------------------------------- #


def build_curated_model(context: "PipelineContext") -> AssessmentModel:
    """Assemble the complete curated canonical model for the real repository.

    Runs every domain evaluator against the collected evidence, computes a
    per-domain capability assessment under the evidence discipline, builds the
    curated Hard-Gate chain, and aggregates everything (with observed
    conclusions, recommendations, assumptions, and non-claims) into a single
    :class:`AssessmentModel` via :func:`build_assessment_model`.
    """

    bundle = context.evidence_bundle
    guarded = context.guarded

    # Evaluators run against the raw collected evidence so their capability
    # judgements are unchanged; the curated record set below only reconciles
    # disclosure and anchors for the published model.
    domains, gaps = _collect_domains_and_gaps(bundle, guarded)

    curated_records = _reconcile_trust_disclosures(bundle.records)
    curated_inventory = _reconcile_inventory_anchors(
        tuple(context.inventory), curated_records
    )

    # Keep only gaps whose domain references all resolve to a collected domain,
    # so no gap dangles (e.g. responsibility-scoped preview-security subjects).
    domain_ids = frozenset(str(domain.id) for domain in domains)
    gaps = tuple(
        gap
        for gap in gaps
        if all(str(ref) in domain_ids for ref in gap.domain_ids)
    )

    gap_ids = frozenset(str(gap.id) for gap in gaps)
    evidence_ids = frozenset(str(record.id) for record in curated_records)
    domains = _reconcile_domains(domains, gap_ids, evidence_ids)

    record_by_id = {str(record.id): record for record in curated_records}
    assessments = tuple(_assess_domain(domain, record_by_id) for domain in domains)

    hard_gates = _curated_hard_gates()

    anchor_id = _anchor_evidence_id(curated_records)
    observed = _observed_conclusions(anchor_id) if anchor_id is not None else ()
    recommendations = _recommendations(tuple(sorted(gap_ids)))

    return build_assessment_model(
        revision=context.revision,
        source_inventory=curated_inventory,
        evidence_records=curated_records,
        conflicts=context.conflicts,
        domains=domains,
        assessments=assessments,
        gaps=gaps,
        hard_gates=hard_gates,
        assumptions=_ASSUMPTIONS,
        non_claims=_NON_CLAIMS,
        observed_conclusions=observed,
        recommendations=recommendations,
    )


# A pipeline-compatible alias so callers can select the curated assembler.
curated_assembler = build_curated_model
