# Backend Developer Guide

This guide is for developers building Nebula backend services, not operators deploying them.

It focuses on the places where real projects tend to drift in non-ideal conditions: timeouts,
panic usage, local smoke, request correlation, keep-alive assumptions, and observe wiring.

See also:

- `docs/backend_operator_guide.md`
- `docs/service_profile.md`
- `docs/reverse_proxy_deployment.md`
- `official/nebula-service/README.md`

## Recommended Entry Shape

Prefer this entrypoint shape for real apps and examples:

1. `config_from_env(service_name, default_port)`
2. `from_env()` for drain/shutdown config when using long-lived services
3. `bind_listener(cfg)`
4. `serve_requests_result_until_drained(...)` or another explicit `service::server` entrypoint

Why:

- operators and tests can override bind host, port, limits, and timeouts without patching source
- request-id policy stays in one place
- drain/shutdown behavior stays aligned with docs and smoke

## Developer Pitfalls

### 1. Do not use `panic(...)` for ordinary request failures

Even with opt-in panic-to-`500`, `panic(...)` is still not the normal request error path.

Use:

- `Result<Response, String>` handlers for recoverable request failures
- explicit `401` / `403` / `400` / `500` response construction for expected application behavior

Do not use:

- `panic(...)` as a substitute for a request-local `500`

If a request-level failure should stay inside the HTTP lifecycle, it should still return through
`Response`/`Result<Response, String>` rather than depending on panic recovery.

Current panic-to-`500` scope:

- enable it explicitly with `NEBULA_PANIC_TO_500=1` or `with_panic_to_500(...)`
- it only recovers user-code handler panics at the request boundary
- it does not make runtime/internal invariant panics a generic recoverable path

### 2. `timeout(...)` is a response deadline, not cancellation

Nebula's current timeout behavior is a deadline on the framework-facing result, not a hard cancel
of the underlying async work.

Implications:

- a timed-out handler may still continue running after the framework has already sent a timeout
  response and closed the connection
- long-running side effects should therefore be idempotent, externally owned, or explicitly
  spawned/managed
- do not assume that a timeout means the work definitely stopped

Practical guidance:

- keep request handlers small and bounded
- move durable/background work behind explicit ownership boundaries
- avoid mixing timeout-wrapped request handlers with hidden side effects you cannot tolerate
  completing late

### 3. `stderr` is the observe channel

The current service/observe story is log-first:

- structured observe events are emitted through `std::log`
- service lifecycle events, request events, and delta counters all flow through that channel

Practical guidance:

- in production or smoke, redirect `stderr` to the observe sink
- do not assume `stdout` carries the operational signal
- avoid writing ad-hoc human text to `stderr` in production paths if you expect collectors/bridges
  to consume it

### 4. Binding to port `0` is valid, but you must consume the real bound address

Tests and local-smoke apps often bind to port `0` to get an ephemeral port.

Practical guidance:

- if you bind with port `0`, consume `listener.local_addr()` when your app/test needs the actual
  socket
- the `listener_bound` lifecycle event now records the real bound host/port when available, so
  local-smoke harnesses can also discover it from the observe stream

Do not assume:

- the configured port in source is the actual bound port when using `0`

### 5. Builder helpers are not all semantically neutral

The current service config helpers are intentionally small, but they are not all independent knobs.

Examples:

- `with_limits(...)` also updates `request_timeout_ms`
- keep-alive only takes effect once you enable it explicitly with a per-connection request limit
  greater than `1`

Practical guidance:

- treat config builders as contractful transforms, not cosmetic setters
- if you are changing limits and timeouts, read the helper names literally and set both layers
  deliberately
- prefer `config_from_env(...)` in app entrypoints so production/test override behavior stays
  obvious

### 6. Keep-alive is intentionally narrow

Keep-alive is now available, but only as an explicit, minimal contract.

What it is:

- opt-in only
- HTTP/1.1 sequential reuse only
- request-at-a-time reuse on a single socket

