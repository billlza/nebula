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
  Function parse_function(std::vector<std::string> annotations,
                          bool is_extern = false,
                          bool is_async = false);
  Struct parse_struct(std::vector<std::string> annotations);
  Enum parse_enum(std::vector<std::string> annotations);
  Ui parse_ui(std::vector<std::string> annotations);
  std::vector<UiNode> parse_ui_node_list();
  UiNode parse_ui_node();
  UiNode parse_ui_view_node();
  UiNode parse_ui_if_node();
  UiNode parse_ui_for_node();
  UiProp parse_ui_prop();

  Param parse_param();
  std::vector<std::string> parse_type_param_names();
  Type parse_type();
  Pattern parse_pattern();
  StructBindingField parse_struct_binding_field();
  std::vector<StructBindingField> parse_struct_binding_fields();

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
