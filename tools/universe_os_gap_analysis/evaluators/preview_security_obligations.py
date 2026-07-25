"""Preview-security ecosystem obligation generator (Task 7.5).

This declarative generator covers Requirement 9.8: *when security-sensitive
packages remain preview, the Gap_Analysis creates Ecosystem_Gap entries for
maintenance, certification, deployment, and vulnerability-response maturity.*
Design Property 17 states the invariant precisely: for every security-sensitive
package/capability whose status is preview, the gap register must contain
ecosystem gaps covering all four obligations *unless direct evidence
independently closes each obligation*.

Layering (Task 7.5 builds strictly ON TOP of Task 7.4):

This module consumes the Task 7.4 application/ecosystem/release evaluator output
(:func:`evaluate_application_ecosystem_release`) and its reusable accessors --
:meth:`~...application_ecosystem_release.ApplicationEcosystemReleaseEvaluation.security_sensitive_drafts`,
:meth:`~...application_ecosystem_release.ApplicationEcosystemReleaseEvaluation.security_sensitive_responsibilities`,
and the ``observed_status`` each draft already carries. It does **not** re-derive
ownership, ecosystem status, or security-sensitivity; those are inputs from the
sibling evaluator. It only turns *preview + security-sensitive* subjects into the
four obligation gaps, subtracting any obligation that direct evidence closes.

Subject selection:

A *subject* is any security-sensitive ecosystem/release capability draft or
application responsibility draft whose strongest observed :class:`EvidenceStatus`
is a preview tier (``Installed_Preview`` or ``Repo_Preview``). Non-preview
security-sensitive subjects are handled by the Task 7.4 evaluator's ordinary gap
logic and are intentionally out of scope here.

Obligation closing rule ("independently and directly closed by evidence"):

Each of the four obligations is evaluated independently. An obligation is
*closed* only when the evidence bundle contains a record that

1. matches that obligation's markers,
2. carries a GA-tier maturity status (``Compiler_Tooling_GA`` or
   ``Backend_SDK_GA``) -- preview/experimental evidence is exactly what triggers
   the obligation and can never close it, and
3. is permitted in present tense by the Claim Guard (direct current-revision
   implementation/executable evidence).

Closing one obligation never closes another; a subject with, say, a mature
vulnerability-response process but no certification evidence still receives a
certification gap.

This module never mutates evidence, assigns maturity scores (Task 8), ranks gaps
(Task 9), renders anything (Task 11), or edits any sibling evaluator module.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping

from ..claim_guard import GuardedEvidence, guard_evidence
from ..evidence import EvidenceBundle
from ..identifiers import ReferenceId, reference, stable_id
from ..models import (
    ClosedStrEnum,
    EvidenceRecord,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    Severity,
    TargetLevel,
)
from .application_ecosystem_release import (
    ApplicationEcosystemReleaseEvaluation,
    ApplicationResponsibilityDraft,
    MaturityDomainDraft,
    evaluate_application_ecosystem_release,
)

# --------------------------------------------------------------------------- #
# Vocabularies                                                                #
# --------------------------------------------------------------------------- #

#: Preview tiers that make a security-sensitive subject accrue obligations.
_PREVIEW_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
    }
)

#: GA-tier maturity statuses. Only these can *close* an obligation, because the
#: obligation is created precisely by preview (non-GA) maturity.
_GA_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
    }
)


class SecurityObligation(ClosedStrEnum):
    """The four Requirement 9.8 obligations a preview security subject accrues."""

    MAINTENANCE = "maintenance"
    CERTIFICATION = "certification"
    DEPLOYMENT = "deployment"
    VULNERABILITY_RESPONSE = "vulnerability-response"


class SubjectKind(ClosedStrEnum):
    """Whether a subject is an ecosystem/release capability or a responsibility."""

    CAPABILITY = "capability"
    RESPONSIBILITY = "responsibility"


@dataclass(frozen=True, slots=True)
class ObligationSpec:
    """A single Requirement 9.8 obligation with its detection markers and template."""

    obligation: SecurityObligation
    label: str
    markers: tuple[str, ...]
    acceptance_evidence: str

    def __post_init__(self) -> None:
        if not isinstance(self.obligation, SecurityObligation):
            raise TypeError("obligation must be a SecurityObligation")
        if not self.label.strip():
            raise ValueError("label must not be empty")
        markers = tuple(self.markers)
        if not markers:
            raise ValueError("markers must not be empty")
        object.__setattr__(self, "markers", markers)
        if not self.acceptance_evidence.strip():
            raise ValueError("acceptance_evidence must not be empty")


#: The four obligations, in a fixed, deterministic order.
OBLIGATION_SPECS: tuple[ObligationSpec, ...] = (
    ObligationSpec(
        obligation=SecurityObligation.MAINTENANCE,
        label="maintenance",
        markers=(
            "security maintenance",
            "actively maintained",
            "maintenance process",
            "maintenance lifecycle",
            "maintained package",
            "ongoing maintenance",
        ),
        acceptance_evidence=(
            "Direct GA-tier evidence of a sustained security-maintenance process "
            "for the preview security-sensitive subject."
        ),
    ),
    ObligationSpec(
        obligation=SecurityObligation.CERTIFICATION,
        label="certification",
        markers=(
            "certification",
            "certified",
            "compliance certification",
            "security certification",
            "attested compliance",
        ),
        acceptance_evidence=(
            "Direct GA-tier evidence of security certification/compliance for the "
            "preview security-sensitive subject."
        ),
    ),
    ObligationSpec(
        obligation=SecurityObligation.DEPLOYMENT,
        label="deployment",
        markers=(
            "production deployment",
            "deployment process",
            "deployment pipeline",
            "supported deployment",
            "deployed in production",
        ),
        acceptance_evidence=(
            "Direct GA-tier evidence of a supported production deployment path for "
            "the preview security-sensitive subject."
        ),
    ),
    ObligationSpec(
        obligation=SecurityObligation.VULNERABILITY_RESPONSE,
        label="vulnerability response",
        markers=(
            "vulnerability response",
            "vulnerability-response",
            "cve response",
            "security advisory",
            "incident response",
            "coordinated disclosure",
        ),
        acceptance_evidence=(
            "Direct GA-tier evidence of a vulnerability-response/incident-response "
            "process for the preview security-sensitive subject."
        ),
    ),
)


# --------------------------------------------------------------------------- #
# Result types                                                                #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True, slots=True, kw_only=True)
class PreviewSecuritySubject:
    """A preview + security-sensitive subject and its obligation disposition."""

    subject_id: ReferenceId
    name: str
    kind: SubjectKind
    observed_status: EvidenceStatus
    target_level: TargetLevel
    supporting_evidence_ids: tuple[ReferenceId, ...]
    open_obligations: tuple[SecurityObligation, ...]
    closed_obligations: tuple[SecurityObligation, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "subject_id", reference(self.subject_id))
        if not self.name.strip():
            raise ValueError("name must not be empty")
        if not isinstance(self.kind, SubjectKind):
            raise TypeError("kind must be a SubjectKind")
        if self.observed_status not in _PREVIEW_STATUSES:
            raise ValueError("observed_status must be a preview tier")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(v) for v in self.supporting_evidence_ids}, key=str)),
        )
        for name in ("open_obligations", "closed_obligations"):
            values = tuple(getattr(self, name))
            if not all(isinstance(v, SecurityObligation) for v in values):
                raise TypeError(f"{name} must contain SecurityObligation values")
            object.__setattr__(self, name, values)


@dataclass(frozen=True, slots=True)
class PreviewSecurityObligationEvaluation:
    """Generator output: preview security subjects and their obligation gaps."""

    subjects: tuple[PreviewSecuritySubject, ...]
    obligation_gaps: tuple[GapEntry, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "subjects",
            tuple(sorted(self.subjects, key=lambda s: str(s.subject_id))),
        )
        object.__setattr__(
            self,
            "obligation_gaps",
            tuple(sorted(self.obligation_gaps, key=lambda gap: str(gap.id))),
        )

    def subject_for(self, subject_id: str) -> PreviewSecuritySubject | None:
        target = str(subject_id)
        for subject in self.subjects:
            if str(subject.subject_id) == target:
                return subject
        return None

    def gaps_for_subject(self, subject_id: str) -> tuple[GapEntry, ...]:
        target = str(subject_id)
        return tuple(
            gap
            for gap in self.obligation_gaps
            if target in {str(ref) for ref in gap.domain_ids}
        )

    def obligation_gap(
        self, subject_id: str, obligation: SecurityObligation
    ) -> GapEntry | None:
        if not isinstance(obligation, SecurityObligation):
            raise TypeError("obligation must be a SecurityObligation")
        gap_id = str(_gap_id(str(subject_id), obligation))
        for gap in self.obligation_gaps:
            if str(gap.id) == gap_id:
                return gap
        return None


# --------------------------------------------------------------------------- #
# Detection helpers                                                           #
# --------------------------------------------------------------------------- #


def _record_text(record: EvidenceRecord) -> str:
    return f"{record.claim_key}\n{record.claim}".lower()


def _gap_id(subject_id: str, obligation: SecurityObligation) -> object:
    return stable_id("gap", "preview-security", subject_id, obligation.value)


def _closed_obligations(
    records: tuple[EvidenceRecord, ...],
    present_permitted: Mapping[str, bool],
) -> frozenset[SecurityObligation]:
    """Return the obligations that direct GA-tier evidence independently closes.

    An obligation is closed only by a record that matches its markers, carries a
    GA-tier status, and is permitted in present tense by the Claim Guard (direct
    current-revision implementation evidence). Each obligation is evaluated on its
    own; closing one never closes another.
    """

    closed: set[SecurityObligation] = set()
    for spec in OBLIGATION_SPECS:
        for record in records:
            if record.status not in _GA_STATUSES:
                continue
            if not present_permitted.get(str(record.id), False):
                continue
            text = _record_text(record)
            if any(marker in text for marker in spec.markers):
                closed.add(spec.obligation)
                break
    return frozenset(closed)


# --------------------------------------------------------------------------- #
# Generator                                                                   #
# --------------------------------------------------------------------------- #


class PreviewSecurityObligationGenerator:
    """Generate Requirement 9.8 obligation gaps from the Task 7.4 evaluation."""

    def generate(
        self,
        bundle: EvidenceBundle,
        evaluation: ApplicationEcosystemReleaseEvaluation | None = None,
        guarded: GuardedEvidence | None = None,
    ) -> PreviewSecurityObligationEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")
        if evaluation is None:
            evaluation = evaluate_application_ecosystem_release(bundle, guarded)
        if not isinstance(evaluation, ApplicationEcosystemReleaseEvaluation):
            raise TypeError(
                "evaluation must be an ApplicationEcosystemReleaseEvaluation"
            )

        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted
            for claim in guarded.claims
        }
        closed = _closed_obligations(bundle.records, present_permitted)

        subjects: list[PreviewSecuritySubject] = []
        gaps: list[GapEntry] = []

        # Consume the Task 7.4 reusable accessors: security-sensitive ecosystem/
        # release drafts and security-sensitive application responsibilities.
        for draft in evaluation.security_sensitive_drafts():
            subject = self._subject_from_draft(draft, closed)
            if subject is not None:
                subjects.append(subject)
                gaps.extend(self._gaps_for_subject(subject))

        for responsibility in evaluation.security_sensitive_responsibilities():
            subject = self._subject_from_responsibility(responsibility, closed)
            if subject is not None:
                subjects.append(subject)
                gaps.extend(self._gaps_for_subject(subject))

        return PreviewSecurityObligationEvaluation(
            subjects=tuple(subjects),
            obligation_gaps=tuple(gaps),
        )

    @staticmethod
    def _subject_from_draft(
        draft: MaturityDomainDraft,
        closed: frozenset[SecurityObligation],
    ) -> PreviewSecuritySubject | None:
        if draft.observed_status not in _PREVIEW_STATUSES:
            return None
        open_obligations = tuple(
            spec.obligation
            for spec in OBLIGATION_SPECS
            if spec.obligation not in closed
        )
        return PreviewSecuritySubject(
            subject_id=reference(draft.domain.id),
            name=draft.domain.name,
            kind=SubjectKind.CAPABILITY,
            observed_status=draft.observed_status,
            target_level=draft.domain.target_level,
            supporting_evidence_ids=draft.supporting_evidence_ids,
            open_obligations=open_obligations,
            closed_obligations=tuple(
                spec.obligation for spec in OBLIGATION_SPECS if spec.obligation in closed
            ),
        )

    @staticmethod
    def _subject_from_responsibility(
        responsibility: ApplicationResponsibilityDraft,
        closed: frozenset[SecurityObligation],
    ) -> PreviewSecuritySubject | None:
        if responsibility.observed_status not in _PREVIEW_STATUSES:
            return None
        open_obligations = tuple(
            spec.obligation
            for spec in OBLIGATION_SPECS
            if spec.obligation not in closed
        )
        return PreviewSecuritySubject(
            subject_id=reference(responsibility.responsibility_id),
            name=responsibility.name,
            kind=SubjectKind.RESPONSIBILITY,
            observed_status=responsibility.observed_status,
            # Application responsibilities are assessed as a T1 platform concern.
            target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
            supporting_evidence_ids=responsibility.supporting_evidence_ids,
            open_obligations=open_obligations,
            closed_obligations=tuple(
                spec.obligation for spec in OBLIGATION_SPECS if spec.obligation in closed
            ),
        )

    def _gaps_for_subject(
        self, subject: PreviewSecuritySubject
    ) -> list[GapEntry]:
        gaps: list[GapEntry] = []
        for spec in OBLIGATION_SPECS:
            if spec.obligation not in subject.open_obligations:
                continue
            gaps.append(self._build_gap(subject, spec))
        return gaps

    @staticmethod
    def _build_gap(
        subject: PreviewSecuritySubject, spec: ObligationSpec
    ) -> GapEntry:
        observed_fact = (
            f"{subject.name} is a security-sensitive {subject.kind.value} at "
            f"preview status {subject.observed_status.value}; the {spec.label} "
            "obligation has no independent direct closing evidence."
        )
        recommendation = (
            f"Provide direct GA-tier implementation/execution evidence closing the "
            f"{spec.label} obligation for {subject.name}; preview status alone does "
            "not satisfy the Requirement 9.8 security-maturity obligation."
        )
        return GapEntry(
            id=stable_id("gap", "preview-security", str(subject.subject_id), spec.obligation.value),
            title=f"{subject.name}: {spec.label} obligation (preview security)",
            primary_category=GapCategory.ECOSYSTEM,
            secondary_categories=(),
            domain_ids=(reference(subject.subject_id),),
            current_status=subject.observed_status,
            target_level=subject.target_level,
            severity=Severity.HIGH,
            dependencies=(),
            acceptance_evidence=(spec.acceptance_evidence,),
            recommended_owner_area="Security",
            dependency_criticality=1,
            safety_impact=1,
            claim_risk=1,
            target_unblock_value=1,
            observed_fact=observed_fact,
            recommendation=recommendation,
        )


def evaluate_preview_security_obligations(
    bundle: EvidenceBundle,
    evaluation: ApplicationEcosystemReleaseEvaluation | None = None,
    guarded: GuardedEvidence | None = None,
) -> PreviewSecurityObligationEvaluation:
    """Convenience API for the preview-security ecosystem obligation generator."""

    return PreviewSecurityObligationGenerator().generate(bundle, evaluation, guarded)
