#include "codegen/freestanding_cpp_emitter.hpp"

#include "boot/protocol_abi_contract.hpp"

#include "codegen/symbol_names.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace nebula::codegen {
namespace {

using nebula::frontend::Diagnostic;
using nebula::frontend::DiagnosticRisk;
using nebula::frontend::DiagnosticStage;
using nebula::frontend::PanicPolicy;
using nebula::frontend::Severity;
using nebula::frontend::Span;
using nebula::frontend::Ty;
using nebula::nir::BinOp;
using nebula::nir::Block;
using nebula::nir::Expr;
using nebula::nir::Function;
using nebula::nir::Program;
using nebula::nir::Stmt;
using nebula::nir::VarId;
using nebula::passes::OwnerKind;
using nebula::passes::RepKind;
using nebula::passes::RepOwnerResult;

constexpr std::string_view kFreestandingTarget = nebula::boot::kUosX86_64TargetTriple;

Diagnostic make_backend_error(std::string code, std::string message, Span span, std::string cause,
                              std::string impact, std::vector<std::string> suggestions = {}) {
  Diagnostic diagnostic;
  diagnostic.severity = Severity::Error;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  diagnostic.span = std::move(span);
  diagnostic.stage = DiagnosticStage::Build;
  diagnostic.risk = DiagnosticRisk::High;
  diagnostic.category = "backend";
  diagnostic.cause = std::move(cause);
  diagnostic.impact = std::move(impact);
  diagnostic.suggestions = std::move(suggestions);
  return diagnostic;
}

std::string type_name(const Ty &type) {
  switch (type.kind) {
  case Ty::Kind::Int:
    return "Int";
  case Ty::Kind::Float:
    return "Float";
  case Ty::Kind::Bool:
    return "Bool";
  case Ty::Kind::String:
    return "String";
  case Ty::Kind::Void:
    return "Void";
  case Ty::Kind::Struct:
    return "struct " + type.name;
  case Ty::Kind::Enum:
    return "enum " + type.name;
  case Ty::Kind::TypeParam:
    return "type parameter " + type.name;
  case Ty::Kind::Callable:
    return "callable";
  case Ty::Kind::Unknown:
    return "unknown";
  }
  return "unknown";
}

bool is_scalar_value_type(const Ty &type) {
  return type.kind == Ty::Kind::Int || type.kind == Ty::Kind::Bool;
}

bool is_supported_return_type(const Ty &type) {
  return is_scalar_value_type(type) || type.kind == Ty::Kind::Void;
}

std::size_t annotation_count(const std::vector<std::string> &annotations,
                             std::string_view expected) {
  return static_cast<std::size_t>(std::count(annotations.begin(), annotations.end(), expected));
}

struct ValidatedProgram {
  const Function *entry = nullptr;
  std::vector<const Function *> reachable;
  std::set<BinOp> arithmetic_operations;
};

class Validator {
public:
  Validator(const Program &program, const RepOwnerResult &rep_owner,
            const FreestandingEmitOptions &options)
      : program_(program), rep_owner_(rep_owner), options_(options) {}

  std::optional<ValidatedProgram> run() {
    if (options_.target != kFreestandingTarget) {
      fail("NBL-BE-FS-TARGET", "freestanding object emission requires target x86_64-unknown-none",
           {}, "the primitive object emitter has one exact target contract",
           "accepting another or approximate triple would mislabel the generated machine object",
           {"pass --target x86_64-unknown-none"});
      return std::nullopt;
    }
    if (options_.panic_policy != PanicPolicy::Trap) {
      fail("NBL-BE-FS-PANIC", "freestanding object emission requires panic policy trap", {},
           "abort and unwind require runtime behavior outside the current freestanding subset",
           "the object could acquire an undeclared hosted or unwinding dependency",
           {"pass --panic trap"});
      return std::nullopt;
    }

    if (!index_functions())
      return std::nullopt;
    const Function *entry = select_entry();
    if (entry == nullptr)
      return std::nullopt;

    ValidatedProgram result;
    result.entry = entry;
    queue_.push_back(entry);
    queued_.insert(nebula::nir::function_identity(*entry));

    for (std::size_t index = 0; index < queue_.size() && diagnostics_.empty(); ++index) {
      const Function &function = *queue_[index];
      if (!validate_function(function, &function == entry)) {
        return std::nullopt;
      }
      result.reachable.push_back(&function);
    }
    if (!diagnostics_.empty())
      return std::nullopt;

    std::sort(result.reachable.begin(), result.reachable.end(),
              [](const Function *lhs, const Function *rhs) {
                return nebula::nir::function_identity(*lhs) < nebula::nir::function_identity(*rhs);
              });
    result.arithmetic_operations = arithmetic_operations_;
    return result;
  }

