#pragma once

#include "frontend/diagnostic.hpp"
#include "passes/escape_analysis.hpp"
#include "nir/ir.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace nebula::passes {

// Mentor-aligned model: Representation × Ownership.
//
// - Region is an allocation domain (representation), not ownership.
// - Ownership only applies to heap representations in v0.1.
enum class RepKind : std::uint8_t { Stack, Region, Heap };
enum class OwnerKind : std::uint8_t { None, Unique, Shared };

struct StorageDecision {
  RepKind rep = RepKind::Stack;
  OwnerKind owner = OwnerKind::None; // only meaningful when rep == Heap
  std::string region;               // only meaningful when rep == Region
};

struct FuncRepOwner {
  std::unordered_map<nebula::nir::VarId, StorageDecision> vars;
};

struct RepOwnerOptions {
  bool strict_region = false;       // `--strict-region`
  bool warnings_as_errors = false;  // `--warnings-as-errors`
};

struct RepOwnerResult {
  std::unordered_map<std::string, FuncRepOwner> by_function;
  std::vector<nebula::frontend::Diagnostic> diags;
};

RepOwnerResult run_rep_owner_infer(const nebula::nir::Program& p,
                                  const EscapeAnalysisResult& escape,
                                  const RepOwnerOptions& opt,
                                  const CallTargetResolution* call_resolution = nullptr);

} // namespace nebula::passes

