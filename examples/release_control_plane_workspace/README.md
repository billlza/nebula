# Release Control Plane Workspace

Linux-first Nebula forcing-app slice that combines:

- a backend service under `apps/service`
- an operator CLI under `apps/ctl`
- a shared domain/validation core under `packages/core`

This first wave keeps the product surface intentionally narrow:

- typed config entries keyed by `app/channel/key`
- current release state keyed by `app/channel`
- per-release approval decisions keyed to `revision_id`
- append-only audit events for release, approval, and apply activity
- internal-event workflow definitions/runs keyed by `app/channel`
- pull-based worker leases for the built-in `apply_release` task
- `apply_release` worker failures follow the extracted `official/nebula-jobs` max-attempt retry
  semantics before the run is marked failed
- opt-in deterministic schedule definitions plus explicit `schedule tick --now-unix-ms ...`
- opt-in durable broker outbox relay surface for workflow lifecycle messages
- opt-in shell sidecar worker tasks backed by argv-based `std::process`
- opt-in signed webhook ingress that submits verified workflow events
- opt-in deploy apply alias that submits target-allowlisted shell workflow events
- `release apply` / `runner apply-once` as compatibility aliases over the workflow lane
- optimistic concurrency through `revision_id` plus `ETag` / `If-Match`
- authenticated operator access with `reader` / `writer` / `admin` / `worker`
- embedded SQLite persistence through the preview `official/nebula-db-sqlite` package
- repo-local Postgres preview package can be selected as the active data plane, while SQLite
  remains the default
- app-level env and mounted-secret inputs are resolved through the preview `official/nebula-config`
  package with redacted diagnostics and explicit mutual-exclusion errors
- preview DAG workflow dependencies and default-off capability contracts for schedules, brokers, Postgres, shell tasks,
  public webhooks, and distributed deploy orchestration
- `official/nebula-jobs` as the extracted repo-local preview kernel for reusable jobs/workflow
  validation, SQLite run storage, worker leases, and outbox semantics

It does not yet add:

- full migration of this product's release/apply business store onto `official/nebula-jobs`
- daemonized cron/scheduler ownership beyond explicit deterministic schedule ticks
- direct Kafka/NATS/RabbitMQ publishing inside the service
- provider-specific GitHub/GitLab/Stripe webhook adapters
- audit export pipelines
- remote distributed deploy agents, rollback orchestration, or broad background job platforms
- first-party accounts, cookie sessions, OIDC browser login, refresh tokens, or IdP management
- native edge TLS / HTTP2 / ALPN on the Nebula service itself
- GUI or thin-host UI

## Platform Preview Boundaries

The service exposes `/v1/platform/capabilities`, and the CLI exposes `ctl platform capabilities`,
so operators can see which larger platform lanes exist and which remain preview or default-off
contracts:

- DAG workflows with explicit `depends_on` stage edges
- cron/schedule
- external broker
- Postgres
- JWT resource-server auth
- free-form shell command tasks
- public webhook integrations
- distributed deploy orchestration

DAG dependencies are available in the workflow definition preview: if any stage declares
`depends_on`, every stage must declare it, and the service rejects duplicate stage names, unknown
dependencies, duplicate dependencies, self-dependencies, and cycles. Omitted `depends_on` keeps the
legacy linear behavior for existing release/apply definitions.

Schedule tick is available as an opt-in preview, not as an in-process daemon. Enable it with
`APP_SCHEDULES_PREVIEW=1` and `APP_AUTH_REQUIRED=1`, then run `ctl schedule tick --now-unix-ms N`
from an external operator-owned clock such as cron or systemd timer. Tick events use stable
`schedule-<app>-<channel>-<schedule_id>-<due_unix_ms>` event ids and submit the existing
`internal_event` workflow lane, so workflow receipt idempotency remains the convergence point.

