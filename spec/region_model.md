# Region Model

This spec describes the current region behavior and its relationship to ownership diagnostics.

## Region Scope

Implemented:

- `region R { ... }` introduces a lexical allocation domain
- `R` names the region for the block
- region-origin values are valid only within that lexical scope
- generated code uses hosted runtime support for region allocation and cleanup

Region scope is lexical, not dynamic ownership. It does not imply kernel memory management, MMU
integration, interrupt safety, or allocator support outside the hosted runtime.

## Outliving Rule

Region-origin values must not outlive their region. When analysis detects an outliving path, it
emits `NBL-R001`.

Current escape trigger families:

- return paths
- call paths
- field writes

The exact diagnostic fields are documented in `spec/diagnostics.md` and `spec/rep_owner_model.md`.

## Default Promotion Policy

Default hosted behavior:

- report `NBL-R001`
- promote the escaping value to heap-safe representation
- preserve machine-readable trigger metadata
- select unique or shared heap ownership through the current owner policy

This default is intentionally visible. It is not a silent fallback.

## Strict Region Policy

Strict behavior:

- `--strict-region` turns region escape into an error
- system-profile modes force strict-region behavior without requiring `--strict-region`

System-profile strictness is part of `UOS-CLI-002` in `docs/universeos/gate_registry.md`.

## Relationship To Ownership

Regions answer where a value is allocated and when it is valid. Ownership answers how heap values are
owned after explicit heap allocation or promotion.

`NBL-R001` connects the two:

- region analysis detects an escape
- representation changes from region to heap in default hosted mode
- owner metadata records `heap-unique` or `heap-shared`
- follow-up lints can use owner metadata for repair ordering

See `spec/ownership_model.md`.

## System Profile

Experimental system-profile flags include:

- `--profile system`
- `--target system`
- `--target freestanding`
- target triples containing `-none`
- `--no-std`

The current system profile rejects hosted bundled std imports and forces strict-region diagnostics.
It is a contract gate only. It does not provide a freestanding runtime, no-std library, kernel mode,
driver API, interrupt model, MMU integration, syscall ABI, scheduler, boot path, or independent
object-code backend.

See `docs/system_profile.md` and `docs/universeos_convergence.md`.

## Open Design Questions

Traits/protocols:

- whether trait/protocol constraints should be allowed on region-origin values
- how generic constraints affect region escape summaries

Lifetimes:

- whether explicit lifetime names should supplement lexical region names
- how lifetime inference would interact with current `NBL-R001` promotion metadata

Collections:

- whether collection storage can be region-owned
- how iterators or views can avoid escaping region storage

Closure capture:

- whether closures can capture region-origin values
- how closure escape would be diagnosed
- whether async closure suspension would be allowed for region-origin captures
