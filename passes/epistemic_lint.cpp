#include "passes/epistemic_lint.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace nebula::passes {

namespace {

using nebula::frontend::Diagnostic;
using nebula::frontend::Severity;
using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::PrefixKind;
using nebula::nir::Program;
using nebula::nir::Stmt;
using nebula::nir::VarId;
using nebula::passes::StorageDecision;
using nebula::passes::OwnerKind;
using nebula::passes::RepKind;
using nebula::frontend::DiagnosticRisk;

struct FunctionStats {
  int stmt_count = 0;
  int call_count = 0;
  int max_loop_depth = 0;
  bool saw_heap_in_loop = false;
  bool saw_shared_hot = false;
};

struct OwnerInferenceTrace {
  std::string machine_trigger_family;
  std::string machine_trigger_family_detail;
  std::string machine_trigger_subreason;
  std::string machine_subreason;
  std::string machine_detail;
  std::string machine_owner_reason;
  std::string machine_owner_reason_detail;
};

using OwnerTraceIndex = std::unordered_map<std::uint64_t, OwnerInferenceTrace>;

static std::uint64_t span_key(const nebula::frontend::Span& span) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(span.start.line)) << 32) ^
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(span.start.col));
}

static int owner_reason_rank(const std::string& reason) {
  if (reason == "alias-fanout") return 7;
  if (reason == "cross-function-return-path-alias-fanout") return 6;
  if (reason == "cross-function-return-path-alias-fanout-mixed") return 5;
  if (reason == "cross-function-return-path-unknown-indirect-unresolved") return 4;
  if (reason == "cross-function-return-path-unknown-external-opaque") return 3;
  if (reason == "cross-function-return-path-unknown-no-summary") return 2;
  if (reason == "cross-function-return-path-fanin") return 1;
  return 0;
}

static std::string strongest_owner_reason(std::string_view reason, std::string_view detail) {
  std::string out(reason);
  int out_rank = owner_reason_rank(out);
  std::size_t begin = 0;
  while (begin <= detail.size()) {
    const std::size_t sep = detail.find('|', begin);
    const std::size_t end = (sep == std::string::npos) ? detail.size() : sep;
    const std::string part(detail.substr(begin, end - begin));
    if (!part.empty()) {
      const int part_rank = owner_reason_rank(part);
      if (part_rank > out_rank) {
        out = part;
        out_rank = part_rank;
      }
    }
    if (sep == std::string::npos) break;
    begin = sep + 1;
  }
  return out;
}

static std::string merge_owner_reason_detail(std::string_view lhs, std::string_view rhs) {
  std::unordered_set<std::string> seen;
  auto collect = [&](std::string_view src) {
    std::size_t begin = 0;
    while (begin <= src.size()) {
      const std::size_t sep = src.find('|', begin);
      const std::size_t end = (sep == std::string::npos) ? src.size() : sep;
      const std::string part(src.substr(begin, end - begin));
      if (!part.empty()) {
        seen.insert(part);
      }
      if (sep == std::string::npos) break;
      begin = sep + 1;
    }
  };
  collect(lhs);
  collect(rhs);
  if (seen.empty()) return {};
  std::vector<std::string> parts;
  parts.reserve(seen.size());
  for (const auto& part : seen) parts.push_back(part);
  std::sort(parts.begin(), parts.end(), [](const std::string& a, const std::string& b) {
    const int ar = owner_reason_rank(a);
    const int br = owner_reason_rank(b);
    if (ar != br) return ar > br;
    return a < b;
  });
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.push_back('|');
    out += parts[i];
  }
  return out;
}

