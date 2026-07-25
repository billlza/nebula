#include "compiler_process.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;

namespace {

void append_detail(std::string &detail, std::string_view additional) {
  if (additional.empty())
    return;
  if (!detail.empty())
    detail += "; ";
  detail += additional;
}

using CompilerResourceState = CompilerContainmentOutcome::ResourceState;

CompilerContainmentOutcome make_containment_outcome(CompilerProcessContainment containment,
                                                    CompilerResourceState resource_state,
                                                    std::string detail, int leader_status = 0,
                                                    bool leader_status_available = false) {
  CompilerContainmentOutcome outcome;
  outcome.containment = containment;
  outcome.resource_state = resource_state;
  outcome.detail = std::move(detail);
  outcome.leader_status = leader_status;
  outcome.leader_status_available = leader_status_available;
  return outcome;
}

CompilerResourceState classify_leader_anchor(CompilerProcessControl &control, pid_t pid,
                                             std::string &detail) {
  const CompilerLeaderObservation observation = control.observe_leader(pid);
  if (observation.state == CompilerLeaderState::Running ||
      observation.state == CompilerLeaderState::Exited) {
    return CompilerResourceState::OwnedLeaderAnchor;
  }
  append_detail(detail, observation.detail.empty()
                          ? "process-group leader ownership could not be verified"
                          : observation.detail);
  return CompilerResourceState::OwnershipLost;
}

constexpr auto kFinalOperationTimeout = std::chrono::seconds(2);
constexpr auto kObservationInterval = std::chrono::milliseconds(10);

CompilerLeaderObservation observe_process_group_leader(pid_t pid) {
  if (pid <= 0)
    return {CompilerLeaderState::Error, "process-group leader PID must be positive"};
  while (true) {
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
      if (info.si_pid == 0)
        return {CompilerLeaderState::Running, {}};
      if (info.si_pid == pid && (info.si_code == CLD_EXITED || info.si_code == CLD_KILLED ||
                                 info.si_code == CLD_DUMPED)) {
        return {CompilerLeaderState::Exited, {}};
      }
      return {CompilerLeaderState::Error, "waitid returned an event for an unexpected process"};
    }
    const int wait_error = errno;
    if (wait_error == EINTR)
      continue;
    if (wait_error == ECHILD) {
      return {CompilerLeaderState::OwnershipLost,
              "lost ownership of process-group leader " + std::to_string(pid)};
    }
    return {CompilerLeaderState::Error, "failed to observe process-group leader " +
                                          std::to_string(pid) + ": " + std::strerror(wait_error)};
  }
}

#if defined(__APPLE__)
bool darwin_process_group_has_only_zombies(pid_t process_group, pid_t expected_leader,
                                           std::string &detail) {
  constexpr std::size_t kMaxProcessGroupMembers = 4096U;
  constexpr std::size_t kMaxSnapshotAttempts = 3U;
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, static_cast<int>(process_group)};
  for (std::size_t attempt = 0; attempt < kMaxSnapshotAttempts; ++attempt) {
    std::size_t requested_bytes = 0U;
    if (::sysctl(mib, 4U, nullptr, &requested_bytes, nullptr, 0U) != 0) {
      detail = "Darwin process-group audit size query failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (requested_bytes == 0U) {
      detail = "Darwin process-group audit returned an empty snapshot";
      return false;
    }
    constexpr std::size_t kMaxSnapshotBytes = kMaxProcessGroupMembers * sizeof(kinfo_proc);
    if (requested_bytes > kMaxSnapshotBytes) {
      detail = "Darwin process-group audit exceeded its 4096-member bound";
      return false;
    }
    const std::size_t requested_records =
      requested_bytes / sizeof(kinfo_proc) + (requested_bytes % sizeof(kinfo_proc) == 0U ? 0U : 1U);
    const std::size_t capacity = std::min(kMaxProcessGroupMembers, requested_records + 16U);
    std::vector<kinfo_proc> processes(capacity);
    std::size_t snapshot_bytes = processes.size() * sizeof(kinfo_proc);
    if (::sysctl(mib, 4U, processes.data(), &snapshot_bytes, nullptr, 0U) != 0) {
      const int snapshot_error = errno;
      if (snapshot_error == ENOMEM && attempt + 1U < kMaxSnapshotAttempts)
        continue;
      detail = "Darwin process-group audit failed: " + std::string(std::strerror(snapshot_error));
      return false;
    }
    if (snapshot_bytes == 0U || snapshot_bytes > processes.size() * sizeof(kinfo_proc) ||
        snapshot_bytes % sizeof(kinfo_proc) != 0U) {
      detail = "Darwin process-group audit returned a malformed snapshot";
      return false;
    }
    bool found_leader = false;
    for (std::size_t index = 0U; index < snapshot_bytes / sizeof(kinfo_proc); ++index) {
      const kinfo_proc &process = processes[index];
      if (process.kp_eproc.e_pgid != process_group) {
        detail = "Darwin process-group audit returned a mismatched process group";
        return false;
      }
      if (process.kp_proc.p_pid == expected_leader)
        found_leader = true;
      if (process.kp_proc.p_stat != SZOMB) {
        detail = "Darwin process group still contains a live process";
        return false;
      }
    }
    if (!found_leader) {
      detail = "Darwin process-group audit did not retain the expected zombie leader";
      return false;
    }
    return true;
  }
  detail = "Darwin process-group audit could not obtain a stable bounded snapshot";
  return false;
}
#endif

