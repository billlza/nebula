#include "runtime/nebula_runtime.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

std::string __nebula_thin_host_encode_event_payload_text(std::string kind,
                                                         std::string payload_text,
                                                         bool terminal,
                                                         std::string correlation_id,
                                                         std::int64_t state_revision);
std::string __nebula_thin_host_encode_snapshot_payload_text(std::string screen,
                                                            std::string payload_text,
                                                            std::int64_t state_revision);

namespace {

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

void append_json_int_field(std::string& out, std::string_view key, std::int64_t value) {
  append_json_string(out, key);
  out.push_back(':');
  out += std::to_string(value);
}

void append_thin_host_schema(std::string& out, std::string_view schema) {
  append_json_string_field(out, "schema", schema);
}

std::string stringify_json_payload(nebula::rt::JsonValue payload) {
  return nebula::rt::json_stringify(std::move(payload));
}

struct FieldSpan {
  std::size_t key_start = 0;
  std::size_t key_end = 0;
  std::size_t value_start = 0;
  std::size_t value_end = 0;
};

struct GeneratedCommandView {
  FieldSpan schema;
  FieldSpan kind;
  FieldSpan correlation_id;
  FieldSpan state_revision;
  std::int64_t revision_value = 0;
};

struct GeneratedCommandCache {
  std::string text;
  GeneratedCommandView view;
  bool valid = false;
};

bool append_literal(std::string_view text, std::size_t& pos, std::string_view literal) {
  if (text.substr(pos, literal.size()) != literal) return false;
  pos += literal.size();
  return true;
}

std::optional<FieldSpan> consume_unescaped_string_field(std::string_view text,
                                                        std::size_t& pos,
                                                        std::string_view key) {
  if (pos >= text.size() || text[pos] != '"') return std::nullopt;
  const std::size_t key_start = pos + 1;
  if (text.substr(key_start, key.size()) != key) return std::nullopt;
  const std::size_t key_end = key_start + key.size();
  if (key_end >= text.size() || text[key_end] != '"' || key_end + 1 >= text.size() || text[key_end + 1] != ':') {
    return std::nullopt;
  }
  pos = key_end + 2;
  if (pos >= text.size() || text[pos] != '"') return std::nullopt;
  const std::size_t value_start = pos;
  ++pos;
  while (pos < text.size()) {
    const char ch = text[pos];
    if (ch == '\\') return std::nullopt;
    if (ch == '"') {
      const std::size_t value_end = pos + 1;
      pos = value_end;
      return FieldSpan{key_start, key_end, value_start, value_end};
    }
    if (static_cast<unsigned char>(ch) < 0x20) return std::nullopt;
    ++pos;
  }
  return std::nullopt;
}

std::optional<FieldSpan> consume_int_field(std::string_view text,
                                           std::size_t& pos,
                                           std::string_view key,
                                           std::int64_t& parsed) {
  if (pos >= text.size() || text[pos] != '"') return std::nullopt;
  const std::size_t key_start = pos + 1;
  if (text.substr(key_start, key.size()) != key) return std::nullopt;
  const std::size_t key_end = key_start + key.size();
  if (key_end >= text.size() || text[key_end] != '"' || key_end + 1 >= text.size() || text[key_end + 1] != ':') {
    return std::nullopt;
  }
  pos = key_end + 2;
  const std::size_t value_start = pos;
  if (pos < text.size() && text[pos] == '-') {
    ++pos;
  }
  if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return std::nullopt;
  if (text[pos] == '0') {
    ++pos;
    if (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') return std::nullopt;
  } else {
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      ++pos;
    }
  }
  const char* begin = text.data() + value_start;
  const char* end = text.data() + pos;
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return FieldSpan{key_start, key_end, value_start, pos};
}

std::optional<GeneratedCommandView> parse_generated_command_view(std::string_view text) {
  std::size_t pos = 0;
  if (!append_literal(text, pos, "{")) return std::nullopt;
  auto schema = consume_unescaped_string_field(text, pos, "schema");
  if (!schema.has_value()) return std::nullopt;
  if (text.substr(schema->value_start, schema->value_end - schema->value_start) != "\"thin-host-bridge.command.v1\"") {
    return std::nullopt;
  }
  if (!append_literal(text, pos, ",")) return std::nullopt;
  auto kind = consume_unescaped_string_field(text, pos, "kind");
  if (!kind.has_value()) return std::nullopt;
  if (!append_literal(text, pos, ",")) return std::nullopt;
  auto correlation_id = consume_unescaped_string_field(text, pos, "correlation_id");
  if (!correlation_id.has_value()) return std::nullopt;
  if (!append_literal(text, pos, ",")) return std::nullopt;
  std::int64_t revision = 0;
  auto state_revision = consume_int_field(text, pos, "state_revision", revision);
  if (!state_revision.has_value()) return std::nullopt;
  if (!append_literal(text, pos, "}")) return std::nullopt;
  if (pos != text.size()) return std::nullopt;
  return GeneratedCommandView{*schema, *kind, *correlation_id, *state_revision, revision};
}

const GeneratedCommandCache* cached_generated_command(std::string_view text) {
  thread_local GeneratedCommandCache cache;
  if (cache.valid && cache.text == text) return &cache;
  auto parsed = parse_generated_command_view(text);
  if (!parsed.has_value()) {
    cache.valid = false;
    cache.text.clear();
    return nullptr;
  }
  cache.text = std::string(text);
  cache.view = *parsed;
  cache.valid = true;
  return &cache;
}

nebula::rt::JsonObjectField string_field(const FieldSpan& span) {
  nebula::rt::JsonObjectField field;
  field.key_start = span.key_start;
  field.key_end = span.key_end;
  field.key_needs_decode = false;
  field.value.kind = nebula::rt::JsonValueKind::String;
  field.value.raw_start = span.value_start;
  field.value.raw_end = span.value_end;
  field.value.string_needs_decode = false;
  return field;
}

nebula::rt::JsonObjectField int_field(const FieldSpan& span, std::int64_t value) {
  nebula::rt::JsonObjectField field;
  field.key_start = span.key_start;
  field.key_end = span.key_end;
  field.key_needs_decode = false;
  field.value.kind = nebula::rt::JsonValueKind::Int;
  field.value.raw_start = span.value_start;
  field.value.raw_end = span.value_end;
  field.value.int_value = value;
  return field;
}

}  // namespace

