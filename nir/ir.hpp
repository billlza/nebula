#pragma once

#include "frontend/source.hpp"
#include "frontend/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::nir {

using frontend::Span;
using frontend::Ty;

struct Expr;
struct Stmt;

using ExprPtr = std::unique_ptr<Expr>;
using VarId = std::uint32_t;

enum class PrefixKind : std::uint8_t { Shared, Unique, Heap, Promote };
enum class UnaryOp : std::uint8_t { Not };
enum class BinOp : std::uint8_t {
  Add,
  Sub,
  Mul,
  Div,
  Mod,
  Eq,
  Ne,
  Lt,
  Lte,
  Gt,
  Gte,
  And,
  Or,
};
enum class CallKind : std::uint8_t { Direct, Indirect };

struct Expr {
  struct IntLit {
    std::int64_t value = 0;
  };
  struct BoolLit {
    bool value = false;
  };
  struct FloatLit {
    double value = 0.0;
  };
  struct StringLit {
    std::string value;
  };
  struct VarRef {
    VarId var{};
    std::string name;
    std::optional<frontend::QualifiedName> top_level_symbol;
  };
  struct Call {
    std::string callee;
    std::optional<frontend::QualifiedName> resolved_callee;
    std::vector<ExprPtr> args;
    std::vector<bool> args_ref;
    CallKind kind = CallKind::Direct;
    VarId callee_var{};
  };
  struct FieldRef {
    VarId base_var{};
    std::string base_name;
    std::string field;
  };
  struct Construct {
    std::string type_name;
    std::optional<frontend::QualifiedName> resolved_type;
    std::optional<std::string> variant_name;
    std::optional<std::uint32_t> variant_index;
    std::vector<ExprPtr> args;
  };
  struct Binary {
    BinOp op{};
    ExprPtr lhs;
    ExprPtr rhs;
  };
  struct Unary {
    UnaryOp op{};
    ExprPtr inner;
  };
  struct Prefix {
    PrefixKind kind{};
    ExprPtr inner;
  };

  Span span{};
  Ty ty = Ty::Unknown();
  std::variant<IntLit, BoolLit, FloatLit, StringLit, VarRef, Call, FieldRef, Construct, Binary,
               Unary, Prefix>
      node;
};

struct Block {
  Span span{};
  std::vector<Stmt> stmts;
};

struct Stmt {
  struct Let {
    VarId var{};
    std::string name;
    Ty ty = Ty::Unknown();
    ExprPtr value;
  };
  struct Return {
    ExprPtr value;
  };
  struct ExprStmt {
    ExprPtr expr;
  };
  struct AssignVar {
    VarId var{};
    std::string name;
    Ty ty = Ty::Unknown();
    ExprPtr value;
  };
  struct AssignField {
    VarId base_var{};
    std::string base_name;
    std::string field;
    Ty ty = Ty::Unknown();
    ExprPtr value;
  };
  struct Region {
    std::string name;
    Block body;
  };
  struct Unsafe {
    Block body;
  };
  struct If {
    ExprPtr cond;
    Block then_body;
    std::optional<Block> else_body;
  };
  struct For {
    VarId var{};
    std::string var_name;
    Ty var_ty = Ty::Int();
    ExprPtr start;
    ExprPtr end;
    Block body;
  };

  Span span{};
  std::vector<std::string> annotations;
  std::variant<Let, Return, ExprStmt, AssignVar, AssignField, Region, Unsafe, If, For> node;
};

struct Param {
  Span span{};
  VarId var{};
  bool is_ref = false;
  std::string name;
  Ty ty = Ty::Unknown();
};

struct Field {
  Span span{};
  std::string name;
  Ty ty = Ty::Unknown();
};

struct Variant {
  Span span{};
  std::string name;
  Ty payload = Ty::Unknown();
};

struct Function {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  frontend::QualifiedName qualified_name;
  std::vector<Param> params;
  Ty ret = Ty::Void();
  bool is_extern = false;
  std::optional<Block> body;
};

struct StructDef {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  frontend::QualifiedName qualified_name;
  std::vector<Field> fields;
};

struct EnumDef {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  frontend::QualifiedName qualified_name;
  std::string type_param;
  std::vector<Variant> variants;
};

struct Item {
  Span span{};
  std::variant<Function, StructDef, EnumDef> node;
};

struct Program {
  std::optional<std::string> package_name;
  std::optional<std::string> module_name;
  std::vector<std::string> imports;
  std::vector<Item> items;
};

inline std::string qualified_identity(const std::optional<frontend::QualifiedName>& name,
                                      std::string_view fallback = {}) {
  if (name.has_value() && !name->empty()) return name->display_name();
  return std::string(fallback);
}

inline std::string qualified_identity(const frontend::QualifiedName& name,
                                      std::string_view fallback = {}) {
  if (!name.empty()) return name.display_name();
  return std::string(fallback);
}

inline std::string function_identity(const Function& fn) {
  return qualified_identity(fn.qualified_name, fn.name);
}

inline std::string call_target_identity(const Expr::Call& call) {
  return qualified_identity(call.resolved_callee, call.callee);
}

inline std::string varref_target_identity(const Expr::VarRef& ref) {
  return qualified_identity(ref.top_level_symbol, ref.name);
}

inline std::string type_identity(const std::optional<frontend::QualifiedName>& name,
                                 std::string_view fallback = {}) {
  return qualified_identity(name, fallback);
}

inline std::string type_identity(const frontend::Ty& ty) {
  return qualified_identity(ty.qualified_name, ty.name);
}

inline std::string defined_type_identity(const StructDef& def) {
  return qualified_identity(def.qualified_name, def.name);
}

inline std::string defined_type_identity(const EnumDef& def) {
  return qualified_identity(def.qualified_name, def.name);
}

inline std::string construct_type_identity(const Expr::Construct& construct) {
  return qualified_identity(construct.resolved_type, construct.type_name);
}

} // namespace nebula::nir
