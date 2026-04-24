#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string command_text(const char* kind) {
  return std::string("{\"schema\":\"thin-host-bridge.command.v1\",\"kind\":\"") + kind + "\"}";
}

std::vector<std::string> g_commands = {
    command_text("increment"),
    command_text("increment"),
    command_text("decrement"),
    command_text("increment"),
    command_text("quit"),
};
std::size_t g_index = 0;

} // namespace

void host_shell_begin() {
  std::cout << "host-shell-begin\n";
}

bool host_shell_open() {
  return g_index < g_commands.size();
}

std::string host_shell_next_command_text() {
  if (g_index >= g_commands.size()) return command_text("quit");
  const std::string command = g_commands[g_index];
  g_index += 1;
  return command;
}

void host_shell_publish_event(std::string event) {
  std::cout << "event:" << event << "\n";
}

void host_shell_render_snapshot(std::string snapshot) {
  std::cout << "render:" << snapshot << "\n";
}

void host_shell_end() {
  std::cout << "host-shell-end\n";
}
