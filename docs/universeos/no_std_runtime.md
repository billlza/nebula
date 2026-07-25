# No-Std Runtime Entry Criteria

Status: entry criteria plus one experimental primitive object prerequisite. Nebula does not
currently implement a freestanding no-std runtime.

This document defines what must be true before Nebula can claim no-std runtime support for
UniverseOS-facing work. It is intentionally not an implementation plan and does not claim that
kernel, driver, bootloader, interrupt, MMU, scheduler, syscall ABI, or direct object-code backend
support exists today.

## Current Boundary

The current system/no-std CLI gate can reject hosted imports, force strict-region diagnostics, and
record target/runtime/panic policy in generated hosted C++ artifacts. That remains a hosted
contract boundary, not proof of freestanding execution.

`build --emit freestanding-object --target x86_64-unknown-none --panic trap
--freestanding-toolchain-root <absolute-clang-root>` is a separate
experimental prerequisite. It emits only a reachable `Int/Bool/Void` subset as an audited ELF64
relocatable object with no hosted runtime or C++ standard-library references in the artifact. It
does not supply startup, allocation, strings/bytes, aggregate ABI, linker, serial I/O, or boot
behavior, and its compiler-side bootstrap still invokes `clang++`.

The current smoke fixture at `examples/system_no_std_smoke` is allowed to exercise:

- `nebula check ... --target system --no-std --panic abort`
- `nebula build ... --target system --no-std --panic abort`
- `nebula check ... --target system --no-std --panic trap`
- `nebula build ... --target system --no-std --panic trap`
- `nebula check ... --target freestanding --no-std --panic abort`
- `nebula build ... --target freestanding --no-std --panic abort`
- `nebula check ... --target freestanding --no-std --panic trap`
- `nebula build ... --target freestanding --no-std --panic trap`
- `nebula check ... --target x86_64-unknown-none --no-std --panic abort`
- `nebula build ... --target x86_64-unknown-none --no-std --panic abort`
- `nebula check ... --target x86_64-unknown-none --no-std --panic trap`
- `nebula build ... --target x86_64-unknown-none --no-std --panic trap`

The build still goes through the hosted C++23 backend and bundled runtime headers. These commands
prove only that the CLI/profile boundary records the requested runtime profile, target, panic
policy, `no_std`, and strict-region markers while avoiding bundled hosted `std` imports.

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
- No complete runtime or compiler independence claim; the primitive object artifact avoids the C++
  standard library, but the bootstrap compiler path still depends on clang.

## First Acceptable Evidence

Before this document can move from entry criteria to implementation specification, the repository
needs:

- `spec/library_layers.md` to be backed by resolver and bundled-library tests.
- `spec/abi_layout.md` to have golden layout tests for current hosted behavior and future
  freestanding expectations.
- system/no-std smoke tests that separate `check`, hosted C++ build, and true freestanding build.
- primitive object evidence for the exact target/panic/subset contract (implemented by `BLD-017`
  through `BLD-020`), followed by separate linker/runtime/boot evidence.
- diagnostics that reject hosted APIs with stable codes.
- release/support-matrix text that keeps the profile experimental until the gates pass.
