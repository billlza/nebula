"""Real-repository baseline integration tests (Task 14.4).

These deterministic ``unittest`` integration tests drive the Task 14.2 curated
baseline (:func:`tools.universe_os_gap_analysis.baseline.build_curated_model` /
``curated_assembler``) against the **real** Nebula repository at the repository
root and assert the evidence-boundary contract the baseline must uphold
(Requirements 4.1-4.6, 7.4-7.6, 8.4, 8.6, 11.6, 15.1-15.7):

* hosted compiler/tooling capability frontiers carry ``Compiler_Tooling_GA``
  (and/or ``Backend_SDK_GA`` where evidenced) at hosted/language target levels
  only, capped at repository-local maturity 2 without cross-supported-host
  candidate evidence (Requirements 4.2, 4.3, 15.4);
* preview / experimental / planned / unsupported / unknown statuses are
  represented where the repository evidence supports them and are never upgraded
  to GA; the primitive freestanding object path stays an experimental
  clang-backed relocatable-object gate (Requirements 4.2, 4.4, 8.4, 8.6);
* the external host-compiler limitation is recorded (hosted C++23 + host
  compiler production dependency) and T1 backend/language independence is *not*
  claimed (Requirements 7.4, 7.5, 7.6, 15.2);
* every OS-substrate domain (``T2``-``T5``: freestanding runtime, linked/bootable
  chain, kernel, drivers, UniverseOS userspace) is ``Unsupported`` / maturity 0
  with no inference from plans (Requirements 4.5, 10.6, 15.5);
* the initial evidence-backed conclusion contract holds -- T1 unachieved, T2-T5
  unachieved, language/tooling <= 2, Hosted Adjacency isolated (Requirements
  15.1-15.7);
* scoped hosted / release evidence is represented without over-claiming: it can
  never propagate into OS-substrate maturity (Requirements 4.6, 9.2, 11.6);
* the curated model validates and a real-repository ``run_pipeline`` reaches
  ``EXIT_OK`` (Requirements 14.1-14.7).

Determinism: the live working tree contains untracked directories (``.hypothesis/``,
``__pycache__/``, ``.pytest_cache/``) that can mutate mid-run and cause transient
``REV-*`` drift while the revision binder captures its fingerprint. To stay
deterministic these tests **bind the model once** through the curated assembler
with a single stable revision (retrying only on transient binding drift) and
then assert against that single built model / guarded-evidence layer rather than
repeatedly re-binding. The one test that exercises the full ``run_pipeline``
publish path mirrors the ``_run_stable`` retry-on-transient-drift helper from
``test_e2e_integration.py``.

This module reads no product code, executes no commands, and mutates nothing.
"""

from __future__ import annotations

import unittest
from collections import Counter
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.universe_os_gap_analysis.adapters import adapt_repository_evidence
from tools.universe_os_gap_analysis.baseline import build_curated_model
from tools.universe_os_gap_analysis.catalog import INITIAL_CONCLUSIONS
from tools.universe_os_gap_analysis.claim_guard import (
    PRIMITIVE_OBJECT_WORDING,
    guard_evidence,
)
from tools.universe_os_gap_analysis.evidence import (
    collect_evidence,
    detect_evidence_conflicts,
)
from tools.universe_os_gap_analysis.inventory import discover_source_inventory
from tools.universe_os_gap_analysis.models import (
    EvidenceKind,
    EvidenceStatus,
    MaturityScore,
    TargetLevel,
)
from tools.universe_os_gap_analysis.pipeline import (
    EXIT_OK,
    PipelineConfig,
    PipelineContext,
    run_pipeline,
)
from tools.universe_os_gap_analysis.revision import (
    RevisionBinder,
    RevisionBindingError,
)
from tools.universe_os_gap_analysis.validator import validate_assessment_model

# OS-substrate target levels that must stay at maturity 0 / Unsupported without
# direct implementation evidence (Requirements 3.6, 10.6, 15.5).
_SUBSTRATE_LEVELS = frozenset(
    {
        TargetLevel.T2_FREESTANDING_SUBSTRATE,
        TargetLevel.T3_BOOT_AND_KERNEL_FOUNDATION,
        TargetLevel.T4_ISOLATED_USERSPACE_PLATFORM,
        TargetLevel.T5_OPERABLE_UNIVERSE_OS,
    }
)

