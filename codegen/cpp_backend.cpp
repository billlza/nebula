#include "codegen/cpp_backend.hpp"

#include "frontend/types.hpp"
#include "nir/runtime_ops.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace nebula::codegen {

namespace {

using nebula::frontend::Ty;
using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Item;
using nebula::nir::Param;
using nebula::nir::PrefixKind;
using nebula::nir::Program;
using nebula::nir::Stmt;
using nebula::nir::StructDef;
using nebula::nir::EnumDef;
using nebula::nir::VarId;
using nebula::passes::RepOwnerResult;
using nebula::passes::StorageDecision;
using nebula::passes::OwnerKind;
using nebula::passes::RepKind;

struct Cpp {
  std::ostringstream os;
  int indent = 0;

  void line(const std::string& s) {
    for (int i = 0; i < indent; ++i) os << "  ";
    os << s << "\n";
  }

  void blank() { os << "\n"; }
};

static std::string escape_string(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
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

static std::string stable_symbol_hash(std::string_view text) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : text) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  std::ostringstream os;
  os << std::hex << h;
  return os.str();
}

static std::string sanitize_ident_piece(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) return "fn";
  if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
  return out;
}

static const char* runtime_profile_name(RuntimeProfile profile) {
  switch (profile) {
  case RuntimeProfile::Hosted: return "hosted";
  case RuntimeProfile::System: return "system";
  }
  return "hosted";
}

static const char* panic_policy_name(PanicPolicy policy) {
  switch (policy) {
  case PanicPolicy::Abort: return "abort";
  case PanicPolicy::Trap: return "trap";
  case PanicPolicy::Unwind: return "unwind";
  }
  return "abort";
}

static void emit_panic_policy(Cpp& out, const EmitOptions& opt, const std::string& msg_expr) {
  switch (opt.panic_policy) {
  case PanicPolicy::Abort:
    out.line("nebula::rt::panic(" + msg_expr + ");");
    return;
  case PanicPolicy::Trap:
    out.line("std::cerr << \"nebula panic: \" << " + msg_expr + " << \"\\n\";");
    out.line("#if defined(__clang__) || defined(__GNUC__)");
    out.line("__builtin_trap();");
    out.line("#else");
    out.line("std::abort();");
    out.line("#endif");
    return;
  case PanicPolicy::Unwind:
    out.line("throw nebula::rt::UserPanic(std::string(" + msg_expr + "));");
    return;
  }
  out.line("nebula::rt::panic(" + msg_expr + ");");
}

static std::string emitted_cpp_type_name_for_identity(std::string_view identity,
                                                      std::string_view fallback_name) {
  if (identity.empty()) return std::string(fallback_name);
  return "__nebula_ty_" + stable_symbol_hash(identity) + "_" + sanitize_ident_piece(fallback_name);
}

static std::string emitted_cpp_type_name_for(const std::optional<nebula::frontend::QualifiedName>& name,
                                             std::string_view fallback_name) {
  return emitted_cpp_type_name_for_identity(nebula::nir::qualified_identity(name), fallback_name);
}

static std::string emitted_cpp_type_name_for(const nebula::frontend::QualifiedName& name,
                                             std::string_view fallback_name) {
  return emitted_cpp_type_name_for_identity(nebula::nir::qualified_identity(name), fallback_name);
}

static std::string cpp_type(const Ty& t);

static bool matches_std_type(const Ty& t, std::string_view module_name, std::string_view local_name) {
  return t.qualified_name.has_value() && t.qualified_name->package_name == "std" &&
         t.qualified_name->module_name == module_name && t.qualified_name->local_name == local_name;
}

static bool is_nebula_crypto_bytes_backed_struct_name(const nebula::frontend::QualifiedName& name) {
  if (name.package_name != "nebula-crypto") return false;
  if (name.module_name == "aead") {
    return name.local_name == "ChaCha20Poly1305Key";
  }
  if (name.module_name == "pqc.kem") {
    return name.local_name == "MlKem768KeyPair" || name.local_name == "MlKem768PublicKey" ||
           name.local_name == "MlKem768SecretKey" || name.local_name == "MlKem768Ciphertext" ||
           name.local_name == "MlKem768SharedSecret" || name.local_name == "MlKem768Encapsulation";
  }
  if (name.module_name == "pqc.sign") {
    return name.local_name == "MlDsa65KeyPair" || name.local_name == "MlDsa65PublicKey" ||
           name.local_name == "MlDsa65SecretKey" || name.local_name == "MlDsa65Signature";
  }
  return false;
}

static bool matches_nebula_crypto_bytes_backed_type(const Ty& t) {
  return t.qualified_name.has_value() && is_nebula_crypto_bytes_backed_struct_name(*t.qualified_name);
}

static bool is_nebula_tls_handle_struct_name(const nebula::frontend::QualifiedName& name) {
  if (name.package_name == "nebula-tls") {
    if (name.module_name != "client") return false;
    return name.local_name == "TlsTrustStore" || name.local_name == "TlsServerName" ||
           name.local_name == "TlsClientIdentity" || name.local_name == "TlsVersionPolicy" ||
           name.local_name == "TlsAlpnPolicy" || name.local_name == "ClientConfig" ||
           name.local_name == "TlsClientStream";
  }
  if (name.package_name == "nebula-tls-server") {
    if (name.module_name != "server") return false;
    return name.local_name == "ServerIdentity" || name.local_name == "ServerConfig" ||
           name.local_name == "TlsListener" || name.local_name == "TlsServerStream";
  }
  return false;
}

static bool matches_nebula_tls_handle_type(const Ty& t) {
  return t.qualified_name.has_value() && is_nebula_tls_handle_struct_name(*t.qualified_name);
}

static bool is_nebula_db_sqlite_handle_struct_name(const nebula::frontend::QualifiedName& name) {
  if (name.package_name != "nebula-db-sqlite") return false;
  if (name.module_name != "sqlite") return false;
  return name.local_name == "Connection" || name.local_name == "Transaction" ||
         name.local_name == "ResultSet" || name.local_name == "Row";
}

static bool matches_nebula_db_sqlite_handle_type(const Ty& t) {
  return t.qualified_name.has_value() && is_nebula_db_sqlite_handle_struct_name(*t.qualified_name);
}

static bool is_nebula_db_postgres_handle_struct_name(const nebula::frontend::QualifiedName& name) {
  if (name.package_name != "nebula-db-postgres") return false;
  if (name.module_name != "postgres") return false;
  return name.local_name == "Connection" || name.local_name == "Transaction" ||
         name.local_name == "ResultSet" || name.local_name == "Row";
}

static bool matches_nebula_db_postgres_handle_type(const Ty& t) {
  return t.qualified_name.has_value() && is_nebula_db_postgres_handle_struct_name(*t.qualified_name);
}

static std::string future_cpp_type(const Ty& inner) {
  return "nebula::rt::Future<" + cpp_type(inner) + ">";
}

static std::string task_cpp_type(const Ty& inner) {
  return "nebula::rt::Task<" + cpp_type(inner) + ">";
}

static std::string cpp_type(const Ty& t) {
  auto callable_sig = [&](const Ty& cty, const std::string& decorated_name) -> std::string {
    std::ostringstream os;
    const std::string ret =
        cty.callable_ret ? cpp_type(*cty.callable_ret) : std::string("void");
    os << ret << " (" << decorated_name << ")(";
    for (std::size_t i = 0; i < cty.callable_params.size(); ++i) {
      if (i) os << ", ";
      os << cpp_type(cty.callable_params[i]);
      if (i < cty.callable_params_ref.size() && cty.callable_params_ref[i]) os << "&";
    }
    os << ")";
    return os.str();
  };

  switch (t.kind) {
  case Ty::Kind::Int: return "std::int64_t";
  case Ty::Kind::Float: return "double";
  case Ty::Kind::Bool: return "bool";
  case Ty::Kind::String: return "std::string";
  case Ty::Kind::Void: return "void";
  case Ty::Kind::Struct: {
    if (matches_nebula_crypto_bytes_backed_type(t)) {
      return "nebula::rt::Bytes";
    }
    if (matches_nebula_tls_handle_type(t)) {
      const auto& name = *t.qualified_name;
      if (name.package_name == "nebula-tls") {
        if (name.local_name == "TlsTrustStore") return "nebula::rt::TlsTrustStore";
        if (name.local_name == "TlsServerName") return "nebula::rt::TlsServerName";
        if (name.local_name == "TlsClientIdentity") return "nebula::rt::TlsClientIdentity";
        if (name.local_name == "TlsVersionPolicy") return "nebula::rt::TlsVersionPolicy";
        if (name.local_name == "TlsAlpnPolicy") return "nebula::rt::TlsAlpnPolicy";
        if (name.local_name == "ClientConfig") return "nebula::rt::TlsClientConfig";
        if (name.local_name == "TlsClientStream") return "nebula::rt::TlsClientStream";
      }
      if (name.package_name == "nebula-tls-server") {
        if (name.local_name == "ServerIdentity") return "nebula::rt::TlsServerIdentity";
        if (name.local_name == "ServerConfig") return "nebula::rt::TlsServerConfig";
        if (name.local_name == "TlsListener") return "nebula::rt::TlsServerListener";
        if (name.local_name == "TlsServerStream") return "nebula::rt::TlsServerStream";
      }
    }
    if (matches_nebula_db_sqlite_handle_type(t)) {
      if (t.qualified_name->local_name == "Connection") return "nebula::rt::SqliteConnection";
      if (t.qualified_name->local_name == "Transaction") return "nebula::rt::SqliteTransaction";
      if (t.qualified_name->local_name == "ResultSet") return "nebula::rt::SqliteResultSet";
      if (t.qualified_name->local_name == "Row") return "nebula::rt::SqliteRow";
    }
    if (matches_nebula_db_postgres_handle_type(t)) {
      if (t.qualified_name->local_name == "Connection") return "nebula::rt::PostgresConnection";
      if (t.qualified_name->local_name == "Transaction") return "nebula::rt::PostgresTransaction";
      if (t.qualified_name->local_name == "ResultSet") return "nebula::rt::PostgresResultSet";
      if (t.qualified_name->local_name == "Row") return "nebula::rt::PostgresRow";
    }
    if (matches_std_type(t, "task", "Future") && t.type_args.size() == 1) {
      return future_cpp_type(t.type_args.front());
    }
    if (matches_std_type(t, "task", "Task") && t.type_args.size() == 1) {
      return task_cpp_type(t.type_args.front());
    }
    if (matches_std_type(t, "time", "Duration")) {
      return "nebula::rt::Duration";
    }
    if (matches_std_type(t, "bytes", "Bytes")) {
      return "nebula::rt::Bytes";
    }
    if (matches_std_type(t, "net", "SocketAddr")) {
      return "nebula::rt::SocketAddr";
    }
    if (matches_std_type(t, "net", "TcpListener")) {
      return "nebula::rt::TcpListener";
    }
    if (matches_std_type(t, "net", "TcpStream")) {
      return "nebula::rt::TcpStream";
    }
    if (matches_std_type(t, "http", "Request")) {
      return "nebula::rt::HttpRequest";
    }
    if (matches_std_type(t, "http", "Response")) {
      return "nebula::rt::HttpResponse";
    }
    if (matches_std_type(t, "http", "ClientRequest")) {
      return "nebula::rt::HttpClientRequest";
    }
    if (matches_std_type(t, "http", "ClientResponse")) {
      return "nebula::rt::HttpClientResponse";
    }
    if (matches_std_type(t, "http", "RouteParams2")) {
      return "nebula::rt::HttpRouteParams2";
    }
    if (matches_std_type(t, "http", "RouteParams3")) {
      return "nebula::rt::HttpRouteParams3";
    }
    if (matches_std_type(t, "http", "RoutePattern")) {
      return "nebula::rt::HttpRoutePattern";
    }
    if (matches_std_type(t, "json", "Json")) {
      return "nebula::rt::JsonValue";
    }
    if (matches_std_type(t, "json", "JsonArrayBuilder")) {
      return "nebula::rt::JsonArrayBuilder";
    }
    if (matches_std_type(t, "process", "ProcessCommand")) {
      return "nebula::rt::ProcessCommand";
    }
    if (matches_std_type(t, "process", "ProcessOutput")) {
      return "nebula::rt::ProcessOutput";
    }
    std::string out = emitted_cpp_type_name_for(t.qualified_name, t.name);
    if (!t.type_args.empty()) {
      out += "<";
      for (std::size_t i = 0; i < t.type_args.size(); ++i) {
        if (i) out += ", ";
        out += cpp_type(t.type_args[i]);
      }
      out += ">";
    }
    return out;
  }
  case Ty::Kind::Enum:
    if (matches_std_type(t, "result", "Result") && t.type_args.size() == 2) {
      return "nebula::rt::Result<" + cpp_type(t.type_args[0]) + ", " + cpp_type(t.type_args[1]) +
             ">";
    }
    if (matches_std_type(t, "http", "Method")) {
      return "nebula::rt::HttpMethod";
    }
    if (!t.type_args.empty()) {
      std::string out = emitted_cpp_type_name_for(t.qualified_name, t.name) + "<";
      for (std::size_t i = 0; i < t.type_args.size(); ++i) {
        if (i) out += ", ";
        out += cpp_type(t.type_args[i]);
      }
      out += ">";
      return out;
    }
    return emitted_cpp_type_name_for(t.qualified_name, t.name);
  case Ty::Kind::TypeParam: return t.name;
  case Ty::Kind::Callable: return callable_sig(t, "*");
  case Ty::Kind::Unknown: return "auto";
  }
  return "auto";
}