  std::vector<Diagnostic> take_diagnostics() { return std::move(diagnostics_); }

private:
  bool index_functions() {
    std::unordered_map<std::string, std::string> emitted_symbols;
    for (const auto &item : program_.items) {
      if (!std::holds_alternative<Function>(item.node))
        continue;
      const auto &function = std::get<Function>(item.node);
      const std::string identity = nebula::nir::function_identity(function);
      if (identity.empty()) {
        fail("NBL-BE-FS-INTERNAL",
             "freestanding code generation found a function without semantic identity",
             function.span, "typed NIR did not preserve a callable identity",
             "direct-call reachability cannot be proven safely");
        return false;
      }
      if (!functions_.emplace(identity, &function).second) {
        fail("NBL-BE-FS-SYMBOL", "duplicate function identity in freestanding input: " + identity,
             function.span, "two NIR functions resolve to the same fully qualified identity",
             "the generated object would contain ambiguous call targets");
        return false;
      }
      const std::string emitted_symbol = emitted_cpp_function_name(function);
      const auto [symbol_it, inserted] = emitted_symbols.emplace(emitted_symbol, identity);
      if (!inserted && symbol_it->second != identity) {
        fail("NBL-BE-FS-SYMBOL", "freestanding function symbol collision: " + emitted_symbol,
             function.span, "two semantic function identities map to one bootstrap C++ symbol",
             "emitting both functions would create an ambiguous or incorrect call target");
        return false;
      }
    }
    return true;
  }

  bool is_root_function(const Function &function) const {
    if (!options_.root_package.has_value())
      return true;
    return function.qualified_name.package_name == *options_.root_package;
  }

  const Function *select_entry() {
    const Function *selected = nullptr;
    for (const auto &[identity, function] : functions_) {
      (void)identity;
      if (!is_root_function(*function))
        continue;
      const std::size_t count = annotation_count(function->annotations, "entry");
      if (count == 0)
        continue;
      if (count != 1 || selected != nullptr) {
        fail("NBL-BE-FS-ENTRY-DUPLICATE",
             "freestanding root package must define exactly one @entry function", function->span,
             "multiple entry annotations or entry functions were found",
             "the object cannot choose one deterministic payload entry target",
             {"keep one @entry function in the root package"});
        return nullptr;
      }
      selected = function;
    }
    if (selected == nullptr) {
      fail("NBL-BE-FS-ENTRY-MISSING", "freestanding root package has no @entry function", {},
           "the object emitter requires an explicit source-level boot entry",
           "no protocol-owned payload entry symbol can be generated",
           {"add @entry to one fn kernel_entry() -> Void function"});
    }
    return selected;
  }

  bool validate_function(const Function &function, bool is_entry) {
    variable_types_.clear();
    seen_variables_.clear();
    if (function.is_extern || function.is_async || !function.type_params.empty() ||
        !function.body.has_value()) {
      fail("NBL-BE-FS-FEATURE",
           "reachable function is outside the primitive freestanding subset: " + function.name,
           function.span,
           "extern, async, generic, or declaration-only functions require an unsupported runtime "
           "or ABI",
           "emitting it could introduce unresolved or hosted dependencies");
      return false;
    }
    for (const auto &annotation : function.annotations) {
      if (is_entry && annotation == "entry")
        continue;
      fail("NBL-BE-FS-FEATURE",
           "unsupported annotation on reachable freestanding function: @" + annotation,
           function.span, "the annotation has no defined meaning in the primitive object contract",
           "silently ignoring it would change source semantics");
      return false;
    }
    if (!is_supported_return_type(function.ret)) {
      return fail_type(function.ret, function.span, "function return");
    }
    if (is_entry && (!function.params.empty() || function.ret.kind != Ty::Kind::Void)) {
      fail("NBL-BE-FS-ENTRY-SIGNATURE", "@entry must have signature fn name() -> Void",
           function.span,
           "the current payload entry contract passes no arguments and consumes no return value",
           "using another signature would invent an undeclared boot ABI");
      return false;
    }
    for (const auto &parameter : function.params) {
      if (parameter.is_ref) {
        fail("NBL-BE-FS-FEATURE",
             "ref parameters are unsupported in reachable freestanding functions", parameter.span,
             "the primitive object gate has no reference aliasing ABI",
             "emitting a reference would create an unproved pointer contract");
        return false;
      }
      if (!is_scalar_value_type(parameter.ty)) {
        return fail_type(parameter.ty, parameter.span, "function parameter");
      }
      if (!register_variable(function, parameter.var, parameter.ty, parameter.span, "parameter")) {
        return false;
      }
    }
    return validate_block(function, *function.body);
  }

