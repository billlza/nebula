#pragma once

#include "bench.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace app_platform_cpp::service_json_db_crud {

struct ConfigValue {
  std::string kind;
  std::string string_value;
  bool bool_value = false;
  std::int64_t int_value = 0;
};

struct ConfigEntry {
  std::string project;
  std::string environment;
  std::string key;
  std::string description;
  ConfigValue value;
  std::int64_t revision_id = 0;
  std::string updated_by;
  std::int64_t updated_unix_ms = 0;
};

struct ConfigRevision {
  std::int64_t revision_id = 0;
  std::string project;
  std::string environment;
  std::string key;
  std::string action;
  std::string description;
  ConfigValue value;
  std::string updated_by;
  std::int64_t updated_unix_ms = 0;
};

enum class Method {
  kGet,
  kPut,
  kDelete,
};

struct Request {
  Method method;
  std::string path;
  std::string body;
};

struct Response {
  int status = 200;
  std::string body;
  std::string etag;
};

struct Identity {
  std::string project;
  std::string environment;
  std::string key;
};

struct PutRequest {
  std::string description;
  ConfigValue value;
};

class SqliteDb {
 public:
  explicit SqliteDb(sqlite3* db) : db_(db) {}

  SqliteDb(const SqliteDb&) = delete;
  SqliteDb& operator=(const SqliteDb&) = delete;

  SqliteDb(SqliteDb&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

  SqliteDb& operator=(SqliteDb&& other) noexcept {
    if (this != &other) {
      close();
      db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
  }

  ~SqliteDb() {
    close();
  }

  sqlite3* get() const {
    return db_;
  }

  void exec(const std::string& sql) const {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &raw_error);
    if (rc != SQLITE_OK) {
      std::string message = raw_error != nullptr ? raw_error : sqlite3_errmsg(db_);
      sqlite3_free(raw_error);
      fail("sqlite exec failed: " + message);
    }
  }

 private:
  void close() {
    if (db_ != nullptr) {
      const int rc = sqlite3_close(db_);
      if (rc != SQLITE_OK) {
        std::fprintf(stderr, "sqlite close failed: %s\n", sqlite3_errmsg(db_));
        std::abort();
      }
      db_ = nullptr;
    }
  }

  sqlite3* db_ = nullptr;
};

class Statement {
 public:
  Statement(sqlite3* db, const std::string& sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) {
      fail("sqlite prepare failed: " + std::string(sqlite3_errmsg(db_)));
    }
  }

  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  ~Statement() {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
  }

  void bind_text(int index, const std::string& value) {
    const int rc = sqlite3_bind_text(stmt_, index, value.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
      fail("sqlite bind text failed: " + std::string(sqlite3_errmsg(db_)));
    }
  }

  void bind_int64(int index, std::int64_t value) {
    const int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK) {
      fail("sqlite bind int failed: " + std::string(sqlite3_errmsg(db_)));
    }
  }

  bool step_row() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
      return true;
    }
    if (rc == SQLITE_DONE) {
      return false;
    }
    fail("sqlite step failed: " + std::string(sqlite3_errmsg(db_)));
  }

  void step_done() {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_DONE) {
      fail("sqlite write failed: " + std::string(sqlite3_errmsg(db_)));
    }
  }

  std::string column_text(int index) const {
    const unsigned char* value = sqlite3_column_text(stmt_, index);
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
  }

  std::int64_t column_int64(int index) const {
    return sqlite3_column_int64(stmt_, index);
  }

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

inline std::filesystem::path db_path() {
#if defined(_WIN32)
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  return std::filesystem::temp_directory_path() /
         ("nebula-app-platform-service-json-db-crud-cpp-" + std::to_string(pid) + ".sqlite");
}