static OwnerTraceIndex build_owner_trace_index(const RepOwnerResult& rep_owner) {
  OwnerTraceIndex out;
  for (const auto& d : rep_owner.diags) {
    if (d.code != "NBL-R001") continue;
    if (d.machine_owner_reason.empty()) continue;
    const std::uint64_t key = span_key(d.span);
    const std::string next_reason =
        strongest_owner_reason(d.machine_owner_reason, d.machine_owner_reason_detail);
    const int next_rank = owner_reason_rank(next_reason);
    const auto it = out.find(key);
    if (it == out.end()) {
      OwnerInferenceTrace trace;
      trace.machine_trigger_family = d.machine_trigger_family;
      trace.machine_trigger_family_detail = d.machine_trigger_family_detail;
      trace.machine_trigger_subreason = d.machine_trigger_subreason;
      trace.machine_subreason = d.machine_subreason;
      trace.machine_detail = d.machine_detail;
      trace.machine_owner_reason = next_reason;
      trace.machine_owner_reason_detail = d.machine_owner_reason_detail;
      out[key] = std::move(trace);
      continue;
    }
    OwnerInferenceTrace& trace = out[key];
    const int cur_rank = owner_reason_rank(trace.machine_owner_reason);
    if (next_rank >= cur_rank) {
      trace.machine_trigger_family = d.machine_trigger_family;
      trace.machine_trigger_family_detail = d.machine_trigger_family_detail;
      trace.machine_trigger_subreason = d.machine_trigger_subreason;
      trace.machine_subreason = d.machine_subreason;
      trace.machine_detail = d.machine_detail;
      trace.machine_owner_reason = next_reason;
    }
    trace.machine_owner_reason_detail = merge_owner_reason_detail(
        trace.machine_owner_reason_detail, d.machine_owner_reason_detail);
    const std::string merged_best = strongest_owner_reason(
        trace.machine_owner_reason, trace.machine_owner_reason_detail);
    if (!merged_best.empty() && owner_reason_rank(merged_best) > owner_reason_rank(trace.machine_owner_reason)) {
      trace.machine_owner_reason = std::move(merged_best);
    }
  }
  return out;
}

static const OwnerInferenceTrace* lookup_owner_trace(const OwnerTraceIndex& index,
                                                     const nebula::frontend::Span& span) {
  const auto it = index.find(span_key(span));
  if (it == index.end()) return nullptr;
  return &it->second;
}

static bool has_ann(const std::vector<std::string>& ann, const std::string& x) {
  return std::find(ann.begin(), ann.end(), x) != ann.end();
}

static Diagnostic make_diag(Severity sev, std::string code, std::string message,
                            nebula::frontend::Span span, std::string category,
                            DiagnosticRisk risk, std::string cause = "",
                            std::string impact = {},
                            std::vector<std::string> suggestions = {},
                            bool predictive = false,
                            std::optional<double> confidence = std::nullopt) {
  Diagnostic d;
  d.severity = sev;
  d.code = std::move(code);
  d.message = std::move(message);
  d.span = span;
  d.category = std::move(category);
  d.risk = risk;
  d.cause = std::move(cause);
  d.impact = std::move(impact);
  d.suggestions = std::move(suggestions);
  d.predictive = predictive;
  d.confidence = confidence;
  return d;
}

static void maybe_escalate(Diagnostic& d, const EpistemicLintOptions& opt) {
  if (opt.warnings_as_errors && d.severity == Severity::Warning) d.severity = Severity::Error;
}

static const StorageDecision* lookup_decision(const RepOwnerResult& rep_owner, const std::string& fn,
                                              VarId var) {
  auto itf = rep_owner.by_function.find(fn);
  if (itf == rep_owner.by_function.end()) return nullptr;
  auto itv = itf->second.vars.find(var);
  if (itv == itf->second.vars.end()) return nullptr;
  return &itv->second;
}

static const Expr* strip_prefix(const Expr& e, PrefixKind* out_prefix) {
  if (!std::holds_alternative<Expr::Prefix>(e.node)) return &e;
  const auto& p = std::get<Expr::Prefix>(e.node);
  if (out_prefix) *out_prefix = p.kind;
  return p.inner.get();
}

static bool is_construct_like(const Expr& e) {
  PrefixKind pk = PrefixKind::Heap;
  const Expr* base = strip_prefix(e, &pk);
  if (!base) return false;
  return std::holds_alternative<Expr::Construct>(base->node);
}

static bool has_prefix_kind(const Expr& e, PrefixKind want) {
  const Expr* cur = &e;
  while (cur && std::holds_alternative<Expr::Prefix>(cur->node)) {
    const auto& p = std::get<Expr::Prefix>(cur->node);
    if (p.kind == want) return true;
    cur = p.inner.get();
  }
  return false;
}

static bool is_deprecated_api_name(const std::string& name) {
  if (name.rfind("legacy_", 0) == 0) return true;
  if (name.rfind("old_", 0) == 0) return true;
  return name.find("deprecated") != std::string::npos;
}

