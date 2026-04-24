# Nebula Contract Tests

This directory hosts the automated contract test system for Nebula CLI and diagnostics.

## Quick start

From repo root (`/Users/bill/Desktop/nebula/nebula`):

```bash
cmake -S . -B build
cmake --build build -j
python3 tests/run.py --suite all --report text
```

Useful filters:

```bash
python3 tests/run.py --suite run --report text
python3 tests/run.py --suite check --filter 'CHK-*' --report json
python3 tests/run.py --filter 'RUN-00[1-6]*' --keep-temp
```

## Harness entrypoint

- `python3 /Users/bill/Desktop/nebula/nebula/tests/run.py`
- Options:
  - `--suite all|check|build|run|test|bench|safety`
  - `--filter <glob>`
  - `--report text|json|junit`
  - `--perf-json-out <path>` (non-gating performance summary)
  - `--perf-top <N>` (slowest-case rows in perf summary)
  - `--keep-temp`
  - `--timeout <seconds>` (default per-step timeout; timed-out steps return `124`)
  - `--binary <path>` (optional override)

Perf baseline diff helper:

```bash
python3 scripts/perf_baseline_diff.py \
  --current /tmp/current_perf.json \
  --baseline /tmp/baseline_perf.json \
  --out-json /tmp/perf_diff.json \
  --out-md /tmp/perf_diff.md
```

Useful flags:
- `--fail-on-regression on|off`
- `--max-total-regression-pct`
- `--max-suite-regression-pct`
- `--max-case-regression-ms`
- `--min-case-duration-ms`
- `--max-cross-stage-reused-drop`
- `--max-cross-stage-saved-ms-drop`
- `--max-disk-hit-drop`
- `--max-disk-miss-increase`
- `--max-disk-eviction-increase`
- `--max-grouping-total-ms-increase`
- `--max-grouping-budget-fallback-increase`

## Case format (`case.toml`)

Top-level (single-step shorthand):

```toml
id = "CHK-001-profile-auto-deep"
suite = "check"
command = "check"
source = "fixtures/complex_fn.nb"
args = ["--diag-format", "json"]
expect_rc = 0

[[expect_diag]]
code = "NBL-C001"
stage = "build"
```

Multi-step form:

```toml
id = "RUN-007-reuse-hit"
suite = "run"

[[steps]]
kind = "nebula"
command = "run"
source = "fixtures/smoke.nb"
args = ["--diag-format", "json", "--run-gate", "none", "-o", "artifacts/reuse_hit.out"]
expect_rc = 0

[[steps]]
kind = "shell"
run = "echo '//touch' >> fixtures/smoke.nb"
expect_rc = 0
```

Supported assertion fields per step:

- `expect_rc`
- `timeout` (per-step override; shell steps are killed as a process group/process tree on timeout
  so nested Python/`nebula run` children do not survive the failed step)
- `expect_stdout_contains[]`
- `forbid_stdout_contains[]`
- `expect_stdout_regex[]`
- `expect_diag[]` / `forbid_diag[]`
  - match keys: `code`, `stage`, `severity`, `risk`, `category`, `predictive`, `confidence_min`, `confidence_max`
- `require_diag_keys[]`
- `must_exist[]` / `must_not_exist[]`

Shell step runtime environment:
- `NEBULA_BINARY`: resolved nebula binary path from harness
- `NEBULA_REPO_ROOT`: repository root (`tests/..`)
- `NEBULA_TESTS_ROOT`: tests directory path

## Directory layout

```text
tests/
  run.py
  requirements.txt
  lib/
  fixtures/
  cases/
    check/ build/ run/ test/ bench/ safety/
  artifacts/
```

## Contract coverage map

### CLI contract (`spec/cli_contract.md`)

- Profile auto/default mapping: `CHK-001`, `CHK-002`
- global `--help`/`--version` behavior: `CHK-072`, `CHK-073`, `CHK-108`
- option parsing rejects unknown flags across all subcommands: `CHK-070`, `BLD-006`, `RUN-016`, `TST-003`, `BEN-003`
- parse error missing/invalid value coverage: `CHK-074`, `CHK-075`, `CHK-107`, `BLD-008`, `RUN-018`, `RUN-027`, `RUN-028`, `RUN-029`, `TST-004`, `BEN-004`
- removed `--san` path (unknown option): `CHK-071`
- intelligent diagnostics option coverage:
  `CHK-076`, `CHK-077`, `CHK-078`, `CHK-079`, `CHK-080`,
  `CHK-083`, `CHK-084`, `CHK-085`, `CHK-086`, `CHK-087`,
  `CHK-088`, `CHK-089`, `CHK-090`, `CHK-091`, `CHK-092`, `CHK-093`, `CHK-094`,
  `CHK-095`, `CHK-096`, `CHK-097`, `CHK-098`, `CHK-099`, `CHK-100`, `CHK-101`, `CHK-102`,
  `CHK-103`, `CHK-104`, `CHK-105`, `CHK-106`, `CHK-107`,
  `BLD-010`, `RUN-019`, `RUN-021`, `RUN-023`, `RUN-024`, `RUN-025`,
  `RUN-026`, `RUN-027`, `RUN-028`, `RUN-029`, `RUN-039`, `RUN-040`, `RUN-041`,
  `RUN-042`, `RUN-043`, `RUN-044`, `RUN-045`, `RUN-046`, `RUN-047`, `RUN-048`
