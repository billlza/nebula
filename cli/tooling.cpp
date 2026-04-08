#include "cli_shared.hpp"
#include "project.hpp"

#include "frontend/errors.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "nir/ir.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <unistd.h>
#include <unordered_map>

namespace {

using nebula::frontend::Block;
using nebula::frontend::Diagnostic;
using nebula::frontend::Expr;
using nebula::frontend::Function;
using nebula::frontend::Item;
using nebula::frontend::Lexer;
using nebula::frontend::Parser;
using nebula::frontend::Program;
using nebula::frontend::Severity;
using nebula::frontend::Stmt;
using nebula::frontend::Struct;
using nebula::frontend::Type;
using nebula::frontend::Variant;

std::string trim(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start += 1;
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end -= 1;
  return std::string(text.substr(start, end - start));
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(c); break;
    }
  }
  return out;
}

std::optional<fs::path> manifest_path_from_input(const fs::path& input) {
  const fs::path abs = fs::absolute(input);
  if (abs.filename() == "nebula.toml" && fs::exists(abs)) return abs;
  fs::path cur = fs::is_directory(abs) ? abs : abs.parent_path();
  std::error_code ec;
  cur = fs::weakly_canonical(cur, ec);
  if (ec) cur = cur.lexically_normal();
  while (!cur.empty()) {
    const fs::path candidate = cur / "nebula.toml";
    if (fs::exists(candidate)) return candidate;
    if (cur == cur.root_path()) break;
    cur = cur.parent_path();
  }
  return std::nullopt;
}

std::string render_string_list(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << json_escape(values[i]) << "\"";
  }
  out << "]";
  return out.str();
}

std::string render_manifest(const PackageManifest& manifest) {
  std::ostringstream out;
  out << "schema_version = " << manifest.schema_version << "\n\n";
  out << "[package]\n";
  out << "name = \"" << json_escape(manifest.name) << "\"\n";
  out << "version = \"" << json_escape(manifest.version) << "\"\n";
  out << "entry = \"" << json_escape(manifest.entry.lexically_normal().string()) << "\"\n";
  out << "src_dir = \"" << json_escape(manifest.src_dir.lexically_normal().string()) << "\"\n";
  if (!manifest.host_cxx.empty()) {
    std::vector<std::string> host_items;
    for (const auto& item : manifest.host_cxx) host_items.push_back(item.lexically_normal().string());
    std::sort(host_items.begin(), host_items.end());
    out << "host_cxx = " << render_string_list(host_items) << "\n";
  }

  if (!manifest.dependencies.empty()) {
    out << "\n[dependencies]\n";
    std::vector<ManifestDependency> deps = manifest.dependencies;
    std::sort(deps.begin(), deps.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.alias < rhs.alias;
    });
    for (const auto& dep : deps) {
      out << dep.alias << " = ";
      if (dep.kind == ManifestDependencyKind::Version) {
        out << "\"" << json_escape(dep.version) << "\"\n";
      } else if (dep.kind == ManifestDependencyKind::Path) {
        out << "{ path = \"" << json_escape(dep.path.lexically_normal().string()) << "\" }\n";
      } else {
        out << "{ git = \"" << json_escape(dep.git) << "\", rev = \"" << json_escape(dep.rev)
            << "\" }\n";
      }
    }
  }

  if (!manifest.workspace_members.empty()) {
    out << "\n[workspace]\n";
    std::vector<std::string> members;
    for (const auto& member : manifest.workspace_members) {
      members.push_back(member.lexically_normal().string());
    }
    std::sort(members.begin(), members.end());
    out << "members = " << render_string_list(members) << "\n";
  }

  return out.str();
}

struct Formatter {
  std::ostringstream out;
  int indent = 0;

  void line(const std::string& text = {}) {
    for (int i = 0; i < indent; ++i) out << "  ";
    out << text << "\n";
  }

  std::string format_type(const Type& ty) {
    if (ty.callable_ret) {
      std::ostringstream s;
      s << (ty.is_unsafe_callable ? "UnsafeFn(" : "Fn(");
      for (std::size_t i = 0; i < ty.callable_params.size(); ++i) {
        if (i) s << ", ";
        s << format_type(ty.callable_params[i]);
      }
      s << ") -> " << format_type(*ty.callable_ret);
      return s.str();
    }
    std::string out_ty = ty.name;
    if (ty.arg) out_ty += "<" + format_type(*ty.arg) + ">";
    return out_ty;
  }

  std::string format_expr(const Expr& expr) {
    return std::visit(
        [&](auto&& n) -> std::string {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::IntLit>) {
            return std::to_string(n.value);
          } else if constexpr (std::is_same_v<N, Expr::FloatLit>) {
            std::ostringstream s;
            s << n.value;
            return s.str();
          } else if constexpr (std::is_same_v<N, Expr::BoolLit>) {
            return n.value ? "true" : "false";
          } else if constexpr (std::is_same_v<N, Expr::StringLit>) {
            return "\"" + json_escape(n.value) + "\"";
          } else if constexpr (std::is_same_v<N, Expr::Name>) {
            return n.ident;
          } else if constexpr (std::is_same_v<N, Expr::Field>) {
            return n.base + "." + n.field;
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            std::ostringstream s;
            s << n.callee << "(";
            for (std::size_t i = 0; i < n.args.size(); ++i) {
              if (i) s << ", ";
              s << format_expr(*n.args[i]);
            }
            s << ")";
            return s.str();
          } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
            std::ostringstream s;
            s << n.base << "." << n.method << "(";
            for (std::size_t i = 0; i < n.args.size(); ++i) {
              if (i) s << ", ";
              s << format_expr(*n.args[i]);
            }
            s << ")";
            return s.str();
          } else if constexpr (std::is_same_v<N, Expr::Binary>) {
            auto op = std::string("+");
            switch (n.op) {
            case Expr::BinOp::Add: op = "+"; break;
            case Expr::BinOp::Sub: op = "-"; break;
            case Expr::BinOp::Mul: op = "*"; break;
            case Expr::BinOp::Div: op = "/"; break;
            case Expr::BinOp::Mod: op = "%"; break;
            case Expr::BinOp::Eq: op = "=="; break;
            case Expr::BinOp::Ne: op = "!="; break;
            case Expr::BinOp::Lt: op = "<"; break;
            case Expr::BinOp::Lte: op = "<="; break;
            case Expr::BinOp::Gt: op = ">"; break;
            case Expr::BinOp::Gte: op = ">="; break;
            case Expr::BinOp::And: op = "&&"; break;
            case Expr::BinOp::Or: op = "||"; break;
            }
            return "(" + format_expr(*n.lhs) + " " + op + " " + format_expr(*n.rhs) + ")";
          } else if constexpr (std::is_same_v<N, Expr::Unary>) {
            return "!" + format_expr(*n.inner);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            std::string kw = "heap";
            switch (n.kind) {
            case Expr::PrefixKind::Shared: kw = "shared"; break;
            case Expr::PrefixKind::Unique: kw = "unique"; break;
            case Expr::PrefixKind::Heap: kw = "heap"; break;
            case Expr::PrefixKind::Promote: kw = "promote"; break;
            }
            return kw + " " + format_expr(*n.inner);
          }
          return "/*expr*/";
        },
        expr.node);
  }

  void format_block(const Block& block) {
    line("{");
    indent += 1;
    for (const auto& stmt : block.stmts) format_stmt(stmt);
    indent -= 1;
    line("}");
  }

  void format_stmt(const Stmt& stmt) {
    for (const auto& ann : stmt.annotations) line("@" + ann);
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            line("let " + n.name + " = " + format_expr(*n.value));
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            line("return " + format_expr(*n.value));
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            line(format_expr(*n.expr));
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            line(n.name + " = " + format_expr(*n.value));
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            line(n.base + "." + n.field + " = " + format_expr(*n.value));
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            line("region " + n.name + " ");
            format_block(n.body);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            line("unsafe ");
            format_block(n.body);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            line("if " + format_expr(*n.cond) + " ");
            format_block(n.then_body);
            if (n.else_body.has_value()) {
              line("else ");
              format_block(*n.else_body);
            }
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            line("for " + n.var + " in " + format_expr(*n.start) + " .. " + format_expr(*n.end) +
                 " ");
            format_block(n.body);
          }
        },
        stmt.node);
  }

  void format_item(const Item& item) {
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Function>) {
            for (const auto& ann : n.annotations) line("@" + ann);
            std::ostringstream sig;
            if (n.is_extern) sig << "extern ";
            sig << "fn " << n.name << "(";
            for (std::size_t i = 0; i < n.params.size(); ++i) {
              if (i) sig << ", ";
              if (n.params[i].is_ref) sig << "ref ";
              sig << n.params[i].name << ": " << format_type(n.params[i].type);
            }
            sig << ")";
            if (n.return_type.has_value()) sig << " -> " << format_type(*n.return_type);
            if (n.is_extern) {
              line(sig.str());
            } else if (n.body.has_value()) {
              line(sig.str() + " ");
              format_block(*n.body);
            }
          } else if constexpr (std::is_same_v<N, Struct>) {
            for (const auto& ann : n.annotations) line("@" + ann);
            line("struct " + n.name + " {");
            indent += 1;
            for (const auto& field : n.fields) {
              line(field.name + ": " + format_type(field.type));
            }
            indent -= 1;
            line("}");
          } else {
            const auto& en = n;
            for (const auto& ann : en.annotations) line("@" + ann);
            line("enum " + en.name + "<" + en.type_param + "> {");
            indent += 1;
            for (const Variant& variant : en.variants) {
              line(variant.name + "(" + format_type(variant.payload) + ")");
            }
            indent -= 1;
            line("}");
          }
        },
        item.node);
  }

  std::string format_program(const Program& program) {
    if (program.module_name.has_value()) line("module " + *program.module_name);
    for (const auto& imp : program.imports) line("import " + imp);
    if ((program.module_name.has_value() || !program.imports.empty()) && !program.items.empty()) line();
    for (std::size_t i = 0; i < program.items.size(); ++i) {
      format_item(program.items[i]);
      if (i + 1 < program.items.size()) line();
    }
    return out.str();
  }
};

