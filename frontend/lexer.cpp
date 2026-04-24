#include "frontend/lexer.hpp"

#include "frontend/errors.hpp"

#include <cctype>
#include <unordered_map>

namespace nebula::frontend {

static TokenKind keyword_kind(std::string_view s) {
  static const std::unordered_map<std::string_view, TokenKind> kMap{
      {"fn", TokenKind::KwFn},         {"let", TokenKind::KwLet},
      {"return", TokenKind::KwReturn}, {"if", TokenKind::KwIf},
      {"else", TokenKind::KwElse},     {"region", TokenKind::KwRegion},
      {"struct", TokenKind::KwStruct}, {"enum", TokenKind::KwEnum},
      {"module", TokenKind::KwModule}, {"import", TokenKind::KwImport},
      {"extern", TokenKind::KwExtern}, {"ref", TokenKind::KwRef},
      {"shared", TokenKind::KwShared}, {"unique", TokenKind::KwUnique},
      {"for", TokenKind::KwFor},       {"while", TokenKind::KwWhile},
      {"break", TokenKind::KwBreak},   {"continue", TokenKind::KwContinue},
      {"in", TokenKind::KwIn},
      {"heap", TokenKind::KwHeap},     {"promote", TokenKind::KwPromote},
      {"unsafe", TokenKind::KwUnsafe}, {"true", TokenKind::KwTrue},
      {"false", TokenKind::KwFalse},   {"match", TokenKind::KwMatch},
      {"async", TokenKind::KwAsync},   {"await", TokenKind::KwAwait},
  };
  auto it = kMap.find(s);
  if (it == kMap.end()) return TokenKind::Ident;
  return it->second;
}

Lexer::Lexer(std::string_view input, std::string source_path)
    : input_(input), source_path_(std::move(source_path)) {}

char Lexer::peek(std::size_t lookahead) const {
  const std::size_t j = i_ + lookahead;
  if (j >= input_.size()) return '\0';
  return input_[j];
}

bool Lexer::match(char c) {
  if (peek() != c) return false;
  bump();
  return true;
}

char Lexer::bump() {
  if (eof()) return '\0';
  const char c = input_[i_++];
  pos_.offset++;
  if (c == '\n') {
    pos_.line++;
    pos_.col = 1;
  } else {
    pos_.col++;
  }
  return c;
}

Span Lexer::span_from(const SourcePos& start) const {
  Span s;
  s.start = start;
  s.end = pos_;
  s.source_path = source_path_;
  return s;
}

void Lexer::skip_ws_and_comments() {
  while (!eof()) {
    const char c = peek();
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      bump();
      continue;
    }
    // Line comment
    if (c == '/' && peek(1) == '/') {
      bump();
      bump();
      while (!eof() && peek() != '\n') bump();
      continue;
    }
    // Block comment (optional)
    if (c == '/' && peek(1) == '*') {
      bump();
      bump();
      while (!eof()) {
        if (peek() == '*' && peek(1) == '/') {
          bump();
          bump();
          break;
        }
        bump();
      }
      continue;
    }
    break;
  }
}

Token Lexer::lex_ident_or_keyword() {
  const SourcePos start = pos_;
  std::string s;
  while (!eof()) {
    const char c = peek();
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      s.push_back(bump());
    } else {
      break;
    }
  }
  Token t;
  t.kind = keyword_kind(s);
  t.lexeme = std::move(s);
  t.span = span_from(start);
  return t;
}

