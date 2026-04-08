#include "frontend/parser.hpp"

#include "frontend/errors.hpp"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace nebula::frontend {

Parser::Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

namespace {
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

ExprPtr make_field_expr(const Token& base, const Token& field) {
  auto lhs = std::make_unique<Expr>();
  Expr::Field lhs_field;
  lhs_field.base = base.lexeme;
  lhs_field.field = field.lexeme;
  lhs->span = make_span(base.span.start, field.span.end, base.span.source_path);
  lhs->node = std::move(lhs_field);
  return lhs;
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
  case TokenKind::KwFn: it.node = parse_function(std::move(annotations)); break;
  case TokenKind::KwExtern: it.node = parse_function(std::move(annotations), true); break;
  case TokenKind::KwStruct: it.node = parse_struct(std::move(annotations)); break;
  case TokenKind::KwEnum: it.node = parse_enum(std::move(annotations)); break;
  default:
    throw ParseError("expected item (fn/extern fn/struct/enum)", t.span);
  }

  // span: conservatively set to child span
  it.span = std::visit([](auto&& n) { return n.span; }, it.node);
  return it;
}

Function Parser::parse_function(std::vector<std::string> annotations, bool is_extern) {
  const Token& kw = is_extern ? expect(TokenKind::KwExtern, "`extern`") : expect(TokenKind::KwFn, "`fn`");
  if (is_extern) expect(TokenKind::KwFn, "`fn`");
  const Token& name = expect(TokenKind::Ident, "function name");

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
  fn.params = std::move(params);
  fn.return_type = std::move(ret);
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
    Type arg = parse_type();
    t.arg = std::make_unique<Type>(std::move(arg));
    const Token& gt = expect(TokenKind::Greater, "`>`");
    t.span.end = gt.span.end;
  } else {
    t.span.end = name.span.end;
  }
  return t;
}

Struct Parser::parse_struct(std::vector<std::string> annotations) {
  const Token& kw = expect(TokenKind::KwStruct, "`struct`");
  const Token& name = expect(TokenKind::Ident, "struct name");
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
  st.fields = std::move(fields);
  st.span = make_span(kw.span.start, rb.span.end, kw.span.source_path);
  return st;
}

Enum Parser::parse_enum(std::vector<std::string> annotations) {
  const Token& kw = expect(TokenKind::KwEnum, "`enum`");
  const Token& name = expect(TokenKind::Ident, "enum name");
  expect(TokenKind::Less, "`<`");
  const Token& tp = expect(TokenKind::Ident, "type parameter");
  expect(TokenKind::Greater, "`>`");
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
  en.type_param = tp.lexeme;
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

  // Assignment statement forms (v0.2.x minimal surface):
  //   Ident '=' Expr
  //   Ident ('+='|'-='|'*='|'/='|'%=') Expr
  //   Ident '.' Ident '=' Expr
  //   Ident '.' Ident ('+='|'-='|'*='|'/='|'%=') Expr
  if (at(TokenKind::Ident) && peek(1).kind == TokenKind::Equal) {
    const Token& name = bump();
    expect(TokenKind::Equal, "`=`");
    ExprPtr value = parse_expr();
    Stmt::AssignVar n;
    n.name = name.lexeme;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::AssignVar>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }
  if (at(TokenKind::Ident) && is_compound_assign_op(peek(1).kind)) {
    const Token& name = bump();
    const TokenKind op = bump().kind;
    ExprPtr value = make_compound_rhs(op, make_var_name_expr(name), parse_expr());
    Stmt::AssignVar n;
    n.name = name.lexeme;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::AssignVar>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }
  if (at(TokenKind::Ident) && peek(1).kind == TokenKind::Dot && peek(2).kind == TokenKind::Ident &&
      peek(3).kind == TokenKind::Equal) {
    const Token& base = bump();
    expect(TokenKind::Dot, "`.`");
    const Token& field = expect(TokenKind::Ident, "field name");
    expect(TokenKind::Equal, "`=`");
    ExprPtr value = parse_expr();
    Stmt::AssignField n;
    n.base = base.lexeme;
    n.field = field.lexeme;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::AssignField>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }
  if (at(TokenKind::Ident) && peek(1).kind == TokenKind::Dot && peek(2).kind == TokenKind::Ident &&
      is_compound_assign_op(peek(3).kind)) {
    const Token& base = bump();
    expect(TokenKind::Dot, "`.`");
    const Token& field = expect(TokenKind::Ident, "field name");
    const TokenKind op = bump().kind;
    ExprPtr value = make_compound_rhs(op, make_field_expr(base, field), parse_expr());
    Stmt::AssignField n;
    n.base = base.lexeme;
    n.field = field.lexeme;
    n.value = std::move(value);
    s.node = std::move(n);
    s.span = make_span(start.span.start, std::get<Stmt::AssignField>(s.node).value->span.end,
                       start.span.source_path);
    return s;
  }

  // Expr statement
  ExprPtr e = parse_expr();
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
  case TokenKind::KwShared: return mk_prefix(Expr::PrefixKind::Shared);
  case TokenKind::KwUnique: return mk_prefix(Expr::PrefixKind::Unique);
  case TokenKind::KwHeap: return mk_prefix(Expr::PrefixKind::Heap);
  case TokenKind::KwPromote: return mk_prefix(Expr::PrefixKind::Promote);
  default: return parse_primary();
  }
}

