#include "runtime/nebula_runtime.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using RtJson = nebula::rt::JsonValue;
using RtResult = nebula::rt::Result<std::string, std::string>;

RtResult ok_string(std::string value) {
  return nebula::rt::ok_result(std::move(value));
}

RtResult err_string(std::string value) {
  return nebula::rt::err_result<std::string>(std::move(value));
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

bool parse_required_string(nebula::rt::JsonCursor& cursor,
                           std::string* out,
                           std::string_view message,
                           std::string* error) {
  std::string value;
  if (!nebula::rt::parse_json_string_literal(cursor, &value) || value.empty()) {
    return set_error(error, std::string(message));
  }
  if (out != nullptr) *out = std::move(value);
  return true;
}

bool capture_json_value(nebula::rt::JsonCursor& cursor, std::string_view* raw) {
  cursor.skip_ws();
  const std::size_t start = cursor.pos();
  if (!nebula::rt::parse_json_value(cursor)) return false;
  if (raw != nullptr) *raw = cursor.slice(start, cursor.pos());
  return true;
}

bool validate_action_node_raw(std::string_view raw,
                              std::string_view target_action,
                              bool* found,
                              std::string* error);

bool validate_children_raw(std::string_view raw,
                           std::string_view target_action,
                           bool* found,
                           std::string* error) {
  nebula::rt::JsonCursor cursor(raw);
  if (!cursor.consume('[')) return set_error(error, "expected UI node children array");
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == ']') {
    cursor.set_pos(cursor.pos() + 1);
    cursor.skip_ws();
    return cursor.eof() || set_error(error, "expected UI node children array");
  }
  while (true) {
    std::string_view child_raw;
    if (!capture_json_value(cursor, &child_raw)) {
      return set_error(error, "expected UI node children array");
    }
    if (!validate_action_node_raw(child_raw, target_action, found, error)) return false;
    cursor.skip_ws();
    if (cursor.eof()) return set_error(error, "expected UI node children array");
    if (cursor.peek() == ']') {
      cursor.set_pos(cursor.pos() + 1);
      cursor.skip_ws();
      return cursor.eof() || set_error(error, "expected UI node children array");
    }
    if (cursor.peek() != ',') return set_error(error, "expected UI node children array");
    cursor.set_pos(cursor.pos() + 1);
  }
}

bool validate_props_raw(std::string_view raw,
                        std::string_view component,
                        std::string_view target_action,
                        bool* found,
                        std::string* error) {
  nebula::rt::JsonCursor cursor(raw);
  if (!cursor.consume('{')) return set_error(error, "expected UI node props object");
  bool has_action = false;
  bool has_accessibility_label = false;
  std::string action;
  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == '}') {
    cursor.set_pos(cursor.pos() + 1);
  } else {
    while (true) {
      std::string key;
      if (!nebula::rt::parse_json_string_literal(cursor, &key) || !cursor.consume(':')) {
        return set_error(error, "expected UI node props object");
      }
      if (key == "action") {
        const std::string_view message = component == "Button"
                                             ? "Button node requires non-empty action string"
                                             : "Input node requires non-empty action string";
        if (!parse_required_string(cursor, &action, message, error)) return false;
        has_action = true;
      } else if (key == "accessibility_label") {
        if (!parse_required_string(cursor,
                                   nullptr,
                                   "Input node requires non-empty accessibility_label string",
                                   error)) {
          return false;
        }
        has_accessibility_label = true;
      } else if (!capture_json_value(cursor, nullptr)) {
        return set_error(error, "expected UI node props object");
      }
      cursor.skip_ws();
      if (cursor.eof()) return set_error(error, "expected UI node props object");
      if (cursor.peek() == '}') {
        cursor.set_pos(cursor.pos() + 1);
        break;
      }
      if (cursor.peek() != ',') return set_error(error, "expected UI node props object");
      cursor.set_pos(cursor.pos() + 1);
    }
  }
  cursor.skip_ws();
  if (!cursor.eof()) return set_error(error, "expected UI node props object");

  if (component == "Button") {
    if (!has_action) return set_error(error, "Button node requires non-empty action string");
    if (action == target_action) *found = true;
  } else if (component == "Input") {
    if (!has_action) return set_error(error, "Input node requires non-empty action string");
    if (!has_accessibility_label) {
      return set_error(error, "Input node requires non-empty accessibility_label string");
    }
    if (action == target_action) *found = true;
  }
  return true;
}

bool validate_action_node_raw(std::string_view raw,
                              std::string_view target_action,
                              bool* found,
                              std::string* error) {
  nebula::rt::JsonCursor cursor(raw);
  if (!cursor.consume('{')) {
    return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
  }

  bool has_schema = false;
  bool has_component = false;
  bool has_props = false;
  bool has_children = false;
  std::string component;
  std::string_view props_raw;
  std::string_view children_raw;

  cursor.skip_ws();
  if (!cursor.eof() && cursor.peek() == '}') {
    cursor.set_pos(cursor.pos() + 1);
  } else {
    while (true) {
      std::string key;
      if (!nebula::rt::parse_json_string_literal(cursor, &key) || !cursor.consume(':')) {
        return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
      }
      if (key == "schema") {
        std::string schema;
        if (!parse_required_string(cursor, &schema, "expected nebula-ui.tree.v1 root-view schema", error)) {
          return false;
        }
        if (schema != "nebula-ui.tree.v1") {
          return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
        }
        has_schema = true;
      } else if (key == "component") {
        if (!parse_required_string(cursor, &component, "expected UI node component string", error)) {
          return false;
        }
        has_component = true;
      } else if (key == "props") {
        if (!capture_json_value(cursor, &props_raw)) {
          return set_error(error, "expected UI node props object");
        }
        has_props = true;
      } else if (key == "children") {
        if (!capture_json_value(cursor, &children_raw)) {
          return set_error(error, "expected UI node children array");
        }
        has_children = true;
      } else if (!capture_json_value(cursor, nullptr)) {
        return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
      }
      cursor.skip_ws();
      if (cursor.eof()) return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
      if (cursor.peek() == '}') {
        cursor.set_pos(cursor.pos() + 1);
        break;
      }
      if (cursor.peek() != ',') {
        return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
      }
      cursor.set_pos(cursor.pos() + 1);
    }
  }
  cursor.skip_ws();
  if (!cursor.eof()) return set_error(error, "expected nebula-ui.tree.v1 root-view schema");

  if (!has_schema) return set_error(error, "expected nebula-ui.tree.v1 root-view schema");
  if (!has_component) return set_error(error, "expected UI node component string");
  if (!validate_component(component, error)) return false;
  if (!has_props) return set_error(error, "expected UI node props object");
  if (!validate_props_raw(props_raw, component, target_action, found, error)) return false;
  if (!has_children) return set_error(error, "expected UI node children array");
  return validate_children_raw(children_raw, target_action, found, error);
}

}  // namespace

RtResult __nebula_ui_headless_dispatch_action_wire(RtJson tree, std::string action) {
  if (action.empty()) return err_string("action id must be non-empty");

  if (tree.text.empty()) return err_string("expected nebula-ui.tree.v1 root-view schema");
  bool found = false;
  std::string error;
  if (!validate_action_node_raw(tree.text, action, &found, &error)) {
    return err_string(std::move(error));
  }
  if (found) return ok_string("nebula-ui-action-dispatched=" + action);
  return err_string("action not found: " + action);
}
