"""Claim Guard: status, wording, scope, and non-claim governance (Task 4.3).

The Claim Guard consumes the Task 4.1 :class:`~tools.universe_os_gap_analysis.evidence.EvidenceBundle`
and produces :class:`GuardedEvidence`: a wording/labelling layer that never
mutates the underlying :class:`~tools.universe_os_gap_analysis.models.EvidenceRecord`
values and never upgrades a status. Its rules implement the design's Claim Guard
responsibilities and Requirements 4.3-4.6, 7.6, 8.6, 11.6, 13.1-13.3, 13.6, 13.7:

* **Present-tense gating (13.1).** A present-tense implementation claim is
  permitted only when a record carries *direct current-revision implementation
  or executable evidence*: an implemented status, a direct implementation/
  executable evidence kind, and a current-revision origin. Release notes, RFCs,
  examples, plans, and tagged-release evidence never license present tense.
* **Status preservation, no summary upgrade (4.3, 8.6, 11.6).** The guarded
  status is always exactly the record's status. GA/preview/experimental/planned/
  unsupported/unknown are preserved verbatim, and hosted/scoped-release statuses
  are flagged as unable to raise OS-substrate maturity.
* **Primitive-object wording (4.4, 7.6).** Evidence about the primitive
  freestanding object path is described only as "clang-backed ELF64
  relocatable-object emission", never as direct backend, linked image, runtime,
  or boot evidence.
* **Example/documentation labelling (4.6, 13.3).** Examples and documentation
  are labelled by their strongest directly supported status and can never claim
  present-tense implementation beyond that status.
* **Explicit non-claims (13.6).** Kernel, driver, interrupt, MMU, scheduler,
  syscall ABI, freestanding runtime, bootability, and backend independence remain
  explicit non-claims until a corresponding accepted gate exists.
* **Prerequisite-gate scope (13.7).** A passing prerequisite gate proves only its
  own named scope; the guard emits that standing statement and a per-gate note.

Out of scope for this component (owned by later tasks):

* Conflict detection and confidence downgrade (Task 4.2 / Requirements 1.5, 13.4).
* Trust-assumption and exclusion auditing (Task 4.4 / Requirements 6.6, 9.5, 9.6).
* Maturity capping and target-level achievement (Tasks 8.x); the guard only
  *flags* scoped evidence, it does not compute scores.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Mapping

from .evidence import EvidenceBundle
from .identifiers import ReferenceId, reference
from .models import (
    ClosedStrEnum,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    RevisionOrigin,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Standing statements and canonical wording                                   #
# --------------------------------------------------------------------------- #

#: Fixed wording for any evidence about the primitive freestanding object path
#: (Requirement 7.6 / design Property 13). The path is described only as
#: relocatable-object emission, never as a backend, linked image, runtime, or
#: boot capability.
PRIMITIVE_OBJECT_WORDING = "clang-backed ELF64 relocatable-object emission"

#: Scope note that accompanies primitive-object wording, naming the exclusions.
PRIMITIVE_OBJECT_SCOPE_NOTE = (
    "Proves only clang-backed ELF64 relocatable-object emission; it is not "
    "direct backend, linked-image, runtime, or boot evidence."
)

#: Terms a primitive-object claim must never assert (Requirement 7.6).
PRIMITIVE_OBJECT_FORBIDDEN_TERMS = (
    "direct backend",
    "native backend",
    "linked image",
    "linked elf",
    "bootable",
    "boot execution",
    "runtime",
    "kernel",
)

#: Standing statement required by Requirement 13.7.
PREREQUISITE_GATE_SCOPE_STATEMENT = (
    "A passing prerequisite gate proves only the named gate scope; it does not "
    "prove any dependent, downstream, or adjacent capability."
)

#: Annotation attached to records that reference the external host compiler,
#: which is a Compiler_Tooling_GA external production dependency (Requirement 4.3).
EXTERNAL_HOST_COMPILER_NOTE = (
    "The external host C++ compiler/toolchain is an external production "
    "dependency; it is not a Nebula-owned implementation."
)

# Case-insensitive markers used to recognize host-compiler evidence.
_HOST_COMPILER_MARKERS: tuple[str, ...] = (
    "host compiler",
    "host c++ compiler",
    "host toolchain",
    "external clang",
    "external host",
    "clang++",
)

# Case-insensitive markers used to recognize primitive freestanding object
# evidence when it is not passed explicitly by the caller.
_PRIMITIVE_OBJECT_MARKERS: tuple[str, ...] = (
    "primitive freestanding",
    "primitive object",
    "et_rel",
    "relocatable object",
    "relocatable-object",
    "freestanding object",
)

# Statuses that assert a present-tense (current) implementation to some scoped
# degree. Distinct tiers are preserved; none of them is ever upgraded.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Hosted or scoped-release statuses that, on their own, cannot raise OS-substrate
# maturity (Requirements 4.6, 8.6, 11.6).
_HOSTED_SCOPED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
    }
)

# Direct implementation/executable evidence kinds. Specification, RFC, release,
# workflow, example, test-definition, and non-claim evidence are not direct
# implementation evidence and therefore never license present tense.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {
        EvidenceKind.SOURCE,
        EvidenceKind.TEST_EXECUTION,
        EvidenceKind.ARTIFACT,
    }
)

# Origins that belong to the bound Assessment_Revision's current state. A
# tagged-release origin describes a published artifact, not current-revision
# implementation, so it never licenses present tense (Requirement 13.1).
_CURRENT_REVISION_ORIGINS: frozenset[RevisionOrigin] = frozenset(
    {
        RevisionOrigin.COMMITTED_REVISION,
        RevisionOrigin.CURRENT_WORKTREE,
        RevisionOrigin.EXECUTION_ARTIFACT,
    }
)

#: Topics that must remain explicit non-claims until an accepted gate exists
#: (Requirement 13.6). Ordered so the emitted non-claims are stable.
MANDATORY_NON_CLAIM_TOPICS: tuple[tuple[str, str], ...] = (
    ("kernel", "kernel entry, runtime, or resource management"),
    ("driver", "a device driver model or drivers"),
    ("interrupt", "interrupt or trap handling"),
    ("mmu", "an MMU or virtual-memory management"),
    ("scheduler", "scheduling or context switching"),
    ("syscall-abi", "a syscall ABI or capability boundary"),
    ("freestanding-runtime", "a freestanding core or runtime"),
    ("bootability", "a linked, bootable image or boot execution"),
    ("backend-independence", "an independent backend or bootstrap path"),
)


class ClaimTense(ClosedStrEnum):
    """The wording tense the guard permits for a record's claim."""

    #: Present-tense implementation wording (direct current-revision evidence).
    PRESENT = "Present"
    #: Future-tense wording for planned/roadmap/RFC work.
    FUTURE = "Future"
    #: Absence wording for explicit non-claims / audited absences.
    ABSENT = "Absent"
    #: Neutral wording for unknown or not-yet-provable evidence.
    NEUTRAL = "Neutral"


