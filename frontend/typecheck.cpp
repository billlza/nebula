#include "frontend/typecheck.hpp"

#include "frontend/errors.hpp"
#include "frontend/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nebula::frontend {

namespace {

struct StructInfo {
  Span span{};
  std::vector<TField> fields;
};

struct EnumInfo {
  Span span{};
  std::string type_param;
  std::vector<TVariant> variants;
};

struct FuncSigInfo {
  Span span{};
  TFunctionSig sig;
  bool is_unsafe_callable = false;
};

struct ImportRef {
  std::string package_name;
  std::string module_name;
};

struct UnitInfo {
  const Program* ast = nullptr;
  std::string package_name;
  std::string module_name;
  std::string source_path;
  std::vector<ImportRef> imports;
};

enum class TypeDeclKind : std::uint8_t { Struct, Enum };

struct TypeCandidate {
  QualifiedName qualified_name;
  TypeDeclKind kind = TypeDeclKind::Struct;
};

struct VariantCandidate {
  QualifiedName enum_name;
  std::string enum_type_param;
  TVariant variant;
  std::uint32_t variant_index = 0;
};

class Typechecker {
public:
  explicit Typechecker(TypecheckOptions opt) : opt_(opt) { install_builtins(); }

  TypecheckResult run(const Program& ast) {
    units_.clear();
    UnitInfo unit;
    unit.ast = &ast;
    unit.package_name = ast.package_name.value_or(std::string{});
    unit.module_name = ast.module_name.value_or(std::string{});
    units_.push_back(std::move(unit));

    auto multi = run_prepared_units();
    TypecheckResult result;
    if (!multi.programs.empty()) result.program = std::move(multi.programs.front());
    result.diags = std::move(multi.diags);
    return result;
  }

  TypecheckUnitsResult run(const std::vector<CompilationUnit>& units) {
    units_.clear();
    units_.reserve(units.size());
    for (const auto& unit : units) {
      auto info = build_unit_info(unit);
      if (info.ast == nullptr) {
        error("NBL-T099", "compilation unit is missing a parsed program", Span{});
        continue;
      }
      units_.push_back(std::move(info));
    }

    return run_prepared_units();
  }

private:
  TypecheckUnitsResult run_prepared_units() {

    validate_unsafe_annotations();
    collect_type_decls();
    collect_function_sigs();
    typecheck_type_bodies();
    detect_safe_struct_cycles();

    const bool has_error =
        std::any_of(diags_.begin(), diags_.end(), [](const Diagnostic& d) { return d.is_error(); });

    TypecheckUnitsResult result;
    if (!has_error) {
      result.programs.reserve(units_.size());
      for (const auto& unit : units_) {
        current_unit_ = &unit;
        TProgram out;
        out.package_name = unit.package_name;
        out.module_name = unit.ast->module_name;
        out.imports = unit.ast->imports;
        out.items.reserve(unit.ast->items.size());

        for (const auto& it : unit.ast->items) {
          std::visit(
              [&](auto&& n) {
                using N = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<N, Function>) {
                  out.items.push_back(TItem{n.span, typecheck_function(n)});
                } else if constexpr (std::is_same_v<N, Struct>) {
                  out.items.push_back(TItem{n.span, typecheck_struct(n)});
                } else if constexpr (std::is_same_v<N, Enum>) {
                  out.items.push_back(TItem{n.span, typecheck_enum(n)});
                }
              },
              it.node);
        }

        result.programs.push_back(std::move(out));
      }
      current_unit_ = nullptr;
    }

    result.diags = std::move(diags_);
    return result;
  }
  TypecheckOptions opt_{};
  std::vector<Diagnostic> diags_;
  std::vector<UnitInfo> units_;
  const UnitInfo* current_unit_ = nullptr;

  std::unordered_map<QualifiedName, StructInfo, QualifiedNameHash> structs_;
  std::unordered_map<QualifiedName, EnumInfo, QualifiedNameHash> enums_;
  std::unordered_map<QualifiedName, FuncSigInfo, QualifiedNameHash> funcs_;
  std::unordered_map<std::string, std::vector<QualifiedName>> struct_names_;
  std::unordered_map<std::string, std::vector<QualifiedName>> enum_names_;
  std::unordered_map<std::string, std::vector<QualifiedName>> func_names_;
  std::unordered_map<std::string, FuncSigInfo> builtins_;

  using BindingId = std::uint32_t;
  static constexpr BindingId kInvalidBindingId = 0;

  struct BindingInfo {
    Ty ty = Ty::Unknown();
    BindingId binding_id = kInvalidBindingId;
  };

  struct VarInfo {
    Ty ty = Ty::Unknown();
    BindingId binding_id = kInvalidBindingId;
  };
  std::vector<std::unordered_map<std::string, VarInfo>> scopes_;
  BindingId next_binding_id_ = 1;

  // Function-local state
  Ty current_ret_ = Ty::Void();
  bool saw_return_ = false;
  bool in_unsafe_fn_ = false;
  int unsafe_depth_ = 0;
  bool in_mapped_method_fn_ = false;
  bool mapped_method_self_is_ref_ = false;
  bool suppress_borrow_read_checks_ = false;

  struct AliasKey {
    enum class Kind : std::uint8_t { Whole, Field };

    BindingId binding_id = kInvalidBindingId;
    std::string base;
    Kind kind = Kind::Whole;
    std::string field;
  };

  struct BorrowEntry {
    AliasKey key;
    Span borrow_span{};
  };

  struct StmtBorrowState {
    bool active = false;
    std::vector<BorrowEntry> active_borrows;
  };
  StmtBorrowState stmt_borrow_;

  enum class BorrowAccessKind : std::uint8_t { Read, Write, RefBorrow };

  void add_diag(Severity sev, std::string code, std::string msg, Span span) {
    Diagnostic d;
    d.severity = sev;
    d.code = std::move(code);
    d.message = std::move(msg);
    d.span = span;
    diags_.push_back(std::move(d));
  }

  void error(std::string code, std::string msg, Span span) {
    add_diag(Severity::Error, std::move(code), std::move(msg), span);
  }

  void warn(std::string code, std::string msg, Span span) {
    add_diag(Severity::Warning, std::move(code), std::move(msg), span);
  }

  static std::optional<ImportRef> parse_import_ref(std::string_view import_name,
                                                   const std::string& default_package) {
    const std::size_t sep = import_name.find("::");
    if (sep == std::string::npos) {
      if (import_name.empty()) return std::nullopt;
      return ImportRef{default_package, std::string(import_name)};
    }
    const std::string package_name(import_name.substr(0, sep));
    const std::string module_name(import_name.substr(sep + 2));
    if (package_name.empty() || module_name.empty()) return std::nullopt;
    return ImportRef{package_name, module_name};
  }

  static UnitInfo build_unit_info(const CompilationUnit& unit) {
    UnitInfo info;
    if (unit.program == nullptr) return info;
    info.ast = unit.program;
    info.package_name =
        !unit.package_name.empty() ? unit.package_name : unit.program->package_name.value_or(std::string{});
    info.module_name = unit.program->module_name.value_or(std::string{});
    info.source_path = unit.source_path;

    const auto& import_specs =
        unit.resolved_imports.empty() ? unit.program->imports : unit.resolved_imports;
    info.imports.reserve(import_specs.size());
    for (const auto& import_name : import_specs) {
      auto parsed = parse_import_ref(import_name, info.package_name);
      if (parsed.has_value()) info.imports.push_back(std::move(*parsed));
    }
    return info;
  }

  static QualifiedName qualify(const UnitInfo& unit, std::string local_name) {
    QualifiedName name;
    name.package_name = unit.package_name;
    name.module_name = unit.module_name;
    name.local_name = std::move(local_name);
    return name;
  }

  static QualifiedName qualify(const QualifiedName& base, std::string local_name) {
    QualifiedName name = base;
    name.local_name = std::move(local_name);
    return name;
  }

  static std::string describe_symbol(const QualifiedName& name) { return name.display_name(); }

  bool is_visible_in_unit(const UnitInfo& unit, const QualifiedName& name) const {
    if (name.package_name == unit.package_name && name.module_name == unit.module_name) return true;
    return std::any_of(unit.imports.begin(), unit.imports.end(), [&](const ImportRef& import_ref) {
      return import_ref.package_name == name.package_name &&
             import_ref.module_name == name.module_name;
    });
  }

  std::vector<QualifiedName> visible_function_candidates(const std::string& name) const {
    std::vector<QualifiedName> matches;
    if (current_unit_ == nullptr) return matches;
    auto it = func_names_.find(name);
    if (it == func_names_.end()) return matches;
    std::unordered_set<QualifiedName, QualifiedNameHash> seen;
    for (const auto& candidate : it->second) {
      if (!is_visible_in_unit(*current_unit_, candidate)) continue;
      if (seen.insert(candidate).second) matches.push_back(candidate);
    }
    return matches;
  }

  std::vector<TypeCandidate> visible_type_candidates(const std::string& name) const {
    std::vector<TypeCandidate> matches;
    if (current_unit_ == nullptr) return matches;
    std::unordered_set<QualifiedName, QualifiedNameHash> seen_structs;
    if (auto it = struct_names_.find(name); it != struct_names_.end()) {
      for (const auto& candidate : it->second) {
        if (is_visible_in_unit(*current_unit_, candidate) && seen_structs.insert(candidate).second) {
          matches.push_back(TypeCandidate{candidate, TypeDeclKind::Struct});
        }
      }
    }
    std::unordered_set<QualifiedName, QualifiedNameHash> seen_enums;
    if (auto it = enum_names_.find(name); it != enum_names_.end()) {
      for (const auto& candidate : it->second) {
        if (is_visible_in_unit(*current_unit_, candidate) && seen_enums.insert(candidate).second) {
          matches.push_back(TypeCandidate{candidate, TypeDeclKind::Enum});
        }
      }
    }
    return matches;
  }

