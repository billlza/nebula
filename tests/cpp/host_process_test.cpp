#include "cli/host_process.hpp"
#include "cli/host_process_test_hooks.hpp"
#include "cli/termination_signal.hpp"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

bool injected_child_is_reaped(std::uint64_t process_id);

int child_main(int argc, char **argv) {
  const std::string_view mode = argv[1];
  if (mode == "logical-argv0") {
    return std::string_view(argv[0]) == "logical-nebula-name" ? 0 : 103;
  }
  if (mode == "delayed-marker") {
    if (argc != 3)
      return 95;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::ofstream marker(argv[2], std::ios::binary | std::ios::trunc);
    marker << "escaped\n";
    marker.close();
    return marker.fail() ? 96 : 0;
  }
  if (mode == "long-delayed-marker") {
    if (argc != 3)
      return 111;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    std::ofstream marker(argv[2], std::ios::binary | std::ios::trunc);
    marker << "escaped\n";
    marker.close();
    return marker.fail() ? 112 : 0;
  }
  if (mode == "very-long-delayed-marker") {
    if (argc != 3)
      return 114;
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    std::ofstream marker(argv[2], std::ios::binary | std::ios::trunc);
    marker << "escaped\n";
    marker.close();
    return marker.fail() ? 115 : 0;
  }
  if (mode == "spawn-marker-child") {
    if (argc != 3)
      return 97;
    nebula::cli::HostProcessRequest request;
    request.executable_path = std::filesystem::absolute(argv[0]).lexically_normal();
    request.arguments = {std::filesystem::absolute(argv[0]).lexically_normal().string(),
                         "delayed-marker", argv[2]};
    request.stdout_mode = nebula::cli::HostProcessStreamMode::Discard;
    request.stderr_mode = nebula::cli::HostProcessStreamMode::Discard;
    const nebula::cli::HostProcessResult child = nebula::cli::run_host_process(request);
    return child.succeeded() ? 0 : 98;
  }
  if (mode == "cleanup-diagnostic-throw-driver") {
    if (argc != 3)
      return 109;
    using namespace nebula::cli::host_process_testing;
    inject_unbounded_post_spawn_termination_failure_once();
    inject_post_cleanup_diagnostic_exception_once();
    nebula::cli::HostProcessRequest request;
    request.executable_path = std::filesystem::absolute(argv[0]).lexically_normal();
    request.arguments = {request.executable_path.string(), "long-delayed-marker", argv[2]};
    request.stdout_mode = nebula::cli::HostProcessStreamMode::Discard;
    request.stderr_mode = nebula::cli::HostProcessStreamMode::Discard;
    try {
      (void)nebula::cli::run_host_process(request);
    } catch (...) {
      return 113;
    }
    return 110;
  }
  if (mode == "live-cleanup-retry-driver") {
    if (argc != 3)
      return 116;
    using namespace nebula::cli::host_process_testing;
    inject_unbounded_post_spawn_termination_failure_once();
    nebula::cli::HostProcessRequest request;
    request.executable_path = std::filesystem::absolute(argv[0]).lexically_normal();
    request.arguments = {request.executable_path.string(), "very-long-delayed-marker", argv[2]};
    request.stdout_mode = nebula::cli::HostProcessStreamMode::Discard;
    request.stderr_mode = nebula::cli::HostProcessStreamMode::Discard;
    const nebula::cli::HostProcessResult result = nebula::cli::run_host_process(request);
    const std::uint64_t process_id = last_injected_process_id();
    const bool classified_unconfirmed =
      result.started && !result.completed() &&
      result.infrastructure_error.find("failed to terminate") != std::string::npos;
    return classified_unconfirmed && !unbounded_post_spawn_termination_failure_pending() &&
               injected_child_is_reaped(process_id) && !std::filesystem::exists(argv[2])
             ? 0
             : 117;
  }
  if (mode == "infinite-output") {
    const std::string chunk(4096U, 'x');
    while (true) {
      std::cout << chunk;
      std::cerr << chunk;
      std::cout.flush();
      std::cerr.flush();
    }
  }
  if (mode == "paced-output") {
    const std::string chunk(4096U, 'p');
    while (true) {
      std::cout << chunk;
      std::cerr << chunk;
      std::cout.flush();
      std::cerr.flush();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (mode == "containment-report") {
#if defined(_WIN32)
    BOOL in_job = FALSE;
    if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job))
      return 99;
    std::cout << (in_job != FALSE ? "contained\n" : "uncontained\n");
#else
    std::cout << (::getpid() == ::getpgrp() ? "contained\n" : "uncontained\n");
#endif
    return 0;
  }
#if !defined(_WIN32)
  if (mode == "leader-exit-with-marker-child") {
    if (argc != 3)
      return 100;
    const pid_t child = ::fork();
    if (child < 0)
      return 101;
    if (child == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      std::ofstream marker(argv[2], std::ios::binary | std::ios::trunc);
      marker << "escaped\n";
      marker.close();
      ::_exit(marker.fail() ? 102 : 0);
    }
    return 17;
  }
#endif
  if (mode == "capture") {
    if (argc != 6 || std::string_view(argv[2]) != "two words" ||
        std::string_view(argv[3]) != "quote\"value" || std::string_view(argv[4]) != "trailing\\" ||
        std::string_view(argv[5]) != "") {
      return 91;
    }
    const char *value = std::getenv("NEBULA_HOST_PROCESS_TEST_VALUE");
    const char *path = std::getenv("PATH");
    if (value == nullptr || std::string_view(value) != "override value" || path == nullptr) {
      return 92;
    }
    std::cout << "stdout-record\n";
    std::cerr << "stderr-record\n";
    return 23;
  }
  if (mode == "isolated-environment") {
    const char *value = std::getenv("NEBULA_HOST_PROCESS_TEST_VALUE");
    const char *path = std::getenv("PATH");
    const char *inherited_marker = std::getenv("NEBULA_HOST_PROCESS_TEST_INHERITED");
    return value != nullptr && std::string_view(value) == "isolated" && path != nullptr &&
               inherited_marker == nullptr
             ? 0
             : 93;
  }
  if (mode == "overflow") {
    std::cout << std::string(8192U, 'o');
    std::cerr << std::string(8192U, 'e');
    return 0;
  }
  if (mode == "discard") {
    std::cout << "discarded stdout\n";
    std::cerr << "discarded stderr\n";
    return 0;
  }
  if (mode == "stdin-eof")
    return std::cin.get() == std::char_traits<char>::eof() ? 0 : 104;
  if (mode == "exit-259")
    return 259;
  if (mode == "standard-fd-reuse") {
    std::cout << "standard-fd-reuse-ok\n";
    return 0;
  }
#if !defined(_WIN32)
  if (mode == "fd-boundary") {
    if (argc != 3)
      return 105;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' || parsed < 3 || parsed > INT_MAX)
      return 106;
    errno = 0;
    if (::fcntl(static_cast<int>(parsed), F_GETFD) != -1 || errno != EBADF)
      return 107;
    std::cout << "fd-boundary-ok\n";
    return 0;
  }
  if (mode == "closed-inherited-stderr") {
    errno = 0;
    if (::fcntl(STDERR_FILENO, F_GETFD) != -1 || errno != EBADF)
      return 108;
    std::cout << "closed-inherited-stderr-ok\n";
    return 0;
  }
#endif
  return 94;
}

