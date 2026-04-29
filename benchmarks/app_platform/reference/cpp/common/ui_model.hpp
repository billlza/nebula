#pragma once

#include "bench.hpp"

#include <string>
#include <utility>
#include <vector>

namespace app_platform_cpp::ui {

struct Prop {
  std::string name;
  std::string value;
  bool string_value = true;
};

struct Node {
  std::string component;
  std::vector<Prop> props;
  std::vector<Node> children;
};

struct Rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct LayoutNode {
  std::string node_id;
  std::string component;
  Rect rect;
  std::vector<Prop> props;
  std::vector<LayoutNode> children;
};

struct RenderCommand {
  std::string kind;
  std::string node_id;
  std::string component;
  Rect rect;
  std::string text;
  std::string action;
};

struct InputLookup {
  bool found = false;
  std::string node_id;
  std::string value;
};

struct FastPreviewTree {
  std::string title;
  std::string text;
  std::string input_value;
  std::string input_action;
  std::string input_accessibility_label;
  std::string button_text;
  std::string button_action;
  int spacing = 12;
};

struct FastRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct FastPreviewLayout {
  int width = 0;
  int height = 0;
  FastRect text_rect;
  FastRect input_rect;
  FastRect button_rect;
  std::string input_action;
  std::string button_action;
  int input_action_code = 1;
  int button_action_code = 2;
  int command_count = 0;
};

struct FastActionIndex {
  std::string first_button_action;
  std::string first_input_action;
  std::string button_action;
  std::string input_action;
  int button_action_code = 2;
  int input_action_code = 1;
  int button_count = 1;
  int input_count = 1;
};

struct FastPatch {
  std::string kind;
  std::string node_id;
  std::string prop;
  std::string value;
};

struct FastPatchPlan {
  int kind_code = 0;
  int node_code = 0;
  int prop_code = 0;
  std::string value;
};

struct ActionSummary {
  std::vector<std::string> actions;
  std::string first_button_action;
  std::string first_input_action;
  int button_count = 0;
  int input_count = 0;

  void add_button(std::string action) {
    ++button_count;
    if (first_button_action.empty()) first_button_action = action;
    actions.push_back(std::move(action));
  }