static std::string cpp_decl(const Ty& t, const std::string& name, bool is_ref = false) {
  if (t.kind != Ty::Kind::Callable) {
    if (is_ref) return cpp_type(t) + "& " + name;
    return cpp_type(t) + " " + name;
  }

  std::ostringstream os;
  const std::string ret = t.callable_ret ? cpp_type(*t.callable_ret) : std::string("void");
  os << ret << " (*";
  if (is_ref) os << "&";
  os << name << ")(";
  for (std::size_t i = 0; i < t.callable_params.size(); ++i) {
    if (i) os << ", ";
    os << cpp_type(t.callable_params[i]);
    if (i < t.callable_params_ref.size() && t.callable_params_ref[i]) os << "&";
  }
  os << ")";
  return os.str();
}

static bool block_reassigns_var(const Block& b, VarId target);

static bool should_pass_param_by_const_ref(const Ty& t) {
  switch (t.kind) {
  case Ty::Kind::String:
  case Ty::Kind::Struct:
  case Ty::Kind::Enum: return true;
  default: return false;
  }
}

static std::string cpp_param_decl(const Function& fn, const Param& p) {
  if (p.is_ref) return cpp_decl(p.ty, p.name, true);
  if (fn.body.has_value() && block_reassigns_var(*fn.body, p.var)) {
    return cpp_decl(p.ty, p.name, false);
  }
  if (!fn.is_async && should_pass_param_by_const_ref(p.ty)) {
    return "const " + cpp_type(p.ty) + "& " + p.name;
  }
  return cpp_decl(p.ty, p.name, false);
}

static bool extern_uses_hosted_const_ref_contract(const Function& fn) {
  if (fn.is_async) return false;
  // Only exact native-hosted preview surfaces that opt into the C++ const-ref ABI belong here.
  // Runtime std externs keep their by-value wrapper signatures unless their definitions move too.
  static const std::unordered_set<std::string_view> kConstRefExterns = {
      "__nebula_ui_headless_action_summary_wire",
      "__nebula_ui_headless_dispatch_action_wire",
      "__nebula_ui_headless_dispatch_action_summary_wire",
      "__nebula_ui_typed_snapshot_text",
      "__nebula_rt_json_get_string",
      "__nebula_rt_json_get_int",
      "__nebula_rt_json_get_bool",
      "__nebula_rt_json_get_value",
      "__nebula_rt_json_as_string",
      "__nebula_rt_json_as_int",
      "__nebula_rt_json_as_bool",
      "__nebula_rt_json_array_len",
      "__nebula_rt_json_array_get",
      "__nebula_thin_host_generated_command_kind",
      "__nebula_thin_host_generated_command_correlation_id",
      "__nebula_thin_host_generated_command_state_revision",
      "__nebula_thin_host_generated_command_payload",
      "__nebula_thin_host_generated_command_payload_string",
      "__nebula_thin_host_generated_command_payload_int",
      "__nebula_thin_host_generated_command_payload_bool",
  };
  return kConstRefExterns.contains(fn.name);
}

static std::string cpp_extern_param_decl(const Function& fn, const Param& p) {
  if (p.is_ref) return cpp_decl(p.ty, p.name, true);
  if (extern_uses_hosted_const_ref_contract(fn) && should_pass_param_by_const_ref(p.ty)) {
    return "const " + cpp_type(p.ty) + "& " + p.name;
  }
  return cpp_decl(p.ty, p.name, false);
}

static std::string ctor_field_init_expr(const Ty& t, const std::string& name) {
  if (should_pass_param_by_const_ref(t)) {
    return "std::move(" + name + ")";
  }
  return name;
}

static bool has_annotation(const std::vector<std::string>& ann, const std::string& x) {
  return std::find(ann.begin(), ann.end(), x) != ann.end();
}

static const StorageDecision* lookup_decision(const RepOwnerResult& rep_owner, const std::string& fn,
                                              VarId var) {
  auto itf = rep_owner.by_function.find(fn);
  if (itf == rep_owner.by_function.end()) return nullptr;
  auto itv = itf->second.vars.find(var);
  if (itv == itf->second.vars.end()) return nullptr;
  return &itv->second;
}

using FunctionSymbolMap = std::unordered_map<std::string, std::string>;

static std::string emitted_cpp_name_for(const Function& fn) {
  if (fn.is_extern) return fn.name;
  const std::string identity = nebula::nir::function_identity(fn);
  return "__nebula_fn_" + stable_symbol_hash(identity) + "_" + sanitize_ident_piece(fn.name);
}

static std::string c_abi_export_name_for(const Function& fn) {
  const auto& q = fn.qualified_name;
  std::string out = "nebula";
  std::string last_piece;
  if (!q.package_name.empty()) {
    last_piece = sanitize_ident_piece(q.package_name);
    out += "_" + last_piece;
  }
  if (!q.module_name.empty()) {
    const std::string module_piece = sanitize_ident_piece(q.module_name);
    if (module_piece != last_piece) {
      out += "_" + module_piece;
      last_piece = module_piece;
    }
  }
  out += "_" + sanitize_ident_piece(fn.name);
  return out;
}

static std::string emitted_cpp_name_for_identity(const FunctionSymbolMap& symbols,
                                                 std::string_view identity,
                                                 std::string_view fallback_name) {
  auto it = symbols.find(std::string(identity));
  if (it != symbols.end()) return it->second;
  return std::string(fallback_name);
}

static std::string emitted_cpp_type_name_for(const StructDef& def) {
  return emitted_cpp_type_name_for(def.qualified_name, def.name);
}

static bool is_c_abi_annotation_set(const std::vector<std::string>& ann) {
  return has_annotation(ann, "export") && has_annotation(ann, "abi_c");
}

static std::optional<std::string> c_abi_cpp_type(const Ty& ty) {
  switch (ty.kind) {
  case Ty::Kind::Int: return std::string("std::int64_t");
  case Ty::Kind::Float: return std::string("double");
  case Ty::Kind::Bool: return std::string("bool");
  case Ty::Kind::Void: return std::string("void");
  default: return std::nullopt;
  }
}

static std::optional<std::string> c_abi_header_type(const Ty& ty) {
  switch (ty.kind) {
  case Ty::Kind::Int: return std::string("int64_t");
  case Ty::Kind::Float: return std::string("double");
  case Ty::Kind::Bool: return std::string("bool");
  case Ty::Kind::Void: return std::string("void");
  default: return std::nullopt;
  }
}

static std::string emitted_cpp_type_name_for(const nebula::nir::EnumDef& def) {
  return emitted_cpp_type_name_for(def.qualified_name, def.name);
}

static std::string op_to_cpp(nebula::nir::BinOp op) {
  switch (op) {
  case nebula::nir::BinOp::Add: return "+";
  case nebula::nir::BinOp::Sub: return "-";
  case nebula::nir::BinOp::Mul: return "*";
  case nebula::nir::BinOp::Div: return "/";
  case nebula::nir::BinOp::Mod: return "%";
  case nebula::nir::BinOp::Eq: return "==";
  case nebula::nir::BinOp::Ne: return "!=";
  case nebula::nir::BinOp::Lt: return "<";
  case nebula::nir::BinOp::Lte: return "<=";
  case nebula::nir::BinOp::Gt: return ">";
  case nebula::nir::BinOp::Gte: return ">=";
  case nebula::nir::BinOp::And: return "&&";
  case nebula::nir::BinOp::Or: return "||";
  }
  return "+";
}

struct EmitCtx {
  const RepOwnerResult* rep_owner = nullptr;
  const EmitOptions* opt = nullptr;
  const FunctionSymbolMap* function_symbols = nullptr;
  const Function* current_function = nullptr;
  std::string fn_name;
  std::vector<std::string> region_stack;
  std::unordered_map<VarId, std::string> stringify_source_vars;
  std::vector<std::vector<VarId>> stringify_scope_stack;
  std::unordered_set<VarId> local_value_vars;
  std::unordered_map<VarId, const Block*> local_value_decl_block;
  const Block* current_block = nullptr;
  std::size_t current_stmt_index = 0;
  mutable std::uint64_t temp_counter = 0;
};

static std::string emit_expr(const EmitCtx& ctx, const Expr& e);
static std::string emit_construct_arg_expr(const EmitCtx& ctx, const Expr& e,
                                           const std::vector<nebula::nir::ExprPtr>& sibling_args,
                                           std::size_t sibling_index);

static bool is_synthetic_try_name(std::string_view name) {
  return name.starts_with("__nebula_try_");
}

static bool member_access_uses_arrow(const EmitCtx& ctx, VarId base_var) {
  const StorageDecision* dec = lookup_decision(*ctx.rep_owner, ctx.fn_name, base_var);
  if (!dec) return false;
  return dec->rep != RepKind::Stack;
}

