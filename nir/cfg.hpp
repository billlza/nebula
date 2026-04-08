#pragma once

#include "nir/ir.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace nebula::nir {

using NodeId = std::size_t;

enum class NodeKind : std::uint8_t { Entry, Exit, Stmt };

struct CfgNode {
  NodeId id{};
  NodeKind kind = NodeKind::Stmt;
  const Stmt* stmt = nullptr; // null for Entry/Exit
  std::vector<NodeId> succ;

  // Context for analyses (computed structurally from NIR).
  std::vector<std::string> region_stack;
  std::vector<std::string> annotation_stack;
};

struct Cfg {
  NodeId entry{};
  NodeId exit{};
  std::vector<CfgNode> nodes;
};

Cfg build_cfg(const Function& f);

} // namespace nebula::nir


