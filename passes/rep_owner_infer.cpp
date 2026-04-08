#include "passes/rep_owner_infer.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nebula::passes {

namespace {

using nebula::frontend::Diagnostic;
using nebula::frontend::Severity;
using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::PrefixKind;
using nebula::nir::Stmt;
using nebula::nir::VarId;

struct VarMeta {
  nebula::frontend::Span span{};
  bool is_region_alloc_site = false; // Construct inside region with no explicit heap/shared/unique/prefix
};

struct FuncCtx {
  const std::unordered_map<std::string, FuncEscapeSummary>* escape_summaries = nullptr;
  const CallTargetResolution* call_resolution = nullptr;
  const std::unordered_set<std::string>* local_functions = nullptr;
  RepOwnerOptions opt{};
  std::string function_name;

  // Per-function results
  FuncRepOwner out;
  std::vector<Diagnostic> diags;

  // Temp meta for analysis
  std::unordered_map<VarId, VarMeta> meta;
  std::unordered_map<VarId, std::unordered_set<VarId>> deps; // let-var -> vars referenced in RHS
  std::unordered_map<VarId, VarId> alias_of;                 // dest -> src for `let dest = src`
  struct ReturnPathEvent {
    bool unknown = false;
    std::uint8_t unknown_source_mask = 0;
    std::unordered_set<VarId> vars;
  };
  std::vector<ReturnPathEvent> return_path_events;

  // Region stack (lexical)
  std::vector<std::string> region_stack;
};

enum class UnknownReturnPathSource : std::uint8_t {
  MissingSummary = 0,
  ExternalOpaque = 1,
  IndirectUnresolved = 2,
};

static std::uint8_t unknown_source_bit(UnknownReturnPathSource s) {
  return static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(s));
}

static const FuncEscapeSummary* lookup_summary(const FuncCtx& ctx, const std::string& callee);

static std::uint8_t classify_unknown_source(const FuncCtx& ctx, const Expr::Call& call,
                                            const std::string& resolved_target,
                                            bool resolved_known) {
  if (call.kind == nebula::nir::CallKind::Indirect && !resolved_known) {
    return unknown_source_bit(UnknownReturnPathSource::IndirectUnresolved);
  }
  if (resolved_known) {
    if (ctx.local_functions && ctx.local_functions->count(resolved_target) == 0) {
      return unknown_source_bit(UnknownReturnPathSource::ExternalOpaque);
    }
    return unknown_source_bit(UnknownReturnPathSource::MissingSummary);
  }
  if (call.kind == nebula::nir::CallKind::Indirect) {
    return unknown_source_bit(UnknownReturnPathSource::IndirectUnresolved);
  }
  return unknown_source_bit(UnknownReturnPathSource::ExternalOpaque);
}

static const FuncEscapeSummary* lookup_summary_for_call(const FuncCtx& ctx, const Expr::Call& call,
                                                        std::uint8_t* unknown_source_mask) {
  std::string resolved_target = nebula::nir::call_target_identity(call);
  bool resolved_known = true;
  if (call.kind == nebula::nir::CallKind::Indirect) {
    resolved_known = false;
    if (ctx.call_resolution != nullptr) {
      const auto resolved = ctx.call_resolution->resolve(ctx.function_name, &call);
      if (resolved.is_known()) {
        resolved_target = resolved.callee;
        resolved_known = true;
      }
    }
  }

  const FuncEscapeSummary* summary = lookup_summary(ctx, resolved_target);
  if (summary != nullptr) return summary;
  if (unknown_source_mask != nullptr) {
    *unknown_source_mask = classify_unknown_source(ctx, call, resolved_target, resolved_known);
  }
  return nullptr;
}

static std::string unknown_owner_reason(const std::uint8_t mask) {
  if ((mask & unknown_source_bit(UnknownReturnPathSource::IndirectUnresolved)) != 0) {
    return "cross-function-return-path-unknown-indirect-unresolved";
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::ExternalOpaque)) != 0) {
    return "cross-function-return-path-unknown-external-opaque";
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::MissingSummary)) != 0) {
    return "cross-function-return-path-unknown-no-summary";
  }
  return "cross-function-return-path-unknown-no-summary";
}

static std::string unknown_owner_reason_detail(const std::uint8_t mask) {
  std::vector<std::string_view> reasons;
  reasons.reserve(3);
  if ((mask & unknown_source_bit(UnknownReturnPathSource::IndirectUnresolved)) != 0) {
    reasons.push_back("cross-function-return-path-unknown-indirect-unresolved");
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::ExternalOpaque)) != 0) {
    reasons.push_back("cross-function-return-path-unknown-external-opaque");
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::MissingSummary)) != 0) {
    reasons.push_back("cross-function-return-path-unknown-no-summary");
  }
  if (reasons.empty()) {
    reasons.push_back("cross-function-return-path-unknown-no-summary");
  }
  std::string out;
  std::size_t cap = 0;
  for (const auto reason : reasons) cap += reason.size();
  if (reasons.size() > 1) cap += reasons.size() - 1;
  out.reserve(cap);
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) out.push_back('|');
    out.append(reasons[i]);
  }
  return out;
}