static std::vector<std::string> split_field_path(std::string_view path) {
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

static std::string emit_member_access(const EmitCtx& ctx, VarId base_var,
                                      const std::string& base_name,
                                      const std::string& field) {
  const auto path = split_field_path(field);
  if (path.empty()) return base_name;
  std::string out = base_name + (member_access_uses_arrow(ctx, base_var) ? "->" : ".") + path.front();
  for (std::size_t i = 1; i < path.size(); ++i) out += "." + path[i];
  return out;
}

static std::string emit_temp_member_access(const EmitCtx& ctx,
                                           const Expr& base,
                                           const std::string& field) {
  std::ostringstream os;
  os << "nebula::rt::project_value(" << emit_expr(ctx, base)
     << ", [&](auto&& __nebula_base) { return (__nebula_base";
  for (const auto& segment : split_field_path(field)) {
    os << "." << segment;
  }
  os << "); })";
  return os.str();
}

static std::string emit_value_field_chain(std::string base, std::string_view field) {
  for (const auto& segment : split_field_path(field)) {
    base += "." + segment;
  }
  return base;
}

static bool is_direct_std_call(const Expr::Call& call, std::string_view module_name,
                               std::string_view local_name) {
  return call.kind == nebula::nir::CallKind::Direct && call.resolved_callee.has_value() &&
         call.resolved_callee->package_name == "std" &&
         call.resolved_callee->module_name == module_name &&
         call.resolved_callee->local_name == local_name;
}

static bool is_std_json_call(const Expr::Call& call, std::string_view local_name) {
  const std::string target = nebula::nir::call_target_identity(call);
  return is_direct_std_call(call, "json", local_name) ||
         target == ("std::json::" + std::string(local_name)) || target == local_name;
}

static bool is_resolved_std_json_call(const Expr::Call& call, std::string_view local_name) {
  const std::string target = nebula::nir::call_target_identity(call);
  return is_direct_std_call(call, "json", local_name) ||
         target == ("std::json::" + std::string(local_name));
}

static bool is_std_http_json_call(const Expr::Call& call, std::string_view local_name) {
  const std::string target = nebula::nir::call_target_identity(call);
  return is_direct_std_call(call, "http_json", local_name) ||
         target == ("std::http_json::" + std::string(local_name));
}

static bool is_std_http_call(const Expr::Call& call, std::string_view local_name) {
  const std::string target = nebula::nir::call_target_identity(call);
  return is_direct_std_call(call, "http", local_name) ||
         target == ("std::http::" + std::string(local_name)) || target == local_name;
}

static bool is_std_bytes_call(const Expr::Call& call, std::string_view local_name) {
  const std::string target = nebula::nir::call_target_identity(call);
  return is_direct_std_call(call, "bytes", local_name) ||
         target == ("std::bytes::" + std::string(local_name));
}

static const std::string* string_lit_value(const Expr& expr) {
  if (!std::holds_alternative<Expr::StringLit>(expr.node)) return nullptr;
  return &std::get<Expr::StringLit>(expr.node).value;
}

struct HttpSingleParamLiteralRoute {
  std::string prefix;
  std::string suffix;
};

static std::optional<HttpSingleParamLiteralRoute> single_param_route_literal(const Expr& expr) {
  const std::string* pattern = string_lit_value(expr);
  if (pattern == nullptr || pattern->empty() || pattern->front() != '/') return std::nullopt;
  const std::size_t colon = pattern->find("/:");
  if (colon == std::string::npos) return std::nullopt;
  const std::size_t param_start = colon + 2;
  const std::size_t param_end = pattern->find('/', param_start);
  const std::size_t suffix_start = (param_end == std::string::npos) ? pattern->size() : param_end;
  if (pattern->find(':', suffix_start) != std::string::npos) return std::nullopt;
  if (param_start >= pattern->size() || suffix_start == param_start) return std::nullopt;
  return HttpSingleParamLiteralRoute{pattern->substr(0, colon + 1), pattern->substr(suffix_start)};
}

static std::optional<std::string> exact_route_literal(const Expr& expr) {
  const std::string* pattern = string_lit_value(expr);
  if (pattern == nullptr || pattern->empty() || pattern->front() != '/') return std::nullopt;
  if (pattern->find(':') != std::string::npos) return std::nullopt;
  return *pattern;
}

static const Expr* bytes_to_string_inner_expr(const Expr& expr) {
  if (!std::holds_alternative<Expr::Call>(expr.node)) return nullptr;
  const auto& call = std::get<Expr::Call>(expr.node);
  if (!is_std_bytes_call(call, "to_string") || call.args.size() != 1) return nullptr;
  return call.args[0].get();
}

static std::string emit_http_route_param1_fast(const EmitCtx& ctx,
                                               const HttpSingleParamLiteralRoute& route,
                                               const Expr& request_expr) {
  return "nebula::rt::http_route_param1_single_param(\"" + escape_string(route.prefix) + "\", \"" +
         escape_string(route.suffix) + "\", (" + emit_expr(ctx, request_expr) + ").path)";
}

static std::string emit_http_route_param1_fast_path(const EmitCtx& ctx,
                                                    const HttpSingleParamLiteralRoute& route,
                                                    const Expr& path_expr) {
  return "nebula::rt::http_route_param1_single_param(\"" + escape_string(route.prefix) + "\", \"" +
         escape_string(route.suffix) + "\", " + emit_expr(ctx, path_expr) + ")";
}

static std::string emit_http_path_matches_fast(const EmitCtx& ctx,
                                               const HttpSingleParamLiteralRoute& route,
                                               const Expr& request_expr) {
  return "nebula::rt::http_path_matches_single_param(\"" + escape_string(route.prefix) + "\", \"" +
         escape_string(route.suffix) + "\", (" + emit_expr(ctx, request_expr) + ").path)";
}

static std::string emit_http_path_matches_fast_path(const EmitCtx& ctx,
                                                    const HttpSingleParamLiteralRoute& route,
                                                    const Expr& path_expr) {
  return "nebula::rt::http_path_matches_single_param(\"" + escape_string(route.prefix) + "\", \"" +
         escape_string(route.suffix) + "\", " + emit_expr(ctx, path_expr) + ")";
}

static std::string emit_http_path_matches_exact(const EmitCtx& ctx,
                                                const std::string& path,
                                                const Expr& request_expr) {
  return "((" + emit_expr(ctx, request_expr) + ").path == \"" + escape_string(path) + "\")";
}

static std::string emit_http_path_matches_exact_path(const EmitCtx& ctx,
                                                     const std::string& path,
                                                     const Expr& path_expr) {
  return "(" + emit_expr(ctx, path_expr) + " == \"" + escape_string(path) + "\")";
}

static std::string match_variant_cpp_type(const Ty& subject_ty, const std::string& variant_name) {
  return cpp_type(subject_ty) + "::" + variant_name;
}

static bool is_std_result_enum(const Ty& subject_ty) {
  return matches_std_type(subject_ty, "result", "Result") && subject_ty.type_args.size() == 2;
}

static std::string emit_match_enum_variant_ptr(const std::string& subject_name,
                                               const Ty& subject_ty,
                                               const std::string& variant_name) {
  return "std::get_if<" + match_variant_cpp_type(subject_ty, variant_name) + ">(&" + subject_name +
         ".data)";
}

static std::string emit_match_enum_variant_ptr_projected(const std::string& subject_name,
                                                         const Ty& subject_ty,
                                                         const std::string& variant_name) {
  return "nebula::rt::project_value_ref(" + subject_name +
         ", [&](auto&& __nebula_enum) { return " +
         emit_match_enum_variant_ptr("__nebula_enum", subject_ty, variant_name) + "; })";
}

static std::string emit_match_enum_is_variant(const std::string& subject_name,
                                              const Ty& subject_ty,
                                              const std::string& variant_name) {
  if (is_std_result_enum(subject_ty)) {
    if (variant_name == "Ok") return "nebula::rt::result_is_ok(" + subject_name + ")";
    if (variant_name == "Err") return "nebula::rt::result_is_err(" + subject_name + ")";
  }
  return "(" + emit_match_enum_variant_ptr_projected(subject_name, subject_ty, variant_name) +
         " != nullptr)";
}

static std::string emit_match_enum_payload(const std::string& subject_name,
                                           const Ty& subject_ty,
                                           const std::string& variant_name) {
  if (is_std_result_enum(subject_ty)) {
    const bool move_synthetic_try = is_synthetic_try_name(subject_name);
    if (variant_name == "Ok") {
      return std::string("nebula::rt::") +
             (move_synthetic_try ? "result_ok_move(" : "result_ok_ref(") + subject_name + ")";
    }
    if (variant_name == "Err") {
      return std::string("nebula::rt::") +
             (move_synthetic_try ? "result_err_move(" : "result_err_ref(") + subject_name + ")";
    }
  }
  return "nebula::rt::project_value_ref(" + subject_name +
         ", [&](auto&& __nebula_enum) -> decltype(auto) { return (" +
         emit_match_enum_variant_ptr("__nebula_enum", subject_ty, variant_name) + "->value); })";
}

static void collect_bytes_concat_terms(const Expr& expr, std::vector<const Expr*>& out) {
  if (std::holds_alternative<Expr::Call>(expr.node)) {
    const auto& call = std::get<Expr::Call>(expr.node);
    if (is_direct_std_call(call, "bytes", "concat") && call.args.size() == 2) {
      collect_bytes_concat_terms(*call.args[0], out);
      collect_bytes_concat_terms(*call.args[1], out);
      return;
    }
  }
  out.push_back(&expr);
}

static std::string emit_bytes_concat_chain(const EmitCtx& ctx, const Expr::Call& call) {
  std::vector<const Expr*> parts;
  parts.reserve(call.args.size());
  for (const auto& arg : call.args) collect_bytes_concat_terms(*arg, parts);

  std::ostringstream os;
  os << "([&]() { ";
  std::vector<std::string> names;
  names.reserve(parts.size());
  for (const Expr* part : parts) {
    std::string name = "__nebula_bytes_part_" + std::to_string(ctx.temp_counter++);
    os << "auto " << name << " = " << emit_expr(ctx, *part) << "; ";
    names.push_back(std::move(name));
  }
  os << "return nebula::rt::bytes_concat_all(";
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i) os << ", ";
    os << names[i];
  }
  os << "); })()";
  return os.str();
}

static std::string cpp_storage_type(const Ty& base, RepKind rep, OwnerKind owner,
                                    const std::string& region) {
  (void)region;
  if (base.kind == Ty::Kind::Void) return "void";
  const std::string t = cpp_type(base);
  if (rep == RepKind::Stack) return t;
  if (rep == RepKind::Region) return t + "*";
  // Heap
  if (owner == OwnerKind::Shared) return "std::shared_ptr<" + t + ">";
  if (owner == OwnerKind::Unique) return "std::unique_ptr<" + t + ">";
  // Heap without owner should not happen; be conservative.
  return "std::shared_ptr<" + t + ">";
}

static StorageDecision join_decision(StorageDecision a, const StorageDecision& b) {
  // Conservative join:
  // - Heap×Shared dominates
  // - Heap×Unique dominates Stack/Region
  // - Stack dominates Region (Region should not escape anyway)
  if (b.rep == RepKind::Heap && b.owner == OwnerKind::Shared) return b;
  if (a.rep == RepKind::Heap && a.owner == OwnerKind::Shared) return a;
  if (b.rep == RepKind::Heap) return b;
  if (a.rep == RepKind::Heap) return a;
  if (b.rep == RepKind::Stack) return b;
  if (a.rep == RepKind::Stack) return a;
  return a;
}

static StorageDecision decision_for_return_expr(const EmitCtx& ctx, const Expr& e) {
  // Default: Stack
  StorageDecision d{RepKind::Stack, OwnerKind::None, ""};

  // Strip a single prefix if present (v0.1 forbids chains).
  const Expr* cur = &e;
  if (std::holds_alternative<Expr::Prefix>(cur->node)) {
    const auto& p = std::get<Expr::Prefix>(cur->node);
    if (std::holds_alternative<Expr::Construct>(p.inner->node)) {
      switch (p.kind) {
      case PrefixKind::Shared: return StorageDecision{RepKind::Heap, OwnerKind::Shared, ""};
      case PrefixKind::Unique: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
      case PrefixKind::Heap: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
      case PrefixKind::Promote: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
      }
    }
    cur = p.inner.get();
  }

  if (cur && std::holds_alternative<Expr::VarRef>(cur->node)) {
    const auto& v = std::get<Expr::VarRef>(cur->node);
    if (const StorageDecision* vd = lookup_decision(*ctx.rep_owner, ctx.fn_name, v.var)) {
      return *vd;
    }
    return d;
  }

  if (cur && std::holds_alternative<Expr::Construct>(cur->node)) {
    // Direct return of constructor in region would escape; default policy promotes to heap unique.
    if (!ctx.region_stack.empty()) {
      return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
    }
    return d;
  }

  return d;
}

static void scan_return_decisions(const Stmt& s, EmitCtx& ctx, StorageDecision& out_dec) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Return>) {
          out_dec = join_decision(out_dec, decision_for_return_expr(ctx, *st.value));
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          ctx.region_stack.push_back(st.name);
          for (const auto& ss : st.body.stmts) scan_return_decisions(ss, ctx, out_dec);
          ctx.region_stack.pop_back();
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          for (const auto& ss : st.body.stmts) scan_return_decisions(ss, ctx, out_dec);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          for (const auto& ss : st.then_body.stmts) scan_return_decisions(ss, ctx, out_dec);
          if (st.else_body.has_value()) {
            for (const auto& ss : st.else_body->stmts) scan_return_decisions(ss, ctx, out_dec);
          }
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          for (const auto& ss : st.body.stmts) scan_return_decisions(ss, ctx, out_dec);
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          for (const auto& ss : st.body.stmts) scan_return_decisions(ss, ctx, out_dec);
        } else {
          // other statements: nothing
        }
      },
      s.node);
}

static std::string function_return_cpp_type(const Function& fn, const RepOwnerResult& rep_owner,
                                            const EmitOptions& opt) {
  if (fn.is_async) return future_cpp_type(fn.ret);
  if (fn.ret.kind == Ty::Kind::Void) return "void";
  EmitCtx ctx;
  ctx.rep_owner = &rep_owner;
  ctx.opt = &opt;
  ctx.fn_name = nebula::nir::function_identity(fn);
  StorageDecision dec{RepKind::Stack, OwnerKind::None, ""};
  if (!fn.body.has_value()) return cpp_type(fn.ret);
  for (const auto& s : fn.body->stmts) scan_return_decisions(s, ctx, dec);
  return cpp_storage_type(fn.ret, dec.rep, dec.owner, dec.region);
}

static bool expr_is_const_json_expr(const Expr& e) {
  return std::visit(
      [&](auto&& n) -> bool {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          if (n.kind != nebula::nir::CallKind::Direct) return false;
          const std::string target = nebula::nir::call_target_identity(n);
          if (target == "std::json::string_value" || target == "string_value") {
            return n.args.size() == 1 && std::holds_alternative<Expr::StringLit>(n.args[0]->node);
          }
          if (target == "std::json::int_value" || target == "int_value") {
            return n.args.size() == 1 && std::holds_alternative<Expr::IntLit>(n.args[0]->node);
          }
          if (target == "std::json::bool_value" || target == "bool_value") {
            return n.args.size() == 1 && std::holds_alternative<Expr::BoolLit>(n.args[0]->node);
          }
          if (target == "std::json::null_value" || target == "null_value") {
            return n.args.empty();
          }
          if ((target.rfind("std::json::object", 0) == 0 || target.rfind("object", 0) == 0) &&
              !n.args.empty() && n.args.size() % 2 == 0) {
            for (std::size_t i = 0; i < n.args.size(); i += 2) {
              if (!std::holds_alternative<Expr::StringLit>(n.args[i]->node)) return false;
              if (!expr_is_const_json_expr(*n.args[i + 1])) return false;
            }
            return true;
          }
          return false;
        } else {
          return false;
        }
      },
      e.node);
}

