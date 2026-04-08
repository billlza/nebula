#pragma once

#include "frontend/source.hpp"
#include "frontend/token.hpp"

#include <string_view>
#include <vector>

namespace nebula::frontend {

class Lexer {
public:
  explicit Lexer(std::string_view input, std::string source_path = {});

  std::vector<Token> lex_all();

private:
  std::string_view input_;
  std::string source_path_;
  std::size_t i_ = 0;
  SourcePos pos_{};

  bool eof() const { return i_ >= input_.size(); }
  char peek(std::size_t lookahead = 0) const;
  bool match(char c);
  char bump();

  void skip_ws_and_comments();

  Token lex_ident_or_keyword();
  Token lex_number();
  Token lex_string();
  Token lex_punct();

  Span span_from(const SourcePos& start) const;
};

} // namespace nebula::frontend

