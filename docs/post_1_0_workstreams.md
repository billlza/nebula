# Post-1.0 Workstreams

This document turns the next major Nebula workstreams into explicit execution tracks instead of
letting them blur together into one oversized branch.

The current `1.0.x` release contract stays narrow:

- compiler / CLI / package workflow / bundled `std`
- Linux backend SDK (`nebula-service`, `nebula-observe`) on explicit opt-in
- preview package slices under `official/*`

The work below is intentionally post-`1.0` and should be delivered as separate tracks with their
own tests, docs, and sign-off.

## Track 1: LSP Daily-Use

Goal:

- move `nebula lsp` from a narrow language-server surface to a credible daily-use editor workflow

Current baseline:

- implemented today:
  - diagnostics
  - hover
  - definition
  - completion
  - references
  - document symbols
  - incremental sync
  - rename
  - semantic tokens
  - code actions
  - signature help
  - workspace symbol
- remaining hardening focus:
  - broader multi-file and broken-parse regression coverage
  - faster workspace-scale indexing than the current on-demand merged-source approach
  - broader structured quick-fix coverage beyond today's first direct `WorkspaceEdit` fixes

Exit criteria:

- `textDocumentSync` supports incremental document changes
- `rename`, `semanticTokens`, `codeAction`, `signatureHelp`, and `workspaceSymbol` are implemented
- README, CLI/spec docs, and VS Code docs all describe the same LSP surface
- VS Code integration prefers LSP features and falls back only when the server does not advertise
  them
- contract tests cover capability advertisement, broken-parse fallback, and multi-file projects

Current status:

- the feature-complete daily-use surface now exists in the repo wave
- the remaining work is hardening, scale, and richer fix semantics rather than basic feature
  absence

## Track 2: Hosted Registry GA

Goal:

- make hosted registry support a real installed-binary feature instead of repo-local preview wiring

Current baseline:

- local file-based registry is part of the main CLI contract
- hosted registry support now ships with installed binaries and still shells out through bundled
  Python tooling for the mirror-to-local-registry path

Exit criteria:

- installed `nebula` binaries can `publish/fetch/update` against a hosted registry directly
- the resolver contract is explicit for auth, immutability, timeouts, and lock replay
- no repo checkout is required to use the hosted registry path
- install/release docs and contract tests cover the hosted path

Current status:

- the installed-binary hosted registry path now exists and is covered by contract smoke
- the remaining work is operational hardening and release discipline on top of the current
  helper-backed path, not a silent resolver rewrite hidden inside `1.0.x`

## Track 3: Service Platform Lift

Goal:

- evolve the current narrow Linux backend SDK into a more complete service platform without
  overstating the maturity contract

Current baseline:

- today the service layer is intentionally narrow:
  - bounded request handling
  - request context
  - request-id policy
  - drain/shutdown files
  - log-first observe helpers
- explicit non-goals still include full framework ecosystems, keep-alive/pooling support,
  panic-to-`500`, and richer deployment/config stories

Exit criteria:

- route composition model is explicit and tested
- middleware/interceptor story is explicit and tested
- keep-alive / pooling / lifecycle behavior is documented and tested
- panic-to-`500` policy is explicit
- Prometheus/OpenTelemetry integration story is explicit
- config / secrets / deployment guidance is upgraded from narrow examples to operator-facing docs

Current status:

- fixed-arity context-aware route composition is now explicit and covered by contract smoke
- named-function middleware/interceptor helpers are now explicit and covered by contract smoke
- named-function auth/verifier hooks are now explicit and covered by example/service smoke
- the forcing-app lane now includes a real internal-event workflow surface with pull-based worker
  leases for the built-in `apply_release` task, while still stopping short of a general queue/DAG
  platform claim
- `official/nebula-jobs` now exists as the first repo-local reusable jobs/workflow kernel preview:
  DAG validation, SQLite-first run storage, worker leases, retry/dead-letter, idempotent receipts,
  and durable outbox helpers are available without copying the release-control-plane business store
  wholesale
- cron/schedule has a first opt-in deterministic tick path for the forcing app:
  `schedule put` defines event-producing schedules, and `schedule tick --now-unix-ms ...` submits
  stable idempotent internal workflow events without adding an in-process scheduler daemon
- external broker has a first opt-in durable outbox path for the forcing app:
  workflow run creation records `workflow.run.created` relay messages, and external relays use
  worker-gated claim/complete calls for retry and dead-letter handling
- public webhook integrations have a first opt-in signed ingress path for the forcing app:
  HMAC-SHA256 verified requests submit provider-neutral workflow events and reuse workflow receipt
  idempotency
- `official/nebula-config` now exists as a preview for app-level env, mounted-secret files, and
  redacted startup preflight diagnostics; it is additionally shipped as an opt-in Linux backend SDK
  installed-preview package, remains deliberately separate from `official/nebula-service` HTTP
  bind/timeout configuration, and does not claim cloud KMS, dynamic rotation, or secret storage
