#include "codegen/cpp_backend.hpp"

#include "frontend/types.hpp"

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
  case Ty::Kind::Struct: return emitted_cpp_type_name_for(t.qualified_name, t.name);
  case Ty::Kind::Enum:
    if (t.type_arg) {
      return emitted_cpp_type_name_for(t.qualified_name, t.name) + "<" + cpp_type(*t.type_arg) +
             ">";
    }
    return emitted_cpp_type_name_for(t.qualified_name, t.name) + "<void>";
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
  std::string fn_name;
  std::vector<std::string> region_stack;
};

static std::string emit_expr(const EmitCtx& ctx, const Expr& e);

static bool member_access_uses_arrow(const EmitCtx& ctx, VarId base_var) {
  const StorageDecision* dec = lookup_decision(*ctx.rep_owner, ctx.fn_name, base_var);
  if (!dec) return false;
  return dec->rep != RepKind::Stack;
}

static std::string emit_member_access(const EmitCtx& ctx, VarId base_var,
                                      const std::string& base_name,
                                      const std::string& field) {
  return base_name + (member_access_uses_arrow(ctx, base_var) ? "->" : ".") + field;
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
        } else {
          // other statements: nothing
        }
      },
      s.node);
}

static std::string function_return_cpp_type(const Function& fn, const RepOwnerResult& rep_owner,
                                            const EmitOptions& opt) {
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

static std::string emit_construct_call(const EmitCtx& ctx, const std::string& type_name,
                                       const std::vector<nebula::nir::ExprPtr>& args) {
  std::ostringstream os;
  os << type_name << "(";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) os << ", ";
    os << emit_expr(ctx, *args[i]);
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
    os << emit_expr(ctx, *args[i]);
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
      os << emit_expr(ctx, *construct.args[i]);
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
      os << emit_expr(ctx, *construct.args[i]);
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
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          return "(" + emit_expr(ctx, *n.lhs) + " " + op_to_cpp(n.op) + " " + emit_expr(ctx, *n.rhs) +
                 ")";
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          return "(!" + emit_expr(ctx, *n.inner) + ")";
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          std::ostringstream os;
          if (n.kind == nebula::nir::CallKind::Indirect) {
            os << n.callee << "(";
          } else {
            const std::string target = nebula::nir::call_target_identity(n);
            if (ctx.function_symbols != nullptr) {
              os << emitted_cpp_name_for_identity(*ctx.function_symbols, target, n.callee) << "(";
            } else {
              os << n.callee << "(";
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
        } else {
          return "/*expr*/";
        }
      },
      e.node);
}

static void emit_stmt(Cpp& out, EmitCtx& ctx, const Stmt& s);

static void emit_block(Cpp& out, EmitCtx& ctx, const Block& b) {
  for (const auto& st : b.stmts) emit_stmt(out, ctx, st);
}

static void emit_let(Cpp& out, EmitCtx& ctx, const Stmt::Let& st) {
  const StorageDecision* dec = lookup_decision(*ctx.rep_owner, ctx.fn_name, st.var);
  const RepKind rep = dec ? dec->rep : RepKind::Stack;
  const OwnerKind owner = dec ? dec->owner : OwnerKind::None;
  const std::string region = (dec ? dec->region : "");

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
  if (return_is_direct_region_construct_escape(ctx, *st.value)) {
    if (ctx.opt && ctx.opt->strict_region) {
      out.line("nebula::rt::panic(\"region escape in strict mode\");");
      out.line("return {};"); // unreachable; avoids UB/invalid returns across return types
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
        if constexpr (std::is_same_v<S, Stmt::Let>) {
          emit_let(out, ctx, st);
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
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
          out.line("if (" + emit_expr(ctx, *st.cond) + ") {");
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
        }
      },
      s.node);
}

static void emit_struct_def(Cpp& out, const StructDef& s) {
  const std::string cpp_name = emitted_cpp_type_name_for(s);
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
      sig << s.fields[i].name << "(" << s.fields[i].name << "_)";
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
  const std::string cpp_name = emitted_cpp_type_name_for(e);
  out.line("template <typename " + e.type_param + ">");
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
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& a : n.args) collect_calls_in_expr(*a, out);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_calls_in_expr(*n.lhs, out);
          collect_calls_in_expr(*n.rhs, out);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          collect_calls_in_expr(*n.inner, out);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          collect_calls_in_expr(*n.inner, out);
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
          if constexpr (std::is_same_v<S, Stmt::Let>) {
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
  if (fn.is_extern) {
    std::ostringstream decl;
    const std::string fn_name = emitted_cpp_name_for(fn);
    decl << cpp_type(fn.ret) << " " << fn_name << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) decl << ", ";
      decl << cpp_decl(fn.params[i].ty, fn.params[i].name, fn.params[i].is_ref);
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

  std::ostringstream sig;
  const std::string ret_cpp = function_return_cpp_type(fn, rep_owner, opt);
  sig << "auto " << emitted_cpp_name_for(fn) << "(";
  for (std::size_t i = 0; i < fn.params.size(); ++i) {
    if (i) sig << ", ";
    const auto& p = fn.params[i];
    sig << cpp_decl(p.ty, p.name, p.is_ref);
  }
  sig << ") -> " << ret_cpp << " {";
  out.line(sig.str());
  out.indent++;

  if (fn.body.has_value()) emit_block(out, ctx, *fn.body);

  // If function is void-ish and no return, keep C++ happy; otherwise let auto-deduction handle.
  out.indent--;
  out.line("}");
}

} // namespace

std::string emit_cpp23(const Program& p, const RepOwnerResult& rep_owner, const EmitOptions& opt) {
  Cpp out;

  out.line("#include \"runtime/nebula_runtime.hpp\"");
  out.line("#include <algorithm>");
  out.line("#include <chrono>");
  out.line("#include <cmath>");
  out.line("#include <cstdint>");
  out.line("#include <iostream>");
  out.line("#include <memory>");
  out.line("#include <string>");
  out.line("#include <variant>");
  out.line("#include <vector>");
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
    emit_function(out, rep_owner, opt, function_symbols, *fn);
    out.blank();
  }

  // Main harness
  if (opt.main_mode != MainMode::None) {
    out.line("int main(int argc, char** argv) {");
    out.indent++;
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
        out.line("(void)" +
                 emitted_cpp_name_for_identity(function_symbols,
                                               nebula::nir::function_identity(*entry_main),
                                               entry_main->name) +
                 "();");
      } else {
        out.line("(void)0;");
      }
      out.line("return 0;");
    } else if (opt.main_mode == MainMode::RunTests) {
      // Call all @test functions.
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "test")) {
          out.line("std::cerr << \"[test] " + fn->name + "\\n\";");
          out.line(emitted_cpp_name_for_identity(function_symbols,
                                                 nebula::nir::function_identity(*fn),
                                                 fn->name) +
                   "();");
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
          out.line(emitted_cpp_name_for_identity(function_symbols,
                                                 nebula::nir::function_identity(*fn),
                                                 fn->name) +
                   "();");
        }
      }
      out.indent--;
      out.line("}");
      out.line("for (int i = 0; i < iters; ++i) {");
      out.indent++;
      out.line("auto t0 = Clock::now();");
      for (const auto* fn : ordered) {
        if (has_annotation(fn->annotations, "bench")) {
          out.line(emitted_cpp_name_for_identity(function_symbols,
                                                 nebula::nir::function_identity(*fn),
                                                 fn->name) +
                   "();");
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

    out.indent--;
    out.line("}");
  }

  return out.os.str();
}

} // namespace nebula::codegen
