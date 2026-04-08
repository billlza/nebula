# Safety contract (v1.0.0)

## Goal

Nebula v1.0.0 hardens safe-subset boundaries with explicit unsafe constructs:
- `@unsafe fn` on function items
- `unsafe { ... }` blocks for scoped opt-in

This contract turns boundary rules into compile-time enforcement.

## Core rules

| Rule | Behavior | Diagnostic |
| --- | --- | --- |
| Safe context calling unsafe-callable | Rejected | `NBL-U001` |
| `@unsafe` on non-function item | Rejected | `NBL-U002` |
| Unsafe code family reserved expansion slot | Reserved code point | `NBL-U003` |

## Callable safety metadata

Unsafe-ness is part of callable signature metadata (`is_unsafe_callable`) and is checked on the
resolved callable target. This applies to both direct calls (`foo(...)`) and indirect calls via
function values/callbacks (`f(...)`), and method sugar calls (`obj.m(...)` -> `Type_m(obj, ...)`),
preventing future drift across call paths.

Method sugar remains in direct-call coverage because `obj.m(...)` is lowered to `Type_m(obj, ...)`
before indirect-target resolution. v0.3-b indirect resolution only applies to callable variables.

## Unsafe context model

A call is allowed when either condition is true:
- current function is annotated `@unsafe`
- current statement nesting is inside `unsafe { ... }`

Otherwise, calling an unsafe-callable emits `NBL-U001` (direct or indirect).

## Ref exclusivity boundary (v0.2.x)

`ref` parameters are treated as exclusive mutable borrows at call sites. For each call
(direct/indirect/method-sugar), Nebula extracts alias keys:
- `VarRef(x)` -> `Whole(x)`
- `FieldRef(x.f)` -> `Field(x, f)` (single-layer)

Overlap rules:
- `Whole(x)` vs `Whole(x)` => overlap
- `Whole(x)` vs `Field(x, any)` => overlap
- `Field(x, f1)` vs `Field(x, f2)` => overlap only when `f1 == f2`
- different bindings => no overlap (including same textual name under shadowing)

`ref self` in mapped methods remains conservative `Whole(self)` borrow.

`ref` arguments accept lvalues in the form `x` and `x.f` (single-layer field only).

Conflicts are rejected as hard errors:
- `NBL-T090`: two `ref` arguments overlap on one alias location
- `NBL-T091`: a `ref` argument overlaps any non-`ref` argument on one alias location
- `NBL-T092`: after a `ref` borrow is established, same-statement read/write/reborrow on
  an overlapping alias location is rejected until statement end
- `NBL-T093`: later-statement read overlaps an active cross-statement borrow window
- `NBL-T094`: later-statement write overlaps an active cross-statement borrow window
- `NBL-T095`: later-statement re-borrow overlaps an active cross-statement borrow window

`NBL-T095` is only for re-borrow conflicts against borrows that were already active before the
current call. Same-call argument overlap remains `NBL-T090/NBL-T091`.

Borrow window in v0.2.x is statement-local and evaluated left-to-right:
- borrow starts when a `ref` argument is bound
- borrow ends at end of current statement
- state is reset for next statement

For escape-risk calls, Nebula also tracks cross-statement active borrow windows until current
lexical block end:
- direct call to `@unsafe` callable (always conservative)
- direct call with summary `KnownMayEscape` or `Unknown`
- resolved indirect call consumes callee summary with the same rule as direct calls
- unresolved indirect callable call (`f(...)`) stays conservative fallback
- unresolved/unknown external callable path (conservative default)

Summary precision model keeps legacy compatibility:
- `param_may_escape=false` => `KnownNoEscape`
- `param_may_escape=true && param_escape_unknown=false` => `KnownMayEscape`
- `param_may_escape=true && param_escape_unknown=true` => `Unknown`
- invariant: `param_escape_unknown=true` must imply `param_may_escape=true`

Profile split in v0.3-c is fixed:
- `fast`: non-trivial SCCs (self-recursive or mutual-recursive) are treated as `Unknown`
  without SCC fixpoint
- `deep`: non-trivial SCCs run fixpoint with iteration cap 64; overflow falls back to `Unknown`

