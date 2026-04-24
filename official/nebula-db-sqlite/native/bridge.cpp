#include "runtime/nebula_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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

struct sqlite3;
struct sqlite3_stmt;

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteInteger = 1;
constexpr int kSqliteFloat = 2;
constexpr int kSqliteText = 3;
constexpr int kSqliteBlob = 4;
constexpr int kSqliteNull = 5;

constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteOpenFullMutex = 0x00010000;

struct SqliteApi {
  bool loaded = false;
  std::string error;

#if defined(_WIN32)
  HMODULE handle = nullptr;
#else
  void* handle = nullptr;
#endif

  int (*open_v2)(const char*, sqlite3**, int, const char*) = nullptr;
  int (*close_v2)(sqlite3*) = nullptr;
  int (*exec)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**) = nullptr;
  const char* (*errmsg)(sqlite3*) = nullptr;
  int (*prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**) = nullptr;
  int (*finalize)(sqlite3_stmt*) = nullptr;
  int (*step)(sqlite3_stmt*) = nullptr;
  int (*reset)(sqlite3_stmt*) = nullptr;
  int (*bind_text)(sqlite3_stmt*, int, const char*, int, void (*)(void*)) = nullptr;
  int (*bind_int64)(sqlite3_stmt*, int, std::int64_t) = nullptr;
  int (*bind_null)(sqlite3_stmt*, int) = nullptr;
  int (*bind_blob)(sqlite3_stmt*, int, const void*, int, void (*)(void*)) = nullptr;
  int (*bind_parameter_count)(sqlite3_stmt*) = nullptr;
  int (*column_count)(sqlite3_stmt*) = nullptr;
  const char* (*column_name)(sqlite3_stmt*, int) = nullptr;
  int (*column_type)(sqlite3_stmt*, int) = nullptr;
  const unsigned char* (*column_text)(sqlite3_stmt*, int) = nullptr;
  std::int64_t (*column_int64)(sqlite3_stmt*, int) = nullptr;
  const void* (*column_blob)(sqlite3_stmt*, int) = nullptr;
  int (*column_bytes)(sqlite3_stmt*, int) = nullptr;
  int (*busy_timeout)(sqlite3*, int) = nullptr;
  std::int64_t (*changes64)(sqlite3*) = nullptr;
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

SqliteApi& sqlite_api() {
  static SqliteApi api;
  static std::once_flag once;
  std::call_once(once, [&] {
#if defined(_WIN32)
    const char* candidates[] = {"sqlite3.dll"};
    for (const char* candidate : candidates) {
      api.handle = LoadLibraryA(candidate);
      if (api.handle != nullptr) break;
    }
    if (api.handle == nullptr) {
      api.error = "sqlite3 dynamic library is unavailable";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(api.handle, symbol));
      return target != nullptr;
    };
#else
    const char* candidates[] = {"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib", "libsqlite3.so.0", "libsqlite3.so"};
    for (const char* candidate : candidates) {
      api.handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
      if (api.handle != nullptr) break;
    }
    if (api.handle == nullptr) {
      api.error = "sqlite3 dynamic library is unavailable";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(dlsym(api.handle, symbol));
      return target != nullptr;
    };
#endif

    if (!load(api.open_v2, "sqlite3_open_v2") || !load(api.close_v2, "sqlite3_close_v2") ||
        !load(api.exec, "sqlite3_exec") || !load(api.errmsg, "sqlite3_errmsg") ||
        !load(api.prepare_v2, "sqlite3_prepare_v2") || !load(api.finalize, "sqlite3_finalize") ||
        !load(api.step, "sqlite3_step") || !load(api.reset, "sqlite3_reset") ||
        !load(api.bind_text, "sqlite3_bind_text") || !load(api.bind_int64, "sqlite3_bind_int64") ||
        !load(api.bind_null, "sqlite3_bind_null") || !load(api.bind_blob, "sqlite3_bind_blob") ||
        !load(api.bind_parameter_count, "sqlite3_bind_parameter_count") ||
        !load(api.column_count, "sqlite3_column_count") || !load(api.column_name, "sqlite3_column_name") ||
        !load(api.column_type, "sqlite3_column_type") || !load(api.column_text, "sqlite3_column_text") ||
        !load(api.column_int64, "sqlite3_column_int64") || !load(api.column_blob, "sqlite3_column_blob") ||
        !load(api.column_bytes, "sqlite3_column_bytes") || !load(api.busy_timeout, "sqlite3_busy_timeout") ||
        !load(api.changes64, "sqlite3_changes64")) {
      api.error = "sqlite3 dynamic library is missing required symbols";
      return;
    }
    api.loaded = true;
  });
  return api;
}