External broker support is also an opt-in durable outbox seam, not an in-process broker client.
Enable it with `APP_EXTERNAL_BROKER_PREVIEW=1`, `APP_EXTERNAL_BROKER_URL`, auth, and a worker token.
Workflow run creation writes `workflow.run.created` messages into SQLite with stable dedupe keys;
an operator-owned relay claims messages through `ctl broker outbox claim`, publishes to the external
system, then completes with `published` or `failed`.

Shell command tasks are an opt-in sidecar worker preview, not service-internal command execution.
Enable the service gate with `APP_SHELL_TASKS_PREVIEW=1`, `APP_AUTH_REQUIRED=1`,
`APP_SHELL_TASK_ALLOWLIST`, and `APP_SHELL_TASK_WORK_DIR`. Workflow stages may then use
`task_kind = "shell_command"`, but only an operator-owned CLI sidecar runs the allowlisted command
through argv-based `std::process`; the service only owns leases, state, capped result payloads, and
audit.

Public webhook ingress is an opt-in signed event entrypoint, not a provider ecosystem. Enable it
with `APP_PUBLIC_WEBHOOKS_PREVIEW=1`, `APP_AUTH_REQUIRED=1`, and `APP_PUBLIC_WEBHOOK_SECRET` or
`APP_PUBLIC_WEBHOOK_SECRET_FILE`. V1 verifies `HMAC-SHA256` over
`timestamp_unix_ms + "." + raw_body`, enforces a 300 second replay window, and submits only the
existing workflow event shape.

App-level config and mounted-secret inputs are a preview package boundary, not a secret-management
platform. `official/nebula-config` resolves direct env values or mounted files, rejects conflicting
`APP_*` and `APP_*_FILE` pairs, trims trailing newline bytes from mounted secrets, and emits only
redacted diagnostics for secret-bearing values. It does not provide cloud KMS, dynamic rotation,
encrypted-at-rest secret storage, or a policy DSL.

Distributed deploy orchestration has a first opt-in sidecar-backed alias, not a remote agent
platform. Enable it with `APP_DISTRIBUTED_DEPLOY_PREVIEW=1`, `APP_AUTH_REQUIRED=1`, and
`APP_DEPLOY_TARGET_ALLOWLIST`; use `ctl deploy target validate` to check the operator-owned target
file, then `ctl deploy apply` to submit a deploy payload that still flows through the existing
workflow receipt and `shell_command` sidecar worker lane. The service does not open SSH sessions,
ship artifacts, or run rollback policy in this wave.

The other large platform lanes are not silently enabled. Dangerous previews require
`APP_AUTH_REQUIRED=1`; shell tasks also require explicit `APP_SHELL_TASK_ALLOWLIST` and
`APP_SHELL_TASK_WORK_DIR`, public webhooks require `APP_PUBLIC_WEBHOOK_SECRET` or file equivalent,
external broker requires `APP_EXTERNAL_BROKER_URL` plus worker auth, Postgres requires
`APP_POSTGRES_DSN` or `APP_POSTGRES_DSN_FILE`, and distributed deploy requires
`APP_DEPLOY_TARGET_ALLOWLIST`. JWT auth requires `APP_AUTH_MODE=jwt`,
`APP_AUTH_JWT_PREVIEW=1`, `APP_AUTH_JWT_ISSUER`, `APP_AUTH_JWT_AUDIENCE`, and
`APP_AUTH_JWKS_FILE`; it is a resource-server verifier, not a session or IdP platform. The Postgres preview currently means the official
`nebula-db-postgres` package and startup gate exist; when enabled, startup also probes the `libpq`
runtime through that official package before binding. `APP_DATA_BACKEND=postgres` opts this
workspace into the Postgres preview active data plane and requires `APP_POSTGRES_PREVIEW=1`,
`APP_AUTH_REQUIRED=1`, and `APP_POSTGRES_DSN` or `APP_POSTGRES_DSN_FILE`. SQLite remains the
default data plane; there is no automatic SQLite-to-Postgres migration, pooling, ORM, or dual-write
behavior in this slice.

Fetch the shared workspace lock first:

```bash
nebula fetch .
```

The workspace root defaults to the CLI member, so this is a quick sanity check:

```bash
nebula run . --run-gate none -- control-plane-validate
```

Build and run the service:

```bash
nebula build apps/service --out-dir .service-out

APP_STATE_DIR=./tmp/release-control-plane-state \
APP_SCHEDULES_PREVIEW=1 \
APP_EXTERNAL_BROKER_PREVIEW=1 \
APP_EXTERNAL_BROKER_URL=http://127.0.0.1:9090 \
APP_SHELL_TASKS_PREVIEW=1 \
APP_SHELL_TASK_ALLOWLIST=./deploy/shell-allowlist.json \
APP_SHELL_TASK_WORK_DIR=./tmp/shell-work \
APP_PUBLIC_WEBHOOKS_PREVIEW=1 \
APP_PUBLIC_WEBHOOK_SECRET=webhook-secret \
APP_AUTH_REQUIRED=1 \
APP_READ_TOKEN=reader-token \
APP_WRITE_TOKEN=writer-token \
APP_ADMIN_TOKEN=admin-token \
APP_WORKER_TOKEN=worker-token \
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
  ./.service-out/main.out
```

Optional Postgres preview data plane:

```bash
APP_DATA_BACKEND=postgres \
APP_POSTGRES_PREVIEW=1 \
APP_POSTGRES_DSN='postgresql://user:pass@127.0.0.1:5432/release_control_plane' \
APP_AUTH_REQUIRED=1 \
APP_READ_TOKEN=reader-token \
APP_WRITE_TOKEN=writer-token \
APP_ADMIN_TOKEN=admin-token \
APP_WORKER_TOKEN=worker-token \
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
  ./.service-out/main.out
```

Mounted-secret deployments can use `APP_POSTGRES_DSN_FILE` instead of `APP_POSTGRES_DSN`.

For a production-style mounted-secret layout, prefer the paired `*_FILE` env vars instead of raw
token values:

```bash
APP_STATE_DIR=./tmp/release-control-plane-state \
APP_SCHEDULES_PREVIEW=1 \
APP_EXTERNAL_BROKER_PREVIEW=1 \
APP_EXTERNAL_BROKER_URL=http://127.0.0.1:9090 \
APP_SHELL_TASKS_PREVIEW=1 \
APP_SHELL_TASK_ALLOWLIST=./deploy/shell-allowlist.json \
APP_SHELL_TASK_WORK_DIR=./tmp/shell-work \
APP_PUBLIC_WEBHOOKS_PREVIEW=1 \
APP_PUBLIC_WEBHOOK_SECRET_FILE=./secrets/webhook.token \
APP_AUTH_REQUIRED=1 \
APP_READ_TOKEN_FILE=./secrets/read.token \
APP_WRITE_TOKEN_FILE=./secrets/write.token \
APP_ADMIN_TOKEN_FILE=./secrets/admin.token \
APP_WORKER_TOKEN_FILE=./secrets/worker.token \
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
  ./.service-out/main.out
```

Template/operator deploy assets ship under:

```text
deploy/
```

In particular:

- `deploy/systemd/release-control-plane.service`
- `deploy/systemd/release-control-plane.env.example`
- `deploy/systemd/release-control-plane.tmpfiles.conf`
- `deploy/logrotate/release-control-plane.observe`
- `deploy/caddy/Caddyfile`
- `deploy/nginx/release-control-plane.conf`
- `deploy/container/Dockerfile.runtime`
- `deploy/k8s/README.md`
- `deploy/secrets/README.md`

In another shell, run the CLI:

```bash
nebula run apps/ctl --run-gate none -- status
nebula run apps/ctl --run-gate none -- info
nebula run apps/ctl --run-gate none -- auth whoami --token admin-token
nebula run apps/ctl --run-gate none -- config list --app control-plane --channel prod --token reader-token
nebula run apps/ctl --run-gate none -- approval get --app control-plane --channel prod --token reader-token
nebula run apps/ctl --run-gate none -- audit list --app control-plane --channel prod --token reader-token
```

