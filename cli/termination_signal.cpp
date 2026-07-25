#include "termination_signal.hpp"

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
#include "freestanding_toolchain_test_hooks.hpp"
#endif

#include <cerrno>
#include <csignal>
#include <cstring>
#include <optional>
#include <utility>

#if !defined(_WIN32)

#include <array>
#include <string_view>

#include <unistd.h>

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
namespace nebula::cli::freestanding_toolchain_testing {
namespace {
std::optional<std::string> injected_restore_failure;
} // namespace

void inject_signal_restore_failure_once(std::string detail) {
  injected_restore_failure = std::move(detail);
}

bool signal_restore_failure_injection_pending() noexcept {
  return injected_restore_failure.has_value();
}

std::optional<std::string> take_injected_signal_restore_failure() {
  std::optional<std::string> result = std::move(injected_restore_failure);
  injected_restore_failure.reset();
  return result;
}
} // namespace nebula::cli::freestanding_toolchain_testing
#endif

namespace {

constexpr std::array<int, 4U> kCompilerTerminationSignals = {
  SIGHUP,
  SIGINT,
  SIGQUIT,
  SIGTERM,
};

static volatile sig_atomic_t compiler_termination_signal = 0;
static volatile sig_atomic_t compiler_termination_scope_active = 0;

void record_compiler_termination_signal(int signal_number) {
  if (compiler_termination_signal == 0)
    compiler_termination_signal = signal_number;
}

void append_detail(std::string &detail, std::string_view additional) {
  if (additional.empty())
    return;
  if (!detail.empty())
    detail += "; ";
  detail += additional;
}

void write_emergency_failure() noexcept {
  constexpr char message[] =
    "[cmd] fatal: compiler termination-signal scope could not be restored\n";
  const char *cursor = message;
  std::size_t remaining = sizeof(message) - 1U;
  while (remaining > 0U) {
    const ssize_t written = ::write(STDERR_FILENO, cursor, remaining);
    if (written > 0) {
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    break;
  }
}

} // namespace

CompilerTerminationSignalScope::~CompilerTerminationSignalScope() noexcept {
  if (!armed_)
    return;
  int intercepted_signal = 0;
  if (!emergency_restore(intercepted_signal)) {
    write_emergency_failure();
    ::_exit(125);
  }
  if (intercepted_signal != 0) {
    if (!emergency_redelivery_enabled_) {
      constexpr char message[] =
        "[cmd] compiler termination signal was suppressed after containment failure\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    } else if (::kill(::getpid(), intercepted_signal) != 0) {
      write_emergency_failure();
      ::_exit(125);
    }
  }
}

bool CompilerTerminationSignalScope::arm(std::string &detail) {
  if (armed_) {
    detail = "compiler termination-signal scope was armed more than once";
    return false;
  }
  installed_.fill(false);
  pending_signal_ = 0;
  frozen_ = false;
  emergency_redelivery_enabled_ = true;
  if (sigemptyset(&termination_set_) != 0) {
    detail =
      "failed to initialize compiler termination-signal set: " + std::string(std::strerror(errno));
    return false;
  }
  for (const int signal_number : kCompilerTerminationSignals) {
    if (sigaddset(&termination_set_, signal_number) != 0) {
      detail =
        "failed to configure compiler termination-signal set: " + std::string(std::strerror(errno));
      return false;
    }
  }
  if (::sigprocmask(SIG_BLOCK, &termination_set_, &previous_mask_) != 0) {
    detail = "failed to block compiler termination signals during scope setup: " +
             std::string(std::strerror(errno));
    return false;
  }
  if (compiler_termination_scope_active != 0) {
    const int restore_error = ::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr);
    detail = "compiler termination-signal scope requires one single-threaded active build";
    if (restore_error != 0) {
      write_emergency_failure();
      ::_exit(125);
    }
    return false;
  }

  compiler_termination_scope_active = 1;
  compiler_termination_signal = 0;
  pending_signal_ = 0;
  armed_ = true;
  frozen_ = true;