void (*sqlite_transient_destructor())(void*) {
  return reinterpret_cast<void (*)(void*)>(-1);
}

nebula::rt::Result<bool, std::string> ensure_sqlite_loaded() {
  const auto& api = sqlite_api();
  if (!api.loaded) {
    return err_result<bool>(api.error.empty() ? "sqlite3 dynamic library is unavailable" : api.error);
  }
  return nebula::rt::ok_result<bool>(true);
}

std::string sqlite_error_message(sqlite3* db, std::string_view action) {
  std::string message(action);
  message += ": ";
  if (db == nullptr || sqlite_api().errmsg == nullptr) {
    message += "sqlite error";
    return message;
  }
  message += sqlite_api().errmsg(db);
  return message;
}

nebula::rt::Result<void, std::string> exec_sql(sqlite3* db, std::string_view sql) {
  if (db == nullptr) return err_void_result("sqlite connection is closed");
  const int rc = sqlite_api().exec(db, std::string(sql).c_str(), nullptr, nullptr, nullptr);
  if (rc != kSqliteOk) {
    return err_void_result(sqlite_error_message(db, sql));
  }
  return ok_void_result();
}

class Statement {
  sqlite3_stmt* stmt_ = nullptr;

public:
  Statement() = default;
  explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement(Statement&& other) noexcept : stmt_(std::exchange(other.stmt_, nullptr)) {}

  Statement& operator=(Statement&& other) noexcept {
    if (this == &other) return *this;
    reset();
    stmt_ = std::exchange(other.stmt_, nullptr);
    return *this;
  }

  ~Statement() { reset(); }

  sqlite3_stmt* get() const { return stmt_; }

  void reset() {
    if (stmt_ == nullptr) return;
    sqlite_api().finalize(stmt_);
    stmt_ = nullptr;
  }
};

template <typename T>
nebula::rt::Result<T, std::string> err_closed(std::string_view label) {
  return err_result<T>(std::string(label) + " is closed");
}

nebula::rt::Result<Statement, std::string> prepare_statement(sqlite3* db, std::string_view sql) {
  if (db == nullptr) return err_result<Statement>("sqlite connection is closed");
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite_api().prepare_v2(db, std::string(sql).c_str(), -1, &stmt, nullptr);
  if (rc != kSqliteOk) {
    return err_result<Statement>(sqlite_error_message(db, "prepare statement"));
  }
  return nebula::rt::ok_result(Statement(stmt));
}

nebula::rt::JsonValue empty_params_json() {
  return nebula::rt::json_array_build(nebula::rt::json_array_builder());
}

std::string safe_text_column(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite_api().column_text(stmt, index);
  const int bytes = sqlite_api().column_bytes(stmt, index);
  if (text == nullptr || bytes <= 0) return "";
  return std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
}

nebula::rt::Result<void, std::string> bind_single_param(sqlite3_stmt* stmt,
                                                        int index,
                                                        const nebula::rt::JsonValue& value) {
  const auto& api = sqlite_api();
  using nebula::rt::JsonValueKind;
  switch (value.parsed.kind) {
  case JsonValueKind::String: {
    auto text = nebula::rt::json_as_string(value);
    if (nebula::rt::result_is_err(text)) {
      return err_void_result(nebula::rt::result_err_ref(text));
    }
    const auto& bound = nebula::rt::result_ok_ref(text);
    if (api.bind_text(stmt,
                      index,
                      bound.c_str(),
                      static_cast<int>(bound.size()),
                      sqlite_transient_destructor()) != kSqliteOk) {
      return err_void_result("failed to bind sqlite string parameter");
    }
    return ok_void_result();
  }
  case JsonValueKind::Int:
    if (api.bind_int64(stmt, index, value.parsed.int_value) != kSqliteOk) {
      return err_void_result("failed to bind sqlite int parameter");
    }
    return ok_void_result();
  case JsonValueKind::Bool:
    if (api.bind_int64(stmt, index, value.parsed.bool_value ? 1 : 0) != kSqliteOk) {
      return err_void_result("failed to bind sqlite bool parameter");
    }
    return ok_void_result();
  case JsonValueKind::Null:
    if (api.bind_null(stmt, index) != kSqliteOk) {
      return err_void_result("failed to bind sqlite null parameter");
    }
    return ok_void_result();
  case JsonValueKind::Object:
  case JsonValueKind::Array: {
    const std::string text = nebula::rt::json_stringify(value);
    if (api.bind_text(stmt,
                      index,
                      text.c_str(),
                      static_cast<int>(text.size()),
                      sqlite_transient_destructor()) != kSqliteOk) {
      return err_void_result("failed to bind sqlite json-text parameter");
    }
    return ok_void_result();
  }
  default:
    return err_void_result("unsupported sqlite parameter kind");
  }
}

