# nebula-service

Official Nebula backend SDK package for Linux backend service helpers on top of `std::http`.

Current surface in this repo wave:

- `service::config`
  - `default_config(name, port) -> ServiceConfig`
  - `config(name, bind_host, port, max_header_bytes, max_body_bytes, request_timeout_ms)`
  - `with_bind(base, bind_host, port) -> ServiceConfig`
  - `with_limits(base, max_header_bytes, max_body_bytes, request_timeout_ms) -> ServiceConfig`
  - `with_timeouts(base, request_timeout_ms, handler_timeout_ms, write_timeout_ms) -> ServiceConfig`
  - `with_keep_alive(base, idle_timeout_ms, max_requests_per_connection) -> ServiceConfig`
  - `with_panic_to_500(base, enabled) -> ServiceConfig`
  - `with_request_header_policy(base, header_policy) -> ServiceConfig`
  - `config_from_env(name, default_port) -> Result<ServiceConfig, String>`
- `service::context`
  - `RequestContext`
  - `RequestContext_as_json(self) -> Json`
  - `RequestContext_is_draining(self) -> Bool`
  - `RequestContext_has_principal(self) -> Bool`
  - `RequestContext_principal(self) -> Result<Principal, String>`
  - `RequestContext_tls_peer_present(self) -> Bool`
  - `RequestContext_tls_peer_verified(self) -> Bool`
  - `RequestContext_tls_peer_subject(self) -> Result<String, String>`
  - `RequestContext_tls_peer_fingerprint_sha256(self) -> Result<String, String>`
  - `RequestContext_tls_peer_san_claims(self) -> Result<Json, String>`
  - `RequestContext_transport_debug(self) -> Json`
  - `RequestContext_tls_peer_identity_debug(self) -> Json`
  - `RequestContext_http2_debug_state(self) -> Json`
  - `Principal`
  - `principal(subject, claims) -> Principal`
- `service::drain`
  - `DrainConfig`
  - `disabled() -> DrainConfig`
  - `sentinel_file(path) -> DrainConfig`
  - `shutdown_file(path) -> DrainConfig`
  - `with_poll_interval(base, poll_interval_ms) -> DrainConfig`
  - `with_shutdown_file(base, shutdown_path) -> DrainConfig`
  - `DrainConfig_enabled(self) -> Bool`
  - `requested(self) -> Result<Bool, String>`
  - `shutdown_requested(self) -> Result<Bool, String>`
  - `from_env() -> Result<DrainConfig, String>`
- `service::errors`
  - `json_error(status, service_name, request_id, request_id_text, code, message) -> Response`
  - `for_request(status, ctx, code, message) -> Response`
  - `bad_request(service_name, request_id, message) -> Response`
  - `request_timeout(service_name, request_id) -> Response`
  - `handler_timeout(ctx) -> Response`
  - `internal_error(ctx) -> Response`
  - `draining(ctx) -> Response`
- `service::headers`
  - `RequestHeaderPolicy`
  - `RequestIdentity`
  - `default_request_header_policy() -> RequestHeaderPolicy`
  - `local_request_id_only() -> RequestHeaderPolicy`
  - `with_request_id_header(base, header_name) -> RequestHeaderPolicy`
  - `with_trusted_request_id(base, trust) -> RequestHeaderPolicy`
  - `with_request_id_max_bytes(base, max_request_id_bytes) -> RequestHeaderPolicy`
  - `RequestHeaderPolicy_as_json(self) -> Json`
  - `resolve_request_identity(self, request, local_request_id) -> RequestIdentity`
- `service::health`
  - `live_response(service_name) -> Response`
  - `ready_response(service_name) -> Response`
  - `draining_response(service_name) -> Response`
  - `live_response_with_context(ctx) -> Response`
  - `ready_response_with_context(ctx) -> Response`
  - `draining_response_with_context(ctx) -> Response`
- `service::middleware`
  - `AuthDecision`
  - `allow() -> AuthDecision`
  - `allow_principal(value) -> AuthDecision`
  - `deny(response) -> AuthDecision`
  - `reject(condition, denied, ctx, request, next) -> Future<Response>`
  - `reject_result(condition, denied, ctx, request, next) -> Future<Result<Response, String>>`
  - `reject_when_draining(ctx, request, next) -> Future<Response>`
  - `reject_when_draining_result(ctx, request, next) -> Future<Result<Response, String>>`
  - `authorize(ctx, request, verifier, next) -> Future<Response>`
  - `authorize_result(ctx, request, verifier, next) -> Future<Result<Response, String>>`
- `service::routing`
  - `dispatch_ctx1(...) -> Future<Response>`
  - `dispatch_ctx1_result(...) -> Future<Result<Response, String>>`
  - `dispatch_ctx2(...) -> Future<Response>`
  - `dispatch_ctx2_result(...) -> Future<Result<Response, String>>`
  - `dispatch_ctx3(...) -> Future<Response>`
  - `dispatch_ctx3_result(...) -> Future<Result<Response, String>>`
  - `dispatch_ctx4(...) -> Future<Response>`
  - `dispatch_ctx4_result(...) -> Future<Result<Response, String>>`
