#pragma once

#include "nir/ir.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace nebula::nir {

enum class RuntimeAsyncCallKind : std::uint8_t { None, Spawn, Join, BlockOn, Timeout };

inline RuntimeAsyncCallKind classify_runtime_async_call(
    const std::optional<nebula::frontend::QualifiedName>& qualified,
    std::string_view fallback_callee = {}) {
  if (qualified.has_value() && qualified->package_name == "std") {
    if (qualified->module_name == "task") {
      if (qualified->local_name == "spawn") return RuntimeAsyncCallKind::Spawn;
      if (qualified->local_name == "join") return RuntimeAsyncCallKind::Join;
      if (qualified->local_name == "block_on") return RuntimeAsyncCallKind::BlockOn;
    }
    if (qualified->module_name == "time" && qualified->local_name == "timeout") {
      return RuntimeAsyncCallKind::Timeout;
    }
  }

  if (fallback_callee == "spawn") return RuntimeAsyncCallKind::Spawn;
  if (fallback_callee == "join") return RuntimeAsyncCallKind::Join;
  if (fallback_callee == "block_on") return RuntimeAsyncCallKind::BlockOn;
  if (fallback_callee == "timeout") return RuntimeAsyncCallKind::Timeout;
  return RuntimeAsyncCallKind::None;
}

inline std::optional<std::string> runtime_std_call_name(
    const std::optional<nebula::frontend::QualifiedName>& qualified,
    std::string_view fallback_callee = {}) {
  if (qualified.has_value() && qualified->package_name == "std") {
    if (qualified->module_name == "bytes") {
      if (qualified->local_name == "from_string") return "nebula::rt::bytes_from_string";
      if (qualified->local_name == "to_string") return "nebula::rt::bytes_to_string";
      if (qualified->local_name == "len") return "nebula::rt::bytes_len";
      if (qualified->local_name == "is_empty") return "nebula::rt::bytes_is_empty";
      if (qualified->local_name == "concat") return "nebula::rt::bytes_concat";
      if (qualified->local_name == "equal") return "nebula::rt::bytes_equal";
    }
    if (qualified->module_name == "json") {
      if (qualified->local_name == "parse") return "nebula::rt::json_parse";
      if (qualified->local_name == "stringify") return "nebula::rt::json_stringify";
      if (qualified->local_name == "string_value") return "nebula::rt::json_string_value";
      if (qualified->local_name == "int_value") return "nebula::rt::json_int_value";
      if (qualified->local_name == "bool_value") return "nebula::rt::json_bool_value";
      if (qualified->local_name == "null_value") return "nebula::rt::json_null_value";
      if (qualified->local_name == "object1") return "nebula::rt::json_object1";
      if (qualified->local_name == "object2") return "nebula::rt::json_object2";
      if (qualified->local_name == "object3") return "nebula::rt::json_object3";
      if (qualified->local_name == "object4") return "nebula::rt::json_object4";
      if (qualified->local_name == "object5") return "nebula::rt::json_object5";
      if (qualified->local_name == "object6") return "nebula::rt::json_object6";
      if (qualified->local_name == "object7") return "nebula::rt::json_object7";
      if (qualified->local_name == "get_string") return "nebula::rt::json_get_string";
      if (qualified->local_name == "get_int") return "nebula::rt::json_get_int";
      if (qualified->local_name == "get_bool") return "nebula::rt::json_get_bool";
    }
    if (qualified->module_name == "http") {
      if (qualified->local_name == "read_request") return "__nebula_rt_http_read_request";
      if (qualified->local_name == "header_value") return "__nebula_rt_http_header";
      if (qualified->local_name == "header_value_unique") return "__nebula_rt_http_header_unique";
      if (qualified->local_name == "content_type") return "__nebula_rt_http_content_type";
      if (qualified->local_name == "Request_close_connection") return "__nebula_rt_http_request_close_connection";
      if (qualified->local_name == "write_response") return "__nebula_rt_http_write_response";
      if (qualified->local_name == "write_response_with_connection") {
        return "__nebula_rt_http_write_response_with_connection";
      }
      if (qualified->local_name == "request1") return "__nebula_rt_http_request1";
      if (qualified->local_name == "request2") return "__nebula_rt_http_request2";
      if (qualified->local_name == "request3") return "__nebula_rt_http_request3";
      if (qualified->local_name == "write_request") return "__nebula_rt_http_write_request";
      if (qualified->local_name == "read_response") return "__nebula_rt_http_read_response";
      if (qualified->local_name == "read_response_for") return "__nebula_rt_http_read_response_for";
      if (qualified->local_name == "response_header") return "__nebula_rt_http_response_header";
    }
  }

  switch (classify_runtime_async_call(qualified, fallback_callee)) {
  case RuntimeAsyncCallKind::Spawn: return "nebula::rt::spawn";
  case RuntimeAsyncCallKind::Join: return "nebula::rt::join";
  case RuntimeAsyncCallKind::BlockOn: return "nebula::rt::block_on";
  case RuntimeAsyncCallKind::Timeout: return "nebula::rt::timeout";
  case RuntimeAsyncCallKind::None: break;
  }
  return std::nullopt;
}

} // namespace nebula::nir
