#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "host_process.hpp"
#include "cli/host_process_capabilities.hpp"
#include "termination_signal.hpp"

#if defined(NEBULA_HOST_PROCESS_TESTING)
#include "host_process_test_hooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include "compiler_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace nebula::cli {

#if defined(NEBULA_HOST_PROCESS_TESTING)
namespace host_process_testing {
namespace {
bool inject_post_spawn_exception = false;
bool inject_unbounded_post_spawn_termination_failure = false;
bool fail_next_cleanup_termination = false;
bool inject_post_cleanup_diagnostic_exception = false;
bool inject_parent_stdin_endpoint_close_error = false;
std::uint64_t injected_process_id = 0U;
} // namespace

void inject_post_spawn_exception_once() noexcept { inject_post_spawn_exception = true; }

bool post_spawn_exception_pending() noexcept { return inject_post_spawn_exception; }

void inject_unbounded_post_spawn_termination_failure_once() noexcept {
  inject_unbounded_post_spawn_termination_failure = true;
}

bool unbounded_post_spawn_termination_failure_pending() noexcept {
  return inject_unbounded_post_spawn_termination_failure || fail_next_cleanup_termination;
}

void inject_post_cleanup_diagnostic_exception_once() noexcept {
  inject_post_cleanup_diagnostic_exception = true;
}

bool post_cleanup_diagnostic_exception_pending() noexcept {
  return inject_post_cleanup_diagnostic_exception;
}

void inject_parent_stdin_endpoint_close_error_once() noexcept {
  inject_parent_stdin_endpoint_close_error = true;
}

bool parent_stdin_endpoint_close_error_pending() noexcept {
  return inject_parent_stdin_endpoint_close_error;
}

std::uint64_t last_injected_process_id() noexcept { return injected_process_id; }

bool take_post_spawn_exception(std::uint64_t process_id) noexcept {
  if (!inject_post_spawn_exception)
    return false;
  inject_post_spawn_exception = false;
  injected_process_id = process_id;
  return true;
}

bool take_unbounded_post_spawn_termination_failure(std::uint64_t process_id) noexcept {
  if (!inject_unbounded_post_spawn_termination_failure)
    return false;
  inject_unbounded_post_spawn_termination_failure = false;
  fail_next_cleanup_termination = true;
  injected_process_id = process_id;
  return true;
}

bool take_cleanup_termination_failure() noexcept {
  if (!fail_next_cleanup_termination)
    return false;
  fail_next_cleanup_termination = false;
  return true;
}

bool take_post_cleanup_diagnostic_exception() noexcept {
  if (!inject_post_cleanup_diagnostic_exception)
    return false;
  inject_post_cleanup_diagnostic_exception = false;
  return true;
}

bool take_parent_stdin_endpoint_close_error(std::uint64_t process_id) noexcept {
  if (!inject_parent_stdin_endpoint_close_error)
    return false;
  inject_parent_stdin_endpoint_close_error = false;
  injected_process_id = process_id;
  return true;
}
} // namespace host_process_testing
#endif

namespace {

bool contains_nul(std::string_view value) { return value.find('\0') != std::string_view::npos; }

bool contains_nul(std::wstring_view value) { return value.find(L'\0') != std::wstring_view::npos; }

bool valid_environment_name(std::string_view name) {
  if (name.empty() || contains_nul(name))
    return false;
  const auto ascii_alpha_or_underscore = [](unsigned char value) {
    return (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) ||
           value == static_cast<unsigned char>('_');
  };
  const auto ascii_digit = [](unsigned char value) {
    return value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9');
  };
  if (!ascii_alpha_or_underscore(static_cast<unsigned char>(name.front())))
    return false;
  return std::all_of(name.begin() + 1, name.end(), [&](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return ascii_alpha_or_underscore(byte) || ascii_digit(byte);
  });
}

wchar_t ascii_fold(wchar_t value) {
  return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

bool windows_name_equal(std::wstring_view lhs, std::wstring_view rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t index = 0U; index < lhs.size(); ++index) {
    if (ascii_fold(lhs[index]) != ascii_fold(rhs[index]))
      return false;
  }
  return true;
}

bool windows_text_less(std::wstring_view lhs, std::wstring_view rhs) {
  const std::size_t shared = std::min(lhs.size(), rhs.size());
  for (std::size_t index = 0U; index < shared; ++index) {
    const wchar_t lhs_folded = ascii_fold(lhs[index]);
    const wchar_t rhs_folded = ascii_fold(rhs[index]);
    if (lhs_folded != rhs_folded)
      return lhs_folded < rhs_folded;
  }
  if (lhs.size() != rhs.size())
    return lhs.size() < rhs.size();
  return lhs < rhs;
}

std::wstring_view windows_environment_entry_name(std::wstring_view entry) {
  if (entry.empty())
    return {};
  const std::size_t search_start = entry.front() == L'=' ? 1U : 0U;
  const std::size_t separator = entry.find(L'=', search_start);
  if (separator == std::wstring_view::npos)
    return {};
  return entry.substr(0U, separator);
}

void append_detail(std::string &detail, std::string_view additional) {
  if (additional.empty())
    return;
  if (!detail.empty())
    detail += "; ";
  detail += additional;
}

void append_captured_bytes(std::string &output, const char *bytes, std::size_t byte_count,
                           std::size_t limit, bool &limit_exceeded) {
  if (byte_count == 0U)
    return;
  const std::size_t remaining = output.size() < limit ? limit - output.size() : 0U;
  const std::size_t accepted = std::min(remaining, byte_count);
  output.append(bytes, accepted);
  if (accepted != byte_count)
    limit_exceeded = true;
}

std::string validate_request(const HostProcessRequest &request) {
  if (request.arguments.empty())
    return "host process requires a nonempty argument vector";
  const std::string executable = request.executable_path.string();
  if (executable.empty() || contains_nul(executable) || !request.executable_path.is_absolute()) {
    return "host process executable must be an absolute NUL-free path";
  }
  if (std::any_of(request.arguments.begin(), request.arguments.end(),
                  [](const std::string &argument) { return contains_nul(argument); })) {
    return "host process arguments must not contain NUL";
  }
#if defined(_WIN32)
  if (request.termination_signals != nullptr) {
    return "host process termination signal control is unavailable on Windows";
  }
#else
  if (request.termination_signals != nullptr &&
      !request.termination_signals->ready_for_execution()) {
    return "host process termination boundary is not armed for execution";
  }
  if (request.termination_signals != nullptr && request.timeout_milliseconds == 0U) {
    return "host process termination boundary requires a positive timeout";
  }
#endif
  const auto validate_stream = [](HostProcessStreamMode mode, std::size_t limit,
                                  std::string_view label) -> std::string {
    if (mode == HostProcessStreamMode::Capture && limit == 0U) {
      return std::string(label) + " capture requires a positive byte limit";
    }
    if (mode != HostProcessStreamMode::Capture && limit != 0U) {
      return std::string(label) + " byte limit is only valid for capture mode";
    }
    return {};
  };
  if (std::string error = validate_stream(request.stdout_mode, request.max_stdout_bytes, "stdout");
      !error.empty()) {
    return error;
  }
  if (std::string error = validate_stream(request.stderr_mode, request.max_stderr_bytes, "stderr");
      !error.empty()) {
    return error;
  }

  for (std::size_t index = 0U; index < request.environment_overrides.size(); ++index) {
    const HostEnvironmentOverride &current = request.environment_overrides[index];
    if (!valid_environment_name(current.name) || contains_nul(current.value)) {
      return "host process contains an invalid environment override";
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
#if defined(_WIN32)
      std::wstring current_name(current.name.begin(), current.name.end());
      std::wstring previous_name(request.environment_overrides[previous].name.begin(),
                                 request.environment_overrides[previous].name.end());
      if (windows_name_equal(current_name, previous_name)) {
#else
      if (current.name == request.environment_overrides[previous].name) {
#endif
        return "host process contains duplicate environment overrides";
      }
    }
  }
  return {};
}

void finalize_capture_limits(const HostProcessRequest &request, HostProcessResult &result) {
  if (result.stdout_limit_exceeded) {
    append_detail(result.infrastructure_error, "stdout capture exceeded its " +
                                                 std::to_string(request.max_stdout_bytes) +
                                                 "-byte limit");
  }
  if (result.stderr_limit_exceeded) {
    append_detail(result.infrastructure_error, "stderr capture exceeded its " +
                                                 std::to_string(request.max_stderr_bytes) +
                                                 "-byte limit");
  }
}

#if defined(_WIN32)

std::string windows_error_message(DWORD error) {
  return std::system_category().message(static_cast<int>(error));
}

bool utf8_to_wide(std::string_view input, std::wstring &output, std::string &error) {
  output.clear();
  if (input.empty())
    return true;
  if (input.size() > static_cast<std::size_t>(INT_MAX)) {
    error = "host process UTF-8 input exceeds the Win32 conversion bound";
    return false;
  }
  const int input_size = static_cast<int>(input.size());
  const int required =
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), input_size, nullptr, 0);
  if (required <= 0) {
    error = "host process input is not valid UTF-8: " + windows_error_message(::GetLastError());
    return false;
  }
  output.resize(static_cast<std::size_t>(required));
  const int converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                              input_size, output.data(), required);
  if (converted != required) {
    error = "host process UTF-8 conversion failed: " + windows_error_message(::GetLastError());
    output.clear();
    return false;
  }
  return true;
}

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;
  UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.release()) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueHandle() { reset(); }

  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  HANDLE release() noexcept {
    HANDLE value = handle_;
    handle_ = nullptr;
    return value;
  }
  void reset(HANDLE replacement = nullptr) noexcept {
    if (valid())
      (void)::CloseHandle(handle_);
    handle_ = replacement;
  }

  DWORD close_with_error() noexcept {
    if (!valid())
      return ERROR_SUCCESS;
    if (::CloseHandle(handle_) == FALSE)
      return ::GetLastError();
    handle_ = nullptr;
    return ERROR_SUCCESS;
  }

private:
  HANDLE handle_ = nullptr;
};

bool create_kill_on_close_job(UniqueHandle &job, std::string &error) {
  HANDLE handle = ::CreateJobObjectW(nullptr, nullptr);
  if (handle == nullptr) {
    error = "failed to create the Windows host-process Job Object: " +
            windows_error_message(::GetLastError());
    return false;
  }
  job.reset(handle);

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
    error = "failed to configure the Windows host-process Job Object: " +
            windows_error_message(::GetLastError());
    job.reset();
    return false;
  }
  return true;
}

enum class WindowsChildOwnershipPhase : std::uint8_t {
  PreJob,
  InJob,
  CleanupUnconfirmed,
  Released,
};

