#pragma once

#include "frontend/typed_ast.hpp"
#include "nir/ir.hpp"

namespace nebula::nir {

Program lower_to_nir(const nebula::frontend::TProgram& t);
Program lower_to_nir(const std::vector<nebula::frontend::TProgram>& programs);

} // namespace nebula::nir

