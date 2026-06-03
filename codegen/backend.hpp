#pragma once

#include "frontend/runtime_profile.hpp"
#include "nir/ir.hpp"
#include "passes/rep_owner_infer.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::codegen {

using nebula::frontend::PanicPolicy;
using nebula::frontend::RuntimeProfile;

enum class MainMode : std::uint8_t { None, CallMainIfPresent, RunTests, RunBench };

struct EmitOptions {
  MainMode main_mode = MainMode::None;
  bool strict_region = false; // if true, prefer trapping on region-escape sites
  RuntimeProfile runtime_profile = RuntimeProfile::Hosted;
  PanicPolicy panic_policy = PanicPolicy::Abort;
  std::string target = "host";
  bool emit_c_abi_wrappers = false;
  std::optional<std::string> c_abi_export_package;
};

struct CAbiFunction {
  std::string export_name;
  nebula::frontend::QualifiedName qualified_name;
  std::string local_name;
  std::vector<nebula::nir::Param> params;
  nebula::frontend::Ty ret = nebula::frontend::Ty::Void();
};

class Backend {
public:
  virtual ~Backend() = default;

  virtual std::string name() const = 0;

  virtual std::string emit_translation_unit(
      const nebula::nir::Program& p,
      const nebula::passes::RepOwnerResult& rep_owner,
      const EmitOptions& opt) const = 0;

  virtual std::vector<CAbiFunction> collect_c_abi_exports(
      const nebula::nir::Program& p,
      std::optional<std::string_view> package_name = std::nullopt) const = 0;

  virtual std::string emit_c_abi_header(
      const nebula::nir::Program& p,
      const std::vector<CAbiFunction>& exports,
      std::string_view header_stem) const = 0;
};

// The C++23 backend is the default and only production backend.
const Backend& cpp23_backend();
const Backend& default_backend();

} // namespace nebula::codegen
