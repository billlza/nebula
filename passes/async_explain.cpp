#include "passes/async_explain.hpp"

#include "nir/runtime_ops.hpp"

#include <algorithm>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace nebula::passes {

namespace {

using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Item;
using nebula::nir::RuntimeAsyncCallKind;
using nebula::nir::Stmt;
using nebula::nir::VarId;

struct VarDeclInfo {
  std::string name;
  nebula::frontend::Span span{};
  bool user_visible = true;
};

struct VarRefUse {
  VarId var = 0;
  nebula::frontend::Span span{};
};

struct AsyncFunctionFacts {
  std::string function_name;
  std::string path;
  nebula::frontend::Span function_span{};
  bool is_async = false;
  std::unordered_map<VarId, VarDeclInfo> declarations;
  std::vector<VarRefUse> references;
  std::vector<nebula::frontend::Span> await_spans;
  std::vector<std::pair<RuntimeAsyncCallKind, nebula::frontend::Span>> async_calls;
};

bool is_user_visible_name(std::string_view name) {
  return !name.empty() && name.rfind("__nebula_", 0) != 0;
}

void record_declaration(AsyncFunctionFacts& facts, VarId var, std::string name, nebula::frontend::Span span) {
  if (var == 0) return;
  VarDeclInfo info;
  info.user_visible = is_user_visible_name(name);
  info.name = std::move(name);
  info.span = std::move(span);
  facts.declarations.insert_or_assign(var, std::move(info));
}

void record_reference(AsyncFunctionFacts& facts, VarId var, const nebula::frontend::Span& span) {
  if (var == 0) return;
  facts.references.push_back({var, span});
}

void collect_expr_facts(const Expr& expr, AsyncFunctionFacts& facts);

void collect_block_facts(const Block& block, AsyncFunctionFacts& facts) {
  for (const auto& stmt : block.stmts) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Stmt::Declare>) {
            record_declaration(facts, node.var, node.name, stmt.span);
          } else if constexpr (std::is_same_v<N, Stmt::Let>) {
            collect_expr_facts(*node.value, facts);
            record_declaration(facts, node.var, node.name, stmt.span);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            collect_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            collect_expr_facts(*node.expr, facts);
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            record_reference(facts, node.var, stmt.span);
            collect_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            record_reference(facts, node.base_var, stmt.span);
            collect_expr_facts(*node.value, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            collect_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            collect_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            collect_expr_facts(*node.cond, facts);
            collect_block_facts(node.then_body, facts);
            if (node.else_body.has_value()) collect_block_facts(*node.else_body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            collect_expr_facts(*node.start, facts);
            collect_expr_facts(*node.end, facts);
            record_declaration(facts, node.var, node.var_name, stmt.span);
            collect_block_facts(node.body, facts);
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            collect_expr_facts(*node.cond, facts);
            collect_block_facts(node.body, facts);
          }
        },
        stmt.node);
  }
}

void collect_expr_facts(const Expr& expr, AsyncFunctionFacts& facts) {
  std::visit(
      [&](auto&& node) {
        using N = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<N, Expr::VarRef>) {
          record_reference(facts, node.var, expr.span);
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          if (node.kind == nebula::nir::CallKind::Indirect) {
            record_reference(facts, node.callee_var, expr.span);
          }
          const auto async_kind =
              nebula::nir::classify_runtime_async_call(node.resolved_callee, node.callee);
          if (async_kind == RuntimeAsyncCallKind::Spawn || async_kind == RuntimeAsyncCallKind::Join ||
              async_kind == RuntimeAsyncCallKind::Timeout) {
            facts.async_calls.push_back({async_kind, expr.span});
          }
          for (const auto& arg : node.args) collect_expr_facts(*arg, facts);
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          record_reference(facts, node.base_var, expr.span);
        } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
          collect_expr_facts(*node.base, facts);
        } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant> ||
                             std::is_same_v<N, Expr::EnumPayload>) {
          collect_expr_facts(*node.subject, facts);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& arg : node.args) collect_expr_facts(*arg, facts);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_expr_facts(*node.lhs, facts);
          collect_expr_facts(*node.rhs, facts);
        } else if constexpr (std::is_same_v<N, Expr::Unary> ||
                             std::is_same_v<N, Expr::Prefix>) {
          collect_expr_facts(*node.inner, facts);
        } else if constexpr (std::is_same_v<N, Expr::Await>) {
          facts.await_spans.push_back(expr.span);
          collect_expr_facts(*node.inner, facts);
        } else if constexpr (std::is_same_v<N, Expr::Match>) {
          collect_expr_facts(*node.subject, facts);
          for (const auto& arm : node.arms) {
            if (arm->payload_binding.has_value()) {
              record_declaration(facts,
                                 arm->payload_binding->var,
                                 arm->payload_binding->name,
                                 arm->payload_binding->span);
            }
            for (const auto& field : arm->payload_struct_bindings) {
              record_declaration(facts, field.binding.var, field.binding.name, field.binding.span);
            }
            collect_expr_facts(*arm->value, facts);
          }
        }
      },
      expr.node);
}

