#pragma once

#include "host_process.hpp"
#include "tool_identity.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebula::cli {

struct HostedToolchainResolutionResult;
struct HostedToolchainResolverAccess;

struct HostedToolchainRequest {
  std::filesystem::path self_executable;
  std::string compiler_command;
  std::optional<std::string> cxx_standard_override;
  bool require_archiver = false;
  std::string archiver_command;
  CompilerTerminationSignalScope *termination_signals = nullptr;
};

// Canonical executable paths needed by hosted compilation. Resolving this
// preview performs no hashing and starts no child processes, so output
// transactions can reject aliases with tools before any compiler-controlled
// code runs.
struct ResolvedHostedToolPaths {
  std::filesystem::path compiler;
  std::optional<std::filesystem::path> archiver;
  std::filesystem::path nebula_executable;
};

enum class HostedToolPathFailureKind : std::uint8_t {
  None,
  InvalidCompilerCommand,
  CompilerUnavailable,
  CompilerUnsafe,
  ArchiverUnavailable,
  ArchiverUnsafe,
  NebulaExecutableUnavailable,
  NebulaExecutableUnsafe,
  Other,
};

struct HostedToolPathResolutionResult {
  std::optional<ResolvedHostedToolPaths> value;
  HostedToolPathFailureKind failure = HostedToolPathFailureKind::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && failure == HostedToolPathFailureKind::None && detail.empty();
  }
};

class ResolvedHostedToolchain final {
public:
  ResolvedHostedToolchain(const ResolvedHostedToolchain &) = default;
  ResolvedHostedToolchain(ResolvedHostedToolchain &&) noexcept = default;
  ResolvedHostedToolchain &operator=(const ResolvedHostedToolchain &) = default;
  ResolvedHostedToolchain &operator=(ResolvedHostedToolchain &&) noexcept = default;

  [[nodiscard]] const ResolvedToolIdentity &compiler() const noexcept { return compiler_; }
  [[nodiscard]] const std::optional<ResolvedToolIdentity> &archiver() const noexcept {
    return archiver_;
  }
  [[nodiscard]] const ResolvedToolIdentity &nebula_executable() const noexcept {
    return nebula_executable_;
  }
  [[nodiscard]] const std::string &target_triple() const noexcept { return target_triple_; }
  [[nodiscard]] const std::string &cxx_standard_flag() const noexcept { return cxx_standard_flag_; }
  [[nodiscard]] const std::string &environment_sha256() const noexcept {
    return environment_sha256_;
  }
  [[nodiscard]] const std::filesystem::path &working_directory() const noexcept {
    return working_directory_;
  }
  [[nodiscard]] const std::vector<ResolvedToolDependency> &compiler_dependencies() const noexcept {
    return compiler_dependencies_;
  }
  // Runs a resolved compiler/archiver command with the exact bounded,
  // non-inherited environment snapshot bound into provenance.
  [[nodiscard]] HostProcessResult execute(const std::vector<std::string> &arguments,
                                          std::uint32_t timeout_milliseconds) const;

  // Re-hashes every explicitly resolved executable and re-captures the compilation-affecting
  // environment. Callers use this immediately before and after compilation.
  [[nodiscard]] bool revalidate(std::string &detail) const;

  // Stable, length-delimited identity text suitable for inclusion in a
  // higher-level artifact build key. It contains hashes, never environment
  // values themselves.
  [[nodiscard]] std::string provenance_identity() const;

private:
  friend struct HostedToolchainResolverAccess;
  friend struct HostedToolchainResolutionResult;
  friend HostedToolchainResolutionResult
  resolve_hosted_toolchain(const HostedToolchainRequest &request);

  ResolvedHostedToolchain(ResolvedToolIdentity compiler,
                          std::optional<ResolvedToolIdentity> archiver,
                          ResolvedToolIdentity nebula_executable, std::string target_triple,
                          std::string cxx_standard_flag, std::filesystem::path working_directory,
                          std::vector<ResolvedToolDependency> compiler_dependencies,
                          std::vector<HostEnvironmentOverride> execution_environment,
                          std::string environment_sha256,
                          CompilerTerminationSignalScope *termination_signals);

  ResolvedToolIdentity compiler_;
  std::optional<ResolvedToolIdentity> archiver_;
  ResolvedToolIdentity nebula_executable_;
  std::string target_triple_;
  std::string cxx_standard_flag_;
  std::filesystem::path working_directory_;
  std::vector<ResolvedToolDependency> compiler_dependencies_;
  std::vector<HostEnvironmentOverride> execution_environment_;
  std::string environment_sha256_;
  CompilerTerminationSignalScope *termination_signals_ = nullptr;
};

struct HostedToolchainResolutionResult {
  std::optional<ResolvedHostedToolchain> value;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return value.has_value() && detail.empty(); }
};

// Resolves only canonical executable paths. This function never executes a
// compiler, archiver, or other child process.
[[nodiscard]] HostedToolPathResolutionResult
resolve_hosted_tool_paths(const HostedToolchainRequest &request);

// Resolves and fingerprints one hosted toolchain snapshot. Empty command
// fields select CXX/clang++ and llvm-ar/ar using the environment observed by
// this call. No command is interpreted by a shell.
[[nodiscard]] HostedToolchainResolutionResult
resolve_hosted_toolchain(const HostedToolchainRequest &request);

// Resolves and fingerprints the toolchain only if the command-selection
// environment still maps the request to the exact paths previewed earlier.
// The path comparison is completed before any child process is started.
[[nodiscard]] HostedToolchainResolutionResult
resolve_hosted_toolchain(const HostedToolchainRequest &request,
                         const ResolvedHostedToolPaths &expected_paths);

} // namespace nebula::cli