struct WindowsChildCleanupOutcome {
  bool already_exited = false;
  bool terminate_requested = false;
  bool process_waited = false;
  bool job_quiescent = false;
  DWORD exit_probe_error = ERROR_SUCCESS;
  DWORD terminate_error = ERROR_SUCCESS;
  DWORD wait_error = ERROR_SUCCESS;
  DWORD quiescence_error = ERROR_SUCCESS;

  [[nodiscard]] bool confirmed(WindowsChildOwnershipPhase phase) const noexcept {
    const bool termination_controlled = phase == WindowsChildOwnershipPhase::InJob
                                          ? terminate_requested
                                          : already_exited || terminate_requested;
    return termination_controlled && process_waited &&
           (phase == WindowsChildOwnershipPhase::PreJob || job_quiescent);
  }

  [[nodiscard]] bool native_resources_gone(WindowsChildOwnershipPhase phase) const noexcept {
    return process_waited && (phase == WindowsChildOwnershipPhase::PreJob || job_quiescent);
  }
};

struct WindowsChildEndpointCloseOutcome {
  DWORD stdin_error = ERROR_SUCCESS;
  DWORD stdout_error = ERROR_SUCCESS;
  DWORD stderr_error = ERROR_SUCCESS;

  [[nodiscard]] bool complete() const noexcept {
    return stdin_error == ERROR_SUCCESS && stdout_error == ERROR_SUCCESS &&
           stderr_error == ERROR_SUCCESS;
  }
};

bool wait_for_job_quiescence_noexcept(HANDLE job, DWORD timeout_milliseconds,
                                      DWORD &error) noexcept {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_milliseconds;
  while (true) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                     sizeof(accounting), nullptr)) {
      error = ::GetLastError();
      return false;
    }
    if (accounting.ActiveProcesses == 0U)
      return true;
    if (::GetTickCount64() >= deadline) {
      error = WAIT_TIMEOUT;
      return false;
    }
    ::Sleep(10U);
  }
}

WindowsChildCleanupOutcome
cleanup_windows_child_noexcept(HANDLE process, HANDLE job,
                               WindowsChildOwnershipPhase phase) noexcept {
  constexpr DWORD kCleanupWaitMilliseconds = 2000U;
  WindowsChildCleanupOutcome outcome;
  const DWORD exit_probe = ::WaitForSingleObject(process, 0U);
  outcome.already_exited = exit_probe == WAIT_OBJECT_0;
  if (exit_probe == WAIT_FAILED)
    outcome.exit_probe_error = ::GetLastError();

  if (phase != WindowsChildOwnershipPhase::PreJob || !outcome.already_exited) {
#if defined(NEBULA_HOST_PROCESS_TESTING)
    const bool inject_termination_failure =
      host_process_testing::take_cleanup_termination_failure();
#else
    constexpr bool inject_termination_failure = false;
#endif
    if (inject_termination_failure) {
      outcome.terminate_error = ERROR_ACCESS_DENIED;
    } else {
      if (phase == WindowsChildOwnershipPhase::PreJob) {
        outcome.terminate_requested = ::TerminateProcess(process, 125U) != FALSE;
      } else {
        outcome.terminate_requested = ::TerminateJobObject(job, 125U) != FALSE;
      }
      if (!outcome.terminate_requested)
        outcome.terminate_error = ::GetLastError();
    }
  }

  const DWORD process_wait = ::WaitForSingleObject(process, kCleanupWaitMilliseconds);
  outcome.process_waited = process_wait == WAIT_OBJECT_0;
  if (!outcome.process_waited)
    outcome.wait_error = process_wait == WAIT_FAILED ? ::GetLastError() : process_wait;

  if (phase == WindowsChildOwnershipPhase::PreJob) {
    outcome.job_quiescent = true;
  } else {
    outcome.job_quiescent =
      wait_for_job_quiescence_noexcept(job, kCleanupWaitMilliseconds, outcome.quiescence_error);
  }
  return outcome;
}

[[noreturn]] void fatal_windows_child_cleanup() noexcept {
  constexpr char message[] =
    "[cmd] fatal: Windows child cleanup was unconfirmed during exception cleanup\n";
  const HANDLE standard_error = ::GetStdHandle(STD_ERROR_HANDLE);
  if (standard_error != nullptr && standard_error != INVALID_HANDLE_VALUE) {
    DWORD written = 0U;
    (void)::WriteFile(standard_error, message, static_cast<DWORD>(sizeof(message) - 1U), &written,
                      nullptr);
  }
  (void)::TerminateProcess(::GetCurrentProcess(), 125U);
  ::ExitProcess(125U);
}

[[noreturn]] void fatal_windows_child_lifecycle_invariant() noexcept {
  constexpr char message[] =
    "[cmd] fatal: Windows child ownership guard reached normal destruction while active\n";
  const HANDLE standard_error = ::GetStdHandle(STD_ERROR_HANDLE);
  if (standard_error != nullptr && standard_error != INVALID_HANDLE_VALUE) {
    DWORD written = 0U;
    (void)::WriteFile(standard_error, message, static_cast<DWORD>(sizeof(message) - 1U), &written,
                      nullptr);
  }
  (void)::TerminateProcess(::GetCurrentProcess(), 125U);
  ::ExitProcess(125U);
}

class WindowsChildOwnershipGuard final {
public:
  WindowsChildOwnershipGuard(HANDLE process, HANDLE job, WindowsChildOwnershipPhase phase) noexcept
      : process_(process), job_(job), phase_(phase), cleanup_phase_(phase),
        uncaught_exceptions_baseline_(std::uncaught_exceptions()) {}
  WindowsChildOwnershipGuard(const WindowsChildOwnershipGuard &) = delete;
  WindowsChildOwnershipGuard &operator=(const WindowsChildOwnershipGuard &) = delete;

  ~WindowsChildOwnershipGuard() noexcept {
    if (phase_ == WindowsChildOwnershipPhase::Released)
      return;
    if (phase_ == WindowsChildOwnershipPhase::CleanupUnconfirmed) {
      if (cleanup_resources_gone_)
        fatal_windows_child_cleanup();
      const WindowsChildCleanupOutcome emergency_cleanup =
        cleanup_windows_child_noexcept(process_, job_, cleanup_phase_);
      if (!emergency_cleanup.native_resources_gone(cleanup_phase_) ||
          std::uncaught_exceptions() > uncaught_exceptions_baseline_) {
        fatal_windows_child_cleanup();
      }
      return;
    }
    const WindowsChildOwnershipPhase cleanup_phase = phase_;
    phase_ = WindowsChildOwnershipPhase::Released;
    const WindowsChildCleanupOutcome cleanup =
      cleanup_windows_child_noexcept(process_, job_, cleanup_phase);
    if (!cleanup.confirmed(cleanup_phase))
      fatal_windows_child_cleanup();
    if (std::uncaught_exceptions() == uncaught_exceptions_baseline_)
      fatal_windows_child_lifecycle_invariant();
  }

  void enter_job() noexcept { phase_ = WindowsChildOwnershipPhase::InJob; }

  WindowsChildCleanupOutcome cleanup_now() noexcept {
    const WindowsChildOwnershipPhase cleanup_phase = phase_;
    cleanup_phase_ = cleanup_phase;
    const WindowsChildCleanupOutcome cleanup =
      cleanup_windows_child_noexcept(process_, job_, cleanup_phase);
    cleanup_resources_gone_ = cleanup.native_resources_gone(cleanup_phase);
    phase_ = cleanup.confirmed(cleanup_phase) ? WindowsChildOwnershipPhase::Released
                                              : WindowsChildOwnershipPhase::CleanupUnconfirmed;
    return cleanup;
  }

  void release() noexcept { phase_ = WindowsChildOwnershipPhase::Released; }

  void acknowledge_cleanup_result() noexcept {
    if (phase_ == WindowsChildOwnershipPhase::CleanupUnconfirmed && cleanup_resources_gone_)
      phase_ = WindowsChildOwnershipPhase::Released;
  }

  [[nodiscard]] WindowsChildOwnershipPhase phase() const noexcept { return phase_; }

private:
  HANDLE process_ = nullptr;
  HANDLE job_ = nullptr;
  WindowsChildOwnershipPhase phase_ = WindowsChildOwnershipPhase::Released;
  WindowsChildOwnershipPhase cleanup_phase_ = WindowsChildOwnershipPhase::Released;
  bool cleanup_resources_gone_ = false;
  int uncaught_exceptions_baseline_ = 0;
};

void append_windows_cleanup_outcome(const WindowsChildCleanupOutcome &cleanup,
                                    WindowsChildOwnershipPhase phase, std::string &detail) {
  if (cleanup.exit_probe_error != ERROR_SUCCESS) {
    append_detail(detail, "failed to inspect the Windows child before cleanup: " +
                            windows_error_message(cleanup.exit_probe_error));
  }
  if (!cleanup.terminate_requested && cleanup.terminate_error != ERROR_SUCCESS) {
    append_detail(detail, std::string(phase == WindowsChildOwnershipPhase::PreJob
                                        ? "failed to terminate the uncontained Windows child: "
                                        : "failed to terminate the Windows child Job Object: ") +
                            windows_error_message(cleanup.terminate_error));
  }
  if (!cleanup.process_waited) {
    append_detail(detail, cleanup.wait_error == WAIT_TIMEOUT
                            ? "Windows child cleanup could not be confirmed within 2000ms"
                            : "failed to wait for the Windows child during cleanup: " +
                                windows_error_message(cleanup.wait_error));
  }
  if (phase == WindowsChildOwnershipPhase::InJob && !cleanup.job_quiescent) {
    append_detail(detail, cleanup.quiescence_error == WAIT_TIMEOUT
                            ? "Windows child Job Object retained active processes after cleanup"
                            : "failed to audit Windows child Job Object quiescence: " +
                                windows_error_message(cleanup.quiescence_error));
  }
}

void append_windows_child_endpoint_close_outcome(
  const WindowsChildEndpointCloseOutcome &close_outcome, std::string &detail) {
  const auto append_error = [&](DWORD error, std::string_view label) {
    if (error != ERROR_SUCCESS) {
      append_detail(detail, "failed to close the parent-side Windows " + std::string(label) +
                              " child endpoint after launch: " + windows_error_message(error));
    }
  };
  append_error(close_outcome.stdin_error, "stdin");
  append_error(close_outcome.stdout_error, "stdout");
  append_error(close_outcome.stderr_error, "stderr");
}

bool duplicate_inheritable_handle(HANDLE source, UniqueHandle &output, std::string &error) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE) {
    error = "requested inherited standard stream is unavailable";
    return false;
  }
  HANDLE duplicate = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &duplicate, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
    error = "failed to duplicate a standard stream: " + windows_error_message(::GetLastError());
    return false;
  }
  output.reset(duplicate);
  return true;
}

