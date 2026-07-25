#include "cli/freestanding_object.hpp"
#include "cli/termination_signal.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

static_assert(!std::is_copy_constructible_v<nebula::cli::ResolvedFreestandingToolchain>);
static_assert(std::is_move_constructible_v<nebula::cli::ResolvedFreestandingToolchain>);
static_assert(std::is_nothrow_move_constructible_v<nebula::cli::ResolvedFreestandingToolchain>);
static_assert(!std::is_move_assignable_v<nebula::cli::ResolvedFreestandingToolchain>);

volatile sig_atomic_t caller_signal = 0;

void record_caller_signal(int signal_number) { caller_signal = signal_number; }

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "freestanding-transaction-test: " << message << '\n';
  return false;
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/nebula-freestanding-transaction-XXXXXX";
    pattern.push_back('\0');
    char *created = ::mkdtemp(pattern.data());
    if (created != nullptr)
      path_ = fs::path(created);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    if (!path_.has_value())
      return;
    std::error_code error;
    fs::remove_all(*path_, error);
    if (error) {
      std::cerr << "freestanding-transaction-test: temporary cleanup failed: " << error.message()
                << '\n';
    }
  }

  [[nodiscard]] const std::optional<fs::path> &path() const noexcept { return path_; }

private:
  std::optional<fs::path> path_;
};

class SignalFixtureGuard {
public:
  SignalFixtureGuard() = default;
  SignalFixtureGuard(const SignalFixtureGuard &) = delete;
  SignalFixtureGuard &operator=(const SignalFixtureGuard &) = delete;

  ~SignalFixtureGuard() {
    if (!active_)
      return;
    std::string detail;
    if (!restore(detail))
      std::cerr << "freestanding-transaction-test: " << detail << '\n';
  }

  bool install(std::string &detail) {
    if (active_) {
      detail = "SIGTERM fixture was installed more than once";
      return false;
    }
    if (sigemptyset(&signal_set_) != 0 || sigaddset(&signal_set_, SIGTERM) != 0) {
      detail = "could not initialize the SIGTERM fixture set";
      return false;
    }
    if (::sigprocmask(SIG_BLOCK, &signal_set_, &previous_mask_) != 0) {
      detail =
        "could not save and block the caller SIGTERM mask: " + std::string(std::strerror(errno));
      return false;
    }
    mask_saved_ = true;

    sigset_t pending_set{};
    if (::sigpending(&pending_set) != 0) {
      detail = "could not inspect caller pending signals: " + std::string(std::strerror(errno));
      restore_mask_after_setup_failure(detail);
      return false;
    }
    const int pending_membership = sigismember(&pending_set, SIGTERM);
    if (pending_membership < 0) {
      detail =
        "could not inspect caller pending SIGTERM membership: " + std::string(std::strerror(errno));
      restore_mask_after_setup_failure(detail);
      return false;
    }
    if (pending_membership != 0) {
      detail = "caller entered the test with a pending SIGTERM";
      restore_mask_after_setup_failure(detail);
      return false;
    }

    struct sigaction action{};
    action.sa_handler = record_caller_signal;
    if (sigemptyset(&action.sa_mask) != 0 ||
        ::sigaction(SIGTERM, &action, &previous_action_) != 0) {
      detail = "could not install the caller SIGTERM fixture: " + std::string(std::strerror(errno));
      restore_mask_after_setup_failure(detail);
      return false;
    }
    handler_installed_ = true;
    active_ = true;

    sigset_t test_mask = previous_mask_;
    if (sigdelset(&test_mask, SIGTERM) != 0 ||
        ::sigprocmask(SIG_SETMASK, &test_mask, nullptr) != 0) {
      detail =
        "could not establish an unblocked SIGTERM fixture: " + std::string(std::strerror(errno));
      std::string restore_detail;
      if (!restore(restore_detail) && !restore_detail.empty())
        detail += "; setup rollback failed: " + restore_detail;
      return false;
    }
    return true;
  }

  bool restore(std::string &detail) {
    if (!active_)
      return true;
    bool complete = true;
    if (::sigprocmask(SIG_BLOCK, &signal_set_, nullptr) != 0) {
      detail =
        "could not block SIGTERM while restoring the fixture: " + std::string(std::strerror(errno));
      complete = false;
    }
    if (handler_installed_ && ::sigaction(SIGTERM, &previous_action_, nullptr) != 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "could not restore the caller SIGTERM action: " + std::string(std::strerror(errno));
      complete = false;
    } else {
      handler_installed_ = false;
    }
    if (mask_saved_ && ::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) != 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "could not restore the caller signal mask: " + std::string(std::strerror(errno));
      complete = false;
    }
    if (complete) {
      mask_saved_ = false;
      active_ = false;
    }
    return complete;
  }

