#include "nir/lower.hpp"

#include <unordered_map>
#include <utility>

namespace nebula::nir {

namespace {

struct Ctx {
  VarId next_var = 1;
  std::vector<std::unordered_map<std::string, VarId>> scopes;
  std::unordered_map<nebula::frontend::LocalBindingId, VarId> local_bindings;

  void push() { scopes.push_back({}); }
  void pop() {
    if (!scopes.empty()) scopes.pop_back();
  }

  VarId declare(const std::string& name,
                nebula::frontend::LocalBindingId binding_id = nebula::frontend::kInvalidLocalBindingId) {
    if (scopes.empty()) push();
    VarId id = next_var++;
    scopes.back().insert({name, id});
    if (binding_id != nebula::frontend::kInvalidLocalBindingId) local_bindings.insert_or_assign(binding_id, id);
    return id;
  }

  VarId lookup(const std::string& name,
               nebula::frontend::LocalBindingId binding_id = nebula::frontend::kInvalidLocalBindingId) const {
    if (binding_id != nebula::frontend::kInvalidLocalBindingId) {
      auto bit = local_bindings.find(binding_id);
      if (bit != local_bindings.end()) return bit->second;
    }
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

static nebula::frontend::QualifiedName qualified_name(std::string package_name,
                                                      std::string module_name,
                                                      std::string local_name) {
  nebula::frontend::QualifiedName name;
  name.package_name = std::move(package_name);
  name.module_name = std::move(module_name);
  name.local_name = std::move(local_name);
  return name;
}

static nebula::frontend::QualifiedName std_json_name(std::string local_name) {
  return qualified_name("std", "json", std::move(local_name));
}

static nebula::frontend::QualifiedName nebula_ui_name(std::string local_name) {
  return qualified_name("nebula-ui", "ui", std::move(local_name));
}

static Ty json_ty() { return Ty::Struct("Json", {}, std_json_name("Json")); }

static Ty ui_view_ty() { return Ty::Struct("View", {}, nebula_ui_name("View")); }

static ExprPtr string_lit(std::string value, Span span) {
  auto out = std::make_unique<Expr>();
  out->span = span;
  out->ty = Ty::String();
  out->node = Expr::StringLit{std::move(value)};
  return out;
}

static ExprPtr call_expr(std::string callee,
                         nebula::frontend::QualifiedName resolved,
                         std::vector<ExprPtr> args,
                         Ty ret,
                         Span span) {
  auto out = std::make_unique<Expr>();
  out->span = span;
  out->ty = std::move(ret);
  Expr::Call call;
  call.callee = std::move(callee);
  call.resolved_callee = std::move(resolved);
  call.args = std::move(args);
  call.args_ref.assign(call.args.size(), false);
  out->node = std::move(call);
  return out;
}

static ExprPtr json_call(std::string callee, std::vector<ExprPtr> args, Span span) {
  auto resolved = std_json_name(callee);
  return call_expr(std::move(callee), std::move(resolved), std::move(args), json_ty(), span);
}

static ExprPtr ui_view_call(std::string callee, std::vector<ExprPtr> args, Span span) {
  auto resolved = nebula_ui_name(callee);
  return call_expr(std::move(callee), std::move(resolved), std::move(args), ui_view_ty(), span);
}

static ExprPtr ui_json_call(std::string callee, std::vector<ExprPtr> args, Span span) {
  auto resolved = nebula_ui_name(callee);
  return call_expr(std::move(callee), std::move(resolved), std::move(args), json_ty(), span);
}

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
          const VarId id = ctx.lookup(n.name, n.binding_id);
          out->node = Expr::VarRef{id, n.name, n.top_level_symbol};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Call>) {
          Expr::Call c;
          c.callee = n.callee;
          c.resolved_callee = n.resolved_callee;
          c.args = lower_args(n.args, ctx);
          c.args_ref = n.args_ref;
          c.kind = lower_call_kind(n.kind);
          c.callee_var =
              (c.kind == CallKind::Indirect) ? ctx.lookup(n.callee, n.callee_binding_id) : 0;
          out->node = std::move(c);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::FieldRef>) {
          const VarId id = ctx.lookup(n.base, n.base_binding_id);
          out->node = Expr::FieldRef{id, n.base, n.field};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::TempFieldRef>) {
          Expr::TempFieldRef field;
          field.base = lower_expr(*n.base, ctx);
          field.field = n.field;
          out->node = std::move(field);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::EnumIsVariant>) {
          Expr::EnumIsVariant test;
          test.subject = lower_expr(*n.subject, ctx);
          test.enum_name = n.enum_name;
          test.variant_name = n.variant_name;
          test.variant_index = n.variant_index;
          out->node = std::move(test);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::EnumPayload>) {
          Expr::EnumPayload payload;
          payload.subject = lower_expr(*n.subject, ctx);
          payload.enum_name = n.enum_name;
          payload.variant_name = n.variant_name;
          payload.variant_index = n.variant_index;
          out->node = std::move(payload);
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
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Await>) {
          Expr::Await a;
          a.inner = lower_expr(*n.inner, ctx);
          out->node = std::move(a);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TExpr::Match>) {
          Expr::Match match;
          match.subject = lower_expr(*n.subject, ctx);
          match.arms.reserve(n.arms.size());
          match.exhaustive = n.exhaustive;
          for (const auto& arm_ptr : n.arms) {
            const auto& arm = *arm_ptr;
            auto lowered_arm = std::make_unique<MatchExprArm>();
            lowered_arm->kind = static_cast<decltype(lowered_arm->kind)>(arm.kind);
            lowered_arm->span = arm.span;
            lowered_arm->bool_value = arm.bool_value;
            lowered_arm->variant_name = arm.variant_name;
            lowered_arm->variant_index = arm.variant_index;
            lowered_arm->payload_ty = arm.payload_ty;
            ctx.push();
            if (arm.payload_binding.has_value()) {
              Expr::Match::Binding binding;
              binding.var = ctx.declare(arm.payload_binding->name, arm.payload_binding->binding_id);
              binding.name = arm.payload_binding->name;
              binding.ty = arm.payload_binding->ty;
              binding.span = arm.payload_binding->span;
              lowered_arm->payload_binding = std::move(binding);
            }
            lowered_arm->payload_struct_bindings.reserve(arm.payload_struct_bindings.size());
            for (const auto& plan : arm.payload_struct_bindings) {
              Expr::Match::StructBinding binding;
              binding.field_name = plan.field_name;
              binding.binding.var = ctx.declare(plan.binding.name, plan.binding.binding_id);
              binding.binding.name = plan.binding.name;
              binding.binding.ty = plan.binding.ty;
              binding.binding.span = plan.binding.span;
              binding.field_span = plan.field_span;
              lowered_arm->payload_struct_bindings.push_back(std::move(binding));
            }
            lowered_arm->value = lower_expr(*arm.value, ctx);
            ctx.pop();
            match.arms.push_back(std::move(lowered_arm));
          }
          out->node = std::move(match);
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
        if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Declare>) {
          const VarId id = ctx.declare(n.name, n.binding_id);
          Stmt::Declare st;
          st.var = id;
          st.name = n.name;
          st.ty = n.ty;
          out.node = std::move(st);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Let>) {
          // Lower RHS before introducing the new binding.
          auto rhs = lower_expr(*n.value, ctx);
          const VarId id = ctx.declare(n.name, n.binding_id);
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
          st.var = ctx.lookup(n.name, n.binding_id);
          st.name = n.name;
          st.ty = n.ty;
          st.value = lower_expr(*n.value, ctx);
          out.node = std::move(st);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::AssignField>) {
          Stmt::AssignField st;
          st.base_var = ctx.lookup(n.base, n.base_binding_id);
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
          const VarId id = ctx.declare(n.var, n.binding_id);
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
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::While>) {
          Stmt::While while_stmt;
          while_stmt.cond = lower_expr(*n.cond, ctx);
          while_stmt.body = lower_block(n.body, ctx);
          out.node = std::move(while_stmt);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Break>) {
          out.node = Stmt::Break{};
        } else if constexpr (std::is_same_v<N, nebula::frontend::TStmt::Continue>) {
          out.node = Stmt::Continue{};
        }
      },
      s.node);

