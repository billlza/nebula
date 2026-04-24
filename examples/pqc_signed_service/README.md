# pqc_signed_service

Repo-local preview example for application-layer signed request/response payloads on top of
Nebula's HTTP service helpers.

What it demonstrates:

- `pqc::signed` as the current narrow authenticated-body envelope contract
- a service-oriented JSON shape that can be layered on plain HTTP behind a reverse proxy today
- a smallest-possible bridge point between the backend service helpers and the preview PQC package
  line

Current routes:

- `GET /healthz`
- `POST /sign`
  - accepts an arbitrary request body
  - returns a JSON envelope with:
    - `algorithm`
    - `public_key_hex`
    - `body_sha3_256`
    - `signature_hex`
- `POST /verify`
  - accepts a flat JSON body with:
    - `body_text`
    - `public_key_hex`
    - `body_sha3_256`
    - `signature_hex`
  - runs through `service::middleware::authorize_result(...)`
  - returns `200 {"verified":true,...}` on success
  - returns `401` with `error = "signature_invalid"` on an invalid signature
  - returns `400` for malformed verification inputs

Why this starter exists:

- It gives the service-platform work a concrete, narrow application-layer auth example without
  pretending Nebula already has inbound TLS termination, mTLS, or a full middleware stack.
- It gives the PQC work a reusable signed-payload shape that higher-level services and communication
  experiments can share.
- It now also gives the service layer an explicit verifier-hook example: the route does not inline
  auth into business logic, it hands verification to a named middleware verifier and only runs the
  success handler on `Allow`.

Non-goals for this example:

- PKI / certificates
- canonical HTTP message-signing specs
- transport security by itself
- a claim that the installed Linux backend SDK already includes the preview PQC packages
