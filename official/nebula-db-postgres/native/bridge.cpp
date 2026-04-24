#include "runtime/nebula_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

struct PGconn;
struct PGresult;

constexpr int kConnectionOk = 0;
constexpr int kPgCommandOk = 1;
constexpr int kPgTuplesOk = 2;

struct PostgresApi {
  bool loaded = false;
  std::string error;

#if defined(_WIN32)
  HMODULE handle = nullptr;
#else
  void* handle = nullptr;
#endif

  PGconn* (*connectdb)(const char*) = nullptr;
  void (*finish)(PGconn*) = nullptr;
  int (*status)(const PGconn*) = nullptr;
  char* (*error_message)(const PGconn*) = nullptr;
  PGresult* (*exec)(PGconn*, const char*) = nullptr;
  PGresult* (*exec_params)(PGconn*, const char*, int, const unsigned int*, const char* const*, const int*, const int*, int) = nullptr;
  int (*result_status)(const PGresult*) = nullptr;
  char* (*result_error_message)(const PGresult*) = nullptr;
  void (*clear)(PGresult*) = nullptr;
  int (*ntuples)(const PGresult*) = nullptr;
  int (*nfields)(const PGresult*) = nullptr;
  char* (*fname)(const PGresult*, int) = nullptr;
  int (*getisnull)(const PGresult*, int, int) = nullptr;
  char* (*getvalue)(const PGresult*, int, int) = nullptr;
  int (*getlength)(const PGresult*, int, int) = nullptr;
  char* (*cmd_tuples)(PGresult*) = nullptr;
};

template <typename T>
nebula::rt::Result<T, std::string> err_result(std::string message) {
  return nebula::rt::err_result<T>(std::move(message));
}

nebula::rt::Result<void, std::string> err_void_result(std::string message) {
  return nebula::rt::err_void_result(std::move(message));
}

nebula::rt::Result<void, std::string> ok_void_result() {
  return nebula::rt::ok_void_result();
}

PostgresApi& postgres_api() {
  static PostgresApi api;
  static std::once_flag once;
  std::call_once(once, [&] {
#if defined(_WIN32)
    const char* candidates[] = {"libpq.dll"};
    for (const char* candidate : candidates) {
      api.handle = LoadLibraryA(candidate);
      if (api.handle != nullptr) break;
    }
    if (api.handle == nullptr) {
      api.error = "libpq dynamic library is unavailable; install PostgreSQL client libraries";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(api.handle, symbol));
      return target != nullptr;
    };
#else
    const char* candidates[] = {
        "libpq.5.dylib",
        "libpq.dylib",
        "/opt/homebrew/opt/libpq/lib/libpq.5.dylib",
        "/opt/homebrew/lib/libpq.5.dylib",
        "/usr/local/opt/libpq/lib/libpq.5.dylib",
        "/usr/local/lib/libpq.5.dylib",
        "/Library/PostgreSQL/18/lib/libpq.5.dylib",
        "/Library/PostgreSQL/17/lib/libpq.5.dylib",
        "/Library/PostgreSQL/16/lib/libpq.5.dylib",
        "/Library/PostgreSQL/15/lib/libpq.5.dylib",
        "/Library/PostgreSQL/14/lib/libpq.5.dylib",
        "libpq.so.5",
        "libpq.so",
    };
    for (const char* candidate : candidates) {
      api.handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
      if (api.handle != nullptr) break;
    }
    if (api.handle == nullptr) {
      api.error = "libpq dynamic library is unavailable; install PostgreSQL client libraries";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(dlsym(api.handle, symbol));
      return target != nullptr;
    };
#endif

    if (!load(api.connectdb, "PQconnectdb") || !load(api.finish, "PQfinish") ||
        !load(api.status, "PQstatus") || !load(api.error_message, "PQerrorMessage") ||
        !load(api.exec, "PQexec") || !load(api.exec_params, "PQexecParams") ||
        !load(api.result_status, "PQresultStatus") ||
        !load(api.result_error_message, "PQresultErrorMessage") || !load(api.clear, "PQclear") ||
        !load(api.ntuples, "PQntuples") || !load(api.nfields, "PQnfields") ||
        !load(api.fname, "PQfname") || !load(api.getisnull, "PQgetisnull") ||
        !load(api.getvalue, "PQgetvalue") || !load(api.getlength, "PQgetlength") ||
        !load(api.cmd_tuples, "PQcmdTuples")) {
      api.error = "libpq dynamic library is missing required symbols";
      return;
    }
    api.loaded = true;
  });
  return api;
}

nebula::rt::Result<bool, std::string> ensure_postgres_loaded() {
  const auto& api = postgres_api();
  if (!api.loaded) {
    return err_result<bool>(api.error.empty() ? "libpq dynamic library is unavailable" : api.error);
  }
  return nebula::rt::ok_result<bool>(true);
}

