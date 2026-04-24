# CLI + Backend Service Workspace

Linux-first Nebula workspace skeleton that combines:

- a backend service under `apps/service`
- an operator/developer CLI under `apps/ctl`
- a shared contract/validation core under `packages/core`

This wave grows the example from `info/echo` into an authenticated config control plane for non-secret
configuration entries keyed by:

- `project`
- `environment`
- `key`

This example stays inside Nebula's current strongest lane:

- CLI/tooling
- Linux backend SDK service
- shared pure logic and JSON contracts
- authenticated operator CLI over HTTP or HTTPS
- SQLite-backed persistence through an app-specific host seam

It does not add:

- an external database service; persistence stays embedded SQLite behind an app-specific host seam
- a generalized auth platform such as user accounts, sessions, OAuth/OIDC, or policy DSLs
- queues/background jobs
- native edge TLS / HTTP2 / ALPN on the Nebula service itself
- hard cancellation guarantees
- GUI or thin-host UI

The config catalog remains explicit about product boundaries:

- persistence is stored in `APP_STATE_DIR/catalog.db`
- values are `string | bool | int`
- config is non-secret only
- item entries expose a monotonic `revision_id` for optimistic concurrency
- public transport is still expected behind a reverse proxy
- HTTPS is for the CLI-to-proxy hop; the Nebula service itself still speaks HTTP/1.1 upstream
- edge TLS / HTTP/2 remain proxy-owned, not native service features
- service startup now preflights auth config and catalog DB reachability before binding

Fetch the shared workspace lock first:

```bash
./build/nebula fetch examples/cli_service_workspace
```

The workspace root defaults to the CLI member, so this is a quick sanity check:

```bash
./build/nebula run examples/cli_service_workspace --run-gate none -- config-validate
```

Build and run the service:

```bash
./build/nebula build examples/cli_service_workspace/apps/service --out-dir .service-out

APP_STATE_DIR=./tmp/cli-service-state \
APP_AUTH_REQUIRED=1 \
APP_READ_TOKEN=reader-token \
APP_WRITE_TOKEN=writer-token \
APP_ADMIN_TOKEN=admin-token \
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=40480 \
NEBULA_PANIC_TO_500=1 \
  ./.service-out/main.out
```

Startup preflight contract:

- when `APP_AUTH_REQUIRED=1`, `APP_READ_TOKEN`, `APP_WRITE_TOKEN`, and `APP_ADMIN_TOKEN` must all
  be present and pairwise distinct
- `APP_AUTH_REQUIRED` must be an explicit boolean-like value: `true/false`, `1/0`, `yes/no`,
  `on/off`
- the service warms the SQLite catalog store before binding; missing `sqlite3` runtime support or an
  unusable `APP_STATE_DIR` now fails fast with a non-zero exit code instead of surfacing only on the
  first config request
- the embedded catalog store is opened with a SQLite busy timeout plus WAL-oriented pragmas so the
  operator path does not start from the most contention-prone defaults

In another shell, run the CLI:

```bash
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- status
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- info
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- ping hello
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- config-validate
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- auth whoami --url http://127.0.0.1:40480 --token admin-token
```

Config catalog commands:

```bash
./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config put --project demo --env prod --key feature.flag --bool true --description "feature gate" --json

./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config get --project demo --env prod --key feature.flag

./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config history --project demo --env prod --key feature.flag

./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config list --project demo --env prod

./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config put --project demo --env prod --key feature.flag --bool false --description "feature gate updated" --if-match 1

./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  config delete --project demo --env prod --key feature.flag --if-match 2
```

Current optimistic concurrency contract:

- create may omit `--if-match`
- once an entry exists, `config put` without `--if-match` is rejected with `428 config_precondition_required`
- stale `--if-match` values are rejected with `412 config_revision_mismatch`
- `config delete` requires `--if-match <revision_id>` in the CLI
- successful writes advance `entry.revision_id`; successful deletes return `deleted_revision_id`
- the raw HTTP layer uses canonical quoted validators such as `If-Match: \"1\"` for conditional
  `PUT` and `DELETE`
- successful `GET /v1/configs/...` and `PUT /v1/configs/...` replies also emit
  `ETag: \"<revision_id>\"`
- the CLI validates that the response `ETag` matches `entry.revision_id` instead of trusting only
  the JSON body

Override the target URL when needed:

```bash
APP_URL=http://127.0.0.1:40480 \
  ./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- status --json
```

Override auth and CA settings when needed:

```bash
APP_TOKEN=admin-token \
APP_CA_FILE=/path/to/ca.pem \
  ./build/nebula run examples/cli_service_workspace/apps/ctl --run-gate none -- \
  auth whoami --url https://proxy.example.internal
```

Stored state is persisted in the SQLite database file:

```text
APP_STATE_DIR/catalog.db
```

Current HTTP surface:

- `GET /healthz`
- `GET /readyz`
- `GET /v1/info`
- `POST /v1/echo`
- `GET /v1/auth/whoami`
- `GET /v1/configs/:project/:environment`
- `GET /v1/configs/:project/:environment/:key`
- `GET /v1/configs/:project/:environment/:key/history`
- `PUT /v1/configs/:project/:environment/:key`
- `DELETE /v1/configs/:project/:environment/:key`

Current config mutation semantics:

- `PUT` on a missing entry creates it and assigns `revision_id = 1`
- `PUT` on an existing entry requires `If-Match` with the current `revision_id`
- `DELETE` on an existing entry requires `If-Match` with the current `revision_id`
- `GET` on a missing entry returns `404`
- stale writers/deleters fail closed instead of silently overwriting newer state

Current auth model:

- disabled unless `APP_AUTH_REQUIRED=1`
- roles are `reader`, `writer`, `admin`
- tokens are loaded from `APP_READ_TOKEN`, `APP_WRITE_TOKEN`, `APP_ADMIN_TOKEN`
- when auth is enabled those three tokens must all be set and must not reuse the same value
- reads require `reader`
- writes require `writer`
- deletes require `admin`
- successful auth now flows through the official `service::middleware::authorize_result(...)`
  principal carrier instead of an app-local auth shuttle struct

Current panic contract:

- `NEBULA_PANIC_TO_500=1` enables request-boundary recovery for user-code `panic(...)`
- recovered panics emit `handler_panic` observe events and `handler_panics` metrics
- runtime/internal invariant panics are still not treated as a generic recoverable app path

Current transport contract:

- CLI base URLs may now use `http://` or `https://`
- `--ca-file` / `APP_CA_FILE` can pin a custom CA bundle for HTTPS
- public TLS and HTTP/2 remain reverse-proxy concerns; the Nebula service itself stays on the
  current HTTP/1.1 backend profile
