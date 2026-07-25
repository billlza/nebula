# RFC 0002: Freestanding Target Support

Status: Draft; the primitive object prerequisite is experimental, while runtime/link/boot design
remains unimplemented

This RFC defines freestanding target support for Nebula. The repository now implements only the
`UOS-BOOT-002` primitive object prerequisite described below. It does not implement a freestanding
runtime, linker/boot flow, allocator, hardware APIs, or production freestanding support.

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
- the exact request `build --emit freestanding-object --target x86_64-unknown-none --panic trap
  --freestanding-toolchain-root <absolute-clang-root>`
  emits an experimental, audited, primitive-only ELF relocatable object through generated C++ and
  fixed `clang++`; ordinary system/no-std builds remain hosted

## Implemented Experimental Delta (2026-07-14)

The first object gate intentionally implements less than the full RFC:

- one exact triple, one explicit trap policy, one root-package `@entry`, and one versioned
  `__nebula_uos_payload_entry_v1` symbol; the future protocol adapter, not the payload, owns `_start`
- macOS/Linux compiler hosts only for the publication slice; Windows compiler/tooling remains GA
  but returns `NBL-CLI-FS-HOST-UNSUPPORTED` for this exact request
- reachable `Int/Bool/Void` code, direct resolved Nebula calls, and stack/non-owning storage only
- include-free generated C++ with checked integer trap semantics
- an explicit owner-controlled toolchain root containing `bin/clang++`, one content-addressed
  compiler identity copied into one owner-private executable lease shared by all resolver queries
  and formal compilation, fixed clang arguments, null-device stdin, a minimal non-inherited compiler
  environment, independent 64 KiB stdout/stderr limits, pre/post lease identity revalidation,
  mandatory pre-publication lease cleanup, and a 30-second process-group timeout, with
  a non-reaped POSIX leader identity anchor, bounded Darwin/Linux zombie-only group audit, and
  group sealing on success/failure/timeout/catchable
  parent termination; `SIGHUP`, `SIGINT`, `SIGQUIT`, and `SIGTERM` are restored and re-delivered
  after confirmed cleanup, while a containment failure suppresses re-delivery and fails explicitly;
  the move-only resolved toolchain owns one continuous signal session from before lease creation
  through resolver queries, analysis/emission, compilation, lease retirement, and caller-state
  restoration, including explicit close on every pre-compilation failure; session close is a
  monotonic `Executable -> Closing -> PreparedFrozen -> Closed` transition, preparation disables
  compiler execution before freezing and retiring the lease, and finalization cannot restore the
  caller signal state until the artifact owner has explicitly classified external cleanup as
  complete or incomplete;
  the transaction freeze collects owned pending signals as the commit/cancellation handoff without
  consuming signals the caller had already blocked; signals after that handoff belong to the
  restored caller disposition but remain blocked through staging cleanup, commit/rollback, guard
  disarm, and output-lock release; restoration is the final lifecycle operation, cleanup failure
  suppresses intercepted-signal handoff, and restore failure reports whether the artifact is
  `Absent`, `Committed`, or `CleanupIncomplete` without attempting an unlocked rollback;
  there is no hosted include/runtime or environment-selected compiler fallback
- bounded in-repo ELF validation (including W^X, allocation, relocation, symbol, and scan budgets),
  object size/SHA-256 metadata, concurrency locking, staging, no-replace publication, and
  deterministic release-mode evidence
- fail-closed `NBL-BE-FS-*` and `NBL-CLI-FS-*` diagnostics

It deliberately does not implement abort, aggregates, raw pointers, volatile/atomics, inline
assembly, section placement, an allocator, startup initialization, a direct object backend, a
linker script, a boot image, or QEMU execution. `BLD-017` through `BLD-020` are the machine evidence
for this delta.

The current transaction and toolchain statements assume trusted, explicitly selected local tooling and a
caller-controlled output directory. They do not claim sandboxing against a malicious compiler,
hostile shared-directory safety, power-loss durability, or portable orphan prevention after
`SIGKILL`, host failure, or a parent-process crash.

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
- one explicit language entry annotation whose object-level payload symbol is versioned separately
  from the boot image's `_start`
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

The current backend boundary and experimental object slice are useful preparation, but they are not
proof of a complete freestanding codegen/runtime surface.

## Required Test Gates

Freestanding support must not be promoted until all of these gates exist:

- object-file generation gate for `x86_64-unknown-none` (experimental prerequisite exists)
- pinned boot protocol/ABI/toolchain contract (a strict Limine v12.3.2 protocol/ABI candidate now
  exists under `boot/uos-x86_64-limine-v1`; exact linker/image-tool closure remains incomplete)
- linked-kernel ELF smoke that proves segment/section placement, entry, protocol markers, W^X, and
  provenance
- deterministic version-pinned boot-media assembly
- QEMU boot hello that checks exact serial output and fails closed on timeout
- negative tests for hosted `std`, hosted runtime, unwind, and C++ standard library dependency
- rollback tests proving hosted CLI/service builds remain unchanged

Proposed gate names:

- `UOS-BOOT-001`: pinned boot protocol, ABI, toolchain, and supply-chain contract
- `UOS-BOOT-002`: freestanding object emission gate
- `UOS-BOOT-003`: deterministic linked kernel ELF gate
- `UOS-BOOT-004`: version-pinned boot media assembly gate
- `UOS-BOOT-005`: QEMU serial hello gate

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

The complete implementation must retain or add stable diagnostics for:

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
