# System Profile

The system profile is the future contract for using Nebula in low-level universeOS-facing code.
It is not implemented today as a full no-std/freestanding runtime and is not part of the 1.0 GA
surface. The repo has an experimental hosted CLI gate that makes the boundary visible:
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
- accepts `--panic abort` and `--panic trap` as artifact-visible policies
- can check and hosted-C++ build the repo-local `examples/system_no_std_smoke` fixture without
  bundled `std` imports

A separate, exact experimental object slice now accepts:

```text
nebula build <path> --emit freestanding-object \
  --target x86_64-unknown-none --panic trap \
  --freestanding-toolchain-root <absolute-clang-root>
```

The publication slice is currently implemented on macOS/Linux compiler hosts. Windows remains in
the compiler/tooling release matrix but rejects this exact request with
`NBL-CLI-FS-HOST-UNSUPPORTED`.

This path does not use the bundled runtime headers or C++ standard library in the emitted artifact.
It accepts only reachable `Int`, `Bool`, and `Void` code with direct resolved internal calls and
stack/non-owning storage, emits an include-free bootstrap translation unit, invokes a fixed
`clang++` cross-target command, and audits the resulting ELF64 relocatable object before a
no-replace transaction publishes it. Its compiler implementation still depends on C++ and clang;
it is not a direct object backend or no-std runtime.

The broader hosted Nebula profile is useful for CLI tools, backend services, control-plane
programs, and thin-host app cores. The primitive relocatable-object slice above is not an
executable service profile, kernel, bootloader, driver, interrupt-handler, or no-std runtime.
The experimental gate is therefore a contract check, not proof of kernel suitability.

## CLI Gate Matrix

| Input | Current effect |
| --- | --- |
| `--target system` | selects system runtime profile, implies no-std, forces strict region |
| `--target freestanding` | selects system runtime profile, implies no-std, forces strict region |
| `--target *-none*` / `*unknown-none*` | selects system runtime profile, implies no-std, forces strict region |
| `--profile system` | selects system runtime profile, implies no-std, forces strict region |
| `--no-std` | rejects bundled `std::...` imports |
| `--panic abort` | accepted and recorded in generated artifacts |
| `--panic trap` | accepted and recorded in generated artifacts |
| `--panic unwind` with system/no-std | rejected before compilation |
| `build --emit freestanding-object --target x86_64-unknown-none --panic trap --freestanding-toolchain-root <absolute-clang-root>` | emits the experimental primitive-only audited ELF object; target, panic policy, and owner-controlled Clang root must be explicit and exact |

The ordinary smoke fixture commands remain hosted C++23 codegen and use bundled runtime headers.
They are not redirected to the object path and are not freestanding runtime builds.

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
- direct object/backend independence from the C++ bootstrap path
- a linked image, startup runtime, QEMU boot, kernel entry, or hardware-facing API

Until those contracts exist, universeOS-adjacent work should use Nebula for tools, services,
control planes, and app-core logic rather than kernel or driver implementation.

## Entry Criteria

The first credible system-profile milestone should include:

- a documented `--target`/profile story separate from hosted CLI/service builds (first
  experimental gate exists)
- a no-std smoke target that does not import hosted `std` modules (first repo fixture and contract
  tests exist)
- explicit diagnostics for forbidden hosted APIs in system-profile code (first bundled-std import
  diagnostic exists)
- strict-region behavior without implicit auto-promote for system targets (first CLI policy exists)
- a panic/abort policy visible in generated artifacts (first codegen marker exists)
- a minimal ABI/layout test suite
- a primitive freestanding object gate with exact target/panic selection and fail-closed artifact
  audit (first experimental slice exists; aggregate ABI, runtime, link, and boot work remains)
- release notes and support-matrix language that keep the profile experimental until proven
