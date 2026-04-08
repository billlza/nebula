#include "nir/lower.hpp"

#include <unordered_map>
#include <utility>

namespace nebula::nir {

namespace {

struct Ctx {
  VarId next_var = 1;
  std::vector<std::unordered_map<std::string, VarId>> scopes;

  void push() { scopes.push_back({}); }
  void pop() {
    if (!scopes.empty()) scopes.pop_back();
  }

  VarId declare(const std::string& name) {
    if (scopes.empty()) push();
    VarId id = next_var++;
    scopes.back().insert({name, id});
    return id;
  }

  VarId lookup(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto jt = it->find(name);
      if (jt != it->end()) return jt->second;
    }
    return 0;
  }
};

} // namespace

static PrefixKind lower_prefix(nebula::frontend::Expr::PrefixKind k) {
  switch (k) {
  case nebula::frontend::Expr::PrefixKind::Shared: return PrefixKind::Shared;
  case nebula::frontend::Expr::PrefixKind::Unique: return PrefixKind::Unique;
  case nebula::frontend::Expr::PrefixKind::Heap: return PrefixKind::Heap;
  case nebula::frontend::Expr::PrefixKind::Promote: return PrefixKind::Promote;
  }
  return PrefixKind::Heap;
}

static BinOp lower_binop(nebula::frontend::Expr::BinOp op) {
  switch (op) {
  case nebula::frontend::Expr::BinOp::Add: return BinOp::Add;
  case nebula::frontend::Expr::BinOp::Sub: return BinOp::Sub;
  case nebula::frontend::Expr::BinOp::Mul: return BinOp::Mul;
  case nebula::frontend::Expr::BinOp::Div: return BinOp::Div;
  case nebula::frontend::Expr::BinOp::Mod: return BinOp::Mod;
  case nebula::frontend::Expr::BinOp::Eq: return BinOp::Eq;
  case nebula::frontend::Expr::BinOp::Ne: return BinOp::Ne;
  case nebula::frontend::Expr::BinOp::Lt: return BinOp::Lt;
  case nebula::frontend::Expr::BinOp::Lte: return BinOp::Lte;
  case nebula::frontend::Expr::BinOp::Gt: return BinOp::Gt;
  case nebula::frontend::Expr::BinOp::Gte: return BinOp::Gte;
  case nebula::frontend::Expr::BinOp::And: return BinOp::And;
  case nebula::frontend::Expr::BinOp::Or: return BinOp::Or;
  }
  return BinOp::Add;
}

static UnaryOp lower_unary_op(nebula::frontend::Expr::UnaryOp op) {
  switch (op) {
  case nebula::frontend::Expr::UnaryOp::Not: return UnaryOp::Not;
  }
  return UnaryOp::Not;
}

static CallKind lower_call_kind(nebula::frontend::TExpr::CallKind kind) {
  switch (kind) {
  case nebula::frontend::TExpr::CallKind::Direct: return CallKind::Direct;
  case nebula::frontend::TExpr::CallKind::Indirect: return CallKind::Indirect;
  }
  return CallKind::Direct;
}

static ExprPtr lower_expr(const nebula::frontend::TExpr& e, Ctx& ctx);
static void append_lowered_program_items(Program& out, const nebula::frontend::TProgram& t);

static std::vector<ExprPtr> lower_args(const std::vector<nebula::frontend::TExprPtr>& xs, Ctx& ctx) {
  std::vector<ExprPtr> out;
  out.reserve(xs.size());
  for (const auto& a : xs) out.push_back(lower_expr(*a, ctx));
  return out;
}

static ExprPtr lower_expr(const nebula::frontend::TExpr& e, Ctx& ctx) {
  auto out = std::make_unique<Expr>();
  out->span = e.span;
  out->ty = e.ty;

  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, nebula::frontend::TExpr::IntLit>) {
          out->node = Expr::IntLit{n.value};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::BoolLit>) {
          out->node = Expr::BoolLit{n.value};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::FloatLit>) {
          out->node = Expr::FloatLit{n.value};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::StringLit>) {
          out->node = Expr::StringLit{n.value};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::VarRef>) {
          const VarId id = ctx.lookup(n.name);
          out->node = Expr::VarRef{id, n.name, n.top_level_symbol};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Call>) {
          Expr::Call c;
          c.callee = n.callee;
          c.resolved_callee = n.resolved_callee;
          c.args = lower_args(n.args, ctx);
          c.args_ref = n.args_ref;
          c.kind = lower_call_kind(n.kind);
          c.callee_var = (c.kind == CallKind::Indirect) ? ctx.lookup(n.callee) : 0;
          out->node = std::move(c);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::FieldRef>) {
          const VarId id = ctx.lookup(n.base);
          out->node = Expr::FieldRef{id, n.base, n.field};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Construct>) {
          Expr::Construct c;
          c.type_name = n.type_name;
          c.resolved_type = n.resolved_type;
          c.variant_name = n.variant_name;
          c.variant_index = n.variant_index;
          c.args = lower_args(n.args, ctx);
          out->node = std::move(c);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Binary>) {
          Expr::Binary b;
          b.op = lower_binop(n.op);
          b.lhs = lower_expr(*n.lhs, ctx);
          b.rhs = lower_expr(*n.rhs, ctx);
          out->node = std::move(b);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Unary>) {
          Expr::Unary u;
          u.op = lower_unary_op(static_cast<nebula::frontend::Expr::UnaryOp>(n.op));
          u.inner = lower_expr(*n.inner, ctx);
          out->node = std::move(u);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Prefix>) {
          Expr::Prefix p;
          p.kind = lower_prefix(n.kind);
          p.inner = lower_expr(*n.inner, ctx);
          out->node = std::move(p);
        }
      },
      e.node);

  return out;
}

