#include "passes/borrow_xstmt.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nebula::passes {

namespace {

using nebula::frontend::Diagnostic;
using nebula::frontend::DiagnosticRisk;
using nebula::frontend::Severity;
using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Stmt;
using nebula::nir::VarId;

using BorrowTokenId = std::uint32_t;

enum class BorrowOriginKind : std::uint8_t {
  KnownMayEscape,
  UnknownUnsafeBoundary,
  TargetUnknown,
  UnknownExternal,
  SummaryUnknown,
  UnknownOrigin
};

enum class AccessKind : std::uint8_t { Read, Write, RefBorrow };

struct AliasKey {
  enum class Kind : std::uint8_t { Whole, Field };

  VarId base_var{};
  std::string base_name;
  Kind kind = Kind::Whole;
  std::string field;
};

struct VarOrigin {
  std::unordered_set<BorrowTokenId> token_ids;
  bool has_unknown_origin = false;
};

struct ActiveBorrow {
  BorrowTokenId token_id = 0;
  AliasKey key;
  nebula::frontend::Span first_span{};
  BorrowOriginKind origin_kind = BorrowOriginKind::SummaryUnknown;
  std::string callee;
};

struct AccessRef {
  std::optional<AliasKey> alias;
  VarOrigin origin;
};

struct ConflictMatch {
  const ActiveBorrow* active = nullptr;
  std::string overlap;
  bool via_alias = false;
  bool via_token = false;
  bool via_unknown_origin = false;
};

struct DirectPersistDecision {
  bool persist = false;
  BorrowOriginKind origin_kind = BorrowOriginKind::KnownMayEscape;
};

static std::string alias_to_string(const AliasKey& key) {
  if (key.kind == AliasKey::Kind::Field) return key.base_name + "." + key.field;
  return key.base_name;
}

static bool field_path_overlaps(std::string_view lhs, std::string_view rhs) {
  if (lhs == rhs) return true;
  if (lhs.size() < rhs.size()) {
    return rhs.substr(0, lhs.size()) == lhs && rhs[lhs.size()] == '.';
  }
  if (rhs.size() < lhs.size()) {
    return lhs.substr(0, rhs.size()) == rhs && lhs[rhs.size()] == '.';
  }
  return false;
}

static bool alias_overlaps(const AliasKey& lhs, const AliasKey& rhs) {
  if (lhs.base_var == 0 || rhs.base_var == 0) return false;
  if (lhs.base_var != rhs.base_var) return false;
  if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return true;
  return field_path_overlaps(lhs.field, rhs.field);
}

static std::string overlap_label(const AliasKey& lhs, const AliasKey& rhs) {
  if (lhs.base_var != rhs.base_var) return alias_to_string(lhs);
  if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return lhs.base_name;
  if (lhs.field == rhs.field) return lhs.base_name + "." + lhs.field;
  if (lhs.field.size() < rhs.field.size() && field_path_overlaps(lhs.field, rhs.field)) {
    return lhs.base_name + "." + lhs.field;
  }
  if (rhs.field.size() < lhs.field.size() && field_path_overlaps(lhs.field, rhs.field)) {
    return rhs.base_name + "." + rhs.field;
  }
  return lhs.base_name + "." + lhs.field;
}

static std::string origin_reason(BorrowOriginKind origin_kind) {
  switch (origin_kind) {
  case BorrowOriginKind::KnownMayEscape:
    return "callee summary marks this ref parameter as MayEscape";
  case BorrowOriginKind::UnknownUnsafeBoundary:
    return "callee is @unsafe and treated conservatively";
  case BorrowOriginKind::TargetUnknown:
    return "call target cannot be resolved for this indirect callable";
  case BorrowOriginKind::UnknownExternal:
    return "callee summary is unavailable (external or unresolved)";
  case BorrowOriginKind::SummaryUnknown:
    return "callee escape precision is unknown";
  case BorrowOriginKind::UnknownOrigin:
    return "value origin chain cannot be fully resolved";
  }
  return "unknown borrow origin";
}

class FunctionAnalyzer {
public:
  FunctionAnalyzer(const Function& fn,
                   const EscapeAnalysisResult& escape,
                   const std::unordered_map<std::string, const Function*>& functions,
                   const CallTargetResolution& resolution)
      : fn_(fn), escape_(escape), functions_(functions), resolution_(resolution) {}

  std::vector<Diagnostic> run() {
    if (fn_.body.has_value()) analyze_block(*fn_.body, false);
    return std::move(diags_);
  }

private:
  const Function& fn_;
  const EscapeAnalysisResult& escape_;
  const std::unordered_map<std::string, const Function*>& functions_;
  const CallTargetResolution& resolution_;

