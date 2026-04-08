#pragma once

#include "nir/ir.hpp"
#include "passes/rep_owner_infer.hpp"

#include <string>
#include <vector>

namespace nebula::codegen {

enum class MainMode : std::uint8_t { None, CallMainIfPresent, RunTests, RunBench };

struct EmitOptions {
  MainMode main_mode = MainMode::None;
  bool strict_region = false; // if true, prefer trapping on region-escape sites
};

// Emit a single C++23 translation unit.
std::string emit_cpp23(const nebula::nir::Program& p, const nebula::passes::RepOwnerResult& rep_owner,
                       const EmitOptions& opt = {});

} // namespace nebula::codegen


