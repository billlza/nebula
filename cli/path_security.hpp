#pragma once

#include <filesystem>
#include <string>

namespace nebula::cli {

#if !defined(_WIN32)
// Validates one exact, non-symlink POSIX directory used for pathname-based
// compiler and executable access. It must be owned by the effective user or
// root. World-writable directories require sticky-entry protection.
// Non-sticky group-writable paths are rejected except on macOS when the group
// is the platform's named `admin` group: Apple and Homebrew toolchains
// conventionally traverse that explicit local-administrator boundary.
// Same-UID processes, and on macOS a second local administrator UID, remain
// inside the caller-controlled trust boundary.
[[nodiscard]] bool validate_owner_controlled_directory(const std::filesystem::path &path,
                                                       std::string &detail);

// Applies the same policy to every component of an existing canonical
// directory path.
[[nodiscard]] bool validate_owner_controlled_directory_chain(const std::filesystem::path &path,
                                                             std::string &detail);

// Validates a canonical executable/tool path used by pathname-based spawn.
// The final object must be a regular file owned by root or the effective user,
// must not be group/world writable, and must be executable by the effective
// credentials. This closes cross-UID replacement in sticky shared parents;
// same-UID replacement remains inside the documented caller trust boundary.
[[nodiscard]] bool validate_owner_controlled_executable(const std::filesystem::path &path,
                                                        std::string &detail);
#endif

} // namespace nebula::cli