- JSON grouped/raw compatibility coverage: `CHK-081`, `CHK-082`
- cache observability (`--cache-report`): `TST-005`, `TST-006`
- baseline script outputs (CSV/JSON): `TST-007`
- perf baseline diff helper pass/fail/schema paths: `TST-008`, `TST-009`, `TST-010`
- cross-stage baseline chained perf diff script path: `TST-011`
- perf baseline diff cache-signal regression fail path: `TST-012`
- perf baseline diff grouped-signal fail/warn paths: `TST-013`, `TST-014`
- cross-stage baseline script grouped-threshold fail/warn passthrough: `TST-015`, `TST-016`
- cache-report JSON line shape is strict single-object output (no trailing chars): `RUN-020`, `RUN-022`, `RUN-024`, `RUN-025`, `TST-006`
- disk cache v1 behavior (`--disk-cache*`): `RUN-030`, `RUN-031`, `RUN-032`, `RUN-033`, `RUN-034`, `RUN-035`, `RUN-036`, `RUN-037`, `RUN-038`
- cross-stage cache direction breakdown: `RUN-020`
- cross-stage reuse experiment (`--cross-stage-reuse safe`): `RUN-022`
- cross-stage reuse off-vs-safe baseline pair: `RUN-025`
- run-only options rejected by `build`: `BLD-009`
- `run` preflight fixed-fast: `RUN-001`, `RUN-005`, `RUN-006`
- `run-gate` semantics: `RUN-002`, `RUN-003`, `RUN-004`, `RUN-042`, `RUN-043`, `RUN-044`, `RUN-045`
- `run` exit-code propagation: `RUN-014`, `RUN-017`
- artifact reuse/no-build/out path handling: `RUN-007`..`RUN-018`, `BLD-004`
- package-manager exact-version/path/workspace/local-registry publish coverage:
  `TST-020`, `TST-021`, `TST-024`, `TST-025`, `TST-032`, `TST-033`, `TST-034`, `TST-035`,
  `TST-036`, `TST-037`, `TST-038`, `TST-039`, `TST-040`
- install/release smoke coverage:
  `TST-039`, `TST-040`, `TST-041`, `TST-042`
- platform/docs/harness stability contracts:
  `TST-238`, `TST-239`, `TST-280`, `TST-281`, `TST-282`
- experimental system-profile gates:
  `CHK-204`, `CHK-205`, `CHK-206`, `CHK-207`, `CHK-208`, `CHK-209`, `CHK-210`,
  `CHK-211`, `CHK-212`, `BLD-011`, `BLD-012`, `RUN-080`

### Diagnostics contract (`spec/diagnostics.md`)

- schema fields + JSON shape (including `warning_dimension`/`warning_reason`/`gate_weight`): `CHK-003`, `SAF-002`
- severity/risk separation (`--warnings-as-errors`): `CHK-004`
- baseline code families:
- `NBL-R001`: `CHK-005`, `SAF-002`, `CHK-127`, `CHK-128`, `CHK-129`, `CHK-131`, `CHK-132`, `CHK-134`, `CHK-136`, `CHK-137`, `CHK-138`, `CHK-139`, `CHK-140`, `CHK-141`, `CHK-142`, `CHK-143`, `CHK-144`, `CHK-145`, `CHK-146`, `CHK-147`, `CHK-148`, `CHK-149`, `CHK-150`, `CHK-151`, `CHK-152`, `CHK-153`, `CHK-154`
- `NBL-S101`: `SAF-001`
- `NBL-P001/NBL-P010/NBL-X001/NBL-X003`: `RUN-002`, `RUN-003`, `RUN-042`, `RUN-043`, `CHK-130`, `CHK-133`, `CHK-135`, `CHK-137`, `CHK-138`, `CHK-141`, `CHK-145`, `CHK-146`, `CHK-148`, `CHK-150`, `CHK-151`, `CHK-153`
- `NBL-C001/NBL-C010`: `CHK-001`, `RUN-006`, `RUN-044`, `RUN-045`
- `NBL-PR002` (budget skip-lint advisory): `CHK-080`
- `NBL-U001/NBL-U002`: `SAF-003`, `SAF-006`
- callable/type boundary checks:
  - `NBL-U001` (indirect): `SAF-007`
  - callable arity/type: `CHK-007`
  - strict safe/unsafe callable typing: `CHK-008`
  - indirect call allow-paths: `SAF-009`, `SAF-010`
