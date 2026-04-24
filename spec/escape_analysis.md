# Escape analysis

Goal: conservatively detect when values (especially region-allocated ones) may **outlive** the scope they were allocated in.

## CFG-based intraprocedural analysis

We build a control-flow graph (CFG) per function:
- sequential edges between statements
- back-edges for `for` loops

We run forward dataflow to compute a conservative provenance for each value:
- `Region(R_id)` (allocated in a region scope)
- `Heap`
- `Param(i)` (flows from a parameter)

## Conservative interprocedural summaries

We compute a call graph and solve to a fixpoint:
- For each function, compute which parameters may flow to its return value (`returns_param` summary).
- Unknown/external calls are treated conservatively (arguments may escape).

## Limitations (v0.2.x)

The escape/rep-owner model remains conservative and mostly alias-blind beyond simple variable aliasing
(`let b = a`). v0.2.x now supports field access/assignment and `ref` calls, but:
- field-sensitive points-to is not implemented for escape/rep-owner (ref/borrow diagnostics
  use single-layer field-sensitive alias keys)
- no lifetime-style borrow checker is run across statement ranges

Current mitigation: call-site `ref` exclusivity and statement-local borrow window checks are enforced
as hard type errors (`NBL-T090`/`NBL-T091`/`NBL-T092`) plus escape-risk cross-statement checks
(`NBL-T093`/`NBL-T094`/`NBL-T095`).
Alias identity for these borrow diagnostics is binding-based, so inner-scope shadowed names do not
conflict with outer bindings. `NBL-T095` remains reserved for true cross-statement re-borrows; same-call
argument overlap stays on `NBL-T090/NBL-T091`.
Current cross-statement lifetime is summary-aware for direct and resolvable indirect calls,
while unresolved/unsafe/unknown-summary paths remain conservative.
Future versions can refine this with callee escape summaries, SCC fixpoint convergence, and
deeper points-to precision.

## Summary precision compatibility (v0.3-a)

To keep existing interfaces stable while enabling richer borrow decisions:
- legacy field remains: `param_may_escape` (`bool`)
- precision companion added: `param_escape_unknown` (`bool`)

Interpretation per parameter:
- `may=false` => `KnownNoEscape`
- `may=true, unknown=false` => `KnownMayEscape`
- `may=true, unknown=true` => `Unknown`

This lets downstream borrow diagnostics distinguish proven MayEscape from conservative Unknown
without changing CLI or grammar.

## Resolved indirect targets (v0.3-b)

`let f = foo; f(...)` calls now use a shared call-target resolver (CFG forward dataflow):
- lattice per callable variable: `Known(callee)` or `Unknown`
- transfer:
  - `let v = fn_item` => `Known(fn_item)`
  - `let v = u` => `Known(k)` only when `u` is `Known(k)`, else `Unknown`
  - callable `AssignVar` (`v = ...`) => `Unknown` kill
- join:
  - `join(Known(a), Known(b)) = (a==b ? Known(a) : Unknown)`
  - `join(Unknown, x) = Unknown`

Resolver output is consumed by both escape summaries and cross-statement borrow diagnostics.
If target is unresolved (`TargetUnknown`), behavior stays conservative.
If target is resolved but summary precision is unknown (`SummaryUnknown`), behavior also stays conservative.

Borrow-window extension must be computed on `ref` argument indices only:
- extend only if any `i` satisfies `args_ref[i] && (param_may_escape[i] || param_escape_unknown[i])`

## SCC recursive summaries (v0.3-c)

v0.3-c adds SCC-aware solving for recursive and mutually-recursive call graphs.

Call graph inputs:
- direct calls
- resolved indirect calls (`let f = foo; f(...)`) from call-target resolver
- unresolved indirect calls do not add a known callee edge and stay conservative

SCC rules:
- non-trivial SCC = self-recursive node or multi-node cycle
- deep profile: run monotone fixpoint inside SCC (max 64 iterations)
- fast profile: skip SCC fixpoint and mark non-trivial SCC summaries as `Unknown`

If deep profile hits iteration cap, affected SCC params are promoted to `Unknown`.

## Profile split (hard contract)

- `fast`: predictable conservative behavior, no SCC fixpoint for non-trivial SCCs
- `deep`: precision mode, SCC fixpoint enabled to reduce false positives where provable

## Tri-state invariants (hard contract)

Internal state: `KnownNoEscape | KnownMayEscape | Unknown`.

External compatibility mapping:
- `KnownNoEscape` -> `may=false, unknown=false`
- `KnownMayEscape` -> `may=true, unknown=false`
- `Unknown` -> `may=true, unknown=true`

Invariant: `unknown=true => may=true` must always hold.

## Upgrade roadmap (post-v0.3-c)

- **v0.3-a (done)**: add callee ref-escape summary precision (`KnownNoEscape | KnownMayEscape | Unknown`)
  while keeping legacy bool compatibility
- **v0.3-b (done)**: consume summaries for resolvable indirect calls via CFG join/kill dataflow
- **v0.3-c (done)**: add SCC/profile split solving for recursive interprocedural borrow precision

## Safe-subset boundary (v0.2)

The strong leak-safety guarantee only applies inside analyzable ownership graphs.
Opaque boundaries (dynamic containers/FFI/unknown external ownership transfer) are not safe by
default and must be explicitly marked `unsafe` in future surface syntax revisions.

## External boundary contracts (v0.3-d)

Bare `extern fn` declarations are treated as opaque by default:
- arguments may escape
- return value may depend on any argument

This keeps FFI/mixed-language ownership boundaries conservative unless the package author declares
an explicit contract on the `extern fn` item:
- `@returns_fresh`
- `@returns_paramN`
- `@paramN_noescape`
- `@paramN_may_escape`
- `@paramN_escape_unknown`

Contract annotations are additive refinements:
- unspecified parameters remain `Unknown`
- return dependency stays conservative unless `@returns_fresh` or `@returns_paramN` is present
- invalid usage or conflicting annotations emit `NBL-U003`

## Unsafe block handling (v0.2.x)

`unsafe { ... }` is preserved in NIR for boundary tracking, then treated as a normal lexical
block by escape analysis and later lowered with zero runtime overhead in codegen.

## Region escape condition

Inside a region scope `R_id`, if a return expression has provenance that includes `Region(R_id)`, the region pointer would outlive the region.

The compiler then applies the **region escape policy** in `spec/region_semantics.md`.