Config control commands:

```bash
nebula run apps/ctl --run-gate none -- \
  config put --app control-plane --channel prod --key feature.flag --bool true --description "feature gate" --token writer-token --json

nebula run apps/ctl --run-gate none -- \
  config get --app control-plane --channel prod --key feature.flag --token reader-token

nebula run apps/ctl --run-gate none -- \
  config history --app control-plane --channel prod --key feature.flag --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  config snapshot --app control-plane --channel prod --config-revision 1 --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  config put --app control-plane --channel prod --key feature.flag --bool false --description "feature gate updated" --if-match 1 --token writer-token

nebula run apps/ctl --run-gate none -- \
  config delete --app control-plane --channel prod --key feature.flag --if-match 2 --token admin-token --json
```

Use the current `snapshot_revision_id` from `config list --json` as the release-side
`--config-revision` anchor for the current config catalog state:

```bash
nebula run apps/ctl --run-gate none -- \
  config list --app control-plane --channel prod --token reader-token --json
```

Release control commands:

```bash
nebula run apps/ctl --run-gate none -- \
  release put --app control-plane --channel prod --version 2026.04.21 --config-revision 1 --description "initial desired state" --token writer-token --json

nebula run apps/ctl --run-gate none -- \
  release get --app control-plane --channel prod --token reader-token

nebula run apps/ctl --run-gate none -- \
  release snapshot --app control-plane --channel prod --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  config snapshot --app control-plane --channel prod --config-revision 1 --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  release list --app control-plane --token reader-token

nebula run apps/ctl --run-gate none -- \
  approval decide --app control-plane --channel prod --if-match 1 --decision approved --reason "ready to promote" --token admin-token --json

nebula run apps/ctl --run-gate none -- \
  release apply --app control-plane --channel prod --if-match 1 --token writer-token --json

# same lane, explicit workflow view:
nebula run apps/ctl --run-gate none -- \
  workflow get --app control-plane --channel prod --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  workflow runs --app control-plane --channel prod --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  workflow event submit --app control-plane --channel prod --event-id release-2026-04-21 --event-kind release_apply_requested --release-revision 1 --token writer-token --json

nebula run apps/ctl --run-gate none -- \
  broker outbox claim --worker-id broker-relay-1 --topic workflow.run.created --now-unix-ms 1000 --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  broker outbox complete --worker-id broker-relay-1 --message-id 1 --lease-id broker-lease-1-1 --status published --message "published" --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  broker outbox get --message-id 1 --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  shell allowlist validate --file ./deploy/shell-allowlist.json --json

nebula run apps/ctl --run-gate none -- \
  shell worker run-once --worker-id shell-worker --allowlist-file ./deploy/shell-allowlist.json --work-dir ./tmp/shell-work --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  deploy target validate --file ./deploy/deploy-targets.json --json

nebula run apps/ctl --run-gate none -- \
  deploy apply --app control-plane --channel prod --target local --event-id deploy-2026-04-21 --release-revision 1 --target-file ./deploy/deploy-targets.json --token writer-token --json

nebula run apps/ctl --run-gate none -- \
  webhook verify --provider generic --event-id release-2026-04-21 --timestamp-unix-ms 1000 --body-file ./webhook.json --signature <hmac-sha256-hex> --json

nebula run apps/ctl --run-gate none -- \
  webhook submit-local --provider generic --app control-plane --channel prod --event-id release-2026-04-21 --event-kind release_apply_requested --release-revision 1 --timestamp-unix-ms 1000 --body-file ./webhook.json --signature <hmac-sha256-hex> --url http://127.0.0.1:40480 --json

nebula run apps/ctl --run-gate none -- \
  schedule put --app control-plane --channel prod --schedule-id nightly --event-kind release_apply_requested --release-revision 1 --interval-ms 60000 --next-due-unix-ms 1000 --token admin-token --json

nebula run apps/ctl --run-gate none -- \
  schedule tick --now-unix-ms 1000 --token admin-token --json

nebula run apps/ctl --run-gate none -- \
  runner apply-once --token admin-token --json

nebula run apps/ctl --run-gate none -- \
  worker claim --worker-id ops-worker-1 --task-kind apply_release --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  worker heartbeat --worker-id ops-worker-1 --lease-id lease-1-1 --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  worker complete --worker-id ops-worker-1 --lease-id lease-1-1 --status succeeded --message "release applied" --token worker-token --json

nebula run apps/ctl --run-gate none -- \
  audit list --app control-plane --channel prod --token reader-token --json

nebula run apps/ctl --run-gate none -- \
  release put --app control-plane --channel prod --version 2026.04.22 --config-revision 2 --description "promote desired state" --if-match 1 --token writer-token
```