std::string postgres_connection_error(PGconn* conn, std::string_view action) {
  std::string message(action);
  message += ": ";
  if (conn == nullptr || postgres_api().error_message == nullptr) {
    message += "postgres connection error";
    return message;
  }
  const char* raw = postgres_api().error_message(conn);
  message += raw == nullptr ? "postgres connection error" : raw;
  return message;
}

std::string postgres_result_error(PGresult* result, std::string_view action) {
  std::string message(action);
  message += ": ";
  if (result == nullptr || postgres_api().result_error_message == nullptr) {
    message += "postgres result error";
    return message;
  }
  const char* raw = postgres_api().result_error_message(result);
  message += raw == nullptr ? "postgres result error" : raw;
  return message;
}

class PgResult {
  PGresult* result_ = nullptr;

public:
  PgResult() = default;
  explicit PgResult(PGresult* result) : result_(result) {}
  PgResult(const PgResult&) = delete;
  PgResult& operator=(const PgResult&) = delete;

  PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}

  PgResult& operator=(PgResult&& other) noexcept {
    if (this == &other) return *this;
    reset();
    result_ = std::exchange(other.result_, nullptr);
    return *this;
  }

  ~PgResult() { reset(); }

  PGresult* get() const { return result_; }

  void reset() {
    if (result_ == nullptr) return;
    postgres_api().clear(result_);
    result_ = nullptr;
  }
};

std::string json_param_to_text(const nebula::rt::JsonValue& value) {
  using nebula::rt::JsonValueKind;
  if (value.parsed.kind == JsonValueKind::String) {
    auto text = nebula::rt::json_as_string(value);
    if (!nebula::rt::result_is_err(text)) return nebula::rt::result_ok_ref(text);
  }
  if (value.parsed.kind == JsonValueKind::Int) return std::to_string(value.parsed.int_value);
  if (value.parsed.kind == JsonValueKind::Bool) return value.parsed.bool_value ? "1" : "0";
  return nebula::rt::json_stringify(value);
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
  std::size_t pos = 0;
  while ((pos = text.find(from, pos)) != std::string::npos) {
    text.replace(pos, from.size(), to);
    pos += to.size();
  }
  return text;
}