inline void ignore_remove(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

inline void cleanup_db_files() {
  const std::filesystem::path path = db_path();
  ignore_remove(path);
  ignore_remove(std::filesystem::path(path.string() + "-wal"));
  ignore_remove(std::filesystem::path(path.string() + "-shm"));
}

inline SqliteDb open_catalog() {
  cleanup_db_files();
  sqlite3* raw = nullptr;
  const std::string path = db_path().string();
  const int rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
  if (rc != SQLITE_OK) {
    const std::string message = raw != nullptr ? sqlite3_errmsg(raw) : "unknown sqlite open failure";
    if (raw != nullptr) {
      sqlite3_close(raw);
    }
    fail("sqlite open failed: " + message);
  }
  SqliteDb db(raw);
  db.exec("CREATE TABLE IF NOT EXISTS config_entries (project TEXT NOT NULL, environment TEXT NOT NULL, key TEXT NOT NULL, description TEXT NOT NULL, kind TEXT NOT NULL, string_value TEXT NOT NULL, bool_value INTEGER NOT NULL, int_value INTEGER NOT NULL, revision_id INTEGER NOT NULL, updated_by TEXT NOT NULL, updated_unix_ms INTEGER NOT NULL, PRIMARY KEY (project, environment, key))");
  db.exec("CREATE TABLE IF NOT EXISTS config_revisions (revision_id INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL, environment TEXT NOT NULL, key TEXT NOT NULL, action TEXT NOT NULL, description TEXT NOT NULL, kind TEXT NOT NULL, string_value TEXT NOT NULL, bool_value INTEGER NOT NULL, int_value INTEGER NOT NULL, updated_by TEXT NOT NULL, updated_unix_ms INTEGER NOT NULL)");
  return db;
}

inline ConfigValue config_bool_value(bool value) {
  return ConfigValue{"bool", "", value, 0};
}

inline void validate_segment(std::string_view label, const std::string& value) {
  if (value.empty()) {
    fail(std::string(label) + " must be non-empty");
  }
  for (unsigned char ch : value) {
    const bool ascii_lower = ch >= 'a' && ch <= 'z';
    const bool digit = ch >= '0' && ch <= '9';
    const bool punctuation = ch == '.' || ch == '_' || ch == '-';
    if (!(ascii_lower || digit || punctuation)) {
      fail(std::string(label) + " contains invalid character");
    }
  }
}

inline void validate_identity(const Identity& identity, const std::string& updated_by) {
  validate_segment("project", identity.project);
  validate_segment("environment", identity.environment);
  validate_segment("key", identity.key);
  validate_segment("actor", updated_by);
}

inline void validate_config_value(const ConfigValue& value) {
  if (value.kind != "bool") {
    fail("only bool config values are used by the reference workload");
  }
  if (!value.string_value.empty() || value.int_value != 0) {
    fail("bool config value carried non-bool payload");
  }
}

inline void validate_put_request(const PutRequest& request) {
  if (request.description.empty()) {
    fail("config put description must be non-empty");
  }
  validate_config_value(request.value);
}

inline std::vector<std::string> split_path(const std::string& path) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      out.push_back(part);
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return out;
}

inline Identity parse_item_path(const std::string& path) {
  const std::vector<std::string> parts = split_path(path);
  if (parts.size() != 5 || parts[0] != "v1" || parts[1] != "configs") {
    fail("config item path did not match");
  }
  return Identity{parts[2], parts[3], parts[4]};
}

inline Identity parse_history_path(const std::string& path) {
  const std::vector<std::string> parts = split_path(path);
  if (parts.size() != 6 || parts[0] != "v1" || parts[1] != "configs" || parts[5] != "history") {
    fail("config history path did not match");
  }
  return Identity{parts[2], parts[3], parts[4]};
}

inline Identity parse_list_path(const std::string& path) {
  const std::vector<std::string> parts = split_path(path);
  if (parts.size() != 4 || parts[0] != "v1" || parts[1] != "configs") {
    fail("config list path did not match");
  }
  return Identity{parts[2], parts[3], ""};
}

