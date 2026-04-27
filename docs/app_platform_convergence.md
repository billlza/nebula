# App Platform Convergence

Nebula is not yet a general-purpose pure-Nebula app platform on the same footing as mature
Swift/Rust/C++ ecosystems.

The current realistic product lane is narrower and more explicit:

- CLI / system tools
- Linux backend services behind a reverse proxy
- backend-first internal control-plane products
- thin-host app cores behind an external UI shell

This document turns that positioning into an explicit convergence contract instead of leaving it as
an informal conclusion scattered across examples and roadmap notes.

## Comparison Model

APP-platform comparisons must be split into three ownership layers:

- `Nebula-owned`
  - language/runtime behavior
  - shared business logic
  - CLI + backend service code
  - embedded-data and transport contracts
  - host bridge contracts
- `Host-owned`
  - renderer, widgets, routing, accessibility, animations
  - browser/desktop/mobile runtime integration
  - platform-specific concurrency and device APIs
- `Ops-owned`
  - packaging, signing, install, upgrade, deploy, telemetry, backup, restore

This split avoids two common mistakes:

- treating host UI capability as if Nebula already owns it
- extrapolating narrow hot-path wins into a full-platform maturity claim

## Current Lane Matrix

### Close To Mature Today

- `CLI / system tools`
  - compiler, CLI, LSP, explain, workspace, registry, and install flows are already credible for
    small-to-medium tools
- `Linux backend service`
  - the installed backend SDK gives Nebula a narrow but real service platform on Linux x86_64
- `backend-first internal control plane`
  - `examples/release_control_plane_workspace` now acts as the forcing-app slice for shared core +
    service + CLI + embedded SQLite state

### Usable But Still Narrow

- `embedded data`
  - `official/nebula-db-sqlite` is the first reusable local-data contract, but it is still a
    preview embedded-data slice rather than a broad database platform
- `proxy-auth internal apps`
  - principal carriage exists for internal operator workflows, and the first RS256 JWT
    resource-server preview now exists; Nebula still lacks first-party accounts, browser login,
    sessions, OIDC client flows, and policy-platform closure
- `internal TLS / mTLS / HTTP2`
  - these transport slices are becoming real platform capabilities, but they are not yet a broad
    public-edge networking story

### Still Clearly Incomplete

- `GUI / native app-shell platform`
  - Nebula does not currently own a renderer, widget toolkit, accessibility stack, routing shell,
    or signing/distribution/update lifecycle
- `broad service ecosystem`
  - there is still no full middleware/database/queue/background-job platform on par with mature
    Rust/C++ ecosystems
- `full-platform performance story`
  - `benchmarks/backend_crypto` supports narrow competitive claims, but Nebula still lacks a broad
    app-platform benchmark story across CLI/service/data/bridge/ops layers

## Reference Artifacts In This Repo

Current convergence work should anchor to one real sample per important lane:

- `examples/cli_service_workspace`
  - backend-service + operator CLI + shared contract core + embedded config store
- `examples/release_control_plane_workspace`
  - current forcing-app slice for internal control-plane workflows, now including the internal-event
    release/apply workflow lane and pull-based worker leases
- `examples/thin_host_app_core`
  - current host/core split sample for future frontend/native directions
- `official/nebula-thin-host-bridge`
  - preview command/event/snapshot envelope contract for thin-host app cores; it standardizes
    correlation, state revision, deterministic replay, and explicit rejection semantics without
    claiming renderer or GUI ownership
- `examples/local_ops_console_ui`
  - first Nebula UI preview slice: `ui` / `view` syntax lowered to semantic JSON IR
- `examples/thin_host_gui_host_shell`
  - first thin-host GUI host-shell pilot: a deterministic C++ host decodes Nebula UI IR, dispatches
    stable action ids into `thin-host-bridge.command.v1`, and receives versioned events/snapshots
    with preview lifecycle/accessibility, app-bundle manifest staging, update-checksum, and
    crash/telemetry correlation coverage, without claiming native renderer ownership
