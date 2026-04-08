#pragma once

#include "frontend/source.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace nebula::frontend {

enum class TokenKind : std::uint16_t {
  Eof = 0,

  Ident,
  IntLit,
  FloatLit,
  StringLit,

  // Keywords
  KwFn,
  KwLet,
  KwReturn,
  KwIf,
  KwElse,
  KwRegion,
  KwStruct,
  KwEnum,
  KwModule,
  KwImport,
  KwExtern,
  KwRef,
  KwShared,
  KwUnique,
  KwFor,
  KwIn,
  KwHeap,
  KwPromote,
  KwUnsafe,
  KwTrue,
  KwFalse,

  // Punctuation / operators
  At,      // @
  LParen,  // (
  RParen,  // )
  LBrace,  // {
  RBrace,  // }
  Comma,   // ,
  Dot,     // .
  Colon,   // :
  ColonColon, // ::
  Less,    // <
  Greater, // >
  Equal,   // =
  Arrow,   // ->
  DotDot,  // ..
  DotDotLess, // ..<
  DotDotDot,  // ...

  Plus,  // +
  Minus, // -
  Bang,  // !
  Star,  // *
  Slash, // /
  Percent, // %
  AmpAmp, // &&
  PipePipe, // ||
  EqualEqual, // ==
  BangEqual, // !=
  LessEqual, // <=
  GreaterEqual, // >=
  PlusEqual,  // +=
  MinusEqual, // -=
  StarEqual,  // *=
  SlashEqual, // /=
  PercentEqual, // %=
};

inline const char* token_kind_name(TokenKind k) {
  switch (k) {
  case TokenKind::Eof: return "eof";
  case TokenKind::Ident: return "ident";
  case TokenKind::IntLit: return "int";
  case TokenKind::FloatLit: return "float";
  case TokenKind::StringLit: return "string";
  case TokenKind::KwFn: return "fn";
  case TokenKind::KwLet: return "let";
  case TokenKind::KwReturn: return "return";
  case TokenKind::KwIf: return "if";
  case TokenKind::KwElse: return "else";
  case TokenKind::KwRegion: return "region";
  case TokenKind::KwStruct: return "struct";
  case TokenKind::KwEnum: return "enum";
  case TokenKind::KwModule: return "module";
  case TokenKind::KwImport: return "import";
  case TokenKind::KwExtern: return "extern";
  case TokenKind::KwRef: return "ref";
  case TokenKind::KwShared: return "shared";
  case TokenKind::KwUnique: return "unique";
  case TokenKind::KwFor: return "for";
  case TokenKind::KwIn: return "in";
  case TokenKind::KwHeap: return "heap";
  case TokenKind::KwPromote: return "promote";
  case TokenKind::KwUnsafe: return "unsafe";
  case TokenKind::KwTrue: return "true";
  case TokenKind::KwFalse: return "false";
  case TokenKind::At: return "@";
  case TokenKind::LParen: return "(";
  case TokenKind::RParen: return ")";
  case TokenKind::LBrace: return "{";
  case TokenKind::RBrace: return "}";
  case TokenKind::Comma: return ",";
  case TokenKind::Dot: return ".";
  case TokenKind::Colon: return ":";
  case TokenKind::ColonColon: return "::";
  case TokenKind::Less: return "<";
  case TokenKind::Greater: return ">";
  case TokenKind::Equal: return "=";
  case TokenKind::Arrow: return "->";
  case TokenKind::DotDot: return "..";
  case TokenKind::DotDotLess: return "..<";
  case TokenKind::DotDotDot: return "...";
  case TokenKind::Plus: return "+";
  case TokenKind::Minus: return "-";
  case TokenKind::Bang: return "!";
  case TokenKind::Star: return "*";
  case TokenKind::Slash: return "/";
  case TokenKind::Percent: return "%";
  case TokenKind::AmpAmp: return "&&";
  case TokenKind::PipePipe: return "||";
  case TokenKind::EqualEqual: return "==";
  case TokenKind::BangEqual: return "!=";
  case TokenKind::LessEqual: return "<=";
  case TokenKind::GreaterEqual: return ">=";
  case TokenKind::PlusEqual: return "+=";
  case TokenKind::MinusEqual: return "-=";
  case TokenKind::StarEqual: return "*=";
  case TokenKind::SlashEqual: return "/=";
  case TokenKind::PercentEqual: return "%=";
  }
  return "unknown";
}

struct Token {
  TokenKind kind{};
  Span span{};
  std::string lexeme;

  bool is(TokenKind k) const { return kind == k; }
  bool is_keyword() const {
    switch (kind) {
    case TokenKind::KwFn:
    case TokenKind::KwLet:
    case TokenKind::KwReturn:
    case TokenKind::KwIf:
    case TokenKind::KwElse:
    case TokenKind::KwRegion:
    case TokenKind::KwStruct:
    case TokenKind::KwEnum:
    case TokenKind::KwModule:
    case TokenKind::KwImport:
    case TokenKind::KwExtern:
    case TokenKind::KwRef:
    case TokenKind::KwShared:
    case TokenKind::KwUnique:
    case TokenKind::KwFor:
    case TokenKind::KwIn:
    case TokenKind::KwHeap:
    case TokenKind::KwPromote:
    case TokenKind::KwUnsafe:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse: return true;
    default: return false;
    }
  }
};

} // namespace nebula::frontend
