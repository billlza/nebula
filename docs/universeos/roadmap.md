# Hosted UniverseOS Roadmap

UniverseOS work should grow from hosted evidence, not from kernel claims. The current goal is to
make Nebula useful for tools, services, control planes, and thin-host app cores while future
freestanding work remains blocked by explicit gates.

## Phase 0: Current Baseline

- Keep C++23 as the default and only production backend.
- Keep system/no-std behavior documented as an experimental hosted codegen gate.
- Keep hosted service/profile tests and full contract tests green.
- Keep GA, installed-preview, repo-local preview, experimental, and future surfaces distinct.

## Phase 1: Hosted Control-Plane Forcing App

- Maintain `examples/universe_control_plane` as the smallest hosted control-plane slice.
- Demonstrate service registry state and desired-state transitions.
- Keep the example file-backed until the preview embedded data package is intentionally adopted.
- Avoid real process supervision, privilege, boot, kernel, or driver claims.

## Phase 2: Package, Build, Observe, Config, State

- Reuse existing workspace, lockfile, build, run, test, and bench flows.
- Promote only the hosted pieces that have contract tests and operator documentation.
- Keep `official/nebula-db-sqlite`, `official/nebula-config`, `official/nebula-auth`, and
  `official/nebula-jobs` as explicit preview dependencies until release review promotes them.

## Phase 3: Thin-Host App Shell

- Continue the host/core split: Nebula owns state, validation, transitions, events, and snapshots.
- Keep rendering, platform I/O, signing, accessibility, and distribution host-owned.
- Promote thin-host APIs only through bridge contract tests and compatibility docs.

## Phase 4: Future Freestanding Substrate

- Finish the freestanding target RFC before implementing runtime or backend behavior.
- Add object-file generation, linker-script, and QEMU serial hello gates.
- Keep hosted CLI/service behavior unaffected by freestanding experiments.
- Do not promote kernel/driver support until boot/runtime/ABI/driver gates exist and pass.

## Roadmap Gate Summary

| Phase | Current gate | Promotion blocker |
| --- | --- | --- |
| Hosted CLI/service baseline | full contract suite | warnings, flaky contracts, or undocumented support drift |
| Hosted control plane | example smoke and docs | no tested state transition path |
| Observe/config/state | package-specific contract tests | preview packages without release posture |
| Thin-host shell | bridge contract tests | host/core ownership ambiguity |
| Freestanding substrate | `UOS-BOOT-*`, ABI/backend/core gates | no object, linker, runtime, or QEMU evidence |

The roadmap intentionally allows hosted UniverseOS work to ship before freestanding work exists.
