#pragma once

#include "frontend/ast.hpp"
#include "frontend/types.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::frontend {

struct TExpr;
struct TStmt;

using TExprPtr = std::unique_ptr<TExpr>;

struct TExpr {
  enum class CallKind : std::uint8_t { Direct, Indirect };
  enum class UnaryOp : std::uint8_t { Not };

  struct IntLit {
    std::int64_t value = 0;
  };
  struct FloatLit {
    double value = 0.0;
  };
  struct BoolLit {
    bool value = false;
  };
  struct StringLit {
    std::string value;
  };
  struct VarRef {
    std::string name;
    std::optional<QualifiedName> top_level_symbol;
  };
  struct Call {
    std::string callee;
    std::optional<QualifiedName> resolved_callee;
    std::vector<TExprPtr> args;
    std::vector<bool> args_ref;
    CallKind kind = CallKind::Direct;
  };
  struct FieldRef {
    std::string base;
    std::string field;
  };
  struct Construct {
    std::string type_name;
    std::optional<QualifiedName> resolved_type;
    std::optional<std::string> variant_name;
    std::optional<std::uint32_t> variant_index;
    std::vector<TExprPtr> args;
  };
  struct Binary {
    Expr::BinOp op{};
    TExprPtr lhs;
    TExprPtr rhs;
  };
  struct Unary {
    Expr::UnaryOp op{};
    TExprPtr inner;
  };
  struct Prefix {
    Expr::PrefixKind kind{};
    TExprPtr inner;
  };

  Span span{};
  Ty ty = Ty::Unknown();
  std::variant<IntLit, FloatLit, BoolLit, StringLit, VarRef, Call, FieldRef, Construct, Binary,
               Unary, Prefix>
      node;
};

struct TBlock {
  Span span{};
  std::vector<TStmt> stmts;
};

struct TStmt {
  struct Let {
    std::string name;
    Ty ty = Ty::Unknown();
    TExprPtr value;
  };
  struct Return {
    TExprPtr value;
  };
  struct ExprStmt {
    TExprPtr expr;
  };
  struct AssignVar {
    std::string name;
    Ty ty = Ty::Unknown();
    TExprPtr value;
  };
  struct AssignField {
    std::string base;
    std::string field;
    Ty ty = Ty::Unknown();
    TExprPtr value;
  };
  struct Region {
    std::string name;
    TBlock body;
  };
  struct Unsafe {
    TBlock body;
  };
  struct If {
    TExprPtr cond;
    TBlock then_body;
    std::optional<TBlock> else_body;
  };
  struct For {
    std::string var;
    Ty var_ty = Ty::Int();
    TExprPtr start;
    TExprPtr end;
    TBlock body;
  };

  Span span{};
  std::vector<std::string> annotations;
  std::variant<Let, Return, ExprStmt, AssignVar, AssignField, Region, Unsafe, If, For> node;
};

struct TField {
  Span span{};
  std::string name;
  Ty ty = Ty::Unknown();
};

struct TVariant {
  Span span{};
  std::string name;
  Ty payload = Ty::Unknown();
};

struct TFunctionSig {
  std::string name;
  QualifiedName qualified_name;
  std::vector<std::string> param_names;
  std::vector<Ty> params;
  std::vector<bool> params_ref;
  Ty ret = Ty::Void();
  bool is_unsafe_callable = false;
};

struct TParam {
  Span span{};
  bool is_ref = false;
  std::string name;
  Ty ty = Ty::Unknown();
};

struct TFunction {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<TParam> params;
  Ty ret = Ty::Void();
  bool is_extern = false;
  std::optional<TBlock> body;
};

struct TStruct {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<TField> fields;
};

struct TEnum {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::string type_param;
  std::vector<TVariant> variants;
};

struct TItem {
  Span span{};
  std::variant<TFunction, TStruct, TEnum> node;
};

struct TProgram {
  std::optional<std::string> package_name;
  std::optional<std::string> module_name;
  std::vector<std::string> imports;
  std::vector<TItem> items;
};

} // namespace nebula::frontend
