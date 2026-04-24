# nebula-auth

Preview authentication helpers for Nebula services.

Current surface:

- `auth::jwt::JwtValidationConfig`
- `auth::jwt::JwtClaims`
- `auth::jwt::verify_rs256_jwt(token, config) -> Result<JwtClaims, String>`
- `auth::jwt::verify_rs256_authorization_header(header, config) -> Result<JwtClaims, String>`
- `auth::jwt::validate_rs256_jwks(jwks_text) -> Result<Bool, String>`

This package is a narrow resource-server JWT verifier:

- RS256 only
- JWKS is loaded by the caller and passed as text
- issuer, audience, expiry, not-before, `kid`, and signature are verified
- roles are read from the `roles` claim

Non-claims:

- no local accounts or password auth
- no cookie sessions
- no OIDC browser login or refresh-token flow
- no JWKS URL fetch/cache/rotation
- no signing API or broad key-management API
- no ES256/EdDSA/HS256 support