inline std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  constexpr char kHex[] = "0123456789abcdef";
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(ch >> 4U) & 0x0FU]);
          out.push_back(kHex[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return out;
}

inline std::string json_string(const std::string& value) {
  return "\"" + json_escape(value) + "\"";
}

inline std::string config_value_json(const ConfigValue& value) {
  return "{\"kind\":" + json_string(value.kind) + ",\"string_value\":" + json_string(value.string_value) +
         ",\"bool_value\":" + (value.bool_value ? "true" : "false") +
         ",\"int_value\":" + std::to_string(value.int_value) + "}";
}

inline std::string put_request_json(const std::string& description, bool bool_value) {
  return "{\"description\":" + json_string(description) + ",\"value\":" + config_value_json(config_bool_value(bool_value)) + "}";
}

inline std::string config_entry_json(const ConfigEntry& entry) {
  return "{\"project\":" + json_string(entry.project) +
         ",\"environment\":" + json_string(entry.environment) +
         ",\"key\":" + json_string(entry.key) +
         ",\"description\":" + json_string(entry.description) +
         ",\"value\":" + config_value_json(entry.value) +
         ",\"revision_id\":" + std::to_string(entry.revision_id) +
         ",\"meta\":{\"updated_by\":" + json_string(entry.updated_by) +
         ",\"updated_unix_ms\":" + std::to_string(entry.updated_unix_ms) + "}}";
}

inline std::string config_revision_json(const ConfigRevision& revision) {
  return "{\"revision_id\":" + std::to_string(revision.revision_id) +
         ",\"project\":" + json_string(revision.project) +
         ",\"environment\":" + json_string(revision.environment) +
         ",\"key\":" + json_string(revision.key) +
         ",\"action\":" + json_string(revision.action) +
         ",\"description\":" + json_string(revision.description) +
         ",\"value\":{\"payload\":" + config_value_json(revision.value) +
         ",\"updated_by\":" + json_string(revision.updated_by) +
         ",\"updated_unix_ms\":" + std::to_string(revision.updated_unix_ms) + "}}";
}

inline std::string entry_envelope_json(const ConfigEntry& entry) {
  return "{\"entry\":" + config_entry_json(entry) + ",\"request_id\":\"bench-req-1\",\"unix_ms\":1700000000000}";
}

inline std::string list_envelope_json(const std::string& project,
                                      const std::string& environment,
                                      const std::vector<ConfigEntry>& entries) {
  std::string body = "{\"project\":" + json_string(project) +
                     ",\"environment\":" + json_string(environment) +
                     ",\"entries\":[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) {
      body += ",";
    }
    body += config_entry_json(entries[i]);
  }
  body += "],\"request_id\":\"bench-req-1\",\"unix_ms\":1700000000000}";
  return body;
}

inline std::string history_envelope_json(const Identity& identity,
                                         const std::vector<ConfigRevision>& revisions) {
  std::string body = "{\"project\":" + json_string(identity.project) +
                     ",\"environment\":" + json_string(identity.environment) +
                     ",\"key\":" + json_string(identity.key) +
                     ",\"revisions\":[";
  for (std::size_t i = 0; i < revisions.size(); ++i) {
    if (i != 0) {
      body += ",";
    }
    body += config_revision_json(revisions[i]);
  }
  body += "],\"request_id\":\"bench-req-1\",\"unix_ms\":1700000000000}";
  return body;
}

inline std::string delete_envelope_json(const Identity& identity, std::int64_t deleted_revision_id) {
  return "{\"project\":" + json_string(identity.project) +
         ",\"environment\":" + json_string(identity.environment) +
         ",\"key\":" + json_string(identity.key) +
         ",\"deleted\":true,\"deleted_revision_id\":" + std::to_string(deleted_revision_id) +
         ",\"request_id\":\"bench-req-1\",\"unix_ms\":1700000000000}";
}

