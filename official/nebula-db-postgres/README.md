# nebula-db-postgres

Preview official Nebula package for a narrow PostgreSQL network database story. It is intentionally
smaller than the SQLite package and exists to establish a reusable contract before the control-plane
forcing app moves any active data plane away from SQLite.

Current surface:

- `postgres::Connection`
- `postgres::Transaction`
- `postgres::ResultSet`
- `postgres::Row`
- `probe_runtime() -> Result<Bool, String>`
- `connect(dsn) -> Result<Connection, String>`
- `Connection_close(self) -> Result<Void, String>`
- `Connection_begin_transaction(self) -> Result<Transaction, String>`
- `Connection_execute(self, sql, params) -> Result<Int, String>`
- `Connection_execute0(self, sql) -> Result<Int, String>`
- `Connection_query(self, sql, params) -> Result<ResultSet, String>`
- `Connection_query0(self, sql) -> Result<ResultSet, String>`
- `Connection_run_migrations(self, migrations) -> Result<Int, String>`
- `Connection_migration_status(self, migrations) -> Result<Json, String>`
- `Transaction_execute(self, sql, params) -> Result<Int, String>`
- `Transaction_execute0(self, sql) -> Result<Int, String>`
- `Transaction_query(self, sql, params) -> Result<ResultSet, String>`
- `Transaction_query0(self, sql) -> Result<ResultSet, String>`
- `Transaction_commit(self) -> Result<Void, String>`
- `Transaction_rollback(self) -> Result<Void, String>`
- `ResultSet_len(self) -> Result<Int, String>`
- `ResultSet_row(self, index) -> Result<Row, String>`
- `Row_get_string(self, column) -> Result<String, String>`
- `Row_get_int(self, column) -> Result<Int, String>`
- `Row_get_bool(self, column) -> Result<Bool, String>`
- `Row_get_json(self, column) -> Result<Json, String>`
- `migration(version, name, sql) -> Json`
- `no_params() -> Json`

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
db_postgres = { path = "/path/to/nebula/official/nebula-db-postgres" }
```

Release posture:

- repo-local preview package
- not part of the Linux backend SDK GA contract
- not included in the opt-in backend SDK installed package payload yet
- intended as the first official network-database step after SQLite-first internal apps

Runtime posture:

- dynamically loads `libpq` at runtime instead of requiring link-time PostgreSQL client setup
- `probe_runtime()` reports whether the PostgreSQL client runtime is available
- missing `libpq` produces an explicit diagnostic rather than a link failure or process crash
- DSN handling is delegated to libpq; callers should use operator-owned config/secrets files for
  credentials

Current guarantees:

- text-parameter `PQexecParams` calls through JSON-array params
- compatibility rewriting for current control-plane SQL:
  - `?` placeholders become `$1..$n`
  - `INSERT OR IGNORE` becomes `ON CONFLICT DO NOTHING`
  - `last_insert_rowid()` becomes `lastval()`
  - `INTEGER PRIMARY KEY AUTOINCREMENT` becomes a Postgres identity-style primary key
  - SQLite `INTEGER` columns become Postgres `BIGINT` columns so `Int` and Unix-millis values keep
    Nebula's 64-bit integer contract
- supported bound parameter kinds are `string`, `int`, `bool`, `null`, and JSON object/array as
  stringified JSON text
- basic migration runner with version ordering, duplicate-version rejection, split migration slice
  support, and applied-history name checks
- read-only migration status snapshots for operator preflight:
  - `current_version`
  - `target_version`
  - `pending_count`
  - `history_issue_count`
  - `safe_to_apply`
  - `status = empty | pending | up_to_date | history_mismatch`
  - `pending`
  - `history_issues`
- row getters for `String`, `Int`, `Bool`, and JSON text

Current non-goals:

- ORM
- query DSL
- connection pools
- transparent SQLite/Postgres abstraction
- server management or testcontainer orchestration
- built-in TLS certificate lifecycle or secret storage for database credentials
