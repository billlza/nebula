#include "frontend/parser.hpp"

#include "frontend/errors.hpp"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace nebula::frontend {

Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

namespace {
std::string join_dotted_path(const std::vector<std::string>& segments) {
  std::string out;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    if (i) out.push_back('.');
    out += segments[i];
  }
  return out;
}

bool is_compound_assign_op(TokenKind kind) {
  return kind == TokenKind::PlusEqual || kind == TokenKind::MinusEqual ||
         kind == TokenKind::StarEqual || kind == TokenKind::SlashEqual ||
         kind == TokenKind::PercentEqual;
}

Expr::BinOp compound_to_binop(TokenKind kind) {
  switch (kind) {
  case TokenKind::PlusEqual: return Expr::BinOp::Add;
  case TokenKind::MinusEqual: return Expr::BinOp::Sub;
  case TokenKind::StarEqual: return Expr::BinOp::Mul;
  case TokenKind::SlashEqual: return Expr::BinOp::Div;
  case TokenKind::PercentEqual: return Expr::BinOp::Mod;
  default: return Expr::BinOp::Add;
  }
}

ExprPtr make_var_name_expr(const Token& name) {
  auto lhs = std::make_unique<Expr>();
  Expr::Name lhs_name;
  lhs_name.ident = name.lexeme;
  lhs->span = name.span;
  lhs->node = std::move(lhs_name);
  return lhs;
}

Span make_span(const SourcePos& start, const SourcePos& end, std::string source_path) {
  Span span;
  span.start = start;
  span.end = end;
  span.source_path = std::move(source_path);
  return span;
}

std::vector<std::string> split_dotted_path(std::string_view path) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t dot = path.find('.', start);
    const std::size_t end = (dot == std::string_view::npos) ? path.size() : dot;
    if (end > start) out.push_back(std::string(path.substr(start, end - start)));
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return out;
}

ExprPtr make_field_expr(ExprPtr base, std::string field_name, SourcePos end) {
  auto lhs = std::make_unique<Expr>();
  Expr::Field lhs_field;
  lhs_field.base = std::move(base);
  lhs_field.field = std::move(field_name);
  lhs->span = make_span(lhs_field.base->span.start, end, lhs_field.base->span.source_path);
  lhs->node = std::move(lhs_field);
  return lhs;
}

ExprPtr make_field_chain_expr(const Token& base, std::string field_path, SourcePos end) {
  ExprPtr out = make_var_name_expr(base);
  for (auto& segment : split_dotted_path(field_path)) {
    out = make_field_expr(std::move(out), segment, end);
  }
  return out;
}

ExprPtr make_compound_rhs(TokenKind op, ExprPtr lhs, ExprPtr rhs) {
  auto out = std::make_unique<Expr>();
  Expr::Binary bin;
  bin.op = compound_to_binop(op);
  bin.lhs = std::move(lhs);
  bin.rhs = std::move(rhs);
  out->span = make_span(bin.lhs->span.start, bin.rhs->span.end, bin.lhs->span.source_path);
  out->node = std::move(bin);
  return out;
}

ExprPtr make_int_lit_expr(std::int64_t value, Span span) {
  auto out = std::make_unique<Expr>();
  Expr::IntLit lit;
  lit.value = value;
  out->span = span;
  out->node = lit;
  return out;
}

ExprPtr make_inclusive_range_end(ExprPtr end_expr) {
  auto out = std::make_unique<Expr>();
  Expr::Binary bin;
  bin.op = Expr::BinOp::Add;
  bin.lhs = std::move(end_expr);
  bin.rhs = make_int_lit_expr(1, bin.lhs->span);
  out->span = make_span(bin.lhs->span.start, bin.rhs->span.end, bin.lhs->span.source_path);
  out->node = std::move(bin);
  return out;
}

int digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::int64_t parse_int_literal(const Token& t) {
  std::string_view s = t.lexeme;
  int base = 10;
  std::size_t i = 0;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
    base = 2;
    i = 2;
  } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
    base = 8;
    i = 2;
  }
  if (i >= s.size()) {
    throw ParseError("expected digit in numeric literal", t.span);
  }
  std::int64_t value = 0;
  for (; i < s.size(); ++i) {
    const int d = digit_value(s[i]);
    if (d < 0 || d >= base) {
      throw ParseError("invalid digit in numeric literal", t.span);
    }
    if (value > (std::numeric_limits<std::int64_t>::max() - d) / base) {
      throw ParseError("integer literal out of range", t.span);
    }
    value = value * base + d;
  }
  return value;
}
} // namespace

const Token& Parser::peek(std::size_t lookahead) const {
  const std::size_t j = i_ + lookahead;
  if (j >= toks_.size()) return toks_.back();
  return toks_[j];
}

bool Parser::eof() const { return at(TokenKind::Eof); }
bool Parser::at(TokenKind k) const { return peek().kind == k; }