nebula::rt::Result<void, std::string> bind_params(sqlite3_stmt* stmt, nebula::rt::JsonValue params) {
  auto array_len = nebula::rt::json_array_len(params);
  if (nebula::rt::result_is_err(array_len)) {
    return err_void_result("sqlite params must be a JSON array");
  }
  const auto params_len = nebula::rt::result_ok_ref(array_len);
  const int expected = sqlite_api().bind_parameter_count(stmt);
  if (expected != params_len) {
    return err_void_result("sqlite parameter count mismatch");
  }
  for (int i = 0; i < expected; ++i) {
    auto item = nebula::rt::json_array_get(params, i);
    if (nebula::rt::result_is_err(item)) {
      return err_void_result(nebula::rt::result_err_ref(item));
    }
    auto bound = bind_single_param(stmt, i + 1, nebula::rt::result_ok_ref(item));
    if (nebula::rt::result_is_err(bound)) {
      return bound;
    }
  }
  return ok_void_result();
}

nebula::rt::Result<std::int64_t, std::string> execute_statement(sqlite3* db,
                                                                std::string_view sql,
                                                                nebula::rt::JsonValue params) {
  auto stmt = prepare_statement(db, sql);
  if (nebula::rt::result_is_err(stmt)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(stmt));
  }
  auto bound = bind_params(nebula::rt::result_ok_ref(stmt).get(), std::move(params));
  if (nebula::rt::result_is_err(bound)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(bound));
  }
  const int rc = sqlite_api().step(nebula::rt::result_ok_ref(stmt).get());
  if (rc == kSqliteRow) {
    return err_result<std::int64_t>("sqlite execute returned rows; use query(...)");
  }
  if (rc != kSqliteDone) {
    return err_result<std::int64_t>(sqlite_error_message(db, "step statement"));
  }
  return nebula::rt::ok_result<std::int64_t>(sqlite_api().changes64(db));
}

} // namespace

namespace nebula::rt {

struct SqliteCell {
  enum class Kind : std::uint8_t { Null, Int, Text, Blob };
  Kind kind = Kind::Null;
  std::int64_t int_value = 0;
  std::string text_value;
  Bytes bytes_value;
};

struct SqliteColumnValue {
  std::string name;
  SqliteCell value;
};

struct SqliteResultRowData {
  std::vector<SqliteColumnValue> columns;
};

struct SqliteConnectionState {
  sqlite3* db = nullptr;
  std::string path;

  ~SqliteConnectionState() {
    if (db != nullptr) sqlite_api().close_v2(db);
  }
};

struct SqliteTransactionState {
  std::shared_ptr<SqliteConnectionState> connection;
  bool active = false;

  ~SqliteTransactionState() {
    if (active && connection != nullptr && connection->db != nullptr) {
      exec_sql(connection->db, "ROLLBACK;");
    }
  }
};

struct SqliteResultSetState {
  std::vector<SqliteResultRowData> rows;
};

} // namespace nebula::rt

