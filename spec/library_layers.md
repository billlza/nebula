# Library Layers

Status: design specification for a future split. No compiler behavior changes are implied by this
document.

Nebula currently ships bundled modules under `std::...`. This document defines the intended future
layering so no-std and system-profile work can move without breaking 1.0.x users.

## Layer Definitions

| Layer | Purpose | Allowed dependencies |
| --- | --- | --- |
| `core` | no-std-safe language-adjacent primitives | compiler builtins, target-independent runtime shims with explicit no-std contracts |
| `std` | hosted application and service APIs | host OS, C++ standard library, bundled hosted runtime headers |
| `system` | future boot/syscall/interrupt/driver-facing APIs | only after a real no-std runtime, ABI, and target story exist |

## Future `core`

Future `core` should contain:

- `Option<T>`
- `Result<T, E>`
- slices and immutable views once representation and bounds rules are specified
- raw pointer abstractions behind explicit unsafe boundaries
- integer, float, bool, and string-adjacent primitives only where their representation is no-std-safe
- atomics placeholders, not a concurrency model, until memory-ordering and target support are
  specified

Current non-goals:

- no allocator API
- no filesystem, network, process, clock, HTTP, JSON parser, or task runtime
- no driver, interrupt, MMU, scheduler, boot, or syscall API
- no promise that current hosted runtime headers are no-std-safe

## Future `std`

Future `std` remains the hosted layer. It owns:

- filesystem
- networking
- process environment and process spawning
- wall-clock and steady-clock APIs
- HTTP client/server helpers
- JSON parsing/stringification
- logging backed by hosted output
- task runtime integration while the runtime is hosted

## Future `system`

Future `system` is not implemented. It is reserved for APIs that need explicit low-level contracts:

- boot entry and startup/shutdown hooks
- syscall surfaces
- interrupt handlers
- volatile memory and MMIO wrappers
- driver-facing abstractions
- allocator hooks and no-allocator modes
- target-specific atomics and fences

No `system` module should be documented as available until the relevant gate, ABI, panic, allocation,
and backend contracts exist.

## Existing Bundled Module Map

| Current module | Current dependency posture | Future destination | Migration note |
| --- | --- | --- | --- |
| `std::result` | Pure Nebula enum surface today | `core::result` | Keep `std::result` as a 1.0.x compatibility import or re-export. |
| `std::bytes` | Runtime-backed byte handle | `core::bytes` candidate | Move only after bytes have a no-std representation and allocation policy. |
| `std::json` | Hosted runtime JSON handle/parser/stringifier | `std::json` | Remains hosted unless a separate no-alloc parser is specified later. |
| `std::http_json` | Hosted HTTP plus JSON composition | `std::http_json` | Remains hosted. |
| `std::fs` | Host filesystem runtime calls | `std::fs` | Remains hosted. |
| `std::net` | Host socket/runtime calls | `std::net` | Remains hosted. |
| `std::http` | Host socket/task/runtime helpers | `std::http` | Remains hosted. |
| `std::process` | Host process execution | `std::process` | Remains hosted. |
| `std::env` | Host environment access | `std::env` | Remains hosted. |
| `std::time` | Host clock/sleep runtime calls | `std::time` | Remains hosted until target clocks are specified separately. |
| `std::task` | Hosted cooperative runtime handles | `std::task` | Remains hosted; not a kernel scheduler. |
| `std::log` | Hosted output/logging | `std::log` | Remains hosted unless a target-specific sink is provided later. |

## Migration Plan

1. Keep every current `std::...` import valid through 1.0.x.
2. Introduce `core::...` modules only after their no-std representation and diagnostics are
   specified.
3. Add compatibility re-exports from `std` to `core` for moved pure primitives.
4. Make system/no-std examples import `core::...` only after the compiler can resolve that layer.
5. Keep hosted APIs in `std` and continue rejecting them under `--target system`, `--target
   freestanding`, `--profile system`, and `--no-std`.
6. Add contract tests before each module migrates.

## Current Release Non-Goals

- No module is renamed in this goal.
- No compiler import resolution changes are made in this goal.
- No freestanding runtime is implemented.
- No `core::...` or `system::...` import path is claimed to work today.
- No ABI, object backend, kernel, driver, interrupt, MMU, scheduler, or syscall support is claimed.
