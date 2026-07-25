#pragma once

#if !defined(NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING)
#error "hosted artifact transaction hooks are test-only"
#endif

#include <cstdint>

namespace nebula::cli::hosted_artifact_transaction_testing {

enum class FaultPoint : std::uint8_t {
  None,
  AfterBackup,
  AfterPublishLink,
  BeforePublicationDirectoryFlush,
};

void inject_fault_once(FaultPoint point);

using BeforeFinalProtectedInputRevalidationHook = void (*)(void *context);

void inject_before_final_protected_input_revalidation_once(
  BeforeFinalProtectedInputRevalidationHook hook, void *context);

} // namespace nebula::cli::hosted_artifact_transaction_testing