inline std::size_t count_occurrences(const std::string& text, const std::string& needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while (true) {
    pos = text.find(needle, pos);
    if (pos == std::string::npos) {
      return count;
    }
    ++count;
    pos += needle.size();
  }
}

inline std::string extract_string_field(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\":\"";
  const std::size_t start = json.find(token);
  if (start == std::string::npos) {
    fail("missing JSON string field: " + key);
  }
  std::string out;
  bool escaping = false;
  for (std::size_t i = start + token.size(); i < json.size(); ++i) {
    const char ch = json[i];
    if (escaping) {
      out.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') {
      return out;
    }
    out.push_back(ch);
  }
  fail("unterminated JSON string field: " + key);
}

inline std::int64_t extract_int_field(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\":";
  const std::size_t start = json.find(token);
  if (start == std::string::npos) {
    fail("missing JSON int field: " + key);
  }
  const char* begin = json.data() + start + token.size();
  const char* end = json.data() + json.size();
  std::int64_t value = 0;
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{}) {
    fail("invalid JSON int field: " + key);
  }
  return value;
}

inline bool extract_bool_field(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\":";
  const std::size_t start = json.find(token);
  if (start == std::string::npos) {
    fail("missing JSON bool field: " + key);
  }
  const std::size_t value_start = start + token.size();
  if (json.compare(value_start, 4, "true") == 0) {
    return true;
  }
  if (json.compare(value_start, 5, "false") == 0) {
    return false;
  }
  fail("invalid JSON bool field: " + key);
}

inline PutRequest parse_put_request(const std::string& body) {
  PutRequest request;
  request.description = extract_string_field(body, "description");
  request.value.kind = extract_string_field(body, "kind");
  request.value.string_value = extract_string_field(body, "string_value");
  request.value.bool_value = extract_bool_field(body, "bool_value");
  request.value.int_value = extract_int_field(body, "int_value");
  validate_put_request(request);
  return request;
}

inline Request config_request(const std::string& project,
                              const std::string& environment,
                              const std::string& key,
                              const std::string& description,
                              bool bool_value) {
  return Request{Method::kPut,
                 "/v1/configs/" + project + "/" + environment + "/" + key,
                 put_request_json(description, bool_value)};
}

inline Request config_get_request(const std::string& project, const std::string& environment, const std::string& key) {
  return Request{Method::kGet, "/v1/configs/" + project + "/" + environment + "/" + key, ""};
}

inline Request config_list_request(const std::string& project, const std::string& environment) {
  return Request{Method::kGet, "/v1/configs/" + project + "/" + environment, ""};
}

inline Request config_history_request(const std::string& project, const std::string& environment, const std::string& key) {
  return Request{Method::kGet, "/v1/configs/" + project + "/" + environment + "/" + key + "/history", ""};
}

inline Request config_delete_request(const std::string& project, const std::string& environment, const std::string& key) {
  return Request{Method::kDelete, "/v1/configs/" + project + "/" + environment + "/" + key, ""};
}

inline ConfigEntry row_entry(Statement& stmt) {
  ConfigEntry entry;
  entry.project = stmt.column_text(0);
  entry.environment = stmt.column_text(1);
  entry.key = stmt.column_text(2);
  entry.description = stmt.column_text(3);
  entry.value = ConfigValue{stmt.column_text(4), stmt.column_text(5), stmt.column_int64(6) != 0, stmt.column_int64(7)};
  entry.revision_id = stmt.column_int64(8);
  entry.updated_by = stmt.column_text(9);
  entry.updated_unix_ms = stmt.column_int64(10);
  validate_config_value(entry.value);
  return entry;
}

