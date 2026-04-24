#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace nebula::cli::json {

namespace {

void append_utf8(std::string& out, std::uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

bool is_hex_digit(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

std::uint16_t hex_digit_value(char ch) {
  if (ch >= '0' && ch <= '9') return static_cast<std::uint16_t>(ch - '0');
  if (ch >= 'a' && ch <= 'f') return static_cast<std::uint16_t>(10 + ch - 'a');
  return static_cast<std::uint16_t>(10 + ch - 'A');
}

std::string escape_string(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    switch (ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        constexpr char digits[] = "0123456789abcdef";
        const unsigned char value = static_cast<unsigned char>(ch);
        out += "\\u00";
        out.push_back(digits[(value >> 4) & 0x0F]);
        out.push_back(digits[value & 0x0F]);
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  return out;
}

class Parser {
 public:
  Parser(std::string_view text, ParseError* error) : text_(text), error_(error) {}

  std::optional<Value> parse_root() {
    skip_ws();
    auto value = parse_value();
    if (!value.has_value()) return std::nullopt;
    skip_ws();
    if (pos_ != text_.size()) {
      fail("trailing characters after JSON value");
      return std::nullopt;
    }
    return value;
  }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
  ParseError* error_ = nullptr;

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) pos_ += 1;
  }

  void fail(std::string message) {
    if (error_ != nullptr && error_->message.empty()) {
      error_->offset = pos_;
      error_->message = std::move(message);
    }
  }

  bool consume(char expected) {
    skip_ws();
    if (pos_ >= text_.size() || text_[pos_] != expected) {
      fail(std::string("expected '") + expected + "'");
      return false;
    }
    pos_ += 1;
    return true;
  }

  bool consume_literal(std::string_view literal) {
    skip_ws();
    if (text_.substr(pos_, literal.size()) != literal) {
      fail("expected literal " + std::string(literal));
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  std::optional<Value> parse_value() {
    skip_ws();
    if (pos_ >= text_.size()) {
      fail("unexpected end of input");
      return std::nullopt;
    }

    const char ch = text_[pos_];
    if (ch == '{') return parse_object();
    if (ch == '[') return parse_array();
    if (ch == '"') {
      auto str = parse_string();
      if (!str.has_value()) return std::nullopt;
      return Value(std::move(*str));
    }
    if (ch == 't') {
      if (!consume_literal("true")) return std::nullopt;
      return Value(true);
    }
    if (ch == 'f') {
      if (!consume_literal("false")) return std::nullopt;
      return Value(false);
    }
    if (ch == 'n') {
      if (!consume_literal("null")) return std::nullopt;
      return Value(nullptr);
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return parse_number();

    fail("unexpected character while parsing JSON value");
    return std::nullopt;
  }

  std::optional<std::uint16_t> parse_unicode_escape_quad() {
    if (pos_ + 4 > text_.size()) {
      fail("incomplete unicode escape");
      return std::nullopt;
    }

    std::uint16_t code_unit = 0;
    for (int i = 0; i < 4; ++i) {
      const char hex = text_[pos_++];
      if (!is_hex_digit(hex)) {
        fail("invalid unicode escape");
        return std::nullopt;
      }
      code_unit = static_cast<std::uint16_t>((code_unit << 4) | hex_digit_value(hex));
    }
    return code_unit;
  }

  std::optional<std::string> parse_string() {
    skip_ws();
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      fail("expected string");
      return std::nullopt;
    }
    pos_ += 1;
    std::string out;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') return out;
      if (ch == '\\') {
        if (pos_ >= text_.size()) {
          fail("unterminated escape sequence");
          return std::nullopt;
        }
        const char esc = text_[pos_++];
        switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          auto first = parse_unicode_escape_quad();
          if (!first.has_value()) return std::nullopt;

          const std::uint16_t first_unit = *first;
          if (first_unit >= 0xD800 && first_unit <= 0xDBFF) {
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              fail("missing low surrogate after high surrogate");
              return std::nullopt;
            }
            pos_ += 2;
            auto second = parse_unicode_escape_quad();
            if (!second.has_value()) return std::nullopt;
            const std::uint16_t second_unit = *second;
            if (second_unit < 0xDC00 || second_unit > 0xDFFF) {
              fail("invalid low surrogate");
              return std::nullopt;
            }
            const std::uint32_t codepoint =
                0x10000u + ((static_cast<std::uint32_t>(first_unit) - 0xD800u) << 10)
                + (static_cast<std::uint32_t>(second_unit) - 0xDC00u);
            append_utf8(out, codepoint);
          } else if (first_unit >= 0xDC00 && first_unit <= 0xDFFF) {
            fail("unexpected low surrogate without leading high surrogate");
            return std::nullopt;
          } else {
            append_utf8(out, first_unit);
          }
          break;
        }
        default:
          fail("invalid escape sequence");
          return std::nullopt;
        }
      } else {
        if (static_cast<unsigned char>(ch) < 0x20) {
          fail("unescaped control character in string");
          return std::nullopt;
        }
        out.push_back(ch);
      }
    }
    fail("unterminated string");
    return std::nullopt;
  }

  std::optional<Value> parse_number() {
    skip_ws();
    const std::size_t start = pos_;
    if (text_[pos_] == '-') pos_ += 1;
    if (pos_ >= text_.size()) {
      fail("incomplete number");
      return std::nullopt;
    }
    if (text_[pos_] == '0') {
      pos_ += 1;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        fail("invalid number");
        return std::nullopt;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_ += 1;
    }

    bool is_double = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_double = true;
      pos_ += 1;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        fail("invalid fractional number");
        return std::nullopt;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_ += 1;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_double = true;
      pos_ += 1;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_ += 1;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        fail("invalid exponent");
        return std::nullopt;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_ += 1;
    }