std::string rewrite_question_placeholders(std::string sql) {
  std::string out;
  out.reserve(sql.size() + 16);
  bool in_single_quote = false;
  std::int64_t index = 1;
  for (std::size_t i = 0; i < sql.size(); ++i) {
    const char ch = sql[i];
    if (ch == '\'') {
      out.push_back(ch);
      if (in_single_quote && i + 1 < sql.size() && sql[i + 1] == '\'') {
        out.push_back(sql[i + 1]);
        ++i;
        continue;
      }
      in_single_quote = !in_single_quote;
      continue;
    }
    if (ch == '?' && !in_single_quote) {
      out.push_back('$');
      out += std::to_string(index++);
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

std::string rewrite_insert_or_ignore(std::string sql) {
  constexpr std::string_view prefix = "INSERT OR IGNORE INTO ";
  if (sql.rfind(prefix, 0) != 0) return sql;
  sql.replace(0, prefix.size(), "INSERT INTO ");
  sql += " ON CONFLICT DO NOTHING";
  return sql;
}

std::string rewrite_postgres_sql(std::string sql) {
  sql = replace_all(std::move(sql), "INTEGER PRIMARY KEY AUTOINCREMENT", "BIGSERIAL PRIMARY KEY");
  sql = replace_all(std::move(sql), "integer primary key autoincrement", "BIGSERIAL PRIMARY KEY");
  sql = replace_all(std::move(sql), " INTEGER", " BIGINT");
  sql = replace_all(std::move(sql), " integer", " BIGINT");
  sql = replace_all(std::move(sql), "last_insert_rowid()", "lastval()");
  sql = rewrite_insert_or_ignore(std::move(sql));
  return rewrite_question_placeholders(std::move(sql));
}

struct BoundParams {
  std::vector<std::string> storage;
  std::vector<const char*> values;
};

nebula::rt::Result<BoundParams, std::string> bind_params(nebula::rt::JsonValue params) {
  auto array_len = nebula::rt::json_array_len(params);
  if (nebula::rt::result_is_err(array_len)) {
    return err_result<BoundParams>("postgres params must be a JSON array");
  }
  BoundParams out;
  const auto count = nebula::rt::result_ok_ref(array_len);
  out.storage.reserve(static_cast<std::size_t>(count));
  out.values.reserve(static_cast<std::size_t>(count));
  for (std::int64_t i = 0; i < count; ++i) {
    auto item = nebula::rt::json_array_get(params, i);
    if (nebula::rt::result_is_err(item)) {
      return err_result<BoundParams>(nebula::rt::result_err_ref(item));
    }
    const auto& value = nebula::rt::result_ok_ref(item);
    if (value.parsed.kind == nebula::rt::JsonValueKind::Null) {
      out.values.push_back(nullptr);
      continue;
    }
    out.storage.push_back(json_param_to_text(value));
    out.values.push_back(out.storage.back().c_str());
  }
  return nebula::rt::ok_result(std::move(out));
}

nebula::rt::Result<PgResult, std::string> exec_simple(PGconn* conn, std::string_view sql) {
  if (conn == nullptr) return err_result<PgResult>("postgres connection is closed");
  const std::string rewritten_sql = rewrite_postgres_sql(std::string(sql));
  PGresult* raw = postgres_api().exec(conn, rewritten_sql.c_str());
  if (raw == nullptr) return err_result<PgResult>(postgres_connection_error(conn, "execute postgres statement"));
  PgResult result(raw);
  const int status = postgres_api().result_status(result.get());
  if (status != kPgCommandOk && status != kPgTuplesOk) {
    return err_result<PgResult>(postgres_result_error(result.get(), "execute postgres statement"));
  }
  return nebula::rt::ok_result(std::move(result));
}

nebula::rt::Result<PgResult, std::string> exec_params(PGconn* conn,
                                                      std::string_view sql,
                                                      nebula::rt::JsonValue params) {
  if (conn == nullptr) return err_result<PgResult>("postgres connection is closed");
  auto bound = bind_params(std::move(params));
  if (nebula::rt::result_is_err(bound)) {
    return err_result<PgResult>(nebula::rt::result_err_ref(bound));
  }
  auto& bound_ref = nebula::rt::result_ok_ref(bound);
  const std::string rewritten_sql = rewrite_postgres_sql(std::string(sql));
  PGresult* raw = postgres_api().exec_params(conn,
                                             rewritten_sql.c_str(),
                                             static_cast<int>(bound_ref.values.size()),
                                             nullptr,
                                             bound_ref.values.empty() ? nullptr : bound_ref.values.data(),
                                             nullptr,
                                             nullptr,
                                             0);
  if (raw == nullptr) return err_result<PgResult>(postgres_connection_error(conn, "execute postgres statement"));
  PgResult result(raw);
  const int status = postgres_api().result_status(result.get());
  if (status != kPgCommandOk && status != kPgTuplesOk) {
    return err_result<PgResult>(postgres_result_error(result.get(), "execute postgres statement"));
  }
  return nebula::rt::ok_result(std::move(result));
}

std::int64_t command_rows_changed(PGresult* result) {
  const char* raw = postgres_api().cmd_tuples(result);
  if (raw == nullptr || raw[0] == '\0') return 0;
  char* end = nullptr;
  const long long parsed = std::strtoll(raw, &end, 10);
  if (end == raw) return 0;
  return static_cast<std::int64_t>(parsed);
}

} // namespace

namespace nebula::rt {

struct PostgresCell {
  bool is_null = true;
  std::string text;
};

struct PostgresColumnValue {
  std::string name;
  PostgresCell value;
};

struct PostgresResultRowData {
  std::vector<PostgresColumnValue> columns;
};

struct PostgresConnectionState {
  PGconn* conn = nullptr;

  ~PostgresConnectionState() {
    if (conn != nullptr) postgres_api().finish(conn);
  }
};

struct PostgresTransactionState {
  std::shared_ptr<PostgresConnectionState> connection;
  bool active = false;

  ~PostgresTransactionState() {
    if (active && connection != nullptr && connection->conn != nullptr) {
      exec_simple(connection->conn, "ROLLBACK");
    }
  }
};

struct PostgresResultSetState {
  std::vector<PostgresResultRowData> rows;
};

} // namespace nebula::rt

namespace {

nebula::rt::Result<nebula::rt::PostgresResultSet, std::string> materialize_query(PGconn* conn,
                                                                                 std::string_view sql,
                                                                                 nebula::rt::JsonValue params) {
  auto result = exec_params(conn, sql, std::move(params));
  if (nebula::rt::result_is_err(result)) {
    return err_result<nebula::rt::PostgresResultSet>(nebula::rt::result_err_ref(result));
  }
  if (postgres_api().result_status(nebula::rt::result_ok_ref(result).get()) != kPgTuplesOk) {
    return err_result<nebula::rt::PostgresResultSet>("postgres query did not return rows");
  }
  auto state = std::make_shared<nebula::rt::PostgresResultSetState>();
  PGresult* raw = nebula::rt::result_ok_ref(result).get();
  const int rows = postgres_api().ntuples(raw);
  const int fields = postgres_api().nfields(raw);
  state->rows.reserve(static_cast<std::size_t>(rows));
  for (int row_index = 0; row_index < rows; ++row_index) {
    nebula::rt::PostgresResultRowData row;
    row.columns.reserve(static_cast<std::size_t>(fields));
    for (int field_index = 0; field_index < fields; ++field_index) {
      nebula::rt::PostgresColumnValue column;
      const char* name = postgres_api().fname(raw, field_index);
      column.name = name == nullptr ? "" : std::string(name);
      column.value.is_null = postgres_api().getisnull(raw, row_index, field_index) != 0;
      if (!column.value.is_null) {
        const char* value = postgres_api().getvalue(raw, row_index, field_index);
        const int length = postgres_api().getlength(raw, row_index, field_index);
        column.value.text = value == nullptr ? "" : std::string(value, static_cast<std::size_t>(std::max(length, 0)));
      }
      row.columns.push_back(std::move(column));
    }
    state->rows.push_back(std::move(row));
  }
  return nebula::rt::ok_result(nebula::rt::PostgresResultSet{std::move(state)});
}

PGconn* require_connection(const nebula::rt::PostgresConnection& self, std::string& error) {
  if (self.state == nullptr) {
    error = "postgres connection is uninitialized";
    return nullptr;
  }
  if (self.state->conn == nullptr) {
    error = "postgres connection is closed";
    return nullptr;
  }
  return self.state->conn;
}

PGconn* require_transaction_connection(const nebula::rt::PostgresTransaction& self, std::string& error) {
  if (self.state == nullptr || self.state->connection == nullptr) {
    error = "postgres transaction is uninitialized";
    return nullptr;
  }
  if (self.state->connection->conn == nullptr) {
    error = "postgres transaction connection is closed";
    return nullptr;
  }
  if (!self.state->active) {
    error = "postgres transaction is inactive";
    return nullptr;
  }
  return self.state->connection->conn;
}

const nebula::rt::PostgresResultRowData* require_row(const nebula::rt::PostgresRow& row, std::string& error) {
  if (row.state == nullptr) {
    error = "postgres row is uninitialized";
    return nullptr;
  }
  if (row.index < 0 || static_cast<std::size_t>(row.index) >= row.state->rows.size()) {
    error = "postgres row index is out of range";
    return nullptr;
  }
  return &row.state->rows[static_cast<std::size_t>(row.index)];
}

const nebula::rt::PostgresCell* find_cell(const nebula::rt::PostgresRow& row,
                                          std::string_view column,
                                          std::string& error) {
  const auto* row_data = require_row(row, error);
  if (row_data == nullptr) return nullptr;
  for (const auto& value : row_data->columns) {
    if (value.name == column) return &value.value;
  }
  error = "postgres column not found: " + std::string(column);
  return nullptr;
}

nebula::rt::Result<std::int64_t, std::string> query_single_int(PGconn* conn,
                                                               std::string_view sql,
                                                               std::string_view column,
                                                               std::string_view context) {
  auto rows = materialize_query(conn,
                                std::string(sql),
                                nebula::rt::json_array_build(nebula::rt::json_array_builder()));
  if (nebula::rt::result_is_err(rows)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(rows));
  }
  nebula::rt::PostgresRow row{nebula::rt::result_ok_ref(rows).state, 0};
  std::string error;
  const auto* cell = find_cell(row, column, error);
  if (cell == nullptr || cell->is_null) {
    return err_result<std::int64_t>(std::string(context) + " was not returned");
  }
  try {
    return nebula::rt::ok_result<std::int64_t>(std::stoll(cell->text));
  } catch (...) {
    return err_result<std::int64_t>(std::string(context) + " is not an integer");
  }
}

nebula::rt::Result<std::int64_t, std::string> schema_migrations_table_exists(PGconn* conn) {
  return query_single_int(conn,
                          "SELECT CASE WHEN to_regclass('__nebula_schema_migrations') IS NULL "
                          "THEN 0 ELSE 1 END AS exists",
                          "exists",
                          "postgres schema migration table existence");
}

nebula::rt::Result<std::int64_t, std::string> read_schema_version(PGconn* conn, bool ensure_table) {
  if (ensure_table) {
    auto ensure = exec_simple(conn,
                              "CREATE TABLE IF NOT EXISTS __nebula_schema_migrations ("
                              "version BIGINT PRIMARY KEY,"
                              "name TEXT NOT NULL,"
                              "applied_unix_ms BIGINT NOT NULL"
                              ")");
    if (nebula::rt::result_is_err(ensure)) {
      return err_result<std::int64_t>(nebula::rt::result_err_ref(ensure));
    }
  } else {
    auto exists = schema_migrations_table_exists(conn);
    if (nebula::rt::result_is_err(exists)) {
      return err_result<std::int64_t>(nebula::rt::result_err_ref(exists));
    }
    if (nebula::rt::result_ok_ref(exists) == 0) {
      return nebula::rt::ok_result<std::int64_t>(0);
    }
  }
  return query_single_int(conn,
                          "SELECT COALESCE(MAX(version), 0) AS version FROM __nebula_schema_migrations",
                          "version",
                          "postgres schema version");
}

nebula::rt::Result<std::int64_t, std::string> current_schema_version(PGconn* conn) {
  return read_schema_version(conn, true);
}

nebula::rt::Result<std::int64_t, std::string> current_schema_version_without_create(PGconn* conn) {
  return read_schema_version(conn, false);
}

struct MigrationSpec {
  std::int64_t version = 0;
  std::string name;
  std::string sql;
};

nebula::rt::Result<std::vector<MigrationSpec>, std::string> decode_migrations(nebula::rt::JsonValue migrations) {
  auto count = nebula::rt::json_array_len(migrations);
  if (nebula::rt::result_is_err(count)) {
    return err_result<std::vector<MigrationSpec>>("postgres migrations must be a JSON array");
  }
  std::vector<MigrationSpec> out;
  out.reserve(static_cast<std::size_t>(nebula::rt::result_ok_ref(count)));
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto item = nebula::rt::json_array_get(migrations, i);
    if (nebula::rt::result_is_err(item)) {
      return err_result<std::vector<MigrationSpec>>(nebula::rt::result_err_ref(item));
    }
    auto version = nebula::rt::json_get_int(nebula::rt::result_ok_ref(item), "version");
    auto name = nebula::rt::json_get_string(nebula::rt::result_ok_ref(item), "name");
    auto sql = nebula::rt::json_get_string(nebula::rt::result_ok_ref(item), "sql");
    if (nebula::rt::result_is_err(version) || nebula::rt::result_is_err(name) || nebula::rt::result_is_err(sql)) {
      return err_result<std::vector<MigrationSpec>>("postgres migration entries require version/name/sql");
    }
    if (nebula::rt::result_ok_ref(version) <= 0) {
      return err_result<std::vector<MigrationSpec>>("postgres migration version must be positive");
    }
    out.push_back(MigrationSpec{nebula::rt::result_ok_ref(version),
                                nebula::rt::result_ok_ref(name),
                                nebula::rt::result_ok_ref(sql)});
  }
  std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.version < rhs.version;
  });
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i - 1].version == out[i].version) {
      return err_result<std::vector<MigrationSpec>>("postgres migration versions must be unique");
    }
  }
  return nebula::rt::ok_result(std::move(out));
}

