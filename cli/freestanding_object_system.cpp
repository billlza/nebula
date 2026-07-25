#include "freestanding_object.hpp"

#include "host_process.hpp"

#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::size_t kCompilerStreamLimitBytes = 64U * 1024U;

CompilerProcessContainment compiler_containment(nebula::cli::HostProcessContainment containment) {
  switch (containment) {
  case nebula::cli::HostProcessContainment::Confirmed:
    return CompilerProcessContainment::Confirmed;
  case nebula::cli::HostProcessContainment::Unconfirmed:
    return CompilerProcessContainment::Unconfirmed;
  case nebula::cli::HostProcessContainment::NotRequested:
    return CompilerProcessContainment::NotStarted;
  }
  return CompilerProcessContainment::Unconfirmed;
}

bool append_environment_override(std::string_view entry,
                                 std::vector<nebula::cli::HostEnvironmentOverride> &overrides,
                                 std::string &detail) {
  const std::size_t separator = entry.find('=');
  if (separator == 0U || separator == std::string_view::npos) {
    detail = "compiler environment contains an invalid NAME=value entry";
    return false;
  }
  overrides.push_back(
    {std::string(entry.substr(0U, separator)), std::string(entry.substr(separator + 1U))});
  return true;
}

} // namespace

CommandExecutionResult
run_command_with_environment(const nebula::cli::ResolvedFreestandingToolchain &toolchain,
                             const std::vector<std::string> &args,
                             const std::vector<std::string> &environment, int timeout_seconds,
                             const CompilerTerminationSignalScope &termination_signals) {
  CommandExecutionResult execution;
  if (timeout_seconds <= 0 || static_cast<unsigned long long>(timeout_seconds) >
                                std::numeric_limits<std::uint32_t>::max() / 1000ULL) {
    execution.infrastructure_error = "compiler execution requires a representable positive timeout";
    return execution;
  }

  nebula::cli::HostProcessRequest request;
  request.arguments = args;
  request.inherit_environment = false;
  request.environment_overrides.reserve(environment.size());
  for (const std::string &entry : environment) {
    if (!append_environment_override(entry, request.environment_overrides,
                                     execution.infrastructure_error)) {
      return execution;
    }
  }
  request.stdin_mode = nebula::cli::HostProcessInputMode::Discard;
  request.stdout_mode = nebula::cli::HostProcessStreamMode::Capture;
  request.stderr_mode = nebula::cli::HostProcessStreamMode::Capture;
  request.max_stdout_bytes = kCompilerStreamLimitBytes;
  request.max_stderr_bytes = kCompilerStreamLimitBytes;
  request.timeout_milliseconds = static_cast<std::uint32_t>(timeout_seconds) * 1000U;
  request.termination_signals = &termination_signals;

  const nebula::cli::HostProcessResult process = toolchain.execute_compiler(std::move(request));
  execution.timed_out = process.timed_out;
  execution.interrupted_signal = process.parent_interruption_signal;
  execution.containment = compiler_containment(process.containment);
  execution.stdout_summary = process.stdout_data;
  execution.stderr_summary = process.stderr_data;
  std::string compatibility_error;
  execution.exit_code =
    nebula::cli::host_process_compatible_exit_code(process, compatibility_error);
  execution.infrastructure_error = std::move(compatibility_error);
  if (execution.interrupted_signal != 0 &&
      execution.containment == CompilerProcessContainment::Confirmed) {
    std::cerr << "[cmd] compiler execution interrupted by signal " << execution.interrupted_signal
              << " after process-group cleanup\n";
  } else if (execution.timed_out) {
    std::cerr << "[cmd] timeout=" << timeout_seconds << "s\n";
  } else if (!execution.infrastructure_error.empty()) {
    std::cerr << "[cmd] infrastructure failure: " << execution.infrastructure_error << '\n';
  } else if (execution.exit_code != 0) {
    std::cerr << "[cmd] exit=" << execution.exit_code << '\n';
  }
  return execution;
}

namespace {

class SystemFreestandingCompilerExecutor final : public FreestandingCompilerExecutor {
public:
  explicit SystemFreestandingCompilerExecutor(nebula::cli::ResolvedFreestandingToolchain &toolchain)
      : toolchain_(toolchain) {}

  CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) override {
    return run_command_with_environment(toolchain_, command, environment, timeout_seconds,
                                        termination_signals);
  }

private:
  nebula::cli::ResolvedFreestandingToolchain &toolchain_;
};

} // namespace

FreestandingObjectResult
build_freestanding_object(const FreestandingObjectRequest &request,
                          nebula::cli::ResolvedFreestandingToolchain &toolchain) {
  SystemFreestandingCompilerExecutor compiler_executor(toolchain);
  return build_freestanding_object(request, toolchain, compiler_executor);
}
