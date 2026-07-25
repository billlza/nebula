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
python3 tests/run.py --suite abi --filter 'ABI-*' --report text
python3 tests/run.py --filter 'RUN-00[1-6]*' --keep-temp
```

## Harness entrypoint

- `python3 /Users/bill/Desktop/nebula/nebula/tests/run.py`
- Options:
  - `--suite all|check|build|run|test|bench|safety|abi`
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
- `timeout` (per-step override; Windows uses an OS-enforced Job Object, while POSIX uses the
  explicit trusted-cooperative contract described below; timed-out steps return `124`)
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

Harness isolation contract:

- command output is drained concurrently and retained up to 8 MiB; a truncation marker is emitted
  after that bound, while the pipe continues to be drained to prevent backpressure
- timed-out steps return `124`; test-infrastructure failures return `125`, cannot be accepted by an
  `expect_rc = 125` assertion, preserve partial output, and stop the remaining suite
- POSIX commands run with `TRUSTED_COOPERATIVE`: the session leader remains unreaped until stable
  identities and the bounded pipe-anchor EOF oracle are sealed, preventing numeric PID/PGID reuse
  during cleanup
- native Windows commands are created suspended, attached to a kill-on-close Job Object, and only
  then resumed; `TST-281` covers timeout, success, nonzero-return, inherited-pipe, and structured
  infrastructure-failure paths
- every repository-controlled POSIX spawn that may detach or outlive its parent must pass the full
  fd stack returned by `cooperative_posix_spawn_pass_fds()` and retain those write-only pipe anchors
  until exit; nested containment preserves verified outer anchors even when the business `env` is
  empty, and rejects missing, malformed, duplicate, closed, non-pipe, or close-on-exec anchors
- a descendant that deliberately closes an anchor, sanitizes it from `pass_fds`, or launches outside
  that adapter violates the trusted contract and is not guaranteed to be found; inherited tokens are
  supplemental discovery evidence, not authentication or a sandbox
- POSIX rejects `OS_ENFORCED_RECURSIVE` before launch. Untrusted programs require a real OS sandbox,
  cgroup, VM, or privileged supervisor; Windows Job Object proof must not be conflated with POSIX
  cooperative proof

## Directory layout

```text
tests/
  run.py
  requirements.txt
  lib/
  fixtures/
  cases/
    check/ build/ run/ test/ bench/ safety/ abi/
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
- protocol install snapshot target: `ctest -R nebula-protocol-install-snapshot-tests` configures
  native-host isolated fixtures and proves production-parser canonical validation, source-tree
  mutation isolation, binary-root containment, install-time snapshot revalidation, validator
  fail-fast behavior, exact/idempotent atomic directory publication, trusted-anchor `DESTDIR`
  canonicalization (including macOS `/tmp` when it is a system symlink), conflict preservation,
  below-anchor symbolic-link and extra-entry rejection, bounded lock failure, and concurrent POSIX
  installation through one persistent parent-local coordination lock without stage residue.
  Atomic no-overwrite behavior is guaranteed only for installers that obey that fixed lock protocol
  and for an owner-private build tree with an exclusive/cooperating install anchor; a non-cooperating
  same-UID writer is outside this CMake 3.20 publisher's threat model. Raising that boundary requires
  a future evidence gate for a native dirfd/handle-based no-replace publisher. Cross-compiling this
  host-side install verifier is intentionally rejected rather than silently skipping canonical
  validation.
- hosted-registry credential process-boundary coverage: `TST-341`
- platform/docs/harness stability contracts:
  `TST-238`, `TST-239`, `TST-280`, `TST-281`, `TST-282`, `TST-329`, `TST-330`, `TST-340`
- experimental system-profile gates:
  `CHK-204`, `CHK-205`, `CHK-206`, `CHK-207`, `CHK-208`, `CHK-209`, `CHK-210`,
  `CHK-211`, `CHK-212`, `CHK-213`, `CHK-214`, `CHK-215`, `CHK-216`, `CHK-217`,
  `CHK-218`, `BLD-011`, `BLD-012`, `BLD-013`, `RUN-080`
