#pragma once

#include "frontend/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::frontend {

struct Type;
struct Expr;
struct Stmt;
struct MatchExprArm;

using TypePtr = std::unique_ptr<Type>;
using ExprPtr = std::unique_ptr<Expr>;
using MatchExprArmPtr = std::unique_ptr<MatchExprArm>;

struct Type {
  Span span{};
  std::string name;
  std::vector<Type> args;
  std::vector<Type> callable_params;
  TypePtr callable_ret;
  bool is_unsafe_callable = false;
};

struct Param {
  Span span{};
  bool is_ref = false;
  std::string name;
  Type type;
};

struct Expr {
  enum class PrefixKind : std::uint8_t { Shared, Unique, Heap, Promote };
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
  struct Name {
    std::string ident;
  };
  struct Call {
    std::string callee;
    Span callee_span{};
    std::vector<ExprPtr> args;
  };
  struct Field {
    ExprPtr base;
    std::string field;
    Span field_span{};
  };
  struct MethodCall {
    ExprPtr base;
    std::string method;
    Span method_span{};
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
  struct Try {
    ExprPtr inner;
  };
  struct Await {
    ExprPtr inner;
  };
  struct Match {
    ExprPtr subject;
    std::vector<MatchExprArmPtr> arms;
  };

  Span span{};
  std::variant<IntLit, FloatLit, BoolLit, StringLit, Name, Call, Field, MethodCall, Binary, Unary,
               Prefix, Try, Await, Match>
      node;

  Expr() : node(IntLit{}) {}
};

struct Block {
  Span span{};
  std::vector<Stmt> stmts;
};

struct StructBindingField {
  Span span{};
  Span binding_span{};
  std::string field_name;
  std::optional<std::string> binding_name;
  bool skip = false;
};

struct Pattern {
  struct Wildcard {};
  struct BoolLit {
    bool value = false;
  };
  struct Variant {
    struct EmptyPayload {};
    struct BindingPayload {
      std::string name;
      Span binding_span{};
    };
    struct WildcardPayload {};
    struct StructPayload {
      std::vector<StructBindingField> fields;
    };

    std::string name;
    Span name_span{};
    using Payload =
        std::variant<EmptyPayload, BindingPayload, WildcardPayload, StructPayload>;
    std::optional<Payload> payload;
  };

  Span span{};
  std::variant<Wildcard, BoolLit, Variant> node;
};

struct MatchExprArm {
  Span span{};
  Pattern pattern;
  ExprPtr value;
};

struct MatchArm {
  Span span{};
  Pattern pattern;
  Block body;
};

struct Stmt {
  struct Let {
    std::string name;
    ExprPtr value;
  };
  struct LetStruct {
    std::vector<StructBindingField> fields;
    ExprPtr value;
  };
  struct Return {
    ExprPtr value;
  };
  struct ExprStmt {
    ExprPtr expr;
  };
  struct AssignVar {
    std::string name;
    ExprPtr value;
  };
  struct AssignField {
    std::string base;
    std::string field;
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
    std::string var;
    ExprPtr start;
    ExprPtr end;
    Block body;
  };
  struct While {
    ExprPtr cond;
    Block body;
  };
  struct Break {};
  struct Continue {};
  struct Match {
    ExprPtr subject;
    std::vector<MatchArm> arms;
  };

  Span span{};
  std::vector<std::string> annotations;
  std::variant<Let, LetStruct, Return, ExprStmt, AssignVar, AssignField, Region, Unsafe, If, For,
               While, Break, Continue, Match>
      node;
};

struct Field {
  Span span{};
  std::string name;
  Type type;
};

struct Variant {
  Span span{};
  std::string name;
  Type payload;
};

struct Function {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::vector<std::string> type_params;
  std::vector<Param> params;
  std::optional<Type> return_type;
  bool is_async = false;
  bool is_extern = false;
  std::optional<Block> body;
};

struct Struct {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::vector<std::string> type_params;
  std::vector<Field> fields;
};

struct Enum {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::vector<std::string> type_params;
  std::vector<Variant> variants;
};

struct UiProp {
  Span span{};
  std::string name;
  ExprPtr value;
};

struct UiNode {
  struct View {
    std::string component;
    Span component_span{};
    std::vector<UiProp> props;
    std::vector<UiNode> children;
  };
  struct If {
    ExprPtr cond;
    std::vector<UiNode> then_children;
  };
  struct For {
    std::string var;
    Span var_span{};
    ExprPtr iterable;
    std::vector<UiNode> body;
  };

  Span span{};
  std::variant<View, If, For> node;
};

struct Ui {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::vector<Param> params;
  std::vector<UiNode> body;
};

struct Item {
  Span span{};
  std::variant<Function, Struct, Enum, Ui> node;
};

struct Program {
  std::optional<std::string> package_name;
  std::optional<std::string> module_name;
  std::vector<std::string> imports;
  std::vector<Item> items;
};

} // namespace nebula::frontend
