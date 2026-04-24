#pragma once

#include "frontend/ast.hpp"
#include "frontend/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::frontend {

using LocalBindingId = std::uint32_t;
inline constexpr LocalBindingId kInvalidLocalBindingId = 0;

struct TExpr;
struct TStmt;
struct TMatchExprArm;

using TExprPtr = std::unique_ptr<TExpr>;
using TMatchExprArmPtr = std::unique_ptr<TMatchExprArm>;

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
    LocalBindingId binding_id = kInvalidLocalBindingId;
  };
  struct Call {
    std::string callee;
    std::optional<QualifiedName> resolved_callee;
    std::vector<TExprPtr> args;
    std::vector<bool> args_ref;
    CallKind kind = CallKind::Direct;
    LocalBindingId callee_binding_id = kInvalidLocalBindingId;
  };
  struct FieldRef {
    std::string base;
    std::string field;
    LocalBindingId base_binding_id = kInvalidLocalBindingId;
  };
  struct TempFieldRef {
    TExprPtr base;
    std::string field;
  };
  struct EnumIsVariant {
    TExprPtr subject;
    std::optional<QualifiedName> enum_name;
    std::string variant_name;
    std::uint32_t variant_index = 0;
  };
  struct EnumPayload {
    TExprPtr subject;
    std::optional<QualifiedName> enum_name;
    std::string variant_name;
    std::uint32_t variant_index = 0;
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
  struct Try {
    TExprPtr inner;
  };
  struct Await {
    TExprPtr inner;
  };
  struct Match {
    struct Binding {
      std::string name;
      Ty ty = Ty::Unknown();
      Span span{};
      LocalBindingId binding_id = kInvalidLocalBindingId;
    };
    struct StructBinding {
      std::string field_name;
      Binding binding;
      Span field_span{};
    };
    struct Arm {
      enum class Kind : std::uint8_t { Wildcard, Bool, EnumVariant };

      Kind kind = Kind::Wildcard;
      Span span{};
      bool bool_value = false;
      std::string variant_name;
      std::uint32_t variant_index = 0;
      Ty payload_ty = Ty::Unknown();
      std::optional<Binding> payload_binding;
      std::vector<StructBinding> payload_struct_bindings;
      TExprPtr value;
    };

    TExprPtr subject;
    std::vector<TMatchExprArmPtr> arms;
    bool exhaustive = false;
  };

  Span span{};
  Ty ty = Ty::Unknown();
  std::variant<IntLit, FloatLit, BoolLit, StringLit, VarRef, Call, FieldRef, TempFieldRef,
               EnumIsVariant, EnumPayload, Construct, Binary, Unary, Prefix, Try, Await, Match>
      node;

  TExpr() : node(IntLit{}) {}
};

struct TMatchExprArm : TExpr::Match::Arm {};

struct TBlock {
  Span span{};
  std::vector<TStmt> stmts;
};

struct TStmt {
  struct Declare {
    std::string name;
    Ty ty = Ty::Unknown();
    LocalBindingId binding_id = kInvalidLocalBindingId;
  };
  struct Let {
    std::string name;
    Ty ty = Ty::Unknown();
    TExprPtr value;
    LocalBindingId binding_id = kInvalidLocalBindingId;
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
    LocalBindingId binding_id = kInvalidLocalBindingId;
  };
  struct AssignField {
    std::string base;
    std::string field;
    Ty ty = Ty::Unknown();
    TExprPtr value;
    LocalBindingId base_binding_id = kInvalidLocalBindingId;
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
    LocalBindingId binding_id = kInvalidLocalBindingId;
  };
  struct While {
    TExprPtr cond;
    TBlock body;
  };
  struct Break {};
  struct Continue {};

  Span span{};
  std::vector<std::string> annotations;
  std::variant<Declare, Let, Return, ExprStmt, AssignVar, AssignField, Region, Unsafe, If, For,
               While, Break, Continue>
      node;

  TStmt() : node(Declare{}) {}
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
  std::vector<std::string> type_params;
  std::vector<std::string> param_names;
  std::vector<Ty> params;
  std::vector<bool> params_ref;
  Ty ret = Ty::Void();
  Ty body_ret = Ty::Void();
  bool is_async = false;
  bool is_unsafe_callable = false;
};

struct TParam {
  Span span{};
  bool is_ref = false;
  std::string name;
  Ty ty = Ty::Unknown();
  LocalBindingId binding_id = kInvalidLocalBindingId;
};

struct TFunction {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<std::string> type_params;
  std::vector<TParam> params;
  Ty ret = Ty::Void();
  bool is_async = false;
  bool is_extern = false;
  std::optional<TBlock> body;
};

struct TStruct {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<std::string> type_params;
  std::vector<TField> fields;
};

struct TEnum {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<std::string> type_params;
  std::vector<TVariant> variants;
};

struct TUiProp {
  Span span{};
  std::string name;
  TExprPtr value;
};

struct TUiNode {
  struct View {
    std::string component;
    Span component_span{};
    std::vector<TUiProp> props;
    std::vector<TUiNode> children;
  };
  struct If {
    TExprPtr cond;
    std::vector<TUiNode> then_children;
  };
  struct For {
    std::string var;
    Ty var_ty = Ty::Unknown();
    LocalBindingId binding_id = kInvalidLocalBindingId;
    TExprPtr iterable;
    std::vector<TUiNode> body;
  };

  Span span{};
  std::variant<View, If, For> node;
};

struct TUi {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  QualifiedName qualified_name;
  std::vector<TParam> params;
  std::vector<TUiNode> body;
};

struct TItem {
  Span span{};
  std::variant<TFunction, TStruct, TEnum, TUi> node;
};

struct TProgram {
  std::optional<std::string> package_name;
  std::optional<std::string> module_name;
  std::vector<std::string> imports;
  std::vector<TItem> items;
};

} // namespace nebula::frontend
