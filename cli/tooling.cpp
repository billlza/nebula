#include "cli_shared.hpp"
#include "project.hpp"

#include "json.hpp"
#include "frontend/errors.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"
#include "nir/ir.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/wait.h>

namespace {

using nebula::frontend::Block;
using nebula::frontend::Diagnostic;
using nebula::frontend::Enum;
using nebula::frontend::Expr;
using nebula::frontend::Function;
using nebula::frontend::Item;
using nebula::frontend::Lexer;
using nebula::frontend::Parser;
using nebula::frontend::Pattern;
using nebula::frontend::Program;
using nebula::frontend::QualifiedName;
using nebula::frontend::Severity;
using nebula::frontend::SourceFile;
using nebula::frontend::Stmt;
using nebula::frontend::StructBindingField;
using nebula::frontend::Struct;
using nebula::frontend::TBlock;
using nebula::frontend::TExpr;
using nebula::frontend::TExprPtr;
using nebula::frontend::TFunction;
using nebula::frontend::TItem;
using nebula::frontend::TProgram;
using nebula::frontend::TStmt;
using nebula::frontend::Type;
using nebula::frontend::Ui;
using nebula::frontend::UiNode;
using nebula::frontend::UiProp;
using nebula::frontend::Variant;
namespace cli_json = nebula::cli::json;

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
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        constexpr char digits[] = "0123456789abcdef";
        const unsigned char value = static_cast<unsigned char>(c);
        out += "\\u00";
        out.push_back(digits[(value >> 4) & 0x0F]);
        out.push_back(digits[value & 0x0F]);
      } else {
        out.push_back(c);
      }
      break;
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

struct HostedRegistryPreviewOptions {
  std::string server = std::getenv("NEBULA_REGISTRY_URL") != nullptr
                           ? std::getenv("NEBULA_REGISTRY_URL")
                           : "";
  std::string token = std::getenv("NEBULA_REGISTRY_TOKEN") != nullptr
                          ? std::getenv("NEBULA_REGISTRY_TOKEN")
                          : "";
  std::string timeout_seconds = std::getenv("NEBULA_REGISTRY_TIMEOUT_SECONDS") != nullptr
                                    ? std::getenv("NEBULA_REGISTRY_TIMEOUT_SECONDS")
                                    : "";
  std::string registry_root;
};

bool hosted_registry_enabled(const HostedRegistryPreviewOptions& options) {
  return !options.server.empty();
}

bool parse_positive_number_string(const std::string& value) {
  if (value.empty()) return false;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  return end != value.c_str() && end != nullptr && *end == '\0' && parsed > 0.0;
}

bool parse_hosted_registry_options(const std::vector<std::string>& args,
                                   std::size_t start_index,
                                   HostedRegistryPreviewOptions& out,
                                   bool allow_registry_root,
                                   bool& force,
                                   std::string& error) {
  for (std::size_t i = start_index; i < args.size(); ++i) {
    if (args[i] == "--force") {
      force = true;
      continue;
    }
    if (args[i] == "--registry-url") {
      if (i + 1 >= args.size()) {
        error = "missing value for --registry-url";
        return false;
      }
      out.server = args[++i];
      continue;
    }
    if (args[i] == "--registry-token") {
      if (i + 1 >= args.size()) {
        error = "missing value for --registry-token";
        return false;
      }
      out.token = args[++i];
      continue;
    }
    if (args[i] == "--registry-timeout-seconds") {
      if (i + 1 >= args.size()) {
        error = "missing value for --registry-timeout-seconds";
        return false;
      }
      out.timeout_seconds = args[++i];
      if (!parse_positive_number_string(out.timeout_seconds)) {
        error = "--registry-timeout-seconds must be > 0";
        return false;
      }
      continue;
    }
    if (args[i] == "--registry-root") {
      if (!allow_registry_root) {
        error = "unknown option: --registry-root";
        return false;
      }
      if (i + 1 >= args.size()) {
        error = "missing value for --registry-root";
        return false;
      }
      out.registry_root = args[++i];
      continue;
    }
    error = "unknown option: " + args[i];
    return false;
  }
  return true;
}

std::optional<fs::path> hosted_registry_client_script(const CliOptions& opt) {
  if (opt.repo_root.empty()) return std::nullopt;
  for (const fs::path& candidate : {
           opt.repo_root / "share" / "nebula" / "registry" / "client.py",
           opt.repo_root / "tooling" / "registry" / "client.py",
       }) {
    if (fs::exists(candidate)) return candidate;
  }
  return std::nullopt;
}

std::string hosted_registry_python() {
  const char* python = std::getenv("PYTHON");
  if (python != nullptr && *python != '\0') return python;
#if defined(_WIN32)
  return "py";
#else
  return "python3";
#endif
}

int normalize_hosted_registry_wait_status(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

int run_hosted_registry_quiet_command(const std::vector<std::string>& args) {
  if (args.empty()) return 1;

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) return 1;

  if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    return 1;
  }
  return normalize_hosted_registry_wait_status(status);
}

bool hosted_registry_command_looks_like_path(std::string_view command) {
  return command.find('/') != std::string_view::npos || command.find('\\') != std::string_view::npos;
}

std::optional<fs::path> hosted_registry_python_executable(std::string& error) {
  const std::string python = hosted_registry_python();
  const fs::path candidate(python);
  const bool explicit_path = candidate.has_parent_path() || hosted_registry_command_looks_like_path(python);
  if (python.find_first_of(" \t\r\n") != std::string::npos && !explicit_path) {
    error = "hosted registry PYTHON setting must name a single executable: " + python;
    return std::nullopt;
  }

  const auto resolved = find_executable_on_path(python);
  if (!resolved.has_value()) {
    if (explicit_path) {
      error = "hosted registry configured Python interpreter is missing or not executable: " + python;
    } else {
      error = "hosted registry --registry-url workflows require Python 3.11+ on PATH, or set PYTHON to a compatible interpreter";
    }
    return std::nullopt;
  }

  const int rc = run_hosted_registry_quiet_command(
      {resolved->string(),
       "-c",
       "import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)"});
  if (rc != 0) {
    error = "hosted registry --registry-url workflows require Python 3.11+; current interpreter is too old or incompatible: "
            + resolved->string();
    return std::nullopt;
  }
  return resolved;
}

std::optional<fs::path> validate_hosted_registry_client_script(const CliOptions& opt) {
  const auto client = hosted_registry_client_script(opt);
  if (client.has_value()) return client;
  std::cerr
      << "error: hosted registry support requires a bundled registry helper or a Nebula repo checkout; "
         "expected share/nebula/registry/client.py in an install prefix, "
         "tooling/registry/client.py in a release archive layout, or "
         "tooling/registry/client.py in a repo checkout\n";
  return std::nullopt;
}

int run_hosted_registry_fetch_preview(const CliOptions& opt,
                                      const HostedRegistryPreviewOptions& options,
                                      const fs::path& project) {
  const auto client = validate_hosted_registry_client_script(opt);
  if (!client.has_value()) {
    return 1;
  }
  std::string python_error;
  const auto python = hosted_registry_python_executable(python_error);
  if (!python.has_value()) {
    std::cerr << "error: " << python_error << "\n";
    return 1;
  }

  std::vector<std::string> cmd = {python->string(),
                                  client->string(),
                                  "--server",
                                  options.server,
                                  "--nebula-binary",
                                  opt.self_executable.empty() ? "nebula" : opt.self_executable.string()};
  if (!options.token.empty()) {
    cmd.push_back("--token");
    cmd.push_back(options.token);
  }
  if (!options.timeout_seconds.empty()) {
    cmd.push_back("--timeout-seconds");
    cmd.push_back(options.timeout_seconds);
  }
  cmd.push_back("fetch");
  cmd.push_back(project.string());
  if (!options.registry_root.empty()) {
    cmd.push_back("--registry-root");
    cmd.push_back(options.registry_root);
  }
  return run_command(cmd);
}

int run_hosted_registry_publish_preview(const CliOptions& opt,
                                        const HostedRegistryPreviewOptions& options,
                                        const fs::path& project,
                                        bool force) {
  const auto client = validate_hosted_registry_client_script(opt);
  if (!client.has_value()) {
    return 1;
  }
  std::string python_error;
  const auto python = hosted_registry_python_executable(python_error);
  if (!python.has_value()) {
    std::cerr << "error: " << python_error << "\n";
    return 1;
  }

  std::vector<std::string> cmd = {python->string(),
                                  client->string(),
                                  "--server",
                                  options.server,
                                  "--nebula-binary",
                                  opt.self_executable.empty() ? "nebula" : opt.self_executable.string()};
  if (!options.token.empty()) {
    cmd.push_back("--token");
    cmd.push_back(options.token);
  }
  if (!options.timeout_seconds.empty()) {
    cmd.push_back("--timeout-seconds");
    cmd.push_back(options.timeout_seconds);
  }
  cmd.push_back("push");
  cmd.push_back(project.string());
  if (force) cmd.push_back("--force");
  return run_command(cmd);
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

std::string render_inline_string_map(const std::vector<std::pair<std::string, std::string>>& values) {
  std::vector<std::pair<std::string, std::string>> ordered = values;
  std::sort(ordered.begin(), ordered.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) return lhs.first < rhs.first;
    return lhs.second < rhs.second;
  });
  std::ostringstream out;
  out << "{ ";
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (i) out << ", ";
    out << ordered[i].first << " = \"" << json_escape(ordered[i].second) << "\"";
  }
  out << " }";
  return out.str();
}

std::string render_native_language(NativeSourceLanguage language) {
  switch (language) {
  case NativeSourceLanguage::C: return "c";
  case NativeSourceLanguage::Cxx: return "cxx";
  case NativeSourceLanguage::Asm: return "asm";
  }
  return "cxx";
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
  if (!manifest.native.empty()) {
    out << "\n[native]\n";
    if (!manifest.native.c_sources.empty()) {
      std::vector<std::string> items;
      for (const auto& item : manifest.native.c_sources) items.push_back(item.lexically_normal().string());
      std::sort(items.begin(), items.end());
      out << "c_sources = " << render_string_list(items) << "\n";
    }
    if (!manifest.native.cxx_sources.empty()) {
      std::vector<std::string> items;
      for (const auto& item : manifest.native.cxx_sources) {
        items.push_back(item.lexically_normal().string());
      }
      std::sort(items.begin(), items.end());
      out << "cxx_sources = " << render_string_list(items) << "\n";
    }
    if (!manifest.native.include_dirs.empty()) {
      std::vector<std::string> items;
      for (const auto& item : manifest.native.include_dirs) {
        items.push_back(item.lexically_normal().string());
      }
      std::sort(items.begin(), items.end());
      out << "include_dirs = " << render_string_list(items) << "\n";
    }
    if (!manifest.native.defines.empty()) {
      std::vector<std::string> items = manifest.native.defines;
      std::sort(items.begin(), items.end());
      out << "defines = " << render_string_list(items) << "\n";
    }
    for (const auto& source : manifest.native.sources) {
      out << "\n[[native.sources]]\n";
      out << "path = \"" << json_escape(source.path.lexically_normal().string()) << "\"\n";
      out << "language = \"" << json_escape(render_native_language(source.language)) << "\"\n";
      if (!source.include_dirs.empty()) {
        std::vector<std::string> items;
        for (const auto& item : source.include_dirs) items.push_back(item.lexically_normal().string());
        std::sort(items.begin(), items.end());
        out << "include_dirs = " << render_string_list(items) << "\n";
      }
      if (!source.defines.empty()) {
        std::vector<std::string> items = source.defines;
        std::sort(items.begin(), items.end());
        out << "defines = " << render_string_list(items) << "\n";
      }
      if (!source.arch.empty()) {
        std::vector<std::string> items = source.arch;
        std::sort(items.begin(), items.end());
        out << "arch = " << render_string_list(items) << "\n";
      }
      if (!source.cpu_features.empty()) {
        std::vector<std::string> items = source.cpu_features;
        std::sort(items.begin(), items.end());
        out << "cpu_features = " << render_string_list(items) << "\n";
      }
    }
    for (const auto& header : manifest.native.generated_headers) {
      out << "\n[[native.generated_headers]]\n";
      out << "out = \"" << json_escape(header.out.lexically_normal().string()) << "\"\n";
      out << "template = \"" << json_escape(header.template_path.lexically_normal().string()) << "\"\n";
      out << "values = " << render_inline_string_map(header.values) << "\n";
    }
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
      } else if (dep.kind == ManifestDependencyKind::Installed) {
        out << "{ installed = \"" << json_escape(dep.installed) << "\" }\n";
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
    if (!ty.args.empty()) {
      out_ty += "<";
      for (std::size_t i = 0; i < ty.args.size(); ++i) {
        if (i) out_ty += ", ";
        out_ty += format_type(ty.args[i]);
      }
      out_ty += ">";
    }
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
            return format_expr(*n.base) + "." + n.field;
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
            s << format_expr(*n.base) << "." << n.method << "(";
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
          } else if constexpr (std::is_same_v<N, Expr::Try>) {
            return format_expr(*n.inner) + "?";
          } else if constexpr (std::is_same_v<N, Expr::Match>) {
            std::ostringstream s;
            s << "match " << format_expr(*n.subject) << " { ";
            for (std::size_t i = 0; i < n.arms.size(); ++i) {
              if (i) s << ", ";
              s << format_pattern(n.arms[i]->pattern) << " => " << format_expr(*n.arms[i]->value);
            }
            s << " }";
            return s.str();
          }
          return "/*expr*/";
        },
        expr.node);
  }

  std::string format_struct_binding_field(const StructBindingField& field) {
    if (field.skip) return field.field_name + ": _";
    if (field.binding_name.has_value() && *field.binding_name != field.field_name) {
      return field.field_name + ": " + *field.binding_name;
    }
    return field.field_name;
  }

  std::string format_pattern(const Pattern& pattern) {
    return std::visit(
        [&](auto&& n) -> std::string {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Pattern::Wildcard>) {
            return "_";
          } else if constexpr (std::is_same_v<N, Pattern::BoolLit>) {
            return n.value ? "true" : "false";
          } else if constexpr (std::is_same_v<N, Pattern::Variant>) {
            std::string out = n.name;
            if (n.payload.has_value()) {
              out += "(";
              std::visit(
                  [&](auto&& payload) {
                    using P = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<P, Pattern::Variant::EmptyPayload>) {
                    } else if constexpr (std::is_same_v<P, Pattern::Variant::BindingPayload>) {
                      out += payload.name;
                    } else if constexpr (std::is_same_v<P, Pattern::Variant::WildcardPayload>) {
                      out += "_";
                    } else if constexpr (std::is_same_v<P, Pattern::Variant::StructPayload>) {
                      out += "{ ";
                      for (std::size_t i = 0; i < payload.fields.size(); ++i) {
                        if (i) out += ", ";
                        out += format_struct_binding_field(payload.fields[i]);
                      }
                      out += " }";
                    }
                  },
                  *n.payload);
              out += ")";
            }
            return out;
          }
          return "_";
        },
        pattern.node);
  }

  void format_block(const Block& block) {
    line("{");
    indent += 1;
    for (const auto& stmt : block.stmts) format_stmt(stmt);
    indent -= 1;
    line("}");
  }

  std::string format_ui_prop(const UiProp& prop) {
    return prop.name + " = " + format_expr(*prop.value);
  }

  void format_ui_nodes(const std::vector<UiNode>& nodes) {
    line("{");
    indent += 1;
    for (const auto& node : nodes) format_ui_node(node);
    indent -= 1;
    line("}");
  }

  void format_ui_node(const UiNode& node) {
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, UiNode::View>) {
            std::ostringstream header;
            header << "view " << n.component << "(";
            for (std::size_t i = 0; i < n.props.size(); ++i) {
              if (i) header << ", ";
              header << format_ui_prop(n.props[i]);
            }
            header << ")";
            if (n.children.empty()) {
              line(header.str());
            } else {
              line(header.str() + " ");
              format_ui_nodes(n.children);
            }
          } else if constexpr (std::is_same_v<N, UiNode::If>) {
            line("if " + format_expr(*n.cond) + " ");
            format_ui_nodes(n.then_children);
          } else if constexpr (std::is_same_v<N, UiNode::For>) {
            line("for " + n.var + " in " + format_expr(*n.iterable) + " ");
            format_ui_nodes(n.body);
          }
        },
        node.node);
  }

  void format_ui(const Ui& ui) {
    for (const auto& ann : ui.annotations) line("@" + ann);
    std::ostringstream sig;
    sig << "ui " << ui.name << "(";
    for (std::size_t i = 0; i < ui.params.size(); ++i) {
      if (i) sig << ", ";
      if (ui.params[i].is_ref) sig << "ref ";
      sig << ui.params[i].name << ": " << format_type(ui.params[i].type);
    }
    sig << ")";
    line(sig.str() + " ");
    format_ui_nodes(ui.body);
  }

  void format_stmt(const Stmt& stmt) {
    for (const auto& ann : stmt.annotations) line("@" + ann);
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            line("let " + n.name + " = " + format_expr(*n.value));
          } else if constexpr (std::is_same_v<N, Stmt::LetStruct>) {
            std::ostringstream binding;
            binding << "let { ";
            for (std::size_t i = 0; i < n.fields.size(); ++i) {
              if (i) binding << ", ";
              binding << format_struct_binding_field(n.fields[i]);
            }
            binding << " } = " << format_expr(*n.value);
            line(binding.str());
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
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            line("while " + format_expr(*n.cond) + " ");
            format_block(n.body);
          } else if constexpr (std::is_same_v<N, Stmt::Break>) {
            line("break");
          } else if constexpr (std::is_same_v<N, Stmt::Continue>) {
            line("continue");
          } else if constexpr (std::is_same_v<N, Stmt::Match>) {
            line("match " + format_expr(*n.subject) + " {");
            indent += 1;
            for (const auto& arm : n.arms) {
              line(format_pattern(arm.pattern) + " => ");
              format_block(arm.body);
            }
            indent -= 1;
            line("}");
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
            sig << "fn " << n.name;
            if (!n.type_params.empty()) {
              sig << "<";
              for (std::size_t i = 0; i < n.type_params.size(); ++i) {
                if (i) sig << ", ";
                sig << n.type_params[i];
              }
              sig << ">";
            }
            sig << "(";
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
            std::string header = "struct " + n.name;
            if (!n.type_params.empty()) {
              header += "<";
              for (std::size_t i = 0; i < n.type_params.size(); ++i) {
                if (i) header += ", ";
                header += n.type_params[i];
              }
              header += ">";
            }
            line(header + " {");
            indent += 1;
            for (const auto& field : n.fields) {
              line(field.name + ": " + format_type(field.type));
            }
            indent -= 1;
            line("}");
          } else if constexpr (std::is_same_v<N, Enum>) {
            const auto& en = n;
            for (const auto& ann : en.annotations) line("@" + ann);
            std::string header = "enum " + en.name;
            if (!en.type_params.empty()) {
              header += "<";
              for (std::size_t i = 0; i < en.type_params.size(); ++i) {
                if (i) header += ", ";
                header += en.type_params[i];
              }
              header += ">";
            }
            line(header + " {");
            indent += 1;
            for (const Variant& variant : en.variants) {
              line(variant.name + "(" + format_type(variant.payload) + ")");
            }
            indent -= 1;
            line("}");
          } else if constexpr (std::is_same_v<N, Ui>) {
            format_ui(n);
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
            if constexpr (std::is_same_v<S, nebula::nir::Stmt::Declare>) {
              out.insert({st.var, st.name});
            } else if constexpr (std::is_same_v<S, nebula::nir::Stmt::Let>) {
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

void lsp_write(const std::string& body) {
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
  std::cout.flush();
}

struct LspRequest {
  cli_json::Value root;
  std::string method;
};

struct LspRequestParseResult {
  std::optional<LspRequest> request;
  int error_code = 0;
  std::string error_message;
  std::string id_json = "null";
};

struct LspTextChange {
  bool full_document = true;
  int start_line = 0;
  int start_character = 0;
  int end_line = 0;
  int end_character = 0;
  std::string text;
};

LspRequestParseResult parse_lsp_request(const std::string& body) {
  cli_json::ParseError error;
  auto root = cli_json::parse(body, &error);
  if (!root.has_value()) {
    return {std::nullopt, -32700, "parse error: " + error.message, "null"};
  }
  if (!root->is_object()) {
    return {std::nullopt, -32600, "invalid request: root must be a JSON object", "null"};
  }

  LspRequest request;
  request.root = std::move(*root);
  const auto version = cli_json::string_at(request.root, {"jsonrpc"});
  const cli_json::Value* id = cli_json::object_get(request.root, "id");
  const std::string id_json = id != nullptr ? cli_json::render_compact(*id) : "null";
  if (!version.has_value() || *version != "2.0") {
    return {std::nullopt, -32600, "invalid request: jsonrpc must be \"2.0\"", id_json};
  }
  const auto method = cli_json::string_at(request.root, {"method"});
  if (!method.has_value()) {
    return {std::nullopt, -32600, "invalid request: method must be a JSON string", id_json};
  }
  request.method = *method;
  return {std::move(request), 0, {}, id_json};
}

const cli_json::Value* lsp_request_id(const LspRequest& request) {
  return cli_json::object_get(request.root, "id");
}

std::string lsp_id_json(const LspRequest& request) {
  if (const auto* id = lsp_request_id(request); id != nullptr) {
    return cli_json::render_compact(*id);
  }
  return "null";
}

cli_json::Value json_i64(std::int64_t value) {
  return cli_json::Value(value);
}

cli_json::Value json_position_value(int line_zero_based, int character_zero_based) {
  cli_json::Value::Object out;
  out["line"] = json_i64(line_zero_based);
  out["character"] = json_i64(character_zero_based);
  return cli_json::Value(std::move(out));
}

cli_json::Value json_range_value(int start_line_zero_based,
                                 int start_character_zero_based,
                                 int end_line_zero_based,
                                 int end_character_zero_based) {
  cli_json::Value::Object out;
  out["start"] = json_position_value(start_line_zero_based, start_character_zero_based);
  out["end"] = json_position_value(end_line_zero_based, end_character_zero_based);
  return cli_json::Value(std::move(out));
}

cli_json::Value::Object jsonrpc_base_object() {
  cli_json::Value::Object out;
  out["jsonrpc"] = cli_json::Value("2.0");
  return out;
}

std::string lsp_render_response(std::string_view id_json, const cli_json::Value& result) {
  auto response = jsonrpc_base_object();
  if (auto parsed_id = cli_json::parse(id_json); parsed_id.has_value()) {
    response["id"] = std::move(*parsed_id);
  } else {
    response["id"] = cli_json::Value(nullptr);
  }
  response["result"] = result;
  return cli_json::render_compact(cli_json::Value(std::move(response)));
}

std::string lsp_render_notification(std::string_view method, const cli_json::Value& params) {
  auto notification = jsonrpc_base_object();
  notification["method"] = cli_json::Value(std::string(method));
  notification["params"] = params;
  return cli_json::render_compact(cli_json::Value(std::move(notification)));
}

std::string lsp_render_error(std::string_view id_json, int code, std::string_view message) {
  auto envelope = jsonrpc_base_object();
  if (auto parsed_id = cli_json::parse(id_json); parsed_id.has_value()) {
    envelope["id"] = std::move(*parsed_id);
  } else {
    envelope["id"] = cli_json::Value(nullptr);
  }

  cli_json::Value::Object error;
  error["code"] = json_i64(code);
  error["message"] = cli_json::Value(std::string(message));
  envelope["error"] = cli_json::Value(std::move(error));
  return cli_json::render_compact(cli_json::Value(std::move(envelope)));
}

void lsp_write_error(std::string_view id_json, int code, std::string_view message) {
  lsp_write(lsp_render_error(id_json, code, message));
}

void lsp_write_response(std::string_view id_json, const cli_json::Value& result) {
  lsp_write(lsp_render_response(id_json, result));
}

void lsp_write_notification(std::string_view method, const cli_json::Value& params) {
  lsp_write(lsp_render_notification(method, params));
}

std::optional<std::string> lsp_request_uri(const LspRequest& request) {
  auto uri = cli_json::string_at(request.root, {"params", "textDocument", "uri"});
  if (uri.has_value()) return uri;
  return cli_json::string_at(request.root, {"params", "uri"});
}

std::optional<std::string> lsp_request_open_text(const LspRequest& request) {
  auto text = cli_json::string_at(request.root, {"params", "textDocument", "text"});
  if (text.has_value()) return text;
  return cli_json::string_at(request.root, {"params", "text"});
}

std::vector<LspTextChange> lsp_request_content_changes(const LspRequest& request) {
  std::vector<LspTextChange> out;
  const cli_json::Value* changes = cli_json::value_at(request.root, {"params", "contentChanges"});
  if (changes == nullptr || !changes->is_array()) {
    if (auto text = cli_json::string_at(request.root, {"params", "text"}); text.has_value()) {
      out.push_back(LspTextChange{true, 0, 0, 0, 0, *text});
    }
    return out;
  }

  for (const auto& change : *changes->as_array()) {
    if (!change.is_object()) return {};
    const auto text = cli_json::string_at(change, {"text"});
    if (!text.has_value()) return {};
    const auto start_line = cli_json::int_at(change, {"range", "start", "line"});
    const auto start_character = cli_json::int_at(change, {"range", "start", "character"});
    const auto end_line = cli_json::int_at(change, {"range", "end", "line"});
    const auto end_character = cli_json::int_at(change, {"range", "end", "character"});
    if (start_line.has_value() || start_character.has_value() || end_line.has_value() || end_character.has_value()) {
      if (!start_line.has_value() || !start_character.has_value() || !end_line.has_value() ||
          !end_character.has_value()) {
        return {};
      }
      if (*start_line < std::numeric_limits<int>::min() || *start_line > std::numeric_limits<int>::max() ||
          *start_character < std::numeric_limits<int>::min() ||
          *start_character > std::numeric_limits<int>::max() ||
          *end_line < std::numeric_limits<int>::min() || *end_line > std::numeric_limits<int>::max() ||
          *end_character < std::numeric_limits<int>::min() ||
          *end_character > std::numeric_limits<int>::max()) {
        return {};
      }
      out.push_back(LspTextChange{false,
                                  static_cast<int>(*start_line),
                                  static_cast<int>(*start_character),
                                  static_cast<int>(*end_line),
                                  static_cast<int>(*end_character),
                                  *text});
      continue;
    }
    out.push_back(LspTextChange{true, 0, 0, 0, 0, *text});
  }
  return out;
}

std::optional<int> lsp_request_line(const LspRequest& request) {
  auto line = cli_json::int_at(request.root, {"params", "position", "line"});
  if (!line.has_value()) return std::nullopt;
  if (*line < std::numeric_limits<int>::min() || *line > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(*line);
}

std::optional<int> lsp_request_character(const LspRequest& request) {
  auto character = cli_json::int_at(request.root, {"params", "position", "character"});
  if (!character.has_value()) return std::nullopt;
  if (*character < std::numeric_limits<int>::min() || *character > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(*character);
}

struct LspRequestedRange {
  int start_line = 0;
  int start_character = 0;
  int end_line = 0;
  int end_character = 0;
};

std::optional<LspRequestedRange> lsp_request_range(const LspRequest& request) {
  const auto start_line = cli_json::int_at(request.root, {"params", "range", "start", "line"});
  const auto start_character =
      cli_json::int_at(request.root, {"params", "range", "start", "character"});
  const auto end_line = cli_json::int_at(request.root, {"params", "range", "end", "line"});
  const auto end_character =
      cli_json::int_at(request.root, {"params", "range", "end", "character"});
  if (!start_line.has_value() && !start_character.has_value() && !end_line.has_value() &&
      !end_character.has_value()) {
    return std::nullopt;
  }
  if (!start_line.has_value() || !start_character.has_value() || !end_line.has_value() ||
      !end_character.has_value()) {
    return std::nullopt;
  }
  return LspRequestedRange{
      static_cast<int>(*start_line),
      static_cast<int>(*start_character),
      static_cast<int>(*end_line),
      static_cast<int>(*end_character),
  };
}

std::vector<std::string> lsp_request_code_action_only(const LspRequest& request) {
  std::vector<std::string> out;
  const auto* only = cli_json::value_at(request.root, {"params", "context", "only"});
  if (only == nullptr || !only->is_array()) return out;
  for (const auto& item : *only->as_array()) {
    if (!item.is_string()) continue;
    out.push_back(*item.as_string());
  }
  return out;
}

std::optional<std::string> lsp_request_query(const LspRequest& request) {
  return cli_json::string_at(request.root, {"params", "query"});
}

std::optional<std::string> lsp_request_new_name(const LspRequest& request) {
  return cli_json::string_at(request.root, {"params", "newName"});
}

struct LspHeaderBlock {
  std::size_t content_length = 0;
};

struct LspHeaderParseResult {
  bool ok = false;
  bool eof = false;
  LspHeaderBlock headers;
  std::string error_message;
};

bool parse_unsigned_size(std::string_view text, std::size_t& value) {
  if (text.empty()) return false;
  std::size_t out = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const std::size_t digit = static_cast<std::size_t>(ch - '0');
    if (out > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
    out = out * 10 + digit;
  }
  value = out;
  return true;
}

std::string lsp_ascii_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

LspHeaderParseResult read_lsp_headers(std::istream& in) {
  std::string header_line;
  bool saw_header = false;
  bool saw_content_length = false;
  LspHeaderBlock headers;

  while (std::getline(in, header_line)) {
    saw_header = true;
    if (!header_line.empty() && header_line.back() == '\r') header_line.pop_back();
    if (header_line.empty()) {
      if (!saw_content_length) {
        return {false, false, {}, "invalid request header: missing Content-Length"};
      }
      return {true, false, headers, {}};
    }

    const std::size_t colon = header_line.find(':');
    if (colon == std::string::npos) {
      return {false, false, {}, "invalid request header: malformed header line"};
    }

    const std::string name = lsp_ascii_lower(trim(header_line.substr(0, colon)));
    const std::string value = trim(header_line.substr(colon + 1));
    if (name.empty()) {
      return {false, false, {}, "invalid request header: empty header name"};
    }
    if (name == "content-length") {
      if (saw_content_length) {
        return {false, false, {}, "invalid request header: duplicate Content-Length"};
      }
      if (!parse_unsigned_size(value, headers.content_length)) {
        return {false, false, {}, "invalid request header: Content-Length must be an unsigned integer"};
      }
      saw_content_length = true;
    }
  }

  if (!saw_header) return {false, true, {}, {}};
  return {false, false, {}, "invalid request header: unterminated header block"};
}

struct Utf8Scalar {
  std::uint32_t codepoint = 0;
  std::size_t byte_length = 1;
};

int utf16_units_for_codepoint(std::uint32_t codepoint) {
  return codepoint > 0xFFFF ? 2 : 1;
}

Utf8Scalar decode_utf8_scalar(std::string_view text, std::size_t offset) {
  const auto continuation = [&](std::size_t index) {
    return index < text.size() &&
           (static_cast<unsigned char>(text[index]) & 0xC0u) == 0x80u;
  };

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80u) return {lead, 1};
  if ((lead & 0xE0u) == 0xC0u && continuation(offset + 1)) {
    const std::uint32_t codepoint =
        (static_cast<std::uint32_t>(lead & 0x1Fu) << 6) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3Fu);
    if (codepoint >= 0x80u) return {codepoint, 2};
  }
  if ((lead & 0xF0u) == 0xE0u && continuation(offset + 1) && continuation(offset + 2)) {
    const std::uint32_t codepoint =
        (static_cast<std::uint32_t>(lead & 0x0Fu) << 12) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3Fu) << 6) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 2]) & 0x3Fu);
    if (codepoint >= 0x800u && !(codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
      return {codepoint, 3};
    }
  }
  if ((lead & 0xF8u) == 0xF0u && continuation(offset + 1) && continuation(offset + 2) &&
      continuation(offset + 3)) {
    const std::uint32_t codepoint =
        (static_cast<std::uint32_t>(lead & 0x07u) << 18) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 1]) & 0x3Fu) << 12) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 2]) & 0x3Fu) << 6) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + 3]) & 0x3Fu);
    if (codepoint >= 0x10000u && codepoint <= 0x10FFFFu) return {codepoint, 4};
  }
  return {lead, 1};
}

