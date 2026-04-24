#include "runtime/nebula_runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
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
constexpr int kSqliteText = 3;

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
  int (*bind_text)(sqlite3_stmt*, int, const char*, int, void (*)(void*)) = nullptr;
  int (*bind_int64)(sqlite3_stmt*, int, std::int64_t) = nullptr;
  const unsigned char* (*column_text)(sqlite3_stmt*, int) = nullptr;
  std::int64_t (*column_int64)(sqlite3_stmt*, int) = nullptr;
  int (*column_type)(sqlite3_stmt*, int) = nullptr;
  int (*busy_timeout)(sqlite3*, int) = nullptr;
};

#define RETURN_IF_ERR_AS(Type, expr)                                                             \
  do {                                                                                           \
    auto _nebula_res = (expr);                                                                   \
    if (nebula::rt::result_is_err(_nebula_res)) {                                                \
      return nebula::rt::err_result<Type>(nebula::rt::result_err_ref(_nebula_res));             \
    }                                                                                            \
  } while (false)

#define ASSIGN_OR_RETURN_AS(Type, name, expr)                                                    \
  auto name##_result = (expr);                                                                   \
  if (nebula::rt::result_is_err(name##_result)) {                                                \
    return nebula::rt::err_result<Type>(nebula::rt::result_err_ref(name##_result));             \
  }                                                                                              \
  auto name = std::move(nebula::rt::result_ok_ref(name##_result))

SqliteApi& sqlite_api() {
  static SqliteApi api;
  static std::once_flag once;
  auto& state = api;
  std::call_once(once, [&] {
#if defined(_WIN32)
    const char* candidates[] = {"sqlite3.dll"};
    for (const char* candidate : candidates) {
      state.handle = LoadLibraryA(candidate);
      if (state.handle != nullptr) break;
    }
    if (state.handle == nullptr) {
      state.error = "sqlite3 dynamic library is unavailable";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(GetProcAddress(state.handle, symbol));
      return target != nullptr;
    };
#else
    const char* candidates[] = {"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib", "libsqlite3.so.0", "libsqlite3.so"};
    for (const char* candidate : candidates) {
      state.handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
      if (state.handle != nullptr) break;
    }
    if (state.handle == nullptr) {
      state.error = "sqlite3 dynamic library is unavailable";
      return;
    }
    auto load = [&](auto& target, const char* symbol) {
      target = reinterpret_cast<std::decay_t<decltype(target)>>(dlsym(state.handle, symbol));
      return target != nullptr;
    };
#endif

    if (!load(state.open_v2, "sqlite3_open_v2") || !load(state.close_v2, "sqlite3_close_v2") ||
        !load(state.exec, "sqlite3_exec") || !load(state.errmsg, "sqlite3_errmsg") ||
        !load(state.prepare_v2, "sqlite3_prepare_v2") || !load(state.finalize, "sqlite3_finalize") ||
        !load(state.step, "sqlite3_step") || !load(state.bind_text, "sqlite3_bind_text") ||
        !load(state.bind_int64, "sqlite3_bind_int64") || !load(state.column_text, "sqlite3_column_text") ||
        !load(state.column_int64, "sqlite3_column_int64") || !load(state.column_type, "sqlite3_column_type") ||
        !load(state.busy_timeout, "sqlite3_busy_timeout")) {
      state.error = "sqlite3 dynamic library is missing required symbols";
      return;
    }
    state.loaded = true;
  });
  return api;
}

using ResultBool = nebula::rt::Result<bool, std::string>;

std::string sqlite_error_message(sqlite3* db, std::string_view action) {
  const auto& api = sqlite_api();
  std::string message(action);
  message += ": ";
  if (db == nullptr || api.errmsg == nullptr) {
    message += "sqlite error";
    return message;
  }
  message += api.errmsg(db);
  return message;
}

nebula::rt::Result<bool, std::string> ensure_sqlite_loaded() {
  const auto& api = sqlite_api();
  if (!api.loaded) {
    return nebula::rt::err_result<bool>(api.error.empty() ? "sqlite3 dynamic library is unavailable" : api.error);
  }
  return nebula::rt::ok_result(true);
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.push_back('"');
  for (unsigned char ch : text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch < 0x20) {
        static const char* hex = "0123456789abcdef";
        out += "\\u00";
        out.push_back(hex[(ch >> 4) & 0x0f]);
        out.push_back(hex[ch & 0x0f]);
      } else {
        out.push_back(static_cast<char>(ch));
      }
      break;
    }
  }
  out.push_back('"');
  return out;
}

std::string json_bool(bool value) {
  return value ? "true" : "false";
}

struct ConfigRecord {
  std::string project;
  std::string environment;
  std::string key;
  std::string description;
  std::string kind;
  std::string string_value;
  bool bool_value = false;
  std::int64_t int_value = 0;
  std::int64_t revision_id = 0;
  std::string updated_by;
  std::int64_t updated_unix_ms = 0;
};

struct RevisionRecord {
  std::int64_t revision_id = 0;
  std::string action;
  ConfigRecord record;
};

std::string config_value_json(const ConfigRecord& record) {
  std::ostringstream out;
  out << "{"
      << "\"kind\":" << json_escape(record.kind) << ","
      << "\"string_value\":" << json_escape(record.string_value) << ","
      << "\"bool_value\":" << json_bool(record.bool_value) << ","
      << "\"int_value\":" << record.int_value << "}";
  return out.str();
}

std::string config_entry_json(const ConfigRecord& record) {
  std::ostringstream out;
  out << "{"
      << "\"project\":" << json_escape(record.project) << ","
      << "\"environment\":" << json_escape(record.environment) << ","
      << "\"key\":" << json_escape(record.key) << ","
      << "\"description\":" << json_escape(record.description) << ","
      << "\"value\":" << config_value_json(record) << ","
      << "\"revision_id\":" << record.revision_id << ","
      << "\"meta\":{"
      << "\"updated_by\":" << json_escape(record.updated_by) << ","
      << "\"updated_unix_ms\":" << record.updated_unix_ms
      << "}"
      << "}";
  return out.str();
}

std::string config_revision_json(const RevisionRecord& revision) {
  std::ostringstream out;
  out << "{"
      << "\"revision_id\":" << revision.revision_id << ","
      << "\"project\":" << json_escape(revision.record.project) << ","
      << "\"environment\":" << json_escape(revision.record.environment) << ","
      << "\"key\":" << json_escape(revision.record.key) << ","
      << "\"action\":" << json_escape(revision.action) << ","
      << "\"description\":" << json_escape(revision.record.description) << ","
      << "\"value\":{"
      << "\"payload\":" << config_value_json(revision.record) << ","
      << "\"updated_by\":" << json_escape(revision.record.updated_by) << ","
      << "\"updated_unix_ms\":" << revision.record.updated_unix_ms
      << "}"
      << "}";
  return out.str();
}

std::string mutation_applied_json(const ConfigRecord& record) {
  std::ostringstream out;
  out << "{"
      << "\"kind\":\"applied\","
      << "\"entry\":" << config_entry_json(record)
      << "}";
  return out.str();
}

std::string mutation_precondition_required_json(std::int64_t current_revision_id) {
  std::ostringstream out;
  out << "{"
      << "\"kind\":\"precondition_required\","
      << "\"current_revision_id\":" << current_revision_id
      << "}";
  return out.str();
}

std::string mutation_conflict_json(std::int64_t expected_revision_id, std::int64_t current_revision_id) {
  std::ostringstream out;
  out << "{"
      << "\"kind\":\"conflict\","
      << "\"expected_revision_id\":" << expected_revision_id << ","
      << "\"current_revision_id\":" << current_revision_id
      << "}";
  return out.str();
}

std::string mutation_deleted_json(std::int64_t deleted_revision_id) {
  std::ostringstream out;
  out << "{"
      << "\"kind\":\"deleted\","
      << "\"deleted_revision_id\":" << deleted_revision_id
      << "}";
  return out.str();
}

std::string mutation_not_found_json() {
  return "{\"kind\":\"not_found\"}";
}

class Database {
  sqlite3* db_ = nullptr;

public:
  Database() = default;
  explicit Database(sqlite3* db) : db_(db) {}
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

  Database& operator=(Database&& other) noexcept {
    if (this == &other) return *this;
    close();
    db_ = std::exchange(other.db_, nullptr);
    return *this;
  }

  ~Database() { close(); }

  sqlite3* get() const { return db_; }

  void close() {
    if (db_ == nullptr) return;
    sqlite_api().close_v2(db_);
    db_ = nullptr;
  }
};

ResultBool configure_database(sqlite3* db);

nebula::rt::Result<Database, std::string> open_database(std::string path) {
  RETURN_IF_ERR_AS(Database, ensure_sqlite_loaded());
  std::error_code ec;
  const auto file_path = std::filesystem::path(path);
  if (file_path.has_parent_path()) {
    std::filesystem::create_directories(file_path.parent_path(), ec);
    if (ec) {
      return nebula::rt::err_result<Database>("failed to create database directory: " + ec.message());
    }
  }

  sqlite3* raw = nullptr;
  const int flags = kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex;
  const int rc = sqlite_api().open_v2(path.c_str(), &raw, flags, nullptr);
  if (rc != kSqliteOk || raw == nullptr) {
    const std::string message = sqlite_error_message(raw, "failed to open sqlite database");
    if (raw != nullptr) sqlite_api().close_v2(raw);
    return nebula::rt::err_result<Database>(message);
  }
  auto configured = configure_database(raw);
  if (nebula::rt::result_is_err(configured)) {
    const std::string message = nebula::rt::result_err_ref(configured);
    sqlite_api().close_v2(raw);
    return nebula::rt::err_result<Database>(message);
  }
  return nebula::rt::ok_result(Database(raw));
}

nebula::rt::Result<bool, std::string> exec_sql(sqlite3* db, const char* sql) {
  char* raw_error = nullptr;
  const int rc = sqlite_api().exec(db, sql, nullptr, nullptr, &raw_error);
  if (rc != kSqliteOk) {
    std::string message = sqlite_error_message(db, "sqlite exec failed");
    return nebula::rt::err_result<bool>(message);
  }
  return nebula::rt::ok_result(true);
}

ResultBool configure_database(sqlite3* db) {
  const int timeout_rc = sqlite_api().busy_timeout(db, 5000);
  if (timeout_rc != kSqliteOk) {
    return nebula::rt::err_result<bool>(sqlite_error_message(db, "failed to configure sqlite busy timeout"));
  }
  RETURN_IF_ERR_AS(bool, exec_sql(db, "PRAGMA journal_mode=WAL;"));
  RETURN_IF_ERR_AS(bool, exec_sql(db, "PRAGMA synchronous=NORMAL;"));
  return nebula::rt::ok_result(true);
}

class Statement {
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;

public:
  Statement() = default;
  Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement(Statement&& other) noexcept
      : db_(other.db_),
        stmt_(std::exchange(other.stmt_, nullptr)) {}

  ~Statement() {
    if (stmt_ != nullptr) sqlite_api().finalize(stmt_);
  }

  sqlite3_stmt* get() const { return stmt_; }
  sqlite3* db() const { return db_; }
};

nebula::rt::Result<Statement, std::string> prepare_statement(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite_api().prepare_v2(db, sql, -1, &stmt, nullptr);
  if (rc != kSqliteOk || stmt == nullptr) {
    return nebula::rt::err_result<Statement>(sqlite_error_message(db, "failed to prepare sqlite statement"));
  }
  return nebula::rt::ok_result(Statement(db, stmt));
}

nebula::rt::Result<bool, std::string> bind_text(sqlite3* db,
                                                sqlite3_stmt* stmt,
                                                int index,
                                                const std::string& value) {
  auto destructor = reinterpret_cast<void (*)(void*)>(-1);
  const int rc = sqlite_api().bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), destructor);
  if (rc != kSqliteOk) {
    return nebula::rt::err_result<bool>(sqlite_error_message(db, "failed to bind sqlite text"));
  }
  return nebula::rt::ok_result(true);
}

nebula::rt::Result<bool, std::string> bind_int64(sqlite3* db,
                                                 sqlite3_stmt* stmt,
                                                 int index,
                                                 std::int64_t value) {
  const int rc = sqlite_api().bind_int64(stmt, index, value);
  if (rc != kSqliteOk) {
    return nebula::rt::err_result<bool>(sqlite_error_message(db, "failed to bind sqlite int"));
  }
  return nebula::rt::ok_result(true);
}

std::string column_text_or_empty(sqlite3_stmt* stmt, int index) {
  const auto* value = sqlite_api().column_text(stmt, index);
  if (value == nullptr) return {};
  return std::string(reinterpret_cast<const char*>(value));
}

ConfigRecord read_config_record(sqlite3_stmt* stmt, int offset = 0) {
  ConfigRecord record;
  record.project = column_text_or_empty(stmt, offset + 0);
  record.environment = column_text_or_empty(stmt, offset + 1);
  record.key = column_text_or_empty(stmt, offset + 2);
  record.description = column_text_or_empty(stmt, offset + 3);
  record.kind = column_text_or_empty(stmt, offset + 4);
  record.string_value = column_text_or_empty(stmt, offset + 5);
  record.bool_value = sqlite_api().column_int64(stmt, offset + 6) != 0;
  record.int_value = sqlite_api().column_int64(stmt, offset + 7);
  record.revision_id = sqlite_api().column_int64(stmt, offset + 8);
  record.updated_by = column_text_or_empty(stmt, offset + 9);
  record.updated_unix_ms = sqlite_api().column_int64(stmt, offset + 10);
  return record;
}

RevisionRecord read_revision_record(sqlite3_stmt* stmt) {
  RevisionRecord revision;
  revision.revision_id = sqlite_api().column_int64(stmt, 0);
  revision.record = read_config_record(stmt, 1);
  revision.action = column_text_or_empty(stmt, 12);
  return revision;
}

nebula::rt::Result<bool, std::string> initialize_schema(sqlite3* db) {
  RETURN_IF_ERR_AS(
      bool,
      exec_sql(db,
               "CREATE TABLE IF NOT EXISTS config_entries ("
               "project TEXT NOT NULL,"
               "environment TEXT NOT NULL,"
               "key TEXT NOT NULL,"
               "description TEXT NOT NULL,"
               "kind TEXT NOT NULL,"
               "string_value TEXT NOT NULL,"
               "bool_value INTEGER NOT NULL,"
               "int_value INTEGER NOT NULL,"
               "revision_id INTEGER NOT NULL,"
               "updated_by TEXT NOT NULL,"
               "updated_unix_ms INTEGER NOT NULL,"
               "PRIMARY KEY(project, environment, key)"
               ");"));
  RETURN_IF_ERR_AS(
      bool,
      exec_sql(db,
               "CREATE TABLE IF NOT EXISTS config_revisions ("
               "revision_id INTEGER PRIMARY KEY AUTOINCREMENT,"
               "project TEXT NOT NULL,"
               "environment TEXT NOT NULL,"
               "key TEXT NOT NULL,"
               "description TEXT NOT NULL,"
               "kind TEXT NOT NULL,"
               "string_value TEXT NOT NULL,"
               "bool_value INTEGER NOT NULL,"
               "int_value INTEGER NOT NULL,"
               "updated_by TEXT NOT NULL,"
               "updated_unix_ms INTEGER NOT NULL,"
               "action TEXT NOT NULL"
               ");"));
  ASSIGN_OR_RETURN_AS(
      bool,
      table_info,
      prepare_statement(
          db,
          "PRAGMA table_info(config_entries);"));
  bool has_revision_column = false;
  while (true) {
    const int rc = sqlite_api().step(table_info.get());
    if (rc == kSqliteDone) break;
    if (rc != kSqliteRow) {
      return nebula::rt::err_result<bool>(sqlite_error_message(db, "failed to inspect config_entries schema"));
    }
    if (column_text_or_empty(table_info.get(), 1) == "revision_id") {
      has_revision_column = true;
      break;
    }
  }
  if (!has_revision_column) {
    RETURN_IF_ERR_AS(bool,
                     exec_sql(db, "ALTER TABLE config_entries ADD COLUMN revision_id INTEGER NOT NULL DEFAULT 0;"));
  }
  RETURN_IF_ERR_AS(
      bool,
      exec_sql(db,
               "UPDATE config_entries "
               "SET revision_id = COALESCE(("
               "SELECT MAX(revision_id) FROM config_revisions "
               "WHERE config_revisions.project = config_entries.project "
               "AND config_revisions.environment = config_entries.environment "
               "AND config_revisions.key = config_entries.key"
               "), 0) "
               "WHERE revision_id = 0;"));
  return nebula::rt::ok_result(true);
}

class Transaction {
  sqlite3* db_ = nullptr;
  bool committed_ = false;

public:
  explicit Transaction(sqlite3* db) : db_(db) {}
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  ~Transaction() {
    if (!committed_ && db_ != nullptr) {
      (void)exec_sql(db_, "ROLLBACK;");
    }
  }

  nebula::rt::Result<bool, std::string> begin() { return exec_sql(db_, "BEGIN IMMEDIATE TRANSACTION;"); }

  nebula::rt::Result<bool, std::string> commit() {
    RETURN_IF_ERR_AS(bool, exec_sql(db_, "COMMIT;"));
    committed_ = true;
    return nebula::rt::ok_result(true);
  }
};

nebula::rt::Result<ConfigRecord, std::string> select_config_record(sqlite3* db,
                                                                   const std::string& project,
                                                                   const std::string& environment,
                                                                   const std::string& key) {
  ASSIGN_OR_RETURN_AS(
      ConfigRecord,
      stmt,
      prepare_statement(
          db,
          "SELECT project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms "
          "FROM config_entries WHERE project = ?1 AND environment = ?2 AND key = ?3;"));
  RETURN_IF_ERR_AS(ConfigRecord, bind_text(db, stmt.get(), 1, project));
  RETURN_IF_ERR_AS(ConfigRecord, bind_text(db, stmt.get(), 2, environment));
  RETURN_IF_ERR_AS(ConfigRecord, bind_text(db, stmt.get(), 3, key));
  const int rc = sqlite_api().step(stmt.get());
  if (rc == kSqliteRow) {
    return nebula::rt::ok_result(read_config_record(stmt.get()));
  }
  if (rc == kSqliteDone) {
    return nebula::rt::err_result<ConfigRecord>("config entry not found");
  }
  return nebula::rt::err_result<ConfigRecord>(sqlite_error_message(db, "failed to read config entry"));
}

nebula::rt::Result<std::int64_t, std::string> select_last_insert_rowid(sqlite3* db) {
  ASSIGN_OR_RETURN_AS(
      std::int64_t,
      stmt,
      prepare_statement(
          db,
          "SELECT last_insert_rowid();"));
  const int rc = sqlite_api().step(stmt.get());
  if (rc == kSqliteRow) {
    return nebula::rt::ok_result(sqlite_api().column_int64(stmt.get(), 0));
  }
  return nebula::rt::err_result<std::int64_t>(sqlite_error_message(db, "failed to read sqlite last_insert_rowid"));
}

std::string database_path_from_state_dir(const std::string& state_dir) {
  return (std::filesystem::path(state_dir) / "catalog.db").string();
}

nebula::rt::Result<std::string, std::string> get_entry_json_impl(const std::string& db_path,
                                                                 const std::string& project,
                                                                 const std::string& environment,
                                                                 const std::string& key) {
  ASSIGN_OR_RETURN_AS(std::string, db, open_database(db_path));
  RETURN_IF_ERR_AS(std::string, initialize_schema(db.get()));
  ASSIGN_OR_RETURN_AS(std::string, record, select_config_record(db.get(), project, environment, key));
  return nebula::rt::ok_result(config_entry_json(record));
}

nebula::rt::Result<std::string, std::string> list_entries_json_impl(const std::string& db_path,
                                                                    const std::string& project,
                                                                    const std::string& environment) {
  ASSIGN_OR_RETURN_AS(std::string, db, open_database(db_path));
  RETURN_IF_ERR_AS(std::string, initialize_schema(db.get()));
  ASSIGN_OR_RETURN_AS(
      std::string,
      stmt,
      prepare_statement(
          db.get(),
          "SELECT project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms "
          "FROM config_entries WHERE project = ?1 AND environment = ?2 ORDER BY key ASC;"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), stmt.get(), 1, project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), stmt.get(), 2, environment));
  std::ostringstream out;
  out << "[";
  bool first = true;
  while (true) {
    const int rc = sqlite_api().step(stmt.get());
    if (rc == kSqliteDone) break;
    if (rc != kSqliteRow) {
      return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to list config entries"));
    }
    if (!first) out << ",";
    first = false;
    out << config_entry_json(read_config_record(stmt.get()));
  }
  out << "]";
  return nebula::rt::ok_result(out.str());
}

