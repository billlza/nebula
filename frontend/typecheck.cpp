#include "frontend/typecheck.hpp"

#include "frontend/external_contract.hpp"
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
  std::vector<std::string> type_params;
  std::vector<TField> fields;
};

struct EnumInfo {
  Span span{};
  std::vector<std::string> type_params;
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
  std::vector<std::string> enum_type_params;
  TVariant variant;
  std::uint32_t variant_index = 0;
};

class Typechecker {
public:
  explicit Typechecker(TypecheckOptions opt) : opt_(opt) {
    install_builtins();
    install_std_symbols();
  }

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
    validate_external_contract_annotations();
    collect_type_decls();
    collect_function_sigs();
    collect_ui_sigs();
    validate_c_abi_annotations();
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
                } else if constexpr (std::is_same_v<N, Ui>) {
                  out.items.push_back(TItem{n.span, typecheck_ui(n)});
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
  Ty current_surface_ret_ = Ty::Void();
  bool saw_return_ = false;
  bool in_unsafe_fn_ = false;
  int unsafe_depth_ = 0;
  int loop_depth_ = 0;
  bool in_mapped_method_fn_ = false;
  bool mapped_method_self_is_ref_ = false;
  bool suppress_borrow_read_checks_ = false;
  bool in_async_context_ = false;
  bool current_async_fn_ = false;
  bool current_fn_has_ref_param_ = false;

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
            VariantCandidate{enum_name, info.type_params, variant, static_cast<std::uint32_t>(i)});
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

  static bool infer_type_args(const Ty& pattern,
                              const Ty& actual,
                              const std::unordered_set<std::string>& type_params,
                              std::unordered_map<std::string, Ty>& inferred) {
    if (pattern.kind == Ty::Kind::TypeParam && type_params.count(pattern.name) != 0) {
      auto it = inferred.find(pattern.name);
      if (it == inferred.end()) {
        inferred.insert({pattern.name, actual});
        return true;
      }
      return ty_equal(it->second, actual);
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
        if (!infer_type_args(pattern.callable_params[i], actual.callable_params[i], type_params,
                             inferred)) {
          return false;
        }
      }
      if (pattern.callable_ret == nullptr || actual.callable_ret == nullptr) {
        return pattern.callable_ret == nullptr && actual.callable_ret == nullptr;
      }
      return infer_type_args(*pattern.callable_ret, *actual.callable_ret, type_params, inferred);
    }

    if (pattern.type_args.size() != actual.type_args.size()) return false;
    for (std::size_t i = 0; i < pattern.type_args.size(); ++i) {
      if (!infer_type_args(pattern.type_args[i], actual.type_args[i], type_params, inferred)) {
        return false;
      }
    }
    return true;
  }

  static bool variant_payload_is_zero_arg(const Ty& payload) {
    return payload.kind == Ty::Kind::Void;
  }

  static Ty substitute_type_params(const Ty& pattern,
                                   const std::unordered_map<std::string, Ty>& inferred) {
    if (pattern.kind == Ty::Kind::TypeParam) {
      auto it = inferred.find(pattern.name);
      if (it != inferred.end()) return it->second;
    }

    Ty out = pattern;
    if (pattern.kind == Ty::Kind::Callable) {
      out.callable_params.clear();
      out.callable_params.reserve(pattern.callable_params.size());
      for (const auto& param : pattern.callable_params) {
        out.callable_params.push_back(substitute_type_params(param, inferred));
      }
      if (pattern.callable_ret != nullptr) {
        out.callable_ret =
            std::make_shared<Ty>(substitute_type_params(*pattern.callable_ret, inferred));
      } else {
        out.callable_ret.reset();
      }
      return out;
    }

    out.type_args.clear();
    out.type_args.reserve(pattern.type_args.size());
    for (const auto& arg : pattern.type_args) {
      out.type_args.push_back(substitute_type_params(arg, inferred));
    }
    return out;
  }

  static std::unordered_map<std::string, Ty> bind_type_params(const std::vector<std::string>& params,
                                                              const std::vector<Ty>& args) {
    std::unordered_map<std::string, Ty> out;
    const std::size_t count = std::min(params.size(), args.size());
    for (std::size_t i = 0; i < count; ++i) out.insert({params[i], args[i]});
    return out;
  }

  static bool all_type_params_inferred(const std::vector<std::string>& params,
                                       const std::unordered_map<std::string, Ty>& inferred) {
    for (const auto& param : params) {
      if (inferred.find(param) == inferred.end()) return false;
    }
    return true;
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

  const EnumInfo* lookup_enum_info(const Ty& ty) const {
    if (ty.kind != Ty::Kind::Enum || !ty.qualified_name.has_value()) return nullptr;
    return lookup_enum_info(*ty.qualified_name);
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

  static bool is_c_abi_safe_scalar_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Int || ty.kind == Ty::Kind::Float || ty.kind == Ty::Kind::Bool ||
           ty.kind == Ty::Kind::Void;
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

  void emit_invalid_external_contract_annotation(const std::string& message, Span span) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-U003";
    d.message = message;
    d.span = span;
    d.category = "unsafe-boundary";
    d.risk = DiagnosticRisk::High;
    d.cause = "external escape/ownership contract is invalid or attached to the wrong boundary";
    d.impact = "opaque external calls would be analyzed with the wrong ownership and escape assumptions";
    d.suggestions = {"apply external contract annotations only to extern fn boundaries",
                     "use valid param indices and one escape state per parameter",
                     "or remove the invalid contract annotation"};
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

  Ty instantiate_struct_field_type(const Ty& struct_ty, const TField& field) const {
    const auto* info = lookup_struct_info(struct_ty);
    if (info == nullptr) return field.ty;
    return substitute_type_params(field.ty, bind_type_params(info->type_params, struct_ty.type_args));
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
    if (lhs.binding_id == kInvalidBindingId || rhs.binding_id == kInvalidBindingId) return false;
    if (lhs.binding_id != rhs.binding_id) return false;
    if (lhs.base != rhs.base) return false;
    if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return true;
    return field_path_overlaps(lhs.field, rhs.field);
  }

  static std::string overlap_label(const AliasKey& lhs, const AliasKey& rhs) {
    if (lhs.base != rhs.base) return alias_to_string(lhs);
    if (lhs.kind == AliasKey::Kind::Whole || rhs.kind == AliasKey::Kind::Whole) return lhs.base;
    if (lhs.field == rhs.field) return lhs.base + "." + lhs.field;
    if (lhs.field.size() < rhs.field.size() && field_path_overlaps(lhs.field, rhs.field)) {
      return lhs.base + "." + lhs.field;
    }
    if (rhs.field.size() < lhs.field.size() && field_path_overlaps(lhs.field, rhs.field)) {
      return rhs.base + "." + rhs.field;
    }
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

  static std::vector<std::string> split_field_path(std::string_view path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < path.size()) {
      const std::size_t dot = path.find('.', start);
      const std::size_t end = (dot == std::string_view::npos) ? path.size() : dot;
      if (end > start) out.push_back(std::string(path.substr(start, end - start)));
      if (dot == std::string_view::npos) break;
      start = dot + 1;
    }
    return out;
  }

  std::optional<Ty> resolve_field_path(const Ty& base_ty,
                                       const std::string& base_name,
                                       std::string_view field_path,
                                       Span span) {
    Ty current = base_ty;
    std::string access_label = base_name;
    for (const auto& segment : split_field_path(field_path)) {
      if (current.kind != Ty::Kind::Struct) {
        if (current.kind != Ty::Kind::Unknown) {
          error("NBL-T081", "field access on non-struct value: " + access_label, span);
        }
        return std::nullopt;
      }
      const TField* fld = find_struct_field(current, segment);
      if (fld == nullptr) {
        error("NBL-T080", "unknown field: " + current.name + "." + segment, span);
        return std::nullopt;
      }
      current = instantiate_struct_field_type(current, *fld);
      access_label += "." + segment;
    }
    return current;
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

  struct RootedFieldAccess {
    std::string base;
    std::string field;
  };

  static std::string append_field_path(std::string lhs, std::string_view rhs) {
    if (rhs.empty()) return lhs;
    if (!lhs.empty()) lhs.push_back('.');
    lhs += rhs;
    return lhs;
  }

  static std::string describe_expr_brief(const Expr& expr, int depth = 0) {
    if (depth >= 3) return "<expr>";
    return std::visit(
        [&](auto&& node) -> std::string {
          using N = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<N, Expr::IntLit>) {
            return std::to_string(node.value);
          } else if constexpr (std::is_same_v<N, Expr::FloatLit>) {
            std::ostringstream out;
            out << node.value;
            return out.str();
          } else if constexpr (std::is_same_v<N, Expr::BoolLit>) {
            return node.value ? "true" : "false";
          } else if constexpr (std::is_same_v<N, Expr::StringLit>) {
            return "\"...\"";
          } else if constexpr (std::is_same_v<N, Expr::Name>) {
            return node.ident;
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            return node.callee + (node.args.empty() ? "()" : "(...)");
          } else if constexpr (std::is_same_v<N, Expr::Field>) {
            return describe_expr_brief(*node.base, depth + 1) + "." + node.field;
          } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
            return describe_expr_brief(*node.base, depth + 1) + "." + node.method +
                   (node.args.empty() ? "()" : "(...)");
          } else if constexpr (std::is_same_v<N, Expr::Binary>) {
            return "(" + describe_expr_brief(*node.lhs, depth + 1) + " ...)";
          } else if constexpr (std::is_same_v<N, Expr::Unary>) {
            return "!" + describe_expr_brief(*node.inner, depth + 1);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            return describe_expr_brief(*node.inner, depth + 1);
          } else if constexpr (std::is_same_v<N, Expr::Try>) {
            return describe_expr_brief(*node.inner, depth + 1) + "?";
          } else if constexpr (std::is_same_v<N, Expr::Await>) {
            return "await " + describe_expr_brief(*node.inner, depth + 1);
          }
          return "<expr>";
        },
        expr.node);
  }

  std::optional<RootedFieldAccess> extract_rooted_field_access(const TExpr& e) const {
    return std::visit(
        [&](auto&& n) -> std::optional<RootedFieldAccess> {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, TExpr::VarRef>) {
            return RootedFieldAccess{n.name, {}};
          } else if constexpr (std::is_same_v<N, TExpr::FieldRef>) {
            return RootedFieldAccess{n.base, n.field};
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

  struct ZeroPayloadVariantMatch {
    std::string variant_name;
    std::uint32_t variant_index = 0;
  };

  struct ExpectedVariantMatch {
    std::string variant_name;
    std::uint32_t variant_index = 0;
    Ty payload_ty = Ty::Unknown();
    bool zero_payload = false;
  };

  struct ResultTyInfo {
    Ty full_ty = Ty::Unknown();
    Ty ok_ty = Ty::Unknown();
    Ty err_ty = Ty::Unknown();
    std::uint32_t ok_variant_index = 0;
    std::uint32_t err_variant_index = 0;
  };

  std::optional<ResultTyInfo> decompose_result_ty(const Ty& ty) const {
    if (ty.kind != Ty::Kind::Enum || ty.name != "Result") return std::nullopt;
    const auto* enum_info = lookup_enum_info(ty);
    if (enum_info == nullptr) return std::nullopt;
    if (enum_info->type_params.size() != 2 || ty.type_args.size() != 2) return std::nullopt;

    const auto bindings = bind_type_params(enum_info->type_params, ty.type_args);
    std::optional<ResultTyInfo> out;
    out.emplace();
    out->full_ty = ty;
    bool saw_ok = false;
    bool saw_err = false;
    for (std::size_t i = 0; i < enum_info->variants.size(); ++i) {
      const auto& variant = enum_info->variants[i];
      if (variant.name == "Ok") {
        out->ok_ty = substitute_type_params(variant.payload, bindings);
        out->ok_variant_index = static_cast<std::uint32_t>(i);
        saw_ok = true;
      } else if (variant.name == "Err") {
        out->err_ty = substitute_type_params(variant.payload, bindings);
        out->err_variant_index = static_cast<std::uint32_t>(i);
        saw_err = true;
      }
    }
    if (!saw_ok || !saw_err) return std::nullopt;
    return out;
  }

  std::optional<ExpectedVariantMatch> try_resolve_expected_variant(
      const std::string& name,
      const Ty& expected_ty) const {
    if (expected_ty.kind != Ty::Kind::Enum) return std::nullopt;
    const auto* enum_info = lookup_enum_info(expected_ty);
    if (enum_info == nullptr) return std::nullopt;
    const auto bindings = bind_type_params(enum_info->type_params, expected_ty.type_args);
    for (std::size_t i = 0; i < enum_info->variants.size(); ++i) {
      const auto& variant = enum_info->variants[i];
      if (variant.name != name) continue;
      return ExpectedVariantMatch{variant.name,
                                  static_cast<std::uint32_t>(i),
                                  substitute_type_params(variant.payload, bindings),
                                  variant_payload_is_zero_arg(variant.payload)};
    }
    return std::nullopt;
  }

  std::optional<ZeroPayloadVariantMatch> try_resolve_expected_zero_payload_variant(
      const std::string& name,
      const Ty& expected_ty) const {
    auto matched = try_resolve_expected_variant(name, expected_ty);
    if (!matched.has_value() || !matched->zero_payload) return std::nullopt;
    return ZeroPayloadVariantMatch{matched->variant_name, matched->variant_index};
  }

  TExprPtr typecheck_try_expr(const Expr& e, const Expr::Try& try_expr, const Ty* expected_ok) {
    Ty expected_result = Ty::Unknown();
    const Ty* operand_expected = nullptr;
    if (expected_ok != nullptr && expected_ok->kind != Ty::Kind::Unknown) {
      if (auto fn_result = decompose_result_ty(current_ret_); fn_result.has_value()) {
        expected_result = Ty::Enum(fn_result->full_ty.name,
                                   {*expected_ok, fn_result->err_ty},
                                   fn_result->full_ty.qualified_name);
        operand_expected = &expected_result;
      }
    }

    auto inner =
        operand_expected != nullptr ? typecheck_expr_with_expected(*try_expr.inner, operand_expected)
                                    : typecheck_expr(*try_expr.inner);
    const auto operand_result = decompose_result_ty(inner->ty);
    const auto fn_result = decompose_result_ty(current_ret_);
    if (!fn_result.has_value()) {
      error("NBL-T125",
            "`?` requires the enclosing function to return Result<T, E>",
            e.span);
    }
    if (!operand_result.has_value()) {
      error("NBL-T126", "`?` operand must have type Result<T, E>", try_expr.inner->span);
    } else if (fn_result.has_value() &&
               !ty_equal(operand_result->err_ty, fn_result->err_ty) &&
               operand_result->err_ty.kind != Ty::Kind::Unknown &&
               fn_result->err_ty.kind != Ty::Kind::Unknown) {
      error("NBL-T127",
            "`?` error type mismatch: operand has " + ty_to_string(operand_result->err_ty) +
                ", function returns Result<_, " + ty_to_string(fn_result->err_ty) + ">",
            e.span);
    }

    auto out = std::make_unique<TExpr>();
    out->span = e.span;
    out->ty = operand_result.has_value() ? operand_result->ok_ty : Ty::Unknown();
    out->node = TExpr::Try{std::move(inner)};
    return out;
  }

  TExprPtr typecheck_await_expr(const Expr& e, const Expr::Await& await_expr) {
    auto inner = typecheck_expr(*await_expr.inner);
    if (!in_async_context_) {
      error("NBL-T132", "`await` is only valid inside async functions", e.span);
    }
    if (current_async_fn_ && current_fn_has_ref_param_) {
      error("NBL-T134",
            "async functions with ref parameters cannot suspend across await in phase 1",
            e.span);
    }

    Ty value_ty = Ty::Unknown();
    if (auto future_inner = future_inner_ty(inner->ty); future_inner.has_value()) {
      value_ty = *future_inner;
    } else if (auto task_inner = task_inner_ty(inner->ty); task_inner.has_value()) {
      value_ty = *task_inner;
    } else if (inner->ty.kind != Ty::Kind::Unknown) {
      error("NBL-T133", "`await` operand must have type Future<T> or Task<T>", inner->span);
    }

    auto out = std::make_unique<TExpr>();
    out->span = e.span;
    out->ty = value_ty;
    out->node = TExpr::Await{std::move(inner)};
    return out;
  }

  TExprPtr typecheck_expr_with_expected(const Expr& e, const Ty* expected) {
    if (expected == nullptr || expected->kind == Ty::Kind::Unknown) {
      return typecheck_expr(e);
    }

    return std::visit(
        [&](auto&& n) -> TExprPtr {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Expr::Name>) {
            if (lookup_var(n.ident).has_value()) return typecheck_expr(e);
            if (!visible_function_candidates(n.ident).empty()) return typecheck_expr(e);
            if (builtins_.find(n.ident) != builtins_.end()) return typecheck_expr(e);
            auto matched = try_resolve_expected_zero_payload_variant(n.ident, *expected);
            if (!matched.has_value()) return typecheck_expr(e);

            auto out = std::make_unique<TExpr>();
            out->span = e.span;
            out->ty = *expected;
            out->node = TExpr::Construct{expected->name,
                                         expected->qualified_name,
                                         matched->variant_name,
                                         matched->variant_index,
                                         {}};
            return out;
          } else if constexpr (std::is_same_v<N, Expr::Call>) {
            if (!visible_function_candidates(n.callee).empty()) return typecheck_expr(e);
            if (builtins_.find(n.callee) != builtins_.end()) return typecheck_expr(e);
            if (auto matched = try_resolve_expected_variant(n.callee, *expected); matched.has_value()) {
              if (matched->zero_payload) {
                if (!n.args.empty()) return typecheck_expr(e);
                auto out = std::make_unique<TExpr>();
                out->span = e.span;
                out->ty = *expected;
                out->node = TExpr::Construct{expected->name,
                                             expected->qualified_name,
                                             matched->variant_name,
                                             matched->variant_index,
                                             {}};
                return out;
              }
              if (n.args.size() == 1) {
                std::vector<TExprPtr> args;
                args.push_back(typecheck_expr_with_expected(*n.args[0], &matched->payload_ty));
                auto out = std::make_unique<TExpr>();
                out->span = e.span;
                out->ty = *expected;
                out->node = TExpr::Construct{expected->name,
                                             expected->qualified_name,
                                             matched->variant_name,
                                             matched->variant_index,
                                             std::move(args)};
                return out;
              }
            }
            return typecheck_expr(e);
          } else if constexpr (std::is_same_v<N, Expr::Prefix>) {
            auto inner = typecheck_expr_with_expected(*n.inner, expected);
            auto out = std::make_unique<TExpr>();
            out->span = e.span;
            out->ty = inner->ty;
            if (std::holds_alternative<TExpr::Prefix>(inner->node)) {
              error("NBL-S001", "multiple ownership/representation directives are not allowed", e.span);
            }
            if (!std::holds_alternative<TExpr::Construct>(inner->node)) {
              error("NBL-S002",
                    "ownership/representation directive must apply to a constructor",
                    e.span);
            }
            TExpr::Prefix p;
            p.kind = n.kind;
            p.inner = std::move(inner);
            out->node = std::move(p);
            return out;
          } else if constexpr (std::is_same_v<N, Expr::Try>) {
            return typecheck_try_expr(e, n, expected);
          } else if constexpr (std::is_same_v<N, Expr::Await>) {
            return typecheck_await_expr(e, n);
          } else if constexpr (std::is_same_v<N, Expr::Match>) {
            return typecheck_match_expr(e, n, expected);
          } else {
            return typecheck_expr(e);
          }
        },
        e.node);
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

  static QualifiedName std_name(std::string module_name, std::string local_name) {
    QualifiedName name;
    name.package_name = "std";
    name.module_name = std::move(module_name);
    name.local_name = std::move(local_name);
    return name;
  }

  static bool matches_std_name(const std::optional<QualifiedName>& name,
                               std::string_view module_name,
                               std::string_view local_name) {
    return name.has_value() && name->package_name == "std" && name->module_name == module_name &&
           name->local_name == local_name;
  }

  static Ty std_struct_ty(std::string module_name,
                          std::string local_name,
                          std::vector<Ty> type_args = {}) {
    const QualifiedName qualified = std_name(std::move(module_name), std::move(local_name));
    return Ty::Struct(qualified.local_name, std::move(type_args), qualified);
  }

  static Ty std_enum_ty(std::string module_name,
                        std::string local_name,
                        std::vector<Ty> type_args = {}) {
    const QualifiedName qualified = std_name(std::move(module_name), std::move(local_name));
    return Ty::Enum(qualified.local_name, std::move(type_args), qualified);
  }

  static Ty future_ty(Ty inner) { return std_struct_ty("task", "Future", {std::move(inner)}); }
  static Ty task_ty(Ty inner) { return std_struct_ty("task", "Task", {std::move(inner)}); }
  static Ty duration_ty() { return std_struct_ty("time", "Duration"); }
  static Ty json_ty() { return std_struct_ty("json", "Json"); }
  static Ty result_ty(Ty ok, Ty err) { return std_enum_ty("result", "Result", {std::move(ok), std::move(err)}); }

  static bool is_future_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Struct && matches_std_name(ty.qualified_name, "task", "Future") &&
           ty.type_args.size() == 1;
  }

  static bool is_task_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Struct && matches_std_name(ty.qualified_name, "task", "Task") &&
           ty.type_args.size() == 1;
  }

  static bool is_duration_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Struct && matches_std_name(ty.qualified_name, "time", "Duration") &&
           ty.type_args.empty();
  }

  static bool is_json_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::Struct && matches_std_name(ty.qualified_name, "json", "Json") &&
           ty.type_args.empty();
  }

  static bool is_ui_prop_ty(const Ty& ty) {
    return ty.kind == Ty::Kind::String || ty.kind == Ty::Kind::Int || ty.kind == Ty::Kind::Bool ||
           ty.kind == Ty::Kind::Unknown || is_json_ty(ty);
  }

  static bool is_non_constructible_runtime_backed_struct(const QualifiedName& name) {
    if (name.package_name == "std") {
      if (name.module_name == "bytes" && name.local_name == "Bytes") return true;
      if (name.module_name == "json" && name.local_name == "Json") return true;
      if (name.module_name == "json" && name.local_name == "JsonArrayBuilder") return true;
      if (name.module_name == "http" &&
          (name.local_name == "RouteParams2" || name.local_name == "RouteParams3")) {
        return true;
      }
      if (name.module_name == "net" &&
          (name.local_name == "SocketAddr" || name.local_name == "TcpListener" ||
           name.local_name == "TcpStream")) {
        return true;
      }
    }
    if (name.package_name == "nebula-crypto") {
      if (name.module_name == "aead" && name.local_name == "ChaCha20Poly1305Key") {
        return true;
      }
      if (name.module_name == "pqc.kem" &&
          (name.local_name == "MlKem768KeyPair" || name.local_name == "MlKem768PublicKey" ||
           name.local_name == "MlKem768SecretKey" || name.local_name == "MlKem768Ciphertext" ||
           name.local_name == "MlKem768SharedSecret" ||
           name.local_name == "MlKem768Encapsulation")) {
        return true;
      }
      if (name.module_name == "pqc.sign" &&
          (name.local_name == "MlDsa65KeyPair" || name.local_name == "MlDsa65PublicKey" ||
           name.local_name == "MlDsa65SecretKey" || name.local_name == "MlDsa65Signature")) {
        return true;
      }
    }
    if (name.package_name == "nebula-tls") {
      if (name.module_name == "client" &&
          (name.local_name == "TlsTrustStore" || name.local_name == "TlsServerName" ||
           name.local_name == "TlsClientIdentity" || name.local_name == "TlsVersionPolicy" ||
           name.local_name == "TlsAlpnPolicy" || name.local_name == "ClientConfig" ||
           name.local_name == "TlsClientStream")) {
        return true;
      }
    }
    if (name.package_name == "nebula-tls-server") {
      if (name.module_name == "server" &&
          (name.local_name == "ServerIdentity" || name.local_name == "ServerConfig" ||
           name.local_name == "TlsListener" || name.local_name == "TlsServerStream")) {
        return true;
      }
    }
    if (name.package_name == "nebula-db-sqlite") {
      if (name.module_name == "sqlite" &&
          (name.local_name == "Connection" || name.local_name == "Transaction" ||
           name.local_name == "ResultSet" || name.local_name == "Row")) {
        return true;
      }
    }
    return false;
  }

  static std::string runtime_backed_struct_kind_label(const QualifiedName& name) {
    if (name.package_name == "std") return "runtime-managed std type";
    if (name.package_name == "nebula-crypto") return "runtime-backed crypto type";
    if (name.package_name == "nebula-tls") return "runtime-backed tls type";
    if (name.package_name == "nebula-tls-server") return "runtime-backed tls server type";
    if (name.package_name == "nebula-db-sqlite") return "runtime-backed sqlite type";
    return "runtime-backed type";
  }

  static std::optional<Ty> future_inner_ty(const Ty& ty) {
    if (!is_future_ty(ty)) return std::nullopt;
    return ty.type_args.front();
  }

  static std::optional<Ty> task_inner_ty(const Ty& ty) {
    if (!is_task_ty(ty)) return std::nullopt;
    return ty.type_args.front();
  }

  void register_std_struct(std::string module_name,
                           std::string local_name,
                           std::vector<std::string> type_params) {
    const QualifiedName qualified = std_name(std::move(module_name), std::move(local_name));
    StructInfo info;
    info.span = Span{};
    info.type_params = std::move(type_params);
    structs_.insert_or_assign(qualified, std::move(info));
    struct_names_[qualified.local_name].push_back(qualified);
  }

  void register_std_enum_result() {
    const QualifiedName qualified = std_name("result", "Result");
    EnumInfo info;
    info.span = Span{};
    info.type_params = {"T", "E"};
    TVariant ok;
    ok.name = "Ok";
    ok.payload = Ty::TypeParam("T");
    TVariant err;
    err.name = "Err";
    err.payload = Ty::TypeParam("E");
    info.variants.push_back(std::move(ok));
    info.variants.push_back(std::move(err));
    enums_.insert_or_assign(qualified, std::move(info));
    enum_names_[qualified.local_name].push_back(qualified);
  }

  void register_std_function(std::string module_name,
                             std::string local_name,
                             std::vector<std::string> type_params,
                             std::vector<Ty> params,
                             Ty ret) {
    const QualifiedName qualified = std_name(std::move(module_name), std::move(local_name));
    TFunctionSig sig;
    sig.name = qualified.local_name;
    sig.qualified_name = qualified;
    sig.type_params = std::move(type_params);
    sig.params = std::move(params);
    sig.params_ref.assign(sig.params.size(), false);
    sig.ret = ret;
    sig.body_ret = ret;
    funcs_.insert_or_assign(qualified, FuncSigInfo{Span{}, sig, false});
    func_names_[qualified.local_name].push_back(qualified);
  }

  void install_std_symbols() {
    register_std_function("task", "spawn", {"T"}, {future_ty(Ty::TypeParam("T"))},
                          task_ty(Ty::TypeParam("T")));
    register_std_function("task", "join", {"T"}, {task_ty(Ty::TypeParam("T"))},
                          task_ty(Ty::TypeParam("T")));
    register_std_function("task", "block_on", {"T"}, {future_ty(Ty::TypeParam("T"))},
                          Ty::TypeParam("T"));
    register_std_function("time", "timeout", {"T"},
                          {duration_ty(), future_ty(Ty::TypeParam("T"))},
                          future_ty(result_ty(Ty::TypeParam("T"), Ty::String())));
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

  void validate_external_contract_annotations() {
    for (const auto& unit : units_) {
      for (const auto& it : unit.ast->items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              const bool has_contract =
                  std::any_of(n.annotations.begin(), n.annotations.end(), [](const std::string& annotation) {
                    return is_external_escape_contract_annotation(annotation);
                  });
              if (!has_contract) return;

              if constexpr (!std::is_same_v<N, Function>) {
                emit_invalid_external_contract_annotation(
                    "external escape contract annotations are only valid on extern fn boundaries",
                    n.span);
              } else {
                if (!n.is_extern) {
                  emit_invalid_external_contract_annotation(
                      "external escape contract annotations require extern fn, not a defined Nebula function",
                      n.span);
                  return;
                }

                const auto contract = parse_external_escape_contract(n.annotations, n.params.size());
                for (const auto& error_message : contract.errors) {
                  emit_invalid_external_contract_annotation(error_message, n.span);
                }
              }
            },
            it.node);
      }
    }
  }

  void validate_c_abi_annotations() {
    for (const auto& unit : units_) {
      for (const auto& it : unit.ast->items) {
        std::visit(
            [&](auto&& n) {
              using N = std::decay_t<decltype(n)>;
              const bool wants_export = has_annotation(n.annotations, "export");
              const bool wants_abi_c = has_annotation(n.annotations, "abi_c");
              if (!wants_export && !wants_abi_c) return;

              if constexpr (!std::is_same_v<N, Function>) {
                error("NBL-T128",
                      "C ABI export annotations are only valid on functions",
                      n.span);
              } else {
                if (wants_export != wants_abi_c) {
                  error("NBL-T129",
                        "C ABI export requires both @export and @abi_c",
                        n.span);
                  return;
                }
                if (n.is_extern) {
                  error("NBL-T129",
                        "C ABI export must be defined by a Nebula function, not extern fn",
                        n.span);
                  return;
                }
                if (!n.type_params.empty()) {
                  error("NBL-T130",
                        "C ABI export does not support generic functions",
                        n.span);
                  return;
                }
                const QualifiedName qualified = qualify(unit, n.name);
                auto sig_it = funcs_.find(qualified);
                if (sig_it == funcs_.end()) return;
                const auto& sig = sig_it->second.sig;
                for (std::size_t i = 0; i < sig.params.size(); ++i) {
                  if (i < sig.params_ref.size() && sig.params_ref[i]) {
                    error("NBL-T130",
                          "C ABI export does not support ref parameters",
                          n.params[i].span);
                    return;
                  }
                  if (!is_c_abi_safe_scalar_ty(sig.params[i]) &&
                      sig.params[i].kind != Ty::Kind::Unknown) {
                    error("NBL-T131",
                          "C ABI export parameter type is not ABI-safe: " +
                              ty_to_string(sig.params[i]),
                          n.params[i].span);
                    return;
                  }
                }
                if (!is_c_abi_safe_scalar_ty(sig.ret) && sig.ret.kind != Ty::Kind::Unknown) {
                  error("NBL-T131",
                        "C ABI export return type is not ABI-safe: " + ty_to_string(sig.ret),
                        n.span);
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
                structs_.insert({qualified, StructInfo{n.span, n.type_params, {}}});
                struct_names_[n.name].push_back(qualified);
              } else if constexpr (std::is_same_v<N, Enum>) {
                if (structs_.find(qualified) != structs_.end() ||
                    enums_.find(qualified) != enums_.end()) {
                  error("NBL-T001", "duplicate type name: " + n.name, n.span);
                  return;
                }
                enums_.insert({qualified, EnumInfo{n.span, n.type_params, {}}});
                enum_names_[n.name].push_back(qualified);
              }
            },
            it.node);
      }
    }
  }

  Ty resolve_type(const Type& syn, const std::vector<std::string>* type_params) {
    if (syn.callable_ret) {
      std::vector<Ty> params;
      std::vector<bool> params_ref;
      params.reserve(syn.callable_params.size());
      params_ref.reserve(syn.callable_params.size());
      for (const auto& p : syn.callable_params) {
        params.push_back(resolve_type(p, type_params));
        params_ref.push_back(false);
      }
      Ty ret = resolve_type(*syn.callable_ret, type_params);
      return Ty::Callable(std::move(params), std::move(ret), syn.is_unsafe_callable,
                          std::move(params_ref));
    }

    // builtins
    if (syn.name == "Int") return Ty::Int();
    if (syn.name == "Float") return Ty::Float();
    if (syn.name == "Bool") return Ty::Bool();
    if (syn.name == "String") return Ty::String();
    if (syn.name == "Void") return Ty::Void();

    // type params (only valid inside generic item definitions)
    if (type_params != nullptr &&
        std::find(type_params->begin(), type_params->end(), syn.name) != type_params->end()) {
      if (!syn.args.empty()) {
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
    const std::size_t type_param_count = [&]() -> std::size_t {
      if (!resolved.has_value()) return 0;
      if (resolved->kind == TypeDeclKind::Struct) {
        auto it = structs_.find(resolved->qualified_name);
        return it != structs_.end() ? it->second.type_params.size() : 0;
      }
      auto it = enums_.find(resolved->qualified_name);
      return it != enums_.end() ? it->second.type_params.size() : 0;
    }();

    if (syn.args.empty()) {
      if (is_struct) {
        if (type_param_count != 0) {
          error("NBL-T012", "missing type arguments for type: " + syn.name, syn.span);
          return Ty::Unknown();
        }
        return Ty::Struct(syn.name, {}, resolved->qualified_name);
      }
      if (is_enum) {
        if (type_param_count != 0) {
          error("NBL-T012", "missing type arguments for type: " + syn.name, syn.span);
          return Ty::Unknown();
        }
        return Ty::Enum(syn.name, {}, resolved->qualified_name);
      }
      error("NBL-T010", "unknown type: " + syn.name, syn.span);
      return Ty::Unknown();
    }

    if (!is_struct && !is_enum) {
      error("NBL-T011", "type does not take type arguments: " + syn.name, syn.span);
      return Ty::Unknown();
    }
    if (syn.args.size() != type_param_count) {
      error("NBL-T011",
            "type argument count mismatch for " + syn.name + ": expected " +
                std::to_string(type_param_count) + ", got " + std::to_string(syn.args.size()),
            syn.span);
      return Ty::Unknown();
    }

    std::vector<Ty> args;
    args.reserve(syn.args.size());
    for (const auto& arg_syn : syn.args) {
      args.push_back(resolve_type(arg_syn, type_params));
    }
    if (is_struct) {
      return Ty::Struct(syn.name, std::move(args), resolved->qualified_name);
    }
    return Ty::Enum(syn.name, std::move(args), resolved->qualified_name);
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
        if (fn.is_extern && !fn.type_params.empty()) {
          error("NBL-T124", "generic extern functions are not supported", fn.span);
          continue;
        }

        TFunctionSig sig;
        sig.name = fn.name;
        sig.qualified_name = qualified;
        sig.type_params = fn.type_params;
        sig.is_async = fn.is_async;
        sig.param_names.reserve(fn.params.size());
        sig.params.reserve(fn.params.size());
        sig.params_ref.reserve(fn.params.size());
        for (const auto& p : fn.params) {
          sig.param_names.push_back(p.name);
          sig.params.push_back(resolve_type(p.type, &fn.type_params));
          sig.params_ref.push_back(p.is_ref);
        }
        if (fn.return_type.has_value()) {
          sig.body_ret = resolve_type(*fn.return_type, &fn.type_params);
        } else {
          sig.body_ret = Ty::Void();
        }
        sig.ret = sig.is_async ? future_ty(sig.body_ret) : sig.body_ret;
        sig.is_unsafe_callable = has_annotation(fn.annotations, "unsafe");

        funcs_.insert_or_assign(qualified, FuncSigInfo{fn.span, sig, sig.is_unsafe_callable});
        func_names_[fn.name].push_back(qualified);
      }
    }
    current_unit_ = nullptr;
  }

  void collect_ui_sigs() {
    for (const auto& unit : units_) {
      current_unit_ = &unit;
      for (const auto& it : unit.ast->items) {
        if (!std::holds_alternative<Ui>(it.node)) continue;
        const auto& ui = std::get<Ui>(it.node);
        const QualifiedName qualified = qualify(unit, ui.name);
        if (has_local_type_name(unit, ui.name)) {
          error("NBL-UI002", "ui name collides with type name: " + ui.name, ui.span);
          continue;
        }
        if (funcs_.find(qualified) != funcs_.end()) {
          error("NBL-UI003", "duplicate callable name: " + ui.name, ui.span);
          continue;
        }

        TFunctionSig sig;
        sig.name = ui.name;
        sig.qualified_name = qualified;
        sig.ret = json_ty();
        sig.body_ret = sig.ret;
        sig.param_names.reserve(ui.params.size());
        sig.params.reserve(ui.params.size());
        sig.params_ref.reserve(ui.params.size());
        for (const auto& p : ui.params) {
          sig.param_names.push_back(p.name);
          sig.params.push_back(resolve_type(p.type, nullptr));
          sig.params_ref.push_back(p.is_ref);
        }
        funcs_.insert_or_assign(qualified, FuncSigInfo{ui.span, sig, false});
        func_names_[ui.name].push_back(qualified);
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
                  tf.ty = resolve_type(f.type, &info.type_params);
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
                  tv.payload = resolve_type(v.payload, &info.type_params);
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
    for (const auto& arg : ty.type_args) collect_struct_refs_from_ty(arg, out);
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

  BindingId fresh_binding_id() { return next_binding_id_++; }

  BindingId declare_var(const std::string& name,
                        Ty ty,
                        Span span,
                        BindingId preferred_binding_id = kInvalidBindingId) {
    if (scopes_.empty()) push_scope();
    auto& m = scopes_.back();
    if (m.count(name)) {
      error("NBL-T020", "duplicate binding: " + name, span);
      return kInvalidBindingId;
    }
    BindingId binding_id = preferred_binding_id;
    if (binding_id == kInvalidBindingId) {
      binding_id = fresh_binding_id();
    } else if (binding_id >= next_binding_id_) {
      next_binding_id_ = binding_id + 1;
    }
    m.insert({name, VarInfo{std::move(ty), binding_id}});
    return binding_id;
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
    out.type_params = st.type_params;
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
    out.type_params = en.type_params;
    auto it = enums_.find(out.qualified_name);
    if (it != enums_.end()) out.variants = it->second.variants;
    return out;
  }

  TUiProp typecheck_ui_prop(const UiProp& prop) {
    TUiProp out;
    out.span = prop.span;
    out.name = prop.name;
    out.value = typecheck_expr(*prop.value);
    if (!is_ui_prop_ty(out.value->ty)) {
      error("NBL-UI004",
            "ui property value must be String, Int, Bool, or Json, got " + ty_to_string(out.value->ty),
            prop.span);
    }
    return out;
  }

  std::vector<TUiNode> typecheck_ui_nodes(const std::vector<UiNode>& nodes) {
    std::vector<TUiNode> out;
    out.reserve(nodes.size());
    for (const auto& node : nodes) out.push_back(typecheck_ui_node(node));
    return out;
  }

  TUiNode typecheck_ui_node(const UiNode& node) {
    TUiNode out;
    out.span = node.span;
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, UiNode::View>) {
            TUiNode::View view;
            view.component = n.component;
            view.component_span = n.component_span;
            if (n.props.size() > 4) {
              error("NBL-UI005", "ui view supports at most 4 properties in the V1 preview", node.span);
            }
            if (n.children.size() > 3) {
              error("NBL-UI006", "ui view supports at most 3 child views in the V1 preview", node.span);
            }
            view.props.reserve(n.props.size());
            for (const auto& prop : n.props) view.props.push_back(typecheck_ui_prop(prop));
            view.children = typecheck_ui_nodes(n.children);
            out.node = std::move(view);
          } else if constexpr (std::is_same_v<N, UiNode::If>) {
            TUiNode::If if_node;
            if_node.cond = typecheck_expr(*n.cond);
            if (if_node.cond->ty.kind != Ty::Kind::Bool &&
                if_node.cond->ty.kind != Ty::Kind::Unknown) {
              error("NBL-UI001", "ui if condition must be Bool", n.cond->span);
            }
            if_node.then_children = typecheck_ui_nodes(n.then_children);
            out.node = std::move(if_node);
          } else if constexpr (std::is_same_v<N, UiNode::For>) {
            TUiNode::For for_node;
            for_node.var = n.var;
            for_node.var_ty = Ty::Unknown();
            for_node.iterable = typecheck_expr(*n.iterable);
            push_scope();
            for_node.binding_id = declare_var(for_node.var, for_node.var_ty, n.var_span);
            for_node.body = typecheck_ui_nodes(n.body);
            pop_scope();
            out.node = std::move(for_node);
          }
        },
        node.node);
    return out;
  }

  TUi typecheck_ui(const Ui& ui) {
    TUi out;
    out.span = ui.span;
    out.annotations = ui.annotations;
    out.name = ui.name;
    out.qualified_name = qualify(*current_unit_, ui.name);

    const auto prev_current_ret = current_ret_;
    const auto prev_current_surface_ret = current_surface_ret_;
    const bool prev_saw_return = saw_return_;
    const bool prev_in_async_context = in_async_context_;
    const bool prev_current_async_fn = current_async_fn_;
    const bool prev_current_fn_has_ref_param = current_fn_has_ref_param_;
    const bool prev_in_mapped_method_fn = in_mapped_method_fn_;
    const bool prev_mapped_method_self_is_ref = mapped_method_self_is_ref_;

    current_ret_ = Ty::Void();
    current_surface_ret_ = Ty::Void();
    saw_return_ = false;
    in_async_context_ = false;
    current_async_fn_ = false;
    current_fn_has_ref_param_ = false;
    in_mapped_method_fn_ = false;
    mapped_method_self_is_ref_ = false;
    scopes_.clear();
    next_binding_id_ = 1;
    clear_persistent_borrow_state();
    push_scope();

    out.params.reserve(ui.params.size());
    for (const auto& p : ui.params) {
      TParam tp;
      tp.span = p.span;
      tp.is_ref = p.is_ref;
      tp.name = p.name;
      tp.ty = resolve_type(p.type, nullptr);
      tp.binding_id = declare_var(tp.name, tp.ty, tp.span);
      out.params.push_back(std::move(tp));
    }
    out.body = typecheck_ui_nodes(ui.body);

    pop_scope();
    clear_persistent_borrow_state();
    current_ret_ = prev_current_ret;
    current_surface_ret_ = prev_current_surface_ret;
    saw_return_ = prev_saw_return;
    in_async_context_ = prev_in_async_context;
    current_async_fn_ = prev_current_async_fn;
    current_fn_has_ref_param_ = prev_current_fn_has_ref_param;
    in_mapped_method_fn_ = prev_in_mapped_method_fn;
    mapped_method_self_is_ref_ = prev_mapped_method_self_is_ref;
    return out;
  }

  TFunction typecheck_function(const Function& fn) {
    TFunction out;
    out.span = fn.span;
    out.annotations = fn.annotations;
    out.name = fn.name;
    out.qualified_name = qualify(*current_unit_, fn.name);
    out.type_params = fn.type_params;
    out.is_async = fn.is_async;
    out.is_extern = fn.is_extern;
    auto sig_it = funcs_.find(out.qualified_name);
    const TFunctionSig* sig = nullptr;
    if (sig_it != funcs_.end()) sig = &sig_it->second.sig;
    out.ret = sig ? sig->body_ret : Ty::Void();

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
    const int prev_loop_depth = loop_depth_;
    const bool prev_in_mapped_method_fn = in_mapped_method_fn_;
    const bool prev_mapped_method_self_is_ref = mapped_method_self_is_ref_;
    const bool prev_in_async_context = in_async_context_;
    const bool prev_current_async_fn = current_async_fn_;
    const bool prev_current_fn_has_ref_param = current_fn_has_ref_param_;
    in_unsafe_fn_ = sig_it != funcs_.end() ? sig_it->second.is_unsafe_callable
                                           : has_annotation(fn.annotations, "unsafe");
    unsafe_depth_ = 0;
    loop_depth_ = 0;
    in_mapped_method_fn_ = false;
    mapped_method_self_is_ref_ = false;
    in_async_context_ = fn.is_async;
    current_async_fn_ = fn.is_async;
    current_fn_has_ref_param_ =
        std::any_of(fn.params.begin(), fn.params.end(), [](const Param& p) { return p.is_ref; });
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
    for (auto& p : out.params) {
      p.binding_id = declare_var(p.name, p.ty, p.span);
    }
    if (!fn.is_extern && fn.body.has_value()) {
      out.body = typecheck_block(*fn.body, true);
      out.body = lower_effectful_block(std::move(*out.body));
    }
    pop_scope();
    clear_persistent_borrow_state();

    if (!fn.is_extern && current_ret_.kind != Ty::Kind::Void &&
        (!out.body.has_value() || !block_guarantees_return(*out.body))) {
      error("NBL-T030", "missing return in non-void function: " + fn.name, fn.span);
    }

    in_unsafe_fn_ = prev_in_unsafe_fn;
    unsafe_depth_ = prev_unsafe_depth;
    loop_depth_ = prev_loop_depth;
    in_mapped_method_fn_ = prev_in_mapped_method_fn;
    mapped_method_self_is_ref_ = prev_mapped_method_self_is_ref;
    in_async_context_ = prev_in_async_context;
    current_async_fn_ = prev_current_async_fn;
    current_fn_has_ref_param_ = prev_current_fn_has_ref_param;
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
  TExprPtr make_var_ref_expr(const std::string& name,
                             Ty ty,
                             Span span,
                             BindingId binding_id = kInvalidBindingId) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = std::move(ty);
    if (binding_id == kInvalidBindingId) {
      if (auto binding = lookup_binding(name); binding.has_value()) binding_id = binding->binding_id;
    }
    out->node = TExpr::VarRef{name, std::nullopt, binding_id};
    return out;
  }

  TExprPtr make_not_expr(TExprPtr inner, Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = inner->ty.kind == Ty::Kind::Unknown ? Ty::Unknown() : Ty::Bool();
    TExpr::Unary unary;
    unary.op = Expr::UnaryOp::Not;
    unary.inner = std::move(inner);
    out->node = std::move(unary);
    return out;
  }

  TExprPtr make_enum_is_variant_expr(TExprPtr subject,
                                     const Ty& subject_ty,
                                     std::string variant_name,
                                     std::uint32_t variant_index,
                                     Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = Ty::Bool();
    out->node = TExpr::EnumIsVariant{std::move(subject), subject_ty.qualified_name,
                                     std::move(variant_name), variant_index};
    return out;
  }

  TExprPtr make_enum_payload_expr(TExprPtr subject,
                                  const Ty& subject_ty,
                                  Ty payload_ty,
                                  std::string variant_name,
                                  std::uint32_t variant_index,
                                  Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = std::move(payload_ty);
    out->node = TExpr::EnumPayload{std::move(subject), subject_ty.qualified_name,
                                   std::move(variant_name), variant_index};
    return out;
  }

  TExprPtr make_rooted_field_ref_expr(const std::string& base,
                                      std::string field,
                                      Ty field_ty,
                                      Span span,
                                      BindingId base_binding_id = kInvalidBindingId) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = std::move(field_ty);
    if (base_binding_id == kInvalidBindingId) {
      if (auto binding = lookup_binding(base); binding.has_value()) base_binding_id = binding->binding_id;
    }
    out->node = TExpr::FieldRef{base, std::move(field), base_binding_id};
    return out;
  }

  struct BindingDecl {
    std::string name;
    Ty ty = Ty::Unknown();
    Span span{};
    BindingId binding_id = kInvalidBindingId;
  };

  BindingDecl make_binding_decl(std::string name, Ty ty, Span span) {
    return BindingDecl{std::move(name), std::move(ty), std::move(span), fresh_binding_id()};
  }

  struct StructBindingPlan {
    std::string field_name;
    BindingDecl binding;
    Span field_span{};
  };

  struct StructBindingAnalysis {
    std::vector<StructBindingPlan> plans;
    std::vector<BindingDecl> recovery_bindings;
  };

  StructBindingAnalysis analyze_struct_binding_fields(
      const std::vector<StructBindingField>& fields,
      const Ty& value_ty,
      Span value_span,
      std::string non_struct_message) {
    StructBindingAnalysis analysis;

    const StructInfo* struct_info = nullptr;
    const bool non_struct_value =
        value_ty.kind != Ty::Kind::Struct && value_ty.kind != Ty::Kind::Unknown;
    if (value_ty.kind == Ty::Kind::Struct) {
      struct_info = lookup_struct_info(value_ty);
    } else if (non_struct_value) {
      error("NBL-T116", std::move(non_struct_message), value_span);
    }

    auto add_recovery_binding = [&](const StructBindingField& field) {
      if (field.skip || !field.binding_name.has_value()) return;
      analysis.recovery_bindings.push_back(
          make_binding_decl(*field.binding_name, Ty::Unknown(), field.binding_span));
    };

    auto push_binding_plan = [&](const StructBindingField& field, Ty field_ty) {
      analysis.plans.push_back(StructBindingPlan{
          field.field_name, make_binding_decl(*field.binding_name, std::move(field_ty), field.binding_span),
          field.span});
    };

    if (fields.empty()) {
      return analysis;
    }

    std::unordered_set<std::string> seen_fields;
    std::unordered_set<std::string> seen_bindings;
    analysis.plans.reserve(fields.size());
    analysis.recovery_bindings.reserve(fields.size());

    for (const auto& field : fields) {
      if (!seen_fields.insert(field.field_name).second) {
        error("NBL-T117", "duplicate field in struct destructuring: " + field.field_name, field.span);
        if (!field.skip && field.binding_name.has_value() &&
            seen_bindings.insert(*field.binding_name).second) {
          add_recovery_binding(field);
        }
        continue;
      }

      if (!field.skip && field.binding_name.has_value() &&
          !seen_bindings.insert(*field.binding_name).second) {
        error("NBL-T020", "duplicate binding: " + *field.binding_name, field.binding_span);
        continue;
      }

      if (non_struct_value) {
        add_recovery_binding(field);
        continue;
      }

      Ty field_ty = Ty::Unknown();
      if (struct_info != nullptr) {
        const auto it = std::find_if(struct_info->fields.begin(),
                                     struct_info->fields.end(),
                                     [&](const TField& candidate) {
                                       return candidate.name == field.field_name;
                                     });
        if (it == struct_info->fields.end()) {
          error("NBL-T080", "unknown field: " + value_ty.name + "." + field.field_name, field.span);
          add_recovery_binding(field);
          continue;
        }
        field_ty = instantiate_struct_field_type(value_ty, *it);
      }

      if (field.skip || !field.binding_name.has_value()) continue;
      push_binding_plan(field, std::move(field_ty));
    }

    return analysis;
  }

  TBlock typecheck_block_with_bindings(const Block& b,
                                       bool propagate_persistent_borrows_to_parent,
                                       const std::vector<BindingDecl>& bindings) {
    TBlock out;
    out.span = b.span;
    out.stmts.reserve(b.stmts.size());
    push_scope();
    push_persistent_borrow_frame();
    for (const auto& binding : bindings) {
      declare_var(binding.name, binding.ty, binding.span, binding.binding_id);
    }
    for (const auto& s : b.stmts) {
      append_typechecked_stmt(out.stmts, s);
    }
    pop_scope();
    pop_persistent_borrow_frame(propagate_persistent_borrows_to_parent);
    return out;
  }

  TBlock typecheck_block(const Block& b, bool propagate_persistent_borrows_to_parent) {
    return typecheck_block_with_bindings(b, propagate_persistent_borrows_to_parent,
                                         std::vector<BindingDecl>{});
  }

  std::string fresh_synthetic_name(const char* prefix) {
    std::string name;
    do {
      name = std::string(prefix) + std::to_string(next_binding_id_++);
    } while (lookup_var(name).has_value());
    return name;
  }

  void append_typechecked_let_struct_stmt(std::vector<TStmt>& out,
                                          const Stmt& s,
                                          const Stmt::LetStruct& let_stmt) {
    auto value = typecheck_expr(*let_stmt.value);
    const Ty value_ty = value->ty;
    auto binding_analysis = analyze_struct_binding_fields(
        let_stmt.fields, value_ty, s.span, "struct destructuring requires a Struct value");

    const std::string temp_name = fresh_synthetic_name("__nebula_destructure_");
    TStmt temp_stmt;
    temp_stmt.span = s.span;
    TStmt::Let temp_let;
    temp_let.name = temp_name;
    temp_let.ty = value_ty;
    temp_let.value = std::move(value);
    temp_let.binding_id = declare_var(temp_let.name, temp_let.ty, s.span);
    temp_stmt.node = std::move(temp_let);
    out.push_back(std::move(temp_stmt));

    for (const auto& plan : binding_analysis.plans) {
      TStmt binding_stmt;
      binding_stmt.span = plan.binding.span;
      TStmt::Let binding_let;
      binding_let.name = plan.binding.name;
      binding_let.ty = plan.binding.ty;
      binding_let.value = make_rooted_field_ref_expr(
          temp_name, plan.field_name, plan.binding.ty, plan.field_span, temp_let.binding_id);
      binding_let.binding_id =
          declare_var(binding_let.name, binding_let.ty, plan.binding.span, plan.binding.binding_id);
      binding_stmt.node = std::move(binding_let);
      out.push_back(std::move(binding_stmt));
    }

    for (const auto& binding : binding_analysis.recovery_bindings) {
      (void)declare_var(binding.name, binding.ty, binding.span, binding.binding_id);
    }
  }

  struct MatchArmInput {
    const Pattern* pattern = nullptr;
    Span span{};
  };

  struct MatchArmAnalysis {
    using Kind = TExpr::Match::Arm::Kind;

    Kind kind = Kind::Wildcard;
    Span span{};
    bool bool_value = false;
    std::string variant_name;
    std::uint32_t variant_index = 0;
    std::optional<BindingDecl> payload_binding;
    std::vector<StructBindingPlan> payload_struct_bindings;
    Ty payload_ty = Ty::Unknown();
    std::vector<BindingDecl> bindings;
  };

  struct MatchAnalysis {
    TExprPtr subject;
    Ty subject_ty = Ty::Unknown();
    std::vector<MatchArmAnalysis> arms;
    bool exhaustive = false;
  };

  TExprPtr typecheck_expr_with_bindings(const Expr& e,
                                        const Ty* expected,
                                        const std::vector<BindingDecl>& bindings,
                                        bool propagate_persistent_borrows_to_parent = false) {
    push_scope();
    push_persistent_borrow_frame();
    for (const auto& binding : bindings) {
      declare_var(binding.name, binding.ty, binding.span, binding.binding_id);
    }
    auto result = typecheck_expr_with_expected(e, expected);
    pop_scope();
    pop_persistent_borrow_frame(propagate_persistent_borrows_to_parent);
    return result;
  }

  MatchAnalysis analyze_match_patterns(TExprPtr subject,
                                       const std::vector<MatchArmInput>& arm_inputs,
                                       Span match_span) {
    MatchAnalysis result;
    result.subject_ty = subject->ty;
    result.subject = std::move(subject);
    result.arms.reserve(arm_inputs.size());

    bool wildcard_seen = false;
    bool bool_true_seen = false;
    bool bool_false_seen = false;
    std::vector<bool> enum_variants_seen;
    const EnumInfo* enum_info = nullptr;
    if (result.subject_ty.kind == Ty::Kind::Enum) {
      enum_info = lookup_enum_info(result.subject_ty);
      if (enum_info != nullptr) enum_variants_seen.assign(enum_info->variants.size(), false);
    }

    const bool subject_unknown = result.subject_ty.kind == Ty::Kind::Unknown;
    const bool subject_bool = result.subject_ty.kind == Ty::Kind::Bool;
    const bool subject_enum = result.subject_ty.kind == Ty::Kind::Enum && enum_info != nullptr;

    if (!subject_bool && !subject_enum && !subject_unknown) {
      error("NBL-T110", "match subject must be Bool or Enum", result.subject->span);
    }

    for (const auto& arm_input : arm_inputs) {
      MatchArmAnalysis arm;
      arm.span = arm_input.span;
      const Pattern& pattern = *arm_input.pattern;

      std::visit(
          [&](auto&& pattern_node) {
            using P = std::decay_t<decltype(pattern_node)>;
            if constexpr (std::is_same_v<P, Pattern::Wildcard>) {
              arm.kind = MatchArmAnalysis::Kind::Wildcard;
              if (wildcard_seen) {
                error("NBL-T111", "duplicate or unreachable wildcard match arm", pattern.span);
              }
              wildcard_seen = true;
            } else if constexpr (std::is_same_v<P, Pattern::BoolLit>) {
              arm.kind = MatchArmAnalysis::Kind::Bool;
              arm.bool_value = pattern_node.value;
              if (!subject_bool && !subject_unknown) {
                error("NBL-T113", "Bool pattern does not match non-Bool subject", pattern.span);
              }
              if (subject_bool) {
                bool& seen = pattern_node.value ? bool_true_seen : bool_false_seen;
                if (wildcard_seen || seen) {
                  error("NBL-T111", "duplicate or unreachable Bool match arm", pattern.span);
                }
                seen = true;
              }
            } else if constexpr (std::is_same_v<P, Pattern::Variant>) {
              arm.kind = MatchArmAnalysis::Kind::EnumVariant;
              arm.variant_name = pattern_node.name;
              if (!subject_enum && !subject_unknown) {
                error("NBL-T113", "enum variant pattern does not match non-enum subject", pattern.span);
                if (pattern_node.payload.has_value()) {
                    if (const auto* binding_payload =
                            std::get_if<Pattern::Variant::BindingPayload>(&*pattern_node.payload)) {
                    arm.bindings.push_back(
                        make_binding_decl(binding_payload->name, Ty::Unknown(), binding_payload->binding_span));
                  } else if (const auto* struct_payload =
                                 std::get_if<Pattern::Variant::StructPayload>(&*pattern_node.payload)) {
                    auto analysis =
                        analyze_struct_binding_fields(struct_payload->fields,
                                                      Ty::Unknown(),
                                                      pattern.span,
                                                      "enum payload struct destructuring requires a Struct payload");
                    for (const auto& plan : analysis.plans) {
                      arm.bindings.push_back(plan.binding);
                    }
                    for (const auto& binding : analysis.recovery_bindings) {
                      arm.bindings.push_back(binding);
                    }
                  }
                }
                return;
              }
              if (!subject_enum) {
                if (pattern_node.payload.has_value()) {
                  if (const auto* binding_payload =
                          std::get_if<Pattern::Variant::BindingPayload>(&*pattern_node.payload)) {
                    arm.bindings.push_back(
                        make_binding_decl(binding_payload->name, Ty::Unknown(), binding_payload->binding_span));
                  } else if (const auto* struct_payload =
                                 std::get_if<Pattern::Variant::StructPayload>(&*pattern_node.payload)) {
                    auto analysis =
                        analyze_struct_binding_fields(struct_payload->fields,
                                                      Ty::Unknown(),
                                                      pattern.span,
                                                      "enum payload struct destructuring requires a Struct payload");
                    for (const auto& plan : analysis.plans) {
                      arm.bindings.push_back(plan.binding);
                    }
                    for (const auto& binding : analysis.recovery_bindings) {
                      arm.bindings.push_back(binding);
                    }
                  }
                }
                return;
              }

              const TVariant* matched_variant = nullptr;
              std::uint32_t matched_index = 0;
              for (std::size_t i = 0; i < enum_info->variants.size(); ++i) {
                if (enum_info->variants[i].name == pattern_node.name) {
                  matched_variant = &enum_info->variants[i];
                  matched_index = static_cast<std::uint32_t>(i);
                  break;
                }
              }
              if (matched_variant == nullptr) {
                error("NBL-T112",
                      "unknown enum variant in match for " + result.subject_ty.name + ": " +
                          pattern_node.name,
                      pattern.span);
                return;
              }

              arm.variant_index = matched_index;
              if (wildcard_seen || enum_variants_seen[matched_index]) {
                error("NBL-T111", "duplicate or unreachable enum match arm", pattern.span);
              }
              enum_variants_seen[matched_index] = true;

              const bool zero_payload = variant_payload_is_zero_arg(matched_variant->payload);
              if (zero_payload) {
                if (pattern_node.payload.has_value() &&
                    !std::holds_alternative<Pattern::Variant::EmptyPayload>(*pattern_node.payload)) {
                  error("NBL-T115",
                        "zero-payload enum variant pattern cannot bind a payload",
                        pattern.span);
                  if (const auto* binding_payload =
                          std::get_if<Pattern::Variant::BindingPayload>(&*pattern_node.payload)) {
                    arm.bindings.push_back(
                        make_binding_decl(binding_payload->name, Ty::Unknown(), binding_payload->binding_span));
                  } else if (const auto* struct_payload =
                                 std::get_if<Pattern::Variant::StructPayload>(&*pattern_node.payload)) {
                    auto analysis =
                        analyze_struct_binding_fields(struct_payload->fields,
                                                      Ty::Unknown(),
                                                      pattern.span,
                                                      "enum payload struct destructuring requires a Struct payload");
                    for (const auto& plan : analysis.plans) {
                      arm.bindings.push_back(plan.binding);
                    }
                    for (const auto& binding : analysis.recovery_bindings) {
                      arm.bindings.push_back(binding);
                    }
                  }
                }
                return;
              }

              if (!pattern_node.payload.has_value() ||
                  std::holds_alternative<Pattern::Variant::EmptyPayload>(*pattern_node.payload)) {
                error("NBL-T115",
                      "payload enum variant pattern must use Variant(name), Variant(_), or Variant({ ... })",
                      pattern.span);
                return;
              }

              arm.payload_ty =
                  substitute_type_params(matched_variant->payload,
                                         bind_type_params(enum_info->type_params,
                                                          result.subject_ty.type_args));
              if (const auto* binding_payload =
                      std::get_if<Pattern::Variant::BindingPayload>(&*pattern_node.payload)) {
                arm.payload_binding =
                    make_binding_decl(binding_payload->name, arm.payload_ty, binding_payload->binding_span);
                arm.bindings.push_back(*arm.payload_binding);
              } else if (const auto* struct_payload =
                             std::get_if<Pattern::Variant::StructPayload>(&*pattern_node.payload)) {
                auto analysis =
                    analyze_struct_binding_fields(struct_payload->fields,
                                                  arm.payload_ty,
                                                  pattern.span,
                                                  "enum payload struct destructuring requires a Struct payload");
                arm.payload_struct_bindings = std::move(analysis.plans);
                for (const auto& plan : arm.payload_struct_bindings) {
                  arm.bindings.push_back(plan.binding);
                }
                for (const auto& binding : analysis.recovery_bindings) {
                  arm.bindings.push_back(binding);
                }
              }
            }
          },
          pattern.node);

      result.arms.push_back(std::move(arm));
    }

    result.exhaustive = wildcard_seen;
    if (subject_bool && !wildcard_seen && (!bool_true_seen || !bool_false_seen)) {
      std::string missing;
      if (!bool_true_seen) missing += "true";
      if (!bool_false_seen) {
        if (!missing.empty()) missing += ", ";
        missing += "false";
      }
      error("NBL-T114", "non-exhaustive Bool match: missing " + missing, match_span);
    } else if (subject_bool && (wildcard_seen || (bool_true_seen && bool_false_seen))) {
      result.exhaustive = true;
    }

    if (subject_enum && !wildcard_seen) {
      std::vector<std::string> missing;
      for (std::size_t i = 0; i < enum_info->variants.size(); ++i) {
        if (!enum_variants_seen[i]) missing.push_back(enum_info->variants[i].name);
      }
      if (!missing.empty()) {
        std::string message = "non-exhaustive enum match on " + result.subject_ty.name + ": missing ";
        for (std::size_t i = 0; i < missing.size(); ++i) {
          if (i != 0) message += ", ";
          message += missing[i];
        }
        error("NBL-T114", std::move(message), match_span);
      } else {
        result.exhaustive = true;
      }
    }

    if (subject_unknown) {
      result.exhaustive = true;
    }

    return result;
  }

  TExprPtr typecheck_match_expr(const Expr& e, const Expr::Match& match_expr, const Ty* expected) {
    std::vector<MatchArmInput> arm_inputs;
    arm_inputs.reserve(match_expr.arms.size());
    for (const auto& arm : match_expr.arms) {
      arm_inputs.push_back(MatchArmInput{&arm->pattern, arm->span});
    }

    MatchAnalysis analysis =
        analyze_match_patterns(typecheck_expr(*match_expr.subject), arm_inputs, e.span);

    Ty result_ty = (expected != nullptr && expected->kind != Ty::Kind::Unknown) ? *expected : Ty::Unknown();

    auto out = std::make_unique<TExpr>();
    out->span = e.span;

    TExpr::Match match_out;
    match_out.subject = std::move(analysis.subject);
    match_out.arms.reserve(match_expr.arms.size());
    match_out.exhaustive = analysis.exhaustive;

    auto make_binding = [](const BindingDecl& binding) {
      return TExpr::Match::Binding{binding.name, binding.ty, binding.span, binding.binding_id};
    };
    auto make_struct_binding = [&](const StructBindingPlan& plan) {
      return TExpr::Match::StructBinding{plan.field_name, make_binding(plan.binding), plan.field_span};
    };

    for (std::size_t i = 0; i < match_expr.arms.size(); ++i) {
      const auto& source_arm = *match_expr.arms[i];
      const auto& analysis_arm = analysis.arms[i];
      const Ty* arm_expected = (result_ty.kind != Ty::Kind::Unknown) ? &result_ty : nullptr;
      auto arm_value = typecheck_expr_with_bindings(*source_arm.value, arm_expected, analysis_arm.bindings);

      if (result_ty.kind == Ty::Kind::Unknown && arm_value->ty.kind != Ty::Kind::Unknown) {
        result_ty = arm_value->ty;
      } else if (arm_expected != nullptr && arm_value->ty.kind != Ty::Kind::Unknown &&
                 arm_expected->kind != Ty::Kind::Unknown && !ty_equal(arm_value->ty, *arm_expected)) {
        error("NBL-T121",
              "match arm type mismatch: expected " + ty_to_string(*arm_expected) + ", got " +
                  ty_to_string(arm_value->ty),
              arm_value->span);
      }

      auto typed_arm = std::make_unique<TMatchExprArm>();
      typed_arm->kind = analysis_arm.kind;
      typed_arm->span = analysis_arm.span;
      typed_arm->bool_value = analysis_arm.bool_value;
      typed_arm->variant_name = analysis_arm.variant_name;
      typed_arm->variant_index = analysis_arm.variant_index;
      typed_arm->payload_ty = analysis_arm.payload_ty;
      if (analysis_arm.payload_binding.has_value()) {
        typed_arm->payload_binding = make_binding(*analysis_arm.payload_binding);
      }
      typed_arm->payload_struct_bindings.reserve(analysis_arm.payload_struct_bindings.size());
      for (const auto& plan : analysis_arm.payload_struct_bindings) {
        typed_arm->payload_struct_bindings.push_back(make_struct_binding(plan));
      }
      typed_arm->value = std::move(arm_value);
      match_out.arms.push_back(std::move(typed_arm));
    }

    out->ty = result_ty;
    out->node = std::move(match_out);
    return out;
  }

  void append_typechecked_match_stmt(std::vector<TStmt>& out,
                                     const Stmt& s,
                                     const Stmt::Match& match_stmt) {
    std::vector<MatchArmInput> arm_inputs;
    arm_inputs.reserve(match_stmt.arms.size());
    for (const auto& arm : match_stmt.arms) {
      arm_inputs.push_back(MatchArmInput{&arm.pattern, arm.span});
    }

    MatchAnalysis analysis =
        analyze_match_patterns(typecheck_expr(*match_stmt.subject), arm_inputs, s.span);
    const Ty subject_ty = analysis.subject_ty;

    std::vector<TBlock> typed_bodies;
    typed_bodies.reserve(match_stmt.arms.size());
    for (std::size_t i = 0; i < match_stmt.arms.size(); ++i) {
      typed_bodies.push_back(
          typecheck_block_with_bindings(match_stmt.arms[i].body, false, analysis.arms[i].bindings));
    }

    std::string temp_name = fresh_synthetic_name("__nebula_match_");
    TStmt temp_stmt;
    temp_stmt.span = s.span;
    TStmt::Let temp_let;
    temp_let.name = temp_name;
    temp_let.ty = subject_ty;
    temp_let.value = std::move(analysis.subject);
    temp_let.binding_id = declare_var(temp_let.name, temp_let.ty, s.span);
    temp_stmt.node = std::move(temp_let);
    out.push_back(std::move(temp_stmt));

    if (analysis.arms.empty()) return;

    auto materialize_arm_body = [&](MatchArmAnalysis& arm, TBlock body) -> TBlock {
      if (arm.kind == MatchArmAnalysis::Kind::EnumVariant &&
          (arm.payload_binding.has_value() || !arm.payload_struct_bindings.empty())) {
        std::vector<TStmt> prefix;
        if (arm.payload_binding.has_value()) {
          TStmt binding_stmt;
          binding_stmt.span = arm.payload_binding->span;
          TStmt::Let binding_let;
          binding_let.name = arm.payload_binding->name;
          binding_let.ty = arm.payload_binding->ty;
          binding_let.binding_id = arm.payload_binding->binding_id;
          binding_let.value = make_enum_payload_expr(
              make_var_ref_expr(temp_name, subject_ty, arm.span, temp_let.binding_id), subject_ty, arm.payload_ty,
              arm.variant_name, arm.variant_index, arm.payload_binding->span);
          binding_stmt.node = std::move(binding_let);
          prefix.push_back(std::move(binding_stmt));
        } else {
          const std::string payload_temp_name = fresh_synthetic_name("__nebula_payload_");

          TStmt payload_stmt;
          payload_stmt.span = arm.span;
          TStmt::Let payload_let;
          payload_let.name = payload_temp_name;
          payload_let.ty = arm.payload_ty;
          payload_let.binding_id = fresh_binding_id();
          payload_let.value = make_enum_payload_expr(
              make_var_ref_expr(temp_name, subject_ty, arm.span, temp_let.binding_id), subject_ty, arm.payload_ty,
              arm.variant_name, arm.variant_index, arm.span);
          payload_stmt.node = std::move(payload_let);
          prefix.push_back(std::move(payload_stmt));

          for (const auto& plan : arm.payload_struct_bindings) {
            TStmt binding_stmt;
            binding_stmt.span = plan.binding.span;
            TStmt::Let binding_let;
            binding_let.name = plan.binding.name;
            binding_let.ty = plan.binding.ty;
            binding_let.binding_id = plan.binding.binding_id;
            binding_let.value = make_rooted_field_ref_expr(payload_temp_name,
                                                           plan.field_name,
                                                           plan.binding.ty,
                                                           plan.field_span,
                                                           payload_let.binding_id);
            binding_stmt.node = std::move(binding_let);
            prefix.push_back(std::move(binding_stmt));
          }
        }

        std::vector<TStmt> merged;
        merged.reserve(prefix.size() + body.stmts.size());
        for (auto& stmt : prefix) merged.push_back(std::move(stmt));
        for (auto& stmt : body.stmts) merged.push_back(std::move(stmt));
        body.stmts = std::move(merged);
      }
      return body;
    };

    auto build_if_chain = [&](auto&& self, std::size_t index) -> TStmt {
      MatchArmAnalysis& arm = analysis.arms[index];
      TStmt if_stmt_out;
      if_stmt_out.span = arm.span;
      if (index == 0) if_stmt_out.annotations = s.annotations;

      TStmt::If if_stmt;
      if (arm.kind == MatchArmAnalysis::Kind::Bool) {
        auto subject_ref = make_var_ref_expr(temp_name, subject_ty, arm.span, temp_let.binding_id);
        if_stmt.cond = arm.bool_value ? std::move(subject_ref)
                                      : make_not_expr(std::move(subject_ref), arm.span);
      } else {
        auto subject_ref = make_var_ref_expr(temp_name, subject_ty, arm.span, temp_let.binding_id);
        if_stmt.cond = make_enum_is_variant_expr(std::move(subject_ref), subject_ty,
                                                 arm.variant_name, arm.variant_index, arm.span);
      }

      if_stmt.then_body = materialize_arm_body(arm, std::move(typed_bodies[index]));

      if (index + 1 < analysis.arms.size()) {
        if (analysis.arms[index + 1].kind == MatchArmAnalysis::Kind::Wildcard) {
          if_stmt.else_body =
              materialize_arm_body(analysis.arms[index + 1], std::move(typed_bodies[index + 1]));
        } else if (analysis.exhaustive && index + 1 == analysis.arms.size() - 1) {
          if_stmt.else_body =
              materialize_arm_body(analysis.arms[index + 1], std::move(typed_bodies[index + 1]));
        } else {
          TBlock else_block;
          else_block.span = analysis.arms[index + 1].span;
          else_block.stmts.push_back(self(self, index + 1));
          if_stmt.else_body = std::move(else_block);
        }
      }

      if_stmt_out.node = std::move(if_stmt);
      return if_stmt_out;
    };

    if (analysis.arms.front().kind == MatchArmAnalysis::Kind::Wildcard) {
      TBlock body = materialize_arm_body(analysis.arms.front(), std::move(typed_bodies.front()));
      for (auto& stmt : body.stmts) {
        out.push_back(std::move(stmt));
      }
      return;
    }

    out.push_back(build_if_chain(build_if_chain, 0));
  }

  void append_typechecked_stmt(std::vector<TStmt>& out, const Stmt& s) {
    begin_stmt_borrow_scope();
    if (std::holds_alternative<Stmt::LetStruct>(s.node)) {
      append_typechecked_let_struct_stmt(out, s, std::get<Stmt::LetStruct>(s.node));
      end_stmt_borrow_scope();
      return;
    }
    if (std::holds_alternative<Stmt::Match>(s.node)) {
      append_typechecked_match_stmt(out, s, std::get<Stmt::Match>(s.node));
      end_stmt_borrow_scope();
      return;
    }
    out.push_back(typecheck_stmt(s));
    end_stmt_borrow_scope();
  }

  TStmt typecheck_stmt(const Stmt& s) {
    TStmt out;
    out.span = s.span;
    out.annotations = s.annotations;

    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Stmt::Let>) {
            auto v = typecheck_expr(*n.value);
            TStmt::Let st;
            st.name = n.name;
            st.ty = v->ty;
            st.value = std::move(v);
            st.binding_id = declare_var(st.name, st.ty, s.span);
            out.node = std::move(st);
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            auto v = typecheck_expr_with_expected(
                *n.value, current_ret_.kind == Ty::Kind::Void ? nullptr : &current_ret_);
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
            auto dst = lookup_binding(n.name);
            auto v = typecheck_expr_with_expected(*n.value, dst.has_value() ? &dst->ty : nullptr);
            TStmt::AssignVar st;
            st.name = n.name;
            st.ty = Ty::Unknown();
            st.value = std::move(v);
            st.binding_id = dst.has_value() ? dst->binding_id : kInvalidBindingId;

            if (!dst.has_value()) {
              error("NBL-T083", "invalid assignment target: " + n.name, s.span);
            } else {
              check_borrow_access_conflict(alias_for_whole_name(n.name), s.span,
                                           BorrowAccessKind::Write);
              st.ty = dst->ty;
              if (!ty_equal(st.value->ty, dst->ty) && st.value->ty.kind != Ty::Kind::Unknown &&
                  dst->ty.kind != Ty::Kind::Unknown) {
                error("NBL-T082",
                      "assignment type mismatch: expected " + ty_to_string(dst->ty) + ", got " +
                          ty_to_string(st.value->ty),
                      s.span);
              }
            }
            out.node = std::move(st);
          } else if constexpr (std::is_same_v<N, Stmt::AssignField>) {
            std::optional<Ty> expected_field_ty;
            auto base_binding = lookup_binding(n.base);
            if (base_binding.has_value()) {
              expected_field_ty = resolve_field_path(base_binding->ty, n.base, n.field, s.span);
            }
            auto v = typecheck_expr_with_expected(*n.value,
                                                  expected_field_ty.has_value() ? &*expected_field_ty
                                                                                : nullptr);
            TStmt::AssignField st;
            st.base = n.base;
            st.field = n.field;
            st.ty = Ty::Unknown();
            st.value = std::move(v);
            st.base_binding_id =
                base_binding.has_value() ? base_binding->binding_id : kInvalidBindingId;

            if (!base_binding.has_value()) {
              error("NBL-T083", "invalid assignment target: " + n.base + "." + n.field, s.span);
            } else {
              check_borrow_access_conflict(
                  alias_for_field_name(n.base, n.field), s.span, BorrowAccessKind::Write);
              if (auto final_ty = expected_field_ty;
                  final_ty.has_value()) {
                st.ty = *final_ty;
                if (!ty_equal(st.value->ty, *final_ty) && st.value->ty.kind != Ty::Kind::Unknown &&
                    final_ty->kind != Ty::Kind::Unknown) {
                  error("NBL-T082",
                        "field assignment type mismatch: expected " + ty_to_string(*final_ty) +
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
            loop_depth_ += 1;
            push_scope();
            const BindingId loop_binding_id = declare_var(n.var, Ty::Int(), s.span);
            TBlock body = typecheck_block(n.body, true);
            pop_scope();
            loop_depth_ -= 1;

            TStmt::For f;
            f.var = n.var;
            f.var_ty = Ty::Int();
            f.start = std::move(a);
            f.end = std::move(b);
            f.body = std::move(body);
            f.binding_id = loop_binding_id;
            out.node = std::move(f);
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            auto cond = typecheck_expr(*n.cond);
            if (cond->ty.kind != Ty::Kind::Bool && cond->ty.kind != Ty::Kind::Unknown) {
              error("NBL-T118", "while condition must be Bool", n.cond->span);
            }

            loop_depth_ += 1;
            TBlock body = typecheck_block(n.body, true);
            loop_depth_ -= 1;

            TStmt::While while_stmt;
            while_stmt.cond = std::move(cond);
            while_stmt.body = std::move(body);
            out.node = std::move(while_stmt);
          } else if constexpr (std::is_same_v<N, Stmt::Break>) {
            if (loop_depth_ <= 0) {
              error("NBL-T119", "`break` is only valid inside a loop", s.span);
            }
            out.node = TStmt::Break{};
          } else if constexpr (std::is_same_v<N, Stmt::Continue>) {
            if (loop_depth_ <= 0) {
              error("NBL-T120", "`continue` is only valid inside a loop", s.span);
            }
            out.node = TStmt::Continue{};
          }
        },
        s.node);

    return out;
  }

  TExprPtr make_bool_lit_expr(bool value, Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = Ty::Bool();
    out->node = TExpr::BoolLit{value};
    return out;
  }

  TExprPtr make_int_lit_texpr(std::int64_t value, Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = Ty::Int();
    out->node = TExpr::IntLit{value};
    return out;
  }

  TExprPtr make_binary_expr(TExprPtr lhs,
                            Expr::BinOp op,
                            TExprPtr rhs,
                            Ty ty,
                            Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = std::move(ty);
    TExpr::Binary binary;
    binary.op = op;
    binary.lhs = std::move(lhs);
    binary.rhs = std::move(rhs);
    out->node = std::move(binary);
    return out;
  }

  TExprPtr make_construct_expr(const Ty& ty,
                               std::string variant_name,
                               std::uint32_t variant_index,
                               std::vector<TExprPtr> args,
                               Span span) {
    auto out = std::make_unique<TExpr>();
    out->span = span;
    out->ty = ty;
    out->node = TExpr::Construct{
        ty.name, ty.qualified_name, std::move(variant_name), variant_index, std::move(args)};
    return out;
  }

  TStmt make_let_stmt(std::string name,
                     Ty ty,
                     TExprPtr value,
                     Span span,
                     BindingId binding_id = kInvalidBindingId) {
    TStmt stmt;
    stmt.span = span;
    TStmt::Let let_stmt;
    let_stmt.name = std::move(name);
    let_stmt.ty = std::move(ty);
    let_stmt.value = std::move(value);
    let_stmt.binding_id = binding_id;
    stmt.node = std::move(let_stmt);
    return stmt;
  }

  TStmt make_declare_stmt(std::string name,
                         Ty ty,
                         Span span,
                         BindingId binding_id = kInvalidBindingId) {
    TStmt stmt;
    stmt.span = span;
    TStmt::Declare declare;
    declare.name = std::move(name);
    declare.ty = std::move(ty);
    declare.binding_id = binding_id;
    stmt.node = std::move(declare);
    return stmt;
  }

  TStmt make_assign_var_stmt(std::string name,
                             Ty ty,
                             TExprPtr value,
                             Span span,
                             BindingId binding_id = kInvalidBindingId) {
    TStmt stmt;
    stmt.span = span;
    TStmt::AssignVar assign;
    assign.name = std::move(name);
    assign.ty = std::move(ty);
    assign.value = std::move(value);
    assign.binding_id = binding_id;
    stmt.node = std::move(assign);
    return stmt;
  }

  TStmt make_return_stmt(TExprPtr value, Span span) {
    TStmt stmt;
    stmt.span = span;
    stmt.node = TStmt::Return{std::move(value)};
    return stmt;
  }

  TStmt make_if_stmt(TExprPtr cond,
                     TBlock then_body,
                     std::optional<TBlock> else_body,
                     Span span) {
    TStmt stmt;
    stmt.span = span;
    TStmt::If if_stmt;
    if_stmt.cond = std::move(cond);
    if_stmt.then_body = std::move(then_body);
    if_stmt.else_body = std::move(else_body);
    stmt.node = std::move(if_stmt);
    return stmt;
  }

  TStmt make_break_stmt(Span span) {
    TStmt stmt;
    stmt.span = span;
    stmt.node = TStmt::Break{};
    return stmt;
  }

  void append_stmt_vector(std::vector<TStmt>& dst, std::vector<TStmt>&& src) {
    for (auto& stmt : src) dst.push_back(std::move(stmt));
  }

  struct LoweredExpr {
    std::vector<TStmt> prefix;
    TExprPtr value;
  };

  LoweredExpr lower_expr_effects(TExprPtr expr) {
    LoweredExpr result;
    const Span span = expr->span;
    const Ty ty = expr->ty;

    std::visit(
        [&](auto& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, TExpr::IntLit> || std::is_same_v<N, TExpr::FloatLit> ||
                        std::is_same_v<N, TExpr::BoolLit> || std::is_same_v<N, TExpr::StringLit> ||
                        std::is_same_v<N, TExpr::VarRef> || std::is_same_v<N, TExpr::FieldRef>) {
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::TempFieldRef>) {
            auto base = lower_expr_effects(std::move(n.base));
            append_stmt_vector(result.prefix, std::move(base.prefix));
            n.base = std::move(base.value);
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::EnumIsVariant> ||
                               std::is_same_v<N, TExpr::EnumPayload>) {
            auto subject = lower_expr_effects(std::move(n.subject));
            append_stmt_vector(result.prefix, std::move(subject.prefix));
            n.subject = std::move(subject.value);
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Call>) {
            for (auto& arg : n.args) {
              auto lowered = lower_expr_effects(std::move(arg));
              append_stmt_vector(result.prefix, std::move(lowered.prefix));
              arg = std::move(lowered.value);
            }
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Construct>) {
            for (auto& arg : n.args) {
              auto lowered = lower_expr_effects(std::move(arg));
              append_stmt_vector(result.prefix, std::move(lowered.prefix));
              arg = std::move(lowered.value);
            }
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Unary>) {
            auto inner = lower_expr_effects(std::move(n.inner));
            append_stmt_vector(result.prefix, std::move(inner.prefix));
            n.inner = std::move(inner.value);
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Prefix>) {
            auto inner = lower_expr_effects(std::move(n.inner));
            append_stmt_vector(result.prefix, std::move(inner.prefix));
            n.inner = std::move(inner.value);
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Binary>) {
            auto lhs = lower_expr_effects(std::move(n.lhs));
            auto rhs = lower_expr_effects(std::move(n.rhs));

            if ((n.op == Expr::BinOp::And || n.op == Expr::BinOp::Or) && !rhs.prefix.empty()) {
              append_stmt_vector(result.prefix, std::move(lhs.prefix));
              const Ty lhs_ty = lhs.value ? lhs.value->ty : Ty::Unknown();
              const std::string temp_name = fresh_synthetic_name("__nebula_short_");
              const BindingId temp_binding_id = fresh_binding_id();
              result.prefix.push_back(
                  make_let_stmt(temp_name, lhs_ty, std::move(lhs.value), span, temp_binding_id));

              TBlock then_body;
              then_body.span = span;
              append_stmt_vector(then_body.stmts, std::move(rhs.prefix));
              then_body.stmts.push_back(make_assign_var_stmt(
                  temp_name, ty, std::move(rhs.value), span, temp_binding_id));

              TExprPtr cond = make_var_ref_expr(temp_name, lhs_ty, span, temp_binding_id);
              if (n.op == Expr::BinOp::Or) {
                cond = make_not_expr(std::move(cond), span);
              }
              result.prefix.push_back(make_if_stmt(std::move(cond), std::move(then_body),
                                                   std::nullopt, span));
              result.value = make_var_ref_expr(temp_name, ty, span, temp_binding_id);
              return;
            }

            append_stmt_vector(result.prefix, std::move(lhs.prefix));
            append_stmt_vector(result.prefix, std::move(rhs.prefix));
            n.lhs = std::move(lhs.value);
            n.rhs = std::move(rhs.value);
            result.value = std::move(expr);
          } else if constexpr (std::is_same_v<N, TExpr::Match>) {
            auto subject = lower_expr_effects(std::move(n.subject));
            append_stmt_vector(result.prefix, std::move(subject.prefix));
            n.subject = std::move(subject.value);

            std::vector<std::vector<TStmt>> arm_prefixes;
            arm_prefixes.reserve(n.arms.size());
            bool any_arm_prefix = false;
            for (auto& arm : n.arms) {
              auto lowered_value = lower_expr_effects(std::move(arm->value));
              any_arm_prefix = any_arm_prefix || !lowered_value.prefix.empty();
              arm_prefixes.push_back(std::move(lowered_value.prefix));
              arm->value = std::move(lowered_value.value);
            }
            if (!any_arm_prefix) {
              result.value = std::move(expr);
              return;
            }

            const Ty subject_ty = n.subject->ty;
            const std::string subject_name = fresh_synthetic_name("__nebula_match_subject_");
            const BindingId subject_binding_id = fresh_binding_id();
            result.prefix.push_back(
                make_let_stmt(subject_name, subject_ty, std::move(n.subject), span, subject_binding_id));

            const std::string result_name = fresh_synthetic_name("__nebula_match_result_");
            const BindingId result_binding_id = fresh_binding_id();
            result.prefix.push_back(make_declare_stmt(result_name, ty, span, result_binding_id));

            auto materialize_arm_body = [&](TExpr::Match::Arm& arm,
                                           std::vector<TStmt> prefix,
                                           TExprPtr value) -> TBlock {
              TBlock body;
              body.span = arm.span;
              if (arm.kind == TExpr::Match::Arm::Kind::EnumVariant) {
                if (arm.payload_binding.has_value()) {
                  body.stmts.push_back(make_let_stmt(
                      arm.payload_binding->name,
                      arm.payload_binding->ty,
                      make_enum_payload_expr(
                          make_var_ref_expr(subject_name, subject_ty, arm.span, subject_binding_id),
                          subject_ty,
                          arm.payload_ty,
                          arm.variant_name,
                          arm.variant_index,
                          arm.payload_binding->span),
                      arm.payload_binding->span,
                      arm.payload_binding->binding_id));
                } else if (!arm.payload_struct_bindings.empty()) {
                  const std::string payload_temp_name = fresh_synthetic_name("__nebula_payload_");
                  const BindingId payload_temp_binding_id = fresh_binding_id();
                  body.stmts.push_back(make_let_stmt(
                      payload_temp_name,
                      arm.payload_ty,
                      make_enum_payload_expr(
                                             make_var_ref_expr(subject_name, subject_ty, arm.span,
                                                               subject_binding_id),
                                             subject_ty,
                                             arm.payload_ty,
                                             arm.variant_name,
                                             arm.variant_index,
                                             arm.span),
                      arm.span,
                      payload_temp_binding_id));
                  for (const auto& plan : arm.payload_struct_bindings) {
                    body.stmts.push_back(make_let_stmt(
                        plan.binding.name,
                        plan.binding.ty,
                        make_rooted_field_ref_expr(payload_temp_name,
                                                   plan.field_name,
                                                   plan.binding.ty,
                                                   plan.field_span,
                                                   payload_temp_binding_id),
                        plan.binding.span,
                        plan.binding.binding_id));
                  }
                }
              }
              append_stmt_vector(body.stmts, std::move(prefix));
              body.stmts.push_back(
                  make_assign_var_stmt(result_name, ty, std::move(value), arm.span, result_binding_id));
              return body;
            };

            auto build_if_chain = [&](auto&& self, std::size_t index) -> TStmt {
              TExpr::Match::Arm& arm = *n.arms[index];
              TStmt out_stmt;
              out_stmt.span = arm.span;
              TStmt::If if_stmt;
              if (arm.kind == TExpr::Match::Arm::Kind::Bool) {
                auto subject_ref = make_var_ref_expr(subject_name, subject_ty, arm.span);
                if_stmt.cond = arm.bool_value ? std::move(subject_ref)
                                              : make_not_expr(std::move(subject_ref), arm.span);
              } else {
                if_stmt.cond = make_enum_is_variant_expr(
                    make_var_ref_expr(subject_name, subject_ty, arm.span),
                    subject_ty,
                    arm.variant_name,
                    arm.variant_index,
                    arm.span);
              }
              if_stmt.then_body = materialize_arm_body(
                  arm, std::move(arm_prefixes[index]), std::move(arm.value));

              const bool unconditional =
                  arm.kind == TExpr::Match::Arm::Kind::Wildcard ||
                  (n.exhaustive && index + 1 == n.arms.size());
              if (!unconditional && index + 1 < n.arms.size()) {
                TExpr::Match::Arm& next_arm = *n.arms[index + 1];
                const bool next_unconditional =
                    next_arm.kind == TExpr::Match::Arm::Kind::Wildcard ||
                    (n.exhaustive && index + 2 == n.arms.size());
                if (next_unconditional) {
                  if_stmt.else_body = materialize_arm_body(
                      next_arm, std::move(arm_prefixes[index + 1]), std::move(next_arm.value));
                } else {
                  TBlock else_block;
                  else_block.span = next_arm.span;
                  else_block.stmts.push_back(self(self, index + 1));
                  if_stmt.else_body = std::move(else_block);
                }
              }

              out_stmt.node = std::move(if_stmt);
              return out_stmt;
            };

            if (n.arms.front()->kind == TExpr::Match::Arm::Kind::Wildcard) {
              TBlock body = materialize_arm_body(
                  *n.arms.front(), std::move(arm_prefixes.front()), std::move(n.arms.front()->value));
              append_stmt_vector(result.prefix, std::move(body.stmts));
            } else {
              result.prefix.push_back(build_if_chain(build_if_chain, 0));
            }

            result.value = make_var_ref_expr(result_name, ty, span, result_binding_id);
          } else if constexpr (std::is_same_v<N, TExpr::Try>) {
            auto inner = lower_expr_effects(std::move(n.inner));
            append_stmt_vector(result.prefix, std::move(inner.prefix));

            const auto operand_result = decompose_result_ty(inner.value->ty);
            const auto fn_result = decompose_result_ty(current_ret_);
            if (!operand_result.has_value() || !fn_result.has_value()) {
              result.value = std::move(inner.value);
              return;
            }

            const std::string temp_name = fresh_synthetic_name("__nebula_try_");
            const BindingId temp_binding_id = fresh_binding_id();
            result.prefix.push_back(
                make_let_stmt(temp_name, inner.value->ty, std::move(inner.value), span, temp_binding_id));

            TBlock then_body;
            then_body.span = span;
            std::vector<TExprPtr> err_args;
            err_args.push_back(make_enum_payload_expr(
                make_var_ref_expr(temp_name, operand_result->full_ty, span, temp_binding_id),
                operand_result->full_ty,
                operand_result->err_ty,
                "Err",
                operand_result->err_variant_index,
                span));
            then_body.stmts.push_back(make_return_stmt(
                make_construct_expr(fn_result->full_ty,
                                    "Err",
                                    fn_result->err_variant_index,
                                    std::move(err_args),
                                    span),
                span));
            result.prefix.push_back(make_if_stmt(
                make_enum_is_variant_expr(make_var_ref_expr(temp_name, operand_result->full_ty, span,
                                                            temp_binding_id),
                                          operand_result->full_ty,
                                          "Err",
                                          operand_result->err_variant_index,
                                          span),
                std::move(then_body),
                std::nullopt,
                span));

            result.value = make_enum_payload_expr(
                                                  make_var_ref_expr(temp_name, operand_result->full_ty, span,
                                                                    temp_binding_id),
                                                  operand_result->full_ty,
                                                  operand_result->ok_ty,
                                                  "Ok",
                                                  operand_result->ok_variant_index,
                                                  span);
          } else if constexpr (std::is_same_v<N, TExpr::Await>) {
            auto inner = lower_expr_effects(std::move(n.inner));
            append_stmt_vector(result.prefix, std::move(inner.prefix));
            auto awaited = std::make_unique<TExpr>();
            awaited->span = span;
            awaited->ty = expr->ty;
            awaited->node = TExpr::Await{std::move(inner.value)};
            result.value = std::move(awaited);
          }
        },
        expr->node);

    return result;
  }

  void append_lowered_stmt(std::vector<TStmt>& out, TStmt stmt) {
    std::visit(
        [&](auto& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, TStmt::Declare>) {
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::Let>) {
            auto lowered = lower_expr_effects(std::move(n.value));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.value = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::Return>) {
            auto lowered = lower_expr_effects(std::move(n.value));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.value = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::ExprStmt>) {
            auto lowered = lower_expr_effects(std::move(n.expr));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.expr = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::AssignVar>) {
            auto lowered = lower_expr_effects(std::move(n.value));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.value = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::AssignField>) {
            auto lowered = lower_expr_effects(std::move(n.value));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.value = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::Region>) {
            n.body = lower_effectful_block(std::move(n.body));
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::Unsafe>) {
            n.body = lower_effectful_block(std::move(n.body));
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::If>) {
            n.then_body = lower_effectful_block(std::move(n.then_body));
            if (n.else_body.has_value()) {
              n.else_body = lower_effectful_block(std::move(*n.else_body));
            }
            auto lowered = lower_expr_effects(std::move(n.cond));
            append_stmt_vector(out, std::move(lowered.prefix));
            n.cond = std::move(lowered.value);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::While>) {
            n.body = lower_effectful_block(std::move(n.body));
            auto lowered = lower_expr_effects(std::move(n.cond));
            if (lowered.prefix.empty()) {
              n.cond = std::move(lowered.value);
              out.push_back(std::move(stmt));
              return;
            }

            TBlock loop_body;
            loop_body.span = stmt.span;
            append_stmt_vector(loop_body.stmts, std::move(lowered.prefix));
            TBlock break_body;
            break_body.span = stmt.span;
            break_body.stmts.push_back(make_break_stmt(stmt.span));
            loop_body.stmts.push_back(
                make_if_stmt(make_not_expr(std::move(lowered.value), stmt.span),
                             std::move(break_body),
                             std::nullopt,
                             stmt.span));
            append_stmt_vector(loop_body.stmts, std::move(n.body.stmts));

            TStmt::While lowered_while;
            lowered_while.cond = make_bool_lit_expr(true, stmt.span);
            lowered_while.body = std::move(loop_body);
            stmt.node = std::move(lowered_while);
            out.push_back(std::move(stmt));
          } else if constexpr (std::is_same_v<N, TStmt::For>) {
            n.body = lower_effectful_block(std::move(n.body));
            auto lowered_start = lower_expr_effects(std::move(n.start));
            auto lowered_end = lower_expr_effects(std::move(n.end));

            if (lowered_end.prefix.empty()) {
              append_stmt_vector(out, std::move(lowered_start.prefix));
              append_stmt_vector(out, std::move(lowered_end.prefix));
              n.start = std::move(lowered_start.value);
              n.end = std::move(lowered_end.value);
              out.push_back(std::move(stmt));
              return;
            }

            append_stmt_vector(out, std::move(lowered_start.prefix));
            const std::string index_name = fresh_synthetic_name("__nebula_for_idx_");
            const BindingId index_binding_id = fresh_binding_id();
            out.push_back(
                make_let_stmt(index_name, Ty::Int(), std::move(lowered_start.value), stmt.span, index_binding_id));

            TBlock loop_body;
            loop_body.span = stmt.span;
            append_stmt_vector(loop_body.stmts, std::move(lowered_end.prefix));

            TBlock break_body;
            break_body.span = stmt.span;
            break_body.stmts.push_back(make_break_stmt(stmt.span));
            loop_body.stmts.push_back(make_if_stmt(
                make_not_expr(
                    make_binary_expr(make_var_ref_expr(index_name, Ty::Int(), stmt.span, index_binding_id),
                                     Expr::BinOp::Lt,
                                     std::move(lowered_end.value),
                                     Ty::Bool(),
                                     stmt.span),
                    stmt.span),
                std::move(break_body),
                std::nullopt,
                stmt.span));
            loop_body.stmts.push_back(
                make_let_stmt(n.var,
                              n.var_ty,
                              make_var_ref_expr(index_name, Ty::Int(), stmt.span, index_binding_id),
                              stmt.span,
                              n.binding_id));
            append_stmt_vector(loop_body.stmts, std::move(n.body.stmts));
            loop_body.stmts.push_back(make_assign_var_stmt(
                index_name,
                Ty::Int(),
                make_binary_expr(make_var_ref_expr(index_name, Ty::Int(), stmt.span, index_binding_id),
                                 Expr::BinOp::Add,
                                 make_int_lit_texpr(1, stmt.span),
                                 Ty::Int(),
                                 stmt.span),
                stmt.span,
                index_binding_id));

            TStmt loop_stmt;
            loop_stmt.span = stmt.span;
            TStmt::While lowered_while;
            lowered_while.cond = make_bool_lit_expr(true, stmt.span);
            lowered_while.body = std::move(loop_body);
            loop_stmt.node = std::move(lowered_while);
            out.push_back(std::move(loop_stmt));
          } else {
            out.push_back(std::move(stmt));
          }
        },
        stmt.node);
  }

  TBlock lower_effectful_block(TBlock block) {
    TBlock out;
    out.span = block.span;
    out.stmts.reserve(block.stmts.size());
    for (auto& stmt : block.stmts) {
      append_lowered_stmt(out.stmts, std::move(stmt));
    }
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
            auto binding = lookup_binding(n.ident);
            if (binding.has_value()) {
              out->ty = binding->ty;
              out->node = TExpr::VarRef{n.ident, std::nullopt, binding->binding_id};
              check_borrow_access_conflict(alias_for_whole_name(n.ident), e.span,
                                           BorrowAccessKind::Read);
              return;
            }

            const auto fn_matches = visible_function_candidates(n.ident);
            if (!fn_matches.empty()) {
              if (fn_matches.size() > 1) {
                resolve_visible_function(n.ident, e.span);
                out->ty = Ty::Unknown();
                out->node = TExpr::VarRef{n.ident, std::nullopt, kInvalidBindingId};
                return;
              }
              const auto* fn_info = lookup_function_info(fn_matches.front());
              if (fn_info != nullptr) {
                out->ty = callable_ty_from_sig(*fn_info);
                out->node = TExpr::VarRef{n.ident, fn_matches.front(), kInvalidBindingId};
                return;
              }
            }

            auto builtin_it = builtins_.find(n.ident);
            if (builtin_it != builtins_.end()) {
              out->ty = callable_ty_from_sig(builtin_it->second);
              out->node = TExpr::VarRef{n.ident, std::nullopt, kInvalidBindingId};
              return;
            }

            const auto variant_matches = visible_variant_candidates(n.ident);
            if (std::any_of(variant_matches.begin(),
                            variant_matches.end(),
                            [&](const VariantCandidate& candidate) {
                              return variant_payload_is_zero_arg(candidate.variant.payload);
                            })) {
              error("NBL-T099",
                    "cannot infer enum type argument for variant: " + n.ident,
                    e.span);
              out->ty = Ty::Unknown();
              out->node = TExpr::VarRef{n.ident, std::nullopt, kInvalidBindingId};
              return;
            }

            error("NBL-T050", "unknown name: " + n.ident, e.span);
            out->ty = Ty::Unknown();
            out->node = TExpr::VarRef{n.ident, std::nullopt, kInvalidBindingId};
          } else if constexpr (std::is_same_v<N, Expr::Field>) {
            auto base = typecheck_expr(*n.base);
            const Ty base_ty = base->ty;
            const auto rooted = extract_rooted_field_access(*base);
            const std::string access_label =
                rooted.has_value() ? append_field_path(rooted->base, rooted->field)
                                   : describe_expr_brief(*n.base);
            if (auto final_ty = resolve_field_path(base_ty, access_label, n.field, e.span);
                final_ty.has_value()) {
              out->ty = *final_ty;
            } else {
              out->ty = Ty::Unknown();
            }

            if (rooted.has_value()) {
              const std::string path = append_field_path(rooted->field, n.field);
              BindingId base_binding_id = kInvalidBindingId;
              if (auto binding = lookup_binding(rooted->base); binding.has_value()) {
                base_binding_id = binding->binding_id;
              }
              out->node = TExpr::FieldRef{rooted->base, path, base_binding_id};
              check_borrow_access_conflict(alias_for_field_name(rooted->base, path), e.span,
                                           BorrowAccessKind::Read);
            } else {
              out->node = TExpr::TempFieldRef{std::move(base), n.field};
            }
          } else if constexpr (std::is_same_v<N, Expr::MethodCall>) {
            auto self = typecheck_expr_for_ref_binding(*n.base);
            Ty base_ty = self->ty;
            const std::string base_label = describe_expr_brief(*n.base);
            if (base_ty.kind != Ty::Kind::Struct && base_ty.kind != Ty::Kind::Unknown) {
              error("NBL-T081", "method call on non-struct value: " + base_label, e.span);
            }

            std::string mapped_callee = (base_ty.kind == Ty::Kind::Struct ? base_ty.name : base_label) +
                                        "_" + n.method;
            std::vector<TExprPtr> args;
            args.reserve(n.args.size() + 1);
            args.push_back(std::move(self));

            out->ty = Ty::Unknown();
            TExpr::Call mapped_call;
            mapped_call.callee = mapped_callee;
            mapped_call.kind = TExpr::CallKind::Direct;
            out->node = std::move(mapped_call);

            if (base_ty.kind != Ty::Kind::Struct) {
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
              const QualifiedName exact =
                  qualify(*base_ty.qualified_name, base_ty.name + "_" + n.method);
              mapped_fn_info = lookup_function_info(exact);
              if (mapped_fn_info != nullptr) mapped_symbol = exact;
            }
            if (mapped_fn_info == nullptr) {
              const auto fn_matches = visible_function_candidates(mapped_callee);
              if (!fn_matches.empty()) {
                if (fn_matches.size() > 1) {
                  resolve_visible_function(mapped_callee, e.span);
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
              if (!is_ref_lvalue(*args[0])) {
                error("NBL-T063", "ref parameter requires an lvalue argument", args[0]->span);
              } else {
                register_ref_borrow(extract_alias_key(*args[0]), e.span, &local_ref_aliases,
                                    &pending_persistent_borrows, persist_ref_borrows);
              }
            } else {
              check_borrow_access_conflict(extract_alias_key(*args[0]), e.span,
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
              const std::size_t sig_i = i + 1;
              const bool wants_ref = (sig_i < sig.params_ref.size()) && sig.params_ref[sig_i];
              auto a = wants_ref
                           ? typecheck_expr_for_ref_binding(*n.args[i])
                           : typecheck_expr_with_expected(*n.args[i],
                                                          sig_i < sig.params.size()
                                                              ? &sig.params[sig_i]
                                                              : nullptr);
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
            if (auto callee_binding = lookup_binding(n.callee); callee_binding.has_value()) {
              const Ty& var_ty = callee_binding->ty;
              if (var_ty.kind != Ty::Kind::Callable) {
                std::vector<TExprPtr> args;
                args.reserve(n.args.size());
                for (const auto& a : n.args) args.push_back(typecheck_expr(*a));
                if (var_ty.kind != Ty::Kind::Unknown) {
                  error("NBL-T065", "callee is not callable: " + n.callee, e.span);
                }
                out->ty = Ty::Unknown();
                TExpr::Call indirect_call;
                indirect_call.callee = n.callee;
                indirect_call.args = std::move(args);
                indirect_call.kind = TExpr::CallKind::Indirect;
                indirect_call.callee_binding_id = callee_binding->binding_id;
                out->node = std::move(indirect_call);
                return;
              }

              enforce_unsafe_call_context(n.callee, var_ty.is_unsafe_callable, e.span, Span{});
              const bool persist_ref_borrows = true;

              if (n.args.size() != var_ty.callable_params.size()) {
                error("NBL-T066",
                      "callable arity mismatch for " + n.callee + ": expected " +
                          std::to_string(var_ty.callable_params.size()) + ", got " +
                          std::to_string(n.args.size()),
                      e.span);
              }

              std::vector<TExprPtr> args;
              args.reserve(n.args.size());
              std::vector<AliasKey> local_ref_aliases;
              std::vector<BorrowEntry> pending_persistent_borrows;
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                const bool wants_ref =
                    (i < var_ty.callable_params_ref.size()) && var_ty.callable_params_ref[i];
                auto a = wants_ref ? typecheck_expr_for_ref_binding(*n.args[i])
                                   : typecheck_expr_with_expected(
                                         *n.args[i],
                                         i < var_ty.callable_params.size()
                                             ? &var_ty.callable_params[i]
                                             : nullptr);
                if (i < var_ty.callable_params.size()) {
                  if (wants_ref) {
                    if (!is_ref_lvalue(*a)) {
                      error("NBL-T063", "ref parameter requires an lvalue argument", a->span);
                    } else {
                      register_ref_borrow(extract_alias_key(*a), a->span, &local_ref_aliases,
                                          &pending_persistent_borrows, persist_ref_borrows);
                    }
                  }
                  if (!ty_equal(a->ty, var_ty.callable_params[i]) &&
                      a->ty.kind != Ty::Kind::Unknown &&
                      var_ty.callable_params[i].kind != Ty::Kind::Unknown) {
                    error("NBL-T067",
                          "callable argument type mismatch for " + n.callee + ": expected " +
                              ty_to_string(var_ty.callable_params[i]) + ", got " +
                              ty_to_string(a->ty),
                          a->span);
                  }
                }
                args.push_back(std::move(a));
              }
              check_ref_alias_conflicts(n.callee, var_ty.callable_params_ref, args, e.span);
              if (persist_ref_borrows) {
                commit_pending_persistent_ref_borrows(pending_persistent_borrows);
              }

              out->ty = var_ty.callable_ret ? *var_ty.callable_ret : Ty::Unknown();
              TExpr::Call indirect_call;
              indirect_call.callee = n.callee;
              indirect_call.args = std::move(args);
              indirect_call.args_ref = var_ty.callable_params_ref;
              indirect_call.kind = TExpr::CallKind::Indirect;
              indirect_call.callee_binding_id = callee_binding->binding_id;
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
            const bool only_zero_payload_variants =
                !variant_matches.empty() &&
                std::all_of(variant_matches.begin(),
                            variant_matches.end(),
                            [&](const VariantCandidate& candidate) {
                              return variant_payload_is_zero_arg(candidate.variant.payload);
                            });

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
            } else if ((struct_matches.size() == 1 ||
                        (!variant_matches.empty() &&
                         !(n.args.empty() && only_zero_payload_variants && fn_matches.size() == 1 &&
                           struct_matches.empty() && !builtin_visible))) &&
                       (!fn_matches.empty() || builtin_visible ||
                        (struct_matches.size() == 1 && !variant_matches.empty()))) {
              std::string message = "ambiguous callee: " + n.callee + " resolves to both a type and a function";
              error("NBL-T098", std::move(message), e.span);
            }

            const bool callee_ambiguous =
                struct_matches.size() > 1 || fn_matches.size() > 1 ||
                ((struct_matches.size() == 1 ||
                  (!variant_matches.empty() &&
                   !(n.args.empty() && only_zero_payload_variants && fn_matches.size() == 1 &&
                     struct_matches.empty() && !builtin_visible))) &&
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

                std::unordered_map<std::string, Ty> inferred;
                const std::unordered_set<std::string> type_param_set(candidate.enum_type_params.begin(),
                                                                    candidate.enum_type_params.end());
                bool payload_ok = true;
                if (!expects_zero) {
                  payload_ok = infer_type_args(candidate.variant.payload, args[0]->ty, type_param_set,
                                               inferred);
                }
                if (!payload_ok) continue;

                if (!all_type_params_inferred(candidate.enum_type_params, inferred)) {
                  continue;
                }

                std::vector<Ty> type_args;
                type_args.reserve(candidate.enum_type_params.size());
                for (const auto& param : candidate.enum_type_params) {
                  auto it = inferred.find(param);
                  type_args.push_back(it == inferred.end() ? Ty::Unknown() : it->second);
                }

                matched_variants.push_back(
                    {candidate,
                     Ty::Enum(candidate.enum_name.local_name, std::move(type_args),
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
              if (is_non_constructible_runtime_backed_struct(struct_matches.front().qualified_name)) {
                error("NBL-T135",
                      "direct construction is not allowed for " +
                          runtime_backed_struct_kind_label(struct_matches.front().qualified_name) +
                          ": " + n.callee,
                      e.span);
                out->ty = Ty::Struct(n.callee, {}, struct_matches.front().qualified_name);
                TExpr::Call direct_call;
                direct_call.callee = n.callee;
                direct_call.kind = TExpr::CallKind::Direct;
                for (const auto& a : n.args) direct_call.args.push_back(typecheck_expr(*a));
                out->node = std::move(direct_call);
                return;
              }
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
              std::unordered_map<std::string, Ty> inferred;
              std::unordered_set<std::string> type_param_set;
              if (st_info != nullptr) {
                type_param_set.insert(st_info->type_params.begin(), st_info->type_params.end());
              }
              args.reserve(n.args.size());
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                auto a = typecheck_expr(*n.args[i]);
                if (i < fields.size()) {
                  (void)infer_type_args(fields[i].ty, a->ty, type_param_set, inferred);
                }
                args.push_back(std::move(a));
              }

              std::vector<Ty> instantiated_type_args;
              if (st_info != nullptr) {
                instantiated_type_args.reserve(st_info->type_params.size());
                for (const auto& param : st_info->type_params) {
                  auto it = inferred.find(param);
                  instantiated_type_args.push_back(it == inferred.end() ? Ty::Unknown() : it->second);
                }
                if (!all_type_params_inferred(st_info->type_params, inferred)) {
                  error("NBL-T122", "cannot infer type arguments for constructor: " + n.callee, e.span);
                }
              }

              for (std::size_t i = 0; i < args.size() && i < fields.size(); ++i) {
                const Ty expected_field_ty = substitute_type_params(fields[i].ty, inferred);
                if (!ty_equal(args[i]->ty, expected_field_ty) &&
                    args[i]->ty.kind != Ty::Kind::Unknown && expected_field_ty.kind != Ty::Kind::Unknown) {
                  error("NBL-T061",
                        "field type mismatch in " + n.callee + ": expected " +
                            ty_to_string(expected_field_ty) + ", got " + ty_to_string(args[i]->ty),
                        args[i]->span);
                }
              }
              out->ty = Ty::Struct(n.callee, std::move(instantiated_type_args),
                                   struct_matches.front().qualified_name);
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
            std::unordered_map<std::string, Ty> inferred;
            if (!sig.type_params.empty()) {
              const std::unordered_set<std::string> type_param_set(sig.type_params.begin(), sig.type_params.end());
              const std::size_t infer_count = std::min(n.args.size(), sig.params.size());
              for (std::size_t i = 0; i < infer_count; ++i) {
                auto probe = typecheck_expr(*n.args[i]);
                (void)infer_type_args(sig.params[i], probe->ty, type_param_set, inferred);
              }
              if (!all_type_params_inferred(sig.type_params, inferred)) {
                error("NBL-T123", "cannot infer type arguments for function: " + n.callee, e.span);
              }
            }
            std::vector<Ty> instantiated_params;
            instantiated_params.reserve(sig.params.size());
            for (const auto& param : sig.params) {
              instantiated_params.push_back(substitute_type_params(param, inferred));
            }
            const Ty instantiated_ret = substitute_type_params(sig.ret, inferred);
            enforce_unsafe_call_context(n.callee, fn_info->is_unsafe_callable, e.span, fn_info->span);
            const bool persist_ref_borrows = false;
            if (n.args.size() != instantiated_params.size()) {
              error("NBL-T062",
                    "call arity mismatch for " + n.callee + ": expected " +
                        std::to_string(instantiated_params.size()) + ", got " +
                        std::to_string(n.args.size()),
                    e.span);
            }
            std::vector<TExprPtr> args;
            args.reserve(n.args.size());
            std::vector<AliasKey> local_ref_aliases;
            std::vector<BorrowEntry> pending_persistent_borrows;
            for (std::size_t i = 0; i < n.args.size(); ++i) {
              const bool wants_ref = (i < sig.params_ref.size()) && sig.params_ref[i];
              auto a = wants_ref ? typecheck_expr_for_ref_binding(*n.args[i])
                                 : typecheck_expr_with_expected(
                                       *n.args[i], i < instantiated_params.size() ? &instantiated_params[i] : nullptr);
              if (i < instantiated_params.size()) {
                if (wants_ref) {
                  if (!is_ref_lvalue(*a)) {
                    error("NBL-T063", "ref parameter requires an lvalue argument", a->span);
                  } else {
                    register_ref_borrow(extract_alias_key(*a), a->span, &local_ref_aliases,
                                        &pending_persistent_borrows, persist_ref_borrows);
                  }
                }
                if (!ty_equal(a->ty, instantiated_params[i]) && a->ty.kind != Ty::Kind::Unknown &&
                    instantiated_params[i].kind != Ty::Kind::Unknown) {
                  error("NBL-T064",
                        "argument type mismatch for " + n.callee + ": expected " +
                            ty_to_string(instantiated_params[i]) + ", got " + ty_to_string(a->ty),
                        a->span);
                }
              }
              args.push_back(std::move(a));
            }
            check_ref_alias_conflicts(n.callee, sig.params_ref, args, e.span);
            if (persist_ref_borrows) {
              commit_pending_persistent_ref_borrows(pending_persistent_borrows);
            }
            out->ty = instantiated_ret;
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
          } else if constexpr (std::is_same_v<N, Expr::Try>) {
            out = typecheck_try_expr(e, n, nullptr);
          } else if constexpr (std::is_same_v<N, Expr::Await>) {
            out = typecheck_await_expr(e, n);
          } else if constexpr (std::is_same_v<N, Expr::Match>) {
            out = typecheck_match_expr(e, n, nullptr);
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
