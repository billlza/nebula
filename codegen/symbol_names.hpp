#pragma once

#include "nir/ir.hpp"

#include <string>
#include <string_view>

namespace nebula::codegen {

// Stable, backend-owned symbol spelling shared by hosted and freestanding C++
// emission. Language/NIR layers intentionally do not know these names.
std::string stable_symbol_hash(std::string_view text);
std::string sanitize_cpp_identifier_piece(std::string_view text);
std::string emitted_cpp_function_name(const nebula::nir::Function &fn);

} // namespace nebula::codegen