  bool register_variable(const Function &function, VarId variable, const Ty &type, const Span &span,
                         std::string_view position) {
    if (variable == 0U || !seen_variables_.insert(variable).second ||
        !variable_types_.emplace(variable, type).second) {
      fail("NBL-BE-FS-INTERNAL",
           "freestanding NIR has an invalid or duplicate " + std::string(position) + " VarId", span,
           "typed NIR variable identities must be nonzero and unique within a function",
           "ambiguous variable identity could miscompile reads or assignments");
      return false;
    }
    if (validate_storage(function, variable, span))
      return true;
    variable_types_.erase(variable);
    return false;
  }

  bool validate_active_variable(VarId variable, const Ty &type, const Span &span,
                                std::string_view position) {
    const auto variable_it = variable_types_.find(variable);
    if (variable == 0U || variable_it == variable_types_.end()) {
      fail("NBL-BE-FS-INTERNAL",
           "freestanding NIR references an undeclared " + std::string(position) + " variable", span,
           "the VarId is not active in the current lexical scope",
           "emitting an undeclared or out-of-scope C++ local would change source semantics");
      return false;
    }
    if (nebula::frontend::ty_equal(variable_it->second, type))
      return true;
    fail("NBL-BE-FS-INTERNAL",
         "freestanding NIR variable type is inconsistent at " + std::string(position), span,
         "the VarId declaration and use carry different types",
         "a C++ implicit conversion could hide a typed-NIR compiler bug");
    return false;
  }

  bool validate_storage(const Function &function, VarId variable, const Span &span) {
    const std::string identity = nebula::nir::function_identity(function);
    const auto function_it = rep_owner_.by_function.find(identity);
    if (function_it == rep_owner_.by_function.end()) {
      fail("NBL-BE-FS-STORAGE", "missing ownership decision for reachable function " + identity,
           span, "the analyzed NIR and ownership result are inconsistent",
           "storage representation cannot be proven freestanding-safe");
      return false;
    }
    const auto variable_it = function_it->second.vars.find(variable);
    if (variable_it == function_it->second.vars.end()) {
      fail("NBL-BE-FS-STORAGE", "missing ownership decision for reachable variable", span,
           "the analyzed NIR and ownership result are inconsistent",
           "storage representation cannot be proven freestanding-safe");
      return false;
    }
    const auto &decision = variable_it->second;
    if (decision.rep != RepKind::Stack || decision.owner != OwnerKind::None ||
        !decision.region.empty()) {
      fail("NBL-BE-FS-STORAGE", "reachable variable requires non-stack storage", span,
           "the primitive object subset permits only Stack + None ownership decisions",
           "region or heap storage would require a runtime and allocator ABI");
      return false;
    }
    return true;
  }

  bool validate_block(const Function &function, const Block &block) {
    std::vector<VarId> block_variables;
    for (const auto &statement : block.stmts) {
      if (!validate_statement(function, statement, block_variables)) {
        for (const VarId variable : block_variables)
          variable_types_.erase(variable);
        return false;
      }
    }
    for (const VarId variable : block_variables)
      variable_types_.erase(variable);
    return true;
  }