Current optimistic concurrency contract:

- config entries use their own `revision_id` plus `ETag` / `If-Match`
- `config list` also reports the current channel-scoped `snapshot_revision_id`
- create may omit `--if-match`
- once a config entry exists, `config put` without `--if-match` is rejected with `428 config_precondition_required`
- stale `--if-match` values are rejected with `412 config_revision_mismatch`
- `config delete` requires the current config `revision_id` through `If-Match`
- successful `GET /v1/configs/...` and `PUT /v1/configs/...` replies emit `ETag: "<revision_id>"`
- the CLI validates that config response `ETag` matches `entry.revision_id`
- create may omit `--if-match`
- `release put --config-revision` must reference a real config snapshot revision for the same `app/channel`
- `config_revision = 0` is only valid for a never-configured empty catalog; once config history exists, releases must use a real snapshot revision from that scope
- once a release state exists, `release put` without `--if-match` is rejected with `428 release_precondition_required`
- stale `--if-match` values are rejected with `412 release_revision_mismatch`
- `approval decide` and `release apply` also bind to the current release `revision_id` through `If-Match`
- successful `GET /v1/releases/...` and `PUT /v1/releases/...` replies emit `ETag: "<revision_id>"`
- the CLI validates that the response `ETag` matches `entry.revision_id`

Current approval / apply contract:

- `config list/get/history` are reader-gated
- `config snapshot` is reader-gated and lets operators inspect the exact catalog state bound by `release.config_revision`
- `release snapshot` is a pure client-side alias that resolves the current release first, then fetches the referenced config snapshot
- `config put` is writer-gated
- `config delete` is admin-gated
- config writes and deletes also emit append-only audit events on the shared `app/channel` audit stream
- `approval get` returns `pending` until a decision is recorded for the current release `revision_id`
- `approval decide` is admin-gated and records either `approved` or `rejected`
- `workflow get/runs/run` are reader-gated
- `workflow event submit` is writer-gated and requires an explicit `event_id` for idempotency
- `schedule get` is reader-gated; `schedule put` and `schedule tick` are admin-gated and require
  `APP_SCHEDULES_PREVIEW=1`
- `broker outbox get` is reader-gated; `broker outbox claim/complete` are worker-gated and require
  `APP_EXTERNAL_BROKER_PREVIEW=1`
- role gates now flow through stable permission names such as `config.write`, `jobs.run.trigger`,
  `schedule.tick`, and `jobs.outbox.relay`; denied requests keep the legacy `required role: ...`
  text and append `permission=<name>` for operator-facing diagnostics
- broker outbox messages are at-least-once relay records; `failed` completion retries by
  `next_attempt_unix_ms`, while exhausted attempts become `dead_letter`
- broker outbox `claim` and `complete` are compare-and-swap operations: only one worker can claim a
  pending message, and duplicate or stale completions return conflict instead of being treated as
  success
- shell sidecar workers are worker-gated, require `APP_SHELL_TASKS_PREVIEW=1`, validate a local
  allowlist, and complete `shell_command` leases with structured `result_payload`