nebula::rt::JsonValue one_int_param(std::int64_t value) {
  auto builder = nebula::rt::json_array_builder();
  builder = nebula::rt::json_array_push(std::move(builder), nebula::rt::json_int_value(value));
  return nebula::rt::json_array_build(std::move(builder));
}

nebula::rt::Result<std::optional<std::string>, std::string>
applied_migration_name(PGconn* conn, std::int64_t version) {
  auto exists = schema_migrations_table_exists(conn);
  if (nebula::rt::result_is_err(exists)) {
    return err_result<std::optional<std::string>>(nebula::rt::result_err_ref(exists));
  }
  if (nebula::rt::result_ok_ref(exists) == 0) {
    return nebula::rt::ok_result<std::optional<std::string>>(std::nullopt);
  }

  auto rows = materialize_query(conn,
                                "SELECT name FROM __nebula_schema_migrations WHERE version = ?",
                                one_int_param(version));
  if (nebula::rt::result_is_err(rows)) {
    return err_result<std::optional<std::string>>(nebula::rt::result_err_ref(rows));
  }
  if (nebula::rt::result_ok_ref(rows).state->rows.empty()) {
    return nebula::rt::ok_result<std::optional<std::string>>(std::nullopt);
  }
  nebula::rt::PostgresRow row{nebula::rt::result_ok_ref(rows).state, 0};
  std::string error;
  const auto* cell = find_cell(row, "name", error);
  if (cell == nullptr || cell->is_null) {
    return err_result<std::optional<std::string>>("postgres migration name was not returned");
  }
  return nebula::rt::ok_result<std::optional<std::string>>(cell->text);
}