std::vector<std::wstring> split_environment_block(const std::vector<wchar_t> &block) {
  std::vector<std::wstring> entries;
  std::size_t offset = 0U;
  while (offset < block.size() && block[offset] != L'\0') {
    const std::size_t start = offset;
    while (offset < block.size() && block[offset] != L'\0')
      ++offset;
    if (offset == block.size())
      return {};
    entries.emplace_back(block.data() + start, offset - start);
    ++offset;
  }
  return entries;
}

void expect(bool condition, std::string_view message, int &failures) {
  if (condition)
    return;
  std::cerr << "host_process_test: " << message << "\n";
  ++failures;
}

bool injected_child_is_reaped(std::uint64_t process_id) {
  if (process_id == 0U)
    return false;
#if defined(_WIN32)
  HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(process_id));
  if (process == nullptr)
    return ::GetLastError() == ERROR_INVALID_PARAMETER;
  const bool exited = ::WaitForSingleObject(process, 0U) == WAIT_OBJECT_0;
  const bool closed = ::CloseHandle(process) != FALSE;
  return exited && closed;
#else
  int status = 0;
  errno = 0;
  return ::waitpid(static_cast<pid_t>(process_id), &status, WNOHANG) == -1 && errno == ECHILD;
#endif
}

void run_post_spawn_exception_case(const std::string &self, const std::filesystem::path &marker,
                                   bool bounded, int &failures) {
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessStreamMode;
  using namespace nebula::cli::host_process_testing;

  std::error_code error;
  std::filesystem::remove(marker, error);
  inject_post_spawn_exception_once();
  HostProcessRequest request;
  request.executable_path = self;
  request.arguments = {self, "delayed-marker", marker.string()};
  request.stdout_mode = HostProcessStreamMode::Discard;
  request.stderr_mode = HostProcessStreamMode::Discard;
  request.timeout_milliseconds = bounded ? 5000U : 0U;
  bool caught_original = false;
  try {
    (void)nebula::cli::run_host_process(request);
  } catch (const std::runtime_error &exception) {
    caught_original =
      std::string_view(exception.what()).find("injected exception after") != std::string_view::npos;
  }
  const std::uint64_t process_id = last_injected_process_id();
  expect(caught_original,
         bounded ? "bounded guard must preserve the injected exception"
                 : "unbounded guard must preserve the injected exception",
         failures);
  expect(!post_spawn_exception_pending(), "post-spawn exception hook must be consumed", failures);
  expect(injected_child_is_reaped(process_id),
         bounded ? "bounded exception guard must reap its child"
                 : "unbounded exception guard must reap its child",
         failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(marker),
         bounded ? "bounded exception guard child must not write its delayed marker"
                 : "unbounded exception guard child must not write its delayed marker",
         failures);
}