- signed webhook ingress accepts provider-neutral HMAC events and maps them only to existing
  workflow event submission
- `deploy apply` is a writer-gated client alias over workflow event submission; it requires a
  target allowlist, a `shell_command` workflow stage, and `APP_DISTRIBUTED_DEPLOY_PREVIEW=1`
- the shipped workflow lane stays `internal_event` with explicit `depends_on` DAG validation:
  - `approval_gate`
  - `leased_task(task_kind="apply_release")`
  - `leased_task(task_kind="shell_command")` when the shell sidecar preview is enabled
- `release apply` remains the compatibility writer path and internally submits the built-in `release_apply_requested` workflow event
- `runner apply-once` remains the compatibility admin path and internally claims/completes one ready `apply_release` lease
- `worker claim/heartbeat/complete` are worker-gated and are the first-class execution surface for the pull-based worker lane
- worker stage `claim`, `heartbeat`, and `complete` are lease-bound compare-and-swap operations:
  wrong worker, stale lease, or duplicate complete returns conflict; parallel DAG sibling completes
  serialize run-state refresh so the aggregate run status cannot drift from completed stage state
- approval state resets to `pending` whenever `release put` advances the release `revision_id`
- audit remains append-only; release writes, approval decisions, apply requests, and runner execution all emit events

Stored state is persisted in the SQLite database file:

```text
APP_STATE_DIR/release-control-plane.db
```

Current HTTP surface:

- `GET /healthz`
- `GET /readyz`
- `GET /v1/info`
- `GET /v1/auth/whoami`
- `GET /v1/configs/:app/:channel`
- `GET /v1/configs/:app/:channel/snapshots/:config_revision`
- `GET /v1/configs/:app/:channel/:key`
- `GET /v1/configs/:app/:channel/:key/history`
- `PUT /v1/configs/:app/:channel/:key`
- `DELETE /v1/configs/:app/:channel/:key`
- `GET /v1/releases/:app`
- `GET /v1/releases/:app/:channel`
- `PUT /v1/releases/:app/:channel`
- `GET /v1/releases/:app/:channel/approval`
- `PUT /v1/releases/:app/:channel/approval`
- `GET /v1/releases/:app/:channel/audit`
- `GET /v1/workflows/:app/:channel`
- `PUT /v1/workflows/:app/:channel`
- `GET /v1/workflows/:app/:channel/runs`
- `GET /v1/workflows/:app/:channel/runs/:run_id`
- `POST /v1/workflows/:app/:channel/events`
- `POST /v1/workers/claim`
- `POST /v1/workers/leases/:lease_id/heartbeat`
- `POST /v1/workers/leases/:lease_id/complete`
- `GET /v1/broker/outbox/:message_id`
- `POST /v1/broker/outbox/claim`
- `POST /v1/broker/outbox/:message_id/complete`
- `POST /v1/webhooks/:provider/:app/:channel`
- `POST /v1/releases/:app/:channel/apply`
- `POST /v1/runner/apply-once`

Current auth model:

- disabled unless `APP_AUTH_REQUIRED=1`
- `APP_AUTH_MODE` defaults to `token`; `APP_AUTH_MODE=jwt` opts into the JWT resource-server
  preview
- roles are `reader`, `writer`, `admin`, `worker`
- tokens are loaded from `APP_READ_TOKEN`, `APP_WRITE_TOKEN`, `APP_ADMIN_TOKEN`, `APP_WORKER_TOKEN`
- mounted secret files may be used through `APP_READ_TOKEN_FILE`, `APP_WRITE_TOKEN_FILE`,
  `APP_ADMIN_TOKEN_FILE`, `APP_WORKER_TOKEN_FILE`
- each `APP_*_TOKEN` / `APP_*_TOKEN_FILE` pair is mutually exclusive
- static token secret files are loaded during startup validation and then treated as the token-mode
  process-local snapshot