std::string trim_left(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() &&
         (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n')) {
    start += 1;
  }
  return std::string(text.substr(start));
}

std::string trim_right(std::string_view text) {
  std::size_t end = text.size();
  while (end > 0 &&
         (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
          text[end - 1] == '\n')) {
    end -= 1;
  }
  return std::string(text.substr(0, end));
}

std::vector<std::string> split_lines_preserve_empty(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = start;
    while (end < text.size() && text[end] != '\n') end += 1;
    lines.emplace_back(text.substr(start, end - start));
    if (end == text.size()) break;
    start = end + 1;
  }
  return lines;
}

std::string leading_indent(std::string_view line) {
  std::size_t end = 0;
  while (end < line.size() && (line[end] == ' ' || line[end] == '\t')) end += 1;
  return std::string(line.substr(0, end));
}

std::vector<std::string> normalize_comment_lines(const std::string& text,
                                                 std::string_view indent_prefix) {
  auto lines = split_lines_preserve_empty(text);
  std::size_t common_indent = std::string::npos;
  for (const auto& line : lines) {
    if (line.empty()) continue;
    std::size_t indent = 0;
    while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) indent += 1;
    common_indent = (common_indent == std::string::npos) ? indent : std::min(common_indent, indent);
  }
  if (common_indent == std::string::npos) common_indent = 0;

  std::vector<std::string> out;
  out.reserve(lines.size());
  const bool multi_line = lines.size() > 1;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    const std::size_t drop = std::min(common_indent, line.size());
    if (multi_line && i > 0) {
      out.push_back(line.substr(drop));
    } else {
      out.push_back(std::string(indent_prefix) + line.substr(drop));
    }
  }
  return out;
}

std::vector<nebula::frontend::Token> lex_non_eof_tokens(const std::string& text,
                                                        const std::string& path) {
  Lexer lex(text, path);
  auto toks = lex.lex_all();
  toks.erase(std::remove_if(toks.begin(), toks.end(),
                            [](const nebula::frontend::Token& tok) {
                              return tok.kind == nebula::frontend::TokenKind::Eof;
                            }),
             toks.end());
  return toks;
}

bool tokens_equivalent(const nebula::frontend::Token& lhs, const nebula::frontend::Token& rhs) {
  using nebula::frontend::TokenKind;
  if (lhs.kind != rhs.kind) return false;
  switch (lhs.kind) {
  case TokenKind::Ident:
  case TokenKind::StringLit: return lhs.lexeme == rhs.lexeme;
  default: return true;
  }
}

std::vector<int> align_original_tokens_to_formatted(
    const std::vector<nebula::frontend::Token>& original,
    const std::vector<nebula::frontend::Token>& formatted) {
  const std::size_t n = original.size();
  const std::size_t m = formatted.size();
  std::vector<int> dp((n + 1) * (m + 1), 0);
  auto at = [&](std::size_t i, std::size_t j) -> int& { return dp[i * (m + 1) + j]; };

  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      if (tokens_equivalent(original[i], formatted[j])) {
        at(i, j) = 1 + at(i + 1, j + 1);
      } else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  std::vector<int> mapping(n, -1);
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n && j < m) {
    if (tokens_equivalent(original[i], formatted[j]) && at(i, j) == 1 + at(i + 1, j + 1)) {
      mapping[i] = static_cast<int>(j);
      i += 1;
      j += 1;
      continue;
    }
    if (at(i + 1, j) >= at(i, j + 1)) {
      i += 1;
    } else {
      j += 1;
    }
  }
  return mapping;
}

struct PreservedComment {
  std::string text;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  int start_line = 1;
  int end_line = 1;
  bool trailing = false;
  bool single_line = true;
  int prev_token_index = -1;
  int next_token_index = -1;
};

std::vector<PreservedComment> collect_preserved_comments(const std::string& text,
                                                         const std::string& path) {
  const auto tokens = lex_non_eof_tokens(text, path);
  std::vector<PreservedComment> comments;

  std::size_t i = 0;
  int line = 1;
  bool in_string = false;
  bool escaped = false;
  while (i < text.size()) {
    const char ch = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      if (ch == '\n') line += 1;
      i += 1;
      continue;
    }

    if (ch == '"') {
      in_string = true;
      i += 1;
      continue;
    }

    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      const std::size_t start = i;
      const int start_line = line;
      i += 2;
      while (i < text.size() && text[i] != '\n') i += 1;
      comments.push_back(
          {text.substr(start, i - start), start, i, start_line, start_line, false, true, -1, -1});
      continue;
    }

    if (ch == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      const std::size_t start = i;
      const int start_line = line;
      i += 2;
      while (i < text.size()) {
        if (text[i] == '\n') line += 1;
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
          i += 2;
          break;
        }
        i += 1;
      }
      comments.push_back(
          {text.substr(start, i - start), start, i, start_line, line, false, start_line == line, -1, -1});
      continue;
    }

    if (ch == '\n') line += 1;
    i += 1;
  }

  std::size_t prev_cursor = 0;
  for (auto& comment : comments) {
    int prev_index = -1;
    while (prev_cursor < tokens.size() && tokens[prev_cursor].span.end.offset <= comment.start_offset) {
      prev_index = static_cast<int>(prev_cursor);
      prev_cursor += 1;
    }
    comment.prev_token_index = prev_index;
    int next_index = static_cast<int>(prev_cursor);
    while (next_index < static_cast<int>(tokens.size()) &&
           tokens[next_index].span.start.offset < comment.end_offset) {
      next_index += 1;
    }
    comment.next_token_index = (next_index < static_cast<int>(tokens.size())) ? next_index : -1;
    if (comment.prev_token_index >= 0 &&
        tokens[comment.prev_token_index].span.end.line == comment.start_line && comment.single_line) {
      const auto& prev = tokens[comment.prev_token_index];
      const std::string gap =
          text.substr(prev.span.end.offset, comment.start_offset - prev.span.end.offset);
      comment.trailing = gap.find_first_not_of(" \t\r") == std::string::npos;
    }
  }

  return comments;
}

