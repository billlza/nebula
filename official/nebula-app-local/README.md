# nebula-app-local

Preview composition package for standalone or thin-host apps that need a small app-local substrate.
It combines existing official packages without claiming a new database abstraction, app framework, or
complete runtime.

Current surface:

- `substrate::AppLocalConfig`
- `substrate::AppLocalPreflight`
- `substrate::AppLocalPrincipal`
- `substrate::AppLocalCommandContext`
- `substrate::AppLocalEvent`
- `substrate::AppLocalRecoveryMarker`
- `substrate::AppLocalUpdateMarker`
- `substrate::AppLocalLifecycleMarker`
- `substrate::AppLocalReceipt`
- `substrate::AppLocalReceiptReplayBatch`
- `substrate::AppLocalReplayTrace`
- `substrate::AppLocalStartupRecoveryPolicy`
- `substrate::app_local_config(...) -> Result<AppLocalConfig, String>`
- `substrate::config_from_env(default_app_id, default_sqlite_path, default_principal_subject, default_observe_service) -> Result<AppLocalConfig, String>`
- `substrate::validate_config(config) -> Result<Bool, String>`
- `substrate::preflight(config) -> Result<AppLocalPreflight, String>`
- `substrate::principal(subject, role, auth_mode) -> Result<AppLocalPrincipal, String>`
- `substrate::principal_from_claims(claims, role) -> Result<AppLocalPrincipal, String>`
- `substrate::command_context(command_id, correlation_id, state_revision, principal, now_unix_ms) -> Result<AppLocalCommandContext, String>`
- `substrate::next_state_revision(context, accepted) -> Int`
- `substrate::command_accepted_event(context, event_kind, message) -> Result<AppLocalEvent, String>`
- `substrate::command_rejected_event(context, message) -> Result<AppLocalEvent, String>`
- `substrate::recovery_marker(app_id, correlation_id, state_revision, status, path) -> Result<AppLocalRecoveryMarker, String>`
- `substrate::update_marker(app_id, correlation_id, state_revision, status, manifest_path, manifest_sha256) -> Result<AppLocalUpdateMarker, String>`
- `substrate::lifecycle_marker(app_id, runtime_session_id, correlation_id, state_revision, status, reason) -> Result<AppLocalLifecycleMarker, String>`
- `substrate::initialize_receipts(path) -> Result<Bool, String>`
- `substrate::record_receipt(path, app_id, receipt_kind, receipt_key, correlation_id, state_revision, schema, payload, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_command_context_receipt(path, app_id, context, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_event_receipt(path, app_id, event, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_snapshot_receipt(path, app_id, receipt_key, correlation_id, state_revision, schema, payload, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_recovery_marker_receipt(path, marker, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_update_marker_receipt(path, marker, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::record_lifecycle_marker_receipt(path, marker, created_unix_ms) -> Result<AppLocalReceipt, String>`
- `substrate::receipt_by_key(path, app_id, receipt_kind, receipt_key) -> Result<AppLocalReceipt, String>`
- `substrate::replay_receipts(path, app_id, receipt_kind, after_receipt_id, limit) -> Result<AppLocalReceiptReplayBatch, String>`
- `substrate::recovery_replay_trace(path, app_id, limit) -> Result<AppLocalReplayTrace, String>`
- `substrate::startup_recovery_policy(path, app_id, limit) -> Result<AppLocalStartupRecoveryPolicy, String>`
- `substrate::receipt_count(path, app_id, receipt_kind) -> Result<Int, String>`
- `substrate::latest_receipt(path, app_id, receipt_kind, correlation_id) -> Result<AppLocalReceipt, String>`
- `substrate::validate_background_stages(stages) -> Result<Bool, String>`
- `substrate::emit_preflight_observe(config, report)`

Dependency shape from a Nebula repo checkout:

```toml
[dependencies]
app_local = { path = "/path/to/nebula/official/nebula-app-local" }
```

Release posture:

- repo-local preview package
- not part of the Linux backend SDK GA contract
- not installed inside the backend SDK preview payload
- intended to prove generic app-local substrate composition before a real validation app hardens
  domain-specific behavior

Current guarantees:

- `config_from_env(...)` reads the narrow app-local environment contract:
  - `APP_LOCAL_APP_ID`
  - `APP_DATA_BACKEND=sqlite|postgres`
  - `APP_SQLITE_PATH`
  - `APP_POSTGRES_PREVIEW`
  - `APP_POSTGRES_DSN` / `APP_POSTGRES_DSN_FILE`
  - `APP_AUTH_REQUIRED`
  - `APP_AUTH_MODE=internal|token|jwt`
  - `APP_PRINCIPAL_SUBJECT`
  - `APP_OBSERVE_SERVICE`
- SQLite remains the default local data plane and is referenced by explicit path in preflight.
- PostgreSQL is preview opt-in and requires `postgres_preview=true`, a DSN, and auth enabled before
  runtime probing is attempted.
- Auth principal carriage is explicit and can be derived from `nebula-auth` JWT claims without adding
  login, session, or OIDC client behavior.
- Command context helpers standardize `command_id`, `correlation_id`, `state_revision`,
  `now_unix_ms`, and actor principal carriage for app-core command processing.
- Accepted command events advance `state_revision`; rejected command events preserve it and use the
  stable `command_rejected` event kind.
- Recovery markers use `nebula.app-local.recovery-marker.v1` and carry app id, correlation id, state
  revision, status, and host-owned marker path.
- Update markers use `nebula.app-local.update-marker.v1` and carry app id, correlation id, state
  revision, status, manifest path, and manifest checksum. They do not download, sign, notarize, or
  apply updates.
- Runtime lifecycle markers use `nebula.app-local.lifecycle-marker.v1` and carry app id,
  `runtime_session_id`, correlation id, state revision, status, and reason. The preview status set is
  `startup_started`, `app_ready`, `app_degraded`, and `shutdown_clean`; these receipts are evidence
  for startup diagnosis, not a hidden runtime state machine.
- Runtime receipts use the default SQLite data plane to persist command context, command event,
  snapshot, recovery marker, update marker, and lifecycle marker payloads under
  `app_local_runtime_receipts`. The
  receipt schema is generic, keyed by `app_id + receipt_kind + receipt_key` for idempotent replay,
  and indexed by app id, receipt kind, correlation id, and state revision so host shells can recover
  or diagnose app-core progress without adopting a media, game, or editor domain model.
- Recovery/replay reads are cursor-based by receipt id. `receipt_by_key(...)` resolves one stable
  receipt, `replay_receipts(...)` pages one receipt kind, and `recovery_replay_trace(...)` returns a
  startup-oriented summary of recent snapshots, command contexts, events, and recovery/update
  markers. Replay limits are bounded and this is not an audit-log query language.
- Startup recovery policy uses `recovery_replay_trace(...)` to produce
  `nebula.app-local.startup-recovery-policy.v1`: latest revision evidence, last snapshot, last
  accepted/rejected command event, last recovery/update marker, and lifecycle session evidence. The
  policy is diagnostic-only, reports `action_owner="app"` and windowed revision confidence, and keeps
  recovery, replay, rollback, or update application decisions in the app.
- Jobs integration is limited to DAG validation in this package; worker lease/outbox storage remains
  in `nebula-jobs`.
- Observe integration emits log-first preflight events and delta-counter metrics through
  `nebula-observe`.
- Preflight JSON contains selected source/status fields and must not contain secret payloads.

Non-goals:

- ORM or query DSL
- transparent SQLite/PostgreSQL abstraction
- PostgreSQL jobs store
- cloud KMS, dynamic secret rotation, local accounts, or password auth
- native renderer/windowing, game loop, media codecs, torrent transport, or app-store packaging
- append-only audit log, event-sourcing engine, external broker, or sync protocol
- exporter, collector, or resident telemetry daemon
