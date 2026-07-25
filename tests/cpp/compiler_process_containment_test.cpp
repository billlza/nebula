#include "cli/compiler_process.hpp"

#include <csignal>
#include <deque>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "compiler-process-containment-test: " << message << '\n';
  return false;
}

bool contains(std::string_view text, std::string_view expected) {
  return text.find(expected) != std::string_view::npos;
}

class ScriptedProcessControl final : public CompilerProcessControl {
public:
  struct SignalResult {
    int signal_number = 0;
    bool success = true;
    std::string detail;
  };

  std::deque<CompilerLeaderObservation> observations;
  std::deque<SignalResult> signal_results;
  bool leader_kill_success = true;
  std::string leader_kill_detail;
  bool audit_success = true;
  std::string audit_detail;
  CompilerLeaderReapOutcome reap_outcome = {CompilerLeaderReapState::Reaped, 0, {}};
  std::vector<std::string> calls;

  CompilerLeaderObservation observe_leader(pid_t pid) override {
    calls.push_back("observe:" + std::to_string(pid));
    if (observations.empty())
      return {CompilerLeaderState::Running, {}};
    CompilerLeaderObservation observation = std::move(observations.front());
    observations.pop_front();
    return observation;
  }

  bool signal_group(pid_t pid, int signal_number, bool leader_exited,
                    std::string &detail) override {
    calls.push_back("signal:" + std::to_string(pid) + ":" + std::to_string(signal_number) + ":" +
                    (leader_exited ? "exited" : "running"));
    if (signal_results.empty()) {
      detail = "unexpected signal request";
      return false;
    }
    SignalResult result = std::move(signal_results.front());
    signal_results.pop_front();
    if (result.signal_number != signal_number) {
      detail = "unexpected signal number";
      return false;
    }
    detail = std::move(result.detail);
    return result.success;
  }

  bool kill_leader(pid_t pid, std::string &detail) override {
    calls.push_back("kill-leader:" + std::to_string(pid));
    detail = leader_kill_detail;
    return leader_kill_success;
  }

  bool wait_for_group_quiescence(pid_t process_group, pid_t expected_leader,
                                 Clock::time_point deadline, std::string &detail) override {
    (void)deadline;
    calls.push_back("audit:" + std::to_string(process_group) + ":" +
                    std::to_string(expected_leader));
    detail = audit_detail;
    return audit_success;
  }

  CompilerLeaderReapOutcome reap_leader(pid_t pid, Clock::time_point deadline) override {
    (void)deadline;
    calls.push_back("reap:" + std::to_string(pid));
    return reap_outcome;
  }

  Clock::time_point now() const noexcept override { return now_; }

  void sleep_for(std::chrono::milliseconds duration) override { now_ += duration; }

private:
  Clock::time_point now_{};
};

void configure_grace_period(ScriptedProcessControl &control) {
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
}

std::vector<std::string> successful_group_cleanup_calls(pid_t pid) {
  const std::string value = std::to_string(pid);
  return {
    "observe:" + value,
    "signal:" + value + ":" + std::to_string(SIGTERM) + ":running",
    "observe:" + value,
    "observe:" + value,
    "observe:" + value,
    "signal:" + value + ":" + std::to_string(SIGKILL) + ":running",
    "audit:" + value + ":" + value,
    "reap:" + value,
  };
}

std::vector<std::string> successful_immediate_cleanup_calls(pid_t pid) {
  const std::string value = std::to_string(pid);
  return {
    "observe:" + value,
    "signal:" + value + ":" + std::to_string(SIGKILL) + ":running",
    "audit:" + value + ":" + value,
    "reap:" + value,
  };
}

bool expect_unconfirmed_interruption(const CompilerContainmentOutcome &containment,
                                     std::string_view expected_detail) {
  bool ok = true;
  ok &= expect(containment.containment == CompilerProcessContainment::Unconfirmed,
               "failure did not produce Unconfirmed containment");
  ok &= expect(contains(containment.detail, expected_detail),
               "containment detail omitted its injected root cause");
  CompilerInterruptionOutcome interruption = classify_compiler_interruption(SIGTERM, containment);
  ok &= expect(interruption.execution.exit_code == 125,
               "Unconfirmed interruption did not map to rc125");
  ok &= expect(interruption.execution.interrupted_signal == 0,
               "Unconfirmed interruption retained a signal for redelivery");
  ok &= expect(interruption.execution.containment == CompilerProcessContainment::Unconfirmed,
               "Unconfirmed interruption changed containment state");
  ok &= expect(!interruption.execution.infrastructure_error.empty(),
               "Unconfirmed interruption omitted its infrastructure error");
  ok &= expect(contains(interruption.status_detail, "cleanup attempt"),
               "Unconfirmed interruption overclaimed successful cleanup");
  ok &= expect(contains(interruption.status_detail, "redelivery was suppressed"),
               "Unconfirmed interruption omitted signal suppression context");
  return ok;
}