nebula::rt::Result<std::string, std::string> put_entry_outcome_json_impl(const std::string& db_path,
                                                                          const ConfigRecord& record,
                                                                          std::int64_t expected_revision_id) {
  ASSIGN_OR_RETURN_AS(std::string, db, open_database(db_path));
  RETURN_IF_ERR_AS(std::string, initialize_schema(db.get()));
  Transaction tx(db.get());
  RETURN_IF_ERR_AS(std::string, tx.begin());

  auto existing = select_config_record(db.get(), record.project, record.environment, record.key);
  if (nebula::rt::result_is_err(existing)) {
    if (nebula::rt::result_err_ref(existing) != "config entry not found") {
      return nebula::rt::err_result<std::string>(nebula::rt::result_err_ref(existing));
    }
    if (expected_revision_id > 0) {
      return nebula::rt::ok_result(mutation_conflict_json(expected_revision_id, 0));
    }
  } else {
    const auto& current = nebula::rt::result_ok_ref(existing);
    if (expected_revision_id < 0) {
      return nebula::rt::ok_result(mutation_precondition_required_json(current.revision_id));
    }
    if (expected_revision_id != current.revision_id) {
      return nebula::rt::ok_result(mutation_conflict_json(expected_revision_id, current.revision_id));
    }
  }

  ASSIGN_OR_RETURN_AS(
      std::string,
      history,
      prepare_statement(
          db.get(),
          "INSERT INTO config_revisions (project, environment, key, description, kind, string_value, bool_value, int_value, updated_by, updated_unix_ms, action) "
          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 1, record.project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 2, record.environment));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 3, record.key));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 4, record.description));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 5, record.kind));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 6, record.string_value));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 7, record.bool_value ? 1 : 0));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 8, record.int_value));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 9, record.updated_by));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 10, record.updated_unix_ms));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 11, "put"));
  if (sqlite_api().step(history.get()) != kSqliteDone) {
    return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to append config revision"));
  }

  ASSIGN_OR_RETURN_AS(std::string, new_revision_id, select_last_insert_rowid(db.get()));
  auto applied = record;
  applied.revision_id = new_revision_id;

  ASSIGN_OR_RETURN_AS(
      std::string,
      upsert,
      prepare_statement(
          db.get(),
          "INSERT INTO config_entries (project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms) "
          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
          "ON CONFLICT(project, environment, key) DO UPDATE SET "
          "description = excluded.description, "
          "kind = excluded.kind, "
          "string_value = excluded.string_value, "
          "bool_value = excluded.bool_value, "
          "int_value = excluded.int_value, "
          "revision_id = excluded.revision_id, "
          "updated_by = excluded.updated_by, "
          "updated_unix_ms = excluded.updated_unix_ms;"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 1, applied.project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 2, applied.environment));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 3, applied.key));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 4, applied.description));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 5, applied.kind));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 6, applied.string_value));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), upsert.get(), 7, applied.bool_value ? 1 : 0));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), upsert.get(), 8, applied.int_value));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), upsert.get(), 9, applied.revision_id));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), upsert.get(), 10, applied.updated_by));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), upsert.get(), 11, applied.updated_unix_ms));
  if (sqlite_api().step(upsert.get()) != kSqliteDone) {
    return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to upsert config entry"));
  }

  RETURN_IF_ERR_AS(std::string, tx.commit());
  return nebula::rt::ok_result(mutation_applied_json(applied));
}