- field/method/assignment boundary checks:
  - field read/write + method sugar happy paths: `CHK-009`, `CHK-010`, `CHK-013`
  - `NBL-T080..NBL-T086`: `CHK-011`, `CHK-012`, `CHK-014`, `CHK-015`, `CHK-016`, `CHK-017`
  - unsafe mapped method call gate: `SAF-011`, `SAF-012`
- ref exclusivity boundary checks:
  - `NBL-T090` (ref/ref alias conflict): `CHK-018`, `CHK-019`, `CHK-020`
  - `NBL-T091` (ref/non-ref overlap): `CHK-021`, `CHK-023`
  - `NBL-T092` (same-statement borrow window conflict): `CHK-024`, `CHK-025`, `CHK-026`, `CHK-027`
  - statement separation / nested non-conflict baselines: `CHK-028`, `CHK-029`
  - non-conflict baseline: `CHK-022`
- field-sensitive ref alias refinement:
  - `ref` field lvalue accepted (`x.f`): `CHK-030`
  - same-field overlap conflicts (`T090`/`T092`): `CHK-031`, `CHK-032`
  - distinct-field non-conflict baseline: `CHK-033`
  - whole-vs-field overlap preserved: `CHK-034`
  - `ref self` whole-object overlap preserved: `CHK-035`
- escape-risk cross-statement borrow window:
  - safe direct call baseline (no cross-statement borrow): `CHK-036`
  - cross-statement read/write/reborrow conflicts: `CHK-037`, `CHK-038`, `CHK-039`
  - resolved indirect `KnownNoEscape` baseline (forbid `T093/T094/T095`): `CHK-040`
  - loop propagation to outer scope: `CHK-041`
  - `unsafe`/`region` no-outward-propagation baselines: `CHK-042`, `CHK-043`
  - same-statement priority guard (`T092` only; forbid `T093/T094/T095`): `CHK-044`
  - field-sensitive cross-statement distinct/overlap checks: `CHK-045`, `CHK-046`
  - shadowed-binding false-positive guard: `CHK-047`
  - same-call `T095` regression guard (`expect T090`, forbid `T095`): `CHK-048`
- summary-aware direct/indirect borrow window:
  - direct `KnownNoEscape` baseline: `CHK-049`
  - direct `KnownMayEscape` read/write conflicts: `CHK-050`, `CHK-051`
  - direct -> indirect reborrow chain via token/origin: `CHK-052`
  - `@unsafe` direct conservative extension: `CHK-053`
  - return-path conservative guard: `CHK-054`
  - unresolved indirect fallback remains active (callable assign kill): `CHK-055`
  - `let` alias token inheritance guard: `CHK-056`
  - resolved indirect `KnownMayEscape`: `CHK-057`
  - resolved indirect `@unsafe` conservative extension: `CHK-058`
  - ref-subset guard (non-ref may-escape must not extend): `CHK-059`
  - target-known + summary-unknown conservative extension: `CHK-060`
  - CFG loop join unknown guard: `CHK-061`
- SCC/profile split recursive summary coverage:
  - recursive `KnownNoEscape` deep baseline: `CHK-062`
  - recursive `KnownMayEscape` read/write/reborrow: `CHK-063`, `CHK-064`, `CHK-065`
  - mutual recursion summary-unknown conservative guard: `CHK-066`
  - resolved indirect recursive no-escape deep baseline: `CHK-067`
  - same-statement priority guard under recursion (fast/deep): `CHK-068`
  - profile split guard (fast conservative, deep precise): `CHK-069`

## CI usage

Single-platform CI entry:

```bash
python3 tests/run.py --suite all --report text \
  --text-out contract-tests.txt \
  --junit-out contract-tests.junit.xml \
  --json-out contract-tests.json \
  --perf-json-out contract-tests.perf.json
```

Recommended artifact uploads:
- text summary report
- JUnit XML report
- JSON report from `--report json`
- optional performance summary JSON from `--perf-json-out` (non-gating)

## Debugging failures

- Use `--keep-temp` to preserve per-case sandboxes.
- Failing report lines include sandbox path.
- Re-run one case quickly:

```bash
python3 tests/run.py --filter 'RUN-011*' --keep-temp
```