#if defined(__linux__)
bool linux_process_group_has_only_zombies(pid_t process_group, pid_t expected_leader,
                                          std::string &detail) {
  constexpr std::size_t kMaxProcEntries = 1'048'576U;
  constexpr std::size_t kMaxStatBytes = 4096U;
  const auto scan_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::error_code iteration_error;
  fs::directory_iterator iterator("/proc", fs::directory_options::skip_permission_denied,
                                  iteration_error);
  if (iteration_error) {
    detail = "Linux process-group audit could not open /proc: " + iteration_error.message();
    return false;
  }
  bool found_leader = false;
  std::size_t scanned_entries = 0U;
  const fs::directory_iterator end;
  while (iterator != end) {
    if (++scanned_entries > kMaxProcEntries) {
      detail = "Linux process-group audit exceeded its 1048576-entry bound";
      return false;
    }
    if (scanned_entries % 256U == 0U && std::chrono::steady_clock::now() >= scan_deadline) {
      detail = "Linux process-group audit exceeded its two-second scan deadline";
      return false;
    }
    const std::string filename = iterator->path().filename().string();
    int candidate_pid = 0;
    const auto parsed =
      std::from_chars(filename.data(), filename.data() + filename.size(), candidate_pid);
    if (parsed.ec == std::errc{} && parsed.ptr == filename.data() + filename.size() &&
        candidate_pid > 0) {
      std::ifstream stat_stream(iterator->path() / "stat");
      std::string stat_line;
      if (!stat_stream || !std::getline(stat_stream, stat_line)) {
        if (::kill(static_cast<pid_t>(candidate_pid), 0) != 0 && errno == ESRCH) {
          iterator.increment(iteration_error);
          if (iteration_error) {
            detail = "Linux process-group audit iteration failed: " + iteration_error.message();
            return false;
          }
          continue;
        }
        detail = "Linux process-group audit could not read /proc/" + filename + "/stat";
        return false;
      }
      if (stat_line.size() > kMaxStatBytes) {
        detail = "Linux process-group audit encountered an oversized proc stat record";
        return false;
      }
      const std::size_t command_end = stat_line.rfind(')');
      if (command_end == std::string::npos || command_end + 2U >= stat_line.size()) {
        detail = "Linux process-group audit encountered a malformed proc stat record";
        return false;
      }
      std::istringstream fields(stat_line.substr(command_end + 2U));
      char state = '\0';
      long long parent_pid = 0;
      long long parsed_group = 0;
      if (!(fields >> state >> parent_pid >> parsed_group)) {
        detail = "Linux process-group audit could not parse a proc stat record";
        return false;
      }
      (void)parent_pid;
      if (parsed_group == static_cast<long long>(process_group)) {
        if (candidate_pid == expected_leader)
          found_leader = true;
        if (state != 'Z' && state != 'X' && state != 'x') {
          detail = "Linux process group still contains a live process";
          return false;
        }
      }
    }
    iterator.increment(iteration_error);
    if (iteration_error) {
      detail = "Linux process-group audit iteration failed: " + iteration_error.message();
      return false;
    }
  }
  if (!found_leader) {
    detail = "Linux process-group audit did not retain the expected zombie leader";
    return false;
  }
  return true;
}
#endif

