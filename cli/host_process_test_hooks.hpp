#pragma once

#if !defined(NEBULA_HOST_PROCESS_TESTING)
#error "host-process hooks are test-only"
#endif

#include <cstdint>

namespace nebula::cli::host_process_testing {

// Injects one fixed exception immediately after native child ownership is
// protected by the platform guard. The production target does not compile
// this hook or its branch.
void inject_post_spawn_exception_once() noexcept;
[[nodiscard]] bool post_spawn_exception_pending() noexcept;

// Forces the next unbounded post-spawn infrastructure cleanup to observe a
// failed native termination request. The child is left to exit naturally so
// tests can prove that reap/wait alone is not classified as controlled
// cleanup. Bounded cleanup uses a separate process-group/Job Object policy and
// intentionally cannot consume this hook.
void inject_unbounded_post_spawn_termination_failure_once() noexcept;
[[nodiscard]] bool unbounded_post_spawn_termination_failure_pending() noexcept;

// Throws after an explicit cleanup outcome has been recorded but before its
// allocating diagnostics are rendered. An unconfirmed guard must fail with
// the platform's fixed status rather than retry native termination.
void inject_post_cleanup_diagnostic_exception_once() noexcept;
[[nodiscard]] bool post_cleanup_diagnostic_exception_pending() noexcept;

// Overrides the next successfully completed parent-side child-stdin close
// outcome with a fixed platform error. The native close still runs first, so
// this hook cannot retain or leak the endpoint it makes observable as failed.
void inject_parent_stdin_endpoint_close_error_once() noexcept;
[[nodiscard]] bool parent_stdin_endpoint_close_error_pending() noexcept;

// Identifies the child created for the most recent injected exception. Tests
// and post-spawn failure use it immediately to prove that the ownership guard
// reaped/terminated it.
[[nodiscard]] std::uint64_t last_injected_process_id() noexcept;

} // namespace nebula::cli::host_process_testing
