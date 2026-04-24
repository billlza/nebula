#include "runtime/nebula_runtime.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

template <typename T>
using RtResult = nebula::rt::Result<T, std::string>;
using RtJson = nebula::rt::JsonValue;

template <typename T>
RtResult<T> make_err(std::string message) {
  return nebula::rt::err_result<T>(std::move(message));
}

template <typename T>
RtResult<T> make_ok(T value) {
  return nebula::rt::ok_result(std::move(value));
}

bool rt_is_err(const auto& result) {
  return nebula::rt::result_is_err(result);
}

template <typename T>
std::string rt_err(const RtResult<T>& result) {
  return nebula::rt::result_err_ref(result);
}

RtResult<RtJson> parse_json(std::string text, std::string_view label) {
  auto parsed = nebula::rt::json_parse(std::move(text));
  if (rt_is_err(parsed)) {
    return make_err<RtJson>(std::string(label) + " is not valid JSON: " + rt_err(parsed));
  }
  return parsed;
}

RtResult<std::string> json_string_field(const RtJson& value, std::string_view key, std::string_view label) {
  auto field = nebula::rt::json_get_string(value, key);
  if (rt_is_err(field)) {
    return make_err<std::string>(std::string(label) + " missing string field: " + std::string(key));
  }
  return field;
}

std::optional<std::string> json_optional_string_field(const RtJson& value, std::string_view key) {
  auto field = nebula::rt::json_get_string(value, key);
  if (rt_is_err(field)) return std::nullopt;
  return nebula::rt::result_ok_ref(field);
}

RtResult<std::int64_t> json_int_field(const RtJson& value, std::string_view key, std::string_view label) {
  auto field = nebula::rt::json_get_int(value, key);
  if (rt_is_err(field)) {
    return make_err<std::int64_t>(std::string(label) + " missing int field: " + std::string(key));
  }
  return field;
}

RtResult<RtJson> json_value_field(const RtJson& value, std::string_view key, std::string_view label) {
  auto field = nebula::rt::json_get_value(value, key);
  if (rt_is_err(field)) {
    return make_err<RtJson>(std::string(label) + " missing field: " + std::string(key));
  }
  return field;
}

int base64url_value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
  if (ch >= '0' && ch <= '9') return ch - '0' + 52;
  if (ch == '-') return 62;
  if (ch == '_') return 63;
  return -1;
}

RtResult<std::vector<std::uint8_t>> base64url_decode(std::string_view input, std::string_view label) {
  if (input.empty()) {
    return make_err<std::vector<std::uint8_t>>(std::string(label) + " must be non-empty");
  }
  std::vector<std::uint8_t> out;
  out.reserve((input.size() * 3) / 4 + 3);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (char ch : input) {
    if (ch == '=') {
      return make_err<std::vector<std::uint8_t>>(std::string(label) + " must be unpadded base64url");
    }
    const int value = base64url_value(ch);
    if (value < 0) {
      return make_err<std::vector<std::uint8_t>>(std::string(label) + " contains invalid base64url character");
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    while (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xffu));
    }
  }
  if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) {
    return make_err<std::vector<std::uint8_t>>(std::string(label) + " has non-zero trailing base64url bits");
  }
  return make_ok(std::move(out));
}