bool prepare_windows_input_stream(HostProcessInputMode mode, UniqueHandle &child_handle,
                                  std::string &error) {
  if (mode == HostProcessInputMode::Inherit) {
    return duplicate_inheritable_handle(::GetStdHandle(STD_INPUT_HANDLE), child_handle, error);
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  HANDLE null_handle = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_handle == INVALID_HANDLE_VALUE) {
    error =
      "failed to open the Windows null input device: " + windows_error_message(::GetLastError());
    return false;
  }
  child_handle.reset(null_handle);
  return true;
}

struct WindowsOutputStream {
  UniqueHandle parent_read;
  UniqueHandle child_handle;
};

bool prepare_windows_output_stream(HostProcessStreamMode mode, DWORD standard_handle,
                                   WindowsOutputStream &stream, std::string &error) {
  if (mode == HostProcessStreamMode::Inherit) {
    return duplicate_inheritable_handle(::GetStdHandle(standard_handle), stream.child_handle,
                                        error);
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  if (mode == HostProcessStreamMode::Discard) {
    HANDLE null_handle = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_handle == INVALID_HANDLE_VALUE) {
      error = "failed to open the Windows null device: " + windows_error_message(::GetLastError());
      return false;
    }
    stream.child_handle.reset(null_handle);
    return true;
  }

  HANDLE read_handle = nullptr;
  HANDLE write_handle = nullptr;
  if (!::CreatePipe(&read_handle, &write_handle, &security, 0)) {
    error = "failed to create a Windows capture pipe: " + windows_error_message(::GetLastError());
    return false;
  }
  stream.parent_read.reset(read_handle);
  stream.child_handle.reset(write_handle);
  if (!::SetHandleInformation(stream.parent_read.get(), HANDLE_FLAG_INHERIT, 0)) {
    error =
      "failed to make a Windows capture handle private: " + windows_error_message(::GetLastError());
    return false;
  }
  return true;
}

class ProcessAttributeList {
public:
  ProcessAttributeList() = default;
  ProcessAttributeList(const ProcessAttributeList &) = delete;
  ProcessAttributeList &operator=(const ProcessAttributeList &) = delete;
  ~ProcessAttributeList() {
    if (list_ != nullptr)
      ::DeleteProcThreadAttributeList(list_);
    if (storage_ != nullptr)
      (void)::HeapFree(::GetProcessHeap(), 0, storage_);
  }

  bool initialize(std::string &error) {
    SIZE_T bytes = 0U;
    (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0U || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      error = "failed to size the Windows process attribute list: " +
              windows_error_message(::GetLastError());
      return false;
    }
    storage_ = ::HeapAlloc(::GetProcessHeap(), 0, bytes);
    if (storage_ == nullptr) {
      error = "failed to allocate the Windows process attribute list";
      return false;
    }
    list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_);
    if (!::InitializeProcThreadAttributeList(list_, 1, 0, &bytes)) {
      error = "failed to initialize the Windows process attribute list: " +
              windows_error_message(::GetLastError());
      list_ = nullptr;
      return false;
    }
    return true;
  }

  bool set_handle_list(std::array<HANDLE, 3U> &handles, std::string &error) {
    if (!::UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                     sizeof(handles), nullptr, nullptr)) {
      error = "failed to restrict Windows child handle inheritance: " +
              windows_error_message(::GetLastError());
      return false;
    }
    return true;
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
  void *storage_ = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

bool read_available_windows_pipe(UniqueHandle &pipe, std::string &output, std::size_t limit,
                                 bool &limit_exceeded, std::string &error) {
  constexpr DWORD kReadBufferBytes = 4096U;
  constexpr std::size_t kMaxPumpBytes = 64U * 1024U;
  std::array<char, kReadBufferBytes> buffer{};
  std::size_t pumped_bytes = 0U;
  while (pipe.valid() && pumped_bytes < kMaxPumpBytes) {
    DWORD available = 0U;
    if (!::PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
      const DWORD pipe_error = ::GetLastError();
      if (pipe_error == ERROR_BROKEN_PIPE) {
        pipe.reset();
        return true;
      }
      error = "failed to inspect a Windows capture pipe: " + windows_error_message(pipe_error);
      pipe.reset();
      return false;
    }
    if (available == 0U)
      return true;

    const DWORD requested = std::min(available, kReadBufferBytes);
    DWORD bytes_read = 0U;
    if (!::ReadFile(pipe.get(), buffer.data(), requested, &bytes_read, nullptr)) {
      const DWORD pipe_error = ::GetLastError();
      if (pipe_error == ERROR_BROKEN_PIPE) {
        pipe.reset();
        return true;
      }
      error = "failed to read a Windows capture pipe: " + windows_error_message(pipe_error);
      pipe.reset();
      return false;
    }
    if (bytes_read == 0U) {
      pipe.reset();
      return true;
    }
    append_captured_bytes(output, buffer.data(), static_cast<std::size_t>(bytes_read), limit,
                          limit_exceeded);
    pumped_bytes += static_cast<std::size_t>(bytes_read);
  }
  return true;
}

