#pragma once

#include "bench.hpp"

#include <string>

namespace app_platform_cpp::thin_host {

struct State {
  int counter;
  std::string last_command;
  int revision;
};

struct Step {
  State state;
  bool should_exit;
  std::string event_kind;
  std::string correlation_id;
};

struct CommandEnvelope {
  std::string kind;
  std::string correlation_id;
  int state_revision;
};

struct CommandCodeEnvelope {
  int kind_code;
  std::string correlation_id;
  int state_revision;
};

inline std::string bool_text(bool value) {
  return value ? "true" : "false";
}

inline std::string counter_status(int counter) {
  if (counter <= 0) {
    return "idle";
  }
  if (counter == 1) {
    return "active";
  }
  return "busy";
}

inline State initial_state() {
  return State{0, "boot", 0};
}

inline std::string counter_payload(const State& state) {
  return "{\"counter\":" + std::to_string(state.counter) +
         ",\"can_decrement\":" + bool_text(state.counter > 0) +
         ",\"status\":\"" + counter_status(state.counter) +
         "\",\"last_command\":\"" + state.last_command + "\"}";
}

inline std::string encode_command(const std::string& kind,
                                  const std::string& correlation_id,
                                  int state_revision) {
  return "{\"schema\":\"thin-host-bridge.command.v1\",\"kind\":\"" + kind +
         "\",\"correlation_id\":\"" + correlation_id +
         "\",\"state_revision\":" + std::to_string(state_revision) + "}";
}

inline std::string encode_event(const std::string& kind,
                                const std::string& payload,
                                bool terminal,
                                const std::string& correlation_id,
                                int state_revision) {
  return "{\"schema\":\"thin-host-bridge.event.v1\",\"kind\":\"" + kind +
         "\",\"payload\":" + payload +
         ",\"terminal\":" + bool_text(terminal) +
         ",\"correlation_id\":\"" + correlation_id +
         "\",\"state_revision\":" + std::to_string(state_revision) + "}";
}

inline std::string encode_snapshot(const std::string& screen,
                                   const std::string& payload,
                                   int state_revision) {
  return "{\"schema\":\"thin-host-bridge.snapshot.v1\",\"screen\":\"" + screen +
         "\",\"payload\":" + payload +
         ",\"state_revision\":" + std::to_string(state_revision) + "}";
}

inline std::string string_field(const std::string& text, const std::string& key) {
  const std::string marker = "\"" + key + "\":\"";
  const std::size_t start = text.find(marker);
  if (start == std::string::npos) {
    fail("missing string field: " + key);
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t value_end = text.find('"', value_start);
  if (value_end == std::string::npos) {
    fail("unterminated string field: " + key);
  }
  return text.substr(value_start, value_end - value_start);
}

inline int int_field(const std::string& text, const std::string& key) {
  const std::string marker = "\"" + key + "\":";
  const std::size_t start = text.find(marker);
  if (start == std::string::npos) {
    fail("missing int field: " + key);
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t value_end = text.find_first_not_of("-0123456789", value_start);
  return std::stoi(text.substr(value_start, value_end - value_start));
}

inline Step reduce_command(const State& state,
                           const std::string& kind,
                           const std::string& correlation_id) {
  if (kind == "increment") {
    return Step{State{state.counter + 1, "increment", state.revision + 1},
                false,
                "increment_applied",
                correlation_id};
  }
  if (kind == "decrement") {
    if (state.counter <= 0) {
      fail("invalid counter transition: cannot decrement below zero");
    }
    return Step{State{state.counter - 1, "decrement", state.revision + 1},
                false,
                "decrement_applied",
                correlation_id};
  }
  if (kind == "quit") {
    return Step{State{state.counter, "quit", state.revision + 1}, true, "quit_requested", correlation_id};
  }
  fail("unknown thin-host command kind");
}

inline Step apply_command_text(const State& state, const std::string& command_text) {
  expect_eq(string_field(command_text, "schema"), "thin-host-bridge.command.v1", "command schema");
  const std::string kind = string_field(command_text, "kind");
  const std::string correlation_id = string_field(command_text, "correlation_id");
  const int revision = int_field(command_text, "state_revision");
  expect_eq(revision, state.revision, "command state_revision");
  return reduce_command(state, kind, correlation_id);
}

inline CommandEnvelope command_at_revision(std::string kind, std::string correlation_id, int state_revision) {
  return CommandEnvelope{std::move(kind), std::move(correlation_id), state_revision};
}

inline int increment_command_code() {
  return 1;
}

inline int decrement_command_code() {
  return 2;
}

inline int quit_command_code() {
  return 3;
}

inline CommandCodeEnvelope command_code_at_revision(int kind_code, std::string correlation_id, int state_revision) {
  return CommandCodeEnvelope{kind_code, std::move(correlation_id), state_revision};
}

inline Step apply_command_envelope(const State& state, const CommandEnvelope& command) {
  expect_eq(command.state_revision, state.revision, "command state_revision");
  return reduce_command(state, command.kind, command.correlation_id);
}

inline Step reduce_command_code(const State& state, int kind_code, const std::string& correlation_id) {
  if (kind_code == increment_command_code()) {
    return Step{State{state.counter + 1, "increment", state.revision + 1},
                false,
                "increment_applied",
                correlation_id};
  }
  if (kind_code == decrement_command_code()) {
    if (state.counter <= 0) {
      fail("invalid counter transition: cannot decrement below zero");
    }
    return Step{State{state.counter - 1, "decrement", state.revision + 1},
                false,
                "decrement_applied",
                correlation_id};
  }
  if (kind_code == quit_command_code()) {
    return Step{State{state.counter, "quit", state.revision + 1}, true, "quit_requested", correlation_id};
  }
  fail("unknown thin-host command kind");
}

inline Step apply_command_code_envelope(const State& state, const CommandCodeEnvelope& command) {
  expect_eq(command.state_revision, state.revision, "command state_revision");
  return reduce_command_code(state, command.kind_code, command.correlation_id);
}

inline State reduce_command_code_state(const State& state, int kind_code) {
  if (kind_code == increment_command_code()) {
    return State{state.counter + 1, "increment", state.revision + 1};
  }
  if (kind_code == decrement_command_code()) {
    if (state.counter <= 0) {
      fail("invalid counter transition: cannot decrement below zero");
    }
    return State{state.counter - 1, "decrement", state.revision + 1};
  }
  if (kind_code == quit_command_code()) {
    return State{state.counter, "quit", state.revision + 1};
  }
  fail("unknown thin-host command kind");
}

inline State apply_command_code_state_envelope(const State& state, const CommandCodeEnvelope& command) {
  expect_eq(command.state_revision, state.revision, "command state_revision");
  return reduce_command_code_state(state, command.kind_code);
}

inline std::string event_text(const Step& step) {
  return encode_event(step.event_kind,
                      counter_payload(step.state),
                      step.should_exit,
                      step.correlation_id,
                      step.state.revision);
}

inline std::string snapshot_text(const State& state) {
  return encode_snapshot("counter", counter_payload(state), state.revision);
}

inline std::uint64_t run_increment_roundtrip() {
  const State state = initial_state();
  const std::string command = encode_command("increment", "cmd-increment", state.revision);
  const Step step = apply_command_text(state, command);
  const std::string event = event_text(step);
  const std::string snapshot = snapshot_text(step.state);
  expect_eq(event,
            "{\"schema\":\"thin-host-bridge.event.v1\",\"kind\":\"increment_applied\","
            "\"payload\":{\"counter\":1,\"can_decrement\":true,\"status\":\"active\","
            "\"last_command\":\"increment\"},\"terminal\":false,\"correlation_id\":\"cmd-increment\","
            "\"state_revision\":1}",
            "increment event");
  expect_eq(snapshot,
            "{\"schema\":\"thin-host-bridge.snapshot.v1\",\"screen\":\"counter\","
            "\"payload\":{\"counter\":1,\"can_decrement\":true,\"status\":\"active\","
            "\"last_command\":\"increment\"},\"state_revision\":1}",
            "increment snapshot");
  return checksum_text(event) ^ checksum_text(snapshot);
}

inline std::uint64_t run_state_sync_tape() {
  State state = initial_state();
  state = apply_command_code_state_envelope(state, command_code_at_revision(increment_command_code(), "sync-1", state.revision));
  state = apply_command_code_state_envelope(state, command_code_at_revision(increment_command_code(), "sync-2", state.revision));
  state = apply_command_code_state_envelope(state, command_code_at_revision(decrement_command_code(), "sync-3", state.revision));
  const std::string snapshot = snapshot_text(state);
  expect_eq(snapshot,
            "{\"schema\":\"thin-host-bridge.snapshot.v1\",\"screen\":\"counter\","
            "\"payload\":{\"counter\":1,\"can_decrement\":true,\"status\":\"active\","
            "\"last_command\":\"decrement\"},\"state_revision\":3}",
            "state sync snapshot");
  return checksum_text(snapshot);
}

}  // namespace app_platform_cpp::thin_host
