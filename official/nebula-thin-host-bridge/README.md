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

Embedding note:

- the host owns rendering, navigation, accessibility, platform I/O, animation, and packaging
- Nebula owns application state, validation, reducers/transitions, and compact snapshots
- bridge traffic should stay coarse-grained: durable commands in, meaningful events out, snapshots
  queried for render state

Non-claims:

- no GUI renderer
- no widget toolkit
- no layout, style, animation, or accessibility tree contract
- no React/SwiftUI/WinUI/AppKit/GTK replacement layer
- no desktop/mobile packaging, signing, or update lifecycle