nebula::rt::Result<std::string, std::string> delete_entry_outcome_json_impl(const std::string& db_path,
                                                                             const std::string& project,
                                                                             const std::string& environment,
                                                                             const std::string& key,
                                                                             std::int64_t expected_revision_id,
                                                                             const std::string& updated_by,
                                                                             std::int64_t updated_unix_ms) {
  ASSIGN_OR_RETURN_AS(std::string, db, open_database(db_path));
  RETURN_IF_ERR_AS(std::string, initialize_schema(db.get()));
  Transaction tx(db.get());
  RETURN_IF_ERR_AS(std::string, tx.begin());

  auto existing = select_config_record(db.get(), project, environment, key);
  if (nebula::rt::result_is_err(existing)) {
    if (nebula::rt::result_err_ref(existing) == "config entry not found") {
      return nebula::rt::ok_result(mutation_not_found_json());
    }
    return nebula::rt::err_result<std::string>(nebula::rt::result_err_ref(existing));
  }

  auto history_record = nebula::rt::result_ok_ref(existing);
  if (expected_revision_id < 0) {
    return nebula::rt::ok_result(mutation_precondition_required_json(history_record.revision_id));
  }
  if (expected_revision_id != history_record.revision_id) {
    return nebula::rt::ok_result(mutation_conflict_json(expected_revision_id, history_record.revision_id));
  }

  history_record.updated_by = updated_by;
  history_record.updated_unix_ms = updated_unix_ms;
  ASSIGN_OR_RETURN_AS(
      std::string,
      history,
      prepare_statement(
          db.get(),
          "INSERT INTO config_revisions (project, environment, key, description, kind, string_value, bool_value, int_value, updated_by, updated_unix_ms, action) "
          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 1, history_record.project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 2, history_record.environment));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 3, history_record.key));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 4, history_record.description));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 5, history_record.kind));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 6, history_record.string_value));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 7, history_record.bool_value ? 1 : 0));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 8, history_record.int_value));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 9, history_record.updated_by));
  RETURN_IF_ERR_AS(std::string, bind_int64(db.get(), history.get(), 10, history_record.updated_unix_ms));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), history.get(), 11, "delete"));
  if (sqlite_api().step(history.get()) != kSqliteDone) {
    return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to append delete revision"));
  }

  ASSIGN_OR_RETURN_AS(std::string, deleted_revision_id, select_last_insert_rowid(db.get()));
  ASSIGN_OR_RETURN_AS(
      std::string,
      removed,
      prepare_statement(
          db.get(),
          "DELETE FROM config_entries WHERE project = ?1 AND environment = ?2 AND key = ?3;"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), removed.get(), 1, project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), removed.get(), 2, environment));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), removed.get(), 3, key));
  if (sqlite_api().step(removed.get()) != kSqliteDone) {
    return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to delete config entry"));
  }

  RETURN_IF_ERR_AS(std::string, tx.commit());
  return nebula::rt::ok_result(mutation_deleted_json(deleted_revision_id));
}

