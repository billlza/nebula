#pragma once

#include <cstdint>

namespace nebula::frontend {

enum class RuntimeProfile : std::uint8_t { Hosted, System };
enum class PanicPolicy : std::uint8_t { Abort, Trap, Unwind };

inline bool is_system_profile(RuntimeProfile profile) {
  return profile == RuntimeProfile::System;
}

inline bool effective_no_std(const RuntimeProfile profile, const bool no_std) {
  return no_std || is_system_profile(profile);
}

inline bool effective_strict_region(const RuntimeProfile profile, const bool strict_region) {
  return strict_region || is_system_profile(profile);
}

inline bool panic_policy_requires_host_unwind(PanicPolicy policy) {
  return policy == PanicPolicy::Unwind;
}

} // namespace nebula::frontend