static const Stmt::Return* const_json_return_stmt(const Function& fn) {
  if (fn.is_async || fn.is_extern) return nullptr;
  if (!matches_std_type(fn.ret, "json", "Json") && cpp_type(fn.ret) != "nebula::rt::JsonValue") {
    return nullptr;
  }
  if (!fn.params.empty() || !fn.body.has_value() || fn.body->stmts.size() != 1) return nullptr;
  if (!std::holds_alternative<Stmt::Return>(fn.body->stmts[0].node)) return nullptr;
  const auto& ret = std::get<Stmt::Return>(fn.body->stmts[0].node);
  if (expr_is_const_json_expr(*ret.value)) return &ret;
  return nullptr;
}

static std::string emit_construct_call(const EmitCtx& ctx, const std::string& type_name,
                                       const std::vector<nebula::nir::ExprPtr>& args) {
  std::ostringstream os;
  os << type_name << "(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) os << ", ";
    os << emit_construct_arg_expr(ctx, *args[i], args, i);
  }
  os << ")";
  return os.str();
}

static std::string emit_heap_make(PrefixKind k, const EmitCtx& ctx, const std::string& type_name,
                                  const std::vector<nebula::nir::ExprPtr>& args) {
  const char* maker = nullptr;
  switch (k) {
  case PrefixKind::Shared: maker = "std::make_shared"; break;
  case PrefixKind::Unique: maker = "std::make_unique"; break;
  case PrefixKind::Heap: maker = "std::make_unique"; break;
  case PrefixKind::Promote: maker = "std::make_unique"; break;
  }
  std::ostringstream os;
  os << maker << "<" << type_name << ">(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) os << ", ";
    os << emit_construct_arg_expr(ctx, *args[i], args, i);
  }
  os << ")";
  return os.str();
}

static std::string emit_construct_call(const EmitCtx& ctx,
                                       const Ty& constructed_ty,
                                       const Expr::Construct& construct) {
  const std::string type_name = cpp_type(constructed_ty);
  if (!construct.variant_name.has_value()) {
    return emit_construct_call(ctx, type_name, construct.args);
  }
  std::ostringstream os;
  os << type_name << "(" << type_name << "::" << *construct.variant_name;
  if (construct.args.empty()) {
    os << "{}";
  } else {
    os << "{";
    for (std::size_t i = 0; i < construct.args.size(); ++i) {
      if (i) os << ", ";
      os << emit_construct_arg_expr(ctx, *construct.args[i], construct.args, i);
    }
    os << "}";
  }
  os << ")";
  return os.str();
}

static std::string emit_heap_make(PrefixKind k,
                                  const EmitCtx& ctx,
                                  const Ty& constructed_ty,
                                  const Expr::Construct& construct) {
  const std::string type_name = cpp_type(constructed_ty);
  if (!construct.variant_name.has_value()) {
    return emit_heap_make(k, ctx, type_name, construct.args);
  }
  const char* maker = nullptr;
  switch (k) {
  case PrefixKind::Shared: maker = "std::make_shared"; break;
  case PrefixKind::Unique: maker = "std::make_unique"; break;
  case PrefixKind::Heap: maker = "std::make_unique"; break;
  case PrefixKind::Promote: maker = "std::make_unique"; break;
  }
  std::ostringstream os;
  os << maker << "<" << type_name << ">(" << type_name << "::" << *construct.variant_name;
  if (construct.args.empty()) {
    os << "{}";
  } else {
    os << "{";
    for (std::size_t i = 0; i < construct.args.size(); ++i) {
      if (i) os << ", ";
      os << emit_construct_arg_expr(ctx, *construct.args[i], construct.args, i);
    }
    os << "}";
  }
  os << ")";
  return os.str();
}

static std::string emit_expr(const EmitCtx& ctx, const Expr& e) {
  return std::visit(
      [&](auto&& n) -> std::string {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::IntLit>) {
          return std::to_string(n.value);
        } else if constexpr (std::is_same_v<N, Expr::BoolLit>) {
          return n.value ? "true" : "false";
        } else if constexpr (std::is_same_v<N, Expr::FloatLit>) {
          std::ostringstream os;
          os << n.value;
          return os.str();
        } else if constexpr (std::is_same_v<N, Expr::StringLit>) {
          return std::string("\"") + escape_string(n.value) + "\"";
        } else if constexpr (std::is_same_v<N, Expr::VarRef>) {
          if (n.var == 0) {
            const std::string target = nebula::nir::varref_target_identity(n);
            if (ctx.function_symbols != nullptr) {
              return emitted_cpp_name_for_identity(*ctx.function_symbols, target, n.name);
            }
            return n.name;
          }
          return n.name;
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          return emit_member_access(ctx, n.base_var, n.base_name, n.field);
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          return emit_temp_member_access(ctx, *n.base, n.field);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
          if (std::holds_alternative<Expr::VarRef>(n.subject->node)) {
            return emit_match_enum_is_variant(emit_expr(ctx, *n.subject), n.subject->ty, n.variant_name);
          }
          return "([&]() { const auto& __nebula_subject = " + emit_expr(ctx, *n.subject) + "; return " +
                 emit_match_enum_is_variant("__nebula_subject", n.subject->ty, n.variant_name) +
                 "; })()";
        } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
          if (std::holds_alternative<Expr::VarRef>(n.subject->node)) {
            return emit_match_enum_payload(emit_expr(ctx, *n.subject), n.subject->ty, n.variant_name);
          }
          return "([&]() -> decltype(auto) { const auto& __nebula_subject = " + emit_expr(ctx, *n.subject) +
                 "; return (" + emit_match_enum_payload("__nebula_subject", n.subject->ty, n.variant_name) +
                 "); })()";
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          if ((n.op == nebula::nir::BinOp::Eq || n.op == nebula::nir::BinOp::Ne)) {
            if (const Expr* bytes_expr = bytes_to_string_inner_expr(*n.lhs)) {
              return std::string(n.op == nebula::nir::BinOp::Ne ? "(!" : "") +
                     "nebula::rt::bytes_equal_string(" + emit_expr(ctx, *bytes_expr) + ", " +
                     emit_expr(ctx, *n.rhs) + ")" + (n.op == nebula::nir::BinOp::Ne ? ")" : "");
            }
            if (const Expr* bytes_expr = bytes_to_string_inner_expr(*n.rhs)) {
              return std::string(n.op == nebula::nir::BinOp::Ne ? "(!" : "") +
                     "nebula::rt::bytes_equal_string(" + emit_expr(ctx, *bytes_expr) + ", " +
                     emit_expr(ctx, *n.lhs) + ")" + (n.op == nebula::nir::BinOp::Ne ? ")" : "");
            }
          }
          return "(" + emit_expr(ctx, *n.lhs) + " " + op_to_cpp(n.op) + " " + emit_expr(ctx, *n.rhs) +
                 ")";
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          return "(!" + emit_expr(ctx, *n.inner) + ")";
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          const std::string target = nebula::nir::call_target_identity(n);
          if ((target == "std::json::parse" || target == "parse") && n.args.size() == 1 &&
              std::holds_alternative<Expr::VarRef>(n.args[0]->node)) {
            const auto& var = std::get<Expr::VarRef>(n.args[0]->node);
            auto it = ctx.stringify_source_vars.find(var.var);
            if (it != ctx.stringify_source_vars.end()) {
              return "nebula::rt::ok_result(" + it->second + ")";
            }
          }
          if ((target == "std::json::parse" || target == "parse") && n.args.size() == 1 &&
              std::holds_alternative<Expr::Call>(n.args[0]->node)) {
            const auto& inner = std::get<Expr::Call>(n.args[0]->node);
            if (is_std_json_call(inner, "stringify") && inner.args.size() == 1) {
              return "nebula::rt::ok_result(" + emit_expr(ctx, *inner.args[0]) + ")";
            }
          }
          if (is_std_http_json_call(n, "parse_json_body") && n.args.size() == 1) {
            return "nebula::rt::json_parse_bytes(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          if (is_std_bytes_call(n, "from_string") && n.args.size() == 1) {
            if (const std::string* literal = string_lit_value(*n.args[0]); literal != nullptr &&
                literal->empty()) {
              return "nebula::rt::Bytes{}";
            }
            return "nebula::rt::bytes_from_string(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          if (is_std_bytes_call(n, "to_string") && n.args.size() == 1) {
            return "nebula::rt::bytes_to_string(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          if (is_std_bytes_call(n, "concat") && n.args.size() == 2) {
            return emit_bytes_concat_chain(ctx, n);
          }
          if (is_std_http_call(n, "response") && n.args.size() == 3) {
            return "nebula::rt::HttpResponse(" + emit_expr(ctx, *n.args[0]) + ", " +
                   emit_expr(ctx, *n.args[1]) + ", " + emit_expr(ctx, *n.args[2]) + ")";
          }
          if (is_std_http_call(n, "text_response") && n.args.size() == 2) {
            return "nebula::rt::HttpResponse(" + emit_expr(ctx, *n.args[0]) +
                   ", \"text/plain; charset=utf-8\", nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[1]) + "))";
          }
          if (is_std_http_call(n, "ok_text") && n.args.size() == 1) {
            return "nebula::rt::HttpResponse(200, \"text/plain; charset=utf-8\", "
                   "nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[0]) + "))";
          }
          if (is_std_http_call(n, "bad_request_text") && n.args.size() == 1) {
            return "nebula::rt::HttpResponse(400, \"text/plain; charset=utf-8\", "
                   "nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[0]) + "))";
          }
          if (is_std_http_call(n, "method_not_allowed_text") && n.args.size() == 1) {
            return "nebula::rt::HttpResponse(405, \"text/plain; charset=utf-8\", "
                   "nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[0]) + "))";
          }
          if (is_std_http_call(n, "not_found_text") && n.args.size() == 1) {
            return "nebula::rt::HttpResponse(404, \"text/plain; charset=utf-8\", "
                   "nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[0]) + "))";
          }
          if (is_std_http_call(n, "internal_error_text") && n.args.size() == 1) {
            return "nebula::rt::HttpResponse(500, \"text/plain; charset=utf-8\", "
                   "nebula::rt::bytes_from_string(" +
                   emit_expr(ctx, *n.args[0]) + "))";
          }
          if (is_std_http_call(n, "header_value") && n.args.size() == 2) {
            return "nebula::rt::http_header(" + emit_expr(ctx, *n.args[0]) + ", " +
                   emit_expr(ctx, *n.args[1]) + ")";
          }
          if (is_std_http_call(n, "header_value_unique") && n.args.size() == 2) {
            return "nebula::rt::http_unique_header(" + emit_expr(ctx, *n.args[0]) + ", " +
                   emit_expr(ctx, *n.args[1]) + ")";
          }
          if (is_std_http_call(n, "content_type") && n.args.size() == 1) {
            return "nebula::rt::http_content_type(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          if (is_std_http_call(n, "response_header") && n.args.size() == 2) {
            return "nebula::rt::http_response_header(" + emit_expr(ctx, *n.args[0]) + ", " +
                   emit_expr(ctx, *n.args[1]) + ")";
          }
          if ((target == "Request_close_connection" || target == "std::http::Request_close_connection") &&
              n.args.size() == 1) {
            return "nebula::rt::http_request_close_connection(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          if ((target == "__nebula_rt_http_route_param1" || n.callee == "__nebula_rt_http_route_param1") &&
              n.args.size() == 2) {
            if (auto route = single_param_route_literal(*n.args[0]); route.has_value()) {
              return emit_http_route_param1_fast_path(ctx, *route, *n.args[1]);
            }
          }
          if ((target == "__nebula_rt_http_path_matches" || n.callee == "__nebula_rt_http_path_matches") &&
              n.args.size() == 2) {
            if (auto path = exact_route_literal(*n.args[0]); path.has_value()) {
              return emit_http_path_matches_exact_path(ctx, *path, *n.args[1]);
            }
            if (auto route = single_param_route_literal(*n.args[0]); route.has_value()) {
              return emit_http_path_matches_fast_path(ctx, *route, *n.args[1]);
            }
          }
          if (is_std_http_call(n, "route_param1") && n.args.size() == 2) {
            if (auto route = single_param_route_literal(*n.args[0]); route.has_value()) {
              return emit_http_route_param1_fast(ctx, *route, *n.args[1]);
            }
            return "nebula::rt::http_route_param1(" + emit_expr(ctx, *n.args[0]) + ", (" +
                   emit_expr(ctx, *n.args[1]) + ").path)";
          }
          if (is_std_http_call(n, "path_matches") && n.args.size() == 2) {
            if (auto path = exact_route_literal(*n.args[0]); path.has_value()) {
              return emit_http_path_matches_exact(ctx, *path, *n.args[1]);
            }
            if (auto route = single_param_route_literal(*n.args[0]); route.has_value()) {
              return emit_http_path_matches_fast(ctx, *route, *n.args[1]);
            }
            return "nebula::rt::http_path_matches(" + emit_expr(ctx, *n.args[0]) + ", (" +
                   emit_expr(ctx, *n.args[1]) + ").path)";
          }
          if (ctx.opt != nullptr && ctx.opt->runtime_profile == RuntimeProfile::System &&
              n.kind == nebula::nir::CallKind::Direct && target == "panic" && n.args.size() == 1) {
            return "__nebula_panic_policy(" + emit_expr(ctx, *n.args[0]) + ")";
          }
          std::ostringstream os;
          if (n.kind == nebula::nir::CallKind::Indirect) {
            os << n.callee << "(";
          } else {
            if (auto runtime_name = nebula::nir::runtime_std_call_name(n.resolved_callee, n.callee);
                runtime_name.has_value()) {
              os << *runtime_name << "(";
            } else {
              if (ctx.function_symbols != nullptr) {
                os << emitted_cpp_name_for_identity(*ctx.function_symbols, target, n.callee) << "(";
              } else {
                os << n.callee << "(";
              }
            }
          }
          for (std::size_t i = 0; i < n.args.size(); ++i) {
            if (i) os << ", ";
            os << emit_expr(ctx, *n.args[i]);
          }
          os << ")";
          return os.str();
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          // Bare construct used as expression (stack default outside region).
          if (!ctx.region_stack.empty()) {
            // In region, expression-level construct defaults to region allocation.
            const std::string& R = ctx.region_stack.back();
            std::ostringstream os;
            os << "NEBULA_ALLOC(" << R << ", " << cpp_type(e.ty);
            if (n.variant_name.has_value()) {
              os << ", " << cpp_type(e.ty) << "::" << *n.variant_name;
              if (n.args.empty()) {
                os << "{}";
              } else {
                os << "{";
                for (std::size_t i = 0; i < n.args.size(); ++i) {
                  if (i) os << ", ";
                  os << emit_expr(ctx, *n.args[i]);
                }
                os << "}";
              }
            } else {
              for (const auto& a : n.args) os << ", " << emit_expr(ctx, *a);
            }
            os << ")";
            return os.str();
          }
          return emit_construct_call(ctx, e.ty, n);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          // Prefix is meaningful mainly over constructors.
          if (std::holds_alternative<Expr::Construct>(n.inner->node)) {
            const auto& c = std::get<Expr::Construct>(n.inner->node);
            return emit_heap_make(n.kind, ctx, e.ty, c);
          }
          return emit_expr(ctx, *n.inner);
        } else if constexpr (std::is_same_v<N, Expr::Await>) {
          return "(co_await " + emit_expr(ctx, *n.inner) + ")";
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          const std::string subject_name =
              "__nebula_match_" + std::to_string(ctx.temp_counter++);
          std::ostringstream os;
          os << "([&]() -> " << cpp_type(e.ty) << " { ";
          os << "auto " << subject_name << " = " << emit_expr(ctx, *n.subject) << "; ";
          for (std::size_t i = 0; i < n.arms.size(); ++i) {
            const auto& arm = *n.arms[i];
            const bool unconditional =
                arm.kind == Expr::Match::Arm::Kind::Wildcard ||
                (n.exhaustive && i + 1 == n.arms.size());
            auto emit_arm_body = [&]() {
              if (arm.kind == Expr::Match::Arm::Kind::EnumVariant) {
                if (arm.payload_binding.has_value()) {
                  os << "const auto& " << arm.payload_binding->name << " = "
                     << emit_match_enum_payload(subject_name, n.subject->ty, arm.variant_name) << "; ";
                } else if (!arm.payload_struct_bindings.empty()) {
                  const std::string payload_name =
                      "__nebula_payload_" + std::to_string(ctx.temp_counter++);
                  os << "const auto& " << payload_name << " = "
                     << emit_match_enum_payload(subject_name, n.subject->ty, arm.variant_name) << "; ";
                  for (const auto& field : arm.payload_struct_bindings) {
                    os << "const auto& " << field.binding.name << " = "
                       << emit_value_field_chain(payload_name, field.field_name) << "; ";
                  }
                }
              }
              os << "return " << emit_expr(ctx, *arm.value) << "; ";
            };

            if (unconditional) {
              emit_arm_body();
            } else {
              if (arm.kind == Expr::Match::Arm::Kind::Bool) {
                os << "if (" << (arm.bool_value ? subject_name : "(!" + subject_name + ")") << ") { ";
              } else {
                const std::string variant_ptr =
                    "__nebula_variant_" + std::to_string(ctx.temp_counter++);
                os << "if (auto* " << variant_ptr << " = "
                   << emit_match_enum_variant_ptr(subject_name, n.subject->ty, arm.variant_name)
                   << ") { ";
                if (arm.payload_binding.has_value()) {
                  os << "const auto& " << arm.payload_binding->name << " = " << variant_ptr << "->value; ";
                } else if (!arm.payload_struct_bindings.empty()) {
                  const std::string payload_name =
                      "__nebula_payload_" + std::to_string(ctx.temp_counter++);
                  os << "const auto& " << payload_name << " = " << variant_ptr << "->value; ";
                  for (const auto& field : arm.payload_struct_bindings) {
                    os << "const auto& " << field.binding.name << " = "
                       << emit_value_field_chain(payload_name, field.field_name) << "; ";
                  }
                }
                os << "return " << emit_expr(ctx, *arm.value) << "; ";
                os << "} ";
                continue;
              }
              emit_arm_body();
              os << "} ";
            }
          }
          if (!n.exhaustive) {
            os << "return " << (e.ty.kind == Ty::Kind::Void ? "" : "{}") << "; ";
          }
          os << "})()";
          return os.str();
        } else {
          return "/*expr*/";
        }
      },
      e.node);
}