std::string format_program_preserving_comments(const std::string& original,
                                               const std::string& path,
                                               const Program& program) {
  Formatter formatter;
  const std::string formatted = formatter.format_program(program);
  const auto comments = collect_preserved_comments(original, path);
  if (comments.empty()) return formatted;

  const auto original_tokens = lex_non_eof_tokens(original, path);
  const auto formatted_tokens = lex_non_eof_tokens(formatted, path);
  const auto mapping = align_original_tokens_to_formatted(original_tokens, formatted_tokens);
  auto lines = split_lines_preserve_empty(formatted);

  std::map<int, std::vector<std::string>> before_lines;
  std::map<int, std::vector<std::string>> after_lines;
  for (const auto& comment : comments) {
    int formatted_prev = -1;
    if (comment.prev_token_index >= 0 && comment.prev_token_index < static_cast<int>(mapping.size())) {
      formatted_prev = mapping[comment.prev_token_index];
    }
    int formatted_next = -1;
    if (comment.next_token_index >= 0 && comment.next_token_index < static_cast<int>(mapping.size())) {
      formatted_next = mapping[comment.next_token_index];
    }

    if (comment.trailing && formatted_prev >= 0 &&
        formatted_prev < static_cast<int>(formatted_tokens.size())) {
      const int line_index = std::max(0, formatted_tokens[formatted_prev].span.end.line - 1);
      if (line_index < static_cast<int>(lines.size())) {
        lines[line_index] = trim_right(lines[line_index]);
        if (!lines[line_index].empty()) lines[line_index] += "  ";
        lines[line_index] += trim_left(comment.text);
        continue;
      }
    }

    int anchor_line = 0;
    bool insert_after = false;
    if (formatted_next >= 0 && formatted_next < static_cast<int>(formatted_tokens.size())) {
      anchor_line = std::max(0, formatted_tokens[formatted_next].span.start.line - 1);
    } else if (formatted_prev >= 0 && formatted_prev < static_cast<int>(formatted_tokens.size())) {
      anchor_line = std::max(0, formatted_tokens[formatted_prev].span.end.line - 1);
      insert_after = true;
    }

    const std::string indent =
        (anchor_line >= 0 && anchor_line < static_cast<int>(lines.size())) ? leading_indent(lines[anchor_line])
                                                                            : std::string{};
    const auto normalized = normalize_comment_lines(comment.text, indent);
    if (insert_after) {
      after_lines[anchor_line].insert(after_lines[anchor_line].end(), normalized.begin(), normalized.end());
    } else {
      before_lines[anchor_line].insert(before_lines[anchor_line].end(), normalized.begin(),
                                       normalized.end());
    }
  }

  std::ostringstream out;
  const int line_count = static_cast<int>(lines.size());
  for (int line_index = 0; line_index < line_count; ++line_index) {
    auto before_it = before_lines.find(line_index);
    if (before_it != before_lines.end()) {
      for (const auto& line : before_it->second) out << line << "\n";
    }
    out << lines[line_index];
    if (line_index + 1 < line_count || !formatted.empty()) out << "\n";
    auto after_it = after_lines.find(line_index);
    if (after_it != after_lines.end()) {
      for (const auto& line : after_it->second) out << line << "\n";
    }
  }
  auto tail_it = before_lines.find(line_count);
  if (tail_it != before_lines.end()) {
    for (const auto& line : tail_it->second) out << line << "\n";
  }
  return out.str();
}

bool parse_program_file(const fs::path& path, Program& out, std::vector<Diagnostic>& diags) {
  std::string text;
  if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) return false;
  try {
    Lexer lex(text, path.string());
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    out = parser.parse_program();
    return true;
  } catch (const nebula::frontend::FrontendError& e) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-PAR900";
    d.message = e.what();
    d.span = e.span;
    d.category = "parser";
    d.risk = nebula::frontend::DiagnosticRisk::High;
    diags.push_back(std::move(d));
    return false;
  }
}

