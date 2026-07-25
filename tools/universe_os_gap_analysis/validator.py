"""Cross-reference, schema, coverage, and fail-closed publish validator (Task 10.1).

This module is the single publish gate for a canonical
:class:`~tools.universe_os_gap_analysis.models.AssessmentModel`. It *inspects* an
already-assembled model (the assembly/publish transaction is Task 10.2's job) and
emits a deterministic :class:`~tools.universe_os_gap_analysis.models.ValidationResult`.
It never mutates evidence, upgrades a status, or edits any evaluator/product code.

The validator fails closed: it produces an ``Error`` finding for every violation
and marks the result invalid whenever any error is present, so a corrupt or
incomplete model can never be published as a "valid report" (Requirement 9.7,
Property 16). Every finding carries the offending object identifiers and the
governing requirement references.

Findings use the design's fail-closed error-code families, each carrying object
IDs and requirement refs:

* ``REV-*`` -- the bound revision / worktree fingerprint is missing or corrupt.
* ``INV-*`` -- the source inventory is empty or misses a required category.
* ``EVD-*`` -- an ``Evidence_Record`` is missing, duplicated, references a source
  path/anchor that is not in the inventory, or a required record referenced by a
  domain/assessment/conflict/conclusion does not resolve (fail closed).
* ``CLM-*`` -- an evidence record leaves a detected exclusion/trust assumption
  unrecorded (delegated to the Task 4.4 trust auditor).
* ``CNF-*`` -- an evidence conflict is malformed (unknown record, a non-``Low``
  confidence, or an inferred winner).
* ``GRF-*`` -- the Hard-Gate dependency graph is not a validated DAG (unknown,
  duplicate, self, or cyclic edge, or an illegal branch/join), or a domain
  references an unknown gate.
* ``MAT-*`` -- a maturity score is out of range, an effective score exceeds its
  raw score, a domain with no direct evidence carries a non-zero score, or an
  assessment references an unknown gate.
* ``RPT-*`` -- a mandatory report section is missing, the six-level target model
  is malformed, the capability-matrix/domain parity is broken, a gap's primary
  category is malformed, a reference to a domain/gap does not resolve, or the
  initial evidence-backed conclusion contract is violated.

The public entry point is :func:`validate_assessment_model`, which returns a
``ValidationResult``; :class:`AssessmentValidator` exposes the same behaviour for
callers that want to reuse a configured instance. Neither ever raises on a model
problem -- every problem becomes a finding so the caller sees the *complete* set
of reasons a model is unpublishable in one pass.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable

from . import catalog
from .hard_gate_graph import (
    GRF_UNKNOWN_NODE,
    HardGateGraphError,
    build_hard_gate_graph,
)
from .identifiers import StableId, reference
from .inventory import REQUIRED_SOURCE_CATEGORIES
from .maturity import MAT_UNKNOWN_GATE
from .models import (
    AssessmentModel,
    CapabilityAssessment,
    CapabilityDomain,
    ConfidenceRating,
    FindingSeverity,
    GapCategory,
    LocationKind,
    MaturityScore,
    TargetLevel,
    ValidationFinding,
    ValidationResult,
)
from .trust_audit import TrustAssumptionAuditor

# --------------------------------------------------------------------------- #
# REV-* : bound revision and worktree fingerprint integrity.                   #
# --------------------------------------------------------------------------- #
REV_MISSING_REVISION = "REV-MISSING-REVISION"
REV_FINGERPRINT_MISSING = "REV-FINGERPRINT-MISSING"

# --------------------------------------------------------------------------- #
# INV-* : required source inventory coverage.                                  #
# --------------------------------------------------------------------------- #
INV_EMPTY = "INV-EMPTY"
INV_MISSING_CATEGORY = "INV-MISSING-CATEGORY"

# --------------------------------------------------------------------------- #
# EVD-* : evidence record completeness and reference resolution.               #
# --------------------------------------------------------------------------- #
EVD_EMPTY = "EVD-EMPTY"
EVD_DUPLICATE_ID = "EVD-DUPLICATE-ID"
EVD_UNKNOWN_REFERENCE = "EVD-UNKNOWN-REFERENCE"
EVD_UNKNOWN_PATH = "EVD-UNKNOWN-PATH"
EVD_MISSING_ANCHOR = "EVD-MISSING-ANCHOR"

# --------------------------------------------------------------------------- #
# CLM-* : unrecorded exclusion / trust assumption (Task 4.4 auditor).          #
# --------------------------------------------------------------------------- #
# (Codes come from the trust auditor's findings directly.)

# --------------------------------------------------------------------------- #
# CNF-* : evidence conflict integrity.                                         #
# --------------------------------------------------------------------------- #
CNF_UNKNOWN_EVIDENCE = "CNF-UNKNOWN-EVIDENCE"
CNF_INVALID_CONFIDENCE = "CNF-INVALID-CONFIDENCE"
CNF_INFERRED_WINNER = "CNF-INFERRED-WINNER"

# --------------------------------------------------------------------------- #
# GRF-* : Hard-Gate dependency graph (reused from the graph builder).          #
# --------------------------------------------------------------------------- #
# (GRF_UNKNOWN_NODE and the HardGateGraphError codes are reused directly.)

# --------------------------------------------------------------------------- #
# MAT-* : maturity score integrity.                                            #
# --------------------------------------------------------------------------- #
MAT_SCORE_RANGE = "MAT-SCORE-RANGE"
MAT_EFFECTIVE_EXCEEDS_RAW = "MAT-EFFECTIVE-EXCEEDS-RAW"
MAT_NO_EVIDENCE_NONZERO = "MAT-NO-EVIDENCE-NONZERO"

# --------------------------------------------------------------------------- #
# RPT-* : report completeness, parity, classification, and conclusions.        #
# --------------------------------------------------------------------------- #
RPT_MISSING_SECTION = "RPT-MISSING-SECTION"
RPT_TARGET_LEVELS = "RPT-TARGET-LEVELS"
RPT_DUPLICATE_ASSESSMENT = "RPT-DUPLICATE-ASSESSMENT"
RPT_DOMAIN_ASSESSMENT_PARITY = "RPT-DOMAIN-ASSESSMENT-PARITY"
RPT_UNKNOWN_DOMAIN = "RPT-UNKNOWN-DOMAIN"
RPT_UNKNOWN_GAP = "RPT-UNKNOWN-GAP"
RPT_GAP_PRIMARY = "RPT-GAP-PRIMARY"
RPT_GAP_SECONDARY = "RPT-GAP-SECONDARY"
RPT_INITIAL_CONCLUSION = "RPT-INITIAL-CONCLUSION"

_MIN_SCORE = int(MaturityScore.ABSENT)
_MAX_SCORE = int(MaturityScore.MATURE_INDEPENDENT_ECOSYSTEM)

# The OS-substrate target levels whose domains must stay at maturity 0 without
# direct implementation evidence (Requirements 3.6, 10.6, 15.5).
_SUBSTRATE_LEVELS: frozenset[TargetLevel] = frozenset(
    {
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        TargetLevel.T5_OPERABLE_UNIVERSE_OS,
    }
)


def _safe_object_refs(refs: Iterable[object]) -> tuple[str, ...]:
    """Return only the parts of ``refs`` that are valid stable identifiers.

    Graph errors sometimes phrase an edge as ``"dependent->dependency"``; those
    are split so both endpoints surface as valid object references. Anything that
    is not a valid identifier is dropped rather than crashing the validator.
    """

    collected: set[str] = set()
    for raw in refs:
        text = str(raw)
        for candidate in text.replace("->", ",").split(","):
            candidate = candidate.strip()
            if not candidate:
                continue
            try:
                collected.add(str(reference(candidate)))
            except (TypeError, ValueError):
                continue
    return tuple(sorted(collected))


@dataclass
class _FindingSink:
    """Accumulates fail-closed findings during a single validation pass."""

    findings: list[ValidationFinding] = field(default_factory=list)

    def error(
        self,
        code: str,
        requirement_refs: Iterable[str],
        object_refs: Iterable[object] = (),
    ) -> None:
        self.findings.append(
            ValidationFinding(
                severity=FindingSeverity.ERROR,
                code=code,
                requirement_refs=tuple(str(ref) for ref in requirement_refs),
                object_refs=_safe_object_refs(object_refs),
            )
        )

    def result(self) -> ValidationResult:
        has_error = any(
            finding.severity is FindingSeverity.ERROR for finding in self.findings
        )
        return ValidationResult(valid=not has_error, findings=tuple(self.findings))


class AssessmentValidator:
    """The publish gate that cross-checks an assembled ``AssessmentModel``.

    Every check appends a fail-closed ``Error`` finding rather than raising, so a
    single :meth:`validate` pass reports the *complete* set of reasons a model is
    unpublishable. The returned :class:`ValidationResult` is ``valid`` only when
    no error finding was produced.
    """

    def validate(self, model: AssessmentModel) -> ValidationResult:
        if not isinstance(model, AssessmentModel):
            raise TypeError("model must be an AssessmentModel")

        sink = _FindingSink()

        # Index the model's objects once for fast reference resolution.
        evidence_ids = {str(record.id) for record in model.evidence_records}
        domain_by_id = {str(domain.id): domain for domain in model.domains}
        gate_ids = {str(gate.id) for gate in model.hard_gates}
        gap_ids = {str(gap.id) for gap in model.gaps}
        inventory_paths = {str(entry.path) for entry in model.source_inventory}
        anchors_by_path: dict[str, set[str]] = {}
        for entry in model.source_inventory:
            anchors_by_path.setdefault(str(entry.path), set()).update(
                str(anchor) for anchor in entry.stable_anchors
            )

        self._check_revision(model, sink)
        self._check_inventory(model, sink)
        self._check_report_sections(model, sink)
        self._check_target_levels(model, sink)
        self._check_evidence_records(
            model, sink, evidence_ids, inventory_paths, anchors_by_path
        )
        self._check_conflicts(model, sink, evidence_ids)
        self._check_domains(model, sink, evidence_ids, gate_ids, gap_ids)
        self._check_assessments(model, sink, evidence_ids, domain_by_id, gate_ids)
        self._check_gaps(model, sink, domain_by_id)
        self._check_recommendations(model, sink, gap_ids)
        self._check_observed_conclusions(model, sink, evidence_ids)
        self._check_hard_gate_graph(model, sink)
        self._check_trust_assumptions(model, sink)
        self._check_initial_conclusion_contract(model, sink, domain_by_id)

        return sink.result()

    # -- REV ------------------------------------------------------------- #

    def _check_revision(self, model: AssessmentModel, sink: _FindingSink) -> None:
        revision = model.revision
        root_id = str(revision.repository_root_id)
        if not str(revision.commit_id).strip():
            sink.error(REV_MISSING_REVISION, ("1.1", "14.1"), (root_id,))
        if not str(revision.worktree_fingerprint).strip():
            sink.error(REV_FINGERPRINT_MISSING, ("1.1", "1.2", "9.7"), (root_id,))
        if revision.evidence_axes is None:
            sink.error(REV_MISSING_REVISION, ("1.2", "14.1"), (root_id,))

    # -- INV ------------------------------------------------------------- #

    def _check_inventory(self, model: AssessmentModel, sink: _FindingSink) -> None:
        if not model.source_inventory:
            sink.error(INV_EMPTY, ("1.3", "14.1", "14.4"))
            return
        present = {entry.category for entry in model.source_inventory}
        for category in sorted(REQUIRED_SOURCE_CATEGORIES, key=lambda item: item.value):
            if category not in present:
                sink.error(
                    INV_MISSING_CATEGORY,
                    ("1.3", "14.1"),
                    (StableId(f"category.{category.value}"),),
                )

    # -- RPT: mandatory report sections (Requirement 14.1) --------------- #

    def _check_report_sections(
        self, model: AssessmentModel, sink: _FindingSink
    ) -> None:
        # Requirement 14.1 enumerates the mandatory report sections. Conflicts,
        # assumptions, and gaps may legitimately be empty; the sections below
        # must be present for a publishable report.
        mandatory = (
            ("source_inventory", model.source_inventory, ("1.3", "14.1")),
            ("evidence_records", model.evidence_records, ("1.4", "14.1")),
            ("domains", model.domains, ("14.1", "14.2")),
            ("assessments", model.assessments, ("3.1", "14.1", "14.2")),
            ("hard_gates", model.hard_gates, ("3.3", "14.1")),
            ("observed_conclusions", model.observed_conclusions, ("14.1", "14.7")),
            ("non_claims", model.non_claims, ("13.6", "14.1")),
        )
        for name, collection, refs in mandatory:
            if not collection:
                sink.error(
                    RPT_MISSING_SECTION, refs, (StableId(f"section.{name}"),)
                )

    # -- RPT: six-level target model (Requirement 2.2, 14.1) ------------- #

    def _check_target_levels(
        self, model: AssessmentModel, sink: _FindingSink
    ) -> None:
        levels = set(model.target_levels)
        if levels != set(TargetLevel) or len(model.target_levels) != len(TargetLevel):
            sink.error(RPT_TARGET_LEVELS, ("2.2", "14.1"))

    # -- EVD ------------------------------------------------------------- #

    def _check_evidence_records(
        self,
        model: AssessmentModel,
        sink: _FindingSink,
        evidence_ids: set[str],
        inventory_paths: set[str],
        anchors_by_path: dict[str, set[str]],
    ) -> None:
        if not model.evidence_records:
            sink.error(EVD_EMPTY, ("1.4", "14.1"))

        seen: set[str] = set()
        for record in model.evidence_records:
            record_id = str(record.id)
            if record_id in seen:
                sink.error(EVD_DUPLICATE_ID, ("1.4", "9.7"), (record_id,))
            seen.add(record_id)

            source_path = str(record.source_path)
            if inventory_paths and source_path not in inventory_paths:
                # Every material conclusion must cite a repository path that the
                # inventory actually inspected (Requirement 14.4).
                sink.error(EVD_UNKNOWN_PATH, ("14.4",), (record_id,))
                continue

            # Anchor resolution (Requirement 14.4, 14.5): non-line-range
            # locations must name a stable anchor the inventory recorded.
            if record.location.kind is LocationKind.LINE_RANGE:
                continue
            anchors = anchors_by_path.get(source_path, set())
            if anchors and str(record.location.value) not in anchors:
                sink.error(EVD_MISSING_ANCHOR, ("14.4", "14.5"), (record_id,))

    # -- CNF ------------------------------------------------------------- #

    def _check_conflicts(
        self, model: AssessmentModel, sink: _FindingSink, evidence_ids: set[str]
    ) -> None:
        for conflict in model.conflicts:
            conflict_id = str(conflict.id)
            missing = [
                str(ref)
                for ref in conflict.evidence_ids
                if str(ref) not in evidence_ids
            ]
            if missing:
                sink.error(
                    CNF_UNKNOWN_EVIDENCE,
                    ("1.5", "13.4", "9.7"),
                    (conflict_id, *missing),
                )
            # A conflict must force Low confidence and never infer a winner
            # (Requirement 13.4). The model normally guarantees this; re-check
            # defensively so a corrupted object still fails closed.
            if conflict.confidence is not ConfidenceRating.LOW:
                sink.error(CNF_INVALID_CONFIDENCE, ("13.4",), (conflict_id,))
            if conflict.winner is not None:
                sink.error(CNF_INFERRED_WINNER, ("1.5", "13.4"), (conflict_id,))

    # -- Domains --------------------------------------------------------- #

    def _check_domains(
        self,
        model: AssessmentModel,
        sink: _FindingSink,
        evidence_ids: set[str],
        gate_ids: set[str],
        gap_ids: set[str],
    ) -> None:
        domain_ids = {str(domain.id) for domain in model.domains}
        for domain in model.domains:
            domain_id = str(domain.id)
            if domain.parent_id is not None and str(domain.parent_id) not in domain_ids:
                sink.error(RPT_UNKNOWN_DOMAIN, ("14.2",), (domain_id, domain.parent_id))
            for evidence_ref in domain.evidence_ids:
                if str(evidence_ref) not in evidence_ids:
                    sink.error(
                        EVD_UNKNOWN_REFERENCE, ("9.7",), (domain_id, evidence_ref)
                    )
            for gate_ref in domain.dependency_gate_ids:
                if str(gate_ref) not in gate_ids:
                    sink.error(GRF_UNKNOWN_NODE, ("3.5",), (domain_id, gate_ref))
            for gap_ref in domain.gap_ids:
                if str(gap_ref) not in gap_ids:
                    sink.error(RPT_UNKNOWN_GAP, ("14.3",), (domain_id, gap_ref))

    # -- Assessments (capability matrix parity) -------------------------- #

    def _check_assessments(
        self,
        model: AssessmentModel,
        sink: _FindingSink,
        evidence_ids: set[str],
        domain_by_id: dict[str, CapabilityDomain],
        gate_ids: set[str],
    ) -> None:
        # Requirement 14.2: exactly one capability row per domain. Detect both
        # duplicate assessments and domains without an assessment.
        assessed: set[str] = set()
        for assessment in model.assessments:
            domain_id = str(assessment.domain_id)
            if domain_id in assessed:
                sink.error(RPT_DUPLICATE_ASSESSMENT, ("14.2",), (domain_id,))
            assessed.add(domain_id)

            if domain_id not in domain_by_id:
                sink.error(RPT_UNKNOWN_DOMAIN, ("14.2",), (domain_id,))

            self._check_assessment_scores(assessment, sink)

            for evidence_ref in assessment.evidence_ids:
                if str(evidence_ref) not in evidence_ids:
                    sink.error(
                        EVD_UNKNOWN_REFERENCE, ("9.7",), (domain_id, evidence_ref)
                    )
            if str(assessment.next_hard_gate_id) not in gate_ids:
                sink.error(
                    MAT_UNKNOWN_GATE,
                    ("3.3", "3.5"),
                    (domain_id, assessment.next_hard_gate_id),
                )
            for gate_ref in assessment.blocking_dependency_ids:
                if str(gate_ref) not in gate_ids:
                    sink.error(
                        MAT_UNKNOWN_GATE, ("3.4", "3.5"), (domain_id, gate_ref)
                    )

        # Every domain needs a capability-matrix row.
        for domain_id in domain_by_id:
            if domain_id not in assessed:
                sink.error(
                    RPT_DOMAIN_ASSESSMENT_PARITY, ("3.1", "14.2"), (domain_id,)
                )

    def _check_assessment_scores(
        self, assessment: CapabilityAssessment, sink: _FindingSink
    ) -> None:
        domain_id = str(assessment.domain_id)
        raw = int(assessment.raw_score)
        effective = int(assessment.effective_score)
        if not (_MIN_SCORE <= raw <= _MAX_SCORE):
            sink.error(MAT_SCORE_RANGE, ("3.2", "3.5"), (domain_id,))
        if not (_MIN_SCORE <= effective <= _MAX_SCORE):
            sink.error(MAT_SCORE_RANGE, ("3.2", "3.5"), (domain_id,))
        if effective > raw:
            sink.error(MAT_EFFECTIVE_EXCEEDS_RAW, ("3.4",), (domain_id,))
        # No direct evidence must fix the score at 0 (Requirement 3.6, 10.6, 15.5).
        if not assessment.evidence_ids and (raw > 0 or effective > 0):
            sink.error(
                MAT_NO_EVIDENCE_NONZERO, ("3.6", "10.6", "15.5"), (domain_id,)
            )

    # -- Gaps (one primary category, resolvable domains) ----------------- #

    def _check_gaps(
        self,
        model: AssessmentModel,
        sink: _FindingSink,
        domain_by_id: dict[str, CapabilityDomain],
    ) -> None:
        for gap in model.gaps:
            gap_id = str(gap.id)
            if not isinstance(gap.primary_category, GapCategory):
                sink.error(RPT_GAP_PRIMARY, ("12.1", "14.3"), (gap_id,))
            secondary = tuple(gap.secondary_categories)
            if gap.primary_category in secondary:
                sink.error(RPT_GAP_SECONDARY, ("12.2", "14.3"), (gap_id,))
            if len(secondary) != len(set(secondary)):
                sink.error(RPT_GAP_SECONDARY, ("12.2", "14.3"), (gap_id,))
            for domain_ref in gap.domain_ids:
                if str(domain_ref) not in domain_by_id:
                    sink.error(RPT_UNKNOWN_DOMAIN, ("14.3",), (gap_id, domain_ref))

    # -- Recommendations ------------------------------------------------- #

    def _check_recommendations(
        self, model: AssessmentModel, sink: _FindingSink, gap_ids: set[str]
    ) -> None:
        for recommendation in model.recommendations:
            rec_id = str(recommendation.id)
            for gap_ref in recommendation.related_gap_ids:
                if str(gap_ref) not in gap_ids:
                    sink.error(RPT_UNKNOWN_GAP, ("14.3", "14.7"), (rec_id, gap_ref))

    # -- Observed conclusions -------------------------------------------- #

    def _check_observed_conclusions(
        self, model: AssessmentModel, sink: _FindingSink, evidence_ids: set[str]
    ) -> None:
        for conclusion in model.observed_conclusions:
            conclusion_id = str(conclusion.id)
            for evidence_ref in conclusion.evidence_ids:
                if str(evidence_ref) not in evidence_ids:
                    sink.error(
                        EVD_UNKNOWN_REFERENCE,
                        ("9.7", "14.7"),
                        (conclusion_id, evidence_ref),
                    )

    # -- GRF: Hard-Gate dependency DAG ----------------------------------- #

    def _check_hard_gate_graph(
        self, model: AssessmentModel, sink: _FindingSink
    ) -> None:
        if not model.hard_gates:
            # Missing section already reported by _check_report_sections.
            return
        try:
            build_hard_gate_graph(model.hard_gates)
        except HardGateGraphError as error:
            sink.error(error.code, ("3.5", "12.7"), error.object_refs)
        except (TypeError, ValueError) as error:  # pragma: no cover - defensive
            sink.error(
                "GRF-INVALID-GRAPH",
                ("3.5", "12.7"),
                _safe_object_refs((str(gate.id) for gate in model.hard_gates)),
            )
            _ = error

    # -- CLM: unrecorded exclusion / trust assumption -------------------- #

    def _check_trust_assumptions(
        self, model: AssessmentModel, sink: _FindingSink
    ) -> None:
        report = TrustAssumptionAuditor().audit(model.evidence_records)
        for finding in report.validation_findings():
            sink.findings.append(finding)

    # -- RPT: initial evidence-backed conclusion contract ---------------- #

    def _check_initial_conclusion_contract(
        self,
        model: AssessmentModel,
        sink: _FindingSink,
        domain_by_id: dict[str, CapabilityDomain],
    ) -> None:
        # The immutable initial-conclusion catalog must stay internally
        # consistent (Requirements 15.1-15.7).
        try:
            catalog.validate_catalog()
        except ValueError:
            sink.error(RPT_INITIAL_CONCLUSION, ("15.1", "15.7"))

        # Substrate-zero rule (Requirements 3.6, 10.6, 15.5): an OS-substrate
        # domain (T2-T5) with no direct implementation evidence must stay at
        # maturity 0. This binds the model to the initial conclusion contract.
        for assessment in model.assessments:
            domain = domain_by_id.get(str(assessment.domain_id))
            if domain is None:
                continue
            if domain.target_level not in _SUBSTRATE_LEVELS:
                continue
            if assessment.evidence_ids:
                continue
            if int(assessment.effective_score) > 0 or int(assessment.raw_score) > 0:
                sink.error(
                    RPT_INITIAL_CONCLUSION,
                    ("3.6", "10.6", "15.5"),
                    (str(assessment.domain_id),),
                )


def validate_assessment_model(model: AssessmentModel) -> ValidationResult:
    """Validate an assembled ``AssessmentModel`` and return a ``ValidationResult``.

    This is the single publish gate (Task 10.1): it inspects the model, emits a
    fail-closed finding for every violation, and returns a result that is
    ``valid`` only when no error was found (Requirement 9.7, Property 16). It
    never mutates the model and never raises on a model problem.
    """

    return AssessmentValidator().validate(model)