- `official/nebula-db-sqlite`
  - the first official embedded-data package for backend-first internal apps
- `official/nebula-auth`
  - the first official resource-server auth package for backend-first internal apps; currently
    limited to RS256 JWT verification against caller-provided JWKS text and additionally shipped as
    an opt-in Linux backend SDK installed-preview package
- `official/nebula-config`
  - preview app-level env, mounted-secret file, and redacted startup preflight helpers; it is
    additionally shipped as an opt-in Linux backend SDK installed-preview package and keeps
    secrets/config lifecycle separate from `nebula-service` HTTP bind/timeout config
- `official/nebula-jobs`
  - the first official jobs/workflow kernel package; currently limited to DAG validation,
    SQLite-first runs, worker leases, idempotent receipts, and durable outbox preview helpers
- `official/nebula-ui`
  - preview semantic UI package; includes `nebula-ui.tree.v1` validation, a headless adapter,
    lookup-only action dispatch, lifecycle/accessibility preview summaries, and guarded AppKit/GTK
    minimal-window smoke sources. The GPU-renderer preview adds deterministic layout,
    `nebula-ui.render-list.v1` display commands, hit-test/action indexing, `ui.patch.v1` smoke
    diffs, and a guarded macOS Metal submit smoke, but not mature native adapter parity
- `benchmarks/backend_crypto`
  - the current narrow competitive hot-path matrix
- `docs/universeos_convergence.md`
  - the staged universeOS positioning gate; Nebula should target tools, services, control planes,
    and thin-host app cores before making kernel or driver-language claims

## Convergence Waves

### Wave 1: Publishable Internal App Standard

Before Nebula claims broader APP-platform maturity, it must first be able to ship one credible
Linux-first internal control plane that is:

- buildable from clean setup
- deployable behind a reverse proxy
- upgradeable and recoverable
- observable by operators
- explainable without repo-local heroics

This wave is anchored on:

- `official/nebula-db-sqlite`
- principal/auth carriage for internal apps
- reusable app-level config and mounted-secret preflight helpers
- deploy/config/secrets/backup/restore docs
- `examples/release_control_plane_workspace`

### Wave 2: Thin-Host Contract, Not Thin-Host Hype

The UI/native direction now has two explicitly separated lanes:

- thin-host remains the safest production-adjacent route for real host shells
- Nebula UI is a preview semantic-UI route inspired by the `Swift + SwiftUI` split

- Nebula owns app state, transitions, validation, and compact view-model snapshots
- the host owns rendering, navigation, accessibility, design system, and platform integration

Nebula UI V1 owns source syntax, semantic view-tree IR validation, a headless adapter, lookup-only
action dispatch, required `Input.accessibility_label`, preview lifecycle/accessibility summaries,
minimal guarded AppKit/GTK smoke sources, and thin-host packaging/update fixtures. It does not yet
own a mature renderer, animation system, native accessibility stack, AppKit/GTK adapter parity, or
distribution pipeline.

This wave should stabilize:

- versioned command/event/snapshot bridge contracts with replay parity
- lifecycle/auth/state-sync seams
- host-side packaging/update recipes
- shared crash/telemetry correlation rules