  bool validate_statement(const Function &function, const Stmt &statement,
                          std::vector<VarId> &block_variables) {
    if (!statement.annotations.empty()) {
      fail("NBL-BE-FS-FEATURE",
           "statement annotations are unsupported in freestanding object emission", statement.span,
           "the primitive subset defines no statement-level annotation semantics",
           "ignoring an annotation could change control-flow or safety meaning");
      return false;
    }
    return std::visit(
      [&](const auto &node) -> bool {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, Stmt::Declare>) {
          fail("NBL-BE-FS-FEATURE",
               "uninitialized declarations are unsupported in freestanding object emission",
               statement.span, "reading an uninitialized C++ scalar would be undefined behavior",
               "the object emitter refuses to invent a default initialization value",
               {"initialize the variable with let"});
          return false;
        } else if constexpr (std::is_same_v<Node, Stmt::Let>) {
          if (!is_scalar_value_type(node.ty))
            return fail_type(node.ty, statement.span, "local variable");
          if (!validate_value(function, node.value.get(), node.ty, statement.span))
            return false;
          if (!register_variable(function, node.var, node.ty, statement.span, "local"))
            return false;
          block_variables.push_back(node.var);
          return true;
        } else if constexpr (std::is_same_v<Node, Stmt::Return>) {
          return validate_value(function, node.value.get(), function.ret, statement.span);
        } else if constexpr (std::is_same_v<Node, Stmt::ExprStmt>) {
          return validate_expression(function, node.expr.get());
        } else if constexpr (std::is_same_v<Node, Stmt::AssignVar>) {
          if (!is_scalar_value_type(node.ty))
            return fail_type(node.ty, statement.span, "assignment");
          if (!validate_active_variable(node.var, node.ty, statement.span, "assignment"))
            return false;
          return validate_value(function, node.value.get(), node.ty, statement.span);
        } else if constexpr (std::is_same_v<Node, Stmt::If>) {
          if (!validate_value(function, node.cond.get(), Ty::Bool(), statement.span))
            return false;
          if (!validate_block(function, node.then_body))
            return false;
          return !node.else_body.has_value() || validate_block(function, *node.else_body);
        } else if constexpr (std::is_same_v<Node, Stmt::For>) {
          if (node.var_ty.kind != Ty::Kind::Int)
            return fail_type(node.var_ty, statement.span, "for variable");
          arithmetic_operations_.insert(BinOp::Add);
          if (!validate_value(function, node.start.get(), Ty::Int(), statement.span) ||
              !validate_value(function, node.end.get(), Ty::Int(), statement.span)) {
            return false;
          }
          if (!register_variable(function, node.var, node.var_ty, statement.span, "for-loop")) {
            return false;
          }
          const bool body_valid = validate_block(function, node.body);
          variable_types_.erase(node.var);
          return body_valid;
        } else if constexpr (std::is_same_v<Node, Stmt::While>) {
          return validate_value(function, node.cond.get(), Ty::Bool(), statement.span) &&
                 validate_block(function, node.body);
        } else if constexpr (std::is_same_v<Node, Stmt::Break> ||
                             std::is_same_v<Node, Stmt::Continue>) {
          return true;
        } else {
          fail("NBL-BE-FS-FEATURE",
               "reachable statement is unsupported in freestanding object emission", statement.span,
               "field writes, region blocks, and unsafe blocks are outside the primitive allowlist",
               "emitting the statement would require an undefined low-level contract");
          return false;
        }
      },
      statement.node);
  }

  bool validate_value(const Function &function, const Expr *expression, const Ty &expected,
                      const Span &fallback_span) {
    if (expression == nullptr) {
      fail("NBL-BE-FS-INTERNAL", "freestanding NIR contains a missing expression", fallback_span,
           "a required expression pointer is null",
           "code generation cannot preserve source semantics");
      return false;
    }
    if (!nebula::frontend::ty_equal(expression->ty, expected)) {
      fail("NBL-BE-FS-INTERNAL", "freestanding NIR expression type does not match its use",
           expression->span,
           "typed NIR reports " + type_name(expression->ty) + " where " + type_name(expected) +
             " is required",
           "emitting inconsistent typed IR could miscompile the program");
      return false;
    }
    return validate_expression(function, expression);
  }