std::string __nebula_thin_host_encode_command_text(std::string kind,
                                                   std::string correlation_id,
                                                   std::int64_t state_revision) {
  const std::string revision = std::to_string(state_revision);
  std::string out;
  out.reserve(93 + kind.size() + correlation_id.size() + revision.size());
  out.push_back('{');
  append_thin_host_schema(out, "thin-host-bridge.command.v1");
  out.push_back(',');
  append_json_string_field(out, "kind", kind);
  out.push_back(',');
  append_json_string_field(out, "correlation_id", correlation_id);
  out.push_back(',');
  append_json_int_field(out, "state_revision", state_revision);
  out.push_back('}');
  return out;
}

std::string __nebula_thin_host_encode_event_text(std::string kind,
                                                 nebula::rt::JsonValue payload,
                                                 bool terminal,
                                                 std::string correlation_id,
                                                 std::int64_t state_revision) {
  return __nebula_thin_host_encode_event_payload_text(kind,
                                                      stringify_json_payload(std::move(payload)),
                                                      terminal,
                                                      correlation_id,
                                                      state_revision);
}

std::string __nebula_thin_host_encode_event_payload_text(std::string kind,
                                                         std::string payload_text,
                                                         bool terminal,
                                                         std::string correlation_id,
                                                         std::int64_t state_revision) {
  const std::string revision = std::to_string(state_revision);
  std::string out;
  out.reserve(119 + kind.size() + payload_text.size() + correlation_id.size() + revision.size());
  out.push_back('{');
  append_thin_host_schema(out, "thin-host-bridge.event.v1");
  out.push_back(',');
  append_json_string_field(out, "kind", kind);
  out += ",\"payload\":";
  out += payload_text;
  out += ",\"terminal\":";
  out += terminal ? "true" : "false";
  out.push_back(',');
  append_json_string_field(out, "correlation_id", correlation_id);
  out.push_back(',');
  append_json_int_field(out, "state_revision", state_revision);
  out.push_back('}');
  return out;
}

