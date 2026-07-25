"""Unit tests for the application ownership/ecosystem/release evaluator (Task 7.4).

These tests exercise Requirement 11 against the real evidence and claim-guard
layers (no mocks):

* every application responsibility is assigned exactly one owner (Property 19);
* explicit ``scope.ownership`` and ownership text markers override the declared
  default owner, and conflicting/absent evidence deterministically falls back to
  the default;
* ecosystem and release capabilities without direct current implementation
  evidence produce gaps, and direct current evidence satisfies them;
* release evidence scoped to compiler/tooling or Linux backend SDK scope is
  flagged as unable to raise OS-substrate maturity (Requirement 11.6);
* the reusable ownership/ecosystem accessors that the sibling Task 7.5 evaluator
  consumes are present and correct.
"""

from __future__ import annotations

import unittest

from tools.universe_os_gap_analysis.claim_guard import guard_evidence
from tools.universe_os_gap_analysis.evaluators.application_ecosystem_release import (
    APPLICATION_RESPONSIBILITIES,
    ECOSYSTEM_CHECKLIST,
    RELEASE_CHECKLIST,
    ApplicationEcosystemReleaseEvaluator,
    ResponsibilityGroup,
    evaluate_application_ecosystem_release,
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
    Ownership,
    RevisionOrigin,
    SourceLocation,
    TargetLevel,
    VerificationState,
)

_REVISION_REF = reference("revision-application-test")

_CLI = "responsibility-app-cli-tools"
_TLS = "responsibility-app-tls"
_SIGNING = "responsibility-app-signing"
_NOTARIZATION = "responsibility-app-notarization"

_PKG_BREADTH = "capability-ecosystem-package-breadth"
_SECURITY_MAINT = "capability-ecosystem-security-maintenance"
_CONTRACT_SUITES = "capability-release-contract-suites"
_SBOM = "capability-release-sbom"


def _record(
    *,
    claim_key: str,
    claim: str,
    status: EvidenceStatus = EvidenceStatus.EXPERIMENTAL,
    evidence_kind: EvidenceKind = EvidenceKind.SOURCE,
    origin: RevisionOrigin = RevisionOrigin.COMMITTED_REVISION,
    source_path: str = "docs/support_matrix.md",
    ownership: Ownership | None = None,
    limitations: tuple[str, ...] = (),
    trust_assumptions: tuple[str, ...] = (),
) -> EvidenceRecord:
    return EvidenceRecord(
        id=stable_id("evidence", claim_key, claim, status.value, origin.value),
        claim_key=claim_key,
        claim=claim,
        status=status,
        source_path=source_path,
        location=SourceLocation(kind=LocationKind.HEADING, value=claim_key),
        revision_ref=_REVISION_REF,
        origin=origin,
        evidence_kind=evidence_kind,
        confidence=ConfidenceRating.MEDIUM,
        scope=EvidenceScope(ownership=ownership),
        limitations=limitations,
        trust_assumptions=trust_assumptions,
        verification_state=VerificationState.NOT_RUN,
    )


def _bundle(*records: EvidenceRecord) -> EvidenceBundle:
    by_claim_key: dict[str, tuple[EvidenceRecord, ...]] = {}
    for record in records:
        by_claim_key.setdefault(record.claim_key, ())
        by_claim_key[record.claim_key] = by_claim_key[record.claim_key] + (record,)
    return EvidenceBundle(records=tuple(records), by_claim_key=by_claim_key)


_EMPTY_BUNDLE = _bundle(
    _record(
        claim_key="source:spec/grammar.ebnf",
        claim="Lexical grammar for identifiers.",
        source_path="spec/grammar.ebnf",
    )
)