ExprPtr Parser::parse_primary() {
  const Token& t = peek();
  if (match(TokenKind::IntLit)) {
    auto e = std::make_unique<Expr>();
    Expr::IntLit n;
    n.value = parse_int_literal(t);
    e->span = t.span;
    e->node = n;
    return e;
  }
  if (match(TokenKind::FloatLit)) {
    auto e = std::make_unique<Expr>();
    Expr::FloatLit n;
    n.value = std::strtod(t.lexeme.c_str(), nullptr);
    e->span = t.span;
    e->node = n;
    return e;
  }
  if (match(TokenKind::KwTrue) || match(TokenKind::KwFalse)) {
    auto e = std::make_unique<Expr>();
    Expr::BoolLit n;
    n.value = (t.kind == TokenKind::KwTrue);
    e->span = t.span;
    e->node = n;
    return e;
  }
  if (match(TokenKind::StringLit)) {
    auto e = std::make_unique<Expr>();
    Expr::StringLit n;
    n.value = t.lexeme;
    e->span = t.span;
    e->node = std::move(n);
    return e;
  }

  if (match(TokenKind::LParen)) {
    ExprPtr inner = parse_expr();
    expect(TokenKind::RParen, "`)`");
    return inner;
  }

  if (match(TokenKind::Ident)) {
    const Token& ident = t;

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

    // Method call / field access (base is identifier only in v0.2.x)
    if (match(TokenKind::Dot)) {
      const Token& member = expect(TokenKind::Ident, "member name");
      if (match(TokenKind::LParen)) {
        std::vector<ExprPtr> args = parse_call_args();
        const Token& rp = expect(TokenKind::RParen, "`)`");
        auto e = std::make_unique<Expr>();
        Expr::MethodCall c;
        c.base = ident.lexeme;
        c.method = member.lexeme;
        c.args = std::move(args);
        e->span = make_span(ident.span.start, rp.span.end, ident.span.source_path);
        e->node = std::move(c);
        return e;
      }
      auto e = std::make_unique<Expr>();
      Expr::Field f;
      f.base = ident.lexeme;
      f.field = member.lexeme;
      e->span = make_span(ident.span.start, member.span.end, ident.span.source_path);
      e->node = std::move(f);
      return e;
    }

    // Call or name
    if (match(TokenKind::LParen)) {
      std::vector<ExprPtr> args = parse_call_args();
      const Token& rp = expect(TokenKind::RParen, "`)`");
      auto e = std::make_unique<Expr>();
      Expr::Call c;
      c.callee = ident.lexeme;
      c.args = std::move(args);
      e->span = make_span(ident.span.start, rp.span.end, ident.span.source_path);
      e->node = std::move(c);
      return e;
    }

    auto e = std::make_unique<Expr>();
    Expr::Name n;
    n.ident = ident.lexeme;
    e->span = ident.span;
    e->node = std::move(n);
    return e;
  }

  throw ParseError("expected expression", t.span);
}

} // namespace nebula::frontend