std::vector<fs::path> collect_format_targets(const fs::path& input) {
  std::vector<fs::path> out;
  if (fs::is_regular_file(input)) {
    out.push_back(input);
    return out;
  }
  for (const auto& entry : fs::recursive_directory_iterator(input)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() == ".nb") out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool parse_int_flag(const std::vector<std::string>& args,
                    std::size_t& i,
                    int& out) {
  if (i + 1 >= args.size()) return false;
  try {
    out = std::stoi(args[++i]);
    return true;
  } catch (...) {
    return false;
  }
}

std::unordered_map<nebula::nir::VarId, std::string> collect_nir_var_names(const nebula::nir::Function& fn) {
  std::unordered_map<nebula::nir::VarId, std::string> out;
  for (const auto& p : fn.params) out.insert({p.var, p.name});

  std::function<void(const nebula::nir::Block&)> walk_block = [&](const nebula::nir::Block& block) {
    for (const auto& stmt : block.stmts) {
      std::visit(
          [&](auto&& st) {
            using S = std::decay_t<decltype(st)>;
            if constexpr (std::is_same_v<S, nebula::nir::Stmt::Let>) {
              out.insert({st.var, st.name});
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::For>) {
              out.insert({st.var, st.var_name});
              walk_block(st.body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::Region>) {
              walk_block(st.body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::Unsafe>) {
              walk_block(st.body);
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::If>) {
              walk_block(st.then_body);
              if (st.else_body.has_value()) walk_block(*st.else_body);
            }
          },
          stmt.node);
    }
  };

  if (fn.body.has_value()) walk_block(*fn.body);
  return out;
}

std::optional<std::size_t> json_value_start(const std::string& body,
                                            std::string_view key,
                                            std::size_t from = 0) {
  const std::string pattern = "\"" + std::string(key) + "\"";
  std::size_t search = from;
  while (search < body.size()) {
    const std::size_t key_pos = body.find(pattern, search);
    if (key_pos == std::string::npos) return std::nullopt;
    std::size_t colon = key_pos + pattern.size();
    while (colon < body.size() && std::isspace(static_cast<unsigned char>(body[colon]))) {
      colon += 1;
    }
    if (colon >= body.size() || body[colon] != ':') {
      search = key_pos + 1;
      continue;
    }
    colon += 1;
    while (colon < body.size() && std::isspace(static_cast<unsigned char>(body[colon]))) {
      colon += 1;
    }
    if (colon >= body.size()) return std::nullopt;
    return colon;
  }
  return std::nullopt;
}

std::optional<std::string> json_parse_string_literal(const std::string& body, std::size_t start) {
  if (start >= body.size() || body[start] != '"') return std::nullopt;
  std::string out;
  bool escaped = false;
  for (std::size_t i = start + 1; i < body.size(); ++i) {
    const char ch = body[i];
    if (escaped) {
      switch (ch) {
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      default: out.push_back(ch); break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') return out;
    out.push_back(ch);
  }
  return std::nullopt;
}

std::optional<std::string> json_raw_value(const std::string& body, std::size_t start) {
  if (start >= body.size()) return std::nullopt;
  if (body[start] == '"') {
    std::size_t end = start + 1;
    bool escaped = false;
    while (end < body.size()) {
      const char ch = body[end];
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        return body.substr(start, end - start + 1);
      }
      end += 1;
    }
    return std::nullopt;
  }

  std::size_t end = start;
  while (end < body.size()) {
    const char ch = body[end];
    if (ch == ',' || ch == '}' || ch == ']' || std::isspace(static_cast<unsigned char>(ch))) break;
    end += 1;
  }
  if (end == start) return std::nullopt;
  return body.substr(start, end - start);
}

std::optional<std::string> json_id_from_request(const std::string& body) {
  const auto start = json_value_start(body, "id");
  if (!start.has_value()) return std::nullopt;
  return json_raw_value(body, *start);
}

std::optional<std::string> json_method_from_request(const std::string& body) {
  const auto start = json_value_start(body, "method");
  if (!start.has_value()) return std::nullopt;
  return json_parse_string_literal(body, *start);
}

void lsp_write(const std::string& body) {
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  std::cout.flush();
}

std::optional<std::string> json_string_field(const std::string& body,
                                             std::string_view key,
                                             std::size_t from = 0) {
  const auto start = json_value_start(body, key, from);
  if (!start.has_value()) return std::nullopt;
  return json_parse_string_literal(body, *start);
}

[[maybe_unused]] std::optional<int> json_int_field(const std::string& body,
                                                   std::string_view key,
                                                   std::size_t from = 0) {
  const auto start_opt = json_value_start(body, key, from);
  if (!start_opt.has_value()) return std::nullopt;
  std::size_t start = *start_opt;
  std::size_t end = start;
  while (end < body.size() &&
         (std::isdigit(static_cast<unsigned char>(body[end])) || body[end] == '-')) {
    end += 1;
  }
  try {
    return std::stoi(body.substr(start, end - start));
  } catch (...) {
    return std::nullopt;
  }
}

std::string uri_to_path(std::string uri) {
  const std::string prefix = "file://";
  if (uri.rfind(prefix, 0) == 0) uri.erase(0, prefix.size());
  std::string out;
  out.reserve(uri.size());
  for (std::size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size()) {
      const std::string hex = uri.substr(i + 1, 2);
      try {
        const char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
        out.push_back(ch);
        i += 2;
        continue;
      } catch (...) {
      }
    }
    out.push_back(uri[i]);
  }
  return out;
}

std::string path_to_uri(const fs::path& path) {
  std::string raw = fs::absolute(path).lexically_normal().string();
  std::string out = "file://";
  for (char ch : raw) {
    if (ch == ' ') out += "%20";
    else out.push_back(ch);
  }
  return out;
}

struct LspDocumentState {
  std::string uri;
  std::string path;
  std::string text;
};

std::string normalized_path_string(const fs::path& path) {
  return fs::absolute(path).lexically_normal().string();
}

std::vector<nebula::frontend::SourceFile> overlay_open_documents(
    std::vector<nebula::frontend::SourceFile> sources,
    const std::unordered_map<std::string, LspDocumentState>* docs) {
  if (docs == nullptr) return sources;
  for (auto& source : sources) {
    auto it = docs->find(normalized_path_string(source.path));
    if (it != docs->end()) source.text = it->second.text;
  }
  return sources;
}

CompilePipelineResult compile_lsp_document(const std::string& path,
                                          const std::string& text,
                                          const std::unordered_map<std::string, LspDocumentState>* docs =
                                              nullptr) {
  auto loaded = load_compile_input(path, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.ok) {
    CompilePipelineResult out;
    out.diags = loaded.diags;
    return out;
  }

  loaded.compile_sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
  bool replaced = false;
  for (auto& source : loaded.compile_sources) {
    if (normalized_path_string(source.path) == normalized_path_string(path)) {
      source.text = text;
      replaced = true;
      break;
    }
  }

  if (replaced) {
    std::ostringstream cache_seed;
    for (const auto& source : loaded.compile_sources) {
      cache_seed << source.path << "\n" << source.text << "\n---\n";
    }
    loaded.cache_key_source = cache_seed.str();
  }

  CompilePipelineOptions popt;
  popt.mode = BuildMode::Debug;
  popt.profile = AnalysisProfile::Deep;
  popt.analysis_tier = AnalysisTier::Smart;
  popt.include_lint = true;
  popt.source_path = path;
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;
  return run_compile_pipeline(loaded.compile_sources, popt);
}

int lsp_severity_for(const Diagnostic& d) {
  if (d.is_error()) return 1;
  if (d.is_warning()) return 2;
  return 3;
}

[[maybe_unused]] bool lsp_position_matches(const nebula::frontend::Span& span,
                                           int line_zero_based,
                                           int char_zero_based) {
  const int line = line_zero_based + 1;
  const int col = char_zero_based + 1;
  if (line < span.start.line || line > span.end.line) return false;
  if (line == span.start.line && col < span.start.col) return false;
  if (line == span.end.line && col >= span.end.col) return false;
  return true;
}

[[maybe_unused]] std::string lsp_markdown_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    if (ch == '\\' || ch == '`') out.push_back('\\');
    if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

[[maybe_unused]] std::optional<std::string> identifier_at_position(const std::string& path,
                                                                   const std::string& text,
                                                                   int line_zero_based,
                                                                   int char_zero_based) {
  Lexer lex(text, path);
  auto toks = lex.lex_all();
  for (const auto& tok : toks) {
    if (tok.kind != nebula::frontend::TokenKind::Ident) continue;
    if (lsp_position_matches(tok.span, line_zero_based, char_zero_based)) {
      return tok.lexeme;
    }
  }
  return std::nullopt;
}

struct SymbolDefinition {
  std::string name;
  std::string detail;
  std::string package_name;
  std::string module_name;
  nebula::frontend::Span span;
  nebula::frontend::Span visibility;
  bool local = false;
};

void collect_block_symbols(const Block& block, std::vector<SymbolDefinition>& out) {
  for (const auto& stmt : block.stmts) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            out.push_back({node.name, "let " + node.name, {}, {}, stmt.span, block.span, true});
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            out.push_back({node.name, "region " + node.name, {}, {}, stmt.span, node.body.span, true});
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            collect_block_symbols(node.then_body, out);
            if (node.else_body.has_value()) collect_block_symbols(*node.else_body, out);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            out.push_back({node.var, "for " + node.var, {}, {}, stmt.span, node.body.span, true});
            collect_block_symbols(node.body, out);
          }
        },
        stmt.node);
  }
}

[[maybe_unused]] std::vector<SymbolDefinition> collect_symbol_definitions(
    const std::vector<nebula::frontend::SourceFile>& sources) {
  std::vector<SymbolDefinition> out;
  for (const auto& source : sources) {
    try {
      Lexer lex(source.text, source.path);
      auto toks = lex.lex_all();
      Parser parser(std::move(toks));
      auto program = parser.parse_program();
      for (const auto& item : program.items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              if constexpr (std::is_same_v<N, Function>) {
                out.push_back({n.name,
                               (n.is_extern ? "extern fn " : "fn ") + n.name,
                               source.package_name,
                               source.module_name,
                               n.span,
                               n.span,
                               false});
                const nebula::frontend::Span fn_visibility =
                    n.body.has_value() ? n.body->span : n.span;
                for (const auto& param : n.params) {
                  out.push_back({param.name,
                                 "param " + param.name,
                                 source.package_name,
                                 source.module_name,
                                 param.span,
                                 fn_visibility,
                                 true});
                }
                if (n.body.has_value()) collect_block_symbols(*n.body, out);
              } else if constexpr (std::is_same_v<N, Struct>) {
                out.push_back(
                    {n.name, "struct " + n.name, source.package_name, source.module_name, n.span, n.span, false});
              } else {
                out.push_back(
                    {n.name, "enum " + n.name, source.package_name, source.module_name, n.span, n.span, false});
              }
            },
            item.node);
      }
    } catch (...) {
    }
  }
  std::sort(out.begin(), out.end(), [](const SymbolDefinition& lhs, const SymbolDefinition& rhs) {
    if (lhs.name != rhs.name) return lhs.name < rhs.name;
    if (lhs.local != rhs.local) return lhs.local > rhs.local;
    if (lhs.span.source_path != rhs.span.source_path) return lhs.span.source_path < rhs.span.source_path;
    if (lhs.span.start.line != rhs.span.start.line) return lhs.span.start.line < rhs.span.start.line;
    if (lhs.span.start.col != rhs.span.start.col) return lhs.span.start.col < rhs.span.start.col;
    return lhs.detail < rhs.detail;
  });
  return out;
}

std::vector<nebula::frontend::SourceFile> overlay_compile_sources(
    std::vector<nebula::frontend::SourceFile> sources,
    const std::string& path,
    const std::string& text) {
  const std::string requested_path = normalized_path_string(path);
  for (auto& source : sources) {
    if (normalized_path_string(source.path) == requested_path) source.text = text;
  }
  return sources;
}

const nebula::frontend::SourceFile* find_source_by_path(
    const std::vector<nebula::frontend::SourceFile>& sources,
    const std::string& path) {
  const std::string requested = normalized_path_string(path);
  for (const auto& source : sources) {
    if (normalized_path_string(source.path) == requested) return &source;
  }
  return nullptr;
}

