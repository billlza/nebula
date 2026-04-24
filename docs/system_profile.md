# System Profile

The system profile is the future contract for using Nebula in low-level universeOS-facing code.
It is not implemented today as a full no-std/freestanding runtime and is not part of the 1.0 GA
surface. The repo now has a first experimental CLI gate that makes the boundary visible:
`--target system|freestanding|<triple>`, `--profile system`, `--no-std`, and
`--panic abort|trap`.

The purpose of this document is to keep system-programming work honest: every future low-level claim
must answer allocation, escape, unsafe, concurrency, panic, ABI, and freestanding-runtime behavior
explicitly before it becomes a release promise.

## Current Status

Current Nebula programs depend on:

- host OS services
- C++23 code generation
- a host C++ compiler, currently `clang++` by release contract
- the C++ standard library and the bundled Nebula runtime headers
- hosted filesystem, networking, process, time, and TLS/crypto facilities where those modules are
  imported

The experimental system/no-std gate currently:

- rejects bundled `std` imports during project loading with `NBL-CLI-SYSTEM-STD`
- forces strict-region diagnostics without requiring an explicit `--strict-region`
- writes runtime profile, target, and panic policy markers into generated C++ artifacts
- rejects reachable host bridge/native package sources for system/no-std builds
- rejects `--panic unwind` because no freestanding unwind/runtime ABI exists yet

That profile is useful for CLI tools, backend services, control-plane programs, and thin-host app
cores. It is not a kernel, bootloader, driver, interrupt-handler, or no-std runtime profile.
The experimental gate is therefore a contract check, not proof of kernel suitability.

## Required Contract Areas

Before Nebula can claim a real system profile, these areas need explicit contracts.

- Allocation: define when heap, region, stack, and static storage are available, and how programs
  opt out of implicit heap promotion.
- Escape: make strict region behavior the default for system targets, with no silent auto-promote
  across system boundaries.
- Unsafe: require auditable unsafe boundaries for hardware, syscall, FFI, volatile memory, inline
  assembly, atomics, and raw pointer operations.
- Concurrency: define task, thread, interrupt, atomic, and data-race rules before claiming
  scheduler or driver suitability.
- Panic and unwind: keep abort/trap as the only accepted system-profile policies until an explicit
  unwind ABI and runtime contract exist; then specify how diagnostics map to those choices.
- ABI: define target triples, calling conventions, symbol export, layout, alignment, linker-script
  integration, and syscall ABI expectations.
- Freestanding runtime: define the no-std runtime subset, boot entry requirements, startup/shutdown
  hooks, allocator hooks, and unavailable hosted APIs.

## Non-Goals Today

The current repo does not claim:

- complete freestanding or no-std builds
- kernel-mode execution
- driver APIs
- interrupt or MMU integration
- raw hardware access
- stable syscall ABI
- direct object/backend independence from the C++23 transpiler path

Until those contracts exist, universeOS-adjacent work should use Nebula for tools, services,
control planes, and app-core logic rather than kernel or driver implementation.

## Entry Criteria

The first credible system-profile milestone should include:

- a documented `--target`/profile story separate from hosted CLI/service builds (first
  experimental gate exists)
- a no-std smoke target that does not import hosted `std` modules (first contract tests exist)
- explicit diagnostics for forbidden hosted APIs in system-profile code (first bundled-std import
  diagnostic exists)
- strict-region behavior without implicit auto-promote for system targets (first CLI policy exists)
- a panic/abort policy visible in generated artifacts (first codegen marker exists)
- a minimal ABI/layout test suite
- release notes and support-matrix language that keep the profile experimental until proven
