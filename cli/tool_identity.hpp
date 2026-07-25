#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nebula::cli {

// Immutable content identity for one executable selected at a toolchain
// boundary. Version text is bounded capability evidence; path, size, and
// SHA-256 are the executable identity used for revalidation.
struct ResolvedToolIdentity {
  std::filesystem::path executable;
  std::uintmax_t size = 0U;
  std::string sha256;
  std::string version;
};

struct ResolvedToolDependency {
  std::string role;
  ResolvedToolIdentity identity;
};

} // namespace nebula::cli