namespace {

nebula::rt::Result<nebula::rt::SqliteCell, std::string> read_cell(sqlite3_stmt* stmt, int index) {
  using Cell = nebula::rt::SqliteCell;
  Cell cell;
  const int kind = sqlite_api().column_type(stmt, index);
  if (kind == kSqliteNull) {
    return nebula::rt::ok_result(cell);
  }
  if (kind == kSqliteInteger) {
    cell.kind = Cell::Kind::Int;
    cell.int_value = sqlite_api().column_int64(stmt, index);
    return nebula::rt::ok_result(cell);
  }
  if (kind == kSqliteText || kind == kSqliteFloat) {
    cell.kind = Cell::Kind::Text;
    cell.text_value = safe_text_column(stmt, index);
    return nebula::rt::ok_result(cell);
  }
  if (kind == kSqliteBlob) {
    cell.kind = Cell::Kind::Blob;
    const void* blob = sqlite_api().column_blob(stmt, index);
    const int bytes = sqlite_api().column_bytes(stmt, index);
    if (blob != nullptr && bytes > 0) {
      cell.bytes_value = nebula::rt::Bytes(std::string(reinterpret_cast<const char*>(blob),
                                                       static_cast<std::size_t>(bytes)));
    }
    return nebula::rt::ok_result(cell);
  }
  return err_result<nebula::rt::SqliteCell>("unsupported sqlite column type");
}

nebula::rt::Result<nebula::rt::SqliteResultSet, std::string> query_statement(sqlite3* db,
                                                                              std::string_view sql,
                                                                              nebula::rt::JsonValue params) {
  auto stmt = prepare_statement(db, sql);
  if (nebula::rt::result_is_err(stmt)) {
    return err_result<nebula::rt::SqliteResultSet>(nebula::rt::result_err_ref(stmt));
  }
  auto bound = bind_params(nebula::rt::result_ok_ref(stmt).get(), std::move(params));
  if (nebula::rt::result_is_err(bound)) {
    return err_result<nebula::rt::SqliteResultSet>(nebula::rt::result_err_ref(bound));
  }

  auto state = std::make_shared<nebula::rt::SqliteResultSetState>();
  while (true) {
    const int rc = sqlite_api().step(nebula::rt::result_ok_ref(stmt).get());
    if (rc == kSqliteDone) break;
    if (rc != kSqliteRow) {
      return err_result<nebula::rt::SqliteResultSet>(sqlite_error_message(db, "step query"));
    }
    nebula::rt::SqliteResultRowData row;
    const int column_count = sqlite_api().column_count(nebula::rt::result_ok_ref(stmt).get());
    row.columns.reserve(static_cast<std::size_t>(column_count));
    for (int i = 0; i < column_count; ++i) {
      auto cell = read_cell(nebula::rt::result_ok_ref(stmt).get(), i);
      if (nebula::rt::result_is_err(cell)) {
        return err_result<nebula::rt::SqliteResultSet>(nebula::rt::result_err_ref(cell));
      }
      nebula::rt::SqliteColumnValue column;
      const char* name = sqlite_api().column_name(nebula::rt::result_ok_ref(stmt).get(), i);
      column.name = name == nullptr ? "" : std::string(name);
      column.value = std::move(nebula::rt::result_ok_ref(cell));
      row.columns.push_back(std::move(column));
    }
    state->rows.push_back(std::move(row));
  }
  return nebula::rt::ok_result(nebula::rt::SqliteResultSet{std::move(state)});
}

const nebula::rt::SqliteResultRowData* require_row(const nebula::rt::SqliteRow& row, std::string& error) {
  if (row.state == nullptr) {
    error = "sqlite row is uninitialized";
    return nullptr;
  }
  if (row.index < 0 || static_cast<std::size_t>(row.index) >= row.state->rows.size()) {
    error = "sqlite row index is out of range";
    return nullptr;
  }
  return &row.state->rows[static_cast<std::size_t>(row.index)];
}

const nebula::rt::SqliteCell* find_cell(const nebula::rt::SqliteRow& row,
                                        std::string_view column,
                                        std::string& error) {
  const auto* row_data = require_row(row, error);
  if (row_data == nullptr) return nullptr;
  for (const auto& value : row_data->columns) {
    if (value.name == column) return &value.value;
  }
  error = "sqlite column not found: " + std::string(column);
  return nullptr;
}

nebula::rt::Result<std::int64_t, std::string> current_schema_version(sqlite3* db) {
  auto ensure_table = exec_sql(db,
                               "CREATE TABLE IF NOT EXISTS __nebula_schema_migrations ("
                               "version INTEGER PRIMARY KEY,"
                               "name TEXT NOT NULL,"
                               "applied_unix_ms INTEGER NOT NULL"
                               ");");
  if (nebula::rt::result_is_err(ensure_table)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(ensure_table));
  }
  auto stmt = prepare_statement(db, "SELECT COALESCE(MAX(version), 0) FROM __nebula_schema_migrations;");
  if (nebula::rt::result_is_err(stmt)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(stmt));
  }
  const int rc = sqlite_api().step(nebula::rt::result_ok_ref(stmt).get());
  if (rc != kSqliteRow) {
    return err_result<std::int64_t>(sqlite_error_message(db, "read schema version"));
  }
  return nebula::rt::ok_result<std::int64_t>(sqlite_api().column_int64(nebula::rt::result_ok_ref(stmt).get(), 0));
}