  std::vector<VariantCandidate> visible_variant_candidates(const std::string& name) const {
    std::vector<VariantCandidate> matches;
    if (current_unit_ == nullptr) return matches;
    for (const auto& [enum_name, info] : enums_) {
      if (!is_visible_in_unit(*current_unit_, enum_name)) continue;
      for (std::size_t i = 0; i < info.variants.size(); ++i) {
        const auto& variant = info.variants[i];
        if (variant.name != name) continue;
        matches.push_back(
            VariantCandidate{enum_name, info.type_param, variant, static_cast<std::uint32_t>(i)});
      }
    }
    std::sort(matches.begin(), matches.end(), [](const VariantCandidate& lhs,
                                                 const VariantCandidate& rhs) {
      if (lhs.enum_name.package_name != rhs.enum_name.package_name) {
        return lhs.enum_name.package_name < rhs.enum_name.package_name;
      }
      if (lhs.enum_name.module_name != rhs.enum_name.module_name) {
        return lhs.enum_name.module_name < rhs.enum_name.module_name;
      }
      return lhs.enum_name.local_name < rhs.enum_name.local_name;
    });
    return matches;
  }

  static bool infer_enum_type_arg(const Ty& pattern,
                                  const Ty& actual,
                                  std::string_view type_param,
                                  std::optional<Ty>& inferred) {
    if (pattern.kind == Ty::Kind::TypeParam && pattern.name == type_param) {
      if (!inferred.has_value()) {
        inferred = actual;
        return true;
      }
      return ty_equal(*inferred, actual);
    }

    if (pattern.kind != actual.kind) return false;
    if (pattern.qualified_name.has_value() || actual.qualified_name.has_value()) {
      if (pattern.qualified_name.has_value() != actual.qualified_name.has_value()) return false;
      if (pattern.qualified_name.has_value() && *pattern.qualified_name != *actual.qualified_name) {
        return false;
      }
    } else if (pattern.name != actual.name) {
      return false;
    }

    if (pattern.kind == Ty::Kind::Callable) {
      if (pattern.callable_params.size() != actual.callable_params.size()) return false;
      if (pattern.callable_params_ref != actual.callable_params_ref) return false;
      for (std::size_t i = 0; i < pattern.callable_params.size(); ++i) {
        if (!infer_enum_type_arg(pattern.callable_params[i], actual.callable_params[i], type_param,
                                 inferred)) {
          return false;
        }
      }
      if (pattern.callable_ret == nullptr || actual.callable_ret == nullptr) {
        return pattern.callable_ret == nullptr && actual.callable_ret == nullptr;
      }
      return infer_enum_type_arg(*pattern.callable_ret, *actual.callable_ret, type_param, inferred);
    }

    if (pattern.type_arg == nullptr || actual.type_arg == nullptr) {
      return pattern.type_arg == nullptr && actual.type_arg == nullptr;
    }
    return infer_enum_type_arg(*pattern.type_arg, *actual.type_arg, type_param, inferred);
  }

  static bool variant_payload_is_zero_arg(const Ty& payload) {
    return payload.kind == Ty::Kind::Void;
  }

