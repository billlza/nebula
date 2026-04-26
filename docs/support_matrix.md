# Support Matrix

Compiler/tooling GA release assets:

- macOS x86_64
- macOS arm64
- Linux x86_64
- Windows x86_64

Primary 1.0 authoring target:

- small-to-medium CLI / system tools
- file-oriented utilities using bundled `std`, including `std::fs`
- process-oriented internal tooling using preview `std::process` on macOS/Linux, with argv-based
  execution, timeout, and capped output; Windows process parity is not claimed

Installed GA surface:

- compiler / CLI
- bundled `std`
- runtime headers
- release documentation
- optional Linux backend SDK (`nebula-service`, `nebula-observe`, backend docs/examples) when explicitly installed

Backend service GA target:

- Production services: Linux x86_64
- Development and local smoke: macOS x86_64/arm64, Linux x86_64

Installed Linux backend SDK package matrix:

- `nebula-service`: Linux backend GA service core; current profile targets Linux x86_64 in production and macOS/Linux for local development, with env-driven service config, request-id header policy, request context, JSON framework errors, and drain/shutdown-file based graceful quiesce
- `nebula-observe`: Linux backend GA observability helper; same host reach as `nebula-service` for log/counter events, including request-correlated error logs
- installation contract: shipped as a separate Linux backend SDK asset and installed only on explicit opt-in

Installed Linux backend SDK preview package matrix:

- `nebula-auth`: Linux x86_64 installed preview package for backend-first internal apps when the
  backend SDK asset is installed; still documented as preview rather than backend SDK GA, with
  RS256 JWT verification against caller-provided JWKS text and no local accounts, browser login
  flow, JWKS URL fetch/cache, or session platform claim.
- `nebula-config`: Linux x86_64 installed preview package for backend-first internal apps when the
  backend SDK asset is installed; still documented as preview rather than backend SDK GA, with
  typed env parsing, mounted-secret file reads, mutual-exclusion checks, and redacted diagnostics.
- `nebula-db-sqlite`: Linux x86_64 installed preview package for backend-first internal apps when
  the backend SDK asset is installed; still documented as preview rather than backend SDK GA, with
  runtime-backed SQLite connection / transaction / result-set handles, migration runner, narrow
  query/execute helpers, and row getters
- `nebula-jobs`: Linux x86_64 installed preview package for backend-first internal apps when the
  backend SDK asset is installed; still documented as preview rather than backend SDK GA, with DAG
  validation, SQLite-first run storage, worker leases, retry/dead-letter, idempotent event receipt,
  and durable outbox helpers

Repo-local preview package matrix:

- `nebula-db-sqlite`: macOS + Linux; current preview embedded-data slice for backend-first internal
  apps, additionally shipped as an installed preview package inside the Linux backend SDK asset on
  Linux x86_64, including runtime-backed SQLite connection / transaction / result-set handles,
  migration runner, narrow query/execute helpers, and row getters
- `nebula-db-postgres`: macOS + Linux hosts with PostgreSQL client libraries available at runtime;
  current preview network-data slice for backend-first internal apps, including dynamic `libpq`
  probing, runtime-backed PostgreSQL connection / result-set handles, migration runner, narrow
  query/execute helpers, and row getters; repo-local path dependency only in this wave. The release
  control-plane forcing app may opt into it with `APP_DATA_BACKEND=postgres`, while SQLite remains
  the default data plane.
- `nebula-auth`: macOS + Linux; current preview resource-server authentication slice for
  backend-first internal apps, with RS256 JWT verification against caller-provided JWKS text and no
  local accounts, browser login flow, JWKS URL fetch/cache, or session platform claim. It is
  additionally shipped as an installed preview package inside the Linux backend SDK asset on Linux
  x86_64.
- `nebula-config`: macOS + Linux; current preview app-level env and mounted-secret helper for
  backend-first internal apps, with typed env parsing and redacted diagnostics. It is additionally
  shipped as an installed preview package inside the Linux backend SDK asset on Linux x86_64 and
  does not claim cloud KMS, dynamic rotation, or secret storage.