Token Lexer::lex_number() {
  const SourcePos start = pos_;
  std::string s;
  auto is_hex_digit = [](char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isdigit(u) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  auto lex_digit_seq = [&](bool allow_empty) {
    bool saw_digit = false;
    bool last_was_sep = false;
    while (!eof()) {
      const char c = peek();
      if (std::isdigit(static_cast<unsigned char>(c))) {
        saw_digit = true;
        last_was_sep = false;
        s.push_back(bump());
        continue;
      }
      if (c == '_') {
        const char next = peek(1);
        if (!saw_digit || last_was_sep || !std::isdigit(static_cast<unsigned char>(next))) {
          throw LexError("invalid numeric separator placement", span_from(start));
        }
        last_was_sep = true;
        bump();
        continue;
      }
      break;
    }
    if (!allow_empty && !saw_digit) {
      throw LexError("expected digit in numeric literal", span_from(start));
    }
    if (last_was_sep) {
      throw LexError("invalid numeric separator placement", span_from(start));
    }
  };

  // Base-prefixed integer literals.
  // - 0x / 0X => hexadecimal
  // - 0b / 0B => binary
  // - 0o / 0O => octal
  if (peek() == '0' &&
      (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B' || peek(1) == 'o' ||
       peek(1) == 'O')) {
    const char prefix = peek(1);
    s.push_back(bump()); // '0'
    s.push_back(bump()); // base prefix
    const bool is_hex = (prefix == 'x' || prefix == 'X');
    const bool is_bin = (prefix == 'b' || prefix == 'B');
    bool saw_digit = false;
    bool last_was_sep = false;
    while (!eof()) {
      const char c = peek();
      const bool valid_digit = is_hex ? is_hex_digit(c) : (is_bin ? (c == '0' || c == '1')
                                                                   : (c >= '0' && c <= '7'));
      if (valid_digit) {
        saw_digit = true;
        last_was_sep = false;
        s.push_back(bump());
        continue;
      }
      if (c == '_') {
        const char next = peek(1);
        const bool next_valid_digit = is_hex ? is_hex_digit(next)
                                             : (is_bin ? (next == '0' || next == '1')
                                                       : (next >= '0' && next <= '7'));
        if (!saw_digit || last_was_sep || !next_valid_digit) {
          throw LexError("invalid numeric separator placement", span_from(start));
        }
        last_was_sep = true;
        bump();
        continue;
      }
      break;
    }
    if (!saw_digit) {
      throw LexError("expected digit in numeric literal", span_from(start));
    }
    if (last_was_sep) {
      throw LexError("invalid numeric separator placement", span_from(start));
    }
    const char next = peek();
    if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
      throw LexError("invalid digit in numeric literal", span_from(start));
    }
    Token t;
    t.kind = TokenKind::IntLit;
    t.lexeme = std::move(s);
    t.span = span_from(start);
    return t;
  }

  lex_digit_seq(false);

  bool is_float = false;
  // Float: digits '.' digits (but not '..' which is range)
  if (!eof() && peek() == '.' && peek(1) != '.') {
    is_float = true;
    s.push_back(bump()); // '.'
    lex_digit_seq(true);
  }
  // Scientific notation: [digits][.digits][e|E][+|-]?digits
  if (!eof() && (peek() == 'e' || peek() == 'E')) {
    is_float = true;
    s.push_back(bump()); // e / E
    if (!eof() && (peek() == '+' || peek() == '-')) {
      s.push_back(bump());
    }
    bool saw_digit = false;
    bool last_was_sep = false;
    while (!eof()) {
      const char c = peek();
      if (std::isdigit(static_cast<unsigned char>(c))) {
        saw_digit = true;
        last_was_sep = false;
        s.push_back(bump());
        continue;
      }
      if (c == '_') {
        const char next = peek(1);
        if (!saw_digit || last_was_sep || !std::isdigit(static_cast<unsigned char>(next))) {
          throw LexError("invalid numeric separator placement", span_from(start));
        }
        last_was_sep = true;
        bump();
        continue;
      }
      break;
    }
    if (!saw_digit) {
      throw LexError("expected digit in numeric literal", span_from(start));
    }
    if (last_was_sep) {
      throw LexError("invalid numeric separator placement", span_from(start));
    }
  }

  Token t;
  t.kind = is_float ? TokenKind::FloatLit : TokenKind::IntLit;
  t.lexeme = std::move(s);
  t.span = span_from(start);
  return t;
}

Token Lexer::lex_string() {
  const SourcePos start = pos_;
  // Opening quote
  bump();

  std::string s;
  while (!eof()) {
    const char c = bump();
    if (c == '"') {
      Token t;
      t.kind = TokenKind::StringLit;
      t.lexeme = std::move(s);
      t.span = span_from(start);
      return t;
    }
    if (c == '\\') {
      if (eof()) break;
      const char e = bump();
      switch (e) {
      case 'n': s.push_back('\n'); break;
      case 'r': s.push_back('\r'); break;
      case 't': s.push_back('\t'); break;
      case '"': s.push_back('"'); break;
      case '\\': s.push_back('\\'); break;
      default:
        // Keep unknown escapes as-is
        s.push_back(e);
        break;
      }
      continue;
    }
    s.push_back(c);
  }
  throw LexError("unterminated string literal", span_from(start));
}

