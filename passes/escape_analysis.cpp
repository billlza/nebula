#include "passes/escape_analysis.hpp"

#include "frontend/external_contract.hpp"
#include "nir/cfg.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nebula::passes {

namespace {

using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Stmt;
using nebula::nir::VarId;

enum class EscapeState : std::uint8_t { KnownNoEscape = 0, KnownMayEscape = 1, Unknown = 2 };

static EscapeState join_escape(EscapeState lhs, EscapeState rhs) {
  return static_cast<EscapeState>(
      std::max(static_cast<std::uint8_t>(lhs), static_cast<std::uint8_t>(rhs)));
}

struct DepSet {
  std::vector<char> bits;

  explicit DepSet(std::size_t n) : bits(n, 0) {}

  bool set(std::size_t i) {
    if (i >= bits.size()) return false;
    const char old = bits[i];
    bits[i] = 1;
    return old == 0;
  }

  bool join(const DepSet& other) {
    bool changed = false;
    const std::size_t n = std::min(bits.size(), other.bits.size());
    for (std::size_t i = 0; i < n; ++i) {
      if (!bits[i] && other.bits[i]) {
        bits[i] = 1;
        changed = true;
      }
    }
    return changed;
  }
};

struct InternalFuncSummary {
  std::vector<char> return_depends_on_param;
  std::vector<EscapeState> param_escape;
};

using Env = std::unordered_map<VarId, DepSet>;

struct FunctionContext {
  const Function* fn = nullptr;
  const FunctionCallTargetResolution* resolution = nullptr;
};

struct CallGraph {
  std::vector<std::string> names;
  std::vector<FunctionContext> funcs;
  std::unordered_map<std::string, std::size_t> index_by_name;
  std::vector<std::vector<std::size_t>> edges;
  std::vector<bool> self_edge;
};

struct SccResult {
  std::vector<std::vector<std::size_t>> components;
  std::vector<std::size_t> component_of;
  std::vector<std::vector<std::size_t>> dag_edges;
  std::vector<std::size_t> topo_order;
};

static bool join_env(Env& dst, const Env& src) {
  bool changed = false;
  for (const auto& [var, dep] : src) {
    auto it = dst.find(var);
    if (it == dst.end()) {
      dst.insert({var, dep});
      changed = true;
    } else {
      changed = it->second.join(dep) || changed;
    }
  }
  return changed;
}

static DepSet dep_none(std::size_t n) { return DepSet(n); }

static DepSet dep_from_param(std::size_t nparams, std::size_t param_index) {
  DepSet d(nparams);
  d.set(param_index);
  return d;
}

static void mark_state_from_dep(const DepSet& d,
                                std::vector<EscapeState>& escaped_params,
                                EscapeState state) {
  const std::size_t n = std::min(d.bits.size(), escaped_params.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (!d.bits[i]) continue;
    escaped_params[i] = join_escape(escaped_params[i], state);
  }
}

static EscapeState summary_param_state(const InternalFuncSummary& s, std::size_t index) {
  if (index >= s.param_escape.size()) return EscapeState::Unknown;
  return s.param_escape[index];
}

static std::optional<std::string> resolve_call_target(const Expr::Call& call,
                                                      const FunctionCallTargetResolution& resolution) {
  if (call.kind == nebula::nir::CallKind::Direct) return nebula::nir::call_target_identity(call);
  const auto resolved = resolution.resolve(&call);
  if (!resolved.is_known()) return std::nullopt;
  return resolved.callee;
}

static DepSet eval_expr(const Expr& e,
                        const Env& env,
                        const std::unordered_map<std::string, InternalFuncSummary>& summaries,
                        const FunctionCallTargetResolution& call_resolution,
                        std::vector<EscapeState>& escaped_params,
                        std::size_t nparams);

static DepSet eval_expr(const Expr& e,
                        const Env& env,
                        const std::unordered_map<std::string, InternalFuncSummary>& summaries,
                        const FunctionCallTargetResolution& call_resolution,
                        std::vector<EscapeState>& escaped_params,
                        std::size_t nparams) {
  return std::visit(
      [&](auto&& n) -> DepSet {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::IntLit> || std::is_same_v<N, Expr::BoolLit> ||
                      std::is_same_v<N, Expr::FloatLit> || std::is_same_v<N, Expr::StringLit>) {
          return dep_none(nparams);
        } else if constexpr (std::is_same_v<N, Expr::VarRef>) {
          if (n.var == 0) return dep_none(nparams);
          auto it = env.find(n.var);
          if (it == env.end()) return dep_none(nparams);
          return it->second;
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          if (n.base_var == 0) return dep_none(nparams);
          auto it = env.find(n.base_var);
          if (it == env.end()) return dep_none(nparams);
          return it->second;
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          return eval_expr(*n.base, env, summaries, call_resolution, escaped_params, nparams);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
          return eval_expr(*n.subject, env, summaries, call_resolution, escaped_params, nparams);
        } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
          return eval_expr(*n.subject, env, summaries, call_resolution, escaped_params, nparams);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          DepSet out = dep_none(nparams);
          for (const auto& a : n.args) {
            out.join(eval_expr(*a, env, summaries, call_resolution, escaped_params, nparams));
          }
          return out;
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          DepSet out = dep_none(nparams);
          out.join(eval_expr(*n.lhs, env, summaries, call_resolution, escaped_params, nparams));
          out.join(eval_expr(*n.rhs, env, summaries, call_resolution, escaped_params, nparams));
          return out;
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          return eval_expr(*n.inner, env, summaries, call_resolution, escaped_params, nparams);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          return eval_expr(*n.inner, env, summaries, call_resolution, escaped_params, nparams);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          DepSet out = eval_expr(*n.subject, env, summaries, call_resolution, escaped_params, nparams);
          for (const auto& arm : n.arms) {
            out.join(eval_expr(*arm->value, env, summaries, call_resolution, escaped_params, nparams));
          }
          return out;
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          std::vector<DepSet> arg_deps;
          arg_deps.reserve(n.args.size());
          for (const auto& a : n.args) {
            arg_deps.push_back(
                eval_expr(*a, env, summaries, call_resolution, escaped_params, nparams));
          }

          const auto target = resolve_call_target(n, call_resolution);
          const auto sit = target.has_value() ? summaries.find(*target) : summaries.end();
          const bool summary_known = sit != summaries.end();

          if (!summary_known) {
            for (const auto& d : arg_deps) {
              mark_state_from_dep(d, escaped_params, EscapeState::Unknown);
            }
            DepSet ret = dep_none(nparams);
            for (const auto& d : arg_deps) ret.join(d);
            return ret;
          }

          const auto& callee = sit->second;
          for (std::size_t i = 0; i < arg_deps.size() && i < callee.param_escape.size(); ++i) {
            const EscapeState param_state = summary_param_state(callee, i);
            if (param_state == EscapeState::KnownNoEscape) continue;
            mark_state_from_dep(arg_deps[i], escaped_params, param_state);
          }

          DepSet ret = dep_none(nparams);
          for (std::size_t i = 0;
               i < arg_deps.size() && i < callee.return_depends_on_param.size();
               ++i) {
            if (callee.return_depends_on_param[i]) ret.join(arg_deps[i]);
          }
          return ret;
        } else {
          return dep_none(nparams);
        }
      },
      e.node);
}

