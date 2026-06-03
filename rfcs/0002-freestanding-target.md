# RFC 0002: Freestanding Target Support

Status: Draft, design-only

This RFC defines future freestanding target support for Nebula. It does not implement compiler,
runtime, codegen, object, linker, boot, allocator, or hardware APIs.

## Current Support

Current Nebula support remains hosted:

- C++23 code generation is the only production backend.
- The host C++ compiler and C++ standard library are required for production builds.
- `--target system`, `--target freestanding`, `--profile system`, and `--no-std` are experimental
  contract gates.
- System/no-std builds can reject hosted `std` imports and make runtime/profile/panic markers
  visible, but they still emit hosted C++ and include bundled runtime headers.
- `--panic abort` and `--panic trap` are accepted policies for system/no-std markers.
- `--panic unwind` is rejected under system/no-std because no unwind ABI exists.

## Target Triple Strategy

Two triple lanes should be reserved:

| Triple | Purpose | Recommendation |
| --- | --- | --- |
| `x86_64-unknown-none` | Conventional freestanding x86_64 none target used by many low-level toolchains | Use first for interoperability and QEMU smoke work |
| `x86_64-universe-none` | Nebula/UniverseOS-owned vendor lane for future ABI/runtime experiments | Reserve until ABI, runtime, and package tooling need a Nebula-owned vendor |

MVP recommendation: implement `x86_64-unknown-none` first, and treat `x86_64-universe-none` as a
future compatibility alias only after ABI and package metadata can express the difference.

## Required Compiler Behavior

Freestanding target support requires all of the following before it can be documented as current:

- no hosted bundled `std` imports
- no C++ standard library dependency in generated artifacts
- no hosted Nebula runtime header dependency
- panic policy limited to `abort` or `trap`
- explicit entry symbol, for example `@entry("_start")`
- linker script support from CLI and package manifests
- allocator hooks for any heap-using language surface
- explicit volatile, atomic, and raw pointer APIs
- stable diagnostics for unsupported hosted APIs
- deterministic target/profile metadata in artifacts and test outputs

## Required Runtime Work

The freestanding runtime must define:

- startup responsibilities and stack assumptions
- static data initialization and zeroing rules
- panic abort/trap implementation
- allocator hook ABI
- unavailable hosted APIs
- minimal formatting/logging story for diagnostics or serial output
- no hidden dependency on process, filesystem, networking, time, TLS, JSON runtime helpers, or host
  environment services

## Required Codegen Work

Codegen must provide:

- direct object output or a documented freestanding backend path
- no generated dependency on `<string>`, `<vector>`, exceptions, RTTI, iostreams, threads, futures,
  filesystem, networking, or other C++ standard library facilities
- explicit symbol naming rules for exported and entry symbols
- target-specific calling convention choices
- linker-section and alignment controls where required by the boot path
- artifact metadata that distinguishes hosted C++23 output from freestanding output

The current backend boundary is useful preparation, but it is not proof of freestanding codegen.

## Required Test Gates

Freestanding support must not be promoted until these gates exist:

- object-file generation gate for `x86_64-unknown-none`
- linker-script smoke that proves section placement and entry symbol
- QEMU boot hello that checks exact serial output and fails closed on timeout
- negative tests for hosted `std`, hosted runtime, unwind, and C++ standard library dependency
- rollback tests proving hosted CLI/service builds remain unchanged

Proposed gate names:

- `UOS-BOOT-002`: freestanding object emission gate
- `UOS-BOOT-003`: linker script and boot artifact gate
- `UOS-BOOT-004`: QEMU serial hello gate

## MVP Boundary

MVP:

- recognize `x86_64-unknown-none` as a freestanding target
- reject hosted APIs and `panic unwind`
- require an explicit entry symbol
- emit or produce a freestanding object without C++ standard library dependencies
- link with an explicit linker script in a controlled smoke
- boot under QEMU and print one exact serial line

Non-MVP:

- drivers
- interrupts
- MMU
- scheduler
- syscall ABI
- process isolation
- filesystems
- networking
- allocator beyond explicit hook contract
- full standard library
- broad optimization/backend parity
- production kernel support

## Diagnostics

Future implementation must add stable diagnostics for:

- hosted `std` import under freestanding target
- hosted runtime dependency under freestanding target
- panic unwind under freestanding target
- missing explicit entry symbol
- missing linker script when a boot artifact is requested
- heap use without allocator hooks
- unsupported volatile/atomic/raw pointer operations
- unsupported backend/profile combinations

## Rollback Strategy

Freestanding work must be reversible without affecting hosted behavior:

1. Keep C++23 hosted backend as `default_backend()`.
2. Keep freestanding target selection explicit.
3. Keep hosted CLI/service tests as required rollback evidence.
4. Gate freestanding packages and examples separately from hosted packages.
5. If QEMU/object/linker evidence regresses, disable freestanding promotion and keep system/no-std
   as an experimental hosted contract gate.

No release note should promote freestanding support unless the gates above pass in the release
candidate environment.
