#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nebula::cli::json {

struct Value {
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

  Storage storage = nullptr;

  Value() = default;
  Value(std::nullptr_t value) : storage(value) {}
  Value(bool value) : storage(value) {}
  Value(std::int64_t value) : storage(value) {}
  Value(double value) : storage(value) {}
  Value(std::string value) : storage(std::move(value)) {}
  Value(const char* value) : storage(std::string(value)) {}
  Value(Array value) : storage(std::move(value)) {}
  Value(Object value) : storage(std::move(value)) {}

  bool is_null() const;
  bool is_bool() const;
  bool is_int() const;
  bool is_double() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  const bool* as_bool() const;
  const std::int64_t* as_int() const;
  const double* as_double() const;
  const std::string* as_string() const;
  const Array* as_array() const;
  const Object* as_object() const;
};

struct ParseError {
  std::size_t offset = 0;
  std::string message;
};

std::optional<Value> parse(std::string_view text, ParseError* error = nullptr);
std::string render_compact(const Value& value);

const Value* object_get(const Value& value, std::string_view key);
const Value* array_get(const Value& value, std::size_t index);
const Value* value_at(const Value& value, std::initializer_list<std::string_view> path);
std::optional<std::string> string_at(const Value& value, std::initializer_list<std::string_view> path);
std::optional<std::int64_t> int_at(const Value& value, std::initializer_list<std::string_view> path);

} // namespace nebula::cli::json
