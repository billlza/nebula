# No-Std Runtime Entry Criteria

Status: entry criteria only. Nebula does not currently implement a freestanding no-std runtime.

This document defines what must be true before Nebula can claim no-std runtime support for
UniverseOS-facing work. It is intentionally not an implementation plan and does not claim that
kernel, driver, bootloader, interrupt, MMU, scheduler, syscall ABI, or direct object-code backend
support exists today.

## Current Boundary

The current system/no-std CLI gate can reject hosted imports, force strict-region diagnostics, and
record target/runtime/panic policy in generated hosted C++ artifacts. That is a contract boundary,
not proof of freestanding execution.

The current smoke fixture at `examples/system_no_std_smoke` is allowed to exercise:

- `nebula check ... --target system --no-std --panic abort`
- `nebula build ... --target system --no-std --panic abort`

The build still goes through the hosted C++23 backend and bundled runtime headers.

## Entry Criteria

A real no-std runtime milestone requires all of the following:

| Area | Required answer before support claim |
| --- | --- |
| Library split | `core`, `std`, and `system` layers are specified and tested. |
| Entry point | Boot or freestanding entry signature, calling convention, and symbol name are fixed. |
| Panic | `abort` and `trap` behavior are specified without hosted exception or unwind dependency. |
| Allocation | Heap, no-heap, region, stack, and static storage availability are explicit. |
| Strings and bytes | Representation, allocation, encoding, and ownership are specified without hosted assumptions. |
| ABI/layout | Scalar, struct, enum, pointer, alignment, padding, and export rules are specified. |
| Target | Target triples, linker behavior, startup objects, and unavailable hosted services are explicit. |
| Unsafe | Raw pointer, volatile, MMIO, syscall, interrupt, and inline assembly boundaries are auditable. |
| Atomics | Supported widths, memory orderings, and target fallbacks are specified. |
| Tests | Contract tests prove rejected hosted APIs and accepted no-std primitives. |

## Non-Goals For The Current Release

- No freestanding execution claim.
- No `core::...` import claim.
- No `system::...` import claim.
- No kernel or driver API.
- No interrupt, MMU, scheduler, or syscall ABI.
- No direct LLVM/object backend support claim.
- No C++ standard library independence claim.

## First Acceptable Evidence

Before this document can move from entry criteria to implementation specification, the repository
needs:

- `spec/library_layers.md` to be backed by resolver and bundled-library tests.
- `spec/abi_layout.md` to have golden layout tests for current hosted behavior and future
  freestanding expectations.
- system/no-std smoke tests that separate `check`, hosted C++ build, and true freestanding build.
- diagnostics that reject hosted APIs with stable codes.
- release/support-matrix text that keeps the profile experimental until the gates pass.