    const std::string raw(text_.substr(start, pos_ - start));
    try {
      if (is_double) return Value(std::stod(raw));
      return Value(static_cast<std::int64_t>(std::stoll(raw)));
    } catch (...) {
      fail("numeric conversion failed");
      return std::nullopt;
    }
  }

  std::optional<Value> parse_array() {
    if (!consume('[')) return std::nullopt;
    Value::Array out;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      pos_ += 1;
      return Value(std::move(out));
    }
    while (true) {
      auto value = parse_value();
      if (!value.has_value()) return std::nullopt;
      out.push_back(std::move(*value));
      skip_ws();
      if (pos_ >= text_.size()) {
        fail("unterminated array");
        return std::nullopt;
      }
      if (text_[pos_] == ']') {
        pos_ += 1;
        return Value(std::move(out));
      }
      if (text_[pos_] != ',') {
        fail("expected ',' or ']'");
        return std::nullopt;
      }
      pos_ += 1;
    }
  }

  std::optional<Value> parse_object() {
    if (!consume('{')) return std::nullopt;
    Value::Object out;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      pos_ += 1;
      return Value(std::move(out));
    }
    while (true) {
      auto key = parse_string();
      if (!key.has_value()) return std::nullopt;
      if (!consume(':')) return std::nullopt;
      auto value = parse_value();
      if (!value.has_value()) return std::nullopt;
      out.insert_or_assign(std::move(*key), std::move(*value));
      skip_ws();
      if (pos_ >= text_.size()) {
        fail("unterminated object");
        return std::nullopt;
      }
      if (text_[pos_] == '}') {
        pos_ += 1;
        return Value(std::move(out));
      }
      if (text_[pos_] != ',') {
        fail("expected ',' or '}'");
        return std::nullopt;
      }
      pos_ += 1;
    }
  }
};

void render_into(std::ostringstream& out, const Value& value) {
  if (const auto* ptr = std::get_if<std::nullptr_t>(&value.storage)) {
    (void)ptr;
    out << "null";
  } else if (const auto* ptr = std::get_if<bool>(&value.storage)) {
    out << (*ptr ? "true" : "false");
  } else if (const auto* ptr = std::get_if<std::int64_t>(&value.storage)) {
    out << *ptr;
  } else if (const auto* ptr = std::get_if<double>(&value.storage)) {
    out << *ptr;
  } else if (const auto* ptr = std::get_if<std::string>(&value.storage)) {
    out << '"' << escape_string(*ptr) << '"';
  } else if (const auto* ptr = std::get_if<Value::Array>(&value.storage)) {
    out << "[";
    for (std::size_t i = 0; i < ptr->size(); ++i) {
      if (i) out << ",";
      render_into(out, (*ptr)[i]);
    }
    out << "]";
  } else {
    const auto& obj = std::get<Value::Object>(value.storage);
    out << "{";
    bool first = true;
    for (const auto& [key, child] : obj) {
      if (!first) out << ",";
      first = false;
      out << '"' << escape_string(key) << "\":";
      render_into(out, child);
    }
    out << "}";
  }
}

} // namespace

bool Value::is_null() const { return std::holds_alternative<std::nullptr_t>(storage); }
bool Value::is_bool() const { return std::holds_alternative<bool>(storage); }
bool Value::is_int() const { return std::holds_alternative<std::int64_t>(storage); }
bool Value::is_double() const { return std::holds_alternative<double>(storage); }
bool Value::is_string() const { return std::holds_alternative<std::string>(storage); }
bool Value::is_array() const { return std::holds_alternative<Array>(storage); }
bool Value::is_object() const { return std::holds_alternative<Object>(storage); }

const bool* Value::as_bool() const { return std::get_if<bool>(&storage); }
const std::int64_t* Value::as_int() const { return std::get_if<std::int64_t>(&storage); }
const double* Value::as_double() const { return std::get_if<double>(&storage); }
const std::string* Value::as_string() const { return std::get_if<std::string>(&storage); }
const Value::Array* Value::as_array() const { return std::get_if<Array>(&storage); }
const Value::Object* Value::as_object() const { return std::get_if<Object>(&storage); }

std::optional<Value> parse(std::string_view text, ParseError* error) {
  if (error != nullptr) *error = ParseError{};
  Parser parser(text, error);
  return parser.parse_root();
}

std::string render_compact(const Value& value) {
  std::ostringstream out;
  render_into(out, value);
  return out.str();
}

const Value* object_get(const Value& value, std::string_view key) {
  const auto* object = value.as_object();
  if (object == nullptr) return nullptr;
  auto it = object->find(std::string(key));
  if (it == object->end()) return nullptr;
  return &it->second;
}

const Value* array_get(const Value& value, std::size_t index) {
  const auto* array = value.as_array();
  if (array == nullptr || index >= array->size()) return nullptr;
  return &(*array)[index];
}

const Value* value_at(const Value& value, std::initializer_list<std::string_view> path) {
  const Value* current = &value;
  for (auto key : path) {
    current = object_get(*current, key);
    if (current == nullptr) return nullptr;
  }
  return current;
}

std::optional<std::string> string_at(const Value& value, std::initializer_list<std::string_view> path) {
  const Value* current = value_at(value, path);
  if (current == nullptr) return std::nullopt;
  if (const auto* text = current->as_string()) return *text;
  return std::nullopt;
}

std::optional<std::int64_t> int_at(const Value& value, std::initializer_list<std::string_view> path) {
  const Value* current = value_at(value, path);
  if (current == nullptr) return std::nullopt;
  if (const auto* iv = current->as_int()) return *iv;
  return std::nullopt;
}

} // namespace nebula::cli::json