nebula::rt::Result<bool, std::string> verify_applied_migration(PGconn* conn,
                                                               const MigrationSpec& migration) {
  auto applied = applied_migration_name(conn, migration.version);
  if (nebula::rt::result_is_err(applied)) {
    return err_result<bool>(nebula::rt::result_err_ref(applied));
  }
  if (!nebula::rt::result_ok_ref(applied).has_value()) {
    return err_result<bool>("postgres migration history missing version " +
                            std::to_string(migration.version));
  }
  if (*nebula::rt::result_ok_ref(applied) != migration.name) {
    return err_result<bool>("postgres migration history mismatch for version " +
                            std::to_string(migration.version));
  }
  return nebula::rt::ok_result<bool>(true);
}

nebula::rt::JsonValue migration_summary_json(const MigrationSpec& migration) {
  return nebula::rt::json_object2("version",
                                  nebula::rt::json_int_value(migration.version),
                                  "name",
                                  nebula::rt::json_string_value(migration.name));
}

nebula::rt::JsonValue migration_status_json(std::int64_t current_version,
                                            std::int64_t target_version,
                                            std::int64_t pending_count,
                                            std::int64_t history_issue_count,
                                            bool safe_to_apply,
                                            std::string_view status,
                                            nebula::rt::JsonValue pending,
                                            nebula::rt::JsonValue history_issues) {
  return nebula::rt::json_object8("current_version",
                                  nebula::rt::json_int_value(current_version),
                                  "target_version",
                                  nebula::rt::json_int_value(target_version),
                                  "pending_count",
                                  nebula::rt::json_int_value(pending_count),
                                  "history_issue_count",
                                  nebula::rt::json_int_value(history_issue_count),
                                  "safe_to_apply",
                                  nebula::rt::json_bool_value(safe_to_apply),
                                  "status",
                                  nebula::rt::json_string_value(status),
                                  "pending",
                                  std::move(pending),
                                  "history_issues",
                                  std::move(history_issues));
}

