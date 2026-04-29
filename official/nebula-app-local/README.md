# nebula-app-local

Preview composition package for standalone or thin-host apps that need a small app-local substrate.
It combines existing official packages without claiming a new database abstraction, app framework, or
complete runtime.

Current surface:

- `substrate::AppLocalConfig`
- `substrate::AppLocalPreflight`
- `substrate::AppLocalPrincipal`
- `substrate::app_local_config(...) -> Result<AppLocalConfig, String>`
- `substrate::config_from_env(default_app_id, default_sqlite_path, default_principal_subject, default_observe_service) -> Result<AppLocalConfig, String>`
- `substrate::validate_config(config) -> Result<Bool, String>`
- `substrate::preflight(config) -> Result<AppLocalPreflight, String>`
- `substrate::principal(subject, role, auth_mode) -> Result<AppLocalPrincipal, String>`
- `substrate::principal_from_claims(claims, role) -> Result<AppLocalPrincipal, String>`
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
- exporter, collector, or resident telemetry daemon