@dataclass(frozen=True, slots=True, kw_only=True)
class GuardedClaim:
    """A record's governed wording, tense, and scope labels (never a mutation)."""

    evidence_id: ReferenceId
    claim_key: str
    status: EvidenceStatus
    tense: ClaimTense
    present_tense_permitted: bool
    guarded_wording: str
    substrate_promotion_blocked: bool
    is_primitive_object: bool = False
    scope_note: str | None = None
    production_dependency_note: str | None = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "evidence_id", reference(self.evidence_id))
        if not isinstance(self.claim_key, str) or not self.claim_key.strip():
            raise ValueError("claim_key must be a non-empty string")
        if not isinstance(self.status, EvidenceStatus):
            raise TypeError("status must be an EvidenceStatus")
        if not isinstance(self.tense, ClaimTense):
            raise TypeError("tense must be a ClaimTense")
        for name in (
            "present_tense_permitted",
            "substrate_promotion_blocked",
            "is_primitive_object",
        ):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")
        if not isinstance(self.guarded_wording, str) or not self.guarded_wording.strip():
            raise ValueError("guarded_wording must be a non-empty string")
        for name in ("scope_note", "production_dependency_note"):
            value = getattr(self, name)
            if value is not None and (not isinstance(value, str) or not value.strip()):
                raise ValueError(f"{name} must be a non-empty string or None")
        # A present-tense wording is only ever emitted when it is permitted.
        if self.tense is ClaimTense.PRESENT and not self.present_tense_permitted:
            raise ValueError("present tense requires present_tense_permitted")