const Token& Parser::bump() {
  if (i_ < toks_.size()) return toks_[i_++];
  return toks_.back();
}

bool Parser::match(TokenKind k) {
  if (!at(k)) return false;
  bump();
  return true;
}

const Token& Parser::expect(TokenKind k, const char* what) {
  if (at(k)) return bump();
  const Token& t = peek();
  throw ParseError(std::string("expected ") + what + ", got " + token_kind_name(t.kind), t.span);
}

std::vector<std::string> Parser::parse_annotations() {
  std::vector<std::string> ann;
  while (match(TokenKind::At)) {
    const Token& t = peek();
    if (t.kind == TokenKind::Ident || t.kind == TokenKind::KwUnsafe) {
      ann.push_back(t.lexeme);
      (void)bump();
      continue;
    }
    throw ParseError("expected annotation name", t.span);
  }
  return ann;
}

std::string Parser::parse_module_path(bool allow_package_qualifier) {
  const Token& first = expect(TokenKind::Ident, "module path");
  std::string path = first.lexeme;
  if (allow_package_qualifier && match(TokenKind::ColonColon)) {
    const Token& next = expect(TokenKind::Ident, "dependency package module path");
    path += "::";
    path += next.lexeme;
  }
  while (match(TokenKind::Dot)) {
    const Token& next = expect(TokenKind::Ident, "module path segment");
    path += ".";
    path += next.lexeme;
  }
  return path;
}

Program Parser::parse_program() {
  Program p;
  if (match(TokenKind::KwModule)) {
    p.module_name = parse_module_path(false);
  }
  while (match(TokenKind::KwImport)) {
    p.imports.push_back(parse_module_path(true));
  }
  while (!eof()) {
    p.items.push_back(parse_item_with_annotations());
  }
  return p;
}

Item Parser::parse_item_with_annotations() {
  auto ann = parse_annotations();
  return parse_item(std::move(ann));
}

Item Parser::parse_item(std::vector<std::string> annotations) {
  const Token& t = peek();
  Item it;
  switch (t.kind) {
  case TokenKind::KwAsync:
    bump();
    it.node = parse_function(std::move(annotations), false, true);
    break;
  case TokenKind::KwFn: it.node = parse_function(std::move(annotations)); break;
  case TokenKind::KwExtern: it.node = parse_function(std::move(annotations), true); break;
  case TokenKind::KwStruct: it.node = parse_struct(std::move(annotations)); break;
  case TokenKind::KwEnum: it.node = parse_enum(std::move(annotations)); break;
  case TokenKind::Ident:
    if (t.lexeme == "ui") {
      it.node = parse_ui(std::move(annotations));
      break;
    }
    [[fallthrough]];
  default:
    throw ParseError("expected item (fn/extern fn/struct/enum/ui)", t.span);
  }

  // span: conservatively set to child span
  it.span = std::visit([](auto&& n) { return n.span; }, it.node);
  return it;
}

Function Parser::parse_function(std::vector<std::string> annotations, bool is_extern, bool is_async) {
  const Token& kw = is_extern ? expect(TokenKind::KwExtern, "`extern`") : expect(TokenKind::KwFn, "`fn`");
  if (is_extern) expect(TokenKind::KwFn, "`fn`");
  const Token& name = expect(TokenKind::Ident, "function name");
  const std::vector<std::string> type_params = parse_type_param_names();

  expect(TokenKind::LParen, "`(`");
  std::vector<Param> params;
  if (!at(TokenKind::RParen)) {
    params.push_back(parse_param());
    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RParen)) break;
      params.push_back(parse_param());
    }
  }
  const Token& rp = expect(TokenKind::RParen, "`)`");

  std::optional<Type> ret;
  if (match(TokenKind::Arrow)) {
    ret = parse_type();
  }

  Function fn;
  fn.annotations = std::move(annotations);
  fn.name = name.lexeme;
  fn.type_params = type_params;
  fn.params = std::move(params);
  fn.return_type = std::move(ret);
  fn.is_async = is_async;
  fn.is_extern = is_extern;
  if (is_extern) {
    fn.span = make_span(kw.span.start,
                        fn.return_type.has_value() ? fn.return_type->span.end : rp.span.end,
                        kw.span.source_path);
  } else {
    fn.body = parse_block();
    fn.span = make_span(kw.span.start, fn.body->span.end, kw.span.source_path);
  }
  return fn;
}

UiProp Parser::parse_ui_prop() {
  const Token& name = expect(TokenKind::Ident, "UI property name");
  expect(TokenKind::Equal, "`=`");
  auto value = parse_expr();
  UiProp prop;
  prop.name = name.lexeme;
  prop.span = make_span(name.span.start, value->span.end, name.span.source_path);
  prop.value = std::move(value);
  return prop;
}

