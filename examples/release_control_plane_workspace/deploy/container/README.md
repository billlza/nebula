# Container Assets

This directory is the narrow container handoff for the current release-control-plane sample.

It assumes you build the service binary first:

```bash
nebula build apps/service --mode release --out-dir .service-out
```

Then build the runtime image:

```bash
docker build -f deploy/container/Dockerfile.runtime -t release-control-plane:local .
```

Run it with bind-mounted state and mounted secret files:

```bash
docker run --rm \
  -p 40480:40480 \
  -e NEBULA_BIND_HOST=0.0.0.0 \
  -e NEBULA_PORT=40480 \
  -e APP_DATA_BACKEND=sqlite \
  -e APP_STATE_DIR=/var/lib/release-control-plane \
  -e APP_AUTH_REQUIRED=1 \
  -e APP_READ_TOKEN_FILE=/run/release-control-plane-secrets/read.token \
  -e APP_WRITE_TOKEN_FILE=/run/release-control-plane-secrets/write.token \
  -e APP_ADMIN_TOKEN_FILE=/run/release-control-plane-secrets/admin.token \
  -e APP_WORKER_TOKEN_FILE=/run/release-control-plane-secrets/worker.token \
  -v "$PWD/tmp/state:/var/lib/release-control-plane" \
  -v "$PWD/deploy/secrets:/run/release-control-plane-secrets:ro" \
  release-control-plane:local
```

This is intentionally a runtime-only image shape:

- it copies the prebuilt `.service-out/main.out`
- it installs `libpq5` so the optional Postgres preview data plane can pass the official runtime
  probe when `APP_DATA_BACKEND=postgres`
- it does not claim a full Nebula-in-container build toolchain story
- it keeps the current mounted-secret and reverse-proxy contract unchanged
- it keeps the observe stream on stdout/stderr instead of introducing an in-container logrotate or
  reverse-proxy sidecar
- `worker.token` is optional unless you expose the worker lease surface to a dedicated worker
- for Postgres preview, set `APP_DATA_BACKEND=postgres`, `APP_POSTGRES_PREVIEW=1`, and
  `APP_POSTGRES_DSN` or `APP_POSTGRES_DSN_FILE`; the container still does not run or manage
  Postgres for you

Before production-like use, read:

```text
deploy/operations/README.md
```

That runbook defines the backup/restore handoff for both the default SQLite state directory and the
optional Postgres preview data plane.

If you hand the sample off to Kubernetes, use the companion assets under:

```text
deploy/k8s/
```