HostProcessResult run_windows_host_process(const HostProcessRequest &request) {
  HostProcessResult result;
  const bool bounded = request.timeout_milliseconds != 0U;
  std::wstring wide_executable;
  if (!utf8_to_wide(request.executable_path.string(), wide_executable,
                    result.infrastructure_error)) {
    return result;
  }
  std::vector<std::wstring> wide_arguments;
  wide_arguments.reserve(request.arguments.size());
  for (const std::string &argument : request.arguments) {
    std::wstring wide;
    if (!utf8_to_wide(argument, wide, result.infrastructure_error))
      return result;
    wide_arguments.push_back(std::move(wide));
  }

  struct EnvironmentBlockGuard {
    wchar_t *block = nullptr;
    ~EnvironmentBlockGuard() {
      if (block != nullptr)
        (void)::FreeEnvironmentStringsW(block);
    }
  } inherited_block;
  std::vector<std::wstring> inherited_entries;
  if (request.inherit_environment) {
    inherited_block.block = ::GetEnvironmentStringsW();
    if (inherited_block.block == nullptr) {
      result.infrastructure_error =
        "failed to snapshot the Windows environment: " + windows_error_message(::GetLastError());
      return result;
    }
    for (const wchar_t *entry = inherited_block.block; *entry != L'\0';
         entry += std::wcslen(entry) + 1U) {
      inherited_entries.emplace_back(entry);
    }
  }

  std::vector<std::pair<std::wstring, std::wstring>> wide_overrides;
  wide_overrides.reserve(request.environment_overrides.size());
  for (const HostEnvironmentOverride &environment_override : request.environment_overrides) {
    std::wstring name;
    std::wstring value;
    if (!utf8_to_wide(environment_override.name, name, result.infrastructure_error) ||
        !utf8_to_wide(environment_override.value, value, result.infrastructure_error)) {
      return result;
    }
    wide_overrides.emplace_back(std::move(name), std::move(value));
  }
  detail::WindowsEnvironmentBlockResult environment =
    detail::build_windows_environment_block(inherited_entries, wide_overrides);
  if (!environment.error.empty()) {
    result.infrastructure_error = std::move(environment.error);
    return result;
  }

  UniqueHandle child_stdin;
  if (!prepare_windows_input_stream(request.stdin_mode, child_stdin, result.infrastructure_error)) {
    return result;
  }
  WindowsOutputStream stdout_stream;
  WindowsOutputStream stderr_stream;
  if (!prepare_windows_output_stream(request.stdout_mode, STD_OUTPUT_HANDLE, stdout_stream,
                                     result.infrastructure_error) ||
      !prepare_windows_output_stream(request.stderr_mode, STD_ERROR_HANDLE, stderr_stream,
                                     result.infrastructure_error)) {
    return result;
  }

  ProcessAttributeList attributes;
  if (!attributes.initialize(result.infrastructure_error))
    return result;
  std::array<HANDLE, 3U> inherited_handles = {child_stdin.get(), stdout_stream.child_handle.get(),
                                              stderr_stream.child_handle.get()};
  if (!attributes.set_handle_list(inherited_handles, result.infrastructure_error))
    return result;

  std::wstring command_line;
  for (std::size_t index = 0U; index < wide_arguments.size(); ++index) {
    if (index != 0U)
      command_line.push_back(L' ');
    command_line += detail::quote_windows_argument(wide_arguments[index]);
  }
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_stdin.get();
  startup.StartupInfo.hStdOutput = stdout_stream.child_handle.get();
  startup.StartupInfo.hStdError = stderr_stream.child_handle.get();
  startup.lpAttributeList = attributes.get();

  UniqueHandle containment_job;
  if (bounded && !create_kill_on_close_job(containment_job, result.infrastructure_error)) {
    return result;
  }

  PROCESS_INFORMATION process_info{};
  DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
  if (bounded)
    creation_flags |= CREATE_SUSPENDED;
  if (!::CreateProcessW(wide_executable.c_str(), mutable_command_line.data(), nullptr, nullptr,
                        TRUE, creation_flags, environment.block.data(), nullptr,
                        &startup.StartupInfo, &process_info)) {
    result.infrastructure_error =
      "failed to start the Windows host process: " + windows_error_message(::GetLastError());
    return result;
  }
  result.started = true;
  if (bounded)
    result.containment = HostProcessContainment::Unconfirmed;
  UniqueHandle process(process_info.hProcess);
  UniqueHandle primary_thread(process_info.hThread);
  WindowsChildOwnershipGuard child_ownership(process.get(), containment_job.get(),
                                             WindowsChildOwnershipPhase::PreJob);
  WindowsChildEndpointCloseOutcome endpoint_close = {
    child_stdin.close_with_error(),
    stdout_stream.child_handle.close_with_error(),
    stderr_stream.child_handle.close_with_error(),
  };
#if defined(NEBULA_HOST_PROCESS_TESTING)
  if (endpoint_close.stdin_error == ERROR_SUCCESS &&
      host_process_testing::take_parent_stdin_endpoint_close_error(
        static_cast<std::uint64_t>(process_info.dwProcessId))) {
    endpoint_close.stdin_error = ERROR_INVALID_HANDLE;
  }
#endif

#if defined(NEBULA_HOST_PROCESS_TESTING)
  if (host_process_testing::take_post_spawn_exception(
        static_cast<std::uint64_t>(process_info.dwProcessId))) {
    throw std::runtime_error("injected exception after Windows child ownership guard installation");
  }
  const bool injected_termination_failure =
    !bounded && host_process_testing::take_unbounded_post_spawn_termination_failure(
                  static_cast<std::uint64_t>(process_info.dwProcessId));
#else
  constexpr bool injected_termination_failure = false;
#endif

  if (!endpoint_close.complete() || injected_termination_failure) {
    const WindowsChildOwnershipPhase cleanup_phase = child_ownership.phase();
    const WindowsChildCleanupOutcome cleanup = child_ownership.cleanup_now();
#if defined(NEBULA_HOST_PROCESS_TESTING)
    if (host_process_testing::take_post_cleanup_diagnostic_exception()) {
      throw std::runtime_error("injected exception while rendering Windows cleanup diagnostics");
    }
#endif
    if (bounded) {
      result.containment = cleanup.confirmed(cleanup_phase) ? HostProcessContainment::Confirmed
                                                            : HostProcessContainment::Unconfirmed;
    }
    if (injected_termination_failure) {
      append_detail(result.infrastructure_error, "injected post-spawn Windows termination failure");
    }
    append_windows_child_endpoint_close_outcome(endpoint_close, result.infrastructure_error);
    append_windows_cleanup_outcome(cleanup, cleanup_phase, result.infrastructure_error);
    child_ownership.acknowledge_cleanup_result();
    return result;
  }

  if (bounded) {
    if (!::AssignProcessToJobObject(containment_job.get(), process.get())) {
      const DWORD assignment_error = ::GetLastError();
      const WindowsChildOwnershipPhase cleanup_phase = child_ownership.phase();
      const WindowsChildCleanupOutcome cleanup = child_ownership.cleanup_now();
      result.containment = cleanup.confirmed(cleanup_phase) ? HostProcessContainment::Confirmed
                                                            : HostProcessContainment::Unconfirmed;
      result.infrastructure_error =
        "failed to assign the suspended Windows host process to its Job Object: " +
        windows_error_message(assignment_error);
      append_windows_cleanup_outcome(cleanup, cleanup_phase, result.infrastructure_error);
      child_ownership.acknowledge_cleanup_result();
      return result;
    }
    child_ownership.enter_job();
    const DWORD resume_count = ::ResumeThread(primary_thread.get());
    if (resume_count != 1U) {
      const DWORD resume_error =
        resume_count == static_cast<DWORD>(-1) ? ::GetLastError() : ERROR_SUCCESS;
      const WindowsChildOwnershipPhase cleanup_phase = child_ownership.phase();
      const WindowsChildCleanupOutcome cleanup = child_ownership.cleanup_now();
      result.containment = cleanup.confirmed(cleanup_phase) ? HostProcessContainment::Confirmed
                                                            : HostProcessContainment::Unconfirmed;
      result.infrastructure_error =
        resume_count == static_cast<DWORD>(-1)
          ? "failed to resume the contained Windows host process: " +
              windows_error_message(resume_error)
          : "contained Windows host process had an unexpected suspend count";
      append_windows_cleanup_outcome(cleanup, cleanup_phase, result.infrastructure_error);
      child_ownership.acknowledge_cleanup_result();
      return result;
    }
  }
  primary_thread.reset();

  bool process_exited = false;
  bool post_spawn_failure = false;
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeout_milliseconds);
  while (!process_exited) {
    std::string capture_error;
    if (!read_available_windows_pipe(stdout_stream.parent_read, result.stdout_data,
                                     request.max_stdout_bytes, result.stdout_limit_exceeded,
                                     capture_error) ||
        !read_available_windows_pipe(stderr_stream.parent_read, result.stderr_data,
                                     request.max_stderr_bytes, result.stderr_limit_exceeded,
                                     capture_error)) {
      append_detail(result.infrastructure_error, capture_error);
      post_spawn_failure = true;
      break;
    }
    if (result.stdout_limit_exceeded || result.stderr_limit_exceeded) {
      post_spawn_failure = true;
      break;
    }
    const bool has_capture = stdout_stream.parent_read.valid() || stderr_stream.parent_read.valid();
    if (bounded && std::chrono::steady_clock::now() >= deadline) {
      result.timed_out = true;
      post_spawn_failure = true;
      break;
    }
    DWORD wait_milliseconds = has_capture ? 10U : INFINITE;
    if (bounded) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      const auto clamped = std::clamp<std::int64_t>(remaining.count(), 1, 10);
      wait_milliseconds = static_cast<DWORD>(clamped);
    }
    const DWORD wait_result = ::WaitForSingleObject(process.get(), wait_milliseconds);
    if (wait_result == WAIT_OBJECT_0) {
      process_exited = true;
      if (!bounded)
        child_ownership.release();
    } else if (wait_result != WAIT_TIMEOUT) {
      append_detail(result.infrastructure_error, "failed to wait for the Windows host process: " +
                                                   windows_error_message(::GetLastError()));
      post_spawn_failure = true;
      break;
    }
  }

  if (bounded && process_exited && !post_spawn_failure) {
    const WindowsChildOwnershipPhase cleanup_phase = child_ownership.phase();
    const WindowsChildCleanupOutcome cleanup = child_ownership.cleanup_now();
    result.containment = cleanup.confirmed(cleanup_phase) ? HostProcessContainment::Confirmed
                                                          : HostProcessContainment::Unconfirmed;
    append_windows_cleanup_outcome(cleanup, cleanup_phase, result.infrastructure_error);
    child_ownership.acknowledge_cleanup_result();
  }

  if (post_spawn_failure) {
    stdout_stream.parent_read.reset();
    stderr_stream.parent_read.reset();
    const WindowsChildOwnershipPhase cleanup_phase = child_ownership.phase();
    const WindowsChildCleanupOutcome cleanup = child_ownership.cleanup_now();
    process_exited = cleanup.process_waited;
    if (bounded) {
      result.containment = cleanup.confirmed(cleanup_phase) ? HostProcessContainment::Confirmed
                                                            : HostProcessContainment::Unconfirmed;
    }
    append_windows_cleanup_outcome(cleanup, cleanup_phase, result.infrastructure_error);
    child_ownership.acknowledge_cleanup_result();
  }

  if (bounded && process_exited && result.containment == HostProcessContainment::Confirmed) {
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (stdout_stream.parent_read.valid() || stderr_stream.parent_read.valid()) {
      std::string capture_error;
      if (!read_available_windows_pipe(stdout_stream.parent_read, result.stdout_data,
                                       request.max_stdout_bytes, result.stdout_limit_exceeded,
                                       capture_error) ||
          !read_available_windows_pipe(stderr_stream.parent_read, result.stderr_data,
                                       request.max_stderr_bytes, result.stderr_limit_exceeded,
                                       capture_error)) {
        append_detail(result.infrastructure_error, capture_error);
        result.containment = HostProcessContainment::Unconfirmed;
        break;
      }
      if (result.stdout_limit_exceeded || result.stderr_limit_exceeded)
        break;
      if (!stdout_stream.parent_read.valid() && !stderr_stream.parent_read.valid())
        break;
      if (std::chrono::steady_clock::now() >= drain_deadline) {
        append_detail(result.infrastructure_error,
                      "Windows capture pipes did not close after Job Object quiescence");
        result.containment = HostProcessContainment::Unconfirmed;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (process_exited) {
    std::string capture_error;
    if (!read_available_windows_pipe(stdout_stream.parent_read, result.stdout_data,
                                     request.max_stdout_bytes, result.stdout_limit_exceeded,
                                     capture_error)) {
      append_detail(result.infrastructure_error, capture_error);
    }
    capture_error.clear();
    if (!read_available_windows_pipe(stderr_stream.parent_read, result.stderr_data,
                                     request.max_stderr_bytes, result.stderr_limit_exceeded,
                                     capture_error)) {
      append_detail(result.infrastructure_error, capture_error);
    }
    stdout_stream.parent_read.reset();
    stderr_stream.parent_read.reset();

    DWORD exit_code = 0U;
    if (!::GetExitCodeProcess(process.get(), &exit_code)) {
      append_detail(result.infrastructure_error,
                    "failed to read the Windows host process exit status: " +
                      windows_error_message(::GetLastError()));
    } else {
      // WAIT_OBJECT_0 already proves termination. 259 is therefore a valid
      // child-selected exit code here, not the STILL_ACTIVE sentinel.
      result.exited = true;
      result.exit_code = static_cast<std::uint32_t>(exit_code);
    }
  }
  finalize_capture_limits(request, result);
  return result;
}

#else

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) : descriptor_(descriptor) {}
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : descriptor_(other.release()) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] int get() const noexcept { return descriptor_; }
  int release() noexcept {
    const int value = descriptor_;
    descriptor_ = -1;
    return value;
  }
  void reset(int replacement = -1) noexcept {
    // POSIX permits close(2) to report EINTR after the descriptor has already
    // been released. Retrying could therefore close an unrelated descriptor
    // that another thread acquired in the meantime.
    if (valid())
      (void)::close(descriptor_);
    descriptor_ = replacement;
  }

  int close_with_error() noexcept {
    if (!valid())
      return 0;
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return ::close(descriptor) == 0 ? 0 : errno;
  }

private:
  int descriptor_ = -1;
};

struct PosixChildEndpointCloseOutcome {
  int stdin_error = 0;
  int stdout_error = 0;
  int stderr_error = 0;

  [[nodiscard]] bool complete() const noexcept {
    return stdin_error == 0 && stdout_error == 0 && stderr_error == 0;
  }
};

bool move_fd_above_standard_streams(UniqueFd &descriptor, std::string &error) {
  if (!descriptor.valid() || descriptor.get() >= 3)
    return true;
  const int duplicate = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, 3);
  if (duplicate < 0) {
    error = "failed to move a host-process descriptor above the standard streams: " +
            std::string(std::strerror(errno));
    return false;
  }
  descriptor.reset(duplicate);
  return true;
}

bool environment_entry_has_name(std::string_view entry, std::string_view name) {
  return entry.size() > name.size() && entry[name.size()] == '=' &&
         entry.substr(0U, name.size()) == name;
}

bool prepare_posix_environment(const HostProcessRequest &request,
                               std::vector<std::string> &environment, std::string &error) {
  if (request.inherit_environment) {
    if (environ == nullptr) {
      error = "host environment is unavailable";
      return false;
    }
    for (char **entry = environ; *entry != nullptr; ++entry) {
      const std::string_view inherited(*entry);
      const bool replaced =
        std::any_of(request.environment_overrides.begin(), request.environment_overrides.end(),
                    [&](const HostEnvironmentOverride &environment_override) {
                      return environment_entry_has_name(inherited, environment_override.name);
                    });
      if (!replaced)
        environment.emplace_back(inherited);
    }
  }
  for (const HostEnvironmentOverride &environment_override : request.environment_overrides) {
    environment.push_back(environment_override.name + "=" + environment_override.value);
  }
  return true;
}

