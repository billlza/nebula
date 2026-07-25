"""Deterministic, read-only orchestration for the assessment pipeline (Task 12.1).

This module is the single place that *connects* the independently built stages
into one deterministic, read-only, fail-closed pipeline:

    Revision Binder -> Source Inventory -> Evidence Collector/Normalizer ->
    Claim Guard -> Evaluators -> (Maturity / gap register / roadmap) ->
    canonical model builder -> Validator -> Renderers (via ``publish_assessment``)

The command-line boundary (:mod:`tools.universe_os_gap_analysis.cli`) stays thin
and delegates here so the orchestration logic has one home and can be unit
tested without spawning a process.

Execution policy (explicit and read-only by default):

* No network access (``NETWORK_ENABLED`` is ``False``).
* No external command execution (``EXTERNAL_COMMANDS_ENABLED`` is ``False`` and
  the reused :class:`~tools.universe_os_gap_analysis.execution.ExecutionPolicy`
  is *disabled* by default). Command execution is opt-in via an explicit local
  allowlist only; the default pipeline never runs a command and therefore
  produces no execution evidence.
* The repository is only ever read; the sole write target is the explicit
  ``--output-dir``, and even then only when the run is *not* a dry run and the
  whole publish transaction succeeds.

Fail-closed exit-code contract (Requirements 1.1-1.6, 9.6, 9.7, 14.1-14.7):

* :data:`EXIT_OK` (0) -- the model validated, every artifact rendered and passed
  cross-artifact parity, and (unless this was a dry run) the artifacts were
  published atomically.
* :data:`EXIT_REPOSITORY_DRIFT` (3) -- the bound revision / worktree fingerprint
  drifted (or the revision could not be bound coherently). Nothing is published.
* :data:`EXIT_PIPELINE_ERROR` (4) -- an upstream analytic stage failed (inventory
  discovery, evidence collection, an evaluator, or model assembly). Nothing is
  published.
* :data:`EXIT_VALIDATION_FAILED` (5) -- the canonical model failed validation.
  Nothing is published (all-or-nothing; Requirement 9.7).
* :data:`EXIT_RENDER_PARITY_FAILED` (6) -- the model validated but a renderer
  failed or an artifact broke cross-artifact reference parity. Nothing is
  published.

Any non-zero exit means *no* artifact was written, so a partially valid report
can never be left behind. A dry run exercises every stage (including validation,
rendering, and parity) but writes nothing, returning ``EXIT_OK`` only when the
full transaction would have succeeded.

The concrete assembly of evaluator drafts into the fully curated canonical model
is the repository-baseline task (Task 14.2); this module keeps that step
*pluggable* through the ``assembler`` argument so the orchestration and the
fail-closed contract can be exercised independently of the curated baseline. The
default assembler wires the deterministic stages that compose today and hands the
result to :func:`~tools.universe_os_gap_analysis.model_builder.build_assessment_model`.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Sequence

from .adapters import AdapterBundle, adapt_repository_evidence
from .claim_guard import GuardedEvidence, guard_evidence
from .evidence import EvidenceBundle, collect_evidence, detect_evidence_conflicts
from .evaluators.memory_concurrency_safety import evaluate_memory_concurrency_safety
from .execution import ExecutionEvidence, ExecutionPolicy
from .inventory import InventoryError, discover_source_inventory
from .json_renderer import render_assessment_json, render_schema
from .language_evaluator import evaluate_language_type_system
from .manifest import ManifestBuilder, build_artifact_manifest
from .markdown_renderer import render_markdown
from .model_builder import (
    PublishResult,
    Renderer,
    build_assessment_model,
    publish_assessment,
)
from .models import (
    AssessmentModel,
    AssessmentRevision,
    CapabilityDomain,
    EvidenceConflict,
    GapEntry,
    SourceInventoryEntry,
    ValidationFinding,
)
from .revision import RevisionBinder, RevisionBindingError
from .table_renderer import (
    render_capability_matrix_csv,
    render_capability_matrix_json,
    render_gap_register_csv,
    render_gap_register_json,
)

# --------------------------------------------------------------------------- #
# Explicit read-only execution policy.                                         #
# --------------------------------------------------------------------------- #
NETWORK_ENABLED = False
EXTERNAL_COMMANDS_ENABLED = False


def read_only_execution_policy() -> ExecutionPolicy:
    """Return the explicit, disabled-by-default local execution policy.

    The default pipeline never enables the network or runs a command; command
    execution is opt-in only through an explicit local allowlist supplied on a
    non-default :class:`ExecutionPolicy` (Requirement 1.4, 13.1).
    """

    return ExecutionPolicy()


# --------------------------------------------------------------------------- #
# Fail-closed exit codes.                                                      #
# --------------------------------------------------------------------------- #
EXIT_OK = 0
EXIT_REPOSITORY_DRIFT = 3
EXIT_PIPELINE_ERROR = 4
EXIT_VALIDATION_FAILED = 5
EXIT_RENDER_PARITY_FAILED = 6

# Ordered stage names, used for deterministic reporting.
STAGE_REVISION = "revision-binder"
STAGE_INVENTORY = "source-inventory"
STAGE_ADAPTER = "evidence-adapter"
STAGE_EVIDENCE = "evidence-collector"
STAGE_CONFLICTS = "conflict-detection"
STAGE_CLAIM_GUARD = "claim-guard"
STAGE_EVALUATORS = "evaluators"
STAGE_ASSEMBLY = "model-assembly"
STAGE_PUBLISH = "validate-and-publish"

PIPELINE_STAGE_ORDER: tuple[str, ...] = (
    STAGE_REVISION,
    STAGE_INVENTORY,
    STAGE_ADAPTER,
    STAGE_EVIDENCE,
    STAGE_CONFLICTS,
    STAGE_CLAIM_GUARD,
    STAGE_EVALUATORS,
    STAGE_ASSEMBLY,
    STAGE_PUBLISH,
)

# The full renderer set published from the single canonical model. Each renderer
# maps the model to exactly one in-memory artifact; ``publish_assessment``
# enforces cross-artifact reference parity before anything is written. The
# digest-bound artifact manifest is not a plain renderer (it must digest the
# other artifacts' bytes), so it is added by the publish transaction via
# ``PipelineConfig.manifest_builder`` and committed atomically with this set.
DEFAULT_RENDERERS: tuple[Renderer, ...] = (
    render_assessment_json,
    render_schema,
    render_capability_matrix_csv,
    render_capability_matrix_json,
    render_gap_register_csv,
    render_gap_register_json,
    render_markdown,
)


@dataclass(frozen=True, slots=True)
class EvaluatorOutput:
    """A normalized evaluator result: the domains and gaps it produced.

    Individual evaluators expose slightly different result shapes; the pipeline
    normalizes each into this common projection so the model assembler can treat
    every evaluator uniformly.
    """

    name: str
    domains: tuple[CapabilityDomain, ...] = ()
    gaps: tuple[GapEntry, ...] = ()


# An evaluator adapter maps the shared evidence layers to a normalized output.
EvaluatorCallable = Callable[[EvidenceBundle, GuardedEvidence], EvaluatorOutput]


def _run_language_evaluator(
    bundle: EvidenceBundle, guarded: GuardedEvidence
) -> EvaluatorOutput:
    draft = evaluate_language_type_system(bundle, guarded)
    return EvaluatorOutput(
        name="language-type-system",
        domains=(draft.domain,),
        gaps=tuple(draft.gaps),
    )


def _run_memory_evaluator(
    bundle: EvidenceBundle, guarded: GuardedEvidence
) -> EvaluatorOutput:
    evaluation = evaluate_memory_concurrency_safety(bundle, guarded)
    return EvaluatorOutput(
        name="memory-concurrency-safety",
        domains=tuple(draft.domain for draft in evaluation.domain_drafts),
        gaps=tuple(evaluation.gaps),
    )


# The declarative evaluators wired into the default pipeline. Task 14.2 supplies
# the remaining curated evaluators through a custom assembler.
DEFAULT_EVALUATORS: tuple[EvaluatorCallable, ...] = (
    _run_language_evaluator,
    _run_memory_evaluator,
)


@dataclass(frozen=True, slots=True)
class PipelineConfig:
    """Immutable configuration for one pipeline run.

    ``execution_policy`` defaults to the disabled, read-only policy; supplying an
    enabled policy is the only way to opt into allowlisted local command
    execution. ``renderers`` defaults to the full artifact set. ``evaluators`` is
    the ordered set of declarative evaluators run against the shared evidence
    layers.
    """

    repo_root: Path
    output_dir: Path
    dry_run: bool = False
    execution_policy: ExecutionPolicy = field(default_factory=read_only_execution_policy)
    renderers: tuple[Renderer, ...] = DEFAULT_RENDERERS
    evaluators: tuple[EvaluatorCallable, ...] = DEFAULT_EVALUATORS
    manifest_builder: ManifestBuilder | None = build_artifact_manifest

    def __post_init__(self) -> None:
        object.__setattr__(self, "repo_root", Path(self.repo_root))
        object.__setattr__(self, "output_dir", Path(self.output_dir))
        if not isinstance(self.execution_policy, ExecutionPolicy):
            raise TypeError("execution_policy must be an ExecutionPolicy")
        object.__setattr__(self, "renderers", tuple(self.renderers))
        object.__setattr__(self, "evaluators", tuple(self.evaluators))


@dataclass(frozen=True, slots=True)
class PipelineContext:
    """Everything the deterministic stages produced, handed to the assembler."""

    config: PipelineConfig
    revision: AssessmentRevision
    inventory: tuple[SourceInventoryEntry, ...]
    adapter_bundle: AdapterBundle
    evidence_bundle: EvidenceBundle
    conflicts: tuple[EvidenceConflict, ...]
    guarded: GuardedEvidence
    evaluator_outputs: tuple[EvaluatorOutput, ...]
    execution_evidence: tuple[ExecutionEvidence, ...] = ()


# A model assembler turns the deterministic context into the canonical model.
ModelAssembler = Callable[[PipelineContext], AssessmentModel]


@dataclass(frozen=True, slots=True)
class StageOutcome:
    """The recorded result of one pipeline stage."""

    name: str
    status: str  # "ok" | "failed" | "skipped"
    detail: str = ""
    error_code: str | None = None


@dataclass(frozen=True, slots=True)
class PipelineResult:
    """The outcome of a full pipeline run.

    ``exit_code`` is the fail-closed process exit code. ``published`` is true only
    when the artifacts were committed (never true for a dry run). ``written_paths``
    lists any files committed to disk (empty on a dry run or any failure).
    """

    exit_code: int
    published: bool
    dry_run: bool
    stages: tuple[StageOutcome, ...]
    revision: AssessmentRevision | None = None
    model: AssessmentModel | None = None
    publish_result: PublishResult | None = None
    findings: tuple[ValidationFinding, ...] = ()
    error_code: str | None = None
    detail: str = ""
    written_paths: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        return self.exit_code == EXIT_OK


def default_model_assembler(context: PipelineContext) -> AssessmentModel:
    """Assemble the canonical model from the deterministic pipeline context.

    This wires the evaluator domains/gaps and the collected revision, inventory,
    evidence, and conflicts into the single
    :class:`~tools.universe_os_gap_analysis.models.AssessmentModel` via
    :func:`build_assessment_model`. It never fabricates maturity, gates, or
    assessments: the curated baseline (Task 14.2) supplies those through a custom
    assembler. When the assembled model is incomplete for a publishable report,
    the validator flags it and the pipeline fails closed rather than emitting a
    partially valid report.
    """

    domains: list[CapabilityDomain] = []
    seen_domains: set[str] = set()
    gaps: dict[str, GapEntry] = {}
    for output in context.evaluator_outputs:
        for domain in output.domains:
            key = str(domain.id)
            if key not in seen_domains:
                seen_domains.add(key)
                domains.append(domain)
        for gap in output.gaps:
            gaps[str(gap.id)] = gap

    return build_assessment_model(
        revision=context.revision,
        source_inventory=context.inventory,
        evidence_records=context.evidence_bundle.records,
        conflicts=context.conflicts,
        domains=tuple(domains),
        gaps=tuple(gaps.values()),
    )


def run_pipeline(
    config: PipelineConfig,
    *,
    assembler: ModelAssembler | None = None,
) -> PipelineResult:
    """Run the full deterministic, read-only, fail-closed assessment pipeline.

    Stages run in the fixed order documented in :data:`PIPELINE_STAGE_ORDER`. The
    first stage failure aborts the run and returns a non-zero exit code with no
    artifacts published. See the module docstring for the exit-code contract.
    """

    if not isinstance(config, PipelineConfig):
        raise TypeError("config must be a PipelineConfig")
    # The curated repository baseline (Task 14.2) is the default assembler: it
    # runs every domain evaluator and produces a complete, publishable model for
    # the real repository. Callers may still inject a custom assembler (e.g. the
    # fail-closed pipeline tests) to exercise the exit-code contract in isolation.
    if assembler is not None:
        assemble = assembler
    else:
        from .baseline import build_curated_model

        assemble = build_curated_model

    repo_root = config.repo_root
    output_dir = config.output_dir
    stages: list[StageOutcome] = []

    # -- Stage: Revision Binder (fail closed on drift / binding failure). ---- #
    try:
        revision = RevisionBinder().bind(repo_root, (output_dir,))
    except RevisionBindingError as error:
        drift = error.code in {"REV-DRIFT", "REV-ROOT-DRIFT"}
        exit_code = EXIT_REPOSITORY_DRIFT if drift else EXIT_PIPELINE_ERROR
        stages.append(
            StageOutcome(STAGE_REVISION, "failed", error.message, error.code)
        )
        return PipelineResult(
            exit_code=exit_code,
            published=False,
            dry_run=config.dry_run,
            stages=tuple(stages),
            error_code=error.code,
            detail=f"{error.code}: {error.message}",
        )
    stages.append(
        StageOutcome(STAGE_REVISION, "ok", f"commit {revision.commit_id[:12]}")
    )

    # -- Stage: Source Inventory. -------------------------------------------- #
    try:
        inventory = tuple(discover_source_inventory(repo_root, revision))
    except InventoryError as error:
        return _stage_failure(
            stages, STAGE_INVENTORY, error.code, str(error), revision, config
        )
    except (OSError, ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_INVENTORY, "INV-UNEXPECTED", str(error), revision, config
        )
    stages.append(
        StageOutcome(STAGE_INVENTORY, "ok", f"{len(inventory)} entries")
    )

    # -- Stage: repository evidence adapter (feeds the collector). ----------- #
    try:
        adapter_bundle = adapt_repository_evidence(repo_root, inventory)
    except InventoryError as error:
        return _stage_failure(
            stages, STAGE_ADAPTER, error.code, str(error), revision, config
        )
    except (OSError, ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_ADAPTER, "INV-ADAPTER", str(error), revision, config
        )
    stages.append(StageOutcome(STAGE_ADAPTER, "ok"))

    # -- Stage: Evidence Collector / Normalizer. ----------------------------- #
    # The default policy runs no commands, so there is no execution evidence.
    execution_evidence: tuple[ExecutionEvidence, ...] = ()
    try:
        evidence_bundle = collect_evidence(
            revision, inventory, adapter_bundle, execution_evidence
        )
    except (ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_EVIDENCE, "EVD-COLLECT", str(error), revision, config
        )
    stages.append(
        StageOutcome(STAGE_EVIDENCE, "ok", f"{len(evidence_bundle.records)} records")
    )

    # -- Stage: evidence conflict detection. --------------------------------- #
    try:
        conflicts = detect_evidence_conflicts(evidence_bundle)
    except (ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_CONFLICTS, "CNF-DETECT", str(error), revision, config
        )
    stages.append(
        StageOutcome(STAGE_CONFLICTS, "ok", f"{len(conflicts)} conflicts")
    )

    # -- Stage: Claim Guard. ------------------------------------------------- #
    try:
        guarded = guard_evidence(evidence_bundle)
    except (ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_CLAIM_GUARD, "CLM-GUARD", str(error), revision, config
        )
    stages.append(StageOutcome(STAGE_CLAIM_GUARD, "ok"))

    # -- Stage: Evaluators. -------------------------------------------------- #
    try:
        evaluator_outputs = tuple(
            evaluator(evidence_bundle, guarded) for evaluator in config.evaluators
        )
    except (ValueError, TypeError) as error:
        return _stage_failure(
            stages, STAGE_EVALUATORS, "EVAL-RUN", str(error), revision, config
        )
    stages.append(
        StageOutcome(STAGE_EVALUATORS, "ok", f"{len(evaluator_outputs)} evaluators")
    )

    context = PipelineContext(
        config=config,
        revision=revision,
        inventory=inventory,
        adapter_bundle=adapter_bundle,
        evidence_bundle=evidence_bundle,
        conflicts=conflicts,
        guarded=guarded,
        evaluator_outputs=evaluator_outputs,
        execution_evidence=execution_evidence,
    )

    # -- Stage: canonical model assembly (Maturity/gap/roadmap -> model). ---- #
    try:
        model = assemble(context)
    except Exception as error:  # noqa: BLE001 - any assembly failure fails closed.
        return _stage_failure(
            stages, STAGE_ASSEMBLY, "RPT-ASSEMBLY", str(error), revision, config
        )
    if not isinstance(model, AssessmentModel):
        return _stage_failure(
            stages,
            STAGE_ASSEMBLY,
            "RPT-ASSEMBLY",
            "assembler did not return an AssessmentModel",
            revision,
            config,
        )
    stages.append(StageOutcome(STAGE_ASSEMBLY, "ok"))

    # -- Stage: Validator + Renderers via the all-or-nothing publish gate. --- #
    publish = publish_assessment(
        model,
        config.renderers,
        output_dir=None if config.dry_run else output_dir,
        manifest_builder=config.manifest_builder,
    )
    if not publish.published:
        if not publish.validation.valid:
            exit_code = EXIT_VALIDATION_FAILED
            code = "RPT-VALIDATION-FAILED"
            detail = "canonical model failed validation; nothing published"
        else:
            exit_code = EXIT_RENDER_PARITY_FAILED
            code = "RPT-RENDER-PARITY-FAILED"
            detail = "rendering or cross-artifact parity failed; nothing published"
        stages.append(StageOutcome(STAGE_PUBLISH, "failed", detail, code))
        return PipelineResult(
            exit_code=exit_code,
            published=False,
            dry_run=config.dry_run,
            stages=tuple(stages),
            revision=revision,
            model=model,
            publish_result=publish,
            findings=publish.findings,
            error_code=code,
            detail=detail,
        )

    detail = "dry run: validated and rendered, nothing written" if config.dry_run else (
        f"published {len(publish.written_paths)} artifacts"
    )
    stages.append(StageOutcome(STAGE_PUBLISH, "ok", detail))
    return PipelineResult(
        exit_code=EXIT_OK,
        published=publish.published and not config.dry_run,
        dry_run=config.dry_run,
        stages=tuple(stages),
        revision=revision,
        model=model,
        publish_result=publish,
        findings=(),
        detail=detail,
        written_paths=publish.written_paths,
    )


def _stage_failure(
    stages: list[StageOutcome],
    name: str,
    code: str,
    detail: str,
    revision: AssessmentRevision | None,
    config: PipelineConfig,
) -> PipelineResult:
    """Record a failed analytic stage and return a fail-closed pipeline result."""

    stages.append(StageOutcome(name, "failed", detail, code))
    return PipelineResult(
        exit_code=EXIT_PIPELINE_ERROR,
        published=False,
        dry_run=config.dry_run,
        stages=tuple(stages),
        revision=revision,
        error_code=code,
        detail=f"{code}: {detail}",
    )