RtResult<std::string> base64url_decode_text(std::string_view input, std::string_view label) {
  auto bytes = base64url_decode(input, label);
  if (rt_is_err(bytes)) return make_err<std::string>(rt_err(bytes));
  const auto& value = nebula::rt::result_ok_ref(bytes);
  return make_ok(std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

struct CompactJwt {
  std::string header_b64;
  std::string payload_b64;
  std::string signature_b64;
  std::string signing_input;
};

RtResult<CompactJwt> split_jwt(std::string token) {
  const auto first = token.find('.');
  if (first == std::string::npos) return make_err<CompactJwt>("jwt must have three compact parts");
  const auto second = token.find('.', first + 1);
  if (second == std::string::npos || token.find('.', second + 1) != std::string::npos) {
    return make_err<CompactJwt>("jwt must have three compact parts");
  }
  CompactJwt out;
  out.header_b64 = token.substr(0, first);
  out.payload_b64 = token.substr(first + 1, second - first - 1);
  out.signature_b64 = token.substr(second + 1);
  if (out.header_b64.empty() || out.payload_b64.empty() || out.signature_b64.empty()) {
    return make_err<CompactJwt>("jwt compact parts must be non-empty");
  }
  out.signing_input = out.header_b64 + "." + out.payload_b64;
  return make_ok(std::move(out));
}

std::string trim_ascii_ws(std::string text) {
  std::size_t start = 0;
  while (start < text.size()) {
    const char ch = text[start];
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
    start += 1;
  }
  std::size_t end = text.size();
  while (end > start) {
    const char ch = text[end - 1];
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
    end -= 1;
  }
  return text.substr(start, end - start);
}

struct BigInt {
  std::vector<std::uint32_t> limbs;
};

void normalize(BigInt& value) {
  while (!value.limbs.empty() && value.limbs.back() == 0) value.limbs.pop_back();
}

BigInt big_from_u32(std::uint32_t value) {
  BigInt out;
  if (value != 0) out.limbs.push_back(value);
  return out;
}

BigInt big_from_be_bytes(const std::vector<std::uint8_t>& bytes) {
  BigInt out;
  std::uint32_t limb = 0;
  int shift = 0;
  for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
    limb |= static_cast<std::uint32_t>(*it) << shift;
    shift += 8;
    if (shift == 32) {
      out.limbs.push_back(limb);
      limb = 0;
      shift = 0;
    }
  }
  if (shift != 0) out.limbs.push_back(limb);
  normalize(out);
  return out;
}

bool big_is_zero(const BigInt& value) {
  return value.limbs.empty();
}

bool big_is_odd(const BigInt& value) {
  return !value.limbs.empty() && (value.limbs[0] & 1u) == 1u;
}

int big_compare(const BigInt& lhs, const BigInt& rhs) {
  if (lhs.limbs.size() != rhs.limbs.size()) return lhs.limbs.size() < rhs.limbs.size() ? -1 : 1;
  for (std::size_t i = lhs.limbs.size(); i > 0; --i) {
    const auto a = lhs.limbs[i - 1];
    const auto b = rhs.limbs[i - 1];
    if (a != b) return a < b ? -1 : 1;
  }
  return 0;
}

BigInt big_add(const BigInt& lhs, const BigInt& rhs) {
  BigInt out;
  const std::size_t count = std::max(lhs.limbs.size(), rhs.limbs.size());
  out.limbs.resize(count);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t a = i < lhs.limbs.size() ? lhs.limbs[i] : 0;
    const std::uint64_t b = i < rhs.limbs.size() ? rhs.limbs[i] : 0;
    const std::uint64_t sum = a + b + carry;
    out.limbs[i] = static_cast<std::uint32_t>(sum);
    carry = sum >> 32;
  }
  if (carry != 0) out.limbs.push_back(static_cast<std::uint32_t>(carry));
  normalize(out);
  return out;
}

BigInt big_subtract(const BigInt& lhs, const BigInt& rhs) {
  BigInt out;
  out.limbs.resize(lhs.limbs.size());
  std::uint64_t borrow = 0;
  for (std::size_t i = 0; i < lhs.limbs.size(); ++i) {
    const std::uint64_t a = lhs.limbs[i];
    const std::uint64_t b = i < rhs.limbs.size() ? rhs.limbs[i] : 0;
    const std::uint64_t subtrahend = b + borrow;
    if (a >= subtrahend) {
      out.limbs[i] = static_cast<std::uint32_t>(a - subtrahend);
      borrow = 0;
    } else {
      out.limbs[i] = static_cast<std::uint32_t>((std::uint64_t{1} << 32) + a - subtrahend);
      borrow = 1;
    }
  }
  normalize(out);
  return out;
}

