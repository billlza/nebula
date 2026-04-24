#pragma once

#include "frontend/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nebula::frontend {

// Semantic type used by typecheck + later lowering.
struct Ty {
  enum class Kind : std::uint8_t {
    Int,
    Float,
    Bool,
    String,
    Void,
    Struct,
    Enum,
    TypeParam,
    Callable,
    Unknown,
  };

  Kind kind = Kind::Unknown;
  std::string name;                       // Struct/Enum/TypeParam name
  std::optional<QualifiedName> qualified_name;
  std::vector<Ty> type_args;
  std::vector<Ty> callable_params;        // Callable parameters
  std::vector<bool> callable_params_ref;  // Callable by-ref parameter flags
  std::shared_ptr<Ty> callable_ret;       // Callable return type
  bool is_unsafe_callable = false;        // Callable safety bit

  static Ty Int() {
    Ty t;
    t.kind = Kind::Int;
    t.name = "Int";
    return t;
  }
  static Ty Float() {
    Ty t;
    t.kind = Kind::Float;
    t.name = "Float";
    return t;
  }
  static Ty String() {
    Ty t;
    t.kind = Kind::String;
    t.name = "String";
    return t;
  }
  static Ty Bool() {
    Ty t;
    t.kind = Kind::Bool;
    t.name = "Bool";
    return t;
  }
  static Ty Void() {
    Ty t;
    t.kind = Kind::Void;
    t.name = "Void";
    return t;
  }
  static Ty Unknown() {
    Ty t;
    t.kind = Kind::Unknown;
    t.name = "?";
    return t;
  }

  static Ty Struct(std::string n,
                   std::vector<Ty> args = {},
                   std::optional<QualifiedName> q = std::nullopt) {
    Ty t;
    t.kind = Kind::Struct;
    t.name = std::move(n);
    t.type_args = std::move(args);
    t.qualified_name = std::move(q);
    return t;
  }
  static Ty Enum(std::string n,
                 std::vector<Ty> args = {},
                 std::optional<QualifiedName> q = std::nullopt) {
    Ty t;
    t.kind = Kind::Enum;
    t.name = std::move(n);
    t.type_args = std::move(args);
    t.qualified_name = std::move(q);
    return t;
  }
  static Ty TypeParam(std::string n) {
    Ty t;
    t.kind = Kind::TypeParam;
    t.name = std::move(n);
    return t;
  }
  static Ty Callable(std::vector<Ty> params,
                     Ty ret,
                     bool unsafe_callable = false,
                     std::vector<bool> params_ref = {}) {
    Ty t;
    t.kind = Kind::Callable;
    if (params_ref.size() != params.size()) params_ref.assign(params.size(), false);
    t.callable_params = std::move(params);
    t.callable_params_ref = std::move(params_ref);
    t.callable_ret = std::make_shared<Ty>(std::move(ret));
    t.is_unsafe_callable = unsafe_callable;
    return t;
  }
};

inline bool ty_equal(const Ty& a, const Ty& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == Ty::Kind::Callable) {
    if (a.is_unsafe_callable != b.is_unsafe_callable) return false;
    if (a.callable_params.size() != b.callable_params.size()) return false;
    if (a.callable_params_ref.size() != b.callable_params_ref.size()) return false;
    for (std::size_t i = 0; i < a.callable_params.size(); ++i) {
      if (!ty_equal(a.callable_params[i], b.callable_params[i])) return false;
      if (a.callable_params_ref[i] != b.callable_params_ref[i]) return false;
    }
    if (a.callable_ret == nullptr && b.callable_ret == nullptr) return true;
    if (a.callable_ret == nullptr || b.callable_ret == nullptr) return false;
    return ty_equal(*a.callable_ret, *b.callable_ret);
  }
  if (a.qualified_name.has_value() || b.qualified_name.has_value()) {
    if (a.qualified_name.has_value() != b.qualified_name.has_value()) return false;
    if (*a.qualified_name != *b.qualified_name) return false;
  }
  if (a.name != b.name) return false;
  if (a.type_args.size() != b.type_args.size()) return false;
  for (std::size_t i = 0; i < a.type_args.size(); ++i) {
    if (!ty_equal(a.type_args[i], b.type_args[i])) return false;
  }
  return true;
}

inline std::string ty_to_string(const Ty& t) {
  switch (t.kind) {
  case Ty::Kind::Int: return "Int";
  case Ty::Kind::Float: return "Float";
  case Ty::Kind::Bool: return "Bool";
  case Ty::Kind::String: return "String";
  case Ty::Kind::Void: return "Void";
  case Ty::Kind::Struct:
    if (t.type_args.empty()) return t.name;
    [[fallthrough]];
  case Ty::Kind::Enum:
    if (!t.type_args.empty()) {
      std::string out = t.name + "<";
      for (std::size_t i = 0; i < t.type_args.size(); ++i) {
        if (i) out += ", ";
        out += ty_to_string(t.type_args[i]);
      }
      out += ">";
      return out;
    }
    return t.name;
  case Ty::Kind::TypeParam: return t.name;
  case Ty::Kind::Callable: {
    std::string out = t.is_unsafe_callable ? "UnsafeFn(" : "Fn(";
    for (std::size_t i = 0; i < t.callable_params.size(); ++i) {
      if (i) out += ", ";
      if (i < t.callable_params_ref.size() && t.callable_params_ref[i]) out += "ref ";
      out += ty_to_string(t.callable_params[i]);
    }
    out += ") -> ";
    out += t.callable_ret ? ty_to_string(*t.callable_ret) : "?";
    return out;
  }
  case Ty::Kind::Unknown: return "?";
  }
  return "?";
}

} // namespace nebula::frontend
