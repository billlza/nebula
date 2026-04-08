#include "passes/call_target_resolver.hpp"

#include "nir/cfg.hpp"

#include <deque>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace nebula::passes {

namespace {

using nebula::frontend::Ty;
using nebula::nir::CallKind;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Stmt;
using nebula::nir::VarId;

using Env = std::unordered_map<VarId, ResolvedTargetState>;

static bool same_state(const ResolvedTargetState& lhs, const ResolvedTargetState& rhs) {
  if (lhs.kind != rhs.kind) return false;
  if (lhs.kind == ResolvedTargetState::Kind::Known) return lhs.callee == rhs.callee;
  return true;
}

static ResolvedTargetState join_state(const ResolvedTargetState& lhs,
                                      const ResolvedTargetState& rhs) {
  if (!lhs.is_known() || !rhs.is_known()) return ResolvedTargetState::unknown();
  if (lhs.callee != rhs.callee) return ResolvedTargetState::unknown();
  return ResolvedTargetState::known(lhs.callee);
}

static ResolvedTargetState env_get(const Env& env, VarId var) {
  if (var == 0) return ResolvedTargetState::unknown();
  auto it = env.find(var);
  if (it == env.end()) return ResolvedTargetState::unknown();
  return it->second;
}

static void record_call_resolution(FunctionCallTargetResolution& out,
                                   const Expr::Call* call,
                                   const ResolvedTargetState& observed) {
  if (call == nullptr) return;
  auto it = out.by_call.find(call);
  if (it == out.by_call.end()) {
    out.by_call.insert({call, observed});
    return;
  }
  it->second = join_state(it->second, observed);
}

static bool join_env(Env& dst, const Env& src) {
  bool changed = false;
  std::unordered_set<VarId> keys;
  keys.reserve(dst.size() + src.size());
  for (const auto& [var, _] : dst) keys.insert(var);
  for (const auto& [var, _] : src) keys.insert(var);

  for (VarId var : keys) {
    const ResolvedTargetState old_state = env_get(dst, var);
    const ResolvedTargetState merged = join_state(old_state, env_get(src, var));
    if (same_state(old_state, merged)) continue;
    dst.insert_or_assign(var, merged);
    changed = true;
  }
  return changed;
}

static void analyze_expr(const Expr& e,
                         const Env& env,
                         const std::unordered_set<std::string>& known_functions,
                         FunctionCallTargetResolution& out);

static ResolvedTargetState callable_state_from_expr(const Expr& e,
                                                    const Env& env,
                                                    const std::unordered_set<std::string>& known_functions) {
  return std::visit(
      [&](auto&& n) -> ResolvedTargetState {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::VarRef>) {
          if (n.var != 0) return env_get(env, n.var);
          const std::string target = nebula::nir::varref_target_identity(n);
          if (known_functions.find(target) != known_functions.end()) {
            return ResolvedTargetState::known(target);
          }
          return ResolvedTargetState::unknown();
        } else {
          return ResolvedTargetState::unknown();
        }
      },
      e.node);
}

static void analyze_expr(const Expr& e,
                         const Env& env,
                         const std::unordered_set<std::string>& known_functions,
                         FunctionCallTargetResolution& out) {
  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          for (const auto& arg : n.args) {
            if (arg) analyze_expr(*arg, env, known_functions, out);
          }
          if (n.kind == CallKind::Indirect) {
            record_call_resolution(out, &n, env_get(env, n.callee_var));
          }
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& arg : n.args) {
            if (arg) analyze_expr(*arg, env, known_functions, out);
          }
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          if (n.lhs) analyze_expr(*n.lhs, env, known_functions, out);
          if (n.rhs) analyze_expr(*n.rhs, env, known_functions, out);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          if (n.inner) analyze_expr(*n.inner, env, known_functions, out);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          if (n.inner) analyze_expr(*n.inner, env, known_functions, out);
        } else if constexpr (std::is_same_v<N, Expr::VarRef> ||
                             std::is_same_v<N, Expr::FieldRef> ||
                             std::is_same_v<N, Expr::IntLit> ||
                             std::is_same_v<N, Expr::BoolLit> ||
                             std::is_same_v<N, Expr::FloatLit> ||
                             std::is_same_v<N, Expr::StringLit>) {
          (void)known_functions;
          (void)out;
        }
      },
      e.node);
}