- auth/identity now has a first repo-local resource-server preview:
  `official/nebula-auth` verifies RS256 JWTs against caller-provided JWKS text, and the
  release-control-plane forcing app can opt into `APP_AUTH_MODE=jwt` while keeping static token mode
  as the default; it is additionally shipped as an opt-in Linux backend SDK installed-preview
  package, while local accounts, browser login, sessions, JWKS URL fetch/cache, and full OIDC
  client flows remain future work
- free-form shell command tasks have a first opt-in sidecar worker path for the forcing app:
  service workflow stages produce `shell_command` leases, while operator-owned CLI sidecars validate
  allowlists and execute argv-based `std::process` commands with timeout and capped-output result
  payloads; the service still never executes shell commands in-process
- distributed deploy orchestration has a first opt-in sidecar-backed alias for the forcing app:
  `ctl deploy apply` validates a target allowlist, submits a deploy-marked workflow event, and
  reuses the `shell_command` sidecar lane; remote agents, rollback orchestration, and artifact
  shipping remain future work
- default lifecycle is now documented and tested as one request / one response / one close per
  accepted connection, with explicit opt-in keep-alive now shipped as a narrow sequential-reuse
  contract
- collector-side Prometheus/OpenTelemetry bridge semantics are explicit through
  `nebula.observe.metric.v1` delta-counter events, and the backend SDK now ships a narrow sample
  `prometheus_bridge.py` helper for render/sidecar `/metrics` use; native exporters and in-process
  scrape endpoints remain future work
- keep-alive, panic-to-`500`, and deploy/config/secrets boundaries are now documented for
  operators; keep-alive has now landed as a minimal explicit contract, while panic recovery still
  remains future runtime/service work rather than a shipped guarantee

## Track 3.5: Embedded Data Plane

Goal:

- move backend-first internal apps from ad hoc app-local SQLite seams to one official embedded-data
  package and migration story

Current baseline:

- file-oriented `std::fs` is GA for CLI/tools
- repo examples can already reach SQLite through app-specific host seams
- reusable official data packages now exist for SQLite-first embedded state and a narrow
  Postgres/network-DB preview

Exit criteria:

- `official/nebula-db-sqlite` exists as a documented repo-local preview package
- runtime-backed connection / transaction / result-set handles are explicit and tested
- migration runner, execute/query helpers, and row getters are explicit and tested
- at least one backend-first internal-app sample uses the official package instead of a one-off
  host seam

Current status:

- this wave starts by landing `official/nebula-db-sqlite` as the first reusable embedded-data
  contract for backend-first internal apps
- the package may now also be distributed inside the opt-in Linux backend SDK asset as an installed
  preview payload, but it still remains outside the backend SDK GA contract
- `official/nebula-db-postgres` now exists as the first repo-local network database preview package:
  it dynamically probes `libpq`, exposes runtime-backed PostgreSQL connection/result handles,
  migrations, and narrow query/execute helpers, and the release-control-plane startup preflight now
  calls that official probe when `APP_POSTGRES_PREVIEW=1`
- the release-control-plane forcing app can now opt into Postgres as its active preview data plane
  through `APP_DATA_BACKEND=postgres`; SQLite remains the default, and no automatic migration,
  connection pooling, or ORM-style abstraction is implied
- broader data-platform work such as migration tooling, connection pooling, queues, or ORM-style
  abstractions remains future work

## Track 4: Crypto / TLS / PQC Hardening

Goal:

- move the preview crypto/TLS/PQC story from useful slices to a more complete lifecycle and
  transport contract

Current baseline:

- `nebula-crypto`: narrow primitive slice, no secure-memory contract
- `nebula-tls`: preview client/outbound TLS with TLS 1.2 / TLS 1.3 policy, explicit client
  identity wiring for outbound mTLS, and explicit ALPN policy shape
- `nebula-tls-server`: preview inbound TLS listener/server stream package for internal east-west
  service hops
- `nebula-pqc-protocols`: application-layer helpers, not transport or PKI integration

Exit criteria:

- secret material lifecycle and zeroization story is explicit
- secure-memory or equivalent protected-secret ownership model is explicit
- TLS 1.3 support is explicit and tested
- mTLS / client certificate policy is explicit and tested
- any hybrid PQC transport story is explicit, tested, and documented as a contract rather than a
  demo

Current status:

- transport-identity inputs are now tightened for the current TLS client slice:
  `server_name` / `host` must be non-empty, and NUL-containing `server_name` values are rejected
  before the native handshake path
- secret-lifecycle boundaries are now more explicit in package docs, but there is still no true
  secure-memory runtime contract
- secure-memory groundwork now has a named package-level contract skeleton that separates
  secret-bearing values from public/ciphertext interop and treats `to_bytes()` as an explicit
  secret-export boundary when the underlying value contains secret material
