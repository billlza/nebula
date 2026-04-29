# App-Local Substrate Preview

This preview contract describes the generic local substrate that a thin-host app can embed before it
adds domain-specific behavior. It is not a media-player template, game runtime, ORM, broker platform,
or app-store packaging story.

The repo-local `official/nebula-app-local` package is the narrow composition point for this contract.
It provides config/preflight structs, explicit principal carriage, jobs DAG validation, and observe
preflight emission while leaving durable SQLite operations, PostgreSQL queries, jobs leases/outbox,
and app-specific schemas in their owning packages.

## Contract Shape

Every app-local substrate smoke should keep these responsibilities explicit:

- SQLite is the default durable local data plane for app state, migrations, receipts, and recovery
  breadcrumbs.
- PostgreSQL is optional repo-preview for server-backed or multi-device metadata and must stay
  opt-in through explicit config preflight.
- `nebula-config` owns env, mounted secret file, typed config parsing, and redacted diagnostics.
- `nebula-auth` owns resource-server JWT validation and principal-shaped claims; app-local internal
  principals must still be explicit at command boundaries.
- `nebula-jobs` owns background work, retry/dead-letter, receipts, and durable outbox semantics.
- `nebula-observe` owns log-first telemetry, correlation markers, and delta-counter events.
- `nebula-app-local` ties the startup preflight/principal/observe pieces together without hiding the
  underlying package boundaries.

The substrate should expose command/event/snapshot-friendly state, but it must not pre-build a media library,
game entity model, editor document model, or operations-console workflow. Those shapes should be
discovered only when a validation app actually needs them.

## Default Data Plane

SQLite remains the default for standalone apps:

- app settings and local state
- command/event receipts
- background job state
- outbox messages
- crash and recovery breadcrumbs

PostgreSQL preview is allowed only when the app opts in with a clear operator preflight. There is no
automatic SQLite-to-PostgreSQL migration claim, no transparent cross-database abstraction, and no
requirement that standalone apps use a network database.

## Startup Preflight

A substrate startup preflight should report:

- selected data backend: `sqlite` by default, `postgres` only when explicitly enabled
- SQLite path or state directory
- redacted secret-file diagnostics
- auth/principal mode
- jobs/outbox database path
- observe log/correlation settings
- update/recovery manifest path when the host shell stages one

`official/nebula-app-local` provides `config_from_env(...)` for the shared environment contract:
`APP_LOCAL_APP_ID`, `APP_DATA_BACKEND`, `APP_SQLITE_PATH`, `APP_POSTGRES_PREVIEW`,
`APP_POSTGRES_DSN` / `APP_POSTGRES_DSN_FILE`, `APP_AUTH_REQUIRED`, `APP_AUTH_MODE`,
`APP_PRINCIPAL_SUBJECT`, and `APP_OBSERVE_SERVICE`.

Secrets must never appear in stdout, stderr, observe logs, or preflight JSON. Diagnostics should show
presence, source, and status, not secret payloads.

## Runtime Context

`official/nebula-app-local` also standardizes the narrow runtime context that every app-core command
can carry:

- `command_id`
- `correlation_id`
- `state_revision`
- `actor_subject` and `actor_role`
- `auth_mode`
- `now_unix_ms`

Accepted command events advance the state revision by one. Rejected command events preserve the
incoming revision and use the stable `command_rejected` event kind. This keeps stale-command
rejection, replay, telemetry, and crash correlation generic enough for a media player, game, editor, or operations console.

Recovery markers use `nebula.app-local.recovery-marker.v1` and carry app id, correlation id, state
revision, status, and a host-owned marker path. They are a contract for staging diagnostics, not a
crash uploader or auto-recovery daemon.

Update markers use `nebula.app-local.update-marker.v1` and carry app id, correlation id, state
revision, status, manifest path, and manifest checksum. They are a sidecar contract for host-owned
bundle/update flows, not a downloader, signer, notarization flow, rollback engine, or auto-updater.

## Runtime Receipts

The default local recoverability path is SQLite. `official/nebula-app-local` provides a generic
`app_local_runtime_receipts` table plus helpers for command contexts, command events, recovery
markers, update markers, and host snapshots. Receipts store the stable schema string, app id, receipt key,
correlation id, state revision, created time, and the same JSON payload that can still be emitted as
sidecar output. `app_id + receipt_kind + receipt_key` is unique so replaying the same command or
marker returns the existing receipt instead of creating an ambiguous duplicate.

This is intentionally narrower than an audit log or event-sourcing framework. It gives a thin-host
shell enough local facts to replay, reject, diagnose crashes, and correlate staged updates, while
leaving media libraries, game save data, editor documents, and sync semantics to the validation app
that actually needs them.

## Recovery Replay

`receipt_by_key(...)` reads a single stable receipt by `app_id`, `receipt_kind`, and `receipt_key`.
`replay_receipts(...)` pages one receipt kind after a receipt id cursor with a bounded limit.
`recovery_replay_trace(...)` assembles the recent generic trajectory a host shell needs at startup:
snapshots, command contexts, command events, and recovery/update markers.

This is a startup diagnostic and replay substrate, not an audit-log query language, sync engine, or
domain state model. The validation app still owns how to interpret a snapshot payload, whether an
event can be replayed, and which recovery action is safe.

## App Boundaries

The substrate is allowed to standardize:

- command id, correlation id, state revision, and actor/principal carriage
- durable local writes and migrations
- background work and outbox receipts
- crash/recovery markers
- telemetry counters and classified logs

The substrate must not standardize:

- media codecs, torrent policy, playback UX, or media-library schema
- game loops, physics, scene formats, or asset pipelines
- editor document formats or collaboration semantics
- external broker clients, cron daemons, or distributed deploy agents
- cloud KMS, secret rotation, or local account/password systems

## Validation Bar

The minimum contract smoke should compile and run a small app that combines:

1. `nebula-config` required env and mounted-secret file lookup with redacted diagnostics.
2. `nebula-db-sqlite` migrations, write, query, indexed row read, and close.
3. `nebula-db-postgres` runtime probe as an explicit preview preflight, without requiring a server.
4. `nebula-auth` principal/claim shape at the command boundary.
5. `nebula-jobs` DAG validation for local background work.
6. `nebula-observe` info event and delta-counter output.

That smoke proves the substrate composition, not a complete app platform.
