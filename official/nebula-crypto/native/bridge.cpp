#include "nebula_crypto_native.h"
#include "secure_memory.hpp"

#include "runtime/nebula_runtime.hpp"

#include "blake3.h"
#include "mbedtls_private_prefix.h"
#include "mbedtls/chachapoly.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#else
#error "nebula-crypto currently supports only macOS and Linux hosts"
#endif

namespace {

using RtBytes = nebula::rt::Bytes;
template <typename T>
using RtResult = nebula::rt::Result<T, std::string>;
using nebula::rt::result_err_ptr;
using nebula::rt::result_is_err;
using nebula::rt::result_ok_ptr;
using nebula::crypto_native::SecretBytesOwner;
using nebula::crypto_native::secure_zeroize;

template <typename T>
RtResult<T> make_err(std::string message) {
  return nebula::rt::err_result<T>(std::move(message));
}

template <typename T>
RtResult<T> make_ok(T value) {
  return nebula::rt::ok_result(std::move(value));
}

template <std::size_t N>
struct ZeroizingArray {
  std::array<unsigned char, N> bytes = {};

  ~ZeroizingArray() { secure_zeroize(bytes.data(), bytes.size()); }

  unsigned char* data() { return bytes.data(); }
  const unsigned char* data() const { return bytes.data(); }
  std::size_t size() const { return bytes.size(); }
};

struct StringZeroizer {
  explicit StringZeroizer(std::string& text_ref) : text(text_ref) {}
  ~StringZeroizer() {
    if (!text.empty()) {
      secure_zeroize(text.data(), text.size());
    }
  }

