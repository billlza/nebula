#include "runtime/nebula_runtime.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using RtJson = nebula::rt::JsonValue;
using RtResult = nebula::rt::Result<std::string, std::string>;
using RtJsonResult = nebula::rt::Result<RtJson, std::string>;

struct ActionSummary {
  std::vector<std::string> actions;
  std::string first_button_action;
  std::string first_input_action;
  std::int64_t button_count = 0;
  std::int64_t input_count = 0;

  void add_button(std::string action) {
    button_count += 1;
    if (first_button_action.empty()) first_button_action = action;
    actions.push_back(std::move(action));
  }

  void add_input(std::string action) {
    input_count += 1;
    if (first_input_action.empty()) first_input_action = action;
    actions.push_back(std::move(action));
  }
};

RtResult ok_string(std::string value) {
  return nebula::rt::ok_result(std::move(value));
}

RtResult err_string(std::string value) {
  return nebula::rt::err_result<std::string>(std::move(value));
}

void append_json_string(std::string& out, std::string_view value) {
  out.push_back('"');
  nebula::rt::json_append_escaped_string(out, value);
  out.push_back('"');
}

void append_json_string_field(std::string& out, std::string_view key, std::string_view value) {
  append_json_string(out, key);
  out.push_back(':');
  append_json_string(out, value);
}

void append_json_int_text_field(std::string& out, std::string_view key, std::string_view value) {
  append_json_string(out, key);
  out.push_back(':');
  out += value;
}

bool set_error(std::string* error, std::string value) {
  if (error != nullptr) *error = std::move(value);
  return false;
}

bool validate_component(std::string_view component, std::string* error) {
  if (component == "Window" || component == "Column" || component == "Row" ||
      component == "Text" || component == "Button" || component == "Input" ||
      component == "Spacer") {
    return true;
  }
  return set_error(error, "unsupported UI component: " + std::string(component));
}

bool get_required_string_field(const RtJson& value,
                               std::string_view key,
                               std::string_view message,
                               std::string* out,
                               std::string* error) {
  auto result = nebula::rt::json_get_string(value, key);
  if (nebula::rt::result_is_err(result) || nebula::rt::result_ok_ref(result).empty()) {
    return set_error(error, std::string(message));
  }
  if (out != nullptr) *out = std::move(nebula::rt::result_ok_ref(result));
  return true;
}

bool get_required_json_field(const RtJson& value,
                             std::string_view key,
                             std::string_view message,
                             RtJson* out,
                             std::string* error) {
  auto result = nebula::rt::json_get_value(value, key);
  if (nebula::rt::result_is_err(result)) return set_error(error, std::string(message));
  if (out != nullptr) *out = std::move(nebula::rt::result_ok_ref(result));
  return true;
}

bool validate_action_node_value(const RtJson& node,
                                std::string_view target_action,
                                bool* found,
                                ActionSummary* summary,
                                std::string* error);

bool validate_children_value(const RtJson& children,
                             std::string_view target_action,
                             bool* found,
                             ActionSummary* summary,
                             std::string* error) {
  auto count = nebula::rt::json_array_len(children);
  if (nebula::rt::result_is_err(count)) return set_error(error, "expected UI node children array");

  const auto total = nebula::rt::result_ok_ref(count);
  for (std::int64_t i = 0; i < total; ++i) {
    auto child = nebula::rt::json_array_get(children, i);
    if (nebula::rt::result_is_err(child)) return set_error(error, "expected UI node children array");
    if (!validate_action_node_value(nebula::rt::result_ok_ref(child), target_action, found, summary, error)) {
      return false;
    }
  }
  return true;
}

bool validate_props_value(const RtJson& props,
                          std::string_view component,
                          std::string_view target_action,
                          bool* found,
                          ActionSummary* summary,
                          std::string* error) {
  if (props.parsed.kind != nebula::rt::JsonValueKind::Object) {
    return set_error(error, "expected UI node props object");
  }

  if (component == "Button") {
    std::string action;
    if (!get_required_string_field(props, "action", "Button node requires non-empty action string", &action, error)) {
      return false;
    }
    if (summary != nullptr) summary->add_button(action);
    if (action == target_action) *found = true;
  } else if (component == "Input") {
    std::string action;
    if (!get_required_string_field(props, "action", "Input node requires non-empty action string", &action, error)) {
      return false;
    }
    if (!get_required_string_field(props,
                                   "accessibility_label",
                                   "Input node requires non-empty accessibility_label string",
                                   nullptr,
                                   error)) {
      return false;
    }
    if (summary != nullptr) summary->add_input(action);
    if (action == target_action) *found = true;
  }
  return true;
}