struct MigrationSpec {
  std::int64_t version = 0;
  std::string name;
  std::string sql;
};

nebula::rt::Result<std::vector<MigrationSpec>, std::string> decode_migrations(nebula::rt::JsonValue migrations) {
  auto count = nebula::rt::json_array_len(migrations);
  if (nebula::rt::result_is_err(count)) {
    return err_result<std::vector<MigrationSpec>>("sqlite migrations must be a JSON array");
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
      return err_result<std::vector<MigrationSpec>>("sqlite migration entries require version/name/sql");
    }
    if (nebula::rt::result_ok_ref(version) <= 0) {
      return err_result<std::vector<MigrationSpec>>("sqlite migration version must be positive");
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
      return err_result<std::vector<MigrationSpec>>("sqlite migration versions must be unique");
    }
  }
  return nebula::rt::ok_result(std::move(out));
}

nebula::rt::Result<std::optional<std::string>, std::string> applied_migration_name(sqlite3* db,
                                                                                   std::int64_t version) {
  auto stmt = prepare_statement(db, "SELECT name FROM __nebula_schema_migrations WHERE version = ?;");
  if (nebula::rt::result_is_err(stmt)) {
    return err_result<std::optional<std::string>>(nebula::rt::result_err_ref(stmt));
  }
  if (sqlite_api().bind_int64(nebula::rt::result_ok_ref(stmt).get(), 1, version) != kSqliteOk) {
    return err_result<std::optional<std::string>>("failed to bind sqlite migration version");
  }
  const int rc = sqlite_api().step(nebula::rt::result_ok_ref(stmt).get());
  if (rc == kSqliteDone) {
    return nebula::rt::ok_result<std::optional<std::string>>(std::nullopt);
  }
  if (rc != kSqliteRow) {
    return err_result<std::optional<std::string>>(sqlite_error_message(db, "read migration metadata"));
  }
  const unsigned char* raw = sqlite_api().column_text(nebula::rt::result_ok_ref(stmt).get(), 0);
  if (raw == nullptr) {
    return err_result<std::optional<std::string>>("sqlite migration name was not returned");
  }
  return nebula::rt::ok_result<std::optional<std::string>>(
      std::string(reinterpret_cast<const char*>(raw)));
}

nebula::rt::Result<bool, std::string> verify_applied_migration(sqlite3* db,
                                                               const MigrationSpec& migration) {
  auto applied = applied_migration_name(db, migration.version);
  if (nebula::rt::result_is_err(applied)) {
    return err_result<bool>(nebula::rt::result_err_ref(applied));
  }
  if (!nebula::rt::result_ok_ref(applied).has_value()) {
    return err_result<bool>("sqlite migration history missing version " +
                            std::to_string(migration.version));
  }
  if (*nebula::rt::result_ok_ref(applied) != migration.name) {
    return err_result<bool>("sqlite migration history mismatch for version " +
                            std::to_string(migration.version));
  }
  return nebula::rt::ok_result<bool>(true);
}

nebula::rt::Result<void, std::string> record_migration(sqlite3* db, const MigrationSpec& migration) {
  auto stmt = prepare_statement(db,
                                "INSERT INTO __nebula_schema_migrations(version, name, applied_unix_ms) "
                                "VALUES (?, ?, ?);");
  if (nebula::rt::result_is_err(stmt)) {
    return err_void_result(nebula::rt::result_err_ref(stmt));
  }
  if (sqlite_api().bind_int64(nebula::rt::result_ok_ref(stmt).get(), 1, migration.version) != kSqliteOk ||
      sqlite_api().bind_text(nebula::rt::result_ok_ref(stmt).get(),
                             2,
                             migration.name.c_str(),
                             static_cast<int>(migration.name.size()),
                             sqlite_transient_destructor()) != kSqliteOk ||
      sqlite_api().bind_int64(nebula::rt::result_ok_ref(stmt).get(), 3, nebula::rt::unix_millis()) != kSqliteOk) {
    return err_void_result("failed to bind sqlite migration metadata");
  }
  const int rc = sqlite_api().step(nebula::rt::result_ok_ref(stmt).get());
  if (rc != kSqliteDone) {
    return err_void_result(sqlite_error_message(db, "record migration"));
  }
  return ok_void_result();
}