  void add_input(std::string action) {
    ++input_count;
    if (first_input_action.empty()) first_input_action = action;
    actions.push_back(std::move(action));
  }
};

inline Node make_node(std::string component, std::vector<Prop> props, std::vector<Node> children) {
  return Node{std::move(component), std::move(props), std::move(children)};
}

inline Node dashboard_tree() {
  Node text = make_node("Text", {Prop{"text", "Local Ops", true}}, {});
  Node input = make_node("Input",
                         {Prop{"value", "targets", true},
                          Prop{"action", "targets.filter", true},
                          Prop{"accessibility_label", "Filter targets", true}},
                         {});
  Node button = make_node("Button",
                          {Prop{"text", "Refresh", true}, Prop{"action", "targets.refresh", true}},
                          {});
  Node column =
      make_node("Column", {Prop{"spacing", "12", false}}, {std::move(text), std::move(input), std::move(button)});
  return make_node("Window", {Prop{"title", "Local Ops", true}}, {std::move(column)});
}

inline const std::string& prop_value(const Node& node, const std::string& name) {
  for (const auto& prop : node.props) {
    if (prop.name == name) return prop.value;
  }
  fail("missing UI prop: " + name);
}

inline const std::string& dashboard_button_action(const Node& root) {
  if (root.children.size() != 1 || root.children[0].children.size() != 3) {
    fail("bad dashboard UI shape");
  }
  return prop_value(root.children[0].children[2], "action");
}

inline const std::string& dashboard_input_action(const Node& root) {
  if (root.children.size() != 1 || root.children[0].children.size() != 3) {
    fail("bad dashboard UI shape");
  }
  return prop_value(root.children[0].children[1], "action");
}

inline bool is_allowed_component(const std::string& component) {
  return component == "Window" || component == "Column" || component == "Row" ||
         component == "Text" || component == "Button" || component == "Input" ||
         component == "Spacer";
}

inline void validate_action_node(const Node& node, const std::string& action, bool& found) {
  if (!is_allowed_component(node.component)) fail("unsupported UI component: " + node.component);
  if (node.component == "Button") {
    const auto& candidate = prop_value(node, "action");
    if (candidate.empty()) fail("Button node requires non-empty action string");
    if (candidate == action) found = true;
  } else if (node.component == "Input") {
    const auto& candidate = prop_value(node, "action");
    if (candidate.empty()) fail("Input node requires non-empty action string");
    const auto& label = prop_value(node, "accessibility_label");
    if (label.empty()) fail("Input node requires non-empty accessibility_label string");
    if (candidate == action) found = true;
  }
  for (const auto& child : node.children) {
    validate_action_node(child, action, found);
  }
}

inline void validate_dispatch_action(const Node& root, const std::string& action) {
  if (action.empty()) fail("action id must be non-empty");
  bool found = false;
  validate_action_node(root, action, found);
  if (!found) fail("action not found: " + action);
}

inline void collect_actions(const Node& node, ActionSummary& summary) {
  if (!is_allowed_component(node.component)) fail("unsupported UI component: " + node.component);
  if (node.component == "Button") {
    const auto& candidate = prop_value(node, "action");
    if (candidate.empty()) fail("Button node requires non-empty action string");
    summary.add_button(candidate);
  } else if (node.component == "Input") {
    const auto& candidate = prop_value(node, "action");
    if (candidate.empty()) fail("Input node requires non-empty action string");
    const auto& label = prop_value(node, "accessibility_label");
    if (label.empty()) fail("Input node requires non-empty accessibility_label string");
    summary.add_input(candidate);
  }
  for (const auto& child : node.children) {
    collect_actions(child, summary);
  }
}

inline ActionSummary action_summary(const Node& root) {
  ActionSummary summary;
  collect_actions(root, summary);
  return summary;
}

inline void dispatch_action_summary(const ActionSummary& summary, const std::string& action) {
  if (action.empty()) fail("action id must be non-empty");
  for (const auto& candidate : summary.actions) {
    if (candidate == action) return;
  }
  fail("action not found: " + action);
}

inline std::string prop_or(const std::vector<Prop>& props, const std::string& name, std::string fallback = "") {
  for (const auto& prop : props) {
    if (prop.name == name) return prop.value;
  }
  return fallback;
}

inline int prop_int_or(const std::vector<Prop>& props, const std::string& name, int fallback) {
  const std::string value = prop_or(props, name);
  if (value.empty()) return fallback;
  return std::stoi(value);
}

inline void append_json_string(std::string& out, const std::string& value) {
  out.push_back('"');
  for (char ch : value) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  out.push_back('"');
}

inline void append_node_json(std::string& out, const Node& node) {
  out += "{\"schema\":\"nebula-ui.tree.v1\",\"component\":";
  append_json_string(out, node.component);
  out += ",\"props\":{";
  for (std::size_t i = 0; i < node.props.size(); ++i) {
    if (i > 0) out.push_back(',');
    append_json_string(out, node.props[i].name);
    out.push_back(':');
    if (node.props[i].string_value) {
      append_json_string(out, node.props[i].value);
    } else {
      out += node.props[i].value;
    }
  }
  out += "},\"children\":[";
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    if (i > 0) out.push_back(',');
    append_node_json(out, node.children[i]);
  }
  out += "]}";
}

inline std::string stringify_dashboard(const Node& root) {
  std::string out;
  out.reserve(256);
  append_node_json(out, root);
  return out;
}

inline int text_width(const std::string& text) {
  return std::max(8, static_cast<int>(text.size()) * 8);
}

inline FastPreviewTree fast_preview_tree(std::string title,
                                         std::string text,
                                         std::string input_value,
                                         std::string input_action,
                                         std::string input_accessibility_label,
                                         std::string button_text,
                                         std::string button_action) {
  if (input_action.empty()) fail("Input node requires non-empty action string");
  if (input_accessibility_label.empty()) fail("Input node requires non-empty accessibility_label string");
  if (button_action.empty()) fail("Button node requires non-empty action string");
  return FastPreviewTree{std::move(title),
                         std::move(text),
                         std::move(input_value),
                         std::move(input_action),
                         std::move(input_accessibility_label),
                         std::move(button_text),
                         std::move(button_action),
                         12};
}

inline FastActionIndex fast_action_index(const FastPreviewTree& tree) {
  if (tree.input_action.empty()) fail("Input node requires non-empty action string");
  if (tree.input_accessibility_label.empty()) fail("Input node requires non-empty accessibility_label string");
  if (tree.button_action.empty()) fail("Button node requires non-empty action string");
  return FastActionIndex{tree.button_action, tree.input_action, tree.button_action, tree.input_action, 2, 1, 1, 1};
}

inline bool fast_dispatch_action_index(const FastActionIndex& index, const std::string& action) {
  if (action.empty()) fail("action id must be non-empty");
  if (action == index.input_action || action == index.button_action) return true;
  fail("action not found: " + action);
  return false;
}

