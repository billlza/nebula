# RFC 0003: Traits, Protocols, And Constrained Generics

Status: Draft, design-only

No compiler support described here is implemented. This RFC is a design record for future work; the
current language still has no accepted trait, protocol, interface constraint, existential value, or
dynamic-dispatch surface.

## Summary

Building on RFC 0001, Nebula should introduce a minimal constrained generic interface model before
adopting a larger Rust-like trait system or Swift-like protocol system. The MVP should preserve
current monomorphized generics, keep conformance explicit, and lower through static direct calls in
the C++23 backend.

The recommended first spelling is:

```nebula
interface Eq<T> {
  fn eq(left: T, right: T) -> Bool
}

impl Eq<Point> {
  fn eq(left: Point, right: Point) -> Bool {
    return left.x == right.x && left.y == right.y
  }
}

fn same<T>(left: T, right: T) -> Bool where T: Eq<T> {
  return Eq<T>.eq(left, right)
}
```

The syntax above is illustrative future syntax only. It is not accepted by the current compiler.

## Goals

- Add named requirements for reusable generic libraries.
- Preserve the current monomorphized generics model.
- Keep region and ownership model effects visible to existing analysis.
- Avoid hosted runtime support, heap allocation, reflection, RTTI, witness tables, or dynamic
  dispatch in the no-std/system profile.
- Keep the C++23 backend as the first lowering target without blocking a future backend boundary.
- Keep C ABI export restrictions narrow and explicit.

## Non-Goals

- No implementation in this goal.
- No trait objects, protocol existentials, `dyn`, `any`, boxed interface values, or dynamic
  dispatch.
- No associated types, generic associated types, default methods, blanket implementations,
  specialization, negative implementations, conditional conformances, or derivation in the MVP.
- No implicit prelude-provided interfaces.
- No C ABI export of constrained generic functions.
- No freestanding runtime, kernel, driver, interrupt, MMU, scheduler, syscall ABI, or process
  supervision claim.

## Current Baseline

The current compiler supports item-level generic functions, structs, and enums. Generic lowering is
monomorphized through the current C++23 backend. There is no trait, protocol, interface declaration,
interface implementation block, constrained generic `where` clause, or interface requirement call.

The current C ABI surface is deliberately narrow: only explicit `@export @abi_c` non-generic
functions over ABI-safe scalar shapes are eligible. Generic exports and non-scalar public C ABI
types remain rejected.

## Candidate Designs

### Rust-like traits

Rust-like traits provide a strong long-term model for static dispatch, associated types, iterator
families, and operator-like abstractions.

Benefits:

- Familiar to systems programmers.
- Fits monomorphized static dispatch.
- Has a clear future path for richer generic libraries.

Costs:

- Coherence, orphan rules, blanket implementations, specialization, associated types, and object
  safety are too large for the first slice.
- Trait object expectations can accidentally imply runtime representation and ABI work.
- Region and ownership summaries would need to be attached to every selected trait method before
  constrained calls can be trusted.

Evaluation:

| Axis | Result |
| --- | --- |
| current monomorphized generics | Good long-term fit, but selection rules are large. |
| region and ownership model | Requires per-method escape and ownership summaries after selection. |
| no-std/system profile | Safe only as static dispatch; trait objects are non-MVP. |
| C++23 backend | Feasible as templates plus resolved helper calls. |
| future backend boundary | Strong path once conformance metadata is frontend-owned. |
| C ABI export restrictions | Constrained generics must stay rejected from public C ABI exports. |

### Swift-like protocols

Swift-like protocols offer a clear nominal conformance vocabulary and a path to existential values
and witness tables.

Benefits:

- Good API-documentation vocabulary.
- Nominal conformance is readable and package-friendly.
- Future witness-table lowering is a known design space.

Costs:

- Protocol culture strongly implies existential values, `Self` rules, extensions, and dynamic
  dispatch.
- Witness tables add runtime and ABI representation before Nebula has a system ABI.
- Dynamic protocol values interact poorly with no-std/system boundaries.

Evaluation:

| Axis | Result |
| --- | --- |
| current monomorphized generics | Good for static constraints, but existentials are easy to expect. |
| region and ownership model | Witnesses need escape effects, borrow rules, and representation policy. |
| no-std/system profile | Static subset can work; dynamic values are not acceptable for the MVP. |
| C++23 backend | Feasible with direct calls; witness tables should not be emitted in MVP. |
| future backend boundary | Viable later after ABI/runtime design. |
| C ABI export restrictions | Protocol-constrained exports remain unsupported. |

