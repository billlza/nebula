# Reverse Proxy Deployment

Nebula 1.0 backend services are expected to sit behind a reverse proxy such as Caddy, Nginx, or a
service mesh ingress.

Recommended deployment pattern:

1. Terminate public TLS at the proxy.
2. Forward plain HTTP/1.1 to the Nebula service on a private interface.
3. Preserve request size/time budgets at both layers.
4. Route `/healthz` and `/readyz` to the Nebula service for liveness/readiness.
5. Use Nebula for outbound TLS and PQC application logic, not for edge termination.

## Connection Behavior Today

Nebula's default backend service profile is still close-after-response on the upstream hop:

- the reverse proxy may keep client-facing connections open
- the Nebula service handles one request per accepted connection by default
- the Nebula service then closes that upstream connection after the response

An explicit opt-in keep-alive contract is now available for services that enable it through
`nebula-service` config:

- scope is HTTP/1.1 sequential reuse only
- pipelining remains unsupported
- reuse stops on client `Connection: close`, idle timeout, configured per-connection request
  limits, or service drain/shutdown transitions observed at request boundaries

Proxy guidance today:

- forward HTTP/1.1 to Nebula
- keep proxy and Nebula timeout budgets aligned
- if you do not opt into keep-alive, size proxy/backend connection churn accordingly
- if you do opt into keep-alive, keep the upstream idle timeout conservative so drain/shutdown
  latency stays bounded

## Failure Model Today

Nebula services can return JSON `500` responses for recoverable request failures. They can also
recover user-code request-handler panics at the request boundary when panic-to-`500` is explicitly
enabled, but this is not global process-wide panic recovery.

That means:

- handler `Err(...)` paths can become JSON `500`
- auth verifier internal failures can become JSON `500`
- opt-in user-code request-handler panics can become correlated JSON `500`
- runtime/internal invariant panics still abort the process

Deploy Nebula behind a supervisor that restarts on failure.

## Operator Guide

For config layering, secrets handling, and example supervisor layout, see:

- `docs/backend_operator_guide.md`

For the current release-control-plane forcing app, the scaffolded deploy assets now ship both:

- `deploy/caddy/Caddyfile`
- `deploy/nginx/release-control-plane.conf`

The same sample also ships:

- `deploy/container/Dockerfile.runtime`
- `deploy/k8s/`

Those container/Kubernetes assets intentionally stop before ingress/controller choice; public
TLS/inbound proxy policy remains operator-owned.

For the current sample, a narrow ingress-nginx example is also shipped under:

- `deploy/k8s/ingress-nginx.yaml`

That asset remains just an example for the current forcing app; it is not a claim that Nebula now
ships a full Kubernetes ingress/deployment framework.

Why this profile is the default:

- it matches the current `nebula-tls` client-only scope
- it keeps inbound transport concerns out of the backend SDK GA claim
- it gives backend adopters a realistic path to production deployment without overstating platform breadth
