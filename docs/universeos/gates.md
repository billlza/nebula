# UniverseOS Gates

UniverseOS is a staged and evidence-gated direction for Nebula. These gates define what must be
proved before repository documentation, examples, or release notes can make stronger UniverseOS
claims.

The machine-checkable gate ID registry lives in `docs/universeos/gate_registry.md`.

Passing an early gate does not imply kernel, driver, interrupt, MMU, scheduler, freestanding
runtime, syscall ABI, or backend-independent object-code support. Those claims remain unsupported
until a later gate names the contract, implementation, tests, and release posture explicitly.

## Gate Status Vocabulary

- `planned`: the gate defines future work only
- `experimental`: an implementation or contract test exists, but the surface is not a support
  promise
- `candidate`: the gate has repeatable tests and documentation, but still needs release review
- `accepted`: the gate has passing tests, support-matrix wording, and rollback guidance

New gates default to `planned`; the machine registry is authoritative for the current status of
each gate.

Registry v2 also makes dependencies authoritative. A gate cannot be promoted beyond its least
mature dependency, and the validator rejects unknown, duplicate, self-referential, or cyclic
dependencies. Hosted-adjacent work is never a substitute for a freestanding dependency.

## UOS-DOC-001: Language And Specification Gate

Purpose: make the system-profile language contract explicit before expanding UniverseOS claims.

Status: experimental.

Depends on: none.

Required evidence:

- language/spec documentation for allocation, escape, unsafe, concurrency, panic, ABI, and runtime
  boundaries
- explicit non-goals for kernel, driver, interrupt, MMU, scheduler, syscall ABI, freestanding
  runtime, and backend independence
- support-matrix wording that keeps the profile experimental until implementation gates pass
- links from `README.md`, `docs/system_profile.md`, and `docs/universeos_convergence.md` to the
  relevant staged plan

Exit criteria:

- reviewers can tell which claims are current, experimental, and future-only
- no public document depends on implied UniverseOS support that lacks a named test or artifact

Non-claim: this gate is documentation readiness only. It does not prove a no-std build,
freestanding runtime, kernel target, or backend implementation.

## UOS-CLI-001: System-Profile Std Import Rejection

Purpose: prove that system-profile compilation rejects hosted bundled `std` imports instead of
silently falling back to the hosted runtime.

Status: experimental.

Depends on: `UOS-DOC-001`.

Required evidence:

- contract tests that compile/check a system-profile target importing hosted `std`
- expected diagnostic for rejected bundled `std` usage, currently `NBL-CLI-SYSTEM-STD`
- coverage for the relevant entry paths: `--target system|freestanding|<triple>`, `--profile
  system`, and `--no-std`
- full strict build/test run after any CLI or project-loader change

Exit criteria:

- the compiler fails closed for hosted `std` imports under the system profile
- diagnostic text explains that hosted runtime services are unavailable in that profile

Non-claim: rejecting `std` imports does not prove a freestanding standard library, kernel runtime,
or hardware-facing API.

## UOS-CLI-002: Strict-Region System-Profile Gate

Purpose: prove that system-profile code uses strict region behavior and does not silently promote
escaping values across system boundaries.

Status: experimental.

Depends on: `UOS-DOC-001`.

Required evidence:

- contract tests that exercise region escapes under system-profile settings
- diagnostics showing strict-region behavior without relying on an explicit user-provided
  `--strict-region` flag
- negative tests proving no fallback auto-promote path is used for the system profile
- documentation that separates hosted convenience behavior from system-profile behavior

Exit criteria:

- system-profile region violations fail with stable diagnostics
- hosted behavior is unchanged unless the goal explicitly updates that contract

Non-claim: strict region diagnostics do not prove a kernel memory model, MMU integration, allocator
contract, or interrupt-safety story.

## UOS-LANG-001: Low-Level Language Soundness Gate

Purpose: close the language-semantic gaps that would otherwise make low-level runtime or kernel
code depend on undocumented alias, initialization, lifetime, or concurrency behavior.

Status: planned.