void run_parent_stdin_endpoint_close_error_case(const std::string &self,
                                                const std::filesystem::path &marker, bool bounded,
                                                int &failures) {
  using nebula::cli::HostProcessContainment;
  using nebula::cli::HostProcessInputMode;
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessStreamMode;
  using namespace nebula::cli::host_process_testing;

  std::error_code error;
  std::filesystem::remove(marker, error);
  inject_parent_stdin_endpoint_close_error_once();
  HostProcessRequest request;
  request.executable_path = self;
  request.arguments = {self, "delayed-marker", marker.string()};
  request.stdin_mode = HostProcessInputMode::Discard;
  request.stdout_mode = HostProcessStreamMode::Discard;
  request.stderr_mode = HostProcessStreamMode::Discard;
  request.timeout_milliseconds = bounded ? 5000U : 0U;
  const nebula::cli::HostProcessResult result = nebula::cli::run_host_process(request);
  const std::uint64_t process_id = last_injected_process_id();
#if defined(_WIN32)
  constexpr std::string_view close_diagnostic =
    "failed to close the parent-side Windows stdin child endpoint after launch";
#else
  constexpr std::string_view close_diagnostic =
    "failed to close the parent-side POSIX stdin child endpoint after launch";
#endif
  expect(!parent_stdin_endpoint_close_error_pending(),
         "parent stdin endpoint close-error hook must be consumed exactly once", failures);
  expect(result.started && !result.completed(),
         bounded ? "bounded stdin close error must remain a started infrastructure failure"
                 : "unbounded stdin close error must remain a started infrastructure failure",
         failures);
  expect(result.infrastructure_error.find(close_diagnostic) != std::string::npos,
         bounded ? "bounded stdin close error must retain its explicit diagnostic"
                 : "unbounded stdin close error must retain its explicit diagnostic",
         failures);
  if (bounded) {
    expect(result.containment == HostProcessContainment::Confirmed,
           "bounded stdin close error must confirm containment cleanup", failures);
  }
  std::string compatible_error;
  expect(nebula::cli::host_process_compatible_exit_code(result, compatible_error) == 125 &&
           compatible_error.find(close_diagnostic) != std::string::npos,
         bounded ? "bounded stdin close error must map to explicit compatible status 125"
                 : "unbounded stdin close error must map to explicit compatible status 125",
         failures);
  expect(injected_child_is_reaped(process_id),
         bounded ? "bounded stdin close-error cleanup must reap its child"
                 : "unbounded stdin close-error cleanup must reap its child",
         failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(marker),
         bounded ? "bounded stdin close-error child must not write its delayed marker"
                 : "unbounded stdin close-error child must not write its delayed marker",
         failures);
  error.clear();
  std::filesystem::remove(marker, error);
}

void run_termination_failure_classification_case(const std::string &self,
                                                 const std::filesystem::path &marker,
                                                 int &failures) {
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessStreamMode;
  using namespace nebula::cli::host_process_testing;

  std::error_code error;
  std::filesystem::remove(marker, error);
  inject_unbounded_post_spawn_termination_failure_once();
  HostProcessRequest request;
  request.executable_path = self;
  request.arguments = {self, "delayed-marker", marker.string()};
  request.stdout_mode = HostProcessStreamMode::Discard;
  request.stderr_mode = HostProcessStreamMode::Discard;
  const nebula::cli::HostProcessResult result = nebula::cli::run_host_process(request);
  const std::uint64_t process_id = last_injected_process_id();
  expect(!unbounded_post_spawn_termination_failure_pending(),
         "termination-failure hook must be consumed", failures);
  expect(result.started && !result.completed() && !result.infrastructure_error.empty(),
         "failed termination followed by natural exit must remain an infrastructure failure",
         failures);
  expect(result.infrastructure_error.find("failed to terminate") != std::string::npos,
         "failed termination must remain observable after the child is reaped", failures);
  std::string compatible_error;
  expect(nebula::cli::host_process_compatible_exit_code(result, compatible_error) == 125 &&
           compatible_error.find("failed to terminate") != std::string::npos,
         "failed termination must map to explicit compatible status 125", failures);
  expect(injected_child_is_reaped(process_id),
         "termination-failure cleanup must still reap the naturally exited child", failures);
  expect(std::filesystem::exists(marker),
         "natural child completion must not be misreported as controlled cleanup", failures);
  error.clear();
  std::filesystem::remove(marker, error);
}

void run_post_cleanup_diagnostic_throw_case(const std::string &self,
                                            const std::filesystem::path &marker, int &failures) {
  using nebula::cli::HostProcessContainment;
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessStreamMode;

  std::error_code error;
  std::filesystem::remove(marker, error);
  HostProcessRequest request;
  request.executable_path = self;
  request.arguments = {self, "cleanup-diagnostic-throw-driver", marker.string()};
  request.stdout_mode = HostProcessStreamMode::Discard;
  request.stderr_mode = HostProcessStreamMode::Capture;
  request.max_stderr_bytes = 4096U;
  request.timeout_milliseconds = 5000U;
  const nebula::cli::HostProcessResult result = nebula::cli::run_host_process(request);
  expect(result.completed() && result.exit_code == 125U &&
           result.containment == HostProcessContainment::Confirmed,
         "unconfirmed post-cleanup diagnostic exception must exit with fixed status 125", failures);
  expect(result.stderr_data.find("child cleanup was unconfirmed") != std::string::npos,
         "fixed cleanup failure must remain observable without a second termination attempt",
         failures);
  expect(std::filesystem::exists(marker),
         "failed termination side effect must remain observable in diagnostic-throw subprocess",
         failures);
  error.clear();
  std::filesystem::remove(marker, error);
}

