#include "runtime/nebula_runtime.hpp"

#include <cstdlib>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

struct ParsedBaseUrl {
  std::string scheme;
  std::string normalized;
  std::string host;
  std::string authority;
  std::string path_prefix;
  std::int64_t port = 80;
};

nebula::rt::Result<ParsedBaseUrl, std::string> parse_base_url(std::string url) {
  if (url.empty()) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL must be non-empty");
  }
  for (unsigned char ch : url) {
    if (ch <= 0x20) {
      return nebula::rt::err_result<ParsedBaseUrl>("base URL must not contain whitespace");
    }
  }

  std::string_view scheme = "http";
  std::int64_t default_port = 80;
  std::size_t prefix_size = 7;
  if (url.rfind("http://", 0) == 0) {
    scheme = "http";
    default_port = 80;
    prefix_size = 7;
  } else if (url.rfind("https://", 0) == 0) {
    scheme = "https";
    default_port = 443;
    prefix_size = 8;
  } else {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL must start with http:// or https://");
  }

  const std::string_view remainder(url.data() + prefix_size, url.size() - prefix_size);
  const std::size_t slash = remainder.find('/');
  const std::string_view authority_view =
      slash == std::string_view::npos ? remainder : remainder.substr(0, slash);
  std::string_view path_view =
      slash == std::string_view::npos ? std::string_view{} : remainder.substr(slash);

  if (authority_view.empty()) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL host is missing");
  }
  if (authority_view.find('@') != std::string_view::npos) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL must not contain userinfo");
  }
  if (authority_view.find('[') != std::string_view::npos || authority_view.find(']') != std::string_view::npos) {
    return nebula::rt::err_result<ParsedBaseUrl>("IPv6 base URLs are not supported in this skeleton");
  }
  if (path_view.find('?') != std::string_view::npos || path_view.find('#') != std::string_view::npos) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL must not contain query or fragment components");
  }
  if (path_view.find("//") != std::string_view::npos) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL path must not contain empty path segments");
  }

  ParsedBaseUrl out;
  const std::size_t colon = authority_view.rfind(':');
  if (colon == std::string_view::npos) {
    out.host = std::string(authority_view);
    out.port = default_port;
  } else {
    if (authority_view.find(':') != colon) {
      return nebula::rt::err_result<ParsedBaseUrl>("base URL authority must be host[:port]");
    }
    const std::string_view host_view = authority_view.substr(0, colon);
    const std::string_view port_view = authority_view.substr(colon + 1);
    if (host_view.empty()) {
      return nebula::rt::err_result<ParsedBaseUrl>("base URL host is missing");
    }
    if (port_view.empty()) {
      return nebula::rt::err_result<ParsedBaseUrl>("base URL port is missing");
    }
    std::int64_t port = 0;
    const auto parse = std::from_chars(port_view.data(), port_view.data() + port_view.size(), port);
    if (parse.ec != std::errc{} || parse.ptr != port_view.data() + port_view.size() || port <= 0 || port > 65535) {
      return nebula::rt::err_result<ParsedBaseUrl>("base URL port must be an integer between 1 and 65535");
    }
    out.host = std::string(host_view);
    out.port = port;
  }

  if (out.host.empty()) {
    return nebula::rt::err_result<ParsedBaseUrl>("base URL host is missing");
  }

  std::string path_prefix = std::string(path_view);
  while (!path_prefix.empty() && path_prefix.size() > 1 && path_prefix.back() == '/') {
    path_prefix.pop_back();
  }
  if (path_prefix == "/") path_prefix.clear();

  out.scheme = std::string(scheme);
  out.path_prefix = path_prefix;
  out.authority = out.host + ":" + std::to_string(out.port);
  out.normalized = out.scheme + "://" + out.authority + out.path_prefix;
  return nebula::rt::ok_result(std::move(out));
}

nebula::rt::Result<bool, std::string> validate_path_segment(std::string label, std::string value) {
  if (value.empty()) {
    return nebula::rt::err_result<bool>(label + " must be non-empty");
  }
  if (value == "." || value == "..") {
    return nebula::rt::err_result<bool>(label + " must not be '.' or '..'");
  }
  for (unsigned char ch : value) {
    const bool ascii_lower = ch >= 'a' && ch <= 'z';
    const bool digit = ch >= '0' && ch <= '9';
    const bool allowed_punct = ch == '.' || ch == '_' || ch == '-';
    if (!(ascii_lower || digit || allowed_punct)) {
      return nebula::rt::err_result<bool>(
          label + " must use lowercase ASCII letters, digits, '.', '_' or '-' only");
    }
  }
  return nebula::rt::ok_result(true);
}

