# Diagnostics contract

Nebula diagnostics are structured, stage-aware, and stable enough for CLI + CI integration.

## Core model

Each diagnostic carries at least:
- `code` (stable)
- `category` (stable bucket)
- `severity` (`error|warning|note`)
- `risk` (`low|medium|high|critical`)
- source `span`

Optional enrichment fields:
- `stage` (`preflight|build`)
- `cause`
- `impact`
- machine-readable classification:
  - `machine_reason`
  - `machine_subreason`
  - `machine_detail`
  - `machine_trigger_family`
  - `machine_trigger_subreason`
  - `machine_owner`
  - `machine_owner_reason`
  - `machine_owner_reason_detail`
  - `caused_by_code`
- `suggestions[]`
- `predictive` + `confidence`
- `related_spans[]`
- warning taxonomy enrichment:
  - `warning_dimension`
  - `warning_reason`
  - `gate_weight`

## Severity vs risk

- **Severity** is compile-time semantics (`error|warning|note`).
- **Risk** is impact assessment (runtime/safety/perf) and is independent from severity.

Example: a warning can still be high-risk.

`--warnings-as-errors` only upgrades warning severity to error; it does **not** change `risk`.

## Output formats

- `--diag-format text`: human-readable multi-line output.
- `--diag-format json`: machine-readable output including enrichment fields.
- grouped JSON (`--diag-view grouped`) keeps `summary/roots/derived/causal_edges` and appends
  root-cause compression v2 sections:
  - `top_root_causes[]`
  - `fix_order[]`
  - each `top_root_causes[]` entry can include follow-up chain hints:
    - `followup_count`
    - `followup_codes[]`
    - `followup_fix_hints[]` (`{code,hint}` in causal-chain order)
    - `priority_machine_reason`
    - `priority_machine_boost`
    - `priority_machine_action`
    - `priority_epistemic_reason`
    - `priority_epistemic_boost`
    - `priority_epistemic_action`
    - `priority_owner_reason`
    - `priority_owner_boost`
    - `priority_owner_weight`
    - `priority_owner_action`
    - `priority_owner_plan[]` (`{reason,boost,count,action}` sorted by repair priority)
    - `priority_fix_plan[]` (`{category,reason,boost,count,action}` merged repair queue
      sorted by boost/count so owner fanout-first cleanup stays ahead of machine/epistemic follow-up)
    - `machine_owner_reason_breakdown[]` (`{name,count}` tuples sorted by count desc,
      span-deduped so follow-up diagnostics on the same site do not inflate owner heat)
    - `machine_reason_breakdown` (`return|call|field` counts from covered `NBL-R001` chain)
    - `machine_subreason_breakdown[]` (`{name,count}` tuples sorted by count desc)
    - `machine_detail_breakdown[]` (`{name,count}` tuples using
      `<machine_reason>/<machine_subreason>`, sorted by count desc)
    - `priority_chain_summary` (for example `NBL-P010->NBL-X003` or transitive
      `NBL-R001->NBL-P010->NBL-X003` when owner inference feeds epistemic follow-up on
      the same localized path)
  and grouped diagnostic perf instrumentation in `summary`:
  - `grouping_total_ms`
  - `grouping_index_ms`
  - `grouping_rank_ms`
  - `grouping_root_cause_v2_ms`
  - `grouping_emit_ms`
  - `grouping_budget_fallback`
- grouped budget fallback keeps top-level shape as `summary + all[]` for compatibility.

## Stage semantics

- `preflight` diagnostics are emitted by `nebula run` preflight pass.
- `build` diagnostics are emitted during full build/check/test/bench analysis.
- `NBL-T093/NBL-T094/NBL-T095` are cross-statement borrow-window diagnostics; same-statement
  conflicts must remain `NBL-T092`.

## Stable code families

- `NBL-Txxx`: typecheck
- `NBL-PARxxx`: parser/lexer
- `NBL-Rxxx`: region and memory-safety rules
- `NBL-Uxxx`: unsafe boundary rules
- `NBL-Sxxx`: safe-subset structural ownership rules
- `NBL-Pxxx`: performance lints
- `NBL-Axxx`: API lifecycle/deprecation lints
- `NBL-Cxxx`: structural complexity lints
- `NBL-Xxxx`: predictive diagnostics
- `NBL-CLI-*`: CLI orchestration/runtime diagnostics

## v0.2 baseline codes