bool set_fd_flag(int descriptor, int command, int flag, std::string &error) {
  const int current = ::fcntl(descriptor, command == F_SETFD ? F_GETFD : F_GETFL);
  if (current < 0 || ::fcntl(descriptor, command, current | flag) < 0) {
    error = "failed to configure a host-process descriptor: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
}

struct PosixOutputStream {
  UniqueFd parent_read;
  UniqueFd child_handle;
};

#if defined(__APPLE__) && NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP
bool add_posix_inherit_action(posix_spawn_file_actions_t &actions, int descriptor,
                              std::string_view label, std::string &error) {
  errno = 0;
  if (::fcntl(descriptor, F_GETFD) < 0) {
    const int descriptor_error = errno;
    if (descriptor_error == EBADF)
      return true;
    error = "failed to inspect inherited POSIX " + std::string(label) + ": " +
            std::strerror(descriptor_error);
    return false;
  }
  const int action_error = ::posix_spawn_file_actions_addinherit_np(&actions, descriptor);
  if (action_error != 0) {
    error = "failed to preserve inherited POSIX " + std::string(label) + ": " +
            std::strerror(action_error);
    return false;
  }
  return true;
}
#endif

bool prepare_posix_input_stream(HostProcessInputMode mode, UniqueFd &child_handle,
                                std::string &error) {
  if (mode == HostProcessInputMode::Inherit)
    return true;
  const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    error = "failed to open the POSIX null input device: " + std::string(std::strerror(errno));
    return false;
  }
  child_handle.reset(descriptor);
  return move_fd_above_standard_streams(child_handle, error);
}

bool prepare_posix_output_stream(HostProcessStreamMode mode, PosixOutputStream &stream,
                                 std::string &error) {
  if (mode == HostProcessStreamMode::Inherit)
    return true;
  if (mode == HostProcessStreamMode::Discard) {
    const int descriptor = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (descriptor < 0) {
      error = "failed to open the POSIX null device: " + std::string(std::strerror(errno));
      return false;
    }
    stream.child_handle.reset(descriptor);
    return move_fd_above_standard_streams(stream.child_handle, error);
  }

  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0) {
    error = "failed to create a POSIX capture pipe: " + std::string(std::strerror(errno));
    return false;
  }
  stream.parent_read.reset(descriptors[0]);
  stream.child_handle.reset(descriptors[1]);
  if (!move_fd_above_standard_streams(stream.parent_read, error) ||
      !move_fd_above_standard_streams(stream.child_handle, error) ||
      !set_fd_flag(stream.parent_read.get(), F_SETFD, FD_CLOEXEC, error) ||
      !set_fd_flag(stream.child_handle.get(), F_SETFD, FD_CLOEXEC, error) ||
      !set_fd_flag(stream.parent_read.get(), F_SETFL, O_NONBLOCK, error)) {
    return false;
  }
  return true;
}

bool add_posix_stream_actions(posix_spawn_file_actions_t &actions, HostProcessStreamMode mode,
                              const PosixOutputStream &stream, int target_descriptor,
                              std::string &error) {
  if (!stream.child_handle.valid()) {
#if defined(__APPLE__) && NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP
    if (mode == HostProcessStreamMode::Inherit)
      return add_posix_inherit_action(actions, target_descriptor,
                                      target_descriptor == STDOUT_FILENO ? "stdout" : "stderr",
                                      error);
#else
    (void)mode;
#endif
    return true;
  }
  int action_error =
    ::posix_spawn_file_actions_adddup2(&actions, stream.child_handle.get(), target_descriptor);
  if (action_error == 0 && stream.parent_read.valid()) {
    action_error = ::posix_spawn_file_actions_addclose(&actions, stream.parent_read.get());
  }
  if (action_error == 0 && stream.child_handle.get() != target_descriptor) {
    action_error = ::posix_spawn_file_actions_addclose(&actions, stream.child_handle.get());
  }
  if (action_error != 0) {
    error =
      "failed to configure POSIX child stream actions: " + std::string(std::strerror(action_error));
    return false;
  }
  return true;
}

bool add_posix_input_action(posix_spawn_file_actions_t &actions, HostProcessInputMode mode,
                            const UniqueFd &child_handle, std::string &error) {
  if (!child_handle.valid()) {
#if defined(__APPLE__) && NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP
    if (mode == HostProcessInputMode::Inherit)
      return add_posix_inherit_action(actions, STDIN_FILENO, "stdin", error);
#else
    (void)mode;
#endif
    return true;
  }
  int action_error = ::posix_spawn_file_actions_adddup2(&actions, child_handle.get(), STDIN_FILENO);
  if (action_error == 0 && child_handle.get() != STDIN_FILENO) {
    action_error = ::posix_spawn_file_actions_addclose(&actions, child_handle.get());
  }
  if (action_error != 0) {
    error = "failed to configure POSIX child stdin: " + std::string(std::strerror(action_error));
    return false;
  }
  return true;
}

bool add_posix_descriptor_boundary_action(posix_spawn_file_actions_t &actions, std::string &error) {
#if defined(__linux__) && NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
  const int action_error = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
  if (action_error != 0) {
    error = "failed to close non-standard POSIX child descriptors: " +
            std::string(std::strerror(action_error));
    return false;
  }
#else
  (void)actions;
  (void)error;
#endif
  return true;
}

bool read_available_posix_pipe(UniqueFd &pipe, std::string &output, std::size_t limit,
                               bool &limit_exceeded, std::string &error) {
  std::array<char, 4096U> buffer{};
  constexpr std::size_t kMaxPumpBytes = 64U * 1024U;
  std::size_t pumped_bytes = 0U;
  while (pipe.valid() && pumped_bytes < kMaxPumpBytes) {
    const std::size_t requested = std::min(buffer.size(), kMaxPumpBytes - pumped_bytes);
    const ssize_t bytes_read = ::read(pipe.get(), buffer.data(), requested);
    if (bytes_read > 0) {
      append_captured_bytes(output, buffer.data(), static_cast<std::size_t>(bytes_read), limit,
                            limit_exceeded);
      pumped_bytes += static_cast<std::size_t>(bytes_read);
      continue;
    }
    if (bytes_read == 0) {
      pipe.reset();
      return true;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;
    error = "failed to read a POSIX capture pipe: " + std::string(std::strerror(errno));
    pipe.reset();
    return false;
  }
  return true;
}

enum class PosixLeaderCleanupFailure : std::uint8_t {
  None,
  Probe,
  Kill,
  Wait,
  OwnershipLost,
  Timeout,
};

struct PosixLeaderCleanupOutcome {
  PosixLeaderCleanupFailure failure = PosixLeaderCleanupFailure::None;
  int system_error = 0;
  bool already_exited = false;
  bool termination_requested = false;
  bool reaped = false;
  bool reap_timed_out = false;
  int wait_status = 0;

  [[nodiscard]] bool confirmed() const noexcept {
    return reaped && (already_exited || termination_requested);
  }

  [[nodiscard]] CompilerContainmentOutcome::ResourceState resource_state() const noexcept {
    if (reaped)
      return CompilerContainmentOutcome::ResourceState::Gone;
    if (failure == PosixLeaderCleanupFailure::OwnershipLost ||
        failure == PosixLeaderCleanupFailure::Probe || failure == PosixLeaderCleanupFailure::Wait) {
      return CompilerContainmentOutcome::ResourceState::OwnershipLost;
    }
    return CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor;
  }
};

PosixLeaderCleanupOutcome cleanup_posix_leader_noexcept(pid_t pid) noexcept {
  PosixLeaderCleanupOutcome outcome;
  while (true) {
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0) {
      outcome.already_exited = info.si_pid == pid;
      break;
    }
    if (errno == EINTR)
      continue;
    outcome.failure =
      errno == ECHILD ? PosixLeaderCleanupFailure::OwnershipLost : PosixLeaderCleanupFailure::Probe;
    outcome.system_error = errno;
    return outcome;
  }
  if (!outcome.already_exited) {
#if defined(NEBULA_HOST_PROCESS_TESTING)
    const bool inject_termination_failure =
      host_process_testing::take_cleanup_termination_failure();
#else
    constexpr bool inject_termination_failure = false;
#endif
    if (inject_termination_failure) {
      errno = EPERM;
      outcome.termination_requested = false;
    } else {
      outcome.termination_requested = ::kill(pid, SIGKILL) == 0;
    }
    if (!outcome.termination_requested) {
      outcome.failure = PosixLeaderCleanupFailure::Kill;
      outcome.system_error = errno;
    }
  }
  int status = 0;
  constexpr unsigned kCleanupPollAttempts = 200U;
  for (unsigned attempt = 0U; attempt < kCleanupPollAttempts; ++attempt) {
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      outcome.reaped = true;
      outcome.wait_status = status;
      return outcome;
    }
    if (waited < 0) {
      if (errno == EINTR)
        continue;
      outcome.failure = errno == ECHILD ? PosixLeaderCleanupFailure::OwnershipLost
                                        : PosixLeaderCleanupFailure::Wait;
      outcome.system_error = errno;
      return outcome;
    }
    (void)::usleep(10'000U);
  }
  outcome.reap_timed_out = true;
  if (outcome.failure == PosixLeaderCleanupFailure::None)
    outcome.failure = PosixLeaderCleanupFailure::Timeout;
  return outcome;
}