std::string trim_secret_text(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

nebula::rt::Result<std::string, std::string> resolve_secret_value_uncached(std::string env_name,
                                                                            std::string file_env_name) {
  const char* env_raw = std::getenv(env_name.c_str());
  const char* file_raw = std::getenv(file_env_name.c_str());
  const std::string env_value = env_raw != nullptr ? env_raw : "";
  const std::string file_path = file_raw != nullptr ? file_raw : "";

  if (!env_value.empty() && !file_path.empty()) {
    return nebula::rt::err_result<std::string>(env_name + " and " + file_env_name + " must not both be set");
  }
  if (!file_path.empty()) {
    std::ifstream handle(file_path, std::ios::binary);
    if (!handle) {
      return nebula::rt::err_result<std::string>("failed to read secret file: " + file_path);
    }
    std::string text((std::istreambuf_iterator<char>(handle)), std::istreambuf_iterator<char>());
    text = trim_secret_text(std::move(text));
    if (text.empty()) {
      return nebula::rt::err_result<std::string>("secret file is empty: " + file_path);
    }
    return nebula::rt::ok_result(std::move(text));
  }
  return nebula::rt::ok_result(env_value);
}

nebula::rt::Result<std::string, std::string> cached_secret_value(std::string env_name, std::string file_env_name) {
  struct SecretSnapshot {
    bool ready = false;
    bool ok = false;
    std::string value;
    std::string error;
  };

  static std::mutex cache_mu;
  static std::unordered_map<std::string, SecretSnapshot> cache;
  const std::string cache_key = env_name + "|" + file_env_name;

  std::lock_guard<std::mutex> lock(cache_mu);
  auto& entry = cache[cache_key];
  if (!entry.ready) {
    auto resolved = resolve_secret_value_uncached(env_name, file_env_name);
    entry.ready = true;
    if (nebula::rt::result_is_err(resolved)) {
      entry.ok = false;
      entry.error = nebula::rt::result_err_ref(resolved);
    } else {
      entry.ok = true;
      entry.value = nebula::rt::result_ok_ref(resolved);
    }
  }

  if (entry.ok) {
    return nebula::rt::ok_result(entry.value);
  }
  return nebula::rt::err_result<std::string>(entry.error);
}

} // namespace

nebula::rt::Result<std::string, std::string> __release_control_plane_core_normalize_base_url(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::string, std::string>(
        nebula::rt::Result<std::string, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).normalized);
}

nebula::rt::Result<std::string, std::string> __release_control_plane_core_base_url_scheme(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::string, std::string>(
        nebula::rt::Result<std::string, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).scheme);
}

nebula::rt::Result<std::string, std::string> __release_control_plane_core_base_url_host(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::string, std::string>(
        nebula::rt::Result<std::string, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).host);
}

nebula::rt::Result<std::int64_t, std::string> __release_control_plane_core_base_url_port(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::int64_t, std::string>(
        nebula::rt::Result<std::int64_t, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).port);
}

nebula::rt::Result<std::string, std::string> __release_control_plane_core_base_url_authority(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::string, std::string>(
        nebula::rt::Result<std::string, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).authority);
}

nebula::rt::Result<std::string, std::string> __release_control_plane_core_base_url_path_prefix(std::string url) {
  auto parsed = parse_base_url(std::move(url));
  if (nebula::rt::result_is_err(parsed)) {
    return nebula::rt::Result<std::string, std::string>(
        nebula::rt::Result<std::string, std::string>::Err{nebula::rt::result_err_ref(parsed)});
  }
  return nebula::rt::ok_result(nebula::rt::result_ok_ref(parsed).path_prefix);
}

nebula::rt::Result<bool, std::string> __release_control_plane_core_validate_path_segment(std::string label,
                                                                                          std::string value) {
  return validate_path_segment(std::move(label), std::move(value));
}

nebula::rt::Result<std::string, std::string> __release_control_plane_core_secret_value(std::string env_name,
                                                                                        std::string file_env_name) {
  return cached_secret_value(std::move(env_name), std::move(file_env_name));
}
