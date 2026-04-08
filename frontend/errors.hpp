#pragma once

#include "frontend/source.hpp"

#include <stdexcept>
#include <string>

namespace nebula::frontend {

struct FrontendError : public std::runtime_error {
  Span span{};
  explicit FrontendError(const std::string& msg, Span s) : std::runtime_error(msg), span(s) {}
};

struct LexError : public FrontendError {
  explicit LexError(const std::string& msg, Span s) : FrontendError(msg, s) {}
};

struct ParseError : public FrontendError {
  explicit ParseError(const std::string& msg, Span s) : FrontendError(msg, s) {}
};

} // namespace nebula::frontend