- `service::server`
  - `bind_listener(cfg) -> Future<Result<TcpListener, String>>`
  - `serve_request_once(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_request_once_result(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_request_once_with_context(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_request_once_result_with_context(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_n(listener, cfg, limit, handler) -> Future<Result<Int, String>>`
  - `serve_requests_n_result(listener, cfg, limit, handler) -> Future<Result<Int, String>>`
  - `serve_requests_n_with_context(listener, cfg, limit, handler) -> Future<Result<Int, String>>`
  - `serve_requests_n_result_with_context(listener, cfg, limit, handler) -> Future<Result<Int, String>>`
  - `serve_requests(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_result(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_with_context(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_result_with_context(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_until_drained(listener, cfg, drain_cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_result_until_drained(listener, cfg, drain_cfg, handler) -> Future<Result<Int, String>>`
- `service::tls`
  - `bind_tls_listener(cfg, tls_cfg) -> Future<Result<TlsListener, String>>`
  - `serve_requests_n_result_with_context_tls(listener, cfg, limit, handler) -> Future<Result<Int, String>>`
  - `serve_requests_result_with_context_tls(listener, cfg, handler) -> Future<Result<Int, String>>`
  - `serve_requests_result_until_drained_tls(listener, cfg, drain_cfg, handler) -> Future<Result<Int, String>>`
  - `mtls_principal(ctx) -> Result<Principal, String>`
  - `verify_mtls_principal(ctx, request) -> Future<Result<AuthDecision, String>>`
  - `authorize_mtls(ctx, request, next) -> Future<Response>`
  - `authorize_mtls_result(ctx, request, next) -> Future<Result<Response, String>>`

Transport debug notes:

- `RequestContext_transport_debug()` carries the same structured TLS/HTTP2 debug snapshot that
  service observe/error paths use.
- `RequestContext_http2_debug_state()` now includes both low-level frame events and higher-level
  semantic timeline events such as `stream_open`, `response_headers_seen`, `stream_closed`, and
  `goaway_observed` when that state is available.
  - semantic phase events also carry a stable `classification.reason` /
    `classification.detail` pair so logs, operator tooling, and hover/explain output can reuse
    the same transport incident vocabulary.
  - TLS request/lifecycle observe events now lift the latest available transport phase
    classification into the top-level log event `classification` field when that signal exists.

Recommended installed backend SDK dependency shape:

```toml
[dependencies]
service = { installed = "nebula-service" }
```

Repo-checkout dependency shape:

```toml
[dependencies]
service = { path = "/path/to/nebula/official/nebula-service" }
```

Release posture:

- Linux backend GA surface
- distributed in the optional Linux backend SDK asset
- not installed by default core CLI/tooling archives

Current guarantees for this package revision:

- explicit service config with env-driven host/port/limits/timeouts override
- request-scoped context for handlers, including numeric local `request_id`, propagated `request_id_text`, and drain state
- TLS-backed service handlers may also read minimal peer metadata from `RequestContext`
  (`tls_peer_present`, `tls_peer_verified`, `tls_peer_subject`,
  `tls_peer_fingerprint_sha256`, `tls_peer_san_claims`) when they run through `service::tls`
- request header policy for inbound request-id trust, header naming, and size bounds
- trusted request-id propagation only when the configured inbound header appears exactly once; duplicate values fall back to the locally generated request id
- bounded request header/body reads
- bounded request-read, handler, and response-write stages
- request timeout wrapping with HEAD-safe JSON timeout replies
  - current timeout behavior is a framework-facing response deadline, not a hard cancellation
    guarantee for underlying async work
- bounded handler error recovery for `Result<Response, String>` handlers with generic JSON `500` replies
- explicit route-composition helpers for context-aware handlers through `service::routing`
- explicit named-function middleware/interceptor helpers through `service::middleware`
- explicit auth/verifier hooks through `service::middleware::authorize(_result)` where a named
  verifier can return `Allow`, `AllowPrincipal(principal)`, or `Deny(response)` without
  introducing a framework registry
  - suitable for narrow header-based auth or application-layer signed-body verification in app
    code; the package does not add a scheme registry, session store, or policy DSL
  - successful verifiers may attach a normalized request principal/claims carrier to
    `RequestContext` for downstream handlers without resorting to app-local auth shuttle structs
  - verifier failures on both `authorize(...)` and `authorize_result(...)` are treated as
    internal verifier failures: they return a generic JSON `500` and emit
    `auth_verifier_error` / `auth_verifier_errors` observability events
    - the `500` reply remains request-correlated through the existing `request_id` /
      `request_id_text` error envelope fields
    - this path is for internal verifier failures only; expected client auth failures should
      return `Deny(response)` with an explicit `401`/`403`/`400`