Depends on: `UOS-DOC-001`, `UOS-CLI-002`.

Required evidence before promotion:

- normative move, lifetime, aliasing, initialization, and destruction rules with no hidden hosted
  runtime dependency
- typed raw-pointer, volatile, atomic, intrinsic, and unsafe-boundary contracts
- concurrency ownership and data-race guarantees with adversarial soundness tests
- stable diagnostics plus a compatibility and migration policy for low-level semantic changes

Exit criteria for a future implementation:

- the language implementation and independent tests agree on every supported low-level operation
- unsupported or unsound operations fail explicitly at the language boundary
- later ABI/runtime gates consume the normative contract rather than host-language behavior

Non-claim: a specified low-level language contract does not prove implementation soundness, a
system ABI, backend independence, a freestanding runtime, a kernel, or userspace support.

## UOS-ABI-001: Layout Golden Tests

Purpose: create repeatable evidence for ABI-relevant layout decisions before any low-level ABI
claim.

Status: experimental.

Depends on: `UOS-DOC-001`.

Required evidence:

- hosted C++23 golden tests and structured assertions for scalar C ABI exports, struct field order,
  enum payload lowering, duplicate symbol rejection, and no-export library rejection
- stable diagnostics rejecting public C ABI exports for unsupported hosted types such as `String`,
  `Result`, struct, enum, `ref` parameters, extern exports, and generic functions
- documentation for which hosted C ABI/layout behavior exists today and which system ABI,
  object-backend, syscall ABI, and freestanding layout work remains future-only
- strict build/test evidence before any release note promotes a stronger ABI or platform-specific
  layout claim

Exit criteria:

- reviewers can distinguish current narrow C ABI guarantees from future system ABI work
- layout changes require intentional golden updates and release-note review

Non-claim: layout goldens do not prove a syscall ABI, calling convention coverage, linker-script
support, object backend, or freestanding runtime.

## UOS-ABI-002: Freestanding System ABI Gate

Purpose: define and prove the target ABI independently of hosted C++ representation choices.

Status: planned.

Depends on: `UOS-ABI-001`, `UOS-LANG-001`.

Required evidence before promotion:

- target data layout, scalar/aggregate representation, alignment, calling convention, stack,
  unwind, and symbol contracts
- cross-language fixtures for every supported argument, return, aggregate, pointer, enum, and
  error representation
- separate versioning rules for compiler, runtime, boot, syscall, and package ABI surfaces
- negative and cross-host tests that reject unsupported surfaces rather than inheriting host C++
  behavior

Exit criteria for a future implementation:

- ABI fixtures are repeatable under every supported target/toolchain pair
- ABI drift is detected before publication and has an explicit migration path
- the freestanding backend/runtime use this contract directly

Non-claim: a freestanding system ABI contract does not prove a direct backend, linker, runtime,
bootable artifact, syscall implementation, kernel, or userspace support.

## UOS-CORE-001: No-Std Smoke

Purpose: prove the smallest no-std/system-profile program path without importing hosted bundled
`std`.

Status: experimental.

Depends on: `UOS-CLI-001`, `UOS-CLI-002`.

Required evidence:

- a minimal smoke target that uses system-profile flags and does not import hosted `std`
- generated artifact inspection or test assertions showing runtime profile, target, and panic
  policy markers
- explicit rejection tests for hosted APIs that the smoke does not use
- library-layer documentation distinguishing future `core`, hosted `std`, and future `system` APIs
  without claiming `core::` or `system::` imports work today
- documentation of what the smoke excludes

Exit criteria:

- the smoke is repeatable in the contract suite
- the result is documented as a compiler/profile contract smoke, not a runtime support claim

Non-claim: this gate does not prove a bootable binary, allocator, panic runtime, syscall layer,
kernel mode, driver support, interrupt handling, MMU integration, or freestanding standard library.

## UOS-CORE-002: Freestanding Core And Runtime Gate

Purpose: provide the smallest real freestanding core/runtime needed before a linked artifact can be
called a kernel prerequisite rather than a raw object-emission experiment.