  bool validate_expression(const Function &function, const Expr *expression) {
    if (expression == nullptr) {
      fail("NBL-BE-FS-INTERNAL", "freestanding NIR contains a missing expression", {},
           "a required expression pointer is null",
           "code generation cannot preserve source semantics");
      return false;
    }
    return std::visit(
      [&](const auto &node) -> bool {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, Expr::IntLit>) {
          return require_expression_type(*expression, Ty::Int());
        } else if constexpr (std::is_same_v<Node, Expr::BoolLit>) {
          return require_expression_type(*expression, Ty::Bool());
        } else if constexpr (std::is_same_v<Node, Expr::VarRef>) {
          if (node.var == 0 || node.top_level_symbol.has_value()) {
            fail("NBL-BE-FS-CALL",
                 "function values are unsupported in freestanding object emission",
                 expression->span,
                 "the variable reference denotes a top-level callable instead of a scalar local",
                 "emitting it would require an indirect-call representation");
            return false;
          }
          if (!is_scalar_value_type(expression->ty)) {
            return fail_type(expression->ty, expression->span, "variable reference");
          }
          return validate_active_variable(node.var, expression->ty, expression->span, "read");
        } else if constexpr (std::is_same_v<Node, Expr::Call>) {
          return validate_call(function, *expression, node);
        } else if constexpr (std::is_same_v<Node, Expr::Binary>) {
          return validate_binary(function, *expression, node);
        } else if constexpr (std::is_same_v<Node, Expr::Unary>) {
          return require_expression_type(*expression, Ty::Bool()) &&
                 validate_value(function, node.inner.get(), Ty::Bool(), expression->span);
        } else {
          fail("NBL-BE-FS-FEATURE",
               "reachable expression is unsupported in freestanding object emission",
               expression->span,
               "only Int/Bool literals, scalar locals, direct internal calls, and primitive "
               "operators are allowed",
               "the expression would require a hosted runtime, aggregate ABI, or unproved "
               "ownership behavior");
          return false;
        }
      },
      expression->node);
  }

  bool validate_call(const Function &function, const Expr &expression, const Expr::Call &call) {
    (void)function;
    if (call.kind != nebula::nir::CallKind::Direct || !call.resolved_callee.has_value() ||
        call.resolved_callee->empty()) {
      fail("NBL-BE-FS-CALL", "freestanding calls must be direct and fully resolved",
           expression.span, "the call target does not carry one exact semantic identity",
           "falling back to a source spelling could call the wrong symbol");
      return false;
    }
    if (std::any_of(call.args_ref.begin(), call.args_ref.end(),
                    [](bool is_ref) { return is_ref; })) {
      fail("NBL-BE-FS-CALL", "ref call arguments are unsupported in freestanding object emission",
           expression.span, "the primitive call ABI passes scalar values only",
           "a reference would require an unproved pointer and aliasing contract");
      return false;
    }

    const std::string identity = call.resolved_callee->display_name();
    const auto target_it = functions_.find(identity);
    if (target_it == functions_.end()) {
      fail("NBL-BE-FS-CALL",
           "reachable call targets a builtin, runtime, or undefined function: " + identity,
           expression.span, "the target is not a Nebula-defined function in analyzed NIR",
           "the generated object would contain an unresolved or hosted dependency");
      return false;
    }
    const Function &target = *target_it->second;
    if (target.is_extern || !target.body.has_value()) {
      fail("NBL-BE-FS-CALL",
           "reachable call targets an external or declaration-only function: " + identity,
           expression.span,
           "the primitive object contract requires every dependency to be defined internally",
           "the generated ELF object would have an undefined symbol");
      return false;
    }
    if (call.args.size() != target.params.size()) {
      fail("NBL-BE-FS-INTERNAL", "resolved call arity is inconsistent in freestanding NIR",
           expression.span, "typed call arguments do not match the resolved function signature",
           "emitting inconsistent IR could corrupt the call ABI");
      return false;
    }
    if (!nebula::frontend::ty_equal(expression.ty, target.ret)) {
      fail("NBL-BE-FS-INTERNAL", "resolved call return type is inconsistent in freestanding NIR",
           expression.span, "typed call result does not match the resolved function signature",
           "emitting inconsistent IR could miscompile the caller");
      return false;
    }
    for (std::size_t index = 0; index < call.args.size(); ++index) {
      if (!validate_value(function, call.args[index].get(), target.params[index].ty,
                          expression.span)) {
        return false;
      }
    }
    if (queued_.insert(identity).second)
      queue_.push_back(&target);
    return true;
  }

  bool validate_binary(const Function &function, const Expr &expression,
                       const Expr::Binary &binary) {
    Ty operand_type = Ty::Int();
    Ty result_type = Ty::Int();
    switch (binary.op) {
    case BinOp::Add:
    case BinOp::Sub:
    case BinOp::Mul:
    case BinOp::Div:
    case BinOp::Mod:
      arithmetic_operations_.insert(binary.op);
      break;
    case BinOp::Lt:
    case BinOp::Lte:
    case BinOp::Gt:
    case BinOp::Gte:
      result_type = Ty::Bool();
      break;
    case BinOp::And:
    case BinOp::Or:
      operand_type = Ty::Bool();
      result_type = Ty::Bool();
      break;
    case BinOp::Eq:
    case BinOp::Ne:
      if (binary.lhs == nullptr || binary.rhs == nullptr ||
          !nebula::frontend::ty_equal(binary.lhs->ty, binary.rhs->ty) ||
          !is_scalar_value_type(binary.lhs->ty)) {
        fail("NBL-BE-FS-INTERNAL", "freestanding equality operands have inconsistent types",
             expression.span, "typed NIR equality is not a same-type Int/Bool comparison",
             "emitting the comparison could change language semantics");
        return false;
      }
      operand_type = binary.lhs->ty;
      result_type = Ty::Bool();
      break;
    }
    return require_expression_type(expression, result_type) &&
           validate_value(function, binary.lhs.get(), operand_type, expression.span) &&
           validate_value(function, binary.rhs.get(), operand_type, expression.span);
  }

  bool require_expression_type(const Expr &expression, const Ty &expected) {
    if (nebula::frontend::ty_equal(expression.ty, expected))
      return true;
    fail("NBL-BE-FS-INTERNAL", "freestanding expression has inconsistent typed NIR",
         expression.span,
         "expression kind requires " + type_name(expected) + " but carries " +
           type_name(expression.ty),
         "emitting inconsistent typed IR could miscompile the program");
    return false;
  }

  bool fail_type(const Ty &type, const Span &span, std::string_view position) {
    fail("NBL-BE-FS-TYPE",
         "unsupported " + std::string(position) +
           " type in freestanding object emission: " + type_name(type),
         span, "the first object gate defines target representation only for Int, Bool, and Void",
         "guessing another representation would create an unstable target ABI");
    return false;
  }

  void fail(std::string code, std::string message, Span span, std::string cause, std::string impact,
            std::vector<std::string> suggestions = {}) {
    if (!diagnostics_.empty())
      return;
    diagnostics_.push_back(make_backend_error(std::move(code), std::move(message), std::move(span),
                                              std::move(cause), std::move(impact),
                                              std::move(suggestions)));
  }

  const Program &program_;
  const RepOwnerResult &rep_owner_;
  const FreestandingEmitOptions &options_;
  std::unordered_map<std::string, const Function *> functions_;
  std::vector<const Function *> queue_;
  std::unordered_set<std::string> queued_;
  std::set<BinOp> arithmetic_operations_;
  std::unordered_map<VarId, Ty> variable_types_;
  std::unordered_set<VarId> seen_variables_;
  std::vector<Diagnostic> diagnostics_;
};