### Minimal constrained generic interfaces

Minimal constrained generic interfaces add only named requirements, explicit implementations, and
generic-only constraints.

Benefits:

- Smallest surface that solves reusable generic requirements.
- Static dispatch only, preserving monomorphization.
- No runtime interface values, witness tables, heap boxes, or RTTI.
- Easy to reject unsupported features with stable diagnostics.
- Leaves room to later rename or grow into a trait/protocol system.

Costs:

- Less expressive than Rust traits or Swift protocols.
- No associated types, default methods, or method-call sugar in the MVP.
- Requires a future package coherence rule before installed packages can publish broad
  implementations.

Evaluation:

| Axis | Result |
| --- | --- |
| current monomorphized generics | Best MVP fit: each specialization resolves concrete evidence. |
| region and ownership model | Best MVP fit: requirement implementations are ordinary functions. |
| no-std/system profile | Best MVP fit: compile-time only, no hosted runtime dependency. |
| C++23 backend | Direct static calls map cleanly to generated C++ functions/templates. |
| future backend boundary | Resolved conformance evidence can lower to direct symbols in any backend. |
| C ABI export restrictions | Keep constrained generic C ABI exports rejected. |

## Recommendation

Adopt minimal constrained generic interfaces for the MVP.

## Recommended MVP Syntax

Future syntax should include only:

- `interface Name<T, U> { fn requirement(...) -> ... }`
- `impl Name<ConcreteArgs> { fn requirement(...) -> ... }`
- `where T: Name<T>` clauses on generic functions
- explicit qualified requirement calls, for example `Name<T>.requirement(value)`

The MVP should not include method-call sugar through interfaces. Keeping calls qualified makes
lowering, diagnostics, and ownership effects easier to audit.

## Recommended Lowering Path

The frontend should resolve conformance evidence before NIR lowering. For every constrained generic
specialization, the compiler records the selected implementation method identity. NIR then lowers
the requirement call as a direct call to that concrete implementation method.

For C++23, codegen should emit the selected implementation as an ordinary generated function and
emit the specialized generic body with direct calls. It must not emit vtables, witness tables,
runtime dictionaries, RTTI, heap boxes, or dynamic dispatch helpers.

A future backend should consume the same resolved conformance evidence and lower direct calls to
backend-owned symbol references after specialization. That keeps the future backend boundary clear:
conformance resolution is frontend/typechecker work, not backend search.

## Compiler Impact

### Parser impact

- Parse `interface` items with requirement signatures only.
- Parse `impl Interface<Args> { ... }` blocks.
- Parse generic `where` clauses.
- Parse qualified requirement calls of the form `Interface<Args>.method(...)`.
- Reject non-MVP syntax early with stable diagnostics.

### Typechecker impact

- Register interface declarations in the item namespace.
- Validate requirement signatures with existing function type rules.
- Register explicit implementations and reject duplicates.
- Check that every implementation provides each required method exactly once.
- Check implementation method signatures against requirements.
- Resolve `where` constraints during generic call inference.
- Reject constrained generic C ABI exports.
- Attach normal ownership and escape summaries to implementation methods.

### NIR impact

- Represent interface declarations and implementation tables as compile-time metadata only.
- Lower implementation methods as ordinary functions with stable qualified identities.
- Record resolved conformance evidence on specialized generic instantiations.
- Keep interface values out of MVP NIR.

### Codegen impact

- Emit direct calls to resolved implementation methods in C++23.
- Preserve current hosted C++23 backend behavior for unconstrained code.
- Do not generate witness tables, virtual methods, RTTI, heap boxes, or runtime dictionaries.
- Keep public C ABI wrappers limited to existing unconstrained non-generic exports.

### Diagnostics impact

Proposed future diagnostics:

- `NBL-T150`: unknown interface in constraint or requirement call
- `NBL-T151`: interface arity mismatch
- `NBL-T152`: implementation missing required method
- `NBL-T153`: implementation method signature does not match requirement
- `NBL-T154`: duplicate reachable implementation
- `NBL-T155`: constrained generic call has no satisfying implementation
- `NBL-T156`: non-MVP constraint form
- `NBL-T157`: qualified requirement call outside a satisfying constraint
- `NBL-T158`: constrained generic C ABI export is unsupported
- `NBL-T159`: non-MVP interface feature is unsupported

### LSP impact

