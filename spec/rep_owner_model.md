# Representation × Ownership model

Region remains an allocation-domain choice, not ownership.

## Representation axis

- `StackValue(T)` -> C++ `T`
- `RegionPtr(R,T)` -> C++ `T*` allocated via `NEBULA_ALLOC`
- `HeapPtr(T)` -> C++ smart pointer wrapper

## Ownership axis (heap-only)

- `Unique(T)` -> `std::unique_ptr<T>`
- `Shared(T)` -> `std::shared_ptr<T>`

## Compiler product model

Nebula tracks `(Rep, Owner, RegionId)` instead of a single total-order lattice:

- `Rep in {Stack, Region, Heap}`
- `Owner in {None, Unique, Shared}` (meaningful only for `Rep=Heap`)
- `RegionId` only meaningful for `Rep=Region`

Upgrades are conservative policy transitions, not lattice joins.

## v0.2 safety policy

- explicit prefixes win (`shared`, `unique`, `heap`, `promote`)
- region escape policy:
  - default: `NBL-R001` + auto-promotion
  - owner selection on auto-promotion:
    - single escaping alias chain -> `Heap+Unique`
    - multi-alias escaping fanout (distinct escaping alias leaves from one root) -> `Heap+Shared`
    - cross-function return-path fan-in of distinct region roots/unknown -> `Heap+Shared`
      (conservative)
  - machine-readable `NBL-R001` fields:
    - `machine_reason`: `return|call|field`
    - `machine_subreason`: trigger subtype (`return-via-alias-chain`,
      `cross-function-return-path`,
      `cross-function-return-path-unknown-no-summary`,
      `cross-function-return-path-unknown-external-opaque`,
      `cross-function-return-path-unknown-indirect-unresolved`,
      `callee-param-escape`, `callee-param-escape-unknown-no-summary`,
      `callee-param-escape-unknown-external-opaque`,
      `callee-param-escape-unknown-indirect-unresolved`,
      `field-write-base-escape`, ...)
    - `machine_detail`: stable path `<machine_reason>/<machine_subreason>`
    - `machine_trigger_family`: normalized trigger slot (`return|call|field`)
    - `machine_trigger_family_detail`: stable `|`-joined trigger family set
      (`return|call|field`) for mixed-trigger roots
    - `machine_trigger_subreason`: normalized trigger subtype slot
    - `machine_owner`: inferred owner result (`heap-unique|heap-shared`)
    - `machine_owner_reason`: owner chooser
      (`single-owner-flow|alias-fanout|cross-function-return-path-alias-fanout|cross-function-return-path-alias-fanout-mixed|cross-function-return-path-fanin|
      cross-function-return-path-unknown-no-summary|
      cross-function-return-path-unknown-external-opaque|
      cross-function-return-path-unknown-indirect-unresolved`)
      and grouped repair priority favors `alias-fanout`, then
      `cross-function-return-path-alias-fanout`, then
      `cross-function-return-path-alias-fanout-mixed`, then unknown return-path causes
      (`...-unknown-no-summary` includes summary-known-but-precision-unknown
      recursive/unstable callees)
    - `machine_owner_reason_detail`: stable `|`-joined split of all unknown return-path
      source tags on the same root (indirect unresolved, external opaque, no summary)
      so grouped ranking can preserve secondary unknown causes after primary owner choice
    - `NBL-R001` suggestions encode first-fix guidance per owner reason to keep root-cause ranking
      directly actionable in grouped output
  - strict: hard error
- safe subset rejects analyzable strong shared-ownership cycles
- unknown ownership boundaries (opaque externals/dynamic containers/FFI) are not safe by default

## Current contract scope

The zero-leak guarantee is scoped to the analyzable safe subset. Entering `unsafe`/opaque boundaries exits the strong guarantee and requires explicit opt-in.

Unsafe boundaries are explicit surface constructs:
- `@unsafe fn` marks callable boundaries.
- `unsafe { ... }` marks scoped opt-in call sites.