  std::string& text;
};

RtResult<RtBytes> export_secret_owner_result(RtResult<SecretBytesOwner> owner_result) {
  if (auto* err = result_err_ptr(owner_result)) {
    return make_err<RtBytes>(err->value);
  }
  auto* ok = result_ok_ptr(owner_result);
  if (ok == nullptr) {
    nebula::rt::panic("secret owner result is in an invalid state");
  }
  return make_ok(ok->value.export_bytes());
}

RtResult<RtBytes> validate_secret_bytes(RtBytes bytes,
                                        std::size_t expected,
                                        std::string_view label) {
  return export_secret_owner_result(
      SecretBytesOwner::adopt_checked(std::move(bytes), expected, label));
}

RtBytes export_secret_bytes_or_panic(RtBytes bytes,
                                     std::size_t expected,
                                     std::string_view label) {
  return SecretBytesOwner::adopt_or_panic(std::move(bytes), expected, label).export_bytes();
}

RtResult<RtBytes> validate_exact_bytes(RtBytes bytes, std::size_t expected, std::string_view label) {
  if (bytes.data.size() != expected) {
    return make_err<RtBytes>(std::string(label) + " must be exactly " + std::to_string(expected) +
                             " bytes");
  }
  return make_ok(std::move(bytes));
}

RtResult<RtBytes> validate_exact_input_bytes(const RtBytes& bytes,
                                             std::size_t expected,
                                             std::string_view label) {
  if (bytes.data.size() != expected) {
    return make_err<RtBytes>(std::string(label) + " must be exactly " + std::to_string(expected) +
                             " bytes");
  }
  return make_ok(bytes);
}

RtBytes require_exact_bytes_or_panic(const RtBytes& bytes, std::size_t expected, std::string_view label) {
  if (bytes.data.size() != expected) {
    nebula::rt::panic("corrupt " + std::string(label) + ": expected " + std::to_string(expected) +
                      " bytes, got " + std::to_string(bytes.data.size()));
  }
  return bytes;
}

RtBytes copy_bytes_or_panic(const std::uint8_t* data, std::size_t len, std::string_view label) {
  try {
    return RtBytes{std::string(reinterpret_cast<const char*>(data), len)};
  } catch (const std::exception& ex) {
    nebula::rt::panic(std::string(label) + " allocation failed: " + ex.what());
  }
}

RtResult<RtBytes> concat_secret_bytes_result(const std::uint8_t* lhs,
                                             std::size_t lhs_len,
                                             const std::uint8_t* rhs,
                                             std::size_t rhs_len,
                                             std::string_view label) {
  try {
    std::string joined;
    joined.reserve(lhs_len + rhs_len);
    joined.append(reinterpret_cast<const char*>(lhs), lhs_len);
    joined.append(reinterpret_cast<const char*>(rhs), rhs_len);
    SecretBytesOwner owner(std::move(joined));
    return make_ok(owner.export_bytes());
  } catch (const std::exception& ex) {
    return make_err<RtBytes>(std::string(label) + " allocation failed: " + ex.what());
  }
}

RtBytes slice_bytes_or_panic(const RtBytes& bytes,
                             std::size_t offset,
                             std::size_t len,
                             std::string_view label) {
  if (offset + len > bytes.data.size()) {
    nebula::rt::panic("corrupt " + std::string(label) + ": slice is out of bounds");
  }
  try {
    return RtBytes{std::string(bytes.data.data() + offset, len)};
  } catch (const std::exception& ex) {
    nebula::rt::panic(std::string(label) + " allocation failed: " + ex.what());
  }
}

RtResult<RtBytes> system_random_bytes(std::int64_t len) {
  if (len < 0) return make_err<RtBytes>("random_bytes len must be non-negative");
  std::string bytes;
  try {
    bytes.assign(static_cast<std::size_t>(len), '\0');
  } catch (const std::exception& ex) {
    return make_err<RtBytes>("random_bytes allocation failed: " + std::string(ex.what()));
  }
  if (bytes.empty()) return make_ok(RtBytes{std::move(bytes)});

#if defined(__APPLE__)
  arc4random_buf(bytes.data(), bytes.size());
  return make_ok(RtBytes{std::move(bytes)});
#else
  std::size_t filled = 0;
  while (filled < bytes.size()) {
    const ssize_t rc = getrandom(bytes.data() + filled, bytes.size() - filled, 0);
    if (rc > 0) {
      filled += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    const int err = (rc < 0) ? errno : 0;
    return make_err<RtBytes>("getrandom failed: " + std::string(std::strerror(err)));
  }
  return make_ok(RtBytes{std::move(bytes)});
#endif
}

std::string bytes_to_hex(const RtBytes& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(bytes.data.size() * 2);
  for (std::size_t i = 0; i < bytes.data.size(); ++i) {
    const unsigned char value = static_cast<unsigned char>(bytes.data[i]);
    out[i * 2] = kHex[(value >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[value & 0x0F];
  }
  return out;
}

int hex_nibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

RtResult<RtBytes> hex_to_bytes(std::string_view text) {
  if ((text.size() % 2) != 0) return make_err<RtBytes>("hex text must have even length");
  try {
    std::string out;
    out.resize(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
      const int hi = hex_nibble(text[i]);
      const int lo = hex_nibble(text[i + 1]);
      if (hi < 0 || lo < 0) return make_err<RtBytes>("hex text contains a non-hex character");
      out[i / 2] = static_cast<char>((hi << 4) | lo);
    }
    return make_ok(RtBytes{std::move(out)});
  } catch (const std::exception& ex) {
    return make_err<RtBytes>("hex decode allocation failed: " + std::string(ex.what()));
  }
}

RtResult<RtBytes> blake3_derive_key_result(std::string_view context,
                                           const RtBytes& material,
                                           std::int64_t out_len) {
  if (out_len < 0) return make_err<RtBytes>("blake3_derive_key out_len must be non-negative");
  try {
    std::string out;
    out.assign(static_cast<std::size_t>(out_len), '\0');
    if (!out.empty()) {
      blake3_hasher hasher;
      blake3_hasher_init_derive_key_raw(&hasher, context.data(), context.size());
      blake3_hasher_update(&hasher, material.data.data(), material.data.size());
      blake3_hasher_finalize(&hasher, reinterpret_cast<std::uint8_t*>(out.data()), out.size());
    }
    return make_ok(RtBytes{std::move(out)});
  } catch (const std::exception& ex) {
    return make_err<RtBytes>("blake3_derive_key allocation failed: " + std::string(ex.what()));
  }
}

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t sha256_rotr(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

std::uint32_t sha256_load_be32(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

void sha256_store_be32(std::uint32_t value, unsigned char* out) {
  out[0] = static_cast<unsigned char>((value >> 24) & 0xffU);
  out[1] = static_cast<unsigned char>((value >> 16) & 0xffU);
  out[2] = static_cast<unsigned char>((value >> 8) & 0xffU);
  out[3] = static_cast<unsigned char>(value & 0xffU);
}

void sha256_store_be64(std::uint64_t value, unsigned char* out) {
  for (int i = 7; i >= 0; --i) {
    out[7 - i] = static_cast<unsigned char>((value >> (i * 8)) & 0xffU);
  }
}

void sha256_process_block(const unsigned char* block, std::array<std::uint32_t, 8>& state) {
  std::array<std::uint32_t, 64> words = {};
  for (std::size_t i = 0; i < 16; ++i) {
    words[i] = sha256_load_be32(block + (i * 4));
  }
  for (std::size_t i = 16; i < words.size(); ++i) {
    const std::uint32_t s0 = sha256_rotr(words[i - 15], 7) ^
                             sha256_rotr(words[i - 15], 18) ^
                             (words[i - 15] >> 3);
    const std::uint32_t s1 = sha256_rotr(words[i - 2], 17) ^
                             sha256_rotr(words[i - 2], 19) ^
                             (words[i - 2] >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (std::size_t i = 0; i < words.size(); ++i) {
    const std::uint32_t sum1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sum1 + ch + kSha256RoundConstants[i] + words[i];
    const std::uint32_t sum0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sum0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

std::array<unsigned char, 32> sha256_digest(std::string_view data) {
  std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  std::size_t offset = 0;
  while (offset + 64 <= data.size()) {
    sha256_process_block(reinterpret_cast<const unsigned char*>(data.data() + offset), state);
    offset += 64;
  }

  std::array<unsigned char, 128> tail = {};
  const std::size_t remaining = data.size() - offset;
  if (remaining > 0) {
    std::memcpy(tail.data(), data.data() + offset, remaining);
  }
  tail[remaining] = 0x80U;
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8U;
  const std::size_t length_offset = remaining >= 56 ? 120 : 56;
  sha256_store_be64(bit_len, tail.data() + length_offset);
  sha256_process_block(tail.data(), state);
  if (remaining >= 56) {
    sha256_process_block(tail.data() + 64, state);
  }

  std::array<unsigned char, 32> out = {};
  for (std::size_t i = 0; i < state.size(); ++i) {
    sha256_store_be32(state[i], out.data() + (i * 4));
  }
  return out;
}

std::string bytes_to_hex_view(const unsigned char* data, std::size_t len) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    const unsigned char value = data[i];
    out[i * 2] = kHex[(value >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[value & 0x0F];
  }
  return out;
}

RtResult<std::string> hmac_sha256_hex_result(std::string_view key, const RtBytes& data) {
  try {
    ZeroizingArray<64> key_block;
    if (key.size() > key_block.size()) {
      ZeroizingArray<32> hashed_key;
      hashed_key.bytes = sha256_digest(key);
      std::memcpy(key_block.data(), hashed_key.data(), hashed_key.size());
    } else if (!key.empty()) {
      std::memcpy(key_block.data(), key.data(), key.size());
    }

    std::string inner;
    StringZeroizer inner_zeroizer(inner);
    inner.reserve(key_block.size() + data.data.size());
    for (unsigned char byte : key_block.bytes) {
      inner.push_back(static_cast<char>(byte ^ 0x36U));
    }
    inner.append(data.data);
    ZeroizingArray<32> inner_digest;
    inner_digest.bytes = sha256_digest(inner);

    std::string outer;
    StringZeroizer outer_zeroizer(outer);
    outer.reserve(key_block.size() + inner_digest.size());
    for (unsigned char byte : key_block.bytes) {
      outer.push_back(static_cast<char>(byte ^ 0x5cU));
    }
    outer.append(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size());
    ZeroizingArray<32> digest;
    digest.bytes = sha256_digest(outer);
    std::string hex = bytes_to_hex_view(digest.data(), digest.size());
    return make_ok(std::move(hex));
  } catch (const std::exception& ex) {
    return make_err<std::string>("hmac_sha256 allocation failed: " + std::string(ex.what()));
  }
}

bool constant_time_text_equal(std::string_view lhs, std::string_view rhs) {
  const std::size_t max_len = lhs.size() > rhs.size() ? lhs.size() : rhs.size();
  unsigned char diff = static_cast<unsigned char>(lhs.size() ^ rhs.size());
  for (std::size_t i = 0; i < max_len; ++i) {
    const unsigned char l = i < lhs.size() ? static_cast<unsigned char>(lhs[i]) : 0;
    const unsigned char r = i < rhs.size() ? static_cast<unsigned char>(rhs[i]) : 0;
    diff = static_cast<unsigned char>(diff | (l ^ r));
  }
  return diff == 0;
}

RtResult<RtBytes> chacha20_poly1305_seal_result(const RtBytes& key_bytes,
                                                const RtBytes& nonce_bytes,
                                                const RtBytes& aad,
                                                const RtBytes& plaintext) {
  auto key_owner = SecretBytesOwner::copy_or_panic(
      key_bytes, NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES, "ChaCha20Poly1305Key");
  auto nonce = validate_exact_input_bytes(
      nonce_bytes, NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES, "ChaCha20Poly1305 nonce");
  if (result_is_err(nonce)) {
    return nonce;
  }
  try {
    std::string out;
    out.assign(plaintext.data.size() + NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES, '\0');
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    const int setkey_rc = mbedtls_chachapoly_setkey(
        &ctx, reinterpret_cast<const unsigned char*>(key_owner.data()));
    if (setkey_rc != 0) {
      mbedtls_chachapoly_free(&ctx);
      return make_err<RtBytes>("chacha20_poly1305 setkey failed");
    }
    const int seal_rc = mbedtls_chachapoly_encrypt_and_tag(
        &ctx,
        plaintext.data.size(),
        reinterpret_cast<const unsigned char*>(nonce_bytes.data.data()),
        reinterpret_cast<const unsigned char*>(aad.data.data()),
        aad.data.size(),
        reinterpret_cast<const unsigned char*>(plaintext.data.data()),
        reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<unsigned char*>(out.data() + plaintext.data.size()));
    mbedtls_chachapoly_free(&ctx);
    if (seal_rc != 0) {
      secure_zeroize(out.data(), out.size());
      return make_err<RtBytes>("chacha20_poly1305 seal failed");
    }
    return make_ok(RtBytes{std::move(out)});
  } catch (const std::exception& ex) {
    return make_err<RtBytes>("chacha20_poly1305 seal allocation failed: " + std::string(ex.what()));
  }
}

RtResult<RtBytes> chacha20_poly1305_open_result(const RtBytes& key_bytes,
                                                const RtBytes& nonce_bytes,
                                                const RtBytes& aad,
                                                const RtBytes& ciphertext) {
  auto key_owner = SecretBytesOwner::copy_or_panic(
      key_bytes, NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES, "ChaCha20Poly1305Key");
  auto nonce = validate_exact_input_bytes(
      nonce_bytes, NEBULA_CRYPTO_CHACHA20_POLY1305_NONCE_BYTES, "ChaCha20Poly1305 nonce");
  if (result_is_err(nonce)) {
    return nonce;
  }
  if (ciphertext.data.size() < NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES) {
    return make_err<RtBytes>("ChaCha20Poly1305 ciphertext must be at least 16 bytes");
  }
  const std::size_t body_len =
      ciphertext.data.size() - NEBULA_CRYPTO_CHACHA20_POLY1305_TAG_BYTES;
  try {
    std::string out;
    out.assign(body_len, '\0');
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    const int setkey_rc = mbedtls_chachapoly_setkey(
        &ctx, reinterpret_cast<const unsigned char*>(key_owner.data()));
    if (setkey_rc != 0) {
      mbedtls_chachapoly_free(&ctx);
      return make_err<RtBytes>("chacha20_poly1305 setkey failed");
    }
    const int open_rc = mbedtls_chachapoly_auth_decrypt(
        &ctx,
        body_len,
        reinterpret_cast<const unsigned char*>(nonce_bytes.data.data()),
        reinterpret_cast<const unsigned char*>(aad.data.data()),
        aad.data.size(),
        reinterpret_cast<const unsigned char*>(ciphertext.data.data() + body_len),
        reinterpret_cast<const unsigned char*>(ciphertext.data.data()),
        reinterpret_cast<unsigned char*>(out.data()));
    mbedtls_chachapoly_free(&ctx);
    if (open_rc == MBEDTLS_ERR_CHACHAPOLY_AUTH_FAILED) {
      secure_zeroize(out.data(), out.size());
      return make_err<RtBytes>("chacha20_poly1305 authentication failed");
    }
    if (open_rc != 0) {
      secure_zeroize(out.data(), out.size());
      return make_err<RtBytes>("chacha20_poly1305 open failed");
    }
    return make_ok(RtBytes{std::move(out)});
  } catch (const std::exception& ex) {
    return make_err<RtBytes>("chacha20_poly1305 open allocation failed: " + std::string(ex.what()));
  }
}

RtResult<RtBytes> ml_kem_keypair_result() {
  std::uint8_t public_key[NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES];
  std::uint8_t secret_key[NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES];
  if (nebula_crypto_ml_kem_768_keypair(public_key, secret_key) != 0) {
    secure_zeroize(public_key, sizeof(public_key));
    secure_zeroize(secret_key, sizeof(secret_key));
    return make_err<RtBytes>("ml_kem_768 keypair failed");
  }
  auto pair = concat_secret_bytes_result(public_key,
                                         sizeof(public_key),
                                         secret_key,
                                         sizeof(secret_key),
                                         "ml_kem_768 keypair");
  secure_zeroize(public_key, sizeof(public_key));
  secure_zeroize(secret_key, sizeof(secret_key));
  return pair;
}

RtBytes ml_kem_keypair_public_key_or_panic(const RtBytes& pair) {
  require_exact_bytes_or_panic(
      pair,
      NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES,
      "MlKem768KeyPair");
  return slice_bytes_or_panic(pair, 0, NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES, "MlKem768PublicKey");
}

RtBytes ml_kem_keypair_secret_key_or_panic(const RtBytes& pair) {
  auto pair_owner = SecretBytesOwner::copy_or_panic(
      pair,
      NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES,
      "MlKem768KeyPair");
  return pair_owner.export_slice_or_panic(NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES,
                                          NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES,
                                          "MlKem768SecretKey");
}

RtResult<RtBytes> ml_kem_encapsulate_result(const RtBytes& public_key_bytes) {
  require_exact_bytes_or_panic(
      public_key_bytes, NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES, "MlKem768PublicKey");
  std::uint8_t ciphertext[NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES];
  std::uint8_t shared_secret[NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES];
  const int rc = nebula_crypto_ml_kem_768_encapsulate(
      ciphertext,
      shared_secret,
      reinterpret_cast<const std::uint8_t*>(public_key_bytes.data.data()));
  if (rc != 0) {
    secure_zeroize(ciphertext, sizeof(ciphertext));
    secure_zeroize(shared_secret, sizeof(shared_secret));
    return make_err<RtBytes>("ml_kem_768 encapsulate failed");
  }
  auto envelope = concat_secret_bytes_result(
      ciphertext, sizeof(ciphertext), shared_secret, sizeof(shared_secret), "ml_kem_768 encapsulate");
  secure_zeroize(ciphertext, sizeof(ciphertext));
  secure_zeroize(shared_secret, sizeof(shared_secret));
  return envelope;
}

RtBytes ml_kem_encapsulation_ciphertext_or_panic(const RtBytes& encap) {
  require_exact_bytes_or_panic(
      encap,
      NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES + NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES,
      "MlKem768Encapsulation");
  return slice_bytes_or_panic(encap, 0, NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES, "MlKem768Ciphertext");
}

RtBytes ml_kem_encapsulation_shared_secret_or_panic(const RtBytes& encap) {
  auto encap_owner = SecretBytesOwner::copy_or_panic(
      encap,
      NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES + NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES,
      "MlKem768Encapsulation");
  return encap_owner.export_slice_or_panic(NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES,
                                           NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES,
                                           "MlKem768SharedSecret");
}

RtResult<RtBytes> ml_kem_decapsulate_result(const RtBytes& secret_key_bytes, const RtBytes& ciphertext_bytes) {
  auto secret_key_owner = SecretBytesOwner::copy_or_panic(
      secret_key_bytes, NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES, "MlKem768SecretKey");
  require_exact_bytes_or_panic(
      ciphertext_bytes, NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES, "MlKem768Ciphertext");
  std::uint8_t shared_secret[NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES];
  const int rc = nebula_crypto_ml_kem_768_decapsulate(
      shared_secret,
      reinterpret_cast<const std::uint8_t*>(ciphertext_bytes.data.data()),
      reinterpret_cast<const std::uint8_t*>(secret_key_owner.data()));
  if (rc != 0) {
    secure_zeroize(shared_secret, sizeof(shared_secret));
    return make_err<RtBytes>("ml_kem_768 decapsulate failed");
  }
  auto secret_owner = SecretBytesOwner::copy_from_raw_or_panic(
      shared_secret, sizeof(shared_secret), "ml_kem_768 shared secret");
  auto secret = make_ok(secret_owner.export_bytes());
  secure_zeroize(shared_secret, sizeof(shared_secret));
  return secret;
}

RtResult<RtBytes> ml_dsa_keypair_result() {
  std::uint8_t public_key[NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES];
  std::uint8_t secret_key[NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES];
  if (nebula_crypto_ml_dsa_65_keypair(public_key, secret_key) != 0) {
    secure_zeroize(public_key, sizeof(public_key));
    secure_zeroize(secret_key, sizeof(secret_key));
    return make_err<RtBytes>("ml_dsa_65 keypair failed");
  }
  auto pair = concat_secret_bytes_result(public_key,
                                         sizeof(public_key),
                                         secret_key,
                                         sizeof(secret_key),
                                         "ml_dsa_65 keypair");
  secure_zeroize(public_key, sizeof(public_key));
  secure_zeroize(secret_key, sizeof(secret_key));
  return pair;
}

RtBytes ml_dsa_keypair_public_key_or_panic(const RtBytes& pair) {
  require_exact_bytes_or_panic(
      pair,
      NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES,
      "MlDsa65KeyPair");
  return slice_bytes_or_panic(pair, 0, NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES, "MlDsa65PublicKey");
}

RtBytes ml_dsa_keypair_secret_key_or_panic(const RtBytes& pair) {
  auto pair_owner = SecretBytesOwner::copy_or_panic(
      pair,
      NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES,
      "MlDsa65KeyPair");
  return pair_owner.export_slice_or_panic(NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES,
                                          NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES,
                                          "MlDsa65SecretKey");
}

RtResult<RtBytes> ml_dsa_sign_result(const RtBytes& secret_key_bytes, const RtBytes& message) {
  auto secret_key_owner = SecretBytesOwner::copy_or_panic(
      secret_key_bytes, NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES, "MlDsa65SecretKey");
  std::uint8_t signature[NEBULA_CRYPTO_ML_DSA_65_SIGNATURE_BYTES];
  std::size_t signature_len = sizeof(signature);
  const int rc = nebula_crypto_ml_dsa_65_sign(
      signature,
      &signature_len,
      reinterpret_cast<const std::uint8_t*>(message.data.data()),
      message.data.size(),
      reinterpret_cast<const std::uint8_t*>(secret_key_owner.data()));
  if (rc != 0) {
    secure_zeroize(signature, sizeof(signature));
    return make_err<RtBytes>("ml_dsa_65 sign failed");
  }
  if (signature_len != sizeof(signature)) {
    secure_zeroize(signature, sizeof(signature));
    return make_err<RtBytes>("ml_dsa_65 sign returned an unexpected signature length");
  }
  auto sig = make_ok(copy_bytes_or_panic(signature, sizeof(signature), "ml_dsa_65 signature"));
  secure_zeroize(signature, sizeof(signature));
  return sig;
}

} // namespace

RtBytes __nebula_crypto_blake3(RtBytes data) {
  std::string digest(NEBULA_CRYPTO_BLAKE3_BYTES, '\0');
  nebula_crypto_blake3_digest(reinterpret_cast<const std::uint8_t*>(data.data.data()),
                              data.data.size(),
                              reinterpret_cast<std::uint8_t*>(digest.data()));
  return RtBytes{std::move(digest)};
}

RtBytes __nebula_crypto_sha3_256(RtBytes data) {
  std::string digest(NEBULA_CRYPTO_SHA3_256_BYTES, '\0');
  nebula_crypto_sha3_256_digest(reinterpret_cast<const std::uint8_t*>(data.data.data()),
                                data.data.size(),
                                reinterpret_cast<std::uint8_t*>(digest.data()));
  return RtBytes{std::move(digest)};
}

RtBytes __nebula_crypto_sha3_512(RtBytes data) {
  std::string digest(NEBULA_CRYPTO_SHA3_512_BYTES, '\0');
  nebula_crypto_sha3_512_digest(reinterpret_cast<const std::uint8_t*>(data.data.data()),
                                data.data.size(),
                                reinterpret_cast<std::uint8_t*>(digest.data()));
  return RtBytes{std::move(digest)};
}

std::string __nebula_crypto_hex(RtBytes data) {
  return bytes_to_hex(data);
}

RtResult<RtBytes> __nebula_crypto_from_hex(std::string text) {
  return hex_to_bytes(text);
}

RtResult<RtBytes> __nebula_crypto_random_bytes(std::int64_t len) {
  return system_random_bytes(len);
}

RtResult<RtBytes> __nebula_crypto_blake3_derive_key(std::string context, RtBytes material, std::int64_t out_len) {
  return blake3_derive_key_result(context, material, out_len);
}

RtResult<std::string> __nebula_crypto_hmac_sha256_hex(std::string key, RtBytes data) {
  return hmac_sha256_hex_result(key, data);
}

bool __nebula_crypto_constant_time_equal_text(std::string lhs, std::string rhs) {
  return constant_time_text_equal(lhs, rhs);
}

RtBytes __nebula_crypto_chacha20_poly1305_key_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self), NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES, "ChaCha20Poly1305Key");
}

RtResult<RtBytes> __nebula_crypto_chacha20_poly1305_key_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes), NEBULA_CRYPTO_CHACHA20_POLY1305_KEY_BYTES, "ChaCha20Poly1305Key");
}

RtResult<RtBytes> __nebula_crypto_chacha20_poly1305_seal(RtBytes key,
                                                          RtBytes nonce,
                                                          RtBytes aad,
                                                          RtBytes plaintext) {
  return chacha20_poly1305_seal_result(key, nonce, aad, plaintext);
}

RtResult<RtBytes> __nebula_crypto_chacha20_poly1305_open(RtBytes key,
                                                          RtBytes nonce,
                                                          RtBytes aad,
                                                          RtBytes ciphertext) {
  return chacha20_poly1305_open_result(key, nonce, aad, ciphertext);
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_keypair() {
  return ml_kem_keypair_result();
}

RtBytes __nebula_crypto_ml_kem_768_keypair_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self),
      NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES,
      "MlKem768KeyPair");
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_keypair_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes),
      NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES,
      "MlKem768KeyPair");
}