void big_shift_right_one(BigInt& value) {
  std::uint32_t carry = 0;
  for (std::size_t i = value.limbs.size(); i > 0; --i) {
    const std::uint32_t next_carry = value.limbs[i - 1] & 1u;
    value.limbs[i - 1] = (value.limbs[i - 1] >> 1) | (carry << 31);
    carry = next_carry;
  }
  normalize(value);
}

BigInt big_add_mod(const BigInt& lhs, const BigInt& rhs, const BigInt& modulus) {
  BigInt out = big_add(lhs, rhs);
  if (big_compare(out, modulus) >= 0) out = big_subtract(out, modulus);
  return out;
}

BigInt big_mod_from_be_bytes(const std::vector<std::uint8_t>& bytes, const BigInt& modulus) {
  BigInt out;
  for (std::uint8_t byte : bytes) {
    for (int i = 0; i < 8; ++i) out = big_add_mod(out, out, modulus);
    if (byte != 0) out = big_add_mod(out, big_from_u32(byte), modulus);
  }
  return out;
}

BigInt big_mul_mod(BigInt lhs, BigInt rhs, const BigInt& modulus) {
  BigInt out;
  while (!big_is_zero(rhs)) {
    if (big_is_odd(rhs)) out = big_add_mod(out, lhs, modulus);
    big_shift_right_one(rhs);
    if (!big_is_zero(rhs)) lhs = big_add_mod(lhs, lhs, modulus);
  }
  return out;
}

BigInt big_pow_mod(BigInt base, BigInt exponent, const BigInt& modulus) {
  BigInt out = big_from_u32(1);
  while (!big_is_zero(exponent)) {
    if (big_is_odd(exponent)) out = big_mul_mod(out, base, modulus);
    big_shift_right_one(exponent);
    if (!big_is_zero(exponent)) base = big_mul_mod(base, base, modulus);
  }
  return out;
}

std::vector<std::uint8_t> big_to_fixed_be_bytes(const BigInt& value, std::size_t width) {
  std::vector<std::uint8_t> out(width, 0);
  for (std::size_t limb_index = 0; limb_index < value.limbs.size(); ++limb_index) {
    std::uint32_t limb = value.limbs[limb_index];
    for (int byte_index = 0; byte_index < 4; ++byte_index) {
      const std::size_t little_offset = limb_index * 4 + byte_index;
      if (little_offset >= width) break;
      out[width - little_offset - 1] = static_cast<std::uint8_t>(limb & 0xffu);
      limb >>= 8;
    }
  }
  return out;
}

std::uint32_t sha256_rotr(std::uint32_t value, int bits) {
  return (value >> bits) | (value << (32 - bits));
}

std::uint32_t sha256_load_be32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

void sha256_store_be32(std::uint32_t value, std::uint8_t* out) {
  out[0] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
  out[1] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
  out[2] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
  out[3] = static_cast<std::uint8_t>(value & 0xffu);
}