static bool has_single_wrapping_parens(std::string_view text) {
  if (text.size() < 2 || text.front() != '(' || text.back() != ')') return false;
  int depth = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '(') {
      depth += 1;
    } else if (ch == ')') {
      depth -= 1;
      if (depth == 0 && i + 1 < text.size()) return false;
    }
    if (depth < 0) return false;
  }
  return depth == 0;
}

static std::string emit_condition_expr(const EmitCtx& ctx, const Expr& e) {
  std::string rendered = emit_expr(ctx, e);
  while (has_single_wrapping_parens(rendered)) {
    rendered = rendered.substr(1, rendered.size() - 2);
  }
  return rendered;
}

static void emit_stmt(Cpp& out, EmitCtx& ctx, const Stmt& s);

static void emit_block(Cpp& out, EmitCtx& ctx, const Block& b) {
  ctx.stringify_scope_stack.push_back({});
  const Block* previous_block = ctx.current_block;
  const std::size_t previous_index = ctx.current_stmt_index;
  ctx.current_block = &b;
  for (std::size_t i = 0; i < b.stmts.size(); ++i) {
    ctx.current_stmt_index = i;
    emit_stmt(out, ctx, b.stmts[i]);
  }
  ctx.current_block = previous_block;
  ctx.current_stmt_index = previous_index;
  for (VarId var : ctx.stringify_scope_stack.back()) {
    ctx.stringify_source_vars.erase(var);
  }
  ctx.stringify_scope_stack.pop_back();
}

static bool stmt_reassigns_var(const Stmt& s, VarId target);

static bool block_reassigns_var(const Block& b, VarId target) {
  for (const auto& st : b.stmts) {
    if (stmt_reassigns_var(st, target)) return true;
  }
  return false;
}

static bool stmt_reassigns_var(const Stmt& s, VarId target) {
  return std::visit(
      [&](auto&& st) -> bool {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          return st.var == target;
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          return st.base_var == target;
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          return block_reassigns_var(st.then_body, target) ||
                 (st.else_body.has_value() && block_reassigns_var(*st.else_body, target));
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          return block_reassigns_var(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          return block_reassigns_var(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          return block_reassigns_var(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          return block_reassigns_var(st.body, target);
        } else {
          return false;
        }
      },
      s.node);
}

static bool current_function_reassigns_var(const EmitCtx& ctx, VarId target) {
  return ctx.current_function != nullptr && ctx.current_function->body.has_value() &&
         block_reassigns_var(*ctx.current_function->body, target);
}

static bool expr_uses_var(const Expr& e, VarId target);

static bool call_uses_var(const Expr::Call& call, VarId target) {
  for (const auto& arg : call.args) {
    if (expr_uses_var(*arg, target)) return true;
  }
  return call.kind == nebula::nir::CallKind::Indirect && call.callee_var == target;
}

static bool expr_uses_var(const Expr& e, VarId target) {
  return std::visit(
      [&](auto&& n) -> bool {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::VarRef>) {
          return n.var == target;
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          return n.base_var == target;
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          return expr_uses_var(*n.base, target);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant> ||
                             std::is_same_v<N, Expr::EnumPayload>) {
          return expr_uses_var(*n.subject, target);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix> ||
                             std::is_same_v<N, Expr::Await>) {
          return expr_uses_var(*n.inner, target);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          return expr_uses_var(*n.lhs, target) || expr_uses_var(*n.rhs, target);
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          return call_uses_var(n, target);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& arg : n.args) {
            if (expr_uses_var(*arg, target)) return true;
          }
          return false;
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          if (expr_uses_var(*n.subject, target)) return true;
          for (const auto& arm : n.arms) {
            if (expr_uses_var(*arm->value, target)) return true;
          }
          return false;
        } else {
          return false;
        }
      },
      e.node);
}

static bool expr_uses_var_outside_parse_alias(const Expr& e, VarId target);

static bool call_uses_var_outside_parse_alias(const Expr::Call& call, VarId target) {
  if ((nebula::nir::call_target_identity(call) == "std::json::parse" || call.callee == "parse") &&
      call.args.size() == 1 && std::holds_alternative<Expr::VarRef>(call.args[0]->node)) {
    const auto& ref = std::get<Expr::VarRef>(call.args[0]->node);
    if (ref.var == target) return false;
  }
  for (const auto& arg : call.args) {
    if (expr_uses_var_outside_parse_alias(*arg, target)) return true;
  }
  return call.kind == nebula::nir::CallKind::Indirect && call.callee_var == target;
}

static bool expr_uses_var_outside_parse_alias(const Expr& e, VarId target) {
  return std::visit(
      [&](auto&& n) -> bool {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::VarRef>) {
          return n.var == target;
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          return n.base_var == target;
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          return expr_uses_var_outside_parse_alias(*n.base, target);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant> ||
                             std::is_same_v<N, Expr::EnumPayload>) {
          return expr_uses_var_outside_parse_alias(*n.subject, target);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix> ||
                             std::is_same_v<N, Expr::Await>) {
          return expr_uses_var_outside_parse_alias(*n.inner, target);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          return expr_uses_var_outside_parse_alias(*n.lhs, target) ||
                 expr_uses_var_outside_parse_alias(*n.rhs, target);
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          return call_uses_var_outside_parse_alias(n, target);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& arg : n.args) {
            if (expr_uses_var_outside_parse_alias(*arg, target)) return true;
          }
          return false;
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          if (expr_uses_var_outside_parse_alias(*n.subject, target)) return true;
          for (const auto& arm : n.arms) {
            if (expr_uses_var_outside_parse_alias(*arm->value, target)) return true;
          }
          return false;
        } else {
          return false;
        }
      },
      e.node);
}

static bool stmt_uses_var_outside_parse_alias(const Stmt& s, VarId target);

static bool block_uses_var_outside_parse_alias(const Block& b, VarId target) {
  for (const auto& st : b.stmts) {
    if (stmt_uses_var_outside_parse_alias(st, target)) return true;
  }
  return false;
}