static Stmt lower_stmt(const nebula::frontend::TStmt& s, Ctx& ctx);
static Block lower_block(const nebula::frontend::TBlock& b, Ctx& ctx);

static Block lower_block(const nebula::frontend::TBlock& b, Ctx& ctx) {
  Block out;
  out.span = b.span;
  out.stmts.reserve(b.stmts.size());
  ctx.push();
  for (const auto& s : b.stmts) out.stmts.push_back(lower_stmt(s, ctx));
  ctx.pop();
  return out;
}

static Stmt lower_stmt(const nebula::frontend::TStmt& s, Ctx& ctx) {
  Stmt out;
  out.span = s.span;
  out.annotations = s.annotations;

  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Let>) {
          // Lower RHS before introducing the new binding.
          auto rhs = lower_expr(*n.value, ctx);
          const VarId id = ctx.declare(n.name);
          Stmt::Let st;
          st.var = id;
          st.name = n.name;
          st.ty = n.ty;
          st.value = std::move(rhs);
          out.node = std::move(st);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Return>) {
          out.node = Stmt::Return{lower_expr(*n.value, ctx)};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::ExprStmt>) {
          out.node = Stmt::ExprStmt{lower_expr(*n.expr, ctx)};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::AssignVar>) {
          Stmt::AssignVar st;
          st.var = ctx.lookup(n.name);
          st.name = n.name;
          st.ty = n.ty;
          st.value = lower_expr(*n.value, ctx);
          out.node = std::move(st);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::AssignField>) {
          Stmt::AssignField st;
          st.base_var = ctx.lookup(n.base);
          st.base_name = n.base;
          st.field = n.field;
          st.ty = n.ty;
          st.value = lower_expr(*n.value, ctx);
          out.node = std::move(st);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Region>) {
          Stmt::Region r;
          r.name = n.name;
          r.body = lower_block(n.body, ctx);
          out.node = std::move(r);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Unsafe>) {
          Stmt::Unsafe u;
          u.body = lower_block(n.body, ctx);
          out.node = std::move(u);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::If>) {
          Stmt::If if_stmt;
          if_stmt.cond = lower_expr(*n.cond, ctx);
          if_stmt.then_body = lower_block(n.then_body, ctx);
          if (n.else_body.has_value()) {
            if_stmt.else_body = lower_block(*n.else_body, ctx);
          }
          out.node = std::move(if_stmt);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::For>) {
          auto start = lower_expr(*n.start, ctx);
          auto end = lower_expr(*n.end, ctx);
          ctx.push();
          const VarId id = ctx.declare(n.var);
          Block body = lower_block(n.body, ctx);
          ctx.pop();
          Stmt::For f;
          f.var = id;
          f.var_name = n.var;
          f.var_ty = n.var_ty;
          f.start = std::move(start);
          f.end = std::move(end);
          f.body = std::move(body);
          out.node = std::move(f);
        }
      },
      s.node);

  return out;
}

static void append_lowered_program_items(Program& out, const nebula::frontend::TProgram& t) {
  for (const auto& it : t.items) {
    Item item;
    item.span = it.span;

    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, nebula::frontend::TFunction>) {
            Ctx ctx;
            ctx.push();
            Function f;
            f.span = n.span;
            f.annotations = n.annotations;
            f.name = n.name;
            f.qualified_name = n.qualified_name;
            f.ret = n.ret;
            f.is_extern = n.is_extern;
            // Params are in scope for the function body.
            for (const auto& p : n.params) {
              Param np;
              np.span = p.span;
              np.var = ctx.declare(p.name);
              np.is_ref = p.is_ref;
              np.name = p.name;
              np.ty = p.ty;
              f.params.push_back(std::move(np));
            }
            if (!n.is_extern && n.body.has_value()) {
              f.body = lower_block(*n.body, ctx);
            }
            item.node = std::move(f);
          } else if constexpr (std::is_same_v<N, nebula::frontend::TStruct>) {
            StructDef sdef;
            sdef.span = n.span;
            sdef.annotations = n.annotations;
            sdef.name = n.name;
            sdef.qualified_name = n.qualified_name;
            for (const auto& f : n.fields) {
              Field nf;
              nf.span = f.span;
              nf.name = f.name;
              nf.ty = f.ty;
              sdef.fields.push_back(std::move(nf));
            }
            item.node = std::move(sdef);
          } else if constexpr (std::is_same_v<N, nebula::frontend::TEnum>) {
            EnumDef edef;
            edef.span = n.span;
            edef.annotations = n.annotations;
            edef.name = n.name;
            edef.qualified_name = n.qualified_name;
            edef.type_param = n.type_param;
            for (const auto& v : n.variants) {
              Variant nv;
              nv.span = v.span;
              nv.name = v.name;
              nv.payload = v.payload;
              edef.variants.push_back(std::move(nv));
            }
            item.node = std::move(edef);
          }
        },
        it.node);

    out.items.push_back(std::move(item));
  }
}

Program lower_to_nir(const nebula::frontend::TProgram& t) {
  Program out;
  out.package_name = t.package_name;
  out.module_name = t.module_name;
  out.imports = t.imports;
  out.items.reserve(t.items.size());
  append_lowered_program_items(out, t);
  return out;
}

Program lower_to_nir(const std::vector<nebula::frontend::TProgram>& programs) {
  Program out;
  if (!programs.empty()) {
    out.package_name = programs.front().package_name;
    out.module_name = programs.front().module_name;
    out.imports = programs.front().imports;
  }

  std::size_t total_items = 0;
  for (const auto& program : programs) total_items += program.items.size();
  out.items.reserve(total_items);
  for (const auto& program : programs) append_lowered_program_items(out, program);

  return out;
}

} // namespace nebula::nir
