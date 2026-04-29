#include "../../official/nebula-ui/adapters/common/ui_tree_preview.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string quoted(const char* value) {
  return std::string("\"") + value + "\"";
}

std::string command_text(const char* kind,
                         const char* correlation_id,
                         int state_revision,
                         const std::string& payload) {
  return std::string("{\"schema\":\"thin-host-bridge.command.v1\",\"kind\":") +
         quoted(kind) + ",\"correlation_id\":" + quoted(correlation_id) +
         ",\"state_revision\":" + std::to_string(state_revision) +
         ",\"payload\":" + payload + "}";
}

std::string bare_command_text(const char* kind, const char* correlation_id, int state_revision) {
  return command_text(kind, correlation_id, state_revision, "{}");
}

std::string file_payload() {
  return "{\"path\":\"file:///fixture/public-domain/sample.mp4\","
         "\"source_hash\":\"sha256-public-domain-file\"}";
}

std::string torrent_payload(const char* policy, bool transport_available) {
  return std::string("{\"source_uri\":\"magnet:?xt=urn:btih:PUBLICDOMAINFIXTURE\",") +
         "\"source_hash\":\"sha256-public-domain-torrent\"," +
         "\"policy\":" + quoted(policy) + "," +
         "\"transport_available\":" + (transport_available ? "true" : "false") + "}";
}

std::string value_payload(const char* value) {
  return std::string("{\"value\":") + quoted(value) + "}";
}

std::vector<std::string> rejection_commands() {
  return {
      "{\"schema\":\"thin-host-bridge.command.v0\",\"kind\":\"library.import_file\","
      "\"correlation_id\":\"media-bad-schema\",\"state_revision\":0}",
      bare_command_text("library.unknown", "media-unknown", 0),
      command_text("library.import_file", "media-missing-path", 0,
                   "{\"source_hash\":\"sha256-missing-path\"}"),
      bare_command_text("quit", "media-reject-quit", 0),
  };
}

std::vector<std::string> default_commands() {
  return {
      command_text("library.import_file", "media-1", 0, file_payload()),
      command_text("playback.set_audio_quality", "media-2", 1, value_payload("lossless")),
      command_text("library.import_torrent", "media-illegal", 2,
                   torrent_payload("copyright-unknown", true)),
      command_text("library.import_torrent", "media-sidecar-down", 2,
                   torrent_payload("public-domain", false)),
      command_text("library.import_torrent", "media-3", 2,
                   torrent_payload("public-domain", true)),
      command_text("playback.set_video_quality", "media-stale", 2, value_payload("4k")),
      command_text("playback.set_video_quality", "media-4", 3, value_payload("4k")),
      command_text("playback.set_bitrate_policy", "media-5", 4, value_payload("adaptive-high")),
      bare_command_text("download.pause", "media-6", 5),
      bare_command_text("download.resume", "media-7", 6),
      bare_command_text("download.cancel", "media-8", 7),
      bare_command_text("quit", "media-9", 8),
  };
}

std::vector<std::string> make_commands() {
  const char* mode = std::getenv("NEBULA_MEDIA_PLAYER_COMMAND_MODE");
  if (mode != nullptr && std::string(mode) == "rejections") {
    return rejection_commands();
  }
  if (mode != nullptr && std::string(mode) == "host_close") {
    return {
        command_text("library.import_file", "media-close-1", 0, file_payload()),
        command_text("playback.set_audio_quality", "media-close-2", 1, value_payload("studio")),
    };
  }
  return default_commands();
}

std::vector<std::string> g_commands = make_commands();
std::size_t g_index = 0;
int g_render_count = 0;

nebula::ui::adapter_preview::TreeSummary decode_tree_or_exit(const std::string& tree) {
  auto parsed = nebula::rt::json_parse(tree);
  if (nebula::rt::result_is_err(parsed)) {
    std::cerr << "thin-host-media-player-error: " << nebula::rt::result_err_ref(parsed) << "\n";
    std::exit(2);
  }

  nebula::ui::adapter_preview::TreeSummary summary;
  std::string error;
  if (!nebula::ui::adapter_preview::summarize_node(nebula::rt::result_ok_ref(parsed), summary, &error)) {
    std::cerr << "thin-host-media-player-error: " << error << "\n";
    std::exit(2);
  }
  return summary;
}

void dispatch_or_exit(const nebula::ui::adapter_preview::TreeSummary& summary,
                      const std::string& action) {
  const auto result = nebula::ui::adapter_preview::dispatch_action(summary, action);
  if (!result.ok) {
    std::cerr << "thin-host-media-player-error: " << result.message << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_dispatch_result(std::cout, result);
}

}  // namespace

void host_player_begin() {
  std::cout << "thin-host-media-player-begin\n";
  std::cout << "thin-host-media-player-boundary=nebula-first-app-core\n";
  std::cout << "thin-host-media-player-telemetry-session=thin-host-media-player-session-0001\n";
  const auto lifecycle = nebula::ui::adapter_preview::host_shell_lifecycle_summary(false);
  std::string error;
  if (!nebula::ui::adapter_preview::validate_lifecycle_order(lifecycle, &error)) {
    std::cerr << "thin-host-media-player-error: " << error << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_lifecycle_summary(std::cout, lifecycle);
}

bool host_player_open() {
  return g_index < g_commands.size();
}

std::string host_player_next_command_text() {
  if (g_index >= g_commands.size()) return bare_command_text("quit", "media-fallback", 0);
  const std::string command = g_commands[g_index];
  g_index += 1;
  return command;
}

void host_player_publish_event(std::string event) {
  std::cout << "event:" << event << "\n";
}

void host_player_publish_snapshot(std::string snapshot) {
  std::cout << "snapshot:" << snapshot << "\n";
}

void host_player_render_tree(std::string tree) {
  const auto summary = decode_tree_or_exit(tree);
  std::cout << "render:" << g_render_count << "\n";
  nebula::ui::adapter_preview::append_smoke_summary(std::cout, summary);
  dispatch_or_exit(summary, "library.import_file");
  dispatch_or_exit(summary, "playback.set_audio_quality");
  ++g_render_count;
}

void host_player_end(std::string correlation_id, long long state_revision) {
  const auto lifecycle = nebula::ui::adapter_preview::host_shell_lifecycle_summary(true);
  std::string error;
  if (!nebula::ui::adapter_preview::validate_lifecycle_order(lifecycle, &error)) {
    std::cerr << "thin-host-media-player-error: " << error << "\n";
    std::exit(2);
  }
  nebula::ui::adapter_preview::append_lifecycle_summary(std::cout, lifecycle);
  std::cout << "thin-host-media-player-crash-report-schema=nebula.gui.preview.crash.v1\n";
  std::cout << "thin-host-media-player-crash-correlation-id=" << correlation_id << "\n";
  std::cout << "thin-host-media-player-crash-state-revision=" << state_revision << "\n";
  std::cout << "thin-host-media-player-recovery-log=logs/thin-host-media-player.ndjson\n";
  std::cout << "thin-host-media-player-recovery-policy=manual-preview\n";
  std::cout << "thin-host-media-player-end renders=" << g_render_count << "\n";
}
