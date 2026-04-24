# nebula-tls

Preview official Nebula package for async outbound/client TLS on top of `std::net::TcpStream`.

Current surface in this repo wave:

- `tls::client::ClientConfig`
- `tls::client::TlsTrustStore`
- `tls::client::TlsServerName`
- `tls::client::TlsClientIdentity`
- `tls::client::TlsVersionPolicy`
- `tls::client::TlsAlpnPolicy`
- `tls::client::TlsClientStream`
- `client_config_from_ca_pem(ca_pem: Bytes) -> Result<ClientConfig, String>`
- `client_config_from_ca_pem_text(ca_pem: String) -> Result<ClientConfig, String>`
- `client_config_default_roots() -> Result<ClientConfig, String>`
- `trust_store_from_ca_pem(_text|_default_roots)(...)`
- `server_name(...)`
- `client_identity_from_pem(_text|_files)(...)`
- `with_client_identity(...)`
- `with_version_policy(...)`
- `with_alpn_policy(...)`
- `async handshake(stream: TcpStream, server_name: String, config: ClientConfig) -> Result<TlsClientStream, String>`
- `async dial(addr: SocketAddr, server_name: String, config: ClientConfig) -> Result<TlsClientStream, String>`
- `async dial_host(host: String, port: Int, config: ClientConfig) -> Result<TlsClientStream, String>`
- `TlsClientStream_read/write/write_all/close/abort/peer_addr/local_addr`
- `TlsClientStream_peer_present/peer_verified/peer_subject/tls_version/alpn_protocol`
- `tls::http`
  - `async TlsClientStream_write_request(self, request: ClientRequest) -> Result<Void, String>`
  - `async TlsClientStream_read_response(self) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_limited(self, max_header_bytes: Int, max_body_bytes: Int) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_for(self, method: Method) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_limited_for(self, method: Method, max_header_bytes: Int, max_body_bytes: Int) -> Result<ClientResponse, String>`
- `tls::http2`
  - `async TlsClientStream_write_request_h2(self, request: ClientRequest) -> Result<Void, String>`
  - `async TlsClientStream_read_response_h2(self) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_limited_h2(self, max_header_bytes: Int, max_body_bytes: Int) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_for_h2(self, method: Method) -> Result<ClientResponse, String>`
  - `async TlsClientStream_read_response_limited_for_h2(self, method: Method, max_header_bytes: Int, max_body_bytes: Int) -> Result<ClientResponse, String>`

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
tls = { path = "/path/to/nebula/official/nebula-tls" }
```

Release posture:

- repo-local preview / pilot surface
- not installed by Nebula binary release archives or install scripts
- transport evolution constraints documented in `EVOLUTION_CONTRACT.md`

Current host support:

- macOS
- Linux

Current guarantees for this package revision:

- client-only TLS over async `TcpStream`
- thin HTTPS client adapters on top of `std::http` request/response types
- narrow preview HTTP/2 client adapters on top of `std::http` request/response types
- PEM CA bundle input
- bundled Mozilla-derived default root bundle
- hostname verification
- SNI
- TLS 1.2 / TLS 1.3 version policy
- explicit client identity attach for outbound mTLS
- explicit ALPN policy wiring
- explicit ALPN negotiation visibility through `TlsClientStream_alpn_protocol(...)`
- explicit rejection of empty / NUL-corrupted transport identity inputs

Current non-goals for this package revision:

- TLS server support
- inbound listener/server stream ownership
- OS trust store integration
- Windows host support

Design notes for this wave:

- The package uses vendored mbedTLS behind a narrow runtime-backed handle layer instead of exposing raw crypto internals.
- Default roots are bundled into the package at build time so client trust does not depend on ad hoc host CA file paths.
- `ClientConfig` and `TlsClientStream` are opaque runtime-managed values; direct construction is rejected.
- `dial_host(...)` is the narrow convenience path for real clients: it resolves the host through
  `std::net`, reuses that same host for TLS SNI / hostname verification, and avoids the
  split-brain `addr`/`server_name` mismatch that low-level callers can still opt into.
- `handshake(...)` / `dial(...)` treat `server_name` as an authenticated transport identity
  input, not a cosmetic label: it must be non-empty, it feeds both SNI and hostname
  verification, and NUL-containing values are rejected instead of being silently truncated at
  the native TLS boundary.
- `dial(addr, server_name, ...)` remains the expert path for callers that intentionally separate
  routing from identity, but those callers own keeping the socket destination and verified
  server identity coherent.
- `tls::http` inherits `std::http`'s narrow client semantics, including skipped interim `1xx`,
  no-body handling for `HEAD` / `204` / `205` / `304`, and the method-aware
  `read_response_for(...)` path when the caller needs `HEAD` correctness.
- `tls::http2` is intentionally a narrow preview transport seam:
  - explicit `h2` ALPN is required
  - Nebula-to-Nebula sequential request/response flows are covered by smoke
  - no hidden downgrade into HTTP/1.1 helpers
  - no server-push, dynamic HPACK table, or broad multiplexing claim in this wave
  - the debug seam exposes both raw frame history and higher-level semantic timeline events such as
    `stream_open`, `response_headers_seen`, `stream_closed`, and `goaway_observed`
  - semantic phase events include a stable `classification.reason` /
    `classification.detail` pair for operator-readable diagnostics
- `spawn`/async/runtime semantics stay in the compiler/runtime; this package only owns the TLS bridge and package surface.
- This wave is intentionally honest and narrow: a real client TLS slice, not a full HTTPS, PKI,
  or edge-termination platform.
- `EVOLUTION_CONTRACT.md` records the current negative-tested boundary and the rules future TLS 1.3
  or mTLS work should follow so the package can grow without hiding contract changes in
  implementation details.
