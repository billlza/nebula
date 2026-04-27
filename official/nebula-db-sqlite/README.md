# nebula-db-sqlite

Preview official Nebula package for embedded SQLite-backed application state on top of a narrow
runtime-backed handle layer.

Current surface in this repo wave:

- `sqlite::Connection`
- `sqlite::Transaction`
- `sqlite::ResultSet`
- `sqlite::Row`
- `open(path) -> Result<Connection, String>`
- `Connection_close(self) -> Result<Void, String>`
- `Connection_begin_transaction(self) -> Result<Transaction, String>`
- `Connection_execute(self, sql, params) -> Result<Int, String>`
- `Connection_execute0(self, sql) -> Result<Int, String>`
- `Connection_query(self, sql, params) -> Result<ResultSet, String>`
- `Connection_query0(self, sql) -> Result<ResultSet, String>`
- `Connection_run_migrations(self, migrations) -> Result<Int, String>`
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
- `Row_get_bytes(self, column) -> Result<Bytes, String>`
- `Row_get_string_at(self, index) -> Result<String, String>`
- `Row_get_int_at(self, index) -> Result<Int, String>`
- `Row_get_bool_at(self, index) -> Result<Bool, String>`
- `Row_get_json_at(self, index) -> Result<Json, String>`
- `Row_get_bytes_at(self, index) -> Result<Bytes, String>`
- `migration(version, name, sql) -> Json`
- `no_params() -> Json`

Recommended preview dependency shape from a Nebula repo checkout:

```toml
[dependencies]
db_sqlite = { path = "/path/to/nebula/official/nebula-db-sqlite" }
```

Installed-preview dependency shape on Linux x86_64 after installing the opt-in backend SDK asset:

```toml
[dependencies]
db_sqlite = { installed = "nebula-db-sqlite" }
```

Release posture:

- repo-local preview / pilot surface
- additionally shipped as an installed preview package inside the opt-in Linux backend SDK asset on
  Linux x86_64
- still not part of the Linux backend SDK GA contract
- intended as the first official embedded-data step toward the backend-first internal app platform

Current guarantees for this package revision:

- single-process local SQLite database access through a runtime-backed connection handle
- explicit transaction handle with `BEGIN IMMEDIATE`, `commit`, and `rollback`
- migration runner with version ordering, duplicate-version rejection, split migration slice support,
  and applied-history name checks
- query result sets through explicit row getters instead of app-local JSON/sqlite seams
- stable-column-order hot paths can use indexed row getters; name-based getters remain the clearer
  default for general app code
- connection-local prepared statement reuse for stable DML/query statements, while DDL and
  migration paths clear cached statements before continuing
- automatic database setup defaults aligned with the current internal-app path:
  - WAL journal mode
  - `synchronous=NORMAL`
  - busy timeout
- dynamic SQLite loading so the package does not require link-time sqlite setup in Nebula apps

Current non-goals for this package revision:

- ORM
- query DSL
- cross-database abstraction
- connection pools
- distributed transactions
- background job / queue platform
- secret storage or encrypted-at-rest key management

Binding notes for this wave:

- `params` is a JSON array
- supported bound value kinds are `string`, `int`, `bool`, `null`
- JSON objects/arrays are stringified and bound as text
- blob parameter binding is intentionally not part of this first preview slice

Design notes:

- This package exists to replace app-specific SQLite host seams with one official, reusable data
  contract for backend-first internal apps.
- It is intentionally narrow and explicit: durable local storage, migrations, transactions, and row
  access first; richer database/platform features come later.