- `NBL-R001`: region escape; auto-promoted
  - `machine_reason` splits root trigger: `return|call|field`
  - `machine_subreason` carries finer trigger subtype (for example
    `return-via-alias-chain`, `cross-function-return-path`,
    `callee-param-escape`, `callee-param-escape-unknown-no-summary`,
    `callee-param-escape-unknown-external-opaque`,
    `callee-param-escape-unknown-indirect-unresolved`,
    `field-write-base-escape`)
    - unknown cross-function return path is split into
      `cross-function-return-path-unknown-no-summary`,
      `cross-function-return-path-unknown-external-opaque`,
      `cross-function-return-path-unknown-indirect-unresolved`
      (`...-unknown-no-summary` also covers summary-known-but-precision-unknown
      recursive/unstable callee paths)
  - `machine_detail` is a stable machine-readable path `<machine_reason>/<machine_subreason>`
    (for example `call/callee-param-escape`)
  - `machine_trigger_family`/`machine_trigger_subreason` mirror the escape-trigger split in
    normalized fixed slots (`return|call|field` + detailed subtype) so downstream tools can
    consume trigger taxonomy without overloading generic machine fields
  - `machine_trigger_family_detail` is a stable `|`-joined family set (`return|call|field`)
    capturing mixed-trigger roots; the first slot matches `machine_trigger_family`
  - `machine_owner` records inferred promotion owner (`heap-unique|heap-shared`)
  - `machine_owner_reason` records owner selection cause
    (`single-owner-flow|alias-fanout|cross-function-return-path-alias-fanout|cross-function-return-path-alias-fanout-mixed|cross-function-return-path-fanin|`
    `cross-function-return-path-unknown-no-summary|`
    `cross-function-return-path-unknown-external-opaque|`
    `cross-function-return-path-unknown-indirect-unresolved`)
    where `alias-fanout` is keyed by distinct escaping alias leaves per alias root
    and `cross-function-return-path-alias-fanout` marks fanout localized to return-path arguments,
    while `cross-function-return-path-alias-fanout-mixed` marks fanout that overlaps return-path
    aliases and other escaping alias leaves
  - `machine_owner_reason_detail` preserves all split unknown return-path source tags as a
    stable `|`-joined list (priority order:
    `...-unknown-indirect-unresolved|...-unknown-external-opaque|...-unknown-no-summary`)
    so grouped root-cause ranking can keep secondary unknown causes visible even when
    `machine_owner_reason` keeps only the highest-priority trigger
  - follow-up diagnostics (`NBL-P010`, `NBL-X003`) inherit this field from their source
    `NBL-R001` trace so epistemic-loop tooling keeps the same unknown-source split fidelity
  - `NBL-R001` includes cause-specific first-fix suggestions to make grouped root-cause ordering
    actionable (fanout cleanup first, then unknown-path hardening)
  - grouped root-cause v2 ranking uses `machine_owner_reason` as priority weight:
    fix `alias-fanout` roots first, then `cross-function-return-path-alias-fanout`,
    then `cross-function-return-path-alias-fanout-mixed`,
    then unknown return-path roots
  - grouped top-root-cause JSON reflects this weight directly via
    `priority_owner_reason` + `priority_owner_boost`, plus
    `priority_owner_action` as the next concrete fix step
    (`priority_owner_reason` is selected from covered-chain owner reasons by max priority)
  - grouped ranking now also emits `priority_owner_weight` (derived from the sorted
    `priority_owner_plan[]` queue and folded into root score), so roots with
    fanout+unknown mixed causes are prioritized ahead of unknown-only roots
  - grouped top-root-cause JSON also exposes the repair queue as
    `priority_owner_plan[]` (`alias-fanout`/return-path fanout first, unknown-path causes next),
    and root selection tie-breaks on this queue so fanout-first ordering is stable when score/risk
    are otherwise equal
  - grouped owner-priority ranking canonicalizes legacy `cross-function-return-path-unknown`
    into `cross-function-return-path-unknown-no-summary` so repair queues remain split by
    unknown source type
  - grouped top-root-cause JSON exposes owner-cause heat map as
    `machine_owner_reason_breakdown[]` (count-desc sorted, span-deduped across covered chain);
    for `NBL-R001`, unknown
    return-path subreasons are folded in even when primary owner reason is fanout so
    fanout-first then unknown-path repair order stays explicit
  - grouped top-root-cause JSON also exposes machine-readable escape trigger splits for
    covered `NBL-R001` chains:
    - `machine_reason_breakdown.return|call|field`
    - `machine_subreason_breakdown[]` (sorted fix-surface heat map)
    - `machine_detail_breakdown[]` (sorted path heat map; span-deduped by detail+site)
  - grouped ranking also materializes `NBL-R001` trigger-family urgency through
    `priority_machine_reason` + `priority_machine_boost` + `priority_machine_action`
    so return-path escapes are queued before call-path, then field-path hardening