bool expect_resource_state(const CompilerContainmentOutcome &containment,
                           CompilerContainmentOutcome::ResourceState expected,
                           std::string_view message) {
  return expect(containment.resource_state == expected, message);
}

bool test_group_kill_failure() {
  ScriptedProcessControl control;
  configure_grace_period(control);
  control.signal_results.push_back({SIGTERM, true, {}});
  control.signal_results.push_back({SIGKILL, false, "injected group kill failure"});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 101, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected group kill failure");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "group-kill failure did not retain the leader anchor");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:101",
                   "signal:101:" + std::to_string(SIGTERM) + ":running",
                   "observe:101",
                   "observe:101",
                   "observe:101",
                   "signal:101:" + std::to_string(SIGKILL) + ":running",
                   "observe:101",
                   "kill-leader:101",
                 },
               "group-kill failure took an unexpected process-control path");
  return ok;
}

bool test_initial_signal_failure_followed_by_complete_cleanup() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGTERM, false, "injected initial signal failure"});
  control.signal_results.push_back({SIGKILL, true, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 110, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected initial signal failure");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::Gone,
                              "complete fallback cleanup did not report resources gone");
  ok &= expect(containment.leader_status_available,
               "complete fallback cleanup did not retain the reaped leader status");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:110",
                   "signal:110:" + std::to_string(SIGTERM) + ":running",
                   "observe:110",
                   "signal:110:" + std::to_string(SIGKILL) + ":running",
                   "audit:110:110",
                   "reap:110",
                 },
               "complete cleanup after initial signal failure took an unexpected control path");
  return ok;
}

bool test_group_audit_failure() {
  ScriptedProcessControl control;
  configure_grace_period(control);
  control.signal_results.push_back({SIGTERM, true, {}});
  control.signal_results.push_back({SIGKILL, true, {}});
  control.audit_success = false;
  control.audit_detail = "injected quiescence audit failure";
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 102, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected quiescence audit failure");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "group-audit failure did not retain the leader anchor");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:102",
                   "signal:102:" + std::to_string(SIGTERM) + ":running",
                   "observe:102",
                   "observe:102",
                   "observe:102",
                   "signal:102:" + std::to_string(SIGKILL) + ":running",
                   "audit:102:102",
                   "observe:102",
                 },
               "audit failure took an unexpected process-control path");
  return ok;
}

bool test_leader_reap_timeout_retains_anchor() {
  ScriptedProcessControl control;
  configure_grace_period(control);
  control.signal_results.push_back({SIGTERM, true, {}});
  control.signal_results.push_back({SIGKILL, true, {}});
  control.reap_outcome = {CompilerLeaderReapState::AnchorRetained, 0,
                          "injected leader reap timeout"};
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 103, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected leader reap timeout");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "leader-reap timeout did not retain the leader anchor");
  ok &= expect(control.calls == successful_group_cleanup_calls(103),
               "leader-reap timeout took an unexpected process-control path");
  return ok;
}

bool test_leader_reap_ownership_loss_closes_authority() {
  ScriptedProcessControl control;
  configure_grace_period(control);
  control.signal_results.push_back({SIGTERM, true, {}});
  control.signal_results.push_back({SIGKILL, true, {}});
  control.reap_outcome = {CompilerLeaderReapState::OwnershipLost, 0,
                          "injected ECHILD ownership loss"};
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 113, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected ECHILD ownership loss");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "ECHILD reap result retained process-control authority");
  ok &= expect(control.calls == successful_group_cleanup_calls(113),
               "ECHILD reap result performed an operation after ownership was lost");
  return ok;
}

bool test_leader_ownership_loss() {
  ScriptedProcessControl control;
  control.observations.push_back(
    {CompilerLeaderState::OwnershipLost, "injected leader ownership loss"});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 104, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected leader ownership loss");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "leader ownership loss did not close the cleanup authority");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:104",
                 },
               "ownership loss performed an unsafe operation after losing the leader anchor");
  return ok;
}

