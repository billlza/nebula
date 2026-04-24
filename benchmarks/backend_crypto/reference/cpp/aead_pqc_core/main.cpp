#include "bench_runtime.hpp"
#include "crypto_ref_support.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void workload() {
  const std::string key_text = "0123456789abcdef0123456789abcdef";
  const std::string nonce_text = "0123456789ab";
  const std::string aad_text = "nebula-aead";
  const std::string plaintext_text = "nebula backend+crypto benchmark";

  std::array<std::uint8_t, NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES> key{};
  std::array<std::uint8_t, NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES> nonce{};
  std::memcpy(key.data(), key_text.data(), key.size());
  std::memcpy(nonce.data(), nonce_text.data(), nonce.size());

  std::array<std::uint8_t, 256> sealed{};
  std::array<std::uint8_t, 256> opened{};
  std::size_t sealed_len = 0;
  std::size_t opened_len = 0;
  if (backend_crypto_ref_chacha20_poly1305_seal(
          sealed.data(),
          sealed.size(),
          &sealed_len,
          key.data(),
          nonce.data(),
          reinterpret_cast<const std::uint8_t*>(aad_text.data()),
          aad_text.size(),
          reinterpret_cast<const std::uint8_t*>(plaintext_text.data()),
          plaintext_text.size()) != 0) {
    std::abort();
  }
  if (backend_crypto_ref_chacha20_poly1305_open(
          opened.data(),
          opened.size(),
          &opened_len,
          key.data(),
          nonce.data(),
          reinterpret_cast<const std::uint8_t*>(aad_text.data()),
          aad_text.size(),
          sealed.data(),
          sealed_len) != 0) {
    std::abort();
  }
  if (opened_len != plaintext_text.size() ||
      std::memcmp(opened.data(), plaintext_text.data(), plaintext_text.size()) != 0) {
    std::abort();
  }

  std::array<std::uint8_t, NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES> public_key{};
  std::array<std::uint8_t, NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES> secret_key{};
  std::array<std::uint8_t, NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES> ciphertext{};
  std::array<std::uint8_t, NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES> shared_secret{};
  std::array<std::uint8_t, NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES> recovered_shared_secret{};
  if (nebula_crypto_ml_kem_768_keypair(public_key.data(), secret_key.data()) != 0) {
    std::abort();
  }
  if (nebula_crypto_ml_kem_768_encapsulate(ciphertext.data(), shared_secret.data(), public_key.data()) != 0) {
    std::abort();
  }
  if (nebula_crypto_ml_kem_768_decapsulate(
          recovered_shared_secret.data(), ciphertext.data(), secret_key.data()) != 0) {
    std::abort();
  }
  if (std::memcmp(shared_secret.data(),
                  recovered_shared_secret.data(),
                  NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES) != 0) {
    std::abort();
  }
}

}  // namespace

int main() {
  const auto metrics = backend_crypto_bench::run_bench(workload);
  backend_crypto_bench::emit_metrics_json(metrics);
  return 0;
}
