"""Runtime, standard-library, layering, and package-system evaluator (Task 6.2).

This declarative evaluator covers Requirement 8 (runtime, standard library, and
package-system gaps). It builds on the Task 4.1 :class:`EvidenceBundle` and the
Task 4.3 Claim Guard: the guard tells the evaluator which claims may be stated in
the present tense (so hosted/planned evidence cannot masquerade as a current
freestanding implementation), while the bundle supplies the full claim text and
disclosed ``limitations`` surfaced onto the gaps.

The evaluator is intentionally self-contained. Sibling evaluator tasks (ABI/
backend 6.1, boot 6.3, kernel/userspace 7.x) run in parallel and may introduce a
shared base later; this module defines only module-local draft/result types so
the evaluators can coexist without fighting over a base class. Nothing here
mutates evidence or edits sibling evaluator files.

Requirement 8 coverage:

* **8.1 runtime.** Hosted runtime services are distinguished from freestanding
  startup, static initialization, panic behavior, allocation hooks, termination,
  unwinding, exception policy, and runtime ABI. A freestanding runtime aspect is
  satisfied only by a *current freestanding* implementation; hosted runtime
  evidence never satisfies a freestanding aspect. Per the design, the CLI no-std
  rejection and the primitive object path are **not** a freestanding runtime.
* **8.2 std.** The current ``std`` is assessed along six dimensions: API
  coverage, host dependency, allocation behavior, platform coverage, stability,
  and verification. Because ``std`` is hosted, its assessment always surfaces a
  semantic/verification gap for freestanding readiness.
* **8.3 layering.** Future ``core``, hosted ``std``, and future ``system`` are
  three *separate* capability domains and never collapse into one (Property 14).
* **8.4 imports.** For a ``core::`` or ``system::`` import, if either resolver
  support or implementation support is absent the import is classified
  ``Planned`` (see :func:`classify_core_system_import`).
* **8.5 packages.** Manifests, workspaces, locks, local registry, hosted registry
  helpers, git dependencies, native dependencies, reproducibility, signing,
  vulnerability response, compatibility, and offline operation are each assessed.
* **8.6 preview preservation.** ``Installed_Preview`` and ``Repo_Preview`` statuses
  survive summaries and target-level calculations unchanged: the evaluator never
  promotes or collapses them (Property 14).
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
    Severity,
    TargetLevel,
)

# --------------------------------------------------------------------------- #
# Parent (T-level umbrella) capabilities from ``catalog.CAPABILITY_DEFINITIONS``
# that these fine-grained domains roll up into, for later reconciliation.
# --------------------------------------------------------------------------- #
_PARENT_T1 = StableId("capability-t1-language-platform")
_PARENT_T2 = StableId("capability-t2-freestanding-substrate")


# --------------------------------------------------------------------------- #
# Marker vocabularies                                                         #
# --------------------------------------------------------------------------- #
#
# Detection scans the lower-cased ``claim_key`` + ``claim`` text of each record.
# Markers are plain substrings so detection is deterministic and order
# independent.

# A record only counts as a *freestanding* implementation when it explicitly
# names a freestanding/no-std/bare-metal scope. This keeps hosted runtime
# evidence from satisfying a freestanding runtime aspect (Requirement 8.1).
_FREESTANDING_MARKERS: tuple[str, ...] = (
    "freestanding",
    "free-standing",
    "no-std",
    "no_std",
    "nostd",
    "bare-metal",
    "bare metal",
)

# A record that names a hosted runtime / host OS / hosted std scope.
_HOSTED_MARKERS: tuple[str, ...] = (
    "hosted",
    "host os",
    "host operating system",
    "c++ standard library",
    "libc++",
    "libstdc++",
    "runtime header",
)

# Verification/stability markers used by the std assessment (Requirement 8.2).
_VERIFICATION_MARKERS: tuple[str, ...] = (
    "stability policy",
    "compatibility policy",
    "cross-platform",
    "cross platform",
    "verified",
    "verification",
    "conformance",
)


class LibraryLayer(ClosedStrEnum):
    """The three separated library layers kept distinct by Requirement 8.3."""

    #: Future freestanding ``core`` layer.
    FUTURE_CORE = "FutureCore"
    #: Current hosted ``std`` layer.
    HOSTED_STD = "HostedStd"
    #: Future ``system`` layer.
    FUTURE_SYSTEM = "FutureSystem"


class RuntimeAspect(ClosedStrEnum):
    """Runtime aspects assessed by Requirement 8.1."""

    HOSTED_RUNTIME_SERVICES = "HostedRuntimeServices"
    FREESTANDING_STARTUP = "FreestandingStartup"
    STATIC_INITIALIZATION = "StaticInitialization"
    PANIC_BEHAVIOR = "PanicBehavior"
    ALLOCATION_HOOKS = "AllocationHooks"
    TERMINATION = "Termination"
    UNWINDING = "Unwinding"
    EXCEPTION_POLICY = "ExceptionPolicy"
    RUNTIME_ABI = "RuntimeAbi"


class StdAssessmentDimension(ClosedStrEnum):
    """The six ``std`` assessment dimensions of Requirement 8.2."""

    API_COVERAGE = "ApiCoverage"
    HOST_DEPENDENCY = "HostDependency"
    ALLOCATION_BEHAVIOR = "AllocationBehavior"
    PLATFORM_COVERAGE = "PlatformCoverage"
    STABILITY = "Stability"
    VERIFICATION = "Verification"


class PackageFacet(ClosedStrEnum):
    """Package-system facets assessed by Requirement 8.5."""

    MANIFEST = "Manifest"
    WORKSPACE = "Workspace"
    LOCK = "Lock"
    LOCAL_REGISTRY = "LocalRegistry"
    HOSTED_REGISTRY = "HostedRegistry"
    GIT_DEPENDENCY = "GitDependency"
    NATIVE_DEPENDENCY = "NativeDependency"
    REPRODUCIBILITY = "Reproducibility"
    SIGNING = "Signing"
    VULNERABILITY_RESPONSE = "VulnerabilityResponse"
    COMPATIBILITY = "Compatibility"
    OFFLINE_OPERATION = "OfflineOperation"


class CapabilityKind(Enum):
    """The classification rule a checklist item applies."""

    #: Satisfied by a *current hosted* implementation (hosted runtime, hosted std).
    HOSTED_PRESENT = "hosted_present"
    #: Satisfied only by a *current freestanding* implementation.
    FREESTANDING_IMPL = "freestanding_impl"
    #: Always drafted; produces a verification gap unless verification exists.
    ASSESSMENT = "assessment"
    #: A future freestanding layer (core/system); Planned until implemented.
    FUTURE_LAYER = "future_layer"
    #: A package-system facet; satisfied only by a current implementation.
    PACKAGE_FACET = "package_facet"


@dataclass(frozen=True, slots=True)
class RuntimeLibraryChecklistItem:
    """One declarative capability check for runtime/library/package assessment."""

    capability_id: StableId
    name: str
    target_level: TargetLevel
    parent_capability_id: StableId
    kind: CapabilityKind
    markers: tuple[str, ...]
    authoritative_paths: tuple[str, ...]
    unsatisfied_category: GapCategory
    requirement_refs: tuple[str, ...]
    acceptance_evidence: tuple[str, ...]
    non_claims: tuple[str, ...]
    owner_area: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "capability_id", StableId(self.capability_id))
        object.__setattr__(self, "parent_capability_id", StableId(self.parent_capability_id))
        if not self.name.strip():
            raise ValueError("checklist name must not be empty")
        if not isinstance(self.target_level, TargetLevel):
            raise TypeError("target_level must be a TargetLevel")
        if not isinstance(self.kind, CapabilityKind):
            raise TypeError("kind must be a CapabilityKind")
        if not isinstance(self.unsatisfied_category, GapCategory):
            raise TypeError("unsatisfied_category must be a GapCategory")
        for name in ("markers", "authoritative_paths", "requirement_refs", "acceptance_evidence"):
            value = tuple(getattr(self, name))
            if not value:
                raise ValueError(f"{name} must not be empty")
            object.__setattr__(self, name, value)
        object.__setattr__(self, "non_claims", tuple(self.non_claims))
        if not self.owner_area.strip():
            raise ValueError("owner_area must not be empty")


@dataclass(frozen=True, slots=True, kw_only=True)
class RuntimeLibraryDomainDraft:
    """A per-capability draft: domain, observed evidence, and classification."""

    domain: CapabilityDomain
    observed_status: EvidenceStatus
    supporting_evidence_ids: tuple[ReferenceId, ...]
    preview_statuses: tuple[EvidenceStatus, ...]
    limitations: tuple[str, ...]
    satisfied: bool
    gap_id: ReferenceId | None

    def __post_init__(self) -> None:
        if not isinstance(self.domain, CapabilityDomain):
            raise TypeError("domain must be a CapabilityDomain")
        object.__setattr__(
            self,
            "supporting_evidence_ids",
            tuple(sorted({reference(value) for value in self.supporting_evidence_ids}, key=str)),
        )
        object.__setattr__(self, "preview_statuses", tuple(self.preview_statuses))
        object.__setattr__(self, "limitations", tuple(self.limitations))
        if not isinstance(self.satisfied, bool):
            raise TypeError("satisfied must be a bool")
        if self.gap_id is not None:
            object.__setattr__(self, "gap_id", reference(self.gap_id))


@dataclass(frozen=True, slots=True)
class RuntimeLibraryEvaluation:
    """Evaluator output: domain drafts, gaps, and preserved preview statuses."""

    domain_drafts: tuple[RuntimeLibraryDomainDraft, ...]
    gaps: tuple[GapEntry, ...]
    preserved_preview_statuses: tuple[EvidenceStatus, ...]

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "domain_drafts",
            tuple(sorted(self.domain_drafts, key=lambda draft: str(draft.domain.id))),
        )
        object.__setattr__(self, "gaps", tuple(sorted(self.gaps, key=lambda gap: str(gap.id))))
        object.__setattr__(
            self,
            "preserved_preview_statuses",
            tuple(sorted(set(self.preserved_preview_statuses), key=lambda status: status.value)),
        )

    def draft_for(self, capability_id: str) -> RuntimeLibraryDomainDraft | None:
        target = str(capability_id)
        for draft in self.domain_drafts:
            if str(draft.domain.id) == target:
                return draft
        return None

    def gap_for(self, capability_id: str) -> GapEntry | None:
        target = str(capability_id)
        for gap in self.gaps:
            if target in {str(ref) for ref in gap.domain_ids}:
                return gap
        return None

    def layer_domain_ids(self) -> Mapping[LibraryLayer, str]:
        """Return the (distinct) domain IDs of the three library layers."""

        return {
            LibraryLayer.FUTURE_CORE: str(_LAYER_CAPABILITY_IDS[LibraryLayer.FUTURE_CORE]),
            LibraryLayer.HOSTED_STD: str(_LAYER_CAPABILITY_IDS[LibraryLayer.HOSTED_STD]),
            LibraryLayer.FUTURE_SYSTEM: str(_LAYER_CAPABILITY_IDS[LibraryLayer.FUTURE_SYSTEM]),
        }


# --------------------------------------------------------------------------- #
# Requirement 8.4: core::/system:: import classification                       #
# --------------------------------------------------------------------------- #

_CORE_SYSTEM_PREFIXES: tuple[str, ...] = ("core::", "system::")


def classify_core_system_import(
    module_path: str,
    *,
    resolver_support: bool,
    implementation_support: bool,
) -> EvidenceStatus:
    """Classify a ``core::`` or ``system::`` import (Requirement 8.4).

    If either resolver support or implementation support is absent, the import is
    ``Planned``. Only when *both* are present is the import treated as a current
    (experimental) implementation. Imports outside the ``core::``/``system::``
    layers are rejected because this rule is scoped to the freestanding layers.
    """

    if not isinstance(module_path, str) or not module_path.strip():
        raise ValueError("module_path must be a non-empty string")
    normalized = module_path.strip().lower()
    if not any(normalized.startswith(prefix) for prefix in _CORE_SYSTEM_PREFIXES):
        raise ValueError(
            "classify_core_system_import applies only to core:: or system:: imports"
        )
    for name, value in (
        ("resolver_support", resolver_support),
        ("implementation_support", implementation_support),
    ):
        if not isinstance(value, bool):
            raise TypeError(f"{name} must be a bool")
    if not (resolver_support and implementation_support):
        return EvidenceStatus.PLANNED
    return EvidenceStatus.EXPERIMENTAL


# --------------------------------------------------------------------------- #
# Declarative checklist                                                        #
# --------------------------------------------------------------------------- #

_LAYER_CAPABILITY_IDS: Mapping[LibraryLayer, StableId] = {
    LibraryLayer.FUTURE_CORE: StableId("capability-library-layer-future-core"),
    LibraryLayer.HOSTED_STD: StableId("capability-library-layer-hosted-std"),
    LibraryLayer.FUTURE_SYSTEM: StableId("capability-library-layer-future-system"),
}


def _runtime_item(
    aspect: RuntimeAspect,
    capability_slug: str,
    name: str,
    kind: CapabilityKind,
    target_level: TargetLevel,
    parent: StableId,
    markers: tuple[str, ...],
    acceptance: str,
    non_claim: str,
) -> RuntimeLibraryChecklistItem:
    return RuntimeLibraryChecklistItem(
        capability_id=StableId(f"capability-runtime-{capability_slug}"),
        name=name,
        target_level=target_level,
        parent_capability_id=parent,
        kind=kind,
        markers=markers,
        authoritative_paths=(
            "docs/universeos/no_std_runtime.md",
            "spec/library_layers.md",
            "runtime/nebula_runtime.hpp",
        ),
        unsatisfied_category=(
            GapCategory.IMPLEMENTATION
            if kind is not CapabilityKind.HOSTED_PRESENT
            else GapCategory.IMPLEMENTATION
        ),
        requirement_refs=("8.1",),
        acceptance_evidence=(acceptance,),
        non_claims=(non_claim,),
        owner_area="Freestanding Runtime",
    )


_RUNTIME_ITEMS: tuple[RuntimeLibraryChecklistItem, ...] = (
    _runtime_item(
        RuntimeAspect.HOSTED_RUNTIME_SERVICES,
        "hosted-services",
        "Hosted runtime services",
        CapabilityKind.HOSTED_PRESENT,
        TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        _PARENT_T1,
        ("hosted runtime", "runtime service", "runtime header", "host runtime"),
        "Current hosted runtime services implemented on the host OS and C++ runtime.",
        "Hosted runtime services depend on the host OS and are not a freestanding runtime.",
    ),
    _runtime_item(
        RuntimeAspect.FREESTANDING_STARTUP,
        "freestanding-startup",
        "Freestanding startup sequence",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("startup", "start-up", "crt0", "_start", "entry point"),
        "A freestanding startup sequence that runs without a hosted runtime.",
        "There is no freestanding startup runtime; the CLI no-std rejection and the "
        "primitive object path are not a runtime.",
    ),
    _runtime_item(
        RuntimeAspect.STATIC_INITIALIZATION,
        "static-init",
        "Freestanding static initialization",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("static init", "static initialization", "global constructor", "ctors", "init array"),
        "A freestanding static-initialization order and mechanism with implementation evidence.",
        "Freestanding static initialization is unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.PANIC_BEHAVIOR,
        "panic",
        "Freestanding panic behavior",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("panic", "abort handler", "panic handler"),
        "A freestanding panic policy and handler with implementation evidence.",
        "Freestanding panic behavior is unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.ALLOCATION_HOOKS,
        "allocation-hooks",
        "Freestanding allocation hooks",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("allocation hook", "allocator hook", "global allocator", "alloc hook", "allocation policy"),
        "Freestanding allocation hooks and an allocator-failure policy with implementation evidence.",
        "Freestanding allocation hooks are unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.TERMINATION,
        "termination",
        "Freestanding termination",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("termination", "atexit", "exit handler", "process exit", "shutdown sequence"),
        "A freestanding termination path with implementation evidence.",
        "Freestanding termination is unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.UNWINDING,
        "unwinding",
        "Freestanding unwinding",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("unwind", "unwinding", "stack unwinding", "landing pad"),
        "A freestanding stack-unwinding mechanism with implementation evidence.",
        "Freestanding unwinding is unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.EXCEPTION_POLICY,
        "exception-policy",
        "Freestanding exception policy",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("exception policy", "exception handling", "throw", "exception model"),
        "A freestanding exception policy with implementation evidence.",
        "Freestanding exception policy is unspecified and unimplemented.",
    ),
    _runtime_item(
        RuntimeAspect.RUNTIME_ABI,
        "runtime-abi",
        "Freestanding runtime ABI",
        CapabilityKind.FREESTANDING_IMPL,
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        _PARENT_T2,
        ("runtime abi", "runtime interface abi", "freestanding abi"),
        "A freestanding runtime ABI contract with implementation evidence.",
        "A freestanding runtime ABI is unspecified and unimplemented.",
    ),
)

_STD_ITEM = RuntimeLibraryChecklistItem(
    capability_id=StableId("capability-std-library-assessment"),
    name="Current std library assessment",
    target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
    parent_capability_id=_PARENT_T1,
    kind=CapabilityKind.ASSESSMENT,
    markers=("std module", "standard library", "std::", "bundled std", "std library"),
    authoritative_paths=("spec/library_layers.md", "std/"),
    unsatisfied_category=GapCategory.VERIFICATION,
    requirement_refs=("8.2",),
    acceptance_evidence=(
        "A std assessment covering API coverage, host dependency, allocation "
        "behavior, platform coverage, stability, and verification, with a "
        "compatibility/stability policy and cross-platform verification evidence.",
    ),
    non_claims=(
        "The current std is hosted: it depends on the host OS and C++ standard "
        "library and is not verified as a freestanding, cross-platform stable API.",
    ),
    owner_area="Standard Library",
)

_LAYER_ITEMS: tuple[RuntimeLibraryChecklistItem, ...] = (
    RuntimeLibraryChecklistItem(
        capability_id=_LAYER_CAPABILITY_IDS[LibraryLayer.FUTURE_CORE],
        name="Future core library layer",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=CapabilityKind.FUTURE_LAYER,
        markers=("core::", "future core", "core layer", "freestanding core", "`core`"),
        authoritative_paths=("spec/library_layers.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("8.3", "8.4"),
        acceptance_evidence=(
            "A freestanding core library with resolver and implementation support "
            "and direct implementation evidence.",
        ),
        non_claims=(
            "The future core layer is planned; core:: imports without resolver and "
            "implementation support are Planned.",
        ),
        owner_area="Freestanding Runtime",
    ),
    RuntimeLibraryChecklistItem(
        capability_id=_LAYER_CAPABILITY_IDS[LibraryLayer.HOSTED_STD],
        name="Hosted std library layer",
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.HOSTED_PRESENT,
        markers=("hosted std", "std layer", "bundled std", "hosted standard library"),
        authoritative_paths=("spec/library_layers.md", "std/"),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("8.3",),
        acceptance_evidence=(
            "A current hosted std layer implemented on the host OS with direct "
            "implementation evidence.",
        ),
        non_claims=(
            "The hosted std layer is a separate domain from future core and future "
            "system and cannot promote freestanding maturity.",
        ),
        owner_area="Standard Library",
    ),
    RuntimeLibraryChecklistItem(
        capability_id=_LAYER_CAPABILITY_IDS[LibraryLayer.FUTURE_SYSTEM],
        name="Future system library layer",
        target_level=TargetLevel.T2_FREESTANDING_SUBSTRATE,
        parent_capability_id=_PARENT_T2,
        kind=CapabilityKind.FUTURE_LAYER,
        markers=("system::", "future system", "system layer", "`system`"),
        authoritative_paths=("spec/library_layers.md",),
        unsatisfied_category=GapCategory.IMPLEMENTATION,
        requirement_refs=("8.3", "8.4"),
        acceptance_evidence=(
            "A freestanding system library with resolver and implementation support "
            "and direct implementation evidence.",
        ),
        non_claims=(
            "The future system layer is planned; system:: imports without resolver "
            "and implementation support are Planned.",
        ),
        owner_area="Freestanding Runtime",
    ),
)


def _package_item(
    facet: PackageFacet,
    slug: str,
    name: str,
    markers: tuple[str, ...],
    unsatisfied_category: GapCategory,
) -> RuntimeLibraryChecklistItem:
    return RuntimeLibraryChecklistItem(
        capability_id=StableId(f"capability-package-{slug}"),
        name=name,
        target_level=TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
        parent_capability_id=_PARENT_T1,
        kind=CapabilityKind.PACKAGE_FACET,
        markers=markers,
        authoritative_paths=("docs/official_package_tiering.md", "spec/library_layers.md"),
        unsatisfied_category=unsatisfied_category,
        requirement_refs=("8.5",),
        acceptance_evidence=(
            f"Implemented {name.lower()} support with direct implementation evidence.",
        ),
        non_claims=(f"{name} has no accepted implementation evidence.",),
        owner_area="Package & Ecosystem",
    )


_PACKAGE_ITEMS: tuple[RuntimeLibraryChecklistItem, ...] = (
    _package_item(
        PackageFacet.MANIFEST, "manifest", "Package manifest",
        ("manifest", "nebula.toml", "package manifest"), GapCategory.IMPLEMENTATION,
    ),
    _package_item(
        PackageFacet.WORKSPACE, "workspace", "Workspace support",
        ("workspace",), GapCategory.IMPLEMENTATION,
    ),
    _package_item(
        PackageFacet.LOCK, "lock", "Lock file",
        ("lockfile", "lock file", "nebula.lock", "lock:"), GapCategory.IMPLEMENTATION,
    ),
    _package_item(
        PackageFacet.LOCAL_REGISTRY, "local-registry", "Local registry",
        ("local registry", "offline registry", "vendored registry"), GapCategory.ECOSYSTEM,
    ),
    _package_item(
        PackageFacet.HOSTED_REGISTRY, "hosted-registry", "Hosted registry helpers",
        ("hosted registry", "remote registry", "registry helper"), GapCategory.ECOSYSTEM,
    ),
    _package_item(
        PackageFacet.GIT_DEPENDENCY, "git-dependency", "Git dependencies",
        ("git dependency", "git dependencies", "git+", "git source"), GapCategory.IMPLEMENTATION,
    ),
    _package_item(
        PackageFacet.NATIVE_DEPENDENCY, "native-dependency", "Native dependencies",
        ("native dependency", "native dependencies", "system library dependency", "native library"),
        GapCategory.IMPLEMENTATION,
    ),
    _package_item(
        PackageFacet.REPRODUCIBILITY, "reproducibility", "Reproducible builds",
        ("reproducib", "deterministic build", "reproducible build"), GapCategory.VERIFICATION,
    ),
    _package_item(
        PackageFacet.SIGNING, "signing", "Package signing",
        ("signing", "signature", "signed package", "provenance signature"), GapCategory.ECOSYSTEM,
    ),
    _package_item(
        PackageFacet.VULNERABILITY_RESPONSE, "vulnerability-response", "Vulnerability response",
        ("vulnerability", "advisory", "security response", "cve"), GapCategory.ECOSYSTEM,
    ),
    _package_item(
        PackageFacet.COMPATIBILITY, "compatibility", "Package compatibility governance",
        ("package compatibility", "semver", "version compatibility", "compatibility governance"),
        GapCategory.ECOSYSTEM,
    ),
    _package_item(
        PackageFacet.OFFLINE_OPERATION, "offline", "Offline operation",
        ("offline operation", "offline mode", "offline install", "air-gapped", "air gapped"),
        GapCategory.ECOSYSTEM,
    ),
)


RUNTIME_LIBRARY_PACKAGE_CHECKLIST: tuple[RuntimeLibraryChecklistItem, ...] = (
    *_RUNTIME_ITEMS,
    _STD_ITEM,
    *_LAYER_ITEMS,
    *_PACKAGE_ITEMS,
)


# --------------------------------------------------------------------------- #
# Status handling                                                             #
# --------------------------------------------------------------------------- #

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

# Preview statuses that must survive summaries unchanged (Requirement 8.6).
_PREVIEW_STATUSES: frozenset[EvidenceStatus] = frozenset(
    {EvidenceStatus.INSTALLED_PREVIEW, EvidenceStatus.REPO_PREVIEW}
)

# Evidence kinds that count as direct implementation for a "current" model.
_DIRECT_IMPLEMENTATION_KINDS: frozenset[EvidenceKind] = frozenset(
    {EvidenceKind.SOURCE, EvidenceKind.TEST_EXECUTION, EvidenceKind.ARTIFACT}
)

# Ordinal ranking of statuses, strongest first, for "strongest observed status".
# Note: this only ever returns a status that is literally present among matched
# records; it never invents or promotes a higher status, so preview statuses are
# preserved unchanged (Requirement 8.6 / Property 14).
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


class RuntimeLibraryPackageEvaluator:
    """Evaluate Requirement 8 runtime, library-layer, and package-system gaps."""

    def evaluate(
        self,
        bundle: EvidenceBundle,
        guarded: GuardedEvidence | None = None,
    ) -> RuntimeLibraryEvaluation:
        if not isinstance(bundle, EvidenceBundle):
            raise TypeError("bundle must be an EvidenceBundle")
        if guarded is None:
            guarded = guard_evidence(bundle)
        if not isinstance(guarded, GuardedEvidence):
            raise TypeError("guarded must be a GuardedEvidence")

        records = bundle.records
        present_permitted = {
            str(claim.evidence_id): claim.present_tense_permitted for claim in guarded.claims
        }

        drafts: list[RuntimeLibraryDomainDraft] = []
        gaps: list[GapEntry] = []
        preserved: set[EvidenceStatus] = set()
        for item in RUNTIME_LIBRARY_PACKAGE_CHECKLIST:
            matched = tuple(
                record for record in records if _contains(_record_text(record), item.markers)
            )
            preserved.update(
                record.status for record in matched if record.status in _PREVIEW_STATUSES
            )
            draft, gap = self._assess_item(item, matched, present_permitted)
            drafts.append(draft)
            if gap is not None:
                gaps.append(gap)

        return RuntimeLibraryEvaluation(
            domain_drafts=tuple(drafts),
            gaps=tuple(gaps),
            preserved_preview_statuses=tuple(preserved),
        )

    # -- per-capability assessment --------------------------------------- #

    def _assess_item(
        self,
        item: RuntimeLibraryChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> tuple[RuntimeLibraryDomainDraft, GapEntry | None]:
        observed_status = _strongest_status(matched)
        supporting_ids = tuple(reference(record.id) for record in matched)
        preview_statuses = tuple(
            sorted(
                {record.status for record in matched if record.status in _PREVIEW_STATUSES},
                key=lambda status: status.value,
            )
        )
        satisfied = self._is_satisfied(item, matched, present_permitted)

        domain = CapabilityDomain(
            id=item.capability_id,
            name=item.name,
            target_level=item.target_level,
            description=(
                f"Requirement {', '.join(item.requirement_refs)} capability assessed by "
                "the runtime/library/package evaluator."
            ),
            mandatory_for_target=True,
            parent_id=reference(item.parent_capability_id),
            evidence_ids=supporting_ids,
        )

        gap: GapEntry | None = None
        if not satisfied:
            gap = self._build_gap(item, matched, observed_status)
            domain = CapabilityDomain(
                id=domain.id,
                name=domain.name,
                target_level=domain.target_level,
                description=domain.description,
                mandatory_for_target=domain.mandatory_for_target,
                parent_id=item.parent_capability_id,
                evidence_ids=supporting_ids,
                gap_ids=(reference(gap.id),),
            )

        draft = RuntimeLibraryDomainDraft(
            domain=domain,
            observed_status=observed_status,
            supporting_evidence_ids=supporting_ids,
            preview_statuses=preview_statuses,
            limitations=tuple(item.non_claims),
            satisfied=satisfied,
            gap_id=reference(gap.id) if gap is not None else None,
        )
        return draft, gap

    def _is_satisfied(
        self,
        item: RuntimeLibraryChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        present_permitted: Mapping[str, bool],
    ) -> bool:
        if item.kind is CapabilityKind.ASSESSMENT:
            # Requirement 8.2: std is only "satisfied" once it has cross-platform
            # verification / stability-policy evidence. Since the current std is
            # hosted, this remains a verification gap until such evidence exists.
            return any(
                _contains(_record_text(record), _VERIFICATION_MARKERS)
                and _has_current_implementation(record)
                and present_permitted.get(str(record.id), False)
                for record in matched
            )
        if item.kind is CapabilityKind.HOSTED_PRESENT:
            # Satisfied by a current hosted implementation.
            return any(
                _has_current_implementation(record)
                and present_permitted.get(str(record.id), False)
                and _contains(_record_text(record), _HOSTED_MARKERS)
                for record in matched
            )
        # FREESTANDING_IMPL / FUTURE_LAYER: satisfied only by a current
        # *freestanding* implementation. Hosted evidence never satisfies these.
        if item.kind in (CapabilityKind.FREESTANDING_IMPL, CapabilityKind.FUTURE_LAYER):
            return any(
                _has_current_implementation(record)
                and present_permitted.get(str(record.id), False)
                and _contains(_record_text(record), _FREESTANDING_MARKERS)
                for record in matched
            )
        # PACKAGE_FACET: satisfied by any current implementation of the facet.
        return any(
            _has_current_implementation(record)
            and present_permitted.get(str(record.id), False)
            for record in matched
        )

    def _build_gap(
        self,
        item: RuntimeLibraryChecklistItem,
        matched: tuple[EvidenceRecord, ...],
        observed_status: EvidenceStatus,
    ) -> GapEntry:
        # Future core/system layers with no freestanding implementation are
        # Planned (Requirement 8.3/8.4 direction); other unsatisfied domains keep
        # their strongest observed status (preserving preview statuses unchanged).
        if item.kind is CapabilityKind.FUTURE_LAYER and observed_status in (
            EvidenceStatus.UNKNOWN,
            EvidenceStatus.PLANNED,
        ):
            current_status = EvidenceStatus.PLANNED
        else:
            current_status = observed_status

        limitations: set[str] = set(item.non_claims)
        # Surface any disclosed limitations from matched records onto the gap.
        for record in matched:
            limitations.update(record.limitations)

        acceptance = tuple(sorted(set(item.acceptance_evidence) | limitations)) or item.acceptance_evidence

        severity = (
            Severity.HIGH
            if item.target_level is TargetLevel.T2_FREESTANDING_SUBSTRATE
            else Severity.MEDIUM
        )
        dependency_criticality = (
            2 if item.target_level is TargetLevel.T2_FREESTANDING_SUBSTRATE else 1
        )

        matched_note = (
            f" (matched evidence: {', '.join(sorted(str(record.id) for record in matched))})"
            if matched
            else " (no matching evidence found)"
        )
        return GapEntry(
            id=stable_id("gap", "runtime-library-package", str(item.capability_id)),
            title=f"Runtime/library/package gap: {item.name}",
            primary_category=item.unsatisfied_category,
            secondary_categories=(),
            domain_ids=(reference(item.capability_id),),
            current_status=current_status,
            target_level=item.target_level,
            severity=severity,
            dependencies=(),
            acceptance_evidence=acceptance,
            recommended_owner_area=item.owner_area,
            dependency_criticality=dependency_criticality,
            safety_impact=0,
            claim_risk=1,
            target_unblock_value=dependency_criticality,
            observed_fact=(
                f"{item.name} has no accepted current implementation satisfying "
                f"Requirement {', '.join(item.requirement_refs)}; strongest observed "
                f"status is {observed_status.value}{matched_note}."
            ),
            recommendation=(
                f"Implement and verify {item.name} before depending on it for "
                "freestanding or ecosystem work."
            ),
        )


def evaluate_runtime_library_package(
    bundle: EvidenceBundle,
    guarded: GuardedEvidence | None = None,
) -> RuntimeLibraryEvaluation:
    """Convenience API for the runtime/library/package evaluator (Task 6.2)."""

    return RuntimeLibraryPackageEvaluator().evaluate(bundle, guarded)
