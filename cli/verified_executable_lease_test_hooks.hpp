#pragma once

#if !defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
#error "verified executable lease hooks are test-only"
#endif

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace nebula::cli::verified_executable_lease_testing {

enum class AcquisitionExceptionPoint : std::uint8_t {
  AfterPrivateEntryCreation,
  AfterWritableSnapshotClosed,
  BeforeImplementationAllocation,
  AfterOwnershipTransfer,
};

using AcquisitionExceptionSetup = void (*)(const std::filesystem::path &lease_path);

// Throws one fixed exception at the selected acquisition phase. setup runs
// immediately before the throw so a test can establish a real path-identity
// conflict without adding production-only branching to the rollback logic.
void inject_acquisition_exception_once(AcquisitionExceptionPoint point,
                                       AcquisitionExceptionSetup setup = nullptr);
[[nodiscard]] bool acquisition_exception_injection_pending() noexcept;

// Forces one structured acquisition failure immediately before implementation
// ownership is allocated. setup can replace the just-created path so the real
// identity-bound rollback returns CleanupIncomplete to its caller.
void inject_acquisition_failure_once(AcquisitionExceptionSetup setup = nullptr);
[[nodiscard]] bool acquisition_failure_injection_pending() noexcept;

// Throws after the first identity-bound rollback attempt reports failure but
// before its typed diagnostic is allocated. This verifies that destructor
// fallback never performs a second filesystem rollback attempt.
void inject_post_rollback_diagnostic_exception_once() noexcept;
[[nodiscard]] bool post_rollback_diagnostic_exception_pending() noexcept;

// Makes one explicit cleanup attempt report owned cleanup incomplete without
// touching the private entry. Destructor tests use this to prove fail-fast
// behavior while normal callers retain the real typed cleanup path.
void inject_cleanup_failure_once() noexcept;
[[nodiscard]] bool cleanup_failure_injection_pending() noexcept;

// Runs one test setup immediately after the deletion-marked Windows lease
// handle closes and before cleanup checks whether the path is absent. This
// deterministically models a foreign object claiming the just-released name.
void inject_post_deletion_cleanup_setup_once(AcquisitionExceptionSetup setup);
[[nodiscard]] bool post_deletion_cleanup_setup_pending() noexcept;

inline constexpr std::string_view kInjectedAcquisitionExceptionDetail =
  "injected verified executable lease acquisition exception";
inline constexpr std::string_view kInjectedAcquisitionFailureDetail =
  "injected verified executable lease acquisition failure";
inline constexpr std::string_view kInjectedPostRollbackDiagnosticExceptionDetail =
  "injected post-rollback diagnostic exception";

} // namespace nebula::cli::verified_executable_lease_testing