static bool stmt_uses_var_outside_parse_alias(const Stmt& s, VarId target) {
  return std::visit(
      [&](auto&& st) -> bool {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Let>) {
          return expr_uses_var_outside_parse_alias(*st.value, target);
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          return expr_uses_var_outside_parse_alias(*st.value, target);
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          return st.base_var == target || expr_uses_var_outside_parse_alias(*st.value, target);
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          return expr_uses_var_outside_parse_alias(*st.expr, target);
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          return expr_uses_var_outside_parse_alias(*st.value, target);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          return expr_uses_var_outside_parse_alias(*st.cond, target) ||
                 block_uses_var_outside_parse_alias(st.then_body, target) ||
                 (st.else_body.has_value() && block_uses_var_outside_parse_alias(*st.else_body, target));
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          return expr_uses_var_outside_parse_alias(*st.start, target) ||
                 expr_uses_var_outside_parse_alias(*st.end, target) ||
                 block_uses_var_outside_parse_alias(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          return expr_uses_var_outside_parse_alias(*st.cond, target) ||
                 block_uses_var_outside_parse_alias(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          return block_uses_var_outside_parse_alias(st.body, target);
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          return block_uses_var_outside_parse_alias(st.body, target);
        } else {
          return false;
        }
      },
      s.node);
}

static bool current_function_uses_var_outside_parse_alias(const EmitCtx& ctx, VarId target) {
  return ctx.current_function != nullptr && ctx.current_function->body.has_value() &&
         block_uses_var_outside_parse_alias(*ctx.current_function->body, target);
}

static bool ty_is_movable_value(const Ty& ty) {
  switch (ty.kind) {
  case Ty::Kind::String:
  case Ty::Kind::Struct:
  case Ty::Kind::Enum: return true;
  default: return false;
  }
}

static bool current_block_uses_var_after_current_stmt(const EmitCtx& ctx, VarId target) {
  if (ctx.current_block == nullptr) return true;
  for (std::size_t i = ctx.current_stmt_index + 1; i < ctx.current_block->stmts.size(); ++i) {
    if (stmt_uses_var_outside_parse_alias(ctx.current_block->stmts[i], target)) return true;
  }
  return false;
}

static bool expr_is_synthetic_try_payload(const Expr& expr) {
  const auto* payload = std::get_if<Expr::EnumPayload>(&expr.node);
  if (payload == nullptr) return false;
  const auto* subject = std::get_if<Expr::VarRef>(&payload->subject->node);
  return subject != nullptr && is_synthetic_try_name(subject->name);
}

static const Expr::VarRef* expr_last_use_movable_local_ref(const EmitCtx& ctx, const Expr& expr) {
  const auto* var = std::get_if<Expr::VarRef>(&expr.node);
  if (var == nullptr || var->var == 0) return nullptr;
  if (!ty_is_movable_value(expr.ty)) return nullptr;
  if (!ctx.local_value_vars.contains(var->var)) return nullptr;
  const auto declared_in = ctx.local_value_decl_block.find(var->var);
  if (declared_in == ctx.local_value_decl_block.end() || declared_in->second != ctx.current_block) {
    return nullptr;
  }
  if (member_access_uses_arrow(ctx, var->var)) return nullptr;
  if (current_block_uses_var_after_current_stmt(ctx, var->var)) return nullptr;
  return var;
}

static bool sibling_construct_args_use_var(const std::vector<nebula::nir::ExprPtr>& args,
                                           std::size_t current_index,
                                           VarId target) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i == current_index) continue;
    if (expr_uses_var(*args[i], target)) return true;
  }
  return false;
}

static std::string emit_construct_arg_expr(const EmitCtx& ctx, const Expr& e,
                                           const std::vector<nebula::nir::ExprPtr>& sibling_args,
                                           std::size_t sibling_index) {
  const Expr::VarRef* movable = expr_last_use_movable_local_ref(ctx, e);
  if (movable != nullptr && !sibling_construct_args_use_var(sibling_args, sibling_index, movable->var)) {
    return "std::move(" + emit_expr(ctx, e) + ")";
  }
  return emit_expr(ctx, e);
}

static bool should_bind_let_by_const_ref(const EmitCtx& ctx, const Stmt::Let& st) {
  if (is_synthetic_try_name(st.name) || expr_is_synthetic_try_payload(*st.value)) return false;
  if (current_function_reassigns_var(ctx, st.var)) return false;
  if (std::holds_alternative<Expr::VarRef>(st.value->node) ||
      std::holds_alternative<Expr::FieldRef>(st.value->node)) {
    return false;
  }
  switch (st.ty.kind) {
  case Ty::Kind::String:
  case Ty::Kind::Struct:
  case Ty::Kind::Enum: return true;
  default: return false;
  }
}

static std::optional<std::string> emit_json_array_builder_self_push(const EmitCtx& ctx,
                                                                    const Stmt::AssignVar& st) {
  if (!matches_std_type(st.ty, "json", "JsonArrayBuilder")) return std::nullopt;
  if (!std::holds_alternative<Expr::Call>(st.value->node)) return std::nullopt;
  const auto& call = std::get<Expr::Call>(st.value->node);
  if (!is_resolved_std_json_call(call, "array_push") || call.args.size() != 2) return std::nullopt;
  if (!std::holds_alternative<Expr::VarRef>(call.args[0]->node)) return std::nullopt;
  const auto& builder_ref = std::get<Expr::VarRef>(call.args[0]->node);
  if (builder_ref.var != st.var) return std::nullopt;
  if (expr_uses_var(*call.args[1], st.var)) return std::nullopt;
  return "nebula::rt::json_array_push(std::move(" + st.name + "), " +
         emit_expr(ctx, *call.args[1]) + ")";
}

static void emit_let(Cpp& out, EmitCtx& ctx, const Stmt::Let& st) {
  ctx.local_value_vars.insert(st.var);
  ctx.local_value_decl_block[st.var] = ctx.current_block;
  const StorageDecision* dec = lookup_decision(*ctx.rep_owner, ctx.fn_name, st.var);
  const RepKind rep = dec ? dec->rep : RepKind::Stack;
  const OwnerKind owner = dec ? dec->owner : OwnerKind::None;
  const std::string region = (dec ? dec->region : "");

  if (std::holds_alternative<Expr::Call>(st.value->node)) {
    const auto& call = std::get<Expr::Call>(st.value->node);
    if (is_std_json_call(call, "stringify") && call.args.size() == 1 &&
        !current_function_reassigns_var(ctx, st.var)) {
      const std::string source_name =
          "__nebula_stringify_src_" + std::to_string(ctx.temp_counter++);
      out.line("const auto& " + source_name + " = " + emit_expr(ctx, *call.args[0]) + ";");
      if (current_function_uses_var_outside_parse_alias(ctx, st.var)) {
        out.line("auto " + st.name + " = nebula::rt::json_stringify(" + source_name + ");");
      }
      ctx.stringify_source_vars[st.var] = source_name;
      if (!ctx.stringify_scope_stack.empty()) ctx.stringify_scope_stack.back().push_back(st.var);
      return;
    }
  }

  // Try to pattern match constructors for better lowering.
  const Expr* rhs = st.value.get();
  if (std::holds_alternative<Expr::Prefix>(rhs->node)) {
    const auto& p = std::get<Expr::Prefix>(rhs->node);
    rhs = p.inner.get();
  }

  const bool is_construct = rhs && std::holds_alternative<Expr::Construct>(rhs->node);

  if (is_construct) {
    const auto& c = std::get<Expr::Construct>(rhs->node);
    if (rep == RepKind::Region) {
      const std::string& R = !region.empty() ? region : (ctx.region_stack.empty() ? "" : ctx.region_stack.back());
      std::ostringstream os;
      os << "auto* " << st.name << " = NEBULA_ALLOC(" << R << ", " << cpp_type(st.value->ty);
      if (c.variant_name.has_value()) {
        os << ", " << cpp_type(st.value->ty) << "::" << *c.variant_name;
        if (c.args.empty()) {
          os << "{}";
        } else {
          os << "{";
          for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i) os << ", ";
            os << emit_expr(ctx, *c.args[i]);
          }
          os << "}";
        }
      } else {
        for (const auto& a : c.args) os << ", " << emit_expr(ctx, *a);
      }
      os << ");";
      out.line(os.str());
      return;
    }
    if (rep == RepKind::Heap && owner == OwnerKind::Shared) {
      out.line("auto " + st.name + " = " + emit_heap_make(PrefixKind::Shared, ctx, st.value->ty, c) +
               ";");
      return;
    }
    if (rep == RepKind::Heap && owner == OwnerKind::Unique) {
      out.line("auto " + st.name + " = " + emit_heap_make(PrefixKind::Unique, ctx, st.value->ty, c) +
               ";");
      return;
    }
    // Stack
    out.line("auto " + st.name + " = " + emit_construct_call(ctx, st.value->ty, c) + ";");
    return;
  }

  // Non-construct RHS
  if (should_bind_let_by_const_ref(ctx, st)) {
    out.line("const auto& " + st.name + " = " + emit_expr(ctx, *st.value) + ";");
    return;
  }
  out.line("auto " + st.name + " = " + emit_expr(ctx, *st.value) + ";");
}

static bool return_is_direct_region_construct_escape(const EmitCtx& ctx, const Expr& e) {
  if (ctx.region_stack.empty()) return false;
  // If return expression is a bare construct (no explicit heap override), it would escape the region.
  const Expr* cur = &e;
  if (std::holds_alternative<Expr::Prefix>(cur->node)) {
    // explicit => not a silent escape
    return false;
  }
  return std::holds_alternative<Expr::Construct>(cur->node);
}

static void emit_return(Cpp& out, EmitCtx& ctx, const Stmt::Return& st) {
  if (ctx.current_function != nullptr && ctx.current_function->is_async) {
    out.line("co_return " + emit_expr(ctx, *st.value) + ";");
    return;
  }
  if (return_is_direct_region_construct_escape(ctx, *st.value)) {
    if (ctx.opt && ctx.opt->strict_region) {
      emit_panic_policy(out, *ctx.opt, "\"region escape in strict mode\"");
      return;
    }
    // Default policy: auto-promote to heap unique on direct return.
    const auto& c = std::get<Expr::Construct>(st.value->node);
    out.line("return " + emit_heap_make(PrefixKind::Unique, ctx, st.value->ty, c) + ";");
    return;
  }

  out.line("return " + emit_expr(ctx, *st.value) + ";");
}

static void emit_stmt(Cpp& out, EmitCtx& ctx, const Stmt& s) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Declare>) {
          out.line(cpp_type(st.ty) + " " + st.name + ";");
        } else if constexpr (std::is_same_v<S, Stmt::Let>) {
          emit_let(out, ctx, st);
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          ctx.stringify_source_vars.erase(st.var);
          if (auto moved_builder = emit_json_array_builder_self_push(ctx, st); moved_builder.has_value()) {
            out.line(st.name + " = " + *moved_builder + ";");
            return;
          }
          out.line(st.name + " = " + emit_expr(ctx, *st.value) + ";");
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          out.line(emit_member_access(ctx, st.base_var, st.base_name, st.field) + " = " +
                   emit_expr(ctx, *st.value) + ";");
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          out.line(emit_expr(ctx, *st.expr) + ";");
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          emit_return(out, ctx, st);
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          out.line("{");
          out.indent++;
          out.line("NEBULA_REGION(" + st.name + ");");
          ctx.region_stack.push_back(st.name);
          emit_block(out, ctx, st.body);
          ctx.region_stack.pop_back();
          out.indent--;
          out.line("}");
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          out.line("{");
          out.indent++;
          emit_block(out, ctx, st.body);
          out.indent--;
          out.line("}");
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          out.line("if (" + emit_condition_expr(ctx, *st.cond) + ") {");
          out.indent++;
          emit_block(out, ctx, st.then_body);
          out.indent--;
          if (st.else_body.has_value()) {
            out.line("} else {");
            out.indent++;
            emit_block(out, ctx, *st.else_body);
            out.indent--;
          }
          out.line("}");
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          const std::string i = st.var_name;
          const std::string a = emit_expr(ctx, *st.start);
          const std::string b = emit_expr(ctx, *st.end);
          out.line("for (std::int64_t " + i + " = (" + a + "); " + i + " < (" + b + "); ++" + i +
                   ") {");
          out.indent++;
          emit_block(out, ctx, st.body);
          out.indent--;
          out.line("}");
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          out.line("while (" + emit_condition_expr(ctx, *st.cond) + ") {");
          out.indent++;
          emit_block(out, ctx, st.body);
          out.indent--;
          out.line("}");
        } else if constexpr (std::is_same_v<S, Stmt::Break>) {
          out.line("break;");
        } else if constexpr (std::is_same_v<S, Stmt::Continue>) {
          out.line("continue;");
        }
      },
      s.node);
}

