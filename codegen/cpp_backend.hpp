#pragma once

#include "codegen/backend.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::codegen {

// Emit a single C++23 translation unit.
std::string emit_cpp23(const nebula::nir::Program& p, const nebula::passes::RepOwnerResult& rep_owner,
                       const EmitOptions& opt = {});

std::vector<CAbiFunction> collect_c_abi_functions(
    const nebula::nir::Program& p,
    std::optional<std::string_view> package_name = std::nullopt);
std::string emit_c_abi_header(const nebula::nir::Program& p,
                              const std::vector<CAbiFunction>& exports,
                              std::string_view header_stem);

} // namespace nebula::codegen
