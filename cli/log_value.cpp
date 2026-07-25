#include "log_value.hpp"

#include <algorithm>
#include <cstddef>

std::string quote_cli_log_value(std::string_view value) {
  std::string result;
  constexpr std::size_t kMaxLoggedBytes = 1024U;
  const std::size_t logged_size = std::min(value.size(), kMaxLoggedBytes);
  result.reserve(logged_size + 32U);
  result.push_back('"');
  constexpr char kHex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < logged_size; ++index) {
    const char character = value[index];
    const unsigned char byte = static_cast<unsigned char>(character);
    if (character == '\\' || character == '"') {
      result.push_back('\\');
      result.push_back(character);
    } else if (character == '\n') {
      result += "\\n";
    } else if (character == '\r') {
      result += "\\r";
    } else if (character == '\t') {
      result += "\\t";
    } else if (byte < 0x20U || byte >= 0x7fU) {
      result += "\\x";
      result.push_back(kHex[byte >> 4U]);
      result.push_back(kHex[byte & 0x0fU]);
    } else {
      result.push_back(character);
    }
  }
  if (logged_size != value.size())
    result += "...<truncated>";
  result.push_back('"');
  return result;
}
