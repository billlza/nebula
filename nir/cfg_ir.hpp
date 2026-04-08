#pragma once

#include "frontend/source.hpp"
#include "frontend/types.hpp"
#include "nir/ir.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::nir::cfgir {

using frontend::Span;
using frontend::Ty;
using nir::BinOp;
using nir::PrefixKind;
using nir::UnaryOp;
using nir::VarId;

using BlockId = std::uint32_t;
using ValueId = std::uint32_t;

struct Phi {
  ValueId result{};
  Ty ty = Ty::Unknown();
  std::string debug_name;
  std::vector<std::pair<BlockId, ValueId>> incomings;
};

struct Op {
  struct Param {
    VarId var{};
    std::string name;
  };
  struct ConstInt {
    std::int64_t value = 0;
  };
  struct ConstBool {
    bool value = false;
  };
  struct ConstFloat {
    double value = 0.0;
  };
  struct ConstString {
    std::string value;
  };
  struct Bin {
    BinOp op{};
    ValueId lhs{};
    ValueId rhs{};
  };
  struct Unary {
    UnaryOp op{};
    ValueId value{};
  };
  struct CmpLt {
    ValueId lhs{};
    ValueId rhs{};
  };
  struct Call {
    std::string callee;
    std::vector<ValueId> args;
  };
  struct LoadField {
    ValueId base{};
    std::string field;
  };
  struct StoreField {
    ValueId base{};
    std::string field;
    ValueId value{};
  };
  struct Construct {
    std::string type_name;
    std::vector<ValueId> args;
  };
  struct AllocHint {
    PrefixKind kind{};
    std::string type_name;
    std::vector<ValueId> args;
  };
  struct Undef {};

  std::variant<Param, ConstInt, ConstBool, ConstFloat, ConstString, Bin, Unary, CmpLt, Call,
               LoadField, StoreField, Construct, AllocHint, Undef>
      node;
};

struct Instr {
  ValueId result{};
  Ty ty = Ty::Unknown();
  Span span{};
  std::string debug_name;
  Op op;
};

struct Terminator {
  struct Jump {
    BlockId target{};
  };
  struct Branch {
    ValueId cond{};
    BlockId then_bb{};
    BlockId else_bb{};
  };
  struct Ret {
    std::optional<ValueId> value;
  };

  Span span{};
  std::variant<Jump, Branch, Ret> node;
};

struct BasicBlock {
  BlockId id{};
  std::vector<std::string> region_stack;
  std::vector<std::string> annotation_stack;

  std::vector<Phi> phis;
  std::vector<Instr> instrs;
  std::optional<Terminator> term;

  std::vector<BlockId> preds;
  std::vector<BlockId> succs;
};

struct FunctionCFG {
  std::string name;
  BlockId entry{};
  std::vector<BasicBlock> blocks;
};

FunctionCFG lower_to_cfg_ir(const nir::Function& fn);
std::string dump_cfg_ir(const FunctionCFG& f);

} // namespace nebula::nir::cfgir
