# Nebula Spec Index

This index lists language, compiler, tooling, and profile specifications. `spec/SPEC.md` remains the
read-order summary inside the spec tree; this document is the docs-facing index for contributors and
Goal-mode work.

## Language

- `spec/language_reference.md`: implementation-anchored language overview
- `spec/language_core.md`: current core language identity
- `spec/grammar.ebnf`: grammar surface
- `spec/type_system.md`: current type-system behavior
- `spec/generics_policy.md`: current generics policy
- `spec/region_model.md`: current region model
- `spec/region_semantics.md`: existing region semantics contract
- `spec/ownership_model.md`: current ownership and promotion model
- `spec/rep_owner_model.md`: existing representation x ownership inference contract
- `spec/interop_c_abi.md`: current C ABI import/export contract
- `spec/library_layers.md`: future core/std/system library split
- `spec/abi_layout.md`: current hosted C++23 ABI/layout behavior and future ABI requirements

## Analysis And Safety

- `spec/static_analysis.md`: escape, borrow/exclusivity assist, and epistemic linting
- `spec/escape_analysis.md`: escape-analysis precision model
- `spec/safety_contract.md`: unsafe boundary and `ref` exclusivity contract
- `spec/diagnostics.md`: diagnostic schema and stable code families

## Tooling And Infrastructure

- `spec/tooling_cli.md`: CLI and package/tooling behavior
- `spec/cli_contract.md`: compatibility CLI contract
- `spec/experimental_infra.md`: cache, reuse, grouping, and baseline internals
- `spec/compiler_pipeline.md`: current pipeline and the small C++23 backend interface boundary

## Product And System Profiles

- `docs/service_profile.md`: backend service profile
- `docs/support_matrix.md`: support posture
- `docs/toolchain_profile.md`: hosted toolchain profile
- `docs/app_platform_convergence.md`: app-platform direction
- `docs/universeos_convergence.md`: staged UniverseOS direction
- `docs/system_profile.md`: experimental system-profile boundary
- `docs/universeos/gates.md`: human-readable staged UniverseOS gates
- `docs/universeos/gate_registry.md`: machine-checkable UniverseOS gate registry
- `docs/universeos/no_std_runtime.md`: no-std runtime entry criteria, not implementation claims
- `docs/universeos/architecture.md`: hosted UniverseOS architecture, not kernel support
- `docs/universeos/roadmap.md`: one-page staged hosted UniverseOS roadmap
- `docs/universeos/qemu_boot_hello.md`: future QEMU boot hello plan, not implemented support
- `docs/universeos/kernel_boundary.md`: future kernel/userspace boundary, not implementation claims

## Current Non-Claims

The current specs do not claim kernel, driver, interrupt, MMU, scheduler, syscall ABI,
freestanding runtime, bootloader, or backend-independent object-code support. UniverseOS work stays
staged and evidence-gated through `docs/universeos/gate_registry.md`.