std::vector<UiNode> Parser::parse_ui_node_list() {
  expect(TokenKind::LBrace, "`{`");
  std::vector<UiNode> nodes;
  while (!at(TokenKind::RBrace) && !eof()) {
    nodes.push_back(parse_ui_node());
  }
  expect(TokenKind::RBrace, "`}`");
  return nodes;
}

UiNode Parser::parse_ui_view_node() {
  const Token& start = expect(TokenKind::Ident, "`view`");
  if (start.lexeme != "view") throw ParseError("expected `view`", start.span);
  const Token& component = expect(TokenKind::Ident, "UI component name");
  expect(TokenKind::LParen, "`(`");
  std::vector<UiProp> props;
  if (!at(TokenKind::RParen)) {
    props.push_back(parse_ui_prop());
    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RParen)) break;
      props.push_back(parse_ui_prop());
    }
  }
  const Token& rp = expect(TokenKind::RParen, "`)`");
  std::vector<UiNode> children;
  SourcePos end = rp.span.end;
  if (at(TokenKind::LBrace)) {
    children = parse_ui_node_list();
    if (!children.empty()) {
      end = children.back().span.end;
    }
  }

  UiNode::View view;
  view.component = component.lexeme;
  view.component_span = component.span;
  view.props = std::move(props);
  view.children = std::move(children);

  UiNode node;
  node.span = make_span(start.span.start, end, start.span.source_path);
  node.node = std::move(view);
  return node;
}

UiNode Parser::parse_ui_if_node() {
  const Token& start = expect(TokenKind::KwIf, "`if`");
  auto cond = parse_expr();
  auto children = parse_ui_node_list();

  UiNode::If if_node;
  if_node.cond = std::move(cond);
  if_node.then_children = std::move(children);

  UiNode node;
  node.span = make_span(start.span.start,
                        if_node.then_children.empty() ? start.span.end : if_node.then_children.back().span.end,
                        start.span.source_path);
  node.node = std::move(if_node);
  return node;
}

UiNode Parser::parse_ui_for_node() {
  const Token& start = expect(TokenKind::KwFor, "`for`");
  const Token& var = expect(TokenKind::Ident, "UI loop variable");
  expect(TokenKind::KwIn, "`in`");
  auto iterable = parse_expr();
  auto children = parse_ui_node_list();

  UiNode::For for_node;
  for_node.var = var.lexeme;
  for_node.var_span = var.span;
  for_node.iterable = std::move(iterable);
  for_node.body = std::move(children);

  UiNode node;
  node.span = make_span(start.span.start,
                        for_node.body.empty() ? start.span.end : for_node.body.back().span.end,
                        start.span.source_path);
  node.node = std::move(for_node);
  return node;
}

UiNode Parser::parse_ui_node() {
  if (at(TokenKind::Ident) && peek().lexeme == "view") return parse_ui_view_node();
  if (at(TokenKind::KwIf)) return parse_ui_if_node();
  if (at(TokenKind::KwFor)) return parse_ui_for_node();
  throw ParseError("expected UI node (view/if/for)", peek().span);
}

Ui Parser::parse_ui(std::vector<std::string> annotations) {
  const Token& kw = expect(TokenKind::Ident, "`ui`");
  if (kw.lexeme != "ui") throw ParseError("expected `ui`", kw.span);
  const Token& name = expect(TokenKind::Ident, "UI name");
  expect(TokenKind::LParen, "`(`");
  std::vector<Param> params;
  if (!at(TokenKind::RParen)) {
    params.push_back(parse_param());
    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RParen)) break;
      params.push_back(parse_param());
    }
  }
  expect(TokenKind::RParen, "`)`");
  auto body = parse_ui_node_list();

  Ui ui;
  ui.annotations = std::move(annotations);
  ui.name = name.lexeme;
  ui.params = std::move(params);
  ui.body = std::move(body);
  ui.span = make_span(kw.span.start, ui.body.empty() ? name.span.end : ui.body.back().span.end, kw.span.source_path);
  return ui;
}

std::vector<std::string> Parser::parse_type_param_names() {
  std::vector<std::string> out;
  if (!match(TokenKind::Less)) return out;
  const Token& first = expect(TokenKind::Ident, "type parameter");
  out.push_back(first.lexeme);
  while (match(TokenKind::Comma)) {
    const Token& next = expect(TokenKind::Ident, "type parameter");
    out.push_back(next.lexeme);
  }
  expect(TokenKind::Greater, "`>`");
  return out;
}

Param Parser::parse_param() {
  const Token& start = peek();
  bool is_ref = match(TokenKind::KwRef);
  const Token& name = expect(TokenKind::Ident, "param name");
  expect(TokenKind::Colon, "`:`");
  Type ty = parse_type();
  Param p;
  p.is_ref = is_ref;
  p.name = name.lexeme;
  p.type = std::move(ty);
  p.span = make_span(start.span.start, p.type.span.end, start.span.source_path);
  return p;
}