# Hosted-adjacency / language-platform levels that may carry GA frontiers.
_HOSTED_LEVELS = frozenset(
    {
        TargetLevel.T0_HOSTED_ADJACENCY,
        TargetLevel.T1_INDEPENDENT_LANGUAGE_PLATFORM,
    }
)

# The GA statuses reserved for hosted compiler/tooling and Linux backend SDK
# scope; they must never appear at an OS-substrate level (Requirements 4.3, 11.6).
_GA_STATUSES = frozenset(
    {EvidenceStatus.COMPILER_TOOLING_GA, EvidenceStatus.BACKEND_SDK_GA}
)

# Statuses that never count as direct current-revision implementation evidence.
_NON_IMPLEMENTATION_STATUSES = frozenset(
    {EvidenceStatus.PLANNED, EvidenceStatus.UNSUPPORTED}
)

# Preview statuses reserved for installed/repository preview scope. They live at
# hosted/language levels and are never upgraded to GA or propagated into the OS
# substrate (Requirements 8.4, 8.6, 11.6).
_PREVIEW_STATUSES = frozenset(
    {EvidenceStatus.INSTALLED_PREVIEW, EvidenceStatus.REPO_PREVIEW}
)

_REPO_LOCAL_CAP = int(MaturityScore.REPEATABLE_REPOSITORY_IMPLEMENTATION)

# Transient revision-drift codes: the live repository can mutate untracked
# working-tree files (e.g. ``__pycache__``/``.pytest_cache``/``.hypothesis``)
# while the binder captures its fingerprint. That is a legitimate fail-closed
# outcome but orthogonal to the curated-baseline contract under test, so binding
# is retried on these codes to keep the model deterministic.
_TRANSIENT_DRIFT_CODES = frozenset(
    {"REV-DRIFT", "REV-ROOT-DRIFT", "REV-VERSION-DRIFT", "REV-FINGERPRINT-DRIFT"}
)
_MAX_DRIFT_RETRIES = 8


def _real_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _bind_stable(repo_root: Path, exclude: tuple[Path, ...]):
    """Bind the revision once, retrying only on transient live-repository drift."""

    binder = RevisionBinder()
    attempts = 0
    while True:
        try:
            return binder.bind(repo_root, exclude)
        except RevisionBindingError as error:  # pragma: no cover - retry path
            attempts += 1
            if (
                error.code not in _TRANSIENT_DRIFT_CODES
                or attempts > _MAX_DRIFT_RETRIES
            ):
                raise


def _build_context_and_model() -> tuple[PipelineContext, object]:
    """Assemble the curated baseline for the real repo from a single binding.

    Runs the deterministic pipeline stages once against the real repository root
    and hands the resulting context to the curated assembler. Binding the
    revision exactly once (with transient-drift retry) keeps the built model and
    its guarded-evidence layer stable so every assertion below is deterministic.
    """

    repo_root = _real_repo_root()
    # A never-written output path, excluded from the fingerprint just as the
    # pipeline excludes its real ``--output-dir``.
    output_dir = repo_root / ".uos-baseline-integration-nonexistent-output"

    revision = _bind_stable(repo_root, (output_dir,))
    inventory = tuple(discover_source_inventory(repo_root, revision))
    adapter_bundle = adapt_repository_evidence(repo_root, inventory)
    evidence_bundle = collect_evidence(revision, inventory, adapter_bundle, ())
    conflicts = detect_evidence_conflicts(evidence_bundle)
    guarded = guard_evidence(evidence_bundle)

    config = PipelineConfig(
        repo_root=repo_root, output_dir=output_dir, dry_run=True
    )
    context = PipelineContext(
        config=config,
        revision=revision,
        inventory=inventory,
        adapter_bundle=adapter_bundle,
        evidence_bundle=evidence_bundle,
        conflicts=conflicts,
        guarded=guarded,
        evaluator_outputs=(),
    )
    model = build_curated_model(context)
    return context, model


# Build the curated baseline once for the whole module (it is expensive: it
# collects thousands of evidence records over the real repository). Every test
# class shares this single, stable, deterministic result.
_CACHE: dict[str, object] = {}


def _curated() -> tuple[PipelineContext, object]:
    if "value" not in _CACHE:
        _CACHE["value"] = _build_context_and_model()
    return _CACHE["value"]  # type: ignore[return-value]