static std::string unknown_call_subreason(const std::uint8_t mask) {
  if ((mask & unknown_source_bit(UnknownReturnPathSource::IndirectUnresolved)) != 0) {
    return "callee-param-escape-unknown-indirect-unresolved";
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::ExternalOpaque)) != 0) {
    return "callee-param-escape-unknown-external-opaque";
  }
  if ((mask & unknown_source_bit(UnknownReturnPathSource::MissingSummary)) != 0) {
    return "callee-param-escape-unknown-no-summary";
  }
  return "callee-param-escape-unknown-no-summary";
}

static bool summary_param_unknown(const FuncEscapeSummary* summary, std::size_t arg_index) {
  if (summary == nullptr) return true;
  if (arg_index >= summary->param_escape_unknown.size()) return true;
  return summary->param_escape_unknown[arg_index];
}

static std::string make_reason_detail(std::string_view reason, std::string_view subreason) {
  if (reason.empty()) return std::string(subreason);
  if (subreason.empty()) return std::string(reason);
  std::string out;
  out.reserve(reason.size() + subreason.size() + 1);
  out.append(reason);
  out.push_back('/');
  out.append(subreason);
  return out;
}

enum class EscapeReason : std::uint8_t {
  Return = 0,
  Call = 1,
  CallUnknown = 2,
  Field = 3,
  ReturnCallPath = 4,
  ReturnCallPathUnknown = 5,
};

struct EscapeSeedInfo {
  std::uint32_t reason_mask = 0;
  bool return_path_unknown = false;
  std::uint8_t unknown_source_mask = 0;
};

static std::uint32_t reason_bit(EscapeReason r) {
  return 1u << static_cast<std::uint8_t>(r);
}

static void mark_seed(std::unordered_map<VarId, EscapeSeedInfo>& seeds, VarId v,
                      EscapeReason reason, std::uint8_t unknown_source_mask = 0) {
  if (v == 0) return;
  auto& info = seeds[v];
  info.reason_mask |= reason_bit(reason);
  info.unknown_source_mask |= unknown_source_mask;
  if (reason == EscapeReason::ReturnCallPathUnknown) {
    info.return_path_unknown = true;
  }
}

static const FuncEscapeSummary* lookup_summary(const FuncCtx& ctx, const std::string& callee) {
  if (!ctx.escape_summaries) return nullptr;
  auto it = ctx.escape_summaries->find(callee);
  if (it == ctx.escape_summaries->end()) return nullptr;
  return &it->second;
}

static Severity apply_warnings_as_errors(const FuncCtx& ctx, Severity sev) {
  if (ctx.opt.warnings_as_errors && sev == Severity::Warning) return Severity::Error;
  return sev;
}

static void collect_var_refs(const Expr& e, std::unordered_set<VarId>& out) {
  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::VarRef>) {
          if (n.var != 0) out.insert(n.var);
        } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
          if (n.base_var != 0) out.insert(n.base_var);
        } else if constexpr (std::is_same_v<N, Expr::Call>) {
          for (const auto& a : n.args) collect_var_refs(*a, out);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& a : n.args) collect_var_refs(*a, out);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          collect_var_refs(*n.lhs, out);
          collect_var_refs(*n.rhs, out);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          collect_var_refs(*n.inner, out);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          collect_var_refs(*n.inner, out);
        } else {
          // literals: nothing
        }
      },
      e.node);
}

static const Expr* strip_prefix_chain(const Expr& e, std::vector<PrefixKind>& chain) {
  const Expr* cur = &e;
  while (std::holds_alternative<Expr::Prefix>(cur->node)) {
    const auto& p = std::get<Expr::Prefix>(cur->node);
    chain.push_back(p.kind);
    cur = p.inner.get();
    if (cur == nullptr) break;
  }
  return cur;
}

static StorageDecision decision_from_prefix_chain(const std::vector<PrefixKind>& chain) {
  // Outermost prefix wins (v0.1 pragmatic). Nested combos are not specified; we treat them as
  // successive directives and pick the first.
  for (PrefixKind k : chain) {
    switch (k) {
    case PrefixKind::Shared: return StorageDecision{RepKind::Heap, OwnerKind::Shared, ""};
    case PrefixKind::Unique: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
    case PrefixKind::Heap: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
    case PrefixKind::Promote: return StorageDecision{RepKind::Heap, OwnerKind::Unique, ""};
    }
  }
  return StorageDecision{RepKind::Stack, OwnerKind::None, ""};
}

static bool is_explicit_heap_override(const std::vector<PrefixKind>& chain) {
  for (PrefixKind k : chain) {
    if (k == PrefixKind::Shared || k == PrefixKind::Unique || k == PrefixKind::Heap ||
        k == PrefixKind::Promote) {
      return true;
    }
  }
  return false;
}

static StorageDecision infer_expr_storage(const Expr& e, const FuncCtx& ctx,
                                         const std::unordered_map<VarId, StorageDecision>& known) {
  // Directives
  if (std::holds_alternative<Expr::Prefix>(e.node)) {
    std::vector<PrefixKind> chain;
    (void)strip_prefix_chain(e, chain);
    return decision_from_prefix_chain(chain);
  }

  if (std::holds_alternative<Expr::VarRef>(e.node)) {
    const auto& v = std::get<Expr::VarRef>(e.node);
    auto it = known.find(v.var);
    if (it != known.end()) return it->second;
    return StorageDecision{RepKind::Stack, OwnerKind::None, ""};
  }

  if (std::holds_alternative<Expr::Construct>(e.node)) {
    if (!ctx.region_stack.empty()) {
      return StorageDecision{RepKind::Region, OwnerKind::None, ctx.region_stack.back()};
    }
    return StorageDecision{RepKind::Stack, OwnerKind::None, ""};
  }

  // Everything else is stack-like in v0.1.
  return StorageDecision{RepKind::Stack, OwnerKind::None, ""};
}

