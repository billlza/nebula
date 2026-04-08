# Nebula v0.5.0 Release Notes

## Overview

v0.5.0 extends the intelligent CLI diagnostics contract with finer warning controls,
cross-stage reuse observability, and deterministic off-vs-safe regression coverage.

## Highlights

### Root-cause graph v1 in grouped diagnostics

- grouped output carries root ranking and causal metadata:
  - `rank_score`
  - `derived_count`
  - `caused_by`
  - `derivation_reason`
  - top-level `causal_edges[]`
- `--max-root-causes <N>` truncation remains observable via grouped summaries.

### Warning policy v2 with class overrides

- `--warn-policy strict|balanced|lenient` sets base warning visibility.
- `--warn-class <class>=on|off` (repeatable) overrides classes after base policy.
- classes: `api-deprecation`, `performance-risk`, `best-practice-drift`,
  `safety-risk`, `general-warning`, `all`.

### Diagnostics UX v2 (warning taxonomy + gate-aware run gate)

- warning diagnostics now include:
  - `warning_dimension`
  - `warning_reason`
  - `gate_weight`
- grouped diagnostics summary includes:
  - `warning_dimension_counts`
  - `gating_warning_counts`
  - `gate_profile` + `gate_dimension_policy`
- new run-only gate controls:
  - `--gate-profile strict|balanced|lenient`
  - `--gate-dimension <name>=on|off` (repeatable, name in
    `api-lifecycle|perf-runtime|best-practice-drift|safety-contract|general|all`)
- `run-gate high` now uses taxonomy + `gate_weight` threshold under effective gate profile/dimension
  instead of the previous risk/predictive-only gate heuristic.
- new contract coverage:
  - `RUN-039` invalid gate profile value
  - `RUN-040` invalid gate dimension value
  - `RUN-041` `perf-runtime` gate-dimension off allows run under `run-gate high`
  - `RUN-042` `gate-profile balanced` blocks `NBL-P001` under `run-gate high`
  - `RUN-043` `gate-profile lenient` allows the same `NBL-P001` warning to pass
  - `RUN-044` `gate-profile strict` blocks mid-weight `NBL-C010` (`gate_weight=55`)
  - `RUN-045` `gate-profile balanced` allows the same `NBL-C010`, proving strict vs balanced split

### Root-cause compression v2 (grouped diagnostics)

- new grouped controls:
  - `--root-cause-v2 auto|on|off`
  - `--root-cause-top-k <N>` (range `1..50`)
  - `--root-cause-min-covered <N>` (default `0`)
  - `--diag-grouping-delay-ms <N>` (default `0`, grouped diagnostics budget-fallback validation)
- grouped JSON now appends:
  - `summary.root_cause_v2_enabled`
  - `summary.top_root_cause_count`
  - `summary.top_root_candidate_count`
  - `summary.covered_derived_total`
  - `summary.grouping_total_ms`
  - `summary.grouping_index_ms`
  - `summary.grouping_rank_ms`
  - `summary.grouping_root_cause_v2_ms`
  - `summary.grouping_emit_ms`
  - `summary.grouping_budget_fallback`
  - `top_root_causes[]`
  - `fix_order[]`
- auto default policy:
  - `check`/`run` grouped: enabled
  - `build`/`test`/`bench` grouped: disabled
- new contract coverage:
  - `CHK-092` grouped JSON exposes v2 fields and selected root cause section
  - `CHK-093` `--root-cause-v2 off` disables v2 selection output
  - `CHK-094` `--root-cause-top-k` limits selected root-cause count
  - `CHK-095` `--root-cause-min-covered` filters candidate set
  - `CHK-096` fix-order output is stable across repeated runs
  - `CHK-097` `check` grouped auto enables v2
  - `CHK-098` grouped backward-compat (`summary/roots/derived/causal_edges`) remains intact
  - `CHK-099` raw JSON output remains free of grouped v2 fields
  - `CHK-100` invalid `--root-cause-v2` value parse failure
  - `CHK-101` invalid `--root-cause-top-k` value parse failure
  - `CHK-102` invalid `--root-cause-min-covered` value parse failure
  - `CHK-103` grouped JSON summary includes grouped perf instrumentation fields
  - `CHK-104` grouped text output includes `grouping-perf` line
  - `CHK-105` grouped JSON budget-fallback path remains `summary + all[]` with perf fields
  - `CHK-106` grouped text budget-fallback path includes `grouping-perf ... budget-fallback=on`
  - `CHK-107` invalid `--diag-grouping-delay-ms` parse failure
  - `RUN-046` `run` grouped auto enables v2
  - `RUN-047` `run` grouped JSON summary includes grouped perf instrumentation fields
  - `RUN-048` `run` grouped JSON budget-fallback path remains stable

