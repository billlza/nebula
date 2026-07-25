#pragma once

#include "compiler_execution.hpp"

#if !defined(_WIN32)

#include <chrono>
#include <cstdint>
#include <string>

#include <sys/types.h>

enum class CompilerLeaderState : std::uint8_t {
  Running,
  Exited,
  OwnershipLost,
  Error,
};

struct CompilerLeaderObservation {
  CompilerLeaderState state = CompilerLeaderState::Error;
  std::string detail;
};

enum class CompilerLeaderReapState : std::uint8_t {
  Reaped,
  AnchorRetained,
  OwnershipLost,
};

struct CompilerLeaderReapOutcome {
  CompilerLeaderReapState state = CompilerLeaderReapState::OwnershipLost;
  int status = 0;
  std::string detail;
};

// A narrow process-control boundary for the containment policy. The caller must be the sole waiter
// and reaper for the child throughout a cleanup operation; retaining the unreaped leader is the
// portable PID/process-group identity anchor. The production adapter owns all POSIX syscalls;
// deterministic tests can exercise policy failures without creating or signaling a real child.
class CompilerProcessControl {
public:
  using Clock = std::chrono::steady_clock;

  CompilerProcessControl() = default;
  CompilerProcessControl(const CompilerProcessControl &) = delete;
  CompilerProcessControl &operator=(const CompilerProcessControl &) = delete;
  virtual ~CompilerProcessControl() = default;

  [[nodiscard]] virtual CompilerLeaderObservation observe_leader(pid_t pid) = 0;
  [[nodiscard]] virtual bool signal_group(pid_t pid, int signal_number, bool leader_exited,
                                          std::string &detail) = 0;
  [[nodiscard]] virtual bool kill_leader(pid_t pid, std::string &detail) = 0;
  [[nodiscard]] virtual bool wait_for_group_quiescence(pid_t process_group, pid_t expected_leader,
                                                       Clock::time_point deadline,
                                                       std::string &detail) = 0;
  [[nodiscard]] virtual CompilerLeaderReapOutcome reap_leader(pid_t pid,
                                                              Clock::time_point deadline) = 0;
  [[nodiscard]] virtual Clock::time_point now() const noexcept = 0;
  virtual void sleep_for(std::chrono::milliseconds duration) = 0;
};

// Production POSIX adapter shared by every bounded compiler/tool process path.
// Keeping process-group ownership and quiescence auditing here prevents subtly
// different cleanup policies from evolving in individual callers.
class SystemCompilerProcessControl final : public CompilerProcessControl {
public:
  [[nodiscard]] CompilerLeaderObservation observe_leader(pid_t pid) override;
  [[nodiscard]] bool signal_group(pid_t pid, int signal_number, bool leader_exited,
                                  std::string &detail) override;
  [[nodiscard]] bool kill_leader(pid_t pid, std::string &detail) override;
  [[nodiscard]] bool wait_for_group_quiescence(pid_t process_group, pid_t expected_leader,
                                               Clock::time_point deadline,
                                               std::string &detail) override;
  [[nodiscard]] CompilerLeaderReapOutcome reap_leader(pid_t pid,
                                                      Clock::time_point deadline) override;
  [[nodiscard]] Clock::time_point now() const noexcept override;
  void sleep_for(std::chrono::milliseconds duration) override;
};

// Last-resort noexcept cleanup for an exception escaping a post-spawn region.
// It terminates the process if complete group containment cannot be proven.
void emergency_process_group_cleanup(pid_t pid) noexcept;

struct CompilerContainmentOutcome {
  CompilerProcessContainment containment = CompilerProcessContainment::Unconfirmed;
  // Containment describes whether the complete process group was proven quiescent. Resource
  // state independently describes whether a stable child identity remains available for a
  // no-throw emergency cleanup. Default to the fail-closed state so omitted initialization can
  // never authorize signaling a potentially reused PID/process group.
  enum class ResourceState : std::uint8_t {
    OwnershipLost,
    OwnedLeaderAnchor,
    Gone,
  };

  ResourceState resource_state = ResourceState::OwnershipLost;
  std::string detail;
  int leader_status = 0;
  bool leader_status_available = false;

  [[nodiscard]] bool confirmed() const noexcept {
    return containment == CompilerProcessContainment::Confirmed && resources_gone();
  }

  [[nodiscard]] bool resources_gone() const noexcept {
    return resource_state == ResourceState::Gone;
  }

  [[nodiscard]] bool owns_leader_anchor() const noexcept {
    return resource_state == ResourceState::OwnedLeaderAnchor;
  }
};

struct CompilerInterruptionOutcome {
  CommandExecutionResult execution;
  std::string status_detail;
};

[[nodiscard]] CompilerContainmentOutcome
clean_compiler_process_group(CompilerProcessControl &control, pid_t pid,
                             std::string initial_detail = {});

[[nodiscard]] CompilerContainmentOutcome
terminate_compiler_process_group(CompilerProcessControl &control, pid_t pid,
                                 std::chrono::milliseconds grace);

[[nodiscard]] CompilerInterruptionOutcome
classify_compiler_interruption(int signal_number, CompilerContainmentOutcome containment);

#endif