const SymbolDefinition* resolve_symbol_definition(const std::vector<SymbolDefinition>& symbols,
                                                  const std::vector<nebula::frontend::SourceFile>& sources,
                                                  const std::string& ident,
                                                  const std::string& path,
                                                  int line_zero_based,
                                                  int char_zero_based) {
  const std::string requested_path = normalized_path_string(path);
  const int line = line_zero_based + 1;
  const int col = char_zero_based + 1;

  const SymbolDefinition* best_local = nullptr;
  for (const auto& symbol : symbols) {
    if (symbol.name != ident || !symbol.local) continue;
    if (fs::absolute(symbol.span.source_path).lexically_normal() != requested_path) continue;
    if (!lsp_position_matches(symbol.visibility, line_zero_based, char_zero_based)) continue;
    if (symbol.span.start.line > line) continue;
    if (symbol.span.start.line == line && symbol.span.start.col > col) continue;
    if (best_local == nullptr || best_local->span.start.line < symbol.span.start.line ||
        (best_local->span.start.line == symbol.span.start.line &&
         best_local->span.start.col < symbol.span.start.col)) {
      best_local = &symbol;
    }
  }
  if (best_local != nullptr) return best_local;

  const auto* current_source = find_source_by_path(sources, path);
  if (current_source != nullptr) {
    const auto is_same_module = [&](const SymbolDefinition& symbol) {
      return !symbol.local && symbol.package_name == current_source->package_name &&
             symbol.module_name == current_source->module_name;
    };
    for (const auto& symbol : symbols) {
      if (symbol.name != ident) continue;
      if (is_same_module(symbol)) return &symbol;
    }

    for (const auto& import_name : current_source->resolved_imports) {
      const std::size_t sep = import_name.find("::");
      if (sep == std::string::npos) continue;
      const std::string package_name = import_name.substr(0, sep);
      const std::string module_name = import_name.substr(sep + 2);
      const SymbolDefinition* imported_match = nullptr;
      for (const auto& symbol : symbols) {
        if (symbol.name != ident || symbol.local) continue;
        if (symbol.package_name != package_name || symbol.module_name != module_name) continue;
        if (imported_match != nullptr) return nullptr;
        imported_match = &symbol;
      }
      if (imported_match != nullptr) return imported_match;
    }
  }

  for (const auto& symbol : symbols) {
    if (symbol.name != ident || symbol.local) continue;
    if (normalized_path_string(symbol.span.source_path) != requested_path) continue;
    return &symbol;
  }

  const SymbolDefinition* unique_global = nullptr;
  for (const auto& symbol : symbols) {
    if (symbol.name != ident || symbol.local) continue;
    if (unique_global != nullptr) return nullptr;
    unique_global = &symbol;
  }
  return unique_global;
}

[[maybe_unused]] std::optional<std::string> definition_result_json(const std::string& path,
                                                                   const std::string& text,
                                                                   int line_zero_based,
                                                                   int char_zero_based,
                                                                   const std::unordered_map<std::string,
                                                                                            LspDocumentState>* docs =
                                                                       nullptr) {
  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (!ident.has_value()) return std::nullopt;

  auto loaded = load_compile_input(path, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.ok) return std::nullopt;
  auto sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
  sources = overlay_compile_sources(std::move(sources), path, text);
  const auto symbols = collect_symbol_definitions(sources);
  const auto* symbol =
      resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based, char_zero_based);
  if (symbol == nullptr) return std::nullopt;
  std::ostringstream out;
  out << "{\"uri\":\"" << json_escape(path_to_uri(symbol->span.source_path))
      << "\",\"range\":{\"start\":{\"line\":" << std::max(0, symbol->span.start.line - 1)
      << ",\"character\":" << std::max(0, symbol->span.start.col - 1)
      << "},\"end\":{\"line\":" << std::max(0, symbol->span.end.line - 1)
      << ",\"character\":" << std::max(0, symbol->span.end.col - 1) << "}}}";
  return out.str();
}

[[maybe_unused]] std::optional<std::string> hover_result_json(const std::string& path,
                                                              const std::string& text,
                                                              int line_zero_based,
                                                              int char_zero_based,
                                                              const std::unordered_map<std::string,
                                                                                       LspDocumentState>* docs =
                                                                  nullptr) {
  auto result = compile_lsp_document(path, text, docs);
  for (const auto& d : result.diags) {
    if (d.span.source_path != path) continue;
    if (!lsp_position_matches(d.span, line_zero_based, char_zero_based)) continue;
    std::ostringstream md;
    md << "**" << lsp_markdown_escape(d.code) << "** " << lsp_markdown_escape(d.message);
    if (!d.cause.empty()) md << "\\n\\nwhy: " << lsp_markdown_escape(d.cause);
    if (!d.impact.empty()) md << "\\nimpact: " << lsp_markdown_escape(d.impact);
    if (!d.machine_owner.empty()) {
      md << "\\nowner: " << lsp_markdown_escape(d.machine_owner);
      if (!d.machine_owner_reason.empty()) {
        md << " (" << lsp_markdown_escape(d.machine_owner_reason) << ")";
      }
    }
    if (!d.suggestions.empty()) md << "\\nfirst-fix: " << lsp_markdown_escape(d.suggestions.front());

    std::ostringstream out;
    out << "{\"contents\":{\"kind\":\"markdown\",\"value\":\"" << md.str() << "\"}}";
    return out.str();
  }
  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (ident.has_value()) {
    auto loaded = load_compile_input(path, nebula::frontend::DiagnosticStage::Build);
    if (loaded.ok) {
      auto sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
      sources = overlay_compile_sources(std::move(sources), path, text);
      const auto symbols = collect_symbol_definitions(sources);
      if (const auto* symbol =
              resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based,
                                        char_zero_based);
          symbol != nullptr) {
        std::ostringstream out;
        out << "{\"contents\":{\"kind\":\"markdown\",\"value\":\""
            << json_escape(lsp_markdown_escape(symbol->detail)) << "\"}}";
        return out.str();
      }
    }
  }
  return std::nullopt;
}

void lsp_publish_diagnostics(const std::string& uri,
                             const std::string& path,
                             const std::string& text,
                             const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto result = compile_lsp_document(path, text, docs);
  std::ostringstream out;
  out << "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
      << "\"uri\":\"" << json_escape(uri) << "\",\"diagnostics\":[";
  bool first = true;
  for (const auto& d : result.diags) {
    if (!d.span.source_path.empty() && d.span.source_path != path) continue;
    if (!first) out << ",";
    first = false;
    out << "{"
        << "\"range\":{\"start\":{\"line\":" << std::max(0, d.span.start.line - 1)
        << ",\"character\":" << std::max(0, d.span.start.col - 1)
        << "},\"end\":{\"line\":" << std::max(0, d.span.end.line - 1)
        << ",\"character\":" << std::max(0, d.span.end.col - 1) << "}},"
        << "\"severity\":" << lsp_severity_for(d) << ","
        << "\"code\":\"" << json_escape(d.code) << "\","
        << "\"source\":\"nebula\","
        << "\"message\":\"" << json_escape(d.message) << "\"}";
  }
  out << "]}}";
  lsp_write(out.str());
}

} // namespace

int cmd_new(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula new <path>\n";
    return 2;
  }
  const fs::path root = fs::absolute(args[2]);
  const std::string name = root.filename().string().empty() ? "nebula-app" : root.filename().string();
  const fs::path src_dir = root / "src";
  fs::create_directories(src_dir);

  PackageManifest manifest_cfg;
  manifest_cfg.name = name;
  manifest_cfg.version = "0.1.0";
  manifest_cfg.entry = "src/main.nb";
  manifest_cfg.src_dir = "src";
  const std::string manifest = render_manifest(manifest_cfg);
  const std::string main_src =
      "module main\n\n"
      "fn main() -> Void {\n"
      "  print(\"Hello from Nebula\")\n"
      "}\n";

  if (!write_text_file(root / "nebula.toml", manifest) || !write_text_file(src_dir / "main.nb", main_src)) {
    std::cerr << "error: failed to scaffold project under " << root.string() << "\n";
    return 1;
  }

  std::cout << "created: " << root.string() << "\n";
  return 0;
}