nebula::rt::Result<std::shared_ptr<nebula::rt::SqliteConnectionState>, std::string> open_connection_state(
    std::string path) {
  auto loaded = ensure_sqlite_loaded();
  if (nebula::rt::result_is_err(loaded)) {
    return err_result<std::shared_ptr<nebula::rt::SqliteConnectionState>>(nebula::rt::result_err_ref(loaded));
  }
  std::error_code ec;
  const auto db_path = std::filesystem::path(path);
  if (db_path.has_parent_path()) {
    std::filesystem::create_directories(db_path.parent_path(), ec);
    if (ec) {
      return err_result<std::shared_ptr<nebula::rt::SqliteConnectionState>>(
          "failed to create sqlite parent directories: " + db_path.parent_path().string() + " (" + ec.message() + ")");
    }
  }

  sqlite3* db = nullptr;
  const int flags = kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex;
  const int rc = sqlite_api().open_v2(path.c_str(), &db, flags, nullptr);
  if (rc != kSqliteOk || db == nullptr) {
    std::string message = sqlite_error_message(db, "open sqlite database");
    if (db != nullptr) sqlite_api().close_v2(db);
    return err_result<std::shared_ptr<nebula::rt::SqliteConnectionState>>(std::move(message));
  }
  sqlite_api().busy_timeout(db, 5000);

  auto wal = exec_sql(db, "PRAGMA journal_mode=WAL;");
  if (nebula::rt::result_is_err(wal)) {
    sqlite_api().close_v2(db);
    return err_result<std::shared_ptr<nebula::rt::SqliteConnectionState>>(nebula::rt::result_err_ref(wal));
  }
  auto sync = exec_sql(db, "PRAGMA synchronous=NORMAL;");
  if (nebula::rt::result_is_err(sync)) {
    sqlite_api().close_v2(db);
    return err_result<std::shared_ptr<nebula::rt::SqliteConnectionState>>(nebula::rt::result_err_ref(sync));
  }

  auto state = std::make_shared<nebula::rt::SqliteConnectionState>();
  state->db = db;
  state->path = std::move(path);
  return nebula::rt::ok_result(std::move(state));
}

sqlite3* require_connection_db(const nebula::rt::SqliteConnection& self, std::string& error) {
  if (self.state == nullptr) {
    error = "sqlite connection is uninitialized";
    return nullptr;
  }
  if (self.state->db == nullptr) {
    error = "sqlite connection is closed";
    return nullptr;
  }
  return self.state->db;
}

sqlite3* require_transaction_db(const nebula::rt::SqliteTransaction& self, std::string& error) {
  if (self.state == nullptr || self.state->connection == nullptr) {
    error = "sqlite transaction is uninitialized";
    return nullptr;
  }
  if (self.state->connection->db == nullptr) {
    error = "sqlite transaction connection is closed";
    return nullptr;
  }
  if (!self.state->active) {
    error = "sqlite transaction is inactive";
    return nullptr;
  }
  return self.state->connection->db;
}

} // namespace

nebula::rt::Result<nebula::rt::SqliteConnection, std::string> __nebula_sqlite_open(std::string path) {
  auto state = open_connection_state(std::move(path));
  if (nebula::rt::result_is_err(state)) {
    return err_result<nebula::rt::SqliteConnection>(nebula::rt::result_err_ref(state));
  }
  return nebula::rt::ok_result(nebula::rt::SqliteConnection{std::move(nebula::rt::result_ok_ref(state))});
}

nebula::rt::Result<void, std::string> __nebula_sqlite_connection_close(nebula::rt::SqliteConnection self) {
  if (self.state == nullptr) return err_void_result("sqlite connection is uninitialized");
  if (self.state->db != nullptr) {
    sqlite_api().close_v2(self.state->db);
    self.state->db = nullptr;
  }
  self.state.reset();
  return ok_void_result();
}

nebula::rt::Result<nebula::rt::SqliteTransaction, std::string>
__nebula_sqlite_connection_begin_transaction(nebula::rt::SqliteConnection self) {
  std::string error;
  auto* db = require_connection_db(self, error);
  if (db == nullptr) return err_result<nebula::rt::SqliteTransaction>(std::move(error));
  auto begin = exec_sql(db, "BEGIN IMMEDIATE;");
  if (nebula::rt::result_is_err(begin)) {
    return err_result<nebula::rt::SqliteTransaction>(nebula::rt::result_err_ref(begin));
  }
  auto state = std::make_shared<nebula::rt::SqliteTransactionState>();
  state->connection = std::move(self.state);
  state->active = true;
  return nebula::rt::ok_result(nebula::rt::SqliteTransaction{std::move(state)});
}