- changing mounted static token files requires a service restart before the new values apply
- optional app/channel scoped policy v1 is enabled by `APP_AUTH_SCOPE_FILE`, which points at a
  JSON document of subject bindings such as
  `{ "bindings": [{ "subject": "writer", "app_scope": ["control-plane"], "channel_scope": ["prod"] }] }`
- JWT mode requires `APP_AUTH_JWT_PREVIEW=1`, `APP_AUTH_JWT_ISSUER`,
  `APP_AUTH_JWT_AUDIENCE`, and `APP_AUTH_JWKS_FILE`
- JWT mode verifies RS256 Bearer tokens against the local JWKS file, maps the `roles` claim to the
  existing role model, and uses `sub` as the audit actor; in this wave `sub` must remain compatible
  with the existing path-safe actor contract
- when `APP_AUTH_SCOPE_FILE` is set, matching is done against the authenticated principal subject
  for both token mode and JWT mode; if a subject has no binding, scoped permissions fail closed
- the JWT JWKS file is validated during startup and read by the verifier when checking JWTs; Wave 1
  has no JWKS URL fetch/cache/rotation contract, so operators should roll key-file changes through
  an explicit service restart for predictable deployment
- JWT mode is resource-server auth only: no local accounts, cookie sessions, OIDC browser login,
  refresh-token handling, or JWKS URL fetch/cache/rotation
- reader-scoped permissions include `identity.read`, `platform.read`, `release.read`,
  `approval.read`, `audit.read`, `config.read`, `jobs.definition.read`, `jobs.run.read`,
  `jobs.outbox.read`, and `schedule.read`
- writer-scoped permissions include `release.write`, `release.apply`, `config.write`,
  and `jobs.run.trigger`
- admin-scoped permissions include `approval.decide`, `config.delete`, `jobs.definition.write`,
  `schedule.write`, `schedule.tick`, and `ops.run_apply_once`
- worker-scoped permissions include `jobs.worker.execute` and `jobs.outbox.relay`
- scoped policy v1 currently applies to app/channel-addressed routes:
  `release.*`, `approval.*`, `audit.read`, `config.*`, `jobs.definition.*`, `jobs.run.read`,
  `jobs.run.trigger`, `schedule.read`, and `schedule.write`
- background execution lanes are also scope-aware in this wave:
  `worker claim/heartbeat/complete` filter or reject against the lease's `app/channel`, and
  `broker outbox claim/get/complete` filter or reject against the message payload's `app/channel`
- `schedule.tick`, `ops.run_apply_once`, `platform.read`, and `identity.read` remain unscoped in
  this wave
- successful auth flows through the official `service::middleware::authorize_result(...)` principal carrier

Current transport contract:

- CLI base URLs may use `http://` or `https://`
- `--ca-file` / `APP_CA_FILE` can pin a custom CA bundle for HTTPS
- public TLS and HTTP/2 remain reverse-proxy concerns; the Nebula service itself stays on the
  current HTTP/1.1 backend profile

Current deploy/operator contract:

- use the shipped `deploy/systemd/` assets as the Linux-first starting point
- install the shipped `deploy/systemd/release-control-plane.tmpfiles.conf` before first start so
  the run/state/log directories exist
- install the shipped `deploy/logrotate/release-control-plane.observe` if you keep the default
  observe log file path
- keep non-secret runtime knobs in `release-control-plane.env`
- prefer mounted token files through `APP_*_TOKEN_FILE`
- treat those token files as startup-only snapshots for the current process lifetime
- terminate public TLS at the reverse proxy; the shipped `deploy/caddy/Caddyfile` and
  `deploy/nginx/release-control-plane.conf` are the narrow reference shapes for this
- for containerized handoff, use `deploy/container/Dockerfile.runtime`
- for Kubernetes handoff, start from `deploy/k8s/` and keep the current single-replica SQLite +
  mounted-secret contract explicit

The current Kubernetes handoff is intentionally the narrow single-replica SQLite + mounted-secret
contract for this forcing app, not a broader scalable control-plane claim.