  struct sigaction handler_action{};
  handler_action.sa_handler = record_compiler_termination_signal;
  if (sigfillset(&handler_action.sa_mask) != 0) {
    detail = "failed to initialize compiler termination handler mask: " +
             std::string(std::strerror(errno));
    return restore_after_setup_failure(detail);
  }
  handler_action.sa_flags = 0;

  for (std::size_t index = 0; index < kCompilerTerminationSignals.size(); ++index) {
    const int signal_number = kCompilerTerminationSignals[index];
    if (::sigaction(signal_number, nullptr, &previous_actions_[index]) != 0) {
      detail = "failed to inspect prior termination handler for signal " +
               std::to_string(signal_number) + ": " + std::strerror(errno);
      return restore_after_setup_failure(detail);
    }
    const int previously_blocked = sigismember(&previous_mask_, signal_number);
    if (previously_blocked < 0) {
      detail = "failed to inspect the caller signal mask for signal " +
               std::to_string(signal_number) + ": " + std::strerror(errno);
      return restore_after_setup_failure(detail);
    }
    if (previous_actions_[index].sa_handler == SIG_IGN || previously_blocked != 0)
      continue;
    if (::sigaction(signal_number, &handler_action, nullptr) != 0) {
      detail = "failed to install compiler termination handler for signal " +
               std::to_string(signal_number) + ": " + std::strerror(errno);
      return restore_after_setup_failure(detail);
    }
    installed_[index] = true;
  }

  if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) != 0) {
    detail = "failed to restore the caller signal mask after signal-scope setup: " +
             std::string(std::strerror(errno));
    return restore_after_setup_failure(detail);
  }
  frozen_ = false;
  return true;
}

bool CompilerTerminationSignalScope::freeze(std::string &detail) {
  if (!armed_) {
    detail = "compiler termination-signal scope was not armed";
    return false;
  }
  if (frozen_)
    return true;
  if (::sigprocmask(SIG_BLOCK, &termination_set_, nullptr) != 0) {
    detail = "failed to block termination signals during transaction cleanup: " +
             std::string(std::strerror(errno));
    return false;
  }
  if (pending_signal_ == 0)
    pending_signal_ = static_cast<int>(compiler_termination_signal);
  compiler_termination_signal = 0;
  int collection_error = 0;
  if (!collect_owned_pending_signals(collection_error)) {
    detail = "failed to collect transaction-owned pending termination signals: " +
             std::string(std::strerror(collection_error));
    return false;
  }
  frozen_ = true;
  return true;
}

int CompilerTerminationSignalScope::intercepted_signal() const noexcept {
  if (!armed_)
    return 0;
  if (pending_signal_ != 0)
    return pending_signal_;
  return static_cast<int>(compiler_termination_signal);
}

bool CompilerTerminationSignalScope::ready_for_execution() const noexcept {
  return armed_ && !frozen_ && compiler_termination_scope_active != 0;
}

void CompilerTerminationSignalScope::suppress_emergency_redelivery() noexcept {
  emergency_redelivery_enabled_ = false;
}

bool CompilerTerminationSignalScope::restore(int &intercepted_signal, std::string &detail) {
  intercepted_signal = 0;
  if (!armed_)
    return true;
  if (!freeze(detail))
    return false;

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
  if (auto injected =
        nebula::cli::freestanding_toolchain_testing::take_injected_signal_restore_failure();
      injected.has_value()) {
    append_detail(detail, *injected);
    return false;
  }
#endif

  bool handlers_restored = true;
  for (std::size_t index = kCompilerTerminationSignals.size(); index-- > 0U;) {
    if (!installed_[index])
      continue;
    if (::sigaction(kCompilerTerminationSignals[index], &previous_actions_[index], nullptr) != 0) {
      append_detail(detail, "failed to restore termination handler for signal " +
                              std::to_string(kCompilerTerminationSignals[index]) + ": " +
                              std::strerror(errno));
      handlers_restored = false;
      continue;
    }
    installed_[index] = false;
  }
  if (!handlers_restored)
    return false;

  compiler_termination_scope_active = 0;
  if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) != 0) {
    compiler_termination_scope_active = 1;
    append_detail(detail,
                  "failed to restore the caller signal mask: " + std::string(std::strerror(errno)));
    return false;
  }

  armed_ = false;
  frozen_ = false;
  intercepted_signal = pending_signal_;
  pending_signal_ = 0;
  return true;
}