@dataclass(frozen=True, slots=True, kw_only=True)
class GuardedNonClaim:
    """A persisting explicit non-claim for an OS-substrate topic (Requirement 13.6)."""

    topic: str
    statement: str

    def __post_init__(self) -> None:
        for name in ("topic", "statement"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value.strip():
                raise ValueError(f"{name} must be a non-empty string")


@dataclass(frozen=True, slots=True)
class GuardedEvidence:
    """The Claim Guard's output: governed claims, non-claims, and standing notes."""

    claims: tuple[GuardedClaim, ...]
    non_claims: tuple[GuardedNonClaim, ...]
    released_non_claim_topics: tuple[str, ...]
    prerequisite_gate_scope_statement: str

    def __post_init__(self) -> None:
        claims = tuple(self.claims)
        if not all(isinstance(item, GuardedClaim) for item in claims):
            raise TypeError("claims must contain GuardedClaim values")
        non_claims = tuple(self.non_claims)
        if not all(isinstance(item, GuardedNonClaim) for item in non_claims):
            raise TypeError("non_claims must contain GuardedNonClaim values")
        object.__setattr__(self, "claims", tuple(sorted(claims, key=lambda item: str(item.evidence_id))))
        object.__setattr__(self, "non_claims", tuple(sorted(non_claims, key=lambda item: item.topic)))
        object.__setattr__(
            self, "released_non_claim_topics", tuple(sorted(set(self.released_non_claim_topics)))
        )
        if not self.prerequisite_gate_scope_statement.strip():
            raise ValueError("prerequisite_gate_scope_statement must be a non-empty string")

    @property
    def by_evidence_id(self) -> Mapping[str, GuardedClaim]:
        return {str(item.evidence_id): item for item in self.claims}

    def claim_for(self, evidence_id: str) -> GuardedClaim | None:
        return self.by_evidence_id.get(str(evidence_id))


class ClaimGuard:
    """Govern wording, status, scope, and non-claims without mutating evidence."""

    def guard(
        self,
        bundle: EvidenceBundle,
        *,
        accepted_gate_topics: Iterable[str] = (),
        primitive_object_claim_keys: Iterable[str] = (),
    ) -> GuardedEvidence:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        accepted = frozenset(self._require_topic(topic) for topic in accepted_gate_topics)
        unknown_topics = accepted - {topic for topic, _ in MANDATORY_NON_CLAIM_TOPICS}
        if unknown_topics:
            raise ValueError(f"unknown non-claim topics: {sorted(unknown_topics)}")
        primitive_keys = frozenset(
            self._require_claim_key(key) for key in primitive_object_claim_keys
        )

        claims = tuple(
            self._guard_record(record, primitive_keys) for record in bundle.records
        )
        non_claims = tuple(
            GuardedNonClaim(topic=topic, statement=self._non_claim_statement(topic, description))
            for topic, description in MANDATORY_NON_CLAIM_TOPICS
            if topic not in accepted
        )
        released = tuple(topic for topic, _ in MANDATORY_NON_CLAIM_TOPICS if topic in accepted)
        return GuardedEvidence(
            claims=claims,
            non_claims=non_claims,
            released_non_claim_topics=released,
            prerequisite_gate_scope_statement=PREREQUISITE_GATE_SCOPE_STATEMENT,
        )

    # -- per-record governance -------------------------------------------- #

    def _guard_record(
        self, record: EvidenceRecord, primitive_keys: frozenset[str]
    ) -> GuardedClaim:
        status = record.status
        present_ok = self._present_tense_permitted(record)
        is_primitive = record.claim_key in primitive_keys or self._is_primitive_object(record)
        substrate_blocked = self._blocks_substrate_promotion(record)
        production_note = (
            EXTERNAL_HOST_COMPILER_NOTE if self._is_host_compiler(record) else None
        )
        scope_note = self._scope_note(record, is_primitive)
        tense = self._tense(status, present_ok)
        wording = self._wording(record, status, tense, is_primitive)
        return GuardedClaim(
            evidence_id=reference(record.id),
            claim_key=record.claim_key,
            status=status,
            tense=tense,
            present_tense_permitted=present_ok,
            guarded_wording=wording,
            substrate_promotion_blocked=substrate_blocked,
            is_primitive_object=is_primitive,
            scope_note=scope_note,
            production_dependency_note=production_note,
        )

    @staticmethod
    def _present_tense_permitted(record: EvidenceRecord) -> bool:
        """Permit present tense only for direct current-revision implementation."""

        return (
            record.status in _IMPLEMENTED_STATUSES
            and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
            and record.origin in _CURRENT_REVISION_ORIGINS
        )

    @staticmethod
    def _blocks_substrate_promotion(record: EvidenceRecord) -> bool:
        """Flag hosted/scoped evidence that cannot raise OS-substrate maturity."""

        if record.status in _HOSTED_SCOPED_STATUSES:
            return True
        if record.evidence_kind is EvidenceKind.EXAMPLE:
            return True
        target_levels = record.scope.target_levels
        return bool(target_levels) and all(
            level is TargetLevel.T0_HOSTED_ADJACENCY for level in target_levels
        )

    @staticmethod
    def _tense(status: EvidenceStatus, present_ok: bool) -> ClaimTense:
        if status is EvidenceStatus.PLANNED:
            return ClaimTense.FUTURE
        if status is EvidenceStatus.UNSUPPORTED:
            return ClaimTense.ABSENT
        if status in _IMPLEMENTED_STATUSES and present_ok:
            return ClaimTense.PRESENT
        return ClaimTense.NEUTRAL

    @staticmethod
    def _wording(
        record: EvidenceRecord,
        status: EvidenceStatus,
        tense: ClaimTense,
        is_primitive: bool,
    ) -> str:
        if is_primitive:
            # Requirement 7.6: describe only as relocatable-object emission.
            return PRIMITIVE_OBJECT_WORDING
        claim = record.claim
        if tense is ClaimTense.PRESENT:
            return f"Implemented ({status.value}): {claim}"
        if tense is ClaimTense.FUTURE:
            return f"Planned future work ({status.value}): {claim}"
        if tense is ClaimTense.ABSENT:
            return f"Not implemented / explicit non-claim ({status.value}): {claim}"
        # NEUTRAL: unknown, or an implemented status lacking direct current
        # implementation evidence. Never asserts present-tense implementation.
        if status in _IMPLEMENTED_STATUSES:
            return (
                f"Reported ({status.value}) but not asserted in present tense "
                f"without direct current-revision implementation evidence: {claim}"
            )
        return f"Unverified ({status.value}): {claim}"

    @staticmethod
    def _scope_note(record: EvidenceRecord, is_primitive: bool) -> str | None:
        if is_primitive:
            return PRIMITIVE_OBJECT_SCOPE_NOTE
        gate_id = ClaimGuard._gate_id(record.claim_key)
        if gate_id is not None:
            return f"A passing {gate_id} proves only its named gate scope."
        return None

    @staticmethod
    def _gate_id(claim_key: str) -> str | None:
        if claim_key.startswith("gate:"):
            return claim_key.split(":", 1)[1]
        if claim_key.startswith("non-claim:gate:"):
            return claim_key.split("non-claim:gate:", 1)[1]
        return None

    @staticmethod
    def _is_primitive_object(record: EvidenceRecord) -> bool:
        haystack = f"{record.claim_key}\n{record.claim}".lower()
        return any(marker in haystack for marker in _PRIMITIVE_OBJECT_MARKERS)

    @staticmethod
    def _is_host_compiler(record: EvidenceRecord) -> bool:
        haystack = f"{record.claim_key}\n{record.claim}".lower()
        return any(marker in haystack for marker in _HOST_COMPILER_MARKERS)

    @staticmethod
    def _non_claim_statement(topic: str, description: str) -> str:
        return (
            f"Nebula does not currently provide {description}; this remains an "
            f"explicit non-claim until an accepted {topic} gate with direct "
            "implementation evidence exists."
        )

    @staticmethod
    def _require_topic(topic: str) -> str:
        if not isinstance(topic, str) or not topic.strip():
            raise ValueError("accepted gate topic must be a non-empty string")
        return topic

    @staticmethod
    def _require_claim_key(key: str) -> str:
        if not isinstance(key, str) or not key.strip():
            raise ValueError("primitive object claim key must be a non-empty string")
        return key


def guard_evidence(
    bundle: EvidenceBundle,
    *,
    accepted_gate_topics: Iterable[str] = (),
    primitive_object_claim_keys: Iterable[str] = (),
) -> GuardedEvidence:
    """Convenience API for the Claim Guard."""

    return ClaimGuard().guard(
        bundle,
        accepted_gate_topics=accepted_gate_topics,
        primitive_object_claim_keys=primitive_object_claim_keys,
    )