bool validate_action_node_value(const RtJson& node,
                                std::string_view target_action,
                                bool* found,
                                ActionSummary* summary,
                                std::string* error) {
  if (node.parsed.kind != nebula::rt::JsonValueKind::Object) {
    return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
  }

  std::string schema;
  if (!get_required_string_field(node, "schema", "expected nebula-ui.tree.v1 root-view schema", &schema, error) ||
      schema != "nebula-ui.tree.v1") {
    return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
  }

  std::string component;
  if (!get_required_string_field(node, "component", "expected UI node component string", &component, error)) {
    return false;
  }
  if (!validate_component(component, error)) return false;

  RtJson props;
  if (!get_required_json_field(node, "props", "expected UI node props object", &props, error)) return false;
  if (!validate_props_value(props, component, target_action, found, summary, error)) return false;

  RtJson children;
  if (!get_required_json_field(node, "children", "expected UI node children array", &children, error)) return false;
  return validate_children_value(children, target_action, found, summary, error);
}

RtJson action_summary_json(const ActionSummary& summary) {
  auto builder = nebula::rt::json_array_builder();
  for (const auto& action : summary.actions) {
    builder = nebula::rt::json_array_push(std::move(builder), nebula::rt::json_string_value(action));
  }
  return nebula::rt::json_object6("schema",
                                  nebula::rt::json_string_value("nebula-ui.action-summary.v1"),
                                  "actions",
                                  nebula::rt::json_array_build(std::move(builder)),
                                  "button_count",
                                  nebula::rt::json_int_value(summary.button_count),
                                  "input_count",
                                  nebula::rt::json_int_value(summary.input_count),
                                  "first_button_action",
                                  nebula::rt::json_string_value(summary.first_button_action),
                                  "first_input_action",
                                  nebula::rt::json_string_value(summary.first_input_action));
}

}  // namespace

RtResult __nebula_ui_headless_dispatch_action_wire(const RtJson& tree, const std::string& action) {
  if (action.empty()) return err_string("action id must be non-empty");

  bool found = false;
  std::string error;
  if (!validate_action_node_value(tree, action, &found, nullptr, &error)) {
    return err_string(std::move(error));
  }
  if (found) return ok_string("nebula-ui-action-dispatched=" + action);
  return err_string("action not found: " + action);
}

RtJsonResult __nebula_ui_headless_action_summary_wire(const RtJson& tree) {
  bool found = false;
  ActionSummary summary;
  std::string error;
  if (!validate_action_node_value(tree, "", &found, &summary, &error)) {
    return nebula::rt::err_result<RtJson>(std::move(error));
  }
  return nebula::rt::ok_result(action_summary_json(summary));
}

RtResult __nebula_ui_headless_dispatch_action_summary_wire(const RtJson& summary, const std::string& action) {
  if (action.empty()) return err_string("action id must be non-empty");

  auto schema = nebula::rt::json_get_string(summary, "schema");
  if (nebula::rt::result_is_err(schema) ||
      nebula::rt::result_ok_ref(schema) != "nebula-ui.action-summary.v1") {
    return err_string("expected nebula-ui.action-summary.v1 schema");
  }
  auto actions = nebula::rt::json_get_value(summary, "actions");
  if (nebula::rt::result_is_err(actions)) return err_string("expected UI action summary actions array");
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(actions));
  if (nebula::rt::result_is_err(count)) return err_string("expected UI action summary actions array");

  const auto total = nebula::rt::result_ok_ref(count);
  for (std::int64_t i = 0; i < total; ++i) {
    auto item = nebula::rt::json_array_get(nebula::rt::result_ok_ref(actions), i);
    if (nebula::rt::result_is_err(item)) return err_string("expected UI action summary actions array");
    auto candidate = nebula::rt::json_as_string(nebula::rt::result_ok_ref(item));
    if (nebula::rt::result_is_err(candidate)) return err_string("expected UI action summary actions array");
    if (nebula::rt::result_ok_ref(candidate) == action) {
      return ok_string("nebula-ui-action-dispatched=" + action);
    }
  }
  return err_string("action not found: " + action);
}

std::string __nebula_ui_typed_snapshot_text(const std::string& title,
                                            const std::string& text,
                                            const std::string& editable_value,
                                            std::int64_t spacing,
                                            const std::string& primary_action,
                                            const std::string& primary_accessibility_label,
                                            const std::string& secondary_action,
                                            const std::string& secondary_accessibility_label) {
  const std::string spacing_text = std::to_string(spacing);
  std::string out;
  out.reserve(310 + title.size() + text.size() + editable_value.size() + spacing_text.size() +
              primary_action.size() + primary_accessibility_label.size() +
              secondary_action.size() + secondary_accessibility_label.size());
  out += "{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Window\",\"props\":{";
  append_json_string_field(out, "title", title);
  out += "},\"children\":[{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Column\",\"props\":{";
  append_json_int_text_field(out, "spacing", spacing_text);
  out += "},\"children\":[{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Text\",\"props\":{";
  append_json_string_field(out, "text", text);
  out += "},\"children\":[]},{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Input\",\"props\":{";
  append_json_string_field(out, "value", editable_value);
  out.push_back(',');
  append_json_string_field(out, "action", primary_action);
  out.push_back(',');
  append_json_string_field(out, "accessibility_label", primary_accessibility_label);
  out += "},\"children\":[]},{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Button\",\"props\":{";
  append_json_string_field(out, "text", secondary_accessibility_label);
  out.push_back(',');
  append_json_string_field(out, "action", secondary_action);
  out += "},\"children\":[]}]}]}";
  return out;
}
