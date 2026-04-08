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

using TypePtr = std::unique_ptr<Type>;
using ExprPtr = std::unique_ptr<Expr>;

struct Type {
  Span span{};
  std::string name;
  TypePtr arg; // optional single type argument (enum generic)
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
    std::vector<ExprPtr> args;
  };
  struct Field {
    std::string base;
    std::string field;
  };
  struct MethodCall {
    std::string base;
    std::string method;
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
  std::variant<IntLit, FloatLit, BoolLit, StringLit, Name, Call, Field, MethodCall, Binary, Unary,
               Prefix>
      node;
};

struct Block {
  Span span{};
  std::vector<Stmt> stmts;
};

struct Stmt {
  struct Let {
    std::string name;
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

  Span span{};
  std::vector<std::string> annotations;
  std::variant<Let, Return, ExprStmt, AssignVar, AssignField, Region, Unsafe, If, For> node;
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
  std::vector<Param> params;
  std::optional<Type> return_type;
  bool is_extern = false;
  std::optional<Block> body;
};

struct Struct {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::vector<Field> fields;
};

struct Enum {
  Span span{};
  std::vector<std::string> annotations;
  std::string name;
  std::string type_param;
  std::vector<Variant> variants;
};

struct Item {
  Span span{};
  std::variant<Function, Struct, Enum> node;
};

struct Program {
  std::optional<std::string> package_name;
  std::optional<std::string> module_name;
  std::vector<std::string> imports;
  std::vector<Item> items;
};

} // namespace nebula::frontend