std::optional<std::string_view> line_text_at(std::string_view text, int line_zero_based) {
  if (line_zero_based < 0) return std::nullopt;
  std::size_t start = 0;
  for (int line = 0; line < line_zero_based; ++line) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string_view::npos) return std::nullopt;
    start = newline + 1;
  }
  std::size_t end = start;
  while (end < text.size() && text[end] != '\n' && text[end] != '\r') end += 1;
  return text.substr(start, end - start);
}

std::optional<std::size_t> lsp_position_to_byte_offset(std::string_view text,
                                                       int line_zero_based,
                                                       int lsp_character_zero_based) {
  if (line_zero_based < 0 || lsp_character_zero_based < 0) return std::nullopt;
  std::size_t line_start = 0;
  for (int line = 0; line < line_zero_based; ++line) {
    const std::size_t newline = text.find('\n', line_start);
    if (newline == std::string_view::npos) return std::nullopt;
    line_start = newline + 1;
  }
  const auto line = line_text_at(text, line_zero_based);
  if (!line.has_value()) return std::nullopt;

  std::size_t byte_offset = 0;
  int consumed = 0;
  while (byte_offset < line->size()) {
    if (consumed == lsp_character_zero_based) {
      return line_start + byte_offset;
    }
    const auto scalar = decode_utf8_scalar(*line, byte_offset);
    const int scalar_units = utf16_units_for_codepoint(scalar.codepoint);
    if (consumed + scalar_units > lsp_character_zero_based) {
      byte_offset += scalar.byte_length;
      return line_start + byte_offset;
    }
    consumed += scalar_units;
    byte_offset += scalar.byte_length;
  }
  if (consumed == lsp_character_zero_based) return line_start + byte_offset;
  return std::nullopt;
}

bool apply_lsp_content_changes(std::string& text, const std::vector<LspTextChange>& changes) {
  for (const auto& change : changes) {
    if (change.full_document) {
      text = change.text;
      continue;
    }
    const auto start = lsp_position_to_byte_offset(text, change.start_line, change.start_character);
    const auto end = lsp_position_to_byte_offset(text, change.end_line, change.end_character);
    if (!start.has_value() || !end.has_value() || *start > *end || *end > text.size()) return false;
    text.replace(*start, *end - *start, change.text);
  }
  return true;
}

std::optional<int> lsp_character_to_source_col_one_based(std::string_view text,
                                                         int line_zero_based,
                                                         int lsp_character_zero_based) {
  if (lsp_character_zero_based < 0) return std::nullopt;
  const auto line = line_text_at(text, line_zero_based);
  if (!line.has_value()) return std::nullopt;

  std::size_t byte_offset = 0;
  int consumed = 0;
  while (byte_offset < line->size()) {
    if (consumed == lsp_character_zero_based) {
      return static_cast<int>(byte_offset) + 1;
    }
    const auto scalar = decode_utf8_scalar(*line, byte_offset);
    const int scalar_units = utf16_units_for_codepoint(scalar.codepoint);
    if (consumed + scalar_units > lsp_character_zero_based) {
      byte_offset += scalar.byte_length;
      return static_cast<int>(byte_offset) + 1;
    }
    consumed += scalar_units;
    byte_offset += scalar.byte_length;
  }
  if (consumed == lsp_character_zero_based) return static_cast<int>(byte_offset) + 1;
  return std::nullopt;
}

int source_col_one_based_to_lsp_character(std::string_view text,
                                          int line_one_based,
                                          int source_col_one_based) {
  if (line_one_based <= 0 || source_col_one_based <= 0) return 0;
  const auto line = line_text_at(text, line_one_based - 1);
  if (!line.has_value()) return 0;

  const std::size_t target_byte_offset =
      std::min<std::size_t>(static_cast<std::size_t>(source_col_one_based - 1), line->size());
  std::size_t byte_offset = 0;
  int units = 0;
  while (byte_offset < line->size() && byte_offset < target_byte_offset) {
    const auto scalar = decode_utf8_scalar(*line, byte_offset);
    if (byte_offset + scalar.byte_length > target_byte_offset) {
      units += utf16_units_for_codepoint(scalar.codepoint);
      break;
    }
    units += utf16_units_for_codepoint(scalar.codepoint);
    byte_offset += scalar.byte_length;
  }
  return units;
}

std::string uri_percent_decode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const std::string hex(text.substr(i + 1, 2));
      try {
        const char ch = static_cast<char>(std::stoi(hex, nullptr, 16));
        out.push_back(ch);
        i += 2;
        continue;
      } catch (...) {
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

std::string uri_to_path(std::string uri) {
  const std::string prefix = "file://";
  if (uri.rfind(prefix, 0) == 0) uri.erase(0, prefix.size());
  std::string decoded = uri_percent_decode(uri);
  if (decoded.rfind("localhost/", 0) == 0) decoded.erase(0, std::string("localhost").size());
#if defined(_WIN32)
  if (!decoded.empty() && decoded[0] == '/' && decoded.size() >= 3 &&
      std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
    decoded.erase(0, 1);
  } else if (!decoded.empty() && decoded[0] != '/' &&
             !(decoded.size() >= 2 && std::isalpha(static_cast<unsigned char>(decoded[0])) &&
               decoded[1] == ':')) {
    decoded = "//" + decoded;
  }
  return fs::path(decoded).make_preferred().string();
#else
  if (!decoded.empty() && decoded[0] != '/') decoded = "//" + decoded;
  return decoded;
#endif
}

bool uri_char_is_unreserved(unsigned char ch) {
  return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':';
}

std::string path_to_uri(const fs::path& path) {
  std::string raw = fs::absolute(path).lexically_normal().generic_string();
#if defined(_WIN32)
  if (raw.rfind("//", 0) == 0) {
    std::string out = "file:";
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char ch : raw) {
      if (uri_char_is_unreserved(ch)) {
        out.push_back(static_cast<char>(ch));
      } else {
        out.push_back('%');
        out.push_back(digits[(ch >> 4) & 0x0F]);
        out.push_back(digits[ch & 0x0F]);
      }
    }
    return out;
  }
  if (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':') {
    raw = "/" + raw;
  }
#endif
  std::string out = "file://";
  constexpr char digits[] = "0123456789ABCDEF";
  for (unsigned char ch : raw) {
    if (uri_char_is_unreserved(ch)) {
      out.push_back(static_cast<char>(ch));
    } else {
      out.push_back('%');
      out.push_back(digits[(ch >> 4) & 0x0F]);
      out.push_back(digits[ch & 0x0F]);
    }
  }
  return out;
}

struct LspDocumentState {
  std::string uri;
  std::string path;
  std::string text;
  std::string last_good_parse_text;
};

fs::path g_lsp_std_root;
fs::path g_lsp_backend_sdk_root;

std::string normalized_path_string(const fs::path& path) {
  std::string normalized = fs::absolute(path).lexically_normal().generic_string();
#if defined(_WIN32)
  if (normalized.size() >= 2 && normalized[1] == ':' &&
      std::isalpha(static_cast<unsigned char>(normalized[0]))) {
    normalized[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized[0])));
  }
#endif
  return normalized;
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
  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
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

enum class SymbolKind : std::uint8_t { Function, Struct, Enum, EnumVariant, Ui, Local };

std::string render_source_type(const Type& ty) {
  if (ty.callable_ret) {
    std::ostringstream s;
    s << (ty.is_unsafe_callable ? "UnsafeFn(" : "Fn(");
    for (std::size_t i = 0; i < ty.callable_params.size(); ++i) {
      if (i) s << ", ";
      s << render_source_type(ty.callable_params[i]);
    }
    s << ") -> " << render_source_type(*ty.callable_ret);
    return s.str();
  }
  std::string out = ty.name;
  if (!ty.args.empty()) {
    out += "<";
    for (std::size_t i = 0; i < ty.args.size(); ++i) {
      if (i) out += ", ";
      out += render_source_type(ty.args[i]);
    }
    out += ">";
  }
  return out;
}

std::string function_symbol_detail(const Function& fn) {
  std::ostringstream sig;
  if (fn.is_extern) {
    sig << "extern fn ";
  } else if (fn.is_async) {
    sig << "async fn ";
  } else {
    sig << "fn ";
  }
  sig << fn.name;
  if (!fn.type_params.empty()) {
    sig << "<";
    for (std::size_t i = 0; i < fn.type_params.size(); ++i) {
      if (i) sig << ", ";
      sig << fn.type_params[i];
    }
    sig << ">";
  }
  sig << "(";
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    if (i) sig << ", ";
    if (fn.params[i].is_ref) sig << "ref ";
    sig << fn.params[i].name << ": " << render_source_type(fn.params[i].type);
  }
  sig << ")";
  if (fn.return_type.has_value()) sig << " -> " << render_source_type(*fn.return_type);
  return sig.str();
}

std::string enum_variant_detail(const std::string& enum_name, const Variant& variant) {
  return "variant " + enum_name + "::" + variant.name + "(" + render_source_type(variant.payload) + ")";
}

std::string ui_symbol_detail(const Ui& ui) {
  std::ostringstream sig;
  sig << "ui " << ui.name << "(";
  for (std::size_t i = 0; i < ui.params.size(); ++i) {
    if (i) sig << ", ";
    if (ui.params[i].is_ref) sig << "ref ";
    sig << ui.params[i].name << ": " << render_source_type(ui.params[i].type);
  }
  sig << ")";
  return sig.str();
}

struct SymbolDefinition {
  std::string name;
  std::string detail;
  std::string package_name;
  std::string module_name;
  nebula::frontend::Span span;
  nebula::frontend::Span visibility;
  SymbolKind kind = SymbolKind::Local;
  std::optional<QualifiedName> qualified_name;
  std::optional<QualifiedName> owner_qualified_name;
  std::optional<std::uint32_t> variant_index;
  bool local = false;
};

QualifiedName qualify_symbol(const SourceFile& source, std::string local_name) {
  return QualifiedName{source.package_name, source.module_name, std::move(local_name)};
}

void push_local_symbol(std::vector<SymbolDefinition>& out,
                       std::string name,
                       std::string detail,
                       nebula::frontend::Span span,
                       nebula::frontend::Span visibility) {
  out.push_back({std::move(name),
                 std::move(detail),
                 {},
                 {},
                 std::move(span),
                 std::move(visibility),
                 SymbolKind::Local,
                 std::nullopt,
                 std::nullopt,
                 std::nullopt,
                 true});
}

void collect_expr_local_symbols(const Expr& expr, std::vector<SymbolDefinition>& out) {
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          for (const auto& arg : node.args) collect_expr_local_symbols(*arg, out);
        } else if constexpr (std::is_same_v<N, Expr::Field>) {
          collect_expr_local_symbols(*node.base, out);
        } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
          collect_expr_local_symbols(*node.base, out);
          for (const auto& arg : node.args) collect_expr_local_symbols(*arg, out);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_expr_local_symbols(*node.lhs, out);
          collect_expr_local_symbols(*node.rhs, out);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix> ||
                             std::is_same_v<N, Expr::Try> ||
                             std::is_same_v<N, Expr::Await>) {
          collect_expr_local_symbols(*node.inner, out);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          collect_expr_local_symbols(*node.subject, out);
          for (const auto& arm : node.arms) {
            if (std::holds_alternative<Pattern::Variant>(arm->pattern.node)) {
              const auto& variant = std::get<Pattern::Variant>(arm->pattern.node);
              if (variant.payload.has_value()) {
                std::visit(
                    [&](auto&& payload) {
                      using P = std::decay_t<decltype(payload)>;
                      if constexpr (std::is_same_v<P, Pattern::Variant::BindingPayload>) {
                        push_local_symbol(out,
                                          payload.name,
                                          "match " + payload.name,
                                          payload.binding_span,
                                          arm->value->span);
                      } else if constexpr (std::is_same_v<P, Pattern::Variant::StructPayload>) {
                        for (const auto& field : payload.fields) {
                          if (!field.skip && field.binding_name.has_value()) {
                            push_local_symbol(out,
                                              *field.binding_name,
                                              "match " + *field.binding_name,
                                              field.binding_span,
                                              arm->value->span);
                          }
                        }
                      }
                    },
                    *variant.payload);
              }
            }
            collect_expr_local_symbols(*arm->value, out);
          }
        }
      },
      expr.node);
}

void collect_block_symbols(const Block& block, std::vector<SymbolDefinition>& out) {
  for (const auto& stmt : block.stmts) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            push_local_symbol(out, node.name, "let " + node.name, stmt.span, block.span);
            collect_expr_local_symbols(*node.value, out);
          } else if constexpr (std::is_same_v<N, Stmt::LetStruct>) {
            for (const auto& field : node.fields) {
              if (!field.skip && field.binding_name.has_value()) {
                push_local_symbol(out,
                                  *field.binding_name,
                                  "let " + *field.binding_name,
                                  field.binding_span,
                                  block.span);
              }
            }
            collect_expr_local_symbols(*node.value, out);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            collect_expr_local_symbols(*node.value, out);
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            collect_expr_local_symbols(*node.expr, out);
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            collect_expr_local_symbols(*node.value, out);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            collect_expr_local_symbols(*node.value, out);
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            push_local_symbol(out, node.name, "region " + node.name, stmt.span, node.body.span);
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            collect_expr_local_symbols(*node.cond, out);
            collect_block_symbols(node.then_body, out);
            if (node.else_body.has_value()) collect_block_symbols(*node.else_body, out);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            push_local_symbol(out, node.var, "for " + node.var, stmt.span, node.body.span);
            collect_expr_local_symbols(*node.start, out);
            collect_expr_local_symbols(*node.end, out);
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            collect_expr_local_symbols(*node.cond, out);
            collect_block_symbols(node.body, out);
          } else if constexpr (std::is_same_v<N, Stmt::Match>) {
            collect_expr_local_symbols(*node.subject, out);
            for (const auto& arm : node.arms) {
              if (std::holds_alternative<Pattern::Variant>(arm.pattern.node)) {
                const auto& variant = std::get<Pattern::Variant>(arm.pattern.node);
                if (variant.payload.has_value()) {
                  std::visit(
                      [&](auto&& payload) {
                        using P = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<P, Pattern::Variant::BindingPayload>) {
                          push_local_symbol(out,
                                            payload.name,
                                            "match " + payload.name,
                                            payload.binding_span,
                                            arm.body.span);
                        } else if constexpr (std::is_same_v<P, Pattern::Variant::StructPayload>) {
                          for (const auto& field : payload.fields) {
                            if (!field.skip && field.binding_name.has_value()) {
                              push_local_symbol(out,
                                                *field.binding_name,
                                                "match " + *field.binding_name,
                                                field.binding_span,
                                                arm.body.span);
                            }
                          }
                        }
                      },
                      *variant.payload);
                }
              }
              collect_block_symbols(arm.body, out);
            }
          }
        },
        stmt.node);
  }
}

void collect_ui_node_symbols(const UiNode& node, std::vector<SymbolDefinition>& out);

void collect_ui_nodes_symbols(const std::vector<UiNode>& nodes, std::vector<SymbolDefinition>& out) {
  for (const auto& node : nodes) collect_ui_node_symbols(node, out);
}

void collect_ui_node_symbols(const UiNode& node, std::vector<SymbolDefinition>& out) {
  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, UiNode::View>) {
          for (const auto& prop : n.props) collect_expr_local_symbols(*prop.value, out);
          collect_ui_nodes_symbols(n.children, out);
        } else if constexpr (std::is_same_v<N, UiNode::If>) {
          collect_expr_local_symbols(*n.cond, out);
          collect_ui_nodes_symbols(n.then_children, out);
        } else if constexpr (std::is_same_v<N, UiNode::For>) {
          push_local_symbol(out, n.var, "ui for " + n.var, n.var_span, node.span);
          collect_expr_local_symbols(*n.iterable, out);
          collect_ui_nodes_symbols(n.body, out);
        }
      },
      node.node);
}

void append_program_symbol_definitions(const Program& program,
                                       const SourceFile& source,
                                       std::vector<SymbolDefinition>& out) {
  for (const auto& item : program.items) {
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Function>) {
            const QualifiedName qname = qualify_symbol(source, n.name);
            out.push_back({n.name,
                           function_symbol_detail(n),
                           source.package_name,
                           source.module_name,
                           n.span,
                           n.span,
                           SymbolKind::Function,
                           qname,
                           std::nullopt,
                           std::nullopt,
                           false});
            const nebula::frontend::Span fn_visibility = n.body.has_value() ? n.body->span : n.span;
            for (const auto& param : n.params) {
              push_local_symbol(out,
                                param.name,
                                "param " + param.name,
                                param.span,
                                fn_visibility);
            }
            if (n.body.has_value()) collect_block_symbols(*n.body, out);
          } else if constexpr (std::is_same_v<N, Struct>) {
            const QualifiedName qname = qualify_symbol(source, n.name);
            out.push_back({n.name,
                           "struct " + n.name,
                           source.package_name,
                           source.module_name,
                           n.span,
                           n.span,
                           SymbolKind::Struct,
                           qname,
                           std::nullopt,
                           std::nullopt,
                           false});
          } else if constexpr (std::is_same_v<N, Enum>) {
            const QualifiedName qname = qualify_symbol(source, n.name);
            out.push_back({n.name,
                           "enum " + n.name,
                           source.package_name,
                           source.module_name,
                           n.span,
                           n.span,
                           SymbolKind::Enum,
                           qname,
                           std::nullopt,
                           std::nullopt,
                           false});
            for (std::size_t i = 0; i < n.variants.size(); ++i) {
              const auto& variant = n.variants[i];
              out.push_back({variant.name,
                             enum_variant_detail(n.name, variant),
                             source.package_name,
                             source.module_name,
                             variant.span,
                             n.span,
                             SymbolKind::EnumVariant,
                             std::nullopt,
                             qname,
                             static_cast<std::uint32_t>(i),
                             false});
            }
          } else if constexpr (std::is_same_v<N, Ui>) {
            const QualifiedName qname = qualify_symbol(source, n.name);
            out.push_back({n.name,
                           ui_symbol_detail(n),
                           source.package_name,
                           source.module_name,
                           n.span,
                           n.span,
                           SymbolKind::Ui,
                           qname,
                           std::nullopt,
                           std::nullopt,
                           false});
            for (const auto& param : n.params) {
              push_local_symbol(out,
                                param.name,
                                "ui param " + param.name,
                                param.span,
                                n.span);
            }
            collect_ui_nodes_symbols(n.body, out);
          }
        },
        item.node);
  }
}

std::size_t skip_balanced_tokens(const std::vector<nebula::frontend::Token>& tokens,
                                 std::size_t start,
                                 nebula::frontend::TokenKind open,
                                 nebula::frontend::TokenKind close) {
  if (start >= tokens.size() || tokens[start].kind != open) return start;
  int depth = 0;
  for (std::size_t i = start; i < tokens.size(); ++i) {
    if (tokens[i].kind == open) depth += 1;
    else if (tokens[i].kind == close) {
      depth -= 1;
      if (depth == 0) return i + 1;
    }
  }
  return tokens.size();
}

