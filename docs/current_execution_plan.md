# Current Execution Plan

This document is the live execution index for near-term Nebula development. `ROADMAP.md` describes
the public wave structure; `docs/post_1_0_workstreams.md` contains the broader backlog. This file is
the shorter operational plan that keeps day-to-day work aligned with the original development bias.

## Non-Negotiable Direction

Nebula should continue moving in this order:

1. Reliable compiler/tooling and contract-test signal.
2. Backend-first internal platform proof.
3. Thin-host app-core and real application validation.
4. App-local runtime, package/update/recovery, and host-adapter closure.
5. GUI renderer work behind preview semantic IR and thin-host boundaries.
6. Crypto/PQC/QKD integration as real preview packages, not unsupported security claims.
7. Freestanding/runtime/kernel work only after the application and system-profile substrate is
   credible.

The default should not drift toward a broad GUI framework, kernel target, or marketing claim before
the corresponding gates are satisfied.

## Current Priority Stack

### P0: Keep The Signal Trustworthy

Goal:

- Preserve green, meaningful focused tests and full-suite credibility.

Required behavior:

- no warning-suppression fixes
- no timeout-padding as a bug mask
- no benchmark threshold loosening to manufacture a win
- contract drift must be documented and tested

Primary gates:

- strict build
- focused contract tests for touched modules
- keep hosted `build`/build-enabled `run` publication, dependency identity, signal cancellation,
  and private execution leases fail-closed; next converge `test`/`bench`, linker-selected inputs,
  crash recovery, and cross-platform namespace/ACL behavior onto the same lifecycle
- `python3 scripts/app_platform_bench.py verify`
- full contract suite before release-facing claims

### P1: Thin-Host Media Player Real App

Goal:

- Turn `examples/thin_host_media_player` into a real validation app without moving business logic
  into native adapters.

Nebula owns:

- media library state
- playback settings
- import/download commands
- command validation
- events and snapshots
- SQLite receipts
- startup recovery diagnostics
- jobs/outbox markers
- bundle/update manifest validation
- observe markers

Host/native sidecars own only:

- file picker and platform I/O
- codec/player engine integration
- torrent engine integration
- native dependency probes
- host rendering shell

Exit criteria:

- the app can launch on macOS and Linux with clear native dependency diagnostics
- local media import/open/play/pause/seek works through Nebula commands/events/snapshots
- torrent flow is loopback/local-testable or explicitly gated with diagnostics
- crash/recovery facts are persisted and explainable from receipts
- Nebula-first boundary tests prove native code does not own app business state

### P2: App-Local Runtime Substrate

Goal:

- Make standalone/thin-host apps get a reusable local runtime baseline without making the substrate
  media-player-specific or game-specific.

Scope:

- SQLite default local receipts
- recovery replay trace API
- startup recovery policy summaries
- lifecycle markers such as `startup_started`, `app_ready`, `app_degraded`, `shutdown_clean`
- first usable snapshot readiness evidence
- config/auth/jobs/observe composition
- PostgreSQL remains preview opt-in, not default app-local state

Exit criteria:

- app runtime can explain the previous session state after startup
- receipt schema is stable enough for replay/recovery smokes
- update/recovery markers are tied to bundle/update manifests
- no app-domain assumptions leak into generic substrate APIs

### P3: Bridge, State Sync, And JSON Wire Performance

Goal:

- Keep pushing the hot paths closest to C++ parity or local wins without compromising semantics.

Priority hot paths:

- `thin_host_payload_command_roundtrip`
- `ui_action_roundtrip`
- `ui_snapshot_render`
- `state_sync_latency`
- JSON wire indexed traversal
- Result access helpers and direct-state preview lanes
- struct-copy/codegen constant-folding costs
- HTTP route/json/bytes/SQLite backend workloads

Rules:

- every parity claim needs same-machine C++ reference JSON
- preserve external JSON wire contracts even when internal paths use typed/indexed models
- direct-state lanes must be explicitly marked preview and only accept already-validated inputs

### P4: Backend Platform Closure

Goal:

- Continue turning backend-first app pieces into repeatable platform assets.

Near-term modules:

- `official/nebula-config` hardening
- `official/nebula-auth` resource-server boundaries
- `official/nebula-db-sqlite` performance and recovery semantics
- `official/nebula-db-postgres` preview contract hardening
- `official/nebula-jobs` installed-preview lifecycle
- `official/nebula-observe` app/runtime evidence integration

Exit criteria:

- release-control-plane and at least one real app consume the reusable packages
- SQLite remains the default durable local data plane
- Postgres is opt-in preview with explicit diagnostics and no hidden fallback
- deploy, upgrade, backup, restore, recovery, logs, and metrics are documented for production-like
  internal apps

### P5: GUI Preview Without Overclaiming

Goal:

- Build Nebula-owned UI semantics while keeping mature rendering/windowing claims behind evidence.

Allowed now:

- typed UI model
- `nebula-ui.tree.v1` validation
- non-JSON internal layout model
- render-list/display-list preview
- action/hit-test index caches
- patch/diff previews
- guarded native adapter smoke
- thin-host host shell and bundle/update previews

Not claimed yet:

- complete native renderer
- complete accessibility stack
- App Store/notarization/update GA
- SwiftUI/Qt/Flutter-class widget toolkit
- full pure-Nebula GUI platform

### P6: Crypto, PQC, And QKD Differentiation

Goal:

- Keep quantum-communication differentiation real and bounded.

Allowed now:

- PQC application-layer protocol helpers
- QKD KME/KMS key-delivery provider contract
- deterministic mock providers
- mTLS adapter preview for production-shaped QKD access
- negative-path tests and explicit error semantics

Not claimed yet:

- security certification
- physical QKD hardware implementation
- trusted-node network ownership
- QKD-TLS GA
- broad PKI lifecycle platform

### P7: UniverseOS / Kernel Work

Goal:

- Keep universeOS as a staged direction, not a current implementation claim.

Current evidence:

- `UOS-BOOT-002` has an experimental primitive-only ELF object slice for the exact
  `x86_64-unknown-none`/trap request.
- `UOS-BOOT-001` now has a strict Limine v12.3.2 protocol/ABI candidate, but remains planned because
  compiler/linker/boot/image-tool provenance and compatibility are incomplete.
- These remove only narrow object and protocol/ABI prerequisites; they do not satisfy the runtime,
  complete toolchain closure (`UOS-BOOT-001`), linked-kernel ELF (`UOS-BOOT-003`), boot-media
  (`UOS-BOOT-004`), QEMU (`UOS-BOOT-005`), direct-backend, or kernel prerequisites below.
- Readiness is tracked by the non-additive evidence ledger in
  `docs/universeos/readiness_assessment.md`; product-completion percentages are intentionally not
  used.

Blocked until:

- system profile has explicit allocation, panic, ABI, unsafe, and hosted-std rejection answers
- no-std/freestanding runtime prototype exists
- app/runtime/platform forcing apps have exposed the necessary abstractions
- direct object backend or LLVM/Cranelift direction has been evaluated
- a reproducible compiler bootstrap plan has been selected; hardening the hosted C++ boundary does
  not by itself make Nebula self-hosting or toolchain-independent

## Module Entry Template

Every new module should state:

- objective
- source surface: GA, installed-preview, repo-preview, or experimental
- non-goals
- user-visible behavior
- failure semantics
- focused tests
- performance gates if relevant
- docs to update
- release impact

## Module Exit Template

A module is not done until:

- code compiles warning-free in the relevant gate
- focused tests pass
- docs match the new boundary
- benchmarks are updated when performance was part of the claim
- full suite is either green or explicitly deferred with a concrete reason
- unrelated worktree drift is excluded from commit

## Current Execution Preference

When there is a choice between adding broad new surface and validating a real forcing app, choose
the forcing app. When there is a choice between polishing a demo and fixing a contract failure,
fix the contract failure. When there is a choice between C/C++ convenience and Nebula-owned app
logic, keep the app logic in Nebula and put native code at the adapter boundary.
