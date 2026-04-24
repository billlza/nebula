# Hello API

Repo-local preview service example built on `official/nebula-service`.

For the recommended installed Linux backend GA starter, use `nebula new --template backend-service`.

Run it with:

```bash
nebula run . --run-gate none
```

Override bind settings with the preview service env vars:

```bash
NEBULA_BIND_HOST=127.0.0.1 NEBULA_PORT=8080 nebula run . --run-gate none
```

Enter draining mode with a sentinel file:

```bash
touch /tmp/hello-api.draining
NEBULA_DRAIN_FILE=/tmp/hello-api.draining nebula run . --run-gate none
```

While draining:

- `/healthz` stays `200`
- `/readyz` flips to `503`
- business routes return a JSON draining error
- the server exits after a quiet poll interval once no new requests arrive

This example now uses the service SDK's explicit route-composition and middleware helpers:

- `service::routing::dispatch_ctx3_result(...)` for health/ready/business routing
- `service::middleware::reject_when_draining_result(...)` to gate business traffic while draining

Connection lifecycle stays intentionally narrow in this wave: each accepted connection handles one
request/response and is then closed. Keep-alive remains out of scope.

Request-id policy defaults to trusting inbound `X-Request-Id` for request context, JSON framework
errors, and structured logs, but only when the configured header appears exactly once. Duplicate
values fall back to the locally generated request id. Override trust entirely with:

```bash
NEBULA_TRUST_REQUEST_ID_HEADER=false nebula run . --run-gate none
```

Trigger graceful shutdown without a drain/readiness phase with:

```bash
touch /tmp/hello-api.shutdown
NEBULA_SHUTDOWN_FILE=/tmp/hello-api.shutdown nebula run . --run-gate none
```

Unlike the drain sentinel, shutdown stops before the next accept and exits once any in-flight work
finishes.

Collector-side `/metrics` bridge:

```bash
nebula run . --run-gate none 2> hello-api.observe.ndjson
python3 ../../official/nebula-observe/prometheus_bridge.py serve --input hello-api.observe.ndjson --port 9464
curl http://127.0.0.1:9464/metrics
```

This helper is intentionally narrow: it translates logged `nebula.observe.metric.v1`
delta-counter events into Prometheus text and does not turn the example itself into a built-in
Prometheus exporter.
