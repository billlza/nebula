# Backend Operator Guide

This guide turns Nebula's current backend service profile into an operator-facing deployment
contract.

It is intentionally explicit about what exists today, what is recommended, and what still remains
future work.

## Current Deployment Shape

Nebula backend services are currently intended to run as:

- one service process per instance
- behind a reverse proxy or service mesh
- with public TLS terminated before traffic reaches the Nebula process
- with stderr redirected to a newline-delimited observe log stream

Today the recommended boundary is:

- reverse proxy owns edge TLS, client-facing keep-alive, and public ingress policy
- Nebula owns application HTTP handling, outbound TLS, and application-layer crypto/PQC logic
- internal service-to-service TLS may now be terminated directly by Nebula for private east-west
  hops through the preview `official/nebula-tls-server` / `service::tls` path

That does not change the public ingress recommendation:

- public/client-facing TLS still belongs at the reverse proxy or service mesh boundary
- Nebula-internal TLS should be treated as a private transport hop, not an edge replacement

See also:

- `docs/service_profile.md`
- `docs/reverse_proxy_deployment.md`
- `official/nebula-service/README.md`

## Keep-Alive Contract Today

Default Nebula backend services still use the conservative one-shot contract:

- one accepted connection
- one request
- one response
- then close

There is now a minimal, explicit opt-in keep-alive contract for backend services that enable it
through `with_keep_alive(...)` or the keep-alive env vars.

That contract is intentionally narrow:

- HTTP/1.1 sequential reuse only
- no pipelining
- no broad HTTP/2 / ALPN / multiplexing claim at the backend service layer
- no shared or pooled connection-management story

When keep-alive is enabled, reuse stops on:

- client `Connection: close`
- keep-alive idle timeout
- configured per-connection request limit
- bad-request / request-timeout / write-timeout paths
- drain / shutdown transitions observed at request boundaries

Implications for operators:

- client-facing keep-alive at the reverse proxy is fine
- HTTP/1.1 forwarding to Nebula is fine
- default services still behave as a close-after-response upstream hop
- keep-alive-enabled services should keep a conservative idle timeout budget so quiesce/shutdown
  remains prompt

## Panic And 500 Contract Today

Nebula backend services currently distinguish between:

- recoverable application failures that return `Result<Response, String>`
- verifier failures that return `Err(msg)` from `service::middleware::authorize(_result)`
- runtime/language panics

What operators can rely on today:

- `Result<Response, String>` handler failures are logged and returned as request-correlated JSON
  `500` responses
- auth verifier internal failures are logged and returned as request-correlated JSON `500`
  responses, with `auth_verifier_errors` metrics
- request timeouts, bad requests, and write timeouts emit explicit observe events/metrics

What operators must **not** assume today:

- panic recovery is not global or magical
- only opt-in user-code request-handler panics are converted into correlated JSON `500` replies
  - this is still a narrow in-process JSON `500` contract, not a blanket process-wide recovery story
- panic(...) is not converted into an in-process JSON `500` outside that opt-in request-handler
  recovery boundary
- runtime/internal invariant panics still abort the process

Operational guidance:

- run Nebula services under a supervisor such as `systemd`, Kubernetes, Nomad, or another process
  manager that restarts on failure
- treat unexpected process exit as a crash signal, not as a handled application error
- enable `NEBULA_PANIC_TO_500=1` only when you explicitly want request-boundary panic recovery
- continue treating process exit as a crash signal even when panic-to-`500` is enabled, because
  not all panic classes are recoverable

## Config Layering

Use three distinct config layers:

1. Service-runtime knobs
2. Application config
3. Secrets

Service-runtime knobs are the `nebula-service` environment contract, for example:

- bind host / port
- request size limits
- request / handler / write timeouts
- request-id trust policy
- drain / shutdown file paths

These belong in ordinary operator config such as:

- `Environment=` or `EnvironmentFile=` under `systemd`
- container env configuration
- deployment-system non-secret config maps

Application config should stay separate from service-runtime knobs. Examples:

- downstream API base URLs
- feature flags
- business-level worker or queue settings
- application-specific file paths

## Secrets Guidance