- `NBL-U001`: safe context calls unsafe-callable (directly/indirectly/method-sugar) without `unsafe` context
- `NBL-U002`: `@unsafe` annotation used on non-function item
- `NBL-U003`: external escape/ownership contract annotation is invalid or attached to a non-`extern fn` boundary
- `NBL-T065`: callee is not callable
- `NBL-T066`: callable arity mismatch
- `NBL-T067`: callable argument type mismatch
- `NBL-T080`: unknown field
- `NBL-T081`: field access on non-struct value
- `NBL-T082`: assignment type mismatch (including field assignment mismatch)
- `NBL-T083`: invalid assignment target
- `NBL-T084`: unknown mapped method (`Type_m`)
- `NBL-T085`: mutating `self` requires `self: ref T`
- `NBL-T086`: mapped method signature invalid (`self: T/ref T` required in slot 0)
- `NBL-T122`: cannot infer type arguments for constructor
- `NBL-T123`: cannot infer type arguments for function
- `NBL-T124`: generic `extern fn` declarations are rejected
- `NBL-T125`: postfix `?` requires the enclosing function to return `Result<T, E>`
- `NBL-T126`: postfix `?` operand must have type `Result<T, E>`
- `NBL-T127`: postfix `?` propagated error type must match the enclosing `Result<_, E>`
- `NBL-T128`: `@export/@abi_c` annotations are only valid on functions
- `NBL-T129`: C ABI export requires both `@export` and `@abi_c`, and cannot target `extern fn`
- `NBL-T130`: C ABI export does not support generics or `ref` parameters
- `NBL-T131`: C ABI export parameter/return type is not ABI-safe in the current slice
- `NBL-T132`: `await` is only valid inside async functions
- `NBL-T133`: `await` operand must have type `Future<T>` or `Task<T>`
- `NBL-T134`: phase-1 async functions reject suspension with `ref` parameters
- `NBL-CLI-CABI-HOSTCXX`: library build rejects `host_cxx` sources because the current public ABI
  contract is limited to Nebula-defined exported wrappers
- `NBL-CLI-CABI-NOEXPORT`: `build --emit staticlib|sharedlib` found no root-package
  `@export @abi_c` functions to publish
- `NBL-CLI-CABI-CONFLICT`: sanitized public C ABI symbol names collided
- `NBL-CLI-AR-MISSING`: static library build could not find `llvm-ar`/`ar`
- `NBL-T090`: mutable ref alias conflict (`ref`/`ref` overlap on same alias location in one call)
- `NBL-T091`: mutable ref overlaps non-ref argument on same alias location
- `NBL-T092`: same-statement borrow conflict after an active `ref` borrow
- `NBL-T093`: cross-statement borrow conflict on read after an active `ref` borrow
- `NBL-T094`: cross-statement borrow conflict on write after an active `ref` borrow
- `NBL-T095`: cross-statement borrow conflict on re-borrow after an active `ref` borrow
  (same-call parameter overlap must stay on `NBL-T090/NBL-T091`)
- `NBL-S101`: analyzable strong ownership cycle rejected in safe subset
- `NBL-P001`: heap allocation inside loop
- `NBL-P010`: shared ownership in hot path
  - when shared ownership is inferred from region promotion, emits:
    - `machine_trigger_family` + `machine_trigger_family_detail` + `machine_trigger_subreason`
      inherited from source `NBL-R001`
    - `machine_owner=heap-shared`
    - `machine_owner_reason=<owner-cause>`
    - `caused_by_code=NBL-R001`
- `NBL-A001`: deprecated API usage may reduce optimization quality
- `NBL-C001`: function structure complexity warning
- `NBL-C010`: explicit style-drift marker warning
- `NBL-X001`: predictive warning for likely performance regression
- `NBL-X002`: predictive note for trend-level complexity pressure
- `NBL-X003`: predictive note when shared ownership in hot path is inferred (not explicit)
  - emits `caused_by_code=NBL-P010` so grouped root-cause output can place it as follow-up signal
  - sets `machine_subreason=inferred-shared-hot-path` for stable downstream filters
  - propagates `machine_owner_reason` from promotion trace when available
  - propagates `machine_trigger_family` + `machine_trigger_subreason` from promotion trace when
    available
  - propagates `machine_trigger_family_detail` from promotion trace when available
  - grouped root-cause v2 can surface transitive repair chains in
    `top_root_causes[].priority_chain_summary` (for example `NBL-R001->NBL-P010->NBL-X003`),
    scoped to span-local causal matches to avoid cross-site chain pollution
  - grouped root-cause v2 also exports ordered action links in
    `top_root_causes[].followup_fix_hints[]` so `NBL-P010 -> NBL-X003` can be consumed as
    a machine-readable remediation chain
  - grouped root-cause v2 now emits explicit epistemic-priority signals:
    - `priority_epistemic_reason`
    - `priority_epistemic_boost`
    - `priority_epistemic_action`
    so the minimal loop (`NBL-R001 -> NBL-P010 -> NBL-X003`) is represented as a concrete
    repair-order hint, not just a passive follow-up chain