nebula::rt::Result<nebula::rt::JsonValue, std::string>
build_migration_status(PGconn* conn, const std::vector<MigrationSpec>& migrations) {
  auto current = current_schema_version_without_create(conn);
  if (nebula::rt::result_is_err(current)) {
    return err_result<nebula::rt::JsonValue>(nebula::rt::result_err_ref(current));
  }
  const std::int64_t current_version = nebula::rt::result_ok_ref(current);
  if (migrations.empty()) {
    return nebula::rt::ok_result(migration_status_json(current_version,
                                                       current_version,
                                                       0,
                                                       0,
                                                       true,
                                                       "empty",
                                                       nebula::rt::json_array_build(
                                                           nebula::rt::json_array_builder()),
                                                       nebula::rt::json_array_build(
                                                           nebula::rt::json_array_builder())));
  }

  const std::int64_t target_version = migrations.back().version;
  auto pending_builder = nebula::rt::json_array_builder();
  auto issue_builder = nebula::rt::json_array_builder();
  std::int64_t pending_count = 0;
  std::int64_t history_issue_count = 0;
  for (const auto& migration : migrations) {
    auto applied = applied_migration_name(conn, migration.version);
    if (nebula::rt::result_is_err(applied)) {
      return err_result<nebula::rt::JsonValue>(nebula::rt::result_err_ref(applied));
    }
    if (nebula::rt::result_ok_ref(applied).has_value()) {
      if (*nebula::rt::result_ok_ref(applied) != migration.name) {
        issue_builder = nebula::rt::json_array_push(std::move(issue_builder),
                                                    migration_summary_json(migration));
        history_issue_count += 1;
      }
      continue;
    }
    if (migration.version <= current_version) {
      issue_builder = nebula::rt::json_array_push(std::move(issue_builder),
                                                  migration_summary_json(migration));
      history_issue_count += 1;
    } else {
      pending_builder = nebula::rt::json_array_push(std::move(pending_builder),
                                                    migration_summary_json(migration));
      pending_count += 1;
    }
  }

  std::string_view status = "up_to_date";
  bool safe_to_apply = true;
  if (history_issue_count > 0) {
    status = "history_mismatch";
    safe_to_apply = false;
  } else if (pending_count > 0) {
    status = "pending";
  }

  return nebula::rt::ok_result(migration_status_json(current_version,
                                                     target_version,
                                                     pending_count,
                                                     history_issue_count,
                                                     safe_to_apply,
                                                     status,
                                                     nebula::rt::json_array_build(
                                                         std::move(pending_builder)),
                                                     nebula::rt::json_array_build(
                                                         std::move(issue_builder))));
}

nebula::rt::Result<void, std::string> record_migration(PGconn* conn, const MigrationSpec& migration) {
  auto builder = nebula::rt::json_array_builder();
  builder = nebula::rt::json_array_push(std::move(builder), nebula::rt::json_int_value(migration.version));
  builder = nebula::rt::json_array_push(std::move(builder), nebula::rt::json_string_value(migration.name));
  builder = nebula::rt::json_array_push(std::move(builder), nebula::rt::json_int_value(nebula::rt::unix_millis()));
  auto result = exec_params(conn,
                            "INSERT INTO __nebula_schema_migrations(version, name, applied_unix_ms) VALUES ($1, $2, $3)",
                            nebula::rt::json_array_build(std::move(builder)));
  if (nebula::rt::result_is_err(result)) {
    return err_void_result(nebula::rt::result_err_ref(result));
  }
  return ok_void_result();
}

} // namespace

using nebula::rt::JsonValue;
using nebula::rt::PostgresConnection;
using nebula::rt::PostgresTransaction;
using nebula::rt::PostgresResultSet;
using nebula::rt::PostgresRow;
using nebula::rt::Result;
using nebula::rt::json_array_build;
using nebula::rt::json_array_builder;
using nebula::rt::json_array_push;
using nebula::rt::json_bool_value;
using nebula::rt::json_int_value;
using nebula::rt::json_null_value;
using nebula::rt::json_object2;
using nebula::rt::json_object8;
using nebula::rt::json_parse;
using nebula::rt::json_string_value;
using nebula::rt::ok_result;
using nebula::rt::result_err_ref;
using nebula::rt::result_is_err;
using nebula::rt::result_ok_ref;