Nebula does not currently provide a built-in secret manager, redaction layer, or protected
service-config store.

Current best practice is:

- prefer mounted files, platform secret stores, or supervisor-managed secret injection over raw
  long-lived environment variables when possible
- keep secrets out of command lines
- never write secrets to stderr/stdout observe streams
- never echo secrets into JSON error payloads

If the application must materialize secret bytes:

- treat file reads / env values as an operator boundary
- keep those values out of logs, traces, and metrics
- prefer narrow load sites over process-wide global secret copies
- when an app provides paired `*_FILE` env vars, prefer those mounted-file hooks over raw secret
  env values
- favor startup-only secret snapshots over hot-reload semantics unless the app explicitly claims
  otherwise

This guidance is consistent with the current crypto contract:

- `official/nebula-crypto/SECURITY_CONTRACT.md`

That contract is explicit that serialized secrets in files, env vars, logs, or network payloads
are outside today's zeroization guarantees.

## Example systemd Layout

One workable deployment shape today is:

```ini
[Unit]
Description=Nebula Backend Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/srv/my-service
EnvironmentFile=/etc/nebula/my-service.env
ExecStart=/usr/bin/bash -lc '/opt/nebula/bin/nebula run . --run-gate none 2>>/var/log/nebula/my-service.observe.ndjson'
Restart=on-failure
RestartSec=2
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

## Release Control Plane Install Steps

For the current `control-plane-workspace` forcing app, the shipped deploy assets now support a
step-by-step Linux handoff instead of leaving operators to assemble the layout manually.

### Host Install Steps

1. Build the service binary from the workspace:

```bash
nebula build apps/service --mode release --out-dir .service-out
```

2. Copy the service workspace onto the host, for example under:

```text
/srv/release-control-plane
```

3. Install the shipped unit/env/tmpfiles/logrotate assets:

```bash
install -D -m 0644 deploy/systemd/release-control-plane.service /etc/systemd/system/release-control-plane.service
install -D -m 0644 deploy/systemd/release-control-plane.env.example /etc/nebula/release-control-plane.env
install -D -m 0644 deploy/systemd/release-control-plane.tmpfiles.conf /etc/tmpfiles.d/release-control-plane.conf
install -D -m 0644 deploy/logrotate/release-control-plane.observe /etc/logrotate.d/release-control-plane.observe
```

4. Create the runtime directories from the shipped `tmpfiles` fragment:

```bash
systemd-tmpfiles --create /etc/tmpfiles.d/release-control-plane.conf
```

5. Put mounted token files under the configured secret directory, for example:

```text
/run/release-control-plane-secrets/read.token
/run/release-control-plane-secrets/write.token
/run/release-control-plane-secrets/admin.token
/run/release-control-plane-secrets/worker.token
```

`worker.token` is optional unless you expose the dedicated worker lease surface to a separate worker
process. `runner apply-once` remains admin-gated for the local compatibility path.

6. Choose and install one reverse-proxy asset:

- host-based Caddy:
  - `deploy/caddy/Caddyfile`
- host-based Nginx:
  - `deploy/nginx/release-control-plane.conf`

For the shipped Nginx asset, the narrow install flow is:

1. copy `deploy/nginx/release-control-plane.conf` into your site config directory
2. edit `server_name`, `ssl_certificate`, and `ssl_certificate_key`
3. verify the Nebula upstream is still `127.0.0.1:40480`
4. run `nginx -t`
5. reload nginx

7. Reload and start the service:

```bash
systemctl daemon-reload
systemctl enable --now release-control-plane.service
```

8. Verify the service and proxy path:

```bash
curl http://127.0.0.1:40480/healthz
curl http://127.0.0.1:40480/readyz
```

For the current secret-file contract, rotation is restart-based:

1. replace the mounted token file contents
2. restart the service
3. let the restarted process take the new startup snapshot

### Container/Kubernetes Starting Point

If you package the sample for containers instead of host `systemd`:

1. build the release binary:

```bash
nebula build apps/service --mode release --out-dir .service-out
```

2. build the runtime image from the shipped asset:

```bash
docker build -f deploy/container/Dockerfile.runtime -t release-control-plane:local .
```

3. on Kubernetes, start from the shipped narrow assets:

- `deploy/k8s/namespace.yaml`
- `deploy/k8s/configmap.yaml`
- `deploy/k8s/secret.example.yaml`
- `deploy/k8s/persistentvolumeclaim.yaml`
- `deploy/k8s/statefulset.yaml`
- `deploy/k8s/service.yaml`
- `deploy/k8s/ingress-nginx.yaml`

4. keep ingress/controller choice explicit and environment-owned; the sample does not ship a
cluster ingress contract because public TLS / ingress policy still belongs to the operator layer

Example `/etc/nebula/my-service.env` for non-secret runtime config:

```bash
NEBULA_BIND_HOST=127.0.0.1
NEBULA_PORT=40480
NEBULA_MAX_HEADER_BYTES=16384
NEBULA_MAX_BODY_BYTES=1048576
NEBULA_REQUEST_TIMEOUT_MS=15000
NEBULA_HANDLER_TIMEOUT_MS=5000
NEBULA_WRITE_TIMEOUT_MS=5000
NEBULA_KEEP_ALIVE=1
NEBULA_KEEP_ALIVE_IDLE_TIMEOUT_MS=1000
NEBULA_KEEP_ALIVE_MAX_REQUESTS=32
NEBULA_PANIC_TO_500=1
NEBULA_DRAIN_FILE=/run/my-service/drain
NEBULA_SHUTDOWN_FILE=/run/my-service/shutdown
```

If the application needs secrets:

- prefer a mounted file such as `/run/secrets/...`; the preview `official/nebula-config` package
  provides the current reusable app-level helper for env/file mutual exclusion, mounted-secret
  reads, and redacted startup diagnostics
- if your platform only supports env-style secret injection, keep it limited to the app-specific
  load site and treat it as an exposure boundary
- for the current release-control-plane forcing app, prefer:
  - `APP_READ_TOKEN_FILE`
  - `APP_WRITE_TOKEN_FILE`
  - `APP_ADMIN_TOKEN_FILE`
  - `APP_WORKER_TOKEN_FILE` when you use `/v1/workers/*`
- the corresponding raw `APP_*_TOKEN` env vars remain available as a fallback, but each raw/file
  pair must stay mutually exclusive
- the current forcing-app secret contract is startup-only: rotate the mounted files and restart the
  process to apply new values
- the current `examples/release_control_plane_workspace` template now ships matching deploy assets
  under:
  - `deploy/systemd/release-control-plane.service`
  - `deploy/systemd/release-control-plane.env.example`
  - `deploy/systemd/release-control-plane.tmpfiles.conf`
  - `deploy/logrotate/release-control-plane.observe`
  - `deploy/caddy/Caddyfile`
  - `deploy/nginx/release-control-plane.conf`
  - `deploy/secrets/README.md`

If you adopt the shipped observe-log path directly, also install:

- the matching `tmpfiles.d` fragment so `/var/log/nebula`, `/var/lib/...`, and `/run/...` exist
- the matching `logrotate` fragment so `release-control-plane.observe.ndjson` does not grow
  unbounded

The sample `logrotate` asset intentionally uses `copytruncate` because the shipped `systemd`
service unit appends directly to the active observe log file.

## Observability Handoff Today

The current service profile is log-first.

For the current release-control-plane assets, there are now two distinct operator shapes:

- host `systemd` layout
  - use the shipped `tmpfiles` and `logrotate` fragments
  - keep the observe stream on the file path referenced by the shipped unit
- container/Kubernetes layout
  - keep observe output on stdout/stderr
  - let the platform logging pipeline collect and rotate logs
  - do not transplant the host `logrotate` fragment into the container

Current workflow/operator contract:

- `release apply` is the writer-facing compatibility trigger over the internal workflow lane
- `runner apply-once` is the admin-facing compatibility trigger for local synchronous execution
- the first-class worker lane is `worker claim/heartbeat/complete`
- if you split that worker lane out of band, give it the dedicated `worker` token instead of
  reusing `admin`

Prometheus path today:

1. redirect stderr to an observe log file
2. run `prometheus_bridge.py serve --input ...`
3. scrape the bridge's `/metrics`

Example scrape-side layout:

```yaml
scrape_configs:
  - job_name: nebula-sidecar
    static_configs:
      - targets: ["127.0.0.1:9464"]
```

Current bridge output includes:

- delta-counter families translated from `nebula.observe.metric.v1`
- `nebula_service_requests_total{service,status}` derived from `request_finished`
- `nebula_service_request_duration_ms_sum`
- `nebula_service_request_duration_ms_count`
- `nebula_service_events_by_classification_total{service,event,reason,detail}` derived from
  classified observe events
- `nebula_service_requests_by_classification_total{service,status,reason,detail}` derived from
  classified `request_finished`
- `nebula_service_request_duration_ms_sum_by_classification`
- `nebula_service_request_duration_ms_count_by_classification`

Bridge lifecycle caveat:

- the sample bridge rereads the input log file when scraping/rendering
- if you append many runs into the same file, the rendered counters will continue to reflect that
  accumulated history
- for production-grade retention/rotation/state handling, use a collector pipeline rather than
  treating the sample bridge as a full metrics daemon

That is enough for useful operator-side PromQL such as:

- request rate: `rate(nebula_service_requests_total[5m])`
- error rate: `sum(rate(nebula_service_requests_total{status=~\"5..\"}[5m])) / sum(rate(nebula_service_requests_total[5m]))`
- mean request latency: `rate(nebula_service_request_duration_ms_sum[5m]) / rate(nebula_service_request_duration_ms_count[5m])`
- classified timeout/error rate:
  `sum(rate(nebula_service_events_by_classification_total{reason=\"request_start\",detail=\"peer_request_ready\"}[5m]))`
- classified request completions:
  `sum(rate(nebula_service_requests_by_classification_total{reason=\"response_complete\",detail=\"response_written\"}[5m]))`
- classified mean request latency:
  `rate(nebula_service_request_duration_ms_sum_by_classification{reason=\"response_complete\",detail=\"response_written\"}[5m]) / rate(nebula_service_request_duration_ms_count_by_classification{reason=\"response_complete\",detail=\"response_written\"}[5m])`

OpenTelemetry path today:

- use an external collector to tail the observe log stream if you want centralized ingestion
- Nebula does not currently emit OTLP directly
- Nebula does not currently expose native tracing spans or an in-process metrics registry

Example OTel Collector filelog recipe:

```yaml
receivers:
  filelog:
    include: ["/var/log/nebula/my-service.observe.ndjson"]
    start_at: beginning
    operators:
      - type: json_parser

processors:
  transform/nebula_classification:
    log_statements:
      - context: log
        statements:
          - set(attributes["nebula.event"], attributes["event"]) where attributes["event"] != nil
          - set(attributes["nebula.classification.reason"], attributes["classification"]["reason"]) where attributes["classification"]["reason"] != nil
          - set(attributes["nebula.classification.detail"], attributes["classification"]["detail"]) where attributes["classification"]["detail"] != nil

exporters:
  otlp:
    endpoint: otel-gateway.internal:4317
    tls:
      insecure: true

service:
  pipelines:
    logs:
      receivers: [filelog]
      processors: [transform/nebula_classification]
      exporters: [otlp]
```

Practical use of that recipe:

- route or filter on `nebula.classification.reason = "shutdown"` for lifecycle transitions
- group on `nebula.classification.reason/detail` instead of only `event`
- keep `event` as a coarse dimension, but treat `classification.reason/detail` as the stable
  machine vocabulary for dashboards, routing, and alerting

This is a collector-side integration story, not a built-in exporter story.

Lifecycle events currently emitted on the observe stream:

- `listener_bound`
- `drain_requested`
- `shutdown_requested`
- `listener_stopped`

## Recommended Operator Checklist

- terminate public TLS before Nebula
- align proxy and Nebula request/time budgets
- assume upstream close-after-response behavior
- supervise the process and restart on failure
- keep non-secret runtime config in ordinary env/config files
- keep secrets out of logs and command lines
- redirect stderr to a durable observe log sink
- use the bridge or collector as the metrics/log handoff layer