Type Parser::parse_type() {
  const Token& name = expect(TokenKind::Ident, "type name");
  Type t;
  t.name = name.lexeme;
  t.span.start = name.span.start;

  if ((t.name == "Fn" || t.name == "UnsafeFn") && at(TokenKind::LParen)) {
    t.is_unsafe_callable = (t.name == "UnsafeFn");
    expect(TokenKind::LParen, "`(`");
    if (!at(TokenKind::RParen)) {
      t.callable_params.push_back(parse_type());
      while (match(TokenKind::Comma)) {
        if (at(TokenKind::RParen)) break;
        t.callable_params.push_back(parse_type());
      }
    }
    expect(TokenKind::RParen, "`)`");
    expect(TokenKind::Arrow, "`->`");
    t.callable_ret = std::make_unique<Type>(parse_type());
    t.span.end = t.callable_ret->span.end;
    return t;
  }

  if (match(TokenKind::Less)) {
    t.args.push_back(parse_type());
    while (match(TokenKind::Comma)) {
      t.args.push_back(parse_type());
    }
    const Token& gt = expect(TokenKind::Greater, "`>`");
    t.span.end = gt.span.end;
  } else {
    t.span.end = name.span.end;
  }
  return t;
}

Pattern Parser::parse_pattern() {
  const Token& start = peek();
  Pattern pattern;

  if (start.kind == TokenKind::Ident && start.lexeme == "_") {
    (void)bump();
    pattern.span = start.span;
    pattern.node = Pattern::Wildcard{};
    return pattern;
  }

  if (match(TokenKind::KwTrue) || match(TokenKind::KwFalse)) {
    Pattern::BoolLit lit;
    lit.value = (start.kind == TokenKind::KwTrue);
    pattern.span = start.span;
    pattern.node = lit;
    return pattern;
  }

  const Token& name = expect(TokenKind::Ident, "match pattern");
  Pattern::Variant variant;
  variant.name = name.lexeme;
  variant.name_span = name.span;
  SourcePos end = name.span.end;

  if (match(TokenKind::LParen)) {
    if (match(TokenKind::LBrace)) {
      Pattern::Variant::StructPayload payload;
      payload.fields = parse_struct_binding_fields();
      const Token& rb = expect(TokenKind::RBrace, "`}`");
      variant.payload = std::move(payload);
      end = rb.span.end;
    } else if (!at(TokenKind::RParen)) {
      const Token& payload = expect(TokenKind::Ident, "pattern binding");
      if (payload.lexeme == "_") {
        variant.payload = Pattern::Variant::WildcardPayload{};
      } else {
        variant.payload = Pattern::Variant::BindingPayload{payload.lexeme, payload.span};
      }
      end = payload.span.end;
    } else {
      variant.payload = Pattern::Variant::EmptyPayload{};
    }
    const Token& rp = expect(TokenKind::RParen, "`)`");
    end = rp.span.end;
  }

  pattern.span = make_span(name.span.start, end, name.span.source_path);
  pattern.node = std::move(variant);
  return pattern;
}

StructBindingField Parser::parse_struct_binding_field() {
  const Token& field = expect(TokenKind::Ident, "field name");
  StructBindingField out;
  out.field_name = field.lexeme;
  out.binding_name = field.lexeme;
  out.span = field.span;
  out.binding_span = field.span;
  if (match(TokenKind::Colon)) {
    const Token& binding = expect(TokenKind::Ident, "binding name");
    out.span = make_span(field.span.start, binding.span.end, field.span.source_path);
    out.binding_span = binding.span;
    if (binding.lexeme == "_") {
      out.skip = true;
      out.binding_name.reset();
    } else {
      out.binding_name = binding.lexeme;
    }
  }
  return out;
}

std::vector<StructBindingField> Parser::parse_struct_binding_fields() {
  std::vector<StructBindingField> fields;
  if (!at(TokenKind::RBrace)) {
    fields.push_back(parse_struct_binding_field());
    while (match(TokenKind::Comma)) {
      if (at(TokenKind::RBrace)) break;
      fields.push_back(parse_struct_binding_field());
    }
  }
  if (fields.empty()) {
    throw ParseError("struct destructuring requires at least one field", peek().span);
  }
  return fields;
}

Struct Parser::parse_struct(std::vector<std::string> annotations) {
  const Token& kw = expect(TokenKind::KwStruct, "`struct`");
  const Token& name = expect(TokenKind::Ident, "struct name");
  const std::vector<std::string> type_params = parse_type_param_names();
  expect(TokenKind::LBrace, "`{`");

  std::vector<Field> fields;
  while (!at(TokenKind::RBrace)) {
    const Token& fstart = expect(TokenKind::Ident, "field name");
    expect(TokenKind::Colon, "`:`");
    Type ty = parse_type();
    Field f;
    f.name = fstart.lexeme;
    f.type = std::move(ty);
    f.span = make_span(fstart.span.start, f.type.span.end, fstart.span.source_path);
    fields.push_back(std::move(f));
    (void)match(TokenKind::Comma); // permissive optional comma
  }

  const Token& rb = expect(TokenKind::RBrace, "`}`");

  Struct st;
  st.annotations = std::move(annotations);
  st.name = name.lexeme;
  st.type_params = type_params;
  st.fields = std::move(fields);
  st.span = make_span(kw.span.start, rb.span.end, kw.span.source_path);
  return st;
}