RtBytes __nebula_crypto_ml_kem_768_keypair_public_key(RtBytes self) {
  return ml_kem_keypair_public_key_or_panic(self);
}

RtBytes __nebula_crypto_ml_kem_768_keypair_secret_key(RtBytes self) {
  return ml_kem_keypair_secret_key_or_panic(self);
}

RtBytes __nebula_crypto_ml_kem_768_public_key_to_bytes(RtBytes self) {
  return self;
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_public_key_from_bytes(RtBytes bytes) {
  return validate_exact_bytes(std::move(bytes),
                              NEBULA_CRYPTO_ML_KEM_768_PUBLIC_KEY_BYTES,
                              "MlKem768PublicKey");
}

RtBytes __nebula_crypto_ml_kem_768_secret_key_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self), NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES, "MlKem768SecretKey");
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_secret_key_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes), NEBULA_CRYPTO_ML_KEM_768_SECRET_KEY_BYTES, "MlKem768SecretKey");
}

RtBytes __nebula_crypto_ml_kem_768_ciphertext_to_bytes(RtBytes self) {
  return self;
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_ciphertext_from_bytes(RtBytes bytes) {
  return validate_exact_bytes(std::move(bytes),
                              NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES,
                              "MlKem768Ciphertext");
}

RtBytes __nebula_crypto_ml_kem_768_shared_secret_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self), NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES, "MlKem768SharedSecret");
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_shared_secret_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes), NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES, "MlKem768SharedSecret");
}