std::string compact_source_excerpt(std::string_view text, std::size_t start, std::size_t end) {
  if (start >= text.size()) return {};
  end = std::min(end, text.size());
  if (end <= start) return {};
  std::string out;
  out.reserve(end - start);
  bool pending_space = false;
  for (std::size_t i = start; i < end; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (std::isspace(ch)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out.push_back(' ');
    out.push_back(static_cast<char>(ch));
    pending_space = false;
  }
  return out;
}

nebula::frontend::Span span_from_token_range(const std::vector<nebula::frontend::Token>& tokens,
                                             std::size_t start,
                                             std::size_t end_exclusive,
                                             const std::string& source_path) {
  if (tokens.empty() || start >= tokens.size()) {
    return nebula::frontend::Span{{}, {}, source_path};
  }
  const std::size_t clamped_end = std::min(end_exclusive, tokens.size());
  const std::size_t last = clamped_end > start ? (clamped_end - 1) : start;
  return nebula::frontend::Span(tokens[start].span.start, tokens[last].span.end, source_path);
}

std::string fallback_function_detail(const SourceFile& source,
                                     const std::vector<nebula::frontend::Token>& tokens,
                                     std::size_t start_index,
                                     std::size_t name_index,
                                     bool is_extern) {
  std::size_t detail_end = name_index + 1;
  std::size_t cursor = name_index + 1;
  if (cursor < tokens.size() && tokens[cursor].kind == nebula::frontend::TokenKind::LParen) {
    detail_end = skip_balanced_tokens(tokens,
                                      cursor,
                                      nebula::frontend::TokenKind::LParen,
                                      nebula::frontend::TokenKind::RParen);
    cursor = detail_end;
  }
  if (cursor < tokens.size() && tokens[cursor].kind == nebula::frontend::TokenKind::Arrow) {
    cursor += 1;
    while (cursor < tokens.size() &&
           tokens[cursor].kind != nebula::frontend::TokenKind::LBrace &&
           tokens[cursor].kind != nebula::frontend::TokenKind::KwFn &&
           tokens[cursor].kind != nebula::frontend::TokenKind::KwStruct &&
           tokens[cursor].kind != nebula::frontend::TokenKind::KwEnum &&
           tokens[cursor].kind != nebula::frontend::TokenKind::KwUi &&
           tokens[cursor].kind != nebula::frontend::TokenKind::KwExtern &&
           tokens[cursor].kind != nebula::frontend::TokenKind::Eof) {
      cursor += 1;
    }
    detail_end = cursor;
  }
  const std::size_t slice_start = tokens[start_index].span.start.offset;
  const std::size_t slice_end =
      detail_end > start_index ? tokens[detail_end - 1].span.end.offset : tokens[name_index].span.end.offset;
  std::string detail = compact_source_excerpt(source.text, slice_start, slice_end);
  if (detail.empty()) detail = (is_extern ? "extern fn " : "fn ") + tokens[name_index].lexeme;
  if (!is_extern && start_index > 0 &&
      tokens[start_index - 1].kind == nebula::frontend::TokenKind::KwAsync) {
    detail = "async " + detail;
  }
  return detail;
}

std::string fallback_variant_detail(const SourceFile& source,
                                    const std::string& enum_name,
                                    const std::vector<nebula::frontend::Token>& tokens,
                                    std::size_t name_index) {
  std::size_t detail_end = name_index + 1;
  if (name_index + 1 < tokens.size() &&
      tokens[name_index + 1].kind == nebula::frontend::TokenKind::LParen) {
    detail_end = skip_balanced_tokens(tokens,
                                      name_index + 1,
                                      nebula::frontend::TokenKind::LParen,
                                      nebula::frontend::TokenKind::RParen);
  }
  const std::size_t slice_start = tokens[name_index].span.start.offset;
  const std::size_t slice_end =
      detail_end > name_index ? tokens[detail_end - 1].span.end.offset : tokens[name_index].span.end.offset;
  std::string payload = compact_source_excerpt(source.text, slice_start, slice_end);
  if (payload.empty()) payload = tokens[name_index].lexeme;
  return "variant " + enum_name + "::" + payload;
}

void append_fallback_symbol_definitions(const SourceFile& source, std::vector<SymbolDefinition>& out) {
  Lexer lex(source.text, source.path);
  auto tokens = lex.lex_all();
  for (std::size_t i = 0; i < tokens.size();) {
    const auto& tok = tokens[i];
    if (tok.kind == nebula::frontend::TokenKind::KwExtern &&
        i + 2 < tokens.size() &&
        tokens[i + 1].kind == nebula::frontend::TokenKind::KwFn &&
        tokens[i + 2].kind == nebula::frontend::TokenKind::Ident) {
      const auto& name = tokens[i + 2];
      nebula::frontend::Span decl_span(tok.span.start, name.span.end, tok.span.source_path);
      out.push_back({name.lexeme,
                     fallback_function_detail(source, tokens, i, i + 2, true),
                     source.package_name,
                     source.module_name,
                     decl_span,
                     decl_span,
                     SymbolKind::Function,
                     qualify_symbol(source, name.lexeme),
                     std::nullopt,
                     std::nullopt,
                     false});
      i += 3;
      continue;
    }
    if (tok.kind == nebula::frontend::TokenKind::KwAsync &&
        i + 2 < tokens.size() &&
        tokens[i + 1].kind == nebula::frontend::TokenKind::KwFn &&
        tokens[i + 2].kind == nebula::frontend::TokenKind::Ident) {
      const auto& name = tokens[i + 2];
      nebula::frontend::Span decl_span(tok.span.start, name.span.end, tok.span.source_path);
      out.push_back({name.lexeme,
                     fallback_function_detail(source, tokens, i + 1, i + 2, false),
                     source.package_name,
                     source.module_name,
                     decl_span,
                     decl_span,
                     SymbolKind::Function,
                     qualify_symbol(source, name.lexeme),
                     std::nullopt,
                     std::nullopt,
                     false});
      i += 3;
      continue;
    }
    if ((tok.kind == nebula::frontend::TokenKind::KwFn ||
         tok.kind == nebula::frontend::TokenKind::KwStruct ||
         tok.kind == nebula::frontend::TokenKind::KwEnum ||
         tok.kind == nebula::frontend::TokenKind::KwUi ||
         (tok.kind == nebula::frontend::TokenKind::Ident && tok.lexeme == "ui")) &&
        i + 1 < tokens.size() &&
        tokens[i + 1].kind == nebula::frontend::TokenKind::Ident) {
      const auto& name = tokens[i + 1];
      const SymbolKind kind = tok.kind == nebula::frontend::TokenKind::KwFn
                                  ? SymbolKind::Function
                                  : (tok.kind == nebula::frontend::TokenKind::KwStruct
                                         ? SymbolKind::Struct
                                         : (tok.kind == nebula::frontend::TokenKind::KwEnum
                                                ? SymbolKind::Enum
                                                : SymbolKind::Ui));
      const std::string detail = kind == SymbolKind::Function
                                     ? fallback_function_detail(source, tokens, i, i + 1, false)
                                     : ((kind == SymbolKind::Struct
                                             ? "struct "
                                             : (kind == SymbolKind::Enum ? "enum " : "ui ")) +
                                        name.lexeme);
      const QualifiedName qname = qualify_symbol(source, name.lexeme);
      nebula::frontend::Span decl_span(tok.span.start, name.span.end, tok.span.source_path);
      out.push_back({name.lexeme,
                     detail,
                     source.package_name,
                     source.module_name,
                     decl_span,
                     decl_span,
                     kind,
                     kind == SymbolKind::EnumVariant ? std::nullopt : std::optional<QualifiedName>(qname),
                     std::nullopt,
                     std::nullopt,
                     false});

      if (tok.kind == nebula::frontend::TokenKind::KwEnum) {
        std::size_t cursor = i + 2;
        if (cursor < tokens.size() && tokens[cursor].kind == nebula::frontend::TokenKind::Less) {
          cursor = skip_balanced_tokens(tokens,
                                        cursor,
                                        nebula::frontend::TokenKind::Less,
                                        nebula::frontend::TokenKind::Greater);
        }
        if (cursor < tokens.size() && tokens[cursor].kind == nebula::frontend::TokenKind::LBrace) {
          std::size_t variant_index = 0;
          int depth = 1;
          cursor += 1;
          while (cursor < tokens.size() && depth > 0) {
            if (tokens[cursor].kind == nebula::frontend::TokenKind::LBrace) {
              depth += 1;
              cursor += 1;
              continue;
            }
            if (tokens[cursor].kind == nebula::frontend::TokenKind::RBrace) {
              depth -= 1;
              cursor += 1;
              continue;
            }
            if (depth == 1 &&
                tokens[cursor].kind == nebula::frontend::TokenKind::Ident &&
                cursor + 1 < tokens.size() &&
                tokens[cursor + 1].kind == nebula::frontend::TokenKind::LParen) {
              const auto& variant = tokens[cursor];
              std::size_t end = skip_balanced_tokens(tokens,
                                                     cursor + 1,
                                                     nebula::frontend::TokenKind::LParen,
                                                     nebula::frontend::TokenKind::RParen);
              nebula::frontend::Span variant_span = variant.span;
              if (end > cursor + 1 && end <= tokens.size()) {
                variant_span.end = tokens[end - 1].span.end;
              }
              out.push_back({variant.lexeme,
                             fallback_variant_detail(source, name.lexeme, tokens, cursor),
                             source.package_name,
                             source.module_name,
                             variant_span,
                             name.span,
                             SymbolKind::EnumVariant,
                             std::nullopt,
                             qname,
                             static_cast<std::uint32_t>(variant_index++),
                             false});
              cursor = end;
              continue;
            }
            cursor += 1;
          }
          i = cursor;
          continue;
        }
      }

      i += 2;
      continue;
    }
    i += 1;
  }
}

[[maybe_unused]] std::vector<SymbolDefinition> collect_symbol_definitions(
    const std::vector<SourceFile>& sources) {
  std::vector<SymbolDefinition> out;
  for (const auto& source : sources) {
    try {
      Lexer lex(source.text, source.path);
      auto toks = lex.lex_all();
      Parser parser(std::move(toks));
      auto program = parser.parse_program();
      append_program_symbol_definitions(program, source, out);
    } catch (const nebula::frontend::FrontendError&) {
      append_fallback_symbol_definitions(source, out);
    } catch (...) {
      append_fallback_symbol_definitions(source, out);
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

std::vector<SymbolDefinition> collect_current_file_symbols_from_text(const SourceFile& source,
                                                                     const std::string& text) {
  std::vector<SymbolDefinition> out;
  SourceFile updated = source;
  updated.text = text;
  try {
    Lexer lex(updated.text, updated.path);
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    auto program = parser.parse_program();
    append_program_symbol_definitions(program, updated, out);
  } catch (const nebula::frontend::FrontendError&) {
    append_fallback_symbol_definitions(updated, out);
  } catch (...) {
    append_fallback_symbol_definitions(updated, out);
  }
  return out;
}

void recover_struct_binding_locals(const std::vector<nebula::frontend::Token>& tokens,
                                   std::size_t begin,
                                   std::size_t end_exclusive,
                                   const nebula::frontend::Span& visibility,
                                   std::string_view detail_prefix,
                                   std::vector<SymbolDefinition>& out) {
  std::size_t i = begin;
  while (i < end_exclusive) {
    if (tokens[i].kind != nebula::frontend::TokenKind::Ident) {
      i += 1;
      continue;
    }
    const auto& field = tokens[i];
    if (i + 2 < end_exclusive &&
        tokens[i + 1].kind == nebula::frontend::TokenKind::Colon &&
        tokens[i + 2].kind == nebula::frontend::TokenKind::Ident) {
      const auto& binding = tokens[i + 2];
      if (binding.lexeme != "_") {
        push_local_symbol(out,
                          binding.lexeme,
                          std::string(detail_prefix) + binding.lexeme,
                          binding.span,
                          visibility);
      }
      i += 3;
      continue;
    }
    push_local_symbol(out,
                      field.lexeme,
                      std::string(detail_prefix) + field.lexeme,
                      field.span,
                      visibility);
    i += 1;
  }
}

std::size_t find_next_body_lbrace(const std::vector<nebula::frontend::Token>& tokens,
                                  std::size_t start,
                                  std::size_t end_exclusive) {
  int paren_depth = 0;
  int angle_depth = 0;
  for (std::size_t i = start; i < end_exclusive; ++i) {
    switch (tokens[i].kind) {
    case nebula::frontend::TokenKind::LParen: paren_depth += 1; break;
    case nebula::frontend::TokenKind::RParen: paren_depth = std::max(0, paren_depth - 1); break;
    case nebula::frontend::TokenKind::Less: angle_depth += 1; break;
    case nebula::frontend::TokenKind::Greater: angle_depth = std::max(0, angle_depth - 1); break;
    case nebula::frontend::TokenKind::LBrace:
      if (paren_depth == 0 && angle_depth == 0) return i;
      break;
    default: break;
    }
  }
  return end_exclusive;
}

void recover_block_local_symbols(const std::vector<nebula::frontend::Token>& tokens,
                                 std::size_t block_open,
                                 std::size_t block_end_exclusive,
                                 const std::string& source_path,
                                 std::vector<SymbolDefinition>& out);

void recover_match_local_symbols(const std::vector<nebula::frontend::Token>& tokens,
                                 std::size_t match_open,
                                 std::size_t match_end_exclusive,
                                 const std::string& source_path,
                                 std::vector<SymbolDefinition>& out) {
  std::size_t i = match_open + 1;
  while (i + 1 < match_end_exclusive) {
    while (i < match_end_exclusive &&
           (tokens[i].kind == nebula::frontend::TokenKind::Comma ||
            tokens[i].kind == nebula::frontend::TokenKind::RBrace)) {
      i += 1;
    }
    if (i >= match_end_exclusive) break;
    const std::size_t pattern_start = i;
    int paren_depth = 0;
    int brace_depth = 0;
    std::size_t arrow = match_end_exclusive;
    for (; i < match_end_exclusive; ++i) {
      const auto kind = tokens[i].kind;
      if (kind == nebula::frontend::TokenKind::LParen) {
        paren_depth += 1;
      } else if (kind == nebula::frontend::TokenKind::RParen) {
        paren_depth = std::max(0, paren_depth - 1);
      } else if (kind == nebula::frontend::TokenKind::LBrace) {
        brace_depth += 1;
      } else if (kind == nebula::frontend::TokenKind::RBrace) {
        if (brace_depth == 0) break;
        brace_depth -= 1;
      } else if (kind == nebula::frontend::TokenKind::FatArrow && paren_depth == 0 &&
                 brace_depth == 0) {
        arrow = i;
        break;
      }
    }
    if (arrow == match_end_exclusive || arrow + 1 >= match_end_exclusive) break;

    std::size_t body_open = arrow + 1;
    while (body_open < match_end_exclusive &&
           tokens[body_open].kind != nebula::frontend::TokenKind::LBrace) {
      body_open += 1;
    }
    if (body_open >= match_end_exclusive) break;
    std::size_t body_end = skip_balanced_tokens(tokens,
                                                body_open,
                                                nebula::frontend::TokenKind::LBrace,
                                                nebula::frontend::TokenKind::RBrace);
    if (body_end > match_end_exclusive) body_end = match_end_exclusive;
    const auto visibility =
        span_from_token_range(tokens, body_open, body_end, source_path);

    if (pattern_start + 1 < arrow &&
        tokens[pattern_start].kind == nebula::frontend::TokenKind::Ident &&
        tokens[pattern_start + 1].kind == nebula::frontend::TokenKind::LParen) {
      if (pattern_start + 3 < arrow &&
          tokens[pattern_start + 2].kind == nebula::frontend::TokenKind::LBrace) {
        const std::size_t fields_end = skip_balanced_tokens(tokens,
                                                            pattern_start + 2,
                                                            nebula::frontend::TokenKind::LBrace,
                                                            nebula::frontend::TokenKind::RBrace);
        if (fields_end != tokens.size() && fields_end > pattern_start + 3) {
          recover_struct_binding_locals(tokens,
                                        pattern_start + 3,
                                        fields_end - 1,
                                        visibility,
                                        "match ",
                                        out);
        }
      } else if (pattern_start + 2 < arrow &&
                 tokens[pattern_start + 2].kind == nebula::frontend::TokenKind::Ident &&
                 tokens[pattern_start + 2].lexeme != "_") {
        push_local_symbol(out,
                          tokens[pattern_start + 2].lexeme,
                          "match " + tokens[pattern_start + 2].lexeme,
                          tokens[pattern_start + 2].span,
                          visibility);
      }
    }

    recover_block_local_symbols(tokens, body_open, body_end, source_path, out);
    i = body_end;
  }
}

void recover_block_local_symbols(const std::vector<nebula::frontend::Token>& tokens,
                                 std::size_t block_open,
                                 std::size_t block_end_exclusive,
                                 const std::string& source_path,
                                 std::vector<SymbolDefinition>& out) {
  const auto visibility =
      span_from_token_range(tokens, block_open, block_end_exclusive, source_path);
  std::size_t i = block_open + 1;
  while (i < block_end_exclusive) {
    const auto kind = tokens[i].kind;
    if (kind == nebula::frontend::TokenKind::KwLet) {
      if (i + 1 < block_end_exclusive && tokens[i + 1].kind == nebula::frontend::TokenKind::Ident) {
        push_local_symbol(out,
                          tokens[i + 1].lexeme,
                          "let " + tokens[i + 1].lexeme,
                          tokens[i + 1].span,
                          visibility);
        i += 2;
        continue;
      }
      if (i + 1 < block_end_exclusive && tokens[i + 1].kind == nebula::frontend::TokenKind::LBrace) {
        const std::size_t fields_end = skip_balanced_tokens(tokens,
                                                            i + 1,
                                                            nebula::frontend::TokenKind::LBrace,
                                                            nebula::frontend::TokenKind::RBrace);
        if (fields_end != tokens.size() && fields_end > i + 2) {
          recover_struct_binding_locals(tokens,
                                        i + 2,
                                        fields_end - 1,
                                        visibility,
                                        "let ",
                                        out);
        }
        i = std::min(fields_end, block_end_exclusive);
        continue;
      }
    } else if (kind == nebula::frontend::TokenKind::KwFor &&
               i + 1 < block_end_exclusive &&
               tokens[i + 1].kind == nebula::frontend::TokenKind::Ident) {
      const std::size_t body_open = find_next_body_lbrace(tokens, i + 2, block_end_exclusive);
      if (body_open < block_end_exclusive) {
        const std::size_t body_end = skip_balanced_tokens(tokens,
                                                          body_open,
                                                          nebula::frontend::TokenKind::LBrace,
                                                          nebula::frontend::TokenKind::RBrace);
        const auto body_visibility =
            span_from_token_range(tokens, body_open, std::min(body_end, block_end_exclusive), source_path);
        push_local_symbol(out,
                          tokens[i + 1].lexeme,
                          "for " + tokens[i + 1].lexeme,
                          tokens[i + 1].span,
                          body_visibility);
        recover_block_local_symbols(tokens, body_open, std::min(body_end, block_end_exclusive), source_path, out);
        i = std::min(body_end, block_end_exclusive);
        continue;
      }
    } else if (kind == nebula::frontend::TokenKind::KwRegion &&
               i + 1 < block_end_exclusive &&
               tokens[i + 1].kind == nebula::frontend::TokenKind::Ident) {
      const std::size_t body_open = find_next_body_lbrace(tokens, i + 2, block_end_exclusive);
      if (body_open < block_end_exclusive) {
        const std::size_t body_end = skip_balanced_tokens(tokens,
                                                          body_open,
                                                          nebula::frontend::TokenKind::LBrace,
                                                          nebula::frontend::TokenKind::RBrace);
        const auto body_visibility =
            span_from_token_range(tokens, body_open, std::min(body_end, block_end_exclusive), source_path);
        push_local_symbol(out,
                          tokens[i + 1].lexeme,
                          "region " + tokens[i + 1].lexeme,
                          tokens[i + 1].span,
                          body_visibility);
        recover_block_local_symbols(tokens, body_open, std::min(body_end, block_end_exclusive), source_path, out);
        i = std::min(body_end, block_end_exclusive);
        continue;
      }
    } else if (kind == nebula::frontend::TokenKind::KwMatch) {
      const std::size_t match_open = find_next_body_lbrace(tokens, i + 1, block_end_exclusive);
      if (match_open < block_end_exclusive) {
        const std::size_t match_end = skip_balanced_tokens(tokens,
                                                           match_open,
                                                           nebula::frontend::TokenKind::LBrace,
                                                           nebula::frontend::TokenKind::RBrace);
        recover_match_local_symbols(tokens, match_open, std::min(match_end, block_end_exclusive), source_path, out);
        i = std::min(match_end, block_end_exclusive);
        continue;
      }
    } else if (kind == nebula::frontend::TokenKind::LBrace) {
      const std::size_t nested_end = skip_balanced_tokens(tokens,
                                                          i,
                                                          nebula::frontend::TokenKind::LBrace,
                                                          nebula::frontend::TokenKind::RBrace);
      recover_block_local_symbols(tokens, i, std::min(nested_end, block_end_exclusive), source_path, out);
      i = std::min(nested_end, block_end_exclusive);
      continue;
    }
    i += 1;
  }
}

std::vector<SymbolDefinition> collect_recovered_current_local_symbols(const SourceFile& source,
                                                                      const std::string& text) {
  std::vector<SymbolDefinition> out;
  Lexer lex(text, source.path);
  auto tokens = lex.lex_all();
  for (std::size_t i = 0; i < tokens.size();) {
    bool handled = false;
    if (tokens[i].kind == nebula::frontend::TokenKind::KwFn &&
        i + 2 < tokens.size() &&
        tokens[i + 1].kind == nebula::frontend::TokenKind::Ident &&
        tokens[i + 2].kind == nebula::frontend::TokenKind::LParen) {
      const std::size_t params_end = skip_balanced_tokens(tokens,
                                                          i + 2,
                                                          nebula::frontend::TokenKind::LParen,
                                                          nebula::frontend::TokenKind::RParen);
      std::size_t body_open = find_next_body_lbrace(tokens, params_end, tokens.size());
      std::size_t body_end = body_open < tokens.size()
                                 ? skip_balanced_tokens(tokens,
                                                        body_open,
                                                        nebula::frontend::TokenKind::LBrace,
                                                        nebula::frontend::TokenKind::RBrace)
                                 : tokens.size();
      const auto visibility =
          body_open < tokens.size()
              ? span_from_token_range(tokens, body_open, std::min(body_end, tokens.size()), source.path)
              : tokens[i + 1].span;

      std::size_t cursor = i + 3;
      while (cursor + 1 < params_end) {
        bool is_ref = false;
        if (tokens[cursor].kind == nebula::frontend::TokenKind::KwRef) {
          is_ref = true;
          cursor += 1;
        }
        if (cursor >= params_end || tokens[cursor].kind != nebula::frontend::TokenKind::Ident) break;
        const auto& name = tokens[cursor];
        std::string detail = "param " + name.lexeme;
        if (cursor + 1 < params_end && tokens[cursor + 1].kind == nebula::frontend::TokenKind::Colon) {
          std::size_t type_end = cursor + 2;
          int angle_depth = 0;
          while (type_end < params_end &&
                 !(angle_depth == 0 &&
                   (tokens[type_end].kind == nebula::frontend::TokenKind::Comma ||
                    tokens[type_end].kind == nebula::frontend::TokenKind::RParen))) {
            if (tokens[type_end].kind == nebula::frontend::TokenKind::Less) angle_depth += 1;
            else if (tokens[type_end].kind == nebula::frontend::TokenKind::Greater) {
              angle_depth = std::max(0, angle_depth - 1);
            }
            type_end += 1;
          }
          const std::size_t slice_start = name.span.start.offset;
          const std::size_t slice_end =
              type_end > cursor ? tokens[type_end - 1].span.end.offset : name.span.end.offset;
          detail = "param " + compact_source_excerpt(text, slice_start, slice_end);
          while (!detail.empty() &&
                 (detail.back() == ')' || detail.back() == ',')) {
            detail.pop_back();
          }
          cursor = type_end;
        } else {
          cursor += 1;
        }
        if (is_ref && detail.rfind("param ", 0) == 0) {
          detail = "param ref " + detail.substr(6);
        }
        push_local_symbol(out, name.lexeme, detail, name.span, visibility);
        if (cursor < params_end && tokens[cursor].kind == nebula::frontend::TokenKind::Comma) cursor += 1;
      }

      if (body_open < tokens.size()) {
        recover_block_local_symbols(tokens, body_open, std::min(body_end, tokens.size()), source.path, out);
      }
      i = std::min(body_end, tokens.size());
      handled = true;
    }
    if (!handled) i += 1;
  }
  return out;
}

std::optional<Program> parse_lsp_program(const std::string& path, const std::string& text);
const LspDocumentState* find_document_state(
    const std::unordered_map<std::string, LspDocumentState>* docs,
    const std::string& path);
const nebula::frontend::SourceFile* find_source_by_path(
    const std::vector<nebula::frontend::SourceFile>& sources,
    const std::string& path);

void append_last_good_current_file_symbols(std::vector<SymbolDefinition>& symbols,
                                           const std::vector<SourceFile>& sources,
                                           const std::string& path,
                                           const std::unordered_map<std::string, LspDocumentState>* docs) {
  const auto* doc = find_document_state(docs, path);
  if (doc == nullptr || doc->last_good_parse_text.empty()) return;
  if (parse_lsp_program(path, doc->text).has_value()) return;
  const auto* source = find_source_by_path(sources, path);
  if (source == nullptr) return;

  auto last_good_symbols = collect_current_file_symbols_from_text(*source, doc->last_good_parse_text);
  symbols.insert(symbols.end(),
                 std::make_move_iterator(last_good_symbols.begin()),
                 std::make_move_iterator(last_good_symbols.end()));
}

void append_recovered_current_file_symbols(std::vector<SymbolDefinition>& symbols,
                                           const std::vector<SourceFile>& sources,
                                           const std::string& path,
                                           const std::string& text,
                                           const std::unordered_map<std::string, LspDocumentState>* docs) {
  if (parse_lsp_program(path, text).has_value()) return;
  const auto* source = find_source_by_path(sources, path);
  if (source == nullptr) return;
  auto recovered = collect_recovered_current_local_symbols(*source, text);
  symbols.insert(symbols.end(),
                 std::make_move_iterator(recovered.begin()),
                 std::make_move_iterator(recovered.end()));
  append_last_good_current_file_symbols(symbols, sources, path, docs);
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

const std::string* find_source_text_by_path(const std::vector<nebula::frontend::SourceFile>& sources,
                                            const std::string& path) {
  if (const auto* source = find_source_by_path(sources, path); source != nullptr) {
    return &source->text;
  }
  return nullptr;
}

const LspDocumentState* find_document_state(
    const std::unordered_map<std::string, LspDocumentState>* docs,
    const std::string& path) {
  if (docs == nullptr) return nullptr;
  auto it = docs->find(normalized_path_string(path));
  if (it == docs->end()) return nullptr;
  return &it->second;
}

const SymbolDefinition* find_symbol_by_qualified_name(const std::vector<SymbolDefinition>& symbols,
                                                      const QualifiedName& qualified_name) {
  for (const auto& symbol : symbols) {
    if (!symbol.qualified_name.has_value()) continue;
    if (*symbol.qualified_name == qualified_name) return &symbol;
  }
  return nullptr;
}

const SymbolDefinition* find_variant_symbol_definition(
    const std::vector<SymbolDefinition>& symbols,
    const QualifiedName& owner_type,
    std::string_view variant_name,
    std::optional<std::uint32_t> variant_index = std::nullopt) {
  for (const auto& symbol : symbols) {
    if (symbol.kind != SymbolKind::EnumVariant) continue;
    if (symbol.name != variant_name) continue;
    if (!symbol.owner_qualified_name.has_value() || *symbol.owner_qualified_name != owner_type) continue;
    if (variant_index.has_value() && symbol.variant_index.has_value() &&
        *symbol.variant_index != *variant_index) {
      continue;
    }
    return &symbol;
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

bool spans_equal(const nebula::frontend::Span& lhs, const nebula::frontend::Span& rhs) {
  return lhs.source_path == rhs.source_path &&
         lhs.start.line == rhs.start.line && lhs.start.col == rhs.start.col &&
         lhs.end.line == rhs.end.line && lhs.end.col == rhs.end.col;
}

template <typename Fn>
void visit_source_exprs(const Expr& expr, Fn&& fn) {
  fn(expr);
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          for (const auto& arg : node.args) visit_source_exprs(*arg, fn);
        } else if constexpr (std::is_same_v<N, Expr::Field>) {
          visit_source_exprs(*node.base, fn);
        } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
          visit_source_exprs(*node.base, fn);
          for (const auto& arg : node.args) visit_source_exprs(*arg, fn);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          visit_source_exprs(*node.lhs, fn);
          visit_source_exprs(*node.rhs, fn);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix> ||
                             std::is_same_v<N, Expr::Try> ||
                             std::is_same_v<N, Expr::Await>) {
          visit_source_exprs(*node.inner, fn);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          visit_source_exprs(*node.subject, fn);
          for (const auto& arm : node.arms) visit_source_exprs(*arm->value, fn);
        }
      },
      expr.node);
}

template <typename Fn>
void visit_source_block_exprs(const Block& block, Fn&& fn);

template <typename Fn>
void visit_source_stmt_exprs(const Stmt& stmt, Fn&& fn) {
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, Stmt::Let>) {
          visit_source_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, Stmt::LetStruct>) {
          visit_source_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, Stmt::Return>) {
          visit_source_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
          visit_source_exprs(*node.expr, fn);
        } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
          visit_source_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
          visit_source_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, Stmt::Region>) {
          visit_source_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
          visit_source_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, Stmt::If>) {
          visit_source_exprs(*node.cond, fn);
          visit_source_block_exprs(node.then_body, fn);
          if (node.else_body.has_value()) visit_source_block_exprs(*node.else_body, fn);
        } else if constexpr (std::is_same_v<N, Stmt::For>) {
          visit_source_exprs(*node.start, fn);
          visit_source_exprs(*node.end, fn);
          visit_source_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, Stmt::While>) {
          visit_source_exprs(*node.cond, fn);
          visit_source_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, Stmt::Match>) {
          visit_source_exprs(*node.subject, fn);
          for (const auto& arm : node.arms) visit_source_block_exprs(arm.body, fn);
        }
      },
      stmt.node);
}

template <typename Fn>
void visit_source_block_exprs(const Block& block, Fn&& fn) {
  for (const auto& stmt : block.stmts) visit_source_stmt_exprs(stmt, fn);
}