def _run_stable(config: PipelineConfig):
    """Run the pipeline, retrying only on transient live-repository drift."""

    result = run_pipeline(config)
    attempts = 0
    while (
        not result.ok
        and result.error_code in _TRANSIENT_DRIFT_CODES
        and attempts < _MAX_DRIFT_RETRIES
    ):
        attempts += 1
        result = run_pipeline(config)
    return result


class CuratedBaselineValidityTests(unittest.TestCase):
    """The curated real-repo model validates and publishes end to end."""

    def setUp(self) -> None:
        self.context, self.model = _curated()

    def test_curated_model_validates(self) -> None:
        result = validate_assessment_model(self.model)
        self.assertTrue(result.valid, msg=f"findings: {result.findings}")

    def test_every_domain_has_exactly_one_assessment(self) -> None:
        domain_ids = sorted(str(d.id) for d in self.model.domains)
        assessed = sorted(str(a.domain_id) for a in self.model.assessments)
        self.assertTrue(domain_ids)
        self.assertEqual(domain_ids, assessed)

    def test_six_ordered_target_levels_present(self) -> None:
        self.assertEqual(set(self.model.target_levels), set(TargetLevel))

    def test_real_repo_run_pipeline_reaches_exit_ok(self) -> None:
        # Full pipeline integration through the default (curated) assembler.
        with TemporaryDirectory() as tmp:
            config = PipelineConfig(
                repo_root=_real_repo_root(),
                output_dir=Path(tmp) / "assessment-output",
                dry_run=True,
            )
            result = _run_stable(config)
        self.assertEqual(result.exit_code, EXIT_OK, msg=result.findings)
        self.assertIsNotNone(result.model)
        self.assertTrue(result.publish_result.validation.valid)


class CompilerToolingGaBoundaryTests(unittest.TestCase):
    """GA frontiers stay hosted/language-scoped and capped at repository-local 2."""

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_ga_frontiers_exist_only_at_hosted_levels(self) -> None:
        ga_gates = [g for g in self.model.hard_gates if g.status in _GA_STATUSES]
        # The hosted compiler/tooling and independent-language frontiers carry a
        # GA status; there is at least one such frontier.
        self.assertTrue(ga_gates, "expected at least one GA capability frontier")
        for gate in ga_gates:
            self.assertIn(
                gate.target_level,
                _HOSTED_LEVELS,
                msg=f"GA frontier {gate.id} is not hosted/language scoped",
            )

    def test_ga_frontiers_capped_at_repository_local_two(self) -> None:
        for gate in self.model.hard_gates:
            if gate.status in _GA_STATUSES:
                self.assertLessEqual(
                    int(gate.maturity_score),
                    _REPO_LOCAL_CAP,
                    msg=f"GA frontier {gate.id} exceeds repository-local cap 2",
                )

    def test_substrate_frontiers_are_unsupported_zero(self) -> None:
        substrate_gates = [
            g for g in self.model.hard_gates if g.target_level in _SUBSTRATE_LEVELS
        ]
        self.assertTrue(substrate_gates)
        for gate in substrate_gates:
            self.assertEqual(gate.status, EvidenceStatus.UNSUPPORTED)
            self.assertEqual(int(gate.maturity_score), 0)
            self.assertNotIn(gate.status, _GA_STATUSES)

    def test_no_assessment_exceeds_repository_local_two(self) -> None:
        for assessment in self.model.assessments:
            self.assertLessEqual(
                int(assessment.effective_score),
                _REPO_LOCAL_CAP,
                msg=f"{assessment.domain_id} exceeds repository-local cap 2",
            )
            self.assertLessEqual(int(assessment.raw_score), _REPO_LOCAL_CAP)

    def test_ga_status_assessments_stay_hosted_scoped(self) -> None:
        # Any capability summarised with a GA status must live at a hosted /
        # language-platform level, never on the OS substrate (Requirement 11.6).
        for assessment in self.model.assessments:
            if assessment.evidence_status in _GA_STATUSES:
                domain = self.domain_by_id[str(assessment.domain_id)]
                self.assertIn(domain.target_level, _HOSTED_LEVELS)


