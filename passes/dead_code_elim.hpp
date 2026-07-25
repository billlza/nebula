#pragma once

#include "nir/ir.hpp"

#include <cstddef>

namespace nebula::passes {

struct DeadCodeElimResult {
  // Number of `let` bindings removed across the whole program.
  std::size_t removed_bindings = 0;
};

// Conservative, machine-independent NIR-level dead-code elimination.
//
// Removes `let` bindings whose bound variable is never referenced anywhere in
// its enclosing function AND whose initializer is provably side-effect free
// (literals, variable/field reads, struct construction, and pure arithmetic;
// any `call`, `await`, or `match` initializer is treated as impure and kept).
//
// The transform is output-equivalent by construction: a dead binding with a
// pure initializer can be deleted without changing any observable behavior. It
// runs after all diagnostic/lint passes so warnings still reflect the program
// as written, while codegen consumes the optimized NIR. Iterates to a fixpoint
// so a removal that makes a prior pure binding dead is also collected.
DeadCodeElimResult run_dead_code_elim(nebula::nir::Program& p);

} // namespace nebula::passes