  std::optional<TypeCandidate> resolve_visible_type(const std::string& name, Span span) {
    const auto matches = visible_type_candidates(name);
    if (matches.empty()) return std::nullopt;
    if (matches.size() == 1) return matches.front();

    std::string message = "ambiguous type name: " + name + " (candidates: ";
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (i != 0) message += ", ";
      message += describe_symbol(matches[i].qualified_name);
    }
    message += ")";
    error("NBL-T096", std::move(message), span);
    return std::nullopt;
  }

  std::optional<QualifiedName> resolve_visible_function(const std::string& name, Span span) {
    const auto matches = visible_function_candidates(name);
    if (matches.empty()) return std::nullopt;
    if (matches.size() == 1) return matches.front();

    std::string message = "ambiguous function name: " + name + " (candidates: ";
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (i != 0) message += ", ";
      message += describe_symbol(matches[i]);
    }
    message += ")";
    error("NBL-T097", std::move(message), span);
    return std::nullopt;
  }

  bool has_local_type_name(const UnitInfo& unit, const std::string& name) const {
    const QualifiedName qualified = qualify(unit, name);
    return structs_.find(qualified) != structs_.end() || enums_.find(qualified) != enums_.end();
  }

  const StructInfo* lookup_struct_info(const QualifiedName& name) const {
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : &it->second;
  }

  const EnumInfo* lookup_enum_info(const QualifiedName& name) const {
    auto it = enums_.find(name);
    return it == enums_.end() ? nullptr : &it->second;
  }

  const FuncSigInfo* lookup_function_info(const QualifiedName& name) const {
    auto it = funcs_.find(name);
    return it == funcs_.end() ? nullptr : &it->second;
  }

  const StructInfo* lookup_struct_info(const Ty& ty) const {
    if (ty.kind != Ty::Kind::Struct || !ty.qualified_name.has_value()) return nullptr;
    return lookup_struct_info(*ty.qualified_name);
  }

  static bool has_annotation(const std::vector<std::string>& annotations,
                             const std::string& name) {
    return std::find(annotations.begin(), annotations.end(), name) != annotations.end();
  }

  bool in_unsafe_context() const { return in_unsafe_fn_ || unsafe_depth_ > 0; }

  void emit_unsafe_call_required(const std::string& callee,
                                 Span call_span,
                                 Span callee_span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-U001";
    d.message = "unsafe callable requires unsafe context: " + callee;
    d.span = call_span;
    d.category = "unsafe-boundary";
    d.risk = DiagnosticRisk::High;
    d.cause = "safe context attempted to call a function marked @unsafe";
    d.impact = "safe-subset guarantees do not apply across an unsafe boundary without explicit opt-in";
    d.suggestions = {"wrap this call in unsafe { ... }",
                     "or mark the enclosing function with @unsafe after manual audit"};
    d.related_spans.push_back(call_span);
    if (callee_span.start.offset != 0 || callee_span.end.offset != 0) {
      d.related_spans.push_back(callee_span);
    }
    diags_.push_back(std::move(d));
  }

  void emit_invalid_unsafe_annotation(std::string item_kind,
                                      const std::string& item_name,
                                      Span span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-U002";
    d.message = "@unsafe can only annotate functions (invalid target: " + item_kind + " " +
                item_name + ")";
    d.span = span;
    d.category = "unsafe-boundary";
    d.risk = DiagnosticRisk::High;
    d.cause = "@unsafe annotation target is not callable";
    d.impact = "unsafe boundary contract becomes ambiguous and cannot be audited reliably";
    d.suggestions = {"remove @unsafe from this " + item_kind,
                     "or move @unsafe to the function that performs unsafe operations"};
    d.related_spans.push_back(span);
    diags_.push_back(std::move(d));
  }

  Ty callable_ty_from_sig(const FuncSigInfo& info) const {
    Ty ret = info.sig.ret;
    if (ret.kind == Ty::Kind::Unknown) ret = Ty::Unknown();
    return Ty::Callable(info.sig.params, std::move(ret), info.is_unsafe_callable,
                        info.sig.params_ref);
  }

  void enforce_unsafe_call_context(const std::string& callee,
                                   bool is_unsafe_callable,
                                   Span call_span,
                                   Span callee_span) {
    if (is_unsafe_callable && !in_unsafe_context()) {
      emit_unsafe_call_required(callee, call_span, callee_span);
    }
  }

  const TField* find_struct_field(const Ty& struct_ty, const std::string& field_name) const {
    const auto* info = lookup_struct_info(struct_ty);
    if (info == nullptr) return nullptr;
    for (const auto& f : info->fields) {
      if (f.name == field_name) return &f;
    }
    return nullptr;
  }

  void begin_stmt_borrow_scope() {
    stmt_borrow_.active = true;
    stmt_borrow_.active_borrows.clear();
  }

  void end_stmt_borrow_scope() {
    stmt_borrow_.active = false;
    stmt_borrow_.active_borrows.clear();
  }

  void clear_persistent_borrow_state() {}

  void push_persistent_borrow_frame() {}

  void pop_persistent_borrow_frame(bool /*propagate_to_parent*/) {}

  static AliasKey make_whole_alias(BindingId binding_id, std::string base) {
    AliasKey key;
    key.binding_id = binding_id;
    key.base = std::move(base);
    key.kind = AliasKey::Kind::Whole;
    return key;
  }

  static AliasKey make_field_alias(BindingId binding_id, std::string base, std::string field) {
    AliasKey key;
    key.binding_id = binding_id;
    key.base = std::move(base);
    key.kind = AliasKey::Kind::Field;
    key.field = std::move(field);
    return key;
  }

  static std::string alias_to_string(const AliasKey& key) {
    if (key.kind == AliasKey::Kind::Field) return key.base + "." + key.field;
    return key.base;
  }

  static bool alias_overlaps(const AliasKey& lhs, const AliasKey& rhs) {
    if (lhs.binding_id == kInvalidBindingId || rhs.binding_id == kInvalidBindingId) return false;
    if (lhs.binding_id != rhs.binding_id) return false;
    if (lhs.base != rhs.base) return false;
    if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return true;
    return lhs.field == rhs.field;
  }

  static std::string overlap_label(const AliasKey& lhs, const AliasKey& rhs) {
    if (lhs.base != rhs.base) return alias_to_string(lhs);
    if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return lhs.base;
    return lhs.base + "." + lhs.field;
  }

  std::optional<AliasKey> alias_for_whole_name(const std::string& name) const {
    auto binding = lookup_binding(name);
    if (!binding.has_value()) return std::nullopt;
    return make_whole_alias(binding->binding_id, name);
  }

  std::optional<AliasKey> alias_for_field_name(const std::string& base,
                                               const std::string& field) const {
    auto binding = lookup_binding(base);
    if (!binding.has_value()) return std::nullopt;
    return make_field_alias(binding->binding_id, base, field);
  }

  std::optional<AliasKey> extract_alias_key(const TExpr& e) const {
    return std::visit(
        [&](auto&& n) -> std::optional<AliasKey> {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, TExpr::VarRef>) {
            return alias_for_whole_name(n.name);
          } else if constexpr (std::is_same_v<N, TExpr::FieldRef>) {
            return alias_for_field_name(n.base, n.field);
          } else {
            return std::nullopt;
          }
        },
        e.node);
  }

  static bool is_ref_lvalue(const TExpr& e) {
    return std::holds_alternative<TExpr::VarRef>(e.node) ||
           std::holds_alternative<TExpr::FieldRef>(e.node);
  }

  void emit_stmt_borrow_conflict(const std::string& overlap,
                                 Span first_borrow_span,
                                 Span conflict_span,
                                 BorrowAccessKind access_kind) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-T092";
    d.span = conflict_span;
    d.category = "typecheck";
    d.risk = DiagnosticRisk::High;
    d.related_spans.push_back(first_borrow_span);
    d.related_spans.push_back(conflict_span);

    if (access_kind == BorrowAccessKind::Read) {
      d.message = "same-statement borrow conflict on `" + overlap + "` during read access";
      d.cause = "value is read after being mutably borrowed by a ref parameter earlier in this statement";
      d.impact = "exclusive mutable borrow is violated and read/write ordering becomes unsafe";
      d.suggestions = {"move the read before the ref-taking call",
                       "or split this logic into multiple statements"};
    } else if (access_kind == BorrowAccessKind::Write) {
      d.message = "same-statement borrow conflict on `" + overlap + "` during write access";
      d.cause = "value is written after being mutably borrowed by a ref parameter earlier in this statement";
      d.impact = "exclusive mutable borrow is violated and may cause overlapping writes";
      d.suggestions = {"move the write before the ref-taking call",
                       "or split this logic into multiple statements"};
    } else {
      d.message = "same-statement borrow conflict on `" + overlap + "` during ref borrow";
      d.cause = "value is borrowed again as ref while an earlier ref borrow is still active in this statement";
      d.impact = "exclusive mutable borrow is violated by overlapping ref borrows";
      d.suggestions = {"avoid re-borrowing the same root in one statement",
                       "or split calls into separate statements"};
    }

    diags_.push_back(std::move(d));
  }

  void emit_persistent_borrow_conflict(const std::string& code,
                                       const std::string& overlap,
                                       Span first_borrow_span,
                                       Span conflict_span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = code;
    d.span = conflict_span;
    d.category = "typecheck";
    d.risk = DiagnosticRisk::High;
    d.related_spans.push_back(first_borrow_span);
    d.related_spans.push_back(conflict_span);

    if (code == "NBL-T093") {
      d.message = "cross-statement borrow conflict on `" + overlap + "` during read access";
      d.cause = "value is read in a later statement while a ref borrow from an earlier statement is still active";
      d.impact = "exclusive mutable borrow is violated across statement boundaries";
      d.suggestions = {"move this read before the ref-taking call",
                       "or isolate the ref-taking operation in a separate block"};
    } else if (code == "NBL-T094") {
      d.message = "cross-statement borrow conflict on `" + overlap + "` during write access";
      d.cause = "value is written in a later statement while a ref borrow from an earlier statement is still active";
      d.impact = "exclusive mutable borrow is violated and may cause overlapping mutation";
      d.suggestions = {"perform this write before the ref-taking call",
                       "or split code so the ref borrow does not span this statement"};
    } else {
      d.message = "cross-statement borrow conflict on `" + overlap + "` during ref borrow";
      d.cause = "value is re-borrowed as ref while a prior statement-level ref borrow is still active";
      d.impact = "exclusive mutable borrow is violated by overlapping borrow windows";
      d.suggestions = {"end the earlier borrow before re-borrowing",
                       "or split operations into disjoint storage roots"};
    }

    diags_.push_back(std::move(d));
  }

  bool check_stmt_borrow_access_conflict(const std::optional<AliasKey>& alias,
                                         Span access_span,
                                         BorrowAccessKind access_kind) {
    if (!stmt_borrow_.active || !alias.has_value()) return false;
    for (const auto& active : stmt_borrow_.active_borrows) {
      if (!alias_overlaps(active.key, *alias)) continue;
      emit_stmt_borrow_conflict(overlap_label(active.key, *alias), active.borrow_span, access_span,
                                access_kind);
      return true;
    }
    return false;
  }

  std::optional<BorrowEntry> find_persistent_borrow_overlap(const AliasKey& alias) const {
    (void)alias;
    return std::nullopt;
  }

  bool check_persistent_borrow_conflict(const std::optional<AliasKey>& alias,
                                        Span access_span,
                                        BorrowAccessKind access_kind) {
    if (!alias.has_value()) return false;
    auto active = find_persistent_borrow_overlap(*alias);
    if (!active.has_value()) return false;
    if (access_kind == BorrowAccessKind::Read) {
      emit_persistent_borrow_conflict("NBL-T093", overlap_label(active->key, *alias),
                                      active->borrow_span, access_span);
    } else if (access_kind == BorrowAccessKind::Write) {
      emit_persistent_borrow_conflict("NBL-T094", overlap_label(active->key, *alias),
                                      active->borrow_span, access_span);
    } else {
      emit_persistent_borrow_conflict("NBL-T095", overlap_label(active->key, *alias),
                                      active->borrow_span, access_span);
    }
    return true;
  }

  bool check_borrow_access_conflict(const std::optional<AliasKey>& alias,
                                    Span access_span,
                                    BorrowAccessKind access_kind) {
    if (suppress_borrow_read_checks_) return false;
    return check_stmt_borrow_access_conflict(alias, access_span, access_kind);
  }

  bool record_stmt_ref_borrow(const std::optional<AliasKey>& alias,
                              Span borrow_span,
                              std::vector<AliasKey>* local_aliases) {
    if (!stmt_borrow_.active || !alias.has_value()) return false;
    for (const auto& active : stmt_borrow_.active_borrows) {
      if (!alias_overlaps(active.key, *alias)) continue;
      bool from_same_call = false;
      if (local_aliases != nullptr) {
        for (const auto& local : *local_aliases) {
          if (alias_overlaps(local, *alias)) {
            from_same_call = true;
            break;
          }
        }
      }
      if (!from_same_call) {
        emit_stmt_borrow_conflict(overlap_label(active.key, *alias), active.borrow_span, borrow_span,
                                  BorrowAccessKind::RefBorrow);
        return true;
      }
      return false;
    }
    stmt_borrow_.active_borrows.push_back(BorrowEntry{*alias, borrow_span});
    if (local_aliases != nullptr) local_aliases->push_back(*alias);
    return false;
  }

  void record_persistent_ref_borrow(const std::optional<AliasKey>& alias, Span borrow_span) {
    (void)alias;
    (void)borrow_span;
  }

  void enqueue_pending_persistent_ref_borrow(const std::optional<AliasKey>& alias,
                                             Span borrow_span,
                                             std::vector<BorrowEntry>* pending_borrows) {
    (void)alias;
    (void)borrow_span;
    (void)pending_borrows;
  }

  void commit_pending_persistent_ref_borrows(const std::vector<BorrowEntry>& pending_borrows) {
    (void)pending_borrows;
  }

  void register_ref_borrow(const std::optional<AliasKey>& alias,
                           Span borrow_span,
                           std::vector<AliasKey>* local_aliases,
                           std::vector<BorrowEntry>* pending_persistent_borrows,
                           bool persist_across_statements) {
    (void)pending_persistent_borrows;
    (void)persist_across_statements;
    const bool stmt_conflict = record_stmt_ref_borrow(alias, borrow_span, local_aliases);
    if (stmt_conflict) return;
  }

  TExprPtr typecheck_expr_for_ref_binding(const Expr& e) {
    const bool prev = suppress_borrow_read_checks_;
    suppress_borrow_read_checks_ = true;
    auto out = typecheck_expr(e);
    suppress_borrow_read_checks_ = prev;
    return out;
  }

  void emit_ref_alias_conflict(std::string code,
                               const std::string& callee,
                               const std::string& overlap,
                               Span call_span,
                               Span lhs_span,
                               Span rhs_span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = std::move(code);
    d.span = call_span;
    d.category = "typecheck";
    d.risk = DiagnosticRisk::High;
    d.related_spans.push_back(lhs_span);
    d.related_spans.push_back(rhs_span);
    if (d.code == "NBL-T090") {
      d.message = "mutable ref alias conflict in call to " + callee + " on `" + overlap + "`";
      d.cause = "multiple ref parameters overlap on the same mutable alias location in a single call";
      d.impact = "exclusive mutable borrow is violated and may cause write-write aliasing";
      d.suggestions = {"pass distinct storage roots to each ref parameter",
                       "split the operation into separate calls to avoid overlapping mutable borrows"};
    } else {
      d.message = "ref parameter overlaps non-ref argument in call to " + callee + " on `" +
                  overlap + "`";
      d.cause = "a ref parameter and a non-ref argument overlap on the same mutable alias location";
      d.impact = "mutable borrow may overlap read/write access and break exclusivity guarantees";
      d.suggestions = {"avoid using the same base value in ref and non-ref arguments",
                       "compute non-ref values before taking the ref argument"};
    }
    diags_.push_back(std::move(d));
  }

  void check_ref_alias_conflicts(const std::string& callee,
                                 const std::vector<bool>& params_ref,
                                 const std::vector<TExprPtr>& args,
                                 Span call_span) {
    const std::size_t n = std::min(params_ref.size(), args.size());
    if (n < 2) return;
    std::vector<std::optional<AliasKey>> keys;
    keys.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      keys.push_back(extract_alias_key(*args[i]));
    }

    for (std::size_t i = 0; i < n; ++i) {
      if (!keys[i].has_value()) continue;
      for (std::size_t j = i + 1; j < n; ++j) {
        if (!keys[j].has_value()) continue;
        if (!alias_overlaps(*keys[i], *keys[j])) continue;
        const std::string overlap = overlap_label(*keys[i], *keys[j]);
        if (params_ref[i] && params_ref[j]) {
          emit_ref_alias_conflict("NBL-T090", callee, overlap, call_span, args[i]->span,
                                  args[j]->span);
          continue;
        }
        if (params_ref[i] || params_ref[j]) {
          emit_ref_alias_conflict("NBL-T091", callee, overlap, call_span, args[i]->span,
                                  args[j]->span);
        }
      }
    }
  }

  void install_builtins() {
    auto add_builtin = [&](std::string name, std::vector<Ty> params, Ty ret) {
      TFunctionSig s;
      s.name = name;
      s.qualified_name.local_name = name;
      s.params = std::move(params);
      s.params_ref.assign(s.params.size(), false);
      s.ret = std::move(ret);
      builtins_.insert({s.name, FuncSigInfo{Span{}, s}});
    };

    add_builtin("expect_eq", {Ty::Int(), Ty::Int()}, Ty::Void());
    add_builtin("print", {Ty::String()}, Ty::Void());
    add_builtin("panic", {Ty::String()}, Ty::Void());
    add_builtin("assert", {Ty::Bool(), Ty::String()}, Ty::Void());
    add_builtin("argc", {}, Ty::Int());
    add_builtin("argv", {Ty::Int()}, Ty::String());
  }

  static bool is_scalar_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Int || ty.kind == Ty::Kind::Float || ty.kind == Ty::Kind::Bool ||
           ty.kind == Ty::Kind::String;
  }

  void validate_unsafe_annotations() {
    for (const auto& unit : units_) {
      for (const auto& it : unit.ast->items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              if constexpr (std::is_same_v<N, Struct>) {
                if (has_annotation(n.annotations, "unsafe")) {
                  emit_invalid_unsafe_annotation("struct", n.name, n.span);
                }
              } else if constexpr (std::is_same_v<N, Enum>) {
                if (has_annotation(n.annotations, "unsafe")) {
                  emit_invalid_unsafe_annotation("enum", n.name, n.span);
                }
              }
            },
            it.node);
      }
    }
  }

  void collect_type_decls() {
    for (const auto& unit : units_) {
      for (const auto& it : unit.ast->items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              const QualifiedName qualified = qualify(unit, n.name);
              if constexpr (std::is_same_v<N, Struct>) {
                if (structs_.find(qualified) != structs_.end() ||
                    enums_.find(qualified) != enums_.end()) {
                  error("NBL-T001", "duplicate type name: " + n.name, n.span);
                  return;
                }
                structs_.insert({qualified, StructInfo{n.span, {}}});
                struct_names_[n.name].push_back(qualified);
              } else if constexpr (std::is_same_v<N, Enum>) {
                if (structs_.find(qualified) != structs_.end() ||
                    enums_.find(qualified) != enums_.end()) {
                  error("NBL-T001", "duplicate type name: " + n.name, n.span);
                  return;
                }
                enums_.insert({qualified, EnumInfo{n.span, n.type_param, {}}});
                enum_names_[n.name].push_back(qualified);
              }
            },
            it.node);
      }
    }
  }

  Ty resolve_type(const Type& syn, std::optional<std::string_view> type_param) {
    if (syn.callable_ret) {
      std::vector<Ty> params;
      std::vector<bool> params_ref;
      params.reserve(syn.callable_params.size());
      params_ref.reserve(syn.callable_params.size());
      for (const auto& p : syn.callable_params) {
        params.push_back(resolve_type(p, type_param));
        params_ref.push_back(false);
      }
      Ty ret = resolve_type(*syn.callable_ret, type_param);
      return Ty::Callable(std::move(params), std::move(ret), syn.is_unsafe_callable,
                          std::move(params_ref));
    }

    // builtins
    if (syn.name == "Int") return Ty::Int();
    if (syn.name == "Float") return Ty::Float();
    if (syn.name == "Bool") return Ty::Bool();
    if (syn.name == "String") return Ty::String();
    if (syn.name == "Void") return Ty::Void();

    // type param (only valid inside enum definition)
    if (type_param && syn.name == *type_param) {
      if (syn.arg) {
        error("NBL-T013", "type parameter cannot take type arguments: " + syn.name, syn.span);
        return Ty::Unknown();
      }
      return Ty::TypeParam(syn.name);
    }

    const auto type_matches = visible_type_candidates(syn.name);
    if (type_matches.size() > 1) {
      resolve_visible_type(syn.name, syn.span);
      return Ty::Unknown();
    }
    const auto resolved =
        type_matches.empty() ? std::optional<TypeCandidate>{} : std::optional<TypeCandidate>{type_matches.front()};
    const bool is_struct = resolved.has_value() && resolved->kind == TypeDeclKind::Struct;
    const bool is_enum = resolved.has_value() && resolved->kind == TypeDeclKind::Enum;
    if (!syn.arg) {
      if (is_struct) return Ty::Struct(syn.name, resolved->qualified_name);
      if (is_enum) {
        error("NBL-T012", "missing type argument for enum: " + syn.name, syn.span);
        return Ty::Unknown();
      }
      error("NBL-T010", "unknown type: " + syn.name, syn.span);
      return Ty::Unknown();
    }

    // Has type argument
    if (!is_enum) {
      error("NBL-T011", "type does not take type arguments: " + syn.name, syn.span);
      return Ty::Unknown();
    }
    Ty arg = resolve_type(*syn.arg, type_param);
    return Ty::Enum(syn.name, std::move(arg), resolved->qualified_name);
  }

  void collect_function_sigs() {
    for (const auto& unit : units_) {
      current_unit_ = &unit;
      for (const auto& it : unit.ast->items) {
        if (!std::holds_alternative<Function>(it.node)) continue;
        const auto& fn = std::get<Function>(it.node);
        const QualifiedName qualified = qualify(unit, fn.name);

        if (has_local_type_name(unit, fn.name)) {
          error("NBL-T002", "function name collides with type name: " + fn.name, fn.span);
          continue;
        }
        if (funcs_.find(qualified) != funcs_.end()) {
          error("NBL-T003", "duplicate function name: " + fn.name, fn.span);
          continue;
        }
        if (builtins_.find(fn.name) != builtins_.end() && !fn.is_extern) {
          error("NBL-T003", "duplicate function name: " + fn.name, fn.span);
          continue;
        }

        TFunctionSig sig;
        sig.name = fn.name;
        sig.qualified_name = qualified;
        sig.param_names.reserve(fn.params.size());
        sig.params.reserve(fn.params.size());
        sig.params_ref.reserve(fn.params.size());
        for (const auto& p : fn.params) {
          sig.param_names.push_back(p.name);
          sig.params.push_back(resolve_type(p.type, std::nullopt));
          sig.params_ref.push_back(p.is_ref);
        }
        if (fn.return_type.has_value()) {
          sig.ret = resolve_type(*fn.return_type, std::nullopt);
        } else {
          sig.ret = Ty::Void();
        }
        sig.is_unsafe_callable = has_annotation(fn.annotations, "unsafe");

        funcs_.insert_or_assign(qualified, FuncSigInfo{fn.span, sig, sig.is_unsafe_callable});
        func_names_[fn.name].push_back(qualified);
      }
    }
    current_unit_ = nullptr;
  }

  void typecheck_type_bodies() {
    for (const auto& unit : units_) {
      current_unit_ = &unit;
      for (const auto& it : unit.ast->items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              const QualifiedName qualified = qualify(unit, n.name);
              if constexpr (std::is_same_v<N, Struct>) {
                auto itst = structs_.find(qualified);
                if (itst == structs_.end()) return;
                StructInfo& info = itst->second;
                info.fields.clear();
                for (const auto& f : n.fields) {
                  TField tf;
                  tf.span = f.span;
                  tf.name = f.name;
                  tf.ty = resolve_type(f.type, std::nullopt);
                  info.fields.push_back(std::move(tf));
                }
              } else if constexpr (std::is_same_v<N, Enum>) {
                auto iten = enums_.find(qualified);
                if (iten == enums_.end()) return;
                EnumInfo& info = iten->second;
                info.variants.clear();
                for (const auto& v : n.variants) {
                  TVariant tv;
                  tv.span = v.span;
                  tv.name = v.name;
                  tv.payload = resolve_type(v.payload, std::string_view{info.type_param});
                  info.variants.push_back(std::move(tv));
                }
              }
            },
            it.node);
      }
    }
    current_unit_ = nullptr;
  }

  void collect_struct_refs_from_ty(const Ty& ty, std::vector<QualifiedName>& out) {
    if (ty.kind == Ty::Kind::Struct && ty.qualified_name.has_value()) {
      out.push_back(*ty.qualified_name);
    }
    if (ty.type_arg) collect_struct_refs_from_ty(*ty.type_arg, out);
  }

  void detect_safe_struct_cycles() {
    std::unordered_map<QualifiedName, std::vector<QualifiedName>, QualifiedNameHash> edges;
    edges.reserve(structs_.size());
    for (const auto& [name, info] : structs_) {
      auto& dst = edges[name];
      for (const auto& f : info.fields) {
        std::vector<QualifiedName> refs;
        collect_struct_refs_from_ty(f.ty, refs);
        for (const auto& r : refs) {
          if (structs_.find(r) != structs_.end()) dst.push_back(r);
        }
      }
    }

    std::unordered_map<QualifiedName, int, QualifiedNameHash> state;
    state.reserve(structs_.size());
    std::vector<QualifiedName> stack;
    std::unordered_set<QualifiedName, QualifiedNameHash> reported;

    std::function<void(const QualifiedName&)> dfs = [&](const QualifiedName& u) {
      state[u] = 1;
      stack.push_back(u);
      for (const auto& v : edges[u]) {
        if (state[v] == 0) {
          dfs(v);
          continue;
        }
        if (state[v] == 1) {
          auto it = std::find(stack.begin(), stack.end(), v);
          for (; it != stack.end(); ++it) {
            if (!reported.insert(*it).second) continue;
            auto sit = structs_.find(*it);
            if (sit == structs_.end()) continue;
            error("NBL-S101",
                  "analyzable strong ownership cycle is not allowed in safe subset: " +
                      describe_symbol(*it),
                  sit->second.span);
          }
        }
      }
      stack.pop_back();
      state[u] = 2;
    };

    for (const auto& [name, _] : structs_) {
      if (state[name] == 0) dfs(name);
    }
  }

  // Scope helpers
  void push_scope() { scopes_.push_back({}); }
  void pop_scope() {
    if (!scopes_.empty()) scopes_.pop_back();
  }

  bool declare_var(const std::string& name, Ty ty, Span span) {
    if (scopes_.empty()) push_scope();
    auto& m = scopes_.back();
    if (m.count(name)) {
      error("NBL-T020", "duplicate binding: " + name, span);
      return false;
    }
    m.insert({name, VarInfo{std::move(ty), next_binding_id_++}});
    return true;
  }

  std::optional<BindingInfo> lookup_binding(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto jt = it->find(name);
      if (jt != it->end()) return BindingInfo{jt->second.ty, jt->second.binding_id};
    }
    return std::nullopt;
  }

  std::optional<Ty> lookup_var(const std::string& name) const {
    auto binding = lookup_binding(name);
    if (binding.has_value()) return binding->ty;
    return std::nullopt;
  }

  // Typecheck items
  TStruct typecheck_struct(const Struct& st) {
    TStruct out;
    out.span = st.span;
    out.annotations = st.annotations;
    out.name = st.name;
    out.qualified_name = qualify(*current_unit_, st.name);
    auto it = structs_.find(out.qualified_name);
    if (it != structs_.end()) out.fields = it->second.fields;
    return out;
  }

  TEnum typecheck_enum(const Enum& en) {
    TEnum out;
    out.span = en.span;
    out.annotations = en.annotations;
    out.name = en.name;
    out.qualified_name = qualify(*current_unit_, en.name);
    out.type_param = en.type_param;
    auto it = enums_.find(out.qualified_name);
    if (it != enums_.end()) out.variants = it->second.variants;
    return out;
  }

  TFunction typecheck_function(const Function& fn) {
    TFunction out;
    out.span = fn.span;
    out.annotations = fn.annotations;
    out.name = fn.name;
    out.qualified_name = qualify(*current_unit_, fn.name);
    out.is_extern = fn.is_extern;
    auto sig_it = funcs_.find(out.qualified_name);
    const TFunctionSig* sig = nullptr;
    if (sig_it != funcs_.end()) sig = &sig_it->second.sig;
    out.ret = sig ? sig->ret : Ty::Void();

    out.params.reserve(fn.params.size());
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      const auto& p = fn.params[i];
      TParam tp;
      tp.span = p.span;
      tp.is_ref = p.is_ref;
      tp.name = p.name;
      tp.ty = (sig && i < sig->params.size()) ? sig->params[i] : Ty::Unknown();
      out.params.push_back(std::move(tp));
    }

    // Function body typecheck
    const bool prev_in_unsafe_fn = in_unsafe_fn_;
    const int prev_unsafe_depth = unsafe_depth_;
    const bool prev_in_mapped_method_fn = in_mapped_method_fn_;
    const bool prev_mapped_method_self_is_ref = mapped_method_self_is_ref_;
    in_unsafe_fn_ = sig_it != funcs_.end() ? sig_it->second.is_unsafe_callable
                                           : has_annotation(fn.annotations, "unsafe");
    unsafe_depth_ = 0;
    in_mapped_method_fn_ = false;
    mapped_method_self_is_ref_ = false;
    const std::size_t us = fn.name.find('_');
    if (us != std::string::npos && us > 0 && us + 1 < fn.name.size() && !fn.params.empty() &&
        fn.params[0].name == "self") {
      in_mapped_method_fn_ = true;
      mapped_method_self_is_ref_ = fn.params[0].is_ref;
    }
    current_ret_ = out.ret;
    saw_return_ = false;
    scopes_.clear();
    next_binding_id_ = 1;
    clear_persistent_borrow_state();
    push_scope();
    for (const auto& p : out.params) {
      declare_var(p.name, p.ty, p.span);
    }
    if (!fn.is_extern && fn.body.has_value()) {
      out.body = typecheck_block(*fn.body, true);
    }
    pop_scope();
    clear_persistent_borrow_state();

    if (!fn.is_extern && current_ret_.kind != Ty::Kind::Void &&
        (!out.body.has_value() || !block_guarantees_return(*out.body))) {
      error("NBL-T030", "missing return in non-void function: " + fn.name, fn.span);
    }

    in_unsafe_fn_ = prev_in_unsafe_fn;
    unsafe_depth_ = prev_unsafe_depth;
    in_mapped_method_fn_ = prev_in_mapped_method_fn;
    mapped_method_self_is_ref_ = prev_mapped_method_self_is_ref;
    return out;
  }

  static bool stmt_guarantees_return(const TStmt& s) {
    return std::visit(
        [&](auto&& st) -> bool {
          using S = std::decay_t<decltype(st)>;
          if constexpr (std::is_same_v<S, TStmt::Return>) {
            return true;
          } else if constexpr (std::is_same_v<S, TStmt::If>) {
            if (!st.else_body.has_value()) return false;
            return block_guarantees_return(st.then_body) && block_guarantees_return(*st.else_body);
          } else if constexpr (std::is_same_v<S, TStmt::Region>) {
            return block_guarantees_return(st.body);
          } else if constexpr (std::is_same_v<S, TStmt::Unsafe>) {
            return block_guarantees_return(st.body);
          } else {
            return false;
          }
        },
        s.node);
  }

  static bool block_guarantees_return(const TBlock& b) {
    for (const auto& st : b.stmts) {
      if (stmt_guarantees_return(st)) return true;
    }
    return false;
  }

  // Blocks/statements
  TBlock typecheck_block(const Block& b, bool propagate_persistent_borrows_to_parent) {
    TBlock out;
    out.span = b.span;
    out.stmts.reserve(b.stmts.size());
    push_scope();
    push_persistent_borrow_frame();
    for (const auto& s : b.stmts) {
      out.stmts.push_back(typecheck_stmt(s));
    }
    pop_scope();
    pop_persistent_borrow_frame(propagate_persistent_borrows_to_parent);
    return out;
  }

  TStmt typecheck_stmt(const Stmt& s) {
    TStmt out;
    out.span = s.span;
    out.annotations = s.annotations;
    begin_stmt_borrow_scope();

    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            auto v = typecheck_expr(*n.value);
            TStmt::Let st;
            st.name = n.name;
            st.ty = v->ty;
            st.value = std::move(v);
            declare_var(st.name, st.ty, s.span);
            out.node = std::move(st);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            auto v = typecheck_expr(*n.value);
            if (current_ret_.kind == Ty::Kind::Void) {
              error("NBL-T031", "cannot return a value from a void function", s.span);
            } else if (!ty_equal(v->ty, current_ret_) && v->ty.kind != Ty::Kind::Unknown) {
              error("NBL-T032",
                    "return type mismatch: expected " + ty_to_string(current_ret_) + ", got " +
                        ty_to_string(v->ty),
                    s.span);
            }
            saw_return_ = true;
            out.node = TStmt::Return{std::move(v)};
          } else if constexpr (std::is_same_v<N, Stmt::ExprStmt>) {
            auto e = typecheck_expr(*n.expr);
            out.node = TStmt::ExprStmt{std::move(e)};
          } else if constexpr (std::is_same_v<N, Stmt::AssignVar>) {
            auto v = typecheck_expr(*n.value);
            TStmt::AssignVar st;
            st.name = n.name;
            st.ty = Ty::Unknown();
            st.value = std::move(v);

            auto dst = lookup_var(n.name);
            if (!dst.has_value()) {
              error("NBL-T083", "invalid assignment target: " + n.name, s.span);
            } else {
              check_borrow_access_conflict(alias_for_whole_name(n.name), s.span,
                                           BorrowAccessKind::Write);
              st.ty = *dst;
              if (!ty_equal(st.value->ty, *dst) && st.value->ty.kind != Ty::Kind::Unknown &&
                  dst->kind != Ty::Kind::Unknown) {
                error("NBL-T082",
                      "assignment type mismatch: expected " + ty_to_string(*dst) + ", got " +
                          ty_to_string(st.value->ty),
                      s.span);
              }
            }
            out.node = std::move(st);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            auto v = typecheck_expr(*n.value);
            TStmt::AssignField st;
            st.base = n.base;
            st.field = n.field;
            st.ty = Ty::Unknown();
            st.value = std::move(v);

            auto base_ty = lookup_var(n.base);
            if (!base_ty.has_value()) {
              error("NBL-T083", "invalid assignment target: " + n.base + "." + n.field, s.span);
            } else if (base_ty->kind != Ty::Kind::Struct) {
              if (base_ty->kind != Ty::Kind::Unknown) {
                error("NBL-T081", "field access on non-struct value: " + n.base, s.span);
              }
            } else {
              check_borrow_access_conflict(
                  alias_for_field_name(n.base, n.field), s.span, BorrowAccessKind::Write);
              const TField* fld = find_struct_field(*base_ty, n.field);
              if (fld == nullptr) {
                error("NBL-T080", "unknown field: " + base_ty->name + "." + n.field, s.span);
              } else {
                st.ty = fld->ty;
                if (!ty_equal(st.value->ty, fld->ty) && st.value->ty.kind != Ty::Kind::Unknown &&
                    fld->ty.kind != Ty::Kind::Unknown) {
                  error("NBL-T082",
                        "field assignment type mismatch: expected " + ty_to_string(fld->ty) +
                            ", got " + ty_to_string(st.value->ty),
                        s.span);
                }
              }
            }

            if (in_mapped_method_fn_ && n.base == "self" && !mapped_method_self_is_ref_) {
              error("NBL-T085", "mutating self requires `self: ref T`", s.span);
            }

            out.node = std::move(st);
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            TStmt::Region r;
            r.name = n.name;
            r.body = typecheck_block(n.body, false);
            out.node = std::move(r);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            unsafe_depth_ += 1;
            TStmt::Unsafe u;
            u.body = typecheck_block(n.body, false);
            unsafe_depth_ -= 1;
            out.node = std::move(u);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            auto cond = typecheck_expr(*n.cond);
            if (cond->ty.kind != Ty::Kind::Bool && cond->ty.kind != Ty::Kind::Unknown) {
              error("NBL-T042", "if condition must be Bool", n.cond->span);
            }
            TStmt::If if_stmt;
            if_stmt.cond = std::move(cond);
            if_stmt.then_body = typecheck_block(n.then_body, false);
            if (n.else_body.has_value()) {
              if_stmt.else_body = typecheck_block(*n.else_body, false);
            }
            out.node = std::move(if_stmt);
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            auto a = typecheck_expr(*n.start);
            auto b = typecheck_expr(*n.end);
            if (a->ty.kind != Ty::Kind::Int && a->ty.kind != Ty::Kind::Unknown) {
              error("NBL-T040", "for-range start must be Int", n.start->span);
            }
            if (b->ty.kind != Ty::Kind::Int && b->ty.kind != Ty::Kind::Unknown) {
              error("NBL-T041", "for-range end must be Int", n.end->span);
            }

            // loop body scope includes loop var
            push_scope();
            declare_var(n.var, Ty::Int(), s.span);
            TBlock body = typecheck_block(n.body, true);
            pop_scope();

            TStmt::For f;
            f.var = n.var;
            f.var_ty = Ty::Int();
            f.start = std::move(a);
            f.end = std::move(b);
            f.body = std::move(body);
            out.node = std::move(f);
          }
        },
        s.node);

    end_stmt_borrow_scope();
    return out;
  }

  // Expressions
  TExprPtr typecheck_expr(const Expr& e) {
    auto out = std::make_unique<TExpr>();
    out->span = e.span;

    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::IntLit>) {
            out->ty = Ty::Int();
            out->node = TExpr::IntLit{n.value};
          } else if constexpr (std::is_same_v<N, Expr::FloatLit>) {
            out->ty = Ty::Float();
            out->node = TExpr::FloatLit{n.value};
          } else if constexpr (std::is_same_v<N, Expr::BoolLit>) {
            out->ty = Ty::Bool();
            out->node = TExpr::BoolLit{n.value};
          } else if constexpr (std::is_same_v<N, Expr::StringLit>) {
            out->ty = Ty::String();
            out->node = TExpr::StringLit{n.value};
          } else if constexpr (std::is_same_v<N, Expr::Name>) {
            auto ty = lookup_var(n.ident);
            if (ty) {
              out->ty = *ty;
              out->node = TExpr::VarRef{n.ident, std::nullopt};
              check_borrow_access_conflict(alias_for_whole_name(n.ident), e.span,
                                           BorrowAccessKind::Read);
              return;
            }

            const auto fn_matches = visible_function_candidates(n.ident);
            if (!fn_matches.empty()) {
              if (fn_matches.size() > 1) {
                resolve_visible_function(n.ident, e.span);
                out->ty = Ty::Unknown();
                out->node = TExpr::VarRef{n.ident, std::nullopt};
                return;
              }
              const auto* fn_info = lookup_function_info(fn_matches.front());
              if (fn_info != nullptr) {
                out->ty = callable_ty_from_sig(*fn_info);
                out->node = TExpr::VarRef{n.ident, fn_matches.front()};
                return;
              }
            }

            auto builtin_it = builtins_.find(n.ident);
            if (builtin_it != builtins_.end()) {
              out->ty = callable_ty_from_sig(builtin_it->second);
              out->node = TExpr::VarRef{n.ident, std::nullopt};
              return;
            }

            error("NBL-T050", "unknown name: " + n.ident, e.span);
            out->ty = Ty::Unknown();
            out->node = TExpr::VarRef{n.ident, std::nullopt};
          } else if constexpr (std::is_same_v<N, Expr::Field>) {
            out->ty = Ty::Unknown();
            out->node = TExpr::FieldRef{n.base, n.field};

            auto base_ty = lookup_var(n.base);
            if (!base_ty.has_value()) {
              error("NBL-T050", "unknown name: " + n.base, e.span);
              return;
            }
            if (base_ty->kind != Ty::Kind::Struct) {
              if (base_ty->kind != Ty::Kind::Unknown) {
                error("NBL-T081", "field access on non-struct value: " + n.base, e.span);
              }
              return;
            }

            const TField* fld = find_struct_field(*base_ty, n.field);
            if (fld == nullptr) {
              error("NBL-T080", "unknown field: " + base_ty->name + "." + n.field, e.span);
              return;
            }

            out->ty = fld->ty;
            check_borrow_access_conflict(alias_for_field_name(n.base, n.field), e.span,
                                         BorrowAccessKind::Read);
          } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
            Ty base_ty = Ty::Unknown();
            auto base_ty_opt = lookup_var(n.base);
            if (!base_ty_opt.has_value()) {
              error("NBL-T050", "unknown name: " + n.base, e.span);
            } else {
              base_ty = *base_ty_opt;
              if (base_ty.kind != Ty::Kind::Struct && base_ty.kind != Ty::Kind::Unknown) {
                error("NBL-T081", "field access on non-struct value: " + n.base, e.span);
              }
            }

            std::string mapped_callee = n.base + "_" + n.method;
            if (base_ty.kind == Ty::Kind::Struct) {
              mapped_callee = base_ty.name + "_" + n.method;
            }
            std::vector<TExprPtr> args;
            args.reserve(n.args.size() + 1);
            auto self = std::make_unique<TExpr>();
            self->span = e.span;
            self->ty = base_ty;
            self->node = TExpr::VarRef{n.base, std::nullopt};
            args.push_back(std::move(self));

            out->ty = Ty::Unknown();
            TExpr::Call mapped_call;
            mapped_call.callee = mapped_callee;
            mapped_call.kind = TExpr::CallKind::Direct;
            out->node = std::move(mapped_call);

            if (base_ty.kind != Ty::Kind::Struct) {
              check_borrow_access_conflict(alias_for_whole_name(n.base), e.span,
                                           BorrowAccessKind::Read);
              std::vector<TExprPtr> user_args;
              user_args.reserve(n.args.size());
              for (const auto& a : n.args) user_args.push_back(typecheck_expr(*a));
              for (auto& a : user_args) args.push_back(std::move(a));
              std::get<TExpr::Call>(out->node).args = std::move(args);
              return;
            }

            std::optional<QualifiedName> mapped_symbol;
            const FuncSigInfo* mapped_fn_info = nullptr;
            if (base_ty.qualified_name.has_value()) {
              const QualifiedName exact = qualify(*base_ty.qualified_name, base_ty.name + "_" + n.method);
              mapped_fn_info = lookup_function_info(exact);
              if (mapped_fn_info != nullptr) mapped_symbol = exact;
            }
            if (mapped_fn_info == nullptr) {
              const auto fn_matches = visible_function_candidates(mapped_callee);
              if (!fn_matches.empty()) {
                if (fn_matches.size() > 1) {
                  resolve_visible_function(mapped_callee, e.span);
                  check_borrow_access_conflict(alias_for_whole_name(n.base), e.span,
                                               BorrowAccessKind::Read);
                  std::vector<TExprPtr> user_args;
                  user_args.reserve(n.args.size());
                  for (const auto& a : n.args) user_args.push_back(typecheck_expr(*a));
                  for (auto& a : user_args) args.push_back(std::move(a));
                  std::get<TExpr::Call>(out->node).args = std::move(args);
                  return;
                }
                mapped_symbol = fn_matches.front();
                mapped_fn_info = lookup_function_info(*mapped_symbol);
              }
            }

            if (mapped_fn_info == nullptr) {
              error("NBL-T084", "unknown mapped method: " + mapped_callee, e.span);
              check_borrow_access_conflict(alias_for_whole_name(n.base), e.span,
                                           BorrowAccessKind::Read);
              std::vector<TExprPtr> user_args;
              user_args.reserve(n.args.size());
              for (const auto& a : n.args) user_args.push_back(typecheck_expr(*a));
              for (auto& a : user_args) args.push_back(std::move(a));
              std::get<TExpr::Call>(out->node).args = std::move(args);
              return;
            }

            const auto& fn_info = *mapped_fn_info;
            const auto& sig = fn_info.sig;
            std::get<TExpr::Call>(out->node).resolved_callee = mapped_symbol;

            const bool bad_shape = sig.params.empty() || sig.param_names.empty() ||
                                   sig.params_ref.size() != sig.params.size() ||
                                   sig.param_names[0] != "self" || !ty_equal(sig.params[0], base_ty);
            if (bad_shape) {
              error("NBL-T086",
                    "mapped method signature invalid for " + mapped_callee +
                        ": first parameter must be `self: " + base_ty.name + "` or `self: ref " +
                        base_ty.name + "`",
                    e.span);
              check_borrow_access_conflict(alias_for_whole_name(n.base), e.span,
                                           BorrowAccessKind::Read);
              std::vector<TExprPtr> user_args;
              user_args.reserve(n.args.size());
              for (const auto& a : n.args) user_args.push_back(typecheck_expr(*a));
              for (auto& a : user_args) args.push_back(std::move(a));
              std::get<TExpr::Call>(out->node).args = std::move(args);
              return;
            }

            enforce_unsafe_call_context(mapped_callee, fn_info.is_unsafe_callable, e.span, fn_info.span);
            const bool persist_ref_borrows = false;
            std::get<TExpr::Call>(out->node).args_ref = sig.params_ref;

            std::vector<AliasKey> local_ref_aliases;
            std::vector<BorrowEntry> pending_persistent_borrows;
            if (sig.params_ref[0]) {
              register_ref_borrow(alias_for_whole_name(n.base), e.span, &local_ref_aliases,
                                  &pending_persistent_borrows, persist_ref_borrows);
            } else {
              check_borrow_access_conflict(alias_for_whole_name(n.base), e.span,
                                           BorrowAccessKind::Read);
            }

            const std::size_t expected_user_arity = sig.params.size() - 1;
            if (n.args.size() != expected_user_arity) {
              error("NBL-T062",
                    "call arity mismatch for " + mapped_callee + ": expected " +
                        std::to_string(expected_user_arity) + ", got " +
                        std::to_string(n.args.size()),
                    e.span);
            }

            for (std::size_t i = 0; i < n.args.size(); ++i) {
              const std::size_t sig_i = i + 1; // slot 0 is injected self
              const bool wants_ref = (sig_i < sig.params_ref.size()) && sig.params_ref[sig_i];
              auto a = wants_ref ? typecheck_expr_for_ref_binding(*n.args[i]) : typecheck_expr(*n.args[i]);
              if (sig_i < sig.params.size()) {
                if (wants_ref) {
                  if (!is_ref_lvalue(*a)) {
                    error("NBL-T063", "ref parameter requires an lvalue argument", a->span);
                  } else {
                    register_ref_borrow(extract_alias_key(*a), a->span, &local_ref_aliases,
                                        &pending_persistent_borrows, persist_ref_borrows);
                  }
                }
                if (!ty_equal(a->ty, sig.params[sig_i]) && a->ty.kind != Ty::Kind::Unknown &&
                    sig.params[sig_i].kind != Ty::Kind::Unknown) {
                  error("NBL-T064",
                        "argument type mismatch for " + mapped_callee + ": expected " +
                            ty_to_string(sig.params[sig_i]) + ", got " + ty_to_string(a->ty),
                        a->span);
                }
              }
              args.push_back(std::move(a));
            }

            std::get<TExpr::Call>(out->node).args = std::move(args);
            auto& rewritten = std::get<TExpr::Call>(out->node);
            check_ref_alias_conflicts(mapped_callee, sig.params_ref, rewritten.args, e.span);
            if (persist_ref_borrows) {
              commit_pending_persistent_ref_borrows(pending_persistent_borrows);
            }
            out->ty = sig.ret;
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            if (auto var_ty = lookup_var(n.callee); var_ty.has_value()) {
              if (var_ty->kind != Ty::Kind::Callable) {
                std::vector<TExprPtr> args;
                args.reserve(n.args.size());
                for (const auto& a : n.args) args.push_back(typecheck_expr(*a));
                if (var_ty->kind != Ty::Kind::Unknown) {
                  error("NBL-T065", "callee is not callable: " + n.callee, e.span);
                }
                out->ty = Ty::Unknown();
                TExpr::Call indirect_call;
                indirect_call.callee = n.callee;
                indirect_call.args = std::move(args);
                indirect_call.kind = TExpr::CallKind::Indirect;
                out->node = std::move(indirect_call);
                return;
              }

              enforce_unsafe_call_context(n.callee, var_ty->is_unsafe_callable, e.span, Span{});
              const bool persist_ref_borrows = true;

              if (n.args.size() != var_ty->callable_params.size()) {
                error("NBL-T066",
                      "callable arity mismatch for " + n.callee + ": expected " +
                          std::to_string(var_ty->callable_params.size()) + ", got " +
                          std::to_string(n.args.size()),
                      e.span);
              }

              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              std::vector<AliasKey> local_ref_aliases;
              std::vector<BorrowEntry> pending_persistent_borrows;
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                const bool wants_ref =
                    (i < var_ty->callable_params_ref.size()) && var_ty->callable_params_ref[i];
                auto a = wants_ref ? typecheck_expr_for_ref_binding(*n.args[i])
                                   : typecheck_expr(*n.args[i]);
                if (i < var_ty->callable_params.size()) {
                  if (wants_ref) {
                    if (!is_ref_lvalue(*a)) {
                      error("NBL-T063", "ref parameter requires an lvalue argument", a->span);
                    } else {
                      register_ref_borrow(extract_alias_key(*a), a->span, &local_ref_aliases,
                                          &pending_persistent_borrows, persist_ref_borrows);
                    }
                  }
                  if (!ty_equal(a->ty, var_ty->callable_params[i]) &&
                      a->ty.kind != Ty::Kind::Unknown &&
                      var_ty->callable_params[i].kind != Ty::Kind::Unknown) {
                    error("NBL-T067",
                          "callable argument type mismatch for " + n.callee + ": expected " +
                              ty_to_string(var_ty->callable_params[i]) + ", got " +
                              ty_to_string(a->ty),
                          a->span);
                  }
                }
                args.push_back(std::move(a));
              }
              check_ref_alias_conflicts(n.callee, var_ty->callable_params_ref, args, e.span);
              if (persist_ref_borrows) {
                commit_pending_persistent_ref_borrows(pending_persistent_borrows);
              }

              out->ty = var_ty->callable_ret ? *var_ty->callable_ret : Ty::Unknown();
              TExpr::Call indirect_call;
              indirect_call.callee = n.callee;
              indirect_call.args = std::move(args);
              indirect_call.args_ref = var_ty->callable_params_ref;
              indirect_call.kind = TExpr::CallKind::Indirect;
              out->node = std::move(indirect_call);
              return;
            }

            // Call vs Ctor disambiguation here.
            std::vector<TypeCandidate> struct_matches;
            for (const auto& candidate : visible_type_candidates(n.callee)) {
              if (candidate.kind == TypeDeclKind::Struct) struct_matches.push_back(candidate);
            }
            const auto variant_matches = visible_variant_candidates(n.callee);
            const auto fn_matches = visible_function_candidates(n.callee);
            const bool builtin_visible = builtins_.find(n.callee) != builtins_.end();

            if (struct_matches.size() > 1) {
              std::string message = "ambiguous constructor name: " + n.callee + " (candidates: ";
              for (std::size_t i = 0; i < struct_matches.size(); ++i) {
                if (i != 0) message += ", ";
                message += describe_symbol(struct_matches[i].qualified_name);
              }
              message += ")";
              error("NBL-T098", std::move(message), e.span);
            } else if (variant_matches.size() > 1 && struct_matches.empty() && fn_matches.empty() &&
                       !builtin_visible) {
              // Delay final ambiguity until after payload-based filtering below.
            } else if (fn_matches.size() > 1) {
              resolve_visible_function(n.callee, e.span);
            } else if ((struct_matches.size() == 1 || !variant_matches.empty()) &&
                       (!fn_matches.empty() || builtin_visible ||
                        (struct_matches.size() == 1 && !variant_matches.empty()))) {
              std::string message = "ambiguous callee: " + n.callee + " resolves to both a type and a function";
              error("NBL-T098", std::move(message), e.span);
            }

            const bool callee_ambiguous =
                struct_matches.size() > 1 || fn_matches.size() > 1 ||
                ((struct_matches.size() == 1 || !variant_matches.empty()) &&
                 (!fn_matches.empty() || builtin_visible ||
                  (struct_matches.size() == 1 && !variant_matches.empty())));
            if (callee_ambiguous) {
              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              for (const auto& a : n.args) args.push_back(typecheck_expr(*a));
              out->ty = Ty::Unknown();
              TExpr::Call direct_call;
              direct_call.callee = n.callee;
              direct_call.args = std::move(args);
              direct_call.kind = TExpr::CallKind::Direct;
              out->node = std::move(direct_call);
              return;
            }

            if (!variant_matches.empty() && struct_matches.empty() && fn_matches.empty() && !builtin_visible) {
              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              for (const auto& a : n.args) args.push_back(typecheck_expr(*a));

              struct MatchedVariant {
                VariantCandidate candidate;
                Ty enum_type = Ty::Unknown();
              };

              std::vector<MatchedVariant> matched_variants;
              for (const auto& candidate : variant_matches) {
                const bool expects_zero = variant_payload_is_zero_arg(candidate.variant.payload);
                if ((expects_zero && !args.empty()) || (!expects_zero && args.size() != 1)) continue;

                std::optional<Ty> inferred_arg;
                bool payload_ok = true;
                if (!expects_zero) {
                  payload_ok = infer_enum_type_arg(candidate.variant.payload, args[0]->ty,
                                                   candidate.enum_type_param, inferred_arg);
                }
                if (!payload_ok) continue;

                if (!inferred_arg.has_value()) {
                  continue;
                }

                matched_variants.push_back(
                    {candidate,
                     Ty::Enum(candidate.enum_name.local_name, std::move(*inferred_arg),
                              candidate.enum_name)});
              }

              if (matched_variants.size() > 1) {
                std::string message = "ambiguous enum variant constructor: " + n.callee + " (candidates: ";
                for (std::size_t i = 0; i < matched_variants.size(); ++i) {
                  if (i != 0) message += ", ";
                  message += describe_symbol(matched_variants[i].candidate.enum_name) + "::" +
                             matched_variants[i].candidate.variant.name;
                }
                message += ")";
                error("NBL-T098", std::move(message), e.span);
              } else if (matched_variants.empty()) {
                bool has_zero_arg_variant = false;
                for (const auto& candidate : variant_matches) {
                  if (variant_payload_is_zero_arg(candidate.variant.payload)) {
                    has_zero_arg_variant = true;
                    break;
                  }
                }
                if (has_zero_arg_variant && args.empty()) {
                  error("NBL-T099",
                        "cannot infer enum type argument for variant: " + n.callee,
                        e.span);
                } else {
                  error("NBL-T061",
                        "variant payload type mismatch for " + n.callee,
                        e.span);
                }
                out->ty = Ty::Unknown();
                TExpr::Call direct_call;
                direct_call.callee = n.callee;
                direct_call.args = std::move(args);
                direct_call.kind = TExpr::CallKind::Direct;
                out->node = std::move(direct_call);
                return;
              }

              out->ty = matched_variants.front().enum_type;
              out->node = TExpr::Construct{matched_variants.front().candidate.enum_name.local_name,
                                           matched_variants.front().candidate.enum_name,
                                           matched_variants.front().candidate.variant.name,
                                           matched_variants.front().candidate.variant_index,
                                           std::move(args)};
              return;
            }

            if (struct_matches.size() == 1 && fn_matches.empty() && !builtin_visible) {
              const auto* st_info = lookup_struct_info(struct_matches.front().qualified_name);
              const std::vector<TField> empty_fields;
              const auto& fields = st_info != nullptr ? st_info->fields : empty_fields;
              if (n.args.size() != fields.size()) {
                error("NBL-T060",
                      "constructor arity mismatch for " + n.callee + ": expected " +
                          std::to_string(fields.size()) + ", got " +
                          std::to_string(n.args.size()),
                      e.span);
              }
              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                auto a = typecheck_expr(*n.args[i]);
                if (i < fields.size() && !ty_equal(a->ty, fields[i].ty) &&
                    a->ty.kind != Ty::Kind::Unknown && fields[i].ty.kind != Ty::Kind::Unknown) {
                  error("NBL-T061",
                        "field type mismatch in " + n.callee + ": expected " +
                            ty_to_string(fields[i].ty) + ", got " + ty_to_string(a->ty),
                        a->span);
                }
                args.push_back(std::move(a));
              }
              out->ty = Ty::Struct(n.callee, struct_matches.front().qualified_name);
              out->node =
                  TExpr::Construct{n.callee, struct_matches.front().qualified_name,
                                   std::nullopt, std::nullopt, std::move(args)};
              return;
            }

            const FuncSigInfo* fn_info = nullptr;
            std::optional<QualifiedName> resolved_callee;
            if (fn_matches.size() == 1) {
              resolved_callee = fn_matches.front();
              fn_info = lookup_function_info(*resolved_callee);
            } else if (fn_matches.empty()) {
              auto builtin_it = builtins_.find(n.callee);
              if (builtin_it != builtins_.end()) fn_info = &builtin_it->second;
            }

            if (fn_info == nullptr) {
              error("NBL-T052", "unknown callee: " + n.callee, e.span);
              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              for (const auto& a : n.args) args.push_back(typecheck_expr(*a));
              out->ty = Ty::Unknown();
              TExpr::Call direct_call;
              direct_call.callee = n.callee;
              direct_call.args = std::move(args);
              direct_call.kind = TExpr::CallKind::Direct;
              out->node = std::move(direct_call);
              return;
            }

            const auto& sig = fn_info->sig;
            enforce_unsafe_call_context(n.callee, fn_info->is_unsafe_callable, e.span, fn_info->span);
            const bool persist_ref_borrows = false;
            if (n.args.size() != sig.params.size()) {
              error("NBL-T062",
                    "call arity mismatch for " + n.callee + ": expected " +
                        std::to_string(sig.params.size()) + ", got " +
                        std::to_string(n.args.size()),
                    e.span);
            }
            std::vector<TExprPtr> args;
            args.reserve(n.args.size());
            std::vector<AliasKey> local_ref_aliases;
            std::vector<BorrowEntry> pending_persistent_borrows;
            for (std::size_t i = 0; i < n.args.size(); ++i) {
              const bool wants_ref = (i < sig.params_ref.size()) && sig.params_ref[i];
              auto a = wants_ref ? typecheck_expr_for_ref_binding(*n.args[i]) : typecheck_expr(*n.args[i]);
              if (i < sig.params.size()) {
                if (wants_ref) {
                  if (!is_ref_lvalue(*a)) {
                    error("NBL-T063", "ref parameter requires an lvalue argument", a->span);
                  } else {
                    register_ref_borrow(extract_alias_key(*a), a->span, &local_ref_aliases,
                                        &pending_persistent_borrows, persist_ref_borrows);
                  }
                }
                if (!ty_equal(a->ty, sig.params[i]) && a->ty.kind != Ty::Kind::Unknown &&
                    sig.params[i].kind != Ty::Kind::Unknown) {
                  error("NBL-T064",
                        "argument type mismatch for " + n.callee + ": expected " +
                            ty_to_string(sig.params[i]) + ", got " + ty_to_string(a->ty),
                        a->span);
                }
              }
              args.push_back(std::move(a));
            }
            check_ref_alias_conflicts(n.callee, sig.params_ref, args, e.span);
            if (persist_ref_borrows) {
              commit_pending_persistent_ref_borrows(pending_persistent_borrows);
            }
            out->ty = sig.ret;
            TExpr::Call direct_call;
            direct_call.callee = n.callee;
            direct_call.resolved_callee = resolved_callee;
            direct_call.args = std::move(args);
            direct_call.args_ref = sig.params_ref;
            direct_call.kind = TExpr::CallKind::Direct;
            out->node = std::move(direct_call);
          } else if constexpr (std::is_same_v<N, Expr::Binary>) {
            auto l = typecheck_expr(*n.lhs);
            auto r = typecheck_expr(*n.rhs);
            Ty ty = Ty::Unknown();
            if (l->ty.kind == Ty::Kind::Unknown || r->ty.kind == Ty::Kind::Unknown) {
              ty = Ty::Unknown();
            } else if (n.op == Expr::BinOp::And || n.op == Expr::BinOp::Or) {
              if (l->ty.kind == Ty::Kind::Bool && r->ty.kind == Ty::Kind::Bool) {
                ty = Ty::Bool();
              } else {
                error("NBL-T043", "logical operator requires Bool operands", e.span);
              }
            } else if (n.op == Expr::BinOp::Eq || n.op == Expr::BinOp::Ne) {
              if (ty_equal(l->ty, r->ty) && is_scalar_ty(l->ty)) {
                ty = Ty::Bool();
              } else {
                error("NBL-T072", "equality operator requires matching scalar operands", e.span);
              }
            } else if (n.op == Expr::BinOp::Lt || n.op == Expr::BinOp::Lte ||
                       n.op == Expr::BinOp::Gt || n.op == Expr::BinOp::Gte) {
              if ((l->ty.kind == Ty::Kind::Int || l->ty.kind == Ty::Kind::Float) &&
                  (r->ty.kind == Ty::Kind::Int || r->ty.kind == Ty::Kind::Float)) {
                ty = Ty::Bool();
              } else {
                error("NBL-T073", "comparison operator requires numeric operands", e.span);
              }
            } else if (n.op == Expr::BinOp::Mod) {
              if (l->ty.kind == Ty::Kind::Int && r->ty.kind == Ty::Kind::Int) {
                ty = Ty::Int();
              } else {
                error("NBL-T071", "modulo operator requires Int operands", e.span);
                ty = Ty::Unknown();
              }
            } else if ((l->ty.kind == Ty::Kind::Int || l->ty.kind == Ty::Kind::Float) &&
                       (r->ty.kind == Ty::Kind::Int || r->ty.kind == Ty::Kind::Float)) {
              // numeric
              if (l->ty.kind == Ty::Kind::Float || r->ty.kind == Ty::Kind::Float) {
                ty = Ty::Float();
              } else {
                ty = Ty::Int();
              }
            } else {
              error("NBL-T070", "binary operator requires numeric operands", e.span);
              ty = Ty::Unknown();
            }
            out->ty = ty;
            TExpr::Binary b;
            b.op = n.op;
            b.lhs = std::move(l);
            b.rhs = std::move(r);
            out->node = std::move(b);
          } else if constexpr (std::is_same_v<N, Expr::Unary>) {
            auto inner = typecheck_expr(*n.inner);
            if (inner->ty.kind == Ty::Kind::Bool || inner->ty.kind == Ty::Kind::Unknown) {
              out->ty = (inner->ty.kind == Ty::Kind::Unknown) ? Ty::Unknown() : Ty::Bool();
            } else {
              error("NBL-T074", "logical not requires a Bool operand", e.span);
              out->ty = Ty::Unknown();
            }
            TExpr::Unary unary;
            unary.op = static_cast<decltype(unary.op)>(n.op);
            unary.inner = std::move(inner);
            out->node = std::move(unary);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            auto inner = typecheck_expr(*n.inner);
            out->ty = inner->ty; // directives do not change semantic type in v0.1

            // v0.1 semantics: ownership/representation directives are only defined on
            // constructor sites, and they are not composable (no prefix chains).
            if (std::holds_alternative<TExpr::Prefix>(inner->node)) {
              error("NBL-S001", "multiple ownership/representation directives are not allowed", e.span);
            }
            if (!std::holds_alternative<TExpr::Construct>(inner->node)) {
              error("NBL-S002", "ownership/representation directive must apply to a constructor", e.span);
            }

            TExpr::Prefix p;
            p.kind = n.kind;
            p.inner = std::move(inner);
            out->node = std::move(p);
          }
        },
        e.node);

    return out;
  }
};

} // namespace

namespace {

void upgrade_warnings_to_errors(std::vector<Diagnostic>& diags) {
  for (auto& d : diags) {
    if (d.is_warning()) d.severity = Severity::Error;
  }
}

bool has_errors(const std::vector<Diagnostic>& diags) {
  return std::any_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.is_error(); });
}

} // namespace

TypecheckUnitsResult typecheck(const std::vector<CompilationUnit>& units,
                               const TypecheckOptions& opt) {
  Typechecker tc(opt);
  auto r = tc.run(units);

  if (opt.warnings_as_errors) {
    upgrade_warnings_to_errors(r.diags);
    if (has_errors(r.diags)) r.programs.clear();
  }

  return r;
}

TypecheckResult typecheck(const Program& ast, const TypecheckOptions& opt) {
  Typechecker tc(opt);
  auto r = tc.run(ast);

  if (opt.warnings_as_errors) {
    upgrade_warnings_to_errors(r.diags);
    if (has_errors(r.diags)) r.program.reset();
  }

  return r;
}

} // namespace nebula::frontend
