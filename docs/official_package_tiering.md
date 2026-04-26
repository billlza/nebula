# Official Package Tiering

Installed Linux backend SDK packages:

- `nebula-service`
- `nebula-observe`

Installed Linux backend SDK preview packages:

- `nebula-db-sqlite`

Backend-SDK expectations:

- Linux backend GA contract with explicit non-goals
- smoke-tested in contract coverage
- distributed in a separate Linux backend SDK asset
- installed only on explicit opt-in

Repo-local preview packages:

- `nebula-db-postgres`
- `nebula-config`
- `nebula-crypto`
- `nebula-tls`
- `nebula-pqc-protocols`
- `nebula-qcomm-sim`
- `nebula-thin-host-bridge`
- `nebula-ui`

Preview-package expectations:

- owned in-repo
- smoke-tested in contract coverage where practical
- documented current guarantees and explicit non-goals
- aligned to the public service profile
- consumed from the repo checkout via `path` dependencies unless a package is explicitly called out
  as an installed preview payload
- not promoted to GA just because it ships in an opt-in backend SDK asset

Installed-preview-package expectations:

- distributed inside the opt-in Linux backend SDK asset for convenience
- still documented as preview, not backend SDK GA
- may be consumed through `installed = "..."` on Linux x86_64 once the backend SDK asset is
  installed
- still allowed to evolve within preview boundaries

Current package intent:

- `nebula-service`: bounded HTTP service helpers with env-driven config, request-id header policy, request context, JSON framework errors, and drain/shutdown-file based graceful quiesce
- `nebula-observe`: structured logs and counter-shaped metrics events, including request-correlated error logs
- `nebula-db-sqlite`: embedded SQLite data-plane slice for backend-first internal apps
- `nebula-db-postgres`: network PostgreSQL data-plane slice with dynamic `libpq` probing, migrations, and narrow query/execute helpers
- `nebula-config`: preview app-level env, mounted-secret, and redacted preflight helpers
- `nebula-crypto`: low-level crypto + PQC primitives
- `nebula-tls`: outbound client-side TLS/HTTPS helpers
- `nebula-pqc-protocols`: application-layer PQC signed helpers + authenticated secure-channel helpers
- `nebula-qcomm-sim`: simulation-only BB84 quantum-communication lab package
- `nebula-thin-host-bridge`: preview command/event/snapshot envelope contract for thin-host app
  cores, including correlation, state revision, replay validation, and explicit non-UI non-goals
- `nebula-ui`: preview semantic UI tree package for `ui` / `view` source syntax and adapter-facing JSON IR
