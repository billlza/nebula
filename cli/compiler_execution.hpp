#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class CompilerTerminationSignalScope;

namespace nebula::cli {
class ResolvedFreestandingToolchain;
}

enum class CompilerProcessContainment : std::uint8_t {
  NotStarted,
  Confirmed,
  Unconfirmed,
};

struct CommandExecutionResult {
  CommandExecutionResult() = default;
  CommandExecutionResult(
    int status, bool timeout, std::string error, int signal = 0,
    CompilerProcessContainment process_containment = CompilerProcessContainment::NotStarted,
    std::string stdout_bytes = {}, std::string stderr_bytes = {})
      : exit_code(status), timed_out(timeout), infrastructure_error(std::move(error)),
        interrupted_signal(signal), containment(process_containment),
        stdout_summary(std::move(stdout_bytes)), stderr_summary(std::move(stderr_bytes)) {}

  int exit_code = 1;
  bool timed_out = false;
  std::string infrastructure_error;
  int interrupted_signal = 0;
  CompilerProcessContainment containment = CompilerProcessContainment::NotStarted;
  // Captured by the process layer under fixed per-stream bounds. Callers must
  // escape or summarize these bytes before rendering diagnostics.
  std::string stdout_summary;
  std::string stderr_summary;
};

CommandExecutionResult
run_command_with_environment(const nebula::cli::ResolvedFreestandingToolchain &toolchain,
                             const std::vector<std::string> &args,
                             const std::vector<std::string> &environment, int timeout_seconds,
                             const CompilerTerminationSignalScope &termination_signals);
