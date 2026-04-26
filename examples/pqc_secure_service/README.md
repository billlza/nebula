# pqc_secure_service

Repo-local preview example for application-layer post-quantum request/response encryption on top of
Nebula's HTTP service helpers.

What it demonstrates:

- `pqc::channel` for authenticated key establishment
- pinned signed initiator identity with `accept_pinned_initiator(...)`
- encrypted request/response bodies carried as JSON text envelopes
- plain HTTP transport today, with the same application-layer payload shape still usable on top of
  `nebula-tls` client transport

The example server expects these environment variables:

- `PQC_SECURE_SERVICE_KEM_PUBLIC_KEY_HEX`
- `PQC_SECURE_SERVICE_KEM_SECRET_KEY_HEX`
- `PQC_SECURE_SERVICE_SIGN_PUBLIC_KEY_HEX`
- `PQC_SECURE_SERVICE_SIGN_SECRET_KEY_HEX`
- `PQC_SECURE_SERVICE_CLIENT_SIGN_PUBLIC_KEY_HEX`

Routes:

- `GET /secure/bootstrap`
  - returns the configured KEM and signing public keys
- `POST /secure/open`
  - expects a JSON body with:
    - `hello_text`
  - returns a JSON body with:
    - `accept_text`
- `POST /secure/echo`
  - expects a JSON body with:
    - `hello_text`
    - `ciphertext_text`
  - returns a JSON body with:
    - `ciphertext_text`

This example is intentionally application-layer and repo-local preview only. It is not TLS
handshake integration, not PKI, and not a claim of full transport security for every deployment.