Enum Parser::parse_enum(std::vector<std::string> annotations) {
  const Token& kw = expect(TokenKind::KwEnum, "`enum`");
  const Token& name = expect(TokenKind::Ident, "enum name");
  const std::vector<std::string> type_params = parse_type_param_names();
  expect(TokenKind::LBrace, "`{`");

  std::vector<Variant> vars;
  while (!at(TokenKind::RBrace)) {
    const Token& vname = expect(TokenKind::Ident, "variant name");
    expect(TokenKind::LParen, "`(`");
    Type payload = parse_type();
    expect(TokenKind::RParen, "`)`");
    Variant v;
    v.name = vname.lexeme;
    v.payload = std::move(payload);
    v.span = make_span(vname.span.start, v.payload.span.end, vname.span.source_path);
    vars.push_back(std::move(v));
    (void)match(TokenKind::Comma); // permissive optional comma
  }
  const Token& rb = expect(TokenKind::RBrace, "`}`");

  Enum en;
  en.annotations = std::move(annotations);
  en.name = name.lexeme;
  en.type_params = type_params;
  en.variants = std::move(vars);
  en.span = make_span(kw.span.start, rb.span.end, kw.span.source_path);
  return en;
}

Block Parser::parse_block() {
  const Token& lb = expect(TokenKind::LBrace, "`{`");
  Block b;
  b.span.start = lb.span.start;
  while (!at(TokenKind::RBrace)) {
    auto ann = parse_annotations();
    b.stmts.push_back(parse_stmt_with_annotations(std::move(ann)));
  }
  const Token& rb = expect(TokenKind::RBrace, "`}`");
  b.span.end = rb.span.end;
  return b;
}

Stmt Parser::parse_stmt() {
  auto ann = parse_annotations();
  return parse_stmt_with_annotations(std::move(ann));
}

