#pragma once

#include "tool_identity.hpp"
#include "verified_executable_lease.hpp"

#include "termination_signal.hpp"

#include "boot/protocol_abi_contract.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>

namespace nebula::cli {

inline constexpr std::string_view kFreestandingTargetTriple = nebula::boot::kUosX86_64TargetTriple;
inline constexpr const char *kFreestandingCommandSchema = "x86_64-none-clang-cxx20-v4";

enum class FreestandingToolchainErrorCode : std::uint8_t {
  None,
  HostUnsupported,
  InvalidRoot,
  MissingCompiler,
  UntrustedCompiler,
  Identity,
  Query,
  Capability,
  SignalBoundary,
  Interrupted,
  Cleanup,
};

struct FreestandingToolchainError {
  FreestandingToolchainErrorCode code = FreestandingToolchainErrorCode::None;
  std::string detail;
};

struct FreestandingToolchainRequest {
  // This is an explicit, caller-selected root. The resolver never consults
  // PATH and accepts only <root>/bin/clang++ for the v4 object contract.
  std::filesystem::path toolchain_root;
  std::filesystem::path self_executable;
};

struct FreestandingToolchainCloseResult {
  int interrupted_signal = 0;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return detail.empty(); }
};

enum class FreestandingToolchainSessionState : std::uint8_t {
  Executable,
  Closing,
  PreparedFrozen,
  Closed,
};

enum class FreestandingExternalCleanup : std::uint8_t {
  Complete,
  Incomplete,
};

struct FreestandingToolchainPrepareResult {
  int observed_signal = 0;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return detail.empty(); }
};

struct FreestandingToolchainResolutionResult;

class ResolvedFreestandingToolchain final {
public:
  ResolvedFreestandingToolchain(const ResolvedFreestandingToolchain &) = delete;
  ResolvedFreestandingToolchain(ResolvedFreestandingToolchain &&other) noexcept;
  ResolvedFreestandingToolchain &operator=(const ResolvedFreestandingToolchain &) = delete;
  ResolvedFreestandingToolchain &operator=(ResolvedFreestandingToolchain &&) noexcept = delete;
  ~ResolvedFreestandingToolchain() noexcept;

  [[nodiscard]] const std::filesystem::path &root() const noexcept { return root_; }
  [[nodiscard]] const ResolvedToolIdentity &compiler() const noexcept { return compiler_; }
  [[nodiscard]] const ResolvedToolIdentity &nebula_executable() const noexcept {
    return nebula_executable_;
  }
  [[nodiscard]] const std::string &target_triple() const noexcept { return target_triple_; }
  [[nodiscard]] const std::filesystem::path &compiler_execution_path() const noexcept {
    return compiler_lease_->execution_path();
  }
  [[nodiscard]] bool compiler_snapshot_active() const noexcept {
    return compiler_lease_ != nullptr && compiler_lease_->active();
  }
  [[nodiscard]] bool session_active() const noexcept {
    return termination_signals_ != nullptr &&
           session_state_ != FreestandingToolchainSessionState::Closed;
  }
  [[nodiscard]] bool session_executable() const noexcept {
    return session_state_ == FreestandingToolchainSessionState::Executable;
  }
  [[nodiscard]] FreestandingToolchainSessionState session_state() const noexcept {
    return session_state_;
  }
  [[nodiscard]] bool signal_redelivery_safe() const noexcept { return signal_redelivery_safe_; }
  [[nodiscard]] CompilerTerminationSignalScope &termination_signals() noexcept {
    return *termination_signals_;
  }
  // Permanently prevents this resolution session from handing an intercepted
  // caller signal back to the dispatch boundary. This is monotonic: once any
  // compiler execution loses confirmed containment, later cleanup cannot make
  // redelivery safe retroactively.
  void mark_signal_redelivery_unsafe() noexcept;
  // Executes only the resolved compiler snapshot. arguments[0] must retain the
  // canonical public compiler path used by provenance; the native execution
  // path is always the private verified lease.
  [[nodiscard]] HostProcessResult execute_compiler(HostProcessRequest request) const;
  [[nodiscard]] bool revalidate(std::string &detail) const;
  // Retires the private compiler snapshot after the only compilation in this
  // resolution. A build must call this before publishing any artifact so a
  // cleanup failure remains an explicit infrastructure failure.
  [[nodiscard]] bool cleanup_compiler_snapshot(std::string &detail);
  // Begins the irreversible close transition. Compiler execution is disabled
  // before caller termination signals are frozen and the verified compiler
  // snapshot is retired. On failure the session remains Closing so its owner
  // can retry without reopening execution.
  [[nodiscard]] FreestandingToolchainPrepareResult prepare_session_close();
  // Restores caller signal state only after the owner has explicitly accounted
  // for every external resource. Incomplete cleanup permanently suppresses an
  // intercepted signal handoff, but restoration is still attempted from the
  // PreparedFrozen state so the caller is not left with an installed handler or
  // blocked mask.
  [[nodiscard]] FreestandingToolchainCloseResult
  finalize_session_close(FreestandingExternalCleanup external_cleanup);
  // Convenience close for call sites that own no transaction resources. Code
  // holding staging paths, rollback ownership, locks, handles, or similar
  // external state must use prepare_session_close/finalize_session_close.
  [[nodiscard]] FreestandingToolchainCloseResult close_session();
  [[nodiscard]] std::string provenance_identity() const;

private:
  friend struct FreestandingToolchainResolutionResult;
  friend FreestandingToolchainResolutionResult
  resolve_freestanding_toolchain(const FreestandingToolchainRequest &request);

  ResolvedFreestandingToolchain(std::filesystem::path root, ResolvedToolIdentity compiler,
                                ResolvedToolIdentity nebula_executable, std::string target_triple,
                                bool signal_redelivery_safe,
                                std::unique_ptr<CompilerTerminationSignalScope> termination_signals,
                                std::unique_ptr<VerifiedExecutableLease> compiler_lease) noexcept;

  std::filesystem::path root_;
  ResolvedToolIdentity compiler_;
  ResolvedToolIdentity nebula_executable_;
  std::string target_triple_;
  // Member order is deliberate: reverse destruction retires the executable
  // lease before the signal scope can restore/redeliver during exception-only
  // fallback cleanup.
  std::unique_ptr<CompilerTerminationSignalScope> termination_signals_;
  std::unique_ptr<VerifiedExecutableLease> compiler_lease_;
  bool signal_redelivery_safe_ = true;
  FreestandingToolchainSessionState session_state_ = FreestandingToolchainSessionState::Executable;
};

struct FreestandingToolchainResolutionResult {
  std::optional<ResolvedFreestandingToolchain> value;
  FreestandingToolchainError error;
  int interrupted_signal = 0;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error.code == FreestandingToolchainErrorCode::None &&
           error.detail.empty() && interrupted_signal == 0;
  }
};

// Resolves one immutable compiler/self snapshot from an explicit root, runs
// bounded identity/capability queries inside a termination/containment scope,
// and never falls back to PATH or another compiler.
[[nodiscard]] FreestandingToolchainResolutionResult
resolve_freestanding_toolchain(const FreestandingToolchainRequest &request);

} // namespace nebula::cli