static InternalFuncSummary analyze_function(
    const Function& f,
    const std::unordered_map<std::string, InternalFuncSummary>& summaries,
    const FunctionCallTargetResolution& call_resolution) {
  const std::size_t nparams = f.params.size();
  InternalFuncSummary out;
  out.return_depends_on_param.assign(nparams, 0);
  out.param_escape.assign(nparams, EscapeState::KnownNoEscape);

  auto cfg = nebula::nir::build_cfg(f);
  std::vector<Env> in(cfg.nodes.size());
  std::vector<char> inited(cfg.nodes.size(), 0);

  Env entry_env;
  entry_env.reserve(nparams);
  for (std::size_t i = 0; i < nparams; ++i) {
    entry_env.insert({f.params[i].var, dep_from_param(nparams, i)});
  }
  in[cfg.entry] = std::move(entry_env);
  inited[cfg.entry] = 1;

  std::deque<nebula::nir::NodeId> work;
  work.push_back(cfg.entry);

  DepSet return_deps(nparams);
  std::vector<EscapeState> escaped_states(nparams, EscapeState::KnownNoEscape);

  while (!work.empty()) {
    const nebula::nir::NodeId id = work.front();
    work.pop_front();

    const Env& env_in = in[id];
    Env env_out = env_in;

    const auto& node = cfg.nodes[id];
    if (node.kind == nebula::nir::NodeKind::Stmt && node.stmt != nullptr) {
      const Stmt& s = *node.stmt;
      std::visit(
          [&](auto&& st) {
            using S = std::decay_t<decltype(st)>;
            if constexpr (std::is_same_v<S, Stmt::Declare>) {
              env_out.insert_or_assign(st.var, DepSet(nparams));
            } else if constexpr (std::is_same_v<S, Stmt::Let>) {
              DepSet d =
                  eval_expr(*st.value, env_in, summaries, call_resolution, escaped_states, nparams);
              env_out.insert_or_assign(st.var, d);
            } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
              DepSet d =
                  eval_expr(*st.value, env_in, summaries, call_resolution, escaped_states, nparams);
              env_out.insert_or_assign(st.var, d);
            } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
              DepSet d =
                  eval_expr(*st.value, env_in, summaries, call_resolution, escaped_states, nparams);
              mark_state_from_dep(d, escaped_states, EscapeState::Unknown);
            } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
              (void)eval_expr(*st.expr, env_in, summaries, call_resolution, escaped_states, nparams);
            } else if constexpr (std::is_same_v<S, Stmt::Return>) {
              DepSet d =
                  eval_expr(*st.value, env_in, summaries, call_resolution, escaped_states, nparams);
              return_deps.join(d);
              mark_state_from_dep(d, escaped_states, EscapeState::KnownMayEscape);
            } else if constexpr (std::is_same_v<S, Stmt::If>) {
              (void)eval_expr(*st.cond, env_in, summaries, call_resolution, escaped_states, nparams);
            } else if constexpr (std::is_same_v<S, Stmt::For>) {
              (void)eval_expr(*st.start, env_in, summaries, call_resolution, escaped_states, nparams);
              (void)eval_expr(*st.end, env_in, summaries, call_resolution, escaped_states, nparams);
            } else if constexpr (std::is_same_v<S, Stmt::While>) {
              (void)eval_expr(*st.cond, env_in, summaries, call_resolution, escaped_states, nparams);
            } else if constexpr (std::is_same_v<S, Stmt::Break> ||
                                 std::is_same_v<S, Stmt::Continue>) {
              // no-op
            } else if constexpr (std::is_same_v<S, Stmt::Region> ||
                                 std::is_same_v<S, Stmt::Unsafe>) {
              // No explicit node in CFG for wrapper blocks.
            }
          },
          s.node);
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

  for (std::size_t i = 0; i < nparams; ++i) {
    out.return_depends_on_param[i] = return_deps.bits[i] != 0;
    out.param_escape[i] = escaped_states[i];
  }
  return out;
}

