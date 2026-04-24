#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::frontend {

enum class ExternalEscapeState : std::uint8_t { Unspecified, NoEscape, MayEscape, Unknown };

struct ExternalEscapeContract {
  bool has_annotations = false;
  bool has_return_contract = false;
  bool returns_fresh = false;
  std::vector<bool> return_depends_on_param;
  std::vector<ExternalEscapeState> param_states;
  std::vector<std::string> errors;
};

inline std::optional<std::size_t> parse_external_contract_index(std::string_view digits) {
  if (digits.empty()) return std::nullopt;
  std::size_t value = 0;
  for (char ch : digits) {
    if (ch < '0' || ch > '9') return std::nullopt;
    value = value * 10 + static_cast<std::size_t>(ch - '0');
  }
  return value;
}

inline std::optional<std::size_t> parse_external_contract_param_annotation(std::string_view annotation,
                                                                           std::string_view suffix) {
  if (!annotation.starts_with("param") || !annotation.ends_with(suffix)) return std::nullopt;
  const std::size_t prefix_len = std::string_view("param").size();
  const std::size_t digits_len = annotation.size() - prefix_len - suffix.size();
  return parse_external_contract_index(annotation.substr(prefix_len, digits_len));
}

inline bool is_external_escape_contract_annotation(std::string_view annotation) {
  if (annotation == "returns_fresh") return true;
  if (annotation.starts_with("returns_param")) return true;
  if (annotation.starts_with("param") && annotation.ends_with("_noescape")) return true;
  if (annotation.starts_with("param") && annotation.ends_with("_may_escape")) return true;
  if (annotation.starts_with("param") && annotation.ends_with("_escape_unknown")) return true;
  return false;
}

inline ExternalEscapeContract parse_external_escape_contract(const std::vector<std::string>& annotations,
                                                             std::size_t param_count) {
  ExternalEscapeContract contract;
  contract.return_depends_on_param.assign(param_count, false);
  contract.param_states.assign(param_count, ExternalEscapeState::Unspecified);

  auto push_missing_param = [&](std::string_view annotation) {
    contract.errors.push_back("external escape contract references a missing parameter: @" +
                              std::string(annotation));
  };
  auto push_conflict = [&](std::string_view detail) {
    contract.errors.push_back("conflicting external escape contract annotation: " +
                              std::string(detail));
  };
  auto set_param_state = [&](std::size_t index,
                             ExternalEscapeState state,
                             std::string_view annotation) {
    if (index >= contract.param_states.size()) {
      push_missing_param(annotation);
      return;
    }
    const ExternalEscapeState existing = contract.param_states[index];
    if (existing != ExternalEscapeState::Unspecified && existing != state) {
      push_conflict("@" + std::string(annotation));
      return;
    }
    contract.param_states[index] = state;
  };

  for (const auto& annotation : annotations) {
    if (annotation == "returns_fresh") {
      contract.has_annotations = true;
      contract.has_return_contract = true;
      if (contract.returns_fresh) continue;
      if (std::find(contract.return_depends_on_param.begin(),
                    contract.return_depends_on_param.end(),
                    true) != contract.return_depends_on_param.end()) {
        push_conflict("@returns_fresh");
        continue;
      }
      contract.returns_fresh = true;
      continue;
    }

    if (annotation.starts_with("returns_param")) {
      contract.has_annotations = true;
      contract.has_return_contract = true;
      const auto index =
          parse_external_contract_index(annotation.substr(std::string_view("returns_param").size()));
      if (!index.has_value() || *index >= contract.return_depends_on_param.size()) {
        push_missing_param(annotation);
        continue;
      }
      if (contract.returns_fresh) {
        push_conflict("@" + annotation);
        continue;
      }
      contract.return_depends_on_param[*index] = true;
      continue;
    }

    if (auto index = parse_external_contract_param_annotation(annotation, "_noescape");
        index.has_value()) {
      contract.has_annotations = true;
      set_param_state(*index, ExternalEscapeState::NoEscape, annotation);
      continue;
    }
    if (auto index = parse_external_contract_param_annotation(annotation, "_may_escape");
        index.has_value()) {
      contract.has_annotations = true;
      set_param_state(*index, ExternalEscapeState::MayEscape, annotation);
      continue;
    }
    if (auto index = parse_external_contract_param_annotation(annotation, "_escape_unknown");
        index.has_value()) {
      contract.has_annotations = true;
      set_param_state(*index, ExternalEscapeState::Unknown, annotation);
      continue;
    }
  }

  return contract;
}

} // namespace nebula::frontend
