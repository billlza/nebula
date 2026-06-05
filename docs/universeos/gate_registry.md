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

```json
{
  "gate_registry_version": 1,
  "gate_naming": {
    "UOS-DOC": "documentation and language/specification gates",
    "UOS-CLI": "CLI and system-profile gates",
    "UOS-ABI": "ABI and layout gates",
    "UOS-CORE": "no-std and core-library gates",
    "UOS-BE": "backend and object-code gates",
    "UOS-BOOT": "boot and freestanding gates"
  },
  "source_doc_mapping": [
    {
      "path": "docs/system_profile.md",
      "maps_to": ["UOS-CLI-001", "UOS-CLI-002", "UOS-CORE-001"],
      "relationship": "Current system-profile documentation defines the experimental std-import rejection, strict-region behavior, no-std smoke boundary, and explicit non-goals."
    },
    {
      "path": "docs/universeos_convergence.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-ABI-001", "UOS-CORE-001", "UOS-BE-001", "UOS-BOOT-001"],
      "relationship": "UniverseOS convergence documentation keeps the staged direction behind evidence gates and prevents broad OS-substrate positioning without named proof."
    },
    {
      "path": "spec/compiler_pipeline.md",
      "maps_to": ["UOS-BE-001"],
      "relationship": "Compiler pipeline documentation names the backend boundary while keeping C++23 as the only production backend."
    },
    {
      "path": "docs/universeos/architecture.md",
      "maps_to": ["UOS-DOC-001", "UOS-BE-001", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004"],
      "relationship": "Hosted UniverseOS architecture maps current packages to hosted layers and blocks kernel/driver claims behind named gates."
    },
    {
      "path": "docs/universeos/roadmap.md",
      "maps_to": ["UOS-DOC-001", "UOS-BE-001", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004"],
      "relationship": "Hosted roadmap keeps product phases staged and separates hosted control-plane work from freestanding substrate work."
    },
    {
      "path": "rfcs/0002-freestanding-target.md",
      "maps_to": ["UOS-CLI-001", "UOS-ABI-001", "UOS-CORE-001", "UOS-BE-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004"],
      "relationship": "Freestanding RFC defines future compiler, runtime, codegen, and test requirements without claiming implementation."
    },
    {
      "path": "docs/universeos/qemu_boot_hello.md",
      "maps_to": ["UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004"],
      "relationship": "QEMU boot hello plan defines future boot artifacts, command shape, serial output, and explicit non-claims."
    },
    {
      "path": "docs/universeos/kernel_boundary.md",
      "maps_to": ["UOS-DOC-001", "UOS-CLI-001", "UOS-CLI-002", "UOS-ABI-001", "UOS-CORE-001", "UOS-BE-001", "UOS-BOOT-001", "UOS-BOOT-002", "UOS-BOOT-003", "UOS-BOOT-004"],
      "relationship": "Kernel boundary documentation ties future kernel, syscall, driver, scheduler, memory-manager, and security claims to existing gates while making current non-support explicit."
    }
  ],
  "gates": [
    {
      "id": "UOS-DOC-001",
      "title": "Language and specification gate",
      "status": "experimental",
      "owner_area": "docs/spec",
      "required_evidence": [
        "Language/spec documentation for allocation, escape, unsafe, concurrency, panic, ABI, and runtime boundaries.",
        "Explicit non-goals for kernel, driver, interrupt, MMU, scheduler, syscall ABI, freestanding runtime, and backend independence.",
        "Support-matrix wording that keeps the system profile experimental until implementation gates pass.",
        "Contract documentation tests such as TST-280 and TST-282 remain green."
      ],
      "non_claim": "This gate does not prove a no-std build, freestanding runtime, kernel target, driver API, scheduler, interrupt model, MMU model, syscall ABI, or backend implementation."
    },
    {
      "id": "UOS-CLI-001",
      "title": "System-profile std import rejection",
      "status": "experimental",
      "owner_area": "cli/project-loader",
      "required_evidence": [
        "Contract tests for system-profile targets that import bundled hosted std modules.",
        "Stable NBL-CLI-SYSTEM-STD diagnostic coverage.",
        "Coverage for --target system, --target freestanding, *-none target strings, --profile system, and --no-std.",
        "Strict build and full contract test run after CLI or project-loader changes."
      ],
      "non_claim": "Rejecting hosted std imports does not prove a freestanding standard library, kernel runtime, driver framework, hardware API, or bootable artifact."
    },
    {
      "id": "UOS-CLI-002",
      "title": "Strict-region system-profile gate",
      "status": "experimental",
      "owner_area": "cli/region-analysis",
      "required_evidence": [
        "Contract tests for region escapes under system-profile settings.",
        "Diagnostics proving strict-region behavior without requiring an explicit --strict-region flag, including --target system, --target freestanding, *-none target strings, --profile system, and --no-std.",
        "Negative coverage showing the system profile does not silently auto-promote escaping region values.",
        "Documentation separating hosted region convenience from system-profile fail-closed behavior."
      ],
      "non_claim": "Strict region diagnostics do not prove a kernel memory model, allocator contract, MMU integration, interrupt-safety model, driver suitability, or scheduler support."
    },
    {
      "id": "UOS-ABI-001",
      "title": "Layout golden tests",
      "status": "planned",
      "owner_area": "abi/codegen",
      "required_evidence": [
        "Golden tests or structured assertions for scalar, struct, enum, alignment, and exported C ABI layout where applicable.",
        "Checked-in expected artifacts that fail on accidental layout drift.",
        "Documentation distinguishing stable narrow C ABI behavior from future system ABI work.",
        "Strict build and platform-specific evidence for every platform that receives a layout claim."
      ],
      "non_claim": "Layout goldens do not prove a syscall ABI, calling-convention coverage, linker-script support, object backend, cross compilation, kernel target, or freestanding runtime."
    },
    {
      "id": "UOS-CORE-001",
      "title": "No-std smoke",
      "status": "experimental",
      "owner_area": "core/std-profile",
      "required_evidence": [
        "A minimal system-profile smoke target that does not import bundled hosted std modules.",
        "Generated artifact inspection or assertions for runtime profile, target, and panic policy markers under --target system, --target freestanding, *-none target strings, --panic abort, and --panic trap.",
        "Explicit rejection tests for hosted APIs not allowed by the smoke.",
        "Documentation stating that the smoke is a compiler/profile contract, not a runtime support claim."
      ],
      "non_claim": "A no-std smoke does not prove a bootable binary, allocator, panic runtime, syscall layer, kernel mode, driver support, interrupt handling, MMU integration, scheduler, or freestanding standard library."
    },
    {
      "id": "UOS-BE-001",
      "title": "Backend interface boundary",
      "status": "experimental",
      "owner_area": "backend/codegen",
      "required_evidence": [
        "Written interface contract between typed or NIR-level compiler state and backend-specific emission.",
        "Tests proving existing hosted C++23 codegen behavior remains unchanged.",
        "Documentation that no backend selector or fallback backend exists in the MVP.",
        "Review notes explaining interface boundaries, implementation details, and future-only surfaces."
      ],
      "non_claim": "Defining a backend boundary does not prove LLVM support, Cranelift support, direct object output, backend independence, cross compilation, boot artifacts, kernel support, or freestanding runtime."
    },
    {
      "id": "UOS-BOOT-001",
      "title": "QEMU hello plan",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "required_evidence": [
        "Target triple, linker script, entry symbol, panic policy, and artifact format plan.",
        "Minimal runtime responsibility plan for startup, stack assumptions, zeroing/copying rules, and unavailable hosted APIs.",
        "QEMU command line, expected output, timeout, and failure diagnostics.",
        "Rollback plan keeping hosted CLI and service behavior unaffected."
      ],
      "non_claim": "A QEMU hello plan does not prove bootability. Even a future QEMU hello would not prove drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-002",
      "title": "Freestanding object emission gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "required_evidence": [
        "Compiler support for producing a freestanding object for x86_64-unknown-none or a documented equivalent target.",
        "Artifact checks proving no hosted std, bundled hosted runtime, C++ standard library, exceptions, RTTI, threads, filesystem, process, networking, or time dependencies.",
        "Stable diagnostics for unsupported hosted APIs and unsupported backend/profile combinations.",
        "Rollback evidence proving hosted C++23 build/run behavior is unchanged."
      ],
      "non_claim": "A freestanding object emission gate does not prove bootability, linker-script correctness, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-003",
      "title": "Linker script and boot artifact gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
      "required_evidence": [
        "Checked linker script with explicit entry symbol, section placement, alignment, and discard rules.",
        "Bootloader configuration and artifact assembly steps that are deterministic and repo-documented.",
        "Negative tests for missing entry symbol, missing linker script, or hosted dependency leakage.",
        "Rollback evidence proving hosted C++23 build/run behavior is unchanged."
      ],
      "non_claim": "A linker script and boot artifact gate does not prove QEMU output, drivers, interrupts, MMU, scheduler, syscall ABI, process isolation, production kernel support, or complete freestanding runtime support."
    },
    {
      "id": "UOS-BOOT-004",
      "title": "QEMU serial hello gate",
      "status": "planned",
      "owner_area": "boot/freestanding",
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
