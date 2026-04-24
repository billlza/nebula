#include "bench_runtime.hpp"
#include "crypto_ref_support.h"

#include <array>
#include <cstdlib>
#include <string>

namespace {

std::string payload() {
  return "nebula-backend-crypto-benchmark-0123456789abcdef0123456789abcdef";
}

void workload() {
  const std::string data = payload();
  std::array<std::uint8_t, NEBULA_CRYPTO_BLAKE3_BYTES> blake{};
  std::array<std::uint8_t, NEBULA_CRYPTO_SHA3_256_BYTES> sha{};
  nebula_crypto_blake3_digest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), blake.data());
  nebula_crypto_sha3_256_digest(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), sha.data());
  if (blake[0] == 0 && sha[0] == 0) {
    std::abort();
  }
}

}  // namespace

int main() {
  const auto metrics = backend_crypto_bench::run_bench(workload);
  backend_crypto_bench::emit_metrics_json(metrics);
  return 0;
}
