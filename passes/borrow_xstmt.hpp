#pragma once

#include "frontend/diagnostic.hpp"
#include "nir/ir.hpp"
#include "passes/call_target_resolver.hpp"
#include "passes/escape_analysis.hpp"

#include <vector>

namespace nebula::passes {

struct BorrowXStmtResult {
  std::vector<nebula::frontend::Diagnostic> diags;
};

BorrowXStmtResult run_borrow_xstmt(const nebula::nir::Program& p,
                                   const EscapeAnalysisResult& escape);
BorrowXStmtResult run_borrow_xstmt(const nebula::nir::Program& p,
                                   const EscapeAnalysisResult& escape,
                                   const CallTargetResolution& resolution);

} // namespace nebula::passes