void sha256_store_be64(std::uint64_t value, std::uint8_t* out) {
  for (int i = 7; i >= 0; --i) {
    out[7 - i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
  }
}

void sha256_process_block(const std::uint8_t* block, std::array<std::uint32_t, 8>& state) {
  static constexpr std::uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  std::array<std::uint32_t, 64> words = {};
  for (int i = 0; i < 16; ++i) words[i] = sha256_load_be32(block + (i * 4));
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = sha256_rotr(words[i - 15], 7) ^ sha256_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
    const std::uint32_t s1 = sha256_rotr(words[i - 2], 17) ^ sha256_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }
  std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + k[i] + words[i];
    const std::uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

std::array<std::uint8_t, 32> sha256_digest(std::string_view data) {
  std::array<std::uint32_t, 8> state = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  std::size_t offset = 0;
  while (offset + 64 <= data.size()) {
    sha256_process_block(reinterpret_cast<const std::uint8_t*>(data.data() + offset), state);
    offset += 64;
  }
  std::array<std::uint8_t, 128> tail = {};
  const std::size_t remaining = data.size() - offset;
  for (std::size_t i = 0; i < remaining; ++i) {
    tail[i] = static_cast<std::uint8_t>(data[offset + i]);
  }
  tail[remaining] = 0x80u;
  const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
  const std::size_t length_offset = remaining + 1 + 8 <= 64 ? 56 : 120;
  sha256_store_be64(bit_len, tail.data() + length_offset);
  sha256_process_block(tail.data(), state);
  if (length_offset == 120) sha256_process_block(tail.data() + 64, state);
  std::array<std::uint8_t, 32> out = {};
  for (std::size_t i = 0; i < state.size(); ++i) sha256_store_be32(state[i], out.data() + (i * 4));
  return out;
}

bool verify_pkcs1_sha256_block(const std::vector<std::uint8_t>& block, const std::array<std::uint8_t, 32>& digest) {
  static constexpr std::array<std::uint8_t, 19> prefix = {
      0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
      0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
  if (block.size() < 11 + prefix.size() + digest.size()) return false;
  if (block[0] != 0x00 || block[1] != 0x01) return false;
  std::size_t index = 2;
  while (index < block.size() && block[index] == 0xff) ++index;
  if (index < 10 || index >= block.size() || block[index] != 0x00) return false;
  ++index;
  if (block.size() - index != prefix.size() + digest.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < prefix.size(); ++i) diff |= block[index + i] ^ prefix[i];
  index += prefix.size();
  for (std::size_t i = 0; i < digest.size(); ++i) diff |= block[index + i] ^ digest[i];
  return diff == 0;
}

RtResult<bool> verify_rs256_signature(const std::string& signing_input,
                                      const std::vector<std::uint8_t>& signature,
                                      const std::vector<std::uint8_t>& modulus_bytes,
                                      const std::vector<std::uint8_t>& exponent_bytes) {
  if (modulus_bytes.size() < 128) return make_err<bool>("jwks RSA modulus is too small");
  if (signature.size() != modulus_bytes.size()) return make_err<bool>("jwt signature length does not match jwk modulus");
  BigInt modulus = big_from_be_bytes(modulus_bytes);
  if (big_is_zero(modulus)) return make_err<bool>("jwks RSA modulus is invalid");
  BigInt signature_mod = big_mod_from_be_bytes(signature, modulus);
  BigInt exponent = big_from_be_bytes(exponent_bytes);
  if (big_is_zero(exponent)) return make_err<bool>("jwks RSA exponent is invalid");
  BigInt opened = big_pow_mod(signature_mod, exponent, modulus);
  const auto block = big_to_fixed_be_bytes(opened, modulus_bytes.size());
  return make_ok(verify_pkcs1_sha256_block(block, sha256_digest(signing_input)));
}

struct RsaJwk {
  std::string kid;
  std::string n;
  std::string e;
};

RtResult<RsaJwk> jwk_from_json(const RtJson& value, std::string_view label) {
  auto kty = json_string_field(value, "kty", label);
  if (rt_is_err(kty)) return make_err<RsaJwk>(rt_err(kty));
  if (nebula::rt::result_ok_ref(kty) != "RSA") return make_err<RsaJwk>("jwks key kty must be RSA");
  auto kid = json_string_field(value, "kid", label);
  if (rt_is_err(kid)) return make_err<RsaJwk>(rt_err(kid));
  auto n = json_string_field(value, "n", label);
  if (rt_is_err(n)) return make_err<RsaJwk>(rt_err(n));
  auto e = json_string_field(value, "e", label);
  if (rt_is_err(e)) return make_err<RsaJwk>(rt_err(e));
  if (auto alg = json_optional_string_field(value, "alg"); alg.has_value() && *alg != "RS256") {
    return make_err<RsaJwk>("jwks key alg must be RS256 when present");
  }
  return make_ok(RsaJwk{nebula::rt::result_ok_ref(kid),
                        nebula::rt::result_ok_ref(n),
                        nebula::rt::result_ok_ref(e)});
}

RtResult<RtJson> jwks_keys(const std::string& jwks_text) {
  auto jwks = parse_json(jwks_text, "jwks");
  if (rt_is_err(jwks)) return make_err<RtJson>(rt_err(jwks));
  return json_value_field(nebula::rt::result_ok_ref(jwks), "keys", "jwks");
}

RtResult<RsaJwk> find_jwk(const std::string& jwks_text, const std::string& kid) {
  auto keys = jwks_keys(jwks_text);
  if (rt_is_err(keys)) return make_err<RsaJwk>(rt_err(keys));
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(keys));
  if (rt_is_err(count)) return make_err<RsaJwk>("jwks keys must be an array");
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto key_value = nebula::rt::json_array_get(nebula::rt::result_ok_ref(keys), i);
    if (rt_is_err(key_value)) return make_err<RsaJwk>(rt_err(key_value));
    auto key = jwk_from_json(nebula::rt::result_ok_ref(key_value), "jwks key");
    if (rt_is_err(key)) return make_err<RsaJwk>(rt_err(key));
    if (nebula::rt::result_ok_ref(key).kid == kid) return key;
  }
  return make_err<RsaJwk>("jwt kid is not present in JWKS");
}