Status: planned.

Depends on: `UOS-BE-002`, `UOS-CORE-001`.

Required evidence before promotion:

- freestanding core types and operations with no hosted standard-library, C++ runtime,
  operating-system, or hidden allocation dependency
- startup, data initialization, panic, allocation-hook, termination, and target-runtime ABI
  implementations bound to `UOS-ABI-002`
- tests for initialization order, panic/abort, allocation failure, atomics, volatile access,
  intrinsics, and forbidden hosted symbols
- bounded resource, compatibility, rollback, and security contracts for each target/runtime pair

Exit criteria for a future implementation:

- runtime artifacts and symbol closures are machine-audited and reproducible
- failure paths are explicit and never fall back to hosted services
- later link/boot gates consume the exact accepted runtime artifacts

Non-claim: a freestanding core and runtime does not prove a linked or bootable kernel, interrupts,
MMU, scheduler, syscalls, drivers, process isolation, or userspace support.

## UOS-BE-001: Backend Interface Boundary

Purpose: define the boundary where future backend work can attach without destabilizing the hosted
C++23 path.

Status: experimental.

Depends on: `UOS-DOC-001`.

Required evidence:

- written interface contract between typed/NIR-level compiler state and backend-specific emission
- tests proving existing hosted C++23 codegen behavior remains unchanged
- documentation that no backend selector or fallback backend exists in the MVP
- review notes explaining what is interface, what is implementation detail, and what remains
  future-only

Exit criteria:

- future backend work has a named boundary and does not require broad parser/typechecker rewrites
- unsupported backend requests fail explicitly instead of falling back to hosted C++ output

Non-claim: defining a backend boundary does not prove LLVM, Cranelift, direct object output,
cross-compilation, boot artifacts, or backend independence.

## UOS-BE-002: Independent Backend And Bootstrap Gate

Purpose: prove a production system-code path that does not require generated C++ as its only
backend and can be rebuilt from pinned, auditable inputs.

Status: planned.

Depends on: `UOS-ABI-002`, `UOS-BE-001`.

Required evidence before promotion:

- a supported direct-object or reproducible stage-0/stage-1 path independent of generated C++
- pinned assembler, linker, runtime, and compiler inputs with closed content-bound provenance
- bootstrap equivalence, deterministic rebuild, optimization, relocation, debug-information, and
  cross-host qualification evidence
- explicit backend selection and diagnostics with no fallback to hosted C++ or an unverified host
  toolchain

Exit criteria for a future implementation:

- bootstrap and target artifacts reproduce under the declared host/target matrix
- every executable tool and linker-selected input is provenance-bound
- hosted C++ remains an explicit compatibility backend rather than hidden independence evidence

Non-claim: an independent backend and bootstrap path does not prove a freestanding runtime, linked
kernel, boot medium, kernel subsystems, or UniverseOS userspace.

## UOS-BOOT-001: Boot Protocol, ABI, And Toolchain Contract

Purpose: freeze the protocol, ABI, memory-layout, toolchain, and supply-chain inputs that every
later link, media, and QEMU gate must consume.

Status: planned.

Depends on: `UOS-ABI-002`.

Implemented candidate subset (not gate completion):

- `boot/uos-x86_64-limine-v1/contract.manifest` is a strict canonical protocol/ABI manifest for
  Limine v12.3.2 and its bootstrap-pinned protocol commit
- vendored protocol header/license bytes are content-addressed and protected from checkout newline
  conversion
- typed parsing/serialization rejects unknown, reordered, duplicated, missing, non-ASCII,
  non-canonical, or unsafe-path fields
- image `_start` and payload `__nebula_uos_payload_entry_v1` ownership are separated
- the gate stays planned because complete clang/ld.lld/bootloader/image-tool provenance and
  compatibility evidence do not yet exist

Required evidence before gate promotion:

- exact target triple, fixed high-half virtual-address contract, entry symbol, panic policy, and
  executable format
- a versioned Limine release, protocol-header revision and digest, selected base revision, request
  start/end marker layout, and an explicit runtime support check; no floating branch or unversioned
  downloaded header may enter the build
