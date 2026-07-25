#include "cli/termination_signal.hpp"

#include <csignal>
#include <iostream>
#include <string>

namespace {

volatile sig_atomic_t delivered_signal = 0;

void record_delivered_signal(int signal_number) { delivered_signal = signal_number; }

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "termination-signal-test: " << message << '\n';
  return false;
}

bool signal_is_pending(int signal_number, bool &pending) {
  sigset_t pending_set{};
  if (::sigpending(&pending_set) != 0)
    return false;
  const int membership = sigismember(&pending_set, signal_number);
  if (membership < 0)
    return false;
  pending = membership != 0;
  return true;
}

bool install_handler(int signal_number, struct sigaction &previous_action) {
  struct sigaction action{};
  action.sa_handler = record_delivered_signal;
  if (sigemptyset(&action.sa_mask) != 0)
    return false;
  return ::sigaction(signal_number, &action, &previous_action) == 0;
}

bool restore_handler(int signal_number, const struct sigaction &previous_action) {
  return ::sigaction(signal_number, &previous_action, nullptr) == 0;
}

bool block_signal(int signal_number) {
  sigset_t signal_set{};
  return sigemptyset(&signal_set) == 0 && sigaddset(&signal_set, signal_number) == 0 &&
         ::sigprocmask(SIG_BLOCK, &signal_set, nullptr) == 0;
}

bool test_suppressed_pending_signal_is_consumed() {
  struct sigaction previous_action{};
  if (!install_handler(SIGTERM, previous_action))
    return expect(false, "could not install SIGTERM fixture handler");

  bool ok = true;
  {
    CompilerTerminationSignalScope scope;
    std::string detail;
    ok &= expect(scope.arm(detail), "could not arm termination scope");
    scope.suppress_emergency_redelivery();
    ok &= expect(block_signal(SIGTERM), "could not queue SIGTERM before the freeze boundary");
    ok &= expect(::raise(SIGTERM) == 0, "could not queue blocked SIGTERM");
    bool pending = false;
    ok &= expect(signal_is_pending(SIGTERM, pending) && pending,
                 "SIGTERM was not pending before the freeze boundary");
    ok &= expect(scope.freeze(detail), "could not freeze termination scope");
    pending = true;
    ok &= expect(signal_is_pending(SIGTERM, pending) && !pending,
                 "freeze did not consume its transaction-owned SIGTERM");
    int intercepted_signal = 0;
    ok &= expect(scope.restore(intercepted_signal, detail),
                 "could not restore the suppressed termination scope");
    ok &= expect(intercepted_signal == SIGTERM, "restore did not report the consumed SIGTERM");
    ok &= expect(delivered_signal == 0, "suppressed SIGTERM reached the caller handler");
    pending = true;
    ok &= expect(signal_is_pending(SIGTERM, pending) && !pending,
                 "suppressed SIGTERM remained pending after restore");
  }

  ok &= expect(restore_handler(SIGTERM, previous_action),
               "could not restore the SIGTERM fixture handler");
  return ok;
}

bool test_caller_blocked_pending_signal_is_preserved() {
  sigset_t blocked_signal{};
  sigset_t previous_mask{};
  if (sigemptyset(&blocked_signal) != 0 || sigaddset(&blocked_signal, SIGHUP) != 0 ||
      ::sigprocmask(SIG_BLOCK, &blocked_signal, &previous_mask) != 0) {
    return expect(false, "could not block the caller-owned SIGHUP fixture");
  }

  bool ok = true;
  ok &= expect(::raise(SIGHUP) == 0, "could not queue caller-owned SIGHUP");
  {
    CompilerTerminationSignalScope scope;
    std::string detail;
    ok &= expect(scope.arm(detail), "could not arm scope with caller-blocked SIGHUP");
    ok &= expect(scope.freeze(detail), "could not freeze scope with caller-blocked SIGHUP");
    int intercepted_signal = 0;
    ok &= expect(scope.restore(intercepted_signal, detail),
                 "could not restore scope with caller-blocked SIGHUP");
    ok &= expect(intercepted_signal == 0, "scope consumed a caller-blocked SIGHUP");
  }

  bool pending = false;
  ok &= expect(signal_is_pending(SIGHUP, pending) && pending,
               "caller-blocked SIGHUP was not preserved");
  int consumed_signal = 0;
  ok &= expect(::sigwait(&blocked_signal, &consumed_signal) == 0 && consumed_signal == SIGHUP,
               "could not consume the preserved caller-owned SIGHUP");
  ok &= expect(::sigprocmask(SIG_SETMASK, &previous_mask, nullptr) == 0,
               "could not restore the caller signal mask");
  return ok;
}

bool test_emergency_restore_consumes_suppressed_pending_signal() {
  struct sigaction previous_action{};
  if (!install_handler(SIGINT, previous_action))
    return expect(false, "could not install SIGINT fixture handler");

  delivered_signal = 0;
  bool ok = true;
  {
    CompilerTerminationSignalScope scope;
    std::string detail;
    ok &= expect(scope.arm(detail), "could not arm emergency-restore scope");
    scope.suppress_emergency_redelivery();
    ok &=
      expect(block_signal(SIGINT), "could not queue SIGINT before the emergency freeze boundary");
    ok &= expect(::raise(SIGINT) == 0, "could not queue blocked SIGINT");
  }

  bool pending = true;
  ok &= expect(delivered_signal == 0, "emergency restore delivered a suppressed SIGINT");
  ok &= expect(signal_is_pending(SIGINT, pending) && !pending,
               "emergency restore left a suppressed SIGINT pending");
  ok &= expect(restore_handler(SIGINT, previous_action),
               "could not restore the SIGINT fixture handler");
  return ok;
}

bool test_post_freeze_signal_is_handed_to_caller() {
  struct sigaction previous_action{};
  if (!install_handler(SIGQUIT, previous_action))
    return expect(false, "could not install SIGQUIT fixture handler");

  delivered_signal = 0;
  bool ok = true;
  {
    CompilerTerminationSignalScope scope;
    std::string detail;
    ok &= expect(scope.arm(detail), "could not arm post-freeze handoff scope");
    ok &= expect(scope.freeze(detail), "could not freeze post-freeze handoff scope");
    ok &= expect(::raise(SIGQUIT) == 0, "could not queue post-freeze SIGQUIT");
    int intercepted_signal = 0;
    ok &= expect(scope.restore(intercepted_signal, detail),
                 "could not restore post-freeze handoff scope");
    ok &=
      expect(intercepted_signal == 0, "scope claimed a signal generated after the freeze boundary");
  }

  bool pending = true;
  ok &= expect(delivered_signal == SIGQUIT, "post-freeze SIGQUIT did not reach caller handler");
  ok &= expect(signal_is_pending(SIGQUIT, pending) && !pending,
               "post-freeze SIGQUIT remained pending after caller handoff");
  ok &= expect(restore_handler(SIGQUIT, previous_action),
               "could not restore the SIGQUIT fixture handler");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= test_suppressed_pending_signal_is_consumed();
  ok &= test_caller_blocked_pending_signal_is_preserved();
  ok &= test_emergency_restore_consumes_suppressed_pending_signal();
  ok &= test_post_freeze_signal_is_handed_to_caller();
  return ok ? 0 : 1;
}
