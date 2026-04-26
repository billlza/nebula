# Mounted Secret Files

Place mounted token files here in local/dev layouts if you want to exercise the same contract as a
production secret mount.

Expected files:

- `read.token`
- `write.token`
- `admin.token`
- `worker.token` when you enable the dedicated worker route
- `jwks.json` when you opt into `APP_AUTH_MODE=jwt`

The static token contract is startup-only:

- the service resolves token files through the preview `official/nebula-config` mounted-secret
  helper during startup validation
- the resolved values become the running process snapshot
- changing token files later requires a process restart to take effect
- direct `APP_*_TOKEN` env values and matching `APP_*_TOKEN_FILE` mounts are mutually exclusive

JWT auth is a resource-server preview. It verifies RS256 Bearer tokens against `APP_AUTH_JWKS_FILE`
and does not fetch JWKS URLs, create sessions, or run an OIDC login flow. The JWKS file is validated
at startup and read by the verifier when checking JWTs; restart the service when rolling key-file
changes so operators get a predictable deployment boundary.

Do not commit real values.