inline bool fast_dispatch_action_code(const FastActionIndex& index, int action_code) {
  if (action_code == index.input_action_code || action_code == index.button_action_code) return true;
  fail("action not found");
  return false;
}

inline FastRect fast_rect(int x, int y, int width, int height) {
  return FastRect{x, y, width, height};
}

inline FastPreviewLayout fast_preview_layout(const FastPreviewTree& tree) {
  const int text_y = 24;
  const int input_y = text_y + 20 + tree.spacing;
  const int button_y = input_y + 32 + tree.spacing;
  return FastPreviewLayout{480,
                           std::max(120, button_y + 32 + 24),
                           fast_rect(24, text_y, text_width(tree.text), 20),
                           fast_rect(24, input_y, std::max(160, text_width(tree.input_value) + 32), 32),
                           fast_rect(24, button_y, std::max(96, text_width(tree.button_text) + 32), 32),
                           tree.input_action,
                           tree.button_action,
                           1,
                           2,
                           5};
}

inline bool fast_contains(const FastRect& rect, int x, int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

inline std::string fast_hit_test(const FastPreviewLayout& layout, int x, int y) {
  if (fast_contains(layout.input_rect, x, y)) return layout.input_action;
  if (fast_contains(layout.button_rect, x, y)) return layout.button_action;
  return "";
}

inline int fast_hit_test_action_code(const FastPreviewLayout& layout, int x, int y) {
  if (fast_contains(layout.input_rect, x, y)) return layout.input_action_code;
  if (fast_contains(layout.button_rect, x, y)) return layout.button_action_code;
  return 0;
}

inline FastPatch fast_patch(const FastPreviewTree& old_tree, const FastPreviewTree& new_tree) {
  if (old_tree.input_value != new_tree.input_value) {
    return FastPatch{"props", "0.0.1", "value", new_tree.input_value};
  }
  if (old_tree.title != new_tree.title || old_tree.text != new_tree.text ||
      old_tree.input_action != new_tree.input_action ||
      old_tree.input_accessibility_label != new_tree.input_accessibility_label ||
      old_tree.button_text != new_tree.button_text || old_tree.button_action != new_tree.button_action) {
    return FastPatch{"replace-root", "0", "", ""};
  }
  return FastPatch{"noop", "", "", ""};
}

inline FastPatchPlan fast_patch_plan(const FastPreviewTree& old_tree, const FastPreviewTree& new_tree) {
  if (old_tree.input_value != new_tree.input_value) {
    return FastPatchPlan{1, 1, 1, new_tree.input_value};
  }
  if (old_tree.title != new_tree.title || old_tree.text != new_tree.text ||
      old_tree.input_action != new_tree.input_action ||
      old_tree.input_accessibility_label != new_tree.input_accessibility_label ||
      old_tree.button_text != new_tree.button_text || old_tree.button_action != new_tree.button_action) {
    return FastPatchPlan{2, 0, 0, ""};
  }
  return FastPatchPlan{0, 0, 0, ""};
}

inline int fast_patch_code(const FastPreviewTree& old_tree, const FastPreviewTree& new_tree) {
  if (old_tree.input_value != new_tree.input_value) {
    return 111;
  }
  if (old_tree.title != new_tree.title || old_tree.text != new_tree.text ||
      old_tree.input_action != new_tree.input_action ||
      old_tree.input_accessibility_label != new_tree.input_accessibility_label ||
      old_tree.button_text != new_tree.button_text || old_tree.button_action != new_tree.button_action) {
    return 200;
  }
  return 0;
}

inline int leaf_height(const std::string& component) {
  if (component == "Text") return 20;
  if (component == "Button" || component == "Input") return 32;
  if (component == "Spacer") return 16;
  return 24;
}

inline int leaf_width(const Node& node, int available_width) {
  if (node.component == "Text") return text_width(prop_or(node.props, "text"));
  if (node.component == "Button") return std::max(96, text_width(prop_or(node.props, "text")) + 32);
  if (node.component == "Input") return std::max(160, text_width(prop_or(node.props, "value")) + 32);
  return available_width;
}

inline std::string child_id(const std::string& parent_id, std::size_t index) {
  return parent_id + "." + std::to_string(index);
}

inline LayoutNode layout_node(const Node& node, std::string node_id, int x, int y, int width);

inline std::vector<LayoutNode> layout_column_children(const std::vector<Node>& children,
                                                      const std::string& parent_id,
                                                      int x,
                                                      int y,
                                                      int width,
                                                      int spacing,
                                                      int& height) {
  std::vector<LayoutNode> out;
  out.reserve(children.size());
  int cursor_y = y;
  height = 0;
  for (std::size_t i = 0; i < children.size(); ++i) {
    LayoutNode child = layout_node(children[i], child_id(parent_id, i), x, cursor_y, width);
    cursor_y += child.rect.height + spacing;
    height += child.rect.height + (i == 0 ? 0 : spacing);
    out.push_back(std::move(child));
  }
  return out;
}

inline std::vector<LayoutNode> layout_row_children(const std::vector<Node>& children,
                                                   const std::string& parent_id,
                                                   int x,
                                                   int y,
                                                   int width,
                                                   int spacing,
                                                   int& height) {
  std::vector<LayoutNode> out;
  out.reserve(children.size());
  if (children.empty()) {
    height = 0;
    return out;
  }
  const int child_width = (width - spacing * static_cast<int>(children.size() - 1U)) /
                          static_cast<int>(children.size());
  if (child_width <= 0) fail("row layout width is too small");
  int cursor_x = x;
  height = 0;
  for (std::size_t i = 0; i < children.size(); ++i) {
    LayoutNode child = layout_node(children[i], child_id(parent_id, i), cursor_x, y, child_width);
    cursor_x += child_width + spacing;
    height = std::max(height, child.rect.height);
    out.push_back(std::move(child));
  }
  return out;
}

inline LayoutNode layout_node(const Node& node, std::string node_id, int x, int y, int width) {
  if (node.component == "Window") {
    int child_height = 0;
    auto children = layout_column_children(node.children, node_id, 24, 24, 432, 12, child_height);
    return LayoutNode{std::move(node_id), node.component, Rect{0, 0, 480, std::max(120, child_height + 48)},
                      node.props, std::move(children)};
  }
  if (node.component == "Column") {
    int child_height = 0;
    auto children = layout_column_children(
        node.children, node_id, x, y, width, prop_int_or(node.props, "spacing", 0), child_height);
    return LayoutNode{std::move(node_id), node.component, Rect{x, y, width, child_height}, node.props,
                      std::move(children)};
  }
  if (node.component == "Row") {
    int child_height = 0;
    auto children =
        layout_row_children(node.children, node_id, x, y, width, prop_int_or(node.props, "spacing", 0), child_height);
    return LayoutNode{std::move(node_id), node.component, Rect{x, y, width, child_height}, node.props,
                      std::move(children)};
  }
  return LayoutNode{std::move(node_id), node.component, Rect{x, y, leaf_width(node, width), leaf_height(node.component)},
                    node.props, {}};
}

inline LayoutNode layout_dashboard() {
  return layout_node(dashboard_tree(), "0", 0, 0, 480);
}

inline std::string command_kind(const std::string& component) {
  if (component == "Text") return "text";
  if (component == "Button" || component == "Input") return "hit-area";
  return "rect";
}

inline std::string command_text(const LayoutNode& node) {
  if (node.component == "Text") return prop_or(node.props, "text");
  if (node.component == "Button") return prop_or(node.props, "text");
  if (node.component == "Input") return prop_or(node.props, "value");
  if (node.component == "Window") return prop_or(node.props, "title");
  return "";
}

inline void append_render_commands(const LayoutNode& node, std::vector<RenderCommand>& commands) {
  commands.push_back(RenderCommand{command_kind(node.component), node.node_id, node.component, node.rect,
                                   command_text(node), prop_or(node.props, "action")});
  for (const auto& child : node.children) {
    append_render_commands(child, commands);
  }
}

inline std::vector<RenderCommand> render_list(const LayoutNode& root) {
  std::vector<RenderCommand> commands;
  commands.reserve(8);
  append_render_commands(root, commands);
  return commands;
}

inline bool contains(const Rect& rect, int x, int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

inline std::string hit_test(const LayoutNode& node, int x, int y) {
  if (!contains(node.rect, x, y)) return "";
  for (const auto& child : node.children) {
    std::string action = hit_test(child, x, y);
    if (!action.empty()) return action;
  }
  if (node.component == "Button" || node.component == "Input") return prop_or(node.props, "action");
  return "";
}

inline InputLookup first_input(const Node& node, const std::string& node_id) {
  if (node.component == "Input") return InputLookup{true, node_id, prop_or(node.props, "value")};
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    auto found = first_input(node.children[i], child_id(node_id, i));
    if (found.found) return found;
  }
  return InputLookup{};
}

inline std::string patch_kind(const Node& old_tree, const Node& new_tree) {
  const InputLookup old_input = first_input(old_tree, "0");
  const InputLookup new_input = first_input(new_tree, "0");
  if (old_input.found && new_input.found && old_input.node_id == new_input.node_id &&
      old_input.value != new_input.value) {
    return "replace-prop:" + new_input.node_id + ":value=" + new_input.value;
  }
  return stringify_dashboard(old_tree) == stringify_dashboard(new_tree) ? "noop" : "replace-root";
}

inline std::uint64_t run_action_roundtrip() {
  const FastPreviewTree tree = fast_preview_tree("Local Ops",
                                                 "Local Ops",
                                                 "targets",
                                                 "targets.filter",
                                                 "Filter targets",
                                                 "Refresh",
                                                 "targets.refresh");
  const FastActionIndex index = fast_action_index(tree);
  expect_eq(index.first_button_action, "targets.refresh", "ui action");
  expect_eq(index.first_input_action, "targets.filter", "ui input action");
  if (!fast_dispatch_action_index(index, index.first_input_action)) fail("action dispatch failed");
  return checksum_text(index.first_button_action + index.first_input_action);
}

inline std::uint64_t run_snapshot_render() {
  const std::string rendered = stringify_dashboard(dashboard_tree());
  const std::string expected =
      "{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Window\",\"props\":{\"title\":\"Local Ops\"},"
      "\"children\":[{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Column\",\"props\":{\"spacing\":12},"
      "\"children\":[{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Text\",\"props\":{\"text\":\"Local Ops\"},"
      "\"children\":[]},{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Input\",\"props\":{\"value\":\"targets\","
      "\"action\":\"targets.filter\",\"accessibility_label\":\"Filter targets\"},\"children\":[]},"
      "{\"schema\":\"nebula-ui.tree.v1\",\"component\":\"Button\",\"props\":{\"text\":\"Refresh\","
      "\"action\":\"targets.refresh\"},\"children\":[]}]}]}";
  expect_eq(rendered, expected, "ui snapshot");
  return checksum_text(rendered);
}

inline std::uint64_t run_layout_pass() {
  const FastPreviewTree tree{"Local Ops", "Local Ops", "targets", "targets.filter",
                             "Filter targets", "Refresh", "targets.refresh", 12};
  const auto layout = fast_preview_layout(tree);
  expect_eq(layout.width, 480, "ui layout width");
  expect_eq(layout.command_count, 5, "ui layout command count");
  return checksum_text(std::to_string(layout.width) + std::to_string(layout.height));
}

inline std::uint64_t run_render_list_build() {
  const FastPreviewTree tree{"Local Ops", "Local Ops", "targets", "targets.filter",
                             "Filter targets", "Refresh", "targets.refresh", 12};
  const auto layout = fast_preview_layout(tree);
  expect_eq(layout.command_count, 5, "ui render command count");
  return checksum_text(std::to_string(layout.command_count) + layout.input_action + layout.button_action);
}

inline std::uint64_t run_hit_test_dispatch() {
  const FastPreviewTree tree{"Local Ops", "Local Ops", "targets", "targets.filter",
                             "Filter targets", "Refresh", "targets.refresh", 12};
  const auto layout = fast_preview_layout(tree);
  const int action_code = fast_hit_test_action_code(layout, 30, 60);
  expect_eq(action_code, 1, "ui hit-test action code");
  return static_cast<std::uint64_t>(action_code);
}

inline std::uint64_t run_patch_apply() {
  const FastPreviewTree old_tree{"Local Ops", "Local Ops", "targets", "targets.filter",
                                 "Filter targets", "Refresh", "targets.refresh", 12};
  const FastPreviewTree new_tree{"Local Ops", "Local Ops", "prod", "targets.filter",
                                 "Filter targets", "Refresh", "targets.refresh", 12};
  const int patch_code = fast_patch_code(old_tree, new_tree);
  expect_eq(patch_code, 111, "ui patch code");
  return static_cast<std::uint64_t>(patch_code);
}

inline std::uint64_t run_gpu_submit_smoke() {
  const FastPreviewTree tree{"Local Ops", "Local Ops", "targets", "targets.filter",
                             "Filter targets", "Refresh", "targets.refresh", 12};
  const auto layout = fast_preview_layout(tree);
  expect_eq(layout.command_count, 5, "ui gpu submit count");
  return checksum_text(std::to_string(layout.command_count) + layout.input_action + layout.button_action);
}

}  // namespace app_platform_cpp::ui