static void transfer_stmt(const Stmt& s,
                          Env& env,
                          const std::unordered_set<std::string>& known_functions,
                          FunctionCallTargetResolution& out) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Let>) {
          analyze_expr(*st.value, env, known_functions, out);
          if (st.ty.kind == Ty::Kind::Callable) {
            env.insert_or_assign(st.var, callable_state_from_expr(*st.value, env, known_functions));
          }
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          analyze_expr(*st.value, env, known_functions, out);
          if (st.ty.kind == Ty::Kind::Callable) {
            env.insert_or_assign(st.var, ResolvedTargetState::unknown());
          }
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          analyze_expr(*st.value, env, known_functions, out);
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          analyze_expr(*st.expr, env, known_functions, out);
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          analyze_expr(*st.value, env, known_functions, out);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          analyze_expr(*st.cond, env, known_functions, out);
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          analyze_expr(*st.start, env, known_functions, out);
          analyze_expr(*st.end, env, known_functions, out);
        } else if constexpr (std::is_same_v<S, Stmt::Region> || std::is_same_v<S, Stmt::Unsafe>) {
          // No explicit CFG statement node for these wrappers.
        }
      },
      s.node);
}

static FunctionCallTargetResolution resolve_function(
    const Function& fn, const std::unordered_set<std::string>& known_functions) {
  FunctionCallTargetResolution out;
  auto cfg = nebula::nir::build_cfg(fn);
  if (cfg.nodes.empty()) return out;

  std::vector<Env> in(cfg.nodes.size());
  std::vector<char> inited(cfg.nodes.size(), 0);

  Env entry_env;
  for (const auto& param : fn.params) {
    if (param.ty.kind == Ty::Kind::Callable) {
      entry_env.insert({param.var, ResolvedTargetState::unknown()});
    }
  }
  in[cfg.entry] = std::move(entry_env);
  inited[cfg.entry] = 1;

  std::deque<nebula::nir::NodeId> work;
  work.push_back(cfg.entry);

  while (!work.empty()) {
    const nebula::nir::NodeId id = work.front();
    work.pop_front();

    Env env_out = in[id];
    const auto& node = cfg.nodes[id];
    if (node.kind == nebula::nir::NodeKind::Stmt && node.stmt != nullptr) {
      transfer_stmt(*node.stmt, env_out, known_functions, out);
    }

    for (nebula::nir::NodeId succ : node.succ) {
      if (!inited[succ]) {
        in[succ] = env_out;
        inited[succ] = 1;
        work.push_back(succ);
        continue;
      }
      if (join_env(in[succ], env_out)) {
        work.push_back(succ);
      }
    }
  }

  return out;
}

} // namespace

CallTargetResolution run_call_target_resolver(const nebula::nir::Program& p) {
  std::unordered_set<std::string> known_functions;
  known_functions.reserve(p.items.size() + 1);
  known_functions.insert("expect_eq");
  known_functions.insert("print");
  known_functions.insert("panic");
  known_functions.insert("assert");
  known_functions.insert("argc");
  known_functions.insert("argv");
  known_functions.insert("args_count");
  known_functions.insert("args_get");
  for (const auto& item : p.items) {
    if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(item.node);
    known_functions.insert(nebula::nir::function_identity(fn));
  }

  CallTargetResolution out;
  for (const auto& item : p.items) {
    if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(item.node);
    out.by_function.insert({nebula::nir::function_identity(fn), resolve_function(fn, known_functions)});
  }
  return out;
}

} // namespace nebula::passes