std::string __nebula_thin_host_encode_snapshot_text(std::string screen,
                                                    nebula::rt::JsonValue payload,
                                                    std::int64_t state_revision) {
  return __nebula_thin_host_encode_snapshot_payload_text(screen,
                                                         stringify_json_payload(std::move(payload)),
                                                         state_revision);
}

std::string __nebula_thin_host_encode_snapshot_payload_text(std::string screen,
                                                            std::string payload_text,
                                                            std::int64_t state_revision) {
  const std::string revision = std::to_string(state_revision);
  std::string out;
  out.reserve(94 + screen.size() + payload_text.size() + revision.size());
  out.push_back('{');
  append_thin_host_schema(out, "thin-host-bridge.snapshot.v1");
  out.push_back(',');
  append_json_string_field(out, "screen", screen);
  out += ",\"payload\":";
  out += payload_text;
  out.push_back(',');
  append_json_int_field(out, "state_revision", state_revision);
  out.push_back('}');
  return out;
}

nebula::rt::Result<nebula::rt::JsonValue, std::string>
__nebula_thin_host_parse_generated_command_text(std::string text) {
  auto parsed = parse_generated_command_view(text);
  if (!parsed.has_value()) {
    return nebula::rt::err_result<nebula::rt::JsonValue>("thin-host command fast path miss");
  }

  nebula::rt::JsonObjectIndex object_fields;
  object_fields.push_back(string_field(parsed->schema));
  object_fields.push_back(string_field(parsed->kind));
  object_fields.push_back(string_field(parsed->correlation_id));
  object_fields.push_back(int_field(parsed->state_revision, parsed->revision_value));

  nebula::rt::JsonValueView view;
  view.kind = nebula::rt::JsonValueKind::Object;
  view.raw_start = 0;
  view.raw_end = text.size();
  return nebula::rt::ok_result(nebula::rt::JsonValue{std::move(text), view, std::move(object_fields)});
}

nebula::rt::Result<std::string, std::string> __nebula_thin_host_generated_command_kind(std::string text) {
  const auto* parsed = cached_generated_command(text);
  if (parsed == nullptr) {
    return nebula::rt::err_result<std::string>("thin-host command fast path miss");
  }
  const auto& span = parsed->view.kind;
  return nebula::rt::ok_result(parsed->text.substr(span.value_start + 1, span.value_end - span.value_start - 2));
}

nebula::rt::Result<std::string, std::string>
__nebula_thin_host_generated_command_correlation_id(std::string text) {
  const auto* parsed = cached_generated_command(text);
  if (parsed == nullptr) {
    return nebula::rt::err_result<std::string>("thin-host command fast path miss");
  }
  const auto& span = parsed->view.correlation_id;
  return nebula::rt::ok_result(parsed->text.substr(span.value_start + 1, span.value_end - span.value_start - 2));
}

nebula::rt::Result<std::int64_t, std::string>
__nebula_thin_host_generated_command_state_revision(std::string text) {
  const auto* parsed = cached_generated_command(text);
  if (parsed == nullptr) {
    return nebula::rt::err_result<std::int64_t>("thin-host command fast path miss");
  }
  return nebula::rt::ok_result(parsed->view.revision_value);
}