  return out;
}

static bool is_json_ty(const Ty& ty) {
  return ty.kind == Ty::Kind::Struct && ty.qualified_name.has_value() &&
         ty.qualified_name->package_name == "std" && ty.qualified_name->module_name == "json" &&
         ty.qualified_name->local_name == "Json";
}

static ExprPtr lower_ui_value_as_json(const nebula::frontend::TExpr& value, Ctx& ctx) {
  auto lowered = lower_expr(value, ctx);
  const Span span = lowered->span;
  if (is_json_ty(lowered->ty)) return lowered;
  if (lowered->ty.kind == Ty::Kind::String) {
    std::vector<ExprPtr> args;
    args.push_back(std::move(lowered));
    return json_call("string_value", std::move(args), span);
  }
  if (lowered->ty.kind == Ty::Kind::Int) {
    std::vector<ExprPtr> args;
    args.push_back(std::move(lowered));
    return json_call("int_value", std::move(args), span);
  }
  if (lowered->ty.kind == Ty::Kind::Bool) {
    std::vector<ExprPtr> args;
    args.push_back(std::move(lowered));
    return json_call("bool_value", std::move(args), span);
  }
  return json_call("null_value", {}, span);
}

static ExprPtr lower_ui_props(const std::vector<nebula::frontend::TUiProp>& props, Ctx& ctx, Span span) {
  if (props.empty()) return json_call("null_value", {}, span);
  std::vector<ExprPtr> args;
  for (const auto& prop : props) {
    args.push_back(string_lit(prop.name, prop.span));
    args.push_back(lower_ui_value_as_json(*prop.value, ctx));
  }
  const std::string callee = "object" + std::to_string(props.size());
  return json_call(callee, std::move(args), span);
}

