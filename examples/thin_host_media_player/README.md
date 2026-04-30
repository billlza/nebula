# thin-host-media-player

Nebula-first validation APP skeleton for the thin-host direction.

The media player app core is written in Nebula: command validation, state transitions, domain events,
snapshots, SQLite app-local receipts, startup recovery policy, jobs/outbox smoke, observe preflight,
and update/recovery markers. The C++ file in this example is only a host adapter/test driver: it
opens the preview shell, supplies command envelopes, decodes UI IR for adapter smoke, and prints host
markers. It does not own media-player business state.

Preview scope:

- `thin-host-bridge.command.v1` wire envelopes carrying `media-player.command.v1` domain commands
  for import, quality, bitrate, and download controls
- phase1 library selection, playback sidecar open/progress, and torrent progress state
- `media-player.event.v1` accepted/rejected events with `correlation_id` and `state_revision`
- `media-player.snapshot.v1` compact host snapshot with selection, playback, download, sidecar,
  and recovery diagnostics
- app-local preflight, startup recovery policy, lifecycle markers, receipts, and host snapshot
  readiness
- Nebula-owned SQLite media tables for library items, settings, import tasks, download tasks, and
  recovery diagnostics
- jobs DAG retry/dead-letter and outbox dead-letter evidence
- bundle/update/recovery preview manifests
- host sidecar manifest for file picker, codec/player, and torrent adapter boundaries

Non-goals:

- native audio/video renderer
- codec engine
- real BitTorrent network transport
- piracy search, DRM bypass, tracker scraping, or hidden executable payload handling
- App Store packaging, notarization, auto-updater, or GUI GA

Torrent import is limited to legal media: public-domain, open-licensed, or operator-owned content.
The host or an operator-approved sidecar owns any future network transport; Nebula validates policy
and persists state/receipts.