RtResult<bool> validate_jwks(const std::string& jwks_text) {
  auto keys = jwks_keys(jwks_text);
  if (rt_is_err(keys)) return make_err<bool>(rt_err(keys));
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(keys));
  if (rt_is_err(count)) return make_err<bool>("jwks keys must be an array");
  if (nebula::rt::result_ok_ref(count) == 0) return make_err<bool>("jwks keys must not be empty");
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto key_value = nebula::rt::json_array_get(nebula::rt::result_ok_ref(keys), i);
    if (rt_is_err(key_value)) return make_err<bool>(rt_err(key_value));
    auto key = jwk_from_json(nebula::rt::result_ok_ref(key_value), "jwks key");
    if (rt_is_err(key)) return make_err<bool>(rt_err(key));
    auto n = base64url_decode(nebula::rt::result_ok_ref(key).n, "jwks n");
    if (rt_is_err(n)) return make_err<bool>(rt_err(n));
    auto e = base64url_decode(nebula::rt::result_ok_ref(key).e, "jwks e");
    if (rt_is_err(e)) return make_err<bool>(rt_err(e));
    if (nebula::rt::result_ok_ref(n).size() < 128) return make_err<bool>("jwks RSA modulus is too small");
    if (nebula::rt::result_ok_ref(e).empty()) return make_err<bool>("jwks RSA exponent is invalid");
  }
  return make_ok(true);
}

bool audience_matches_string_or_array(const RtJson& payload, const std::string& expected) {
  auto aud_value = nebula::rt::json_get_value(payload, "aud");
  if (rt_is_err(aud_value)) return false;
  auto aud_string = nebula::rt::json_as_string(nebula::rt::result_ok_ref(aud_value));
  if (!rt_is_err(aud_string)) return nebula::rt::result_ok_ref(aud_string) == expected;
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(aud_value));
  if (rt_is_err(count)) return false;
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto item = nebula::rt::json_array_get(nebula::rt::result_ok_ref(aud_value), i);
    if (rt_is_err(item)) return false;
    auto text = nebula::rt::json_as_string(nebula::rt::result_ok_ref(item));
    if (!rt_is_err(text) && nebula::rt::result_ok_ref(text) == expected) return true;
  }
  return false;
}