nebula::rt::Result<std::string, std::string> list_history_json_impl(const std::string& db_path,
                                                                    const std::string& project,
                                                                    const std::string& environment,
                                                                    const std::string& key) {
  ASSIGN_OR_RETURN_AS(std::string, db, open_database(db_path));
  RETURN_IF_ERR_AS(std::string, initialize_schema(db.get()));
  ASSIGN_OR_RETURN_AS(
      std::string,
      stmt,
      prepare_statement(
          db.get(),
          "SELECT revision_id, project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms, action "
          "FROM config_revisions WHERE project = ?1 AND environment = ?2 AND key = ?3 ORDER BY revision_id DESC;"));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), stmt.get(), 1, project));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), stmt.get(), 2, environment));
  RETURN_IF_ERR_AS(std::string, bind_text(db.get(), stmt.get(), 3, key));
  std::ostringstream out;
  out << "[";
  bool first = true;
  while (true) {
    const int rc = sqlite_api().step(stmt.get());
    if (rc == kSqliteDone) break;
    if (rc != kSqliteRow) {
      return nebula::rt::err_result<std::string>(sqlite_error_message(db.get(), "failed to list config history"));
    }
    if (!first) out << ",";
    first = false;
    out << config_revision_json(read_revision_record(stmt.get()));
  }
  out << "]";
  return nebula::rt::ok_result(out.str());
}