template <typename Fn>
void visit_typed_exprs(const TExpr& expr, Fn&& fn) {
  fn(expr);
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, TExpr::Call>) {
          for (const auto& arg : node.args) visit_typed_exprs(*arg, fn);
        } else if constexpr (std::is_same_v<N, TExpr::FieldRef>) {
        } else if constexpr (std::is_same_v<N, TExpr::TempFieldRef>) {
          visit_typed_exprs(*node.base, fn);
        } else if constexpr (std::is_same_v<N, TExpr::EnumIsVariant> ||
                             std::is_same_v<N, TExpr::EnumPayload>) {
          visit_typed_exprs(*node.subject, fn);
        } else if constexpr (std::is_same_v<N, TExpr::Unary> ||
                             std::is_same_v<N, TExpr::Prefix> ||
                             std::is_same_v<N, TExpr::Try> ||
                             std::is_same_v<N, TExpr::Await>) {
          visit_typed_exprs(*node.inner, fn);
        } else if constexpr (std::is_same_v<N, TExpr::Construct>) {
          for (const auto& arg : node.args) visit_typed_exprs(*arg, fn);
        } else if constexpr (std::is_same_v<N, TExpr::Binary>) {
          visit_typed_exprs(*node.lhs, fn);
          visit_typed_exprs(*node.rhs, fn);
        } else if constexpr (std::is_same_v<N, TExpr::Match>) {
          visit_typed_exprs(*node.subject, fn);
          for (const auto& arm : node.arms) visit_typed_exprs(*arm->value, fn);
        }
      },
      expr.node);
}

template <typename Fn>
void visit_typed_block_exprs(const TBlock& block, Fn&& fn);

template <typename Fn>
void visit_typed_stmt_exprs(const TStmt& stmt, Fn&& fn) {
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, TStmt::Declare>) {
        } else if constexpr (std::is_same_v<N, TStmt::Let>) {
          visit_typed_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, TStmt::Return>) {
          visit_typed_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, TStmt::ExprStmt>) {
          visit_typed_exprs(*node.expr, fn);
        } else if constexpr (std::is_same_v<N, TStmt::AssignVar>) {
          visit_typed_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, TStmt::AssignField>) {
          visit_typed_exprs(*node.value, fn);
        } else if constexpr (std::is_same_v<N, TStmt::Region>) {
          visit_typed_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, TStmt::Unsafe>) {
          visit_typed_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, TStmt::If>) {
          visit_typed_exprs(*node.cond, fn);
          visit_typed_block_exprs(node.then_body, fn);
          if (node.else_body.has_value()) visit_typed_block_exprs(*node.else_body, fn);
        } else if constexpr (std::is_same_v<N, TStmt::For>) {
          visit_typed_exprs(*node.start, fn);
          visit_typed_exprs(*node.end, fn);
          visit_typed_block_exprs(node.body, fn);
        } else if constexpr (std::is_same_v<N, TStmt::While>) {
          visit_typed_exprs(*node.cond, fn);
          visit_typed_block_exprs(node.body, fn);
        }
      },
      stmt.node);
}

template <typename Fn>
void visit_typed_block_exprs(const TBlock& block, Fn&& fn) {
  for (const auto& stmt : block.stmts) visit_typed_stmt_exprs(stmt, fn);
}

std::optional<nebula::frontend::LocalBindingId> find_local_binding_id_in_expr(
    const TExpr& expr,
    const SymbolDefinition& symbol) {
  std::optional<nebula::frontend::LocalBindingId> found;
  visit_typed_exprs(expr, [&](const TExpr& nested) {
    if (found.has_value()) return;
    if (auto match = std::get_if<TExpr::Match>(&nested.node); match != nullptr) {
      for (const auto& arm : match->arms) {
        if (arm->payload_binding.has_value() && arm->payload_binding->name == symbol.name &&
            spans_equal(arm->payload_binding->span, symbol.span)) {
          found = arm->payload_binding->binding_id;
          return;
        }
        for (const auto& field : arm->payload_struct_bindings) {
          if (field.binding.name == symbol.name && spans_equal(field.binding.span, symbol.span)) {
            found = field.binding.binding_id;
            return;
          }
        }
      }
    }
  });
  return found;
}

std::optional<nebula::frontend::LocalBindingId> find_local_binding_id_in_block(
    const TBlock& block,
    const SymbolDefinition& symbol) {
  for (const auto& stmt : block.stmts) {
    std::optional<nebula::frontend::LocalBindingId> found;
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, TStmt::Let>) {
            if (node.name == symbol.name && spans_equal(stmt.span, symbol.span)) {
              found = node.binding_id;
              return;
            }
            found = find_local_binding_id_in_expr(*node.value, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::Return>) {
            found = find_local_binding_id_in_expr(*node.value, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::ExprStmt>) {
            found = find_local_binding_id_in_expr(*node.expr, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::AssignVar>) {
            found = find_local_binding_id_in_expr(*node.value, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::AssignField>) {
            found = find_local_binding_id_in_expr(*node.value, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::Region>) {
            found = find_local_binding_id_in_block(node.body, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::Unsafe>) {
            found = find_local_binding_id_in_block(node.body, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::If>) {
            found = find_local_binding_id_in_expr(*node.cond, symbol);
            if (!found.has_value()) found = find_local_binding_id_in_block(node.then_body, symbol);
            if (!found.has_value() && node.else_body.has_value()) {
              found = find_local_binding_id_in_block(*node.else_body, symbol);
            }
          } else if constexpr (std::is_same_v<N, TStmt::For>) {
            if (node.var == symbol.name && spans_equal(stmt.span, symbol.span)) {
              found = node.binding_id;
              return;
            }
            found = find_local_binding_id_in_expr(*node.start, symbol);
            if (!found.has_value()) found = find_local_binding_id_in_expr(*node.end, symbol);
            if (!found.has_value()) found = find_local_binding_id_in_block(node.body, symbol);
          } else if constexpr (std::is_same_v<N, TStmt::While>) {
            found = find_local_binding_id_in_expr(*node.cond, symbol);
            if (!found.has_value()) found = find_local_binding_id_in_block(node.body, symbol);
          }
        },
        stmt.node);
    if (found.has_value()) return found;
  }
  return std::nullopt;
}

std::optional<nebula::frontend::LocalBindingId> find_local_binding_id_for_symbol(
    const TProgram& program,
    const SymbolDefinition& symbol) {
  if (!symbol.local) return std::nullopt;
  for (const auto& item : program.items) {
    if (!std::holds_alternative<TFunction>(item.node)) continue;
    const auto& fn = std::get<TFunction>(item.node);
    for (const auto& param : fn.params) {
      if (param.name == symbol.name && spans_equal(param.span, symbol.span)) {
        return param.binding_id;
      }
    }
    if (!fn.body.has_value()) continue;
    if (auto found = find_local_binding_id_in_block(*fn.body, symbol); found.has_value()) return found;
  }
  return std::nullopt;
}

void append_local_reference_locations(cli_json::Value::Array& out,
                                      std::set<std::tuple<std::string, int, int, int, int>>& seen,
                                      const TProgram& program,
                                      const std::string& path,
                                      const std::string& text,
                                      nebula::frontend::LocalBindingId binding_id) {
  if (binding_id == nebula::frontend::kInvalidLocalBindingId) return;
  auto append_span = [&](const nebula::frontend::Span& span) {
    const auto key = std::make_tuple(path, span.start.line, span.start.col, span.end.line, span.end.col);
    if (!seen.insert(key).second) return;
    cli_json::Value::Object item;
    item["uri"] = cli_json::Value(path_to_uri(path));
    item["range"] = json_range_value(
        std::max(0, span.start.line - 1),
        source_col_one_based_to_lsp_character(text, span.start.line, span.start.col),
        std::max(0, span.end.line - 1),
        source_col_one_based_to_lsp_character(text, span.end.line, span.end.col));
    out.push_back(cli_json::Value(std::move(item)));
  };
  auto append_stmt = [&](const TStmt& stmt, auto&& self) -> void {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, TStmt::AssignVar>) {
            if (node.binding_id == binding_id) append_span(stmt.span);
          } else if constexpr (std::is_same_v<N, TStmt::AssignField>) {
            if (node.base_binding_id == binding_id) append_span(stmt.span);
          } else if constexpr (std::is_same_v<N, TStmt::Region>) {
            for (const auto& nested : node.body.stmts) self(nested, self);
          } else if constexpr (std::is_same_v<N, TStmt::Unsafe>) {
            for (const auto& nested : node.body.stmts) self(nested, self);
          } else if constexpr (std::is_same_v<N, TStmt::If>) {
            for (const auto& nested : node.then_body.stmts) self(nested, self);
            if (node.else_body.has_value()) {
              for (const auto& nested : node.else_body->stmts) self(nested, self);
            }
          } else if constexpr (std::is_same_v<N, TStmt::For>) {
            for (const auto& nested : node.body.stmts) self(nested, self);
          } else if constexpr (std::is_same_v<N, TStmt::While>) {
            for (const auto& nested : node.body.stmts) self(nested, self);
          }
        },
        stmt.node);

    visit_typed_stmt_exprs(stmt, [&](const TExpr& expr) {
      std::visit(
          [&](auto&& expr_node) {
            using E = std::decay_t<decltype(expr_node)>;
            if constexpr (std::is_same_v<E, TExpr::VarRef>) {
              if (expr_node.binding_id == binding_id) append_span(expr.span);
            } else if constexpr (std::is_same_v<E, TExpr::FieldRef>) {
              if (expr_node.base_binding_id == binding_id) append_span(expr.span);
            } else if constexpr (std::is_same_v<E, TExpr::Call>) {
              if (expr_node.kind == TExpr::CallKind::Indirect &&
                  expr_node.callee_binding_id == binding_id) {
                append_span(expr.span);
              }
            }
          },
          expr.node);
    });
  };

  for (const auto& item : program.items) {
    if (!std::holds_alternative<TFunction>(item.node)) continue;
    const auto& fn = std::get<TFunction>(item.node);
    if (!fn.body.has_value()) continue;
    for (const auto& stmt : fn.body->stmts) append_stmt(stmt, append_stmt);
  }
}

using AsyncExplainEntry = nebula::passes::AsyncExplainEntry;

struct AsyncFunctionFacts {
  std::string function_name;
  std::string path;
  nebula::frontend::Span function_span{};
  nebula::frontend::Span body_span{};
  bool is_async = false;
  std::vector<nebula::frontend::Span> await_spans;
  std::vector<std::pair<std::string, nebula::frontend::Span>> call_spans;
  std::vector<std::pair<std::string, nebula::frontend::Span>> declarations;
  std::vector<std::pair<std::string, nebula::frontend::Span>> references;
};

void collect_async_expr_facts(const Expr& expr, AsyncFunctionFacts& facts);

void collect_async_block_facts(const Block& block, AsyncFunctionFacts& facts) {
  for (const auto& stmt : block.stmts) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            facts.declarations.push_back({node.name, stmt.span});
            collect_async_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::LetStruct>) {
            for (const auto& field : node.fields) {
              if (!field.skip && field.binding_name.has_value()) {
                facts.declarations.push_back({*field.binding_name, field.binding_span});
              }
            }
            collect_async_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            collect_async_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            collect_async_expr_facts(*node.expr, facts);
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            facts.references.push_back({node.name, stmt.span});
            collect_async_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            facts.references.push_back({node.base, stmt.span});
            collect_async_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            collect_async_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            collect_async_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            collect_async_expr_facts(*node.cond, facts);
            collect_async_block_facts(node.then_body, facts);
            if (node.else_body.has_value()) collect_async_block_facts(*node.else_body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            facts.declarations.push_back({node.var, stmt.span});
            collect_async_expr_facts(*node.start, facts);
            collect_async_expr_facts(*node.end, facts);
            collect_async_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            collect_async_expr_facts(*node.cond, facts);
            collect_async_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Match>) {
            collect_async_expr_facts(*node.subject, facts);
            for (const auto& arm : node.arms) {
              if (std::holds_alternative<Pattern::Variant>(arm.pattern.node)) {
                const auto& variant = std::get<Pattern::Variant>(arm.pattern.node);
                if (variant.payload.has_value()) {
                  std::visit(
                      [&](auto&& payload) {
                        using P = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<P, Pattern::Variant::BindingPayload>) {
                          facts.declarations.push_back({payload.name, payload.binding_span});
                        } else if constexpr (std::is_same_v<P, Pattern::Variant::StructPayload>) {
                          for (const auto& field : payload.fields) {
                            if (!field.skip && field.binding_name.has_value()) {
                              facts.declarations.push_back({*field.binding_name, field.binding_span});
                            }
                          }
                        }
                      },
                      *variant.payload);
                }
              }
              collect_async_block_facts(arm.body, facts);
            }
          }
        },
        stmt.node);
  }
}

void collect_async_expr_facts(const Expr& expr, AsyncFunctionFacts& facts) {
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, Expr::Name>) {
          facts.references.push_back({node.ident, expr.span});
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          facts.call_spans.push_back({node.callee, expr.span});
          for (const auto& arg : node.args) collect_async_expr_facts(*arg, facts);
        } else if constexpr (std::is_same_v<N, Expr::Field>) {
          collect_async_expr_facts(*node.base, facts);
        } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
          collect_async_expr_facts(*node.base, facts);
          for (const auto& arg : node.args) collect_async_expr_facts(*arg, facts);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_async_expr_facts(*node.lhs, facts);
          collect_async_expr_facts(*node.rhs, facts);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix> ||
                             std::is_same_v<N, Expr::Try>) {
          collect_async_expr_facts(*node.inner, facts);
        } else if constexpr (std::is_same_v<N, Expr::Await>) {
          facts.await_spans.push_back(expr.span);
          collect_async_expr_facts(*node.inner, facts);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          collect_async_expr_facts(*node.subject, facts);
          for (const auto& arm : node.arms) collect_async_expr_facts(*arm->value, facts);
        }
      },
      expr.node);
}

std::vector<std::string> carried_values_after_await(const AsyncFunctionFacts& facts,
                                                    const nebula::frontend::Span& await_span) {
  std::set<std::string> carried;
  for (const auto& [name, decl_span] : facts.declarations) {
    if (decl_span.start.offset >= await_span.start.offset) continue;
    const bool referenced_after =
        std::any_of(facts.references.begin(), facts.references.end(), [&](const auto& ref) {
          return ref.first == name && ref.second.start.offset > await_span.end.offset;
        });
    if (referenced_after) carried.insert(name);
  }
  return std::vector<std::string>(carried.begin(), carried.end());
}

std::vector<AsyncExplainEntry> collect_async_explain_entries_for_source(const SourceFile& source) {
  std::vector<AsyncExplainEntry> out;
  const auto program = parse_lsp_program(source.path, source.text);
  if (!program.has_value()) return out;

  for (const auto& item : program->items) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (!std::is_same_v<N, Function>) {
            return;
          } else {
            if (!node.body.has_value()) return;
            AsyncFunctionFacts facts;
            facts.function_name = node.name;
            facts.path = source.path;
            facts.function_span = node.span;
            facts.body_span = node.body->span;
            facts.is_async = node.is_async;
            for (const auto& param : node.params) facts.declarations.push_back({param.name, param.span});
            collect_async_block_facts(*node.body, facts);

            if (facts.is_async) {
              std::set<std::string> carried_all;
              for (const auto& await_span : facts.await_spans) {
                for (const auto& name : carried_values_after_await(facts, await_span)) {
                  carried_all.insert(name);
                }
              }
              AsyncExplainEntry entry;
              entry.kind = "function";
              entry.function_name = facts.function_name;
              entry.path = facts.path;
              entry.span = facts.function_span;
              entry.summary = facts.await_spans.empty()
                                  ? "async function with no current suspension point"
                                  : "async function with " + std::to_string(facts.await_spans.size()) +
                                        " suspension point(s)";
              entry.allocation = facts.await_spans.empty() ? "unknown" : "async-frame";
              entry.reason = facts.await_spans.empty()
                                 ? "the function is async, but this body does not currently suspend"
                                 : "await keeps the frame resumable and may keep later-used values live across suspension";
              entry.carried_values.assign(carried_all.begin(), carried_all.end());
              out.push_back(std::move(entry));
            }

            for (const auto& await_span : facts.await_spans) {
              AsyncExplainEntry entry;
              entry.kind = "await";
              entry.function_name = facts.function_name;
              entry.path = facts.path;
              entry.span = await_span;
              entry.summary = "await suspension point";
              entry.allocation = "async-frame";
              entry.reason =
                  "await suspends the async frame; values referenced later remain live until resume";
              entry.carried_values = carried_values_after_await(facts, await_span);
              entry.suspension_point = true;
              out.push_back(std::move(entry));
            }

            for (const auto& [call_name, call_span] : facts.call_spans) {
              if (!(call_name == "spawn" || call_name == "join" || call_name == "timeout")) continue;
              AsyncExplainEntry entry;
              entry.kind = call_name;
              entry.function_name = facts.function_name;
              entry.path = facts.path;
              entry.span = call_span;
              entry.task_boundary = true;
              if (call_name == "spawn") {
                entry.summary = "spawn task boundary";
                entry.allocation = "task-state";
                entry.reason = "spawn transfers the future into task-owned state";
              } else if (call_name == "join") {
                entry.summary = "join task boundary";
                entry.allocation = "existing-task-state";
                entry.reason = "join observes task-owned state produced earlier by spawn";
              } else {
                entry.summary = "timeout task boundary";
                entry.allocation = "task-state";
                entry.reason = "timeout creates a task boundary internally to race the future against a deadline";
              }
              out.push_back(std::move(entry));
            }
          }
        },
        item.node);
  }

  std::sort(out.begin(), out.end(), [](const AsyncExplainEntry& lhs, const AsyncExplainEntry& rhs) {
    if (lhs.span.source_path != rhs.span.source_path) return lhs.span.source_path < rhs.span.source_path;
    if (lhs.span.start.line != rhs.span.start.line) return lhs.span.start.line < rhs.span.start.line;
    if (lhs.span.start.col != rhs.span.start.col) return lhs.span.start.col < rhs.span.start.col;
    if (lhs.span.end.line != rhs.span.end.line) return lhs.span.end.line < rhs.span.end.line;
    return lhs.span.end.col < rhs.span.end.col;
  });
  return out;
}

std::vector<AsyncExplainEntry> collect_async_explain_entries(const std::vector<SourceFile>& sources) {
  std::vector<AsyncExplainEntry> out;
  for (const auto& source : sources) {
    auto entries = collect_async_explain_entries_for_source(source);
    out.insert(out.end(),
               std::make_move_iterator(entries.begin()),
               std::make_move_iterator(entries.end()));
  }
  return out;
}

struct TransportExplainEntry {
  std::string kind;
  std::string symbol_name;
  std::string surface_name;
  std::string enclosing_function;
  std::string path;
  nebula::frontend::Span span{};
  std::string summary;
};

struct TransportExplainSurfaceSpec {
  std::string_view kind;
  std::string_view symbol_name;
  std::string_view method_name;
  std::string_view surface_name;
  std::string_view summary;
};

const TransportExplainSurfaceSpec* transport_surface_spec_for_symbol(std::string_view symbol_name) {
  static constexpr std::array<TransportExplainSurfaceSpec, 3> kSpecs{{
      {"transport_debug",
       "RequestContext_transport_debug",
       "transport_debug",
       "RequestContext.transport_debug()",
       "structured transport debug snapshot that carries ALPN, peer identity, and HTTP/2 state"},
      {"tls_peer_identity_debug",
       "RequestContext_tls_peer_identity_debug",
       "tls_peer_identity_debug",
       "RequestContext.tls_peer_identity_debug()",
       "TLS peer certificate debug snapshot with SAN claims and certificate-chain details"},
      {"http2_debug_state",
       "RequestContext_http2_debug_state",
       "http2_debug_state",
       "RequestContext.http2_debug_state()",
       "HTTP/2 debug snapshot with flow-control, GOAWAY, reset, and recent frame timeline state"},
  }};
  const auto it = std::find_if(kSpecs.begin(), kSpecs.end(), [&](const auto& spec) {
    return spec.symbol_name == symbol_name;
  });
  return it == kSpecs.end() ? nullptr : &*it;
}

const TransportExplainSurfaceSpec* transport_surface_spec_for_method(std::string_view method_name) {
  static constexpr std::array<TransportExplainSurfaceSpec, 3> kSpecs{{
      {"transport_debug",
       "RequestContext_transport_debug",
       "transport_debug",
       "RequestContext.transport_debug()",
       "structured transport debug snapshot that carries ALPN, peer identity, and HTTP/2 state"},
      {"tls_peer_identity_debug",
       "RequestContext_tls_peer_identity_debug",
       "tls_peer_identity_debug",
       "RequestContext.tls_peer_identity_debug()",
       "TLS peer certificate debug snapshot with SAN claims and certificate-chain details"},
      {"http2_debug_state",
       "RequestContext_http2_debug_state",
       "http2_debug_state",
       "RequestContext.http2_debug_state()",
       "HTTP/2 debug snapshot with flow-control, GOAWAY, reset, and recent frame timeline state"},
  }};
  const auto it = std::find_if(kSpecs.begin(), kSpecs.end(), [&](const auto& spec) {
    return spec.method_name == method_name;
  });
  return it == kSpecs.end() ? nullptr : &*it;
}

cli_json::Value transport_identity_debug_contract_json() {
  cli_json::Value::Object validity;
  validity["not_before"] = cli_json::Value("2026-04-21T00:00:00Z");
  validity["not_after"] = cli_json::Value("2026-04-23T00:00:00Z");

  cli_json::Value::Object san_claims;
  san_claims["dns_names"] = cli_json::Value(cli_json::Value::Array{cli_json::Value("client.local")});
  san_claims["ip_addresses"] = cli_json::Value(cli_json::Value::Array{cli_json::Value("10.1.2.3")});
  san_claims["email_addresses"] =
      cli_json::Value(cli_json::Value::Array{cli_json::Value("client@example.com")});
  san_claims["uris"] =
      cli_json::Value(cli_json::Value::Array{cli_json::Value("spiffe://nebula/client")});

  cli_json::Value::Object chain_leaf;
  chain_leaf["index"] = json_i64(0);
  chain_leaf["subject"] = cli_json::Value("CN=nebula-client");
  chain_leaf["issuer"] = cli_json::Value("CN=Nebula Test CA");
  chain_leaf["serial_hex"] = cli_json::Value("00AB12CD");
  chain_leaf["validity"] = cli_json::Value(validity);
  chain_leaf["fingerprint_sha256"] = cli_json::Value("<sha256-hex>");
  chain_leaf["san_claims"] = cli_json::Value(san_claims);

  cli_json::Value::Array chain;
  chain.push_back(cli_json::Value(std::move(chain_leaf)));

  cli_json::Value::Object identity;
  identity["present"] = cli_json::Value(true);
  identity["verified"] = cli_json::Value(true);
  identity["subject"] = cli_json::Value("CN=nebula-client");
  identity["fingerprint_sha256"] = cli_json::Value("<sha256-hex>");
  identity["san_claims"] = cli_json::Value(san_claims);
  identity["chain"] = cli_json::Value(std::move(chain));
  return cli_json::Value(std::move(identity));
}

cli_json::Value transport_http2_timeline_contract_json() {
  auto make_flags = [](std::int64_t raw, bool ack, bool end_headers, bool end_stream) {
    cli_json::Value::Object flags;
    flags["raw"] = json_i64(raw);
    flags["ack"] = cli_json::Value(ack);
    flags["end_headers"] = cli_json::Value(end_headers);
    flags["end_stream"] = cli_json::Value(end_stream);
    return cli_json::Value(std::move(flags));
  };
  auto make_flow = [](std::int64_t connection_window,
                      std::int64_t stream_window,
                      std::int64_t error_code,
                      const std::string& error_name) {
    cli_json::Value::Object flow;
    flow["connection_window"] = json_i64(connection_window);
    flow["stream_window"] = json_i64(stream_window);
    flow["error_code"] = json_i64(error_code);
    flow["error_name"] = cli_json::Value(error_name);
    return cli_json::Value(std::move(flow));
  };
  auto make_event = [&](std::int64_t seq,
                        const std::string& kind,
                        const std::string& direction,
                        const std::string& frame_type,
                        std::int64_t stream_id,
                        std::optional<std::pair<std::string, std::string>> classification,
                        cli_json::Value flags,
                        cli_json::Value flow_control) {
    cli_json::Value::Object event;
    event["seq"] = json_i64(seq);
    event["kind"] = cli_json::Value(kind);
    event["direction"] = cli_json::Value(direction);
    event["frame_type"] = cli_json::Value(frame_type);
    event["stream_id"] = json_i64(stream_id);
    if (classification.has_value()) {
      cli_json::Value::Object meaning;
      meaning["reason"] = cli_json::Value(classification->first);
      meaning["detail"] = cli_json::Value(classification->second);
      event["classification"] = cli_json::Value(std::move(meaning));
    }
    event["flags"] = std::move(flags);
    event["flow_control"] = std::move(flow_control);
    return cli_json::Value(std::move(event));
  };

  cli_json::Value::Array events;
  events.push_back(make_event(12,
                              "frame",
                              "outbound",
                              "HEADERS",
                              1,
                              std::nullopt,
                              make_flags(4, false, true, false),
                              make_flow(65535, 65535, 0, "NO_ERROR")));
  events.push_back(make_event(13,
                              "stream_open",
                              "outbound",
                              "HEADERS",
                              1,
                              std::make_pair(std::string("request_start"),
                                             std::string("local_request_headers_sent")),
                              make_flags(0, false, false, false),
                              make_flow(65535, 65535, 0, "NO_ERROR")));
  events.push_back(make_event(14,
                              "frame",
                              "outbound",
                              "DATA",
                              1,
                              std::nullopt,
                              make_flags(1, false, false, true),
                              make_flow(65531, 65531, 0, "NO_ERROR")));
  events.push_back(make_event(15,
                              "response_headers_seen",
                              "inbound",
                              "HEADERS",
                              1,
                              std::make_pair(std::string("response_start"),
                                             std::string("peer_final_response_headers_seen")),
                              make_flags(4, false, true, false),
                              make_flow(65531, 65531, 0, "NO_ERROR")));
  events.push_back(make_event(16,
                              "frame",
                              "inbound",
                              "WINDOW_UPDATE",
                              1,
                              std::nullopt,
                              make_flags(0, false, false, false),
                              make_flow(131071, 98303, 0, "NO_ERROR")));
  events.push_back(make_event(17,
                              "goaway_observed",
                              "inbound",
                              "GOAWAY",
                              0,
                              std::make_pair(std::string("shutdown"),
                                             std::string("peer_goaway_no_error")),
                              make_flags(0, false, false, false),
                              make_flow(131071, 98303, 0, "NO_ERROR")));
  events.push_back(make_event(18,
                              "stream_closed",
                              "inbound",
                              "DATA",
                              1,
                              std::make_pair(std::string("response_complete"),
                                             std::string("data_end_stream")),
                              make_flags(0, false, false, false),
                              make_flow(131071, 98303, 0, "NO_ERROR")));
  events.push_back(make_event(19,
                              "frame",
                              "inbound",
                              "GOAWAY",
                              0,
                              std::nullopt,
                              make_flags(0, false, false, false),
                              make_flow(131071, 98303, 0, "NO_ERROR")));

  cli_json::Value::Object timeline;
  timeline["overflowed"] = cli_json::Value(false);
  timeline["recent_events"] = cli_json::Value(std::move(events));
  return cli_json::Value(std::move(timeline));
}