nebula::rt::Result<std::int64_t, std::string>
__nebula_sqlite_connection_execute(nebula::rt::SqliteConnection self,
                                   std::string sql,
                                   nebula::rt::JsonValue params) {
  std::string error;
  auto* db = require_connection_db(self, error);
  if (db == nullptr) return err_result<std::int64_t>(std::move(error));
  return execute_statement(db, sql, std::move(params));
}

nebula::rt::Result<nebula::rt::SqliteResultSet, std::string>
__nebula_sqlite_connection_query(nebula::rt::SqliteConnection self,
                                 std::string sql,
                                 nebula::rt::JsonValue params) {
  std::string error;
  auto* db = require_connection_db(self, error);
  if (db == nullptr) return err_result<nebula::rt::SqliteResultSet>(std::move(error));
  return query_statement(db, sql, std::move(params));
}

nebula::rt::Result<std::int64_t, std::string>
__nebula_sqlite_connection_run_migrations(nebula::rt::SqliteConnection self,
                                          nebula::rt::JsonValue migrations) {
  std::string error;
  auto* db = require_connection_db(self, error);
  if (db == nullptr) return err_result<std::int64_t>(std::move(error));

  auto decoded = decode_migrations(std::move(migrations));
  if (nebula::rt::result_is_err(decoded)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(decoded));
  }
  const auto& items = nebula::rt::result_ok_ref(decoded);
  if (items.empty()) return nebula::rt::ok_result<std::int64_t>(0);

  auto current = current_schema_version(db);
  if (nebula::rt::result_is_err(current)) {
    return err_result<std::int64_t>(nebula::rt::result_err_ref(current));
  }
  const auto current_version = nebula::rt::result_ok_ref(current);

  std::int64_t applied = 0;
  for (const auto& migration : items) {
    if (migration.version <= current_version) {
      auto verified = verify_applied_migration(db, migration);
      if (nebula::rt::result_is_err(verified)) {
        return err_result<std::int64_t>(nebula::rt::result_err_ref(verified));
      }
      continue;
    }
    auto begin = exec_sql(db, "BEGIN IMMEDIATE;");
    if (nebula::rt::result_is_err(begin)) {
      return err_result<std::int64_t>(nebula::rt::result_err_ref(begin));
    }
    auto applied_sql = exec_sql(db, migration.sql);
    if (nebula::rt::result_is_err(applied_sql)) {
      exec_sql(db, "ROLLBACK;");
      return err_result<std::int64_t>(nebula::rt::result_err_ref(applied_sql));
    }
    auto recorded = record_migration(db, migration);
    if (nebula::rt::result_is_err(recorded)) {
      exec_sql(db, "ROLLBACK;");
      return err_result<std::int64_t>(nebula::rt::result_err_ref(recorded));
    }
    auto committed = exec_sql(db, "COMMIT;");
    if (nebula::rt::result_is_err(committed)) {
      exec_sql(db, "ROLLBACK;");
      return err_result<std::int64_t>(nebula::rt::result_err_ref(committed));
    }
    applied += 1;
  }
  return nebula::rt::ok_result<std::int64_t>(applied);
}

nebula::rt::Result<std::int64_t, std::string>
__nebula_sqlite_transaction_execute(nebula::rt::SqliteTransaction self,
                                    std::string sql,
                                    nebula::rt::JsonValue params) {
  std::string error;
  auto* db = require_transaction_db(self, error);
  if (db == nullptr) return err_result<std::int64_t>(std::move(error));
  return execute_statement(db, sql, std::move(params));
}

nebula::rt::Result<nebula::rt::SqliteResultSet, std::string>
__nebula_sqlite_transaction_query(nebula::rt::SqliteTransaction self,
                                  std::string sql,
                                  nebula::rt::JsonValue params) {
  std::string error;
  auto* db = require_transaction_db(self, error);
  if (db == nullptr) return err_result<nebula::rt::SqliteResultSet>(std::move(error));
  return query_statement(db, sql, std::move(params));
}

nebula::rt::Result<void, std::string> __nebula_sqlite_transaction_commit(nebula::rt::SqliteTransaction self) {
  std::string error;
  auto* db = require_transaction_db(self, error);
  if (db == nullptr) return err_void_result(std::move(error));
  auto committed = exec_sql(db, "COMMIT;");
  if (nebula::rt::result_is_err(committed)) {
    return committed;
  }
  self.state->active = false;
  return ok_void_result();
}

