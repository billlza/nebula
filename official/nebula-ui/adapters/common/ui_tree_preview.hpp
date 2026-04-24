#pragma once

#ifdef assert
#undef assert
#endif

#include "runtime/nebula_runtime.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nebula::ui::adapter_preview {

struct ButtonSummary {
  std::string text;
  std::string action;
};

struct TreeSummary {
  std::string title = "Nebula UI";
  std::vector<std::string> text_labels;
  std::vector<ButtonSummary> buttons;
};

struct LoadResult {
  bool ok = false;
  TreeSummary summary;
  std::string error;
};

struct ActionDispatchResult {
  bool ok = false;
  std::string message;
};

inline std::optional<std::string> string_prop(const nebula::rt::JsonValue& node,
                                              std::string_view name) {
  auto props = nebula::rt::json_get_value(node, "props");
  if (nebula::rt::result_is_err(props)) return std::nullopt;
  auto value = nebula::rt::json_get_string(nebula::rt::result_ok_ref(props), name);
  if (nebula::rt::result_is_err(value)) return std::nullopt;
  return nebula::rt::result_ok_ref(value);
}

inline bool validate_node_schema(const nebula::rt::JsonValue& node, std::string* error) {
  auto schema = nebula::rt::json_get_string(node, "schema");
  if (nebula::rt::result_is_err(schema) || nebula::rt::result_ok_ref(schema) != "nebula-ui.tree.v1") {
    if (error != nullptr) *error = "expected nebula-ui.tree.v1 root-view schema";
    return false;
  }
  return true;
}

inline bool summarize_node(const nebula::rt::JsonValue& node,
                           TreeSummary& summary,
                           std::string* error) {
  if (!validate_node_schema(node, error)) return false;

  auto component = nebula::rt::json_get_string(node, "component");
  if (nebula::rt::result_is_err(component)) {
    if (error != nullptr) *error = "expected UI node component string";
    return false;
  }

  const auto& component_text = nebula::rt::result_ok_ref(component);
  if (component_text == "Window") {
    if (auto title = string_prop(node, "title")) summary.title = *title;
  } else if (component_text == "Text") {
    if (auto text = string_prop(node, "text")) summary.text_labels.push_back(*text);
  } else if (component_text == "Button") {
    ButtonSummary button;
    if (auto text = string_prop(node, "text")) button.text = *text;
    auto action = string_prop(node, "action");
    if (!action.has_value() || action->empty()) {
      if (error != nullptr) *error = "Button node requires non-empty action string";
      return false;
    }
    button.action = *action;
    summary.buttons.push_back(std::move(button));
  }

  auto children = nebula::rt::json_get_value(node, "children");
  if (nebula::rt::result_is_err(children)) {
    if (error != nullptr) *error = "expected UI node children array";
    return false;
  }
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(children));
  if (nebula::rt::result_is_err(count)) {
    if (error != nullptr) *error = "expected UI node children array";
    return false;
  }
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto child = nebula::rt::json_array_get(nebula::rt::result_ok_ref(children), i);
    if (nebula::rt::result_is_err(child)) {
      if (error != nullptr) *error = nebula::rt::result_err_ref(child);
      return false;
    }
    if (!summarize_node(nebula::rt::result_ok_ref(child), summary, error)) return false;
  }
  return true;
}

inline LoadResult load_tree_json_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return LoadResult{false, {}, "failed to open UI tree JSON file: " + path};

  std::ostringstream buffer;
  buffer << in.rdbuf();

  auto parsed = nebula::rt::json_parse(buffer.str());
  if (nebula::rt::result_is_err(parsed)) {
    return LoadResult{false, {}, "failed to parse UI tree JSON: " + nebula::rt::result_err_ref(parsed)};
  }

  TreeSummary summary;
  std::string summary_error;
  if (!summarize_node(nebula::rt::result_ok_ref(parsed), summary, &summary_error)) {
    return LoadResult{false, {}, summary_error};
  }
  return LoadResult{true, std::move(summary), ""};
}

inline void append_smoke_summary(std::ostream& out, const TreeSummary& summary) {
  out << "nebula-ui-tree-title=" << summary.title << "\n";
  if (!summary.text_labels.empty()) {
    out << "nebula-ui-tree-text=" << summary.text_labels.front() << "\n";
  }
  if (!summary.buttons.empty()) {
    out << "nebula-ui-tree-button=" << summary.buttons.front().text << "\n";
    out << "nebula-ui-tree-button-action=" << summary.buttons.front().action << "\n";
  }
}

inline ActionDispatchResult dispatch_action(const TreeSummary& summary, const std::string& action) {
  if (action.empty()) return ActionDispatchResult{false, "action id must be non-empty"};
  for (const auto& button : summary.buttons) {
    if (button.action == action) {
      return ActionDispatchResult{true, "nebula-ui-action-dispatched=" + action};
    }
  }
  return ActionDispatchResult{false, "action not found: " + action};
}

inline void append_dispatch_result(std::ostream& out, const ActionDispatchResult& result) {
  out << result.message << "\n";
}

}  // namespace nebula::ui::adapter_preview
