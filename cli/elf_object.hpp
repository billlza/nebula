#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace nebula::cli {

inline constexpr std::size_t kMaxFreestandingObjectBytes = 64U * 1024U * 1024U;

struct ElfInspectionResult {
  bool valid = false;
  std::string reason;
  std::string detail;

  [[nodiscard]] bool ok() const { return valid; }
};

// Validate the exact ELF64 relocatable-object contract accepted by the first
// x86_64 freestanding gate. Every offset/count is checked before dereference.
ElfInspectionResult inspect_freestanding_elf64_x86_64(std::span<const std::uint8_t> bytes);

} // namespace nebula::cli
