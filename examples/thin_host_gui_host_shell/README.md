# thin-host-gui-host-shell

Preview pilot for the production-adjacent GUI direction:

- the native host shell owns the render loop and platform adapter
- Nebula owns state, validation, action-to-command reduction, and snapshot/event envelopes
- `official/nebula-ui` provides the semantic `nebula-ui.tree.v1` IR consumed by the host preview
  decoder
- `official/nebula-thin-host-bridge` provides the command/event/snapshot envelope contract

This is not a native renderer, layout engine, app distribution story, or GUI GA surface. It is the
smallest app-shell forcing slice that proves a host can decode Nebula UI IR, dispatch stable action
ids, send versioned thin-host commands, and receive deterministic snapshots/events from Nebula.

Preview closure markers:

- window lifecycle summary: `boot`, `window-created`, `window-shown`, `window-focused`, `rendered`,
  `close-requested`, `closed`
- accessibility summary: `Window` maps to `window`, `Input.accessibility_label` maps to `textbox`,
  and `Button.text` maps to `button`
- runtime preflight: `app-local-preflight:` publishes `nebula.app-local.preflight.v1` from the
  app-local env/config contract before receipt replay or new commands
- bundle preview manifest: `deploy/bundle/manifest.preview.json` records the host API version,
  entry binary name, staged assets, update-manifest checksum, telemetry correlation source, and
  crash-report marker schema
- startup recovery policy: `app-local-startup-policy:` publishes
  `nebula.app-local.startup-recovery-policy.v1` from the local receipt DB before new commands run;
  it is diagnostic-only and keeps `action_owner="app"`
- runtime lifecycle: `app-local-lifecycle:` publishes `nebula.app-local.lifecycle-marker.v1` for
  `startup_started`, `app_ready`, and `shutdown_clean` evidence
- deploy examples under `deploy/` are schema fixtures for host packaging/update contracts only;
  they are not signing, notarization, store packaging, or auto-update implementation