Stmt Parser::parse_stmt_with_annotations(std::vector<std::string> annotations) {
  const Token& start = peek();

  Stmt s;
  s.annotations = std::move(annotations);

  if (match(TokenKind::KwLet)) {
    if (match(TokenKind::LBrace)) {
      std::vector<StructBindingField> fields = parse_struct_binding_fields();
      expect(TokenKind::RBrace, "`}`");
      expect(TokenKind::Equal, "`=`");
      ExprPtr value = parse_expr();
      Stmt::LetStruct n;
      n.fields = std::move(fields);
      n.value = std::move(value);
      s.node = std::move(n);
      s.span = make_span(start.span.start, std::get<Stmt::LetStruct>(s.node).value->span.end,
                         start.span.source_path);
      return s;
    }

    const Token& name = expect(TokenKind::Ident, "binding name");
    expect(TokenKind::Equal, "`=`");
    ExprPtr value = parse_expr();
    Stmt::Let n;
    n.name = name.lexeme;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::Let>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwReturn)) {
    ExprPtr value = parse_expr();
    Stmt::Return n;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::Return>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwRegion)) {
    const Token& name = expect(TokenKind::Ident, "region name");
    Block body = parse_block();
    Stmt::Region n;
    n.name = name.lexeme;
    n.body = std::move(body);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::Region>(s.node).body.span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwUnsafe)) {
    Block body = parse_block();
    Stmt::Unsafe n;
    n.body = std::move(body);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::Unsafe>(s.node).body.span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwIf)) {
    ExprPtr cond = parse_expr();
    Block then_body = parse_block();
    std::optional<Block> else_body;
    if (match(TokenKind::KwElse)) {
      if (at(TokenKind::KwIf)) {
        Block nested;
        nested.span.start = peek().span.start;
        nested.stmts.push_back(parse_stmt());
        nested.span.end = nested.stmts.back().span.end;
        nested.span.source_path = nested.stmts.back().span.source_path;
        else_body = std::move(nested);
      } else {
        else_body = parse_block();
      }
    }
    Stmt::If n;
    n.cond = std::move(cond);
    n.then_body = std::move(then_body);
    n.else_body = std::move(else_body);
    const auto& if_stmt = n;
    const Span end_span = if_stmt.else_body.has_value() ? if_stmt.else_body->span : if_stmt.then_body.span;
    s.node = std::move(n);
    s.span = make_span(start.span.start, end_span.end, start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwMatch)) {
    ExprPtr subject = parse_expr();
    const Token& lb = expect(TokenKind::LBrace, "`{`");
    std::vector<MatchArm> arms;
    while (!at(TokenKind::RBrace)) {
      MatchArm arm;
      arm.pattern = parse_pattern();
      expect(TokenKind::FatArrow, "`=>`");
      arm.body = parse_block();
      arm.span = make_span(arm.pattern.span.start, arm.body.span.end, arm.pattern.span.source_path);
      arms.push_back(std::move(arm));
      (void)match(TokenKind::Comma);
    }
    const Token& rb = expect(TokenKind::RBrace, "`}`");
    if (arms.empty()) {
      throw ParseError("match requires at least one arm", lb.span);
    }

    Stmt::Match n;
    n.subject = std::move(subject);
    n.arms = std::move(arms);
    s.node = std::move(n);
    s.span = make_span(start.span.start, rb.span.end, start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwFor)) {
    const Token& var = expect(TokenKind::Ident, "loop variable");
    expect(TokenKind::KwIn, "`in`");
    ExprPtr a = parse_expr();
    const bool inclusive_range = match(TokenKind::DotDotDot);
    if (!inclusive_range && !match(TokenKind::DotDot) && !match(TokenKind::DotDotLess)) {
      const Token& t = peek();
      throw ParseError("expected range operator (`..`, `..<`, or `...`)", t.span);
    }
    ExprPtr b = parse_expr();
    Block body = parse_block();
    Stmt::For n;
    n.var = var.lexeme;
    n.start = std::move(a);
    n.end = inclusive_range ? make_inclusive_range_end(std::move(b)) : std::move(b);
    n.body = std::move(body);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::For>(s.node).body.span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwWhile)) {
    ExprPtr cond = parse_expr();
    Block body = parse_block();
    Stmt::While n;
    n.cond = std::move(cond);
    n.body = std::move(body);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::While>(s.node).body.span.end,
                       start.span.source_path);
    return s;
  }

  if (match(TokenKind::KwBreak)) {
    s.node = Stmt::Break{};
    s.span = start.span;
    return s;
  }

  if (match(TokenKind::KwContinue)) {
    s.node = Stmt::Continue{};
    s.span = start.span;
    return s;
  }

  // Rooted assignment statement forms:
  //   Ident ('=' | op=) Expr
  //   Ident ('.' Ident)+ ('=' | op=) Expr
  if (at(TokenKind::Ident)) {
    std::size_t lookahead = 1;
    std::vector<std::string> path_segments;
    while (peek(lookahead).kind == TokenKind::Dot && peek(lookahead + 1).kind == TokenKind::Ident) {
      path_segments.push_back(peek(lookahead + 1).lexeme);
      lookahead += 2;
    }

    const TokenKind tail = peek(lookahead).kind;
    if (tail == TokenKind::Equal || is_compound_assign_op(tail)) {
      const Token& base = bump();
      SourcePos path_end = base.span.end;
      for (std::size_t i = 0; i < path_segments.size(); ++i) {
        expect(TokenKind::Dot, "`.`");
        const Token& segment = expect(TokenKind::Ident, "field name");
        path_end = segment.span.end;
      }

      if (path_segments.empty()) {
        if (tail == TokenKind::Equal) {
          expect(TokenKind::Equal, "`=`");
          ExprPtr value = parse_expr();
          Stmt::AssignVar n;
          n.name = base.lexeme;
          n.value = std::move(value);
          s.node = std::move(n);
          s.span = make_span(start.span.start, std::get<Stmt::AssignVar>(s.node).value->span.end,
                             start.span.source_path);
          return s;
        }

        const TokenKind op = bump().kind;
        ExprPtr value = make_compound_rhs(op, make_var_name_expr(base), parse_expr());
        Stmt::AssignVar n;
        n.name = base.lexeme;
        n.value = std::move(value);
        s.node = std::move(n);
        s.span = make_span(start.span.start, std::get<Stmt::AssignVar>(s.node).value->span.end,
                           start.span.source_path);
        return s;
      }

      const std::string field_path = join_dotted_path(path_segments);
      if (tail == TokenKind::Equal) {
        expect(TokenKind::Equal, "`=`");
        ExprPtr value = parse_expr();
        Stmt::AssignField n;
        n.base = base.lexeme;
        n.field = field_path;
        n.value = std::move(value);
        s.node = std::move(n);
        s.span = make_span(start.span.start, std::get<Stmt::AssignField>(s.node).value->span.end,
                           start.span.source_path);
        return s;
      }

      const TokenKind op = bump().kind;
      ExprPtr value = make_compound_rhs(op, make_field_chain_expr(base, field_path, path_end), parse_expr());
      Stmt::AssignField n;
      n.base = base.lexeme;
      n.field = field_path;
      n.value = std::move(value);
      s.node = std::move(n);
      s.span = make_span(start.span.start, std::get<Stmt::AssignField>(s.node).value->span.end,
                         start.span.source_path);
      return s;
    }
  }

  // Expr statement
  ExprPtr e = parse_expr();
  if (at(TokenKind::Equal) || is_compound_assign_op(peek().kind)) {
    throw ParseError(
        "assignment target must be a rooted name or rooted field chain; temporary-base assignment is not supported",
        peek().span);
  }
  Stmt::ExprStmt n;
  n.expr = std::move(e);
  s.node = std::move(n);
  s.span = make_span(start.span.start, std::get<Stmt::ExprStmt>(s.node).expr->span.end,
                     start.span.source_path);
  return s;
}

