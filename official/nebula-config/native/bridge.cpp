#include "runtime/nebula_runtime.hpp"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

constexpr std::streamoff kMaxSecretFileBytes = 64 * 1024;

std::string trim_secret_text(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

nebula::rt::Result<std::string, std::string> read_secret_file(std::string file_path) {
  std::ifstream handle(file_path, std::ios::binary | std::ios::ate);
  if (!handle) {
    return nebula::rt::err_result<std::string>("failed to read secret file: " + file_path);
  }
  const std::streamoff file_size = handle.tellg();
  if (file_size < 0) {
    return nebula::rt::err_result<std::string>("failed to read secret file: " + file_path);
  }
  if (file_size > kMaxSecretFileBytes) {
    return nebula::rt::err_result<std::string>("secret file is too large: " + file_path);
  }
  handle.seekg(0, std::ios::beg);
  std::string text;
  text.resize(static_cast<std::size_t>(file_size));
  if (!text.empty()) {
    handle.read(text.data(), file_size);
    if (!handle) {
      return nebula::rt::err_result<std::string>("failed to read secret file: " + file_path);
    }
  }
  text = trim_secret_text(std::move(text));
  if (text.empty()) {
    return nebula::rt::err_result<std::string>("secret file is empty: " + file_path);
  }
  return nebula::rt::ok_result(std::move(text));
}

nebula::rt::Result<std::string, std::string> optional_secret_value(std::string env_name,
                                                                   std::string file_env_name) {
  const char* env_raw = std::getenv(env_name.c_str());
  const char* file_raw = std::getenv(file_env_name.c_str());
  const std::string env_value = env_raw != nullptr ? env_raw : "";
  const std::string file_path = file_raw != nullptr ? file_raw : "";

  if (!env_value.empty() && !file_path.empty()) {
    return nebula::rt::err_result<std::string>(env_name + " and " + file_env_name + " must not both be set");
  }
  if (!file_path.empty()) {
    return read_secret_file(std::move(file_path));
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
    auto resolved = optional_secret_value(env_name, file_env_name);
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

}  // namespace

nebula::rt::Result<std::string, std::string> __nebula_config_optional_secret_value(std::string env_name,
                                                                                   std::string file_env_name) {
  return cached_secret_value(std::move(env_name), std::move(file_env_name));
}

nebula::rt::Result<std::string, std::string> __nebula_config_required_secret_value(std::string env_name,
                                                                                   std::string file_env_name) {
  auto value = cached_secret_value(env_name, file_env_name);
  if (nebula::rt::result_is_err(value)) {
    return value;
  }
  if (nebula::rt::result_ok_ref(value).empty()) {
    return nebula::rt::err_result<std::string>(env_name + " or " + file_env_name + " is required");
  }
  return value;
}
