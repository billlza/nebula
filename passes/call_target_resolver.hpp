#pragma once

#include "nir/ir.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace nebula::passes {

struct ResolvedTargetState {
  enum class Kind : std::uint8_t { Known, Unknown };

  Kind kind = Kind::Unknown;
  std::string callee;

  static ResolvedTargetState known(std::string callee_name) {
    ResolvedTargetState out;
    out.kind = Kind::Known;
    out.callee = std::move(callee_name);
    return out;
  }

  static ResolvedTargetState unknown() { return {}; }

  bool is_known() const { return kind == Kind::Known; }
};

struct FunctionCallTargetResolution {
  std::unordered_map<const nebula::nir::Expr::Call*, ResolvedTargetState> by_call;

  ResolvedTargetState resolve(const nebula::nir::Expr::Call* call) const {
    if (call == nullptr) return ResolvedTargetState::unknown();
    auto it = by_call.find(call);
    if (it == by_call.end()) return ResolvedTargetState::unknown();
    return it->second;
  }
};

struct CallTargetResolution {
  std::unordered_map<std::string, FunctionCallTargetResolution> by_function;

  ResolvedTargetState resolve(const std::string& function_name,
                              const nebula::nir::Expr::Call* call) const {
    auto fit = by_function.find(function_name);
    if (fit == by_function.end()) return ResolvedTargetState::unknown();
    return fit->second.resolve(call);
  }
};

CallTargetResolution run_call_target_resolver(const nebula::nir::Program& p);

} // namespace nebula::passes