ResultBool initialize_catalog_store_impl(const std::string& db_path) {
  ASSIGN_OR_RETURN_AS(bool, db, open_database(db_path));
  RETURN_IF_ERR_AS(bool, initialize_schema(db.get()));
  return nebula::rt::ok_result(true);
}

} // namespace

nebula::rt::Result<std::string, std::string> __cli_service_db_get_entry_json(std::string db_path,
                                                                              std::string project,
                                                                              std::string environment,
                                                                              std::string key) {
  return get_entry_json_impl(db_path, project, environment, key);
}

nebula::rt::Result<std::string, std::string> __cli_service_db_list_entries_json(std::string db_path,
                                                                                 std::string project,
                                                                                 std::string environment) {
  return list_entries_json_impl(db_path, project, environment);
}

nebula::rt::Result<std::string, std::string> __cli_service_db_put_entry_outcome_json(std::string db_path,
                                                                                      std::string project,
                                                                                      std::string environment,
                                                                                      std::string key,
                                                                                      std::string description,
                                                                                      std::string kind,
                                                                                      std::string string_value,
                                                                                      bool bool_value,
                                                                                      std::int64_t int_value,
                                                                                      std::int64_t expected_revision_id,
                                                                                      std::string updated_by,
                                                                                      std::int64_t updated_unix_ms) {
  ConfigRecord record;
  record.project = std::move(project);
  record.environment = std::move(environment);
  record.key = std::move(key);
  record.description = std::move(description);
  record.kind = std::move(kind);
  record.string_value = std::move(string_value);
  record.bool_value = bool_value;
  record.int_value = int_value;
  record.updated_by = std::move(updated_by);
  record.updated_unix_ms = updated_unix_ms;
  return put_entry_outcome_json_impl(db_path, record, expected_revision_id);
}

nebula::rt::Result<std::string, std::string> __cli_service_db_delete_entry_outcome_json(std::string db_path,
                                                                                         std::string project,
                                                                                         std::string environment,
                                                                                         std::string key,
                                                                                         std::int64_t expected_revision_id,
                                                                                         std::string updated_by,
                                                                                         std::int64_t updated_unix_ms) {
  return delete_entry_outcome_json_impl(db_path,
                                        project,
                                        environment,
                                        key,
                                        expected_revision_id,
                                        updated_by,
                                        updated_unix_ms);
}

nebula::rt::Result<std::string, std::string> __cli_service_db_list_history_json(std::string db_path,
                                                                                 std::string project,
                                                                                 std::string environment,
                                                                                 std::string key) {
  return list_history_json_impl(db_path, project, environment, key);
}

nebula::rt::Result<bool, std::string> __cli_service_db_initialize(std::string db_path) {
  return initialize_catalog_store_impl(db_path);
}
