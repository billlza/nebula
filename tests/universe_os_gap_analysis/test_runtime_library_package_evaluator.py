"""Unit tests for the runtime/library-layer/package evaluator (Task 6.2).

These tests exercise Requirement 8 behaviours against the real evidence and
claim-guard layers (no mocks): hosted vs freestanding runtime aspects, the
six-dimension std assessment, separate core/std/system library layers, the
core::/system:: import classification rule, package-facet coverage, and the
preservation of Installed_Preview/Repo_Preview statuses in summaries.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.runtime_library_package import (
    RUNTIME_LIBRARY_PACKAGE_CHECKLIST,
    CapabilityKind,
    LibraryLayer,
    PackageFacet,
    RuntimeAspect,
    RuntimeLibraryPackageEvaluator,
    classify_core_system_import,
    evaluate_runtime_library_package,
)
from tools.universe_os_gap_analysis.evidence import EvidenceBundle
from tools.universe_os_gap_analysis.identifiers import reference, stable_id
from tools.universe_os_gap_analysis.models import (
    ConfidenceRating,
    EvidenceKind,
    EvidenceRecord,
    EvidenceScope,
    EvidenceStatus,
    GapCategory,
    LocationKind,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-runtime-library-test")

_HOSTED_RUNTIME = "capability-runtime-hosted-services"
_FREESTANDING_STARTUP = "capability-runtime-freestanding-startup"
_PANIC = "capability-runtime-panic"
_RUNTIME_ABI = "capability-runtime-runtime-abi"
_STD = "capability-std-library-assessment"
_CORE_LAYER = "capability-library-layer-future-core"
_STD_LAYER = "capability-library-layer-hosted-std"
_SYSTEM_LAYER = "capability-library-layer-future-system"
_MANIFEST = "capability-package-manifest"
_SIGNING = "capability-package-signing"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "spec/library_layers.md",
    limitations: tuple[str, ...] = (),
    verification_state: VerificationState = VerificationState.NOT_RUN,
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, evidence_kind.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(),
        limitations=limitations,
        trust_assumptions=(),
        verification_state=verification_state,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


class RuntimeAspectTests(unittest.TestCase):
    """Requirement 8.1: hosted vs freestanding runtime aspects."""

    def test_hosted_runtime_evidence_satisfies_hosted_services_only(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:runtime/nebula_runtime.hpp",
                claim="Hosted runtime services run on the host OS and C++ runtime header.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="runtime/nebula_runtime.hpp",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        hosted = result.draft_for(_HOSTED_RUNTIME)
        assert hosted is not None
        self.assertTrue(hosted.satisfied)
        self.assertIsNone(result.gap_for(_HOSTED_RUNTIME))

    def test_hosted_runtime_does_not_satisfy_freestanding_startup(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:runtime/nebula_runtime.hpp",
                claim="Hosted runtime startup services run on the host OS entry point.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="runtime/nebula_runtime.hpp",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        startup = result.draft_for(_FREESTANDING_STARTUP)
        assert startup is not None
        self.assertFalse(startup.satisfied)
        gap = result.gap_for(_FREESTANDING_STARTUP)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)
        self.assertEqual(gap.target_level, TargetLevel.T2_FREESTANDING_SUBSTRATE)

    def test_freestanding_implementation_satisfies_panic(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:runtime/freestanding_panic.cpp",
                claim="A freestanding no-std panic handler aborts without the hosted runtime.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SOURCE,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path="runtime/freestanding_panic.cpp",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        panic = result.draft_for(_PANIC)
        assert panic is not None
        self.assertTrue(panic.satisfied)
        self.assertIsNone(result.gap_for(_PANIC))

    def test_all_runtime_aspects_are_drafted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/library_layers.md",
                claim="Documentation of runtime abi, panic, unwinding and termination.",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        drafted = {str(d.domain.id) for d in result.domain_drafts}
        for aspect_slug in (
            _HOSTED_RUNTIME,
            _FREESTANDING_STARTUP,
            _PANIC,
            _RUNTIME_ABI,
        ):
            self.assertIn(aspect_slug, drafted)


class StdAssessmentTests(unittest.TestCase):
    """Requirement 8.2: std assessed by six dimensions -> verification gap."""

    def test_hosted_std_without_verification_yields_verification_gap(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:std/io.nb",
                claim="The bundled std standard library depends on the host OS.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="std/io.nb",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        std = result.draft_for(_STD)
        assert std is not None
        self.assertFalse(std.satisfied)
        gap = result.gap_for(_STD)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.VERIFICATION)

    def test_std_with_cross_platform_verification_is_satisfied(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="test:std/conformance",
                claim=(
                    "The std standard library has a cross-platform stability policy "
                    "verified by a conformance suite."
                ),
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.TEST_EXECUTION,
                source_path="std/conformance.nb",
                verification_state=VerificationState.VALIDATED,
            )
        )
        result = evaluate_runtime_library_package(bundle)
        std = result.draft_for(_STD)
        assert std is not None
        self.assertTrue(std.satisfied)


class LibraryLayerTests(unittest.TestCase):
    """Requirement 8.3: core/std/system are separate domains."""

    def test_three_layers_are_distinct_domains(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:spec/library_layers.md",
                claim="core::, hosted std, and system:: layers are documented.",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        ids = result.layer_domain_ids()
        self.assertEqual(len({ids[LibraryLayer.FUTURE_CORE],
                              ids[LibraryLayer.HOSTED_STD],
                              ids[LibraryLayer.FUTURE_SYSTEM]}), 3)
        for cap in (_CORE_LAYER, _STD_LAYER, _SYSTEM_LAYER):
            self.assertIsNotNone(result.draft_for(cap))

    def test_future_core_without_implementation_is_planned_gap(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="plan:spec/library_layers.md",
                claim="The future core layer (core::) is planned.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
            )
        )
        result = evaluate_runtime_library_package(bundle)
        gap = result.gap_for(_CORE_LAYER)
        assert gap is not None
        self.assertEqual(gap.current_status, EvidenceStatus.PLANNED)
        self.assertEqual(gap.primary_category, GapCategory.IMPLEMENTATION)


class ImportClassificationTests(unittest.TestCase):
    """Requirement 8.4: core::/system:: import classification."""

    def test_missing_resolver_support_is_planned(self) -> None:
        status = classify_core_system_import(
            "core::mem", resolver_support=False, implementation_support=True
        )
        self.assertEqual(status, EvidenceStatus.PLANNED)

    def test_missing_implementation_support_is_planned(self) -> None:
        status = classify_core_system_import(
            "system::io", resolver_support=True, implementation_support=False
        )
        self.assertEqual(status, EvidenceStatus.PLANNED)

    def test_both_present_is_not_planned(self) -> None:
        status = classify_core_system_import(
            "core::mem", resolver_support=True, implementation_support=True
        )
        self.assertNotEqual(status, EvidenceStatus.PLANNED)

    def test_non_core_system_import_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            classify_core_system_import(
                "std::io", resolver_support=True, implementation_support=True
            )


class PackageFacetTests(unittest.TestCase):
    """Requirement 8.5: package facets are all assessed."""

    def test_all_package_facets_are_drafted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/official_package_tiering.md",
                claim="Documentation of the package system.",
                source_path="docs/official_package_tiering.md",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        drafted = {str(d.domain.id) for d in result.domain_drafts}
        for facet in PackageFacet:
            slug = {
                PackageFacet.MANIFEST: "capability-package-manifest",
                PackageFacet.WORKSPACE: "capability-package-workspace",
                PackageFacet.LOCK: "capability-package-lock",
                PackageFacet.LOCAL_REGISTRY: "capability-package-local-registry",
                PackageFacet.HOSTED_REGISTRY: "capability-package-hosted-registry",
                PackageFacet.GIT_DEPENDENCY: "capability-package-git-dependency",
                PackageFacet.NATIVE_DEPENDENCY: "capability-package-native-dependency",
                PackageFacet.REPRODUCIBILITY: "capability-package-reproducibility",
                PackageFacet.SIGNING: "capability-package-signing",
                PackageFacet.VULNERABILITY_RESPONSE: "capability-package-vulnerability-response",
                PackageFacet.COMPATIBILITY: "capability-package-compatibility",
                PackageFacet.OFFLINE_OPERATION: "capability-package-offline",
            }[facet]
            self.assertIn(slug, drafted)

    def test_implemented_manifest_is_satisfied(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:cli/manifest.cpp",
                claim="The package manifest (nebula.toml) is parsed and validated.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="cli/manifest.cpp",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        manifest = result.draft_for(_MANIFEST)
        assert manifest is not None
        self.assertTrue(manifest.satisfied)

    def test_signing_without_evidence_is_ecosystem_gap(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:docs/official_package_tiering.md",
                claim="Package signing and signature verification are described.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/official_package_tiering.md",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        gap = result.gap_for(_SIGNING)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.ECOSYSTEM)


class PreviewPreservationTests(unittest.TestCase):
    """Requirement 8.6: Installed_Preview/Repo_Preview survive unchanged."""

    def test_repo_preview_status_is_preserved_in_summary(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:official/nebula-observe",
                claim="The hosted registry helper package is a repo preview.",
                status=EvidenceStatus.REPO_PREVIEW,
                source_path="official/nebula-observe/pkg.nb",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        self.assertIn(EvidenceStatus.REPO_PREVIEW, result.preserved_preview_statuses)
        hosted_registry = result.draft_for("capability-package-hosted-registry")
        assert hosted_registry is not None
        self.assertEqual(hosted_registry.observed_status, EvidenceStatus.REPO_PREVIEW)
        self.assertIn(EvidenceStatus.REPO_PREVIEW, hosted_registry.preview_statuses)

    def test_installed_preview_is_not_promoted(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:official/nebula-service",
                claim="The local registry package is an installed preview.",
                status=EvidenceStatus.INSTALLED_PREVIEW,
                source_path="official/nebula-service/pkg.nb",
            )
        )
        result = evaluate_runtime_library_package(bundle)
        local_registry = result.draft_for("capability-package-local-registry")
        assert local_registry is not None
        self.assertEqual(local_registry.observed_status, EvidenceStatus.INSTALLED_PREVIEW)
        # Preview status must not be promoted to a GA status.
        self.assertNotEqual(local_registry.observed_status, EvidenceStatus.COMPILER_TOOLING_GA)


class DeterminismTests(unittest.TestCase):
    def test_evaluation_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="source:runtime/nebula_runtime.hpp",
                claim="Hosted runtime services on the host OS.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="runtime/nebula_runtime.hpp",
            ),
            _record(
                claim_key="source:std/io.nb",
                claim="The bundled std standard library.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="std/io.nb",
            ),
            _record(
                claim_key="plan:spec/library_layers.md",
                claim="The future system:: layer is planned.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
            ),
        )
        forward = evaluate_runtime_library_package(_bundle(*records))
        reverse = evaluate_runtime_library_package(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps], [str(g.id) for g in reverse.gaps]
        )
        self.assertEqual(
            forward.preserved_preview_statuses, reverse.preserved_preview_statuses
        )

    def test_uses_provided_guarded_evidence(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:std/io.nb",
                claim="The bundled std standard library depends on the host OS.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                source_path="std/io.nb",
            )
        )
        guarded = guard_evidence(bundle)
        result = RuntimeLibraryPackageEvaluator().evaluate(bundle, guarded)
        std = result.draft_for(_STD)
        assert std is not None
        self.assertFalse(std.satisfied)

    def test_checklist_capability_ids_are_unique(self) -> None:
        ids = [str(item.capability_id) for item in RUNTIME_LIBRARY_PACKAGE_CHECKLIST]
        self.assertEqual(len(ids), len(set(ids)))


if __name__ == "__main__":
    unittest.main()
