"""Exclusion and trust-assumption auditing (Task 4.4).

This component builds on the Task 4.1 :class:`~tools.universe_os_gap_analysis.evidence.EvidenceBundle`
and the Task 4.3 Claim Guard layer. Where the Claim Guard governs *wording*, this
auditor governs *disclosure*: it detects the opaque/dynamic/FFI/unsafe safety
exclusions and the trusted-tool / cooperative-descendant / caller-controlled-
directory / host-security-service trust assumptions that an ``Evidence_Record``
implies, then requires every detected assumption to be recorded in that record's
disclosure fields (``limitations`` or ``trust_assumptions``).

It never mutates evidence. Its single job is to compute, per record, the set
difference between *detected* and *recorded* assumptions and to fail closed the
moment any detected assumption is left unrecorded, emitting a structured
``CLM-*`` finding/error that cites the affected ``Evidence_Record`` references and
the governing requirement references.

Requirements implemented here:

* **6.6 -- exclusion disclosure.** Any safety guarantee that excludes an opaque,
  dynamic, FFI, or unsafe boundary must carry that exclusion in the related
  ``Evidence_Record``.
* **9.5 -- trust-assumption disclosure.** Evidence that assumes trusted tools,
  cooperative descendants, caller-controlled directories, or host security
  services must record each such assumption as a limitation.
* **9.6 -- fail closed on omission.** Any unrecorded, detected assumption
  (a non-empty ``detected - recorded`` set difference) immediately invalidates the
  assessment with a ``CLM-*`` finding that cites the affected record references.

Detection is symmetric with recording: the same marker scanner runs over the
claim text (to detect implied assumptions) and over the recorded disclosure text
(to detect satisfied assumptions), so an assumption is "recorded" exactly when a
limitation/trust-assumption entry names the same category. This makes
``detected - recorded`` a well-defined, order-independent set difference.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Mapping

from .evidence import EvidenceBundle
from .identifiers import ReferenceId, reference
from .models import ClosedStrEnum, EvidenceRecord, FindingSeverity, ValidationFinding

#: Structured error family for unrecorded exclusions/trust assumptions. Every
#: finding and error raised by this auditor uses this ``CLM-*`` code so that the
#: publish-time validator (Task 10) and any caller can route it deterministically.
CLM_UNRECORDED_ASSUMPTION_CODE = "CLM-TRUST-UNRECORDED"


class AssumptionKind(ClosedStrEnum):
    """Whether a detected assumption is a safety exclusion or a trust assumption."""

    #: An opaque/dynamic/FFI/unsafe safety-guarantee exclusion (Requirement 6.6).
    EXCLUSION = "Exclusion"
    #: A trusted-tool / cooperative-descendant / caller-controlled-directory /
    #: host-security-service trust assumption (Requirement 9.5).
    TRUST_ASSUMPTION = "TrustAssumption"


class AssumptionCategory(ClosedStrEnum):
    """A single detectable exclusion or trust-assumption category."""

    # -- opaque/dynamic/FFI/unsafe safety exclusions (Requirement 6.6) -------- #
    OPAQUE_EXCLUSION = "opaque-exclusion"
    DYNAMIC_EXCLUSION = "dynamic-exclusion"
    FFI_EXCLUSION = "ffi-exclusion"
    UNSAFE_EXCLUSION = "unsafe-exclusion"
    # -- trust assumptions (Requirement 9.5) --------------------------------- #
    TRUSTED_TOOL = "trusted-tool"
    COOPERATIVE_DESCENDANT = "cooperative-descendant"
    CALLER_CONTROLLED_DIRECTORY = "caller-controlled-directory"
    HOST_SECURITY_SERVICE = "host-security-service"


@dataclass(frozen=True, slots=True)
class _CategorySpec:
    """The detection contract for one assumption category."""

    category: AssumptionCategory
    kind: AssumptionKind
    requirement_ref: str
    #: Whole-word markers matched with word boundaries (case-insensitive).
    word_markers: tuple[str, ...] = ()
    #: Multi-word phrase markers matched as substrings (case-insensitive).
    phrase_markers: tuple[str, ...] = ()


# The category registry. ``detect`` and ``record`` both consume this identical
# table so detection and recording stay symmetric.
_CATEGORY_SPECS: tuple[_CategorySpec, ...] = (
    _CategorySpec(
        category=AssumptionCategory.OPAQUE_EXCLUSION,
        kind=AssumptionKind.EXCLUSION,
        requirement_ref="6.6",
        word_markers=("opaque",),
    ),
    _CategorySpec(
        category=AssumptionCategory.DYNAMIC_EXCLUSION,
        kind=AssumptionKind.EXCLUSION,
        requirement_ref="6.6",
        word_markers=("dynamic",),
        phrase_markers=("dynamic dispatch", "dynamic boundary"),
    ),
    _CategorySpec(
        category=AssumptionCategory.FFI_EXCLUSION,
        kind=AssumptionKind.EXCLUSION,
        requirement_ref="6.6",
        word_markers=("ffi",),
        phrase_markers=("foreign function interface", "foreign-function"),
    ),
    _CategorySpec(
        category=AssumptionCategory.UNSAFE_EXCLUSION,
        kind=AssumptionKind.EXCLUSION,
        requirement_ref="6.6",
        word_markers=("unsafe",),
    ),
    _CategorySpec(
        category=AssumptionCategory.TRUSTED_TOOL,
        kind=AssumptionKind.TRUST_ASSUMPTION,
        requirement_ref="9.5",
        phrase_markers=(
            "trusted tool",
            "trusted toolchain",
            "trusted compiler",
            "trusted linker",
            "trusted build tool",
        ),
    ),
    _CategorySpec(
        category=AssumptionCategory.COOPERATIVE_DESCENDANT,
        kind=AssumptionKind.TRUST_ASSUMPTION,
        requirement_ref="9.5",
        phrase_markers=(
            "cooperative descendant",
            "cooperative child",
            "cooperative subprocess",
            "cooperative descendants",
            "well-behaved descendant",
        ),
    ),
    _CategorySpec(
        category=AssumptionCategory.CALLER_CONTROLLED_DIRECTORY,
        kind=AssumptionKind.TRUST_ASSUMPTION,
        requirement_ref="9.5",
        phrase_markers=(
            "caller-controlled directory",
            "caller controlled directory",
            "caller-provided directory",
            "caller-supplied directory",
            "caller-controlled path",
        ),
    ),
    _CategorySpec(
        category=AssumptionCategory.HOST_SECURITY_SERVICE,
        kind=AssumptionKind.TRUST_ASSUMPTION,
        requirement_ref="9.5",
        phrase_markers=(
            "host security service",
            "host security services",
            "host-provided security",
            "host-provided security service",
            "operating system security service",
            "os security service",
        ),
    ),
)

_CATEGORY_BY_VALUE: Mapping[AssumptionCategory, _CategorySpec] = {
    spec.category: spec for spec in _CATEGORY_SPECS
}

# Requirement references cited by every unrecorded-assumption finding: the two
# disclosure requirements (6.6 exclusion, 9.5 trust assumption) and the
# fail-closed requirement (9.6).
_FAILURE_REQUIREMENT_REFS: tuple[str, ...] = ("6.6", "9.5", "9.6")

# Precompiled whole-word patterns for single-word markers.
_WORD_PATTERNS: Mapping[AssumptionCategory, tuple[re.Pattern[str], ...]] = {
    spec.category: tuple(
        re.compile(rf"\b{re.escape(marker)}\b", re.IGNORECASE)
        for marker in spec.word_markers
    )
    for spec in _CATEGORY_SPECS
}


def _detect_categories(text: str) -> frozenset[AssumptionCategory]:
    """Return every assumption category implied by ``text`` (case-insensitive)."""

    if not text:
        return frozenset()
    lowered = text.lower()
    found: set[AssumptionCategory] = set()
    for spec in _CATEGORY_SPECS:
        if any(phrase in lowered for phrase in spec.phrase_markers):
            found.add(spec.category)
            continue
        if any(pattern.search(text) for pattern in _WORD_PATTERNS[spec.category]):
            found.add(spec.category)
    return frozenset(found)


@dataclass(frozen=True, slots=True, kw_only=True)
class RecordAssumptionAudit:
    """Per-record detection, recording, and set-difference result."""

    evidence_id: ReferenceId
    claim_key: str
    detected: tuple[AssumptionCategory, ...]
    recorded: tuple[AssumptionCategory, ...]
    unrecorded: tuple[AssumptionCategory, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "evidence_id", reference(self.evidence_id))
        if not isinstance(self.claim_key, str) or not self.claim_key.strip():
            raise ValueError("claim_key must be a non-empty string")
        for name in ("detected", "recorded", "unrecorded"):
            values = getattr(self, name)
            if not all(isinstance(item, AssumptionCategory) for item in values):
                raise TypeError(f"{name} must contain AssumptionCategory values")
            object.__setattr__(
                self, name, tuple(sorted(set(values), key=lambda item: item.value))
            )
        # The unrecorded set is exactly detected minus recorded; enforce it so a
        # constructed audit can never disagree with the set-difference contract.
        expected = tuple(
            category
            for category in self.detected
            if category not in set(self.recorded)
        )
        if self.unrecorded != expected:
            raise ValueError("unrecorded must equal detected minus recorded")

    @property
    def is_complete(self) -> bool:
        """True when every detected assumption is recorded."""

        return not self.unrecorded


class TrustAssumptionAuditError(RuntimeError):
    """Structured ``CLM-*`` failure for an unrecorded exclusion/trust assumption.

    Raised by :func:`audit_trust_assumptions` (fail-closed) the moment any record
    leaves a detected assumption unrecorded. Carries the affected evidence
    references and governing requirement references so callers never continue on
    a partial or silently-repaired audit.
    """

    def __init__(
        self,
        message: str,
        *,
        evidence_refs: Iterable[ReferenceId | str],
        requirement_refs: Iterable[str] = _FAILURE_REQUIREMENT_REFS,
        code: str = CLM_UNRECORDED_ASSUMPTION_CODE,
    ) -> None:
        if not code.startswith("CLM-"):
            raise ValueError("trust-assumption error codes must start with CLM-")
        super().__init__(message)
        self.code = code
        self.message = message
        self.evidence_refs = tuple(sorted({str(ref) for ref in evidence_refs}))
        self.requirement_refs = tuple(sorted({str(ref) for ref in requirement_refs}))

    def to_dict(self) -> dict[str, object]:
        return {
            "code": self.code,
            "message": self.message,
            "evidenceRefs": list(self.evidence_refs),
            "requirementRefs": list(self.requirement_refs),
        }


@dataclass(frozen=True, slots=True)
class TrustAuditReport:
    """The auditor's output: per-record audits plus fail-closed findings."""

    audits: tuple[RecordAssumptionAudit, ...]

    def __post_init__(self) -> None:
        audits = tuple(self.audits)
        if not all(isinstance(item, RecordAssumptionAudit) for item in audits):
            raise TypeError("audits must contain RecordAssumptionAudit values")
        object.__setattr__(
            self, "audits", tuple(sorted(audits, key=lambda item: str(item.evidence_id)))
        )

    @property
    def is_complete(self) -> bool:
        """True when no record has an unrecorded, detected assumption."""

        return all(audit.is_complete for audit in self.audits)

    @property
    def incomplete_audits(self) -> tuple[RecordAssumptionAudit, ...]:
        """Every record that left a detected assumption unrecorded."""

        return tuple(audit for audit in self.audits if not audit.is_complete)

    @property
    def unrecorded_evidence_refs(self) -> tuple[str, ...]:
        return tuple(str(audit.evidence_id) for audit in self.incomplete_audits)

    def audit_for(self, evidence_id: ReferenceId | str) -> RecordAssumptionAudit | None:
        target = str(evidence_id)
        for audit in self.audits:
            if str(audit.evidence_id) == target:
                return audit
        return None

    def validation_findings(self) -> tuple[ValidationFinding, ...]:
        """Structured ``CLM-*`` findings for every unrecorded assumption.

        One finding is emitted per (record, unrecorded category) pair so the
        report cites both the affected record and the specific missing
        disclosure, with the requirement reference that governs the category and
        the fail-closed requirement 9.6.
        """

        findings: list[ValidationFinding] = []
        for audit in self.incomplete_audits:
            for category in audit.unrecorded:
                spec = _CATEGORY_BY_VALUE[category]
                findings.append(
                    ValidationFinding(
                        severity=FindingSeverity.ERROR,
                        code=CLM_UNRECORDED_ASSUMPTION_CODE,
                        requirement_refs=(spec.requirement_ref, "9.6"),
                        object_refs=(audit.evidence_id,),
                    )
                )
        return tuple(findings)

    def enforce(self) -> "TrustAuditReport":
        """Fail closed if any detected assumption is unrecorded (Requirement 9.6)."""

        if self.is_complete:
            return self
        missing = sorted(
            (
                f"{audit.claim_key} [{audit.evidence_id}]: "
                f"{', '.join(category.value for category in audit.unrecorded)}"
            )
            for audit in self.incomplete_audits
        )
        requirement_refs: set[str] = {"9.6"}
        for audit in self.incomplete_audits:
            for category in audit.unrecorded:
                requirement_refs.add(_CATEGORY_BY_VALUE[category].requirement_ref)
        raise TrustAssumptionAuditError(
            "unrecorded exclusion/trust assumption(s) detected: " + "; ".join(missing),
            evidence_refs=self.unrecorded_evidence_refs,
            requirement_refs=requirement_refs,
        )