bool __nebula_crypto_ml_kem_768_shared_secret_equal(RtBytes self, RtBytes other) {
  auto lhs = SecretBytesOwner::adopt_or_panic(
      std::move(self), NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES, "MlKem768SharedSecret");
  auto rhs = SecretBytesOwner::adopt_or_panic(
      std::move(other), NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES, "MlKem768SharedSecret");
  return lhs.constant_time_equal(rhs);
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_encapsulate(RtBytes public_key) {
  return ml_kem_encapsulate_result(public_key);
}

RtBytes __nebula_crypto_ml_kem_768_encapsulation_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self),
      NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES + NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES,
      "MlKem768Encapsulation");
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_encapsulation_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes),
      NEBULA_CRYPTO_ML_KEM_768_CIPHERTEXT_BYTES + NEBULA_CRYPTO_ML_KEM_768_SHARED_SECRET_BYTES,
      "MlKem768Encapsulation");
}

RtBytes __nebula_crypto_ml_kem_768_encapsulation_ciphertext(RtBytes self) {
  return ml_kem_encapsulation_ciphertext_or_panic(self);
}

RtBytes __nebula_crypto_ml_kem_768_encapsulation_shared_secret(RtBytes self) {
  return ml_kem_encapsulation_shared_secret_or_panic(self);
}

