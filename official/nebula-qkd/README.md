# nebula-qkd

Preview official package for QKD key-delivery software integration.

Current V1 surface:

- `qkd::provider`: provider-facing config, validation, ETSI-style client request builders, and
  deterministic mock provider helpers. It also exposes a preview KME target resolver and a
  cleartext HTTP lab client for local/dev KME contract tests plus an outbound mTLS adapter for
  preview KME integration.
- `qkd::etsi014`: ETSI GS QKD 014 v1.1.1-style REST key-delivery wire helpers for status,
  `enc_keys`, and `dec_keys` paths plus JSON response parsing.
- `qkd::mock`: local deterministic provider for contract tests and development without QKD
  hardware.
- `qkd::types`: preview data types for provider config, status, key batches, key requests, and
  structured provider errors.

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
qkd = { path = "/path/to/nebula/official/nebula-qkd" }
```

Release posture:

- repo-local preview / pilot surface
- not installed by Nebula binary release archives or backend SDK assets

Design notes for this wave:

- This package supports real QKD software integration at the KME/KMS key-delivery boundary. It does
  not implement the physical quantum channel, optical hardware, detector control, trusted-node
  network, or a security certification.
- The path and key-container shape follows the narrow ETSI GS QKD 014 v1.1.1 REST endpoints:
  `/api/v1/keys/{SAE_ID}/status`, `/api/v1/keys/{SAE_ID}/enc_keys`, and
  `/api/v1/keys/{SAE_ID}/dec_keys`; key containers serialize as `{"keys":[{"key_ID":"...","key":"..."}]}`.
- `kme_base_url` is parsed as `http(s)://host[:port][/prefix]`; request builders derive the HTTP
  authority and full ETSI path from that target and reject userinfo, query, fragment, invalid port,
  host/port drift, and scheme/`use_tls` mismatch.
- The preview HTTP lab client only supports `http://` targets with `use_tls=false`.
- The mTLS adapter only supports `https://` targets with `use_tls=true`, builds its own
  `nebula-tls` client identity from caller-provided CA/client cert/client key PEM, and uses HTTP/1.1
  over TLS for ETSI-style KME requests.
- Production deployments of QKD key-delivery APIs normally require mutually authenticated TLS. This
  package now has a narrow mTLS adapter smoke, but it still does not claim QKD-TLS GA, certificate
  lifecycle management, vendor SDK support, or security certification.
- `official/nebula-qcomm-sim` remains simulation-only BB84 lab work; this package is the separate
  real-integration contract for application-facing key delivery.