- x86-64 entry ABI, including the restricted System V calling convention, unavailable floating
  point/SIMD state, minimum boot stack, zeroed/copy-initialized data responsibilities, and the rule
  that the entry point never returns
- an exact supported `clang`/`ld.lld` combination plus provenance, checksums, licenses, and
  compatibility evidence for every external boot/image tool
- deterministic configuration and artifact manifests, plus a rollback plan that leaves hosted
  CLI/service behavior unchanged

Exit criteria for a future implementation:

- a repository-owned manifest pins every external revision and content digest
- contract tests validate the protocol markers, base-revision support check, entry ABI, high-half
  address policy, and exact tool identity before link or image assembly begins
- unsupported or mismatched tools fail explicitly and never fall back to hosted execution

Non-claim: this contract does not prove a linked kernel, boot medium, QEMU output, drivers,
interrupts, MMU, scheduler, syscall ABI, process isolation, or production kernel support.

## UOS-BOOT-002: Freestanding Object Emission Gate

Purpose: prove that Nebula can produce a freestanding object for the chosen target without hosted
runtime or C++ standard library dependencies.

Status: experimental.

Depends on: `UOS-BE-001`, `UOS-CORE-001`.

Current experimental evidence:

- the publication implementation is currently a macOS/Linux host capability; the Windows
  compiler/tooling release fails this request explicitly instead of compiling unsafe POSIX shims
- the exact request `nebula build <path> --emit freestanding-object --target
  x86_64-unknown-none --panic trap --freestanding-toolchain-root <absolute-clang-root>` produces an
  ELF64 `ET_REL` / `EM_X86_64` object
- the reachable subset is deliberately limited to `Int`, `Bool`, `Void`, direct resolved
  Nebula-defined calls, and stack/non-owning storage; unsupported features fail before toolchain
  execution with stable `NBL-BE-FS-*` diagnostics
- generated bootstrap C++ has no includes, hosted Nebula runtime reference, C++ standard-library
  reference, exceptions, RTTI, allocation, or hosted service surface
- one explicitly rooted, content-bound, pre/post-revalidated, minimal-environment,
  30-second-bounded `clang++` cross-target invocation is followed
  by a repo-local bounded ELF audit that rejects undefined symbols, dynamic/TLS/exception/init
  sections, W^X or oversized allocations, invalid relocations, malformed ranges, or an invalid
  `__nebula_uos_payload_entry_v1` contract and payload `_start` rejection
- catchable parent termination (`SIGINT`/`SIGTERM`, with the same implementation contract for
  `SIGHUP`/`SIGQUIT`) seals the compiler process group before the original signal is re-delivered;
  the toolchain then enters a non-executable prepared-frozen state and restores the caller only
  after staging cleanup, commit/rollback, guard disarm, and output-lock release; uncatchable parent
  death remains an explicit future-supervisor boundary
- restore or cleanup failure is status `125` and preserves an explicit `Absent`, `Committed`, or
  `CleanupIncomplete` artifact disposition; a committed restore failure never performs an unlocked
  rollback
- `BLD-017` through `BLD-020` cover the object, request matrix, fail-closed NIR allowlist,
  deterministic output, isolated toolchain, concurrency lock, no-replace publication, and failure
  cleanup paths, including external `SIGINT`/`SIGTERM`
- hosted C++23 default build/run behavior remains separately protected by `BLD-014` and `RUN-081`

Exit criteria before candidate promotion:

- repeat the object and rollback evidence on supported macOS/Linux release hosts, while preserving
  the explicit Windows host-unsupported contract
- define whether the experimental emitted C++ sidecar remains a diagnostic artifact or becomes
  private before support promotion
- keep metadata digest validation and ELF audit fail-closed as the primitive subset expands

Non-claim: this is a clang-backed bootstrap object path, not a direct object backend or independent
compiler. It does not prove bootability, linker-script correctness, startup/runtime initialization,
drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or
complete freestanding runtime support.