RtResult<RtJson> roles_array_from_payload(const RtJson& payload) {
  auto roles = json_value_field(payload, "roles", "jwt payload");
  if (rt_is_err(roles)) return make_err<RtJson>(rt_err(roles));
  auto count = nebula::rt::json_array_len(nebula::rt::result_ok_ref(roles));
  if (rt_is_err(count)) return make_err<RtJson>("jwt roles claim must be an array");
  auto builder = nebula::rt::json_array_builder();
  for (std::int64_t i = 0; i < nebula::rt::result_ok_ref(count); ++i) {
    auto item = nebula::rt::json_array_get(nebula::rt::result_ok_ref(roles), i);
    if (rt_is_err(item)) return make_err<RtJson>(rt_err(item));
    auto role = nebula::rt::json_as_string(nebula::rt::result_ok_ref(item));
    if (rt_is_err(role)) return make_err<RtJson>("jwt roles claim must contain only strings");
    builder = nebula::rt::json_array_push(std::move(builder),
                                          nebula::rt::json_string_value(nebula::rt::result_ok_ref(role)));
  }
  return make_ok(nebula::rt::json_array_build(std::move(builder)));
}

RtResult<RtJson> verify_jwt_claims(const RtJson& header,
                                   const RtJson& payload,
                                   const CompactJwt& compact,
                                   const std::string& jwks_text,
                                   const std::string& issuer,
                                   const std::string& audience,
                                   std::int64_t now_unix_s) {
  auto alg = json_string_field(header, "alg", "jwt header");
  if (rt_is_err(alg)) return make_err<RtJson>(rt_err(alg));
  if (nebula::rt::result_ok_ref(alg) != "RS256") return make_err<RtJson>("jwt alg must be RS256");
  auto kid = json_string_field(header, "kid", "jwt header");
  if (rt_is_err(kid)) return make_err<RtJson>(rt_err(kid));
  auto key = find_jwk(jwks_text, nebula::rt::result_ok_ref(kid));
  if (rt_is_err(key)) return make_err<RtJson>(rt_err(key));
  auto modulus = base64url_decode(nebula::rt::result_ok_ref(key).n, "jwks n");
  if (rt_is_err(modulus)) return make_err<RtJson>(rt_err(modulus));
  auto exponent = base64url_decode(nebula::rt::result_ok_ref(key).e, "jwks e");
  if (rt_is_err(exponent)) return make_err<RtJson>(rt_err(exponent));
  auto signature = base64url_decode(compact.signature_b64, "jwt signature");
  if (rt_is_err(signature)) return make_err<RtJson>(rt_err(signature));
  auto verified = verify_rs256_signature(compact.signing_input,
                                         nebula::rt::result_ok_ref(signature),
                                         nebula::rt::result_ok_ref(modulus),
                                         nebula::rt::result_ok_ref(exponent));
  if (rt_is_err(verified)) return make_err<RtJson>(rt_err(verified));
  if (!nebula::rt::result_ok_ref(verified)) return make_err<RtJson>("jwt signature is invalid");

  auto iss = json_string_field(payload, "iss", "jwt payload");
  if (rt_is_err(iss)) return make_err<RtJson>(rt_err(iss));
  if (nebula::rt::result_ok_ref(iss) != issuer) return make_err<RtJson>("jwt issuer mismatch");
  if (!audience_matches_string_or_array(payload, audience)) return make_err<RtJson>("jwt audience mismatch");
  auto sub = json_string_field(payload, "sub", "jwt payload");
  if (rt_is_err(sub)) return make_err<RtJson>(rt_err(sub));
  if (nebula::rt::result_ok_ref(sub).empty()) return make_err<RtJson>("jwt subject must be non-empty");
  auto exp = json_int_field(payload, "exp", "jwt payload");
  if (rt_is_err(exp)) return make_err<RtJson>(rt_err(exp));
  if (now_unix_s >= nebula::rt::result_ok_ref(exp)) return make_err<RtJson>("jwt is expired");
  std::int64_t nbf_value = 0;
  auto nbf = nebula::rt::json_get_int(payload, "nbf");
  if (!rt_is_err(nbf)) {
    nbf_value = nebula::rt::result_ok_ref(nbf);
    if (now_unix_s < nbf_value) return make_err<RtJson>("jwt is not valid yet");
  }
  auto roles = roles_array_from_payload(payload);
  if (rt_is_err(roles)) return make_err<RtJson>(rt_err(roles));
  return make_ok(nebula::rt::json_object6("subject",
                                          nebula::rt::json_string_value(nebula::rt::result_ok_ref(sub)),
                                          "issuer",
                                          nebula::rt::json_string_value(nebula::rt::result_ok_ref(iss)),
                                          "audience",
                                          nebula::rt::json_string_value(audience),
                                          "expires_unix_s",
                                          nebula::rt::json_int_value(nebula::rt::result_ok_ref(exp)),
                                          "not_before_unix_s",
                                          nebula::rt::json_int_value(nbf_value),
                                          "roles",
                                          nebula::rt::result_ok_ref(roles)));
}