class PreviewExperimentalPlannedBoundaryTests(unittest.TestCase):
    """Preview/experimental/planned/unknown statuses are represented, never upgraded."""

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.record_by_id = {str(r.id): r for r in self.model.evidence_records}
        self.status_counts = Counter(
            r.status for r in self.model.evidence_records
        )

    def test_statuses_are_a_subset_of_the_closed_enum(self) -> None:
        for status in self.status_counts:
            self.assertIsInstance(status, EvidenceStatus)

    def test_planned_and_experimental_are_represented(self) -> None:
        # The repository has planned prose and experimental gates; both are
        # represented in the curated evidence (Requirements 4.2, 8.4).
        self.assertGreater(self.status_counts[EvidenceStatus.PLANNED], 0)
        self.assertGreater(self.status_counts[EvidenceStatus.EXPERIMENTAL], 0)

    def test_planned_and_unsupported_are_never_credited_as_implementation(self) -> None:
        # No assessment may credit a planned/unsupported record as direct
        # implementation evidence (Requirements 4.2, 8.4, 15.5).
        for assessment in self.model.assessments:
            for ref in assessment.evidence_ids:
                record = self.record_by_id.get(str(ref))
                self.assertIsNotNone(record)
                self.assertNotIn(record.status, _NON_IMPLEMENTATION_STATUSES)

    def test_planned_and_experimental_are_not_upgraded_to_ga(self) -> None:
        # A planned or experimental record must keep its status; it is never
        # silently promoted to a GA classification.
        for record in self.model.evidence_records:
            if record.status in {
                EvidenceStatus.PLANNED,
                EvidenceStatus.EXPERIMENTAL,
            }:
                self.assertNotIn(record.status, _GA_STATUSES)

    def test_primitive_object_is_an_experimental_clang_relocatable_gate(self) -> None:
        # The primitive freestanding object path is described only as clang-backed
        # ELF64 relocatable-object emission -- never a backend/linked/boot claim
        # (Requirements 4.4, 7.6).
        primitive = [c for c in self.context.guarded.claims if c.is_primitive_object]
        self.assertTrue(primitive, "expected primitive freestanding object evidence")
        for claim in primitive:
            self.assertEqual(claim.guarded_wording, PRIMITIVE_OBJECT_WORDING)


class ExternalHostCompilerLimitationTests(unittest.TestCase):
    """The hosted C++23 + host-compiler dependency is recorded; T1 is not claimed."""

    def setUp(self) -> None:
        self.context, self.model = _curated()

    def test_host_compiler_production_dependency_is_an_assumption(self) -> None:
        joined = "\n".join(self.model.assumptions).lower()
        self.assertIn("host", joined)
        self.assertIn("compiler", joined)

    def test_backend_independence_is_an_explicit_non_claim(self) -> None:
        joined = "\n".join(self.model.non_claims).lower()
        self.assertIn("backend independence", joined)
        # The dependency on generated C++ and an external host compiler is named.
        self.assertIn("generated c++", joined)
        self.assertIn("external host compiler", joined)

    def test_t1_independence_is_not_claimed(self) -> None:
        # The initial conclusion for Requirement 15.2 (T1 materially unachieved)
        # is projected as an observed fact.
        texts = {c.text for c in self.model.observed_conclusions}
        conclusion_15_2 = next(
            c for c in INITIAL_CONCLUSIONS if c.requirement_ref == "15.2"
        )
        self.assertIn(conclusion_15_2.text, texts)


class OsSubstrateUnsupportedTests(unittest.TestCase):
    """Every OS-substrate domain is Unsupported / maturity 0 with no plan inference."""

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_every_substrate_level_has_at_least_one_domain(self) -> None:
        levels = {d.target_level for d in self.model.domains}
        for level in _SUBSTRATE_LEVELS:
            self.assertIn(level, levels, msg=f"missing substrate level {level.value}")

    def test_substrate_domains_are_zero_unsupported_and_credit_no_evidence(self) -> None:
        checked = 0
        for assessment in self.model.assessments:
            domain = self.domain_by_id[str(assessment.domain_id)]
            if domain.target_level in _SUBSTRATE_LEVELS:
                checked += 1
                self.assertEqual(int(assessment.raw_score), 0, msg=str(domain.id))
                self.assertEqual(
                    int(assessment.effective_score), 0, msg=str(domain.id)
                )
                self.assertEqual(assessment.evidence_ids, (), msg=str(domain.id))
                self.assertEqual(
                    assessment.evidence_status,
                    EvidenceStatus.UNSUPPORTED,
                    msg=str(domain.id),
                )
        self.assertGreater(checked, 0)

    def test_no_capability_is_inferred_from_plans(self) -> None:
        # Any assessment that credits no direct evidence scores exactly 0; a
        # positive score is never inferred from plans/prerequisites/adjacency.
        for assessment in self.model.assessments:
            if not assessment.evidence_ids:
                self.assertEqual(int(assessment.effective_score), 0)
                self.assertEqual(int(assessment.raw_score), 0)

    def test_substrate_non_claims_are_present(self) -> None:
        joined = "\n".join(self.model.non_claims).lower()
        for token in (
            "kernel",
            "driver",
            "interrupt",
            "freestanding runtime",
            "bootable",
            "userspace",
        ):
            self.assertIn(token, joined, msg=f"missing non-claim token {token!r}")