class TextEmitter {
public:
  TextEmitter(const ValidatedProgram &program,
              const std::unordered_map<std::string, const Function *> &functions)
      : program_(program), functions_(functions) {}

  std::optional<std::string> run() {
    line("// nebula-freestanding-profile target=x86_64-unknown-none panic_policy=trap");
    line("using __nebula_i64 = __INT64_TYPE__;");
    line("static_assert(sizeof(__nebula_i64) == 8);");
    line("static_assert(alignof(__nebula_i64) == 8);");
    line("static_assert(sizeof(bool) == 1);");
    blank();
    emit_checked_arithmetic();

    for (const Function *function : program_.reachable)
      emit_function_declaration(*function);
    blank();
    for (const Function *function : program_.reachable) {
      emit_function(*function);
      blank();
    }
    line("extern \"C\" __attribute__((noreturn, used, visibility(\"default\"))) void " +
         std::string(nebula::boot::kUosX86_64PayloadEntrySymbol) + "() {");
    ++indent_;
    line(emitted_cpp_function_name(*program_.entry) + "();");
    line("__builtin_trap();");
    --indent_;
    line("}");
    if (failed_)
      return std::nullopt;
    return output_.str();
  }

  const std::string &failure() const { return failure_; }

private:
  void line(const std::string &text) {
    for (int index = 0; index < indent_; ++index)
      output_ << "  ";
    output_ << text << '\n';
  }

  void blank() { output_ << '\n'; }

  static std::string cpp_type(const Ty &type) {
    switch (type.kind) {
    case Ty::Kind::Int:
      return "__nebula_i64";
    case Ty::Kind::Bool:
      return "bool";
    case Ty::Kind::Void:
      return "void";
    default:
      return {};
    }
  }

  static std::string variable_name(VarId variable) {
    return "__nebula_v_" + std::to_string(variable);
  }

