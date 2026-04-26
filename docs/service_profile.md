# Service Profile

Nebula's backend service GA targets production backend services with a deliberately narrow operating
profile.

This profile is intentionally narrower than Nebula's broader compiler/tooling GA. Binary release
assets keep the core compiler/CLI surface separate from the Linux backend SDK: backend packages are
installed only when the Linux backend SDK asset is explicitly selected.

Current profile:

- Production target: Linux x86_64
- Development hosts: macOS arm64/x86_64, Linux x86_64
- Inbound public TLS: terminated by a reverse proxy or service mesh
- Nebula service responsibility: application HTTP handling, outbound TLS, preview internal
  east-west TLS/mTLS hops, crypto/PQC flows
- Recommended runtime shape: single service process with explicit request limits, env-driven service config, and JSON logs
- Installed backend SDK packages `nebula-service` and `nebula-observe` provide request-scoped `RequestContext` values, inbound request-id policy with unique-header trust rules, JSON framework errors, drain-file draining, and shutdown-file graceful stop
- Route composition and middleware are explicit but intentionally narrow: use named-function
  `service::routing` and `service::middleware` helpers rather than a framework-style registry
- Auth/verifier hooks are explicit but intentionally narrow: use named-function
  `service::middleware::authorize(_result)` verifiers that return allow/deny decisions rather than
  a framework-style auth chain
  - on either path, verifier `Err(msg)` is treated as an internal verifier failure:
    clients get the generic JSON `500`, while operators get request-correlated
    `auth_verifier_error` logs plus `auth_verifier_errors` delta counters
- Default connection lifecycle is one request / one response / one close per accepted connection;
  reverse proxies may keep client-side connections open
- Explicit keep-alive is now available as an opt-in `nebula-service` contract for HTTP/1.1
  sequential reuse only
  - no pipelining, HTTP/2, ALPN, or pooled/shared connection story is implied by this contract
  - drain/shutdown transitions are enforced at request boundaries; already-open reused connections
    should therefore keep a small idle timeout budget
- Backend service helpers support bounded `Result<Response, String>` handlers that log the internal error and return a generic JSON `500` reply
- Opt-in panic-to-`500` is available for user-code request-handler panics through
  `with_panic_to_500(...)` or `NEBULA_PANIC_TO_500`; recovered panics are returned as
  request-correlated JSON `500` replies and emitted as observe events/metrics
- Runtime/internal invariant panics are still process failures, so services must continue to run
  under an external supervisor even when request-boundary panic recovery is enabled
- Service-framework counters are emitted as exporter-friendly `nebula.observe.metric.v1` delta
  counter events on the log stream; Prometheus/OTel translation remains a collector concern
  - the installed backend SDK ships `nebula-observe/prometheus_bridge.py` as a narrow sample
    helper that can render those delta counters plus request-finished derived request totals /
    duration counters into Prometheus text or serve a sidecar `/metrics` endpoint from the
    redirected log file
- Operator-facing deployment, config layering, and secrets guidance now lives in
  `docs/backend_operator_guide.md`
- backend-first internal apps may pair the GA service layer with preview app auth, app config,
  embedded state, and jobs/workflow kernel packages, either from a repo checkout or from the
  installed-preview `official/nebula-auth`, `official/nebula-config`, `official/nebula-db-sqlite`,
  and `official/nebula-jobs` payloads shipped inside the opt-in backend SDK asset on Linux x86_64;
  those preview packages still remain outside the installed backend SDK GA contract in this wave
- Preview application examples such as `examples/pqc_secure_service` are intended as repo-local patterns for application-layer secure payloads, not as a promise of full transport/framework maturity
- Preview transport helpers now include `official/nebula-tls-server` plus the thin
  `service::tls` adapter for internal/private TLS listener integration; this does not change the
  public edge-TLS non-goal
  - preview `h2` service-to-service handling now exists for explicit internal ALPN `h2`
    negotiation, but this remains a narrow transport path rather than a broad platform claim

Current non-goals for the profile:

- direct public-edge TLS termination in Nebula itself
- hidden ALPN-driven protocol switching or a broad HTTP/2 edge/platform claim
- broad Windows service-host parity
- full framework-style middleware ecosystems
- experimental quantum-communication simulation packages such as `official/nebula-qcomm-sim`