RtResult<RtBytes> __nebula_crypto_ml_kem_768_decapsulate(RtBytes secret_key, RtBytes ciphertext) {
  return ml_kem_decapsulate_result(secret_key, ciphertext);
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_keypair() {
  return ml_dsa_keypair_result();
}

RtBytes __nebula_crypto_ml_dsa_65_keypair_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self),
      NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES,
      "MlDsa65KeyPair");
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_keypair_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes),
      NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES + NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES,
      "MlDsa65KeyPair");
}

RtBytes __nebula_crypto_ml_dsa_65_keypair_public_key(RtBytes self) {
  return ml_dsa_keypair_public_key_or_panic(self);
}

RtBytes __nebula_crypto_ml_dsa_65_keypair_secret_key(RtBytes self) {
  return ml_dsa_keypair_secret_key_or_panic(self);
}

RtBytes __nebula_crypto_ml_dsa_65_public_key_to_bytes(RtBytes self) {
  return self;
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_public_key_from_bytes(RtBytes bytes) {
  return validate_exact_bytes(std::move(bytes),
                              NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES,
                              "MlDsa65PublicKey");
}

RtBytes __nebula_crypto_ml_dsa_65_secret_key_to_bytes(RtBytes self) {
  return export_secret_bytes_or_panic(
      std::move(self), NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES, "MlDsa65SecretKey");
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_secret_key_from_bytes(RtBytes bytes) {
  return validate_secret_bytes(
      std::move(bytes), NEBULA_CRYPTO_ML_DSA_65_SECRET_KEY_BYTES, "MlDsa65SecretKey");
}

RtBytes __nebula_crypto_ml_dsa_65_signature_to_bytes(RtBytes self) {
  return self;
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_signature_from_bytes(RtBytes bytes) {
  return validate_exact_bytes(std::move(bytes),
                              NEBULA_CRYPTO_ML_DSA_65_SIGNATURE_BYTES,
                              "MlDsa65Signature");
}

RtResult<RtBytes> __nebula_crypto_ml_dsa_65_sign(RtBytes secret_key, RtBytes message) {
  return ml_dsa_sign_result(secret_key, message);
}

bool __nebula_crypto_ml_dsa_65_verify(RtBytes public_key, RtBytes message, RtBytes signature) {
  require_exact_bytes_or_panic(public_key, NEBULA_CRYPTO_ML_DSA_65_PUBLIC_KEY_BYTES, "MlDsa65PublicKey");
  require_exact_bytes_or_panic(signature, NEBULA_CRYPTO_ML_DSA_65_SIGNATURE_BYTES, "MlDsa65Signature");
  return nebula_crypto_ml_dsa_65_verify(reinterpret_cast<const std::uint8_t*>(message.data.data()),
                                        message.data.size(),
                                        reinterpret_cast<const std::uint8_t*>(signature.data.data()),
                                        signature.data.size(),
                                        reinterpret_cast<const std::uint8_t*>(public_key.data.data())) == 0;
}