void run_live_cleanup_retry_case(const std::string &self, const std::filesystem::path &marker,
                                 int &failures) {
  using nebula::cli::HostProcessContainment;
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessStreamMode;

  std::error_code error;
  std::filesystem::remove(marker, error);
  HostProcessRequest request;
  request.executable_path = self;
  request.arguments = {self, "live-cleanup-retry-driver", marker.string()};
  request.stdout_mode = HostProcessStreamMode::Discard;
  request.stderr_mode = HostProcessStreamMode::Capture;
  request.max_stderr_bytes = 4096U;
  request.timeout_milliseconds = 7000U;
  const nebula::cli::HostProcessResult result = nebula::cli::run_host_process(request);
  expect(result.succeeded() && result.containment == HostProcessContainment::Confirmed,
         "live-owned child must be reaped by one emergency cleanup retry", failures);
  expect(!std::filesystem::exists(marker),
         "emergency cleanup retry must prevent the long-delayed child side effect", failures);
  error.clear();
  std::filesystem::remove(marker, error);
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1)
    return child_main(argc, argv);

  using nebula::cli::HostEnvironmentOverride;
  using nebula::cli::HostProcessContainment;
  using nebula::cli::HostProcessInputMode;
  using nebula::cli::HostProcessRequest;
  using nebula::cli::HostProcessResult;
  using nebula::cli::HostProcessStreamMode;
  using nebula::cli::detail::build_windows_environment_block;
  using nebula::cli::detail::quote_windows_argument;

  int failures = 0;
  expect(quote_windows_argument(L"") == L"\"\"", "empty Windows argument quoting", failures);
  expect(quote_windows_argument(L"plain") == L"plain", "plain Windows argument quoting", failures);
  expect(quote_windows_argument(L"two words") == L"\"two words\"", "space Windows argument quoting",
         failures);
  expect(quote_windows_argument(L"two\twords") == L"\"two\twords\"", "tab Windows argument quoting",
         failures);
  expect(quote_windows_argument(L"say\"hi") == L"\"say\\\"hi\"", "quote Windows argument escaping",
         failures);
  expect(quote_windows_argument(L"C:\\space path\\") == L"\"C:\\space path\\\\\"",
         "trailing backslash Windows argument quoting", failures);
  std::wstring slash_quote_input = L"a";
  slash_quote_input.append(2U, L'\\');
  slash_quote_input += L"\"b";
  std::wstring slash_quote_expected = L"\"a";
  slash_quote_expected.append(5U, L'\\');
  slash_quote_expected += L"\"b\"";
  expect(quote_windows_argument(slash_quote_input) == slash_quote_expected,
         "multiple backslashes before Windows quote", failures);
  expect(quote_windows_argument(L"星云") == L"星云", "Unicode Windows argument quoting", failures);
  expect(quote_windows_argument(L"星 云") == L"\"星 云\"",
         "Unicode spaced Windows argument quoting", failures);

  const auto empty_environment = build_windows_environment_block({}, {});
  expect(empty_environment.error.empty(), "empty Windows environment construction", failures);
  expect(empty_environment.block == std::vector<wchar_t>({L'\0', L'\0'}),
         "empty Windows environment must be double-NUL terminated", failures);

  const auto environment = build_windows_environment_block(
    {L"Path=old", L"KEEP=two", L"=C:=C:\\root"}, {{L"PATH", L"new"}, {L"TOKEN", L"秘密"}});
  expect(environment.error.empty(), "Windows environment override construction", failures);
  const std::vector<std::wstring> environment_entries = split_environment_block(environment.block);
  expect(environment_entries ==
           std::vector<std::wstring>({L"=C:=C:\\root", L"KEEP=two", L"PATH=new", L"TOKEN=秘密"}),
         "Windows environment replacement, drive entry preservation, and ordering", failures);
  expect(environment.block.size() >= 2U &&
           environment.block[environment.block.size() - 1U] == L'\0' &&
           environment.block[environment.block.size() - 2U] == L'\0',
         "nonempty Windows environment must be double-NUL terminated", failures);
  const auto duplicate_environment =
    build_windows_environment_block({}, {{L"Path", L"one"}, {L"PATH", L"two"}});
  expect(!duplicate_environment.error.empty(),
         "Windows environment overrides must reject case-insensitive duplicates", failures);

  const std::string self = std::filesystem::absolute(argv[0]).lexically_normal().string();

  HostProcessRequest containment_request;
  containment_request.executable_path = self;
  containment_request.arguments = {self, "containment-report"};
  containment_request.stdout_mode = HostProcessStreamMode::Capture;
  containment_request.stderr_mode = HostProcessStreamMode::Capture;
  containment_request.max_stdout_bytes = 1024U;
  containment_request.max_stderr_bytes = 1024U;
  containment_request.timeout_milliseconds = 1000U;
  const HostProcessResult containment = nebula::cli::run_host_process(containment_request);
  expect(containment.succeeded() && containment.stdout_data == "contained\n",
         "bounded child must start inside its containment domain", failures);
  expect(containment.containment == HostProcessContainment::Confirmed,
         "successful bounded process containment must be confirmed", failures);

  HostProcessRequest logical_argv0_request;
  logical_argv0_request.executable_path = self;
  logical_argv0_request.arguments = {"logical-nebula-name", "logical-argv0"};
  logical_argv0_request.stdout_mode = HostProcessStreamMode::Discard;
  logical_argv0_request.stderr_mode = HostProcessStreamMode::Discard;
  const HostProcessResult logical_argv0 = nebula::cli::run_host_process(logical_argv0_request);
  expect(logical_argv0.succeeded(),
         "verified executable path must remain separate from logical argv[0]", failures);

  const std::string marker_suffix =
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::filesystem::path unbounded_exception_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-exception-unbounded-" + marker_suffix);
  const std::filesystem::path bounded_exception_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-exception-bounded-" + marker_suffix);
  const std::filesystem::path unbounded_stdin_close_error_marker =
    std::filesystem::temp_directory_path() /
    ("nebula-host-stdin-close-error-unbounded-" + marker_suffix);
  const std::filesystem::path bounded_stdin_close_error_marker =
    std::filesystem::temp_directory_path() /
    ("nebula-host-stdin-close-error-bounded-" + marker_suffix);
  const std::filesystem::path termination_failure_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-termination-failure-" + marker_suffix);
  const std::filesystem::path diagnostic_throw_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-diagnostic-throw-" + marker_suffix);
  const std::filesystem::path live_cleanup_retry_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-live-cleanup-retry-" + marker_suffix);
  run_post_spawn_exception_case(self, unbounded_exception_marker, false, failures);
  run_post_spawn_exception_case(self, bounded_exception_marker, true, failures);
  run_parent_stdin_endpoint_close_error_case(self, unbounded_stdin_close_error_marker, false,
                                             failures);
  run_parent_stdin_endpoint_close_error_case(self, bounded_stdin_close_error_marker, true,
                                             failures);
  run_termination_failure_classification_case(self, termination_failure_marker, failures);
  run_post_cleanup_diagnostic_throw_case(self, diagnostic_throw_marker, failures);
  run_live_cleanup_retry_case(self, live_cleanup_retry_marker, failures);

  const std::filesystem::path timeout_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-timeout-" + marker_suffix);
  std::error_code marker_error;
  std::filesystem::remove(timeout_marker, marker_error);
  HostProcessRequest timeout_request;
  timeout_request.executable_path = self;
  timeout_request.arguments = {self, "delayed-marker", timeout_marker.string()};
  timeout_request.stdout_mode = HostProcessStreamMode::Discard;
  timeout_request.stderr_mode = HostProcessStreamMode::Discard;
  timeout_request.timeout_milliseconds = 50U;
  const auto timeout_started = std::chrono::steady_clock::now();
  const HostProcessResult timeout = nebula::cli::run_host_process(timeout_request);
  const auto timeout_elapsed = std::chrono::steady_clock::now() - timeout_started;
  expect(timeout.timed_out && timeout.containment == HostProcessContainment::Confirmed,
         "timeout must confirm containment-domain cleanup", failures);
  expect(timeout_elapsed < std::chrono::seconds(3), "timeout cleanup must remain bounded",
         failures);
  std::string timeout_exit_error;
  expect(nebula::cli::host_process_compatible_exit_code(timeout, timeout_exit_error) == 124 &&
           timeout_exit_error.empty(),
         "confirmed timeout must map to exit status 124", failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(timeout_marker),
         "timed-out child must not write its delayed marker", failures);

  const std::filesystem::path descendant_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-descendant-" + marker_suffix);
  marker_error.clear();
  std::filesystem::remove(descendant_marker, marker_error);
  marker_error.clear();
  std::filesystem::remove(unbounded_exception_marker, marker_error);
  marker_error.clear();
  std::filesystem::remove(bounded_exception_marker, marker_error);
  HostProcessRequest descendant_request;
  descendant_request.executable_path = self;
  descendant_request.arguments = {self, "spawn-marker-child", descendant_marker.string()};
  descendant_request.stdout_mode = HostProcessStreamMode::Discard;
  descendant_request.stderr_mode = HostProcessStreamMode::Discard;
  descendant_request.timeout_milliseconds = 50U;
  const HostProcessResult descendant_timeout = nebula::cli::run_host_process(descendant_request);
  expect(descendant_timeout.timed_out &&
           descendant_timeout.containment == HostProcessContainment::Confirmed,
         "timeout must contain a conforming descendant in the dedicated domain", failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(descendant_marker),
         "contained descendant must not escape timeout cleanup", failures);

  HostProcessRequest output_timeout_request;
  output_timeout_request.executable_path = self;
  output_timeout_request.arguments = {self, "paced-output"};
  output_timeout_request.stdout_mode = HostProcessStreamMode::Capture;
  output_timeout_request.stderr_mode = HostProcessStreamMode::Capture;
  output_timeout_request.max_stdout_bytes = 64U * 1024U * 1024U;
  output_timeout_request.max_stderr_bytes = 64U * 1024U * 1024U;
  output_timeout_request.timeout_milliseconds = 50U;
  const auto output_timeout_started = std::chrono::steady_clock::now();
  const HostProcessResult output_timeout = nebula::cli::run_host_process(output_timeout_request);
  expect(output_timeout.timed_out &&
           output_timeout.containment == HostProcessContainment::Confirmed,
         "continuous output must not starve timeout observation", failures);
  expect(std::chrono::steady_clock::now() - output_timeout_started < std::chrono::seconds(3),
         "continuous-output timeout cleanup must remain bounded", failures);

