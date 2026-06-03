# Ownership Model

This spec describes current ownership and promotion behavior. Region allocation is related but
specified separately in `spec/region_model.md`.

## Core Model

Nebula tracks representation and ownership as a product:

- representation: stack, region, or heap
- owner: none, unique, or shared
- region id: meaningful only for region values

Ownership is meaningful for heap representation only:

- `Unique(T)` maps to a unique heap owner in generated code
- `Shared(T)` maps to shared heap ownership in generated code

Region remains an allocation-domain choice, not ownership.

## Explicit Directives

Implemented directives:

- `heap expr`
- `promote expr`
- `unique expr`
- `shared expr`

The current implementation defines these directives on constructor sites. Directive chains are
rejected.

## Promotion

When a region-origin value escapes its lexical region, the compiler reports `NBL-R001`.

Default mode:

- emits `NBL-R001` as a warning
- auto-promotes to heap-safe representation
- chooses `heap-unique` for single-owner escape paths
- chooses `heap-shared` for alias fanout and conservative cross-function return paths

Strict mode:

- treats the region escape as an error
- does not silently promote

System-profile modes force strict-region behavior even when `--strict-region` is not passed. That is
a compiler/CLI contract gate, not a no-std runtime claim.

## NBL-R001 Metadata

`NBL-R001` exposes promotion and escape metadata for tools:

- `machine_reason`: `return`, `call`, or `field`
- `machine_subreason`: detailed trigger subtype
- `machine_detail`: stable `<machine_reason>/<machine_subreason>` path
- `machine_trigger_family`
- `machine_trigger_family_detail`
- `machine_trigger_subreason`
- `machine_owner`: `heap-unique` or `heap-shared`
- `machine_owner_reason`
- `machine_owner_reason_detail`

Follow-up diagnostics such as `NBL-P010` and `NBL-X003` inherit relevant ownership metadata so
grouped diagnostics can preserve repair ordering.

Related performance/epistemic diagnostics include `NBL-P001` for heap allocation in a loop and
`NBL-P010` / `NBL-X003` for shared ownership in hot paths.

## Borrow And Exclusivity Assist

Borrow/exclusivity diagnostics are a safety assist layer, not the ownership core.

Current `ref` diagnostics include:

- `NBL-T090`: two `ref` arguments overlap
- `NBL-T091`: `ref` overlaps non-`ref`
- `NBL-T092`: same-statement use after active `ref`
- `NBL-T093`: cross-statement read conflict
- `NBL-T094`: cross-statement write conflict
- `NBL-T095`: cross-statement re-borrow conflict

The current borrow window model is statement-local for ordinary calls and conservative across block
scope for escape-risk calls. Details live in `spec/safety_contract.md`.

## Safe-Subset Boundaries

Implemented:

- analyzable strong shared-ownership cycles are rejected with `NBL-S101`
- unsafe or opaque boundaries leave the strong safe-subset guarantee
- extern contracts can refine escape behavior for specific parameters and return paths

Current explicit unsafe constructs:

- `@unsafe fn`
- `unsafe { ... }`

See `spec/safety_contract.md` and `spec/static_analysis.md`.

## Relationship To Regions

Regions produce region-origin values that are valid only within lexical region scope. Ownership
metadata becomes relevant when a value is explicitly heap-allocated or promoted from a region escape.

`NBL-R001` is the bridge between the region model and the ownership model:

- region analysis detects the escape
- promotion policy chooses heap representation
- ownership policy chooses unique or shared
- diagnostics expose both trigger and owner metadata

## Open Design Questions

Traits/protocols:

- whether trait/protocol objects imply shared ownership, unique ownership, or borrowed views
- how trait/protocol dispatch interacts with C ABI export restrictions

Lifetimes:

- whether explicit lifetime parameters should constrain `ref` or region-origin values
- whether lifetime diagnostics should be separate from region escape diagnostics

Collections:

- how collection element ownership is represented
- whether standard collections can be analyzable for shared-cycle rejection
- which collection APIs remain available under system-profile restrictions

Closure capture:

- explicit capture-by-move versus capture-by-reference syntax
- ownership of captured region-origin values
- interaction between closure escape and `NBL-R001`