Unknown source kinds are explicit in diagnostics:
- `TargetUnknown`: indirect call target cannot be resolved
- `UnknownExternal`: target is known but summary is unavailable
- `SummaryUnknown`: target is known but summary precision is unknown
- `UnknownUnsafeBoundary`: target is `@unsafe` (direct or resolved-indirect), always conservative
- `UnknownOrigin`: only for `ref` arguments whose origin cannot be reduced to tracked caller roots

Window extension is computed on the `ref` argument subset only:
- extend only when an index `i` satisfies
  `args_ref[i] && (param_may_escape[i] || param_escape_unknown[i])`
- non-`ref` parameters must not trigger extension by themselves

Indirect target resolution uses CFG forward dataflow with join:
- `let v = fn_item` => `Known(fn)`
- `let v = u` => `Known(k)` only if `u=Known(k)`, otherwise `Unknown`
- callable `v = ...` assignment => `Unknown` kill
- `join(Known(a), Known(b)) = (a==b ? Known(a) : Unknown)`, `join(Unknown, x)=Unknown`

`ref` borrow tracking now carries analysis-only borrow tokens:
- token is created when a ref argument enters cross-statement active window
- token may flow through simple variable aliases only (`let y = x`, `y = x`)
- conflicts may be reported by alias overlap, token overlap, or unknown-origin conservative guard

Propagation rules for nested blocks:
- `for` body borrows conservatively propagate to outer block
- `unsafe` block borrows do not propagate out
- `region` block borrows do not propagate out

Priority rule: same-statement conflicts always report `NBL-T092`; they must not be replaced by
`NBL-T093/T094/T095`.
Implementation rule: cross-statement pass compares only borrows committed from previous
statements; current-statement borrows commit at statement end.

## IR and lowering

- AST/TAST/NIR preserve `Unsafe` statement nodes through analysis passes.
- Codegen lowers `unsafe { ... }` as a normal lexical block (`{ ... }`) with zero runtime overhead.

## Safe-subset leak guarantee boundary

The v0.2 guarantee covers, inside analyzable safe subset:
- heap object leaks
- shared strong cycles in analyzable ownership graph
- region resource destructor-miss scenarios

Crossing unknown boundaries (opaque external calls, dynamic containers, FFI) requires explicit
`unsafe` opt-in and exits the zero-leak strong guarantee scope.

## Diagnostic payload requirements

`NBL-U001` and `NBL-U002` must include:
- `code`, `category="unsafe-boundary"`, `severity`, `risk`
- `cause`, `impact`, `suggestions[]`
- `related_spans[]` (at minimum call site / annotation site; add callee span when known)

Default unsafe-boundary `risk` is `High` in v0.2.

## Examples

```nebula
@unsafe
fn raw_add(x: Int, y: Int) -> Int {
  return x + y
}

fn bad() -> Int {
  return raw_add(1, 2) // NBL-U001
}
```

```nebula
@unsafe
fn raw_add(x: Int, y: Int) -> Int {
  return x + y
}

fn ok() -> Int {
  unsafe {
    return raw_add(1, 2)
  }
}
```

## JSON examples

```json
{
  "code": "NBL-U001",
  "category": "unsafe-boundary",
  "severity": "error",
  "risk": "high",
  "cause": "safe context attempted to call a function marked @unsafe",
  "impact": "safe-subset guarantees do not apply across an unsafe boundary without explicit opt-in",
  "suggestions": [
    "wrap this call in unsafe { ... }",
    "or mark the enclosing function with @unsafe after manual audit"
  ],
  "related_spans": [
    {"line": 7, "col": 10, "end_line": 7, "end_col": 23}
  ]
}
```

```json
{
  "code": "NBL-U002",
  "category": "unsafe-boundary",
  "severity": "error",
  "risk": "high",
  "cause": "@unsafe annotation target is not callable",
  "impact": "unsafe boundary contract becomes ambiguous and cannot be audited reliably",
  "suggestions": [
    "remove @unsafe from this struct",
    "or move @unsafe to the function that performs unsafe operations"
  ],
  "related_spans": [
    {"line": 1, "col": 1, "end_line": 4, "end_col": 2}
  ]
}
```