#if !defined(_WIN32)
  const std::filesystem::path leader_exit_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-leader-exit-" + marker_suffix);
  marker_error.clear();
  std::filesystem::remove(leader_exit_marker, marker_error);
  HostProcessRequest leader_exit_request;
  leader_exit_request.executable_path = self;
  leader_exit_request.arguments = {self, "leader-exit-with-marker-child",
                                   leader_exit_marker.string()};
  leader_exit_request.stdout_mode = HostProcessStreamMode::Discard;
  leader_exit_request.stderr_mode = HostProcessStreamMode::Discard;
  leader_exit_request.timeout_milliseconds = 2000U;
  const auto leader_exit_started = std::chrono::steady_clock::now();
  const HostProcessResult leader_exit = nebula::cli::run_host_process(leader_exit_request);
  expect(leader_exit.completed() && leader_exit.exit_code == 17U &&
           leader_exit.containment == HostProcessContainment::Confirmed,
         "normal leader exit must preserve status after sealing descendants", failures);
  expect(std::chrono::steady_clock::now() - leader_exit_started < std::chrono::seconds(1),
         "leader exit must seal descendants without waiting for the request timeout", failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(leader_exit_marker),
         "leader-exit descendant must not survive group sealing", failures);

  const std::filesystem::path interrupted_marker =
    std::filesystem::temp_directory_path() / ("nebula-host-interrupted-" + marker_suffix);
  marker_error.clear();
  std::filesystem::remove(interrupted_marker, marker_error);
  CompilerTerminationSignalScope termination_signals;
  std::string signal_detail;
  expect(termination_signals.arm(signal_detail), "arm hosted termination signal boundary",
         failures);
  HostProcessRequest unbounded_signal_request;
  unbounded_signal_request.executable_path = self;
  unbounded_signal_request.arguments = {self, "discard"};
  unbounded_signal_request.stdout_mode = HostProcessStreamMode::Discard;
  unbounded_signal_request.stderr_mode = HostProcessStreamMode::Discard;
  unbounded_signal_request.termination_signals = &termination_signals;
  const HostProcessResult unbounded_signal =
    nebula::cli::run_host_process(unbounded_signal_request);
  expect(!unbounded_signal.started &&
           unbounded_signal.infrastructure_error.find("positive timeout") != std::string::npos,
         "termination signal control must reject an unbounded process before spawn", failures);
  HostProcessRequest interrupted_request;
  interrupted_request.executable_path = self;
  interrupted_request.arguments = {self, "delayed-marker", interrupted_marker.string()};
  interrupted_request.stdout_mode = HostProcessStreamMode::Discard;
  interrupted_request.stderr_mode = HostProcessStreamMode::Discard;
  interrupted_request.timeout_milliseconds = 2000U;
  interrupted_request.termination_signals = &termination_signals;
  std::thread interrupter([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)::kill(::getpid(), SIGINT);
  });
  const HostProcessResult interrupted = nebula::cli::run_host_process(interrupted_request);
  interrupter.join();
  expect(interrupted.parent_interruption_signal == SIGINT &&
           interrupted.containment == HostProcessContainment::Confirmed,
         "caller interruption must confirm process-group cleanup", failures);
  std::string interrupted_exit_error;
  expect(nebula::cli::host_process_compatible_exit_code(interrupted, interrupted_exit_error) ==
             128 + SIGINT &&
           interrupted_exit_error.empty(),
         "confirmed caller interruption must preserve signal exit semantics", failures);
  int restored_signal = 0;
  signal_detail.clear();
  expect(termination_signals.restore(restored_signal, signal_detail) && restored_signal == SIGINT,
         "hosted termination boundary must restore and return the caller signal", failures);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  expect(!std::filesystem::exists(interrupted_marker),
         "interrupted child must not write its delayed marker", failures);

  struct sigaction previous_sigchld{};
  struct sigaction ignored_sigchld{};
  ignored_sigchld.sa_handler = SIG_IGN;
  expect(sigemptyset(&ignored_sigchld.sa_mask) == 0, "initialize SIGCHLD test mask", failures);
  expect(::sigaction(SIGCHLD, &ignored_sigchld, &previous_sigchld) == 0,
         "install SIGCHLD ignore disposition", failures);
  const auto run_invalid_sigchld_case = [&](std::uint64_t timeout_milliseconds) {
    HostProcessRequest invalid_sigchld_request;
    invalid_sigchld_request.executable_path = self;
    invalid_sigchld_request.arguments = {self, "discard"};
    invalid_sigchld_request.stdout_mode = HostProcessStreamMode::Discard;
    invalid_sigchld_request.stderr_mode = HostProcessStreamMode::Discard;
    invalid_sigchld_request.timeout_milliseconds = timeout_milliseconds;
    const HostProcessResult invalid_sigchld =
      nebula::cli::run_host_process(invalid_sigchld_request);
    expect(!invalid_sigchld.started && invalid_sigchld.infrastructure_error.find(
                                         "default SIGCHLD disposition") != std::string::npos,
           timeout_milliseconds == 0U
             ? "unbounded process must reject incompatible SIGCHLD ownership before spawn"
             : "bounded process must reject incompatible SIGCHLD ownership before spawn",
           failures);
  };
  run_invalid_sigchld_case(0U);
  run_invalid_sigchld_case(100U);
  expect(::sigaction(SIGCHLD, &previous_sigchld, nullptr) == 0, "restore SIGCHLD disposition",
         failures);
