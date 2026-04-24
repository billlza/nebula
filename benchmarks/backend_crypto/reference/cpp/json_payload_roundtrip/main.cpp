#include "bench_runtime.hpp"

#include <cstdlib>
#include <string>

namespace {

std::string payload_json() {
  return "{\"user\":\"nebula\",\"count\":7,\"ok\":true,\"zone\":\"prod-cn\",\"kind\":\"api\"}";
}

std::string extract_string(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t start = text.find(needle);
  if (start == std::string::npos) {
    std::abort();
  }
  const std::size_t value_start = start + needle.size();
  const std::size_t value_end = text.find('"', value_start);
  if (value_end == std::string::npos) {
    std::abort();
  }
  return text.substr(value_start, value_end - value_start);
}

int extract_int(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t start = text.find(needle);
  if (start == std::string::npos) {
    std::abort();
  }
  const std::size_t value_start = start + needle.size();
  const std::size_t value_end = text.find_first_of(",}", value_start);
  if (value_end == std::string::npos) {
    std::abort();
  }
  return std::stoi(text.substr(value_start, value_end - value_start));
}

bool extract_bool(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\":";
  const std::size_t start = text.find(needle);
  if (start == std::string::npos) {
    std::abort();
  }
  const std::size_t value_start = start + needle.size();
  if (text.compare(value_start, 4, "true") == 0) {
    return true;
  }
  if (text.compare(value_start, 5, "false") == 0) {
    return false;
  }
  std::abort();
}

void workload() {
  const std::string text = payload_json();
  if (extract_string(text, "user") != "nebula" || extract_int(text, "count") != 7 ||
      !extract_bool(text, "ok")) {
    std::abort();
  }
}

}  // namespace

int main() {
  const auto metrics = backend_crypto_bench::run_bench(workload);
  backend_crypto_bench::emit_metrics_json(metrics);
  return 0;
}