int Parser::precedence(TokenKind op) {
  switch (op) {
  case TokenKind::PipePipe: return 3;
  case TokenKind::AmpAmp: return 4;
  case TokenKind::EqualEqual:
  case TokenKind::BangEqual: return 5;
  case TokenKind::Less:
  case TokenKind::Greater:
  case TokenKind::LessEqual:
  case TokenKind::GreaterEqual: return 6;
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent: return 20;
  case TokenKind::Plus:
  case TokenKind::Minus: return 10;
  default: return -1;
  }
}

Expr::BinOp Parser::binop(TokenKind op) {
  switch (op) {
  case TokenKind::Plus: return Expr::BinOp::Add;
  case TokenKind::Minus: return Expr::BinOp::Sub;
  case TokenKind::Star: return Expr::BinOp::Mul;
  case TokenKind::Slash: return Expr::BinOp::Div;
  case TokenKind::Percent: return Expr::BinOp::Mod;
  case TokenKind::EqualEqual: return Expr::BinOp::Eq;
  case TokenKind::BangEqual: return Expr::BinOp::Ne;
  case TokenKind::Less: return Expr::BinOp::Lt;
  case TokenKind::LessEqual: return Expr::BinOp::Lte;
  case TokenKind::Greater: return Expr::BinOp::Gt;
  case TokenKind::GreaterEqual: return Expr::BinOp::Gte;
  case TokenKind::AmpAmp: return Expr::BinOp::And;
  case TokenKind::PipePipe: return Expr::BinOp::Or;
  default: return Expr::BinOp::Add;
  }
}

ExprPtr Parser::parse_expr(int min_prec) {
  ExprPtr lhs = parse_prefix();

  while (true) {
    const Token& op = peek();
    const int prec = precedence(op.kind);
    if (prec < min_prec) break;

    bump();
    ExprPtr rhs = parse_expr(prec + 1); // left associative

    auto e = std::make_unique<Expr>();
    Expr::Binary b;
    b.op = binop(op.kind);
    b.lhs = std::move(lhs);
    b.rhs = std::move(rhs);
    e->span = make_span(b.lhs->span.start, b.rhs->span.end, b.lhs->span.source_path);
    e->node = std::move(b);
    lhs = std::move(e);
  }

  return lhs;
}

ExprPtr Parser::parse_prefix() {
  const Token& t = peek();

  auto mk_prefix = [&](Expr::PrefixKind k) -> ExprPtr {
    const Token& kw = bump();
    ExprPtr inner = parse_prefix();
    auto e = std::make_unique<Expr>();
    Expr::Prefix p;
    p.kind = k;
    p.inner = std::move(inner);
    e->span = make_span(kw.span.start, p.inner->span.end, kw.span.source_path);
    e->node = std::move(p);
    return e;
  };

  if (match(TokenKind::Plus)) {
    return parse_prefix();
  }
  if (match(TokenKind::Minus)) {
    const Token minus = t;
    ExprPtr rhs = parse_prefix();
    auto out = std::make_unique<Expr>();
    Expr::Binary bin;
    bin.op = Expr::BinOp::Sub;
    bin.lhs = make_int_lit_expr(0, minus.span);
    bin.rhs = std::move(rhs);
    out->span = make_span(minus.span.start, bin.rhs->span.end, minus.span.source_path);
    out->node = std::move(bin);
    return out;
  }
  if (match(TokenKind::Bang)) {
    const Token bang = t;
    ExprPtr rhs = parse_prefix();
    auto out = std::make_unique<Expr>();
    Expr::Unary unary;
    unary.op = Expr::UnaryOp::Not;
    unary.inner = std::move(rhs);
    out->span = make_span(bang.span.start, unary.inner->span.end, bang.span.source_path);
    out->node = std::move(unary);
    return out;
  }

  switch (t.kind) {
  case TokenKind::KwAwait: {
    const Token& kw = bump();
    ExprPtr inner = parse_prefix();
    auto e = std::make_unique<Expr>();
    Expr::Await a;
    a.inner = std::move(inner);
    e->span = make_span(kw.span.start, a.inner->span.end, kw.span.source_path);
    e->node = std::move(a);
    return e;
  }
  case TokenKind::KwShared: return mk_prefix(Expr::PrefixKind::Shared);
  case TokenKind::KwUnique: return mk_prefix(Expr::PrefixKind::Unique);
  case TokenKind::KwHeap: return mk_prefix(Expr::PrefixKind::Heap);
  case TokenKind::KwPromote: return mk_prefix(Expr::PrefixKind::Promote);
  default: return parse_primary();
  }
}