inline ConfigRevision row_revision(Statement& stmt) {
  ConfigRevision revision;
  revision.revision_id = stmt.column_int64(0);
  revision.project = stmt.column_text(1);
  revision.environment = stmt.column_text(2);
  revision.key = stmt.column_text(3);
  revision.action = stmt.column_text(4);
  revision.description = stmt.column_text(5);
  revision.value = ConfigValue{stmt.column_text(6), stmt.column_text(7), stmt.column_int64(8) != 0, stmt.column_int64(9)};
  revision.updated_by = stmt.column_text(10);
  revision.updated_unix_ms = stmt.column_int64(11);
  validate_config_value(revision.value);
  return revision;
}

inline std::optional<ConfigEntry> load_entry(SqliteDb& db, const Identity& identity) {
  Statement stmt(db.get(), "SELECT project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms FROM config_entries WHERE project = ? AND environment = ? AND key = ?");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  stmt.bind_text(3, identity.key);
  if (!stmt.step_row()) {
    return std::nullopt;
  }
  return row_entry(stmt);
}

inline std::vector<ConfigEntry> list_entries(SqliteDb& db, const Identity& identity) {
  Statement stmt(db.get(), "SELECT project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms FROM config_entries WHERE project = ? AND environment = ? ORDER BY key ASC");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  std::vector<ConfigEntry> entries;
  while (stmt.step_row()) {
    entries.push_back(row_entry(stmt));
  }
  return entries;
}

inline std::vector<ConfigRevision> list_history(SqliteDb& db, const Identity& identity) {
  Statement stmt(db.get(), "SELECT revision_id, project, environment, key, action, description, kind, string_value, bool_value, int_value, updated_by, updated_unix_ms FROM config_revisions WHERE project = ? AND environment = ? AND key = ? ORDER BY revision_id ASC");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  stmt.bind_text(3, identity.key);
  std::vector<ConfigRevision> revisions;
  while (stmt.step_row()) {
    revisions.push_back(row_revision(stmt));
  }
  return revisions;
}

inline std::int64_t current_revision_id(SqliteDb& db, const Identity& identity) {
  const std::optional<ConfigEntry> entry = load_entry(db, identity);
  return entry.has_value() ? entry->revision_id : -1;
}

