#pragma once

#include <array>
#include <cstddef>
#include <string>

#if !defined(_WIN32)
#include <signal.h>
#endif

class CompilerTerminationSignalScope {
public:
  CompilerTerminationSignalScope() = default;
  CompilerTerminationSignalScope(const CompilerTerminationSignalScope &) = delete;
  CompilerTerminationSignalScope &operator=(const CompilerTerminationSignalScope &) = delete;
  ~CompilerTerminationSignalScope() noexcept;

  bool arm(std::string &detail);
  bool freeze(std::string &detail);
  bool restore(int &intercepted_signal, std::string &detail);

  [[nodiscard]] int intercepted_signal() const noexcept;
  [[nodiscard]] bool ready_for_execution() const noexcept;
  void suppress_emergency_redelivery() noexcept;

private:
#if !defined(_WIN32)
  bool restore_after_setup_failure(std::string &detail);
  [[nodiscard]] bool collect_owned_pending_signals(int &collection_error) noexcept;
  [[nodiscard]] bool emergency_restore(int &intercepted_signal) noexcept;

  static constexpr std::size_t kSignalCount = 4U;
  sigset_t termination_set_{};
  sigset_t previous_mask_{};
  std::array<struct sigaction, kSignalCount> previous_actions_{};
  std::array<bool, kSignalCount> installed_{};
  int pending_signal_ = 0;
  bool armed_ = false;
  bool frozen_ = false;
  bool emergency_redelivery_enabled_ = true;
#endif
};