static void emit_struct_def(Cpp& out, const StructDef& s) {
  if (is_nebula_crypto_bytes_backed_struct_name(s.qualified_name)) {
    return;
  }
  if (is_nebula_tls_handle_struct_name(s.qualified_name)) {
    return;
  }
  if (is_nebula_db_sqlite_handle_struct_name(s.qualified_name)) {
    return;
  }
  if (is_nebula_db_postgres_handle_struct_name(s.qualified_name)) {
    return;
  }
  if (s.qualified_name.package_name == "std" &&
      (s.qualified_name.module_name == "task" || s.qualified_name.module_name == "time" ||
       s.qualified_name.module_name == "bytes" || s.qualified_name.module_name == "net")) {
    return;
  }
  if (s.qualified_name.package_name == "std" && s.qualified_name.module_name == "http" &&
      (s.qualified_name.local_name == "Request" || s.qualified_name.local_name == "Response" ||
       s.qualified_name.local_name == "ClientRequest" ||
       s.qualified_name.local_name == "ClientResponse" ||
       s.qualified_name.local_name == "RouteParams2" ||
       s.qualified_name.local_name == "RouteParams3" ||
       s.qualified_name.local_name == "RoutePattern")) {
    return;
  }
  if (s.qualified_name.package_name == "std" && s.qualified_name.module_name == "json" &&
      (s.qualified_name.local_name == "Json" || s.qualified_name.local_name == "JsonArrayBuilder")) {
    return;
  }
  if (s.qualified_name.package_name == "std" && s.qualified_name.module_name == "process" &&
      (s.qualified_name.local_name == "ProcessCommand" ||
       s.qualified_name.local_name == "ProcessOutput")) {
    return;
  }
  const std::string cpp_name = emitted_cpp_type_name_for(s);
  if (!s.type_params.empty()) {
    std::string tmpl = "template <";
    for (std::size_t i = 0; i < s.type_params.size(); ++i) {
      if (i) tmpl += ", ";
      tmpl += "typename " + s.type_params[i];
    }
    tmpl += ">";
    out.line(tmpl);
  }
  out.line("struct " + cpp_name + " {");
  out.indent++;
  for (const auto& f : s.fields) {
    out.line(cpp_decl(f.ty, f.name) + ";");
  }
  if (!s.fields.empty()) {
    std::ostringstream sig;
    sig << cpp_name << "(";
    for (std::size_t i = 0; i < s.fields.size(); ++i) {
      if (i) sig << ", ";
      sig << cpp_decl(s.fields[i].ty, s.fields[i].name + "_");
    }
    sig << ") : ";
    for (std::size_t i = 0; i < s.fields.size(); ++i) {
      if (i) sig << ", ";
      sig << s.fields[i].name << "(" << ctor_field_init_expr(s.fields[i].ty, s.fields[i].name + "_") << ")";
      }
      sig << " {}";
    out.line(sig.str());
  } else {
    out.line(cpp_name + "() = default;");
  }
  out.indent--;
  out.line("};");
}

static void emit_enum_def(Cpp& out, const EnumDef& e) {
  if (e.qualified_name.package_name == "std" && e.qualified_name.module_name == "result" &&
      e.qualified_name.local_name == "Result") {
    return;
  }
  if (e.qualified_name.package_name == "std" && e.qualified_name.module_name == "http" &&
      e.qualified_name.local_name == "Method") {
    return;
  }
  const std::string cpp_name = emitted_cpp_type_name_for(e);
  if (!e.type_params.empty()) {
    std::string tmpl = "template <";
    for (std::size_t i = 0; i < e.type_params.size(); ++i) {
      if (i) tmpl += ", ";
      tmpl += "typename " + e.type_params[i];
    }
    tmpl += ">";
    out.line(tmpl);
  }
  out.line("struct " + cpp_name + " {");
  out.indent++;
  for (const auto& variant : e.variants) {
    const std::string payload_cpp =
        (variant.payload.kind == Ty::Kind::Void) ? "std::monostate" : cpp_type(variant.payload);
    out.line("struct " + variant.name + " {");
    out.indent++;
    if (variant.payload.kind != Ty::Kind::Void) {
      out.line(payload_cpp + " value;");
      out.line(variant.name + "(" + payload_cpp + " value_) : value(value_) {}");
    } else {
      out.line(variant.name + "() = default;");
    }
    out.indent--;
    out.line("};");
  }
  std::ostringstream storage;
  storage << "std::variant<";
  for (std::size_t i = 0; i < e.variants.size(); ++i) {
    if (i) storage << ", ";
    storage << e.variants[i].name;
  }
  storage << "> data;";
  out.line(storage.str());
  for (const auto& variant : e.variants) {
    out.line(cpp_name + "(" + variant.name + " value) : data(std::move(value)) {}");
  }
  out.indent--;
  out.line("};");
}

static void collect_calls_in_expr(const Expr& e, std::unordered_set<std::string>& out) {
  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          if (n.kind == nebula::nir::CallKind::Direct) out.insert(nebula::nir::call_target_identity(n));
          for (const auto& a : n.args) collect_calls_in_expr(*a, out);
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          // no nested calls
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          collect_calls_in_expr(*n.base, out);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
          collect_calls_in_expr(*n.subject, out);
        } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
          collect_calls_in_expr(*n.subject, out);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& a : n.args) collect_calls_in_expr(*a, out);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_calls_in_expr(*n.lhs, out);
          collect_calls_in_expr(*n.rhs, out);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          collect_calls_in_expr(*n.inner, out);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          collect_calls_in_expr(*n.inner, out);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          collect_calls_in_expr(*n.subject, out);
          for (const auto& arm : n.arms) collect_calls_in_expr(*arm->value, out);
        } else {
        }
      },
      e.node);
}

static void collect_calls_in_block(const Block& b, std::unordered_set<std::string>& out) {
  for (const auto& s : b.stmts) {
    std::visit(
        [&](auto&& st) {
          using S = std::decay_t<decltype(st)>;
          if constexpr (std::is_same_v<S, Stmt::Declare>) {
          } else if constexpr (std::is_same_v<S, Stmt::Let>) {
            collect_calls_in_expr(*st.value, out);
          } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
            collect_calls_in_expr(*st.value, out);
          } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
            collect_calls_in_expr(*st.value, out);
          } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
            collect_calls_in_expr(*st.expr, out);
          } else if constexpr (std::is_same_v<S, Stmt::Return>) {
            collect_calls_in_expr(*st.value, out);
          } else if constexpr (std::is_same_v<S, Stmt::Region>) {
            collect_calls_in_block(st.body, out);
          } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
            collect_calls_in_block(st.body, out);
          } else if constexpr (std::is_same_v<S, Stmt::If>) {
            collect_calls_in_expr(*st.cond, out);
            collect_calls_in_block(st.then_body, out);
            if (st.else_body.has_value()) collect_calls_in_block(*st.else_body, out);
          } else if constexpr (std::is_same_v<S, Stmt::For>) {
            collect_calls_in_expr(*st.start, out);
            collect_calls_in_expr(*st.end, out);
            collect_calls_in_block(st.body, out);
          } else if constexpr (std::is_same_v<S, Stmt::While>) {
            collect_calls_in_expr(*st.cond, out);
            collect_calls_in_block(st.body, out);
          }
        },
        s.node);
  }
}

static std::vector<const Function*> topo_sort_functions(const Program& p) {
  // v0.1 best-effort: attempt to order functions so callees appear before callers.
  std::unordered_map<std::string, const Function*> fns;
  for (const auto& it : p.items) {
    if (std::holds_alternative<Function>(it.node)) {
      const auto& fn = std::get<Function>(it.node);
      fns.insert({nebula::nir::function_identity(fn), &fn});
    }
  }

  std::unordered_map<std::string, std::unordered_set<std::string>> edges;
  std::unordered_map<std::string, std::size_t> indeg;
  for (const auto& [name, fn] : fns) {
    std::unordered_set<std::string> calls;
    if (fn->body.has_value()) collect_calls_in_block(*fn->body, calls);
    for (const auto& callee : calls) {
      if (fns.count(callee) == 0) continue;
      if (callee == name) continue;
      edges[name].insert(callee);
    }
    indeg[name] = 0;
  }
  for (const auto& [u, vs] : edges) {
    for (const auto& v : vs) {
      indeg[v] += 1;
    }
  }

  // Kahn
  std::vector<std::string> q;
  for (const auto& [name, d] : indeg) {
    if (d == 0) q.push_back(name);
  }
  std::sort(q.begin(), q.end());

  std::vector<const Function*> ordered;
  ordered.reserve(fns.size());

  while (!q.empty()) {
    std::string u = q.back();
    q.pop_back();
    ordered.push_back(fns[u]);
    for (const auto& v : edges[u]) {
      if (--indeg[v] == 0) q.push_back(v);
    }
  }

  // If cycle exists, append remaining in source order.
  if (ordered.size() != fns.size()) {
    std::unordered_set<std::string> seen;
    for (const auto* fn : ordered) seen.insert(nebula::nir::function_identity(*fn));
    for (const auto& it : p.items) {
      if (!std::holds_alternative<Function>(it.node)) continue;
      const auto& fn = std::get<Function>(it.node);
      if (!seen.count(nebula::nir::function_identity(fn))) ordered.push_back(&fn);
    }
  }

  // Emit callees first: current order is sinks-first due to q.pop_back sorting; reverse it.
  std::reverse(ordered.begin(), ordered.end());
  return ordered;
}

static void emit_function(Cpp& out,
                          const RepOwnerResult& rep_owner,
                          const EmitOptions& opt,
                          const FunctionSymbolMap& function_symbols,
                          const Function& fn) {
  if (!fn.type_params.empty()) {
    std::string tmpl = "template <";
    for (std::size_t i = 0; i < fn.type_params.size(); ++i) {
      if (i) tmpl += ", ";
      tmpl += "typename " + fn.type_params[i];
    }
    tmpl += ">";
    out.line(tmpl);
  }
  if (fn.is_extern) {
    std::ostringstream decl;
    const std::string fn_name = emitted_cpp_name_for(fn);
    decl << cpp_type(fn.ret) << " " << fn_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) decl << ", ";
      decl << cpp_extern_param_decl(fn, fn.params[i]);
    }
    decl << ");";
    out.line(decl.str());
    return;
  }
  EmitCtx ctx;
  ctx.rep_owner = &rep_owner;
  ctx.opt = &opt;
  ctx.function_symbols = &function_symbols;
  ctx.fn_name = nebula::nir::function_identity(fn);
  ctx.current_function = &fn;

  std::ostringstream sig;
  const Stmt::Return* const_json_return = const_json_return_stmt(fn);
  const std::string ret_cpp =
      (const_json_return != nullptr) ? ("const " + cpp_type(fn.ret) + "&")
                                     : function_return_cpp_type(fn, rep_owner, opt);
  sig << "auto " << emitted_cpp_name_for(fn) << "(";
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    if (i) sig << ", ";
    const auto& p = fn.params[i];
    sig << cpp_param_decl(fn, p);
  }
  sig << ") -> " << ret_cpp << " {";
  out.line(sig.str());
  out.indent++;

  if (const_json_return != nullptr) {
    out.line("static const auto __nebula_cached = " + emit_expr(ctx, *const_json_return->value) + ";");
    out.line("return __nebula_cached;");
  } else if (fn.body.has_value()) {
    emit_block(out, ctx, *fn.body);
  }

  if (fn.is_async && fn.ret.kind == Ty::Kind::Void) {
    out.line("co_return;");
  }

  out.indent--;
  out.line("}");
}

static void emit_function_forward_decl(Cpp& out,
                                       const RepOwnerResult& rep_owner,
                                       const EmitOptions& opt,
                                       const Function& fn) {
  if (!fn.type_params.empty()) {
    std::string tmpl = "template <";
    for (std::size_t i = 0; i < fn.type_params.size(); ++i) {
      if (i) tmpl += ", ";
      tmpl += "typename " + fn.type_params[i];
    }
    tmpl += ">";
    out.line(tmpl);
  }
  if (fn.is_extern) {
    std::ostringstream decl;
    const std::string fn_name = emitted_cpp_name_for(fn);
    decl << cpp_type(fn.ret) << " " << fn_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) decl << ", ";
      decl << cpp_extern_param_decl(fn, fn.params[i]);
    }
    decl << ");";
    out.line(decl.str());
    return;
  }
  std::ostringstream sig;
  const std::string ret_cpp =
      (const_json_return_stmt(fn) != nullptr) ? ("const " + cpp_type(fn.ret) + "&")
                                              : function_return_cpp_type(fn, rep_owner, opt);
  sig << "auto " << emitted_cpp_name_for(fn) << "(";
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    if (i) sig << ", ";
    const auto& p = fn.params[i];
    sig << cpp_param_decl(fn, p);
  }
  sig << ") -> " << ret_cpp << ";";
  out.line(sig.str());
}

static std::vector<CAbiFunction> collect_c_abi_functions_impl(
    const Program& p,
    std::optional<std::string_view> package_name) {
  std::vector<CAbiFunction> out;
  for (const auto& it : p.items) {
    if (!std::holds_alternative<Function>(it.node)) continue;
    const auto& fn = std::get<Function>(it.node);
    if (fn.is_extern || !is_c_abi_annotation_set(fn.annotations)) continue;
    if (package_name.has_value() && fn.qualified_name.package_name != *package_name) continue;
    out.push_back(CAbiFunction{
        c_abi_export_name_for(fn), fn.qualified_name, fn.name, fn.params, fn.ret});
  }
  return out;
}