cli_json::Value transport_http2_debug_contract_json() {
  cli_json::Value::Object preface;
  preface["client_preface_sent"] = cli_json::Value(true);
  preface["client_preface_received"] = cli_json::Value(true);

  cli_json::Value::Object settings;
  settings["local_settings_sent"] = cli_json::Value(true);
  settings["local_settings_acknowledged"] = cli_json::Value(true);
  settings["remote_settings_seen"] = cli_json::Value(true);
  settings["peer_max_concurrent_streams"] = json_i64(100);

  cli_json::Value::Object streams;
  streams["next_local_stream_id"] = json_i64(3);
  streams["active_stream_id"] = json_i64(1);
  streams["last_peer_stream_id"] = json_i64(1);

  cli_json::Value::Object flow_control;
  flow_control["connection_window"] = json_i64(65535);
  flow_control["stream_window"] = json_i64(65535);
  flow_control["initial_stream_window"] = json_i64(65535);
  flow_control["max_frame_size"] = json_i64(16384);

  cli_json::Value::Object goaway;
  goaway["sent"] = cli_json::Value(false);
  goaway["received"] = cli_json::Value(false);
  goaway["last_stream_id"] = json_i64(0);
  goaway["error_code"] = json_i64(0);
  goaway["error_name"] = cli_json::Value("NO_ERROR");

  cli_json::Value::Object reset;
  reset["stream_id"] = json_i64(0);
  reset["error_code"] = json_i64(0);
  reset["error_name"] = cli_json::Value("NO_ERROR");

  cli_json::Value::Object http2;
  http2["preface"] = cli_json::Value(std::move(preface));
  http2["settings"] = cli_json::Value(std::move(settings));
  http2["streams"] = cli_json::Value(std::move(streams));
  http2["flow_control"] = cli_json::Value(std::move(flow_control));
  http2["goaway"] = cli_json::Value(std::move(goaway));
  http2["reset"] = cli_json::Value(std::move(reset));
  http2["timeline"] = transport_http2_timeline_contract_json();
  return cli_json::Value(std::move(http2));
}

cli_json::Value transport_debug_contract_json(std::string_view kind) {
  if (kind == "tls_peer_identity_debug") {
    return transport_identity_debug_contract_json();
  }
  if (kind == "http2_debug_state") {
    return transport_http2_debug_contract_json();
  }

  cli_json::Value::Object transport;
  transport["alpn_protocol"] = cli_json::Value("h2");
  transport["tls_peer_identity"] = transport_identity_debug_contract_json();
  transport["http2"] = transport_http2_debug_contract_json();
  return cli_json::Value(std::move(transport));
}

std::vector<TransportExplainEntry> collect_transport_explain_entries_for_source(const SourceFile& source) {
  std::vector<TransportExplainEntry> out;
  const auto program = parse_lsp_program(source.path, source.text);
  if (!program.has_value()) return out;

  for (const auto& item : program->items) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (!std::is_same_v<N, Function>) {
            return;
          } else {
            if (const auto* spec = transport_surface_spec_for_symbol(node.name); spec != nullptr) {
              TransportExplainEntry entry;
              entry.kind = std::string(spec->kind);
              entry.symbol_name = std::string(spec->symbol_name);
              entry.surface_name = std::string(spec->surface_name);
              entry.enclosing_function = node.name;
              entry.path = source.path;
              entry.span = node.span;
              entry.summary = std::string(spec->summary);
              out.push_back(std::move(entry));
            }
            if (!node.body.has_value()) return;
            visit_source_block_exprs(*node.body, [&](const Expr& expr) {
              std::visit(
                  [&](auto&& expr_node) {
                    using E = std::decay_t<decltype(expr_node)>;
                    if constexpr (std::is_same_v<E, Expr::MethodCall>) {
                      if (const auto* spec =
                              transport_surface_spec_for_method(expr_node.method);
                          spec != nullptr) {
                        TransportExplainEntry entry;
                        entry.kind = std::string(spec->kind);
                        entry.symbol_name = std::string(spec->symbol_name);
                        entry.surface_name = std::string(spec->surface_name);
                        entry.enclosing_function = node.name;
                        entry.path = source.path;
                        entry.span = expr.span;
                        entry.summary = std::string(spec->summary);
                        out.push_back(std::move(entry));
                      }
                    } else if constexpr (std::is_same_v<E, Expr::Call>) {
                      if (const auto* spec =
                              transport_surface_spec_for_symbol(expr_node.callee);
                          spec != nullptr) {
                        TransportExplainEntry entry;
                        entry.kind = std::string(spec->kind);
                        entry.symbol_name = std::string(spec->symbol_name);
                        entry.surface_name = std::string(spec->surface_name);
                        entry.enclosing_function = node.name;
                        entry.path = source.path;
                        entry.span = expr.span;
                        entry.summary = std::string(spec->summary);
                        out.push_back(std::move(entry));
                      }
                    }
                  },
                  expr.node);
            });
          }
        },
        item.node);
  }

  std::sort(out.begin(), out.end(), [](const TransportExplainEntry& lhs,
                                       const TransportExplainEntry& rhs) {
    if (lhs.path != rhs.path) return lhs.path < rhs.path;
    if (lhs.span.start.line != rhs.span.start.line) return lhs.span.start.line < rhs.span.start.line;
    if (lhs.span.start.col != rhs.span.start.col) return lhs.span.start.col < rhs.span.start.col;
    if (lhs.span.end.line != rhs.span.end.line) return lhs.span.end.line < rhs.span.end.line;
    return lhs.span.end.col < rhs.span.end.col;
  });
  return out;
}

std::vector<TransportExplainEntry> collect_transport_explain_entries(
    const std::vector<SourceFile>& sources) {
  std::vector<TransportExplainEntry> out;
  for (const auto& source : sources) {
    auto entries = collect_transport_explain_entries_for_source(source);
    out.insert(out.end(),
               std::make_move_iterator(entries.begin()),
               std::make_move_iterator(entries.end()));
  }
  return out;
}

std::string transport_entry_markdown(const TransportExplainEntry& entry) {
  std::ostringstream md;
  md << "**transport " << lsp_markdown_escape(entry.kind) << "**";
  md << "\nsurface: `" << entry.surface_name << "`";
  if (!entry.enclosing_function.empty() && entry.enclosing_function != entry.symbol_name) {
    md << "\nfunction: " << lsp_markdown_escape(entry.enclosing_function);
  }
  if (!entry.summary.empty()) {
    md << "\nsummary: " << lsp_markdown_escape(entry.summary);
  }
  md << "\n```json\n"
     << cli_json::render_compact(transport_debug_contract_json(entry.kind))
     << "\n```";
  return md.str();
}

std::optional<TransportExplainEntry> transport_entry_at_position(
    const std::vector<TransportExplainEntry>& entries,
    const std::string& path,
    int line_zero_based,
    int char_zero_based) {
  std::optional<TransportExplainEntry> best;
  const std::string requested_path = normalized_path_string(path);
  for (const auto& entry : entries) {
    if (normalized_path_string(entry.path) != requested_path) continue;
    if (!lsp_position_matches(entry.span, line_zero_based, char_zero_based)) continue;
    if (!best.has_value() ||
        (best->span.end.offset - best->span.start.offset) >
            (entry.span.end.offset - entry.span.start.offset)) {
      best = entry;
    }
  }
  return best;
}

void append_hover_markdown_section(std::string& out, const std::string& section) {
  if (section.empty()) return;
  if (!out.empty()) out += "\n\n";
  out += section;
}

std::string async_entry_markdown(const AsyncExplainEntry& entry) {
  std::ostringstream md;
  md << "**async " << lsp_markdown_escape(entry.kind) << "**";
  if (!entry.function_name.empty()) {
    md << "\\nfunction: " << lsp_markdown_escape(entry.function_name);
  }
  if (!entry.summary.empty()) {
    md << "\\nsummary: " << lsp_markdown_escape(entry.summary);
  }
  if (!entry.allocation.empty()) {
    md << "\\nallocation: " << lsp_markdown_escape(entry.allocation);
  }
  if (!entry.reason.empty()) {
    md << "\\nreason: " << lsp_markdown_escape(entry.reason);
  }
  if (!entry.carried_values.empty()) {
    md << "\\ncarried: ";
    for (std::size_t i = 0; i < entry.carried_values.size(); ++i) {
      if (i) md << ", ";
      md << lsp_markdown_escape(entry.carried_values[i]);
    }
  }
  return md.str();
}

std::optional<AsyncExplainEntry> async_entry_at_position(const std::vector<AsyncExplainEntry>& entries,
                                                         const std::string& path,
                                                         int line_zero_based,
                                                         int char_zero_based) {
  std::optional<AsyncExplainEntry> best;
  const std::string requested_path = normalized_path_string(path);
  for (const auto& entry : entries) {
    if (normalized_path_string(entry.path) != requested_path) continue;
    if (!lsp_position_matches(entry.span, line_zero_based, char_zero_based)) continue;
    if (!best.has_value() ||
        (best->span.end.offset - best->span.start.offset) >
            (entry.span.end.offset - entry.span.start.offset)) {
      best = entry;
    }
  }
  return best;
}

const TExpr* find_typed_expr_by_span(const TProgram& program, const nebula::frontend::Span& span) {
  const TExpr* found = nullptr;
  for (const auto& item : program.items) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, TFunction>) {
            if (!node.body.has_value()) return;
            visit_typed_block_exprs(*node.body, [&](const TExpr& expr) {
              if (found == nullptr && spans_equal(expr.span, span)) found = &expr;
            });
          }
        },
        item.node);
    if (found != nullptr) break;
  }
  return found;
}

const TProgram* find_typed_program_by_path(const std::vector<SourceFile>& sources,
                                           const std::vector<TProgram>& programs,
                                           const std::string& path) {
  const std::string requested = normalized_path_string(path);
  const std::size_t count = std::min(sources.size(), programs.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (normalized_path_string(sources[i].path) == requested) return &programs[i];
  }
  return nullptr;
}

std::optional<Program> parse_lsp_program(const std::string& path, const std::string& text) {
  try {
    Lexer lex(text, path);
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    return parser.parse_program();
  } catch (const nebula::frontend::FrontendError&) {
    return std::nullopt;
  }
}

struct SemanticReferenceSite {
  enum class Kind : std::uint8_t { Name, CallCallee, Method, PatternVariant };

  Kind kind = Kind::Name;
  nebula::frontend::Span token_span{};
  nebula::frontend::Span expr_span{};
};

std::optional<SemanticReferenceSite> reference_site_at_position(const Program& program,
                                                                const std::string& ident,
                                                                int line_zero_based,
                                                                int char_zero_based) {
  std::optional<SemanticReferenceSite> best;
  auto consider = [&](SemanticReferenceSite::Kind kind,
                      const nebula::frontend::Span& token_span,
                      const nebula::frontend::Span& expr_span) {
    if (!lsp_position_matches(token_span, line_zero_based, char_zero_based)) return;
    if (!best.has_value() ||
        (best->token_span.end.offset - best->token_span.start.offset) >
            (token_span.end.offset - token_span.start.offset)) {
      best = SemanticReferenceSite{kind, token_span, expr_span};
    }
  };

  std::function<void(const Block&)> scan_block;
  std::function<void(const Stmt&)> scan_stmt;
  auto scan_expr = [&](const Expr& expr) {
    std::visit(
        [&](auto&& expr_node) {
          using E = std::decay_t<decltype(expr_node)>;
          if constexpr (std::is_same_v<E, Expr::Name>) {
            if (expr_node.ident == ident) {
              consider(SemanticReferenceSite::Kind::Name, expr.span, expr.span);
            }
          } else if constexpr (std::is_same_v<E, Expr::Call>) {
            if (expr_node.callee == ident) {
              consider(SemanticReferenceSite::Kind::CallCallee, expr_node.callee_span, expr.span);
            }
          } else if constexpr (std::is_same_v<E, Expr::MethodCall>) {
            if (expr_node.method == ident) {
              consider(SemanticReferenceSite::Kind::Method, expr_node.method_span, expr.span);
            }
          } else if constexpr (std::is_same_v<E, Expr::Match>) {
            for (const auto& arm : expr_node.arms) {
              if (std::holds_alternative<Pattern::Variant>(arm->pattern.node)) {
                const auto& variant = std::get<Pattern::Variant>(arm->pattern.node);
                if (variant.name == ident) {
                  consider(SemanticReferenceSite::Kind::PatternVariant,
                           variant.name_span,
                           expr_node.subject->span);
                }
              }
            }
          }
        },
        expr.node);
  };

  scan_block = [&](const Block& block) {
    for (const auto& stmt : block.stmts) scan_stmt(stmt);
  };
  scan_stmt = [&](const Stmt& stmt) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            visit_source_exprs(*node.value, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::LetStruct>) {
            visit_source_exprs(*node.value, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            visit_source_exprs(*node.value, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            visit_source_exprs(*node.expr, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            visit_source_exprs(*node.value, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            visit_source_exprs(*node.value, scan_expr);
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            scan_block(node.body);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            scan_block(node.body);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            visit_source_exprs(*node.cond, scan_expr);
            scan_block(node.then_body);
            if (node.else_body.has_value()) scan_block(*node.else_body);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            visit_source_exprs(*node.start, scan_expr);
            visit_source_exprs(*node.end, scan_expr);
            scan_block(node.body);
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            visit_source_exprs(*node.cond, scan_expr);
            scan_block(node.body);
          } else if constexpr (std::is_same_v<N, Stmt::Match>) {
            visit_source_exprs(*node.subject, scan_expr);
            for (const auto& arm : node.arms) {
              if (std::holds_alternative<Pattern::Variant>(arm.pattern.node)) {
                const auto& variant = std::get<Pattern::Variant>(arm.pattern.node);
                if (variant.name == ident) {
                  consider(SemanticReferenceSite::Kind::PatternVariant,
                           variant.name_span,
                           node.subject->span);
                }
              }
              scan_block(arm.body);
            }
          }
        },
        stmt.node);
  };

  for (const auto& item : program.items) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Function>) {
            if (!node.body.has_value()) return;
            scan_block(*node.body);
          }
        },
        item.node);
  }
  return best;
}

struct SemanticTarget {
  enum class Kind : std::uint8_t { Symbol, EnumVariant };

  Kind kind = Kind::Symbol;
  QualifiedName symbol{};
  std::string variant_name;
  std::optional<std::uint32_t> variant_index;
};

std::optional<SemanticTarget> resolve_semantic_reference(const Program& program,
                                                         const TProgram& typed_program,
                                                         const std::string& ident,
                                                         int line_zero_based,
                                                         int char_zero_based) {
  const auto site = reference_site_at_position(program, ident, line_zero_based, char_zero_based);
  if (!site.has_value()) return std::nullopt;

  const TExpr* typed_expr = find_typed_expr_by_span(typed_program, site->expr_span);
  if (typed_expr == nullptr) return std::nullopt;

  return std::visit(
      [&](auto&& node) -> std::optional<SemanticTarget> {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, TExpr::Call>) {
          if (node.resolved_callee.has_value()) {
            return SemanticTarget{SemanticTarget::Kind::Symbol, *node.resolved_callee, {}, std::nullopt};
          }
        } else if constexpr (std::is_same_v<N, TExpr::Construct>) {
          if (!node.resolved_type.has_value()) return std::nullopt;
          if (node.variant_name.has_value()) {
            return SemanticTarget{SemanticTarget::Kind::EnumVariant,
                                  *node.resolved_type,
                                  *node.variant_name,
                                  node.variant_index};
          }
          return SemanticTarget{SemanticTarget::Kind::Symbol, *node.resolved_type, {}, std::nullopt};
        } else if constexpr (std::is_same_v<N, TExpr::VarRef>) {
          if (node.top_level_symbol.has_value()) {
            return SemanticTarget{SemanticTarget::Kind::Symbol, *node.top_level_symbol, {}, std::nullopt};
          }
        }
        if (site->kind == SemanticReferenceSite::Kind::PatternVariant &&
            typed_expr->ty.kind == nebula::frontend::Ty::Kind::Enum &&
            typed_expr->ty.qualified_name.has_value()) {
          return SemanticTarget{SemanticTarget::Kind::EnumVariant,
                                *typed_expr->ty.qualified_name,
                                ident,
                                std::nullopt};
        }
        return std::nullopt;
      },
      typed_expr->node);
}

const SymbolDefinition* resolve_semantic_symbol_definition(
    const Program& parsed_program,
    const TProgram& typed_program,
    const std::vector<SymbolDefinition>& symbols,
    const std::string& ident,
    int line_zero_based,
    int char_zero_based) {
  const auto target =
      resolve_semantic_reference(parsed_program, typed_program, ident, line_zero_based, char_zero_based);
  if (!target.has_value()) return nullptr;
  if (target->kind == SemanticTarget::Kind::Symbol) {
    return find_symbol_by_qualified_name(symbols, target->symbol);
  }
  return find_variant_symbol_definition(symbols, target->symbol, target->variant_name, target->variant_index);
}

const SymbolDefinition* resolve_semantic_symbol_definition(
    const std::vector<SourceFile>& sources,
    const CompilePipelineResult& result,
    const std::vector<SymbolDefinition>& symbols,
    const std::string& ident,
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based) {
  if (!result.typed_programs || result.has_error) return nullptr;
  const TProgram* typed_program = find_typed_program_by_path(sources, *result.typed_programs, path);
  if (typed_program == nullptr) return nullptr;
  const auto parsed_program = parse_lsp_program(path, text);
  if (!parsed_program.has_value()) return nullptr;
  return resolve_semantic_symbol_definition(
      *parsed_program, *typed_program, symbols, ident, line_zero_based, char_zero_based);
}

[[maybe_unused]] std::optional<cli_json::Value> definition_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (!ident.has_value()) return std::nullopt;

  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  if (!loaded.ok) return std::nullopt;
  auto sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
  sources = overlay_compile_sources(std::move(sources), path, text);
  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);
  auto compiled = compile_lsp_document(path, text, docs);
  std::string semantic_text = text;
  if (!parse_lsp_program(path, text).has_value()) {
    if (const auto* doc = find_document_state(docs, path);
        doc != nullptr && !doc->last_good_parse_text.empty()) {
      semantic_text = doc->last_good_parse_text;
    }
  }
  const auto* symbol =
      resolve_semantic_symbol_definition(sources,
                                         compiled,
                                         symbols,
                                         *ident,
                                         path,
                                         semantic_text,
                                         line_zero_based,
                                         char_zero_based);
  if (symbol == nullptr) {
    symbol = resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based, char_zero_based);
  }
  if (symbol == nullptr) return std::nullopt;

  std::string definition_text;
  if (const auto* source_text = find_source_text_by_path(sources, symbol->span.source_path);
      source_text != nullptr) {
    definition_text = *source_text;
  } else {
    std::vector<Diagnostic> scratch;
    if (!read_source(symbol->span.source_path, definition_text, scratch,
                     nebula::frontend::DiagnosticStage::Build)) {
      definition_text.clear();
    }
  }

  cli_json::Value::Object out;
  out["uri"] = cli_json::Value(path_to_uri(symbol->span.source_path));
  out["range"] = json_range_value(
      std::max(0, symbol->span.start.line - 1),
      source_col_one_based_to_lsp_character(definition_text, symbol->span.start.line, symbol->span.start.col),
      std::max(0, symbol->span.end.line - 1),
      source_col_one_based_to_lsp_character(definition_text, symbol->span.end.line, symbol->span.end.col));
  return cli_json::Value(std::move(out));
}

[[maybe_unused]] std::optional<cli_json::Value> hover_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
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

    cli_json::Value::Object contents;
    contents["kind"] = cli_json::Value("markdown");
    contents["value"] = cli_json::Value(md.str());
    cli_json::Value::Object out;
    out["contents"] = cli_json::Value(std::move(contents));
    return cli_json::Value(std::move(out));
  }
  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  std::vector<SourceFile> sources;
  std::optional<AsyncExplainEntry> async_hover;
  std::optional<TransportExplainEntry> transport_hover;
  if (loaded.ok) {
    sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
    sources = overlay_compile_sources(std::move(sources), path, text);
    std::vector<AsyncExplainEntry> async_entries;
    if (result.async_explain) {
      async_entries = result.async_explain->entries;
    } else {
      async_entries = collect_async_explain_entries(sources);
    }
    async_hover =
        async_entry_at_position(async_entries, path, line_zero_based, char_zero_based);
    auto transport_entries = collect_transport_explain_entries(sources);
    transport_hover =
        transport_entry_at_position(transport_entries, path, line_zero_based, char_zero_based);
  }
  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (ident.has_value()) {
    if (loaded.ok) {
      auto symbols = collect_symbol_definitions(sources);
      append_recovered_current_file_symbols(symbols, sources, path, text, docs);
      auto compiled = compile_lsp_document(path, text, docs);
      std::string semantic_text = text;
      if (!parse_lsp_program(path, text).has_value()) {
        if (const auto* doc = find_document_state(docs, path);
            doc != nullptr && !doc->last_good_parse_text.empty()) {
          semantic_text = doc->last_good_parse_text;
        }
      }
      const auto* symbol =
          resolve_semantic_symbol_definition(sources,
                                             compiled,
                                             symbols,
                                             *ident,
                                             path,
                                             semantic_text,
                                             line_zero_based,
                                             char_zero_based);
      if (symbol == nullptr) {
        symbol = resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based,
                                           char_zero_based);
      }
      if (symbol != nullptr) {
        cli_json::Value::Object contents;
        contents["kind"] = cli_json::Value("markdown");
        std::string value = symbol->detail;
        if (async_hover.has_value()) {
          append_hover_markdown_section(value, async_entry_markdown(*async_hover));
        }
        if (transport_hover.has_value()) {
          append_hover_markdown_section(value, transport_entry_markdown(*transport_hover));
        }
        contents["value"] = cli_json::Value(std::move(value));
        cli_json::Value::Object out;
        out["contents"] = cli_json::Value(std::move(contents));
        return cli_json::Value(std::move(out));
      }
    }
  }
  if (async_hover.has_value() || transport_hover.has_value()) {
    cli_json::Value::Object contents;
    contents["kind"] = cli_json::Value("markdown");
    std::string value;
    if (async_hover.has_value()) {
      append_hover_markdown_section(value, async_entry_markdown(*async_hover));
    }
    if (transport_hover.has_value()) {
      append_hover_markdown_section(value, transport_entry_markdown(*transport_hover));
    }
    contents["value"] = cli_json::Value(std::move(value));
    cli_json::Value::Object out;
    out["contents"] = cli_json::Value(std::move(contents));
    return cli_json::Value(std::move(out));
  }
  return std::nullopt;
}

void lsp_publish_diagnostics(const std::string& uri,
                             const std::string& path,
                             const std::string& text,
                             const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto result = compile_lsp_document(path, text, docs);
  cli_json::Value::Array diagnostics;
  for (const auto& d : result.diags) {
    if (!d.span.source_path.empty() && d.span.source_path != path) continue;
    cli_json::Value::Object diag;
    diag["range"] = json_range_value(
        std::max(0, d.span.start.line - 1),
        source_col_one_based_to_lsp_character(text, d.span.start.line, d.span.start.col),
        std::max(0, d.span.end.line - 1),
        source_col_one_based_to_lsp_character(text, d.span.end.line, d.span.end.col));
    diag["severity"] = json_i64(lsp_severity_for(d));
    diag["code"] = cli_json::Value(d.code);
    diag["source"] = cli_json::Value("nebula");
    diag["message"] = cli_json::Value(d.message);
    diagnostics.push_back(cli_json::Value(std::move(diag)));
  }
  cli_json::Value::Object params;
  params["uri"] = cli_json::Value(uri);
  params["diagnostics"] = cli_json::Value(std::move(diagnostics));
  lsp_write_notification("textDocument/publishDiagnostics", cli_json::Value(std::move(params)));
}

std::int64_t lsp_document_symbol_kind(SymbolKind kind);
cli_json::Value json_range_from_span(const std::string& text, const nebula::frontend::Span& span);
const std::string* find_source_text_by_path(const std::vector<nebula::frontend::SourceFile>& sources,
                                            const std::string& path);
const SymbolDefinition* resolve_semantic_symbol_definition(
    const std::vector<SourceFile>& sources,
    const CompilePipelineResult& result,
    const std::vector<SymbolDefinition>& symbols,
    const std::string& ident,
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based);
const SymbolDefinition* resolve_symbol_definition(const std::vector<SymbolDefinition>& symbols,
                                                  const std::vector<nebula::frontend::SourceFile>& sources,
                                                  const std::string& ident,
                                                  const std::string& path,
                                                  int line_zero_based,
                                                  int char_zero_based);
std::optional<cli_json::Value> references_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs);

cli_json::Value json_lsp_diagnostic(const std::string& text, const Diagnostic& d) {
  cli_json::Value::Object diag;
  diag["range"] = json_range_value(
      std::max(0, d.span.start.line - 1),
      source_col_one_based_to_lsp_character(text, d.span.start.line, d.span.start.col),
      std::max(0, d.span.end.line - 1),
      source_col_one_based_to_lsp_character(text, d.span.end.line, d.span.end.col));
  diag["severity"] = json_i64(lsp_severity_for(d));
  diag["code"] = cli_json::Value(d.code);
  diag["source"] = cli_json::Value("nebula");
  diag["message"] = cli_json::Value(d.message);
  return cli_json::Value(std::move(diag));
}

std::string lsp_ascii_lower_copy(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return out;
}

bool lsp_query_matches(std::string_view query, const SymbolDefinition& symbol) {
  if (query.empty()) return true;
  const std::string needle = lsp_ascii_lower_copy(query);
  const std::string name = lsp_ascii_lower_copy(symbol.name);
  const std::string detail = lsp_ascii_lower_copy(symbol.detail);
  const std::string container = lsp_ascii_lower_copy(symbol.package_name + "::" + symbol.module_name);
  return name.find(needle) != std::string::npos || detail.find(needle) != std::string::npos ||
         container.find(needle) != std::string::npos;
}

void merge_workspace_source(std::map<std::string, SourceFile>& merged, SourceFile source) {
  merged[normalized_path_string(source.path)] = std::move(source);
}

std::vector<SourceFile> collect_lsp_workspace_sources(
    const std::unordered_map<std::string, LspDocumentState>* docs) {
  std::map<std::string, SourceFile> merged;
  if (docs == nullptr) return {};
  for (const auto& [path, doc] : *docs) {
    auto loaded = load_compile_input(path,
                                     nebula::frontend::DiagnosticStage::Build,
                                     g_lsp_std_root,
                                     g_lsp_backend_sdk_root);
    if (loaded.ok) {
      auto sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
      for (auto& source : sources) merge_workspace_source(merged, std::move(source));
      continue;
    }

    SourceFile current;
    current.path = path;
    current.text = doc.text;
    if (auto program = parse_lsp_program(path, doc.text); program.has_value()) {
      current.package_name = program->package_name.value_or("");
      current.module_name = program->module_name.value_or("");
      for (const auto& import_name : program->imports) current.resolved_imports.push_back(import_name);
    }
    merge_workspace_source(merged, std::move(current));
  }

  std::vector<SourceFile> out;
  out.reserve(merged.size());
  for (auto& [_, source] : merged) out.push_back(std::move(source));
  return out;
}

std::vector<SourceFile> merge_lsp_request_sources(std::vector<SourceFile> sources,
                                                  const std::string& path,
                                                  const std::string& text,
                                                  const std::unordered_map<std::string, LspDocumentState>* docs) {
  sources = overlay_open_documents(std::move(sources), docs);
  sources = overlay_compile_sources(std::move(sources), path, text);

  std::map<std::string, SourceFile> merged;
  for (auto& source : sources) merge_workspace_source(merged, std::move(source));
  for (auto& source : collect_lsp_workspace_sources(docs)) merge_workspace_source(merged, std::move(source));

  std::vector<SourceFile> out;
  out.reserve(merged.size());
  for (auto& [_, source] : merged) out.push_back(std::move(source));
  return out;
}