### Run-stage cross-stage reuse experiment

- new run-only switch: `--cross-stage-reuse off|safe` (default `off`).
- `safe` enables compatible preflight/build analysis reuse.
- reused diagnostics are retagged to current stage.
- preflight-only latency advisory `NBL-PR001` is not forwarded into build-stage reuse.

### Cache report schema extensions

- `--cache-report on` now reports:
  - cross-stage candidates and direction breakdown
  - cross-stage reused and direction breakdown
  - `cross-stage-saved-ms-estimate`
- `--cache-report-format json` exposes the same fields for machine parsing.
- fixed malformed cache-report JSON line closure; output is now strict single-object JSON.
- disk cache metrics are available in cache report:
  - `disk_hits`, `disk_misses`, `disk_writes`, `disk_expired`, `disk_evictions`, `disk_entries`

### Disk cache v1 (run-only)

- new run-only controls:
  - `--disk-cache on|off`
  - `--disk-cache-ttl-sec <sec>`
  - `--disk-cache-max-entries <N>`
  - `--disk-cache-dir <path>`
  - `--disk-cache-prune`
- v1 persists analysis payloads under `.nebula-cache` with TTL and entry-cap pruning.
- cache read/parse/write failures fall back safely to local pipeline execution with note diagnostics.

### Baseline regression additions

- new contract case `RUN-025` compares `--cross-stage-reuse off` vs `safe` in one case.
- new script `scripts/run_cross_stage_baseline.sh` writes
  `benchmark_results/cross_stage_reuse_baseline.csv` and
  `benchmark_results/cross_stage_reuse_baseline.json` for local baseline checks
  without invoking build automatically.
- new test case `TST-007` validates baseline script artifact generation.
- test harness supports non-gating performance summary output via
  `python3 tests/run.py --perf-json-out <path> [--perf-top N]`.
- new script `scripts/perf_baseline_diff.py` compares current vs baseline perf summary JSON
  with threshold-based judgement and optional fail mode:
  - `--fail-on-regression on|off`
  - `--max-total-regression-pct`, `--max-suite-regression-pct`, `--max-case-regression-ms`,
    `--min-case-duration-ms`
  - cache signal regression controls:
    - `--max-cross-stage-reused-drop`
    - `--max-cross-stage-saved-ms-drop`
    - `--max-disk-hit-drop`
    - `--max-disk-miss-increase`
    - `--max-disk-eviction-increase`
  - grouped diagnostics signal regression controls:
    - `--max-grouping-total-ms-increase`
    - `--max-grouping-budget-fallback-increase`
  - optional outputs: `--out-json`, `--out-md`
- new test cases validate helper behavior:
  - `TST-008` pass path + artifact outputs
  - `TST-009` fail-on-regression path (`rc=1`)
  - `TST-010` invalid schema path (`rc=2`)
  - `TST-011` cross-stage baseline script chained perf diff path
  - `TST-012` cache-signal regression fail path
  - `TST-013` grouped-signal regression fail path
  - `TST-014` grouped-signal regression warn (non-gating) path
  - `TST-015` cross-stage baseline script grouped-threshold fail passthrough
  - `TST-016` cross-stage baseline script grouped-threshold warn passthrough
- `scripts/run_cross_stage_baseline.sh` now supports optional chained perf diff by passing
  `--perf-current` + `--perf-baseline` (plus threshold flags), while preserving legacy output.

## Validation

- Contract suite status at release cut:
  - `cases: 197`
  - `passed: 197`
  - `failed: 0`