- `nebula-jobs`: macOS + Linux; current preview jobs/workflow kernel with DAG stage validation,
  SQLite-first run storage, pull-based worker leases, retry/dead-letter, idempotent event receipt,
  and durable outbox helpers. It is additionally shipped as an installed preview package inside the
  Linux backend SDK asset on Linux x86_64 and does not claim Postgres jobs storage, native broker
  clients, cron daemon ownership, workflow UI, in-process shell execution, or distributed agents.
- `nebula-crypto`: macOS + Linux
- `nebula-tls`: macOS + Linux; preview outbound/client TLS with TLS 1.2 / TLS 1.3 policy,
  explicit client identity attach for mTLS, explicit ALPN policy shape, and a narrow preview
  HTTP/2 client path
- `nebula-tls-server`: macOS + Linux; preview inbound TLS listener/server stream package for
  internal east-west service hops, plus the thin `service::tls` integration seam and narrow
  preview HTTP/2 service-to-service path
- `nebula-pqc-protocols`: same host reach as `nebula-crypto`; current preview includes signed
  payload helpers, ciphertext-only KEM envelopes, and `pqc::channel` with pinned signed initiator
  acceptance for server-side mutual-auth experiments
- `nebula-qcomm-sim`: same host reach as `nebula-crypto`; experimental BB84 simulation-only preview
- `nebula-thin-host-bridge`: compiler/tooling hosts; preview command/event/snapshot envelope
  contract for thin-host app cores, including `correlation_id`, `state_revision`, deterministic
  replay, and rejection semantics; no renderer, widget, layout, style, accessibility, packaging, or
  native UI platform contract
- `nebula-ui`: compiler/tooling hosts; preview semantic UI tree package paired with `ui` / `view`
  syntax, JSON IR, and a headless adapter, without a mature native renderer or app-store
  distribution contract

Repo-local preview example matrix:

- `examples/pqc_secure_service`: development/smoke on macOS + Linux from a repo checkout; illustrative application-layer PQC flow, not an installed GA service surface
- `examples/qcomm_bb84_lab`: development/smoke on macOS + Linux from a repo checkout; simulation-only lab example
- `examples/thin_host_app_core`: development/smoke on compiler/tooling hosts from a repo checkout; thin-host architecture sample, not a native UI platform contract
- `examples/thin_host_bridge_contract`: development/smoke on compiler/tooling hosts from a repo
  checkout; focused v1 envelope and negative-path smoke for `official/nebula-thin-host-bridge`

Important caveat:

- Windows remains part of the compiler/tooling release contract, not the full service-package maturity contract.
- fresh-install and installed-binary smoke for the GA surface are centered on the CLI/tooling story, including `nebula new --template cli` and `nebula run ... -- <program-args...>`
- The default binary archives and install scripts do not install the backend SDK; Linux users must opt in to the backend SDK asset when they want installed backend packages.
- Remaining preview packages under `official/*` are still consumed from a source checkout via `path` dependencies.
- the first internal-app platform wave remains backend-first: embedded data through
  `nebula-db-sqlite` preview comes before any claim of a mature UI/native app platform
- Native UI / desktop platform work is still post-1.0 preview scope. `nebula-ui` establishes a
  semantic UI IR plus guarded AppKit/GTK minimal-window smoke assets, not a complete
  SwiftUI/Qt/Flutter-class UI platform.
- the intended frontend/native direction is still the thin-host split documented in
  `docs/thin_host_app_shell.md`, not a pure-Nebula UI framework story
- the lane-by-lane maturity/gap contract for broader APP-platform claims lives in
  `docs/app_platform_convergence.md`
- universeOS and low-level system-profile claims are not part of the current support matrix; the
  staged convergence and future no-std/freestanding gates live in `docs/universeos_convergence.md`
  and `docs/system_profile.md`
- experimental system-profile CLI gates are available for contract checking:
  `--target system|freestanding|<triple>`, `--profile system`, `--no-std`, and
  `--panic abort|trap`; they forbid bundled `std` imports and force strict-region
  diagnostics, but they do not yet provide a freestanding runtime or kernel/driver support
- `nebula-qcomm-sim` is experimental preview only and is not a security or hardware-support contract.