- drain-file based draining mode that keeps health alive, flips readiness to `503`, and lets business handlers reject while the server waits for an idle poll window
- shutdown-file based graceful stop that exits before the next accept once in-flight work finishes
- JSON health/readiness payloads
- structured request logs and counter-style metrics through `nebula-observe`
- request-correlated error logs through `observe::log::error_event_request(...)`
- service-internal counters are emitted as `nebula.observe.metric.v1` delta counter events on
  the log stream rather than through an in-process exporter
  - current emitted metric names: `bad_requests`, `request_timeouts`, `handler_errors`,
    `handler_panics`, `handler_timeouts`, `write_timeouts`, `auth_verifier_errors`
  - current contract is collector-side bridge friendly for Prometheus/OpenTelemetry, but the
    package still does not expose a built-in scrape endpoint or OTLP exporter
- the backend SDK ships a narrow sample sidecar bridge beside `nebula-observe`
  - redirect service stderr to a log file such as `service.observe.ndjson`
  - `prometheus_bridge.py render --input service.observe.ndjson` renders a point-in-time text
    exposition snapshot for the accumulated delta counters in that file
  - run `prometheus_bridge.py serve --input service.observe.ndjson`
  - scrape the helper's `/metrics` endpoint instead of the Nebula service process directly
- lifecycle-style observe events are emitted for listener/service transitions
  - current lifecycle event names: `listener_bound`, `drain_requested`, `shutdown_requested`,
    `listener_stopped`
  - request and lifecycle events now include a stable `classification = { reason, detail }`
    object for operator/collector consumption
- preview internal TLS listener integration now exists through `service::tls`
  - same narrow request/read, handler, write, keep-alive, and drain/shutdown boundaries as `service::server`
  - default HTTP/1.1 sequential reuse still remains the primary/most-mature path
  - preview `h2` service handling now exists when ALPN negotiates `h2`
    - scope stays intentionally narrow: sequential request handling only, no multiplexing claim
  - verified mTLS request handlers can lift richer peer identity into `Principal.claims`
    through `mtls_principal(...)`, including the leaf certificate SHA-256 fingerprint and
    normalized SAN claims
  - same request/read, handler, write, keep-alive, and drain/shutdown boundaries
  - intended for internal east-west TLS hops, not for public edge ingress
- default accepted service connections are still handled as one request / one response / one close
  lifecycles
- explicit keep-alive is now available as an opt-in service contract through `with_keep_alive(...)`
  or `config_from_env(...)`
  - scope is intentionally narrow: HTTP/1.1 sequential reuse only, no pipelining, no HTTP/2, no
    shared connection pool contract
  - reuse stops on client `Connection: close`, keep-alive idle timeout, configured per-connection
    request limit, bad-request / request-timeout / write-timeout paths, or drain / shutdown
    transitions observed at request boundaries
- opt-in panic-to-`500` is available through `with_panic_to_500(...)` or `NEBULA_PANIC_TO_500`
  - scope is intentionally narrow: user-code request handler panics are recovered at the
    request boundary and returned as correlated JSON `500` replies
  - recovered panics emit `handler_panic` observe events and `handler_panics` delta counters
  - internal runtime/invariant panics are still not a generic recoverable app contract
- operator-facing deployment, keep-alive, panic, config, and secrets guidance lives in:
  - `docs/backend_operator_guide.md`
  - `docs/reverse_proxy_deployment.md`
- developer-facing service guardrails live in:
  - `docs/backend_developer_guide.md`

Supported environment overrides for `config_from_env(...)`:

- `NEBULA_BIND_HOST`
- `NEBULA_PORT`
- `NEBULA_MAX_HEADER_BYTES`
- `NEBULA_MAX_BODY_BYTES`
- `NEBULA_REQUEST_TIMEOUT_MS`
- `NEBULA_HANDLER_TIMEOUT_MS`
- `NEBULA_WRITE_TIMEOUT_MS`
- `NEBULA_KEEP_ALIVE`
- `NEBULA_KEEP_ALIVE_IDLE_TIMEOUT_MS`
- `NEBULA_KEEP_ALIVE_MAX_REQUESTS`
- `NEBULA_PANIC_TO_500`
- `NEBULA_REQUEST_ID_HEADER`
- `NEBULA_TRUST_REQUEST_ID_HEADER`
- `NEBULA_REQUEST_ID_MAX_BYTES`
- `NEBULA_DRAIN_FILE`
- `NEBULA_SHUTDOWN_FILE`
- `NEBULA_DRAIN_POLL_MS`

Current non-goals for this package revision:

- framework-style middleware registries or closures with captured state
- automatic service-owned response header policy such as implicit `X-Request-Id`
  - app code may still return explicit response headers through narrow `std::http` /
    `std::http_json` helpers; the package does not auto-inject or negotiate them for you
- pipelining or shared/pool connection management beyond the narrow opt-in sequential
  keep-alive contract
- broad HTTP/2 multiplexing / middleware / edge-runtime claims beyond the preview transport path
- public edge TLS termination inside the Nebula process