static void gather_meta_and_initial_decisions_in_block(FuncCtx& ctx, const Block& b,
                                                       std::unordered_map<VarId, StorageDecision>& known);

static void gather_meta_and_initial_decisions_in_stmt(FuncCtx& ctx, const Stmt& s,
                                                      std::unordered_map<VarId, StorageDecision>& known) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Let>) {
          // Dependencies of RHS
          std::unordered_set<VarId> refs;
          collect_var_refs(*st.value, refs);
          ctx.deps[st.var] = refs;
          ctx.meta[st.var].span = s.span;

          // Determine alloc-site marker (Construct in region w/out explicit override).
          std::vector<PrefixKind> chain;
          const Expr* base = strip_prefix_chain(*st.value, chain);
          const bool explicit_override = is_explicit_heap_override(chain);
          const bool is_construct = base && std::holds_alternative<Expr::Construct>(base->node);
          const bool in_region = !ctx.region_stack.empty();
          ctx.meta[st.var].is_region_alloc_site = (in_region && is_construct && !explicit_override);

          // Alias tracking: v0.1 only tracks simple variable aliasing via `let b = a`.
          if (!explicit_override && base && std::holds_alternative<Expr::VarRef>(base->node)) {
            const auto& vr = std::get<Expr::VarRef>(base->node);
            if (vr.var != 0) ctx.alias_of[st.var] = vr.var;
          }

          // Initial decision
          StorageDecision d;
          if (explicit_override) {
            d = decision_from_prefix_chain(chain);
          } else if (is_construct && in_region) {
            d = StorageDecision{RepKind::Region, OwnerKind::None, ctx.region_stack.back()};
          } else {
            d = infer_expr_storage(*st.value, ctx, known);
          }
          known[st.var] = d;
          ctx.out.vars[st.var] = d;
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          std::unordered_set<VarId> refs;
          collect_var_refs(*st.value, refs);
          ctx.deps[st.var] = refs;

          std::vector<PrefixKind> chain;
          const Expr* base = strip_prefix_chain(*st.value, chain);
          const bool explicit_override = is_explicit_heap_override(chain);
          if (!explicit_override && base && std::holds_alternative<Expr::VarRef>(base->node)) {
            const auto& vr = std::get<Expr::VarRef>(base->node);
            if (vr.var != 0) {
              ctx.alias_of[st.var] = vr.var;
            } else {
              ctx.alias_of.erase(st.var);
            }
          } else {
            ctx.alias_of.erase(st.var);
          }

          if (st.var != 0 && known.find(st.var) == known.end()) {
            const StorageDecision d = infer_expr_storage(*st.value, ctx, known);
            known[st.var] = d;
            ctx.out.vars[st.var] = d;
          }
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          // Field assignment can carry call dependencies through RHS.
          std::unordered_set<VarId> refs;
          collect_var_refs(*st.value, refs);
          if (st.base_var != 0) refs.insert(st.base_var);
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          // nothing
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          // nothing here; escape handled in second pass
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          gather_meta_and_initial_decisions_in_block(ctx, st.then_body, known);
          if (st.else_body.has_value()) {
            gather_meta_and_initial_decisions_in_block(ctx, *st.else_body, known);
          }
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          ctx.region_stack.push_back(st.name);
          gather_meta_and_initial_decisions_in_block(ctx, st.body, known);
          ctx.region_stack.pop_back();
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          gather_meta_and_initial_decisions_in_block(ctx, st.body, known);
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          // Loop variable is a binding, but not a heap/region decision.
          known[st.var] = StorageDecision{RepKind::Stack, OwnerKind::None, ""};
          ctx.out.vars[st.var] = StorageDecision{RepKind::Stack, OwnerKind::None, ""};
          gather_meta_and_initial_decisions_in_block(ctx, st.body, known);
        }
      },
      s.node);
}

static void gather_meta_and_initial_decisions_in_block(
    FuncCtx& ctx, const Block& b, std::unordered_map<VarId, StorageDecision>& known) {
  for (const auto& s : b.stmts) {
    gather_meta_and_initial_decisions_in_stmt(ctx, s, known);
  }
}

static void compute_closure_from_seed(
    VarId seed, const std::unordered_map<VarId, std::unordered_set<VarId>>& deps,
    std::unordered_set<VarId>& out) {
  if (seed == 0) return;
  if (!out.insert(seed).second) return;
  auto it = deps.find(seed);
  if (it == deps.end()) return;
  for (VarId d : it->second) compute_closure_from_seed(d, deps, out);
}