bool test_grace_period_ownership_loss_stops_cleanup() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGTERM, true, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back(
    {CompilerLeaderState::OwnershipLost, "injected grace-period ownership loss"});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 114, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected grace-period ownership loss");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "grace-period ownership loss retained cleanup authority");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:114",
                   "signal:114:" + std::to_string(SIGTERM) + ":running",
                   "observe:114",
                 },
               "grace-period ownership loss performed an unsafe follow-up operation");
  return ok;
}

bool test_exited_leader_is_revalidated_before_cleanup() {
  ScriptedProcessControl control;
  control.observations.push_back({CompilerLeaderState::Exited, {}});
  control.observations.push_back(
    {CompilerLeaderState::OwnershipLost, "injected ownership loss before cleanup"});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 115, std::chrono::milliseconds(20));

  bool ok = expect_unconfirmed_interruption(containment, "injected ownership loss before cleanup");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "stale exited-leader observation authorized cleanup");
  ok &= expect(control.calls == std::vector<std::string>{"observe:115", "observe:115"},
               "exited leader was not safely revalidated before cleanup");
  return ok;
}

bool test_confirmed_interruption() {
  ScriptedProcessControl control;
  configure_grace_period(control);
  control.signal_results.push_back({SIGTERM, true, {}});
  control.signal_results.push_back({SIGKILL, true, {}});
  const CompilerContainmentOutcome containment =
    terminate_compiler_process_group(control, 105, std::chrono::milliseconds(20));
  const CompilerInterruptionOutcome interruption =
    classify_compiler_interruption(SIGINT, containment);

  bool ok = true;
  ok &= expect(containment.confirmed(), "successful cleanup was not confirmed");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::Gone,
                              "confirmed cleanup did not report resources gone");
  ok &= expect(containment.leader_status_available,
               "confirmed cleanup did not retain the reaped leader status");
  ok &= expect(interruption.execution.exit_code == 128 + SIGINT,
               "confirmed interruption returned the wrong conventional status");
  ok &= expect(interruption.execution.interrupted_signal == SIGINT,
               "confirmed interruption did not retain the signal for redelivery");
  ok &= expect(interruption.execution.infrastructure_error.empty(),
               "confirmed cancellation was mislabeled as infrastructure failure");
  ok &= expect(contains(interruption.status_detail, "after process-group cleanup"),
               "confirmed cancellation omitted cleanup status");
  ok &= expect(!contains(interruption.status_detail, "attempt"),
               "confirmed cancellation was described as an unconfirmed attempt");
  ok &= expect(control.calls == successful_group_cleanup_calls(105),
               "confirmed cleanup took an unexpected process-control path");
  return ok;
}

bool test_immediate_cleanup_group_signal_failure() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGKILL, false, "injected immediate group signal failure"});
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 106);

  bool ok = expect_unconfirmed_interruption(containment, "injected immediate group signal failure");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "immediate signal failure did not retain the leader anchor");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:106",
                   "signal:106:" + std::to_string(SIGKILL) + ":running",
                   "observe:106",
                   "kill-leader:106",
                 },
               "immediate group-signal failure took an unexpected cleanup path");
  return ok;
}

bool test_immediate_cleanup_anchor_observation_error() {
  ScriptedProcessControl control;
  control.observations.push_back(
    {CompilerLeaderState::Error, "injected leader-anchor observation error"});
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 109);

  bool ok =
    expect_unconfirmed_interruption(containment, "injected leader-anchor observation error");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "leader-anchor observation error did not close cleanup authority");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:109",
                 },
               "anchor-observation error performed an unsafe operation after ownership became "
               "unverifiable");
  return ok;
}

bool test_immediate_cleanup_anchor_ownership_loss() {
  ScriptedProcessControl control;
  control.observations.push_back(
    {CompilerLeaderState::OwnershipLost, "injected leader-anchor ownership loss"});
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 111);

  bool ok = expect_unconfirmed_interruption(containment, "injected leader-anchor ownership loss");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "leader-anchor ownership loss did not close cleanup authority");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:111",
                 },
               "immediate ownership loss performed an unsafe operation after losing the anchor");
  return ok;
}