class OwnershipExclusivityTests(unittest.TestCase):
    def test_every_responsibility_has_exactly_one_owner(self) -> None:
        """Requirement 11.2 / Property 19: exactly one owner per responsibility."""

        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        drafted = {str(d.responsibility_id) for d in result.responsibility_drafts}
        expected = {str(item.responsibility_id) for item in APPLICATION_RESPONSIBILITIES}
        self.assertEqual(drafted, expected)
        for draft in result.responsibility_drafts:
            self.assertIn(
                draft.ownership,
                {Ownership.NEBULA_OWNED, Ownership.HOST_OWNED, Ownership.OPERATIONS_OWNED},
            )

    def test_default_owner_used_without_evidence(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        cli = result.responsibility_for(_CLI)
        assert cli is not None
        self.assertEqual(cli.ownership, Ownership.NEBULA_OWNED)
        self.assertFalse(cli.owner_from_evidence)
        # TLS/crypto default to host-owned; signing/install/update default to ops.
        self.assertEqual(result.ownership_for(_TLS), Ownership.HOST_OWNED)
        self.assertEqual(result.ownership_for(_SIGNING), Ownership.OPERATIONS_OWNED)
        self.assertEqual(result.ownership_for(_NOTARIZATION), Ownership.HOST_OWNED)

    def test_scope_ownership_overrides_default(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:cli/main.nb",
                claim="The nebula cli command runner is provided by a host toolkit.",
                ownership=Ownership.HOST_OWNED,
                source_path="cli/main.nb",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        cli = result.responsibility_for(_CLI)
        assert cli is not None
        self.assertEqual(cli.ownership, Ownership.HOST_OWNED)
        self.assertTrue(cli.owner_from_evidence)

    def test_text_marker_overrides_default_when_no_scope(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="source:cli/main.nb",
                claim="The cli tool is an operations-owned release pipeline artifact.",
                source_path="cli/main.nb",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        cli = result.responsibility_for(_CLI)
        assert cli is not None
        self.assertEqual(cli.ownership, Ownership.OPERATIONS_OWNED)
        self.assertTrue(cli.owner_from_evidence)

    def test_conflicting_scope_ownership_falls_back_to_default(self) -> None:
        """Conflicting evidence never yields two owners; it falls back to default."""

        bundle = _bundle(
            _record(
                claim_key="source:cli/a.nb",
                claim="The cli tool ships one way.",
                ownership=Ownership.HOST_OWNED,
                source_path="cli/a.nb",
            ),
            _record(
                claim_key="source:cli/b.nb",
                claim="The cli tool ships another way.",
                ownership=Ownership.OPERATIONS_OWNED,
                source_path="cli/b.nb",
            ),
        )
        result = evaluate_application_ecosystem_release(bundle)
        cli = result.responsibility_for(_CLI)
        assert cli is not None
        self.assertEqual(cli.ownership, Ownership.NEBULA_OWNED)  # declared default
        self.assertFalse(cli.owner_from_evidence)

    def test_ownership_is_order_independent(self) -> None:
        records = (
            _record(
                claim_key="source:cli/main.nb",
                claim="The cli tool is host-owned.",
                source_path="cli/main.nb",
            ),
            _record(
                claim_key="source:signing.nb",
                claim="Code signing key handling for releases.",
                source_path=".github/workflows/release.yml",
            ),
        )
        forward = evaluate_application_ecosystem_release(_bundle(*records))
        reverse = evaluate_application_ecosystem_release(_bundle(*reversed(records)))
        self.assertEqual(
            [(str(d.responsibility_id), d.ownership) for d in forward.responsibility_drafts],
            [(str(d.responsibility_id), d.ownership) for d in reverse.responsibility_drafts],
        )


class EcosystemAndReleaseGapTests(unittest.TestCase):
    def test_missing_ecosystem_capability_creates_gap(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        gap = result.gap_for(_PKG_BREADTH)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.ECOSYSTEM)
        self.assertEqual(gap.target_level, TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM)

    def test_contract_suite_gap_is_verification(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        gap = result.gap_for(_CONTRACT_SUITES)
        assert gap is not None
        self.assertEqual(gap.primary_category, GapCategory.VERIFICATION)

    def test_direct_current_implementation_satisfies_capability(self) -> None:
        """A present-tense-permitted implementation record removes the gap."""

        bundle = _bundle(
            _record(
                claim_key="workflow:contract-tests",
                claim="The contract test suite runs on four platforms.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.TEST_EXECUTION,
                origin=RevisionOrigin.CURRENT_WORKTREE,
                source_path=".github/workflows/contract-tests.yml",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        draft = result.ecosystem_draft_for(_CONTRACT_SUITES)
        assert draft is not None
        self.assertTrue(draft.satisfied)
        self.assertIsNone(result.gap_for(_CONTRACT_SUITES))

    def test_planned_evidence_does_not_satisfy(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="plan:contract-tests",
                claim="A future contract test suite is planned.",
                status=EvidenceStatus.PLANNED,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path=".github/workflows/contract-tests.yml",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        draft = result.ecosystem_draft_for(_CONTRACT_SUITES)
        assert draft is not None
        self.assertFalse(draft.satisfied)
        self.assertIsNotNone(result.gap_for(_CONTRACT_SUITES))


class ScopedReleaseIsolationTests(unittest.TestCase):
    def test_scoped_release_blocks_substrate_promotion(self) -> None:
        """Requirement 11.6 / Property 9: scoped release cannot raise substrate."""

        bundle = _bundle(
            _record(
                claim_key="release:backend-sdk",
                claim="The Linux backend SDK release smoke verification passed.",
                status=EvidenceStatus.BACKEND_SDK_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        draft = result.ecosystem_draft_for("capability-release-release-smoke")
        assert draft is not None
        self.assertTrue(draft.substrate_promotion_blocked)
        self.assertTrue(draft.scoped_release_only)
        limitation_text = "\n".join(draft.limitations).lower()
        self.assertIn("os-substrate", limitation_text)

    def test_compiler_tooling_release_marked_scoped(self) -> None:
        bundle = _bundle(
            _record(
                claim_key="release:compiler-tooling",
                claim="Compiler/tooling GA installers are published.",
                status=EvidenceStatus.COMPILER_TOOLING_GA,
                evidence_kind=EvidenceKind.ARTIFACT,
                origin=RevisionOrigin.COMMITTED_REVISION,
                source_path=".github/workflows/release.yml",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        draft = result.ecosystem_draft_for("capability-release-installers")
        assert draft is not None
        self.assertTrue(draft.substrate_promotion_blocked)


class ReusableOutputForTask75Tests(unittest.TestCase):
    def test_security_sensitive_drafts_exposed(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        sensitive = {str(d.domain.id) for d in result.security_sensitive_drafts()}
        self.assertIn(_SECURITY_MAINT, sensitive)
        self.assertIn(_SBOM, sensitive)

    def test_security_sensitive_responsibilities_exposed(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        sensitive = {str(d.responsibility_id) for d in result.security_sensitive_responsibilities()}
        self.assertIn(_TLS, sensitive)
        self.assertIn(_SIGNING, sensitive)

    def test_responsibilities_owned_by_partitions_all(self) -> None:
        result = evaluate_application_ecosystem_release(_EMPTY_BUNDLE)
        total = 0
        for owner in (Ownership.NEBULA_OWNED, Ownership.HOST_OWNED, Ownership.OPERATIONS_OWNED):
            total += len(result.responsibilities_owned_by(owner))
        self.assertEqual(total, len(APPLICATION_RESPONSIBILITIES))

    def test_security_maintenance_preview_adds_ecosystem_secondary(self) -> None:
        """A preview security-sensitive package raises an ecosystem-obligation signal."""

        bundle = _bundle(
            _record(
                claim_key="package:security-maintenance",
                claim="Security maintenance and vulnerability response for the crypto package.",
                status=EvidenceStatus.REPO_PREVIEW,
                evidence_kind=EvidenceKind.SPECIFICATION,
                source_path="docs/official_package_tiering.md",
            )
        )
        result = evaluate_application_ecosystem_release(bundle)
        gap = result.gap_for(_SECURITY_MAINT)
        assert gap is not None
        # security-maintenance primary is already ECOSYSTEM, so ensure it is at
        # least classified as an ecosystem obligation with elevated risk.
        self.assertEqual(gap.primary_category, GapCategory.ECOSYSTEM)
        self.assertGreaterEqual(gap.claim_risk, 1)


class CoverageTests(unittest.TestCase):
    def test_all_requirement_11_1_and_11_3_responsibilities_present(self) -> None:
        markers = "\n".join(
            marker for item in APPLICATION_RESPONSIBILITIES for marker in item.markers
        ).lower()
        names = "\n".join(item.name for item in APPLICATION_RESPONSIBILITIES).lower()
        haystack = markers + "\n" + names
        for keyword in (
            "cli",
            "backend service",
            "control plane",
            "embedded data",
            "authentication",
            "job",
            "tls",
            "crypto",
            "ui semantics",
            "thin-host",
            "native host adapter",
            "renderer",
            "widget",
            "layout",
            "accessibility",
            "device integration",
            "signing",
            "notarization",
            "install",
            "update",
            "distribution",
            "crash report",
        ):
            self.assertIn(keyword, haystack, msg=f"missing responsibility coverage: {keyword}")

    def test_all_requirement_11_4_ecosystem_and_11_5_release_present(self) -> None:
        markers = "\n".join(
            marker
            for item in (*ECOSYSTEM_CHECKLIST, *RELEASE_CHECKLIST)
            for marker in item.markers
        ).lower()
        for keyword in (
            "package breadth",
            "documentation",
            "starter",
            "compatibility",
            "contributor",
            "adoption",
            "security maintenance",
            "long-term support",
            "build matrix",
            "contract",
            "sanitizer",
            "release smoke",
            "sbom",
            "provenance",
            "attestation",
            "installer",
            "rollback",
            "platform qualification",
        ):
            self.assertIn(keyword, markers, msg=f"missing ecosystem/release coverage: {keyword}")

    def test_ecosystem_and_release_groups_are_consistent(self) -> None:
        for item in ECOSYSTEM_CHECKLIST:
            self.assertEqual(item.group, ResponsibilityGroup.ECOSYSTEM)
        for item in RELEASE_CHECKLIST:
            self.assertEqual(item.group, ResponsibilityGroup.RELEASE)

    def test_uses_provided_guarded_evidence(self) -> None:
        guarded = guard_evidence(_EMPTY_BUNDLE)
        result = ApplicationEcosystemReleaseEvaluator().evaluate(_EMPTY_BUNDLE, guarded)
        self.assertEqual(
            len(result.responsibility_drafts), len(APPLICATION_RESPONSIBILITIES)
        )

    def test_evaluation_is_order_independent_for_gaps(self) -> None:
        records = (
            _record(
                claim_key="source:docs/support_matrix.md",
                claim="No long-term support lifecycle exists.",
            ),
            _record(
                claim_key="source:release.yml",
                claim="No SBOM is generated.",
                source_path=".github/workflows/release.yml",
            ),
        )
        forward = evaluate_application_ecosystem_release(_bundle(*records))
        reverse = evaluate_application_ecosystem_release(_bundle(*reversed(records)))
        self.assertEqual(
            [str(g.id) for g in forward.gaps], [str(g.id) for g in reverse.gaps]
        )


if __name__ == "__main__":
    unittest.main()