static void lint_expr_calls(const Expr& e, const EpistemicLintOptions& opt,
                            std::vector<Diagnostic>& out, FunctionStats& stats) {
  std::visit(
      [&](auto&& ex) {
        using E = std::decay_t<decltype(ex)>;
        if constexpr (std::is_same_v<E, Expr::Call>) {
          stats.call_count += 1;
          if (opt.profile == EpistemicLintProfile::Deep &&
              is_deprecated_api_name(ex.callee)) {
            Diagnostic diag = make_diag(
                Severity::Warning, "NBL-A001",
                "deprecated API may reduce optimization quality: " + ex.callee, e.span,
                "api-lifecycle", DiagnosticRisk::Medium,
                "callee is marked by legacy/deprecated naming convention",
                "older API patterns often miss newer inlining/memory improvements",
                {"migrate to a non-legacy API variant when available",
                 "isolate legacy call behind a compatibility wrapper"});
            maybe_escalate(diag, opt);
            out.push_back(std::move(diag));
          }
          for (const auto& a : ex.args) lint_expr_calls(*a, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::FieldRef>) {
          // no nested calls
        } else if constexpr (std::is_same_v<E, Expr::Construct>) {
          for (const auto& a : ex.args) lint_expr_calls(*a, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::TempFieldRef>) {
          lint_expr_calls(*ex.base, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::Binary>) {
          lint_expr_calls(*ex.lhs, opt, out, stats);
          lint_expr_calls(*ex.rhs, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::Unary>) {
          lint_expr_calls(*ex.inner, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::Prefix>) {
          lint_expr_calls(*ex.inner, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::EnumIsVariant>) {
          lint_expr_calls(*ex.subject, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::EnumPayload>) {
          lint_expr_calls(*ex.subject, opt, out, stats);
        } else if constexpr (std::is_same_v<E, Expr::Match>) {
          lint_expr_calls(*ex.subject, opt, out, stats);
          for (const auto& arm : ex.arms) lint_expr_calls(*arm->value, opt, out, stats);
        } else {
        }
      },
      e.node);
}

static void lint_stmt(const RepOwnerResult& rep_owner, const EpistemicLintOptions& opt,
                      const OwnerTraceIndex& owner_traces, const std::string& fn_name,
                      int loop_depth, bool in_hot, const Stmt& s, std::vector<Diagnostic>& out,
                      FunctionStats& stats);

static void lint_block(const RepOwnerResult& rep_owner, const EpistemicLintOptions& opt,
                       const OwnerTraceIndex& owner_traces, const std::string& fn_name,
                       int loop_depth, bool in_hot, const Block& b,
                       std::vector<Diagnostic>& out, FunctionStats& stats) {
  for (const auto& s : b.stmts) {
    lint_stmt(rep_owner, opt, owner_traces, fn_name, loop_depth, in_hot, s, out, stats);
  }
}

static void lint_stmt(const RepOwnerResult& rep_owner, const EpistemicLintOptions& opt,
                      const OwnerTraceIndex& owner_traces, const std::string& fn_name,
                      int loop_depth, bool in_hot, const Stmt& s, std::vector<Diagnostic>& out,
                      FunctionStats& stats) {
  const bool in_loop = loop_depth > 0;
  const bool hot_here = in_hot || has_ann(s.annotations, "hot");
  stats.stmt_count += 1;
  stats.max_loop_depth = std::max(stats.max_loop_depth, loop_depth);

  std::visit(
      [&](auto&& st) {
        using S = std::decay_t<decltype(st)>;
        if constexpr (std::is_same_v<S, Stmt::Declare>) {
        } else if constexpr (std::is_same_v<S, Stmt::Let>) {
          lint_expr_calls(*st.value, opt, out, stats);
          const StorageDecision* d = lookup_decision(rep_owner, fn_name, st.var);
          const RepKind rep = d ? d->rep : RepKind::Stack;
          const OwnerKind owner = d ? d->owner : OwnerKind::None;
          const bool alloc_site = is_construct_like(*st.value);
          const OwnerInferenceTrace* owner_trace = lookup_owner_trace(owner_traces, s.span);

          // NBL-P001: heap allocation in loop
          if (in_loop && alloc_site && rep == RepKind::Heap) {
            Diagnostic diag = make_diag(
                Severity::Warning, "NBL-P001", "heap allocation inside loop", s.span,
                "performance", DiagnosticRisk::Medium,
                "constructor call in loop lowered to heap allocation",
                "repeated heap traffic can increase latency variance",
                {"hoist allocation outside loop when possible",
                 "prefer region allocation or stack value if non-escaping"});
            maybe_escalate(diag, opt);
            out.push_back(std::move(diag));
            stats.saw_heap_in_loop = true;
          }

          // NBL-P010: shared_ptr in hot path
          if (hot_here && alloc_site && rep == RepKind::Heap && owner == OwnerKind::Shared) {
            Diagnostic diag = make_diag(
                Severity::Warning, "NBL-P010", "shared_ptr in @hot path", s.span,
                "performance", DiagnosticRisk::High,
                "hot path allocation uses shared ownership",
                "reference counting in tight paths can hurt throughput and tail latency",
                {"prefer unique ownership in hot path and share only at boundaries",
                 "if safe, use region/stack storage for transient values"});
            if (owner_trace != nullptr) {
              diag.machine_reason = "caused-by-owner-inference";
              diag.machine_trigger_family = owner_trace->machine_trigger_family;
              diag.machine_trigger_family_detail = owner_trace->machine_trigger_family_detail;
              diag.machine_trigger_subreason = owner_trace->machine_trigger_subreason;
              diag.machine_subreason = owner_trace->machine_subreason;
              diag.machine_detail = owner_trace->machine_detail;
              diag.machine_owner = "heap-shared";
              diag.machine_owner_reason = owner_trace->machine_owner_reason;
              diag.machine_owner_reason_detail = owner_trace->machine_owner_reason_detail;
              diag.caused_by_code = "NBL-R001";
            }
            maybe_escalate(diag, opt);
            out.push_back(std::move(diag));
            stats.saw_shared_hot = true;
          }

          // NBL-X003: inferred shared ownership in hot path.
          // This closes the minimum epistemic loop: rep×owner inference -> lint feedback.
          const bool explicit_shared = has_prefix_kind(*st.value, PrefixKind::Shared);
          if (hot_here && alloc_site && rep == RepKind::Heap && owner == OwnerKind::Shared &&
              !explicit_shared) {
            double confidence = 0.72;
            if (owner_trace != nullptr) {
              if (owner_trace->machine_owner_reason == "alias-fanout") {
                confidence = 0.82;
              } else if (owner_trace->machine_owner_reason ==
                         "cross-function-return-path-alias-fanout") {
                confidence = 0.80;
              } else if (owner_trace->machine_owner_reason ==
                         "cross-function-return-path-unknown-indirect-unresolved") {
                confidence = 0.79;
              } else if (owner_trace->machine_owner_reason ==
                         "cross-function-return-path-unknown-external-opaque") {
                confidence = 0.77;
              } else if (owner_trace->machine_owner_reason ==
                         "cross-function-return-path-unknown-no-summary") {
                confidence = 0.75;
              } else if (owner_trace->machine_owner_reason == "cross-function-return-path-fanin") {
                confidence = 0.74;
              }
            }
            Diagnostic diag = make_diag(
                Severity::Note, "NBL-X003",
                "predictive note: shared ownership in @hot path was inferred, not explicit",
                s.span, "predictive", DiagnosticRisk::Medium,
                "rep×owner inference promoted/selected shared ownership for this allocation",
                "throughput may regress if this path is expected to remain single-owner",
                {"if ownership is truly shared, keep as-is and document boundary intent",
                 "if single-owner in practice, force explicit unique/promotion earlier"},
                true, confidence);
            diag.machine_reason = "predictive-followup";
            if (owner_trace != nullptr) {
              diag.machine_trigger_family = owner_trace->machine_trigger_family;
              diag.machine_trigger_family_detail = owner_trace->machine_trigger_family_detail;
              diag.machine_trigger_subreason = owner_trace->machine_trigger_subreason;
            }
            diag.machine_subreason = "inferred-shared-hot-path";
            diag.machine_detail = "inferred-shared-hot-path";
            diag.machine_owner = "heap-shared";
            if (owner_trace != nullptr) {
              diag.machine_owner_reason = owner_trace->machine_owner_reason;
              diag.machine_owner_reason_detail = owner_trace->machine_owner_reason_detail;
            }
            diag.caused_by_code = "NBL-P010";
            out.push_back(std::move(diag));
          }
        } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
          lint_expr_calls(*st.value, opt, out, stats);
        } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
          lint_expr_calls(*st.value, opt, out, stats);
        } else if constexpr (std::is_same_v<S, Stmt::For>) {
          lint_expr_calls(*st.start, opt, out, stats);
          lint_expr_calls(*st.end, opt, out, stats);
          // Loop body counts as in_loop, and hot scope propagates.
          lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth + 1, hot_here, st.body, out,
                     stats);
        } else if constexpr (std::is_same_v<S, Stmt::While>) {
          lint_expr_calls(*st.cond, opt, out, stats);
          lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth + 1, hot_here, st.body, out,
                     stats);
        } else if constexpr (std::is_same_v<S, Stmt::Break> ||
                             std::is_same_v<S, Stmt::Continue>) {
          // no-op
        } else if constexpr (std::is_same_v<S, Stmt::Region>) {
          lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth, hot_here, st.body, out,
                     stats);
        } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
          lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth, hot_here, st.body, out,
                     stats);
        } else if constexpr (std::is_same_v<S, Stmt::If>) {
          lint_expr_calls(*st.cond, opt, out, stats);
          lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth, hot_here, st.then_body, out,
                     stats);
          if (st.else_body.has_value()) {
            lint_block(rep_owner, opt, owner_traces, fn_name, loop_depth, hot_here, *st.else_body, out,
                       stats);
          }
        } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
          lint_expr_calls(*st.expr, opt, out, stats);
        } else if constexpr (std::is_same_v<S, Stmt::Return>) {
          lint_expr_calls(*st.value, opt, out, stats);
        }
      },
      s.node);
}

