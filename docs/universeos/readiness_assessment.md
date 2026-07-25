# UniverseOS Readiness Assessment

This is an evidence ledger, not a delivery percentage or schedule. It answers “how far” by naming
what the repository can prove, what it cannot prove, and which gate must move next. Scores are
ordinal within a row and must not be added or converted into an overall completion percentage.

Assessment date: 2026-07-16.

Revision basis: this assessment covers the source-frozen working tree rooted at
`0a1429a9a596ce8fc9d6681bdfaac30435059231`, with its uncommitted changes identified by source
fingerprint `acf269f15b5f7bc0e5cfdf2dfd2a539da1d114a70b097e480c187f803fa92565`. The fingerprint
covers the sorted path, file kind, permission bits, and contents of every entry returned by
`git ls-files -co --exclude-standard`; this assessment file is excluded to avoid a self-reference.
Strict build, native-test, and full contract-suite evidence applies only when the pre- and
post-validation fingerprints match this value. Release claims still require the eventual commit
and its own clean, cross-platform validation record.

Tool-provenance basis: the production backend still emits C++23 and consumes an external host
compiler/linker. `UOS-BOOT-002` accepts a caller-selected absolute clang toolchain root and binds the
resolved executable identity for one experimental invocation, but it is not a release-pinned,
closed compiler/linker/runtime toolchain. The Limine v12.3.2 protocol/ABI candidate is likewise only
one input to planned `UOS-BOOT-001`; neither fact is independent compiler or boot-chain evidence.

## Rubric

| Score | Meaning |
| --- | --- |
| 0 | No implementation evidence for the capability. A plan or adjacent prerequisite does not score. |
| 1 | Narrow prototype or experimental contract evidence exists; unsupported outside the named slice. |
| 2 | Repeatable repository-local implementation exists across meaningful cases, but the capability is not a stable independent platform surface. |
| 3 | Candidate contract is repeatable across supported hosts, has migration/rollback guidance, and is ready for release review. |
| 4 | Accepted and supported production capability with compatibility, operations, and security evidence. |
| 5 | Mature independent ecosystem capability with broad libraries, tooling, interoperability, and sustained compatibility evidence. |

A row may advance only when its evidence is machine-checkable, the full relevant strict suite is
green, documentation names non-claims, and the registry gate is promoted intentionally. Adjacent
work cannot compensate for a missing hard dependency. In particular, hosted services cannot raise
the boot or kernel score.

## Evidence Matrix