#endif

  marker_error.clear();
  std::filesystem::remove(timeout_marker, marker_error);
  marker_error.clear();
  std::filesystem::remove(descendant_marker, marker_error);
#if !defined(_WIN32)
  marker_error.clear();
  std::filesystem::remove(leader_exit_marker, marker_error);
  marker_error.clear();
  std::filesystem::remove(interrupted_marker, marker_error);
#endif

  HostProcessRequest capture_request;
  capture_request.executable_path = self;
  capture_request.arguments = {self, "capture", "two words", "quote\"value", "trailing\\", ""};
  capture_request.environment_overrides = {
    HostEnvironmentOverride{"NEBULA_HOST_PROCESS_TEST_VALUE", "override value"}};
  capture_request.stdout_mode = HostProcessStreamMode::Capture;
  capture_request.stderr_mode = HostProcessStreamMode::Capture;
  capture_request.max_stdout_bytes = 1024U;
  capture_request.max_stderr_bytes = 1024U;
  const HostProcessResult capture = nebula::cli::run_host_process(capture_request);
  expect(capture.completed(), "captured host process must complete", failures);
  expect(capture.exit_code == 23U, "captured host process exit code", failures);
  expect(capture.stdout_data == "stdout-record\n", "captured stdout", failures);
  expect(capture.stderr_data == "stderr-record\n", "captured stderr", failures);

  const char *parent_path = std::getenv("PATH");
