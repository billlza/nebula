# nebula-thin-host-bridge

Preview thin-host app-core bridge contract for Nebula applications embedded behind an external host
shell.

Current surface:

- `thin-host-bridge.command.v1` command envelopes
- `thin-host-bridge.event.v1` lifecycle event envelopes
- `thin-host-bridge.snapshot.v1` compact state/view-model snapshot envelopes
- deterministic `correlation_id` and `state_revision` fields for replay, telemetry correlation, and
  host/core parity checks
- schema validation and negative-path helpers for malformed, missing-field, and version-mismatch
  envelopes
- native canonical-envelope encoders for the preview bridge hot path, plus a conservative generated
  command fast path that falls back to full JSON parsing for non-canonical but valid host input
- payload-text encoder helpers are intentionally named `unchecked`: callers must pass JSON text
  produced by trusted bridge/app-core encoders, not arbitrary host input

Embedding note:

- the host owns rendering, navigation, accessibility, platform I/O, animation, and packaging
- Nebula owns application state, validation, reducers/transitions, and compact snapshots
- bridge traffic should stay coarse-grained: durable commands in, meaningful events out, snapshots
  queried for render state
- performance work must preserve fallback parsing and negative-path diagnostics; canonical fast
  paths are an implementation detail, not a narrower wire contract

Non-claims:

- no GUI renderer
- no widget toolkit
- no layout, style, animation, or accessibility tree contract
- no React/SwiftUI/WinUI/AppKit/GTK replacement layer
- no desktop/mobile packaging, signing, or update lifecycle