std::vector<std::string> carried_values_after_await(const AsyncFunctionFacts& facts,
                                                    const nebula::frontend::Span& await_span) {
  std::set<std::string> carried;
  for (const auto& ref : facts.references) {
    if (ref.span.start.offset <= await_span.end.offset) continue;
    auto decl_it = facts.declarations.find(ref.var);
    if (decl_it == facts.declarations.end()) continue;
    if (!decl_it->second.user_visible) continue;
    if (decl_it->second.span.start.offset >= await_span.start.offset) continue;
    carried.insert(decl_it->second.name);
  }
  return std::vector<std::string>(carried.begin(), carried.end());
}

AsyncExplainEntry make_async_call_entry(const AsyncFunctionFacts& facts,
                                        RuntimeAsyncCallKind kind,
                                        const nebula::frontend::Span& span) {
  AsyncExplainEntry entry;
  entry.function_name = facts.function_name;
  entry.path = facts.path;
  entry.span = span;
  entry.task_boundary = true;
  switch (kind) {
  case RuntimeAsyncCallKind::Spawn:
    entry.kind = "spawn";
    entry.summary = "spawn task boundary";
    entry.allocation = "task-state";
    entry.reason =
        "spawn moves the future handle into TaskStateImpl and schedules that task-owned state";
    break;
  case RuntimeAsyncCallKind::Join:
    entry.kind = "join";
    entry.summary = "join task boundary";
    entry.allocation = "existing-task-state";
    entry.reason =
        "join forwards existing task state; awaiting it waits on the spawned task without allocating a new task";
    break;
  case RuntimeAsyncCallKind::Timeout:
    entry.kind = "timeout";
    entry.summary = "timeout task boundary";
    entry.allocation = "task-state";
    entry.reason =
        "timeout internally calls spawn before racing the task against sleep, so the task boundary is created inside timeout";
    break;
  case RuntimeAsyncCallKind::BlockOn:
  case RuntimeAsyncCallKind::None: break;
  }
  return entry;
}

} // namespace

AsyncExplainResult run_async_explain(const nebula::nir::Program& program) {
  AsyncExplainResult result;
  for (const auto& item : program.items) {
    std::visit(
        [&](auto&& node) {
          using N = std::decay_t<decltype(node)>;
          if constexpr (!std::is_same_v<N, Function>) {
            return;
          } else {
            if (!node.body.has_value()) return;
            AsyncFunctionFacts facts;
            facts.function_name = node.name;
            facts.path = node.span.source_path;
            facts.function_span = node.span;
            facts.is_async = node.is_async;
            for (const auto& param : node.params) {
              record_declaration(facts, param.var, param.name, param.span);
            }
            collect_block_facts(*node.body, facts);

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
                                  ? "async function with no explicit await in the current body"
                                  : "async function with " + std::to_string(facts.await_spans.size()) +
                                        " suspension point(s)";
              entry.allocation = "async-frame";
              entry.reason =
                  "async functions lower to Future coroutine state; explicit await sites keep the frame resumable across suspension";
              entry.carried_values.assign(carried_all.begin(), carried_all.end());
              result.entries.push_back(std::move(entry));
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
                  "co_await stores the continuation in Future state and resumes through the coroutine frame";
              entry.carried_values = carried_values_after_await(facts, await_span);
              entry.suspension_point = true;
              result.entries.push_back(std::move(entry));
            }

            for (const auto& [kind, span] : facts.async_calls) {
              result.entries.push_back(make_async_call_entry(facts, kind, span));
            }
          }
        },
        item.node);
  }

  std::sort(result.entries.begin(),
            result.entries.end(),
            [](const AsyncExplainEntry& lhs, const AsyncExplainEntry& rhs) {
              if (lhs.path != rhs.path) return lhs.path < rhs.path;
              if (lhs.span.start.line != rhs.span.start.line) return lhs.span.start.line < rhs.span.start.line;
              if (lhs.span.start.col != rhs.span.start.col) return lhs.span.start.col < rhs.span.start.col;
              if (lhs.span.end.line != rhs.span.end.line) return lhs.span.end.line < rhs.span.end.line;
              return lhs.span.end.col < rhs.span.end.col;
            });
  return result;
}

} // namespace nebula::passes