class TrustAssumptionAuditor:
    """Detect and reconcile exclusions/trust assumptions per evidence record."""

    def audit(
        self, source: EvidenceBundle | Iterable[EvidenceRecord]
    ) -> TrustAuditReport:
        """Compute the per-record ``detected - recorded`` audit (never raises)."""

        records = self._records(source)
        audits: list[RecordAssumptionAudit] = []
        for record in records:
            detected = _detect_categories(f"{record.claim_key}\n{record.claim}")
            if not detected:
                # A record that implies no exclusion/trust assumption needs no
                # disclosure; it can never be a source of an unrecorded gap.
                audits.append(
                    RecordAssumptionAudit(
                        evidence_id=reference(record.id),
                        claim_key=record.claim_key,
                        detected=(),
                        recorded=(),
                        unrecorded=(),
                    )
                )
                continue
            recorded_text = "\n".join(
                (*record.limitations, *record.trust_assumptions)
            )
            recorded = _detect_categories(recorded_text)
            unrecorded = tuple(
                sorted(
                    (category for category in detected if category not in recorded),
                    key=lambda item: item.value,
                )
            )
            audits.append(
                RecordAssumptionAudit(
                    evidence_id=reference(record.id),
                    claim_key=record.claim_key,
                    detected=tuple(detected),
                    recorded=tuple(recorded),
                    unrecorded=unrecorded,
                )
            )
        return TrustAuditReport(audits=tuple(audits))

    @staticmethod
    def _records(
        source: EvidenceBundle | Iterable[EvidenceRecord],
    ) -> tuple[EvidenceRecord, ...]:
        if isinstance(source, EvidenceBundle):
            return source.records
        records = tuple(source)
        if not all(isinstance(item, EvidenceRecord) for item in records):
            raise TypeError("source must be an EvidenceBundle or EvidenceRecord values")
        return records


def audit_trust_assumptions(
    source: EvidenceBundle | Iterable[EvidenceRecord],
) -> TrustAuditReport:
    """Audit exclusions/trust assumptions and fail closed on any omission.

    Returns the complete :class:`TrustAuditReport` when every detected exclusion
    and trust assumption is recorded; raises :class:`TrustAssumptionAuditError`
    (a structured ``CLM-*`` failure) immediately otherwise (Requirement 9.6).
    """

    return TrustAssumptionAuditor().audit(source).enforce()
