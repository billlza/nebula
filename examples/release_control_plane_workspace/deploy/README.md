# Release Control Plane Deploy Assets

These files are the operator-facing starting point for the current Nebula forcing app.

They intentionally stay narrow and Linux-first:

- `systemd/release-control-plane.service`
  - one-process service unit with stderr redirected to the observe log stream
- `systemd/release-control-plane.env.example`
  - non-secret runtime config plus mounted-secret file paths
- `systemd/release-control-plane.tmpfiles.conf`
  - creates the run/state/log directories expected by the sample deployment layout
- `logrotate/release-control-plane.observe`
  - rotates the observe log file using `copytruncate` because the shipped `systemd` unit appends
    directly to the file
- `caddy/Caddyfile`
  - reverse-proxy example for public ingress with TLS terminated at Caddy
- `nginx/release-control-plane.conf`
  - second reverse-proxy example for teams standardizing on Nginx
- `container/`
  - runtime image handoff that packages a prebuilt `.service-out/main.out`
- `k8s/`
  - narrow Kubernetes StatefulSet/Service/PVC/Secret handoff for the current forcing app
- `operations/`
  - backup, restore, and startup-migration runbook for SQLite default and Postgres preview data
    planes
- `secrets/`
  - placeholder directory for mounted token files; do not commit real secrets

Current secret contract for this app:

- prefer `APP_READ_TOKEN_FILE`
- prefer `APP_WRITE_TOKEN_FILE`
- prefer `APP_ADMIN_TOKEN_FILE`
- prefer `APP_WORKER_TOKEN_FILE` when you expose the worker lease routes to a dedicated worker
- raw `APP_*_TOKEN` values remain fallback-only
- each raw/file pair is mutually exclusive
- secret files are read during startup validation and then treated as a startup snapshot
- changing a mounted token file does not affect a running process; restart the service to pick up
  the new secret value

This is still not a built-in secret manager, hot-reload contract, or distributed deployment
platform. It is the narrow operator-ready asset bundle for the current control-plane slice.

Recommended handoff order:

1. Install the `systemd/` unit and env example.
2. Install the `tmpfiles` fragment so the runtime directories exist before startup.
3. Install the `logrotate` fragment for `release-control-plane.observe.ndjson`.
4. Choose one reverse-proxy asset:
   - `caddy/Caddyfile`
   - `nginx/release-control-plane.conf`
5. Read `operations/README.md` before the first production-like rollout so backup/restore ownership
   is clear before migrations run.
6. If you run in containers instead of host `systemd`, switch to:
   - `container/Dockerfile.runtime`
   - `k8s/*.yaml`
   - `k8s/README.md` for apply order and secret-rotation rollout steps

Current workflow/operator lane:

- `release apply` remains the writer-facing compatibility trigger
- `runner apply-once` remains the admin-facing compatibility trigger for a local synchronous handoff
- the first-class workflow execution surface is now `workflow event submit` + `worker claim/heartbeat/complete`
- if you deploy a dedicated worker outside the service host, mount a distinct `worker.token` and use
  the `worker` role instead of reusing `admin`
