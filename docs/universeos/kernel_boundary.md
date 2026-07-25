# UniverseOS Kernel Boundary

Status: boundary document only. Nebula does not currently implement a UniverseOS kernel,
freestanding kernel runtime, syscall ABI, driver framework, scheduler, MMU integration, interrupt
model, or bootable artifact.

This document defines the future line between a UniverseOS kernel and hosted/userspace components.
Every kernel-facing claim below is a dependency statement tied to gates in
`docs/universeos/gate_registry.md`; none of the claims are current support.

## Current Posture

Current Nebula support remains hosted:

- C++23 is the only production backend.
- `--target system`, `--target freestanding`, `--profile system`, and `--no-std` are experimental
  contract gates.
- System/no-std checks may reject hosted imports and expose target/runtime/panic markers, but current
  builds still use the hosted C++23 backend and bundled runtime headers.
- A separate exact `freestanding-object` request can emit an audited primitive-only ELF relocatable
  object through generated C++ and an explicitly rooted immutable clang snapshot; it has no
  startup/runtime/link/boot behavior.
- A strict Limine v12.3.2 protocol/ABI candidate fixes image/payload entry ownership and protocol
  bytes, but complete compiler/linker/image-tool closure is absent and `UOS-BOOT-001` remains planned.
- No direct object backend, linker-script flow, boot image, QEMU boot, syscall ABI, driver ABI,
  scheduler, interrupt model, MMU model, or kernel entry path exists.

## Required Gate Dependencies

| Dependency | Required gate IDs | Why it blocks kernel work |
| --- | --- | --- |
| Documentation and non-claim posture | `UOS-DOC-001` | Kernel wording must stay explicit about current non-support. |
| Hosted std rejection | `UOS-CLI-001` | Kernel code cannot depend on hosted bundled `std` modules; current rejection evidence includes stable `NBL-CLI-SYSTEM-STD` diagnostics for system/no-std imports. |
| Strict region behavior | `UOS-CLI-002` | Future kernel/system APIs need fail-closed ownership and escape diagnostics. |
| ABI and layout | `UOS-ABI-001` | Syscalls, entry symbols, context frames, and driver payloads need stable layout rules. |
| No-std smoke boundary | `UOS-CORE-001` | Kernel code requires no-std-safe primitives before runtime work can begin. |
| Backend boundary | `UOS-BE-001` | Future object output must attach behind a backend boundary without changing hosted behavior. |
| Boot protocol, ABI, and toolchain | `UOS-BOOT-001` | Boot work needs pinned target, entry, protocol, memory-layout, toolchain, provenance, and rollback contracts. |
| Freestanding object output | `UOS-BOOT-002` | The primitive prerequisite exists, but kernel-capable types, ABI, runtime, and hardware operations remain unavailable. |
| Linked kernel ELF | `UOS-BOOT-003` | Kernel segments, sections, entry, protocol markers, and provenance must be deterministic and audited. |
| Boot-media assembly | `UOS-BOOT-004` | A linked ELF must be assembled with pinned bootloader inputs before it is called a boot medium. |
| QEMU serial hello | `UOS-BOOT-005` | Bootability must be proven by a bounded smoke before any kernel milestone is claimed. |

These gates are prerequisites, not sufficient proof of production kernel support. Driver,
interrupt, scheduler, MMU, syscall-stability, process-isolation, and security-hardening gates are
future registry work after `UOS-BOOT-005`.

## Kernel Responsibilities

All kernel responsibilities are future work and require at least `UOS-DOC-001`, `UOS-ABI-001`,
`UOS-CORE-001`, `UOS-BE-001`, and `UOS-BOOT-001` through `UOS-BOOT-005` before implementation can
be claimed.

| Future responsibility | Boundary | Gate dependency |
| --- | --- | --- |
| Boot handoff | Receive control from the selected boot path, validate entry assumptions, establish early stack and runtime invariants. | `UOS-BOOT-001`, `UOS-BOOT-003`, `UOS-BOOT-004`, `UOS-BOOT-005` |
| Panic path | Provide `abort` or `trap` behavior without hosted unwind, exception, or C++ standard library dependencies. | `UOS-CLI-001`, `UOS-CORE-001`, `UOS-BOOT-002` |
| Memory ownership | Own physical and virtual memory policy once those policies exist; reject implicit hosted allocation assumptions. | `UOS-ABI-001`, `UOS-CORE-001`, future MMU gate |
| Trap and interrupt dispatch | Own trap/interrupt entry, masking, dispatch, and return contracts after a future interrupt gate exists. | `UOS-DOC-001`, `UOS-ABI-001`, future interrupt gate |
| Syscall dispatch | Validate syscall numbers, ABI version, argument layout, capability handles, and error returns. | `UOS-ABI-001`, `UOS-BOOT-002`, future syscall gate |
| Capability enforcement | Mediate access to kernel objects, drivers, memory regions, and privileged operations. | `UOS-DOC-001`, `UOS-ABI-001`, future security gate |
| Driver coordination | Own hardware-facing registration, interrupt routing, DMA policy, and device capability grants after future driver gates exist. | `UOS-BOOT-005`, future driver gate |
| Scheduling | Own execution-context selection only after timer/context-switch assumptions are specified and tested. | `UOS-ABI-001`, `UOS-BOOT-005`, future scheduler gate |