- Hover should show interface requirements, implementation owners, and selected constraint evidence.
- Go-to-definition should navigate from a qualified requirement call to the selected implementation
  after type resolution.
- Diagnostics should point both to the failed constrained call and to relevant interface
  requirements.

### Formatter impact

- Format `interface` blocks like `struct`/`enum` items with one requirement per line.
- Format `impl Interface<Args>` blocks like normal item blocks.
- Keep `where` clauses stable and deterministic.
- Preserve comments inside interface and implementation blocks.

### Testing impact

The first implementation slice should include parser, typechecker, NIR, codegen, diagnostics, LSP,
formatter, no-std/system, and C ABI negative coverage. Tests should prove hosted behavior remains
unchanged for unconstrained generics.

## Region And Ownership Model

Requirement signatures use the same ownership, `ref`, unsafe, and region rules as normal functions.
Implementation methods are analyzed like ordinary functions. A constrained generic function
inherits the escape behavior of the selected implementation methods for each specialization.

System-profile strict-region behavior must run after conformance resolution. An escaping
region-origin value through an interface implementation is still an escaping region-origin value; it
must not be hidden behind generic abstraction.

## No-Std/System Profile

Interface declarations and explicit implementations are compile-time constructs and may be safe for
no-std/system code when their signatures and bodies avoid hosted imports. The MVP must not require
hosted allocation, filesystem, networking, process, reflection, RTTI, exceptions, threads, or
dynamic metadata.

Any implementation that imports hosted bundled `std` modules remains unavailable under existing
system/no-std gates.

## C ABI Export Restrictions

Constrained generic functions must remain rejected from public C ABI export. The generated C ABI
surface should continue to accept only the current narrow non-generic ABI-safe export set. This
avoids exposing monomorphization names, conformance evidence, or future ABI choices as stable C
symbols.

## Future Backend Boundary

The backend should receive already-resolved conformance evidence. Backend implementations should not
search for implementations, perform coherence checks, or decide dispatch mode. That preserves the
current C++23 default backend and leaves LLVM, direct object, or other future backends as consumers
of typed/NIR-level direct-call decisions.

## Proposed Future Test Names

These names are proposed only for a future implementation. They should not be added as passing
feature tests until the feature exists.

- `TRT-001-interface-declaration-parses`
- `TRT-002-interface-requirement-signature-typechecks`
- `TRT-003-interface-impl-requirement-call-typechecks`
- `TRT-004-interface-impl-missing-method-rejected`
- `TRT-005-interface-impl-signature-mismatch-rejected`
- `TRT-006-duplicate-interface-impl-rejected`
- `TRT-007-generic-constraint-satisfied-by-explicit-impl`
- `TRT-008-generic-constraint-missing-impl-rejected`
- `TRT-009-interface-call-outside-constraint-rejected`
- `TRT-010-constrained-generic-cabi-export-rejected`
- `TRT-011-system-profile-interface-no-std-static-dispatch`
- `TRT-012-region-escape-through-interface-method-rejected-under-system`
- `TRT-013-cpp23-lowering-direct-call-no-witness-table`
- `TRT-014-lsp-interface-implementation-definition`
- `TRT-015-formatter-interface-block-idempotent`
- `TRT-016-non-mvp-interface-value-rejected`

## Open Questions

Ownership open questions:

- Should future interface requirements be allowed to express ownership effects explicitly, or should
  summaries remain inferred per implementation method?
- How should duplicate implementations with different ownership behavior be diagnosed across
  package boundaries?
- Should method-call sugar require additional borrow checking rules before it is enabled?

Region escape open questions:

- How should diagnostics display the selected implementation path when a constrained generic call
  causes `NBL-R001`?
- Should region escape summaries become part of package metadata before installed packages can
  publish interface implementations?
- Can system-profile checks remain entirely post-resolution, or do constraints need early
  no-escape annotations?

Dynamic dispatch open questions:

- Is there a future safe subset for interface values, or should Nebula require explicit enum/value
  modeling instead?
- If dynamic dispatch is added later, what ownership model governs boxed or borrowed interface
  values?
- What ABI boundary would witness tables or vtables belong to, and how would no-std/system profiles
  reject unsupported runtime representation?

## Rollback Strategy

If constrained generic implementation work destabilizes hosted compilation, disable the parser gate
for `interface` and `where` clauses while keeping this RFC as design-only documentation. Existing
unconstrained generic tests, C ABI export tests, system/no-std gates, and C++23 backend boundary
tests must remain the rollback baseline.