static void lint_function(const Program& p, const RepOwnerResult& rep_owner,
                          const EpistemicLintOptions& opt, const OwnerTraceIndex& owner_traces,
                          const Function& fn,
                          std::vector<Diagnostic>& out) {
  (void)p;
  const bool hot = has_ann(fn.annotations, "hot");
  FunctionStats stats;
  if (fn.body.has_value()) {
    lint_block(rep_owner, opt, owner_traces, nebula::nir::function_identity(fn), /*loop_depth=*/0,
               /*in_hot=*/hot, *fn.body,
               out, stats);
  }

  // NBL-C010: explicit style-drift marker.
  // Keep this active in fast/deep to provide a mid-weight gate candidate for run preflight.
  if (has_ann(fn.annotations, "style_drift")) {
    Diagnostic diag = make_diag(
        Severity::Warning, "NBL-C010",
        "explicit style-drift marker; review local convention rationale",
        fn.span, "complexity", DiagnosticRisk::Medium,
        "function is annotated with @style_drift",
        "intended convention deviations should remain explicit and documented",
        {"remove @style_drift after convergence to local style",
         "or document why deviation is intentional for this function"});
    maybe_escalate(diag, opt);
    out.push_back(std::move(diag));
  }

  if (opt.profile == EpistemicLintProfile::Deep) {
    const bool complex = stats.stmt_count >= 24 || stats.max_loop_depth >= 3 || stats.call_count >= 20;
    if (complex) {
      Diagnostic diag = make_diag(
          Severity::Warning, "NBL-C001", "function structure is complex and may drift from best practice",
          fn.span, "complexity", DiagnosticRisk::Medium,
          "statement/call volume exceeded the current complexity threshold",
          "larger control-flow surfaces can reduce readability and optimization predictability",
          {"extract helper functions for nested regions/loops",
           "separate data construction from hot execution path"});
      maybe_escalate(diag, opt);
      out.push_back(std::move(diag));
    }
  }

  if (stats.saw_shared_hot || (opt.profile == EpistemicLintProfile::Deep && stats.saw_heap_in_loop)) {
    Diagnostic diag = make_diag(
        Severity::Warning, "NBL-X001",
        "predictive warning: likely performance regression in release build",
        fn.span, "predictive", DiagnosticRisk::High,
        "hot-path ownership/allocation pattern matches known high-latency signatures",
        "release binaries may show elevated p99 latency under load",
        {"switch to unique or region-backed allocation in hot loop segments",
         "move shared ownership edges to cold-path boundaries"},
        true, 0.85);
    maybe_escalate(diag, opt);
    out.push_back(std::move(diag));
  } else if (opt.profile == EpistemicLintProfile::Deep && stats.stmt_count >= 30) {
    Diagnostic diag = make_diag(
        Severity::Note, "NBL-X002",
        "predictive note: refactoring may improve compile-time and runtime stability",
        fn.span, "predictive", DiagnosticRisk::Medium,
        "complexity trend indicates increasing optimization pressure",
        "may not fail now, but future edits could trigger hard performance warnings",
        {"track function complexity trend in CI",
         "consider splitting function before adding more nested logic"},
        true, 0.60);
    out.push_back(std::move(diag));
  }
}

} // namespace

std::vector<Diagnostic> run_epistemic_lint(const Program& p, const RepOwnerResult& rep_owner,
                                          const EpistemicLintOptions& opt) {
  std::vector<Diagnostic> out;
  const OwnerTraceIndex owner_traces = build_owner_trace_index(rep_owner);
  for (const auto& it : p.items) {
    if (!std::holds_alternative<Function>(it.node)) continue;
    const auto& fn = std::get<Function>(it.node);
    lint_function(p, rep_owner, opt, owner_traces, fn, out);
  }
  return out;
}

} // namespace nebula::passes