  std::vector<Diagnostic> diags_;
  std::unordered_map<VarId, VarOrigin> var_origins_;
  std::vector<std::vector<ActiveBorrow>> active_frames_;
  BorrowTokenId next_token_id_ = 1;

  void merge_origin(VarOrigin& dst, const VarOrigin& src) const {
    dst.has_unknown_origin = dst.has_unknown_origin || src.has_unknown_origin;
    dst.token_ids.insert(src.token_ids.begin(), src.token_ids.end());
  }

  VarOrigin origin_for_assignment_rhs(const Expr& e) const {
    return std::visit(
        [&](auto&& n) -> VarOrigin {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::VarRef>) {
            return origin_for_var(n.var);
          } else {
            VarOrigin out = origin_from_expr(e);
            out.token_ids.clear();
            return out;
          }
        },
        e.node);
  }

  VarOrigin origin_for_var(VarId var) const {
    if (var == 0) return {};
    auto it = var_origins_.find(var);
    if (it == var_origins_.end()) return {};
    return it->second;
  }

  std::optional<AliasKey> alias_for_var(VarId var, const std::string& name) const {
    if (var == 0) return std::nullopt;
    AliasKey key;
    key.base_var = var;
    key.base_name = name;
    key.kind = AliasKey::Kind::Whole;
    return key;
  }

  std::optional<AliasKey> alias_for_field(VarId base_var,
                                          const std::string& base_name,
                                          const std::string& field) const {
    if (base_var == 0) return std::nullopt;
    AliasKey key;
    key.base_var = base_var;
    key.base_name = base_name;
    key.kind = AliasKey::Kind::Field;
    key.field = field;
    return key;
  }

