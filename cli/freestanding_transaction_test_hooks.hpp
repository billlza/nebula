#pragma once

#if !defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
#error "freestanding transaction hooks are test-only"
#endif

#include <cstdint>

namespace nebula::cli::freestanding_transaction_testing {

enum class Phase : std::uint8_t {
  OutputLockOpened,
  StagingDirectoryCreated,
  SessionPrepared,
  StagingCleanupFinished,
  PublicationFinalized,
  GuardsDisarmed,
  OutputLockReleased,
  BeforeSessionFinalize,
};

enum class Fault : std::uint8_t {
  StagingPermissionsRollbackCleanup,
  BeforeStagingOwnershipTransfer,
  OutputLockAcquireRollbackClose,
  StagingCleanup,
  SecondPublication,
  PublicationRollback,
  OutputLockRelease,
  PostCleanupDiagnostic,
};

using PhaseObserver = void (*)(Phase);

// Installs one synchronous observer. The production transaction has no hook
// storage or branch because this API and its implementation are compiled only
// into the dedicated deterministic test target.
void set_phase_observer(PhaseObserver observer) noexcept;
void clear_phase_observer() noexcept;

// Faults are independent and one-shot so a test can combine publication and
// rollback failures without nondeterministic filesystem races.
void inject_fault_once(Fault fault) noexcept;
[[nodiscard]] bool fault_pending(Fault fault) noexcept;
void clear_faults() noexcept;

// Returns the real descriptor retained by the deterministic acquisition-close
// injection. The transaction must close it before returning; tests use the
// numeric value immediately, before any operation can reuse it.
[[nodiscard]] int last_injected_output_lock_descriptor() noexcept;

} // namespace nebula::cli::freestanding_transaction_testing
