#include "bench_runtime.hpp"

#include <cstdlib>
#include <string>

namespace {

std::string payload() {
  return "0123456789abcdef0123456789abcdef";
}

void workload() {
  const std::string frame = std::string("hdr:") + payload() + "|" + payload();
  if (frame.empty() || frame.size() != 69) {
    std::abort();
  }
}

}  // namespace

int main() {
  const auto metrics = backend_crypto_bench::run_bench(workload);
  backend_crypto_bench::emit_metrics_json(metrics);
  return 0;
}
