# UniverseOS Convergence

Nebula should treat `universeOS` as a staged product direction, not as a current release claim.
The current codebase is a credible compiler/tooling and backend-first platform seed, but it is not
yet a Swift/Rust/C++23-scale language ecosystem or an operating-system implementation substrate.

## Current Readiness

The auditable snapshot is `docs/universeos/readiness_assessment.md`. It replaces percentage ranges
that had no stable denominator, weighting, revision binding, or gate-to-score calculation.

As of 2026-07-15, the strongest repository-local hosted language/tooling rows score 2 on the
documented 0-to-5 ordinal rubric. Safety, the hosted ABI prototype, ecosystem breadth, and hosted
UniverseOS-adjacent assets score 1. Backend/bootstrap/toolchain independence, the freestanding
runtime, linked/bootable artifact chain, kernel subsystems, and actual UniverseOS userspace all
score 0. Hosted services and control-plane examples do not raise the userspace score. No row is a
release candidate at score 3.

These row scores are not additive and are not a delivery estimate. One passing object gate cannot
be interpreted as a fraction of an operating system. A complete usable UniverseOS still lacks its
freestanding runtime, boot chain, kernel subsystems, drivers, userspace isolation, storage,
networking, recovery, and operating ecosystem.

The strongest Nebula-owned assets today are:

- explicit regions, Rep x Owner inference, and unsafe boundaries
- a working compiler pipeline through C++23 codegen and `clang++`
- an experimental primitive-only, clang-backed `x86_64-unknown-none` object path with a fail-closed
  NIR allowlist, ELF audit, content digest, explicit immutable toolchain snapshot, and transactional
  publication
- a strict Limine v12.3.2 protocol/ABI candidate with content-addressed vendored protocol bytes and
  separate image `_start` / versioned payload entry ownership; full toolchain/link/boot closure is
  still missing
- project/workspace/package tooling, formatter, explain, LSP, and contract tests
- narrow but real `std` slices for CLI, async, TCP, HTTP, JSON, files, process, and logging
- backend-first official packages for service, observe, data, auth, jobs, TLS, crypto, and UI
  experiments, with most non-core packages still explicitly preview

## Parity Gaps

Nebula should not claim broad parity until these gaps are closed.

Swift-class gaps:

- protocols, extensions, associated types, and richer value-semantics ergonomics
- actor or structured-concurrency model beyond the current cooperative async foundation
- stable ABI, mature package workflows, and deep IDE/debugger integration
- SwiftUI/AppKit/UIKit-class UI ownership; current Nebula UI is semantic JSON IR only

Rust-class gaps:

- semantic move, borrow, and lifetime model rather than conservative borrow assistance only
- trait bounds, data-race model, Send/Sync-style guarantees, macro story, and cargo-scale registry
- MIR/LLVM-class optimization and diagnostics maturity

C++23-class gaps:

- direct language support for concepts/templates/constexpr/modules/ranges/allocator-level control
- mature RAII and low-level systems-control breadth
- independent native/object backend; production remains hosted C++23, while the experimental
  primitive object slice still bootstraps through generated C++ and fixed `clang++`

UniverseOS substrate gaps after the primitive object slice and protocol/ABI candidate:

- freestanding/no-std runtime profile
- general target specification, aggregate layout, broader calling conventions, linker scripts,
  complete boot-tool provenance, runtime ABI, and syscall ABI; only the exact primitive object and
  narrow boot-entry ABI candidate exist
- panic/unwind policy, atomics, volatile memory, intrinsics, inline assembly, and hardware-facing APIs
- driver, interrupt, MMU, scheduler, sandbox, capability, and process-isolation models

## Execution Order

1. Stabilize the engineering baseline.
   - Keep GA, installed-preview, repo-preview, and experimental surfaces distinct.
   - Make the full contract suite finish reliably on local and CI hosts.
   - Avoid hiding real failures behind broad fallback behavior.

2. Harden the language core.
   - Add constrained generics through traits/protocols or an equivalent model.
   - Define the system profile for strict region behavior, explicit heap use, and unsafe audit.
   - Add collections, slices, maps, visibility, closures, and deeper pattern matching only after
     their semantics align with the safety model.

3. Industrialize the compiler.
   - Evaluate LLVM, Cranelift, or a direct object backend to reduce long-term C++ transpiler
     dependency.
   - Add debug info, stable ABI planning, optimization passes, and incremental build/indexing.
   - Split oversized implementation units before adding more platform surface.

4. Platformize through forcing apps.
   - Promote service, observe, data, auth, jobs, TLS, and crypto from preview only when deployment,
     upgrade, recovery, observability, and security contracts are repeatable.
   - Keep UI production-adjacent work on the thin-host split: Nebula owns state, validation,
     transitions, and snapshots; the host owns rendering, accessibility, platform I/O, and signing.
   - Use Nebula for universeOS tools, control plane, services, and app-core first; do not position it
     as a kernel or driver language until the freestanding substrate exists.

## Claim Gate

Public universeOS positioning should stay behind these gates:

- full contract suite completes with bounded per-case timeouts and no orphaned child processes
- at least one backend-first internal app is cleanly buildable, deployable, upgradeable,
  observable, and recoverable
- thin-host command/query/event bridge is stable enough for a real host shell
- strict system profile has explicit answers for allocation, escape, unsafe, concurrency, panic,
  and ABI behavior; see `docs/system_profile.md`
- experimental `--target system` / `--no-std` gates remain green and keep rejecting hosted `std`
  imports instead of silently falling back to the hosted runtime
- the experimental primitive object gate remains green without being presented as a no-std runtime,
  direct object backend, linked image, QEMU boot, kernel, or supported OS substrate
- direct OS-substrate claims still require a no-std/freestanding runtime prototype and boot evidence
  rather than relying on the host compiler process or the primitive object gate alone
