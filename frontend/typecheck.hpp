#pragma once

#include "frontend/ast.hpp"
#include "frontend/diagnostic.hpp"
#include "frontend/typed_ast.hpp"

#include <optional>
#include <vector>

namespace nebula::frontend {

struct TypecheckOptions {
  bool warnings_as_errors = false;
};

struct CompilationUnit {
  const Program* program = nullptr;
  std::string package_name;
  std::string source_path;
  std::vector<std::string> resolved_imports;
};

struct TypecheckResult {
  std::optional<TProgram> program;
  std::vector<Diagnostic> diags;
};

struct TypecheckUnitsResult {
  std::vector<TProgram> programs;
  std::vector<Diagnostic> diags;
};

TypecheckResult typecheck(const Program& ast, const TypecheckOptions& opt = {});
TypecheckUnitsResult typecheck(const std::vector<CompilationUnit>& units,
                               const TypecheckOptions& opt = {});

} // namespace nebula::frontend