int cmd_add(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 5) {
    std::cerr << "error: usage: nebula add <project-or-manifest> <name> <version>\n"
                 "       nebula add <project-or-manifest> <name> --path <dir>\n"
                 "       nebula add <project-or-manifest> <name> --git <url> --rev <rev>\n";
    return 2;
  }

  const auto manifest_path = manifest_path_from_input(args[2]);
  if (!manifest_path.has_value()) {
    std::cerr << "error: manifest not found for " << fs::absolute(args[2]).string() << "\n";
    return 1;
  }

  std::vector<Diagnostic> diags;
  PackageManifest manifest;
  if (!read_package_manifest(*manifest_path, manifest, diags, nebula::frontend::DiagnosticStage::Build)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }
  if (!manifest.has_explicit_package && !manifest.workspace_members.empty()) {
    std::cerr << "error: cannot add dependencies to a workspace-only manifest; target a workspace member package instead\n";
    return 1;
  }

  ManifestDependency dep;
  dep.alias = args[3];
  if (args[4] == "--path") {
    if (args.size() < 6) {
      std::cerr << "error: missing value for --path\n";
      return 2;
    }
    dep.kind = ManifestDependencyKind::Path;
    dep.path = args[5];
  } else if (args[4] == "--git") {
    if (args.size() < 8 || args[6] != "--rev") {
      std::cerr << "error: usage: nebula add <project> <name> --git <url> --rev <rev>\n";
      return 2;
    }
    dep.kind = ManifestDependencyKind::Git;
    dep.git = args[5];
    dep.rev = args[7];
  } else {
    dep.kind = ManifestDependencyKind::Version;
    dep.version = args[4];
  }

  auto it = std::find_if(manifest.dependencies.begin(), manifest.dependencies.end(),
                         [&](const ManifestDependency& existing) { return existing.alias == dep.alias; });
  if (it == manifest.dependencies.end()) {
    manifest.dependencies.push_back(std::move(dep));
  } else {
    *it = std::move(dep);
  }

  if (!write_text_file(*manifest_path, render_manifest(manifest))) {
    std::cerr << "error: failed to write manifest " << manifest_path->string() << "\n";
    return 1;
  }

  std::cout << "updated: " << manifest_path->string() << "\n";
  return 0;
}

int cmd_fetch(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula fetch <project-or-manifest>\n";
    return 2;
  }

  std::vector<Diagnostic> diags;
  ProjectLock lock;
  if (!resolve_project_lock(fs::absolute(args[2]), lock, diags, nebula::frontend::DiagnosticStage::Build,
                            false)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }

  const auto root_it = std::find_if(lock.packages.begin(), lock.packages.end(),
                                    [](const LockedPackage& pkg) { return pkg.is_root; });
  if (root_it == lock.packages.end()) {
    std::cerr << "error: resolved dependency graph has no root package\n";
    return 1;
  }
  const fs::path lock_path = lockfile_path_for_manifest(root_it->manifest_path);
  if (!write_project_lock(lock_path, lock, diags, nebula::frontend::DiagnosticStage::Build)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }
  std::cout << "wrote: " << lock_path.string() << "\n";
  return 0;
}

int cmd_update(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula update <project-or-manifest>\n";
    return 2;
  }

  std::vector<Diagnostic> diags;
  ProjectLock lock;
  if (!resolve_project_lock(fs::absolute(args[2]), lock, diags, nebula::frontend::DiagnosticStage::Build,
                            true)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }

  const auto root_it = std::find_if(lock.packages.begin(), lock.packages.end(),
                                    [](const LockedPackage& pkg) { return pkg.is_root; });
  if (root_it == lock.packages.end()) {
    std::cerr << "error: resolved dependency graph has no root package\n";
    return 1;
  }
  const fs::path lock_path = lockfile_path_for_manifest(root_it->manifest_path);
  if (!write_project_lock(lock_path, lock, diags, nebula::frontend::DiagnosticStage::Build)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }
  std::cout << "updated lock: " << lock_path.string() << "\n";
  return 0;
}

int cmd_publish(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula publish <project-or-manifest> [--force]\n";
    return 2;
  }

  bool force = false;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--force") {
      force = true;
      continue;
    }
    std::cerr << "error: unknown option: " << args[i] << "\n";
    return 2;
  }

  std::vector<Diagnostic> diags;
  PublishPackageResult result;
  if (!publish_package_to_local_registry(fs::absolute(args[2]), result, diags,
                                         nebula::frontend::DiagnosticStage::Build, force)) {
    for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
    return 1;
  }

  if (result.unchanged) {
    std::cout << "already published: " << result.package_name << "@" << result.package_version << " -> "
              << result.published_root.string() << "\n";
  } else if (result.replaced) {
    std::cout << "replaced: " << result.package_name << "@" << result.package_version << " -> "
              << result.published_root.string() << "\n";
  } else {
    std::cout << "published: " << result.package_name << "@" << result.package_version << " -> "
              << result.published_root.string() << "\n";
  }
  return 0;
}

int cmd_fmt(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula fmt <file-or-dir>\n";
    return 2;
  }

  const fs::path input = fs::absolute(args[2]);
  const auto targets = collect_format_targets(input);
  if (targets.empty()) {
    std::cerr << "error: no .nb files found at " << input.string() << "\n";
    return 1;
  }

  int failed = 0;
  for (const auto& file : targets) {
    Program program;
    std::vector<Diagnostic> diags;
    if (!parse_program_file(file, program, diags)) {
      for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
      failed = 1;
      continue;
    }
    std::string original;
    if (!read_source(file, original, diags, nebula::frontend::DiagnosticStage::Build)) {
      for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
      std::cerr << "error: failed to read " << file.string() << "\n";
      failed = 1;
      continue;
    }
    const std::string formatted = format_program_preserving_comments(original, file.string(), program);
    if (!write_text_file(file, formatted)) {
      std::cerr << "error: failed to write " << file.string() << "\n";
      failed = 1;
      continue;
    }
    std::cout << "formatted: " << file.string() << "\n";
  }

  return failed ? 1 : 0;
}