#if defined(_WIN32)
  const bool marker_set = ::_putenv_s("NEBULA_HOST_PROCESS_TEST_INHERITED", "must-not-leak") == 0;
#else
  const bool marker_set = ::setenv("NEBULA_HOST_PROCESS_TEST_INHERITED", "must-not-leak", 1) == 0;
#endif
  expect(parent_path != nullptr, "parent PATH for isolated environment process", failures);
  expect(marker_set, "set inherited environment marker", failures);
  if (parent_path != nullptr && marker_set) {
    HostProcessRequest isolated_environment_request;
    isolated_environment_request.executable_path = self;
    isolated_environment_request.arguments = {self, "isolated-environment"};
    isolated_environment_request.inherit_environment = false;
    isolated_environment_request.environment_overrides = {
      HostEnvironmentOverride{"NEBULA_HOST_PROCESS_TEST_VALUE", "isolated"},
      HostEnvironmentOverride{"PATH", parent_path}};
    isolated_environment_request.stdout_mode = HostProcessStreamMode::Discard;
    isolated_environment_request.stderr_mode = HostProcessStreamMode::Discard;
    const HostProcessResult isolated_environment =
      nebula::cli::run_host_process(isolated_environment_request);
    expect(isolated_environment.succeeded(), "isolated environment process", failures);
  }

  HostProcessRequest overflow_request;
  overflow_request.executable_path = self;
  overflow_request.arguments = {self, "overflow"};
  overflow_request.stdout_mode = HostProcessStreamMode::Capture;
  overflow_request.stderr_mode = HostProcessStreamMode::Capture;
  overflow_request.max_stdout_bytes = 17U;
  overflow_request.max_stderr_bytes = 19U;
  const HostProcessResult overflow = nebula::cli::run_host_process(overflow_request);
  expect(overflow.exited, "overflowing child must still be reaped", failures);
  expect(!overflow.completed(), "capture overflow must fail explicitly", failures);
  expect(overflow.stdout_limit_exceeded && overflow.stderr_limit_exceeded,
         "both capture overflow flags", failures);
  expect(overflow.stdout_data.size() == overflow_request.max_stdout_bytes &&
           overflow.stderr_data.size() == overflow_request.max_stderr_bytes,
         "capture data must remain strictly bounded", failures);
  expect(overflow.infrastructure_error.find("stdout capture exceeded") != std::string::npos &&
           overflow.infrastructure_error.find("stderr capture exceeded") != std::string::npos,
         "capture overflow diagnostics", failures);

  HostProcessRequest discard_request;
  discard_request.executable_path = self;
  discard_request.arguments = {self, "discard"};
  discard_request.stdout_mode = HostProcessStreamMode::Discard;
  discard_request.stderr_mode = HostProcessStreamMode::Discard;
  const HostProcessResult discarded = nebula::cli::run_host_process(discard_request);
  expect(discarded.succeeded(), "discarded stream process", failures);
  expect(discarded.stdout_data.empty() && discarded.stderr_data.empty(),
         "discarded streams must not be captured", failures);

  HostProcessRequest discarded_stdin_request;
  discarded_stdin_request.executable_path = self;
  discarded_stdin_request.arguments = {self, "stdin-eof"};
  discarded_stdin_request.stdin_mode = HostProcessInputMode::Discard;
  discarded_stdin_request.stdout_mode = HostProcessStreamMode::Discard;
  discarded_stdin_request.stderr_mode = HostProcessStreamMode::Discard;
  const HostProcessResult discarded_stdin = nebula::cli::run_host_process(discarded_stdin_request);
  expect(discarded_stdin.succeeded(), "discarded stdin must present immediate EOF", failures);

  HostProcessRequest exit_259_request;
  exit_259_request.executable_path = self;
  exit_259_request.arguments = {self, "exit-259"};
  exit_259_request.stdout_mode = HostProcessStreamMode::Discard;
  exit_259_request.stderr_mode = HostProcessStreamMode::Discard;
  const HostProcessResult exit_259 = nebula::cli::run_host_process(exit_259_request);
  expect(exit_259.completed(), "259 exit-code child must complete", failures);
#if defined(_WIN32)
  expect(exit_259.exit_code == 259U, "Windows exit code 259 must not mean STILL_ACTIVE after wait",
         failures);
#else
  expect(exit_259.exit_code == 3U, "POSIX exit status retains its native eight-bit contract",
         failures);
#endif

