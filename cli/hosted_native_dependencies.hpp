#pragma once

#include "artifact_digest.hpp"
#include "hosted_toolchain.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::cli {

struct HostedNativeDependencyLimits {
  std::size_t max_translation_units = 4096U;
  std::size_t max_dependencies = 1'000'000U;
  std::size_t max_encoded_path_bytes = 256U * 1024U * 1024U;
  std::uintmax_t max_depfile_bytes = 16U * 1024U * 1024U;
  std::uintmax_t max_total_depfile_bytes = 256U * 1024U * 1024U;
  std::uintmax_t max_file_bytes = kMaxReusableArtifactBytes;
  std::uintmax_t max_total_file_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
};

// compiler_arguments contains the exact language, standard, include, define,
// and source-local options used by the real compilation, but not argv[0], the
// source path, an output path, or dependency-generation control options.
struct HostedNativeDependencyUnit {
  std::filesystem::path source;
  std::vector<std::string> compiler_arguments;
  // Generated/probe source bytes are already bound by Nebula source provenance
  // and do not have a stable public path. Their transitive dependencies remain
  // in the snapshot while the source path itself is omitted.
  bool exclude_source_from_snapshot = false;
};

struct HostedNativeDependencyFile {
  // canonical_path is absolute. A compiler spelling that traverses a symlink
  // is resolved to the regular target which is actually hashed and protected.
  std::filesystem::path canonical_path;
  FileDigest content;

  bool operator==(const HostedNativeDependencyFile &) const = default;
};

struct HostedNativeDependencySnapshot {
  // Sorted by canonical_path and duplicate-free.
  std::vector<HostedNativeDependencyFile> files;
  // Binds every canonical path and exact content digest under a versioned
  // domain. This value is suitable for inclusion in hosted artifact metadata.
  std::string identity_sha256;

  bool operator==(const HostedNativeDependencySnapshot &) const = default;
};

struct HostedNativeDependencyDepfile {
  std::filesystem::path path;
  std::string expected_target;
  std::optional<std::filesystem::path> excluded_dependency;
};

struct HostedNativeDependencyDiscoveryResult {
  std::optional<HostedNativeDependencySnapshot> snapshot;
  // Zero on success. Compiler/preprocessor failures retain their compatible
  // exit status; infrastructure, bounds, parsing, stability, and cleanup
  // failures use 125.
  int exit_code = 125;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return snapshot.has_value() && exit_code == 0 && detail.empty();
  }
};

// Runs the resolved Clang/GCC driver in full `-M` mode for every translation
// unit. Two complete discovery-and-hash passes must agree, so a changing or
// newly shadowing include cannot be collapsed into one reusable build key.
// scratch_directory must already be a private, non-symlink directory. POSIX
// enforces euid ownership and mode 0700; same-UID processes remain inside the
// caller-controlled trust boundary. Discovered POSIX spellings are resolved
// component-by-component: links and files must be root/euid-owned, mutable
// world paths require sticky-entry protection, and shared file writes fail.
// macOS additionally treats the named local `admin` group as an explicit
// platform-administrator boundary because Apple/Homebrew toolchain headers
// conventionally traverse group-writable roots. This is not isolation from a
// second local administrator UID. Windows callers must pass a directory created
// with Nebula's private workspace ACL. Every depfile is removed before this
// function returns; cleanup uncertainty fails.
[[nodiscard]] HostedNativeDependencyDiscoveryResult discover_hosted_native_dependencies(
  const ResolvedHostedToolchain &toolchain, const std::vector<HostedNativeDependencyUnit> &units,
  const std::filesystem::path &scratch_directory, const HostedNativeDependencyLimits &limits = {},
  std::uint32_t timeout_milliseconds = 5U * 60U * 1000U);

// Captures one bounded dependency pass. Pipeline callers pair this with the
// real compilation depfile (fresh build) or a second pass immediately before
// lease execution (reuse). The paired comparison provides stability without
// performing four full system-header scans per reuse command.
[[nodiscard]] HostedNativeDependencyDiscoveryResult discover_hosted_native_dependencies_once(
  const ResolvedHostedToolchain &toolchain, const std::vector<HostedNativeDependencyUnit> &units,
  const std::filesystem::path &scratch_directory, const HostedNativeDependencyLimits &limits = {},
  std::uint32_t timeout_milliseconds = 5U * 60U * 1000U);

// Consumes the `-MD -MF` files emitted by the real native compilation. Every
// depfile must live directly inside scratch_directory and name its caller-
// supplied fixed target. The files are removed on both success and failure;
// their canonical dependency closure is hashed once for comparison with the
// stable pre-compilation snapshot.
[[nodiscard]] HostedNativeDependencyDiscoveryResult collect_compiled_hosted_native_dependencies(
  const std::vector<HostedNativeDependencyDepfile> &depfiles,
  const std::filesystem::path &working_directory, const std::filesystem::path &scratch_directory,
  const HostedNativeDependencyLimits &limits = {});

namespace detail {

struct MakeDependencyParseResult {
  std::vector<std::string> dependencies;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return detail.empty(); }
};

// Strict parser for the single GNU make rule emitted by Clang/GCC `-M -MF`.
// Exposed only for platform-independent parser contract tests.
[[nodiscard]] MakeDependencyParseResult
parse_make_dependency_rule(std::string_view depfile, std::string_view expected_target,
                           std::size_t max_dependencies, std::size_t max_encoded_path_bytes);

} // namespace detail
} // namespace nebula::cli