int cmd_explain(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula explain <path> [--file PATH] [--line N] [--col N] [--symbol NAME] [--format text|json]\n";
    return 2;
  }

  fs::path input = fs::absolute(args[2]);
  DiagFormat format = DiagFormat::Text;
  int line = 0;
  int col = 0;
  std::string symbol;
  std::string file_filter;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "--format") {
      if (i + 1 >= args.size()) return 2;
      const std::string value = args[++i];
      if (value == "json") format = DiagFormat::Json;
      else if (value == "text") format = DiagFormat::Text;
      else return 2;
    } else if (args[i] == "--file") {
      if (i + 1 >= args.size()) return 2;
      file_filter = args[++i];
    } else if (args[i] == "--line") {
      if (!parse_int_flag(args, i, line)) return 2;
    } else if (args[i] == "--col") {
      if (!parse_int_flag(args, i, col)) return 2;
    } else if (args[i] == "--symbol") {
      if (i + 1 >= args.size()) return 2;
      symbol = args[++i];
    } else {
      std::cerr << "error: unknown option: " << args[i] << "\n";
      return 2;
    }
  }

  if (col > 0 && line == 0) {
    std::cerr << "error: --col requires --line\n";
    return 2;
  }

  auto loaded = load_compile_input(input, nebula::frontend::DiagnosticStage::Build);
  if (!loaded.diags.empty()) emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) return 1;

  const bool input_is_file = fs::is_regular_file(input);
  const std::string normalized_input_file =
      input_is_file ? fs::absolute(input).lexically_normal().string() : std::string{};
  const std::string normalized_file_filter = file_filter.empty()
                                                 ? std::string{}
                                                 : fs::absolute(file_filter).lexically_normal().string();
  std::string effective_file_filter = normalized_file_filter;
  if (!symbol.empty() && effective_file_filter.empty()) {
    if (normalized_input_file.empty()) {
      std::cerr << "error: --symbol requires --file when explaining a project or manifest root\n";
      return 2;
    }
    effective_file_filter = normalized_input_file;
  }

  CompilePipelineOptions popt;
  popt.mode = opt.mode;
  popt.profile = resolve_profile(opt.mode, opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Deep);
  popt.analysis_tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  popt.strict_region = opt.strict_region;
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.include_lint = true;
  popt.source_path = input.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  auto result = run_compile_pipeline(loaded.compile_sources, popt);
  std::vector<Diagnostic> selected;
  for (const auto& d : result.diags) {
    if (!effective_file_filter.empty()) {
      const fs::path diag_path = d.span.source_path.empty() ? fs::path{} : fs::absolute(d.span.source_path);
      if (diag_path.lexically_normal().string() != effective_file_filter) continue;
    }
    if (line > 0) {
      const bool line_matches = d.span.start.line <= line && d.span.end.line >= line;
      if (!line_matches) continue;
    }
    if (col > 0) {
      const bool same_start_line = (line == 0 || d.span.start.line == line);
      const bool same_end_line = (line == 0 || d.span.end.line == line);
      if (same_start_line && d.span.start.col > col) continue;
      if (same_end_line && d.span.end.col < col) continue;
    }
    selected.push_back(d);
  }
  if (line == 0 && col == 0 && effective_file_filter.empty() && symbol.empty()) selected = result.diags;

  struct ExplainVar {
    std::string fn_name;
    std::string var_name;
    std::string rep;
    std::string owner;
    std::string region;
  };
  std::vector<ExplainVar> vars;
  if (result.nir_prog && result.rep_owner) {
    for (const auto& item : result.nir_prog->items) {
      if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
      const auto& fn = std::get<nebula::nir::Function>(item.node);
      auto fit = result.rep_owner->by_function.find(nebula::nir::function_identity(fn));
      if (fit == result.rep_owner->by_function.end()) continue;
      const auto names = collect_nir_var_names(fn);
      for (const auto& [var_id, dec] : fit->second.vars) {
        auto nit = names.find(var_id);
        if (nit == names.end()) continue;
        if (!symbol.empty() && nit->second != symbol) continue;
        vars.push_back({nebula::nir::function_identity(fn),
                        nit->second,
                        dec.rep == nebula::passes::RepKind::Stack
                            ? "Stack"
                            : (dec.rep == nebula::passes::RepKind::Region ? "Region" : "Heap"),
                        dec.owner == nebula::passes::OwnerKind::Unique
                            ? "Unique"
                            : (dec.owner == nebula::passes::OwnerKind::Shared ? "Shared" : "None"),
                        dec.region});
      }
    }
    std::sort(vars.begin(), vars.end(), [](const ExplainVar& a, const ExplainVar& b) {
      if (a.fn_name != b.fn_name) return a.fn_name < b.fn_name;
      return a.var_name < b.var_name;
    });
  }

  struct ExplainSymbolMatch {
    std::string name;
    std::string detail;
    std::string path;
    int line = 0;
    int col = 0;
    int end_line = 0;
    int end_col = 0;
    bool local = false;
  };
  std::vector<ExplainSymbolMatch> symbol_matches;
  if (!symbol.empty()) {
    const auto definitions = collect_symbol_definitions(loaded.compile_sources);
    for (const auto& def : definitions) {
      if (def.name != symbol) continue;
      const std::string def_path = fs::absolute(def.span.source_path).lexically_normal().string();
      if (!effective_file_filter.empty() && def_path != effective_file_filter) continue;
      symbol_matches.push_back(
          {def.name, def.detail, def_path, def.span.start.line, def.span.start.col, def.span.end.line,
           def.span.end.col, def.local});
    }
    std::sort(symbol_matches.begin(), symbol_matches.end(), [](const ExplainSymbolMatch& lhs,
                                                               const ExplainSymbolMatch& rhs) {
      if (lhs.path != rhs.path) return lhs.path < rhs.path;
      if (lhs.line != rhs.line) return lhs.line < rhs.line;
      if (lhs.col != rhs.col) return lhs.col < rhs.col;
      return lhs.detail < rhs.detail;
    });
  }

  const std::string query_kind =
      !symbol.empty() ? "symbol" : ((line > 0 || !effective_file_filter.empty()) ? "span" : "all");
  const bool inferred_result = !symbol.empty() || !vars.empty();

  auto emit_diag_json = [&](std::ostringstream& out_json, const Diagnostic& d) {
    out_json << "{";
    out_json << "\"code\":\"" << json_escape(d.code) << "\",";
    out_json << "\"message\":\"" << json_escape(d.message) << "\",";
    out_json << "\"severity\":\"" << nebula::frontend::severity_name(d.severity) << "\",";
    out_json << "\"path\":\"" << json_escape(d.span.source_path) << "\",";
    out_json << "\"line\":" << d.span.start.line << ",";
    out_json << "\"col\":" << d.span.start.col << ",";
    out_json << "\"end_line\":" << d.span.end.line << ",";
    out_json << "\"end_col\":" << d.span.end.col << ",";
    out_json << "\"cause\":\"" << json_escape(d.cause) << "\",";
    out_json << "\"impact\":\"" << json_escape(d.impact) << "\",";
    out_json << "\"machine_reason\":\"" << json_escape(d.machine_reason) << "\",";
    out_json << "\"machine_subreason\":\"" << json_escape(d.machine_subreason) << "\",";
    out_json << "\"machine_detail\":\"" << json_escape(d.machine_detail) << "\",";
    out_json << "\"machine_trigger_family\":\"" << json_escape(d.machine_trigger_family) << "\",";
    out_json << "\"machine_trigger_family_detail\":\""
             << json_escape(d.machine_trigger_family_detail) << "\",";
    out_json << "\"machine_trigger_subreason\":\"" << json_escape(d.machine_trigger_subreason) << "\",";
    out_json << "\"machine_owner\":\"" << json_escape(d.machine_owner) << "\",";
    out_json << "\"machine_owner_reason\":\"" << json_escape(d.machine_owner_reason) << "\",";
    out_json << "\"machine_owner_reason_detail\":\""
             << json_escape(d.machine_owner_reason_detail) << "\",";
    out_json << "\"caused_by_code\":\"" << json_escape(d.caused_by_code) << "\",";
    out_json << "\"first_fix\":\""
             << json_escape(d.suggestions.empty() ? std::string{} : d.suggestions.front()) << "\",";
    out_json << "\"inferred\":" << ((d.predictive || !d.machine_reason.empty() || !d.machine_owner.empty())
                                         ? "true"
                                         : "false")
             << ",";
    out_json << "\"span\":{\"path\":\"" << json_escape(d.span.source_path) << "\",\"start\":{\"line\":"
             << d.span.start.line << ",\"col\":" << d.span.start.col << "},\"end\":{\"line\":"
             << d.span.end.line << ",\"col\":" << d.span.end.col << "}},";
    out_json << "\"root_cause\":{\"cause\":\"" << json_escape(d.cause) << "\",\"impact\":\""
             << json_escape(d.impact) << "\"},";
    out_json << "\"trigger\":{\"reason\":\"" << json_escape(d.machine_reason)
             << "\",\"subreason\":\"" << json_escape(d.machine_subreason) << "\",\"detail\":\""
             << json_escape(d.machine_detail) << "\",\"family\":\""
             << json_escape(d.machine_trigger_family) << "\",\"family_detail\":\""
             << json_escape(d.machine_trigger_family_detail) << "\",\"trigger_subreason\":\""
             << json_escape(d.machine_trigger_subreason) << "\"},";
    out_json << "\"owner\":{\"kind\":\"" << json_escape(d.machine_owner) << "\",\"reason\":\""
             << json_escape(d.machine_owner_reason) << "\",\"reason_detail\":\""
             << json_escape(d.machine_owner_reason_detail) << "\"}";
    out_json << "}";
  };

  if (format == DiagFormat::Json) {
    std::ostringstream out_json;
    out_json << "{";
    out_json << "\"schema_version\":1,";
    out_json << "\"path\":\"" << json_escape(input.string()) << "\",";
    out_json << "\"query\":{";
    out_json << "\"kind\":\"" << query_kind << "\",";
    out_json << "\"path\":\"" << json_escape(input.string()) << "\",";
    out_json << "\"file\":\"" << json_escape(effective_file_filter) << "\",";
    out_json << "\"line\":" << line << ",";
    out_json << "\"col\":" << col << ",";
    out_json << "\"symbol\":\"" << json_escape(symbol) << "\"";
    out_json << "},";
    out_json << "\"symbol_matches\":[";
    for (std::size_t i = 0; i < symbol_matches.size(); ++i) {
      const auto& match = symbol_matches[i];
      if (i) out_json << ",";
      out_json << "{"
               << "\"name\":\"" << json_escape(match.name) << "\","
               << "\"detail\":\"" << json_escape(match.detail) << "\","
               << "\"path\":\"" << json_escape(match.path) << "\","
               << "\"line\":" << match.line << ","
               << "\"col\":" << match.col << ","
               << "\"end_line\":" << match.end_line << ","
               << "\"end_col\":" << match.end_col << ","
               << "\"local\":" << (match.local ? "true" : "false")
               << "}";
    }
    out_json << "],";
    out_json << "\"variables\":[";
    for (std::size_t i = 0; i < vars.size(); ++i) {
      const auto& v = vars[i];
      if (i) out_json << ",";
      out_json << "{"
               << "\"function\":\"" << json_escape(v.fn_name) << "\","
               << "\"name\":\"" << json_escape(v.var_name) << "\","
               << "\"rep\":\"" << json_escape(v.rep) << "\","
               << "\"owner\":\"" << json_escape(v.owner) << "\","
               << "\"region\":\"" << json_escape(v.region) << "\""
               << "}";
    }
    out_json << "],";
    out_json << "\"matches\":[";
    for (std::size_t i = 0; i < selected.size(); ++i) {
      if (i) out_json << ",";
      emit_diag_json(out_json, selected[i]);
    }
    out_json << "],";
    out_json << "\"diagnostics\":[";
    for (std::size_t i = 0; i < selected.size(); ++i) {
      if (i) out_json << ",";
      emit_diag_json(out_json, selected[i]);
    }
    out_json << "],";
    out_json << "\"inferred\":" << (inferred_result ? "true" : "false");
    out_json << "]}";
    std::cout << out_json.str() << "\n";
  } else {
    std::cout << "path: " << input.string() << "\n";
    std::cout << "query: kind=" << query_kind;
    if (!effective_file_filter.empty()) std::cout << " file=" << effective_file_filter;
    if (line > 0) std::cout << " line=" << line;
    if (col > 0) std::cout << " col=" << col;
    if (!symbol.empty()) std::cout << " symbol=" << symbol;
    std::cout << "\n";
    for (const auto& match : symbol_matches) {
      std::cout << "symbol-match: " << match.detail << " @"
                << match.path << ":" << match.line << ":" << match.col;
      if (match.local) std::cout << " local=true";
      std::cout << "\n";
    }
    for (const auto& v : vars) {
      std::cout << "var: fn=" << v.fn_name << " name=" << v.var_name << " rep=" << v.rep
                << " owner=" << v.owner;
      if (!v.region.empty()) std::cout << " region=" << v.region;
      std::cout << "\n";
    }
    for (const auto& d : selected) {
      std::cout << d.code << " @";
      if (!d.span.source_path.empty()) std::cout << d.span.source_path << ":";
      std::cout << d.span.start.line << ":" << d.span.start.col << "\n";
      if (!d.message.empty()) std::cout << "  what: " << d.message << "\n";
      if (!d.cause.empty()) std::cout << "  why: " << d.cause << "\n";
      if (!d.impact.empty()) std::cout << "  impact: " << d.impact << "\n";
      if (!d.machine_reason.empty() || !d.machine_subreason.empty()) {
        std::cout << "  trigger: " << d.machine_reason;
        if (!d.machine_subreason.empty()) std::cout << "/" << d.machine_subreason;
        if (!d.machine_detail.empty()) std::cout << " detail=" << d.machine_detail;
        std::cout << "\n";
      }
      if (!d.machine_owner.empty() || !d.machine_owner_reason.empty()) {
        std::cout << "  owner: " << d.machine_owner;
        if (!d.machine_owner_reason.empty()) std::cout << " (" << d.machine_owner_reason << ")";
        if (!d.machine_owner_reason_detail.empty()) {
          std::cout << " detail=" << d.machine_owner_reason_detail;
        }
        std::cout << "\n";
      }
      if (!d.suggestions.empty()) std::cout << "  first-fix: " << d.suggestions.front() << "\n";
    }
  }

  return 0;
}

