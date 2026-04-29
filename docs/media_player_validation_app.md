# Media Player Validation Probe Preview

The first full-app validation probe is a thin-host media player. It is a forcing app for Nebula's
app-core, app-local substrate, packaging/update, and performance contracts; it is not a reusable app template
and not a claim that Nebula owns the native audio/video renderer.

The platform should not pre-build media-player-specific capabilities before the app work needs them.
Generic pieces such as command/event/snapshot envelopes, typed view models, local storage,
background jobs, config preflight, auth principals, telemetry, bundle manifests, update manifests,
and recovery markers belong in Nebula. Domain behavior such as codecs, torrent policy, playback
quality semantics, and media-library UX should be discovered and hardened while building this
validation app. A game, editor, or operations console should be able to stress the same generic
substrate without inheriting media-specific assumptions.

The reusable substrate boundary is tracked in `docs/app_local_substrate.md`; this document should
only add media-player-specific commands once the validation app implementation actually needs them.

## Product Scope

- import local audio/video files
- choose audio quality, video quality, and bitrate policy from explicit app settings
- keep playback state, library metadata, and download/import history in the app data plane
- emit deterministic command/event/snapshot envelopes through `thin-host-bridge.*.v1`
- stage a bundle/update/recovery manifest with crash and telemetry correlation

## Legal Torrent Boundary

Torrent import is preview-scoped to legal media only: public-domain content, open-licensed content,
or media the operator has rights to download. The validation app must not include piracy search,
copyright-bypass, DRM circumvention, tracker scraping for infringing content, or hidden executable
payload handling.

The first implementation should treat torrent support as a host-owned transport adapter:

- Nebula validates the import request, policy, desired quality, and state transition
- the host or an operator-approved sidecar owns network I/O and the BitTorrent engine
- Nebula receives progress/completion/failure events and persists metadata
- all download events carry `correlation_id`, `state_revision`, source URI hash, and content-policy
  status

## App-Local Substrate Probe

SQLite remains the default local data plane for this probe:

- library index
- playback preferences
- import queue
- torrent/download receipts
- crash/recovery breadcrumbs

PostgreSQL is included as an optional preview backend for multi-device or server-backed library
metadata:

- opt-in only
- explicit DSN preflight through `nebula-config`
- no automatic SQLite-to-PostgreSQL migration claim
- no claim that PostgreSQL is required for a standalone desktop app

Other platform pieces remain generic capabilities, not media-player-only APIs:

- `nebula-config`: quality defaults, import directories, legal-source policy, secret/path preflight
- `nebula-auth`: internal principal / local operator identity where needed
- `nebula-jobs`: background import, metadata scan, checksum, and download state transitions
- `nebula-observe`: telemetry, crash correlation, import/download progress counters

## First App-Specific Contract Slice

When the media player implementation starts, define `media-player.command.v1` commands:
   - `library.import_file`
   - `library.import_torrent`
   - `playback.set_audio_quality`
   - `playback.set_video_quality`
   - `playback.set_bitrate_policy`
   - `download.pause`
   - `download.resume`
   - `download.cancel`
2. Emit `media-player.event.v1` events with correlation and state revision.
3. Emit a compact `media-player.snapshot.v1` view model for the host.
4. Reject illegal/unsupported torrent sources explicitly and preserve state.
5. Stage a preview bundle/update/recovery manifest before any native renderer claim.

This app should advance only after the thin-host app shell closure remains green.