std::optional<cli_json::Value> workspace_symbols_result_json(
    const std::string& query,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto sources = collect_lsp_workspace_sources(docs);
  if (sources.empty()) return cli_json::Value(cli_json::Value::Array{});

  auto symbols = collect_symbol_definitions(sources);
  cli_json::Value::Array out;
  for (const auto& symbol : symbols) {
    if (symbol.local) continue;
    if (!lsp_query_matches(query, symbol)) continue;
    std::string source_text;
    if (const auto* text = find_source_text_by_path(sources, symbol.span.source_path); text != nullptr) {
      source_text = *text;
    }
    cli_json::Value::Object item;
    item["name"] = cli_json::Value(symbol.name);
    item["kind"] = json_i64(lsp_document_symbol_kind(symbol.kind));
    if (!symbol.module_name.empty()) {
      item["containerName"] = cli_json::Value(symbol.package_name + "::" + symbol.module_name);
    }
    cli_json::Value::Object location;
    location["uri"] = cli_json::Value(path_to_uri(symbol.span.source_path));
    location["range"] = json_range_from_span(source_text, symbol.span);
    item["location"] = cli_json::Value(std::move(location));
    out.push_back(cli_json::Value(std::move(item)));
  }
  return cli_json::Value(std::move(out));
}

struct SignatureHelpSite {
  std::string callee;
  int callee_line_zero_based = 0;
  int callee_char_zero_based = 0;
  bool method_context = false;
  int active_parameter = 0;
};

std::optional<SignatureHelpSite> signature_help_site_at_position(const std::string& path,
                                                                 const std::string& text,
                                                                 int line_zero_based,
                                                                 int char_zero_based) {
  const auto cursor_offset = lsp_position_to_byte_offset(text, line_zero_based, char_zero_based);
  if (!cursor_offset.has_value()) return std::nullopt;

  Lexer lex(text, path);
  auto tokens = lex.lex_all();
  struct CallFrame {
    std::string callee;
    nebula::frontend::Span callee_span;
    bool method_context = false;
    int active_parameter = 0;
  };
  std::vector<CallFrame> stack;

  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& tok = tokens[i];
    if (tok.span.start.offset >= *cursor_offset) break;
    if (tok.kind == nebula::frontend::TokenKind::LParen) {
      std::optional<std::size_t> callee_index;
      bool method_context = false;
      if (i > 0 && tokens[i - 1].kind == nebula::frontend::TokenKind::Ident) {
        callee_index = i - 1;
        if (i > 1 && tokens[i - 2].kind == nebula::frontend::TokenKind::Dot) method_context = true;
      }
      if (callee_index.has_value()) {
        stack.push_back(
            CallFrame{tokens[*callee_index].lexeme, tokens[*callee_index].span, method_context, 0});
      } else {
        stack.push_back(CallFrame{});
      }
    } else if (tok.kind == nebula::frontend::TokenKind::Comma) {
      if (!stack.empty()) stack.back().active_parameter += 1;
    } else if (tok.kind == nebula::frontend::TokenKind::RParen) {
      if (!stack.empty()) stack.pop_back();
    }
  }

  while (!stack.empty()) {
    const auto site = stack.back();
    stack.pop_back();
    if (site.callee.empty()) continue;
    return SignatureHelpSite{
        site.callee,
        std::max(0, site.callee_span.start.line - 1),
        source_col_one_based_to_lsp_character(text, site.callee_span.start.line, site.callee_span.start.col),
        site.method_context,
        site.active_parameter};
  }
  return std::nullopt;
}

std::vector<std::string> split_signature_parameters(std::string_view label) {
  const std::size_t open = label.find('(');
  const std::size_t close = label.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open + 1) return {};
  std::vector<std::string> out;
  std::size_t part_start = open + 1;
  int angle_depth = 0;
  int paren_depth = 0;
  for (std::size_t i = open + 1; i < close; ++i) {
    const char ch = label[i];
    if (ch == '<') angle_depth += 1;
    else if (ch == '>') angle_depth = std::max(0, angle_depth - 1);
    else if (ch == '(') paren_depth += 1;
    else if (ch == ')') paren_depth = std::max(0, paren_depth - 1);
    else if (ch == ',' && angle_depth == 0 && paren_depth == 0) {
      out.push_back(trim(std::string(label.substr(part_start, i - part_start))));
      part_start = i + 1;
    }
  }
  const std::string tail = trim(std::string(label.substr(part_start, close - part_start)));
  if (!tail.empty()) out.push_back(tail);
  return out;
}

std::optional<cli_json::Value> signature_help_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  const auto site = signature_help_site_at_position(path, text, line_zero_based, char_zero_based);
  if (!site.has_value()) return std::nullopt;

  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  if (!loaded.ok) return std::nullopt;
  auto sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
  sources = overlay_compile_sources(std::move(sources), path, text);
  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);
  auto compiled = compile_lsp_document(path, text, docs);

  std::string semantic_text = text;
  if (!parse_lsp_program(path, text).has_value()) {
    if (const auto* doc = find_document_state(docs, path);
        doc != nullptr && !doc->last_good_parse_text.empty()) {
      semantic_text = doc->last_good_parse_text;
    }
  }

  const auto* symbol =
      resolve_semantic_symbol_definition(sources,
                                         compiled,
                                         symbols,
                                         site->callee,
                                         path,
                                         semantic_text,
                                         site->callee_line_zero_based,
                                         site->callee_char_zero_based);
  if (symbol == nullptr) {
    symbol = resolve_symbol_definition(
        symbols, sources, site->callee, path, site->callee_line_zero_based, site->callee_char_zero_based);
  }
  if (symbol == nullptr) return std::nullopt;

  cli_json::Value::Object signature;
  signature["label"] = cli_json::Value(symbol->detail);
  cli_json::Value::Array parameters;
  for (const auto& parameter : split_signature_parameters(symbol->detail)) {
    cli_json::Value::Object param;
    param["label"] = cli_json::Value(parameter);
    parameters.push_back(cli_json::Value(std::move(param)));
  }
  if (!parameters.empty()) signature["parameters"] = cli_json::Value(std::move(parameters));

  cli_json::Value::Array signatures;
  signatures.push_back(cli_json::Value(std::move(signature)));

  cli_json::Value::Object out;
  out["signatures"] = cli_json::Value(std::move(signatures));
  out["activeSignature"] = json_i64(0);
  out["activeParameter"] = json_i64(std::max(0, site->active_parameter));
  return cli_json::Value(std::move(out));
}

bool is_valid_lsp_identifier(std::string_view name) {
  if (name.empty()) return false;
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!(std::isalpha(first) || first == '_')) return false;
  for (char ch : name) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (!(std::isalnum(value) || value == '_')) return false;
  }
  return true;
}

bool lsp_symbol_identity_matches(const SymbolDefinition& lhs, const SymbolDefinition& rhs) {
  if (lhs.kind != rhs.kind) return false;
  if (lhs.kind == SymbolKind::EnumVariant) {
    return lhs.name == rhs.name &&
           lhs.owner_qualified_name == rhs.owner_qualified_name &&
           lhs.variant_index == rhs.variant_index;
  }
  return lhs.qualified_name == rhs.qualified_name && lhs.name == rhs.name;
}

bool lsp_symbol_supports_precise_rename(const SymbolDefinition& symbol) {
  if (symbol.local) return false;
  return symbol.kind == SymbolKind::Function || symbol.kind == SymbolKind::EnumVariant;
}

std::optional<nebula::frontend::Span> find_symbol_definition_identifier_span(const SourceFile& source,
                                                                              const SymbolDefinition& symbol) {
  try {
    Lexer lex(source.text, source.path);
    auto tokens = lex.lex_all();
    for (const auto& token : tokens) {
      if (token.kind != nebula::frontend::TokenKind::Ident) continue;
      if (token.lexeme != symbol.name) continue;
      if (token.span.start.offset < symbol.span.start.offset ||
          token.span.end.offset > symbol.span.end.offset) {
        continue;
      }
      return token.span;
    }
  } catch (const nebula::frontend::FrontendError&) {
    return std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}

void append_unique_rename_span(std::vector<nebula::frontend::Span>& out,
                               std::set<std::tuple<std::string, int, int, int, int>>& seen,
                               const nebula::frontend::Span& span) {
  const auto key = std::make_tuple(
      normalized_path_string(span.source_path), span.start.line, span.start.col, span.end.line, span.end.col);
  if (!seen.insert(key).second) return;
  out.push_back(span);
}

std::vector<nebula::frontend::Span> collect_precise_rename_spans_for_symbol(
    const std::vector<SourceFile>& sources,
    const CompilePipelineResult& compiled,
    const std::vector<SymbolDefinition>& symbols,
    const SymbolDefinition& target) {
  std::vector<nebula::frontend::Span> out;
  std::set<std::tuple<std::string, int, int, int, int>> seen;
  const std::string target_path = normalized_path_string(target.span.source_path);

  for (const auto& source : sources) {
    if (normalized_path_string(source.path) == target_path) {
      if (const auto decl_span = find_symbol_definition_identifier_span(source, target); decl_span.has_value()) {
        append_unique_rename_span(out, seen, *decl_span);
      }
    }

    const auto parsed_program = parse_lsp_program(source.path, source.text);
    const TProgram* typed_program =
        (compiled.typed_programs != nullptr)
            ? find_typed_program_by_path(sources, *compiled.typed_programs, source.path)
            : nullptr;

    try {
      Lexer lex(source.text, source.path);
      auto tokens = lex.lex_all();
      for (const auto& token : tokens) {
        if (token.kind != nebula::frontend::TokenKind::Ident) continue;
        if (token.lexeme != target.name) continue;
        const int line_zero_based = std::max(0, token.span.start.line - 1);
        const int char_zero_based =
            source_col_one_based_to_lsp_character(source.text, token.span.start.line, token.span.start.col);
        const SymbolDefinition* symbol = nullptr;
        if (parsed_program.has_value() && typed_program != nullptr) {
          symbol = resolve_semantic_symbol_definition(
              *parsed_program, *typed_program, symbols, token.lexeme, line_zero_based, char_zero_based);
        }
        if (symbol == nullptr) {
          symbol = resolve_symbol_definition(
              symbols, sources, token.lexeme, source.path, line_zero_based, char_zero_based);
        }
        if (symbol == nullptr) continue;
        if (!lsp_symbol_identity_matches(*symbol, target)) continue;
        append_unique_rename_span(out, seen, token.span);
      }
    } catch (const nebula::frontend::FrontendError&) {
      continue;
    } catch (...) {
      continue;
    }
  }

  return out;
}

std::optional<cli_json::Value> rename_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::string& new_name,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  if (!is_valid_lsp_identifier(new_name)) return std::nullopt;

  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  if (!loaded.ok) return std::nullopt;
  auto sources = merge_lsp_request_sources(std::move(loaded.compile_sources), path, text, docs);
  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);
  auto compiled = compile_lsp_document(path, text, docs);

  std::string semantic_text = text;
  if (!parse_lsp_program(path, text).has_value()) {
    if (const auto* doc = find_document_state(docs, path);
        doc != nullptr && !doc->last_good_parse_text.empty()) {
      semantic_text = doc->last_good_parse_text;
    }
  }

  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (!ident.has_value()) return std::nullopt;
  const auto* symbol = resolve_semantic_symbol_definition(
      sources, compiled, symbols, *ident, path, semantic_text, line_zero_based, char_zero_based);
  if (symbol == nullptr) {
    symbol = resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based, char_zero_based);
  }
  if (symbol == nullptr || !lsp_symbol_supports_precise_rename(*symbol)) return std::nullopt;

  const auto spans = collect_precise_rename_spans_for_symbol(sources, compiled, symbols, *symbol);
  if (spans.empty()) return std::nullopt;

  std::map<std::string, cli_json::Value::Array> changes;
  for (const auto& span : spans) {
    std::string source_text;
    if (const auto* text_for_path = find_source_text_by_path(sources, span.source_path); text_for_path != nullptr) {
      source_text = *text_for_path;
    }
    cli_json::Value::Object edit;
    edit["range"] = json_range_from_span(source_text, span);
    edit["newText"] = cli_json::Value(new_name);
    changes[path_to_uri(span.source_path)].push_back(cli_json::Value(std::move(edit)));
  }

  cli_json::Value::Object out;
  cli_json::Value::Object json_changes;
  for (auto& [uri, edits] : changes) {
    json_changes[uri] = cli_json::Value(std::move(edits));
  }
  out["changes"] = cli_json::Value(std::move(json_changes));
  return cli_json::Value(std::move(out));
}

nebula::frontend::SourcePos source_pos_from_offset(std::string_view text, std::size_t offset) {
  nebula::frontend::SourcePos pos;
  pos.offset = std::min(offset, text.size());
  pos.line = 1;
  pos.col = 1;
  for (std::size_t i = 0; i < pos.offset; ++i) {
    if (text[i] == '\n') {
      pos.line += 1;
      pos.col = 1;
    } else {
      pos.col += 1;
    }
  }
  return pos;
}

std::optional<nebula::frontend::Span> preceding_annotation_span(const std::string& text,
                                                                const nebula::frontend::Span& item_span,
                                                                std::string_view annotation) {
  if (item_span.start.offset > text.size()) return std::nullopt;
  const std::string_view prefix(text.data(), item_span.start.offset);
  const std::size_t pos = prefix.rfind(annotation);
  if (pos == std::string_view::npos) return std::nullopt;
  std::size_t cursor = pos + annotation.size();
  while (cursor < item_span.start.offset) {
    const char ch = text[cursor];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return std::nullopt;
    cursor += 1;
  }
  return nebula::frontend::Span(source_pos_from_offset(text, pos),
                                source_pos_from_offset(text, item_span.start.offset),
                                item_span.source_path);
}

cli_json::Value workspace_edit_replace_json(const std::string& path,
                                            const std::string& text,
                                            const nebula::frontend::Span& span,
                                            std::string new_text) {
  cli_json::Value::Object edit;
  edit["range"] = json_range_from_span(text, span);
  edit["newText"] = cli_json::Value(std::move(new_text));
  cli_json::Value::Array edits;
  edits.push_back(cli_json::Value(std::move(edit)));

  cli_json::Value::Object changes;
  changes[path_to_uri(path)] = cli_json::Value(std::move(edits));

  cli_json::Value::Object out;
  out["changes"] = cli_json::Value(std::move(changes));
  return cli_json::Value(std::move(out));
}

std::optional<cli_json::Value> quickfix_workspace_edit_json(const std::string& path,
                                                            const std::string& text,
                                                            const Diagnostic& d) {
  if (d.code == "NBL-U002") {
    const auto annotation_span = preceding_annotation_span(text, d.span, "@unsafe");
    if (!annotation_span.has_value()) return std::nullopt;
    return workspace_edit_replace_json(path, text, *annotation_span, "");
  }
  return std::nullopt;
}

std::optional<cli_json::Value> code_actions_result_json(
    const std::string& path,
    const std::string& text,
    const std::optional<LspRequestedRange>& requested_range,
    const std::vector<std::string>& only_kinds,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto result = compile_lsp_document(path, text, docs);
  cli_json::Value::Array out;
  const auto quickfix_allowed = [&only_kinds](std::string_view kind) {
    if (only_kinds.empty()) return true;
    for (const auto& requested : only_kinds) {
      if (requested == kind) return true;
      if (kind.size() > requested.size() && kind.substr(0, requested.size()) == requested &&
          kind[requested.size()] == '.') {
        return true;
      }
    }
    return false;
  };
  const auto diagnostic_matches_range = [&text, &requested_range](const Diagnostic& d) {
    if (!requested_range.has_value()) return true;
    const auto start =
        lsp_position_to_byte_offset(text, requested_range->start_line, requested_range->start_character);
    const auto end =
        lsp_position_to_byte_offset(text, requested_range->end_line, requested_range->end_character);
    if (!start.has_value() || !end.has_value()) return true;
    const std::size_t range_start = std::min(*start, *end);
    const std::size_t range_end = std::max(*start, *end);
    return !(d.span.end.offset <= range_start || d.span.start.offset >= range_end);
  };
  for (const auto& d : result.diags) {
    if (!d.span.source_path.empty() && normalized_path_string(d.span.source_path) != normalized_path_string(path)) {
      continue;
    }
    if (d.suggestions.empty()) continue;
    if (!quickfix_allowed("quickfix")) continue;
    if (!diagnostic_matches_range(d)) continue;
    const auto edit = quickfix_workspace_edit_json(path, text, d);
    if (!edit.has_value()) continue;
    cli_json::Value::Object action;
    action["title"] = cli_json::Value(d.suggestions.front());
    action["kind"] = cli_json::Value("quickfix");
    action["isPreferred"] = cli_json::Value(true);
    cli_json::Value::Array diagnostics;
    diagnostics.push_back(json_lsp_diagnostic(text, d));
    action["diagnostics"] = cli_json::Value(std::move(diagnostics));
    action["edit"] = *edit;
    out.push_back(cli_json::Value(std::move(action)));
  }

  if (quickfix_allowed("source")) {
    cli_json::Value::Object format_action;
    format_action["title"] = cli_json::Value("Format Document");
    format_action["kind"] = cli_json::Value("source");
    cli_json::Value::Object format_command;
    format_command["title"] = cli_json::Value("Format Document");
    format_command["command"] = cli_json::Value("editor.action.formatDocument");
    format_action["command"] = cli_json::Value(std::move(format_command));
    out.push_back(cli_json::Value(std::move(format_action)));
  }

  if (quickfix_allowed("refactor.rewrite")) {
    cli_json::Value::Object explain_action;
    explain_action["title"] = cli_json::Value("Explain Symbol");
    explain_action["kind"] = cli_json::Value("refactor.rewrite");
    cli_json::Value::Object explain_command;
    explain_command["title"] = cli_json::Value("Nebula: Explain Symbol");
    explain_command["command"] = cli_json::Value("nebula.explainSymbol");
    explain_action["command"] = cli_json::Value(std::move(explain_command));
    out.push_back(cli_json::Value(std::move(explain_action)));
  }

  return cli_json::Value(std::move(out));
}

enum class LspSemanticTokenType : std::int64_t {
  Namespace = 0,
  Type = 1,
  Struct = 2,
  Enum = 3,
  EnumMember = 4,
  Function = 5,
  Method = 6,
  Parameter = 7,
  Variable = 8,
  Property = 9,
  Keyword = 10,
  Number = 11,
  String = 12,
};

struct SemanticTokenEntry {
  int line = 0;
  int start = 0;
  int length = 0;
  LspSemanticTokenType type = LspSemanticTokenType::Variable;
  int modifiers = 0;
};

int semantic_token_length_utf16(const std::string& text, const nebula::frontend::Span& span) {
  const int start =
      source_col_one_based_to_lsp_character(text, span.start.line, span.start.col);
  const int end = source_col_one_based_to_lsp_character(text, span.end.line, span.end.col);
  return std::max(0, end - start);
}

void append_semantic_token(std::vector<SemanticTokenEntry>& out,
                           const std::string& text,
                           const nebula::frontend::Span& span,
                           LspSemanticTokenType type) {
  const int line = std::max(0, span.start.line - 1);
  const int start = source_col_one_based_to_lsp_character(text, span.start.line, span.start.col);
  const int length = semantic_token_length_utf16(text, span);
  if (length <= 0) return;
  out.push_back({line, start, length, type, 0});
}

std::optional<SymbolDefinition> symbol_at_identifier_token(
    const std::vector<SourceFile>& sources,
    const CompilePipelineResult& compiled,
    const std::vector<SymbolDefinition>& symbols,
    const std::string& path,
    const std::string& text,
    const std::string& semantic_text,
    const nebula::frontend::Token& token) {
  const int line_zero_based = std::max(0, token.span.start.line - 1);
  const int char_zero_based =
      source_col_one_based_to_lsp_character(text, token.span.start.line, token.span.start.col);
  if (const auto* semantic = resolve_semantic_symbol_definition(
          sources, compiled, symbols, token.lexeme, path, semantic_text, line_zero_based, char_zero_based);
      semantic != nullptr) {
    return *semantic;
  }
  if (const auto* fallback =
          resolve_symbol_definition(symbols, sources, token.lexeme, path, line_zero_based, char_zero_based);
      fallback != nullptr) {
    return *fallback;
  }
  return std::nullopt;
}

std::optional<cli_json::Value> semantic_tokens_result_json(
    const std::string& path,
    const std::string& text,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  std::vector<SourceFile> sources;
  if (loaded.ok) {
    sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
    sources = overlay_compile_sources(std::move(sources), path, text);
  } else {
    SourceFile current;
    current.path = path;
    current.text = text;
    if (auto program = parse_lsp_program(path, text); program.has_value()) {
      current.package_name = program->package_name.value_or("");
      current.module_name = program->module_name.value_or("");
      for (const auto& import_name : program->imports) current.resolved_imports.push_back(import_name);
    }
    sources.push_back(std::move(current));
  }
  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);
  auto compiled = compile_lsp_document(path, text, docs);

  std::string semantic_text = text;
  if (!parse_lsp_program(path, text).has_value()) {
    if (const auto* doc = find_document_state(docs, path);
        doc != nullptr && !doc->last_good_parse_text.empty()) {
      semantic_text = doc->last_good_parse_text;
    }
  }

  Lexer lex(text, path);
  auto tokens = lex.lex_all();
  std::vector<SemanticTokenEntry> entries;
  entries.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const auto& token = tokens[i];
    if (token.kind == nebula::frontend::TokenKind::Eof) continue;
    if (token.is_keyword()) {
      append_semantic_token(entries, text, token.span, LspSemanticTokenType::Keyword);
      continue;
    }
    if (token.kind == nebula::frontend::TokenKind::IntLit ||
        token.kind == nebula::frontend::TokenKind::FloatLit) {
      append_semantic_token(entries, text, token.span, LspSemanticTokenType::Number);
      continue;
    }
    if (token.kind == nebula::frontend::TokenKind::StringLit) {
      append_semantic_token(entries, text, token.span, LspSemanticTokenType::String);
      continue;
    }
    if (token.kind != nebula::frontend::TokenKind::Ident) continue;
    if (token.lexeme == "ui" || token.lexeme == "view") {
      append_semantic_token(entries, text, token.span, LspSemanticTokenType::Keyword);
      continue;
    }

    const auto symbol =
        symbol_at_identifier_token(sources, compiled, symbols, path, text, semantic_text, token);
    if (symbol.has_value()) {
      switch (symbol->kind) {
      case SymbolKind::Function:
        append_semantic_token(entries,
                              text,
                              token.span,
                              (i > 0 && tokens[i - 1].kind == nebula::frontend::TokenKind::Dot)
                                  ? LspSemanticTokenType::Method
                                  : LspSemanticTokenType::Function);
        break;
      case SymbolKind::Struct:
        append_semantic_token(entries, text, token.span, LspSemanticTokenType::Struct);
        break;
      case SymbolKind::Enum:
        append_semantic_token(entries, text, token.span, LspSemanticTokenType::Enum);
        break;
      case SymbolKind::EnumVariant:
        append_semantic_token(entries, text, token.span, LspSemanticTokenType::EnumMember);
        break;
      case SymbolKind::Ui:
        append_semantic_token(entries, text, token.span, LspSemanticTokenType::Type);
        break;
      case SymbolKind::Local:
        append_semantic_token(entries,
                              text,
                              token.span,
                              symbol->detail.rfind("param", 0) == 0 ? LspSemanticTokenType::Parameter
                                                                     : LspSemanticTokenType::Variable);
        break;
      }
      continue;
    }

    append_semantic_token(
        entries,
        text,
        token.span,
        (i > 0 && tokens[i - 1].kind == nebula::frontend::TokenKind::Dot)
            ? LspSemanticTokenType::Property
            : LspSemanticTokenType::Variable);
  }

  std::sort(entries.begin(), entries.end(), [](const SemanticTokenEntry& lhs, const SemanticTokenEntry& rhs) {
    if (lhs.line != rhs.line) return lhs.line < rhs.line;
    if (lhs.start != rhs.start) return lhs.start < rhs.start;
    return lhs.length < rhs.length;
  });

  cli_json::Value::Array data;
  int prev_line = 0;
  int prev_start = 0;
  for (const auto& entry : entries) {
    const int delta_line = entry.line - prev_line;
    const int delta_start = delta_line == 0 ? (entry.start - prev_start) : entry.start;
    data.push_back(json_i64(delta_line));
    data.push_back(json_i64(delta_start));
    data.push_back(json_i64(entry.length));
    data.push_back(json_i64(static_cast<std::int64_t>(entry.type)));
    data.push_back(json_i64(entry.modifiers));
    prev_line = entry.line;
    prev_start = entry.start;
  }

  cli_json::Value::Object out;
  out["data"] = cli_json::Value(std::move(data));
  return cli_json::Value(std::move(out));
}

std::int64_t lsp_document_symbol_kind(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Function: return 12;
  case SymbolKind::Struct: return 23;
  case SymbolKind::Enum: return 10;
  case SymbolKind::EnumVariant: return 22;
  case SymbolKind::Ui: return 5;
  case SymbolKind::Local: return 13;
  }
  return 13;
}

std::int64_t lsp_completion_kind(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Function: return 3;
  case SymbolKind::Struct: return 22;
  case SymbolKind::Enum: return 13;
  case SymbolKind::EnumVariant: return 20;
  case SymbolKind::Ui: return 7;
  case SymbolKind::Local: return 6;
  }
  return 1;
}

cli_json::Value json_range_from_span(const std::string& text, const nebula::frontend::Span& span) {
  return json_range_value(
      std::max(0, span.start.line - 1),
      source_col_one_based_to_lsp_character(text, span.start.line, span.start.col),
      std::max(0, span.end.line - 1),
      source_col_one_based_to_lsp_character(text, span.end.line, span.end.col));
}

void append_completion_item(cli_json::Value::Array& out,
                            std::unordered_set<std::string>& seen,
                            std::string label,
                            std::int64_t kind,
                            const std::string& detail = {}) {
  if (!seen.insert(label).second) return;
  cli_json::Value::Object item;
  item["label"] = cli_json::Value(label);
  item["kind"] = json_i64(kind);
  if (!detail.empty()) item["detail"] = cli_json::Value(detail);
  out.push_back(cli_json::Value(std::move(item)));
}

std::optional<cli_json::Value> completion_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  std::vector<SourceFile> sources;
  if (loaded.ok) {
    sources = overlay_open_documents(std::move(loaded.compile_sources), docs);
    sources = overlay_compile_sources(std::move(sources), path, text);
  } else {
    SourceFile current;
    current.path = path;
    current.text = text;
    if (auto program = parse_lsp_program(path, text); program.has_value()) {
      current.package_name = program->package_name.value_or("");
      current.module_name = program->module_name.value_or("");
      for (const auto& import_name : program->imports) current.resolved_imports.push_back(import_name);
    }
    sources.push_back(std::move(current));
  }

  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);

  cli_json::Value::Array items;
  std::unordered_set<std::string> seen;
  for (const char* keyword : {"async",
                              "await",
                              "break",
                              "continue",
                              "else",
                              "enum",
                              "extern",
                              "fn",
                              "for",
                              "if",
                              "import",
                              "let",
                              "match",
                              "module",
                              "region",
                              "return",
                              "shared",
                              "struct",
                              "unique",
                              "unsafe",
                              "while"}) {
    append_completion_item(items, seen, keyword, 14);
  }

  const std::string requested_path = normalized_path_string(path);
  const auto* current_source = find_source_by_path(sources, path);
  for (const auto& symbol : symbols) {
    if (symbol.local) {
      if (normalized_path_string(symbol.span.source_path) != requested_path) continue;
      if (!lsp_position_matches(symbol.visibility, line_zero_based, char_zero_based)) continue;
      if (symbol.span.start.line > line_zero_based + 1) continue;
      append_completion_item(items, seen, symbol.name, lsp_completion_kind(symbol.kind), symbol.detail);
      continue;
    }
    if (current_source != nullptr) {
      const bool same_module =
          symbol.package_name == current_source->package_name && symbol.module_name == current_source->module_name;
      bool imported = false;
      for (const auto& import_name : current_source->resolved_imports) {
        const std::size_t sep = import_name.find("::");
        if (sep == std::string::npos) continue;
        if (symbol.package_name == import_name.substr(0, sep) &&
            symbol.module_name == import_name.substr(sep + 2)) {
          imported = true;
          break;
        }
      }
      if (!same_module && !imported) continue;
    } else if (normalized_path_string(symbol.span.source_path) != requested_path) {
      continue;
    }
    append_completion_item(items, seen, symbol.name, lsp_completion_kind(symbol.kind), symbol.detail);
  }

  return cli_json::Value(std::move(items));
}

