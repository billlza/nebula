# `official/nebula-tls-server`

`official/nebula-tls-server` exposes Nebula's preview inbound TLS transport surface for
Linux/macOS development and internal east-west service links.

Current scope:

- server-side TLS listener and accepted stream support
- TLS 1.2 / TLS 1.3 version policy via `tls::client::*` policy types
- explicit client-auth policy for mTLS:
  - `Disabled`
  - `Optional(trust_store)`
  - `Required(trust_store)`
- HTTP/1.1 over TLS helpers for request/response handling
- narrow preview HTTP/2 over TLS helpers for request/response handling
- peer certificate presence / verification / subject metadata
- richer peer identity metadata for verified mTLS peers, including SHA-256 certificate
  fingerprint and normalized SAN claims (`dns_names`, `ip_addresses`, `email_addresses`, `uris`)
- explicit ALPN policy wiring, including preview `h2` transport helpers
- thin `service::tls` adapter integration for internal service handlers on top of `nebula-service`

Out of scope for this preview package:

- public internet edge-TLS positioning
- hidden ALPN-driven protocol switching
- broad HTTP/2 multiplexing / push / dynamic-table claims
- native deployment framework or full `nebula-service` rewrite

Typical composition:

- `official/nebula-tls` owns outbound/client transport plus trust and identity policy types
- `official/nebula-tls-server` owns inbound listener/server stream types
- `official/nebula-service` now consumes this package through the preview `service::tls` adapter
  seam rather than embedding certificate parsing or TLS lifecycle policy directly into
  `service::*`

Current positioning:

- internal east-west TLS / mTLS first
- no public edge-TLS claim
- HTTP/2 is now a narrow preview transport path, not a general edge/platform claim
- ALPN policy shape remains explicit and visible to application code
- peer identity metadata is transport-owned and normalized before it reaches `service::tls`;
  application code does not need to parse X.509 SANs by hand just to inspect common internal
  identity claims