What it is not:

- no pipelining
- no hidden ALPN switching or broad HTTP/2 platform claim
- no broad connection pool / multiplexing story

### 6.5. Internal TLS is a thin adapter, not a new service framework

If you need a private east-west TLS hop, prefer the preview `service::tls` adapter over
rebuilding handler loops yourself.

Current shape:

- `official/nebula-tls` owns outbound/client trust + identity policy
- `official/nebula-tls-server` owns inbound listener/server stream transport
- `service::tls` lets `nebula-service` handlers run on a TLS listener while preserving the current
  narrow request/read, handler-timeout, write-timeout, keep-alive, and drain semantics
- verified mTLS handlers can read richer peer identity from `RequestContext`, including
  `tls_peer_fingerprint_sha256()` and normalized SAN claims through `tls_peer_san_claims()`
  without hand-parsing certificates in app code
- `mtls_principal(...)` keeps `Principal.subject` on the existing subject string, while richer
  peer identity lives in `Principal.claims` for explicit policy checks

Current boundary:

- internal/private TLS hops only
- HTTP/1.1 remains the primary/stable service path
- preview `h2` service handling is now available for internal hops when both sides explicitly
  negotiate `h2`
- no broad multiplexing or edge-runtime HTTP/2 claim
- no public edge-TLS claim

Practical guidance:

- leave keep-alive off unless you actually benefit from it
- if you enable it, keep idle timeouts conservative
- if your client/proxy depends on pipelining or advanced upstream reuse semantics, that is outside
  the current contract

### 7. Drain and shutdown are observed at request boundaries

Current graceful behavior is intentionally simple:

- active work is allowed to finish
- readiness flips during drain
- the service stops reading new work once the drain/shutdown transition is observed at a safe
  request boundary

Implications:

- do not assume an in-flight request will be preempted
- keep request handlers bounded so quiesce latency stays acceptable
- if keep-alive is enabled, large idle windows will directly hurt graceful-stop responsiveness

### 8. Request correlation lives in context, payloads, and logs

Today Nebula's request correlation contract is:

- `RequestContext`
- JSON error payloads
- observe log events / metrics

Current observe events also carry a stable machine classification when the service/runtime knows
it:

- `classification.reason`
- `classification.detail`

If you want dashboards, bridges, or collectors to stay coherent, prefer reusing that vocabulary
instead of inventing ad-hoc event-name forks for the same incident class.

Do not assume:

- `X-Request-Id` is automatically injected into response headers

If your API contract requires response-header propagation, do that deliberately in app code or at
the proxy boundary.

### 9. The sample observe bridge is a bridge, not a stateful metrics daemon

`prometheus_bridge.py` is intentionally small and sidecar-friendly.

That means:

- it rereads the observe log file when rendering/scraping
- append-only long-lived files will keep historical counters in view
- restart-to-same-file and ad-hoc log reuse can skew what a developer thinks is “current”

Practical guidance:

- prefer a fresh observe file per run/smoke when validating local behavior
- be deliberate about rotation and retention if you keep using the sample bridge outside smoke
- treat it as a collector-side helper, not as a full in-process metrics lifecycle

## Coding Style That Ages Well

Prefer:

- thin request adapters
- pure or near-pure state/business logic under the adapter
- explicit route handlers with `service::routing`
- explicit named middleware with `service::middleware`
- explicit config loading near the process boundary

Avoid:

- hidden process-global mutable state in request handlers
- request handlers that both mutate state and own long-running background work without a clear
  boundary
- implicit transport assumptions that are not stated in `service_profile.md`

## Good Default Checklist

- start from `config_from_env(...)`
- use `serve_requests_result_until_drained(...)` for long-lived services
- return `Err(msg)` / `Response`, not `panic(...)`, for recoverable request failures
- keep `stderr` wired to the observe sink
- leave keep-alive off until you have a measured reason to enable it
- keep handler side effects bounded and timeout-safe