static void collect_call_edges_expr(const Expr& e,
                                    std::size_t caller_index,
                                    const FunctionCallTargetResolution& resolution,
                                    const std::unordered_map<std::string, std::size_t>& index_by_name,
                                    std::unordered_set<std::size_t>& out_edges,
                                    bool& self_edge) {
  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          const auto target = resolve_call_target(n, resolution);
          if (target.has_value()) {
            auto it = index_by_name.find(*target);
            if (it != index_by_name.end()) {
              out_edges.insert(it->second);
              if (it->second == caller_index) self_edge = true;
            }
          }
          for (const auto& a : n.args) {
            collect_call_edges_expr(*a,
                                    caller_index,
                                    resolution,
                                    index_by_name,
                                    out_edges,
                                    self_edge);
          }
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& a : n.args) {
            collect_call_edges_expr(*a,
                                    caller_index,
                                    resolution,
                                    index_by_name,
                                    out_edges,
                                    self_edge);
          }
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_call_edges_expr(*n.lhs,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
          collect_call_edges_expr(*n.rhs,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          collect_call_edges_expr(*n.base,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          collect_call_edges_expr(*n.inner,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          collect_call_edges_expr(*n.inner,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
          collect_call_edges_expr(*n.subject,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
          collect_call_edges_expr(*n.subject,
                                  caller_index,
                                  resolution,
                                  index_by_name,
                                  out_edges,
                                  self_edge);
        } else {
          // literals, var refs, fields
        }
      },
      e.node);
}

static void collect_call_edges_block(const Block& b,
                                     std::size_t caller_index,
                                     const FunctionCallTargetResolution& resolution,
                                     const std::unordered_map<std::string, std::size_t>& index_by_name,
                                     std::unordered_set<std::size_t>& out_edges,
                                     bool& self_edge);

static void collect_call_edges_stmt(const Stmt& s,
                                    std::size_t caller_index,
                                    const FunctionCallTargetResolution& resolution,
                                    const std::unordered_map<std::string, std::size_t>& index_by_name,
                                    std::unordered_set<std::size_t>& out_edges,
                                    bool& self_edge) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Declare>) {
        } else if constexpr (std::is_same_v<S, Stmt::Let>) {
          collect_call_edges_expr(
              *st.value, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          collect_call_edges_expr(
              *st.value, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          collect_call_edges_expr(
              *st.value, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          collect_call_edges_expr(
              *st.expr, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          collect_call_edges_expr(
              *st.value, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          collect_call_edges_expr(
              *st.cond, caller_index, resolution, index_by_name, out_edges, self_edge);
          collect_call_edges_block(
              st.then_body, caller_index, resolution, index_by_name, out_edges, self_edge);
          if (st.else_body.has_value()) {
            collect_call_edges_block(
                *st.else_body, caller_index, resolution, index_by_name, out_edges, self_edge);
          }
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          collect_call_edges_expr(
              *st.start, caller_index, resolution, index_by_name, out_edges, self_edge);
          collect_call_edges_expr(
              *st.end, caller_index, resolution, index_by_name, out_edges, self_edge);
          collect_call_edges_block(
              st.body, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          collect_call_edges_expr(
              *st.cond, caller_index, resolution, index_by_name, out_edges, self_edge);
          collect_call_edges_block(
              st.body, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::Break> ||
                             std::is_same_v<S, Stmt::Continue>) {
          // no-op
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          collect_call_edges_block(
              st.body, caller_index, resolution, index_by_name, out_edges, self_edge);
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          collect_call_edges_block(
              st.body, caller_index, resolution, index_by_name, out_edges, self_edge);
        }
      },
      s.node);
}

static void collect_call_edges_block(const Block& b,
                                     std::size_t caller_index,
                                     const FunctionCallTargetResolution& resolution,
                                     const std::unordered_map<std::string, std::size_t>& index_by_name,
                                     std::unordered_set<std::size_t>& out_edges,
                                     bool& self_edge) {
  for (const auto& st : b.stmts) {
    collect_call_edges_stmt(st, caller_index, resolution, index_by_name, out_edges, self_edge);
  }
}

static CallGraph build_call_graph(const nebula::nir::Program& p,
                                  const CallTargetResolution& resolution) {
  CallGraph graph;
  for (const auto& item : p.items) {
    if (!std::holds_alternative<Function>(item.node)) continue;
    const auto& fn = std::get<Function>(item.node);
    if (fn.is_extern) continue;
    const std::string identity = nebula::nir::function_identity(fn);
    graph.index_by_name.insert({identity, graph.names.size()});
    graph.names.push_back(identity);
    graph.funcs.push_back(FunctionContext{&fn, nullptr});
  }

  graph.edges.resize(graph.names.size());
  graph.self_edge.assign(graph.names.size(), false);

  for (std::size_t i = 0; i < graph.names.size(); ++i) {
    const auto fit = resolution.by_function.find(graph.names[i]);
    static const FunctionCallTargetResolution kEmptyResolution{};
    graph.funcs[i].resolution = (fit == resolution.by_function.end()) ? &kEmptyResolution : &fit->second;

    std::unordered_set<std::size_t> out_edges;
    bool self_edge = false;
    if (graph.funcs[i].fn->body.has_value()) {
      collect_call_edges_block(*graph.funcs[i].fn->body,
                               i,
                               *graph.funcs[i].resolution,
                               graph.index_by_name,
                               out_edges,
                               self_edge);
    }
    graph.edges[i].assign(out_edges.begin(), out_edges.end());
    graph.self_edge[i] = self_edge;
  }

  return graph;
}

static SccResult compute_scc(const CallGraph& graph) {
  const std::size_t n = graph.names.size();
  SccResult out;
  out.component_of.assign(n, 0);

  std::vector<int> index(n, -1);
  std::vector<int> low(n, 0);
  std::vector<char> on_stack(n, 0);
  std::vector<std::size_t> stack;
  int next_index = 0;

  std::function<void(std::size_t)> dfs = [&](std::size_t v) {
    index[v] = next_index;
    low[v] = next_index;
    ++next_index;
    stack.push_back(v);
    on_stack[v] = 1;

    for (std::size_t w : graph.edges[v]) {
      if (index[w] == -1) {
        dfs(w);
        low[v] = std::min(low[v], low[w]);
      } else if (on_stack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }

    if (low[v] != index[v]) return;

    std::vector<std::size_t> comp;
    while (!stack.empty()) {
      const std::size_t w = stack.back();
      stack.pop_back();
      on_stack[w] = 0;
      out.component_of[w] = out.components.size();
      comp.push_back(w);
      if (w == v) break;
    }
    out.components.push_back(std::move(comp));
  };

  for (std::size_t v = 0; v < n; ++v) {
    if (index[v] == -1) dfs(v);
  }

  out.dag_edges.assign(out.components.size(), {});
  std::vector<std::unordered_set<std::size_t>> dedup(out.components.size());
  std::vector<int> indegree(out.components.size(), 0);

  for (std::size_t v = 0; v < n; ++v) {
    const std::size_t cv = out.component_of[v];
    for (std::size_t w : graph.edges[v]) {
      const std::size_t cw = out.component_of[w];
      if (cv == cw) continue;
      if (!dedup[cv].insert(cw).second) continue;
      out.dag_edges[cv].push_back(cw);
      indegree[cw] += 1;
    }
  }

  std::queue<std::size_t> q;
  for (std::size_t c = 0; c < indegree.size(); ++c) {
    if (indegree[c] == 0) q.push(c);
  }

  while (!q.empty()) {
    const std::size_t c = q.front();
    q.pop();
    out.topo_order.push_back(c);
    for (std::size_t nxt : out.dag_edges[c]) {
      indegree[nxt] -= 1;
      if (indegree[nxt] == 0) q.push(nxt);
    }
  }

  if (out.topo_order.size() != out.components.size()) {
    out.topo_order.clear();
    for (std::size_t c = 0; c < out.components.size(); ++c) out.topo_order.push_back(c);
  }

  return out;
}

static bool is_nontrivial_component(const CallGraph& graph,
                                    const std::vector<std::size_t>& component) {
  if (component.size() > 1) return true;
  if (component.empty()) return false;
  return graph.self_edge[component.front()];
}

static InternalFuncSummary make_empty_summary(std::size_t nparams) {
  InternalFuncSummary out;
  out.return_depends_on_param.assign(nparams, 0);
  out.param_escape.assign(nparams, EscapeState::KnownNoEscape);
  return out;
}

static InternalFuncSummary make_unknown_summary(std::size_t nparams) {
  InternalFuncSummary out;
  out.return_depends_on_param.assign(nparams, 1);
  out.param_escape.assign(nparams, EscapeState::Unknown);
  return out;
}

static InternalFuncSummary make_extern_summary(const Function& fn) {
  const std::size_t nparams = fn.params.size();
  const auto contract = nebula::frontend::parse_external_escape_contract(fn.annotations, nparams);
  if (!contract.has_annotations || !contract.errors.empty()) {
    return make_unknown_summary(nparams);
  }

  InternalFuncSummary out = make_unknown_summary(nparams);
  if (contract.has_return_contract) {
    out.return_depends_on_param.assign(nparams, 0);
    if (!contract.returns_fresh) {
      for (std::size_t i = 0; i < contract.return_depends_on_param.size(); ++i) {
        out.return_depends_on_param[i] = contract.return_depends_on_param[i] ? 1 : 0;
      }
    }
  }

  for (std::size_t i = 0; i < contract.param_states.size(); ++i) {
    switch (contract.param_states[i]) {
    case nebula::frontend::ExternalEscapeState::Unspecified:
      break;
    case nebula::frontend::ExternalEscapeState::NoEscape:
      out.param_escape[i] = EscapeState::KnownNoEscape;
      break;
    case nebula::frontend::ExternalEscapeState::MayEscape:
      out.param_escape[i] = EscapeState::KnownMayEscape;
      break;
    case nebula::frontend::ExternalEscapeState::Unknown:
      out.param_escape[i] = EscapeState::Unknown;
      break;
    }
  }
  return out;
}

static bool has_valid_extern_contract(const Function& fn) {
  if (!fn.is_extern) return false;
  const auto contract = nebula::frontend::parse_external_escape_contract(fn.annotations, fn.params.size());
  return contract.has_annotations && contract.errors.empty();
}

static bool merge_summary(InternalFuncSummary& dst, const InternalFuncSummary& src) {
  bool changed = false;
  const std::size_t nret = std::min(dst.return_depends_on_param.size(), src.return_depends_on_param.size());
  for (std::size_t i = 0; i < nret; ++i) {
    if (dst.return_depends_on_param[i] || !src.return_depends_on_param[i]) continue;
    dst.return_depends_on_param[i] = 1;
    changed = true;
  }

  const std::size_t nparam = std::min(dst.param_escape.size(), src.param_escape.size());
  for (std::size_t i = 0; i < nparam; ++i) {
    const EscapeState joined = join_escape(dst.param_escape[i], src.param_escape[i]);
    if (joined == dst.param_escape[i]) continue;
    dst.param_escape[i] = joined;
    changed = true;
  }
  return changed;
}

static FuncEscapeSummary export_summary(const InternalFuncSummary& in) {
  FuncEscapeSummary out;
  out.return_depends_on_param.assign(in.return_depends_on_param.size(), false);
  out.param_may_escape.assign(in.param_escape.size(), false);
  out.param_escape_unknown.assign(in.param_escape.size(), false);

  for (std::size_t i = 0; i < in.return_depends_on_param.size(); ++i) {
    out.return_depends_on_param[i] = in.return_depends_on_param[i] != 0;
  }

  for (std::size_t i = 0; i < in.param_escape.size(); ++i) {
    const EscapeState state = in.param_escape[i];
    const bool unknown = state == EscapeState::Unknown;
    const bool may_escape = state != EscapeState::KnownNoEscape;
    out.param_escape_unknown[i] = unknown;
    out.param_may_escape[i] = may_escape || unknown;
  }

  return out;
}

static EscapeAnalysisOptions normalize_options(const EscapeAnalysisOptions& options) {
  EscapeAnalysisOptions normalized = options;
  return normalized;
}

} // namespace

EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p,
                                         const CallTargetResolution& resolution,
                                         const EscapeAnalysisOptions& options) {
  const EscapeAnalysisOptions normalized = normalize_options(options);
  const CallGraph graph = build_call_graph(p, resolution);
  const SccResult scc = compute_scc(graph);

  std::unordered_map<std::string, InternalFuncSummary> internal;
  internal.reserve(graph.names.size() + 1);

  internal.insert({"expect_eq", make_empty_summary(2)});
  internal.insert({"print", make_empty_summary(1)});
  internal.insert({"panic", make_empty_summary(1)});
  internal.insert({"assert", make_empty_summary(1)});
  internal.insert({"argc", make_empty_summary(0)});
  internal.insert({"argv", make_empty_summary(1)});
  internal.insert({"args_count", make_empty_summary(0)});
  internal.insert({"args_get", make_empty_summary(1)});
  for (std::size_t i = 0; i < graph.names.size(); ++i) {
    internal.insert({graph.names[i], make_empty_summary(graph.funcs[i].fn->params.size())});
  }
  for (const auto& item : p.items) {
    if (!std::holds_alternative<Function>(item.node)) continue;
    const auto& fn = std::get<Function>(item.node);
    if (!has_valid_extern_contract(fn)) continue;
    internal.insert({nebula::nir::function_identity(fn), make_extern_summary(fn)});
  }

  std::vector<std::size_t> scc_order = scc.topo_order;
  std::reverse(scc_order.begin(), scc_order.end());

  for (std::size_t comp_id : scc_order) {
    const auto& comp = scc.components[comp_id];
    const bool nontrivial = is_nontrivial_component(graph, comp);

    if (normalized.profile == EscapeAnalysisProfile::Fast && nontrivial) {
      for (std::size_t fn_index : comp) {
        internal[graph.names[fn_index]] =
            make_unknown_summary(graph.funcs[fn_index].fn->params.size());
      }
      continue;
    }

    if (normalized.profile == EscapeAnalysisProfile::Deep && nontrivial) {
      bool stabilized = false;
      for (std::size_t iter = 0; iter < 64; ++iter) {
        bool changed = false;
        for (std::size_t fn_index : comp) {
          const auto& fn_ctx = graph.funcs[fn_index];
          const InternalFuncSummary analyzed =
              analyze_function(*fn_ctx.fn, internal, *fn_ctx.resolution);
          changed = merge_summary(internal[graph.names[fn_index]], analyzed) || changed;
        }
        if (!changed) {
          stabilized = true;
          break;
        }
      }

      if (!stabilized) {
        for (std::size_t fn_index : comp) {
          internal[graph.names[fn_index]] =
              make_unknown_summary(graph.funcs[fn_index].fn->params.size());
        }
      }
      continue;
    }

    for (std::size_t fn_index : comp) {
      const auto& fn_ctx = graph.funcs[fn_index];
      const InternalFuncSummary analyzed =
          analyze_function(*fn_ctx.fn, internal, *fn_ctx.resolution);
      (void)merge_summary(internal[graph.names[fn_index]], analyzed);
    }
  }

  EscapeAnalysisResult out;
  out.by_function.reserve(internal.size());
  for (auto& [name, summary] : internal) {
    out.by_function.insert({name, export_summary(summary)});
  }
  return out;
}

EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p,
                                         const CallTargetResolution& resolution) {
  EscapeAnalysisOptions options;
  options.profile = EscapeAnalysisProfile::Fast;
  return run_escape_analysis(p, resolution, options);
}

EscapeAnalysisResult run_escape_analysis(const nebula::nir::Program& p) {
  const auto resolution = run_call_target_resolver(p);
  EscapeAnalysisOptions options;
  options.profile = EscapeAnalysisProfile::Fast;
  return run_escape_analysis(p, resolution, options);
}

} // namespace nebula::passes