inline std::int64_t insert_revision(SqliteDb& db,
                                    const Identity& identity,
                                    const std::string& action,
                                    const std::string& description,
                                    const ConfigValue& value,
                                    const std::string& updated_by,
                                    std::int64_t updated_unix_ms) {
  Statement stmt(db.get(), "INSERT INTO config_revisions(project, environment, key, action, description, kind, string_value, bool_value, int_value, updated_by, updated_unix_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  stmt.bind_text(3, identity.key);
  stmt.bind_text(4, action);
  stmt.bind_text(5, description);
  stmt.bind_text(6, value.kind);
  stmt.bind_text(7, value.string_value);
  stmt.bind_int64(8, value.bool_value ? 1 : 0);
  stmt.bind_int64(9, value.int_value);
  stmt.bind_text(10, updated_by);
  stmt.bind_int64(11, updated_unix_ms);
  stmt.step_done();
  return sqlite3_last_insert_rowid(db.get());
}

inline void upsert_entry(SqliteDb& db,
                         const Identity& identity,
                         const std::string& description,
                         const ConfigValue& value,
                         std::int64_t revision_id,
                         const std::string& updated_by,
                         std::int64_t updated_unix_ms) {
  Statement stmt(db.get(), "INSERT OR REPLACE INTO config_entries(project, environment, key, description, kind, string_value, bool_value, int_value, revision_id, updated_by, updated_unix_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  stmt.bind_text(3, identity.key);
  stmt.bind_text(4, description);
  stmt.bind_text(5, value.kind);
  stmt.bind_text(6, value.string_value);
  stmt.bind_int64(7, value.bool_value ? 1 : 0);
  stmt.bind_int64(8, value.int_value);
  stmt.bind_int64(9, revision_id);
  stmt.bind_text(10, updated_by);
  stmt.bind_int64(11, updated_unix_ms);
  stmt.step_done();
}

inline void delete_entry(SqliteDb& db, const Identity& identity) {
  Statement stmt(db.get(), "DELETE FROM config_entries WHERE project = ? AND environment = ? AND key = ?");
  stmt.bind_text(1, identity.project);
  stmt.bind_text(2, identity.environment);
  stmt.bind_text(3, identity.key);
  stmt.step_done();
}

inline Response ok_json(const std::string& body) {
  return Response{200, body, ""};
}

inline Response ok_json_with_etag(const std::string& body, std::int64_t revision_id) {
  return Response{200, body, "\"" + std::to_string(revision_id) + "\""};
}

inline Response handle_put(SqliteDb& db,
                           const Request& request,
                           std::int64_t expected_revision_id,
                           const std::string& updated_by,
                           std::int64_t updated_unix_ms) {
  if (request.method != Method::kPut) {
    fail("put handler received non-PUT request");
  }
  const Identity identity = parse_item_path(request.path);
  validate_identity(identity, updated_by);
  const PutRequest put = parse_put_request(request.body);
  const std::int64_t current_revision = current_revision_id(db, identity);
  if (current_revision >= 0 && expected_revision_id != current_revision) {
    fail("revision conflict");
  }
  const std::int64_t revision_id = insert_revision(db, identity, "put", put.description, put.value, updated_by, updated_unix_ms);
  upsert_entry(db, identity, put.description, put.value, revision_id, updated_by, updated_unix_ms);
  const std::optional<ConfigEntry> entry = load_entry(db, identity);
  if (!entry.has_value()) {
    fail("entry missing after put");
  }
  return ok_json_with_etag(entry_envelope_json(*entry), entry->revision_id);
}

inline Response handle_get(SqliteDb& db, const Request& request) {
  if (request.method != Method::kGet) {
    fail("get handler received non-GET request");
  }
  const Identity identity = parse_item_path(request.path);
  const std::optional<ConfigEntry> entry = load_entry(db, identity);
  if (!entry.has_value()) {
    fail("config entry not found");
  }
  return ok_json_with_etag(entry_envelope_json(*entry), entry->revision_id);
}

inline Response handle_list(SqliteDb& db, const Request& request) {
  if (request.method != Method::kGet) {
    fail("list handler received non-GET request");
  }
  const Identity identity = parse_list_path(request.path);
  return ok_json(list_envelope_json(identity.project, identity.environment, list_entries(db, identity)));
}

inline Response handle_history(SqliteDb& db, const Request& request) {
  if (request.method != Method::kGet) {
    fail("history handler received non-GET request");
  }
  const Identity identity = parse_history_path(request.path);
  return ok_json(history_envelope_json(identity, list_history(db, identity)));
}

inline Response handle_delete(SqliteDb& db,
                              const Request& request,
                              std::int64_t expected_revision_id,
                              const std::string& updated_by,
                              std::int64_t updated_unix_ms) {
  if (request.method != Method::kDelete) {
    fail("delete handler received non-DELETE request");
  }
  const Identity identity = parse_item_path(request.path);
  validate_identity(identity, updated_by);
  const std::optional<ConfigEntry> entry = load_entry(db, identity);
  if (!entry.has_value()) {
    fail("config entry not found");
  }
  if (expected_revision_id != entry->revision_id) {
    fail("delete revision conflict");
  }
  const std::int64_t deleted_revision_id =
      insert_revision(db, identity, "delete", entry->description, entry->value, updated_by, updated_unix_ms);
  delete_entry(db, identity);
  return ok_json(delete_envelope_json(identity, deleted_revision_id));
}

inline std::uint64_t response_checksum(const Response& response) {
  return checksum_text(std::to_string(response.status) + "|" + response.etag + "|" + response.body);
}

inline Response create_step(SqliteDb& db,
                            const std::string& project,
                            const std::string& environment,
                            const std::string& key) {
  const Response response = handle_put(db,
                                       config_request(project, environment, key, "create flag", true),
                                       -1,
                                       "bench-admin",
                                       1001);
  expect_eq(static_cast<int>(extract_int_field(response.body, "revision_id")), 1, "created revision_id");
  if (!extract_bool_field(response.body, "bool_value")) {
    fail("created bool value must be true");
  }
  return response;
}

inline Response update_step(SqliteDb& db,
                            const std::string& project,
                            const std::string& environment,
                            const std::string& key,
                            std::int64_t expected_revision_id) {
  const Response response = handle_put(db,
                                       config_request(project, environment, key, "disable flag", false),
                                       expected_revision_id,
                                       "bench-admin",
                                       1002);
  expect_eq(static_cast<int>(extract_int_field(response.body, "revision_id")), 2, "updated revision_id");
  if (extract_bool_field(response.body, "bool_value")) {
    fail("updated bool value must be false");
  }
  return response;
}

inline Response verify_get_step(SqliteDb& db,
                                const std::string& project,
                                const std::string& environment,
                                const std::string& key) {
  const Response response = handle_get(db, config_get_request(project, environment, key));
  expect_eq(extract_string_field(response.body, "description"), "disable flag", "loaded description");
  return response;
}

inline Response verify_list_step(SqliteDb& db, const std::string& project, const std::string& environment) {
  const Response response = handle_list(db, config_list_request(project, environment));
  expect_eq(static_cast<int>(count_occurrences(response.body, "\"revision_id\":")), 1, "list entry count");
  return response;
}

inline Response verify_history_step(SqliteDb& db,
                                    const std::string& project,
                                    const std::string& environment,
                                    const std::string& key,
                                    int expected_count,
                                    const std::string& label) {
  const Response response = handle_history(db, config_history_request(project, environment, key));
  expect_eq(static_cast<int>(count_occurrences(response.body, "\"revision_id\":")), expected_count, label);
  return response;
}

inline Response delete_step(SqliteDb& db,
                            const std::string& project,
                            const std::string& environment,
                            const std::string& key,
                            std::int64_t expected_revision_id) {
  const Response response = handle_delete(db,
                                          config_delete_request(project, environment, key),
                                          expected_revision_id,
                                          "bench-admin",
                                          1003);
  if (!extract_bool_field(response.body, "deleted")) {
    fail("deleted flag must be true");
  }
  expect_eq(static_cast<int>(extract_int_field(response.body, "deleted_revision_id")), 3, "deleted revision_id");
  return response;
}

inline std::uint64_t run_catalog_lifecycle(SqliteDb& db,
                                           const std::string& project,
                                           const std::string& environment,
                                           const std::string& key) {
  std::uint64_t checksum = 0;
  const Response created = create_step(db, project, environment, key);
  checksum ^= response_checksum(created);
  const Response updated = update_step(db, project, environment, key, extract_int_field(created.body, "revision_id"));
  checksum ^= response_checksum(updated);
  checksum ^= response_checksum(verify_get_step(db, project, environment, key));
  checksum ^= response_checksum(verify_list_step(db, project, environment));
  checksum ^= response_checksum(verify_history_step(db, project, environment, key, 2, "history revision count before delete"));
  checksum ^= response_checksum(delete_step(db, project, environment, key, extract_int_field(updated.body, "revision_id")));
  checksum ^= response_checksum(verify_history_step(db, project, environment, key, 3, "history revision count after delete"));
  return checksum;
}

inline std::uint64_t run_workload_once() {
  std::uint64_t checksum = 0;
  cleanup_db_files();
  {
    SqliteDb db = open_catalog();
    checksum = run_catalog_lifecycle(db, "control-plane", "prod", "feature-flag");
  }
  cleanup_db_files();
  return checksum;
}

}  // namespace app_platform_cpp::service_json_db_crud