Result<bool, std::string> __nebula_postgres_probe_runtime() {
  return ensure_postgres_loaded();
}

Result<PostgresConnection, std::string> __nebula_postgres_connect(std::string dsn) {
  auto loaded = ensure_postgres_loaded();
  if (result_is_err(loaded)) {
    return err_result<PostgresConnection>(result_err_ref(loaded));
  }
  PGconn* conn = postgres_api().connectdb(dsn.c_str());
  if (conn == nullptr) {
    return err_result<PostgresConnection>("postgres connection allocation failed");
  }
  if (postgres_api().status(conn) != kConnectionOk) {
    std::string message = postgres_connection_error(conn, "connect postgres");
    postgres_api().finish(conn);
    return err_result<PostgresConnection>(std::move(message));
  }
  auto state = std::make_shared<nebula::rt::PostgresConnectionState>();
  state->conn = conn;
  return ok_result(PostgresConnection{std::move(state)});
}

Result<void, std::string> __nebula_postgres_connection_close(PostgresConnection self) {
  if (self.state == nullptr) return err_void_result("postgres connection is uninitialized");
  if (self.state->conn != nullptr) {
    postgres_api().finish(self.state->conn);
    self.state->conn = nullptr;
  }
  self.state.reset();
  return ok_void_result();
}

Result<PostgresTransaction, std::string> __nebula_postgres_connection_begin_transaction(PostgresConnection self) {
  std::string error;
  auto* conn = require_connection(self, error);
  if (conn == nullptr) return err_result<PostgresTransaction>(std::move(error));
  auto begin = exec_simple(conn, "BEGIN");
  if (result_is_err(begin)) {
    return err_result<PostgresTransaction>(result_err_ref(begin));
  }
  auto state = std::make_shared<nebula::rt::PostgresTransactionState>();
  state->connection = std::move(self.state);
  state->active = true;
  return ok_result(PostgresTransaction{std::move(state)});
}

Result<std::int64_t, std::string> __nebula_postgres_connection_execute(PostgresConnection self,
                                                                       std::string sql,
                                                                       JsonValue params) {
  std::string error;
  auto* conn = require_connection(self, error);
  if (conn == nullptr) return err_result<std::int64_t>(std::move(error));
  auto result = exec_params(conn, sql, std::move(params));
  if (result_is_err(result)) {
    return err_result<std::int64_t>(result_err_ref(result));
  }
  if (postgres_api().result_status(result_ok_ref(result).get()) == kPgTuplesOk) {
    return err_result<std::int64_t>("postgres execute returned rows; use query(...)");
  }
  return ok_result<std::int64_t>(command_rows_changed(result_ok_ref(result).get()));
}

Result<PostgresResultSet, std::string> __nebula_postgres_connection_query(PostgresConnection self,
                                                                          std::string sql,
                                                                          JsonValue params) {
  std::string error;
  auto* conn = require_connection(self, error);
  if (conn == nullptr) return err_result<PostgresResultSet>(std::move(error));
  return materialize_query(conn, sql, std::move(params));
}

Result<std::int64_t, std::string> __nebula_postgres_transaction_execute(PostgresTransaction self,
                                                                        std::string sql,
                                                                        JsonValue params) {
  std::string error;
  auto* conn = require_transaction_connection(self, error);
  if (conn == nullptr) return err_result<std::int64_t>(std::move(error));
  auto result = exec_params(conn, sql, std::move(params));
  if (result_is_err(result)) {
    return err_result<std::int64_t>(result_err_ref(result));
  }
  if (postgres_api().result_status(result_ok_ref(result).get()) == kPgTuplesOk) {
    return err_result<std::int64_t>("postgres execute returned rows; use query(...)");
  }
  return ok_result<std::int64_t>(command_rows_changed(result_ok_ref(result).get()));
}

Result<PostgresResultSet, std::string> __nebula_postgres_transaction_query(PostgresTransaction self,
                                                                           std::string sql,
                                                                           JsonValue params) {
  std::string error;
  auto* conn = require_transaction_connection(self, error);
  if (conn == nullptr) return err_result<PostgresResultSet>(std::move(error));
  return materialize_query(conn, sql, std::move(params));
}

Result<void, std::string> __nebula_postgres_transaction_commit(PostgresTransaction self) {
  std::string error;
  auto* conn = require_transaction_connection(self, error);
  if (conn == nullptr) return err_void_result(std::move(error));
  auto committed = exec_simple(conn, "COMMIT");
  if (result_is_err(committed)) {
    return err_void_result(result_err_ref(committed));
  }
  self.state->active = false;
  return ok_void_result();
}

