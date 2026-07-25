"""Application ownership, ecosystem, and release-engineering evaluator (Task 7.4).

This declarative evaluator covers Requirement 11 (application platform,
ecosystem, and release engineering). It is a cross-cutting view over the hosted
application assets rather than an OS-substrate layer: its job is to state *who
owns* each application responsibility, to assess the surrounding ecosystem and
release-engineering maturity, and -- crucially -- to guarantee that release
evidence scoped to compiler/tooling or Linux backend SDK scope can never raise
OS-substrate maturity.

It builds on the Task 4.1 :class:`EvidenceBundle` and the Task 4.3 Claim Guard,
exactly like the sibling evaluators: the guard tells the evaluator which claims
may be asserted in the present tense (so a hosted/scoped-release asset cannot be
read as a Nebula-owned OS capability), while the bundle supplies claim text and
disclosed ``limitations`` that the evaluator surfaces on the gaps it emits.

Requirement 11 coverage:

* **11.1 application responsibilities.** CLI tools, backend services,
  control-plane applications, embedded data, authentication, jobs, TLS, crypto,
  UI semantics, thin-host bridges, and native host adapters are each assessed as
  an application responsibility with exactly one ownership.
* **11.2 / Property 19 exclusive ownership.** Every application responsibility is
  assigned exactly one of ``NebulaOwned | HostOwned | OperationsOwned``; the
  assignment is deterministic and ownership does not imply maturity outside that
  owner's capability domain.
* **11.3 renderer/distribution ownership.** Renderer, widget, layout,
  accessibility, device integration, signing, notarization, install, update,
  application distribution, and crash reporting are each assigned to their actual
  owner.
* **11.4 ecosystem maturity.** Package breadth, documentation, starter projects,
  compatibility governance, contributor workflow, third-party adoption, security
  maintenance, and long-term support are assessed and produce ``Ecosystem_Gap``
  entries when direct implementation evidence is absent.
* **11.5 release engineering.** Strict build matrices, contract suites, sanitizer
  lanes, release smoke, SBOM, provenance, attestations, installers, rollback, and
  cross-platform qualification are assessed and produce verification/ecosystem
  gaps when unproven.
* **11.6 / Property 9 scoped-release isolation.** Release evidence scoped to
  compiler/tooling GA or Linux backend SDK GA is flagged as unable to raise any
  OS-substrate maturity; the evaluator never converts such evidence into
  substrate credit.

Reusable output for the sibling Task 7.5 (preview-security ecosystem
obligations, Requirement 9.8): the evaluation exposes the ownership drafts and
the ecosystem drafts (including a ``security_sensitive`` flag and the observed
:class:`EvidenceStatus`) so Task 7.5 can derive maintenance/certification/
deployment/vulnerability-response obligations from this evaluator's output rather
than re-deriving ownership and ecosystem status itself.

Nothing here mutates evidence, assigns Hard-Gate scores (Task 8), ranks gaps
(Task 9), renders anything (Task 11), or edits sibling evaluator files.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Mapping

from ..claim_guard import GuardedEvidence, guard_evidence
from ..evidence import EvidenceBundle
from ..identifiers import ReferenceId, StableId, reference, stable_id
from ..models import (
    CapabilityDomain,
    ClosedStrEnum,
    EvidenceKind,
    EvidenceRecord,
    EvidenceStatus,
    GapCategory,
    GapEntry,
    Ownership,
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent.

# Ownership override markers. When a record does not carry an explicit
# ``scope.ownership`` these text markers let the evaluator recognise the owner.
_HOST_OWNED_MARKERS: tuple[str, ...] = (
    "host-owned",
    "host owned",
    "host os",
    "host operating system",
    "host toolkit",
    "host platform",
    "host library",
    "host framework",
    "thin-host",
    "thin host",
    "native host adapter",
    "host security service",
    "host crypto",
    "host tls",
)

_OPERATIONS_OWNED_MARKERS: tuple[str, ...] = (
    "operations-owned",
    "operations owned",
    "release engineering",
    "release pipeline",
    "release operations",
    "operational process",
    "signing pipeline",
    "distribution channel",
    "rollout",
    "ops-owned",
)

_NEBULA_OWNED_MARKERS: tuple[str, ...] = (
    "nebula-owned",
    "nebula owned",
    "nebula cli",
    "nebula compiler",
    "nebula std",
    "nebula runtime",
    "nebula package",
)

# Release evidence scoped to compiler/tooling or Linux backend SDK scope. Such
# evidence can never raise OS-substrate maturity (Requirement 11.6 / Property 9).
_SCOPED_RELEASE_MARKERS: tuple[str, ...] = (
    "compiler/tooling",
    "compiler tooling",
    "tooling ga",
    "linux backend sdk",
    "backend sdk",
    "backend-sdk",
    "hosted release",
    "scoped release",
)

# Statuses that are hosted/scoped-release GA tiers, mirroring the Claim Guard's
# hosted/scoped-release set. On their own they cannot raise OS-substrate maturity.
_SCOPED_RELEASE_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
    }
)

# Preview statuses (Requirement 8.6 tiers preserved through summaries).
_PREVIEW_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
    }
)

# Statuses that assert a present-tense/current implementation to some scoped
# degree, mirroring the Claim Guard's implemented-status set.
_IMPLEMENTED_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {
        EvidenceStatus.COMPILER_TOOLING_GA,
        EvidenceStatus.BACKEND_SDK_GA,
        EvidenceStatus.INSTALLED_PREVIEW,
        EvidenceStatus.REPO_PREVIEW,
        EvidenceStatus.EXPERIMENTAL,
    }
)

# Evidence kinds that count as direct implementation for a "current" capability.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Ordinal ranking of statuses, strongest first, for "strongest observed status".
_STATUS_STRENGTH: tuple[EvidenceStatus, ...] = (
    EvidenceStatus.COMPILER_TOOLING_GA,
    EvidenceStatus.BACKEND_SDK_GA,
    EvidenceStatus.INSTALLED_PREVIEW,
    EvidenceStatus.REPO_PREVIEW,
    EvidenceStatus.EXPERIMENTAL,
    EvidenceStatus.PLANNED,
    EvidenceStatus.UNSUPPORTED,
    EvidenceStatus.UNKNOWN,
)


class ResponsibilityGroup(ClosedStrEnum):
    """Which Requirement 11 concern a checklist item belongs to."""

    #: Application responsibilities partitioned by ownership (11.1, 11.3).
    APPLICATION = "application"
    #: Ecosystem maturity concerns (11.4).
    ECOSYSTEM = "ecosystem"
    #: Release-engineering concerns (11.5).
    RELEASE = "release"


class CapabilityKind(Enum):
    """The classification rule a checklist item applies."""

    #: An application responsibility that is assigned exactly one ownership.
    OWNERSHIP = "ownership"
    #: An ecosystem capability assessed for maturity.
    ECOSYSTEM_CAPABILITY = "ecosystem_capability"
    #: A release-engineering capability assessed for maturity.
    RELEASE_CAPABILITY = "release_capability"


# --------------------------------------------------------------------------- #
# Checklist item definitions                                                  #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True, slots=True)
class ApplicationResponsibilityItem:
    """One application responsibility with a default owner (Requirement 11.1/11.3)."""

    responsibility_id: StableId
    name: str
    default_ownership: Ownership
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    requirement_refs: tuple[str, ...]
    security_sensitive: bool
    owner_area: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "responsibility_id", StableId(self.responsibility_id))
        if not self.name.strip():
            raise ValueError("responsibility name must not be empty")
        if not isinstance(self.default_ownership, Ownership):
            raise TypeError("default_ownership must be an Ownership")
        for name in ("markers", "authoritative_paths", "requirement_refs"):
            value = tuple(getattr(self, name))
            if not value:
                raise ValueError(f"{name} must not be empty")
            object.__setattr__(self, name, value)
        if not isinstance(self.security_sensitive, bool):
            raise TypeError("security_sensitive must be a bool")
        if not self.owner_area.strip():
            raise ValueError("owner_area must not be empty")


@dataclass(frozen=True, slots=True)
class MaturityChecklistItem:
    """One ecosystem or release-engineering capability check (Requirement 11.4/11.5)."""

    capability_id: StableId
    name: str
    group: ResponsibilityGroup
    kind: CapabilityKind
    target_level: TargetLevel
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    unsatisfied_category: GapCategory
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    security_sensitive: bool
    owner_area: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.group, ResponsibilityGroup):
            raise TypeError("group must be a ResponsibilityGroup")
        if not isinstance(self.kind, CapabilityKind):
            raise TypeError("kind must be a CapabilityKind")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.unsatisfied_category, GapCategory):
            raise TypeError("unsatisfied_category must be a GapCategory")
        for name in ("markers", "authoritative_paths", "requirement_refs", "acceptance_evidence"):
            value = tuple(getattr(self, name))
            if not value:
                raise ValueError(f"{name} must not be empty")
            object.__setattr__(self, name, value)
        object.__setattr__(self, "non_claims", tuple(self.non_claims))
        if not isinstance(self.security_sensitive, bool):
            raise TypeError("security_sensitive must be a bool")
        if not self.owner_area.strip():
            raise ValueError("owner_area must not be empty")


# --------------------------------------------------------------------------- #
# Draft / evaluation result types                                             #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True, slots=True, kw_only=True)
class ApplicationResponsibilityDraft:
    """A responsibility with exactly one assigned owner (Requirement 11.2)."""

    responsibility_id: ReferenceId
    name: str
    ownership: Ownership
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    security_sensitive: bool
    owner_from_evidence: bool

    def __post_init__(self) -> None:
        object.__setattr__(self, "responsibility_id", reference(self.responsibility_id))
        if not isinstance(self.ownership, Ownership):
            raise TypeError("ownership must be an Ownership")
        if not isinstance(self.observed_status, EvidenceStatus):
            raise TypeError("observed_status must be an EvidenceStatus")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(value) for value in self.supporting_evidence_ids}, key=str)),
        )
        object.__setattr__(self, "limitations", tuple(self.limitations))
        for name in ("security_sensitive", "owner_from_evidence"):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")


@dataclass(frozen=True, slots=True, kw_only=True)
class MaturityDomainDraft:
    """A per-capability draft for an ecosystem or release-engineering domain."""

    domain: CapabilityDomain
    group: ResponsibilityGroup
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    security_sensitive: bool
    scoped_release_only: bool
    substrate_promotion_blocked: bool
    gap_id: ReferenceId | None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        if not isinstance(self.group, ResponsibilityGroup):
            raise TypeError("group must be a ResponsibilityGroup")
        if not isinstance(self.observed_status, EvidenceStatus):
            raise TypeError("observed_status must be an EvidenceStatus")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(value) for value in self.supporting_evidence_ids}, key=str)),
        )
        object.__setattr__(self, "limitations", tuple(self.limitations))
        for name in (
            "satisfied",
            "security_sensitive",
            "scoped_release_only",
            "substrate_promotion_blocked",
        ):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} must be a bool")
        if self.gap_id is not None:
            object.__setattr__(self, "gap_id", reference(self.gap_id))


@dataclass(frozen=True, slots=True)
class ApplicationEcosystemReleaseEvaluation:
    """Evaluator output: ownership drafts, ecosystem/release drafts, and gaps."""

    responsibility_drafts: tuple[ApplicationResponsibilityDraft, ...]
    ecosystem_drafts: tuple[MaturityDomainDraft, ...]
    release_drafts: tuple[MaturityDomainDraft, ...]
    gaps: tuple[GapEntry, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "responsibility_drafts",
            tuple(sorted(self.responsibility_drafts, key=lambda d: str(d.responsibility_id))),
        )
        object.__setattr__(
            self,
            "ecosystem_drafts",
            tuple(sorted(self.ecosystem_drafts, key=lambda d: str(d.domain.id))),
        )
        object.__setattr__(
            self,
            "release_drafts",
            tuple(sorted(self.release_drafts, key=lambda d: str(d.domain.id))),
        )
        object.__setattr__(
            self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id)))
        )

    # -- accessors (reused by the sibling Task 7.5 evaluator) ------------- #

    def responsibility_for(self, responsibility_id: str) -> ApplicationResponsibilityDraft | None:
        target = str(responsibility_id)
        for draft in self.responsibility_drafts:
            if str(draft.responsibility_id) == target:
                return draft
        return None

    def ownership_for(self, responsibility_id: str) -> Ownership | None:
        draft = self.responsibility_for(responsibility_id)
        return draft.ownership if draft is not None else None

    def responsibilities_owned_by(
        self, ownership: Ownership
    ) -> tuple[ApplicationResponsibilityDraft, ...]:
        if not isinstance(ownership, Ownership):
            raise TypeError("ownership must be an Ownership")
        return tuple(
            draft for draft in self.responsibility_drafts if draft.ownership is ownership
        )

    def maturity_drafts(self) -> tuple[MaturityDomainDraft, ...]:
        return self.ecosystem_drafts + self.release_drafts

    def ecosystem_draft_for(self, capability_id: str) -> MaturityDomainDraft | None:
        target = str(capability_id)
        for draft in self.maturity_drafts():
            if str(draft.domain.id) == target:
                return draft
        return None

    def security_sensitive_drafts(self) -> tuple[MaturityDomainDraft, ...]:
        """Ecosystem/release drafts flagged security-sensitive (input for Task 7.5)."""

        return tuple(
            draft for draft in self.maturity_drafts() if draft.security_sensitive
        )

    def security_sensitive_responsibilities(
        self,
    ) -> tuple[ApplicationResponsibilityDraft, ...]:
        """Security-sensitive application responsibilities (input for Task 7.5)."""

        return tuple(
            draft for draft in self.responsibility_drafts if draft.security_sensitive
        )

    def gap_for(self, capability_id: str) -> GapEntry | None:
        target = str(capability_id)
        for gap in self.gaps:
            if target in {str(ref) for ref in gap.domain_ids}:
                return gap
        return None


# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS``.
_PARENT_T0 = StableId("capability-t0-hosted-adjacency")
_PARENT_T1 = StableId("capability-t1-language-platform")


APPLICATION_RESPONSIBILITIES: tuple[ApplicationResponsibilityItem, ...] = (
    # -- Requirement 11.1 core application responsibilities --------------- #
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-cli-tools"),
        name="CLI tools",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("cli tool", "command-line tool", "nebula cli", "cli command"),
        authoritative_paths=("README.md", "cli/"),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Developer Tooling",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-backend-services"),
        name="Backend services",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("backend service", "nebula-service", "nebula service", "service runtime"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Backend Services",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-control-plane"),
        name="Control-plane applications",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("control plane", "control-plane", "management application"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Backend Services",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-embedded-data"),
        name="Embedded data",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("embedded data", "embedded database", "embedded store", "bundled data"),
        authoritative_paths=("std/",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Standard Library",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-authentication"),
        name="Authentication",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("authentication", "auth flow", "login", "credential"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=True,
        owner_area="Security",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-jobs"),
        name="Jobs and background work",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("job scheduler", "background job", "job queue", "worker job"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Backend Services",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-tls"),
        name="TLS",
        default_ownership=Ownership.HOST_OWNED,
        markers=("tls", "transport layer security", "https stack"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=True,
        owner_area="Security",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-crypto"),
        name="Cryptography",
        default_ownership=Ownership.HOST_OWNED,
        markers=("crypto", "cryptography", "cryptographic", "cipher"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=True,
        owner_area="Security",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-ui-semantics"),
        name="UI semantics",
        default_ownership=Ownership.NEBULA_OWNED,
        markers=("ui semantics", "ui model", "view semantics", "interface semantics"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Application UI",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-thin-host-bridge"),
        name="Thin-host bridges",
        default_ownership=Ownership.HOST_OWNED,
        markers=("thin-host", "thin host", "host bridge", "app-core bridge"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Host Integration",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-native-host-adapter"),
        name="Native host adapters",
        default_ownership=Ownership.HOST_OWNED,
        markers=("native host adapter", "native adapter", "host adapter", "platform adapter"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.1", "11.2"),
        security_sensitive=False,
        owner_area="Host Integration",
    ),
    # -- Requirement 11.3 renderer / distribution responsibilities -------- #
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-renderer"),
        name="Renderer",
        default_ownership=Ownership.HOST_OWNED,
        markers=("renderer", "rendering engine", "render pipeline"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Application UI",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-widget"),
        name="Widget toolkit",
        default_ownership=Ownership.HOST_OWNED,
        markers=("widget", "widget toolkit", "ui control"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Application UI",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-layout"),
        name="Layout",
        default_ownership=Ownership.HOST_OWNED,
        markers=("layout engine", "layout system", "layout"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Application UI",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-accessibility"),
        name="Accessibility",
        default_ownership=Ownership.HOST_OWNED,
        markers=("accessibility", "a11y", "assistive technology", "screen reader"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Application UI",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-device-integration"),
        name="Device integration",
        default_ownership=Ownership.HOST_OWNED,
        markers=("device integration", "device access", "peripheral integration"),
        authoritative_paths=("docs/universeos/architecture.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Host Integration",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-signing"),
        name="Signing",
        default_ownership=Ownership.OPERATIONS_OWNED,
        markers=("code signing", "signing key", "sign the", "signature"),
        authoritative_paths=(".github/workflows/release.yml",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=True,
        owner_area="Release Engineering",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-notarization"),
        name="Notarization",
        default_ownership=Ownership.HOST_OWNED,
        markers=("notarization", "notarize", "notarized"),
        authoritative_paths=(".github/workflows/release.yml",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=True,
        owner_area="Release Engineering",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-install"),
        name="Install",
        default_ownership=Ownership.OPERATIONS_OWNED,
        markers=("installer", "install flow", "installation package"),
        authoritative_paths=(".github/workflows/release.yml",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-update"),
        name="Update",
        default_ownership=Ownership.OPERATIONS_OWNED,
        markers=("update channel", "app update", "software update", "auto-update"),
        authoritative_paths=(".github/workflows/release.yml",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-distribution"),
        name="Application distribution",
        default_ownership=Ownership.OPERATIONS_OWNED,
        markers=("app distribution", "application distribution", "distribution channel", "release distribution"),
        authoritative_paths=(".github/workflows/release.yml",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    ApplicationResponsibilityItem(
        responsibility_id=StableId("responsibility-app-crash-reporting"),
        name="Crash reporting",
        default_ownership=Ownership.OPERATIONS_OWNED,
        markers=("crash reporting", "crash report", "crash telemetry"),
        authoritative_paths=("docs/support_matrix.md",),
        requirement_refs=("11.3", "11.2"),
        security_sensitive=False,
        owner_area="Operations",
    ),
)


ECOSYSTEM_CHECKLIST: tuple[MaturityChecklistItem, ...] = (
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-package-breadth"),
        name="Package breadth",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("package breadth", "package ecosystem", "official package", "registry breadth"),
        authoritative_paths=("docs/official_package_tiering.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "A broad, maintained package ecosystem with direct implementation "
            "evidence beyond preview tiers.",
        ),
        non_claims=(
            "Current official packages are largely installed/repo preview and do "
            "not constitute a broad independent package ecosystem.",
        ),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-documentation"),
        name="Documentation",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("documentation", "docs coverage", "reference docs", "guide"),
        authoritative_paths=("README.md", "docs/"),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "Comprehensive, maintained documentation covering the supported "
            "language, tooling, and package surface.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-starter-projects"),
        name="Starter projects",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("starter project", "starter template", "example project", "scaffold"),
        authoritative_paths=("README.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "Maintained starter projects and templates for common application "
            "shapes with direct implementation evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-compatibility-governance"),
        name="Compatibility governance",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("compatibility governance", "stability policy", "compatibility policy", "semver policy"),
        authoritative_paths=("docs/stability_policy.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "A governed compatibility policy with enforced backward-compatibility "
            "guarantees and direct implementation evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-contributor-workflow"),
        name="Contributor workflow",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("contributor workflow", "contributing guide", "contribution process"),
        authoritative_paths=("CONTRIBUTING.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "A documented, active contributor workflow with direct evidence of "
            "sustained external contribution.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-third-party-adoption"),
        name="Third-party adoption",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("third-party adoption", "third party adoption", "external adoption", "production adoption"),
        authoritative_paths=("README.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "Direct evidence of sustained third-party production adoption.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-security-maintenance"),
        name="Security maintenance",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("security maintenance", "vulnerability response", "security advisory", "cve response"),
        authoritative_paths=("docs/official_package_tiering.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "A sustained security-maintenance process with vulnerability response "
            "and direct implementation evidence.",
        ),
        non_claims=(
            "Security-sensitive packages remain preview without a proven "
            "vulnerability-response process.",
        ),
        security_sensitive=True,
        owner_area="Security",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-ecosystem-long-term-support"),
        name="Long-term support",
        group=ResponsibilityGroup.ECOSYSTEM,
        kind=CapabilityKind.ECOSYSTEM_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("long-term support", "long term support", "lts", "support lifecycle"),
        authoritative_paths=("docs/support_matrix.md",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.4",),
        acceptance_evidence=(
            "A committed long-term support lifecycle with direct implementation "
            "evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Ecosystem",
    ),
)


RELEASE_CHECKLIST: tuple[MaturityChecklistItem, ...] = (
    MaturityChecklistItem(
        capability_id=StableId("capability-release-strict-build-matrix"),
        name="Strict build matrices",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("strict build matrix", "build matrix", "four-platform build", "cross-platform build"),
        authoritative_paths=("CMakeLists.txt", ".github/workflows/contract-tests.yml"),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "A strict multi-platform build matrix with direct execution evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-contract-suites"),
        name="Contract suites",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("contract suite", "contract test", "contract-tests"),
        authoritative_paths=(".github/workflows/contract-tests.yml", "tests/README.md"),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "An executed contract test suite with direct execution evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-sanitizer-lanes"),
        name="Sanitizer lanes",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("sanitizer lane", "sanitizer", "asan", "ubsan"),
        authoritative_paths=(".github/workflows/contract-tests.yml",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Executed sanitizer lanes with direct execution evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-release-smoke"),
        name="Release smoke",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("release smoke", "smoke test", "release verification"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Executed release smoke verification with direct execution evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-sbom"),
        name="SBOM",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("sbom", "software bill of materials", "bill of materials"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Generated and published SBOM artifacts with direct evidence.",
        ),
        non_claims=(),
        security_sensitive=True,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-provenance"),
        name="Provenance",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("provenance", "build provenance", "supply-chain provenance"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Generated build provenance attestations with direct evidence.",
        ),
        non_claims=(),
        security_sensitive=True,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-attestations"),
        name="Attestations",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("attestation", "signed attestation", "artifact attestation"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Signed release attestations with direct evidence.",
        ),
        non_claims=(),
        security_sensitive=True,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-installers"),
        name="Installers",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("installer", "install package", "installation artifact"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Published installers with direct implementation evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-rollback"),
        name="Rollback",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("release rollback", "rollback", "roll back release"),
        authoritative_paths=(".github/workflows/release.yml",),
        unsatisfied_category=GapCategory.ECOSYSTEM,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "A proven release rollback mechanism with direct implementation evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
    MaturityChecklistItem(
        capability_id=StableId("capability-release-platform-qualification"),
        name="Cross-platform qualification",
        group=ResponsibilityGroup.RELEASE,
        kind=CapabilityKind.RELEASE_CAPABILITY,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        markers=("cross-platform qualification", "platform qualification", "platform certification"),
        authoritative_paths=("docs/support_matrix.md",),
        unsatisfied_category=GapCategory.VERIFICATION,
        requirement_refs=("11.5",),
        acceptance_evidence=(
            "Executed cross-platform qualification with direct execution evidence.",
        ),
        non_claims=(),
        security_sensitive=False,
        owner_area="Release Engineering",
    ),
)


# --------------------------------------------------------------------------- #
# Detection helpers                                                           #
# --------------------------------------------------------------------------- #


def _contains(text: str, markers: Iterable[str]) -> bool:
    return any(marker in text for marker in markers)


def _record_text(record: EvidenceRecord) -> str:
    return f"{record.claim_key}\n{record.claim}".lower()


def _strongest_status(records: Iterable[EvidenceRecord]) -> EvidenceStatus:
    present = {record.status for record in records}
    for status in _STATUS_STRENGTH:
        if status in present:
            return status
    return EvidenceStatus.UNKNOWN


def _has_current_implementation(record: EvidenceRecord) -> bool:
    return (
        record.status in _IMPLEMENTED_STATUSES
        and record.evidence_kind in _DIRECT_IMPLEMENTATION_KINDS
    )


def _is_scoped_release(record: EvidenceRecord) -> bool:
    """A record scoped to compiler/tooling or Linux backend SDK scope.

    Such evidence cannot raise OS-substrate maturity (Requirement 11.6 /
    Property 9). Recognised either by its hosted/scoped GA status or by an
    explicit compiler/tooling or backend-SDK marker.
    """

    if record.status in _SCOPED_RELEASE_STATUSES:
        return True
    return _contains(_record_text(record), _SCOPED_RELEASE_MARKERS)


def _text_ownership(record: EvidenceRecord) -> Ownership | None:
    """Recognise an owner from record text markers (used only as a fallback)."""

    text = _record_text(record)
    # Host-owned takes precedence: a hosted asset can never be a Nebula-owned
    # OS-substrate capability (design Property 9).
    if _contains(text, _HOST_OWNED_MARKERS):
        return Ownership.HOST_OWNED
    if _contains(text, _OPERATIONS_OWNED_MARKERS):
        return Ownership.OPERATIONS_OWNED
    if _contains(text, _NEBULA_OWNED_MARKERS):
        return Ownership.NEBULA_OWNED
    return None


class ApplicationEcosystemReleaseEvaluator:
    """Evaluate Requirement 11 application, ecosystem, and release gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> ApplicationEcosystemReleaseEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")

        records = bundle.records
        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted
            for claim in guarded.claims
        }

        responsibility_drafts = tuple(
            self._assess_responsibility(item, self._match(records, item.markers))
            for item in APPLICATION_RESPONSIBILITIES
        )

        ecosystem_drafts: list[MaturityDomainDraft] = []
        release_drafts: list[MaturityDomainDraft] = []
        gaps: list[GapEntry] = []
        for item in (*ECOSYSTEM_CHECKLIST, *RELEASE_CHECKLIST):
            matched = self._match(records, item.markers)
            draft, gap = self._assess_maturity_item(item, matched, present_permitted)
            if item.group is ResponsibilityGroup.ECOSYSTEM:
                ecosystem_drafts.append(draft)
            else:
                release_drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        return ApplicationEcosystemReleaseEvaluation(
            responsibility_drafts=responsibility_drafts,
            ecosystem_drafts=tuple(ecosystem_drafts),
            release_drafts=tuple(release_drafts),
            gaps=tuple(gaps),
        )

    @staticmethod
    def _match(
        records: tuple[EvidenceRecord, ...], markers: tuple[str, ...]
    ) -> tuple[EvidenceRecord, ...]:
        return tuple(
            record for record in records if _contains(_record_text(record), markers)
        )

    # -- application ownership (Requirement 11.1-11.3, Property 19) ------- #

    def _assess_responsibility(
        self,
        item: ApplicationResponsibilityItem,
        matched: tuple[EvidenceRecord, ...],
    ) -> ApplicationResponsibilityDraft:
        ownership, from_evidence = self._determine_ownership(item, matched)
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)
        limitations = self._responsibility_limitations(item, matched)
        return ApplicationResponsibilityDraft(
            responsibility_id=reference(item.responsibility_id),
            name=item.name,
            ownership=ownership,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            limitations=limitations,
            security_sensitive=item.security_sensitive,
            owner_from_evidence=from_evidence,
        )

    @staticmethod
    def _determine_ownership(
        item: ApplicationResponsibilityItem,
        matched: tuple[EvidenceRecord, ...],
    ) -> tuple[Ownership, bool]:
        """Assign exactly one owner (Requirement 11.2 / Property 19).

        Precedence guarantees a single deterministic owner regardless of input
        order:

        1. If the matched records that carry an explicit ``scope.ownership`` all
           agree on a single owner, use it.
        2. Otherwise, if the matched records' text markers all resolve to a
           single owner, use it.
        3. Otherwise fall back to the responsibility's declared default owner.

        Conflicting or absent evidence never yields more than one owner; it
        deterministically falls back to the declared default.
        """

        declared = {
            record.scope.ownership
            for record in matched
            if record.scope.ownership is not None
        }
        if len(declared) == 1:
            return next(iter(declared)), True

        text_owners = {
            owner
            for owner in (_text_ownership(record) for record in matched)
            if owner is not None
        }
        if len(text_owners) == 1:
            return next(iter(text_owners)), True

        return item.default_ownership, False

    @staticmethod
    def _responsibility_limitations(
        item: ApplicationResponsibilityItem,
        matched: tuple[EvidenceRecord, ...],
    ) -> tuple[str, ...]:
        limitations: set[str] = set()
        for record in matched:
            for limitation in record.limitations:
                limitations.add(limitation)
        return tuple(sorted(limitations))

    # -- ecosystem / release maturity (Requirement 11.4-11.6) ------------ #

    def _assess_maturity_item(
        self,
        item: MaturityChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> tuple[MaturityDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)

        scoped_records = tuple(record for record in matched if _is_scoped_release(record))
        scoped_release_only = bool(matched) and len(scoped_records) == len(matched)

        # A capability is satisfied only with direct current implementation
        # evidence that the Claim Guard permits in present tense.
        satisfied = any(
            _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in matched
        )

        # Requirement 11.6 / Property 9: scoped compiler/tooling or Linux backend
        # SDK release evidence can never raise OS-substrate maturity. Because
        # every capability here targets a hosted/T1 owner area, we simply record
        # that any scoped-release evidence is blocked from promoting substrate.
        substrate_promotion_blocked = any(_is_scoped_release(record) for record in matched)

        limitations = self._maturity_limitations(item, matched, scoped_release_only)

        gap: GapEntry | None = None
        gap_ids: tuple[ReferenceId, ...] = ()
        if not satisfied:
            gap = self._build_gap(item, matched, observed_status, limitations)
            gap_ids = (reference(gap.id),)

        parent = (
            _PARENT_T0
            if item.target_level is TargetLevel.T0_HOSTED_ADJACENCY
            else _PARENT_T1
        )
        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} "
                f"{item.group.value} capability assessed by the application "
                "ownership/ecosystem/release evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(parent),
            evidence_ids=supporting_ids,
            gap_ids=gap_ids,
        )

        draft = MaturityDomainDraft(
            domain=domain,
            group=item.group,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            limitations=limitations,
            satisfied=satisfied,
            security_sensitive=item.security_sensitive,
            scoped_release_only=scoped_release_only,
            substrate_promotion_blocked=substrate_promotion_blocked,
            gap_id=reference(gap.id) if gap is not None else None,
        )
        return draft, gap

    @staticmethod
    def _maturity_limitations(
        item: MaturityChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        scoped_release_only: bool,
    ) -> tuple[str, ...]:
        limitations: set[str] = set(item.non_claims)
        if scoped_release_only:
            limitations.add(
                "Release evidence is scoped to compiler/tooling or Linux backend "
                "SDK scope and cannot raise OS-substrate maturity."
            )
        for record in matched:
            for limitation in record.limitations:
                limitations.add(limitation)
        return tuple(sorted(limitations))

    def _build_gap(
        self,
        item: MaturityChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
        limitations: tuple[str, ...],
    ) -> GapEntry:
        primary = item.unsatisfied_category
        secondary: list[GapCategory] = []
        preview_present = any(record.status in _PREVIEW_STATUSES for record in matched)
        if item.security_sensitive and preview_present and primary is not GapCategory.ECOSYSTEM:
            secondary.append(GapCategory.ECOSYSTEM)

        acceptance = tuple(item.acceptance_evidence)
        if limitations:
            acceptance = acceptance + (
                "Disclosed limitations must remain recorded: " + "; ".join(limitations),
            )

        observed_fact = (
            f"{item.name}: no direct current implementation evidence "
            f"(strongest observed status {observed_status.value}); "
            f"authoritative source {item.authoritative_paths[0]}."
        )
        recommendation = (
            f"Provide direct implementation/execution evidence for {item.name}; "
            "scoped compiler/tooling or Linux backend SDK release evidence cannot "
            "raise OS-substrate maturity."
        )

        return GapEntry(
            id=stable_id("gap", "application", str(item.capability_id)),
            title=f"{item.name} gap",
            primary_category=primary,
            secondary_categories=tuple(secondary),
            domain_ids=(reference(item.capability_id),),
            current_status=observed_status,
            target_level=item.target_level,
            severity=self._severity(item),
            dependencies=(),
            acceptance_evidence=acceptance,
            recommended_owner_area=item.owner_area,
            dependency_criticality=1,
            safety_impact=1 if item.security_sensitive else 0,
            claim_risk=1 if item.security_sensitive else 0,
            target_unblock_value=1,
            observed_fact=observed_fact,
            recommendation=recommendation,
        )

    @staticmethod
    def _severity(item: MaturityChecklistItem) -> Severity:
        if item.security_sensitive:
            return Severity.HIGH
        if item.group is ResponsibilityGroup.RELEASE:
            return Severity.MEDIUM
        return Severity.MEDIUM


def evaluate_application_ecosystem_release(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> ApplicationEcosystemReleaseEvaluation:
    """Convenience API for the application ownership/ecosystem/release evaluator."""

    return ApplicationEcosystemReleaseEvaluator().evaluate(bundle, guarded)
