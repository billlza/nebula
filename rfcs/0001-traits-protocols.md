# RFC 0001: Traits, Protocols, And Minimal Constrained Interfaces

Status: draft, design-only. No compiler support described here is implemented.

## Summary

Nebula should add a minimal constrained generic interface model as the first MVP for shared behavior.
The MVP keeps generic dispatch monomorphized, keeps conformance explicit, and avoids runtime
existentials, dynamic dispatch, blanket implementations, associated types, and standard-library
hierarchy commitments.

This RFC compares three designs:

1. Rust-like traits
2. Swift-like protocols
3. minimal constrained generic interfaces

The recommendation is option 3: a small interface declaration plus explicit implementation blocks
usable only as generic constraints.

## Goals

- Express reusable generic requirements without adding erased trait/protocol values.
- Preserve the current monomorphized generics policy.
- Fit strict-region and ownership analysis before adding lifetime parameters.
- Keep the system/no-std profile independent of hosted runtime dictionaries.
- Lower cleanly through the current C++23 backend.
- Leave a straightforward path to future LLVM/object backends.

## Non-Goals

- No implementation in this goal.
- No trait objects, protocol existentials, `dyn`, `any`, or runtime interface values.
- No implicit conformance search across packages.
- No blanket implementations.
- No specialization priority rules.
- No associated types, generic associated types, default methods, extension methods, or conditional
  conformances in the MVP.
- No C ABI export of constrained generic functions.
- No promise that syntax in this RFC is accepted by the current compiler.

## Current Baseline

The current compiler has item-level generic `fn`, `struct`, and `enum` declarations. Generic
functions are inferred from argument and expected-result context, and lowering is monomorphized
through C++ templates in the C++23 backend. There is no trait, protocol, interface, or constrained
generic syntax today.

## Candidate Designs

### Option A: Rust-Like Traits

Illustrative syntax only:

```nebula
trait Eq<T> {
  fn eq(self: T, other: T) -> Bool
}

impl Eq<Point> for Point {
  fn eq(self: Point, other: Point) -> Bool {
    return self.x == other.x && self.y == other.y
  }
}

fn contains<T>(items: Slice<T>, needle: T) -> Bool where T: Eq<T> {
  // future Slice and iteration syntax are not part of this RFC
  return false
}
```

Benefits:

- Familiar model for monomorphized generic constraints.
- Strong path to associated types, operator traits, and future iterator abstractions.
- Maps well to LLVM when the compiler owns vtable-free specialization.

Costs:

- Pulls Nebula toward a large trait system too early.
- Implies hard design work for coherence, orphan rules, blanket impls, specialization, and
  associated types.
- Risks documenting capabilities that the current language, package system, and std split cannot
  yet support.
- Needs careful interaction with region and ownership summaries before trait methods can be
  trusted across package boundaries.

Evaluation:

| Axis | Result |
| --- | --- |
| monomorphized generics | Strong fit, but trait selection rules are large. |
| region/ownership model | Needs per-method escape summaries and conformance-level ownership effects. |
| no-std/system profile | Good if limited to static dispatch; dangerous if trait objects arrive early. |
| C++23 backend feasibility | Feasible as templates plus constrained helper wrappers, but coherence is compiler work. |
| future LLVM/object backend feasibility | Strong long-term path, high front-end complexity. |

### Option B: Swift-Like Protocols

Illustrative syntax only:

```nebula
protocol Equatable {
  fn equals(self: Self, other: Self) -> Bool
}

extension Point: Equatable {
  fn equals(self: Point, other: Point) -> Bool {
    return self.x == other.x && self.y == other.y
  }
}

fn contains<T>(items: Slice<T>, needle: T) -> Bool where T: Equatable {
  return false
}
```

Benefits:

- Clean nominal conformance story.
- Future existential and witness-table model is well understood.
- Natural fit for package-facing API documentation.

Costs:

- Protocol existentials are easy for users to expect and hard for Nebula to support safely now.
- Witness tables and `Self` requirements need runtime representation and ABI decisions.
- Extension-based syntax introduces a broad surface area unrelated to the MVP.
- Dynamic protocol dispatch is hostile to the current no-std/system boundary unless very tightly
  specified.

Evaluation:

| Axis | Result |
| --- | --- |
| monomorphized generics | Good for generic constraints, but protocol culture tends toward existentials. |
| region/ownership model | Witnesses need ownership and escape effects; `Self` rules need lifetime design. |
| no-std/system profile | Static-only subset is possible; dynamic protocol values are non-MVP. |
| C++23 backend feasibility | Witness tables are possible but introduce ABI/runtime surface too early. |
| future LLVM/object backend feasibility | Good once ABI is mature; too much for the first slice. |

### Option C: Minimal Constrained Generic Interfaces

Illustrative syntax only:

```nebula
interface Eq<T> {
  fn eq(self: T, other: T) -> Bool
}

impl Eq<Point> {
  fn eq(self: Point, other: Point) -> Bool {
    return self.x == other.x && self.y == other.y
  }
}

fn same<T>(left: T, right: T) -> Bool where T: Eq<T> {
  return Eq<T>.eq(left, right)
}
```

Benefits:

- Adds only what generic libraries need first: named requirements and explicit implementations.
- Keeps all dispatch static and monomorphized.
- Avoids implied trait-object/protocol-existential support.
- Lets region, ownership, and no-std rules remain explicit.
- Leaves syntax room to rename or desugar into a richer future trait/protocol system.

Costs:

- Less expressive than Rust traits or Swift protocols.
- No default methods or associated types in the MVP.
- Method-call sugar through interfaces should remain non-MVP, so calls are more explicit.
- Users may eventually want a richer conformance model.