cli_json::Value json_document_symbol(const std::string& text,
                                     std::string name,
                                     std::int64_t kind,
                                     const nebula::frontend::Span& range,
                                     const nebula::frontend::Span& selection_range,
                                     const std::string& detail = {},
                                     cli_json::Value::Array children = {}) {
  cli_json::Value::Object item;
  item["name"] = cli_json::Value(std::move(name));
  item["kind"] = json_i64(kind);
  item["range"] = json_range_from_span(text, range);
  item["selectionRange"] = json_range_from_span(text, selection_range);
  if (!detail.empty()) item["detail"] = cli_json::Value(detail);
  if (!children.empty()) item["children"] = cli_json::Value(std::move(children));
  return cli_json::Value(std::move(item));
}

std::optional<cli_json::Value> document_symbols_result_json(
    const std::string& path,
    const std::string& text,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  (void)docs;
  cli_json::Value::Array out;
  if (auto program = parse_lsp_program(path, text); program.has_value()) {
    for (const auto& item : program->items) {
      std::visit(
          [&](auto&& node) {
            using N = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<N, Function>) {
              const auto range = node.body.has_value() ? node.body->span : node.span;
              out.push_back(json_document_symbol(
                  text, node.name, lsp_document_symbol_kind(SymbolKind::Function), range, node.span,
                  function_symbol_detail(node)));
            } else if constexpr (std::is_same_v<N, Struct>) {
              out.push_back(json_document_symbol(
                  text, node.name, lsp_document_symbol_kind(SymbolKind::Struct), node.span, node.span,
                  "struct " + node.name));
            } else if constexpr (std::is_same_v<N, Enum>) {
              cli_json::Value::Array children;
              for (const auto& variant : node.variants) {
                children.push_back(json_document_symbol(
                    text,
                    variant.name,
                    lsp_document_symbol_kind(SymbolKind::EnumVariant),
                    variant.span,
                    variant.span,
                    enum_variant_detail(node.name, variant)));
              }
              out.push_back(json_document_symbol(
                  text,
                  node.name,
                  lsp_document_symbol_kind(SymbolKind::Enum),
                  node.span,
                  node.span,
                  "enum " + node.name,
                  std::move(children)));
            } else if constexpr (std::is_same_v<N, Ui>) {
              out.push_back(json_document_symbol(
                  text, node.name, lsp_document_symbol_kind(SymbolKind::Ui), node.span, node.span,
                  ui_symbol_detail(node)));
            }
          },
          item.node);
    }
    return cli_json::Value(std::move(out));
  }

  SourceFile source;
  source.path = path;
  source.text = text;
  auto symbols = collect_current_file_symbols_from_text(source, text);
  const std::string requested_path = normalized_path_string(path);
  for (const auto& symbol : symbols) {
    if (symbol.local) continue;
    if (normalized_path_string(symbol.span.source_path) != requested_path) continue;
    if (symbol.kind == SymbolKind::EnumVariant) continue;
    out.push_back(json_document_symbol(
        text, symbol.name, lsp_document_symbol_kind(symbol.kind), symbol.span, symbol.span, symbol.detail));
  }
  return cli_json::Value(std::move(out));
}

[[maybe_unused]] bool spans_less(const nebula::frontend::Span& lhs, const nebula::frontend::Span& rhs) {
  if (lhs.source_path != rhs.source_path) return lhs.source_path < rhs.source_path;
  if (lhs.start.line != rhs.start.line) return lhs.start.line < rhs.start.line;
  if (lhs.start.col != rhs.start.col) return lhs.start.col < rhs.start.col;
  if (lhs.end.line != rhs.end.line) return lhs.end.line < rhs.end.line;
  return lhs.end.col < rhs.end.col;
}

void append_reference_location(cli_json::Value::Array& out,
                               std::set<std::tuple<std::string, int, int, int, int>>& seen,
                               const std::string& path,
                               const std::string& text,
                               const nebula::frontend::Span& span) {
  const auto key = std::make_tuple(path, span.start.line, span.start.col, span.end.line, span.end.col);
  if (!seen.insert(key).second) return;
  cli_json::Value::Object item;
  item["uri"] = cli_json::Value(path_to_uri(path));
  item["range"] = json_range_from_span(text, span);
  out.push_back(cli_json::Value(std::move(item)));
}

std::optional<cli_json::Value> references_result_json(
    const std::string& path,
    const std::string& text,
    int line_zero_based,
    int char_zero_based,
    const std::unordered_map<std::string, LspDocumentState>* docs = nullptr) {
  const auto ident = identifier_at_position(path, text, line_zero_based, char_zero_based);
  if (!ident.has_value()) return cli_json::Value(cli_json::Value::Array{});

  auto loaded = load_compile_input(path,
                                   nebula::frontend::DiagnosticStage::Build,
                                   g_lsp_std_root,
                                   g_lsp_backend_sdk_root);
  if (!loaded.ok) return cli_json::Value(cli_json::Value::Array{});
  auto sources = merge_lsp_request_sources(std::move(loaded.compile_sources), path, text, docs);
  auto symbols = collect_symbol_definitions(sources);
  append_recovered_current_file_symbols(symbols, sources, path, text, docs);
  auto compiled = compile_lsp_document(path, text, docs);

  std::string semantic_text = text;
  if (!parse_lsp_program(path, text).has_value()) {
    if (const auto* doc = find_document_state(docs, path);
        doc != nullptr && !doc->last_good_parse_text.empty()) {
      semantic_text = doc->last_good_parse_text;
    }
  }

  const auto* symbol =
      resolve_semantic_symbol_definition(sources,
                                         compiled,
                                         symbols,
                                         *ident,
                                         path,
                                         semantic_text,
                                         line_zero_based,
                                         char_zero_based);
  if (symbol == nullptr) {
    symbol = resolve_symbol_definition(symbols, sources, *ident, path, line_zero_based, char_zero_based);
  }
  if (symbol == nullptr) return cli_json::Value(cli_json::Value::Array{});

  cli_json::Value::Array out;
  std::set<std::tuple<std::string, int, int, int, int>> seen;
  std::string definition_text;
  if (const auto* source_text = find_source_text_by_path(sources, symbol->span.source_path);
      source_text != nullptr) {
    definition_text = *source_text;
  }
  if (symbol->local) {
    append_reference_location(out,
                              seen,
                              symbol->span.source_path,
                              definition_text.empty() ? text : definition_text,
                              symbol->span);
    if (compiled.typed_programs) {
      if (const TProgram* typed_program =
              find_typed_program_by_path(sources, *compiled.typed_programs, symbol->span.source_path);
          typed_program != nullptr) {
        if (auto binding_id = find_local_binding_id_for_symbol(*typed_program, *symbol);
            binding_id.has_value()) {
          append_local_reference_locations(out, seen, *typed_program, symbol->span.source_path,
                                           definition_text.empty() ? text : definition_text, *binding_id);
        }
      }
    }
    return cli_json::Value(std::move(out));
  }

  for (const auto& span : collect_precise_rename_spans_for_symbol(sources, compiled, symbols, *symbol)) {
    std::string source_text;
    if (const auto* text_for_path = find_source_text_by_path(sources, span.source_path); text_for_path != nullptr) {
      source_text = *text_for_path;
    }
    append_reference_location(out,
                              seen,
                              span.source_path,
                              source_text.empty() ? text : source_text,
                              span);
  }

  return cli_json::Value(std::move(out));
}

enum class NewProjectTemplateKind {
  Basic,
  Cli,
  HttpService,
  BackendService,
  ControlPlaneWorkspace,
};

struct ScaffoldFile {
  fs::path path;
  std::string text;
};

struct ScaffoldProject {
  std::string template_name;
  std::vector<ScaffoldFile> files;
};

bool read_scaffold_text_file(const fs::path& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  out = buffer.str();
  return true;
}

std::string scaffold_package_name(const fs::path& root) {
  std::string raw = root.filename().string();
  if (raw.empty()) return "nebula-app";

  std::string out;
  out.reserve(raw.size());
  bool last_dash = false;
  for (char ch : raw) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      out.push_back(static_cast<char>(std::tolower(uch)));
      last_dash = false;
      continue;
    }
    if (ch == '_' || ch == '-') {
      out.push_back(ch);
      last_dash = (ch == '-');
      continue;
    }
    if (!last_dash) {
      out.push_back('-');
      last_dash = true;
    }
  }

  while (!out.empty() && (out.front() == '-' || out.front() == '_')) out.erase(out.begin());
  while (!out.empty() && (out.back() == '-' || out.back() == '_')) out.pop_back();
  if (out.empty()) return "nebula-app";
  if (std::isdigit(static_cast<unsigned char>(out.front()))) return "app-" + out;
  return out;
}

std::string new_project_template_names() {
  return "basic|cli|http-service|backend-service|control-plane-workspace";
}

std::optional<NewProjectTemplateKind> parse_new_project_template(std::string_view value) {
  if (value == "basic") return NewProjectTemplateKind::Basic;
  if (value == "cli") return NewProjectTemplateKind::Cli;
  if (value == "http-service") return NewProjectTemplateKind::HttpService;
  if (value == "backend-service") return NewProjectTemplateKind::BackendService;
  if (value == "control-plane-workspace") return NewProjectTemplateKind::ControlPlaneWorkspace;
  return std::nullopt;
}

PackageManifest scaffold_manifest(const std::string& name) {
  PackageManifest manifest_cfg;
  manifest_cfg.name = name;
  manifest_cfg.version = "0.1.0";
  manifest_cfg.entry = "src/main.nb";
  manifest_cfg.src_dir = "src";
  return manifest_cfg;
}

bool backend_sdk_ready(const CliOptions& opt) {
  if (opt.backend_sdk_root.empty()) return false;
  return fs::exists(opt.backend_sdk_root / "nebula-service" / "nebula.toml") &&
         fs::exists(opt.backend_sdk_root / "nebula-observe" / "nebula.toml");
}

bool installed_backend_package_ready(const CliOptions& opt, std::string_view package_name) {
  if (opt.backend_sdk_root.empty()) return false;
  return fs::exists(opt.backend_sdk_root / package_name / "nebula.toml");
}

std::optional<fs::path> repo_preview_package_root(const CliOptions& opt, std::string_view package_name) {
  if (opt.repo_root.empty()) return std::nullopt;
  const fs::path root = opt.repo_root / "official" / package_name;
  if (!fs::exists(root / "nebula.toml")) return std::nullopt;
  return root;
}

std::optional<fs::path> control_plane_template_source_root(const CliOptions& opt) {
  if (opt.repo_root.empty()) return std::nullopt;
  const fs::path root = opt.repo_root / "examples" / "release_control_plane_workspace";
  if (!fs::exists(root / "nebula.toml")) return std::nullopt;
  return root;
}

bool control_plane_template_ready(const CliOptions& opt) {
  return backend_sdk_ready(opt) && control_plane_template_source_root(opt).has_value() &&
         installed_backend_package_ready(opt, "nebula-db-sqlite") &&
         repo_preview_package_root(opt, "nebula-auth").has_value() &&
         repo_preview_package_root(opt, "nebula-config").has_value() &&
         repo_preview_package_root(opt, "nebula-crypto").has_value() &&
         repo_preview_package_root(opt, "nebula-db-postgres").has_value() &&
         repo_preview_package_root(opt, "nebula-jobs").has_value() &&
         repo_preview_package_root(opt, "nebula-tls").has_value();
}

void replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
  if (needle.empty()) return;
  std::size_t pos = 0;
  while ((pos = text.find(needle.data(), pos, needle.size())) != std::string::npos) {
    text.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
}

std::string scaffold_path_dependency(const fs::path& from_dir, const fs::path& dep_root) {
  const fs::path from = fs::absolute(from_dir).lexically_normal();
  const fs::path dep = fs::absolute(dep_root).lexically_normal();
  const fs::path rel = dep.lexically_relative(from);
  if (!rel.empty()) return rel.string();
  return dep.string();
}

std::string scaffold_preview_package_dependency(const fs::path& from_dir, const fs::path& dep_root) {
  const std::string path = scaffold_path_dependency(from_dir, dep_root);
  if (path.rfind("../../../../official/", 0) == 0) {
    return fs::absolute(dep_root).lexically_normal().string();
  }
  return path;
}

bool append_scaffold_files_from_directory(ScaffoldProject& project, const fs::path& source_root) {
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(source_root), end; it != end; ++it) {
    if (!it->is_regular_file()) continue;
    const fs::path rel = fs::relative(it->path(), source_root);
    if (rel.filename() == "nebula.lock") continue;
    files.push_back(rel);
  }
  std::sort(files.begin(), files.end());
  for (const auto& rel : files) {
    std::string text;
    if (!read_scaffold_text_file(source_root / rel, text)) return false;
    project.files.push_back({rel, std::move(text)});
  }
  return true;
}

std::string scaffold_gitignore() {
  return ".nebula/\n"
         "benchmark_results/\n"
         "generated_cpp/\n";
}

std::string scaffold_basic_readme() {
  return "# Nebula App\n\n"
         "This project was scaffolded with `nebula new`.\n\n"
         "Run it with:\n\n"
         "```bash\n"
         "nebula run . --run-gate none\n"
         "```\n";
}

std::string scaffold_cli_readme() {
  return "# Nebula CLI Starter\n\n"
         "This project was scaffolded with `nebula new --template cli`.\n\n"
         "This starter is a small file-copy tool.\n\n"
         "Run it with:\n\n"
         "```bash\n"
         "nebula run . --run-gate none -- input.bin output.bin\n"
         "```\n";
}

std::string scaffold_http_service_readme() {
  return "# Nebula HTTP Service Starter\n\n"
         "This project was scaffolded with `nebula new --template http-service`.\n\n"
         "Run it with:\n\n"
         "```bash\n"
         "nebula run . --run-gate none\n"
         "```\n\n"
         "Override the bind port with `PORT` when needed:\n\n"
         "```bash\n"
         "PORT=8080 nebula run . --run-gate none\n"
         "```\n\n"
         "Try it with:\n\n"
         "```bash\n"
         "curl http://127.0.0.1:40480/healthz\n"
         "curl http://127.0.0.1:40480/hello/nebula\n"
         "```\n";
}

std::string scaffold_backend_service_readme(const fs::path& backend_sdk_root) {
  const fs::path bridge_path = backend_sdk_root / "nebula-observe" / "prometheus_bridge.py";
  return "# Nebula Backend Service Starter\n\n"
         "This project was scaffolded with `nebula new --template backend-service`.\n\n"
         "It targets Nebula's Linux backend service GA surface and is the recommended installed backend-service path.\n"
         "Repo-local examples such as `examples/hello_api` remain preview/reference examples rather than the installed\n"
         "GA starter path.\n\n"
         "Fetch dependencies with:\n\n"
         "```bash\n"
         "nebula fetch .\n"
         "```\n\n"
         "Run it with:\n\n"
         "```bash\n"
         "nebula run . --run-gate none\n"
         "```\n\n"
         "Recommended probes:\n\n"
         "```bash\n"
         "curl http://127.0.0.1:40480/healthz\n"
         "curl http://127.0.0.1:40480/readyz\n"
         "curl http://127.0.0.1:40480/hello/nebula\n"
         "```\n\n"
         "Collector-side `/metrics` bridge:\n\n"
         "```bash\n"
         "nebula run . --run-gate none 2> service.observe.ndjson\n"
         "python3 " + bridge_path.string() + " serve --input service.observe.ndjson --port 9464\n"
         "curl http://127.0.0.1:9464/metrics\n"
         "```\n\n"
         "The bridge helper ships with the backend SDK beside `nebula-observe`; it renders the current\n"
         "delta-counter log seam as Prometheus text and does not turn this starter into a built-in\n"
         "Prometheus exporter.\n\n"
         "Guardrails for real services:\n\n"
         "- use `Result<Response, String>` or explicit `Response` values for recoverable request failures\n"
         "- do not use `panic(...)` as a request-local `500`; panics still abort the process\n"
         "- treat `timeout(...)` as a response deadline, not a hard cancellation guarantee\n"
         "- keep request handlers bounded and avoid hidden side effects that cannot tolerate late completion\n";
}

ScaffoldProject make_scaffold_project(NewProjectTemplateKind kind,
                                      const std::string& name,
                                      const fs::path& scaffold_root,
                                      const CliOptions& opt) {
  auto manifest_cfg = scaffold_manifest(name);
  if (kind == NewProjectTemplateKind::BackendService) {
    ManifestDependency service_dep;
    service_dep.alias = "service";
    service_dep.kind = ManifestDependencyKind::Installed;
    service_dep.installed = "nebula-service";
    manifest_cfg.dependencies.push_back(std::move(service_dep));
  }
  ScaffoldProject project;
  project.files.push_back({".gitignore", scaffold_gitignore()});
  if (kind != NewProjectTemplateKind::ControlPlaneWorkspace) {
    const std::string manifest = render_manifest(manifest_cfg);
    project.files.push_back({"nebula.toml", manifest});
  }

  switch (kind) {
  case NewProjectTemplateKind::Basic:
    project.template_name = "basic";
    project.files.push_back({"README.md", scaffold_basic_readme()});
    project.files.push_back({"src/main.nb",
                             "module main\n\n"
                             "fn main() -> Void {\n"
                             "  print(\"Hello from Nebula\")\n"
                             "}\n"});
    return project;
  case NewProjectTemplateKind::Cli:
    project.template_name = "cli";
    project.files.push_back({"README.md", scaffold_cli_readme()});
    project.files.push_back({"src/main.nb",
                             "module main\n"
                             "import app\n\n"
                             "fn main() -> Int {\n"
                             "  return run()\n"
                             "}\n"});
    project.files.push_back({"src/app.nb",
                             "module app\n"
                             "import io\n"
                             "import std::result\n\n"
                             "fn usage() -> Void {\n"
                             "  print(\"usage: nebula run . --run-gate none -- <input> <output>\")\n"
                             "}\n\n"
                             "fn run() -> Int {\n"
                             "  if argc() != 3 {\n"
                             "    usage()\n"
                             "    return 1\n"
                             "  }\n"
                             "  let input = argv(1)\n"
                             "  let output = argv(2)\n"
                             "  match copy_file(input, output) {\n"
                             "    Ok(_) => {\n"
                             "      return 0\n"
                             "    }\n"
                             "    Err(msg) => {\n"
                             "      print(msg)\n"
                             "      return 1\n"
                             "    }\n"
                             "  }\n"
                             "}\n"});
    project.files.push_back({"src/io.nb",
                             "module io\n"
                             "import std::fs\n"
                             "import std::result\n\n"
                             "fn copy_file(input: String, output: String) -> Result<Void, String> {\n"
                             "  match read_bytes(input) {\n"
                             "    Ok(data) => {\n"
                             "      return write_bytes(output, data)\n"
                             "    }\n"
                             "    Err(msg) => {\n"
                             "      return Err(msg)\n"
                             "    }\n"
                             "  }\n"
                             "}\n"});
    return project;
  case NewProjectTemplateKind::HttpService:
    project.template_name = "http-service";
    project.files.push_back({"README.md", scaffold_http_service_readme()});
    project.files.push_back({"src/main.nb",
                             "module main\n"
                             "import server\n"
                             "import std::result\n\n"
                             "async fn main() -> Void {\n"
                             "  match await run_service() {\n"
                             "    Ok(_) => {}\n"
                             "    Err(msg) => {\n"
                             "      print(msg)\n"
                             "    }\n"
                             "  }\n"
                             "}\n"});
    project.files.push_back({"src/server.nb",
                             "module server\n"
                             "import std::env\n"
                             "import std::http\n"
                             "import std::net\n"
                             "import std::result\n\n"
                             "fn bind_port() -> Result<Int, String> {\n"
                             "  return get_int_or(\"PORT\", 40480)\n"
                             "}\n\n"
                             "async fn handle_request(request: Request) -> Response {\n"
                             "  return await dispatch2(\n"
                             "    request,\n"
                             "    Get,\n"
                             "    \"/healthz\",\n"
                             "    handle_health,\n"
                             "    Get,\n"
                             "    \"/hello/:name\",\n"
                             "    handle_hello,\n"
                             "    not_found_text(\"not found\")\n"
                             "  )\n"
                             "}\n\n"
                             "async fn handle_health(request: Request) -> Response {\n"
                             "  return ok_text(\"ok\")\n"
                             "}\n\n"
                             "async fn handle_hello(request: Request) -> Response {\n"
                             "  match route_param1(\"/hello/:name\", request) {\n"
                             "    Ok(name) => {\n"
                             "      return ok_text(name)\n"
                             "    }\n"
                             "    Err(msg) => {\n"
                             "      return internal_error_text(msg)\n"
                             "    }\n"
                             "  }\n"
                             "}\n\n"
                             "async fn run_service() -> Result<Void, String> {\n"
                             "  match bind_port() {\n"
                             "    Ok(port) => {\n"
                             "      match ipv4(\"127.0.0.1\", port) {\n"
                             "        Ok(addr) => {\n"
                             "          match await bind(addr) {\n"
                             "            Ok(listener) => {\n"
                             "              return await serve(listener, handle_request)\n"
                             "            }\n"
                             "            Err(msg) => {\n"
                             "              return Err(msg)\n"
                             "            }\n"
                             "          }\n"
                             "        }\n"
                             "        Err(msg) => {\n"
                             "          return Err(msg)\n"
                             "        }\n"
                             "      }\n"
                             "    }\n"
                             "    Err(msg) => {\n"
                             "      return Err(msg)\n"
                             "    }\n"
                             "  }\n"
                             "}\n"});
    return project;
  case NewProjectTemplateKind::BackendService:
    project.template_name = "backend-service";
    project.files.push_back({"README.md", scaffold_backend_service_readme(opt.backend_sdk_root)});
    project.files.push_back({"src/main.nb",
                             "module main\n"
                             "import service::config\n"
                             "import service::drain\n"
                             "import service::server\n"
                             "import server\n\n"
                             "fn app_service_name() -> String {\n"
                             "  return \"" + name + "\"\n"
                             "}\n\n"
                             "async fn main() -> Void {\n"
                             "  match config_from_env(app_service_name(), 40480) {\n"
                             "    Ok(cfg) => {\n"
                             "      match from_env() {\n"
                             "        Ok(drain_cfg) => {\n"
                             "          match await bind_listener(cfg) {\n"
                             "            Ok(listener) => {\n"
                             "              match await serve_requests_result_until_drained(listener, cfg, drain_cfg, handle_request) {\n"
                             "                Ok(_) => {}\n"
                             "                Err(msg) => { print(msg) }\n"
                             "              }\n"
                             "            }\n"
                             "            Err(msg) => { print(msg) }\n"
                             "          }\n"
                             "        }\n"
                             "        Err(msg) => { print(msg) }\n"
                             "      }\n"
                             "    }\n"
                             "    Err(msg) => { print(msg) }\n"
                             "  }\n"
                             "}\n"});
    project.files.push_back({"src/server.nb",
                             "module server\n"
                             "import service::context\n"
                             "import service::errors\n"
                             "import service::health\n"
                             "import service::middleware\n"
                             "import service::routing\n"
                             "import std::http\n"
                             "import std::result\n\n"
                             "async fn handle_request(ctx: RequestContext, request: Request) -> Result<Response, String> {\n"
                             "  return await dispatch_ctx3_result(\n"
                             "    ctx,\n"
                             "    request,\n"
                             "    Get,\n"
                             "    \"/healthz\",\n"
                             "    handle_health_route,\n"
                             "    Get,\n"
                             "    \"/readyz\",\n"
                             "    handle_ready_route,\n"
                             "    Get,\n"
                             "    \"/hello/:name\",\n"
                             "    handle_hello_route,\n"
                             "    not_found_text(\"not found\")\n"
                             "  )\n"
                             "}\n\n"
                             "async fn handle_health_route(ctx: RequestContext, request: Request) -> Result<Response, String> {\n"
                             "  return Ok(live_response_with_context(ctx))\n"
                             "}\n\n"
                             "async fn handle_ready_route(ctx: RequestContext, request: Request) -> Result<Response, String> {\n"
                             "  if ctx.is_draining() {\n"
                             "    return Ok(draining_response_with_context(ctx))\n"
                             "  }\n"
                             "  return Ok(ready_response_with_context(ctx))\n"
                             "}\n\n"
                             "async fn handle_hello_route(ctx: RequestContext, request: Request) -> Result<Response, String> {\n"
                             "  return await reject_when_draining_result(ctx, request, handle_hello_route_inner)\n"
                             "}\n\n"
                             "async fn handle_hello_route_inner(ctx: RequestContext, request: Request) -> Result<Response, String> {\n"
                             "  match route_param1(\"/hello/:name\", request) {\n"
                             "    Ok(name) => {\n"
                             "      return Ok(ok_text(name))\n"
                             "    }\n"
                             "    Err(msg) => {\n"
                             "      return Err(msg)\n"
                             "    }\n"
                             "  }\n"
                             "}\n"});
    return project;
  case NewProjectTemplateKind::ControlPlaneWorkspace: {
    project.template_name = "control-plane-workspace";
    const auto source_root = control_plane_template_source_root(opt);
    const auto auth_root = repo_preview_package_root(opt, "nebula-auth");
    const auto config_root = repo_preview_package_root(opt, "nebula-config");
    const auto crypto_root = repo_preview_package_root(opt, "nebula-crypto");
    const auto postgres_root = repo_preview_package_root(opt, "nebula-db-postgres");
    const auto jobs_root = repo_preview_package_root(opt, "nebula-jobs");
    const auto tls_root = repo_preview_package_root(opt, "nebula-tls");
    if (!source_root.has_value() || !installed_backend_package_ready(opt, "nebula-db-sqlite") ||
        !auth_root.has_value() || !config_root.has_value() || !crypto_root.has_value() || !postgres_root.has_value() ||
        !jobs_root.has_value() || !tls_root.has_value()) {
      return project;
    }
    if (!append_scaffold_files_from_directory(project, *source_root)) {
      project.files.clear();
      return project;
    }
    const std::string auth_service_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "service", *auth_root);
    const std::string auth_ctl_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "ctl", *auth_root);
    const std::string crypto_service_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "service", *crypto_root);
    const std::string crypto_ctl_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "ctl", *crypto_root);
    const std::string config_service_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "service", *config_root);
    const std::string config_core_path =
        scaffold_preview_package_dependency(scaffold_root / "packages" / "core", *config_root);
    const std::string jobs_service_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "service", *jobs_root);
    const std::string jobs_core_path =
        scaffold_preview_package_dependency(scaffold_root / "packages" / "core", *jobs_root);
    const std::string postgres_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "service", *postgres_root);
    const std::string tls_path =
        scaffold_preview_package_dependency(scaffold_root / "apps" / "ctl", *tls_root);
    for (auto& file : project.files) {
      if (file.path == fs::path("apps/service/nebula.toml")) {
        replace_all(file.text,
                    "db_sqlite = { path = \"../../../../official/nebula-db-sqlite\" }",
                    "db_sqlite = { installed = \"nebula-db-sqlite\" }");
        replace_all(file.text, "../../../../official/nebula-db-postgres", postgres_path);
        replace_all(file.text, "../../../../official/nebula-auth", auth_service_path);
        replace_all(file.text, "../../../../official/nebula-config", config_service_path);
        replace_all(file.text, "../../../../official/nebula-crypto", crypto_service_path);
        replace_all(file.text, "../../../../official/nebula-jobs", jobs_service_path);
      } else if (file.path == fs::path("apps/ctl/nebula.toml")) {
        replace_all(file.text, "../../../../official/nebula-tls", tls_path);
        replace_all(file.text, "../../../../official/nebula-auth", auth_ctl_path);
        replace_all(file.text, "../../../../official/nebula-crypto", crypto_ctl_path);
      } else if (file.path == fs::path("packages/core/nebula.toml")) {
        replace_all(file.text, "../../../../official/nebula-jobs", jobs_core_path);
        replace_all(file.text, "../../../../official/nebula-config", config_core_path);
      } else if (file.path == fs::path("README.md")) {
        replace_all(file.text,
                    "embedded SQLite persistence through the preview `official/nebula-db-sqlite` package",
                    "embedded SQLite persistence through the preview installed `nebula-db-sqlite` package");
      }
    }
    return project;
  }
  }

  return project;
}

