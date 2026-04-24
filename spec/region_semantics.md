# Region semantics

## Region scope

`region R { ... }` introduces a lexical allocation scope.

- `Lifetime(R)`: exactly the lexical block.
- Runtime lowering: stack `Region` object + bump allocation blocks.
- On scope exit: blocks are freed and registered destructors are executed in reverse order.

## Outliving rule

Region values are represented as pointers in generated C++ and must not outlive the region scope.

Invariant:
- region pointer escape is illegal unless explicitly promoted to heap semantics.

## Escape policy

When the compiler detects a region escape:

- default behavior: emit `NBL-R001` warning and auto-promote to heap-safe representation
  - owner defaults to unique for single-owner escape paths
  - owner upgrades to shared for alias fanout and conservative cross-function return paths
    with multiple distinct region roots
  - diagnostics expose machine fields (`machine_reason`, `machine_subreason`,
    `machine_detail`, `machine_owner`)
- strict behavior (`--strict-region`): treat as error and fail compilation

This divergence is always explicit via diagnostics.

## Explicit override

- `heap e`: force heap representation
- `promote e`: explicit region-to-heap intent
- `shared e` / `unique e`: explicit ownership intent on heap representation

## Safe-subset leak boundary

v0.2 leak-safety contract includes:
- heap object leaks
- shared ownership cycles in analyzable ownership graphs
- region-contained resources that require destructors

Unknown boundaries (dynamic containers, opaque external calls, FFI) are outside analyzable-safe scope unless explicitly marked `unsafe`.

## Unsafe boundary interaction (v0.2.x)

- `@unsafe fn` declares a callable boundary that requires explicit unsafe context.
- `unsafe { ... }` creates that explicit context for the enclosed block.
- Calls to unsafe-callables from safe context are rejected with `NBL-U001`.
- Misplaced `@unsafe` annotations are rejected with `NBL-U002`.
