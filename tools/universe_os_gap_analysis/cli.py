"""Command-line boundary for the repository-only assessment pipeline.

This module is intentionally thin: it parses arguments, validates the read-only
path contract, and delegates the deterministic Binder -> Inventory ->
Collector/Normalizer -> Claim Guard -> Evaluators -> Maturity -> Validator ->
Renderer pipeline to :mod:`tools.universe_os_gap_analysis.pipeline`. The pipeline
owns the fail-closed exit-code contract; ``main`` simply reports a deterministic
JSON summary and returns the pipeline's exit code.

The execution policy is explicit and read-only by default: no network, no
external command execution, repository reads only, and writes confined to the
explicit ``--output-dir`` (and only on a successful, non-dry-run publish).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Sequence

from . import pipeline
from .pipeline import PipelineConfig, PipelineResult, read_only_execution_policy, run_pipeline

# Explicit, read-only default policy surfaced at the CLI boundary.
NETWORK_ENABLED = pipeline.NETWORK_ENABLED
EXTERNAL_COMMANDS_ENABLED = pipeline.EXTERNAL_COMMANDS_ENABLED


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="universe-os-gap-analysis",
        description="Read-only Nebula repository evidence assessment.",
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
        required=True,
        help="Explicit directory reserved for generated assessment artifacts.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Run the full pipeline (bind, collect, evaluate, validate, render) "
        "without writing any assessment artifacts.",
    )
    return parser


def validated_paths(repo_root: Path, output_dir: Path) -> tuple[Path, Path]:
    repo = repo_root.expanduser().resolve(strict=True)
    if not repo.is_dir():
        raise ValueError(f"repository root is not a directory: {repo}")
    output = output_dir.expanduser().resolve(strict=False)
    if output == repo:
        raise ValueError("output directory must not be the repository root")
    return repo, output


def _summary(repo: Path, output: Path, result: PipelineResult) -> dict[str, object]:
    """Build the deterministic JSON summary printed by :func:`main`."""

    if result.ok:
        status = "assessment-published" if result.published else "dry-run-valid"
    else:
        status = "assessment-failed"
    return {
        "dryRun": result.dry_run,
        "errorCode": result.error_code,
        "exitCode": result.exit_code,
        "externalCommandsEnabled": EXTERNAL_COMMANDS_ENABLED,
        "findingCodes": sorted({finding.code for finding in result.findings}),
        "networkEnabled": NETWORK_ENABLED,
        "outputDirectory": str(output),
        "published": result.published,
        "repositoryRoot": str(repo),
        "stages": [
            {
                "name": stage.name,
                "status": stage.status,
                "errorCode": stage.error_code,
            }
            for stage in result.stages
        ],
        "status": status,
        "writtenPaths": list(result.written_paths),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        repo, output = validated_paths(args.repo_root, args.output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    config = PipelineConfig(
        repo_root=repo,
        output_dir=output,
        dry_run=args.dry_run,
        execution_policy=read_only_execution_policy(),
    )
    result = run_pipeline(config)
    print(json.dumps(_summary(repo, output, result), sort_keys=True, separators=(",", ":")))
    return result.exit_code