- UniverseOS gate registry and evidence binding: `TST-329`
- experimental primitive freestanding object gate:
  `BLD-017` (ELF/symbol/metadata contract), `BLD-018` (exact request state machine), `BLD-019`
  (reachable NIR allowlist and entry selection), and `BLD-020` (determinism, fixed toolchain,
  bounded timeout, normal-exit and timeout process-group sealing, lost-`SIGCHLD` fail-fast,
  external `SIGINT`/`SIGTERM` cleanup with original signal exit semantics, no-replace transaction,
  concurrency, log escaping, hostile compiler-output types, rollback, failure cleanup, and the
  explicit Windows host-unsupported contract)
- POSIX termination-scope unit target: `ctest -R nebula-termination-signal-tests` proves that a
  pending signal cannot bypass suppression before the transaction freeze, that later signals are
  handed to the caller only after cleanup, and that caller-blocked pending signals are preserved
- POSIX compiler-containment unit target:
  `ctest -R nebula-compiler-process-containment-tests` deterministically injects group-kill,
  quiescence-audit, leader-reap, and ownership-loss failures without creating a real child; every
  unconfirmed path must return infrastructure status `125` with signal redelivery disabled
- POSIX freestanding-transaction unit target:
  `ctest -R nebula-freestanding-transaction-tests` injects an unconfirmed compiler result while a
  real SIGTERM is intercepted and proves suppression, explicit diagnostics, no publication or
  staging residue, and release of the output lock
- POSIX freestanding two-phase lifecycle target:
  `ctest -R nebula-freestanding-toolchain-signal-tests` proves the explicit session-state machine,
  rejects compiler execution after close preparation, uses a synchronous no-sleep phase barrier to
  keep post-prepare SIGTERM blocked through staging cleanup, rollback, guard disarm, and output-lock
  release, and covers committed/rolled-back restore failure plus no-second-retry cleanup guards
- hosted metadata-path conflict preservation regression: `BLD-021` proves a caller-owned
  artifact and non-file sidecar survive a rejected transaction unchanged
- hosted output alias/concurrency/signal regressions: `BLD-022` through `BLD-025`; `BLD-024`
  proves SIGINT returns 130 only after compiler-group cleanup, while `BLD-025` delivers SIGINT after
  the final host link and proves the sealed output is aborted before commit. Both preserve the
  previously published artifact and metadata and leave no private transaction/object/execution
  state
- hosted reuse content/native-header/argv0 regressions: `RUN-088`, `RUN-089`, `RUN-090`
- hosted native-dependency unit target: `ctest -R nebula-hosted-native-dependencies-tests` covers
  bounded compiler-generated Make dependency parsing, full system/user-header discovery, canonical
  path and exact digest identity, double-snapshot drift rejection, private depfile cleanup, and
  malformed/oversized/escaped-path cases
- hosted execution-lease unit target: `ctest -R nebula-verified-executable-lease-tests` covers exact
  digest acquisition, logical `argv[0]`, public-path replacement isolation, private-path conflict
  preservation, failed-acquisition cleanup, retryable cleanup, unsafe-parent rejection, executable
  permission/platform-origin policy, and actual private-copy execution. Windows builds additionally
  assert protected ACLs, 128-bit object identity, and rename/delete denial while the lease is active
- hosted object-workspace unit target: `ctest -R nebula-hosted-object-workspace-tests` covers
  owner-private creation, identity-bound recursive cleanup, replacement preservation, retry after a
  non-regular child is removed, and Windows lifetime rename/delete denial
- freestanding support C++ unit target: `ctest -R nebula-freestanding-support-tests` covers SHA-256,
  padding/block boundary vectors, bounded ELF validation, malformed ranges, W^X/allocation/
  relocation policy, escaped untrusted names, undefined symbols, missing/weak payload entry,
  forbidden payload `_start`, and a
  deterministic byte-mutation corpus
- ABI/layout hosted C++23 goldens:
  `ABI-001`, `ABI-002`, `ABI-003`, `ABI-004`, `ABI-005`

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
