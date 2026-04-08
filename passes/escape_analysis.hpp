#pragma once

#include "nir/ir.hpp"
#include "passes/call_target_resolver.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace nebula::passes {

enum class EscapeAnalysisProfile : std::uint8_t { Fast, Deep };

struct EscapeAnalysisOptions {
  EscapeAnalysisProfile profile = EscapeAnalysisProfile::Fast;
};

// Interprocedural escape summaries (v0.3-a).
//
// This pass is conservative-by-construction:
// - Unknown/external calls: assume arguments may escape and return may depend on any argument.
// - Known calls: use the callee summary (solved to a fixpoint over the call graph).
struct FuncEscapeSummary {
  std::vector<bool> return_depends_on_param; // size = #params
  std::vector<bool> param_may_escape;        // size = #params
  std::vector<bool> param_escape_unknown;    // size = #params; true => may_escape source precision unknown
};

struct EscapeAnalysisResult {
  std::unordered_map<std::string, FuncEscapeSummary> by_function;
};

EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p);
EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p,
                                         const CallTargetResolution& resolution);
EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p,
                                         const CallTargetResolution& resolution,
                                         const EscapeAnalysisOptions& options);

} // namespace nebula::passes
