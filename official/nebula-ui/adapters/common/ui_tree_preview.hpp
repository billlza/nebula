#pragma once

#ifdef assert
#undef assert
#endif

#include "runtime/nebula_runtime.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::ui::adapter_preview {

struct ButtonSummary {
  std::string text;
  std::string action;
};

struct InputSummary {
  std::string value;
  std::string action;
  std::string accessibility_label;
};

struct AccessibilitySummary {
  std::string role;
  std::string name;
  std::string action;
};

struct TreeSummary {
  std::string title = "Nebula UI";
  std::vector<std::string> text_labels;
  std::vector<ButtonSummary> buttons;
  std::vector<InputSummary> inputs;
  std::vector<AccessibilitySummary> accessibility;
  bool has_window = false;
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

struct WindowLifecycleSummary {
  std::vector<std::string> events;
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

inline bool validate_component_name(const std::string& component_text, std::string* error) {
  if (component_text == "Window" || component_text == "Column" || component_text == "Row" ||
      component_text == "Text" || component_text == "Button" || component_text == "Input" ||
      component_text == "Spacer") {
    return true;
  }
  if (error != nullptr) *error = "unsupported UI component: " + component_text;
  return false;
}

inline bool require_node_props(const nebula::rt::JsonValue& node, std::string* error) {
  auto props = nebula::rt::json_get_value(node, "props");
  if (nebula::rt::result_is_err(props)) {
    if (error != nullptr) *error = "expected UI node props object";
    return false;
  }
  return true;
}

inline bool require_non_empty_prop(const nebula::rt::JsonValue& node,
                                   std::string_view name,
                                   const std::string& message,
                                   std::string* out,
                                   std::string* error) {
  auto value = string_prop(node, name);
  if (!value.has_value() || value->empty()) {
    if (error != nullptr) *error = message;
    return false;
  }
  if (out != nullptr) *out = *value;
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
  if (!validate_component_name(component_text, error)) return false;
  if (!require_node_props(node, error)) return false;

  if (component_text == "Window") {
    if (auto title = string_prop(node, "title")) summary.title = *title;
    summary.has_window = true;
    summary.accessibility.push_back(AccessibilitySummary{"window", summary.title, ""});
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
    summary.accessibility.push_back(
        AccessibilitySummary{"button", button.text.empty() ? button.action : button.text, button.action});
    summary.buttons.push_back(std::move(button));
  } else if (component_text == "Input") {
    InputSummary input;
    if (auto value = string_prop(node, "value")) input.value = *value;
    if (!require_non_empty_prop(node,
                                "action",
                                "Input node requires non-empty action string",
                                &input.action,
                                error)) {
      return false;
    }
    if (!require_non_empty_prop(node,
                                "accessibility_label",
                                "Input node requires non-empty accessibility_label string",
                                &input.accessibility_label,
                                error)) {
      return false;
    }
    summary.accessibility.push_back(AccessibilitySummary{"textbox", input.accessibility_label, input.action});
    summary.inputs.push_back(std::move(input));
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

inline WindowLifecycleSummary host_shell_lifecycle_summary(bool closed) {
  WindowLifecycleSummary summary;
  summary.events = {"boot", "window-created", "window-shown", "window-focused", "rendered"};
  if (closed) {
    summary.events.push_back("close-requested");
    summary.events.push_back("closed");
  }
  return summary;
}

inline bool validate_lifecycle_order(const WindowLifecycleSummary& lifecycle, std::string* error) {
  const std::vector<std::string> expected_prefix = {
      "boot", "window-created", "window-shown", "window-focused", "rendered"};
  if (lifecycle.events.size() < expected_prefix.size()) {
    if (error != nullptr) *error = "window lifecycle is missing required startup events";
    return false;
  }
  for (std::size_t i = 0; i < expected_prefix.size(); ++i) {
    if (lifecycle.events[i] != expected_prefix[i]) {
      if (error != nullptr) *error = "window lifecycle startup order mismatch";
      return false;
    }
  }
  for (std::size_t i = expected_prefix.size(); i < lifecycle.events.size(); ++i) {
    const auto& event = lifecycle.events[i];
    if (event != "close-requested" && event != "closed" && event != "resize" && event != "focus") {
      if (error != nullptr) *error = "unsupported window lifecycle event: " + event;
      return false;
    }
  }
  for (std::size_t i = 0; i + 1 < lifecycle.events.size(); ++i) {
    if (lifecycle.events[i] == "closed" && lifecycle.events[i + 1] != "closed") {
      if (error != nullptr) *error = "window lifecycle event after closed";
      return false;
    }
  }
  return true;
}

inline void append_lifecycle_summary(std::ostream& out, const WindowLifecycleSummary& lifecycle) {
  for (const auto& event : lifecycle.events) {
    out << "nebula-ui-window-lifecycle=" << event << "\n";
  }
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
  if (!summary.inputs.empty()) {
    out << "nebula-ui-tree-input-value=" << summary.inputs.front().value << "\n";
    out << "nebula-ui-tree-input-action=" << summary.inputs.front().action << "\n";
    out << "nebula-ui-tree-input-accessibility-label=" << summary.inputs.front().accessibility_label
        << "\n";
  }
  for (const auto& node : summary.accessibility) {
    out << "nebula-ui-accessibility-node=role:" << node.role << ";name:" << node.name;
    if (!node.action.empty()) out << ";action:" << node.action;
    out << "\n";
  }
}

inline ActionDispatchResult dispatch_action(const TreeSummary& summary, const std::string& action) {
  if (action.empty()) return ActionDispatchResult{false, "action id must be non-empty"};
  for (const auto& button : summary.buttons) {
    if (button.action == action) {
      return ActionDispatchResult{true, "nebula-ui-action-dispatched=" + action};
    }
  }
  for (const auto& input : summary.inputs) {
    if (input.action == action) {
      return ActionDispatchResult{true, "nebula-ui-action-dispatched=" + action};
    }
  }
  return ActionDispatchResult{false, "action not found: " + action};
}

inline void append_dispatch_result(std::ostream& out, const ActionDispatchResult& result) {
  out << result.message << "\n";
}

}  // namespace nebula::ui::adapter_preview