  void emit_checked_arithmetic() {
    const auto used = [&](BinOp operation) {
      return program_.arithmetic_operations.find(operation) != program_.arithmetic_operations.end();
    };
    if (used(BinOp::Add)) {
      line("static __nebula_i64 __nebula_checked_add(__nebula_i64 lhs, __nebula_i64 rhs) {");
      ++indent_;
      line("__nebula_i64 result = 0;");
      line("if (__builtin_add_overflow(lhs, rhs, &result)) __builtin_trap();");
      line("return result;");
      --indent_;
      line("}");
      blank();
    }
    if (used(BinOp::Sub)) {
      line("static __nebula_i64 __nebula_checked_sub(__nebula_i64 lhs, __nebula_i64 rhs) {");
      ++indent_;
      line("__nebula_i64 result = 0;");
      line("if (__builtin_sub_overflow(lhs, rhs, &result)) __builtin_trap();");
      line("return result;");
      --indent_;
      line("}");
      blank();
    }
    if (used(BinOp::Mul)) {
      line("static __nebula_i64 __nebula_checked_mul(__nebula_i64 lhs, __nebula_i64 rhs) {");
      ++indent_;
      line("__nebula_i64 result = 0;");
      line("if (__builtin_mul_overflow(lhs, rhs, &result)) __builtin_trap();");
      line("return result;");
      --indent_;
      line("}");
      blank();
    }
    if (used(BinOp::Div) || used(BinOp::Mod)) {
      line("static constexpr __nebula_i64 __nebula_i64_min = "
           "static_cast<__nebula_i64>(-9223372036854775807LL - 1LL);");
      blank();
    }
    if (used(BinOp::Div)) {
      line("static __nebula_i64 __nebula_checked_div(__nebula_i64 lhs, __nebula_i64 rhs) {");
      ++indent_;
      line("if (rhs == 0 || (lhs == __nebula_i64_min && rhs == -1)) __builtin_trap();");
      line("return lhs / rhs;");
      --indent_;
      line("}");
      blank();
    }
    if (used(BinOp::Mod)) {
      line("static __nebula_i64 __nebula_checked_mod(__nebula_i64 lhs, __nebula_i64 rhs) {");
      ++indent_;
      line("if (rhs == 0 || (lhs == __nebula_i64_min && rhs == -1)) __builtin_trap();");
      line("return lhs % rhs;");
      --indent_;
      line("}");
      blank();
    }
  }

  std::string signature(const Function &function) {
    const std::string return_type = cpp_type(function.ret);
    if (return_type.empty())
      return fail("unsupported function return type reached emitter");
    std::ostringstream signature;
    signature << "static " << return_type << " " << emitted_cpp_function_name(function) << "(";
    for (std::size_t index = 0; index < function.params.size(); ++index) {
      if (index != 0)
        signature << ", ";
      const std::string parameter_type = cpp_type(function.params[index].ty);
      if (parameter_type.empty())
        return fail("unsupported parameter type reached emitter");
      signature << parameter_type << " " << variable_name(function.params[index].var);
    }
    signature << ")";
    return signature.str();
  }

  void emit_function_declaration(const Function &function) {
    const std::string text = signature(function);
    if (!failed_)
      line(text + ";");
  }

  void emit_function(const Function &function) {
    const std::string text = signature(function);
    if (failed_)
      return;
    line(text + " {");
    ++indent_;
    emit_block(*function.body);
    --indent_;
    line("}");
  }

  void emit_block(const Block &block) {
    for (const auto &statement : block.stmts)
      emit_statement(statement);
  }

