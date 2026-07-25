#include "passes/dead_code_elim.hpp"

#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nebula::passes {
namespace {

using namespace nebula::nir;

// ---------------------------------------------------------------------------
// Purity: an expression is pure when evaluating it has no observable effect, so
// a binding that captures it and is never read can be dropped. We are
// deliberately conservative — anything that could call user code, suspend, or
// branch on side-effecting arms (Call / Await / Match) is treated as impure.
// ---------------------------------------------------------------------------
bool is_pure(const Expr& e);

bool all_pure(const std::vector<ExprPtr>& xs) {
  for (const auto& x : xs) {
    if (x && !is_pure(*x)) return false;
  }
  return true;
}

bool is_pure(const Expr& e) {
  return std::visit(
      [](const auto& n) -> bool {
        using T = std::decay_t<decltype(n)>;
        (void)n;
        if constexpr (std::is_same_v<T, Expr::IntLit> || std::is_same_v<T, Expr::BoolLit> ||
                      std::is_same_v<T, Expr::FloatLit> || std::is_same_v<T, Expr::StringLit> ||
                      std::is_same_v<T, Expr::VarRef> || std::is_same_v<T, Expr::FieldRef>) {
          return true;
        } else if constexpr (std::is_same_v<T, Expr::TempFieldRef>) {
          return n.base && is_pure(*n.base);
        } else if constexpr (std::is_same_v<T, Expr::Construct>) {
          return all_pure(n.args);
        } else if constexpr (std::is_same_v<T, Expr::Binary>) {
          return n.lhs && n.rhs && is_pure(*n.lhs) && is_pure(*n.rhs);
        } else if constexpr (std::is_same_v<T, Expr::Unary>) {
          return n.inner && is_pure(*n.inner);
        } else if constexpr (std::is_same_v<T, Expr::Prefix>) {
          return n.inner && is_pure(*n.inner);
        } else if constexpr (std::is_same_v<T, Expr::EnumIsVariant>) {
          return n.subject && is_pure(*n.subject);
        } else if constexpr (std::is_same_v<T, Expr::EnumPayload>) {
          return n.subject && is_pure(*n.subject);
        } else {
          // Call, Await, Match: conservatively impure.
          return false;
        }
      },
      e.node);
}

// ---------------------------------------------------------------------------
// Occurrence counting: every place a VarId textually appears, whether a
// definition or a use. A `let` whose variable appears exactly once (only at its
// own definition) is referenced nowhere else and is a removal candidate. VarIds
// are unique per binding, so count == 1 is a sound liveness under-approximation.
// ---------------------------------------------------------------------------
using Counts = std::unordered_map<VarId, int>;

void count_expr(const Expr& e, Counts& cnt);
void count_block(const Block& b, Counts& cnt);

void count_expr(const Expr& e, Counts& cnt) {
  std::visit(
      [&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        (void)n;
        if constexpr (std::is_same_v<T, Expr::VarRef>) {
          cnt[n.var]++;
        } else if constexpr (std::is_same_v<T, Expr::FieldRef>) {
          cnt[n.base_var]++;
        } else if constexpr (std::is_same_v<T, Expr::Call>) {
          if (n.kind == CallKind::Indirect) cnt[n.callee_var]++;
          for (const auto& a : n.args) {
            if (a) count_expr(*a, cnt);
          }
        } else if constexpr (std::is_same_v<T, Expr::TempFieldRef>) {
          if (n.base) count_expr(*n.base, cnt);
        } else if constexpr (std::is_same_v<T, Expr::EnumIsVariant> ||
                             std::is_same_v<T, Expr::EnumPayload>) {
          if (n.subject) count_expr(*n.subject, cnt);
        } else if constexpr (std::is_same_v<T, Expr::Construct>) {
          for (const auto& a : n.args) {
            if (a) count_expr(*a, cnt);
          }
        } else if constexpr (std::is_same_v<T, Expr::Binary>) {
          if (n.lhs) count_expr(*n.lhs, cnt);
          if (n.rhs) count_expr(*n.rhs, cnt);
        } else if constexpr (std::is_same_v<T, Expr::Unary> ||
                             std::is_same_v<T, Expr::Prefix> || std::is_same_v<T, Expr::Await>) {
          if (n.inner) count_expr(*n.inner, cnt);
        } else if constexpr (std::is_same_v<T, Expr::Match>) {
          if (n.subject) count_expr(*n.subject, cnt);
          for (const auto& arm : n.arms) {
            if (!arm) continue;
            if (arm->payload_binding) cnt[arm->payload_binding->var]++;
            for (const auto& sb : arm->payload_struct_bindings) cnt[sb.binding.var]++;
            if (arm->value) count_expr(*arm->value, cnt);
          }
        } else {
          // Literals carry no VarId.
        }
      },
      e.node);
}

void count_stmt(const Stmt& s, Counts& cnt) {
  std::visit(
      [&](const auto& n) {
        using T = std::decay_t<decltype(n)>;
        (void)n;
        if constexpr (std::is_same_v<T, Stmt::Declare>) {
          cnt[n.var]++;
        } else if constexpr (std::is_same_v<T, Stmt::Let>) {
          cnt[n.var]++;
          if (n.value) count_expr(*n.value, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::Return>) {
          if (n.value) count_expr(*n.value, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::ExprStmt>) {
          if (n.expr) count_expr(*n.expr, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::AssignVar>) {
          cnt[n.var]++;
          if (n.value) count_expr(*n.value, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::AssignField>) {
          cnt[n.base_var]++;
          if (n.value) count_expr(*n.value, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::Region> ||
                             std::is_same_v<T, Stmt::Unsafe>) {
          count_block(n.body, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::If>) {
          if (n.cond) count_expr(*n.cond, cnt);
          count_block(n.then_body, cnt);
          if (n.else_body) count_block(*n.else_body, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::For>) {
          cnt[n.var]++;
          if (n.start) count_expr(*n.start, cnt);
          if (n.end) count_expr(*n.end, cnt);
          count_block(n.body, cnt);
        } else if constexpr (std::is_same_v<T, Stmt::While>) {
          if (n.cond) count_expr(*n.cond, cnt);
          count_block(n.body, cnt);
        } else {
          // Break, Continue: no VarIds.
        }
      },
      s.node);
}

void count_block(const Block& b, Counts& cnt) {
  for (const auto& s : b.stmts) count_stmt(s, cnt);
}

// ---------------------------------------------------------------------------
// Elimination sweep over a block. Recurses into nested blocks first, then drops
// dead pure `let` bindings at this level. Uses a fixed `cnt` snapshot; the
// caller re-counts and re-sweeps to a fixpoint so cascading dead bindings are
// also collected.
// ---------------------------------------------------------------------------
void elim_block(Block& b, const Counts& cnt, std::size_t& removed);

void elim_block(Block& b, const Counts& cnt, std::size_t& removed) {
  std::vector<Stmt> kept;
  kept.reserve(b.stmts.size());
  for (auto& s : b.stmts) {
    std::visit(
        [&](auto& n) {
          using T = std::decay_t<decltype(n)>;
          (void)n;
          if constexpr (std::is_same_v<T, Stmt::Region> || std::is_same_v<T, Stmt::Unsafe> ||
                        std::is_same_v<T, Stmt::For> || std::is_same_v<T, Stmt::While>) {
            elim_block(n.body, cnt, removed);
          } else if constexpr (std::is_same_v<T, Stmt::If>) {
            elim_block(n.then_body, cnt, removed);
            if (n.else_body) elim_block(*n.else_body, cnt, removed);
          }
        },
        s.node);

    bool drop = false;
    if (std::holds_alternative<Stmt::Let>(s.node)) {
      const auto& let = std::get<Stmt::Let>(s.node);
      const auto it = cnt.find(let.var);
      const int occurrences = (it == cnt.end()) ? 0 : it->second;
      if (occurrences <= 1 && let.value && is_pure(*let.value)) drop = true;
    }

    if (drop) {
      removed++;
    } else {
      kept.push_back(std::move(s));
    }
  }
  b.stmts = std::move(kept);
}

} // namespace

DeadCodeElimResult run_dead_code_elim(Program& p) {
  DeadCodeElimResult res;
  for (auto& item : p.items) {
    if (!std::holds_alternative<Function>(item.node)) continue;
    auto& fn = std::get<Function>(item.node);
    if (!fn.body) continue;
    for (;;) {
      Counts cnt;
      count_block(*fn.body, cnt);
      const std::size_t before = res.removed_bindings;
      elim_block(*fn.body, cnt, res.removed_bindings);
      if (res.removed_bindings == before) break;
    }
  }
  return res;
}

} // namespace nebula::passes
