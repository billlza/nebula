# Hosted UniverseOS Architecture

UniverseOS is a staged product direction for Nebula. The current credible lane is hosted tools,
services, control planes, and thin-host app cores. Kernel and driver support is blocked by
system-profile, ABI, backend, runtime, and boot gates.

This document maps the hosted architecture without claiming that Nebula is a kernel, driver
framework, bootloader, interrupt model, MMU model, scheduler, syscall ABI, freestanding runtime, or
backend-independent object backend.

## Current Anchor Documents

- `docs/app_platform_convergence.md`: hosted app/platform ownership split
- `docs/service_profile.md`: current backend service profile
- `docs/system_profile.md`: experimental system/no-std CLI gate and explicit non-goals
- `docs/universeos_convergence.md`: staged UniverseOS positioning and parity gaps
- `docs/universeos/gate_registry.md`: machine-checkable claim gates

## Product Layers

| Layer | Current hosted shape | Current Nebula anchors | Current status |
| --- | --- | --- | --- |
| Universe CLI tools | Operator and developer CLIs built with hosted Nebula packages | `nebula` CLI, `std::env`, `std::fs`, `std::json`, `std::process`, `examples/cli_service_workspace/apps/ctl`, `examples/release_control_plane_workspace/apps/ctl` | usable hosted lane |
| Universe service manager | Hosted service-registry/control-plane logic, not a process supervisor | `examples/universe_control_plane`, `official/nebula-service`, `official/nebula-observe`, `examples/release_control_plane_workspace` | repo-local MVP example |
| Universe package/build manager | Workspace/package/build/update flows for hosted projects | `nebula fetch`, `nebula build`, `nebula run`, `nebula test`, `nebula bench`, `nebula.lock`, `spec/tooling_cli.md` | existing compiler/tooling lane |
| Universe observe/config/state plane | Hosted operator state, config, health, logs, and preview embedded state | `official/nebula-observe`, `official/nebula-config`, `official/nebula-db-sqlite`, `official/nebula-auth`, `official/nebula-jobs`, release-control-plane example | installed-preview/backend-first lane |
| Thin-host app shell | Nebula owns state transitions and validated snapshots; host owns rendering and platform I/O | `official/nebula-thin-host-bridge`, `official/nebula-ui`, `examples/thin_host_app_core`, `examples/thin_host_gui_host_shell` | preview only |
| Future no-std/freestanding substrate | Future runtime, ABI, object, linker, boot, and hardware-facing APIs | `docs/system_profile.md`, `docs/universeos/no_std_runtime.md`, `rfcs/0002-freestanding-target.md`, `docs/universeos/qemu_boot_hello.md` | blocked/future |

## Hosted MVP Shape

The hosted MVP should look like a small internal platform:

1. CLI tools manage local and remote hosted state.
2. Services expose health/status/config/state endpoints through the service profile.
3. Package/build flows use existing workspace manifests and lockfiles.
4. Observe/config/state packages remain explicit dependencies, with preview packages documented as
   preview.
5. Thin-host app work stays behind the host/core split.
6. Future no-std/freestanding work stays behind gates and does not affect hosted behavior.

The current `examples/universe_control_plane` skeleton is intentionally small: it demonstrates a
hosted service registry and desired-state transitions. It does not supervise real processes.

## Current Package Mapping

| Existing package/module | Hosted UniverseOS destination | Notes |
| --- | --- | --- |
| `std::env` | Universe CLI tools | Hosted process environment and argv support only |
| `std::fs` | CLI tools and state plane | Hosted filesystem access, not freestanding storage |
| `std::json` / `std::http_json` | CLI, service, observe/config/state plane | JSON contracts for hosted control-plane payloads |
| `std::http` / `std::net` | Service manager and service plane | Hosted TCP/HTTP only |
| `std::process` | Package/build manager and operator sidecars | Hosted subprocess execution, not process supervision in the OS sense |
| `std::time` | Observe/config/state plane | Hosted clocks only |
| `official/nebula-service` | Universe service manager | GA backend service layer under `docs/service_profile.md` |
| `official/nebula-observe` | Observe plane | Event/metric/log shape for hosted services |
| `official/nebula-config` | Config plane | Installed-preview app config helpers |
| `official/nebula-db-sqlite` | State plane | Preview embedded state, not a system storage subsystem |
| `official/nebula-auth` | Observe/config/state control plane | Preview resource-server auth helpers |
| `official/nebula-jobs` | Package/build manager and control-plane workflows | Preview DAG/jobs kernel |
| `official/nebula-thin-host-bridge` | Thin-host app shell | Preview command/event/snapshot bridge |
| `official/nebula-ui` | Thin-host app shell | Preview semantic UI IR, not a mature renderer |

## Claim Gate Table

| Claim | Required gate before claim | Current posture |
| --- | --- | --- |
| Hosted CLI/service control plane | `UOS-DOC-001`, service profile tests, example smoke | viable hosted lane |
| System/no-std import rejection | `UOS-CLI-001` | experimental CLI gate |
| Strict-region system behavior | `UOS-CLI-002` | experimental CLI gate |
| Stable hosted ABI/layout statements | `UOS-ABI-001` | experimental hosted golden tests |
| No-std smoke boundary | `UOS-CORE-001` | experimental smoke only |
| Backend boundary | `UOS-BE-001` | experimental backend interface boundary |
| Object backend or LLVM/Cranelift support | future backend gate beyond `UOS-BE-001` | unsupported |
| Bootable QEMU hello | `UOS-BOOT-002`, `UOS-BOOT-003`, `UOS-BOOT-004` | future only |
| Kernel or driver support | future boot/runtime/driver gates after QEMU hello | blocked |

## Non-Goals For The Current Release

- no kernel or driver framework
- no process supervisor
- no syscall ABI
- no interrupt, MMU, scheduler, or capability model
- no freestanding standard library
- no direct object backend
- no LLVM or Cranelift dependency
- no public claim that hosted system-profile checks are bootable artifacts

The hosted architecture should remain useful even if future freestanding work is delayed or
rolled back.
