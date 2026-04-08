#pragma once

#include "frontend/diagnostic.hpp"
#include "nir/ir.hpp"
#include "passes/rep_owner_infer.hpp"

#include <vector>

namespace nebula::passes {

enum class EpistemicLintProfile : std::uint8_t { Fast, Deep };

struct EpistemicLintOptions {
  bool warnings_as_errors = false;
  EpistemicLintProfile profile = EpistemicLintProfile::Fast;
};

// v0.1: a small set of static performance/latency-oriented diagnostics.
std::vector<nebula::frontend::Diagnostic> run_epistemic_lint(const nebula::nir::Program& p,
                                                            const RepOwnerResult& rep_owner,
                                                            const EpistemicLintOptions& opt = {});

} // namespace nebula::passes