ExprPtr Parser::parse_primary() {
  const Token& t = peek();
  auto parse_call_args = [&]() {
    std::vector<ExprPtr> args;
    if (!at(TokenKind::RParen)) {
      args.push_back(parse_expr());
      while (match(TokenKind::Comma)) {
        if (at(TokenKind::RParen)) break;
        args.push_back(parse_expr());
      }
    }
    return args;
  };

  ExprPtr base;
  if (match(TokenKind::KwMatch)) {
    ExprPtr subject = parse_expr();
    const Token& lb = expect(TokenKind::LBrace, "`{`");
    std::vector<MatchExprArmPtr> arms;
    while (!at(TokenKind::RBrace)) {
      auto arm = std::make_unique<MatchExprArm>();
      arm->pattern = parse_pattern();
      expect(TokenKind::FatArrow, "`=>`");
      arm->value = parse_expr();
      arm->span =
          make_span(arm->pattern.span.start, arm->value->span.end, arm->pattern.span.source_path);
      arms.push_back(std::move(arm));
      (void)match(TokenKind::Comma);
    }
    const Token& rb = expect(TokenKind::RBrace, "`}`");
    if (arms.empty()) {
      throw ParseError("match requires at least one arm", lb.span);
    }
    auto e = std::make_unique<Expr>();
    Expr::Match m;
    m.subject = std::move(subject);
    m.arms = std::move(arms);
    e->span = make_span(t.span.start, rb.span.end, t.span.source_path);
    e->node = std::move(m);
    base = std::move(e);
  } else if (match(TokenKind::IntLit)) {
    auto e = std::make_unique<Expr>();
    Expr::IntLit n;
    n.value = parse_int_literal(t);
    e->span = t.span;
    e->node = n;
    base = std::move(e);
  } else if (match(TokenKind::FloatLit)) {
    auto e = std::make_unique<Expr>();
    Expr::FloatLit n;
    n.value = std::strtod(t.lexeme.c_str(), nullptr);
    e->span = t.span;
    e->node = n;
    base = std::move(e);
  } else if (match(TokenKind::KwTrue) || match(TokenKind::KwFalse)) {
    auto e = std::make_unique<Expr>();
    Expr::BoolLit n;
    n.value = (t.kind == TokenKind::KwTrue);
    e->span = t.span;
    e->node = n;
    base = std::move(e);
  } else if (match(TokenKind::StringLit)) {
    auto e = std::make_unique<Expr>();
    Expr::StringLit n;
    n.value = t.lexeme;
    e->span = t.span;
    e->node = std::move(n);
    base = std::move(e);
  } else if (match(TokenKind::LParen)) {
    base = parse_expr();
    expect(TokenKind::RParen, "`)`");
  } else if (match(TokenKind::Ident)) {
    const Token& ident = t;

    if (match(TokenKind::LParen)) {
      std::vector<ExprPtr> args = parse_call_args();
      const Token& rp = expect(TokenKind::RParen, "`)`");
      auto e = std::make_unique<Expr>();
      Expr::Call c;
      c.callee = ident.lexeme;
      c.callee_span = ident.span;
      c.args = std::move(args);
      e->span = make_span(ident.span.start, rp.span.end, ident.span.source_path);
      e->node = std::move(c);
      base = std::move(e);
    } else {
      auto e = std::make_unique<Expr>();
      Expr::Name n;
      n.ident = ident.lexeme;
      e->span = ident.span;
      e->node = std::move(n);
      base = std::move(e);
    }
  } else {
    throw ParseError("expected expression", t.span);
  }

  while (true) {
    if (match(TokenKind::Dot)) {
      const Token& member = expect(TokenKind::Ident, "member name");
      if (match(TokenKind::LParen)) {
        std::vector<ExprPtr> args = parse_call_args();
        const Token& rp = expect(TokenKind::RParen, "`)`");
        auto e = std::make_unique<Expr>();
        Expr::MethodCall c;
        c.base = std::move(base);
        c.method = member.lexeme;
        c.method_span = member.span;
        c.args = std::move(args);
        e->span = make_span(c.base->span.start, rp.span.end, c.base->span.source_path);
        e->node = std::move(c);
        base = std::move(e);
      } else {
        base = make_field_expr(std::move(base), member.lexeme, member.span.end);
      }
      continue;
    }
    if (match(TokenKind::Question)) {
      const Token& q = toks_[i_ - 1];
      auto e = std::make_unique<Expr>();
      Expr::Try tr;
      tr.inner = std::move(base);
      e->span = make_span(tr.inner->span.start, q.span.end, tr.inner->span.source_path);
      e->node = std::move(tr);
      base = std::move(e);
      continue;
    }
    break;
  }

  return base;
}

} // namespace nebula::frontend