- `NBL-PR001`: preflight latency budget note (advisory)
- `NBL-PR002`: analysis budget note indicating optional lint was skipped

## Exit behavior

- For `check/build/test/bench`: any `error` returns non-zero.
- For `run`: preflight gate behavior is defined in `spec/cli_contract.md`.
- For `run`: if analysis/build succeeds and execution starts, the process exit code mirrors the
  executed artifact exit code.
- Command-line parse failures for any subcommand return exit code `2` and are reported as plain CLI
  errors (not structured diagnostics). Unknown flags include `unknown option: <flag>`.
- Global flags `--help`/`-h`/`--version` are successful CLI paths and return `0`.

## Unsafe boundary risk semantics

`unsafe-boundary` diagnostics default to `risk=High` in v0.2 to emphasize required manual
audit at boundary crossings or misannotations. Future versions may refine specific codes to
lower risk tiers without breaking this contract.

For cross-statement borrow diagnostics, `cause` should explain whether the active borrow came
from known MayEscape summary data or from an unknown/conservative source (unsafe boundary,
indirect call target unknown path, unresolved external summary, summary-unknown precision,
or unknown origin chain).

Cause trigger rules (hard contract):
- `TargetUnknown`: call target is unresolved (resolved-indirect lookup failed)
- `UnknownExternal`: call target is known but no summary exists (external/builtin/opaque)
- `SummaryUnknown`: call target is known and summary exists, but parameter state is Unknown
- `UnknownUnsafeBoundary`: target function is `@unsafe` (direct or resolved-indirect), always conservative
- `UnknownOrigin`: only for `ref` arguments where origin cannot be reduced to a tracked caller source
  (non-`ref` arguments must not emit this cause path)

Summary mapping invariant (hard contract):
- `unknown=true` must imply `may_escape=true`

## Contract test mapping (v0.2)

- Schema field presence and JSON shape: `CHK-003`, `SAF-002`
- Severity vs risk (`warnings-as-errors`): `CHK-004`
- `NBL-R001`: `CHK-005`, `SAF-002`, `CHK-127`, `CHK-128`, `CHK-129`
- `NBL-S101`: `SAF-001`
- `NBL-U001`: `SAF-003`, `SAF-007`, `SAF-011`
- `NBL-U002`: `SAF-006`
- `NBL-T065/NBL-T066/NBL-T067`: `CHK-007`, `CHK-008`
- `NBL-T080..NBL-T086`: `CHK-009`..`CHK-017`
- `NBL-T090/NBL-T091`: `CHK-018`..`CHK-023`, `CHK-032`, `CHK-034`
- same-call overlap must not leak to cross-statement codes: `CHK-048`
- `NBL-T092`: `CHK-024`..`CHK-029`, `CHK-031`, `CHK-035`
- `NBL-T093/NBL-T094/NBL-T095`: `CHK-037`..`CHK-039`, `CHK-041`, `CHK-046`,
  `CHK-050`..`CHK-058`, `CHK-060`, `CHK-061`
- resolved indirect `KnownNoEscape` baseline (forbid `T093/T094/T095`): `CHK-040`
- summary-aware direct/indirect borrow window coverage: `CHK-049`..`CHK-061`
- unresolved indirect fallback guard: `CHK-055`
- ref-subset guard (non-ref may-escape must not extend): `CHK-059`
- SCC/profile split coverage: `CHK-062`..`CHK-069`
  - recursive no-escape deep baseline: `CHK-062`
  - recursive may-escape read/write/reborrow: `CHK-063`, `CHK-064`, `CHK-065`
  - mutual recursion summary-unknown conservative guard: `CHK-066`
  - resolved-indirect recursive no-escape deep baseline: `CHK-067`
  - recursive same-statement priority guard (fast/deep): `CHK-068`
  - fast/deep split guard (fast conservative, deep precise): `CHK-069`
- T092 priority guard (`forbid T093/T094/T095` on same-statement conflict): `CHK-044`
- binding-identity shadowing guard (`forbid T093/T094/T095`): `CHK-047`
- `NBL-P001/NBL-P010/NBL-X001`: `RUN-002`, `RUN-003`, `RUN-041`, `RUN-042`, `RUN-043`
- `NBL-C001/NBL-C010`: `CHK-001`, `RUN-006`, `RUN-044`, `RUN-045`
- CLI parse error path (non-diagnostic):
  `CHK-070`, `CHK-071`, `CHK-074`, `CHK-075`,
  `BLD-006`, `BLD-008`,
  `RUN-016`, `RUN-018`,
  `TST-003`, `TST-004`,
  `BEN-003`, `BEN-004`