static void emit_c_abi_wrapper_block(Cpp& out,
                                     const std::vector<CAbiFunction>& exports,
                                     const FunctionSymbolMap& function_symbols) {
  if (exports.empty()) return;
  out.line("#if defined(_WIN32)");
  out.line("#define NEBULA_CABI_EXPORT __declspec(dllexport)");
  out.line("#else");
  out.line("#define NEBULA_CABI_EXPORT");
  out.line("#endif");
  out.blank();
  for (const auto& fn : exports) {
    const std::string internal_name =
        emitted_cpp_name_for_identity(function_symbols,
                                      nebula::nir::qualified_identity(fn.qualified_name, fn.local_name),
                                      fn.local_name);
    const std::string ret_cpp = c_abi_cpp_type(fn.ret).value_or("void");
    std::ostringstream sig;
    sig << "extern \"C\" NEBULA_CABI_EXPORT " << ret_cpp << " " << fn.export_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) sig << ", ";
      sig << c_abi_cpp_type(fn.params[i].ty).value_or("void") << " " << fn.params[i].name;
    }
    sig << ") {";
    out.line(sig.str());
    out.indent++;
    std::ostringstream call;
    if (fn.ret.kind != Ty::Kind::Void) call << "return ";
    call << internal_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) call << ", ";
      call << fn.params[i].name;
    }
    call << ");";
    out.line(call.str());
    out.indent--;
    out.line("}");
    out.blank();
  }
}

static std::string make_header_guard(std::string_view header_stem) {
  std::string out = "NEBULA_";
  for (char ch : header_stem) {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    } else {
      out.push_back('_');
    }
  }
  out += "_H";
  return out;
}

} // namespace

std::string emit_cpp23(const Program& p, const RepOwnerResult& rep_owner, const EmitOptions& opt) {
  Cpp out;

  out.line("#include \"runtime/nebula_runtime.hpp\"");
  out.line("#include <algorithm>");
  out.line("#include <chrono>");
  out.line("#include <cmath>");
  out.line("#include <cstdint>");
  out.line("#include <cstdlib>");
  out.line("#include <iostream>");
  out.line("#include <memory>");
  out.line("#include <string>");
  out.line("#include <utility>");
  out.line("#include <variant>");
  out.line("#include <vector>");
  out.blank();

  out.line("// nebula-codegen-profile runtime_profile=" +
           std::string(runtime_profile_name(opt.runtime_profile)) + " target=" +
           escape_string(opt.target) + " panic_policy=" + panic_policy_name(opt.panic_policy));
  out.line("static constexpr const char* NEBULA_RUNTIME_PROFILE = \"" +
           std::string(runtime_profile_name(opt.runtime_profile)) + "\";");
  out.line("static constexpr const char* NEBULA_TARGET = \"" + escape_string(opt.target) + "\";");
  out.line("static constexpr const char* NEBULA_PANIC_POLICY = \"" +
           std::string(panic_policy_name(opt.panic_policy)) + "\";");
  out.line("[[noreturn]] static inline void __nebula_panic_policy(std::string msg) {");
  out.indent++;
  emit_panic_policy(out, opt, "msg");
  out.indent--;
  out.line("}");
  out.blank();

  out.line("static inline void expect_eq(std::int64_t a, std::int64_t b) {");
  out.indent++;
  out.line("nebula::rt::expect_eq_i64(a, b, \"expect_eq\");");
  out.indent--;
  out.line("}");
  out.line("static inline void assert(bool cond) {");
  out.indent++;
  out.line("nebula::rt::assert(cond, \"assertion failed\");");
  out.indent--;
  out.line("}");
  out.blank();

  // Types first
  for (const auto& it : p.items) {
    if (std::holds_alternative<StructDef>(it.node)) {
      emit_struct_def(out, std::get<StructDef>(it.node));
      out.blank();
    } else if (std::holds_alternative<EnumDef>(it.node)) {
      emit_enum_def(out, std::get<EnumDef>(it.node));
      out.blank();
    }
  }

  // Functions (best-effort topological order)
  auto ordered = topo_sort_functions(p);
  FunctionSymbolMap function_symbols;
  function_symbols.reserve(ordered.size());
  for (const auto* fn : ordered) {
    function_symbols.insert({nebula::nir::function_identity(*fn), emitted_cpp_name_for(*fn)});
  }
  for (const auto* fn : ordered) {
    emit_function_forward_decl(out, rep_owner, opt, *fn);
    out.blank();
  }
  for (const auto* fn : ordered) {
    emit_function(out, rep_owner, opt, function_symbols, *fn);
    out.blank();
  }

  if (opt.emit_c_abi_wrappers) {
    emit_c_abi_wrapper_block(out, collect_c_abi_functions_impl(p, opt.c_abi_export_package),
                             function_symbols);
  }

  // Main harness
  if (opt.main_mode != MainMode::None) {
    out.line("int main(int argc, char** argv) {");
    out.indent++;
    const bool catch_user_panic = opt.runtime_profile != RuntimeProfile::System;
    if (catch_user_panic) {
      out.line("try {");
      out.indent++;
    }
    out.line("nebula::rt::set_process_args(argc, argv);");

    if (opt.main_mode == MainMode::CallMainIfPresent) {
      const Function* entry_main = nullptr;
      for (const auto* fn : ordered) {
        if (fn->qualified_name.local_name != "main") continue;
        if (p.package_name.has_value() &&
            fn->qualified_name.package_name == *p.package_name &&
            (!p.module_name.has_value() || fn->qualified_name.module_name == *p.module_name)) {
          entry_main = fn;
          break;
        }
        if (entry_main == nullptr) entry_main = fn;
      }
      if (entry_main != nullptr) {
        const std::string entry_name =
            emitted_cpp_name_for_identity(function_symbols,
                                          nebula::nir::function_identity(*entry_main),
                                          entry_main->name);
        if (entry_main->is_async) {
          if (entry_main->ret.kind == Ty::Kind::Int) {
            out.line("return static_cast<int>(nebula::rt::block_on(" + entry_name + "()));");
          } else {
            out.line("nebula::rt::block_on(" + entry_name + "());");
          }
        } else {
          if (entry_main->ret.kind == Ty::Kind::Int) {
            out.line("return static_cast<int>(" + entry_name + "());");
          } else {
            out.line("(void)" + entry_name + "();");
          }
        }
      } else {
        out.line("(void)0;");
      }
      out.line("return 0;");
    } else if (opt.main_mode == MainMode::RunTests) {
      // Call all @test functions.
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "test")) {
          out.line("std::cerr << \"[test] " + fn->name + "\\n\";");
          const std::string fn_name =
              emitted_cpp_name_for_identity(function_symbols,
                                            nebula::nir::function_identity(*fn),
                                            fn->name);
          if (fn->is_async) {
            out.line("nebula::rt::block_on(" + fn_name + "());");
          } else {
            out.line(fn_name + "();");
          }
        }
      }
      out.line("std::cerr << \"[test] ok\\n\";");
      out.line("return 0;");
    } else if (opt.main_mode == MainMode::RunBench) {
      out.line("using Clock = std::chrono::steady_clock;");
      out.line("std::vector<double> samples_ms;");
      out.line("int bench_fns = 0;");
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "bench")) out.line("bench_fns += 1;");
      }
      out.line("const int warmup = 50;");
      out.line("const int iters = 1000;");
      out.line("for (int i = 0; i < warmup; ++i) {");
      out.indent++;
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "bench")) {
          const std::string fn_name =
              emitted_cpp_name_for_identity(function_symbols,
                                            nebula::nir::function_identity(*fn),
                                            fn->name);
          if (fn->is_async) {
            out.line("nebula::rt::block_on(" + fn_name + "());");
          } else {
            out.line(fn_name + "();");
          }
        }
      }
      out.indent--;
      out.line("}");
      out.line("for (int i = 0; i < iters; ++i) {");
      out.indent++;
      out.line("auto t0 = Clock::now();");
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "bench")) {
          const std::string fn_name =
              emitted_cpp_name_for_identity(function_symbols,
                                            nebula::nir::function_identity(*fn),
                                            fn->name);
          if (fn->is_async) {
            out.line("nebula::rt::block_on(" + fn_name + "());");
          } else {
            out.line(fn_name + "();");
          }
        }
      }
      out.line("auto t1 = Clock::now();");
      out.line("double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();");
      out.line("samples_ms.push_back(ms);");
      out.indent--;
      out.line("}");
      out.line("std::sort(samples_ms.begin(), samples_ms.end());");
      out.line("auto pct = [&](double p) {");
      out.indent++;
      out.line("if (samples_ms.empty()) return 0.0;");
      out.line("std::size_t idx = (std::size_t)((p / 100.0) * (samples_ms.size() - 1));");
      out.line("return samples_ms[idx];");
      out.indent--;
      out.line("};");
      out.line("double p50 = pct(50);");
      out.line("double p90 = pct(90);");
      out.line("double p99 = pct(99);");
      out.line("double sum_ms = 0.0;");
      out.line("for (double x : samples_ms) sum_ms += x;");
      out.line("double mean_ms = samples_ms.empty() ? 0.0 : (sum_ms / (double)samples_ms.size());");
      out.line("double variance = 0.0;");
      out.line("for (double x : samples_ms) {");
      out.indent++;
      out.line("double delta = x - mean_ms;");
      out.line("variance += delta * delta;");
      out.indent--;
      out.line("}");
      out.line("double stddev_ms = samples_ms.size() <= 1 ? 0.0 : std::sqrt(variance / (double)(samples_ms.size() - 1));");
      out.line("double total_s = sum_ms / 1000.0;");
      out.line("double ops = (double)iters * (double)bench_fns;");
      out.line("double throughput = (total_s > 0.0) ? (ops / total_s) : 0.0;");
      out.line("const char* bench_os =");
      out.line("#if defined(__APPLE__)");
      out.line("\"macos\";");
      out.line("#elif defined(__linux__)");
      out.line("\"linux\";");
      out.line("#elif defined(_WIN32)");
      out.line("\"windows\";");
      out.line("#else");
      out.line("\"unknown\";");
      out.line("#endif");
      out.line("const char* bench_arch =");
      out.line("#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)");
      out.line("\"arm64\";");
      out.line("#elif defined(__x86_64__) || defined(_M_X64)");
      out.line("\"x64\";");
      out.line("#elif defined(__i386__) || defined(_M_IX86)");
      out.line("\"x86\";");
      out.line("#else");
      out.line("\"unknown\";");
      out.line("#endif");
      out.line("const char* perf_capability = \"unsupported\";");
      out.line("const char* perf_reason = \"not_implemented\";");
      out.line("std::cout << \"NEBULA_BENCH warmup_iterations=\" << warmup"
               " << \" measure_iterations=\" << iters"
               " << \" samples=\" << samples_ms.size()"
               " << \" p50_ms=\" << p50"
               " << \" p90_ms=\" << p90"
               " << \" p99_ms=\" << p99"
               " << \" mean_ms=\" << mean_ms"
               " << \" stddev_ms=\" << stddev_ms"
               " << \" throughput_ops_s=\" << throughput"
               " << \" clock=steady_clock\""
               " << \" platform=\" << bench_os << \"-\" << bench_arch"
               " << \" perf_capability=\" << perf_capability"
               " << \" perf_counters=unsupported\""
               " << \" perf_reason=\" << perf_reason << \"\\n\";");
      out.line("return 0;");
    }

    if (catch_user_panic) {
      out.indent--;
      out.line("} catch (const nebula::rt::UserPanic& ex) {");
      out.indent++;
      emit_panic_policy(out, opt, "ex.what()");
      out.indent--;
      out.line("}");
    }
    out.indent--;
    out.line("}");
  }

  return out.os.str();
}

std::vector<CAbiFunction> collect_c_abi_functions(const Program& p,
                                                  std::optional<std::string_view> package_name) {
  return collect_c_abi_functions_impl(p, package_name);
}

std::string emit_c_abi_header(const Program& p,
                              const std::vector<CAbiFunction>& exports,
                              std::string_view header_stem) {
  (void)p;
  std::ostringstream out;
  const std::string guard = make_header_guard(header_stem);
  out << "#ifndef " << guard << "\n";
  out << "#define " << guard << "\n\n";
  out << "#include <stdbool.h>\n";
  out << "#include <stdint.h>\n\n";
  out << "#if defined(__cplusplus)\n";
  out << "extern \"C\" {\n";
  out << "#endif\n\n";
  out << "#if defined(_WIN32)\n";
  out << "#define NEBULA_CABI_API\n";
  out << "#else\n";
  out << "#define NEBULA_CABI_API\n";
  out << "#endif\n\n";
  for (const auto& fn : exports) {
    out << "NEBULA_CABI_API " << c_abi_header_type(fn.ret).value_or("void") << " "
        << fn.export_name << "(";
    if (fn.params.empty()) {
      out << "void";
    } else {
      for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out << ", ";
        out << c_abi_header_type(fn.params[i].ty).value_or("void") << " " << fn.params[i].name;
      }
    }
    out << ");\n";
  }
  if (!exports.empty()) out << "\n";
  out << "#if defined(__cplusplus)\n";
  out << "}\n";
  out << "#endif\n\n";
  out << "#endif\n";
  return out.str();
}

} // namespace nebula::codegen
