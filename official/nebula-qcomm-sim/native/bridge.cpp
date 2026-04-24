#include "runtime/nebula_runtime.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using RtBytes = nebula::rt::Bytes;
template <typename T>
using RtResult = nebula::rt::Result<T, std::string>;

template <typename T>
RtResult<T> make_err(std::string message) {
  return nebula::rt::err_result<T>(std::move(message));
}

template <typename T>
RtResult<T> make_ok(T value) {
  return nebula::rt::ok_result(std::move(value));
}

struct PcgState {
  std::uint64_t state = 0;
};

std::uint64_t seed_state(const RtBytes& seed) {
  std::uint64_t state = 0xcbf29ce484222325ULL;
  for (unsigned char ch : seed.data) {
    state ^= static_cast<std::uint64_t>(ch);
    state *= 0x100000001b3ULL;
  }
  if (state == 0) return 0x9e3779b97f4a7c15ULL;
  return state;
}

std::uint32_t next_u32(PcgState& state) {
  state.state = state.state * 6364136223846793005ULL + 1442695040888963407ULL;
  const std::uint64_t xorshifted = ((state.state >> 18u) ^ state.state) >> 27u;
  const std::uint32_t rot = static_cast<std::uint32_t>(state.state >> 59u);
  return static_cast<std::uint32_t>((xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u)));
}

bool next_bit(PcgState& state) {
  return (next_u32(state) & 1u) != 0u;
}

bool next_ppm(PcgState& state, std::int64_t ppm) {
  if (ppm <= 0) return false;
  if (ppm >= 1000000) return true;
  const std::uint32_t sample = next_u32(state) % 1000000u;
  return sample < static_cast<std::uint32_t>(ppm);
}

std::string hex_encode_bits(const std::vector<bool>& bits) {
  static constexpr char kHex[] = "0123456789abcdef";
  if (bits.empty()) return "";
  const std::size_t byte_count = (bits.size() + 7u) / 8u;
  std::string bytes(byte_count, '\0');
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (!bits[i]) continue;
    const std::size_t byte_index = i / 8u;
    const std::size_t bit_offset = 7u - (i % 8u);
    bytes[byte_index] = static_cast<char>(static_cast<unsigned char>(bytes[byte_index]) | (1u << bit_offset));
  }
  std::string out(byte_count * 2u, '\0');
  for (std::size_t i = 0; i < byte_count; ++i) {
    const unsigned char value = static_cast<unsigned char>(bytes[i]);
    out[i * 2u] = kHex[(value >> 4u) & 0x0fu];
    out[i * 2u + 1u] = kHex[value & 0x0fu];
  }
  return out;
}

RtResult<std::string> simulate_bb84(std::int64_t qubit_count,
                                    std::int64_t sample_reveal_bits,
                                    std::int64_t channel_flip_ppm,
                                    std::int64_t intercept_resend_ppm,
                                    const RtBytes& seed) {
  if (qubit_count <= 0) return make_err<std::string>("bb84 qubit_count must be positive");
  if (sample_reveal_bits < 0) return make_err<std::string>("bb84 sample_reveal_bits must be non-negative");
  if (sample_reveal_bits > qubit_count) {
    return make_err<std::string>("bb84 sample_reveal_bits must not exceed qubit_count");
  }
  if (channel_flip_ppm < 0 || channel_flip_ppm > 1000000) {
    return make_err<std::string>("bb84 channel_flip_ppm must be between 0 and 1000000");
  }
  if (intercept_resend_ppm < 0 || intercept_resend_ppm > 1000000) {
    return make_err<std::string>("bb84 intercept_resend_ppm must be between 0 and 1000000");
  }

  PcgState rng{seed_state(seed)};
  std::int64_t sample_count = 0;
  std::int64_t mismatches = 0;
  std::vector<bool> kept_bits;
  kept_bits.reserve(static_cast<std::size_t>(qubit_count));

  for (std::int64_t i = 0; i < qubit_count; ++i) {
    const bool alice_bit = next_bit(rng);
    const bool alice_basis = next_bit(rng);
    const bool bob_basis = next_bit(rng);
    bool bob_bit = next_bit(rng);

    if (next_ppm(rng, intercept_resend_ppm)) {
      const bool eve_basis = next_bit(rng);
      const bool eve_bit = (eve_basis == alice_basis) ? alice_bit : next_bit(rng);
      bob_bit = (bob_basis == eve_basis) ? eve_bit : next_bit(rng);
    } else if (bob_basis == alice_basis) {
      bob_bit = alice_bit;
    }

    if (next_ppm(rng, channel_flip_ppm)) {
      bob_bit = !bob_bit;
    }

    if (bob_basis != alice_basis) continue;

    if (sample_count < sample_reveal_bits) {
      sample_count += 1;
      if (bob_bit != alice_bit) mismatches += 1;
      continue;
    }
    kept_bits.push_back(bob_bit);
  }

  const std::int64_t qber_ppm =
      sample_count == 0 ? 0 : (mismatches * 1000000LL) / sample_count;
  const bool high_qber = qber_ppm >= 110000;
  const std::string abort_reason = high_qber ? "high_qber" : "";
  const std::string shared_key_hex = high_qber ? "" : hex_encode_bits(kept_bits);

  std::ostringstream json;
  json << "{\"raw_bits\":" << qubit_count << ",\"sifted_key_bits\":"
       << static_cast<std::int64_t>(kept_bits.size()) << ",\"qber_ppm\":" << qber_ppm
       << ",\"abort_reason\":\"" << abort_reason << "\",\"shared_key_hex\":\"" << shared_key_hex
       << "\"}";
  return make_ok<std::string>(json.str());
}

}  // namespace

RtResult<std::string> __nebula_qcomm_bb84_simulate(std::int64_t qubit_count,
                                                   std::int64_t sample_reveal_bits,
                                                   std::int64_t channel_flip_ppm,
                                                   std::int64_t intercept_resend_ppm,
                                                   bool /*fail_classical_auth*/,
                                                   RtBytes seed) {
  return simulate_bb84(qubit_count,
                       sample_reveal_bits,
                       channel_flip_ppm,
                       intercept_resend_ppm,
                       seed);
}