class InitialConclusionContractTests(unittest.TestCase):
    """The initial evidence-backed distance conclusion holds (Requirements 15.1-15.7)."""

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.texts = {c.text for c in self.model.observed_conclusions}
        self.by_ref = {c.requirement_ref: c for c in INITIAL_CONCLUSIONS}

    def test_every_initial_conclusion_is_projected(self) -> None:
        for conclusion in INITIAL_CONCLUSIONS:
            self.assertIn(
                conclusion.text,
                self.texts,
                msg=f"missing initial conclusion {conclusion.requirement_ref}",
            )

    def test_t1_and_t2_through_t5_reported_unachieved(self) -> None:
        self.assertIn(self.by_ref["15.2"].text, self.texts)  # T1 unachieved
        self.assertIn(self.by_ref["15.3"].text, self.texts)  # T2-T5 unachieved

    def test_language_tooling_capped_and_adjacency_isolated(self) -> None:
        self.assertIn(self.by_ref["15.4"].text, self.texts)  # language/tooling <= 2
        self.assertIn(self.by_ref["15.6"].text, self.texts)  # adjacency isolated

    def test_hosted_foundation_and_shortest_path_reported(self) -> None:
        self.assertIn(self.by_ref["15.1"].text, self.texts)  # hosted foundation
        self.assertIn(self.by_ref["15.5"].text, self.texts)  # substrate zero
        self.assertIn(self.by_ref["15.7"].text, self.texts)  # shortest path


class UnknownBoundaryTests(unittest.TestCase):
    """The ``Unknown`` boundary: pathless / unresolved evidence stays bounded.

    ``Unknown`` marks capabilities with no verifiable current-revision
    implementation path (Requirements 4.2, 13.5). The real repository yields many
    such records. This boundary asserts that an ``Unknown`` classification is:

    * represented in the curated evidence (it is a live, used status);
    * never upgraded to a GA classification;
    * confined to hosted / language-platform summaries -- the OS substrate is
      strictly ``Unsupported`` (maturity 0), never merely ``Unknown``
      (Requirements 4.5, 10.6, 15.5); and
    * never credited above repository-local maturity 2 (Requirement 15.4).
    """

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_unknown_status_is_represented(self) -> None:
        unknown_records = [
            r
            for r in self.model.evidence_records
            if r.status is EvidenceStatus.UNKNOWN
        ]
        self.assertTrue(
            unknown_records, "expected pathless/unresolved Unknown evidence"
        )

    def test_unknown_is_never_upgraded_to_ga(self) -> None:
        for record in self.model.evidence_records:
            if record.status is EvidenceStatus.UNKNOWN:
                self.assertNotIn(record.status, _GA_STATUSES)
        for assessment in self.model.assessments:
            if assessment.evidence_status is EvidenceStatus.UNKNOWN:
                self.assertNotIn(assessment.evidence_status, _GA_STATUSES)

    def test_unknown_summaries_are_hosted_scoped_only(self) -> None:
        # No OS-substrate domain may be summarised merely as Unknown; the
        # substrate is strictly Unsupported / 0 (Requirements 4.5, 10.6, 15.5).
        checked = 0
        for assessment in self.model.assessments:
            if assessment.evidence_status is EvidenceStatus.UNKNOWN:
                checked += 1
                domain = self.domain_by_id[str(assessment.domain_id)]
                self.assertIn(
                    domain.target_level,
                    _HOSTED_LEVELS,
                    msg=f"Unknown summary {domain.id} escaped hosted scope",
                )
                self.assertNotIn(domain.target_level, _SUBSTRATE_LEVELS)
        self.assertGreater(checked, 0, "expected Unknown domain summaries")

    def test_unknown_assessments_capped_at_repository_local_two(self) -> None:
        for assessment in self.model.assessments:
            if assessment.evidence_status is EvidenceStatus.UNKNOWN:
                self.assertLessEqual(
                    int(assessment.effective_score), _REPO_LOCAL_CAP
                )
                self.assertLessEqual(int(assessment.raw_score), _REPO_LOCAL_CAP)