void print_new_usage(std::ostream& os) {
  os << "usage: nebula new <path> [--template " << new_project_template_names() << "]\n";
}

void print_fmt_usage(std::ostream& os) {
  os << "usage: nebula fmt <file-or-dir>\n";
}

} // namespace

int cmd_new(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() >= 3 && (args[2] == "--help" || args[2] == "-h" || args[2] == "-help")) {
    print_new_usage(std::cout);
    return 0;
  }
  if (args.size() < 3) {
    std::cerr << "error: ";
    print_new_usage(std::cerr);
    return 2;
  }

  std::optional<fs::path> root_input;
  NewProjectTemplateKind template_kind = NewProjectTemplateKind::Basic;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (args[i] == "--template") {
      if (i + 1 >= args.size()) {
        std::cerr << "error: missing value for --template\n";
        return 2;
      }
      auto parsed = parse_new_project_template(args[i + 1]);
      if (!parsed.has_value()) {
        std::cerr << "error: unknown template '" << args[i + 1] << "' (expected "
                  << new_project_template_names() << ")\n";
        return 2;
      }
      template_kind = *parsed;
      i += 1;
      continue;
    }
    if (root_input.has_value()) {
      std::cerr << "error: unexpected argument '" << args[i] << "'\n";
      return 2;
    }
    root_input = fs::path(args[i]);
  }

  if (!root_input.has_value()) {
    std::cerr << "error: ";
    print_new_usage(std::cerr);
    return 2;
  }

  if (template_kind == NewProjectTemplateKind::BackendService && !backend_sdk_ready(opt)) {
    std::cerr << "error: backend-service template requires the Nebula backend SDK\n";
    if (!opt.backend_sdk_root_error.empty()) {
      std::cerr << "note: " << opt.backend_sdk_root_error << "\n";
    }
    std::cerr << "hint: install Nebula on Linux with --with-backend-sdk, or use a repo checkout build where official/ is present\n";
    return 1;
  }
  if (template_kind == NewProjectTemplateKind::ControlPlaneWorkspace && !control_plane_template_ready(opt)) {
    std::cerr << "error: control-plane-workspace template requires a repo checkout build with the Nebula backend SDK,\n"
                 "       installed-preview nebula-db-sqlite, official/nebula-auth, official/nebula-config, official/nebula-crypto,\n"
                 "       official/nebula-db-postgres, official/nebula-jobs, official/nebula-tls,\n"
                 "       and examples/release_control_plane_workspace\n";
    if (!backend_sdk_ready(opt) && !opt.backend_sdk_root_error.empty()) {
      std::cerr << "note: " << opt.backend_sdk_root_error << "\n";
    }
    std::cerr << "hint: this template is repo-local preview wiring and is not shipped in installed release archives yet\n";
    return 1;
  }

  const fs::path root = fs::absolute(*root_input);
  std::error_code ec;
  if (fs::exists(root, ec)) {
    if (ec || !fs::is_directory(root, ec)) {
      std::cerr << "error: target path is not a directory: " << root.string() << "\n";
      return 1;
    }
    if (!fs::is_empty(root, ec)) {
      std::cerr << "error: target directory is not empty: " << root.string() << "\n";
      return 1;
    }
  }

  const std::string name = scaffold_package_name(root);
  const auto project = make_scaffold_project(template_kind, name, root, opt);
  if (project.files.empty()) {
    std::cerr << "error: failed to scaffold template assets for " << root.string() << "\n";
    return 1;
  }
  for (const auto& file : project.files) {
    if (fs::exists(root / file.path)) {
      std::cerr << "error: refusing to overwrite existing file " << (root / file.path).string() << "\n";
      return 1;
    }
  }

  for (const auto& file : project.files) {
    if (write_text_file(root / file.path, file.text)) continue;
    std::cerr << "error: failed to scaffold project under " << root.string() << "\n";
    return 1;
  }

  std::cout << "created: " << root.string() << " (" << project.template_name << ")\n";
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
    if (!preflight_git_dependency_environment(
            dep, diags, nebula::frontend::DiagnosticStage::Preflight, true, "add")) {
      for (const auto& d : diags) nebula::frontend::print_diagnostic(std::cerr, d);
      return 1;
    }
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

int cmd_fetch(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula fetch <project-or-manifest> [--registry-url URL] [--registry-token TOKEN] [--registry-timeout-seconds N] [--registry-root PATH]\n";
    return 2;
  }

  HostedRegistryPreviewOptions registry;
  bool force = false;
  std::string parse_error;
  if (!parse_hosted_registry_options(args, 3, registry, true, force, parse_error)) {
    std::cerr << "error: " << parse_error << "\n";
    return 2;
  }
  if (force) {
    std::cerr << "error: unknown option: --force\n";
    return 2;
  }
  const fs::path input = fs::absolute(args[2]);
  if (hosted_registry_enabled(registry)) {
    const int mirror_rc = run_hosted_registry_fetch_preview(opt, registry, input);
    if (mirror_rc != 0) return mirror_rc;
  }

  std::vector<Diagnostic> diags;
  ProjectLock lock;
  if (!resolve_project_lock(input,
                            lock,
                            diags,
                            nebula::frontend::DiagnosticStage::Build,
                            opt.backend_sdk_root,
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

int cmd_update(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula update <project-or-manifest> [--registry-url URL] [--registry-token TOKEN] [--registry-timeout-seconds N] [--registry-root PATH]\n";
    return 2;
  }

  HostedRegistryPreviewOptions registry;
  bool force = false;
  std::string parse_error;
  if (!parse_hosted_registry_options(args, 3, registry, true, force, parse_error)) {
    std::cerr << "error: " << parse_error << "\n";
    return 2;
  }
  if (force) {
    std::cerr << "error: unknown option: --force\n";
    return 2;
  }
  const fs::path input = fs::absolute(args[2]);
  if (hosted_registry_enabled(registry)) {
    const int mirror_rc = run_hosted_registry_fetch_preview(opt, registry, input);
    if (mirror_rc != 0) return mirror_rc;
  }

  std::vector<Diagnostic> diags;
  ProjectLock lock;
  if (!resolve_project_lock(input,
                            lock,
                            diags,
                            nebula::frontend::DiagnosticStage::Build,
                            opt.backend_sdk_root,
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

int cmd_publish(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() < 3) {
    std::cerr << "error: usage: nebula publish <project-or-manifest> [--force] [--registry-url URL] [--registry-token TOKEN] [--registry-timeout-seconds N]\n";
    return 2;
  }

  HostedRegistryPreviewOptions registry;
  bool force = false;
  std::string parse_error;
  if (!parse_hosted_registry_options(args, 3, registry, false, force, parse_error)) {
    std::cerr << "error: " << parse_error << "\n";
    return 2;
  }
  const fs::path input = fs::absolute(args[2]);
  if (hosted_registry_enabled(registry)) {
    return run_hosted_registry_publish_preview(opt, registry, input, force);
  }

  std::vector<Diagnostic> diags;
  PublishPackageResult result;
  if (!publish_package_to_local_registry(input, result, diags,
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
  if (args.size() >= 3 && (args[2] == "--help" || args[2] == "-h" || args[2] == "-help")) {
    print_fmt_usage(std::cout);
    return 0;
  }
  if (args.size() < 3) {
    std::cerr << "error: ";
    print_fmt_usage(std::cerr);
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

namespace {

void print_explain_usage(std::ostream& os) {
  os << "usage: nebula explain <path> [--file PATH] [--line N] [--col N] [--symbol NAME] [--format text|json]\n";
}

} // namespace

int cmd_explain(const std::vector<std::string>& args, const CliOptions& opt) {
  if (args.size() >= 3 && (args[2] == "--help" || args[2] == "-h" || args[2] == "-help")) {
    print_explain_usage(std::cout);
    return 0;
  }
  if (args.size() < 3) {
    std::cerr << "error: ";
    print_explain_usage(std::cerr);
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

  auto loaded =
      load_compile_input(input, nebula::frontend::DiagnosticStage::Build, opt.std_root,
                         opt.backend_sdk_root);
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

  std::vector<AsyncExplainEntry> async_entries =
      result.async_explain ? result.async_explain->entries
                           : collect_async_explain_entries(loaded.compile_sources);
  std::vector<AsyncExplainEntry> selected_async;
  for (const auto& entry : async_entries) {
    const std::string entry_path = fs::absolute(entry.path).lexically_normal().string();
    if (!effective_file_filter.empty() && entry_path != effective_file_filter) continue;
    if (!symbol.empty()) {
      const bool carried_match =
          std::find(entry.carried_values.begin(), entry.carried_values.end(), symbol) != entry.carried_values.end();
      if (entry.function_name != symbol && !carried_match) continue;
    } else if (line > 0) {
      const bool line_matches = entry.span.start.line <= line && entry.span.end.line >= line;
      if (!line_matches) continue;
      if (col > 0) {
        const bool same_start_line = entry.span.start.line == line;
        const bool same_end_line = entry.span.end.line == line;
        if (same_start_line && entry.span.start.col > col) continue;
        if (same_end_line && entry.span.end.col < col) continue;
      }
    }
    selected_async.push_back(entry);
  }

  std::vector<TransportExplainEntry> transport_entries =
      collect_transport_explain_entries(loaded.compile_sources);
  std::vector<TransportExplainEntry> selected_transport;
  for (const auto& entry : transport_entries) {
    const std::string entry_path = fs::absolute(entry.path).lexically_normal().string();
    if (!effective_file_filter.empty() && entry_path != effective_file_filter) continue;
    if (!symbol.empty()) {
      const bool symbol_match =
          entry.symbol_name == symbol || entry.kind == symbol || entry.surface_name == symbol;
      if (!symbol_match) continue;
    } else if (line > 0) {
      const bool line_matches = entry.span.start.line <= line && entry.span.end.line >= line;
      if (!line_matches) continue;
      if (col > 0) {
        const bool same_start_line = entry.span.start.line == line;
        const bool same_end_line = entry.span.end.line == line;
        if (same_start_line && entry.span.start.col > col) continue;
        if (same_end_line && entry.span.end.col < col) continue;
      }
    }
    selected_transport.push_back(entry);
  }

  const std::string query_kind =
      !symbol.empty() ? "symbol" : ((line > 0 || !effective_file_filter.empty()) ? "span" : "all");
  const bool inferred_result =
      !symbol.empty() || !vars.empty() || !selected_async.empty() || !selected_transport.empty();

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

  auto emit_async_json = [&](std::ostringstream& out_json, const AsyncExplainEntry& entry) {
    out_json << "{";
    out_json << "\"kind\":\"" << json_escape(entry.kind) << "\",";
    out_json << "\"function\":\"" << json_escape(entry.function_name) << "\",";
    out_json << "\"path\":\"" << json_escape(entry.path) << "\",";
    out_json << "\"line\":" << entry.span.start.line << ",";
    out_json << "\"col\":" << entry.span.start.col << ",";
    out_json << "\"end_line\":" << entry.span.end.line << ",";
    out_json << "\"end_col\":" << entry.span.end.col << ",";
    out_json << "\"summary\":\"" << json_escape(entry.summary) << "\",";
    out_json << "\"allocation\":\"" << json_escape(entry.allocation) << "\",";
    out_json << "\"reason\":\"" << json_escape(entry.reason) << "\",";
    out_json << "\"suspension_point\":" << (entry.suspension_point ? "true" : "false") << ",";
    out_json << "\"task_boundary\":" << (entry.task_boundary ? "true" : "false") << ",";
    out_json << "\"carried_values\":[";
    for (std::size_t i = 0; i < entry.carried_values.size(); ++i) {
      if (i) out_json << ",";
      out_json << "\"" << json_escape(entry.carried_values[i]) << "\"";
    }
    out_json << "]";
    out_json << "}";
  };

  auto emit_transport_json = [&](std::ostringstream& out_json, const TransportExplainEntry& entry) {
    out_json << "{";
    out_json << "\"kind\":\"" << json_escape(entry.kind) << "\",";
    out_json << "\"symbol\":\"" << json_escape(entry.symbol_name) << "\",";
    out_json << "\"surface\":\"" << json_escape(entry.surface_name) << "\",";
    out_json << "\"function\":\"" << json_escape(entry.enclosing_function) << "\",";
    out_json << "\"path\":\"" << json_escape(entry.path) << "\",";
    out_json << "\"line\":" << entry.span.start.line << ",";
    out_json << "\"col\":" << entry.span.start.col << ",";
    out_json << "\"end_line\":" << entry.span.end.line << ",";
    out_json << "\"end_col\":" << entry.span.end.col << ",";
    out_json << "\"summary\":\"" << json_escape(entry.summary) << "\",";
    out_json << "\"contract\":" << cli_json::render_compact(transport_debug_contract_json(entry.kind));
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
    out_json << "\"async\":[";
    for (std::size_t i = 0; i < selected_async.size(); ++i) {
      if (i) out_json << ",";
      emit_async_json(out_json, selected_async[i]);
    }
    out_json << "],";
    out_json << "\"transport\":[";
    for (std::size_t i = 0; i < selected_transport.size(); ++i) {
      if (i) out_json << ",";
      emit_transport_json(out_json, selected_transport[i]);
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
    out_json << "}";
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
    for (const auto& entry : selected_async) {
      std::cout << "async: kind=" << entry.kind;
      if (!entry.function_name.empty()) std::cout << " fn=" << entry.function_name;
      std::cout << " @" << entry.path << ":" << entry.span.start.line << ":" << entry.span.start.col;
      if (!entry.summary.empty()) std::cout << " summary=" << entry.summary;
      if (!entry.allocation.empty()) std::cout << " allocation=" << entry.allocation;
      if (entry.task_boundary) std::cout << " task_boundary=true";
      if (entry.suspension_point) std::cout << " suspension_point=true";
      if (!entry.carried_values.empty()) {
        std::cout << " carried=";
        for (std::size_t i = 0; i < entry.carried_values.size(); ++i) {
          if (i) std::cout << ",";
          std::cout << entry.carried_values[i];
        }
      }
      std::cout << "\n";
      if (!entry.reason.empty()) std::cout << "  why: " << entry.reason << "\n";
    }
    for (const auto& entry : selected_transport) {
      std::cout << "transport: kind=" << entry.kind
                << " surface=" << entry.surface_name
                << " @" << entry.path << ":" << entry.span.start.line << ":" << entry.span.start.col;
      if (!entry.enclosing_function.empty()) std::cout << " fn=" << entry.enclosing_function;
      if (!entry.summary.empty()) std::cout << " summary=" << entry.summary;
      std::cout << "\n";
      std::cout << "  contract: "
                << cli_json::render_compact(transport_debug_contract_json(entry.kind)) << "\n";
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

int cmd_lsp(const std::vector<std::string>& args, const CliOptions& opt) {
  g_lsp_std_root = opt.std_root;
  g_lsp_backend_sdk_root = opt.backend_sdk_root;
  std::unordered_map<std::string, LspDocumentState> docs;
  auto handle_body = [&](const std::string& body) -> bool {
    const auto parsed = parse_lsp_request(body);
    if (!parsed.request.has_value()) {
      lsp_write_error(parsed.id_json, parsed.error_code, parsed.error_message);
      return false;
    }

    const LspRequest& request = *parsed.request;
    const std::string id_json = lsp_id_json(request);
    const std::string& method = request.method;
    if (method == "initialize") {
      if (lsp_request_id(request) == nullptr) {
        lsp_write_error("null", -32600, "invalid request: initialize requires an id");
        return false;
      }
      cli_json::Value::Object capabilities;
      cli_json::Value::Object text_document_sync;
      text_document_sync["openClose"] = cli_json::Value(true);
      text_document_sync["change"] = json_i64(2);
      capabilities["textDocumentSync"] = cli_json::Value(std::move(text_document_sync));
      capabilities["hoverProvider"] = cli_json::Value(true);
      capabilities["definitionProvider"] = cli_json::Value(true);
      capabilities["referencesProvider"] = cli_json::Value(true);
      capabilities["documentSymbolProvider"] = cli_json::Value(true);
      cli_json::Value::Object completion_provider;
      completion_provider["resolveProvider"] = cli_json::Value(false);
      capabilities["completionProvider"] = cli_json::Value(std::move(completion_provider));
      capabilities["renameProvider"] = cli_json::Value(true);
      capabilities["workspaceSymbolProvider"] = cli_json::Value(true);
      capabilities["codeActionProvider"] = cli_json::Value(true);
      cli_json::Value::Object signature_help_provider;
      cli_json::Value::Array signature_triggers;
      signature_triggers.push_back(cli_json::Value("("));
      signature_triggers.push_back(cli_json::Value(","));
      signature_help_provider["triggerCharacters"] = cli_json::Value(std::move(signature_triggers));
      capabilities["signatureHelpProvider"] = cli_json::Value(std::move(signature_help_provider));
      cli_json::Value::Object semantic_tokens_provider;
      cli_json::Value::Object semantic_legend;
      cli_json::Value::Array token_types;
      for (const char* token_type : {"namespace",
                                     "type",
                                     "struct",
                                     "enum",
                                     "enumMember",
                                     "function",
                                     "method",
                                     "parameter",
                                     "variable",
                                     "property",
                                     "keyword",
                                     "number",
                                     "string"}) {
        token_types.push_back(cli_json::Value(token_type));
      }
      semantic_legend["tokenTypes"] = cli_json::Value(std::move(token_types));
      semantic_legend["tokenModifiers"] = cli_json::Value(cli_json::Value::Array{});
      semantic_tokens_provider["legend"] = cli_json::Value(std::move(semantic_legend));
      semantic_tokens_provider["full"] = cli_json::Value(true);
      capabilities["semanticTokensProvider"] = cli_json::Value(std::move(semantic_tokens_provider));
      cli_json::Value::Object result;
      result["capabilities"] = cli_json::Value(std::move(capabilities));
      lsp_write_response(id_json, cli_json::Value(std::move(result)));
    } else if (method == "textDocument/didOpen") {
      const auto uri = lsp_request_uri(request);
      const auto text = lsp_request_open_text(request);
      if (uri.has_value() && text.has_value()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        LspDocumentState state{*uri, path, *text, {}};
        if (parse_lsp_program(path, *text).has_value()) state.last_good_parse_text = *text;
        docs[path] = std::move(state);
        lsp_publish_diagnostics(*uri, path, *text, &docs);
      }
    } else if (method == "textDocument/didChange") {
      const auto uri = lsp_request_uri(request);
      const auto changes = lsp_request_content_changes(request);
      if (uri.has_value() && !changes.empty()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        auto it = docs.find(path);
        std::string current_text;
        std::string last_good;
        if (it != docs.end()) {
          current_text = it->second.text;
          last_good = it->second.last_good_parse_text;
        }
        if (!apply_lsp_content_changes(current_text, changes)) {
          if (lsp_request_id(request) != nullptr) {
            lsp_write_error(id_json, -32602,
                            "invalid params: didChange supplied an invalid incremental change range");
          }
          return false;
        }
        LspDocumentState state{*uri, path, std::move(current_text), std::move(last_good)};
        if (parse_lsp_program(path, state.text).has_value()) state.last_good_parse_text = state.text;
        docs[path] = std::move(state);
        lsp_publish_diagnostics(*uri, path, docs[path].text, &docs);
      } else if (lsp_request_id(request) != nullptr) {
        lsp_write_error(id_json, -32602,
                        "invalid params: didChange requires contentChanges with text and optional ranges");
      }
    } else if (method == "textDocument/didClose") {
      const auto uri = lsp_request_uri(request);
      if (uri.has_value()) {
        const std::string path = normalized_path_string(uri_to_path(*uri));
        docs.erase(path);
        cli_json::Value::Object params;
        params["uri"] = cli_json::Value(*uri);
        params["diagnostics"] = cli_json::Value(cli_json::Value::Array{});
        lsp_write_notification("textDocument/publishDiagnostics", cli_json::Value(std::move(params)));
      }
    } else if (method == "shutdown") {
      lsp_write_response(id_json, cli_json::Value(nullptr));
    } else if (method == "exit") {
      return true;
    } else if (method == "initialized" || method == "$/cancelRequest") {
      return false;
    } else if (method == "textDocument/hover") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (const auto character =
                    lsp_character_to_source_col_one_based(text, *line, *lsp_character);
                character.has_value()) {
              if (auto result = hover_result_json(path, text, *line, *character - 1, &docs);
                  result.has_value()) {
                lsp_write_response(id_json, *result);
                return false;
              }
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: hover requires textDocument.uri and position line/character");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(nullptr));
      }
    } else if (method == "textDocument/definition") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (const auto character =
                    lsp_character_to_source_col_one_based(text, *line, *lsp_character);
                character.has_value()) {
              if (auto result = definition_result_json(path, text, *line, *character - 1, &docs);
                  result.has_value()) {
                lsp_write_response(id_json, *result);
                return false;
              }
            }
          }
        } else {
          lsp_write_error(
              id_json, -32602,
              "invalid params: definition requires textDocument.uri and position line/character");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(nullptr));
      }
    } else if (method == "textDocument/completion") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (const auto character =
                    lsp_character_to_source_col_one_based(text, *line, *lsp_character);
                character.has_value()) {
              if (auto result = completion_result_json(path, text, *line, *character - 1, &docs);
                  result.has_value()) {
                lsp_write_response(id_json, *result);
                return false;
              }
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: completion requires textDocument.uri and position line/character");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(cli_json::Value::Array{}));
      }
    } else if (method == "textDocument/references") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (const auto character =
                    lsp_character_to_source_col_one_based(text, *line, *lsp_character);
                character.has_value()) {
              if (auto result = references_result_json(path, text, *line, *character - 1, &docs);
                  result.has_value()) {
                lsp_write_response(id_json, *result);
                return false;
              }
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: references requires textDocument.uri and position line/character");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(cli_json::Value::Array{}));
      }
    } else if (method == "textDocument/documentSymbol") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        if (uri.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = document_symbols_result_json(path, text, &docs); result.has_value()) {
              lsp_write_response(id_json, *result);
              return false;
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: documentSymbol requires textDocument.uri");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(cli_json::Value::Array{}));
      }
    } else if (method == "workspace/symbol") {
      if (lsp_request_id(request) != nullptr) {
        const auto query = lsp_request_query(request).value_or("");
        if (auto result = workspace_symbols_result_json(query, &docs); result.has_value()) {
          lsp_write_response(id_json, *result);
        } else {
          lsp_write_response(id_json, cli_json::Value(cli_json::Value::Array{}));
        }
      }
    } else if (method == "textDocument/signatureHelp") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = signature_help_result_json(path, text, *line, *lsp_character, &docs);
                result.has_value()) {
              lsp_write_response(id_json, *result);
              return false;
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: signatureHelp requires textDocument.uri and position line/character");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(nullptr));
      }
    } else if (method == "textDocument/codeAction") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        if (uri.has_value()) {
          const auto requested_range = lsp_request_range(request);
          const auto only_kinds = lsp_request_code_action_only(request);
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = code_actions_result_json(path, text, requested_range, only_kinds, &docs);
                result.has_value()) {
              lsp_write_response(id_json, *result);
              return false;
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: codeAction requires textDocument.uri");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(cli_json::Value::Array{}));
      }
    } else if (method == "textDocument/rename") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        const auto line = lsp_request_line(request);
        const auto lsp_character = lsp_request_character(request);
        const auto new_name = lsp_request_new_name(request);
        if (uri.has_value() && line.has_value() && lsp_character.has_value() && new_name.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = rename_result_json(path, text, *line, *lsp_character, *new_name, &docs);
                result.has_value()) {
              lsp_write_response(id_json, *result);
              return false;
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: rename requires textDocument.uri, position, and newName");
          return false;
        }
        lsp_write_response(id_json, cli_json::Value(nullptr));
      }
    } else if (method == "textDocument/semanticTokens/full") {
      if (lsp_request_id(request) != nullptr) {
        const auto uri = lsp_request_uri(request);
        if (uri.has_value()) {
          const std::string path = normalized_path_string(uri_to_path(*uri));
          auto doc_it = docs.find(path);
          std::string text;
          if (doc_it != docs.end()) text = doc_it->second.text;
          else {
            std::vector<Diagnostic> diags;
            if (!read_source(path, text, diags, nebula::frontend::DiagnosticStage::Build)) text.clear();
          }
          if (!text.empty()) {
            if (auto result = semantic_tokens_result_json(path, text, &docs); result.has_value()) {
              lsp_write_response(id_json, *result);
              return false;
            }
          }
        } else {
          lsp_write_error(id_json, -32602,
                          "invalid params: semanticTokens/full requires textDocument.uri");
          return false;
        }
        cli_json::Value::Object empty;
        empty["data"] = cli_json::Value(cli_json::Value::Array{});
        lsp_write_response(id_json, cli_json::Value(std::move(empty)));
      }
    } else if (lsp_request_id(request) != nullptr) {
      lsp_write_error(id_json, -32601, "method not implemented");
    }
    return false;
  };

  auto process_stream = [&](std::istream& in) -> int {
    while (true) {
      const auto header_result = read_lsp_headers(in);
      if (header_result.eof) break;
      if (!header_result.ok) {
        lsp_write_error("null", -32600, header_result.error_message);
        break;
      }
      if (header_result.headers.content_length >
          static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        lsp_write_error("null", -32600, "invalid request header: Content-Length is too large");
        break;
      }

      const std::size_t content_length = header_result.headers.content_length;
      std::string body(content_length, '\0');
      in.read(body.data(), static_cast<std::streamsize>(content_length));
      if (in.gcount() != static_cast<std::streamsize>(content_length)) {
        lsp_write_error("null", -32700, "parse error: truncated request body");
        break;
      }
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