Token Lexer::lex_punct() {
  const SourcePos start = pos_;
  const char c = peek();

  // Two-char tokens first
  if (c == '-' && peek(1) == '>') {
    bump();
    bump();
    return Token{TokenKind::Arrow, span_from(start), "->"};
  }
  if (c == '+' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::PlusEqual, span_from(start), "+="};
  }
  if (c == '&' && peek(1) == '&') {
    bump();
    bump();
    return Token{TokenKind::AmpAmp, span_from(start), "&&"};
  }
  if (c == '|' && peek(1) == '|') {
    bump();
    bump();
    return Token{TokenKind::PipePipe, span_from(start), "||"};
  }
  if (c == '=' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::EqualEqual, span_from(start), "=="};
  }
  if (c == '=' && peek(1) == '>') {
    bump();
    bump();
    return Token{TokenKind::FatArrow, span_from(start), "=>"};
  }
  if (c == '!' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::BangEqual, span_from(start), "!="};
  }
  if (c == ':' && peek(1) == ':') {
    bump();
    bump();
    return Token{TokenKind::ColonColon, span_from(start), "::"};
  }
  if (c == '<' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::LessEqual, span_from(start), "<="};
  }
  if (c == '>' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::GreaterEqual, span_from(start), ">="};
  }
  if (c == '-' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::MinusEqual, span_from(start), "-="};
  }
  if (c == '*' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::StarEqual, span_from(start), "*="};
  }
  if (c == '/' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::SlashEqual, span_from(start), "/="};
  }
  if (c == '%' && peek(1) == '=') {
    bump();
    bump();
    return Token{TokenKind::PercentEqual, span_from(start), "%="};
  }
  if (c == '.' && peek(1) == '.' && peek(2) == '.') {
    bump();
    bump();
    bump();
    return Token{TokenKind::DotDotDot, span_from(start), "..."};
  }
  if (c == '.' && peek(1) == '.' && peek(2) == '<') {
    bump();
    bump();
    bump();
    return Token{TokenKind::DotDotLess, span_from(start), "..<"};
  }
  if (c == '.' && peek(1) == '.') {
    bump();
    bump();
    return Token{TokenKind::DotDot, span_from(start), ".."};
  }

  bump();

  switch (c) {
  case '@': return Token{TokenKind::At, span_from(start), "@"};
  case '(': return Token{TokenKind::LParen, span_from(start), "("};
  case ')': return Token{TokenKind::RParen, span_from(start), ")"};
  case '{': return Token{TokenKind::LBrace, span_from(start), "{"};
  case '}': return Token{TokenKind::RBrace, span_from(start), "}"};
  case ',': return Token{TokenKind::Comma, span_from(start), ","};
  case '.': return Token{TokenKind::Dot, span_from(start), "."};
  case '?': return Token{TokenKind::Question, span_from(start), "?"};
  case ':': return Token{TokenKind::Colon, span_from(start), ":"};
  case '<': return Token{TokenKind::Less, span_from(start), "<"};
  case '>': return Token{TokenKind::Greater, span_from(start), ">"};
  case '=': return Token{TokenKind::Equal, span_from(start), "="};
  case '+': return Token{TokenKind::Plus, span_from(start), "+"};
  case '-': return Token{TokenKind::Minus, span_from(start), "-"};
  case '!': return Token{TokenKind::Bang, span_from(start), "!"};
  case '*': return Token{TokenKind::Star, span_from(start), "*"};
  case '/': return Token{TokenKind::Slash, span_from(start), "/"};
  case '%': return Token{TokenKind::Percent, span_from(start), "%"};
  default:
    break;
  }

  throw LexError(std::string("unexpected character: '") + c + "'", span_from(start));
}

std::vector<Token> Lexer::lex_all() {
  std::vector<Token> out;

  while (true) {
    skip_ws_and_comments();
    if (eof()) {
      Token t;
      t.kind = TokenKind::Eof;
      t.lexeme = "";
      t.span = span_from(pos_);
      out.push_back(std::move(t));
      break;
    }

    const char c = peek();
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      out.push_back(lex_ident_or_keyword());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      out.push_back(lex_number());
      continue;
    }
    if (c == '"') {
      out.push_back(lex_string());
      continue;
    }

    out.push_back(lex_punct());
  }

  return out;
}

} // namespace nebula::frontend