static ExprPtr lower_ui_node(const nebula::frontend::TUiNode& node, Ctx& ctx);

static ExprPtr lower_ui_children(const std::vector<nebula::frontend::TUiNode>& children,
                                 Ctx& ctx,
                                 Span span) {
  if (children.empty()) return ui_json_call("empty_children", {}, span);
  std::vector<ExprPtr> args;
  for (const auto& child : children) args.push_back(lower_ui_node(child, ctx));
  const std::string callee = "child" + std::to_string(children.size());
  return ui_json_call(callee, std::move(args), span);
}

static ExprPtr lower_ui_view_node(std::string component,
                                  const std::vector<nebula::frontend::TUiProp>& props,
                                  const std::vector<nebula::frontend::TUiNode>& children,
                                  Span span,
                                  Ctx& ctx) {
  std::vector<ExprPtr> args;
  args.push_back(string_lit(std::move(component), span));
  args.push_back(lower_ui_props(props, ctx, span));
  args.push_back(lower_ui_children(children, ctx, span));
  return ui_view_call("view_node", std::move(args), span);
}

static ExprPtr lower_ui_node(const nebula::frontend::TUiNode& node, Ctx& ctx) {
  return std::visit(
      [&](auto&& n) -> ExprPtr {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, nebula::frontend::TUiNode::View>) {
          return lower_ui_view_node(n.component, n.props, n.children, node.span, ctx);
        } else if constexpr (std::is_same_v<N, nebula::frontend::TUiNode::If>) {
          std::vector<ExprPtr> prop_args;
          prop_args.push_back(string_lit("condition", n.cond->span));
          prop_args.push_back(lower_ui_value_as_json(*n.cond, ctx));
          std::vector<ExprPtr> args;
          args.push_back(string_lit("If", node.span));
          args.push_back(json_call("object1", std::move(prop_args), node.span));
          args.push_back(lower_ui_children(n.then_children, ctx, node.span));
          return ui_view_call("view_node", std::move(args), node.span);
        } else {
          std::vector<nebula::frontend::TUiProp> props;
          nebula::frontend::TUiProp var;
          var.span = node.span;
          var.name = "var";
          auto var_expr = std::make_unique<nebula::frontend::TExpr>();
          var_expr->span = node.span;
          var_expr->ty = Ty::String();
          var_expr->node = nebula::frontend::TExpr::StringLit{n.var};
          var.value = std::move(var_expr);
          props.push_back(std::move(var));
          return lower_ui_view_node("For", props, n.body, node.span, ctx);
        }
      },
      node.node);
}

static ExprPtr lower_ui_root_view(const std::vector<nebula::frontend::TUiNode>& body,
                                  Ctx& ctx,
                                  Span span) {
  if (body.size() == 1) return lower_ui_node(body.front(), ctx);
  return lower_ui_view_node("Fragment", {}, body, span, ctx);
}

static Function lower_ui_function(const nebula::frontend::TUi& n) {
  Ctx ctx;
  ctx.push();
  Function f;
  f.span = n.span;
  f.annotations = n.annotations;
  f.name = n.name;
  f.qualified_name = n.qualified_name;
  f.ret = json_ty();
  for (const auto& p : n.params) {
    Param np;
    np.span = p.span;
    np.var = ctx.declare(p.name, p.binding_id);
    np.is_ref = p.is_ref;
    np.name = p.name;
    np.ty = p.ty;
    f.params.push_back(std::move(np));
  }
  Block body;
  body.span = n.span;
  Stmt ret;
  ret.span = n.span;
  std::vector<ExprPtr> args;
  args.push_back(lower_ui_root_view(n.body, ctx, n.span));
  ret.node = Stmt::Return{ui_json_call("View_as_json", std::move(args), n.span)};
  body.stmts.push_back(std::move(ret));
  f.body = std::move(body);
  return f;
}

static void append_lowered_program_items(Program& out, const nebula::frontend::TProgram& t) {
  for (const auto& it : t.items) {
    Item item;
    item.span = it.span;
    bool lowered = true;

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
            f.type_params = n.type_params;
            f.ret = n.ret;
            f.is_async = n.is_async;
            f.is_extern = n.is_extern;
            // Params are in scope for the function body.
            for (const auto& p : n.params) {
              Param np;
              np.span = p.span;
              np.var = ctx.declare(p.name, p.binding_id);
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
            sdef.type_params = n.type_params;
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
            edef.type_params = n.type_params;
            for (const auto& v : n.variants) {
              Variant nv;
              nv.span = v.span;
              nv.name = v.name;
              nv.payload = v.payload;
              edef.variants.push_back(std::move(nv));
            }
            item.node = std::move(edef);
          } else if constexpr (std::is_same_v<N, nebula::frontend::TUi>) {
            item.node = lower_ui_function(n);
          }
        },
        it.node);

    if (lowered) out.items.push_back(std::move(item));
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