#if !defined(_WIN32)
  const int sentinel_base = ::open("/dev/null", O_RDONLY);
  expect(sentinel_base >= 0, "open parent sentinel descriptor", failures);
  if (sentinel_base >= 0) {
    const int sentinel = ::fcntl(sentinel_base, F_DUPFD, 64);
    const bool base_closed = ::close(sentinel_base) == 0;
    expect(sentinel >= 64 && base_closed, "create high non-CLOEXEC sentinel descriptor", failures);
    if (sentinel >= 64) {
      const int descriptor_flags = ::fcntl(sentinel, F_GETFD);
      const bool non_cloexec =
        descriptor_flags >= 0 && ::fcntl(sentinel, F_SETFD, descriptor_flags & ~FD_CLOEXEC) == 0;
      expect(non_cloexec, "clear CLOEXEC on parent sentinel descriptor", failures);
      HostProcessRequest descriptor_boundary_request;
      descriptor_boundary_request.executable_path = self;
      descriptor_boundary_request.arguments = {self, "fd-boundary", std::to_string(sentinel)};
      descriptor_boundary_request.stdout_mode = HostProcessStreamMode::Capture;
      descriptor_boundary_request.stderr_mode = HostProcessStreamMode::Inherit;
      descriptor_boundary_request.max_stdout_bytes = 1024U;
      const HostProcessResult descriptor_boundary =
        nebula::cli::run_host_process(descriptor_boundary_request);
      const bool sentinel_closed = ::close(sentinel) == 0;
      expect(sentinel_closed, "close parent sentinel descriptor", failures);
      expect(descriptor_boundary.succeeded() &&
               descriptor_boundary.stdout_data == "fd-boundary-ok\n",
             "POSIX child must inherit only standard descriptors while capture remains usable",
             failures);
    }
  }

  std::cerr.flush();
  const int saved_stderr = ::dup(STDERR_FILENO);
  expect(saved_stderr >= 0, "save stderr before closed-inherit test", failures);
  if (saved_stderr >= 0) {
    const bool stderr_closed = ::close(STDERR_FILENO) == 0;
    HostProcessResult closed_inherited_stderr;
    if (stderr_closed) {
      HostProcessRequest closed_inherited_stderr_request;
      closed_inherited_stderr_request.executable_path = self;
      closed_inherited_stderr_request.arguments = {self, "closed-inherited-stderr"};
      closed_inherited_stderr_request.stdout_mode = HostProcessStreamMode::Capture;
      closed_inherited_stderr_request.stderr_mode = HostProcessStreamMode::Inherit;
      closed_inherited_stderr_request.max_stdout_bytes = 1024U;
      closed_inherited_stderr = nebula::cli::run_host_process(closed_inherited_stderr_request);
    }
    const bool restored = ::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO;
    const bool saved_closed = ::close(saved_stderr) == 0;
    expect(stderr_closed && restored && saved_closed, "restore stderr after closed-inherit test",
           failures);
    expect(closed_inherited_stderr.succeeded() &&
             closed_inherited_stderr.stdout_data == "closed-inherited-stderr-ok\n",
           "closed inherited standard descriptors must remain closed in the POSIX child", failures);
  }

  std::cout.flush();
  const int saved_stdout = ::dup(STDOUT_FILENO);
  expect(saved_stdout >= 0, "save stdout before descriptor-reuse test", failures);
  if (saved_stdout >= 0) {
    expect(::close(STDOUT_FILENO) == 0, "close stdout before descriptor-reuse test", failures);
    HostProcessRequest descriptor_reuse_request;
    descriptor_reuse_request.executable_path = self;
    descriptor_reuse_request.arguments = {self, "standard-fd-reuse"};
    descriptor_reuse_request.stdout_mode = HostProcessStreamMode::Capture;
    descriptor_reuse_request.stderr_mode = HostProcessStreamMode::Discard;
    descriptor_reuse_request.max_stdout_bytes = 1024U;
    const HostProcessResult descriptor_reuse =
      nebula::cli::run_host_process(descriptor_reuse_request);
    const bool restored = ::dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO;
    const bool saved_closed = ::close(saved_stdout) == 0;
    expect(restored && saved_closed, "restore stdout after descriptor-reuse test", failures);
    expect(descriptor_reuse.succeeded() && descriptor_reuse.stdout_data == "standard-fd-reuse-ok\n",
           "POSIX child descriptor must survive exec when standard fd was initially free",
           failures);
  }
#endif

  CompilerTerminationSignalScope unsupported_signal_scope;
  HostProcessRequest unsupported_signal_request;
  unsupported_signal_request.executable_path = self;
  unsupported_signal_request.arguments = {self, "discard"};
  unsupported_signal_request.stdout_mode = HostProcessStreamMode::Discard;
  unsupported_signal_request.stderr_mode = HostProcessStreamMode::Discard;
  unsupported_signal_request.timeout_milliseconds = 100U;
  unsupported_signal_request.termination_signals = &unsupported_signal_scope;
  const HostProcessResult unsupported_signal =
    nebula::cli::run_host_process(unsupported_signal_request);
#if defined(_WIN32)
  expect(!unsupported_signal.started && unsupported_signal.infrastructure_error.find(
                                          "unavailable on Windows") != std::string::npos,
         "Windows must reject POSIX termination signal control before spawn", failures);
#else
  expect(!unsupported_signal.started &&
           unsupported_signal.infrastructure_error.find("not armed") != std::string::npos,
         "POSIX must reject an unarmed termination signal boundary before spawn", failures);
#endif

  HostProcessRequest invalid_request;
  invalid_request.executable_path = "relative-program";
  invalid_request.arguments = {"logical-program-name"};
  const HostProcessResult invalid = nebula::cli::run_host_process(invalid_request);
  expect(!invalid.started && !invalid.infrastructure_error.empty(),
         "relative executable must fail before spawn", failures);

  HostProcessRequest invalid_capture_request;
  invalid_capture_request.executable_path = self;
  invalid_capture_request.arguments = {self, "discard"};
  invalid_capture_request.stdout_mode = HostProcessStreamMode::Capture;
  const HostProcessResult invalid_capture = nebula::cli::run_host_process(invalid_capture_request);
  expect(!invalid_capture.started && !invalid_capture.infrastructure_error.empty(),
         "capture without a byte limit must fail before spawn", failures);

  if (failures != 0)
    return 1;
  std::cout << "host-process-tests-ok\n";
  return 0;
}
