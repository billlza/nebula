"""Durable assessment generation entrypoint (Task 15.1).

This thin, additive module runs the *completed* read-only assessment pipeline
against the real Nebula repository using the curated repository baseline (the
Task 14.2 default assembler) and commits the versioned artifact set into a
durable, committed output directory (``tools/universe_os_gap_analysis/artifacts/``
by default). It produces the full deliverable set from the single canonical
:class:`~tools.universe_os_gap_analysis.models.AssessmentModel`:

* ``assessment.json`` and ``assessment.schema.json``;
* ``capability_matrix.csv`` / ``capability_matrix.json``;
* ``gap_register.csv`` / ``gap_register.json``;
* ``assessment.md`` (the narrative report);
* ``assessment.manifest.json`` (the digest-bound artifact manifest).

It never modifies product code, the compiler/runtime/kernel, or any spec/test;
it only *reads* the repository and *writes* the explicit output directory. The
revision binder already excludes the output directory from the worktree
fingerprint, so committing the artifacts there does not create a self-reference.

Because the live worktree can carry untracked churn (``__pycache__``,
``.pytest_cache``, ``.hypothesis``) that drifts mid-bind, generation retries only
on transient revision-drift outcomes -- exactly the pattern used by the
end-to-end integration tests -- so the deterministic model contract is preserved
without masking real failures.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Sequence

from .pipeline import (
    EXIT_OK,
    PipelineConfig,
    PipelineResult,
    read_only_execution_policy,
    run_pipeline,
)

# The default durable output directory for the committed deliverable. It lives
# inside the tool's own area (not scattered across the repo) and is excluded from
# the worktree fingerprint by the revision binder.
DEFAULT_OUTPUT_DIR = Path(__file__).resolve().parent / "artifacts"


# Transient revision-drift codes: the live repository can mutate untracked
# working-tree files while the binder captures its fingerprint. That is a
# legitimate fail-closed outcome but orthogonal to the assessment, so runs that
# hit it are retried. Any other failure is returned as-is.
_TRANSIENT_DRIFT_CODES = frozenset(
    {"REV-DRIFT", "REV-ROOT-DRIFT", "REV-VERSION-DRIFT", "REV-FINGERPRINT-DRIFT"}
)
_MAX_DRIFT_RETRIES = 8


def generate_assessment(
    repo_root: str | Path,
    output_dir: str | Path,
    *,
    dry_run: bool = False,
    max_drift_retries: int = _MAX_DRIFT_RETRIES,
) -> PipelineResult:
    """Run the full curated pipeline against ``repo_root`` and publish artifacts.

    The pipeline uses its default (curated repository baseline) assembler, so the
    conclusions are the real evidence-backed assessment of the bound revision.
    Only transient revision-drift outcomes are retried; any other non-zero exit
    is returned unchanged so real failures are never masked.

    Returns the :class:`PipelineResult`; ``result.exit_code == EXIT_OK`` and
    ``result.published`` indicate a successful durable publish.
    """

    repo = Path(repo_root).expanduser().resolve()
    output = Path(output_dir).expanduser().resolve()

    config = PipelineConfig(
        repo_root=repo,
        output_dir=output,
        dry_run=dry_run,
        execution_policy=read_only_execution_policy(),
    )

    result = run_pipeline(config)
    attempts = 0
    while (
        result.exit_code != EXIT_OK
        and result.error_code in _TRANSIENT_DRIFT_CODES
        and attempts < max_drift_retries
    ):
        attempts += 1
        result = run_pipeline(config)
    return result


def _summary(repo: Path, output: Path, result: PipelineResult) -> dict[str, object]:
    return {
        "exitCode": result.exit_code,
        "published": result.published,
        "dryRun": result.dry_run,
        "errorCode": result.error_code,
        "repositoryRoot": str(repo),
        "outputDirectory": str(output),
        "writtenPaths": list(result.written_paths),
    }


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point: generate the durable assessment deliverable."""

    parser = argparse.ArgumentParser(
        prog="universe-os-gap-analysis-generate",
        description="Generate the durable Nebula Universe OS gap assessment artifacts.",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path.cwd(),
        help="Nebula repository root to read (default: current directory).",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Durable directory for the committed assessment artifacts "
        f"(default: {DEFAULT_OUTPUT_DIR}).",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Run the full pipeline without writing any artifacts.",
    )
    args = parser.parse_args(argv)

    repo = args.repo_root.expanduser().resolve()
    output = args.output_dir.expanduser().resolve()
    result = generate_assessment(repo, output, dry_run=args.dry_run)
    print(json.dumps(_summary(repo, output, result), sort_keys=True, separators=(",", ":")))
    return result.exit_code


if __name__ == "__main__":  # pragma: no cover - module CLI shim.
    raise SystemExit(main())