The release control-plane forcing app also exposes a platform capability matrix for the next large
server-side lanes. DAG workflow dependencies are now a preview kernel inside the existing workflow
definition path. Cron/schedule has advanced to an opt-in deterministic tick contract:
`ctl schedule tick --now-unix-ms N` uses an explicit clock value, stable schedule event ids, and the
existing workflow event receipt table for idempotency. It is still default-off, requires
`APP_SCHEDULES_PREVIEW=1` plus auth, and leaves clock ownership to an external cron/systemd timer.
External broker has advanced to a durable outbox seam: workflow run creation records
`workflow.run.created` messages in SQLite, and an operator-owned relay claims/completes them through
worker-gated HTTP/CLI calls. It is still default-off and does not publish to Kafka/NATS/RabbitMQ
inside the service. Public webhook integrations now have a first signed ingress seam: HMAC-SHA256
verified requests map into existing workflow event submission and inherit workflow receipt
idempotency. Free-form shell command tasks now have a first runnable sidecar preview: service-side
workflow stages only produce leases, while an operator-owned CLI worker validates an allowlist and
uses argv-based `std::process` with timeouts and capped output. Distributed deploy orchestration now
has a narrow sidecar-backed alias: `ctl deploy apply` validates an operator target file, submits a
deploy-marked workflow event, and still relies on the existing `shell_command` lease/result payload
contract. The reusable jobs/workflow kernel has moved into `official/nebula-jobs` and is now
available as an opt-in backend SDK installed-preview package; release-control-plane remains the
forcing app and this is not a broad external workflow/queue platform. Postgres has moved from diagnostics-only to a repo-local official preview package:
`official/nebula-db-postgres` dynamically probes `libpq`, exposes narrow execute/query/migration
helpers, is wired into the release-control-plane startup preflight when `APP_POSTGRES_PREVIEW=1`,
and can be selected as the active release-control-plane data plane with
`APP_DATA_BACKEND=postgres`. SQLite remains the default, and this preview does not imply automatic
SQLite migration, connection pooling, ORM, or a backend SDK installed package.
Dangerous execution paths must stay auth-gated and operator-configured before they become broader
product features.

It must not be presented as a pure-Nebula GUI platform.

### Wave 3: Broader Platform Closure

Only after Waves 1-2 are solid should Nebula widen toward:

- Postgres / network DB stories
- jobs / queues / background task platform
- richer config/secrets lifecycle
- more operator-credible TLS/mTLS/HTTP2 contracts
- stronger packaging/install/upgrade system stories

### Wave 4: Re-run Broader Parity Claims

Only after the earlier waves are closed should Nebula revisit comparisons against mature
Swift/Rust/C++ app platforms in the broader sense.

Until then, the truthful claim is:

- Nebula is approaching a backend-first internal app platform
- Nebula is not yet a full pure-Nebula desktop/mobile/web app platform

## Measurement Gates

`benchmarks/backend_crypto` remains the current narrow hard-win program for backend/crypto hot
paths.

Broader APP-platform convergence now has its own fixed planning matrix under
`benchmarks/app_platform`, with these representative workloads:

- `cli_cold_start`
- `service_json_db_crud`
- `thin_host_bridge_roundtrip`
- `state_sync_latency`
- `resident_memory`

The measurement contract for this lane is:

- comparisons stay split into `Nebula-owned`, `Host-owned`, and `Ops-owned` responsibility
- host-renderer speed is not treated as a Nebula runtime win/loss
- C++ reference results are measurement-only baselines until each workload has a matching runnable
  reference implementation and explicit comparison output
- public app-platform maturity claims stay blocked until the internal-app standard and thin-host
  contract are both real and repeatable

Use:

```bash
python3 scripts/app_platform_bench.py verify
python3 scripts/app_platform_bench.py plan --format json
python3 scripts/app_platform_bench.py run-nebula --binary ./build/nebula --workload cli_cold_start --workload service_json_db_crud --workload thin_host_bridge_roundtrip --workload state_sync_latency --workload resident_memory
python3 scripts/app_platform_bench.py run-reference --stack cpp --workload service_json_db_crud --workload thin_host_bridge_roundtrip --workload state_sync_latency --json-out artifacts/app-platform-cpp.json
python3 scripts/app_platform_bench.py compare --stack cpp --nebula-json artifacts/app-platform-nebula.json --reference-json artifacts/app-platform-cpp.json
```

This wave now includes real Nebula workload execution for:

- the release-control-plane CLI cold-start slice
- the service + JSON + SQLite CRUD lane
- the thin-host bridge/state lane
- the representative long-lived service resident-memory lane

It still does not pretend that Nebula already has a complete cross-language end-to-end app
benchmark story.

The C++ reference lane now covers the backend `service_json_db_crud` hot path and the first
thin-host bridge/state hot paths. It remains a measurement baseline, not a claim that Nebula has a
complete C++/Qt/asio/beast parity stack.