RtResult<RtJson> verify_token(std::string token,
                              std::string jwks_text,
                              std::string issuer,
                              std::string audience,
                              std::int64_t now_unix_s) {
  if (issuer.empty()) return make_err<RtJson>("jwt issuer must be configured");
  if (audience.empty()) return make_err<RtJson>("jwt audience must be configured");
  auto compact = split_jwt(trim_ascii_ws(std::move(token)));
  if (rt_is_err(compact)) return make_err<RtJson>(rt_err(compact));
  auto header_text = base64url_decode_text(nebula::rt::result_ok_ref(compact).header_b64, "jwt header");
  if (rt_is_err(header_text)) return make_err<RtJson>(rt_err(header_text));
  auto payload_text = base64url_decode_text(nebula::rt::result_ok_ref(compact).payload_b64, "jwt payload");
  if (rt_is_err(payload_text)) return make_err<RtJson>(rt_err(payload_text));
  auto header = parse_json(nebula::rt::result_ok_ref(header_text), "jwt header");
  if (rt_is_err(header)) return make_err<RtJson>(rt_err(header));
  auto payload = parse_json(nebula::rt::result_ok_ref(payload_text), "jwt payload");
  if (rt_is_err(payload)) return make_err<RtJson>(rt_err(payload));
  return verify_jwt_claims(nebula::rt::result_ok_ref(header),
                           nebula::rt::result_ok_ref(payload),
                           nebula::rt::result_ok_ref(compact),
                           jwks_text,
                           issuer,
                           audience,
                           now_unix_s);
}

RtResult<std::string> bearer_from_authorization_header(const std::string& header_value) {
  static constexpr std::string_view prefix = "Bearer ";
  if (header_value.size() <= prefix.size() ||
      std::string_view(header_value).substr(0, prefix.size()) != prefix) {
    return make_err<std::string>("authorization header must use Bearer token");
  }
  return make_ok(header_value.substr(prefix.size()));
}

}  // namespace

RtResult<bool> __nebula_auth_validate_rs256_jwks(std::string jwks_text) {
  return validate_jwks(jwks_text);
}

RtResult<RtJson> __nebula_auth_verify_rs256_jwt(std::string token,
                                                std::string jwks_text,
                                                std::string issuer,
                                                std::string audience,
                                                std::int64_t now_unix_s) {
  return verify_token(std::move(token), std::move(jwks_text), std::move(issuer), std::move(audience), now_unix_s);
}

RtResult<RtJson> __nebula_auth_verify_rs256_authorization_header(std::string header_value,
                                                                 std::string jwks_text,
                                                                 std::string issuer,
                                                                 std::string audience,
                                                                 std::int64_t now_unix_s) {
  auto token = bearer_from_authorization_header(header_value);
  if (rt_is_err(token)) return make_err<RtJson>(rt_err(token));
  return verify_token(nebula::rt::result_ok_ref(token),
                      std::move(jwks_text),
                      std::move(issuer),
                      std::move(audience),
                      now_unix_s);
}