[[noreturn]] void fatal_posix_leader_cleanup() noexcept {
  constexpr char message[] =
    "[cmd] fatal: POSIX child cleanup was unconfirmed during exception cleanup\n";
  (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
  ::_exit(125);
}

[[noreturn]] void fatal_posix_child_lifecycle_invariant() noexcept {
  constexpr char message[] =
    "[cmd] fatal: POSIX child ownership guard reached normal destruction while active\n";
  (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
  ::_exit(125);
}

class PosixChildOwnershipGuard final {
public:
  PosixChildOwnershipGuard(pid_t pid, bool bounded) noexcept
      : pid_(pid), bounded_(bounded), uncaught_exceptions_baseline_(std::uncaught_exceptions()) {}
  PosixChildOwnershipGuard(const PosixChildOwnershipGuard &) = delete;
  PosixChildOwnershipGuard &operator=(const PosixChildOwnershipGuard &) = delete;

  ~PosixChildOwnershipGuard() noexcept {
    if (state_ == State::Released)
      return;
    if (state_ == State::CleanupGone || state_ == State::OwnershipLost)
      fatal_posix_leader_cleanup();
    if (state_ == State::CleanupOwnedLeaderAnchor) {
      if (bounded_) {
        emergency_process_group_cleanup(pid_);
      } else if (!cleanup_posix_leader_noexcept(pid_).reaped) {
        fatal_posix_leader_cleanup();
      }
      if (std::uncaught_exceptions() > uncaught_exceptions_baseline_)
        fatal_posix_leader_cleanup();
      return;
    }
    state_ = State::Released;
    if (bounded_) {
      emergency_process_group_cleanup(pid_);
      if (std::uncaught_exceptions() == uncaught_exceptions_baseline_)
        fatal_posix_child_lifecycle_invariant();
      return;
    }
    if (!cleanup_posix_leader_noexcept(pid_).confirmed())
      fatal_posix_leader_cleanup();
    if (std::uncaught_exceptions() == uncaught_exceptions_baseline_)
      fatal_posix_child_lifecycle_invariant();
  }

  PosixLeaderCleanupOutcome cleanup_unbounded_now() noexcept {
    const PosixLeaderCleanupOutcome cleanup = cleanup_posix_leader_noexcept(pid_);
    record_explicit_cleanup(cleanup.confirmed(), cleanup.resource_state());
    return cleanup;
  }

  void record_explicit_cleanup(bool confirmed,
                               CompilerContainmentOutcome::ResourceState resource_state) noexcept {
    if (confirmed) {
      state_ = resource_state == CompilerContainmentOutcome::ResourceState::Gone
                 ? State::Released
                 : State::OwnershipLost;
      return;
    }
    switch (resource_state) {
    case CompilerContainmentOutcome::ResourceState::Gone:
      state_ = State::CleanupGone;
      return;
    case CompilerContainmentOutcome::ResourceState::OwnedLeaderAnchor:
      state_ = State::CleanupOwnedLeaderAnchor;
      return;
    case CompilerContainmentOutcome::ResourceState::OwnershipLost:
      state_ = State::OwnershipLost;
      return;
    }
    state_ = State::OwnershipLost;
  }

  void acknowledge_cleanup_result() noexcept {
    if (state_ == State::CleanupGone)
      state_ = State::Released;
  }

  void release() noexcept { state_ = State::Released; }

private:
  enum class State : std::uint8_t {
    Active,
    CleanupGone,
    CleanupOwnedLeaderAnchor,
    OwnershipLost,
    Released,
  };

  pid_t pid_ = -1;
  bool bounded_ = false;
  State state_ = State::Active;
  int uncaught_exceptions_baseline_ = 0;
};

void append_posix_leader_cleanup_outcome(const PosixLeaderCleanupOutcome &cleanup,
                                         std::string &detail) {
  switch (cleanup.failure) {
  case PosixLeaderCleanupFailure::None:
    break;
  case PosixLeaderCleanupFailure::Probe:
    append_detail(detail, "failed to inspect the POSIX host-process leader before cleanup: " +
                            std::string(std::strerror(cleanup.system_error)));
    break;
  case PosixLeaderCleanupFailure::Kill:
    append_detail(detail, "failed to terminate the POSIX host-process leader: " +
                            std::string(std::strerror(cleanup.system_error)));
    break;
  case PosixLeaderCleanupFailure::Wait:
    append_detail(detail, "failed to reap the POSIX host-process leader: " +
                            std::string(std::strerror(cleanup.system_error)));
    break;
  case PosixLeaderCleanupFailure::OwnershipLost:
    append_detail(detail, "POSIX host-process leader ownership was lost before reap confirmation");
    break;
  case PosixLeaderCleanupFailure::Timeout:
    append_detail(detail, "POSIX host-process leader cleanup was not confirmed within 2000ms");
    break;
  }
  if (cleanup.reap_timed_out && cleanup.failure != PosixLeaderCleanupFailure::Timeout) {
    append_detail(detail, "POSIX host-process leader cleanup was not confirmed within 2000ms");
  }
}

void append_posix_child_endpoint_close_outcome(const PosixChildEndpointCloseOutcome &close_outcome,
                                               std::string &detail) {
  const auto append_error = [&](int error, std::string_view label) {
    if (error != 0) {
      append_detail(detail, "failed to close the parent-side POSIX " + std::string(label) +
                              " child endpoint after launch: " + std::strerror(error));
    }
  };
  append_error(close_outcome.stdin_error, "stdin");
  append_error(close_outcome.stdout_error, "stdout");
  append_error(close_outcome.stderr_error, "stderr");
}

void append_posix_spawn_metadata_destroy_outcome(int action_error, int attribute_error,
                                                 std::string &detail) {
  if (action_error != 0) {
    append_detail(detail, "failed to destroy POSIX process file actions after launch: " +
                            std::string(std::strerror(action_error)));
  }
  if (attribute_error != 0) {
    append_detail(detail, "failed to destroy POSIX process attributes after launch: " +
                            std::string(std::strerror(attribute_error)));
  }
}

HostProcessResult run_posix_host_process(const HostProcessRequest &request) {
  HostProcessResult result;
  const bool bounded = request.timeout_milliseconds != 0U;
#if defined(__APPLE__)
#if !NEBULA_HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT || !NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDINHERIT_NP
  result.infrastructure_error =
    "POSIX host processes require CLOEXEC-default spawn attributes and explicit standard-stream "
    "inherit actions on Darwin";
  return result;
#endif
#elif defined(__linux__)
#if !NEBULA_HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
  result.infrastructure_error =
    "POSIX host processes require posix_spawn close-from file actions on Linux";
  return result;
#endif
#else
  result.infrastructure_error =
    "POSIX host processes require an audited Darwin or Linux descriptor boundary";
  return result;
#endif
  struct sigaction child_signal_action{};
  if (::sigaction(SIGCHLD, nullptr, &child_signal_action) != 0) {
    result.infrastructure_error =
      "failed to inspect SIGCHLD ownership: " + std::string(std::strerror(errno));
    return result;
  }
  if (child_signal_action.sa_handler != SIG_DFL ||
      (child_signal_action.sa_flags & SA_NOCLDWAIT) != 0) {
    result.infrastructure_error =
      "POSIX host processes require the default SIGCHLD disposition and retained child status";
    return result;
  }
  std::vector<std::string> environment;
  if (!prepare_posix_environment(request, environment, result.infrastructure_error))
    return result;

  std::vector<char *> argv;
  argv.reserve(request.arguments.size() + 1U);
  for (const std::string &argument : request.arguments) {
    argv.push_back(const_cast<char *>(argument.c_str()));
  }
  argv.push_back(nullptr);
  std::vector<char *> envp;
  envp.reserve(environment.size() + 1U);
  for (std::string &entry : environment)
    envp.push_back(entry.data());
  envp.push_back(nullptr);

  UniqueFd child_stdin;
  PosixOutputStream stdout_stream;
  PosixOutputStream stderr_stream;
  if (!prepare_posix_input_stream(request.stdin_mode, child_stdin, result.infrastructure_error) ||
      !prepare_posix_output_stream(request.stdout_mode, stdout_stream,
                                   result.infrastructure_error) ||
      !prepare_posix_output_stream(request.stderr_mode, stderr_stream,
                                   result.infrastructure_error)) {
    return result;
  }

  posix_spawn_file_actions_t actions{};
  const int init_error = ::posix_spawn_file_actions_init(&actions);
  if (init_error != 0) {
    result.infrastructure_error =
      "failed to initialize POSIX process file actions: " + std::string(std::strerror(init_error));
    return result;
  }
  if (!add_posix_input_action(actions, request.stdin_mode, child_stdin,
                              result.infrastructure_error) ||
      !add_posix_stream_actions(actions, request.stdout_mode, stdout_stream, STDOUT_FILENO,
                                result.infrastructure_error) ||
      !add_posix_stream_actions(actions, request.stderr_mode, stderr_stream, STDERR_FILENO,
                                result.infrastructure_error) ||
      !add_posix_descriptor_boundary_action(actions, result.infrastructure_error)) {
    const int destroy_error = ::posix_spawn_file_actions_destroy(&actions);
    if (destroy_error != 0) {
      append_detail(result.infrastructure_error, "failed to destroy POSIX process file actions: " +
                                                   std::string(std::strerror(destroy_error)));
    }
    return result;
  }

  posix_spawnattr_t attributes{};
  bool attributes_initialized = false;
#if defined(__APPLE__)
  constexpr bool platform_requires_attributes = true;
#else
  constexpr bool platform_requires_attributes = false;
#endif
  if (bounded || platform_requires_attributes) {
    const int attribute_init_error = ::posix_spawnattr_init(&attributes);
    if (attribute_init_error != 0) {
      result.infrastructure_error = "failed to initialize POSIX process attributes: " +
                                    std::string(std::strerror(attribute_init_error));
      const int action_destroy_error = ::posix_spawn_file_actions_destroy(&actions);
      if (action_destroy_error != 0) {
        append_detail(result.infrastructure_error,
                      "failed to destroy POSIX process file actions: " +
                        std::string(std::strerror(action_destroy_error)));
      }
      return result;
    }
    attributes_initialized = true;
    sigset_t empty_signal_mask{};
    sigset_t default_signals{};
    int attribute_error = 0;
    short spawn_flags = 0;
#if defined(__APPLE__) && NEBULA_HAVE_POSIX_SPAWN_CLOEXEC_DEFAULT
    spawn_flags = static_cast<short>(spawn_flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    if (bounded) {
      if (sigemptyset(&empty_signal_mask) != 0 || sigemptyset(&default_signals) != 0)
        attribute_error = errno;
      for (const int signal_number : {SIGHUP, SIGINT, SIGQUIT, SIGPIPE, SIGTERM, SIGCHLD}) {
        if (attribute_error == 0 && sigaddset(&default_signals, signal_number) != 0)
          attribute_error = errno;
      }
      spawn_flags = static_cast<short>(spawn_flags | POSIX_SPAWN_SETPGROUP |
                                       POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    }
    if (attribute_error == 0)
      attribute_error = ::posix_spawnattr_setflags(&attributes, spawn_flags);
    if (bounded && attribute_error == 0) {
      // A pgroup value of zero assigns the child's PID as the new process
      // group ID atomically as part of posix_spawn.
      attribute_error = ::posix_spawnattr_setpgroup(&attributes, 0);
    }
    if (bounded && attribute_error == 0)
      attribute_error = ::posix_spawnattr_setsigmask(&attributes, &empty_signal_mask);
    if (bounded && attribute_error == 0)
      attribute_error = ::posix_spawnattr_setsigdefault(&attributes, &default_signals);
    if (attribute_error != 0) {
      result.infrastructure_error =
        "failed to configure the POSIX host-process containment group: " +
        std::string(std::strerror(attribute_error));
      const int attribute_destroy_error = ::posix_spawnattr_destroy(&attributes);
      const int action_destroy_error = ::posix_spawn_file_actions_destroy(&actions);
      if (attribute_destroy_error != 0) {
        append_detail(result.infrastructure_error,
                      "failed to destroy POSIX process attributes: " +
                        std::string(std::strerror(attribute_destroy_error)));
      }
      if (action_destroy_error != 0) {
        append_detail(result.infrastructure_error,
                      "failed to destroy POSIX process file actions: " +
                        std::string(std::strerror(action_destroy_error)));
      }
      return result;
    }
  }

  pid_t pid = -1;
  const int spawn_error =
    ::posix_spawn(&pid, request.executable_path.c_str(), &actions,
                  attributes_initialized ? &attributes : nullptr, argv.data(), envp.data());
  if (spawn_error != 0) {
    const int destroy_error = ::posix_spawn_file_actions_destroy(&actions);
    const int attribute_destroy_error =
      attributes_initialized ? ::posix_spawnattr_destroy(&attributes) : 0;
    result.infrastructure_error =
      "failed to start the POSIX host process: " + std::string(std::strerror(spawn_error));
    if (destroy_error != 0) {
      append_detail(result.infrastructure_error, "failed to destroy POSIX process file actions: " +
                                                   std::string(std::strerror(destroy_error)));
    }
    if (attribute_destroy_error != 0) {
      append_detail(result.infrastructure_error,
                    "failed to destroy POSIX process attributes: " +
                      std::string(std::strerror(attribute_destroy_error)));
    }
    return result;
  }
  result.started = true;
  if (bounded)
    result.containment = HostProcessContainment::Unconfirmed;
  PosixChildOwnershipGuard child_ownership(pid, bounded);

  // Native child ownership is protected before destroying spawn metadata or
  // closing parent-side child endpoints. Capture only primitive status first;
  // diagnostic allocation happens after the testable exception boundary.
  const int destroy_error = ::posix_spawn_file_actions_destroy(&actions);
  const int attribute_destroy_error =
    attributes_initialized ? ::posix_spawnattr_destroy(&attributes) : 0;
  PosixChildEndpointCloseOutcome endpoint_close = {
    child_stdin.close_with_error(),
    stdout_stream.child_handle.close_with_error(),
    stderr_stream.child_handle.close_with_error(),
  };
#if defined(NEBULA_HOST_PROCESS_TESTING)
  if (endpoint_close.stdin_error == 0 &&
      host_process_testing::take_parent_stdin_endpoint_close_error(
        static_cast<std::uint64_t>(pid))) {
    endpoint_close.stdin_error = EBADF;
  }
#endif

#if defined(NEBULA_HOST_PROCESS_TESTING)
  if (host_process_testing::take_post_spawn_exception(static_cast<std::uint64_t>(pid))) {
    throw std::runtime_error("injected exception after POSIX child ownership guard installation");
  }
  const bool injected_termination_failure =
    !bounded && host_process_testing::take_unbounded_post_spawn_termination_failure(
                  static_cast<std::uint64_t>(pid));
#else
  constexpr bool injected_termination_failure = false;
#endif

  if (!endpoint_close.complete() || destroy_error != 0 || attribute_destroy_error != 0 ||
      injected_termination_failure) {
    if (bounded) {
      SystemCompilerProcessControl process_control;
      CompilerContainmentOutcome cleanup = clean_compiler_process_group(process_control, pid);
      child_ownership.record_explicit_cleanup(cleanup.confirmed(), cleanup.resource_state);
#if defined(NEBULA_HOST_PROCESS_TESTING)
      if (host_process_testing::take_post_cleanup_diagnostic_exception()) {
        throw std::runtime_error("injected exception while rendering POSIX cleanup diagnostics");
      }
#endif
      result.containment = cleanup.confirmed() ? HostProcessContainment::Confirmed
                                               : HostProcessContainment::Unconfirmed;
      if (injected_termination_failure) {
        append_detail(result.infrastructure_error, "injected post-spawn POSIX termination failure");
      }
      append_posix_child_endpoint_close_outcome(endpoint_close, result.infrastructure_error);
      append_posix_spawn_metadata_destroy_outcome(destroy_error, attribute_destroy_error,
                                                  result.infrastructure_error);
      if (!cleanup.detail.empty())
        append_detail(result.infrastructure_error, cleanup.detail);
      if (!cleanup.confirmed() && cleanup.detail.empty()) {
        append_detail(result.infrastructure_error,
                      "POSIX host-process group cleanup could not be confirmed");
      }
      child_ownership.acknowledge_cleanup_result();
      return result;
    }

    const PosixLeaderCleanupOutcome cleanup = child_ownership.cleanup_unbounded_now();
#if defined(NEBULA_HOST_PROCESS_TESTING)
    if (host_process_testing::take_post_cleanup_diagnostic_exception()) {
      throw std::runtime_error("injected exception while rendering POSIX cleanup diagnostics");
    }
#endif
    if (injected_termination_failure) {
      append_detail(result.infrastructure_error, "injected post-spawn POSIX termination failure");
    }
    append_posix_child_endpoint_close_outcome(endpoint_close, result.infrastructure_error);
    append_posix_spawn_metadata_destroy_outcome(destroy_error, attribute_destroy_error,
                                                result.infrastructure_error);
    append_posix_leader_cleanup_outcome(cleanup, result.infrastructure_error);
    child_ownership.acknowledge_cleanup_result();
    return result;
  }

  int status = 0;
  bool child_reaped = false;
  bool post_spawn_failure = !result.infrastructure_error.empty();
  bool unbounded_ownership_lost = false;
  const bool has_capture = stdout_stream.parent_read.valid() || stderr_stream.parent_read.valid();
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(request.timeout_milliseconds);

  if (bounded) {
    SystemCompilerProcessControl process_control;
    bool ownership_lost = false;
    CompilerProcessContainment bounded_containment = CompilerProcessContainment::Unconfirmed;
    while (!post_spawn_failure) {
      if (request.termination_signals != nullptr) {
        const int interrupted = request.termination_signals->intercepted_signal();
        if (interrupted != 0) {
          result.parent_interruption_signal = interrupted;
          post_spawn_failure = true;
          break;
        }
      }
      std::string capture_error;
      if (!read_available_posix_pipe(stdout_stream.parent_read, result.stdout_data,
                                     request.max_stdout_bytes, result.stdout_limit_exceeded,
                                     capture_error) ||
          !read_available_posix_pipe(stderr_stream.parent_read, result.stderr_data,
                                     request.max_stderr_bytes, result.stderr_limit_exceeded,
                                     capture_error)) {
        append_detail(result.infrastructure_error, capture_error);
        post_spawn_failure = true;
        break;
      }
      if (result.stdout_limit_exceeded || result.stderr_limit_exceeded) {
        post_spawn_failure = true;
        break;
      }

      const CompilerLeaderObservation observation = process_control.observe_leader(pid);
      if (observation.state == CompilerLeaderState::Exited) {
        CompilerContainmentOutcome cleanup = clean_compiler_process_group(process_control, pid);
        bounded_containment = cleanup.confirmed() ? CompilerProcessContainment::Confirmed
                                                  : CompilerProcessContainment::Unconfirmed;
        child_reaped = cleanup.resources_gone() && cleanup.leader_status_available;
        if (child_reaped)
          status = cleanup.leader_status;
        child_ownership.record_explicit_cleanup(cleanup.confirmed(), cleanup.resource_state);
        if (!cleanup.detail.empty())
          append_detail(result.infrastructure_error, cleanup.detail);
        if (!cleanup.confirmed() && cleanup.detail.empty()) {
          append_detail(result.infrastructure_error,
                        "POSIX host-process group cleanup could not be confirmed");
        }
        child_ownership.acknowledge_cleanup_result();
        break;
      }
      if (observation.state == CompilerLeaderState::OwnershipLost) {
        ownership_lost = true;
        child_ownership.record_explicit_cleanup(
          false, CompilerContainmentOutcome::ResourceState::OwnershipLost);
        append_detail(result.infrastructure_error, observation.detail);
        child_ownership.acknowledge_cleanup_result();
        post_spawn_failure = true;
        break;
      }
      if (observation.state == CompilerLeaderState::Error) {
        ownership_lost = true;
        child_ownership.record_explicit_cleanup(
          false, CompilerContainmentOutcome::ResourceState::OwnershipLost);
        append_detail(result.infrastructure_error, observation.detail);
        child_ownership.acknowledge_cleanup_result();
        post_spawn_failure = true;
        break;
      }

      if (std::chrono::steady_clock::now() >= deadline) {
        result.timed_out = true;
        post_spawn_failure = true;
        break;
      }

      std::array<pollfd, 2U> poll_descriptors{};
      nfds_t poll_count = 0U;
      if (stdout_stream.parent_read.valid()) {
        poll_descriptors[poll_count++] =
          pollfd{stdout_stream.parent_read.get(), POLLIN | POLLHUP, 0};
      }
      if (stderr_stream.parent_read.valid()) {
        poll_descriptors[poll_count++] =
          pollfd{stderr_stream.parent_read.get(), POLLIN | POLLHUP, 0};
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
      const int wait_milliseconds =
        static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 20));
      const int poll_result = poll_count == 0U
                                ? ::poll(nullptr, 0, wait_milliseconds)
                                : ::poll(poll_descriptors.data(), poll_count, wait_milliseconds);
      if (poll_result < 0 && errno != EINTR) {
        append_detail(result.infrastructure_error,
                      "failed to poll POSIX capture pipes: " + std::string(std::strerror(errno)));
        post_spawn_failure = true;
        break;
      }
    }

    if (post_spawn_failure && !ownership_lost) {
      stdout_stream.parent_read.reset();
      stderr_stream.parent_read.reset();
      // A timeout is a normal cancellation boundary for long-running tools:
      // give the leader a short, bounded opportunity to run its SIGTERM
      // cleanup before sealing the entire containment group. Capture
      // overflow and other infrastructure failures remain fail-fast and use
      // immediate group cleanup.
      CompilerContainmentOutcome cleanup =
        result.timed_out
          ? terminate_compiler_process_group(process_control, pid, std::chrono::milliseconds(200))
          : clean_compiler_process_group(process_control, pid);
      bounded_containment = cleanup.confirmed() ? CompilerProcessContainment::Confirmed
                                                : CompilerProcessContainment::Unconfirmed;
      child_ownership.record_explicit_cleanup(cleanup.confirmed(), cleanup.resource_state);
      if (!cleanup.detail.empty())
        append_detail(result.infrastructure_error, cleanup.detail);
      if (!cleanup.confirmed() && cleanup.detail.empty()) {
        append_detail(result.infrastructure_error,
                      "POSIX host-process group cleanup could not be confirmed");
      }
      child_ownership.acknowledge_cleanup_result();
    }
    result.containment = bounded_containment == CompilerProcessContainment::Confirmed
                           ? HostProcessContainment::Confirmed
                           : HostProcessContainment::Unconfirmed;
  } else if (!has_capture) {
    while (true) {
      const pid_t waited = ::waitpid(pid, &status, 0);
      if (waited == pid) {
        child_reaped = true;
        child_ownership.release();
        break;
      }
      if (waited < 0 && errno == EINTR)
        continue;
      const int wait_error = waited < 0 ? errno : ECHILD;
      unbounded_ownership_lost = true;
      child_ownership.record_explicit_cleanup(
        false, CompilerContainmentOutcome::ResourceState::OwnershipLost);
      append_detail(result.infrastructure_error, "failed to wait for the POSIX host process: " +
                                                   std::string(std::strerror(wait_error)));
      post_spawn_failure = true;
      break;
    }
  } else {
    while (!child_reaped && !post_spawn_failure) {
      std::string capture_error;
      if (!read_available_posix_pipe(stdout_stream.parent_read, result.stdout_data,
                                     request.max_stdout_bytes, result.stdout_limit_exceeded,
                                     capture_error) ||
          !read_available_posix_pipe(stderr_stream.parent_read, result.stderr_data,
                                     request.max_stderr_bytes, result.stderr_limit_exceeded,
                                     capture_error)) {
        append_detail(result.infrastructure_error, capture_error);
        post_spawn_failure = true;
        break;
      }
      if (result.stdout_limit_exceeded || result.stderr_limit_exceeded) {
        post_spawn_failure = true;
        break;
      }
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        child_reaped = true;
        child_ownership.release();
        break;
      }
      if (waited < 0) {
        if (errno == EINTR)
          continue;
        const int wait_error = errno;
        unbounded_ownership_lost = true;
        child_ownership.record_explicit_cleanup(
          false, CompilerContainmentOutcome::ResourceState::OwnershipLost);
        append_detail(result.infrastructure_error, "failed to inspect the POSIX host process: " +
                                                     std::string(std::strerror(wait_error)));
        post_spawn_failure = true;
        break;
      }
      std::array<pollfd, 2U> poll_descriptors{};
      nfds_t poll_count = 0U;
      if (stdout_stream.parent_read.valid())
        poll_descriptors[poll_count++] =
          pollfd{stdout_stream.parent_read.get(), POLLIN | POLLHUP, 0};
      if (stderr_stream.parent_read.valid())
        poll_descriptors[poll_count++] =
          pollfd{stderr_stream.parent_read.get(), POLLIN | POLLHUP, 0};
      if (::poll(poll_descriptors.data(), poll_count, 20) < 0 && errno != EINTR) {
        append_detail(result.infrastructure_error,
                      "failed to poll POSIX capture pipes: " + std::string(std::strerror(errno)));
        post_spawn_failure = true;
      }
    }
  }

  if (!bounded && post_spawn_failure && !child_reaped && !unbounded_ownership_lost) {
    stdout_stream.parent_read.reset();
    stderr_stream.parent_read.reset();
    const PosixLeaderCleanupOutcome cleanup = child_ownership.cleanup_unbounded_now();
    child_reaped = cleanup.reaped;
    if (child_reaped)
      status = cleanup.wait_status;
    append_posix_leader_cleanup_outcome(cleanup, result.infrastructure_error);
    child_ownership.acknowledge_cleanup_result();
  }

  if (bounded && child_reaped && result.containment == HostProcessContainment::Confirmed) {
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (stdout_stream.parent_read.valid() || stderr_stream.parent_read.valid()) {
      std::string capture_error;
      if (!read_available_posix_pipe(stdout_stream.parent_read, result.stdout_data,
                                     request.max_stdout_bytes, result.stdout_limit_exceeded,
                                     capture_error) ||
          !read_available_posix_pipe(stderr_stream.parent_read, result.stderr_data,
                                     request.max_stderr_bytes, result.stderr_limit_exceeded,
                                     capture_error)) {
        append_detail(result.infrastructure_error, capture_error);
        result.containment = HostProcessContainment::Unconfirmed;
        break;
      }
      if (result.stdout_limit_exceeded || result.stderr_limit_exceeded)
        break;
      if (!stdout_stream.parent_read.valid() && !stderr_stream.parent_read.valid())
        break;
      if (std::chrono::steady_clock::now() >= drain_deadline) {
        append_detail(result.infrastructure_error,
                      "POSIX capture pipes did not close after process-group quiescence");
        result.containment = HostProcessContainment::Unconfirmed;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  if (child_reaped) {
    std::string capture_error;
    if (!read_available_posix_pipe(stdout_stream.parent_read, result.stdout_data,
                                   request.max_stdout_bytes, result.stdout_limit_exceeded,
                                   capture_error)) {
      append_detail(result.infrastructure_error, capture_error);
    }
    capture_error.clear();
    if (!read_available_posix_pipe(stderr_stream.parent_read, result.stderr_data,
                                   request.max_stderr_bytes, result.stderr_limit_exceeded,
                                   capture_error)) {
      append_detail(result.infrastructure_error, capture_error);
    }
    stdout_stream.parent_read.reset();
    stderr_stream.parent_read.reset();

    if (WIFEXITED(status)) {
      result.exited = true;
      result.exit_code = static_cast<std::uint32_t>(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      result.exited = true;
      result.termination_signal = WTERMSIG(status);
      result.exit_code = static_cast<std::uint32_t>(128 + result.termination_signal);
    } else {
      append_detail(result.infrastructure_error,
                    "POSIX host process ended with an unsupported wait status");
    }
  }
  finalize_capture_limits(request, result);
  return result;
}

#endif

} // namespace

namespace detail {

std::wstring quote_windows_argument(std::wstring_view argument) {
  const bool requires_quotes =
    argument.empty() || argument.find_first_of(L" \t\r\n\v\f\"") != std::wstring_view::npos;
  if (!requires_quotes)
    return std::wstring(argument);

  std::wstring quoted;
  quoted.push_back(L'"');
  std::size_t backslashes = 0U;
  for (const wchar_t value : argument) {
    if (value == L'\\') {
      ++backslashes;
      continue;
    }
    if (value == L'"') {
      quoted.append(backslashes * 2U + 1U, L'\\');
      quoted.push_back(L'"');
      backslashes = 0U;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0U;
    quoted.push_back(value);
  }
  quoted.append(backslashes * 2U, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

WindowsEnvironmentBlockResult build_windows_environment_block(
  const std::vector<std::wstring> &inherited_entries,
  const std::vector<std::pair<std::wstring, std::wstring>> &overrides) {
  WindowsEnvironmentBlockResult result;
  for (const auto &[name, value] : overrides) {
    if (name.empty() || contains_nul(name) || name.find(L'=') != std::wstring::npos ||
        contains_nul(value)) {
      result.error = "Windows environment override is malformed";
      return result;
    }
  }
  for (std::size_t index = 0U; index < overrides.size(); ++index) {
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (windows_name_equal(overrides[index].first, overrides[previous].first)) {
        result.error = "Windows environment contains duplicate overrides";
        return result;
      }
    }
  }

  std::vector<std::wstring> entries;
  entries.reserve(inherited_entries.size() + overrides.size());
  for (const std::wstring &inherited : inherited_entries) {
    if (inherited.empty() || contains_nul(inherited) ||
        windows_environment_entry_name(inherited).empty()) {
      result.error = "inherited Windows environment contains a malformed entry";
      return result;
    }
    const std::wstring_view inherited_name = windows_environment_entry_name(inherited);
    const bool replaced = std::any_of(overrides.begin(), overrides.end(), [&](const auto &item) {
      return windows_name_equal(inherited_name, item.first);
    });
    if (!replaced)
      entries.push_back(inherited);
  }
  for (const auto &[name, value] : overrides)
    entries.push_back(name + L"=" + value);

  // Keep the environment strings at stable addresses and sort only a view of
  // pointers. Besides avoiding moves of potentially large inherited values,
  // this keeps comparison independent of std::sort's internal moved-from
  // pivot objects.
  std::vector<const std::wstring *> ordered_entries;
  ordered_entries.reserve(entries.size());
  for (const std::wstring &entry : entries)
    ordered_entries.push_back(&entry);
  std::sort(
    ordered_entries.begin(), ordered_entries.end(),
    [](const std::wstring *lhs, const std::wstring *rhs) { return windows_text_less(*lhs, *rhs); });

  std::size_t required = 1U;
  for (const std::wstring *entry : ordered_entries) {
    if (entry->size() > std::numeric_limits<std::size_t>::max() - required - 1U) {
      result.error = "Windows environment block size overflow";
      return result;
    }
    required += entry->size() + 1U;
  }
  if (ordered_entries.empty())
    ++required;
  result.block.reserve(required);
  for (const std::wstring *entry : ordered_entries) {
    result.block.insert(result.block.end(), entry->begin(), entry->end());
    result.block.push_back(L'\0');
  }
  result.block.push_back(L'\0');
  if (ordered_entries.empty())
    result.block.push_back(L'\0');
  return result;
}

} // namespace detail

HostProcessResult run_host_process(const HostProcessRequest &request) {
  HostProcessResult result;
  result.infrastructure_error = validate_request(request);
  if (!result.infrastructure_error.empty())
    return result;
#if defined(_WIN32)
  return run_windows_host_process(request);
#else
  return run_posix_host_process(request);
#endif
}

int host_process_compatible_exit_code(const HostProcessResult &result, std::string &error) {
  error.clear();
  if (result.parent_interruption_signal != 0) {
    if (result.parent_interruption_signal > INT_MAX - 128) {
      error = "host process observed an unsupported caller interruption signal";
      return 125;
    }
    if (result.containment != HostProcessContainment::Confirmed ||
        !result.infrastructure_error.empty()) {
      error = result.infrastructure_error.empty()
                ? "caller interruption occurred without confirmed process-group cleanup"
                : result.infrastructure_error;
      return 125;
    }
    return 128 + result.parent_interruption_signal;
  }
  if (result.timed_out) {
    if (result.containment == HostProcessContainment::Confirmed &&
        result.infrastructure_error.empty()) {
      return 124;
    }
    error = result.infrastructure_error.empty()
              ? "host process timed out and containment-domain cleanup was not confirmed"
              : result.infrastructure_error;
    return 125;
  }
  if (!result.completed()) {
    error = result.infrastructure_error.empty() ? "host process did not complete"
                                                : result.infrastructure_error;
    return 125;
  }
  if (result.termination_signal != 0) {
    if (result.termination_signal > INT_MAX - 128) {
      error = "host process returned an unsupported termination signal";
      return 125;
    }
    return 128 + result.termination_signal;
  }
  if (result.exit_code > static_cast<std::uint32_t>(INT_MAX)) {
    error = "host process returned an unsupported exit status";
    return 125;
  }
  return static_cast<int>(result.exit_code);
}

int run_host_process_with_environment_override(
  const std::vector<std::string> &arguments, const HostEnvironmentOverride &environment_override) {
  HostProcessRequest request;
  if (!arguments.empty())
    request.executable_path = arguments.front();
  request.arguments = arguments;
  request.environment_overrides.push_back(environment_override);
  const HostProcessResult result = run_host_process(request);
  std::string exit_error;
  const int exit_code = host_process_compatible_exit_code(result, exit_error);
  if (!exit_error.empty()) {
    std::cerr << "[cmd] infrastructure failure: " << exit_error << "\n";
    return exit_code;
  }
  if (exit_code != 0)
    std::cerr << "[cmd] exit=" << exit_code << "\n";
  return exit_code;
}

} // namespace nebula::cli