- `nebula-crypto/native` now has a package-local `SecretBytesOwner` seam for bridge-owned secret
  copies, zeroize-on-destroy behavior, and explicit export back to ordinary runtime `Bytes`
  without pretending persistent Nebula-side secret values are already protected storage
- TLS groundwork now has named evolution rules and negative tests that keep the current package
- `nebula-tls-server` now has a package-level smoke plus a thin `service::tls` adapter for
  internal service handling over TLS, including narrow preview `h2` transport smokes, while
  public edge-TLS remains out of scope

## Track 5: Quantum Direction

Goal:

- decide whether Nebula's quantum differentiation continues as communication/security simulation
  work, or whether a separate quantum-computing platform track is opened

Current baseline:

- `nebula-qcomm-sim` is simulation-only BB84 lab work
- there is no current circuit / gate / backend / provider abstraction for quantum computing

Required decision:

- Option A: continue `qcomm` / QKD simulation as the differentiation line
- Option B: open a separate quantum-computing wave with a new architecture and product contract

Current decision for this wave:

- choose Option A and continue `qcomm` / QKD simulation as Nebula's active quantum-differentiation
  line
- do not open a quantum-computing platform wave inside the current preview packages
- reuse the preview PQC authenticated-body envelope work where helpful so `qcomm` and the broader
  PQC line share concrete application-layer contracts instead of drifting into separate experiments

This remains a product and architecture decision, not a refactor hidden inside the current preview
packages. A future quantum-computing wave would require a separate circuit/gate/backend/provider
architecture and an explicit product contract.

## Track 6: App Platform Convergence

Goal:

- make Nebula's broader APP-platform claims measurable and lane-specific instead of drifting
  between backend/service success and implied GUI/platform parity

Current baseline:

- the repo already has the first backend-first internal-app substrate:
  - installed Linux backend SDK
  - `official/nebula-db-sqlite`
  - `official/nebula-thin-host-bridge`
  - `examples/release_control_plane_workspace`
  - `examples/thin_host_app_core`
- there is still no broad pure-Nebula desktop/mobile/web platform contract
- `benchmarks/backend_crypto` only covers narrow backend/crypto hot paths, not broader APP-platform
  maturity

Exit criteria:

- `docs/app_platform_convergence.md` defines the comparison model and truthful public positioning
- `benchmarks/app_platform` fixes the representative workload set and reference stacks for broader
  APP-platform comparisons
- internal-app and thin-host reference lanes both have explicit, maintained repo samples
- thin-host bridge contracts have deterministic host/replay parity and negative-path smoke coverage
- `control-plane-workspace` scaffolding consumes installed-preview `nebula-auth`, `nebula-config`,
  and `nebula-db-sqlite` when the backend SDK is present, while keeping remaining preview-only deps
  explicit
- README/support-matrix/thin-host/roadmap docs all describe the same positioning

Current status:

- this track starts by locking the comparison lanes and representative workloads, not by pretending
  full cross-language APP parity already exists
- the current truthful milestone remains backend-first internal apps first, thin-host contract
  second, broader pure-Nebula app-platform claims later

## Track 7: Nebula UI Preview

Goal:

- grow a Nebula-owned semantic UI layer without pretending the project already owns a mature
  renderer/windowing/accessibility platform

Current baseline:

- `official/nebula-ui` defines the first preview JSON view-tree IR
- `ui` / `view` source syntax parses, formats, typechecks, and lowers to a callable JSON tree
- `examples/local_ops_console_ui` is the first UI forcing slice and deliberately avoids Tauri
- `headless` provides deterministic CI/debug rendering for the same JSON tree
- guarded AppKit/GTK minimal-window smoke assets prove the first native adapter boundary without
  claiming renderer parity

Exit criteria:

- AppKit and GTK adapters can consume the same `nebula-ui.tree.v1` tree
- action dispatch, accessibility labels, and lifecycle events are tested through headless/native
  adapter smoke paths
- docs continue to distinguish Nebula UI preview from SwiftUI/Qt/Flutter maturity claims

## Execution Order

Recommended order:

1. Track 1: LSP Daily-Use
2. Track 2: Hosted Registry GA
3. Track 3: Service Platform Lift
4. Track 3.5: Embedded Data Plane
5. Track 6: App Platform Convergence
6. Track 7: Nebula UI Preview
7. Track 4: Crypto / TLS / PQC Hardening
8. Track 5: Quantum Direction

Why this order:

- Track 1 improves daily developer ergonomics immediately
- Track 2 removes a distribution/product bottleneck
- Track 3 should build on stable package/distribution expectations
- Track 3.5 gives backend-first internal apps a reusable data story before the project widens into
  thin-host/UI platform claims
- Track 6 fixes the truthful APP-platform lane and benchmark shape before Nebula starts making
  broader parity claims
- Track 7 starts the Nebula + Nebula UI split as preview semantics/IR first, before native renderer
  maturity claims
- Track 4 changes security claims and must not be mixed casually with platform-lift churn
- Track 5 branches product direction and should not be smuggled in as an implementation detail
