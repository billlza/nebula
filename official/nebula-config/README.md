# nebula-config

Preview configuration and mounted-secret helpers for backend-first Nebula apps.

Current surface:

- `config::required_env(name) -> Result<String, String>`
- `config::optional_env(name, fallback) -> String`
- `config::required_int_env(name) -> Result<Int, String>`
- `config::int_env_or(name, fallback) -> Result<Int, String>`
- `config::required_bool_env(name) -> Result<Bool, String>`
- `config::bool_env_or(name, fallback) -> Result<Bool, String>`
- `config::optional_secret_value(env_name, file_env_name) -> Result<String, String>`
- `config::required_secret_value(env_name, file_env_name) -> Result<String, String>`
- `config::env_diagnostic(name, required, secret) -> Json`
- `config::secret_diagnostic(env_name, file_env_name, required) -> Json`

Secret rules:

- direct env and mounted-secret file inputs for the same value are mutually exclusive
- mounted secret files are read as binary text and trailing CR/LF is trimmed
- missing files, unreadable files, and empty secret files are explicit errors
- mounted secret files over 64 KiB are rejected before reading the payload
- secret values are snapshotted per process after the first lookup
- diagnostic JSON redacts secret values as `<redacted>`

Recommended preview dependency shape:

```toml
[dependencies]
app_config = { path = "/path/to/nebula/official/nebula-config" }
```

Release posture:

- repo-local preview package
- not part of the Linux backend SDK GA contract
- not shipped as an installed-preview package yet

Non-claims:

- no cloud KMS integration
- no dynamic secret rotation platform
- no encrypted-at-rest secret store
- no policy DSL or authorization engine
- no service HTTP bind/timeout config; that remains owned by `official/nebula-service`