private:
  void restore_mask_after_setup_failure(std::string &detail) {
    if (!mask_saved_)
      return;
    if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) == 0) {
      mask_saved_ = false;
      return;
    }
    detail += "; caller signal-mask rollback failed: ";
    detail += std::strerror(errno);
    active_ = true;
  }

  sigset_t signal_set_{};
  sigset_t previous_mask_{};
  struct sigaction previous_action_{};
  bool mask_saved_ = false;
  bool handler_installed_ = false;
  bool active_ = false;
};

bool compiler_environment_is_minimal_and_private(const std::vector<std::string> &environment) {
  if (environment.size() != 4U)
    return false;
  bool has_locale = false;
  bool has_language = false;
  bool has_timezone = false;
  std::optional<fs::path> temporary_directory;
  constexpr std::string_view kTemporaryDirectoryPrefix = "TMPDIR=";
  for (const std::string &entry : environment) {
    if (entry == "LC_ALL=C") {
      has_locale = true;
    } else if (entry == "LANG=C") {
      has_language = true;
    } else if (entry == "TZ=UTC") {
      has_timezone = true;
    } else if (entry.starts_with(kTemporaryDirectoryPrefix)) {
      temporary_directory = fs::path(entry.substr(kTemporaryDirectoryPrefix.size()));
    } else {
      return false;
    }
  }
  if (!has_locale || !has_language || !has_timezone || !temporary_directory.has_value())
    return false;
  struct stat status{};
  return ::stat(temporary_directory->c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         (status.st_mode & 0777) == S_IRWXU && status.st_uid == ::geteuid();
}

bool compiler_command_matches(const std::vector<std::string> &command,
                              const fs::path &expected_compiler) {
  constexpr std::array<std::string_view, 36U> kFixedArguments = {
    "--target=x86_64-unknown-none",
    "--no-default-config",
    "-std=c++20",
    "-x",
    "c++",
    "-ffreestanding",
    "-nostdinc",
    "-nostdinc++",
    "-m64",
    "-mabi=sysv",
    "-mno-red-zone",
    "-mno-80387",
    "-mno-mmx",
    "-mno-sse",
    "-mno-sse2",
    "-fno-builtin",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-fno-threadsafe-statics",
    "-fno-use-cxa-atexit",
    "-fno-pic",
    "-fno-pie",
    "-fno-ident",
    "-fno-addrsig",
    "-fvisibility=hidden",
    "-mcmodel=kernel",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-O2",
    "-c",
    "-o",
  };
  if (command.size() != 1U + kFixedArguments.size() + 3U ||
      command.front() != expected_compiler.string()) {
    std::cerr << "freestanding-transaction-test: compiler command shape mismatch: size="
              << command.size() << " expected_size=" << (1U + kFixedArguments.size() + 3U)
              << " argv0=" << (command.empty() ? std::string("<empty>") : command.front())
              << " expected_argv0=" << expected_compiler.string() << '\n';
    return false;
  }
  for (std::size_t index = 0; index < kFixedArguments.size(); ++index) {
    if (command[index + 1U] != kFixedArguments[index]) {
      std::cerr << "freestanding-transaction-test: compiler argument mismatch at " << index
                << ": actual=" << command[index + 1U] << " expected=" << kFixedArguments[index]
                << '\n';
      return false;
    }
  }
  const fs::path object = command[1U + kFixedArguments.size()];
  if (command[2U + kFixedArguments.size()] != "--")
    return false;
  const fs::path source = command[3U + kFixedArguments.size()];
  return object.filename() == "compiler-output.o" && source.filename() == "unit.cpp" &&
         object.parent_path() == source.parent_path();
}

class UnconfirmedCompilerExecutor final : public FreestandingCompilerExecutor {
public:
  explicit UnconfirmedCompilerExecutor(fs::path expected_compiler)
      : expected_compiler_(std::move(expected_compiler)) {}

  CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) override {
    called = true;
    saw_ready_scope = termination_signals.ready_for_execution();
    saw_fixed_timeout = timeout_seconds == 30;
    saw_command = compiler_command_matches(command, expected_compiler_);
    saw_environment = compiler_environment_is_minimal_and_private(environment);
    raise_succeeded = ::raise(SIGTERM) == 0;
    return {
      125,
      false,
      "injected compiler leader ownership loss",
      0,
      CompilerProcessContainment::Unconfirmed,
    };
  }

  bool called = false;
  bool saw_ready_scope = false;
  bool saw_fixed_timeout = false;
  bool saw_command = false;
  bool saw_environment = false;
  bool raise_succeeded = false;

private:
  fs::path expected_compiler_;
};

class EmptyUnconfirmedCompilerExecutor final : public FreestandingCompilerExecutor {
public:
  CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) override {
    (void)command;
    (void)environment;
    (void)timeout_seconds;
    (void)termination_signals;
    return {
      0, false, {}, 0, CompilerProcessContainment::Unconfirmed,
    };
  }
};

class FixedCompilerExecutor final : public FreestandingCompilerExecutor {
public:
  FixedCompilerExecutor(fs::path expected_compiler, CommandExecutionResult result)
      : expected_compiler_(std::move(expected_compiler)), result_(std::move(result)) {}

  CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) override {
    called = true;
    saw_command = compiler_command_matches(command, expected_compiler_);
    saw_environment = compiler_environment_is_minimal_and_private(environment);
    saw_timeout = timeout_seconds == 30;
    saw_signal_scope = termination_signals.ready_for_execution();
    contract_valid = saw_command && saw_environment && saw_timeout && saw_signal_scope;
    return result_;
  }

  bool called = false;
  bool contract_valid = false;
  bool saw_command = false;
  bool saw_environment = false;
  bool saw_timeout = false;
  bool saw_signal_scope = false;

private:
  fs::path expected_compiler_;
  CommandExecutionResult result_;
};

bool write_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return !output.fail();
}

bool has_diagnostic(const FreestandingObjectResult &result, std::string_view code,
                    std::string_view text) {
  for (const auto &diagnostic : result.diagnostics) {
    if (diagnostic.code == code && (diagnostic.message.find(text) != std::string::npos ||
                                    diagnostic.cause.find(text) != std::string::npos ||
                                    diagnostic.impact.find(text) != std::string::npos)) {
      return true;
    }
  }
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

bool output_lock_is_available(const fs::path &lock_path, std::string &detail) {
  int flags = O_RDWR;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(lock_path.c_str(), flags);
  if (descriptor < 0) {
    detail = "lock open failed: " + std::string(std::strerror(errno));
    return false;
  }
  const bool acquired = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
  if (!acquired)
    detail = "lock acquire failed: " + std::string(std::strerror(errno));
  bool released = true;
  if (acquired && ::flock(descriptor, LOCK_UN) != 0) {
    detail = "lock release failed: " + std::string(std::strerror(errno));
    released = false;
  }
  if (::close(descriptor) != 0) {
    detail = "lock close failed: " + std::string(std::strerror(errno));
    released = false;
  }
  return acquired && released;
}

bool staging_is_absent(const fs::path &root) {
  std::error_code error;
  for (fs::directory_iterator iterator(root, error), end; !error && iterator != end;
       iterator.increment(error)) {
    const std::string name = iterator->path().filename().string();
    if (name.starts_with(".nebula-fs-"))
      return false;
  }
  return !error;
}

bool compiler_lease_is_absent(const fs::path &toolchain_root) {
  std::error_code error;
  const fs::path compiler_directory = toolchain_root / "bin";
  for (fs::directory_iterator iterator(compiler_directory, error), end; !error && iterator != end;
       iterator.increment(error)) {
    if (iterator->path().filename().string().starts_with(".nebula-exec-"))
      return false;
  }
  return !error;
}

bool run_unconfirmed_transaction_test(const fs::path &self_executable) {
  TemporaryDirectory temporary;
  if (!expect(temporary.path().has_value(), "could not create a temporary directory"))
    return false;
  const fs::path root = *temporary.path();
  const fs::path toolchain_root = root / "toolchain";
  std::error_code directory_error;
  fs::create_directories(toolchain_root / "bin", directory_error);
  if (!expect(!directory_error, "could not create the fake toolchain root"))
    return false;
  const fs::path fake_clang = toolchain_root / "bin" / "clang++";
  constexpr std::string_view kFakeClang =
    "#!/bin/sh\n"
    "if [ \"$#\" -eq 1 ] && [ \"$1\" = '--version' ]; then "
    "printf '%s\\n' 'Nebula test clang 1.0'; exit 0; fi\n"
    "for argument in \"$@\"; do\n"
    "  if [ \"$argument\" = '-dumpmachine' ]; then "
    "printf '%s\\n' 'x86_64-unknown-none'; exit 0; fi\n"
    "  if [ \"$argument\" = '-fsyntax-only' ]; then exit 0; fi\n"
    "done\n"
    "exit 99\n";
  if (!expect(write_file(fake_clang, kFakeClang), "could not write the fake clang fixture") ||
      !expect(::chmod(fake_clang.c_str(), S_IRWXU) == 0,
              "could not make the fake clang fixture executable")) {
    return false;
  }

  SignalFixtureGuard signal_fixture;
  std::string signal_fixture_detail;
  if (!expect(signal_fixture.install(signal_fixture_detail), signal_fixture_detail))
    return false;

  nebula::cli::FreestandingToolchainResolutionResult toolchain_resolution =
    nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
  if (!expect(toolchain_resolution.ok(), toolchain_resolution.error.detail.empty()
                                           ? "could not resolve the fake freestanding toolchain"
                                           : toolchain_resolution.error.detail))
    return false;
  nebula::cli::ResolvedFreestandingToolchain &toolchain = *toolchain_resolution.value;
  bool snapshot_ok = true;
  snapshot_ok &= expect(toolchain.compiler_execution_path() != toolchain.compiler().executable,
                        "resolved compiler execution path was not a private snapshot");
  snapshot_ok &= expect(fs::exists(toolchain.compiler_execution_path()),
                        "resolved compiler snapshot does not exist");

  const fs::path original_fake_clang = toolchain_root / "bin" / "clang++.public-original";
  std::error_code swap_error;
  fs::rename(fake_clang, original_fake_clang, swap_error);
  snapshot_ok &= expect(!swap_error, "could not move the public compiler for snapshot test");
  constexpr std::string_view kReplacementClang =
    "#!/bin/sh\nprintf '%s\\n' 'replacement compiler executed'\nexit 86\n";
  snapshot_ok &= expect(write_file(fake_clang, kReplacementClang),
                        "could not install the public compiler replacement");
  snapshot_ok &= expect(::chmod(fake_clang.c_str(), S_IRWXU) == 0,
                        "could not make the public compiler replacement executable");

  nebula::cli::HostProcessRequest snapshot_request;
  snapshot_request.arguments = {toolchain.compiler().executable.string(), "--version"};
  snapshot_request.inherit_environment = false;
  snapshot_request.environment_overrides = {{"LANG", "C"}, {"LC_ALL", "C"}, {"TZ", "UTC"}};
  snapshot_request.stdin_mode = nebula::cli::HostProcessInputMode::Discard;
  snapshot_request.stdout_mode = nebula::cli::HostProcessStreamMode::Capture;
  snapshot_request.stderr_mode = nebula::cli::HostProcessStreamMode::Capture;
  snapshot_request.max_stdout_bytes = 1024U;
  snapshot_request.max_stderr_bytes = 1024U;
  snapshot_request.timeout_milliseconds = 1000U;
  snapshot_request.termination_signals = &toolchain.termination_signals();
  const nebula::cli::HostProcessResult snapshot_execution =
    toolchain.execute_compiler(std::move(snapshot_request));
  snapshot_ok &= expect(snapshot_execution.succeeded() &&
                          snapshot_execution.stdout_data == "Nebula test clang 1.0\n" &&
                          snapshot_execution.stderr_data.empty(),
                        "public compiler replacement changed the leased execution bytes");

  fs::remove(fake_clang, swap_error);
  snapshot_ok &= expect(!swap_error, "could not remove the public compiler replacement");
  swap_error.clear();
  fs::rename(original_fake_clang, fake_clang, swap_error);
  snapshot_ok &= expect(!swap_error, "could not restore the public compiler after snapshot test");
  std::string revalidation_detail;
  snapshot_ok &= expect(toolchain.revalidate(revalidation_detail), revalidation_detail);
  if (!snapshot_ok)
    return false;

  const fs::path input = root / "input.nb";
  if (!expect(write_file(input, "@entry fn main() -> Void {}\n"),
              "could not write the input fixture")) {
    return false;
  }

  caller_signal = 0;
  const fs::path object = root / "result.o";
  FreestandingObjectRequest request;
  request.input_path = input;
  request.generated_source_path = root / "result.freestanding.cpp";
  request.object_path = object;
  request.translation_unit = "extern \"C\" void __nebula_uos_payload_entry_v1() noexcept {}\n";
  request.mode = BuildMode::Release;
  request.build_key.build_inputs_sha256 = std::string(64U, '1');
  request.build_key.mode = "release";
  request.build_key.profile = "deep";
  request.build_key.artifact_kind = "freestanding-object";
  request.build_key.compiler_schema_version = 1;
  request.build_key.cache_schema_version = 4;
  request.build_key.strict_region = true;
  request.build_key.warnings_as_errors = false;
  request.build_key.no_std = true;
  request.build_key.runtime_profile = "system";
  request.build_key.target = "x86_64-unknown-none";
  request.build_key.panic_policy = "trap";

  UnconfirmedCompilerExecutor executor(toolchain.compiler().executable);
  const FreestandingObjectResult result = build_freestanding_object(request, toolchain, executor);

  bool pending = true;
  bool ok = true;
  ok &= expect(executor.called && executor.saw_ready_scope && executor.raise_succeeded,
               "executor did not run inside an armed signal transaction");
  ok &= expect(executor.saw_fixed_timeout && executor.saw_command && executor.saw_environment,
               "executor did not receive the fixed compiler contract");
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure,
               "Unconfirmed containment was not classified as infrastructure failure");
  ok &= expect(result.exit_code() == 125, "Unconfirmed containment did not map to rc125");
  ok &= expect(result.interrupted_signal == 0,
               "Unconfirmed containment retained SIGTERM for redelivery");
  ok &= expect(caller_signal == 0, "suppressed SIGTERM reached the caller handler");
  ok &= expect(signal_is_pending(SIGTERM, pending) && !pending,
               "suppressed SIGTERM remained pending after transaction cleanup");
  ok &= expect(
    has_diagnostic(result, "NBL-CLI-FS-TOOLCHAIN", "injected compiler leader ownership loss"),
    "toolchain diagnostic omitted the injected containment root cause");
  ok &= expect(has_diagnostic(result, "NBL-CLI-FS-INTERRUPTED", "redelivery remains suppressed"),
               "interruption diagnostic omitted signal suppression");
  for (const fs::path &unpublished :
       {object, fs::path(object.string() + ".nebmeta"), request.generated_source_path}) {
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(unpublished, status_error);
    const bool absent = status_error == std::errc::no_such_file_or_directory ||
                        (!status_error && status.type() == fs::file_type::not_found);
    ok &= expect(absent, "Unconfirmed transaction published an output path");
  }
  ok &= expect(staging_is_absent(root), "Unconfirmed transaction left a staging directory");
  ok &= expect(compiler_lease_is_absent(toolchain_root),
               "Unconfirmed transaction left a compiler execution lease");
  std::string lock_detail;
  ok &= expect(output_lock_is_available(fs::path(object.string() + ".nebula.lock"), lock_detail),
               lock_detail.empty() ? "output lock remained held" : lock_detail);

  FreestandingObjectRequest gap_interruption_request = request;
  gap_interruption_request.object_path = root / "gap-interruption-invalid-extension.bin";
  gap_interruption_request.generated_source_path = root / "gap-interruption.cpp";
  nebula::cli::FreestandingToolchainResolutionResult gap_interruption_resolution =
    nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
  ok &= expect(gap_interruption_resolution.ok(),
               gap_interruption_resolution.error.detail.empty()
                 ? "could not resolve the resolver-to-object signal-gap toolchain"
                 : gap_interruption_resolution.error.detail);
  FreestandingObjectResult gap_interruption_result;
  bool gap_executor_called = true;
  bool gap_raise_succeeded = false;
  if (gap_interruption_resolution.ok()) {
    FixedCompilerExecutor gap_executor(gap_interruption_resolution.value->compiler().executable,
                                       {0, false, {}, 0, CompilerProcessContainment::Confirmed});
    gap_raise_succeeded = ::raise(SIGTERM) == 0;
    gap_interruption_result = build_freestanding_object(
      gap_interruption_request, *gap_interruption_resolution.value, gap_executor);
    gap_executor_called = gap_executor.called;
  }
  ok &=
    expect(gap_raise_succeeded, "could not raise SIGTERM in the resolver-to-object lifecycle gap");
  ok &= expect(!gap_executor_called,
               "resolver-to-object lifecycle gap test unexpectedly invoked the compiler");
  ok &= expect(gap_interruption_result.interrupted_signal == SIGTERM &&
                 gap_interruption_result.exit_code() == 1,
               "resolver-owned signal session did not retain the lifecycle-gap interruption");
  ok &= expect(has_diagnostic(gap_interruption_result, "NBL-CLI-FS-INTERRUPTED",
                              "received termination signal"),
               "resolver-to-object lifecycle gap omitted its interruption diagnostic");
  ok &= expect(!fs::exists(gap_interruption_request.object_path) &&
                 !fs::exists(gap_interruption_request.generated_source_path) &&
                 !fs::exists(fs::path(gap_interruption_request.object_path.string() + ".nebmeta")),
               "resolver-to-object lifecycle gap published output");
  ok &= expect(staging_is_absent(root),
               "resolver-to-object lifecycle gap created freestanding staging state");
  ok &= expect(compiler_lease_is_absent(toolchain_root),
               "resolver-to-object lifecycle gap left a compiler execution lease");
  ok &= expect(caller_signal == 0,
               "resolver-to-object lifecycle-gap SIGTERM escaped before explicit handoff");

  signal_fixture_detail.clear();
  ok &= expect(signal_fixture.restore(signal_fixture_detail), signal_fixture_detail);

  FreestandingObjectRequest precompile_request = request;
  precompile_request.object_path = root / "precompile-invalid-extension.bin";
  precompile_request.generated_source_path = root / "precompile-invalid-extension.cpp";
  nebula::cli::FreestandingToolchainResolutionResult precompile_toolchain_resolution =
    nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
  ok &= expect(precompile_toolchain_resolution.ok(),
               precompile_toolchain_resolution.error.detail.empty()
                 ? "could not resolve the precompile validation test toolchain"
                 : precompile_toolchain_resolution.error.detail);
  FreestandingObjectResult precompile_result;
  bool precompile_executor_called = true;
  if (precompile_toolchain_resolution.ok()) {
    FixedCompilerExecutor precompile_executor(
      precompile_toolchain_resolution.value->compiler().executable,
      {0, false, {}, 0, CompilerProcessContainment::Confirmed});
    precompile_result = build_freestanding_object(
      precompile_request, *precompile_toolchain_resolution.value, precompile_executor);
    precompile_executor_called = precompile_executor.called;
  }
  ok &= expect(!precompile_executor_called,
               "precompile request validation unexpectedly invoked the compiler");
  ok &= expect(precompile_result.failure == FreestandingObjectFailure::Build &&
                 precompile_result.exit_code() == 1,
               "precompile request validation did not retain build-failure semantics");
  ok &= expect(has_diagnostic(precompile_result, "NBL-CLI-FS-OUTPUT", ".o extension"),
               "precompile request validation omitted its output diagnostic");
  ok &= expect(!fs::exists(precompile_request.object_path) &&
                 !fs::exists(precompile_request.generated_source_path),
               "precompile request validation published output");
  ok &= expect(compiler_lease_is_absent(toolchain_root),
               "precompile request validation left a compiler execution lease");

  SignalFixtureGuard cleanup_signal_fixture;
  std::string cleanup_signal_fixture_detail;
  if (!expect(cleanup_signal_fixture.install(cleanup_signal_fixture_detail),
              cleanup_signal_fixture_detail)) {
    return false;
  }
  caller_signal = 0;
  nebula::cli::FreestandingToolchainResolutionResult cleanup_failure_resolution =
    nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
  ok &= expect(cleanup_failure_resolution.ok(),
               cleanup_failure_resolution.error.detail.empty()
                 ? "could not resolve the cleanup-failure validation toolchain"
                 : cleanup_failure_resolution.error.detail);
  if (cleanup_failure_resolution.ok()) {
    const fs::path lease_path = cleanup_failure_resolution.value->compiler_execution_path();
    std::error_code replacement_error;
    fs::remove(lease_path, replacement_error);
    ok &= expect(!replacement_error, "could not unlink the cleanup-failure lease fixture");
    ok &= expect(write_file(lease_path, "replacement must be preserved\n"),
                 "could not create the cleanup-failure replacement fixture");
    ok &= expect(::raise(SIGTERM) == 0,
                 "could not inject SIGTERM before compiler snapshot cleanup failure");

    FreestandingObjectRequest cleanup_failure_request = precompile_request;
    cleanup_failure_request.object_path = root / "precompile-cleanup-failure.bin";
    cleanup_failure_request.generated_source_path = root / "precompile-cleanup-failure.cpp";
    FixedCompilerExecutor cleanup_failure_executor(
      cleanup_failure_resolution.value->compiler().executable,
      {0, false, {}, 0, CompilerProcessContainment::Confirmed});
    const FreestandingObjectResult cleanup_failure_result = build_freestanding_object(
      cleanup_failure_request, *cleanup_failure_resolution.value, cleanup_failure_executor);
    ok &= expect(!cleanup_failure_executor.called,
                 "cleanup-failure precompile validation unexpectedly invoked the compiler");
    ok &= expect(cleanup_failure_result.failure == FreestandingObjectFailure::Infrastructure &&
                   cleanup_failure_result.exit_code() == 125,
                 "precompile lease cleanup failure did not map to infrastructure status 125");
    ok &= expect(cleanup_failure_result.interrupted_signal == 0,
                 "compiler snapshot cleanup failure retained SIGTERM for redelivery");
    ok &= expect(caller_signal == 0,
                 "compiler snapshot cleanup failure redelivered SIGTERM to the caller");
    ok &= expect(has_diagnostic(cleanup_failure_result, "NBL-CLI-FS-CLEANUP",
                                "lease path now names a different object"),
                 "precompile lease cleanup failure omitted its root-cause diagnostic");
    ok &= expect(has_diagnostic(cleanup_failure_result, "NBL-CLI-FS-INTERRUPTED",
                                "redelivery remains suppressed"),
                 "precompile lease cleanup failure omitted its signal-suppression diagnostic");
    ok &= expect(fs::exists(lease_path),
                 "precompile lease cleanup removed a concurrently replaced path");

    replacement_error.clear();
    fs::remove(lease_path, replacement_error);
    ok &= expect(!replacement_error,
                 "could not remove the preserved cleanup-failure replacement fixture");
    const nebula::cli::FreestandingToolchainCloseResult cleanup_retry =
      cleanup_failure_resolution.value->close_session();
    ok &= expect(cleanup_retry.ok() && cleanup_retry.interrupted_signal == 0,
                 cleanup_retry.detail.empty() ? "could not close the recovered toolchain session"
                                              : cleanup_retry.detail);
    ok &= expect(compiler_lease_is_absent(toolchain_root),
                 "cleanup-failure validation left a compiler execution lease");
  }
  cleanup_signal_fixture_detail.clear();
  ok &= expect(cleanup_signal_fixture.restore(cleanup_signal_fixture_detail),
               cleanup_signal_fixture_detail);

  FreestandingObjectRequest invariant_request = request;
  invariant_request.object_path = root / "empty-unconfirmed.o";
  invariant_request.generated_source_path = root / "empty-unconfirmed.freestanding.cpp";
  nebula::cli::FreestandingToolchainResolutionResult invariant_toolchain_resolution =
    nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
  ok &=
    expect(invariant_toolchain_resolution.ok(), invariant_toolchain_resolution.error.detail.empty()
                                                  ? "could not resolve the invariant test toolchain"
                                                  : invariant_toolchain_resolution.error.detail);
  EmptyUnconfirmedCompilerExecutor invariant_executor;
  FreestandingObjectResult invariant_result;
  if (invariant_toolchain_resolution.ok()) {
    invariant_result = build_freestanding_object(
      invariant_request, *invariant_toolchain_resolution.value, invariant_executor);
  }
  ok &= expect(invariant_result.failure == FreestandingObjectFailure::Infrastructure &&
                 invariant_result.exit_code() == 125,
               "empty Unconfirmed result did not fail closed as infrastructure status 125");
  ok &= expect(has_diagnostic(invariant_result, "NBL-CLI-FS-TOOLCHAIN",
                              "Unconfirmed containment without the required root-cause detail"),
               "empty Unconfirmed result omitted its invariant diagnostic");
  ok &= expect(!fs::exists(invariant_request.object_path) &&
                 !fs::exists(invariant_request.generated_source_path) &&
                 !fs::exists(fs::path(invariant_request.object_path.string() + ".nebmeta")),
               "empty Unconfirmed result published output");
  ok &= expect(staging_is_absent(root), "empty Unconfirmed result left a staging directory");
  ok &= expect(compiler_lease_is_absent(toolchain_root),
               "empty Unconfirmed result left a compiler execution lease");
  lock_detail.clear();
  ok &= expect(output_lock_is_available(
                 fs::path(invariant_request.object_path.string() + ".nebula.lock"), lock_detail),
               lock_detail.empty() ? "empty Unconfirmed output lock remained held" : lock_detail);

  const auto run_result_case = [&](std::string_view stem, CommandExecutionResult execution) {
    FreestandingObjectRequest case_request = request;
    case_request.object_path = root / (std::string(stem) + ".o");
    case_request.generated_source_path = root / (std::string(stem) + ".freestanding.cpp");
    nebula::cli::FreestandingToolchainResolutionResult case_toolchain_resolution =
      nebula::cli::resolve_freestanding_toolchain({toolchain_root, self_executable});
    ok &= expect(case_toolchain_resolution.ok(),
                 case_toolchain_resolution.error.detail.empty()
                   ? std::string(stem) + " could not resolve a fresh test toolchain"
                   : case_toolchain_resolution.error.detail);
    const fs::path case_compiler = case_toolchain_resolution.ok()
                                     ? case_toolchain_resolution.value->compiler().executable
                                     : fake_clang;
    FixedCompilerExecutor case_executor(case_compiler, std::move(execution));
    FreestandingObjectResult case_result;
    if (case_toolchain_resolution.ok()) {
      case_result =
        build_freestanding_object(case_request, *case_toolchain_resolution.value, case_executor);
    }
    ok &= expect(case_executor.called && case_executor.contract_valid,
                 std::string(stem) + " executor did not receive the fixed compiler contract");
    ok &= expect(case_executor.saw_command, std::string(stem) + " compiler command changed");
    ok &=
      expect(case_executor.saw_environment, std::string(stem) + " compiler environment changed");
    ok &= expect(case_executor.saw_timeout, std::string(stem) + " compiler timeout changed");
    ok &= expect(case_executor.saw_signal_scope,
                 std::string(stem) + " compiler signal scope was not armed");
    ok &= expect(!fs::exists(case_request.object_path) &&
                   !fs::exists(case_request.generated_source_path) &&
                   !fs::exists(fs::path(case_request.object_path.string() + ".nebmeta")),
                 std::string(stem) + " result published output");
    ok &= expect(staging_is_absent(root), std::string(stem) + " result left staging state");
    ok &= expect(compiler_lease_is_absent(toolchain_root),
                 std::string(stem) + " result left a compiler execution lease");
    std::string case_lock_detail;
    ok &= expect(output_lock_is_available(
                   fs::path(case_request.object_path.string() + ".nebula.lock"), case_lock_detail),
                 case_lock_detail.empty() ? std::string(stem) + " output lock remained held"
                                          : case_lock_detail);
    return case_result;
  };

  const FreestandingObjectResult timeout_result =
    run_result_case("confirmed-timeout", {124, true, {}, 0, CompilerProcessContainment::Confirmed});
  ok &= expect(timeout_result.failure == FreestandingObjectFailure::Timeout &&
                 timeout_result.exit_code() == 124,
               "exact Confirmed timeout did not map to timeout status 124");
  ok &= expect(has_diagnostic(timeout_result, "NBL-CLI-FS-TOOLCHAIN", "exceeded"),
               "exact Confirmed timeout omitted its timeout diagnostic");

  const FreestandingObjectResult zero_timeout_result =
    run_result_case("zero-timeout", {0, true, {}, 0, CompilerProcessContainment::Confirmed});
  ok &= expect(zero_timeout_result.failure == FreestandingObjectFailure::Infrastructure &&
                 zero_timeout_result.exit_code() == 125,
               "exit 0 plus timed_out was not rejected as infrastructure status 125");
  ok &= expect(
    has_diagnostic(zero_timeout_result, "NBL-CLI-FS-TOOLCHAIN", "inconsistent timeout result"),
    "exit 0 plus timed_out omitted its invariant diagnostic");

  const FreestandingObjectResult not_started_result =
    run_result_case("zero-not-started", {0, false, {}, 0, CompilerProcessContainment::NotStarted});
  ok &= expect(not_started_result.failure == FreestandingObjectFailure::Infrastructure &&
                 not_started_result.exit_code() == 125,
               "exit 0 plus NotStarted was not rejected as infrastructure status 125");
  ok &= expect(has_diagnostic(not_started_result, "NBL-CLI-FS-TOOLCHAIN", "NotStarted containment"),
               "exit 0 plus NotStarted omitted its invariant diagnostic");

  const FreestandingObjectResult zero_interrupted_result = run_result_case(
    "zero-interrupted", {0, false, {}, SIGTERM, CompilerProcessContainment::Confirmed});
  ok &= expect(zero_interrupted_result.failure == FreestandingObjectFailure::Infrastructure &&
                 zero_interrupted_result.exit_code() == 125,
               "exit 0 plus interrupted_signal was not rejected as infrastructure status 125");
  ok &= expect(zero_interrupted_result.interrupted_signal == 0,
               "invalid interrupted_signal was retained for caller redelivery");
  ok &= expect(has_diagnostic(zero_interrupted_result, "NBL-CLI-FS-TOOLCHAIN",
                              "inconsistent interruption result"),
               "exit 0 plus interrupted_signal omitted its invariant diagnostic");

  const FreestandingObjectResult forged_interrupted_result =
    run_result_case("forged-interrupted",
                    {128 + SIGTERM, false, {}, SIGTERM, CompilerProcessContainment::Confirmed});
  ok &= expect(forged_interrupted_result.failure == FreestandingObjectFailure::Infrastructure &&
                 forged_interrupted_result.exit_code() == 125,
               "unobserved executor interruption was not rejected as infrastructure status 125");
  ok &= expect(forged_interrupted_result.interrupted_signal == 0,
               "unobserved executor interruption was retained for caller redelivery");
  ok &= expect(has_diagnostic(forged_interrupted_result, "NBL-CLI-FS-TOOLCHAIN",
                              "inconsistent interruption result"),
               "unobserved executor interruption omitted its invariant diagnostic");

  const FreestandingObjectResult compiler_124_result = run_result_case(
    "compiler-exit-124", {124, false, {}, 0, CompilerProcessContainment::Confirmed});
  ok &= expect(compiler_124_result.failure == FreestandingObjectFailure::Build &&
                 compiler_124_result.exit_code() == 1,
               "ordinary compiler exit 124 was misclassified as an infrastructure timeout");
  ok &= expect(has_diagnostic(compiler_124_result, "NBL-CLI-FS-TOOLCHAIN", "nonzero status"),
               "ordinary compiler exit 124 omitted its build-failure diagnostic");

  const std::string noisy_stdout = "stdout-line\n" + std::string(2048U, 'o');
  const std::string noisy_stderr = "stderr-line\r\n" + std::string(2048U, 'e');
  const FreestandingObjectResult noisy_failure_result = run_result_case(
    "compiler-noisy-failure",
    {92, false, {}, 0, CompilerProcessContainment::Confirmed, noisy_stdout, noisy_stderr});
  bool noisy_diagnostic_is_safe_and_bounded = false;
  for (const nebula::frontend::Diagnostic &diagnostic : noisy_failure_result.diagnostics) {
    if (diagnostic.code != "NBL-CLI-FS-TOOLCHAIN")
      continue;
    noisy_diagnostic_is_safe_and_bounded =
      diagnostic.cause.size() < 4096U &&
      diagnostic.cause.find("stdout-line\\n") != std::string::npos &&
      diagnostic.cause.find("stderr-line\\r\\n") != std::string::npos &&
      diagnostic.cause.find("diagnostic truncated after 1024 of 2060 captured bytes") !=
        std::string::npos &&
      diagnostic.cause.find('\n') == std::string::npos &&
      diagnostic.cause.find(std::string(1024U, 'o')) == std::string::npos &&
      diagnostic.cause.find(std::string(1024U, 'e')) == std::string::npos;
  }
  ok &= expect(noisy_diagnostic_is_safe_and_bounded,
               "compiler failure output was not escaped and explicitly diagnostic-bounded");
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 1 || argv == nullptr || argv[0] == nullptr) {
    std::cerr << "freestanding-transaction-test: executable identity is unavailable\n";
    return 1;
  }
  std::error_code error;
  const fs::path self = fs::canonical(argv[0], error);
  if (error) {
    std::cerr << "freestanding-transaction-test: could not canonicalize executable: "
              << error.message() << '\n';
    return 1;
  }
  return run_unconfirmed_transaction_test(self) ? 0 : 1;
}