int cmd_lsp(const std::vector<std::string>& args, const CliOptions& /*opt*/) {
  std::unordered_map<std::string, LspDocumentState> docs;
  auto handle_body = [&](const std::string& body) -> bool {
    const auto id = json_id_from_request(body);
    const std::string method = json_method_from_request(body).value_or("");
    if (method == "initialize") {
      const std::string response =
          "{\"jsonrpc\":\"2.0\",\"id\":" + id.value_or("0") +
          ",\"result\":{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,"
          "\"definitionProvider\":true}}}";
      lsp_write(response);
    } else if (method == "textDocument/didOpen") {
      const auto uri = json_string_field(body, "uri");
      const auto text = json_string_field(body, "text");
      if (uri.has_value() && text.has_value()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        docs[path] = LspDocumentState{*uri, path, *text};
        lsp_publish_diagnostics(*uri, path, *text, &docs);
      }
    } else if (method == "textDocument/didChange") {
      const auto uri = json_string_field(body, "uri");
      const auto text = json_string_field(body, "text", body.find("\"contentChanges\""));
      if (uri.has_value() && text.has_value()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        docs[path] = LspDocumentState{*uri, path, *text};
        lsp_publish_diagnostics(*uri, path, *text, &docs);
      }
    } else if (method == "textDocument/didClose") {
      const auto uri = json_string_field(body, "uri");
      if (uri.has_value()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        docs.erase(path);
        lsp_write("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
                  "\"uri\":\"" + json_escape(*uri) + "\",\"diagnostics\":[]}}");
      }
    } else if (method == "shutdown") {
      lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + id.value_or("0") + ",\"result\":null}");
    } else if (method == "exit") {
      return true;
    } else if (method == "textDocument/hover") {
      if (id.has_value()) {
        const auto uri = json_string_field(body, "uri");
        const std::size_t pos_pos = body.find("\"position\"");
        const auto line = json_int_field(body, "line", pos_pos);
        const auto character = json_int_field(body, "character", pos_pos);
        if (uri.has_value() && line.has_value() && character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = hover_result_json(path, text, *line, *character, &docs);
                result.has_value()) {
              lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + *id + ",\"result\":" + *result + "}");
              return false;
            }
          }
        }
        lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + *id + ",\"result\":null}");
      }
    } else if (method == "textDocument/definition") {
      if (id.has_value()) {
        const auto uri = json_string_field(body, "uri");
        const std::size_t pos_pos = body.find("\"position\"");
        const auto line = json_int_field(body, "line", pos_pos);
        const auto character = json_int_field(body, "character", pos_pos);
        if (uri.has_value() && line.has_value() && character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = definition_result_json(path, text, *line, *character, &docs);
                result.has_value()) {
              lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + *id + ",\"result\":" + *result + "}");
              return false;
            }
          }
        }
        lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + *id + ",\"result\":null}");
      }
    } else if (id.has_value()) {
      lsp_write("{\"jsonrpc\":\"2.0\",\"id\":" + *id +
                ",\"error\":{\"code\":-32601,\"message\":\"method not implemented\"}}");
    }
    return false;
  };

  auto process_stream = [&](std::istream& in) -> int {
    std::string header_line;
    while (true) {
      std::size_t content_length = 0;
      bool saw_header = false;
      while (std::getline(in, header_line)) {
        saw_header = true;
        if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
        if (header_line.empty()) break;
        if (header_line.rfind("Content-Length:", 0) == 0) {
          try {
            content_length = static_cast<std::size_t>(std::stoul(trim(header_line.substr(15))));
          } catch (...) {
            content_length = 0;
          }
        }
      }
      if (!saw_header) break;
      if (content_length == 0) {
        if (!in) break;
        continue;
      }

      std::string body(content_length, '\0');
      in.read(body.data(), static_cast<std::streamsize>(content_length));
      if (in.gcount() != static_cast<std::streamsize>(content_length)) break;
      if (handle_body(body)) break;
    }
    return 0;
  };

  if (args.size() >= 3) {
    std::ifstream in(args[2], std::ios::binary);
    if (!in) {
      std::cerr << "error: cannot open lsp payload file: " << args[2] << "\n";
      return 1;
    }
    return process_stream(in);
  }
  return process_stream(std::cin);
}