| Capability | Score | Confidence | Current evidence | Blocking evidence before the next score |
| --- | ---: | --- | --- | --- |
| Language semantics and type system | 2 | medium | Working parser/typechecker and hosted codegen; generics, async foundations, region analysis, enums/results, pattern matching, and contract cases exist. | Trait/protocol bounds, closures, more complete collections/slices, precise move/borrow semantics, and stable compatibility policy. |
| Safety and ownership model | 1 | medium | Explicit regions, `Rep x Owner` inference, unsafe boundaries, and strict system-profile rejection are tested by `UOS-CLI-002`. | Semantic lifetime/move rules, alias model, data-race guarantees, concurrency ownership, unsafe audit rules, and adversarial soundness testing. |
| Hosted ABI prototype | 1 | high | Hosted layout goldens (`UOS-ABI-001`) and the primitive clang-backed `UOS-BOOT-002` `ET_REL` slice provide narrow representation evidence. | Complete and promote the planned freestanding system ABI contract (`UOS-ABI-002`), including aggregate/calling-convention coverage, target layout, versioning, compatibility, and cross-language fixtures. |
| Backend, bootstrap, and toolchain independence | 0 | high | `UOS-BE-001` is an interface boundary, not an independent backend. Production emits C++23 through external host tools, and `UOS-BOOT-002` is a caller-rooted clang bootstrap path. | Complete `UOS-BE-002`: a supported direct backend or reproducible stage-0/stage-1 bootstrap, equivalence evidence, pinned linker/runtime inputs, debug/optimization support, and cross-host rebuild qualification. |
| Compiler, package, and developer tooling | 2 | medium | Build/run/check/test/bench/package/workspace flows, formatter, explain, LSP, and a large contract suite are repository-local. Experimental hosted build/run now also have bounded compiler-child execution with explicit containment status under the documented trusted-tool boundary, compiler-discovered native header closure, versioned content-bound build/reuse identity, process-level transactional publication, pre-commit signal cancellation, and private verified execution copies. | Linker-selected inputs are not yet a closed provenance set; `test`/`bench` still bypass the shared hosted lifecycle; durable multi-file recovery, execution-time signal cleanup, cross-host release evidence, stable distribution/registry, incremental/indexing, debugger, performance, and smaller-module evidence remain. |
| Standard library and package ecosystem | 1 | medium | Narrow hosted `std` slices and backend-first official service/observe/data/auth/jobs/TLS/crypto/UI experiments exist. | Stable core/collections/concurrency APIs, package registry and compatibility policy, broad production libraries, sustained security/maintenance evidence, and independent ecosystem adoption. |
| Freestanding runtime | 0 | high | `UOS-BOOT-002` proves only primitive object emission; it explicitly contains no runtime. | Startup/data initialization, allocator hooks, panic runtime, raw pointer/volatile/atomic/intrinsic contracts, freestanding core library, and runtime ABI tests. |
| Linked and bootable artifact chain | 0 | high | A bounded audited `ET_REL` prerequisite and strict Limine v12.3.2 protocol/ABI candidate exist; neither is a linked image, and complete compiler/linker/image-tool closure, boot medium, and QEMU proof do not exist. | Complete and promote `UOS-BOOT-001` and `UOS-CORE-002`, retain the explicit `UOS-BOOT-002` boundary, then implement `UOS-BOOT-003`, `UOS-BOOT-004`, and `UOS-BOOT-005`, in that order. |
| Kernel subsystems | 0 | high | No kernel entry/runtime, interrupts, memory manager, scheduler, syscall layer, driver model, storage, networking, or isolation implementation exists. | Separate evidence gates and implementations for every subsystem after the boot/runtime prerequisites. |
| Hosted UniverseOS-adjacent application assets | 1 | medium | Hosted tools, services, control-plane examples, and thin-host app-core experiments are useful reusable assets, but they execute on existing host operating systems. | Stable hosted package/platform contracts and an explicit future port boundary; this row can never substitute for kernel or userspace proof. |
| UniverseOS userspace and process platform | 0 | high | No program executes across a Nebula-owned process, syscall, capability, storage, network, install/update, recovery, or system-service boundary. Hosted services are excluded from this score. | A real OS process/runtime boundary, capability model, system services, install/update/recovery, storage/network stack, GUI/accessibility shell, and end-to-end operation on the future kernel. |

Evidence-infrastructure boundary: hosted artifact transactions provide identity- and digest-bound
process-level publication and rollback; they are not journaled, crash-atomic, or power-loss-durable
filesystem transactions. The Darwin/Linux contract-runner lifecycle is trusted-cooperative:
repository-controlled descendants that may detach or outlive their parent must preserve the bounded
writer-FD anchor stack, and cleanup revalidates stable process identities. It is not OS-enforced
recursive containment or a hostile-child sandbox; Windows Job Object containment is a separate
test-infrastructure capability. Likewise, `UOS-BOOT-002` remains a primitive caller-rooted,
clang-backed, audited ELF64 `ET_REL` bootstrap slice. These mechanisms strengthen evidence
collection; they do not establish an independent compiler/backend, freestanding runtime, linked or
bootable artifact, kernel, or UniverseOS userspace.

## Decision Summary

No assessed capability is at candidate score 3. The strongest current lane is repository-local
hosted language/tooling at score 2, but the production compiler is not self-hosting and is not
independent of a C++23 toolchain. Every actual OS-substrate row is score 0. Hosted-adjacent
application assets are tracked separately at score 1 and cannot raise UniverseOS userspace above 0.

The shortest honest path is therefore not “start writing kernel subsystems.” It is:

1. preserve the strict hosted baseline and complete `UOS-LANG-001` after its `UOS-DOC-001` and
   `UOS-CLI-002` prerequisites, defining the low-level language/safety semantics needed by system
   code;
2. complete `UOS-ABI-002` only after both `UOS-ABI-001` and `UOS-LANG-001` satisfy its required
   maturity;
3. after `UOS-ABI-002`, advance two lanes in parallel: complete `UOS-BE-002` after `UOS-BE-001`,
   then join it with `UOS-CORE-001` at a real `UOS-CORE-002` freestanding core/runtime; separately,
   complete the compiler/linker/image-tool closure required by `UOS-BOOT-001`;
4. join `UOS-BOOT-001`, the current experimental `UOS-BOOT-002`, and `UOS-CORE-002` at linked ELF
   (`UOS-BOOT-003`), then advance boot media (`UOS-BOOT-004`) and bounded QEMU serial execution
   (`UOS-BOOT-005`) in order;
5. add kernel subsystems behind their own ABI, safety, resource, and security gates;
6. move hosted UniverseOS-adjacent services onto that substrate only after the boundary exists.

This ordering is intentionally dependency-driven. A serial hello would be meaningful boot evidence
but would still leave virtually all kernel, driver, isolation, storage, networking, recovery, and
ecosystem work outstanding.

## Remaining Distance Ledger

There is no defensible single completion percentage. The dependency graph has two distinct gaps,
and the larger one contains kernel subsystems whose production acceptance gates do not exist yet.

For Nebula to become an independent, generally usable language platform, the next hard gates are:

1. stabilize the low-level language contract: move/lifetime/alias rules, concurrency ownership,
   atomics/volatile/raw-pointer/intrinsic surfaces, ABI and compatibility policy;
2. define and validate the freestanding system ABI, including target layout, calling convention,
   stack/unwind rules, cross-language fixtures, versioning, and explicit rejection of unsupported
   surfaces;
3. replace the production C++23-only route with a reproducible direct backend or a documented
   stage-0/stage-1 bootstrap, while keeping hosted C++ as an explicit compatibility backend;
4. provide a freestanding `core` and runtime with startup, panic, allocation hooks, data
   initialization, target descriptions, linker inputs, and debug information;
5. close production tool provenance and lifecycle gaps, including linker-selected files,
   `test`/`bench`, crash recovery, cross-platform namespace/ACL behavior, and execution cleanup;
6. prove reproducible bootstrap, distribution, package compatibility, debugger/indexer support,
   performance envelopes, and multi-host release qualification.

For UniverseOS, those language-platform gates are prerequisites rather than OS progress. The exact
hard joins are `(UOS-DOC-001, UOS-CLI-002) -> UOS-LANG-001`, then
`(UOS-ABI-001, UOS-LANG-001) -> UOS-ABI-002`. After `UOS-ABI-002`,
`(UOS-ABI-002, UOS-BE-001) -> UOS-BE-002` and then
`(UOS-BE-002, UOS-CORE-001) -> UOS-CORE-002` form one lane, while
`UOS-ABI-002 -> UOS-BOOT-001` may advance in parallel. The join
`(UOS-BOOT-001, UOS-BOOT-002, UOS-CORE-002) -> UOS-BOOT-003` must close before
`UOS-BOOT-004 -> UOS-BOOT-005` can follow. After that, the repository still needs separately
specified and tested gates for memory management/MMU, interrupts,
scheduling, syscalls/capabilities, drivers/DMA, storage, networking, process isolation, userspace,
updates/recovery, and the product shell. The current repository therefore remains much closer to a
promising hosted language/toolchain than to an independently bootable operating system.

## Reassessment Rules

- Record the exact repository commit and toolchain manifest for each reassessment.
- Change a score only when a named implementation gate and its evidence advance; prose or test
  count alone is insufficient.
- Keep score 0 when only an adjacent prerequisite exists.
- Never average these rows into a product completion percentage or delivery date.
- Lower a score when evidence becomes non-repeatable, unsupported, insecure, or incompatible.
- Keep historical assessments in version control; do not rewrite a score merely to improve product
  positioning.