Security boundary: this evidence assumes a trusted local `clang++` and a caller-controlled output
directory. Its rollback is process-level, not a hostile-directory sandbox or power-loss-durable
filesystem transaction.

## UOS-BOOT-003: Deterministic Linked Kernel ELF Gate

Purpose: prove that the future object and a repository-owned protocol adapter can be linked into a
bounded, audited kernel ELF with explicit segment, section, entry, and provenance contracts.

Status: planned.

Depends on: `UOS-BOOT-001`, `UOS-BOOT-002`, `UOS-CORE-002`.

Required evidence before promotion:

- a fixed linker script with explicit program headers, high-half placement, entry symbol, alignment,
  Limine request retention, and discard rules
- a fixed `ld.lld` invocation with no shell, search path, plugin, response file, implicit library,
  build ID, undefined symbol, warning downgrade, or orphan-section acceptance
- bounded ELF64 `ET_EXEC` auditing for entry placement, W^X, segment overlap, file/memory ranges,
  dynamic/interpreter/TLS/unwind/relocation absence, alloc-section ownership, and resource limits
- metadata binding the source object, linker script, protocol adapter, exact tool identity, and
  linked ELF by SHA-256
- negative tests for malformed or hostile ELF output, missing entry/protocol markers, dependency
  leakage, timeout, containment loss, path attacks, concurrent publication, and partial failure
- rollback evidence proving hosted C++23 build/run behavior is unchanged

Exit criteria for a future implementation:

- a contract test reproducibly builds the same audited kernel ELF under the pinned toolchain
- linker and ELF-policy failures are diagnosed explicitly and published transactionally
- hosted build/run behavior remains unchanged

Non-claim: a linked kernel ELF is not a boot medium and does not prove bootloader integration, QEMU
output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel
support, or complete freestanding runtime support.

## UOS-BOOT-004: Version-Pinned Boot Media Assembly Gate

Purpose: prove that the audited kernel ELF can be assembled with the pinned Limine release and
configuration into a deterministic, provenance-bound boot medium.

Status: planned.

Depends on: `UOS-BOOT-003`.

Required evidence before promotion:

- repository-owned, version-pinned Limine configuration with the exact protocol and kernel path
- bootloader binaries and media tools verified against the `UOS-BOOT-001` provenance manifest
- deterministic ISO or raw-image assembly whose metadata binds the kernel ELF, configuration,
  bootloader inputs, assembly command, and final image digests
- bounded no-shell process execution, explicit tool/timeout/containment failures, no-replace
  transactional publication, and negative tests for missing or tampered inputs
- rollback evidence proving hosted C++23 build/run behavior is unchanged

Exit criteria for a future implementation:

- a contract test assembles and re-audits the boot medium from the exact `UOS-BOOT-003` artifact
- same-input assembly is byte-identical within the pinned toolchain and declared filesystem/time
  normalization contract
- all provenance and publication failures are explicit

Non-claim: an assembled boot medium does not prove that firmware or QEMU can execute it, serial
output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel
support, or complete freestanding runtime support.

## UOS-BOOT-005: QEMU Serial Hello Gate

Purpose: prove the future boot artifact can run under QEMU and emit a single exact serial hello
line.

Status: planned.

Depends on: `UOS-BOOT-004`.

Required evidence before promotion:

- QEMU command with bounded timeout and exact expected serial output
- contract test or documented smoke that fails closed on timeout, missing QEMU, missing output, or
  unexpected process status
- artifact checks showing the boot image was built from the freestanding object and linker script
- provenance checks showing the boot image is exactly the accepted `UOS-BOOT-004` artifact
- documentation stating that a serial hello remains experimental and does not imply kernel/driver
  support

Exit criteria for a future implementation:

- QEMU serial output contains exactly the expected hello line
- failure modes are bounded and explicit
- the support matrix still marks the result experimental until runtime and ABI gates mature

Non-claim: this gate does not prove drivers, interrupts, MMU, scheduler, syscall ABI, process
isolation, production kernel support, or complete freestanding runtime support.
