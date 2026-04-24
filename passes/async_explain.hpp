#pragma once

#include "frontend/source.hpp"
#include "nir/ir.hpp"

#include <string>
#include <vector>

namespace nebula::passes {

struct AsyncExplainEntry {
  std::string kind;
  std::string function_name;
  std::string path;
  nebula::frontend::Span span{};
  std::string summary;
  std::string allocation;
  std::string reason;
  std::vector<std::string> carried_values;
  bool suspension_point = false;
  bool task_boundary = false;
};

struct AsyncExplainResult {
  std::vector<AsyncExplainEntry> entries;
};

AsyncExplainResult run_async_explain(const nebula::nir::Program& program);

} // namespace nebula::passes