bool process_group_has_only_zombies(pid_t process_group, pid_t expected_leader,
                                    std::string &detail) {
#if defined(__APPLE__)
  return darwin_process_group_has_only_zombies(process_group, expected_leader, detail);
#elif defined(__linux__)
  return linux_process_group_has_only_zombies(process_group, expected_leader, detail);
#else
  (void)process_group;
  (void)expected_leader;
  detail = "process-group quiescence audit is unavailable on this POSIX host";
  return false;
#endif
}

bool wait_for_process_group_quiescence(pid_t process_group, pid_t expected_leader,
                                       std::chrono::steady_clock::time_point deadline,
                                       std::string &detail) {
  if (process_group <= 0 || expected_leader <= 0) {
    detail = "process-group quiescence audit requires positive process identifiers";
    return false;
  }
  std::string last_audit_detail;
  while (true) {
    last_audit_detail.clear();
    if (process_group_has_only_zombies(process_group, expected_leader, last_audit_detail))
      return true;
    if (std::chrono::steady_clock::now() >= deadline) {
      detail = "process group " + std::to_string(process_group) +
               " did not reach a quiescent zombie-only state";
      if (!last_audit_detail.empty())
        detail += ": " + last_audit_detail;
      return false;
    }
    std::this_thread::sleep_for(kObservationInterval);
  }
}

bool signal_owned_process_group(pid_t pid, int signal_number, bool leader_exited,
                                std::string &detail) {
  if (pid <= 0) {
    detail = "process-group signaling requires a positive leader PID";
    return false;
  }
  if (::kill(-pid, signal_number) == 0)
    return true;
  const int signal_error = errno;
  // Ownership is established once by the policy layer before this call. Never perform a second
  // hidden observation here: ownership loss is monotonic, and a later bare-PID observation could
  // otherwise mistake a concurrently reused child identifier for the original leader.
  if (signal_error == ESRCH && leader_exited)
    return true;
#if defined(__APPLE__)
  if (signal_error == EPERM && leader_exited) {
    std::string audit_detail;
    if (darwin_process_group_has_only_zombies(pid, pid, audit_detail))
      return true;
    detail = "permission denied while signaling process group " + std::to_string(pid) + "; " +
             audit_detail;
    return false;
  }
#endif
  detail = "failed to signal process group " + std::to_string(pid) + " with signal " +
           std::to_string(signal_number) + ": " + std::strerror(signal_error);
  return false;
}

CompilerLeaderReapOutcome
reap_process_group_leader(pid_t pid, std::chrono::steady_clock::time_point deadline) {
  if (pid <= 0) {
    return {CompilerLeaderReapState::OwnershipLost, 0,
            "process-group leader reap requires a positive PID"};
  }
  while (true) {
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      return {CompilerLeaderReapState::Reaped, status, {}};
    if (waited < 0) {
      const int wait_error = errno;
      if (wait_error == EINTR)
        continue;
      return {CompilerLeaderReapState::OwnershipLost, 0,
              "failed to reap process-group leader " + std::to_string(pid) + ": " +
                std::strerror(wait_error)};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return {CompilerLeaderReapState::AnchorRetained, 0,
              "process-group leader " + std::to_string(pid) +
                " did not become reapable after final termination"};
    }
    std::this_thread::sleep_for(kObservationInterval);
  }
}

} // namespace

CompilerLeaderObservation SystemCompilerProcessControl::observe_leader(pid_t pid) {
  return observe_process_group_leader(pid);
}

bool SystemCompilerProcessControl::signal_group(pid_t pid, int signal_number, bool leader_exited,
                                                std::string &detail) {
  return signal_owned_process_group(pid, signal_number, leader_exited, detail);
}

