#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class CompilerTerminationSignalScope;

namespace nebula::cli {

struct HostEnvironmentOverride {
  std::string name;
  std::string value;
};

enum class HostProcessStreamMode : std::uint8_t {
  Inherit,
  Capture,
  Discard,
};

enum class HostProcessInputMode : std::uint8_t {
  Inherit,
  Discard,
};

enum class HostProcessContainment : std::uint8_t {
  NotRequested,
  Confirmed,
  Unconfirmed,
};

struct HostProcessRequest {
  // The executable path is distinct from logical argv[0]. This lets callers
  // bind an already-verified executable while preserving public invocation
  // semantics such as argv[0] and loader-relative behavior.
  std::filesystem::path executable_path;
  // arguments must be nonempty; arguments[0] is the child's logical argv[0].
  std::vector<std::string> arguments;
  bool inherit_environment = true;
  std::vector<HostEnvironmentOverride> environment_overrides;
  // Discard binds stdin to the platform null device, which gives tools an
  // immediate EOF and prevents an unattended build from consuming caller
  // input or inheriting an unrelated descriptor.
  HostProcessInputMode stdin_mode = HostProcessInputMode::Inherit;
  HostProcessStreamMode stdout_mode = HostProcessStreamMode::Inherit;
  HostProcessStreamMode stderr_mode = HostProcessStreamMode::Inherit;
  std::size_t max_stdout_bytes = 0U;
  std::size_t max_stderr_bytes = 0U;
  // Zero preserves the historical unbounded wait. A positive timeout creates
  // a Job Object on Windows or a dedicated process group on Darwin/Linux
  // before user code starts. POSIX containment assumes a trusted child that
  // does not deliberately escape its assigned process group.
  std::uint32_t timeout_milliseconds = 0U;
  // Optional POSIX build boundary. When armed, an intercepted caller
  // termination signal cancels and audits the contained process group before
  // returning control to the artifact transaction for cleanup/redelivery.
  const CompilerTerminationSignalScope *termination_signals = nullptr;
};

struct HostProcessResult {
  bool started = false;
  bool exited = false;
  std::uint32_t exit_code = 0U;
  int termination_signal = 0;
  int parent_interruption_signal = 0;
  std::string stdout_data;
  std::string stderr_data;
  bool stdout_limit_exceeded = false;
  bool stderr_limit_exceeded = false;
  bool timed_out = false;
  HostProcessContainment containment = HostProcessContainment::NotRequested;
  std::string infrastructure_error;

  [[nodiscard]] bool completed() const noexcept {
    return started && exited && !timed_out && infrastructure_error.empty() &&
           !stdout_limit_exceeded && !stderr_limit_exceeded &&
           containment != HostProcessContainment::Unconfirmed;
  }

  [[nodiscard]] bool succeeded() const noexcept {
    return completed() && termination_signal == 0 && exit_code == 0U;
  }
};

// Executes a process directly without a shell. Arguments and environment
// values are never rendered by this layer. Captured streams require a positive
// byte limit; exceeding it is returned as an explicit infrastructure error.
[[nodiscard]] HostProcessResult run_host_process(const HostProcessRequest &request);

// Converts a completed platform result to the CLI's int exit-code contract.
// Infrastructure failures and unrepresentable native statuses map to 125 and
// return a nonempty error description.
[[nodiscard]] int host_process_compatible_exit_code(const HostProcessResult &result,
                                                    std::string &error);

// Launches an absolute executable directly, without a shell. The child receives
// a snapshot of the current environment with exactly one variable replaced.
// Environment values and command arguments are intentionally excluded from the
// runner's diagnostics so callers can use the override for credentials.
int run_host_process_with_environment_override(const std::vector<std::string> &arguments,
                                               const HostEnvironmentOverride &environment_override);

namespace detail {

// Internal pure helpers are declared here so the platform-independent quoting
// and environment-block contracts can be tested on every build host.
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

struct WindowsEnvironmentBlockResult {
  std::vector<wchar_t> block;
  std::string error;
};

[[nodiscard]] WindowsEnvironmentBlockResult build_windows_environment_block(
  const std::vector<std::wstring> &inherited_entries,
  const std::vector<std::pair<std::wstring, std::wstring>> &overrides);

} // namespace detail

} // namespace nebula::cli
