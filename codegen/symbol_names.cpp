#include "codegen/symbol_names.hpp"

#include <cctype>
#include <cstdint>
#include <sstream>

namespace nebula::codegen {

std::string stable_symbol_hash(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::string sanitize_cpp_identifier_piece(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty())
    return "fn";
  if (std::isdigit(static_cast<unsigned char>(out.front())) != 0)
    out.insert(out.begin(), '_');
  return out;
}

std::string emitted_cpp_function_name(const nebula::nir::Function &fn) {
  if (fn.is_extern)
    return fn.name;
  const std::string identity = nebula::nir::function_identity(fn);
  return "__nebula_fn_" + stable_symbol_hash(identity) + "_" +
         sanitize_cpp_identifier_piece(fn.name);
}

} // namespace nebula::codegen