bool SystemCompilerProcessControl::kill_leader(pid_t pid, std::string &detail) {
  if (pid <= 0) {
    detail = "direct process-group leader kill requires a positive PID";
    return false;
  }
  if (::kill(pid, SIGKILL) == 0 || errno == ESRCH)
    return true;
  detail = "direct process-group leader kill failed: " + std::string(std::strerror(errno));
  return false;
}

bool SystemCompilerProcessControl::wait_for_group_quiescence(pid_t process_group,
                                                             pid_t expected_leader,
                                                             Clock::time_point deadline,
                                                             std::string &detail) {
  return ::wait_for_process_group_quiescence(process_group, expected_leader, deadline, detail);
}

CompilerLeaderReapOutcome SystemCompilerProcessControl::reap_leader(pid_t pid,
                                                                    Clock::time_point deadline) {
  return reap_process_group_leader(pid, deadline);
}

CompilerProcessControl::Clock::time_point SystemCompilerProcessControl::now() const noexcept {
  return Clock::now();
}

void SystemCompilerProcessControl::sleep_for(std::chrono::milliseconds duration) {
  std::this_thread::sleep_for(duration);
}

void emergency_process_group_cleanup(pid_t pid) noexcept {
  if (pid <= 0) {
    constexpr char message[] =
      "[cmd] fatal: exception cleanup received an invalid process identifier\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }
  siginfo_t info{};
  while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
    if (errno == EINTR)
      continue;
    if (errno == ECHILD) {
      constexpr char message[] =
        "[cmd] fatal: process containment was unconfirmed after child ownership was lost\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
    constexpr char message[] =
      "[cmd] fatal: could not verify process ownership during exception cleanup\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }

  bool group_cleanup_failed = false;
  if (::kill(-pid, SIGKILL) != 0) {
    const int group_error = errno;
    group_cleanup_failed = group_error != ESRCH;
    if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
      constexpr char message[] =
        "[cmd] fatal: could not terminate process during exception cleanup\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
  }
  if (!group_cleanup_failed) {
    try {
      std::string audit_detail;
      if (!wait_for_process_group_quiescence(
            pid, pid, std::chrono::steady_clock::now() + kFinalOperationTimeout, audit_detail)) {
        constexpr char message[] =
          "[cmd] fatal: process group did not quiesce during exception cleanup\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
        ::_exit(125);
      }
    } catch (...) {
      constexpr char message[] =
        "[cmd] fatal: process group audit failed during exception cleanup\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR)
      continue;
    if (errno == ECHILD) {
      constexpr char message[] =
        "[cmd] fatal: child ownership was lost before exception cleanup could reap it\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
    constexpr char message[] = "[cmd] fatal: could not reap process during exception cleanup\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }
  if (group_cleanup_failed) {
    constexpr char message[] =
      "[cmd] fatal: process group cleanup was incomplete during exception cleanup\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }
}

CompilerContainmentOutcome clean_compiler_process_group(CompilerProcessControl &control, pid_t pid,
                                                        std::string initial_detail) {
  if (pid <= 0) {
    append_detail(initial_detail, "process-group cleanup requires a positive leader PID");
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                    CompilerResourceState::OwnershipLost,
                                    std::move(initial_detail));
  }
  const CompilerLeaderObservation initial_observation = control.observe_leader(pid);
  if (initial_observation.state == CompilerLeaderState::OwnershipLost ||
      initial_observation.state == CompilerLeaderState::Error) {
    append_detail(initial_detail, initial_observation.detail);
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                    CompilerResourceState::OwnershipLost,
                                    std::move(initial_detail));
  }
  const bool leader_exited = initial_observation.state == CompilerLeaderState::Exited;

  std::string signal_detail;
  const bool group_signaled = control.signal_group(pid, SIGKILL, leader_exited, signal_detail);
  if (!group_signaled) {
    append_detail(initial_detail, signal_detail);
    CompilerResourceState resource_state = classify_leader_anchor(control, pid, initial_detail);
    if (resource_state == CompilerResourceState::OwnedLeaderAnchor && !leader_exited) {
      std::string leader_kill_detail;
      if (!control.kill_leader(pid, leader_kill_detail)) {
        append_detail(initial_detail, leader_kill_detail);
        resource_state = classify_leader_anchor(control, pid, initial_detail);
      }
    }
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed, resource_state,
                                    std::move(initial_detail));
  }

  std::string audit_detail;
  const bool group_quiescent = control.wait_for_group_quiescence(
    pid, pid, control.now() + kFinalOperationTimeout, audit_detail);
  if (!group_quiescent) {
    append_detail(initial_detail, audit_detail);
    const CompilerResourceState resource_state =
      classify_leader_anchor(control, pid, initial_detail);
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed, resource_state,
                                    std::move(initial_detail));
  }

  CompilerLeaderReapOutcome reap = control.reap_leader(pid, control.now() + kFinalOperationTimeout);
  if (reap.state != CompilerLeaderReapState::Reaped) {
    append_detail(initial_detail, reap.detail);
    const CompilerResourceState resource_state =
      reap.state == CompilerLeaderReapState::AnchorRetained
        ? CompilerResourceState::OwnedLeaderAnchor
        : CompilerResourceState::OwnershipLost;
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed, resource_state,
                                    std::move(initial_detail));
  }

  return make_containment_outcome(CompilerProcessContainment::Confirmed,
                                  CompilerResourceState::Gone, std::move(initial_detail),
                                  reap.status, true);
}

CompilerContainmentOutcome terminate_compiler_process_group(CompilerProcessControl &control,
                                                            pid_t pid,
                                                            std::chrono::milliseconds grace) {
  std::string detail;
  if (pid <= 0) {
    append_detail(detail, "process-group termination requires a positive leader PID");
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                    CompilerResourceState::OwnershipLost, std::move(detail));
  }
  const CompilerLeaderObservation initial_observation = control.observe_leader(pid);
  if (initial_observation.state == CompilerLeaderState::OwnershipLost ||
      initial_observation.state == CompilerLeaderState::Error) {
    append_detail(detail, initial_observation.detail);
    return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                    CompilerResourceState::OwnershipLost, std::move(detail));
  }
  if (initial_observation.state == CompilerLeaderState::Exited)
    return clean_compiler_process_group(control, pid);

  std::string signal_detail;
  if (!control.signal_group(pid, SIGTERM, false, signal_detail)) {
    append_detail(detail, signal_detail);
    CompilerContainmentOutcome cleanup =
      clean_compiler_process_group(control, pid, std::move(detail));
    cleanup.containment = CompilerProcessContainment::Unconfirmed;
    return cleanup;
  }

  const auto termination_deadline = control.now() + grace;
  while (control.now() < termination_deadline) {
    const CompilerLeaderObservation observation = control.observe_leader(pid);
    if (observation.state == CompilerLeaderState::Exited)
      break;
    if (observation.state == CompilerLeaderState::OwnershipLost) {
      append_detail(detail, observation.detail);
      return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                      CompilerResourceState::OwnershipLost, std::move(detail));
    }
    if (observation.state == CompilerLeaderState::Error) {
      append_detail(detail, observation.detail);
      return make_containment_outcome(CompilerProcessContainment::Unconfirmed,
                                      CompilerResourceState::OwnershipLost, std::move(detail));
    }
    control.sleep_for(kObservationInterval);
  }
  return clean_compiler_process_group(control, pid, std::move(detail));
}

CompilerInterruptionOutcome classify_compiler_interruption(int signal_number,
                                                           CompilerContainmentOutcome containment) {
  CompilerInterruptionOutcome outcome;
  outcome.status_detail = "compiler execution interrupted by signal " +
                          std::to_string(signal_number) +
                          (containment.confirmed() ? " after process-group cleanup"
                                                   : " after process-group cleanup attempt");
  if (!containment.detail.empty())
    outcome.status_detail += "; cleanup context: " + containment.detail;

  if (!containment.confirmed()) {
    outcome.status_detail +=
      "; original signal redelivery was suppressed because process containment was not "
      "confirmed";
    outcome.execution = {
      125, false, outcome.status_detail, 0, CompilerProcessContainment::Unconfirmed,
    };
    return outcome;
  }

  outcome.execution = {
    128 + signal_number, false, {}, signal_number, CompilerProcessContainment::Confirmed,
  };
  return outcome;
}

#endif
