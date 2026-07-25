#pragma once

#include "frontend/diagnostic.hpp"
#include "frontend/runtime_profile.hpp"
#include "nir/ir.hpp"
#include "passes/rep_owner_infer.hpp"

#include <optional>
#include <string>
#include <vector>

namespace nebula::codegen {

struct FreestandingEmitOptions {
  std::string target;
  nebula::frontend::PanicPolicy panic_policy = nebula::frontend::PanicPolicy::Abort;
  std::optional<std::string> root_package;
};

struct FreestandingCppEmission {
  std::optional<std::string> translation_unit;
  std::vector<nebula::frontend::Diagnostic> diagnostics;

  [[nodiscard]] bool ok() const { return translation_unit.has_value() && diagnostics.empty(); }
};

// Emit the deliberately small, freestanding C++ bootstrap subset used by the
// x86_64 object gate. This does not use hosted cpp_type/runtime lowering.
FreestandingCppEmission emit_freestanding_cpp(const nebula::nir::Program &program,
                                              const nebula::passes::RepOwnerResult &rep_owner,
                                              const FreestandingEmitOptions &options);

} // namespace nebula::codegen