  void emit_statement(const Stmt &statement) {
    if (failed_)
      return;
    std::visit(
      [&](const auto &node) {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, Stmt::Let>) {
          line(cpp_type(node.ty) + " " + variable_name(node.var) + " = " +
               emit_expression(*node.value) + ";");
        } else if constexpr (std::is_same_v<Node, Stmt::Return>) {
          line("return " + emit_expression(*node.value) + ";");
        } else if constexpr (std::is_same_v<Node, Stmt::ExprStmt>) {
          line("(void)(" + emit_expression(*node.expr) + ");");
        } else if constexpr (std::is_same_v<Node, Stmt::AssignVar>) {
          line(variable_name(node.var) + " = " + emit_expression(*node.value) + ";");
        } else if constexpr (std::is_same_v<Node, Stmt::If>) {
          line("if (" + emit_expression(*node.cond) + ") {");
          ++indent_;
          emit_block(node.then_body);
          --indent_;
          if (node.else_body.has_value()) {
            line("} else {");
            ++indent_;
            emit_block(*node.else_body);
            --indent_;
          }
          line("}");
        } else if constexpr (std::is_same_v<Node, Stmt::For>) {
          const std::string variable = variable_name(node.var);
          line("for (__nebula_i64 " + variable + " = " + emit_expression(*node.start) + "; " +
               variable + " < " + emit_expression(*node.end) + "; " + variable +
               " = __nebula_checked_add(" + variable + ", static_cast<__nebula_i64>(1LL))) {");
          ++indent_;
          emit_block(node.body);
          --indent_;
          line("}");
        } else if constexpr (std::is_same_v<Node, Stmt::While>) {
          line("while (" + emit_expression(*node.cond) + ") {");
          ++indent_;
          emit_block(node.body);
          --indent_;
          line("}");
        } else if constexpr (std::is_same_v<Node, Stmt::Break>) {
          line("break;");
        } else if constexpr (std::is_same_v<Node, Stmt::Continue>) {
          line("continue;");
        } else {
          (void)fail("unsupported statement reached freestanding emitter");
        }
      },
      statement.node);
  }

  std::string emit_expression(const Expr &expression) {
    if (failed_)
      return {};
    return std::visit(
      [&](const auto &node) -> std::string {
        using Node = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Node, Expr::IntLit>) {
          if (node.value == std::numeric_limits<std::int64_t>::min()) {
            return "static_cast<__nebula_i64>(-9223372036854775807LL - 1LL)";
          }
          return "static_cast<__nebula_i64>(" + std::to_string(node.value) + "LL)";
        } else if constexpr (std::is_same_v<Node, Expr::BoolLit>) {
          return node.value ? "true" : "false";
        } else if constexpr (std::is_same_v<Node, Expr::VarRef>) {
          return variable_name(node.var);
        } else if constexpr (std::is_same_v<Node, Expr::Call>) {
          if (!node.resolved_callee.has_value())
            return fail("unresolved call reached emitter");
          const auto target = functions_.find(node.resolved_callee->display_name());
          if (target == functions_.end())
            return fail("unknown call target reached emitter");
          std::ostringstream call;
          call << emitted_cpp_function_name(*target->second) << "(";
          for (std::size_t index = 0; index < node.args.size(); ++index) {
            if (index != 0)
              call << ", ";
            call << emit_expression(*node.args[index]);
          }
          call << ")";
          return call.str();
        } else if constexpr (std::is_same_v<Node, Expr::Binary>) {
          const std::string lhs = emit_expression(*node.lhs);
          const std::string rhs = emit_expression(*node.rhs);
          switch (node.op) {
          case BinOp::Add:
            return "__nebula_checked_add(" + lhs + ", " + rhs + ")";
          case BinOp::Sub:
            return "__nebula_checked_sub(" + lhs + ", " + rhs + ")";
          case BinOp::Mul:
            return "__nebula_checked_mul(" + lhs + ", " + rhs + ")";
          case BinOp::Div:
            return "__nebula_checked_div(" + lhs + ", " + rhs + ")";
          case BinOp::Mod:
            return "__nebula_checked_mod(" + lhs + ", " + rhs + ")";
          case BinOp::Eq:
            return "(" + lhs + " == " + rhs + ")";
          case BinOp::Ne:
            return "(" + lhs + " != " + rhs + ")";
          case BinOp::Lt:
            return "(" + lhs + " < " + rhs + ")";
          case BinOp::Lte:
            return "(" + lhs + " <= " + rhs + ")";
          case BinOp::Gt:
            return "(" + lhs + " > " + rhs + ")";
          case BinOp::Gte:
            return "(" + lhs + " >= " + rhs + ")";
          case BinOp::And:
            return "(" + lhs + " && " + rhs + ")";
          case BinOp::Or:
            return "(" + lhs + " || " + rhs + ")";
          }
          return fail("unknown binary operation reached emitter");
        } else if constexpr (std::is_same_v<Node, Expr::Unary>) {
          return "(!" + emit_expression(*node.inner) + ")";
        } else {
          return fail("unsupported expression reached freestanding emitter");
        }
      },
      expression.node);
  }

  std::string fail(std::string message) {
    if (!failed_)
      failure_ = std::move(message);
    failed_ = true;
    return {};
  }

  const ValidatedProgram &program_;
  const std::unordered_map<std::string, const Function *> &functions_;
  std::ostringstream output_;
  int indent_ = 0;
  bool failed_ = false;
  std::string failure_;
};

std::unordered_map<std::string, const Function *>
index_reachable_functions(const ValidatedProgram &program) {
  std::unordered_map<std::string, const Function *> functions;
  for (const Function *function : program.reachable) {
    functions.emplace(nebula::nir::function_identity(*function), function);
  }
  return functions;
}

} // namespace

FreestandingCppEmission emit_freestanding_cpp(const Program &program,
                                              const RepOwnerResult &rep_owner,
                                              const FreestandingEmitOptions &options) {
  Validator validator(program, rep_owner, options);
  auto validated = validator.run();
  if (!validated.has_value()) {
    return {std::nullopt, validator.take_diagnostics()};
  }

  const auto functions = index_reachable_functions(*validated);
  TextEmitter emitter(*validated, functions);
  auto translation_unit = emitter.run();
  if (!translation_unit.has_value()) {
    auto diagnostic = make_backend_error(
      "NBL-BE-FS-INTERNAL", "freestanding emitter rejected previously validated NIR",
      validated->entry != nullptr ? validated->entry->span : Span{}, emitter.failure(),
      "the compiler stopped instead of emitting a potentially incorrect object");
    return {std::nullopt, {std::move(diagnostic)}};
  }

  return {std::move(translation_unit), {}};
}

} // namespace nebula::codegen
