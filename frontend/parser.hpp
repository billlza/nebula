#pragma once

#include "frontend/ast.hpp"
#include "frontend/token.hpp"

#include <vector>

namespace nebula::frontend {

class Parser {
public:
  explicit Parser(std::vector<Token> tokens);

  Program parse_program();

private:
  std::vector<Token> toks_;
  std::size_t i_ = 0;

  const Token& peek(std::size_t lookahead = 0) const;
  bool eof() const;
  bool at(TokenKind k) const;
  const Token& bump();
  bool match(TokenKind k);
  const Token& expect(TokenKind k, const char* what);

  std::vector<std::string> parse_annotations();
  std::string parse_module_path(bool allow_package_qualifier = false);
  Item parse_item_with_annotations();
  Item parse_item(std::vector<std::string> annotations);
  Function parse_function(std::vector<std::string> annotations, bool is_extern = false);
  Struct parse_struct(std::vector<std::string> annotations);
  Enum parse_enum(std::vector<std::string> annotations);

  Param parse_param();
  Type parse_type();

  Block parse_block();
  Stmt parse_stmt();
  Stmt parse_stmt_with_annotations(std::vector<std::string> annotations);

  // Expressions
  ExprPtr parse_expr(int min_prec = 0);
  ExprPtr parse_prefix();
  ExprPtr parse_primary();

  static int precedence(TokenKind op);
  static Expr::BinOp binop(TokenKind op);
};

} // namespace nebula::frontend