bool CompilerTerminationSignalScope::restore_after_setup_failure(std::string &detail) {
  int intercepted_signal = 0;
  std::string restore_detail;
  if (!restore(intercepted_signal, restore_detail)) {
    append_detail(detail,
                  restore_detail.empty() ? "signal-scope setup rollback failed" : restore_detail);
  } else if (intercepted_signal != 0 && ::kill(::getpid(), intercepted_signal) != 0) {
    constexpr char message[] =
      "[cmd] fatal: compiler setup signal could not be redelivered after rollback\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }
  return false;
}

bool CompilerTerminationSignalScope::collect_owned_pending_signals(int &collection_error) noexcept {
  collection_error = 0;
  sigset_t pending_signals{};
  if (::sigpending(&pending_signals) != 0) {
    collection_error = errno;
    return false;
  }

  // freeze() owns this snapshot as the transaction commit/cancellation handoff. Signals pending
  // before it belong to this transaction and are consumed while our handlers remain installed.
  // Signals generated after the snapshot belong to the restored caller disposition. Signals that
  // the caller had already blocked were never installed_ and remain pending for the caller.
  for (std::size_t index = 0; index < kCompilerTerminationSignals.size(); ++index) {
    if (!installed_[index])
      continue;
    const int signal_number = kCompilerTerminationSignals[index];
    const int is_pending = sigismember(&pending_signals, signal_number);
    if (is_pending < 0) {
      collection_error = errno;
      return false;
    }
    if (is_pending == 0)
      continue;

    sigset_t single_signal{};
    if (sigemptyset(&single_signal) != 0 || sigaddset(&single_signal, signal_number) != 0) {
      collection_error = errno;
      return false;
    }
    int received_signal = 0;
    const int wait_error = ::sigwait(&single_signal, &received_signal);
    if (wait_error != 0) {
      collection_error = wait_error;
      return false;
    }
    if (pending_signal_ == 0)
      pending_signal_ = received_signal;
  }
  return true;
}

bool CompilerTerminationSignalScope::emergency_restore(int &intercepted_signal) noexcept {
  intercepted_signal = 0;
  if (!armed_)
    return true;
  if (!frozen_) {
    if (::sigprocmask(SIG_BLOCK, &termination_set_, nullptr) != 0)
      return false;
    if (pending_signal_ == 0)
      pending_signal_ = static_cast<int>(compiler_termination_signal);
    compiler_termination_signal = 0;
    int collection_error = 0;
    if (!collect_owned_pending_signals(collection_error))
      return false;
    frozen_ = true;
  }
  for (std::size_t index = kCompilerTerminationSignals.size(); index-- > 0U;) {
    if (!installed_[index])
      continue;
    if (::sigaction(kCompilerTerminationSignals[index], &previous_actions_[index], nullptr) != 0)
      return false;
    installed_[index] = false;
  }
  compiler_termination_scope_active = 0;
  if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) != 0) {
    compiler_termination_scope_active = 1;
    return false;
  }
  armed_ = false;
  frozen_ = false;
  intercepted_signal = pending_signal_;
  pending_signal_ = 0;
  return true;
}

#else

CompilerTerminationSignalScope::~CompilerTerminationSignalScope() noexcept = default;

bool CompilerTerminationSignalScope::arm(std::string &detail) {
  detail = "compiler termination-signal scope is unavailable on Windows";
  return false;
}

bool CompilerTerminationSignalScope::freeze(std::string &detail) {
  detail = "compiler termination-signal scope is unavailable on Windows";
  return false;
}

bool CompilerTerminationSignalScope::restore(int &intercepted_signal, std::string &detail) {
  intercepted_signal = 0;
  detail = "compiler termination-signal scope is unavailable on Windows";
  return false;
}

int CompilerTerminationSignalScope::intercepted_signal() const noexcept { return 0; }

bool CompilerTerminationSignalScope::ready_for_execution() const noexcept { return false; }

void CompilerTerminationSignalScope::suppress_emergency_redelivery() noexcept {}

#endif