Result<void, std::string> __nebula_postgres_transaction_rollback(PostgresTransaction self) {
  std::string error;
  auto* conn = require_transaction_connection(self, error);
  if (conn == nullptr) return err_void_result(std::move(error));
  auto rolled_back = exec_simple(conn, "ROLLBACK");
  if (result_is_err(rolled_back)) {
    return err_void_result(result_err_ref(rolled_back));
  }
  self.state->active = false;
  return ok_void_result();
}

Result<std::int64_t, std::string> __nebula_postgres_connection_run_migrations(PostgresConnection self,
                                                                              JsonValue migrations) {
  std::string error;
  auto* conn = require_connection(self, error);
  if (conn == nullptr) return err_result<std::int64_t>(std::move(error));
  auto decoded = decode_migrations(std::move(migrations));
  if (result_is_err(decoded)) {
    return err_result<std::int64_t>(result_err_ref(decoded));
  }
  const auto& items = result_ok_ref(decoded);
  if (items.empty()) return ok_result<std::int64_t>(0);
  auto current = current_schema_version(conn);
  if (result_is_err(current)) {
    return err_result<std::int64_t>(result_err_ref(current));
  }
  const auto current_version = result_ok_ref(current);
  std::int64_t applied = 0;
  for (const auto& migration : items) {
    if (migration.version <= current_version) {
      auto verified = verify_applied_migration(conn, migration);
      if (result_is_err(verified)) {
        return err_result<std::int64_t>(result_err_ref(verified));
      }
      continue;
    }
    auto begin = exec_simple(conn, "BEGIN");
    if (result_is_err(begin)) return err_result<std::int64_t>(result_err_ref(begin));
    auto applied_sql = exec_simple(conn, migration.sql);
    if (result_is_err(applied_sql)) {
      exec_simple(conn, "ROLLBACK");
      return err_result<std::int64_t>(result_err_ref(applied_sql));
    }
    auto recorded = record_migration(conn, migration);
    if (result_is_err(recorded)) {
      exec_simple(conn, "ROLLBACK");
      return err_result<std::int64_t>(result_err_ref(recorded));
    }
    auto committed = exec_simple(conn, "COMMIT");
    if (result_is_err(committed)) {
      exec_simple(conn, "ROLLBACK");
      return err_result<std::int64_t>(result_err_ref(committed));
    }
    applied += 1;
  }
  return ok_result<std::int64_t>(applied);
}

Result<JsonValue, std::string> __nebula_postgres_connection_migration_status(PostgresConnection self,
                                                                             JsonValue migrations) {
  std::string error;
  auto* conn = require_connection(self, error);
  if (conn == nullptr) return err_result<JsonValue>(std::move(error));
  auto decoded = decode_migrations(std::move(migrations));
  if (result_is_err(decoded)) {
    return err_result<JsonValue>(result_err_ref(decoded));
  }
  return build_migration_status(conn, result_ok_ref(decoded));
}

Result<std::int64_t, std::string> __nebula_postgres_result_set_len(PostgresResultSet self) {
  if (self.state == nullptr) return err_result<std::int64_t>("postgres result set is uninitialized");
  return ok_result<std::int64_t>(static_cast<std::int64_t>(self.state->rows.size()));
}

Result<PostgresRow, std::string> __nebula_postgres_result_set_row(PostgresResultSet self, std::int64_t index) {
  if (self.state == nullptr) return err_result<PostgresRow>("postgres result set is uninitialized");
  if (index < 0 || static_cast<std::size_t>(index) >= self.state->rows.size()) {
    return err_result<PostgresRow>("postgres row index is out of range");
  }
  return ok_result(PostgresRow{self.state, index});
}

Result<std::string, std::string> __nebula_postgres_row_get_string(PostgresRow self, std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<std::string>(std::move(error));
  if (cell->is_null) return err_result<std::string>("postgres column is null");
  return ok_result(cell->text);
}

Result<std::int64_t, std::string> __nebula_postgres_row_get_int(PostgresRow self, std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<std::int64_t>(std::move(error));
  if (cell->is_null) return err_result<std::int64_t>("postgres column is null");
  try {
    return ok_result<std::int64_t>(std::stoll(cell->text));
  } catch (...) {
    return err_result<std::int64_t>("postgres column is not an int-compatible value");
  }
}

Result<bool, std::string> __nebula_postgres_row_get_bool(PostgresRow self, std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<bool>(std::move(error));
  if (cell->is_null) return err_result<bool>("postgres column is null");
  if (cell->text == "t" || cell->text == "true" || cell->text == "1") return ok_result(true);
  if (cell->text == "f" || cell->text == "false" || cell->text == "0") return ok_result(false);
  return err_result<bool>("postgres column is not a bool-compatible value");
}

Result<JsonValue, std::string> __nebula_postgres_row_get_json(PostgresRow self, std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<JsonValue>(std::move(error));
  if (cell->is_null) return ok_result(json_null_value());
  return json_parse(cell->text);
}