bool test_immediate_cleanup_direct_leader_kill_failure_loses_anchor() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGKILL, false, "injected immediate group signal failure"});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back({CompilerLeaderState::Running, {}});
  control.observations.push_back(
    {CompilerLeaderState::OwnershipLost, "injected ownership loss after direct kill failure"});
  control.leader_kill_success = false;
  control.leader_kill_detail = "injected direct leader kill failure";
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 112);

  bool ok = expect_unconfirmed_interruption(containment, "injected direct leader kill failure");
  ok &= expect(contains(containment.detail, "injected ownership loss after direct kill failure"),
               "direct-kill failure omitted the terminal ownership-loss context");
  ok &= expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "direct-kill failure retained authority after ownership was lost");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:112",
                   "signal:112:" + std::to_string(SIGKILL) + ":running",
                   "observe:112",
                   "kill-leader:112",
                   "observe:112",
                 },
               "direct-kill failure performed an unsafe operation after losing the anchor");
  return ok;
}

bool expect_invalid_pid_rejected_without_calls(pid_t pid) {
  ScriptedProcessControl cleanup_control;
  const CompilerContainmentOutcome cleanup = clean_compiler_process_group(cleanup_control, pid);
  bool ok = expect_unconfirmed_interruption(cleanup, "positive leader PID");
  ok &= expect_resource_state(cleanup, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "invalid cleanup PID retained process-control authority");
  ok &= expect(cleanup_control.calls.empty(),
               "invalid cleanup PID reached the process-control boundary");

  ScriptedProcessControl termination_control;
  const CompilerContainmentOutcome termination =
    terminate_compiler_process_group(termination_control, pid, std::chrono::milliseconds(20));
  ok &= expect_unconfirmed_interruption(termination, "positive leader PID");
  ok &= expect_resource_state(termination, CompilerContainmentOutcome::ResourceState::OwnershipLost,
                              "invalid termination PID retained process-control authority");
  ok &= expect(termination_control.calls.empty(),
               "invalid termination PID reached the process-control boundary");
  return ok;
}

bool test_invalid_pids_issue_no_process_control_calls() {
  bool ok = expect_invalid_pid_rejected_without_calls(0);
  ok &= expect_invalid_pid_rejected_without_calls(static_cast<pid_t>(-17));
  return ok;
}

bool test_immediate_cleanup_group_audit_failure() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGKILL, true, {}});
  control.audit_success = false;
  control.audit_detail = "injected immediate group audit failure";
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 107);

  bool ok = expect_unconfirmed_interruption(containment, "injected immediate group audit failure");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "immediate audit failure did not retain the leader anchor");
  ok &= expect(control.calls ==
                 std::vector<std::string>{
                   "observe:107",
                   "signal:107:" + std::to_string(SIGKILL) + ":running",
                   "audit:107:107",
                   "observe:107",
                 },
               "immediate group-audit failure took an unexpected cleanup path");
  return ok;
}

bool test_immediate_cleanup_leader_reap_timeout() {
  ScriptedProcessControl control;
  control.signal_results.push_back({SIGKILL, true, {}});
  control.reap_outcome = {CompilerLeaderReapState::AnchorRetained, 0,
                          "injected immediate leader reap timeout"};
  const CompilerContainmentOutcome containment = clean_compiler_process_group(control, 108);

  bool ok = expect_unconfirmed_interruption(containment, "injected immediate leader reap timeout");
  ok &=
    expect_resource_state(containment, CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor,
                          "immediate reap timeout did not retain the leader anchor");
  ok &= expect(control.calls == successful_immediate_cleanup_calls(108),
               "immediate leader-reap timeout took an unexpected cleanup path");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= test_group_kill_failure();
  ok &= test_initial_signal_failure_followed_by_complete_cleanup();
  ok &= test_group_audit_failure();
  ok &= test_leader_reap_timeout_retains_anchor();
  ok &= test_leader_reap_ownership_loss_closes_authority();
  ok &= test_leader_ownership_loss();
  ok &= test_grace_period_ownership_loss_stops_cleanup();
  ok &= test_exited_leader_is_revalidated_before_cleanup();
  ok &= test_confirmed_interruption();
  ok &= test_immediate_cleanup_group_signal_failure();
  ok &= test_immediate_cleanup_anchor_observation_error();
  ok &= test_immediate_cleanup_anchor_ownership_loss();
  ok &= test_immediate_cleanup_direct_leader_kill_failure_loses_anchor();
  ok &= test_immediate_cleanup_group_audit_failure();
  ok &= test_immediate_cleanup_leader_reap_timeout();
  ok &= test_invalid_pids_issue_no_process_control_calls();
  return ok ? 0 : 1;
}
