# Thin-Host App Shell Guide

Nebula's production-adjacent frontend/native-app direction is still the thin-host split:

- the host owns the shell, rendering stack, navigation, accessibility, platform I/O, and event loop
- Nebula owns application state, domain logic, validation, and event-to-state transitions

Current sequencing constraint for the backend-first internal-app wave:

- Nebula should not claim a mature GUI/app-shell platform before the backend/internal-app
  substrate is stronger
- embedded data, service lifecycle, auth/principal carriage, deploy/upgrade/observe flows, and
  internal control-plane examples should land before thin-host is treated as a platform milestone

Nebula UI now exists as a separate preview lane for semantic UI trees. That lane is inspired by the
`Swift + SwiftUI` split, but V1 only owns source syntax, stable view-tree IR, a headless adapter,
guarded AppKit/GTK minimal-window smoke assets, lifecycle/accessibility summaries, and packaging/
update fixture contracts; native renderer parity remains future work.

Nebula is not trying to become a replacement for:

- React / SwiftUI / WinUI / GTK / AppKit
- CSS/layout engines
- browser runtimes
- large client-side component frameworks

That split is the key to staying performant without drifting away from the project's original
intent.

See:

- `examples/thin_host_app_core/README.md`
- `examples/thin_host_gui_host_shell/README.md`
- `docs/support_matrix.md`
- `docs/app_platform_convergence.md`

## Responsibilities

### Host Owns

- windowing and app shell
- navigation and view composition
- accessibility semantics
- animation and frame timing
- network/file/device/platform integration
- rendering technology choices
- design tokens, theming, and component-system implementation

### Nebula Core Owns

- domain state
- reducers / transitions / business workflows
- validation and policy
- deterministic formatting / serialization logic
- compact view-model snapshots passed to the host

### Ops Owns

- packaging and signing
- install/update lifecycle
- crash collection and telemetry plumbing
- deployment/runtime policy outside the host shell

The `examples/thin_host_gui_host_shell/deploy` files are preview fixtures for the shape of those
ops-owned contracts. They are intentionally not signing, notarization, store distribution, or a
real auto-updater.

`examples/thin_host_gui_host_shell/deploy/bundle/manifest.preview.json` is the first app-bundle
preview contract. It fixes the repo-local shape for app id, version, host API version, entry binary,
staged asset list, update-manifest checksum, telemetry correlation source, and crash marker schema.
It is still only a staging contract: the host shell can be built and launched by tests, but Nebula is
not yet providing store packaging, notarization, auto-update, or crash-upload infrastructure.

The next validation probe is a thin-host media player, tracked in
`docs/media_player_validation_app.md`. That app should prove app-core state, app-local storage,
optional PostgreSQL metadata, background jobs, telemetry, and preview bundle/update/recovery
contracts before Nebula claims a complete APP platform. It is not a reusable app template: generic
substrate belongs in Nebula, while media-specific behavior should be discovered while building the
app. Torrent import for that app is limited to public-domain, open-licensed, or operator-owned media;
Nebula should validate policy and state transitions while a host/sidecar owns the legal network
transport.

The generic substrate boundary is tracked separately in `docs/app_local_substrate.md`: SQLite default
state, optional PostgreSQL preview preflight, config/secrets, auth principal carriage, jobs/outbox,
and observe telemetry are platform capabilities, while app-specific schemas should emerge from the
actual app being built.

That third layer matters for parity discussions: Nebula should not claim a mature APP platform
until the Nebula-owned, Host-owned, and Ops-owned responsibilities are all explicit enough to ship.

## UI Best Practices

### Keep the host->core boundary coarse-grained

Send:

- user intents
- route changes
- form submissions
- durable commands

Avoid sending:

- per-pixel updates
- layout-only data that the host can derive itself
- host-specific widget state that does not belong to the app model

### Treat Nebula output as an app-state or view-model snapshot

Good shape:

- the host asks for current app state or a focused view model
- the host renders from that snapshot
- the host sends explicit events back into Nebula

The current `examples/thin_host_app_core` sample now demonstrates this as an explicit
command/query/event seam:

- commands in from the host
- lifecycle events back out to the host
- snapshot queries for render state
- versioned `thin-host-bridge.command.v1`, `event.v1`, and `snapshot.v1` envelopes from
  `official/nebula-thin-host-bridge`
- `correlation_id` and `state_revision` fields for deterministic replay, stale-command rejection,
  and telemetry/crash correlation
- rejected commands preserve state and return a `command_rejected` event instead of silently
  applying hidden fallback behavior

Avoid:

- using Nebula as an immediate-mode render loop
- making every small widget property a separate `extern fn` round trip

### Keep design in the host layer

Typography, spacing, component styling, animation curves, transitions, and accessibility semantics
should stay in the UI host.

Nebula should not become the place where you encode:

- raw color tokens
- layout grids
- responsive breakpoints
- pixel-level animation details

That keeps the product visually intentional while letting the UI evolve without rewriting the app
core.

## Performance Best Practices

### 1. Batch event traffic

Prefer fewer, meaningful host->core calls over many tiny calls.

Examples:

- send a debounced text-edit intent instead of every keystroke when full fidelity is unnecessary
- send a structured submit/save/search event rather than multiple micro-mutations

### 2. Compute expensive derivations on state changes, not per render

If a list/filter/sort/projection is expensive:

- derive it when the relevant app state changes
- cache it in the app/core state or in a stable view-model layer

Avoid recomputing heavy derived state on every host render pass.

### 3. Virtualize large collections in the host

Long lists, tables, trees, and feeds should be virtualized by the host UI framework.

Nebula should provide:

- data identity
- ordering
- selection/filter state
- compact row/item view models

The host should own:

- viewport math
- cell reuse
- scrolling behavior

### 4. Keep render pure

Rendering should not mutate app state.

Good render path:

- read snapshot
- diff in the host framework
- paint

Bad render path:

- render calls back into Nebula for business mutations
- render performs blocking I/O
- render allocates large transient host<->core traffic for every frame

### 5. Let the host handle platform concurrency

If you need:

- background image decode
- network fetch orchestration
- local database adapters
- OS notifications

keep the platform/runtime-specific concurrency in the host shell, then hand structured results or
events into Nebula.

That avoids coupling the app core to one UI/runtime technology.

## Anti-Patterns

- Turning Nebula into a UI toolkit instead of an app-core layer
- Encoding layout/styling rules in the app core
- Performing render-time side effects through `extern fn`
- Building chatty host<->core APIs with one call per tiny widget field
- Treating thin-host as a shortcut for skipping a real design system in the host

## Recommended Mental Model

Use Nebula for:

- what the app means
- how the app changes
- what state the UI should reflect

Use the host for:

- how the UI looks
- how it animates
- how it integrates with the platform

That is the path that best preserves Nebula's original goals while still enabling strong,
performant frontend and UI work.