static void scan_expr_for_calls_in_region(FuncCtx& ctx, const Expr& e,
                                          std::unordered_map<VarId, EscapeSeedInfo>& seeds) {
  // Only meaningful if inside region.
  if (ctx.region_stack.empty()) return;

  std::visit(
      [&](auto&& n) {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Expr::Call>) {
          std::uint8_t unknown_source_mask = 0;
          const FuncEscapeSummary* summary =
              lookup_summary_for_call(ctx, n, &unknown_source_mask);
          // For each escaping param, mark arg deps as seeds.
          for (std::size_t i = 0; i < n.args.size(); ++i) {
            bool may_escape = false;
            EscapeReason reason = EscapeReason::Call;
            if (summary == nullptr || i >= summary->param_may_escape.size()) {
              may_escape = true;
              reason = EscapeReason::CallUnknown;
            } else if (summary->param_may_escape[i]) {
              may_escape = true;
              if (summary_param_unknown(summary, i)) {
                reason = EscapeReason::CallUnknown;
              }
            }
            if (!may_escape) continue;
            std::unordered_set<VarId> refs;
            collect_var_refs(*n.args[i], refs);
            std::uint8_t seed_unknown_mask = 0;
            if (reason == EscapeReason::CallUnknown) {
              seed_unknown_mask = unknown_source_mask;
              if (seed_unknown_mask == 0) {
                seed_unknown_mask = unknown_source_bit(UnknownReturnPathSource::MissingSummary);
              }
            }
            for (VarId v : refs) mark_seed(seeds, v, reason, seed_unknown_mask);
          }
          // Also scan inside args for nested calls.
          for (const auto& a : n.args) scan_expr_for_calls_in_region(ctx, *a, seeds);
        } else if constexpr (std::is_same_v<N, Expr::Construct>) {
          for (const auto& a : n.args) scan_expr_for_calls_in_region(ctx, *a, seeds);
        } else if constexpr (std::is_same_v<N, Expr::Binary>) {
          scan_expr_for_calls_in_region(ctx, *n.lhs, seeds);
          scan_expr_for_calls_in_region(ctx, *n.rhs, seeds);
        } else if constexpr (std::is_same_v<N, Expr::Unary>) {
          scan_expr_for_calls_in_region(ctx, *n.inner, seeds);
        } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
          scan_expr_for_calls_in_region(ctx, *n.inner, seeds);
        } else {
          // literals/varref: nothing
        }
      },
      e.node);
}

static void gather_escape_seeds(FuncCtx& ctx, const Block& b,
                                std::unordered_map<VarId, EscapeSeedInfo>& seeds);