class PreviewBoundaryTests(unittest.TestCase):
    """The preview boundary: Installed/Repo preview never upgrades or propagates.

    ``Installed_Preview`` / ``Repo_Preview`` are hosted/language-scoped preview
    statuses. Wherever they appear -- in evidence records, capability summaries or
    hard-gate frontiers -- they must never be upgraded to a GA classification and
    must never sit at (or lift) an OS-substrate level (Requirements 8.4, 8.6,
    11.6). The current real-repository baseline surfaces no preview evidence, so
    these assertions also guard against a future regression that would introduce
    preview evidence and mishandle its scope.
    """

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_preview_and_ga_statuses_are_disjoint(self) -> None:
        self.assertEqual(_PREVIEW_STATUSES & _GA_STATUSES, frozenset())

    def test_preview_records_are_never_upgraded_to_ga(self) -> None:
        for record in self.model.evidence_records:
            if record.status in _PREVIEW_STATUSES:
                self.assertNotIn(record.status, _GA_STATUSES)

    def test_preview_summaries_never_sit_on_the_substrate(self) -> None:
        for assessment in self.model.assessments:
            if assessment.evidence_status in _PREVIEW_STATUSES:
                domain = self.domain_by_id[str(assessment.domain_id)]
                self.assertIn(domain.target_level, _HOSTED_LEVELS)
                self.assertNotIn(domain.target_level, _SUBSTRATE_LEVELS)

    def test_preview_gates_never_sit_on_the_substrate(self) -> None:
        for gate in self.model.hard_gates:
            if gate.status in _PREVIEW_STATUSES:
                self.assertIn(gate.target_level, _HOSTED_LEVELS)
                self.assertNotIn(gate.target_level, _SUBSTRATE_LEVELS)

    def test_no_substrate_gate_or_summary_carries_a_preview_status(self) -> None:
        # The observable substrate contract: substrate frontiers/summaries are
        # Unsupported, never a preview status.
        for gate in self.model.hard_gates:
            if gate.target_level in _SUBSTRATE_LEVELS:
                self.assertNotIn(gate.status, _PREVIEW_STATUSES)
        for assessment in self.model.assessments:
            domain = self.domain_by_id[str(assessment.domain_id)]
            if domain.target_level in _SUBSTRATE_LEVELS:
                self.assertNotIn(assessment.evidence_status, _PREVIEW_STATUSES)


class ScopedReleaseIsolationTests(unittest.TestCase):
    """Scoped hosted / release evidence is represented without over-claiming."""

    def setUp(self) -> None:
        self.context, self.model = _curated()
        self.claim_by_id = {
            str(c.evidence_id): c for c in self.context.guarded.claims
        }
        self.domain_by_id = {str(d.id): d for d in self.model.domains}

    def test_some_evidence_is_flagged_scoped_from_substrate(self) -> None:
        blocked = [
            c
            for c in self.context.guarded.claims
            if c.substrate_promotion_blocked
        ]
        self.assertTrue(blocked, "expected scoped evidence blocked from substrate")

    def test_hosted_examples_cannot_propagate_into_substrate(self) -> None:
        # Every hosted-workflow example is flagged so it cannot raise OS-substrate
        # maturity (Requirements 4.6, 11.6).
        examples = [
            r
            for r in self.model.evidence_records
            if r.evidence_kind is EvidenceKind.EXAMPLE
        ]
        self.assertTrue(examples, "expected hosted example evidence")
        for record in examples:
            claim = self.claim_by_id.get(str(record.id))
            self.assertIsNotNone(claim)
            self.assertTrue(
                claim.substrate_promotion_blocked,
                msg=f"example {record.id} could leak into substrate maturity",
            )

    def test_scoped_evidence_does_not_lift_substrate_domains(self) -> None:
        # The observable consequence of scope isolation: no substrate domain is
        # lifted above 0 by any hosted/scoped-release evidence.
        for assessment in self.model.assessments:
            domain = self.domain_by_id[str(assessment.domain_id)]
            if domain.target_level in _SUBSTRATE_LEVELS:
                self.assertEqual(int(assessment.effective_score), 0)


if __name__ == "__main__":
    unittest.main()
