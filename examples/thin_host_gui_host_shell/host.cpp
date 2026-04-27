#include "../../official/nebula-ui/adapters/common/ui_tree_preview.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string command_text(const char* kind, const char* correlation_id, int state_revision) {
  return std::string("{\"schema\":\"thin-host-bridge.command.v1\",\"kind\":\"") + kind +
         "\",\"correlation_id\":\"" + correlation_id + "\",\"state_revision\":" +
         std::to_string(state_revision) + "}";
}

std::vector<std::string> g_commands = {
    command_text("targets.refresh", "gui-1", 0),
    command_text("targets.filter", "gui-2", 1),
    command_text("quit", "gui-3", 2),
};
constexpr const char* kTelemetrySessionId = "thin-host-gui-preview-session-0001";
std::size_t g_index = 0;
int g_render_count = 0;

nebula::ui::adapter_preview::TreeSummary decode_tree_or_exit(const std::string& tree) {
  auto parsed = nebula::rt::json_parse(tree);
  if (nebula::rt::result_is_err(parsed)) {
    std::cerr << "thin-host-gui-shell-error: " << nebula::rt::result_err_ref(parsed) << "\n";
    std::exit(2);
  }

  nebula::ui::adapter_preview::TreeSummary summary;
  std::string error;
  if (!nebula::ui::adapter_preview::summarize_node(nebula::rt::result_ok_ref(parsed), summary, &error)) {
    std::cerr << "thin-host-gui-shell-error: " << error << "\n";
    std::exit(2);
  }
  return summary;
}

void dispatch_or_exit(const nebula::ui::adapter_preview::TreeSummary& summary, const std::string& action) {
  const auto result = nebula::ui::adapter_preview::dispatch_action(summary, action);
  if (!result.ok) {
    std::cerr << "thin-host-gui-shell-error: " << result.message << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_dispatch_result(std::cout, result);
}

}  // namespace

void host_shell_begin() {
  std::cout << "thin-host-gui-shell-begin\n";
  std::cout << "thin-host-gui-shell-telemetry-session=" << kTelemetrySessionId << "\n";
  const auto lifecycle = nebula::ui::adapter_preview::host_shell_lifecycle_summary(false);
  std::string error;
  if (!nebula::ui::adapter_preview::validate_lifecycle_order(lifecycle, &error)) {
    std::cerr << "thin-host-gui-shell-error: " << error << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_lifecycle_summary(std::cout, lifecycle);
}

bool host_shell_open() {
  return g_index < g_commands.size();
}

std::string host_shell_next_command_text() {
  if (g_index >= g_commands.size()) return command_text("quit", "gui-fallback", 0);
  const std::string command = g_commands[g_index];
  g_index += 1;
  return command;
}

void host_shell_publish_event(std::string event) {
  std::cout << "event:" << event << "\n";
}

void host_shell_publish_snapshot(std::string snapshot) {
  std::cout << "snapshot:" << snapshot << "\n";
}

void host_shell_render_tree(std::string tree) {
  const auto summary = decode_tree_or_exit(tree);
  std::cout << "render:" << g_render_count << "\n";
  nebula::ui::adapter_preview::append_smoke_summary(std::cout, summary);
  dispatch_or_exit(summary, "targets.refresh");
  dispatch_or_exit(summary, "targets.filter");
  ++g_render_count;
}

void host_shell_end() {
  const auto lifecycle = nebula::ui::adapter_preview::host_shell_lifecycle_summary(true);
  std::string error;
  if (!nebula::ui::adapter_preview::validate_lifecycle_order(lifecycle, &error)) {
    std::cerr << "thin-host-gui-shell-error: " << error << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_lifecycle_summary(std::cout, lifecycle);
  std::cout << "thin-host-gui-shell-crash-report-schema=nebula.gui.preview.crash.v1\n";
  std::cout << "thin-host-gui-shell-crash-correlation-id=gui-3\n";
  std::cout << "thin-host-gui-shell-crash-state-revision=3\n";
  std::cout << "thin-host-gui-shell-end renders=" << g_render_count << "\n";
}