  std::optional<AliasKey> alias_from_expr(const Expr& e) const {
    return std::visit(
        [&](auto&& n) -> std::optional<AliasKey> {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::VarRef>) {
            return alias_for_var(n.var, n.name);
          } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
            return alias_for_field(n.base_var, n.base_name, n.field);
          } else {
            return std::nullopt;
          }
        },
        e.node);
  }

  VarOrigin origin_from_expr(const Expr& e) const {
    return std::visit(
        [&](auto&& n) -> VarOrigin {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::IntLit> || std::is_same_v<N, Expr::BoolLit> ||
                        std::is_same_v<N, Expr::FloatLit> || std::is_same_v<N, Expr::StringLit>) {
            return {};
          } else if constexpr (std::is_same_v<N, Expr::VarRef>) {
            return origin_for_var(n.var);
          } else if constexpr (std::is_same_v<N, Expr::FieldRef>) {
            return origin_for_var(n.base_var);
          } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
            return origin_from_expr(*n.base);
          } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
            return origin_from_expr(*n.subject);
          } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
            return origin_from_expr(*n.subject);
          } else if constexpr (std::is_same_v<N, Expr::Binary>) {
            VarOrigin out = origin_from_expr(*n.lhs);
            merge_origin(out, origin_from_expr(*n.rhs));
            return out;
          } else if constexpr (std::is_same_v<N, Expr::Unary>) {
            return origin_from_expr(*n.inner);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            return origin_from_expr(*n.inner);
          } else if constexpr (std::is_same_v<N, Expr::Construct>) {
            VarOrigin out;
            for (const auto& a : n.args) merge_origin(out, origin_from_expr(*a));
            return out;
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            VarOrigin out;
            const auto target = resolve_call_target(n);
            if (!target.is_known()) {
              out.has_unknown_origin = true;
              return out;
            }

            auto sit = escape_.by_function.find(target.callee);
            if (sit == escape_.by_function.end()) {
              out.has_unknown_origin = true;
              return out;
            }

            const auto& s = sit->second;
            for (std::size_t i = 0; i < n.args.size() && i < s.return_depends_on_param.size(); ++i) {
              if (!s.return_depends_on_param[i]) continue;
              const auto arg_origin = origin_from_expr(*n.args[i]);
              out.has_unknown_origin = out.has_unknown_origin || arg_origin.has_unknown_origin;
            }
            if (is_unsafe_function(target.callee)) {
              out.has_unknown_origin = true;
            }
            return out;
          } else {
            return {};
          }
        },
        e.node);
  }

  AccessRef access_from_expr(const Expr& e) const {
    AccessRef out;
    out.alias = alias_from_expr(e);
    if (out.alias.has_value()) {
      if (out.alias->kind == AliasKey::Kind::Whole) {
        out.origin = origin_for_var(out.alias->base_var);
      } else {
        out.origin = {};
      }
      return out;
    }
    out.origin = origin_from_expr(e);
    return out;
  }

  void push_frame() { active_frames_.push_back({}); }

  void pop_frame(bool propagate_to_parent) {
    if (active_frames_.empty()) return;
    auto frame = std::move(active_frames_.back());
    active_frames_.pop_back();
    if (!propagate_to_parent || active_frames_.empty()) return;
    auto& parent = active_frames_.back();
    parent.reserve(parent.size() + frame.size());
    for (auto& b : frame) parent.push_back(std::move(b));
  }

  std::optional<ConflictMatch> find_conflict(const AccessRef& access,
                                             bool allow_unknown_origin) const {
    if (active_frames_.empty()) return std::nullopt;
    const bool has_alias = access.alias.has_value();
    const bool has_token = !access.origin.token_ids.empty();
    const bool has_unknown_origin = allow_unknown_origin && access.origin.has_unknown_origin;
    if (!has_alias && !has_token && !has_unknown_origin) return std::nullopt;

    for (auto fit = active_frames_.rbegin(); fit != active_frames_.rend(); ++fit) {
      for (auto bit = fit->rbegin(); bit != fit->rend(); ++bit) {
        const ActiveBorrow& active = *bit;
        if (has_alias && alias_overlaps(active.key, *access.alias)) {
          return ConflictMatch{&active, overlap_label(active.key, *access.alias), true, false, false};
        }
        if (has_token && access.origin.token_ids.find(active.token_id) != access.origin.token_ids.end()) {
          return ConflictMatch{&active, alias_to_string(active.key), false, true, false};
        }
        if (has_unknown_origin) {
          return ConflictMatch{&active, alias_to_string(active.key), false, false, true};
        }
      }
    }
    return std::nullopt;
  }

  std::string diag_code_for(AccessKind kind) const {
    if (kind == AccessKind::Read) return "NBL-T093";
    if (kind == AccessKind::Write) return "NBL-T094";
    return "NBL-T095";
  }

  void emit_conflict_diag(AccessKind kind,
                          const ConflictMatch& conflict,
                          nebula::frontend::Span conflict_span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = diag_code_for(kind);
    d.span = conflict_span;
    d.category = "typecheck";
    d.risk = DiagnosticRisk::High;
    d.related_spans.push_back(conflict.active->first_span);
    d.related_spans.push_back(conflict_span);

    if (kind == AccessKind::Read) {
      d.message = "cross-statement borrow conflict on `" + conflict.overlap + "` during read access";
      d.impact = "exclusive mutable borrow is violated across statement boundaries";
      d.suggestions = {"move the read before the ref-taking call",
                       "or isolate the ref-taking operation in a separate block"};
    } else if (kind == AccessKind::Write) {
      d.message = "cross-statement borrow conflict on `" + conflict.overlap + "` during write access";
      d.impact = "exclusive mutable borrow is violated by overlapping writes";
      d.suggestions = {"perform this write before the ref-taking call",
                       "or end the borrow by splitting into separate blocks"};
    } else {
      d.message = "cross-statement borrow conflict on `" + conflict.overlap + "` during ref borrow";
      d.impact = "exclusive mutable borrow is violated by overlapping borrow windows";
      d.suggestions = {"end the earlier borrow before taking another ref borrow",
                       "or borrow disjoint storage roots"};
    }

    if (conflict.via_alias) {
      d.cause =
          "later-statement access overlaps an active ref borrow from an earlier statement on the same alias location";
    } else if (conflict.via_token) {
      d.cause = "later-statement access derives from the same borrow token created by an earlier ref borrow";
    } else {
      d.cause = "analysis cannot prove origin is disjoint from an active ref borrow (" +
                origin_reason(conflict.active->origin_kind) + ")";
    }

    diags_.push_back(std::move(d));
  }

  bool check_conflict(AccessKind kind, const AccessRef& access, nebula::frontend::Span span) {
    const bool allow_unknown_origin = kind == AccessKind::RefBorrow;
    auto conflict = find_conflict(access, allow_unknown_origin);
    if (!conflict.has_value()) return false;
    emit_conflict_diag(kind, *conflict, span);
    return true;
  }

  bool is_unsafe_function(const std::string& callee) const {
    auto it = functions_.find(callee);
    if (it == functions_.end()) return false;
    const auto& anns = it->second->annotations;
    return std::find(anns.begin(), anns.end(), "unsafe") != anns.end();
  }

  ResolvedTargetState resolve_call_target(const Expr::Call& call) const {
    if (call.kind == nebula::nir::CallKind::Direct) {
      return ResolvedTargetState::known(nebula::nir::call_target_identity(call));
    }
    return resolution_.resolve(nebula::nir::function_identity(fn_), &call);
  }

  DirectPersistDecision classify_call_persist(const Expr::Call& call, std::size_t arg_index) const {
    DirectPersistDecision out;
    if (arg_index >= call.args_ref.size() || !call.args_ref[arg_index]) return out;
    if (arg_index >= call.args.size() || !call.args[arg_index]) return out;

    auto target = resolve_call_target(call);
    if (!target.is_known()) {
      out.persist = true;
      out.origin_kind = BorrowOriginKind::TargetUnknown;
      return out;
    }

    if (is_unsafe_function(target.callee)) {
      out.persist = true;
      out.origin_kind = BorrowOriginKind::UnknownUnsafeBoundary;
      return out;
    }

    auto sit = escape_.by_function.find(target.callee);
    if (sit == escape_.by_function.end()) {
      out.persist = true;
      out.origin_kind = BorrowOriginKind::UnknownExternal;
      return out;
    }

    const auto& s = sit->second;
    const auto& arg_ty = call.args[arg_index]->ty;
    const bool scalar_copy_arg = arg_ty.kind == nebula::frontend::Ty::Kind::Int ||
                                 arg_ty.kind == nebula::frontend::Ty::Kind::Bool ||
                                 arg_ty.kind == nebula::frontend::Ty::Kind::Float ||
                                 arg_ty.kind == nebula::frontend::Ty::Kind::String ||
                                 arg_ty.kind == nebula::frontend::Ty::Kind::Void;
    if (scalar_copy_arg) return out;
    const bool may_escape =
        (arg_index < s.param_may_escape.size()) ? s.param_may_escape[arg_index] : true;
    const bool unknown =
        (arg_index < s.param_escape_unknown.size()) ? s.param_escape_unknown[arg_index] : true;
    if (!(may_escape || unknown)) return out;
    out.persist = true;
    out.origin_kind = unknown ? BorrowOriginKind::SummaryUnknown : BorrowOriginKind::KnownMayEscape;
    return out;
  }

  void commit_pending(std::vector<ActiveBorrow>& pending) {
    if (pending.empty()) return;
    if (active_frames_.empty()) push_frame();
    auto& frame = active_frames_.back();
    frame.reserve(frame.size() + pending.size());
    for (auto& b : pending) {
      if (b.key.base_var != 0) var_origins_[b.key.base_var].token_ids.insert(b.token_id);
      frame.push_back(std::move(b));
    }
    pending.clear();
  }

  void analyze_call(const Expr::Call& call, std::vector<ActiveBorrow>& pending) {
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      const bool wants_ref = (i < call.args_ref.size()) && call.args_ref[i];
      const Expr& arg = *call.args[i];
      if (wants_ref) {
        AccessRef access = access_from_expr(arg);
        (void)check_conflict(AccessKind::RefBorrow, access, arg.span);
      } else {
        analyze_expr(arg, pending);
      }
    }

    const auto resolved_target = resolve_call_target(call);

    for (std::size_t i = 0; i < call.args.size(); ++i) {
      auto persist = classify_call_persist(call, i);
      if (!persist.persist) continue;
      auto alias = alias_from_expr(*call.args[i]);
      if (!alias.has_value()) continue;

      ActiveBorrow borrow;
      borrow.token_id = next_token_id_++;
      borrow.key = *alias;
      borrow.first_span = call.args[i]->span;
      borrow.origin_kind = persist.origin_kind;
      borrow.callee = resolved_target.is_known() ? resolved_target.callee
                                                 : nebula::nir::call_target_identity(call);
      pending.push_back(std::move(borrow));
    }
  }

  void analyze_expr(const Expr& e, std::vector<ActiveBorrow>& pending) {
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::VarRef> || std::is_same_v<N, Expr::FieldRef>) {
            AccessRef access = access_from_expr(e);
            (void)check_conflict(AccessKind::Read, access, e.span);
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            analyze_call(n, pending);
          } else if constexpr (std::is_same_v<N, Expr::TempFieldRef>) {
            analyze_expr(*n.base, pending);
          } else if constexpr (std::is_same_v<N, Expr::Construct>) {
            for (const auto& a : n.args) analyze_expr(*a, pending);
          } else if constexpr (std::is_same_v<N, Expr::Binary>) {
            analyze_expr(*n.lhs, pending);
            analyze_expr(*n.rhs, pending);
          } else if constexpr (std::is_same_v<N, Expr::Unary>) {
            analyze_expr(*n.inner, pending);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            analyze_expr(*n.inner, pending);
          } else if constexpr (std::is_same_v<N, Expr::EnumIsVariant>) {
            analyze_expr(*n.subject, pending);
          } else if constexpr (std::is_same_v<N, Expr::EnumPayload>) {
            analyze_expr(*n.subject, pending);
          } else if constexpr (std::is_same_v<N, Expr::Match>) {
            analyze_expr(*n.subject, pending);
            for (const auto& arm : n.arms) analyze_expr(*arm->value, pending);
          } else {
            // literals
          }
        },
        e.node);
  }

  void analyze_stmt(const Stmt& s) {
    std::vector<ActiveBorrow> pending;

    std::visit(
        [&](auto&& st) {
          using S = std::decay_t<decltype(st)>;
          if constexpr (std::is_same_v<S, Stmt::Declare>) {
            var_origins_.erase(st.var);
          } else if constexpr (std::is_same_v<S, Stmt::Let>) {
            analyze_expr(*st.value, pending);
            var_origins_[st.var] = origin_for_assignment_rhs(*st.value);
          } else if constexpr (std::is_same_v<S, Stmt::AssignVar>) {
            analyze_expr(*st.value, pending);
            AccessRef lhs;
            lhs.alias = alias_for_var(st.var, st.name);
            lhs.origin = origin_for_var(st.var);
            (void)check_conflict(AccessKind::Write, lhs, s.span);
            var_origins_[st.var] = origin_for_assignment_rhs(*st.value);
          } else if constexpr (std::is_same_v<S, Stmt::AssignField>) {
            analyze_expr(*st.value, pending);
            AccessRef lhs;
            lhs.alias = alias_for_field(st.base_var, st.base_name, st.field);
            lhs.origin = origin_for_var(st.base_var);
            (void)check_conflict(AccessKind::Write, lhs, s.span);
          } else if constexpr (std::is_same_v<S, Stmt::ExprStmt>) {
            analyze_expr(*st.expr, pending);
          } else if constexpr (std::is_same_v<S, Stmt::Return>) {
            analyze_expr(*st.value, pending);
          } else if constexpr (std::is_same_v<S, Stmt::If>) {
            analyze_expr(*st.cond, pending);
            analyze_block(st.then_body, false);
            if (st.else_body.has_value()) analyze_block(*st.else_body, false);
          } else if constexpr (std::is_same_v<S, Stmt::Region>) {
            analyze_block(st.body, false);
          } else if constexpr (std::is_same_v<S, Stmt::Unsafe>) {
            analyze_block(st.body, false);
          } else if constexpr (std::is_same_v<S, Stmt::For>) {
            analyze_expr(*st.start, pending);
            analyze_expr(*st.end, pending);
            var_origins_.erase(st.var);
            analyze_block(st.body, true);
          } else if constexpr (std::is_same_v<S, Stmt::While>) {
            analyze_expr(*st.cond, pending);
            analyze_block(st.body, true);
          } else if constexpr (std::is_same_v<S, Stmt::Break> ||
                               std::is_same_v<S, Stmt::Continue>) {
            // no-op
          }
        },
        s.node);

    commit_pending(pending);
  }

  void analyze_block(const Block& b, bool propagate_to_parent) {
    push_frame();
    for (const auto& s : b.stmts) analyze_stmt(s);
    pop_frame(propagate_to_parent);
  }
};

} // namespace

BorrowXStmtResult run_borrow_xstmt(const nebula::nir::Program& p,
                                   const EscapeAnalysisResult& escape,
                                   const CallTargetResolution& resolution) {
  std::unordered_map<std::string, const Function*> functions;
  for (const auto& item : p.items) {
    if (!std::holds_alternative<Function>(item.node)) continue;
    const auto& fn = std::get<Function>(item.node);
    functions.insert({nebula::nir::function_identity(fn), &fn});
  }

  BorrowXStmtResult out;
  for (const auto& item : p.items) {
    if (!std::holds_alternative<Function>(item.node)) continue;
    const auto& fn = std::get<Function>(item.node);
    FunctionAnalyzer analyzer(fn, escape, functions, resolution);
    auto diags = analyzer.run();
    out.diags.insert(out.diags.end(),
                     std::make_move_iterator(diags.begin()),
                     std::make_move_iterator(diags.end()));
  }
  return out;
}

BorrowXStmtResult run_borrow_xstmt(const nebula::nir::Program& p,
                                   const EscapeAnalysisResult& escape) {
  const auto resolution = run_call_target_resolver(p);
  return run_borrow_xstmt(p, escape, resolution);
}

} // namespace nebula::passes