Evaluation:

| Axis | Result |
| --- | --- |
| monomorphized generics | Best MVP fit: constraints select a concrete implementation per specialization. |
| region/ownership model | Best MVP fit: every required method has a normal function signature and summary. |
| no-std/system profile | Best MVP fit: no runtime dictionary or hosted allocation is required. |
| C++23 backend feasibility | Feasible as constrained templates or generated static helper calls. |
| future LLVM/object backend feasibility | Feasible as direct calls after type-directed conformance resolution. |

## Recommended MVP

Adopt minimal constrained generic interfaces with explicit implementation blocks and generic-only
constraints.

MVP surface:

- `interface Name<T, U> { fn requirement(...) -> ... }`
- `impl Name<ConcreteArgs> { fn requirement(...) -> ... }`
- generic `where` clauses limited to concrete type-parameter constraints, for example
  `where T: Eq<T>`
- explicit qualified requirement calls such as `Eq<T>.eq(left, right)`
- one implementation per fully resolved interface/type argument tuple in a package graph
- static dispatch only

MVP deliberately excludes:

- interface values
- dynamic dispatch
- trait/protocol objects
- default methods
- associated types
- extension methods
- blanket implementations
- negative implementations
- specialization
- implicit prelude imports
- automatic derivation
- cross-crate orphan/coherence policy beyond "one reachable explicit implementation"

## Proposed Diagnostics

These diagnostic codes are proposed for the future implementation. They are not emitted by the
current compiler.

- `NBL-T150`: unknown interface name in a constraint or qualified requirement call
- `NBL-T151`: interface arity mismatch
- `NBL-T152`: implementation does not provide a required method
- `NBL-T153`: implementation method signature does not match the interface requirement
- `NBL-T154`: duplicate reachable implementation for the same interface/type tuple
- `NBL-T155`: generic call does not satisfy a required interface constraint
- `NBL-T156`: interface constraint uses a non-type parameter in the MVP
- `NBL-T157`: interface requirement call is not available outside a satisfying generic constraint
- `NBL-T158`: constrained generic C ABI export is not supported
- `NBL-T159`: interface declaration uses a non-MVP feature

## Lowering Strategy

Parser changes:

- Add `interface` item parsing with function signatures only.
- Add `impl Interface<Args> { ... }` item parsing.
- Add optional `where` clauses on generic functions.
- Add qualified requirement-call parsing for `Interface<Args>.name(...)`.

Typechecker changes:

- Register interface declarations in the item namespace.
- Validate requirement signatures using existing function type rules.
- Register explicit implementations and reject duplicate reachable implementations.
- Check that every implementation provides each required method exactly once.
- Resolve `where` constraints against generic type parameters.
- During generic call inference, require evidence for each used constrained function.
- Reject constrained generic C ABI exports.
- Attach normal escape/ownership summaries to implementation methods.

NIR changes:

- Represent interface declarations as compile-time metadata only.
- Lower implementation methods as ordinary functions with stable qualified identities.
- Record resolved conformance evidence on constrained generic instantiations.
- Keep no runtime interface value in MVP NIR.

Codegen changes:

- For C++23, emit monomorphized function bodies that call resolved implementation functions directly.
- Do not emit vtables, witness tables, RTTI, heap boxes, or dynamic dispatch helpers.
- Keep generated C ABI wrappers limited to unconstrained non-generic functions.
- For a future LLVM/object backend, lower resolved requirement calls to direct symbol references
  after specialization.

## Region And Ownership Rules

- Requirement signatures use the same value, `ref`, unsafe, and region rules as normal functions.
- Implementation methods are analyzed like normal functions.
- A constrained generic function inherits the escape effects of the resolved implementation methods
  used by each specialization.
- In the MVP, constraints cannot introduce lifetime parameters or hide region escapes.
- In system profile, strict-region diagnostics still apply after conformance resolution.

## No-Std/System Rules

- Interface declarations and implementations are compile-time constructs and may be no-std-safe.
- The MVP must not require hosted allocation, reflection, dynamic type metadata, or C++ RTTI.
- Any interface that depends on hosted modules remains unavailable under the existing
  system/no-std import gate.
- A future `core` package may define no-std-safe interfaces only after `spec/library_layers.md`
  has a stable core boundary.

## Future Contract Test Names

These are future test names only. They should not be added until the feature is implemented.

- `TRT-001-interface-declaration-parses`
- `TRT-002-interface-impl-requirement-call-typechecks`
- `TRT-003-interface-impl-missing-method-rejected`
- `TRT-004-interface-impl-signature-mismatch-rejected`
- `TRT-005-duplicate-interface-impl-rejected`
- `TRT-006-generic-constraint-satisfied-by-explicit-impl`
- `TRT-007-generic-constraint-missing-impl-rejected`
- `TRT-008-interface-call-outside-constraint-rejected`
- `TRT-009-constrained-generic-cabi-export-rejected`
- `TRT-010-system-profile-interface-no-std-static-dispatch`
- `TRT-011-region-escape-through-interface-method-rejected-under-system`
- `TRT-012-cpp23-lowering-direct-call-no-witness-table`
- `TRT-013-llvm-lowering-direct-symbol-after-specialization`
- `TRT-014-non-mvp-default-method-rejected`
- `TRT-015-non-mvp-interface-value-rejected`

## Open Questions

- Should the keyword be `interface`, `trait`, or `protocol` once the feature is implemented?
- Should `Eq<T>.eq(...)` remain the explicit call form, or should method sugar be added later?
- What package coherence rule is needed before installed packages can publish implementations?
- Should future `core` interfaces be shipped by the compiler or by versioned packages?
- How should diagnostics point from a failed constrained call to the missing implementation owner?
