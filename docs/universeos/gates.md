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

Default status is `planned` unless a goal updates the status with direct evidence.

## UOS-DOC-001: Language And Specification Gate

Purpose: make the system-profile language contract explicit before expanding UniverseOS claims.

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

## UOS-ABI-001: Layout Golden Tests

Purpose: create repeatable evidence for ABI-relevant layout decisions before any low-level ABI
claim.

Required evidence:

- golden tests for scalar, struct, enum, alignment, and exported C ABI layout where applicable
- checked-in expected artifacts or structured assertions that fail on accidental layout drift
- documentation for which layouts are stable, experimental, or intentionally unspecified
- strict build/test evidence on every platform where a layout claim is made

Exit criteria:

- reviewers can distinguish current narrow C ABI guarantees from future system ABI work
- layout changes require intentional golden updates and release-note review

Non-claim: layout goldens do not prove a syscall ABI, calling convention coverage, linker-script
support, object backend, or freestanding runtime.

## UOS-CORE-001: No-Std Smoke

Purpose: prove the smallest no-std/system-profile program path without importing hosted bundled
`std`.

Required evidence:

- a minimal smoke target that uses system-profile flags and does not import hosted `std`
- generated artifact inspection or test assertions showing runtime profile, target, and panic
  policy markers
- explicit rejection tests for hosted APIs that the smoke does not use
- documentation of what the smoke excludes

Exit criteria:

- the smoke is repeatable in the contract suite
- the result is documented as a compiler/profile contract smoke, not a runtime support claim

Non-claim: this gate does not prove a bootable binary, allocator, panic runtime, syscall layer,
kernel mode, driver support, interrupt handling, MMU integration, or freestanding standard library.

## UOS-BE-001: Backend Interface Boundary

Purpose: define the boundary where future backend work can attach without destabilizing the hosted
C++23 path.

Status: experimental.

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

## UOS-BOOT-001: QEMU Hello Plan

Purpose: define the evidence required for a future boot-level hello-world experiment without
claiming current boot support.

Required evidence before implementation:

- target triple, linker script, entry symbol, panic policy, and artifact format plan
- minimal runtime responsibilities, including startup, stack assumptions, zeroing/copying rules, and
  unavailable hosted APIs
- QEMU command line, expected output, timeout, and failure diagnostics
- rollback plan that keeps hosted CLI/service behavior unaffected

Exit criteria for a future implementation:

- a contract test or documented smoke runs the artifact under QEMU and checks exact output
- failure modes are explicit and do not fall back to hosted execution
- docs continue to label the result as an experiment until support-matrix review promotes it

Non-claim: the plan itself proves nothing about bootability. Even a future QEMU hello would not by
itself prove drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, or production
kernel support.

## UOS-BOOT-002: Freestanding Object Emission Gate

Purpose: prove that Nebula can produce a freestanding object for the chosen target without hosted
runtime or C++ standard library dependencies.

Required evidence before promotion:

- compiler support for producing a freestanding object for `x86_64-unknown-none` or a documented
  equivalent target
- artifact checks proving no hosted `std`, bundled hosted runtime, C++ standard library,
  exceptions, RTTI, threads, filesystem, process, networking, or time dependencies
- stable diagnostics for unsupported hosted APIs and unsupported backend/profile combinations
- rollback evidence proving hosted C++23 build/run behavior is unchanged

Exit criteria for a future implementation:

- object output is deterministic and checked by the contract harness
- unsupported hosted dependencies fail explicitly
- hosted CLI/service behavior remains unchanged

Non-claim: this gate does not prove bootability, linker-script correctness, drivers, interrupts,
MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding
runtime support.

## UOS-BOOT-003: Linker Script And Boot Artifact Gate

Purpose: prove the future object can be linked into a boot artifact with explicit section and entry
contracts.

Required evidence before promotion:

- checked linker script with explicit entry symbol, section placement, alignment, and discard rules
- bootloader configuration and artifact assembly steps that are deterministic and repo-documented
- negative tests for missing entry symbol, missing linker script, or hosted dependency leakage
- rollback evidence proving hosted C++23 build/run behavior is unchanged

Exit criteria for a future implementation:

- a contract test builds the boot artifact from freestanding object output
- linker-script failures are diagnosed explicitly
- hosted build/run behavior remains unchanged

Non-claim: this gate does not prove QEMU output, drivers, interrupts, MMU, scheduler, syscall ABI,
process isolation, production kernel support, or complete freestanding runtime support.

## UOS-BOOT-004: QEMU Serial Hello Gate

Purpose: prove the future boot artifact can run under QEMU and emit a single exact serial hello
line.

Required evidence before promotion:

- QEMU command with bounded timeout and exact expected serial output
- contract test or documented smoke that fails closed on timeout, missing QEMU, missing output, or
  unexpected process status
- artifact checks showing the boot image was built from the freestanding object and linker script
- documentation stating that a serial hello remains experimental and does not imply kernel/driver
  support

Exit criteria for a future implementation:

- QEMU serial output contains exactly the expected hello line
- failure modes are bounded and explicit
- the support matrix still marks the result experimental until runtime and ABI gates mature

Non-claim: this gate does not prove drivers, interrupts, MMU, scheduler, syscall ABI, process
isolation, production kernel support, or complete freestanding runtime support.