No row above is implemented today.

## Userspace Responsibilities

Userspace is the place where current Nebula work is credible today. Hosted UniverseOS components
remain responsible for:

- CLI tools and package/build workflows.
- Hosted services and service-control examples.
- Observe/config/state plane logic.
- Thin-host app cores where the host owns platform I/O and rendering.
- Operator-facing policy, configuration, release control, and state management.

Future kernel/userspace separation requires these rules:

| Userspace claim | Boundary | Gate dependency |
| --- | --- | --- |
| Hosted control plane | Runs on the host OS today and does not imply kernel process supervision. | `UOS-DOC-001` |
| Future syscall use | Calls only documented syscall wrappers after a syscall ABI is specified. | `UOS-ABI-001`, future syscall gate |
| Future driver access | Talks through capability-checked handles or userspace service protocols, not raw hardware access by default. | `UOS-DOC-001`, future driver/security gates |
| Future no-std userspace | Uses no-std-safe `core`/`system` layers only after those layers are specified and tested. | `UOS-CORE-001`, `UOS-CLI-001` |

Hosted service-manager examples are not an OS process supervisor and must not be documented as
kernel scheduling, init, service isolation, or driver management.

## Syscall ABI Boundary

Current status: no syscall ABI exists.

Future syscall ABI work may begin only after the ABI/layout and object/boot prerequisites are in
place. The first syscall ABI specification must define:

- target triple and architecture scope
- ABI versioning and compatibility policy
- syscall number allocation and reserved ranges
- register or stack argument passing rules
- scalar, pointer, slice, string/bytes, struct, and enum layout policy
- ownership and lifetime rules for borrowed buffers
- error return representation
- capability-handle representation
- trap instruction or call mechanism
- symbol naming for entry and dispatch stubs
- negative diagnostics for unsupported hosted/runtime dependencies

Gate ties:

| Syscall ABI claim | Required gates before claim |
| --- | --- |
| "Syscall layout is specified" | `UOS-ABI-001` |
| "Syscall code can be emitted into a freestanding artifact" | `UOS-BE-001`, `UOS-BOOT-002` |
| "Syscall artifact can be linked into a kernel ELF" | `UOS-BOOT-003` |
| "Syscall kernel ELF can participate in a boot medium" | `UOS-BOOT-004` |
| "Syscall path can be smoke-tested under QEMU" | `UOS-BOOT-005`, plus a future syscall gate |

Until those gates exist and pass, any syscall examples must be pseudocode only.

## Driver Boundary

Current status: no driver API or driver ABI exists.

Future driver work must keep these boundaries:

- The kernel owns privileged hardware access, interrupt routing, DMA policy, memory mapping, and
  capability grants.
- Drivers translate device protocols into kernel or userspace service contracts.
- Userspace receives device access through capability-checked handles or hosted service protocols.
- Raw pointer, volatile, MMIO, atomics, and interrupt APIs require explicit no-std/system-layer
  specifications before they can be used by drivers.

Gate ties:

| Driver claim | Required gates before claim |
| --- | --- |
| "Driver-facing types have stable layout" | `UOS-ABI-001` |
| "Driver code is no-std-safe" | `UOS-CLI-001`, `UOS-CORE-001` |
| "Driver code can be emitted without hosted runtime dependency" | `UOS-BE-001`, `UOS-BOOT-002` |
| "Driver artifact participates in a linked kernel ELF" | `UOS-BOOT-003` |
| "Driver kernel ELF participates in a boot medium" | `UOS-BOOT-004` |
| "Driver path has boot smoke evidence" | `UOS-BOOT-005`, plus future driver/interrupt gates |

The current gate registry has no positive driver-support gate. Therefore, no document should claim
driver support until new driver, interrupt, and hardware-safety gates are registered and pass.

## Capability And Security Model

Current status: no UniverseOS kernel capability model exists.

Future capability work should assume:

- no ambient authority for userspace
- explicit capability handles for kernel objects and device access
- capability checks at syscall entry
- revocation and lifetime rules for handles
- least-privilege service registration
- audit-friendly denial diagnostics
- no implicit privilege escalation through hosted `std`, native sources, or host bridge code

Gate ties:

| Security claim | Required gates before claim |
| --- | --- |
| "Capability handle layout is stable" | `UOS-ABI-001` |
| "Capability checks run in a freestanding artifact" | `UOS-CORE-001`, `UOS-BOOT-002` |
| "Capability failures are tested at boot/runtime boundary" | `UOS-BOOT-005`, plus future security gate |
| "Hosted native/bridge code cannot bypass system profile" | `UOS-CLI-001`, `UOS-CLI-002` |

Security hardening is not implied by a QEMU hello. It requires separate threat-model, negative-test,
and release-support gates.

## Scheduler Assumptions

Current status: no UniverseOS scheduler exists.

The first kernel milestone should assume the smallest possible scheduling surface:

- A QEMU serial hello may run as a single boot execution path and does not require a scheduler.
- No preemptive scheduling claim exists without timer interrupt, context-frame layout, and
  save/restore tests.
- No process isolation claim exists without address-space, memory manager, syscall, and scheduler
  gates.
- Hosted service manager examples are userspace control-plane examples, not scheduler evidence.

Gate ties:

| Scheduler claim | Required gates before claim |
| --- | --- |
| "Context frame layout is specified" | `UOS-ABI-001` |
| "Scheduler code can be emitted freestanding" | `UOS-BE-001`, `UOS-BOOT-002` |
| "Scheduler is present in a linked kernel ELF" | `UOS-BOOT-003` |
| "Scheduler kernel ELF participates in a boot medium" | `UOS-BOOT-004` |
| "Scheduler behavior is smoke-tested" | `UOS-BOOT-005`, plus future scheduler/interrupt gates |

No scheduler claim is current.

## Memory Manager Assumptions

Current status: no UniverseOS kernel memory manager exists.

Future memory-manager work must define:

- boot memory map ownership and parsing
- physical frame allocator policy
- kernel heap or no-heap policy
- static data initialization and zeroing
- stack layout and guard assumptions
- page table and MMU policy, if any
- DMA-safe memory rules before driver work
- raw pointer, slice, volatile, atomic, and alignment guarantees
- allocator hook ABI for any heap-using language feature

Gate ties:

| Memory-manager claim | Required gates before claim |
| --- | --- |
| "Primitive and aggregate layout is stable enough for memory structures" | `UOS-ABI-001` |
| "No hosted allocation/runtime dependency is present" | `UOS-CORE-001`, `UOS-BOOT-002` |
| "Memory-manager sections are placed by a linker script" | `UOS-BOOT-003` |
| "Memory-manager-linked ELF participates in a boot medium" | `UOS-BOOT-004` |
| "Memory-manager smoke reaches QEMU serial output" | `UOS-BOOT-005`, plus future memory/MMU gates |

No MMU, page-table, allocator, or kernel heap support is current.

## Implementation Boundary

Do not add kernel code until the gate registry has positive implementation gates for the target
slice. Current acceptable work includes the isolated primitive object prerequisite, documentation,
design review, and contract-test planning; linker/runtime/boot work must receive its own positive
gate before it is presented as kernel progress.

Future implementation work must remain reversible:

1. Keep hosted C++23 build/run behavior unchanged.
2. Keep hosted UniverseOS docs and examples useful without freestanding support.
3. Gate object/linker/QEMU work separately.
4. Fail explicitly for unsupported backend, target, panic, hosted runtime, native source, and
   hosted `std` dependencies.
5. Avoid fallback paths that make a hosted artifact look like a kernel artifact.

## Unsupported Wording

Do not use the following phrases in release notes, examples, or public docs unless the relevant
gates are promoted and the claim is tied to passing evidence:

| Unsupported wording today | Minimum gates before reconsidering |
| --- | --- |
| "Nebula kernel runtime" | `UOS-DOC-001`, `UOS-CORE-001`, `UOS-BE-001`, `UOS-BOOT-001` through `UOS-BOOT-005`, plus future runtime gate |
| "UniverseOS driver framework" | `UOS-ABI-001`, `UOS-CORE-001`, `UOS-BOOT-005`, plus future driver and interrupt gates |
| "bootable OS" | `UOS-BOOT-001` through `UOS-BOOT-005`, plus release-support review |
| "syscall ABI support" | `UOS-ABI-001`, `UOS-BOOT-002`, plus future syscall gate |
| "direct object backend" | `UOS-BE-001`, `UOS-BOOT-002`, plus backend-specific contract tests |
| "scheduler support" | `UOS-ABI-001`, `UOS-BOOT-005`, plus future scheduler and interrupt gates |
| "MMU or page-table support" | `UOS-ABI-001`, `UOS-BOOT-005`, plus future memory/MMU gates |

## Non-Goals

The current release does not provide:

- kernel code
- bootloader integration
- direct object backend
- syscall ABI
- driver ABI or driver framework
- interrupt handling
- MMU or page tables
- scheduler or process isolation
- freestanding allocator or kernel heap
- raw hardware APIs
- production UniverseOS kernel support