static void gather_escape_seeds_in_stmt(FuncCtx& ctx, const Stmt& s,
                                        std::unordered_map<VarId, EscapeSeedInfo>& seeds) {
  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Let>) {
          // Calls inside RHS might cause escape of arguments.
          scan_expr_for_calls_in_region(ctx, *st.value, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          scan_expr_for_calls_in_region(ctx, *st.value, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          scan_expr_for_calls_in_region(ctx, *st.value, seeds);
          if (!ctx.region_stack.empty() && st.base_var != 0) {
            mark_seed(seeds, st.base_var, EscapeReason::Field);
          }
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          scan_expr_for_calls_in_region(ctx, *st.expr, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          if (!ctx.region_stack.empty()) {
            // Any vars referenced by return inside region are escaping that region.
            std::unordered_set<VarId> refs;
            collect_var_refs(*st.value, refs);
            for (VarId v : refs) mark_seed(seeds, v, EscapeReason::Return);

            // Cross-function return path: if the return value is a call, use escape summary
            // dependency info to refine which arg paths actually flow to return.
            std::vector<PrefixKind> return_chain;
            const Expr* return_base = strip_prefix_chain(*st.value, return_chain);
            if (return_base && std::holds_alternative<Expr::Call>(return_base->node)) {
              const auto& call = std::get<Expr::Call>(return_base->node);
              std::uint8_t unknown_source_mask = 0;
              const FuncEscapeSummary* summary =
                  lookup_summary_for_call(ctx, call, &unknown_source_mask);
              FuncCtx::ReturnPathEvent event;
              event.unknown = (summary == nullptr);
              event.unknown_source_mask = unknown_source_mask;
              if (summary == nullptr) {
                for (const auto& arg : call.args) {
                  std::unordered_set<VarId> arg_refs;
                  collect_var_refs(*arg, arg_refs);
                  for (VarId v : arg_refs) {
                    mark_seed(seeds, v, EscapeReason::ReturnCallPathUnknown,
                              unknown_source_mask);
                    event.vars.insert(v);
                  }
                }
              } else {
                for (std::size_t i = 0; i < call.args.size(); ++i) {
                  if (i >= summary->return_depends_on_param.size()) continue;
                  if (!summary->return_depends_on_param[i]) continue;
                  std::unordered_set<VarId> arg_refs;
                  collect_var_refs(*call.args[i], arg_refs);
                  const bool unknown_dep = summary_param_unknown(summary, i);
                  if (unknown_dep) {
                    event.unknown = true;
                    event.unknown_source_mask |=
                        unknown_source_bit(UnknownReturnPathSource::MissingSummary);
                  }
                  for (VarId v : arg_refs) {
                    mark_seed(seeds, v,
                              unknown_dep ? EscapeReason::ReturnCallPathUnknown
                                          : EscapeReason::ReturnCallPath,
                              unknown_dep
                                  ? unknown_source_bit(UnknownReturnPathSource::MissingSummary)
                                  : 0);
                    event.vars.insert(v);
                  }
                }
              }
              if (!event.vars.empty()) {
                ctx.return_path_events.push_back(std::move(event));
              }
            }

            // Also handle direct `return Construct(...)` inside region: it implies region allocation escape
            // unless explicit heap/promote/shared/unique is present.
            std::vector<PrefixKind> chain;
            const Expr* base = strip_prefix_chain(*st.value, chain);
            const bool explicit_override = is_explicit_heap_override(chain);
            const bool is_construct = base && std::holds_alternative<Expr::Construct>(base->node);
            if (is_construct && !explicit_override) {
              // No VarId to attach; still emit diag on return span.
              const bool strict = ctx.opt.strict_region;
              const auto sev = apply_warnings_as_errors(
                  ctx, strict ? Severity::Error : Severity::Warning);
              Diagnostic d;
              d.severity = sev;
              d.code = "NBL-R001";
              d.message = "region escape; auto-promoted (direct return of region allocation)";
              d.span = s.span;
              d.machine_reason = "return";
              d.machine_subreason = "direct-construct";
              d.machine_detail = make_reason_detail(d.machine_reason, d.machine_subreason);
              d.machine_trigger_family = d.machine_reason;
              d.machine_trigger_subreason = d.machine_subreason;
              d.machine_owner = "heap-unique";
              d.machine_owner_reason = "single-owner-flow";
              ctx.diags.push_back(std::move(d));
            }
          }

          scan_expr_for_calls_in_region(ctx, *st.value, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          scan_expr_for_calls_in_region(ctx, *st.cond, seeds);
          gather_escape_seeds(ctx, st.then_body, seeds);
          if (st.else_body.has_value()) gather_escape_seeds(ctx, *st.else_body, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          ctx.region_stack.push_back(st.name);
          gather_escape_seeds(ctx, st.body, seeds);
          ctx.region_stack.pop_back();
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          gather_escape_seeds(ctx, st.body, seeds);
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          // range expressions might contain calls
          scan_expr_for_calls_in_region(ctx, *st.start, seeds);
          scan_expr_for_calls_in_region(ctx, *st.end, seeds);
          gather_escape_seeds(ctx, st.body, seeds);
        }
      },
      s.node);
}

static void gather_escape_seeds(FuncCtx& ctx, const Block& b,
                                std::unordered_map<VarId, EscapeSeedInfo>& seeds) {
  for (const auto& s : b.stmts) gather_escape_seeds_in_stmt(ctx, s, seeds);
}

static bool is_region_ptr(const StorageDecision& d) { return d.rep == RepKind::Region; }

static VarId alias_root(const std::unordered_map<VarId, VarId>& alias_of, VarId v) {
  if (v == 0) return v;
  std::unordered_set<VarId> seen;
  VarId cur = v;
  while (cur != 0 && seen.insert(cur).second) {
    auto it = alias_of.find(cur);
    if (it == alias_of.end() || it->second == 0 || it->second == cur) break;
    cur = it->second;
  }
  return cur;
}

} // namespace

RepOwnerResult run_rep_owner_infer(const nebula::nir::Program& p, const EscapeAnalysisResult& escape,
                                  const RepOwnerOptions& opt,
                                  const CallTargetResolution* call_resolution) {
  RepOwnerResult res;
  std::unordered_set<std::string> local_functions;
  local_functions.reserve(p.items.size());
  for (const auto& item : p.items) {
    if (!std::holds_alternative<nebula::nir::Function>(item.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(item.node);
    local_functions.insert(nebula::nir::function_identity(fn));
  }

  // For each function, compute initial decisions + region escape policy.
  for (const auto& it : p.items) {
    if (!std::holds_alternative<nebula::nir::Function>(it.node)) continue;
    const auto& fn = std::get<nebula::nir::Function>(it.node);

    FuncCtx ctx;
    ctx.escape_summaries = &escape.by_function;
    ctx.call_resolution = call_resolution;
    ctx.local_functions = &local_functions;
    ctx.opt = opt;
    ctx.function_name = nebula::nir::function_identity(fn);

    std::unordered_map<VarId, StorageDecision> known;
    known.reserve(64);

    // Seed known vars for params (stack by default; ref does not change storage kind here).
    for (const auto& p : fn.params) {
      StorageDecision d{RepKind::Stack, OwnerKind::None, ""};
      known[p.var] = d;
      ctx.out.vars[p.var] = d;
      ctx.meta[p.var].span = p.span;
    }

    // First pass: gather deps/meta and initial decisions.
    ctx.region_stack.clear();
    if (fn.body.has_value()) {
      gather_meta_and_initial_decisions_in_block(ctx, *fn.body, known);
    }

    // Second pass: detect region escapes (return/call escapes inside region), compute closure,
    // then warn/error and auto-promote.
    std::unordered_map<VarId, EscapeSeedInfo> seeds;
    ctx.region_stack.clear();
    if (fn.body.has_value()) {
      gather_escape_seeds(ctx, *fn.body, seeds);
    }

    std::unordered_set<VarId> closure;
    for (const auto& [v, _] : seeds) compute_closure_from_seed(v, ctx.deps, closure);
    std::unordered_map<VarId, EscapeSeedInfo> closure_reasons;
    auto merge_reason_info = [](EscapeSeedInfo& dst, const EscapeSeedInfo& src) {
      dst.reason_mask |= src.reason_mask;
      dst.return_path_unknown = dst.return_path_unknown || src.return_path_unknown;
      dst.unknown_source_mask |= src.unknown_source_mask;
    };
    auto propagate_seed_reason = [&](auto&& self, VarId cur, const EscapeSeedInfo& info,
                                     std::unordered_set<VarId>& seen) -> void {
      if (cur == 0) return;
      if (!seen.insert(cur).second) return;
      merge_reason_info(closure_reasons[cur], info);
      auto it = ctx.deps.find(cur);
      if (it == ctx.deps.end()) return;
      for (VarId dep : it->second) self(self, dep, info, seen);
    };
    for (const auto& [seed_var, seed_info] : seeds) {
      std::unordered_set<VarId> seen;
      propagate_seed_reason(propagate_seed_reason, seed_var, seed_info, seen);
    }

    std::unordered_map<VarId, std::unordered_set<VarId>> escaping_alias_fanout_vars;
    struct RootEscapeInfo {
      std::uint32_t reason_mask = 0;
      std::size_t max_return_path_root_fanin = 0;
      std::size_t max_return_path_alias_fanout = 0;
      bool return_path_unknown = false;
      std::uint8_t unknown_return_path_source_mask = 0;
      std::uint8_t unknown_call_source_mask = 0;
      bool return_seed_direct_root = false;
      bool return_seed_via_alias = false;
    };
    std::unordered_map<VarId, RootEscapeInfo> root_escape_info;
    std::unordered_map<VarId, std::unordered_set<VarId>> return_path_alias_fanout_vars;
    for (const auto& [var, dec] : ctx.out.vars) {
      if (!is_region_ptr(dec)) continue;
      if (closure.count(var) == 0) continue;
      const VarId root = alias_root(ctx.alias_of, var);
      auto sit = closure_reasons.find(var);
      if (sit == closure_reasons.end()) sit = closure_reasons.find(root);
      if (sit != closure_reasons.end()) {
        auto& info = root_escape_info[root];
        info.reason_mask |= sit->second.reason_mask;
        info.return_path_unknown = info.return_path_unknown || sit->second.return_path_unknown;
        if ((sit->second.reason_mask & reason_bit(EscapeReason::ReturnCallPathUnknown)) != 0) {
          info.unknown_return_path_source_mask |= sit->second.unknown_source_mask;
        }
        if ((sit->second.reason_mask & reason_bit(EscapeReason::CallUnknown)) != 0) {
          info.unknown_call_source_mask |= sit->second.unknown_source_mask;
        }
      }
    }
    for (const auto& event : ctx.return_path_events) {
      std::unordered_set<VarId> escaped_roots;
      std::unordered_map<VarId, std::size_t> per_event_root_alias_count;
      for (VarId v : event.vars) {
        auto vit = ctx.out.vars.find(v);
        if (vit == ctx.out.vars.end()) continue;
        if (!is_region_ptr(vit->second)) continue;
        if (closure.count(v) == 0) continue;
        const VarId root = alias_root(ctx.alias_of, v);
        escaped_roots.insert(root);
        ++per_event_root_alias_count[root];
        return_path_alias_fanout_vars[root].insert(v);
      }
      if (escaped_roots.empty()) continue;
      for (const auto& [root, alias_count] : per_event_root_alias_count) {
        auto& info = root_escape_info[root];
        info.max_return_path_alias_fanout =
            std::max(info.max_return_path_alias_fanout, alias_count);
      }
      for (VarId root : escaped_roots) {
        auto& info = root_escape_info[root];
        info.reason_mask |= reason_bit(event.unknown ? EscapeReason::ReturnCallPathUnknown
                                                     : EscapeReason::ReturnCallPath);
        info.max_return_path_root_fanin =
            std::max(info.max_return_path_root_fanin, escaped_roots.size());
        if (event.unknown) {
          info.return_path_unknown = true;
          info.unknown_return_path_source_mask |= event.unknown_source_mask;
        }
      }
    }
    for (const auto& [seed_var, seed_info] : seeds) {
      auto it = ctx.out.vars.find(seed_var);
      if (it != ctx.out.vars.end() && is_region_ptr(it->second) && closure.count(seed_var) != 0) {
        const VarId root = alias_root(ctx.alias_of, seed_var);
        escaping_alias_fanout_vars[root].insert(seed_var);
      }
      if ((seed_info.reason_mask & reason_bit(EscapeReason::Return)) == 0) continue;
      const VarId root = alias_root(ctx.alias_of, seed_var);
      auto& info = root_escape_info[root];
      if (seed_var == root) {
        info.return_seed_direct_root = true;
      } else {
        info.return_seed_via_alias = true;
      }
    }

    auto root_reason_primary = [](const RootEscapeInfo& info) -> std::string {
      if (info.reason_mask & reason_bit(EscapeReason::Return)) return "return";
      if (info.reason_mask & reason_bit(EscapeReason::Call)) return "call";
      if (info.reason_mask & reason_bit(EscapeReason::CallUnknown)) return "call";
      if (info.reason_mask & reason_bit(EscapeReason::Field)) return "field";
      return "return";
    };
    auto root_trigger_family_detail = [](const RootEscapeInfo& info) -> std::string {
      std::vector<std::string_view> families;
      families.reserve(3);
      if ((info.reason_mask &
           (reason_bit(EscapeReason::Return) | reason_bit(EscapeReason::ReturnCallPath) |
            reason_bit(EscapeReason::ReturnCallPathUnknown))) != 0) {
        families.push_back("return");
      }
      if ((info.reason_mask &
           (reason_bit(EscapeReason::Call) | reason_bit(EscapeReason::CallUnknown))) != 0) {
        families.push_back("call");
      }
      if ((info.reason_mask & reason_bit(EscapeReason::Field)) != 0) {
        families.push_back("field");
      }
      if (families.empty()) {
        families.push_back("return");
      }
      std::string out;
      std::size_t cap = 0;
      for (const auto family : families) cap += family.size();
      if (families.size() > 1) cap += families.size() - 1;
      out.reserve(cap);
      for (std::size_t i = 0; i < families.size(); ++i) {
        if (i != 0) out.push_back('|');
        out.append(families[i]);
      }
      return out;
    };
    auto unknown_return_path_subreason = [](const RootEscapeInfo& info) -> std::string {
      return unknown_owner_reason(info.unknown_return_path_source_mask);
    };
    auto root_reason_subreason = [&](const RootEscapeInfo& info) -> std::string {
      if (info.reason_mask & reason_bit(EscapeReason::ReturnCallPathUnknown)) {
        return unknown_return_path_subreason(info);
      }
      if (info.reason_mask & reason_bit(EscapeReason::ReturnCallPath)) {
        return "cross-function-return-path";
      }
      if (info.reason_mask & reason_bit(EscapeReason::Return)) {
        if (info.return_seed_via_alias && !info.return_seed_direct_root) {
          return "return-via-alias-chain";
        }
        if (info.return_seed_direct_root && info.return_seed_via_alias) {
          return "return-direct-and-alias";
        }
        return "direct-return";
      }
      if (info.reason_mask & reason_bit(EscapeReason::CallUnknown)) {
        return unknown_call_subreason(info.unknown_call_source_mask);
      }
      if (info.reason_mask & reason_bit(EscapeReason::Call)) return "callee-param-escape";
      if (info.reason_mask & reason_bit(EscapeReason::Field)) return "field-write-base-escape";
      return "direct-return";
    };
    auto root_reason_detail = [&](const RootEscapeInfo& info) -> std::string {
      if (info.reason_mask & reason_bit(EscapeReason::ReturnCallPathUnknown)) {
        return unknown_return_path_subreason(info);
      }
      if (info.reason_mask & reason_bit(EscapeReason::ReturnCallPath)) {
        return "cross-function-return-path";
      }
      if (info.reason_mask & reason_bit(EscapeReason::Return)) {
        if (info.return_seed_via_alias && !info.return_seed_direct_root) {
          return "return-via-alias-chain";
        }
        if (info.return_seed_direct_root && info.return_seed_via_alias) {
          return "return-direct-and-alias";
        }
        return "direct-return";
      }
      if (info.reason_mask & reason_bit(EscapeReason::CallUnknown)) {
        return unknown_call_subreason(info.unknown_call_source_mask);
      }
      if (info.reason_mask & reason_bit(EscapeReason::Call)) return "callee-param-escape";
      if (info.reason_mask & reason_bit(EscapeReason::Field)) return "field-write-base-escape";
      return "direct-return";
    };

    // Determine which vars are region pointers and in closure.
    for (auto& [var, dec] : ctx.out.vars) {
      if (!is_region_ptr(dec)) continue;
      if (closure.count(var) == 0) continue;
      const VarId root = alias_root(ctx.alias_of, var);
      const auto fit = escaping_alias_fanout_vars.find(root);
      const std::size_t fanout = (fit == escaping_alias_fanout_vars.end()) ? 1 : fit->second.size();
      const auto rit = root_escape_info.find(root);
      const RootEscapeInfo reason_info = (rit == root_escape_info.end()) ? RootEscapeInfo{} : rit->second;
      const auto rp_fit = return_path_alias_fanout_vars.find(root);
      const std::size_t return_path_alias_fanout =
          (rp_fit == return_path_alias_fanout_vars.end()) ? 0 : rp_fit->second.size();
      const bool has_return_path_reason =
          (reason_info.reason_mask & reason_bit(EscapeReason::ReturnCallPath)) != 0 ||
          (reason_info.reason_mask & reason_bit(EscapeReason::ReturnCallPathUnknown)) != 0;
      const bool fanout_from_return_path_only =
          has_return_path_reason && fanout > 1 && return_path_alias_fanout > 1 &&
          return_path_alias_fanout == fanout;
      const bool fanout_with_return_path_overlap =
          has_return_path_reason && fanout > 1 && return_path_alias_fanout > 1 &&
          return_path_alias_fanout < fanout;
      OwnerKind promoted_owner = OwnerKind::Unique;
      std::string owner_reason = "single-owner-flow";
      if (fanout > 1) {
        promoted_owner = OwnerKind::Shared;
        if (fanout_from_return_path_only) {
          owner_reason = "cross-function-return-path-alias-fanout";
        } else if (fanout_with_return_path_overlap) {
          owner_reason = "cross-function-return-path-alias-fanout-mixed";
        } else {
          owner_reason = "alias-fanout";
        }
      } else if (reason_info.max_return_path_alias_fanout > 1) {
        promoted_owner = OwnerKind::Shared;
        owner_reason = "cross-function-return-path-alias-fanout";
      } else if (reason_info.return_path_unknown) {
        promoted_owner = OwnerKind::Shared;
        owner_reason = unknown_owner_reason(reason_info.unknown_return_path_source_mask);
      } else if (reason_info.max_return_path_root_fanin > 1) {
        promoted_owner = OwnerKind::Shared;
        owner_reason = "cross-function-return-path-fanin";
      }

      // Only emit NBL-R001 at allocation sites (avoid alias spam).
      const auto meta_it = ctx.meta.find(var);
      const bool is_site = (meta_it != ctx.meta.end()) ? meta_it->second.is_region_alloc_site : false;
      if (is_site) {
        const bool strict = opt.strict_region;
        const auto sev =
            apply_warnings_as_errors(ctx, strict ? Severity::Error : Severity::Warning);
        Diagnostic d;
        d.severity = sev;
        d.code = "NBL-R001";
        d.span = meta_it->second.span;
        d.machine_reason = root_reason_primary(reason_info);
        d.machine_subreason = root_reason_subreason(reason_info);
        d.machine_detail = make_reason_detail(d.machine_reason, root_reason_detail(reason_info));
        d.machine_trigger_family = d.machine_reason;
        d.machine_trigger_family_detail = root_trigger_family_detail(reason_info);
        d.machine_trigger_subreason = d.machine_subreason;
        d.machine_owner = (promoted_owner == OwnerKind::Shared) ? "heap-shared" : "heap-unique";
        d.machine_owner_reason = owner_reason;
        if (reason_info.return_path_unknown) {
          d.machine_owner_reason_detail =
              unknown_owner_reason_detail(reason_info.unknown_return_path_source_mask);
        }
        if (owner_reason == "alias-fanout") {
          d.suggestions.push_back(
              "eliminate escaping alias fanout first; keep one owner path to boundary");
        } else if (owner_reason == "cross-function-return-path-alias-fanout-mixed") {
          d.suggestions.push_back(
              "collapse mixed fanout (return-path + non-return-path) to one owner path before unknown-path tuning");
        } else if (owner_reason == "cross-function-return-path-alias-fanout") {
          d.suggestions.push_back(
              "collapse cross-function return aliases to one owner path before tuning unknown paths");
        } else if (owner_reason == "cross-function-return-path-fanin") {
          d.suggestions.push_back(
              "reduce cross-function return fan-in to one escaping root before sharing");
        } else if (owner_reason == "cross-function-return-path-unknown-no-summary") {
          d.suggestions.push_back(
              "add callee return-path summary so owner inference can remain unique when safe");
        } else if (owner_reason == "cross-function-return-path-unknown-external-opaque") {
          d.suggestions.push_back(
              "wrap opaque external return in an explicit ownership boundary contract");
        } else if (owner_reason == "cross-function-return-path-unknown-indirect-unresolved") {
          d.suggestions.push_back(
              "resolve indirect callee target or add a summary stub for this call path");
        } else if (owner_reason == "single-owner-flow") {
          d.suggestions.push_back("keep single-owner flow; share only at explicit boundaries");
        }
        if (promoted_owner == OwnerKind::Shared && owner_reason == "alias-fanout") {
          d.message = "region escape; auto-promoted to heap shared (alias fanout)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-alias-fanout-mixed") {
          d.message =
              "region escape; auto-promoted to heap shared (mixed cross-function return path alias fanout)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-alias-fanout") {
          d.message =
              "region escape; auto-promoted to heap shared (cross-function return path alias fanout)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-fanin") {
          d.message = "region escape; auto-promoted to heap shared (cross-function return path)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-unknown-no-summary") {
          d.message =
              "region escape; auto-promoted to heap shared (cross-function return path unknown: no summary)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-unknown-external-opaque") {
          d.message =
              "region escape; auto-promoted to heap shared (cross-function return path unknown: external opaque)";
        } else if (promoted_owner == OwnerKind::Shared &&
                   owner_reason == "cross-function-return-path-unknown-indirect-unresolved") {
          d.message =
              "region escape; auto-promoted to heap shared (cross-function return path unknown: indirect unresolved)";
        } else {
          d.message = "region escape; auto-promoted to heap unique";
        }
        ctx.diags.push_back(std::move(d));
      }

      // Auto-promote in default mode only (not strict). Choose owner by alias fanout:
      // unique when single-owner flow is provable, shared when aliases escape together.
      if (!opt.strict_region) {
        dec.rep = RepKind::Heap;
        dec.owner = promoted_owner;
        dec.region.clear();
      }
    }

    // Propagate final decisions along simple alias edges (`let b = a`).
    // This keeps `--dump-ownership` and downstream analyses consistent after promotions.
    if (!ctx.alias_of.empty()) {
      std::vector<std::pair<VarId, VarId>> aliases;
      aliases.reserve(ctx.alias_of.size());
      for (const auto& [dst, src] : ctx.alias_of) aliases.push_back({dst, src});
      std::sort(aliases.begin(), aliases.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (const auto& [dst, src] : aliases) {
        auto it_src = ctx.out.vars.find(src);
        auto it_dst = ctx.out.vars.find(dst);
        if (it_src != ctx.out.vars.end() && it_dst != ctx.out.vars.end()) {
          it_dst->second = it_src->second;
        }
      }
    }

    res.by_function.insert({nebula::nir::function_identity(fn), std::move(ctx.out)});
    res.diags.insert(res.diags.end(), ctx.diags.begin(), ctx.diags.end());
  }

  return res;
}

} // namespace nebula::passes