nebula::rt::Result<void, std::string> __nebula_sqlite_transaction_rollback(nebula::rt::SqliteTransaction self) {
  std::string error;
  auto* db = require_transaction_db(self, error);
  if (db == nullptr) return err_void_result(std::move(error));
  auto rolled_back = exec_sql(db, "ROLLBACK;");
  if (nebula::rt::result_is_err(rolled_back)) {
    return rolled_back;
  }
  self.state->active = false;
  return ok_void_result();
}

nebula::rt::Result<std::int64_t, std::string> __nebula_sqlite_result_set_len(nebula::rt::SqliteResultSet self) {
  if (self.state == nullptr) return err_result<std::int64_t>("sqlite result set is uninitialized");
  return nebula::rt::ok_result<std::int64_t>(static_cast<std::int64_t>(self.state->rows.size()));
}

nebula::rt::Result<nebula::rt::SqliteRow, std::string>
__nebula_sqlite_result_set_row(nebula::rt::SqliteResultSet self, std::int64_t index) {
  if (self.state == nullptr) return err_result<nebula::rt::SqliteRow>("sqlite result set is uninitialized");
  if (index < 0 || static_cast<std::size_t>(index) >= self.state->rows.size()) {
    return err_result<nebula::rt::SqliteRow>("sqlite row index is out of range");
  }
  return nebula::rt::ok_result(nebula::rt::SqliteRow{self.state, index});
}

nebula::rt::Result<std::string, std::string> __nebula_sqlite_row_get_string(nebula::rt::SqliteRow self,
                                                                             std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<std::string>(std::move(error));
  if (cell->kind != nebula::rt::SqliteCell::Kind::Text) {
    return err_result<std::string>("sqlite column is not a string/text value");
  }
  return nebula::rt::ok_result(cell->text_value);
}

nebula::rt::Result<std::int64_t, std::string> __nebula_sqlite_row_get_int(nebula::rt::SqliteRow self,
                                                                           std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<std::int64_t>(std::move(error));
  if (cell->kind != nebula::rt::SqliteCell::Kind::Int) {
    return err_result<std::int64_t>("sqlite column is not an int value");
  }
  return nebula::rt::ok_result<std::int64_t>(cell->int_value);
}

nebula::rt::Result<bool, std::string> __nebula_sqlite_row_get_bool(nebula::rt::SqliteRow self,
                                                                    std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<bool>(std::move(error));
  if (cell->kind == nebula::rt::SqliteCell::Kind::Int) {
    if (cell->int_value == 0) return nebula::rt::ok_result(false);
    if (cell->int_value == 1) return nebula::rt::ok_result(true);
  }
  if (cell->kind == nebula::rt::SqliteCell::Kind::Text) {
    if (cell->text_value == "false") return nebula::rt::ok_result(false);
    if (cell->text_value == "true") return nebula::rt::ok_result(true);
  }
  return err_result<bool>("sqlite column is not a bool-compatible value");
}

nebula::rt::Result<nebula::rt::JsonValue, std::string> __nebula_sqlite_row_get_json(nebula::rt::SqliteRow self,
                                                                                      std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<nebula::rt::JsonValue>(std::move(error));
  if (cell->kind == nebula::rt::SqliteCell::Kind::Text) {
    return nebula::rt::json_parse(cell->text_value);
  }
  if (cell->kind == nebula::rt::SqliteCell::Kind::Int) {
    return nebula::rt::ok_result(nebula::rt::json_int_value(cell->int_value));
  }
  if (cell->kind == nebula::rt::SqliteCell::Kind::Null) {
    return nebula::rt::ok_result(nebula::rt::json_null_value());
  }
  return err_result<nebula::rt::JsonValue>("sqlite column is not a json-compatible value");
}

nebula::rt::Result<nebula::rt::Bytes, std::string> __nebula_sqlite_row_get_bytes(nebula::rt::SqliteRow self,
                                                                                   std::string column) {
  std::string error;
  const auto* cell = find_cell(self, column, error);
  if (cell == nullptr) return err_result<nebula::rt::Bytes>(std::move(error));
  if (cell->kind != nebula::rt::SqliteCell::Kind::Blob) {
    return err_result<nebula::rt::Bytes>("sqlite column is not a blob value");
  }
  return nebula::rt::ok_result(cell->bytes_value);
}
