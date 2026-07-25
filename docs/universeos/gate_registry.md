# UniverseOS Gate Registry

This registry is the machine-checkable source for UniverseOS gate IDs. It does not implement
runtime behavior. It names evidence requirements and non-claims so future work cannot silently turn
an experimental system-profile check into an unsupported operating-system claim.

Gate status values:

- `planned`: future work only
- `experimental`: an implementation or contract test exists, but it is not a support promise
- `candidate`: repeatable tests and documentation exist, pending release review
- `accepted`: passing tests, support-matrix wording, and rollback guidance exist

The JSON block below is intentionally parsed by `scripts/check_universeos_gate_docs.py`.
Every non-planned gate must list exact existing contract IDs in `evidence_cases`; the validator
scans `tests/cases/**/case.toml` and rejects missing, duplicate, or stale references.
Every gate also declares `depends_on`. A gate cannot be promoted beyond the least mature dependency;
unknown, duplicate, self-referential, or cyclic dependencies invalidate the registry.

```json
{
  "gate_registry_version": 2,
  "gate_naming": {
    "UOS-DOC": "documentation and language/specification gates",
    "UOS-CLI": "CLI and system-profile gates",
    "UOS-LANG": "low-level language-soundness gates",
    "UOS-ABI": "ABI and layout gates",
    "UOS-CORE": "no-std and core-library gates",
    "UOS-BE": "backend and object-code gates",
    "UOS-BOOT": "boot and freestanding gates"
  },
  "source_doc_mapping": [
    {
      "path": "docs/system_profile.md",
      "maps_to": ["UOS-CLI-001", "UOS-CLI-002", "UOS-LANG-001", "UOS-CORE-001", "UOS-BOOT-002"],
      "relationship": "Current system-profile documentation defines the experimental std-import rejection, strict-region behavior, no-std smoke boundary, primitive freestanding object slice, and explicit non-goals."
    },
    {
      "path": "docs/support_matrix.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-LANG-001", "UOS-ABI-001", "UOS-ABI-002", "UOS-CORE-001", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "Support matrix documentation records every UniverseOS gate as experimental evidence-only or planned future-only work and prevents release/support posture from silently promoting OS/runtime support."
    },
    {
      "path": "docs/universeos_convergence.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-LANG-001", "UOS-ABI-001", "UOS-ABI-002", "UOS-CORE-001", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002"],
      "relationship": "UniverseOS convergence documentation keeps the staged direction behind evidence gates and prevents broad OS-substrate positioning without named proof."
    },
    {
      "path": "docs/universeos/readiness_assessment.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-LANG-001", "UOS-ABI-001", "UOS-ABI-002", "UOS-CORE-001", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "The revision-bound ordinal evidence ledger replaces unauditable product-completion percentages and keeps adjacent hosted work from inflating freestanding, boot, or kernel readiness."
    },
    {
      "path": "spec/compiler_pipeline.md",
      "maps_to": ["UOS-BE-001", "UOS-BE-002", "UOS-BOOT-002"],
      "relationship": "Compiler pipeline documentation names the backend boundary, keeps hosted C++23 as the only production backend, and records the isolated experimental freestanding object bootstrap path."
    },
    {
      "path": "spec/abi_layout.md",
      "maps_to": ["UOS-ABI-001", "UOS-ABI-002", "UOS-BOOT-002"],
      "relationship": "ABI layout documentation records the current hosted C++23 representation, C ABI export restrictions, primitive freestanding object representation, golden test coverage, and future-only system ABI requirements."
    },
    {
      "path": "spec/library_layers.md",
      "maps_to": ["UOS-CORE-001", "UOS-CORE-002"],
      "relationship": "Library layering documentation separates future core, hosted std, and future system APIs while keeping current core/system import support explicitly unclaimed."
    },
    {
      "path": "docs/universeos/architecture.md",
      "maps_to": ["UOS-DOC-001", "UOS-LANG-001", "UOS-ABI-002", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "Hosted UniverseOS architecture maps current packages to hosted layers and blocks kernel/driver claims behind named gates."
    },
    {
      "path": "docs/universeos/roadmap.md",
      "maps_to": ["UOS-DOC-001", "UOS-LANG-001", "UOS-ABI-002", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "Hosted roadmap keeps product phases staged and separates hosted control-plane work from freestanding substrate work."
    },
    {
      "path": "rfcs/0002-freestanding-target.md",
      "maps_to": ["UOS-CLI-001", "UOS-LANG-001", "UOS-ABI-001", "UOS-ABI-002", "UOS-CORE-001", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "Freestanding RFC defines future compiler, runtime, codegen, and test requirements without claiming implementation."
    },
    {
      "path": "docs/universeos/qemu_boot_hello.md",
      "maps_to": ["UOS-ABI-002", "UOS-CORE-002", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "QEMU boot hello plan separates the pinned protocol/toolchain, linked ELF, boot-media assembly, and serial-execution contracts."
    },
    {
      "path": "docs/universeos/kernel_boundary.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-LANG-001", "UOS-ABI-001", "UOS-ABI-002", "UOS-CORE-001", "UOS-CORE-002", "UOS-BE-001", "UOS-BE-002", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004", "UOS-BOOT-005"],
      "relationship": "Kernel boundary documentation ties future kernel, syscall, driver, scheduler, memory-manager, and security claims to existing gates while making current non-support explicit."
    }
  ],
  "gates": [
    {
      "id": "UOS-DOC-001",
      "title": "Language and specification gate",
      "status": "experimental",
      "owner_area": "docs/spec",
      "depends_on": [],
      "required_evidence": [
        "Language/spec documentation for allocation, escape, unsafe, concurrency, panic, ABI, and runtime boundaries.",
        "Explicit non-goals for kernel, driver, interrupt, MMU, scheduler, syscall ABI, freestanding runtime, and backend independence.",
        "Support-matrix wording that keeps the system profile experimental until implementation gates pass.",
        "Contract documentation tests such as TST-280 and TST-282 remain green."
      ],
      "evidence_cases": [
        "TST-280-universeos-convergence-docs-contract",
        "TST-282-system-profile-docs-contract",
        "TST-329-universeos-gate-registry-docs-contract"
      ],
      "non_claim": "This gate does not prove a no-std build, freestanding runtime, kernel target, driver API, scheduler, interrupt model, MMU model, syscall ABI, or backend implementation."
    },
    {
      "id": "UOS-CLI-001",
      "title": "System-profile std import rejection",
      "status": "experimental",
      "owner_area": "cli/project-loader",
      "depends_on": ["UOS-DOC-001"],
      "required_evidence": [
        "Contract tests for system-profile targets that import bundled hosted std modules.",
        "Stable NBL-CLI-SYSTEM-STD diagnostic coverage.",
        "Coverage for --target system, --target freestanding, *-none target strings, --profile system, and --no-std.",
        "Strict build and full contract test run after CLI or project-loader changes."
      ],
      "evidence_cases": [
        "CHK-225-uos-cli-001-system-std-rejection-matrix",
        "CHK-226-hosted-allows-system-forbidden-std-import"
      ],
      "non_claim": "Rejecting hosted std imports does not prove a freestanding standard library, kernel runtime, driver framework, hardware API, or bootable artifact."
    },
    {
      "id": "UOS-CLI-002",
      "title": "Strict-region system-profile gate",
      "status": "experimental",
      "owner_area": "cli/region-analysis",
      "depends_on": ["UOS-DOC-001"],
      "required_evidence": [
        "Contract tests for region escapes under system-profile settings.",
        "Diagnostics proving strict-region behavior without requiring an explicit --strict-region flag, including --target system, --target freestanding, *-none target strings, --profile system, and --no-std.",
        "Negative coverage showing the system profile does not silently auto-promote escaping region values.",
        "Documentation separating hosted region convenience from system-profile fail-closed behavior."
      ],
      "evidence_cases": [
        "CHK-227-uos-cli-002-system-strict-region-matrix"
      ],
      "non_claim": "Strict region diagnostics do not prove a kernel memory model, allocator contract, MMU integration, interrupt-safety model, driver suitability, or scheduler support."
    },
    {
      "id": "UOS-LANG-001",
      "title": "Low-level language soundness gate",
      "status": "planned",
      "owner_area": "language/semantics",
      "depends_on": ["UOS-DOC-001", "UOS-CLI-002"],
      "required_evidence": [
        "Normative move, lifetime, aliasing, initialization, and destruction rules with no implicit hosted-runtime dependency.",
        "Typed raw-pointer, volatile, atomic, intrinsic, and unsafe-boundary contracts suitable for explicit low-level review.",
        "Concurrency ownership and data-race guarantees with adversarial soundness tests and stable diagnostics.",
        "Compatibility and migration policy for changes to the low-level language contract."
      ],
      "non_claim": "A specified low-level language contract does not prove implementation soundness, a system ABI, backend independence, a freestanding runtime, a kernel, or userspace support."
    },
    {
      "id": "UOS-ABI-001",
      "title": "Layout golden tests",
      "status": "experimental",
      "owner_area": "abi/codegen",
      "depends_on": ["UOS-DOC-001"],
      "required_evidence": [
        "Hosted C++23 golden tests and structured assertions for scalar C ABI exports, struct field order, enum payload lowering, duplicate symbol rejection, and no-export library rejection.",
        "Stable diagnostics rejecting public C ABI exports for unsupported hosted types such as String, Result, struct, enum, ref parameters, extern exports, and generic functions.",
        "Documentation distinguishing current hosted C ABI behavior from future system ABI, object backend, syscall ABI, and freestanding layout work.",
        "Strict build and contract-suite evidence before any release note promotes a stronger ABI or platform-specific layout claim."
      ],
      "evidence_cases": [
        "ABI-001-exported-scalar-function",
        "ABI-002-struct-field-order-codegen",
        "ABI-003-enum-payload-lowering-codegen",
        "ABI-004-duplicate-exported-symbol-diagnostic",
        "ABI-005-library-without-export-diagnostic"
      ],
      "non_claim": "Layout goldens do not prove a syscall ABI, calling-convention coverage, linker-script support, object backend, cross compilation, kernel target, or freestanding runtime."
    },
    {
      "id": "UOS-ABI-002",
      "title": "Freestanding system ABI gate",
      "status": "planned",
      "owner_area": "abi/freestanding",
      "depends_on": ["UOS-ABI-001", "UOS-LANG-001"],
      "required_evidence": [
        "Normative target data layout, scalar and aggregate representation, alignment, calling convention, stack, unwind, and symbol contracts.",
        "Cross-language ABI fixtures for supported argument, return, aggregate, pointer, enum, and error representations.",
        "Versioning, compatibility, and migration policy that distinguishes compiler ABI, runtime ABI, boot ABI, syscall ABI, and package ABI.",
        "Negative and cross-host tests that reject unsupported ABI surfaces rather than inheriting host C++ behavior."
      ],
      "non_claim": "A freestanding system ABI contract does not prove a direct backend, linker, runtime, bootable artifact, syscall implementation, kernel, or userspace support."
    },
    {
      "id": "UOS-CORE-001",
      "title": "No-std smoke",
      "status": "experimental",
      "owner_area": "core/std-profile",
      "depends_on": ["UOS-CLI-001", "UOS-CLI-002"],
      "required_evidence": [
        "A minimal system-profile smoke target that does not import bundled hosted std modules.",
        "Generated artifact inspection or assertions for runtime profile, target, and panic policy markers under --target system, --target freestanding, *-none target strings, --panic abort, and --panic trap.",
        "Explicit rejection tests for hosted APIs not allowed by the smoke.",
        "Library-layer documentation distinguishes future core, hosted std, and future system APIs without claiming core:: or system:: imports work today.",
        "Documentation stating that the smoke is a compiler/profile contract, not a runtime support claim."
      ],
      "evidence_cases": [
        "BLD-013-system-no-std-smoke-build-policy-markers",
        "CHK-228-uos-library-layer-imports-unavailable"
      ],
      "non_claim": "A no-std smoke does not prove a bootable binary, allocator, panic runtime, syscall layer, kernel mode, driver support, interrupt handling, MMU integration, scheduler, or freestanding standard library."
    },
    {
      "id": "UOS-CORE-002",
      "title": "Freestanding core and runtime gate",
      "status": "planned",
      "owner_area": "core/freestanding-runtime",
      "depends_on": ["UOS-BE-002", "UOS-CORE-001"],
      "required_evidence": [
        "Freestanding core types and operations with no hosted standard-library, C++ runtime, operating-system, or hidden allocation dependency.",
        "Startup, data initialization, panic, allocation-hook, termination, and target-runtime ABI implementations bound to UOS-ABI-002.",
        "Tests for initialization order, panic/abort paths, allocation failure, atomics/volatile/intrinsics, and forbidden hosted symbols.",
        "Resource, compatibility, rollback, and security documentation for every supported target/runtime configuration."
      ],
      "non_claim": "A freestanding core and runtime does not prove a linked or bootable kernel, interrupts, MMU, scheduler, syscalls, drivers, process isolation, or userspace support."
    },
    {
      "id": "UOS-BE-001",
      "title": "Backend interface boundary",
      "status": "experimental",
      "owner_area": "backend/codegen",
      "depends_on": ["UOS-DOC-001"],
      "required_evidence": [
        "Written interface contract between typed or NIR-level compiler state and backend-specific emission.",
        "Tests proving existing hosted C++23 codegen behavior remains unchanged.",
        "Documentation that no backend selector or fallback backend exists in the MVP.",
        "Review notes explaining interface boundaries, implementation details, and future-only surfaces."
      ],
      "evidence_cases": [
        "BLD-014-backend-default-cpp23-build-unchanged",
        "RUN-081-backend-default-cpp23-run-unchanged",
        "TST-331-backend-interface-docs-contract"
      ],
      "non_claim": "Defining a backend boundary does not prove LLVM support, Cranelift support, direct object output, backend independence, cross compilation, boot artifacts, kernel support, or freestanding runtime."
    },
    {
      "id": "UOS-BE-002",
      "title": "Independent backend and bootstrap gate",
      "status": "planned",
      "owner_area": "backend/toolchain",
      "depends_on": ["UOS-ABI-002", "UOS-BE-001"],
      "required_evidence": [
        "A supported direct object or reproducible stage-0/stage-1 compiler path that does not require generated C++ as the production system backend.",
        "Pinned assembler/linker/runtime inputs and a closed, content-bound toolchain provenance manifest.",
        "Bootstrap equivalence, deterministic rebuild, optimization, relocation, debug-information, and cross-host qualification evidence.",
        "Explicit backend selection and diagnostics with no silent fallback to hosted C++ or an unverified host toolchain."
      ],
      "non_claim": "An independent backend and bootstrap path does not prove a freestanding runtime, linked kernel, boot medium, kernel subsystems, or UniverseOS userspace."
    },
    {
      "id": "UOS-BOOT-001",
      "title": "Boot protocol, ABI, and toolchain contract",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "depends_on": ["UOS-ABI-002"],
      "required_evidence": [
        "Exact target triple, fixed high-half layout, entry symbol, panic policy, and executable format.",
        "Versioned Limine release and protocol header with content digests, selected base revision, request markers, and runtime support check.",
        "Restricted x86-64 System V entry ABI, floating-point/SIMD prohibition, stack, initialization, and non-return contracts.",
        "Pinned clang, ld.lld, bootloader, and image-tool provenance, checksums, licenses, compatibility evidence, deterministic manifests, and hosted rollback plan."
      ],
      "non_claim": "A boot protocol, ABI, and toolchain contract does not prove a linked kernel, boot medium, QEMU output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-002",
      "title": "Freestanding object emission gate",
      "status": "experimental",
      "owner_area": "boot/freestanding",
      "depends_on": ["UOS-BE-001", "UOS-CORE-001"],
      "required_evidence": [
        "Compiler support for producing a freestanding object for x86_64-unknown-none or a documented equivalent target.",
        "Artifact checks proving no hosted std, bundled hosted runtime, C++ standard library, exceptions, RTTI, threads, filesystem, process, networking, or time dependencies.",
        "Stable diagnostics for unsupported hosted APIs and unsupported backend/profile combinations.",
        "Rollback evidence proving hosted C++23 build/run behavior is unchanged."
      ],
      "evidence_cases": [
        "BLD-014-backend-default-cpp23-build-unchanged",
        "BLD-017-freestanding-object-elf-contract",
        "BLD-018-freestanding-request-state-machine",
        "BLD-019-freestanding-nir-allowlist",
        "BLD-020-freestanding-transaction-and-toolchain",
        "RUN-081-backend-default-cpp23-run-unchanged"
      ],
      "non_claim": "A freestanding object emission gate does not prove bootability, linker-script correctness, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-003",
      "title": "Deterministic linked kernel ELF gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "depends_on": ["UOS-BOOT-001", "UOS-BOOT-002", "UOS-CORE-002"],
      "required_evidence": [
        "Fixed linker script with explicit program headers, high-half placement, entry symbol, alignment, Limine request retention, and discard rules.",
        "Fixed no-shell ld.lld invocation and bounded ELF64 ET_EXEC audit for entry, segments, W^X, overlap, resource, relocation, dynamic, interpreter, TLS, and unwind policy.",
        "Digest metadata plus negative tests for malformed output, missing entry or protocol markers, dependency leakage, timeout, containment loss, path attacks, concurrency, and partial publication.",
        "Rollback evidence proving hosted C++23 build/run behavior is unchanged."
      ],
      "non_claim": "A linked kernel ELF is not a boot medium and does not prove bootloader integration, QEMU output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-004",
      "title": "Version-pinned boot media assembly gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "depends_on": ["UOS-BOOT-003"],
      "required_evidence": [
        "Repository-owned version-pinned Limine configuration with exact protocol and kernel paths.",
        "Bootloader and image-tool inputs verified against the UOS-BOOT-001 provenance manifest.",
        "Deterministic boot-media assembly with digest-bound kernel, configuration, bootloader, command, and output metadata.",
        "Bounded no-shell execution, explicit timeout and containment failures, transactional publication, negative input tests, and hosted rollback evidence."
      ],
      "non_claim": "An assembled boot medium does not prove that firmware or QEMU can execute it, serial output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-005",
      "title": "QEMU serial hello gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "depends_on": ["UOS-BOOT-004"],
      "required_evidence": [
        "QEMU command with bounded timeout and exact expected serial output.",
        "Contract test or documented smoke that fails closed on timeout, missing QEMU, missing output, or unexpected process status.",
        "Artifact checks showing the boot image was built from the freestanding object and linker script.",
        "Documentation stating that a serial hello remains experimental and does not imply kernel/driver support."
      ],
      "non_claim": "A QEMU serial hello gate does not prove drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    }
  ]
}
```

Human-readable gate details may live in `docs/universeos/gates.md`, but the JSON block above is the
registry that validation and Goal-mode tracking should consume.
