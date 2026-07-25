#pragma once

#if !defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
#error "freestanding toolchain hooks are test-only"
#endif

#include "host_process.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace nebula::cli::freestanding_toolchain_testing {

// Replaces exactly one bounded compiler query result. signal_to_raise is
// delivered while the resolver-owned signal scope is armed, allowing tests to
// exercise the handoff policy without manufacturing an unsafe real process.
void inject_query_result_once(HostProcessResult result, int signal_to_raise);
[[nodiscard]] bool query_result_injection_pending() noexcept;

// Fails exactly one explicit compiler snapshot retirement without touching the
// lease. The next cleanup call therefore exercises the real retry path.
void inject_compiler_snapshot_cleanup_failure_once(std::string detail);
[[nodiscard]] bool compiler_snapshot_cleanup_failure_injection_pending() noexcept;

// Runs setup against the newly created private compiler lease and then throws
// one fixed resolver exception. The setup callback lets the test establish a
// real identity-replacement and pending-signal fixture before stack unwinding.
using ResolverExceptionSetup = void (*)(const std::filesystem::path &lease_path);
void inject_resolver_exception_after_lease_once(ResolverExceptionSetup setup);
[[nodiscard]] bool resolver_exception_injection_pending() noexcept;
inline constexpr std::string_view kInjectedResolverExceptionDetail =
  "injected resolver exception after verified compiler lease creation";

// Fails exactly one explicit signal-scope restore after freeze has captured
// any pending signal. Destructor fallback remains real, so the test also
// verifies that unsafe emergency redelivery was disabled before unwinding.
void inject_signal_restore_failure_once(std::string detail);
[[nodiscard]] bool signal_restore_failure_injection_pending() noexcept;

} // namespace nebula::cli::freestanding_toolchain_testing
