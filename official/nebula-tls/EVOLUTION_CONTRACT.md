# nebula-tls Evolution Contract

This file describes the current transport boundary for the preview `nebula-tls` package and the
constraints that future TLS 1.3 or mTLS work should respect.

It is a contract for honest evolution, not a promise that those features already exist.

## Current Transport Contract

Today the package is explicitly:

- client-side only
- TLS 1.2 only
- roots-based server authentication only
- SNI + hostname verification driven by the caller's `host` / `server_name`

Today the package is explicitly not:

- a TLS server package
- an mTLS / client-certificate package
- an ALPN package
- an OS trust-store integration package
- a TLS 1.3 client package

## Tested Boundary Today

The current transport slice is intentionally narrow and now has negative tests that make that
boundary executable:

- empty or NUL-containing transport identity inputs are rejected before the native handshake path
- split-brain routing/identity mismatches are rejected by hostname verification
- TLS 1.3-only servers are rejected by the current TLS 1.2 client profile
- servers that require client certificates are rejected because this package does not send a client
  identity

Those negative tests are part of the current contract, not bugs to "paper over" with looser
fallback behavior.

## Evolution Rules

Future TLS 1.3 support should:

- be added explicitly and tested explicitly
- not silently change the current "TLS 1.2 only" story without updating docs and contract tests
- preserve the current transport-identity rules around `host` / `server_name`

Future mTLS support should:

- add an explicit client-identity configuration path
- avoid overloading the current roots-only constructors with hidden certificate behavior
- keep trust roots and client identity as separate concepts even if they live under one opaque
  `ClientConfig` owner

Future server-side TLS support should:

- arrive through a separate server surface
- avoid implicitly expanding the current client package into a full TLS platform without a new
  contract

## API Shape Guidance

The current `ClientConfig` opacity is useful groundwork.

That opacity should continue to be the ownership boundary for:

- trust roots
- transport profile selection
- any future optional client-certificate identity

What should stay explicit at the call boundary:

- the transport identity being verified (`host` / `server_name`)
- whether the caller uses the safe default `dial_host(...)` path or the expert `dial(addr, server_name, ...)` path

What should not happen in a future upgrade:

- silent fallback from TLS 1.3 back to TLS 1.2 without policy visibility
- hidden mTLS enablement through environment variables or ad hoc host files
- mixing routing and identity semantics so loosely that hostname verification becomes cosmetic
