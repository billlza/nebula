#include "cli/freestanding_toolchain.hpp"
#include "cli/freestanding_toolchain_test_hooks.hpp"
#include "cli/freestanding_object.hpp"
#include "cli/freestanding_transaction_test_hooks.hpp"
#include "cli/verified_executable_lease_test_hooks.hpp"
#include "boot/protocol_abi_contract.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

volatile sig_atomic_t caller_signal = 0;

fs::path resolver_exception_replacement_path;
bool resolver_exception_original_removed = false;
bool resolver_exception_replacement_written = false;
bool resolver_exception_signal_raised = false;

constexpr std::string_view kResolverReplacementContents =
  "resolver exception replacement must be preserved";

void record_caller_signal(int signal_number) { caller_signal = signal_number; }

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "freestanding-toolchain-signal-test: " << message << '\n';
  return false;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/nebula-freestanding-signal-XXXXXX";
    pattern.push_back('\0');
    if (char *created = ::mkdtemp(pattern.data()); created != nullptr)
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
      std::cerr << "freestanding-toolchain-signal-test: temporary cleanup failed: "
                << error.message() << '\n';
    }
  }

  [[nodiscard]] const std::optional<fs::path> &path() const noexcept { return path_; }

private:
  std::optional<fs::path> path_;
};

class CallerSignalFixture final {
public:
  CallerSignalFixture() = default;
  CallerSignalFixture(const CallerSignalFixture &) = delete;
  CallerSignalFixture &operator=(const CallerSignalFixture &) = delete;

  ~CallerSignalFixture() {
    std::string detail;
    if (!restore(detail))
      std::cerr << "freestanding-toolchain-signal-test: " << detail << '\n';
  }

  bool install(std::string &detail) {
    if (active_) {
      detail = "caller signal fixture was installed twice";
      return false;
    }
    if (sigemptyset(&signal_set_) != 0 || sigaddset(&signal_set_, SIGTERM) != 0) {
      detail = "could not initialize the caller SIGTERM set";
      return false;
    }
    if (::sigprocmask(SIG_BLOCK, &signal_set_, &previous_mask_) != 0) {
      detail = "could not save the caller signal mask: " + std::string(std::strerror(errno));
      return false;
    }
    mask_saved_ = true;

    struct sigaction action{};
    action.sa_handler = record_caller_signal;
    if (sigemptyset(&action.sa_mask) != 0 ||
        ::sigaction(SIGTERM, &action, &previous_action_) != 0) {
      detail = "could not install the caller SIGTERM handler: " + std::string(std::strerror(errno));
      return false;
    }
    handler_installed_ = true;

    sigset_t active_mask = previous_mask_;
    if (sigdelset(&active_mask, SIGTERM) != 0 ||
        ::sigprocmask(SIG_SETMASK, &active_mask, nullptr) != 0) {
      detail = "could not unblock caller SIGTERM: " + std::string(std::strerror(errno));
      return false;
    }
    active_ = true;
    return true;
  }

  bool restore(std::string &detail) {
    if (!mask_saved_ && !handler_installed_)
      return true;
    bool complete = true;
    if (::sigprocmask(SIG_BLOCK, &signal_set_, nullptr) != 0) {
      detail =
        "could not block SIGTERM during fixture cleanup: " + std::string(std::strerror(errno));
      complete = false;
    }
    if (handler_installed_) {
      if (::sigaction(SIGTERM, &previous_action_, nullptr) != 0) {
        if (!detail.empty())
          detail += "; ";
        detail += "could not restore the caller SIGTERM handler: ";
        detail += std::strerror(errno);
        complete = false;
      } else {
        handler_installed_ = false;
      }
    }
    if (mask_saved_) {
      if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) != 0) {
        if (!detail.empty())
          detail += "; ";
        detail += "could not restore the caller signal mask: ";
        detail += std::strerror(errno);
        complete = false;
      } else {
        mask_saved_ = false;
      }
    }
    if (complete)
      active_ = false;
    return complete;
  }

private:
  sigset_t signal_set_{};
  sigset_t previous_mask_{};
  struct sigaction previous_action_{};
  bool mask_saved_ = false;
  bool handler_installed_ = false;
  bool active_ = false;
};

bool write_executable(const fs::path &path) {
  constexpr std::string_view body = "#!/bin/sh\n"
                                    "if [ \"$#\" -eq 1 ] && [ \"$1\" = '--version' ]; then\n"
                                    "  printf '%s\\n' 'Nebula signal-policy clang 1.0'\n"
                                    "  exit 0\n"
                                    "fi\n"
                                    "for argument in \"$@\"; do\n"
                                    "  if [ \"$argument\" = '-dumpmachine' ]; then\n"
                                    "    printf '%s\\n' 'x86_64-unknown-none'\n"
                                    "    exit 0\n"
                                    "  fi\n"
                                    "  if [ \"$argument\" = '-fsyntax-only' ]; then exit 0; fi\n"
                                    "done\n"
                                    "exit 99\n";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(body.data(), static_cast<std::streamsize>(body.size()));
  output.close();
  return !output.fail() && ::chmod(path.c_str(), S_IRWXU) == 0;
}

bool compiler_snapshot_absent(const fs::path &toolchain_root) {
  std::error_code error;
  for (fs::directory_iterator iterator(toolchain_root / "bin", error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->path().filename().string().starts_with(".nebula-exec-"))
      return false;
  }
  return !error;
}

void prepare_resolver_exception_fixture(const fs::path &lease_path) {
  resolver_exception_replacement_path = lease_path;
  std::error_code error;
  resolver_exception_original_removed = fs::remove(lease_path, error) && !error;

  std::ofstream replacement(lease_path, std::ios::binary | std::ios::trunc);
  replacement.write(kResolverReplacementContents.data(),
                    static_cast<std::streamsize>(kResolverReplacementContents.size()));
  replacement.close();
  resolver_exception_replacement_written = !replacement.fail();
  resolver_exception_signal_raised = std::raise(SIGTERM) == 0;
}

nebula::cli::HostProcessResult
interrupted_query_result(nebula::cli::HostProcessContainment containment, int parent_signal,
                         std::string detail) {
  nebula::cli::HostProcessResult result;
  result.started = true;
  result.exited = true;
  result.exit_code = 125U;
  result.parent_interruption_signal = parent_signal;
  result.containment = containment;
  result.infrastructure_error = std::move(detail);
  return result;
}

bool contains(std::string_view text, std::string_view expected) {
  return text.find(expected) != std::string_view::npos;
}

constexpr std::size_t kMinimalSectionTable = 168U;
constexpr std::size_t kMinimalSymbolNamesOffset = kMinimalSectionTable + 5U * 64U;

void write_u16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
  bytes.at(offset) = static_cast<std::uint8_t>(value & 0xffU);
  bytes.at(offset + 1U) = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index)
    bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

void write_u64(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index)
    bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

void write_section(std::vector<std::uint8_t> &bytes, std::size_t index, std::uint32_t name,
                   std::uint32_t type, std::uint64_t flags, std::uint64_t offset,
                   std::uint64_t size, std::uint32_t link, std::uint32_t info,
                   std::uint64_t alignment, std::uint64_t entry_size) {
  const std::size_t base = kMinimalSectionTable + index * 64U;
  write_u32(bytes, base, name);
  write_u32(bytes, base + 4U, type);
  write_u64(bytes, base + 8U, flags);
  write_u64(bytes, base + 24U, offset);
  write_u64(bytes, base + 32U, size);
  write_u32(bytes, base + 40U, link);
  write_u32(bytes, base + 44U, info);
  write_u64(bytes, base + 48U, alignment);
  write_u64(bytes, base + 56U, entry_size);
}

std::vector<std::uint8_t> minimal_valid_object() {
  std::string symbol_names(1U, '\0');
  symbol_names.append(nebula::boot::kUosX86_64PayloadEntrySymbol);
  symbol_names.push_back('\0');
  std::vector<std::uint8_t> bytes(kMinimalSymbolNamesOffset + symbol_names.size(), 0U);
  bytes[0] = 0x7fU;
  bytes[1] = 'E';
  bytes[2] = 'L';
  bytes[3] = 'F';
  bytes[4] = 2U;
  bytes[5] = 1U;
  bytes[6] = 1U;
  write_u16(bytes, 16U, 1U);
  write_u16(bytes, 18U, 62U);
  write_u32(bytes, 20U, 1U);
  write_u64(bytes, 40U, kMinimalSectionTable);
  write_u16(bytes, 52U, 64U);
  write_u16(bytes, 58U, 64U);
  write_u16(bytes, 60U, 5U);
  write_u16(bytes, 62U, 4U);
  bytes[64] = 0xccU;
  write_u32(bytes, 96U, 1U);
  bytes[100U] = 0x12U;
  write_u16(bytes, 102U, 1U);
  write_u64(bytes, 112U, 1U);
  std::copy(symbol_names.begin(), symbol_names.end(), bytes.begin() + kMinimalSymbolNamesOffset);
  constexpr std::string_view kSectionNames("\0.text\0.symtab\0.strtab\0.shstrtab\0", 33U);
  std::copy(kSectionNames.begin(), kSectionNames.end(), bytes.begin() + 128U);
  write_section(bytes, 1U, 1U, 1U, 0x6U, 64U, 1U, 0U, 0U, 16U, 0U);
  write_section(bytes, 2U, 7U, 2U, 0U, 72U, 48U, 3U, 1U, 8U, 24U);
  write_section(bytes, 3U, 15U, 3U, 0U, kMinimalSymbolNamesOffset, symbol_names.size(), 0U, 0U, 1U,
                0U);
  write_section(bytes, 4U, 23U, 3U, 0U, 128U, 33U, 0U, 0U, 1U, 0U);
  return bytes;
}

class ValidObjectCompilerExecutor final : public FreestandingCompilerExecutor {
public:
  explicit ValidObjectCompilerExecutor(int signal_to_raise = 0)
      : signal_to_raise_(signal_to_raise) {}

  CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) override {
    (void)environment;
    (void)timeout_seconds;
    (void)termination_signals;
    const auto output_argument = std::find(command.begin(), command.end(), "-o");
    if (output_argument == command.end() || std::next(output_argument) == command.end()) {
      return {125, false, "test compiler command omitted its output path", 0,
              CompilerProcessContainment::NotStarted};
    }
    const std::vector<std::uint8_t> object = minimal_valid_object();
    std::ofstream output(*std::next(output_argument), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(object.data()),
                 static_cast<std::streamsize>(object.size()));
    output.close();
    if (output.fail()) {
      return {125, false, "test compiler could not write its object", 0,
              CompilerProcessContainment::Confirmed};
    }
    if (signal_to_raise_ != 0)
      signal_raise_succeeded = std::raise(signal_to_raise_) == 0;
    return {0, false, {}, 0, CompilerProcessContainment::Confirmed};
  }

  bool signal_raise_succeeded = false;

private:
  int signal_to_raise_ = 0;
};

FreestandingObjectRequest make_object_request(const fs::path &root, std::string_view stem) {
  FreestandingObjectRequest request;
  request.input_path = root / (std::string(stem) + ".nb");
  request.generated_source_path = root / (std::string(stem) + ".freestanding.cpp");
  request.object_path = root / (std::string(stem) + ".o");
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
  request.build_key.target = nebula::cli::kFreestandingTargetTriple;
  request.build_key.panic_policy = "trap";
  std::ofstream input(request.input_path, std::ios::binary | std::ios::trunc);
  input << "@entry fn main() -> Void {}\n";
  return request;
}

bool object_outputs_absent(const FreestandingObjectRequest &request) {
  return !fs::exists(request.generated_source_path) && !fs::exists(request.object_path) &&
         !fs::exists(fs::path(request.object_path.string() + ".nebmeta"));
}

bool has_result_text(const FreestandingObjectResult &result, std::string_view expected) {
  return std::any_of(
    result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &diagnostic) {
      return contains(diagnostic.message, expected) || contains(diagnostic.cause, expected) ||
             contains(diagnostic.impact, expected);
    });
}

bool has_diagnostic_code(const FreestandingObjectResult &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &diagnostic) { return diagnostic.code == code; });
}

std::optional<fs::path> find_staging_path(const fs::path &root) {
  std::error_code error;
  for (fs::directory_iterator iterator(root, error), end; !error && iterator != end;
       iterator.increment(error)) {
    if (iterator->path().filename().string().starts_with(".nebula-fs-"))
      return iterator->path();
  }
  return std::nullopt;
}

bool staging_path_exists(const fs::path &root) { return find_staging_path(root).has_value(); }

using TransactionPhase = nebula::cli::freestanding_transaction_testing::Phase;
constexpr std::array<TransactionPhase, 8U> kExpectedTransactionPhases = {
  TransactionPhase::OutputLockOpened,     TransactionPhase::StagingDirectoryCreated,
  TransactionPhase::SessionPrepared,      TransactionPhase::StagingCleanupFinished,
  TransactionPhase::PublicationFinalized, TransactionPhase::GuardsDisarmed,
  TransactionPhase::OutputLockReleased,   TransactionPhase::BeforeSessionFinalize,
};
std::size_t observed_phase_count = 0U;
bool phase_order_valid = true;
bool signal_blocked_through_cleanup = true;
bool phase_signal_raise_succeeded = false;
bool phase_output_lock_available = false;
bool phase_staging_absent = false;
bool phase_compiler_lease_absent = false;
bool phase_outputs_absent = false;
fs::path observed_transaction_root;
fs::path observed_toolchain_root;
FreestandingObjectRequest observed_request;
TransactionPhase acquisition_signal_phase = TransactionPhase::OutputLockOpened;
bool acquisition_signal_raise_succeeded = false;

void acquisition_signal_observer(TransactionPhase phase) {
  if (phase == acquisition_signal_phase)
    acquisition_signal_raise_succeeded = std::raise(SIGTERM) == 0;
}

bool acquire_independent_output_lock(const fs::path &path) {
  int flags = O_RDWR;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0)
    return false;
  const bool acquired = ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
  const bool unlocked = !acquired || ::flock(descriptor, LOCK_UN) == 0;
  const bool closed = ::close(descriptor) == 0;
  return acquired && unlocked && closed;
}

void transaction_phase_observer(TransactionPhase phase) {
  if (observed_phase_count >= kExpectedTransactionPhases.size() ||
      phase != kExpectedTransactionPhases[observed_phase_count]) {
    phase_order_valid = false;
  }
  ++observed_phase_count;
  if (caller_signal != 0)
    signal_blocked_through_cleanup = false;
  if (phase == TransactionPhase::SessionPrepared) {
    phase_signal_raise_succeeded = std::raise(SIGTERM) == 0;
    if (caller_signal != 0)
      signal_blocked_through_cleanup = false;
  }
  if (phase == TransactionPhase::OutputLockReleased) {
    phase_output_lock_available = acquire_independent_output_lock(
      fs::path(observed_request.object_path.string() + ".nebula.lock"));
    phase_staging_absent = !staging_path_exists(observed_transaction_root);
    phase_compiler_lease_absent = compiler_snapshot_absent(observed_toolchain_root);
    phase_outputs_absent = object_outputs_absent(observed_request);
    if (caller_signal != 0)
      signal_blocked_through_cleanup = false;
  }
}

bool run_resolution_case(const fs::path &toolchain_root, const fs::path &self,
                         nebula::cli::HostProcessResult query_result, int signal_to_raise,
                         bool restore_failure, int expected_signal,
                         nebula::cli::FreestandingToolchainErrorCode expected_code,
                         std::string_view expected_detail) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;

  caller_signal = 0;
  inject_query_result_once(std::move(query_result), signal_to_raise);
  if (restore_failure)
    inject_signal_restore_failure_once("injected signal restore failure");
  FreestandingToolchainResolutionResult result =
    resolve_freestanding_toolchain({toolchain_root, self});

  bool ok = true;
  ok &= expect(!result.ok(), "injected interrupted query unexpectedly resolved a toolchain");
  ok &= expect(result.interrupted_signal == expected_signal,
               "interrupted query returned the wrong caller signal");
  ok &= expect(result.error.code == expected_code,
               "interrupted query returned the wrong toolchain error code");
  ok &= expect(contains(result.error.detail, expected_detail),
               "interrupted query omitted its primary or suppression detail");
  if (restore_failure) {
    ok &= expect(contains(result.error.detail, "freestanding clang++ version query failed"),
                 "restore failure swallowed the original query error");
    ok &= expect(contains(result.error.detail, "injected signal restore failure"),
                 "restore failure omitted its injected root cause");
    ok &= expect(contains(result.error.detail, "redelivery was suppressed"),
                 "restore failure omitted its redelivery suppression reason");
  }
  ok &=
    expect(caller_signal == 0, "resolver redelivered a caller signal before the dispatch handoff");
  ok &= expect(compiler_snapshot_absent(toolchain_root),
               "interrupted query left a verified compiler snapshot");
  ok &= expect(!query_result_injection_pending() && !signal_restore_failure_injection_pending(),
               "interrupted query did not consume every one-shot test injection");
  return ok;
}

bool run_resolver_exception_cleanup(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;

  caller_signal = 0;
  resolver_exception_replacement_path.clear();
  resolver_exception_original_removed = false;
  resolver_exception_replacement_written = false;
  resolver_exception_signal_raised = false;
  inject_resolver_exception_after_lease_once(prepare_resolver_exception_fixture);

  bool caught_expected_exception = false;
  try {
    const FreestandingToolchainResolutionResult unexpected =
      resolve_freestanding_toolchain({toolchain_root, self});
    (void)unexpected;
  } catch (const std::runtime_error &error) {
    caught_expected_exception = error.what() == kInjectedResolverExceptionDetail;
  } catch (...) {
    caught_expected_exception = false;
  }

  bool ok = true;
  ok &= expect(caught_expected_exception,
               "resolver exception cleanup did not preserve the original exception");
  ok &= expect(resolver_exception_original_removed && resolver_exception_replacement_written &&
                 resolver_exception_signal_raised,
               "resolver exception fixture did not establish replacement and SIGTERM state");
  ok &= expect(caller_signal == 0,
               "resolver exception cleanup handed the captured SIGTERM to the caller");
  ok &= expect(!resolver_exception_injection_pending(),
               "resolver exception injection was not consumed exactly once");

  std::ifstream replacement(resolver_exception_replacement_path, std::ios::binary);
  std::string replacement_contents;
  std::getline(replacement, replacement_contents, '\0');
  ok &= expect(replacement.good() || replacement.eof(),
               "resolver exception replacement could not be read after unwinding");
  ok &= expect(replacement_contents == kResolverReplacementContents,
               "resolver exception cleanup removed or changed the replacement path");
  replacement.close();

  std::error_code error;
  const bool replacement_removed = !resolver_exception_replacement_path.empty() &&
                                   fs::remove(resolver_exception_replacement_path, error) && !error;
  ok &= expect(replacement_removed,
               "resolver exception replacement fixture could not be removed by the test");
  ok &= expect(compiler_snapshot_absent(toolchain_root),
               "resolver exception cleanup left a private compiler snapshot");
  return ok;
}

bool run_acquisition_cleanup_incomplete(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::verified_executable_lease_testing;

  caller_signal = 0;
  resolver_exception_replacement_path.clear();
  resolver_exception_original_removed = false;
  resolver_exception_replacement_written = false;
  resolver_exception_signal_raised = false;
  inject_acquisition_failure_once(prepare_resolver_exception_fixture);

  const FreestandingToolchainResolutionResult result =
    resolve_freestanding_toolchain({toolchain_root, self});

  bool ok = true;
  ok &= expect(!result.ok() && result.error.code == FreestandingToolchainErrorCode::Cleanup &&
                 result.interrupted_signal == 0,
               "incomplete acquisition rollback did not become cleanup rc125 with signal zero");
  ok &= expect(contains(result.error.detail, "rollback-executable-lease") &&
                 contains(result.error.detail, "replacement was preserved") &&
                 contains(result.error.detail, "redelivery was suppressed"),
               "incomplete acquisition rollback omitted cleanup or signal-suppression detail");
  ok &= expect(resolver_exception_original_removed && resolver_exception_replacement_written &&
                 resolver_exception_signal_raised,
               "acquisition rollback fixture did not establish replacement and SIGTERM state");
  ok &= expect(caller_signal == 0,
               "incomplete acquisition rollback handed the captured SIGTERM to the caller");
  ok &= expect(!acquisition_failure_injection_pending(),
               "acquisition failure injection was not consumed exactly once");

  std::ifstream replacement(resolver_exception_replacement_path, std::ios::binary);
  std::string replacement_contents;
  std::getline(replacement, replacement_contents, '\0');
  ok &= expect(replacement.good() || replacement.eof(),
               "acquisition rollback replacement could not be read");
  ok &= expect(replacement_contents == kResolverReplacementContents,
               "acquisition rollback removed or changed the replacement path");
  replacement.close();

  std::error_code error;
  const bool replacement_removed = !resolver_exception_replacement_path.empty() &&
                                   fs::remove(resolver_exception_replacement_path, error) && !error;
  ok &= expect(replacement_removed,
               "acquisition rollback replacement fixture could not be removed by the test");
  ok &= expect(compiler_snapshot_absent(toolchain_root),
               "acquisition rollback left a private compiler-owned snapshot");
  return ok;
}

bool run_active_session_exception_cleanup(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;

  caller_signal = 0;
  resolver_exception_replacement_path.clear();
  resolver_exception_original_removed = false;
  resolver_exception_replacement_written = false;
  resolver_exception_signal_raised = false;
  fs::path lease_path;
  bool caught_expected_exception = false;
  try {
    FreestandingToolchainResolutionResult resolution =
      resolve_freestanding_toolchain({toolchain_root, self});
    if (!expect(resolution.ok(), resolution.error.detail.empty()
                                   ? "could not resolve the exception-cleanup toolchain"
                                   : resolution.error.detail)) {
      return false;
    }
    lease_path = resolution.value->compiler_execution_path();
    prepare_resolver_exception_fixture(lease_path);
    throw std::runtime_error("injected active toolchain exception");
  } catch (const std::runtime_error &error) {
    caught_expected_exception =
      std::string_view(error.what()) == "injected active toolchain exception";
  } catch (...) {
    caught_expected_exception = false;
  }

  bool ok = true;
  ok &= expect(!lease_path.empty() && resolver_exception_replacement_path == lease_path &&
                 resolver_exception_original_removed && resolver_exception_replacement_written &&
                 resolver_exception_signal_raised,
               "active toolchain fixture did not establish replacement and SIGTERM state");
  ok &= expect(caught_expected_exception,
               "active toolchain unwinding did not preserve the original exception");
  ok &= expect(caller_signal == 0,
               "active toolchain unwinding handed the captured SIGTERM to the caller");

  std::ifstream replacement(resolver_exception_replacement_path, std::ios::binary);
  std::string replacement_contents;
  std::getline(replacement, replacement_contents, '\0');
  ok &= expect(replacement.good() || replacement.eof(),
               "active toolchain replacement could not be read after unwinding");
  ok &= expect(replacement_contents == kResolverReplacementContents,
               "active toolchain unwinding removed or changed the replacement path");
  replacement.close();

  std::error_code error;
  const bool replacement_removed = !resolver_exception_replacement_path.empty() &&
                                   fs::remove(resolver_exception_replacement_path, error) && !error;
  ok &= expect(replacement_removed,
               "active toolchain replacement fixture could not be removed by the test");
  ok &= expect(compiler_snapshot_absent(toolchain_root),
               "active toolchain unwinding retained an owned compiler snapshot");
  return ok;
}

bool run_move_construction_lifecycle(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the move-construction toolchain"
                                 : resolution.error.detail)) {
    return false;
  }

  ResolvedFreestandingToolchain moved(std::move(*resolution.value));
  bool ok = true;
  ok &= expect(resolution.value->session_state() == FreestandingToolchainSessionState::Closed &&
                 !resolution.value->session_active() && !resolution.value->session_executable() &&
                 !resolution.value->compiler_snapshot_active(),
               "moved-from toolchain did not become a closed inert object");
  resolution.value.reset();
  ok &= expect(moved.session_state() == FreestandingToolchainSessionState::Executable &&
                 moved.session_active() && moved.session_executable() &&
                 moved.compiler_snapshot_active(),
               "move construction did not preserve the active toolchain session");
  ok &= expect(std::raise(SIGTERM) == 0,
               "could not raise SIGTERM in the move-constructed toolchain session");
  const FreestandingToolchainCloseResult closed = moved.close_session();
  ok &=
    expect(closed.ok() && closed.interrupted_signal == SIGTERM,
           closed.detail.empty() ? "move-constructed toolchain did not return its captured SIGTERM"
                                 : closed.detail);
  ok &= expect(caller_signal == 0,
               "move-constructed toolchain delivered SIGTERM before the dispatch boundary");
  ok &= expect(moved.session_state() == FreestandingToolchainSessionState::Closed &&
                 !moved.session_active() && !moved.compiler_snapshot_active() &&
                 compiler_snapshot_absent(toolchain_root),
               "move-constructed toolchain did not close and retire its compiler snapshot");
  return ok;
}

bool run_explicit_close_restore_failure(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the explicit-close test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  ResolvedFreestandingToolchain &toolchain = *resolution.value;
  bool ok = true;
  ok &=
    expect(std::raise(SIGTERM) == 0, "could not inject SIGTERM into the explicit-close session");
  inject_signal_restore_failure_once("injected explicit-close restore failure");
  const FreestandingToolchainCloseResult failed_close = toolchain.close_session();
  ok &= expect(!failed_close.ok(), "injected explicit close restore failure was ignored");
  ok &= expect(failed_close.interrupted_signal == 0,
               "restore failure returned a signal for caller redelivery");
  ok &= expect(contains(failed_close.detail, "injected explicit-close restore failure") &&
                 contains(failed_close.detail, "redelivery was suppressed"),
               "explicit close failure omitted its root cause or suppression reason");
  ok &= expect(caller_signal == 0,
               "explicit close restore failure redelivered SIGTERM through fallback cleanup");

  const FreestandingToolchainCloseResult retry = toolchain.close_session();
  ok &= expect(retry.ok() && retry.interrupted_signal == 0,
               retry.detail.empty() ? "explicit close recovery did not remain signal-suppressed"
                                    : retry.detail);
  ok &=
    expect(caller_signal == 0, "explicit close recovery redelivered a previously unsafe signal");
  ok &= expect(compiler_snapshot_absent(toolchain_root),
               "explicit close restore failure left a verified compiler snapshot");
  ok &= expect(!query_result_injection_pending() && !signal_restore_failure_injection_pending(),
               "explicit close did not consume every one-shot test injection");
  return ok;
}

bool run_session_state_machine(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the session-state test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  ResolvedFreestandingToolchain &toolchain = *resolution.value;
  bool ok = true;
  ok &= expect(toolchain.session_state() == FreestandingToolchainSessionState::Executable &&
                 toolchain.session_executable(),
               "fresh toolchain session was not executable");

  const FreestandingToolchainCloseResult premature_finalize =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Complete);
  ok &=
    expect(!premature_finalize.ok() && contains(premature_finalize.detail, "must be prepared") &&
             toolchain.session_state() == FreestandingToolchainSessionState::Executable,
           "premature finalization did not fail without mutating the executable state");

  const FreestandingToolchainPrepareResult prepared = toolchain.prepare_session_close();
  ok &= expect(prepared.ok() &&
                 toolchain.session_state() == FreestandingToolchainSessionState::PreparedFrozen &&
                 !toolchain.session_executable() && !toolchain.compiler_snapshot_active(),
               prepared.detail.empty() ? "session preparation did not freeze and retire the lease"
                                       : prepared.detail);
  const FreestandingToolchainPrepareResult prepared_again = toolchain.prepare_session_close();
  ok &= expect(prepared_again.ok() &&
                 toolchain.session_state() == FreestandingToolchainSessionState::PreparedFrozen,
               "session preparation was not idempotent");

  HostProcessRequest forbidden_execution;
  forbidden_execution.arguments = {toolchain.compiler().executable.string(), "--version"};
  const HostProcessResult execution_after_prepare =
    toolchain.execute_compiler(std::move(forbidden_execution));
  ok &= expect(!execution_after_prepare.succeeded() &&
                 contains(execution_after_prepare.infrastructure_error,
                          "unavailable after session close begins"),
               "prepared session still allowed compiler execution through the public API");

  const FreestandingToolchainCloseResult finalized =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Complete);
  ok &= expect(finalized.ok() && finalized.interrupted_signal == 0 &&
                 toolchain.session_state() == FreestandingToolchainSessionState::Closed &&
                 !toolchain.session_active(),
               finalized.detail.empty() ? "prepared session did not finalize" : finalized.detail);
  const FreestandingToolchainCloseResult finalized_again =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Complete);
  const FreestandingToolchainPrepareResult prepared_after_close = toolchain.prepare_session_close();
  const FreestandingToolchainCloseResult closed_again = toolchain.close_session();
  ok &= expect(finalized_again.ok() && finalized_again.interrupted_signal == 0 &&
                 prepared_after_close.ok() && prepared_after_close.observed_signal == 0 &&
                 closed_again.ok() && closed_again.interrupted_signal == 0,
               "closed session operations were not idempotent or attempted a second handoff");
  return ok;
}

bool run_closing_state_retry(const fs::path &toolchain_root, const fs::path &self) {
  using namespace nebula::cli;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the closing-state test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  ResolvedFreestandingToolchain &toolchain = *resolution.value;
  const fs::path lease_path = toolchain.compiler_execution_path();
  std::error_code error;
  fs::remove(lease_path, error);
  if (!expect(!error, "could not unlink the closing-state lease fixture"))
    return false;
  std::ofstream replacement(lease_path, std::ios::binary | std::ios::trunc);
  replacement << "replacement lease path must be preserved\n";
  replacement.close();
  if (!expect(!replacement.fail(), "could not write the closing-state replacement fixture"))
    return false;

  bool ok = true;
  const FreestandingToolchainPrepareResult failed_prepare = toolchain.prepare_session_close();
  ok &= expect(!failed_prepare.ok() &&
                 contains(failed_prepare.detail, "lease path now names a different object") &&
                 toolchain.session_state() == FreestandingToolchainSessionState::Closing &&
                 !toolchain.session_executable(),
               "lease retirement failure did not retain a non-executable Closing state");

  HostProcessRequest forbidden_execution;
  forbidden_execution.arguments = {toolchain.compiler().executable.string(), "--version"};
  const HostProcessResult execution_while_closing =
    toolchain.execute_compiler(std::move(forbidden_execution));
  ok &= expect(contains(execution_while_closing.infrastructure_error,
                        "unavailable after session close begins"),
               "Closing state still allowed compiler execution through the public API");
  const FreestandingToolchainCloseResult premature_finalize =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Complete);
  ok &= expect(!premature_finalize.ok() &&
                 contains(premature_finalize.detail, "preparation did not complete") &&
                 toolchain.session_state() == FreestandingToolchainSessionState::Closing,
               "Closing-state finalization did not fail without restoring caller state");

  error.clear();
  fs::remove(lease_path, error);
  ok &= expect(!error, "could not remove the closing-state replacement fixture");
  const FreestandingToolchainPrepareResult retry_prepare = toolchain.prepare_session_close();
  ok &= expect(retry_prepare.ok() &&
                 toolchain.session_state() == FreestandingToolchainSessionState::PreparedFrozen,
               retry_prepare.detail.empty() ? "Closing-state preparation did not recover on retry"
                                            : retry_prepare.detail);
  const FreestandingToolchainCloseResult finalized =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Complete);
  ok &= expect(finalized.ok() && finalized.interrupted_signal == 0 &&
                 toolchain.session_state() == FreestandingToolchainSessionState::Closed,
               finalized.detail.empty() ? "recovered Closing state did not finalize"
                                        : finalized.detail);
  return ok;
}

bool run_incomplete_external_cleanup_signal_policy(const fs::path &toolchain_root,
                                                   const fs::path &self) {
  using namespace nebula::cli;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the incomplete-cleanup test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  ResolvedFreestandingToolchain &toolchain = *resolution.value;
  bool ok = true;
  ok &=
    expect(std::raise(SIGTERM) == 0, "could not inject SIGTERM before incomplete external cleanup");
  const FreestandingToolchainPrepareResult prepared = toolchain.prepare_session_close();
  ok &= expect(prepared.ok() && prepared.observed_signal == SIGTERM,
               prepared.detail.empty() ? "prepare did not capture the injected SIGTERM"
                                       : prepared.detail);
  const FreestandingToolchainCloseResult finalized =
    toolchain.finalize_session_close(FreestandingExternalCleanup::Incomplete);
  ok &= expect(finalized.ok() && finalized.interrupted_signal == 0 &&
                 toolchain.session_state() == FreestandingToolchainSessionState::Closed &&
                 !toolchain.signal_redelivery_safe(),
               finalized.detail.empty()
                 ? "incomplete external cleanup did not suppress the signal handoff"
                 : finalized.detail);
  ok &= expect(caller_signal == 0,
               "incomplete external cleanup delivered an intercepted signal to the caller");
  return ok;
}

bool run_cleanup_phase_signal_barrier(const fs::path &root, const fs::path &toolchain_root,
                                      const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the cleanup-barrier test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  caller_signal = 0;
  observed_phase_count = 0U;
  phase_order_valid = true;
  signal_blocked_through_cleanup = true;
  phase_signal_raise_succeeded = false;
  phase_output_lock_available = false;
  phase_staging_absent = false;
  phase_compiler_lease_absent = false;
  phase_outputs_absent = false;
  clear_faults();
  inject_fault_once(Fault::SecondPublication);
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectRequest request = make_object_request(root, "phase-barrier");
  observed_transaction_root = root;
  observed_toolchain_root = toolchain_root;
  observed_request = request;
  set_phase_observer(transaction_phase_observer);
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);
  clear_phase_observer();

  bool ok = true;
  ok &= expect(phase_signal_raise_succeeded, "could not raise SIGTERM after session preparation");
  ok &= expect(phase_order_valid && observed_phase_count == kExpectedTransactionPhases.size(),
               "transaction cleanup phases were missing or out of order");
  ok &= expect(signal_blocked_through_cleanup,
               "post-prepare SIGTERM reached the caller before transaction cleanup completed");
  ok &= expect(phase_output_lock_available,
               "output lock was not independently acquirable before signal restoration");
  ok &= expect(phase_staging_absent && phase_compiler_lease_absent && phase_outputs_absent,
               "staging, compiler lease, or rolled-back outputs remained before restoration");
  ok &= expect(caller_signal == SIGTERM,
               "post-prepare SIGTERM was not delivered by the final caller-state restoration");
  ok &= expect(result.failure == FreestandingObjectFailure::Build &&
                 result.artifact_disposition == FreestandingArtifactDisposition::Absent &&
                 object_outputs_absent(request),
               "deterministic rollback barrier did not leave an absent build failure");
  ok &= expect(resolution.value->session_state() == FreestandingToolchainSessionState::Closed,
               "cleanup-barrier transaction did not finalize its signal session");
  ok &= expect(!fault_pending(Fault::SecondPublication),
               "cleanup-barrier transaction did not consume its publication fault");
  clear_faults();
  caller_signal = 0;
  return ok;
}

bool run_committed_restore_failure(const fs::path &root, const fs::path &toolchain_root,
                                   const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the committed-restore test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  const FreestandingObjectRequest request = make_object_request(root, "committed-restore");
  inject_signal_restore_failure_once("injected committed artifact restore failure");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);

  bool ok = true;
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::Committed,
               "committed restore failure did not retain committed rc125 semantics");
  ok &= expect(fs::exists(request.generated_source_path) && fs::exists(request.object_path) &&
                 fs::exists(fs::path(request.object_path.string() + ".nebmeta")),
               "committed restore failure rolled back a complete artifact trio");
  ok &= expect(has_result_text(result, "complete artifact trio remains committed") &&
                 has_result_text(result, "injected committed artifact restore failure"),
               "committed restore failure diagnostic misstated artifact disposition or root cause");
  ok &=
    expect(resolution.value->session_state() == FreestandingToolchainSessionState::PreparedFrozen,
           "restore failure did not retain the prepared-frozen retry state");
  const FreestandingToolchainCloseResult retry = resolution.value->close_session();
  ok &= expect(retry.ok() && retry.interrupted_signal == 0 &&
                 resolution.value->session_state() == FreestandingToolchainSessionState::Closed,
               retry.detail.empty() ? "committed restore failure did not recover on explicit retry"
                                    : retry.detail);
  ok &= expect(!signal_restore_failure_injection_pending(),
               "committed restore test did not consume its restore fault");
  return ok;
}

bool run_rolled_back_restore_failure(const fs::path &root, const fs::path &toolchain_root,
                                     const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;
  using namespace nebula::cli::freestanding_transaction_testing;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the rolled-back restore test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  clear_faults();
  inject_fault_once(Fault::SecondPublication);
  inject_signal_restore_failure_once("injected rolled-back artifact restore failure");
  const FreestandingObjectRequest request = make_object_request(root, "rolled-back-restore");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);

  bool ok = true;
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::Absent &&
                 object_outputs_absent(request),
               "rolled-back restore failure did not retain absent rc125 semantics");
  ok &= expect(has_result_text(result, "publication was rolled back") &&
                 has_result_text(result, "injected rolled-back artifact restore failure"),
               "rolled-back restore failure diagnostic misstated disposition or root cause");
  const FreestandingToolchainCloseResult retry = resolution.value->close_session();
  ok &= expect(retry.ok() && retry.interrupted_signal == 0,
               retry.detail.empty() ? "rolled-back restore failure did not recover on retry"
                                    : retry.detail);
  ok &=
    expect(!fault_pending(Fault::SecondPublication) && !signal_restore_failure_injection_pending(),
           "rolled-back restore test left a one-shot fault pending");
  clear_faults();
  return ok;
}

bool run_transient_snapshot_cleanup_failure(const fs::path &root, const fs::path &toolchain_root,
                                            const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_toolchain_testing;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the transient-cleanup test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  inject_compiler_snapshot_cleanup_failure_once(
    "injected transient compiler snapshot cleanup failure");
  const FreestandingObjectRequest request = make_object_request(root, "transient-cleanup");
  ValidObjectCompilerExecutor executor(SIGTERM);
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);

  bool ok = true;
  ok &= expect(executor.signal_raise_succeeded,
               "test compiler did not raise SIGTERM before snapshot retirement");
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 && result.interrupted_signal == 0 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::Absent,
               "transient snapshot cleanup failure did not retain rc125/signal0/Absent semantics");
  ok &=
    expect(caller_signal == 0, "transient snapshot cleanup failure handed SIGTERM to the caller");
  ok &= expect(resolution.value->session_state() == FreestandingToolchainSessionState::Closed &&
                 !resolution.value->compiler_snapshot_active(),
               "prepare retry did not close the session and retire the compiler lease");
  ok &= expect(compiler_snapshot_absent(toolchain_root) && !staging_path_exists(root) &&
                 object_outputs_absent(request),
               "transient snapshot cleanup failure left lease, staging, or output state");
  ok &= expect(has_result_text(result, "injected transient compiler snapshot cleanup failure") &&
                 has_result_text(result, "first explicit cleanup attempt"),
               "transient cleanup diagnostics omitted the root cause or suppression reason");
  ok &= expect(!resolution.value->signal_redelivery_safe(),
               "transient cleanup failure did not monotonically disable signal handoff");
  ok &= expect(!compiler_snapshot_cleanup_failure_injection_pending(),
               "transient compiler snapshot cleanup fault was not consumed exactly once");
  return ok;
}

bool run_staging_acquisition_cleanup_failure(const fs::path &root, const fs::path &toolchain_root,
                                             const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the staging-acquisition test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  clear_faults();
  inject_fault_once(Fault::StagingPermissionsRollbackCleanup);
  acquisition_signal_phase = TransactionPhase::StagingDirectoryCreated;
  acquisition_signal_raise_succeeded = false;
  set_phase_observer(acquisition_signal_observer);
  const FreestandingObjectRequest request = make_object_request(root, "staging-acquisition");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);
  clear_phase_observer();

  const std::optional<fs::path> retained_staging = find_staging_path(root);
  bool ok = true;
  ok &= expect(acquisition_signal_raise_succeeded,
               "could not raise SIGTERM after the staging directory was created");
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 && result.interrupted_signal == 0 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::CleanupIncomplete,
               "staging acquisition rollback failure did not retain rc125/signal0/"
               "CleanupIncomplete semantics");
  ok &=
    expect(caller_signal == 0, "staging acquisition rollback failure handed SIGTERM to the caller");
  ok &= expect(retained_staging.has_value() && object_outputs_absent(request),
               "staging acquisition guard retried cleanup or an output was published");
  ok &= expect(has_diagnostic_code(result, "NBL-CLI-FS-CLEANUP") &&
                 has_result_text(result, "injected staging permissions failure") &&
                 has_result_text(result, "device") && has_result_text(result, "inode"),
               "staging acquisition diagnostic omitted cleanup classification, identity, or "
               "root cause");
  ok &= expect(!fault_pending(Fault::StagingPermissionsRollbackCleanup),
               "staging acquisition cleanup fault was not consumed exactly once");
  ok &= expect(!resolution.value->signal_redelivery_safe() &&
                 resolution.value->session_state() == FreestandingToolchainSessionState::Closed,
               "staging acquisition cleanup failure did not monotonically close the signal "
               "session as unsafe");

  if (retained_staging.has_value()) {
    std::error_code cleanup_error;
    fs::remove_all(*retained_staging, cleanup_error);
    ok &= expect(!cleanup_error, cleanup_error.message());
  }
  clear_faults();
  caller_signal = 0;
  return ok;
}

bool run_output_lock_acquisition_cleanup_failure(const fs::path &root,
                                                 const fs::path &toolchain_root,
                                                 const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  caller_signal = 0;
  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the lock-acquisition test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  clear_faults();
  inject_fault_once(Fault::OutputLockAcquireRollbackClose);
  acquisition_signal_phase = TransactionPhase::OutputLockOpened;
  acquisition_signal_raise_succeeded = false;
  set_phase_observer(acquisition_signal_observer);
  const FreestandingObjectRequest request = make_object_request(root, "lock-acquisition");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);
  clear_phase_observer();

  const int injected_descriptor = last_injected_output_lock_descriptor();
  errno = 0;
  const int descriptor_status = ::fcntl(injected_descriptor, F_GETFD);
  const int descriptor_error = errno;
  bool ok = true;
  ok &= expect(acquisition_signal_raise_succeeded,
               "could not raise SIGTERM after the output lock descriptor was opened");
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 && result.interrupted_signal == 0 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::CleanupIncomplete,
               "lock acquisition rollback failure did not retain rc125/signal0/"
               "CleanupIncomplete semantics");
  ok &=
    expect(caller_signal == 0, "lock acquisition rollback failure handed SIGTERM to the caller");
  ok &= expect(injected_descriptor >= 0 && descriptor_status == -1 && descriptor_error == EBADF,
               "the retained acquisition descriptor was not closed before the transaction "
               "returned");
  ok &= expect(object_outputs_absent(request) && !has_diagnostic_code(result, "NBL-CLI-FS-BUSY"),
               "lock acquisition cleanup failure was misreported as Busy or published output");
  ok &= expect(has_diagnostic_code(result, "NBL-CLI-FS-CLEANUP") &&
                 has_result_text(result, "injected output lock inspection failure") &&
                 has_result_text(result, "device") && has_result_text(result, "inode"),
               "lock acquisition diagnostic omitted cleanup classification, identity, or root "
               "cause");
  ok &= expect(!fault_pending(Fault::OutputLockAcquireRollbackClose),
               "lock acquisition cleanup fault was not consumed exactly once");
  ok &= expect(!resolution.value->signal_redelivery_safe() &&
                 resolution.value->session_state() == FreestandingToolchainSessionState::Closed,
               "lock acquisition cleanup failure did not monotonically close the signal session "
               "as unsafe");
  clear_faults();
  caller_signal = 0;
  return ok;
}

bool run_staging_ownership_transfer_exception(const fs::path &root, const fs::path &toolchain_root,
                                              const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the ownership-transfer test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  clear_faults();
  inject_fault_once(Fault::BeforeStagingOwnershipTransfer);
  const FreestandingObjectRequest request = make_object_request(root, "ownership-transfer");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);

  bool ok = true;
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.exit_code() == 125 &&
                 result.artifact_disposition == FreestandingArtifactDisposition::Absent,
               "ownership-transfer exception did not become a clean rc125 infrastructure "
               "failure");
  ok &= expect(!staging_path_exists(root) && object_outputs_absent(request),
               "ownership-transfer exception leaked staging or output state");
  ok &= expect(has_result_text(result, "injected failure before staging ownership transfer"),
               "ownership-transfer exception diagnostic omitted its root cause");
  ok &= expect(!fault_pending(Fault::BeforeStagingOwnershipTransfer),
               "ownership-transfer fault was not consumed exactly once");
  clear_faults();
  return ok;
}

bool run_guard_disarm_failures(const fs::path &root, const fs::path &toolchain_root,
                               const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  bool ok = true;
  clear_faults();
  FreestandingToolchainResolutionResult staging_resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(staging_resolution.ok(), staging_resolution.error.detail.empty()
                                         ? "could not resolve the staging-guard test toolchain"
                                         : staging_resolution.error.detail)) {
    return false;
  }
  inject_fault_once(Fault::StagingCleanup);
  inject_fault_once(Fault::PostCleanupDiagnostic);
  const FreestandingObjectRequest staging_request = make_object_request(root, "staging-guard");
  ValidObjectCompilerExecutor staging_executor;
  const FreestandingObjectResult staging_result =
    build_freestanding_object(staging_request, *staging_resolution.value, staging_executor);
  ok &= expect(staging_result.failure == FreestandingObjectFailure::Infrastructure &&
                 staging_result.artifact_disposition ==
                   FreestandingArtifactDisposition::CleanupIncomplete,
               "staging cleanup failure did not become cleanup-incomplete rc125");
  ok &= expect(staging_path_exists(root),
               "staging guard silently retried an explicitly failed cleanup attempt");
  ok &= expect(object_outputs_absent(staging_request),
               "staging cleanup failure retained published artifact paths");
  ok &= expect(!fault_pending(Fault::StagingCleanup),
               "staging cleanup fault was not consumed exactly once");
  ok &=
    expect(!fault_pending(Fault::PostCleanupDiagnostic) &&
             has_result_text(staging_result, "injected post-cleanup diagnostic rendering failure"),
           "post-staging-cleanup diagnostic throw was not contained after guard disarm");

  FreestandingToolchainResolutionResult rollback_resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(rollback_resolution.ok(), rollback_resolution.error.detail.empty()
                                          ? "could not resolve the rollback-guard test toolchain"
                                          : rollback_resolution.error.detail)) {
    return false;
  }
  inject_fault_once(Fault::SecondPublication);
  inject_fault_once(Fault::PublicationRollback);
  inject_fault_once(Fault::PostCleanupDiagnostic);
  const FreestandingObjectRequest rollback_request = make_object_request(root, "rollback-guard");
  ValidObjectCompilerExecutor rollback_executor;
  const FreestandingObjectResult rollback_result =
    build_freestanding_object(rollback_request, *rollback_resolution.value, rollback_executor);
  ok &= expect(rollback_result.failure == FreestandingObjectFailure::Infrastructure &&
                 rollback_result.artifact_disposition ==
                   FreestandingArtifactDisposition::CleanupIncomplete,
               "rollback cleanup failure did not become cleanup-incomplete rc125");
  ok &= expect(fs::exists(rollback_request.generated_source_path) &&
                 !fs::exists(rollback_request.object_path) &&
                 !fs::exists(fs::path(rollback_request.object_path.string() + ".nebmeta")),
               "rollback guard retried and removed the explicitly preserved published path");
  ok &=
    expect(!fault_pending(Fault::SecondPublication) && !fault_pending(Fault::PublicationRollback),
           "rollback cleanup faults were not consumed exactly once");
  ok &=
    expect(!fault_pending(Fault::PostCleanupDiagnostic) &&
             has_result_text(rollback_result, "injected post-cleanup diagnostic rendering failure"),
           "post-publication-cleanup diagnostic throw was not contained after guard disarm");
  clear_faults();
  return ok;
}

bool run_output_lock_disposition_failure(const fs::path &root, const fs::path &toolchain_root,
                                         const fs::path &self) {
  using namespace nebula::cli;
  using namespace nebula::cli::freestanding_transaction_testing;

  FreestandingToolchainResolutionResult resolution =
    resolve_freestanding_toolchain({toolchain_root, self});
  if (!expect(resolution.ok(), resolution.error.detail.empty()
                                 ? "could not resolve the lock-disposition test toolchain"
                                 : resolution.error.detail)) {
    return false;
  }
  clear_faults();
  inject_fault_once(Fault::OutputLockRelease);
  const FreestandingObjectRequest request = make_object_request(root, "lock-disposition");
  ValidObjectCompilerExecutor executor;
  const FreestandingObjectResult result =
    build_freestanding_object(request, *resolution.value, executor);

  bool ok = true;
  ok &= expect(result.failure == FreestandingObjectFailure::Infrastructure &&
                 result.artifact_disposition == FreestandingArtifactDisposition::CleanupIncomplete,
               "unconfirmed lock release did not become cleanup-incomplete rc125");
  ok &= expect(fs::exists(request.generated_source_path) && fs::exists(request.object_path) &&
                 fs::exists(fs::path(request.object_path.string() + ".nebmeta")),
               "lock confirmation failure unexpectedly removed the published artifact trio");
  ok &= expect(has_result_text(result, "output lock release was not confirmed"),
               "lock confirmation failure omitted its artifact-state diagnostic");
  ok &= expect(!fault_pending(Fault::OutputLockRelease),
               "output lock confirmation fault was not consumed exactly once");
  clear_faults();
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 1 || argv == nullptr || argv[0] == nullptr) {
    std::cerr << "freestanding-toolchain-signal-test: executable identity is unavailable\n";
    return 1;
  }

  TemporaryDirectory temporary;
  if (!expect(temporary.path().has_value(), "could not create a temporary directory"))
    return 1;
  const fs::path toolchain_root = *temporary.path() / "toolchain";
  std::error_code error;
  fs::create_directories(toolchain_root / "bin", error);
  if (!expect(!error, "could not create the fake toolchain directory") ||
      !expect(write_executable(toolchain_root / "bin" / "clang++"),
              "could not create the fake compiler")) {
    return 1;
  }
  const fs::path self = fs::canonical(argv[0], error);
  if (!expect(!error, "could not canonicalize the test executable"))
    return 1;

  CallerSignalFixture signal_fixture;
  std::string fixture_detail;
  if (!expect(signal_fixture.install(fixture_detail), fixture_detail))
    return 1;

  using nebula::cli::FreestandingToolchainErrorCode;
  using nebula::cli::HostProcessContainment;
  bool ok = true;
  ok &= run_resolution_case(
    toolchain_root, self,
    interrupted_query_result(HostProcessContainment::Unconfirmed, SIGTERM,
                             "injected unconfirmed query cleanup"),
    SIGTERM, false, 0, FreestandingToolchainErrorCode::Query,
    "redelivery is disabled because the compiler query containment cleanup was not confirmed");
  ok &= run_resolution_case(
    toolchain_root, self,
    interrupted_query_result(HostProcessContainment::Unconfirmed, 0,
                             "injected unconfirmed query before signal gap"),
    SIGTERM, false, 0, FreestandingToolchainErrorCode::Query,
    "redelivery is disabled because the compiler query containment cleanup was not confirmed");
  ok &= run_resolution_case(
    toolchain_root, self, interrupted_query_result(HostProcessContainment::Confirmed, SIGTERM, {}),
    SIGTERM, false, SIGTERM, FreestandingToolchainErrorCode::Interrupted,
    "freestanding toolchain resolution was interrupted by signal");
  ok &= run_resolution_case(
    toolchain_root, self, interrupted_query_result(HostProcessContainment::Confirmed, SIGTERM, {}),
    SIGTERM, true, 0, FreestandingToolchainErrorCode::SignalBoundary,
    "original caller termination signal redelivery was suppressed");
  ok &= run_acquisition_cleanup_incomplete(toolchain_root, self);
  ok &= run_resolver_exception_cleanup(toolchain_root, self);
  ok &= run_active_session_exception_cleanup(toolchain_root, self);
  ok &= run_move_construction_lifecycle(toolchain_root, self);
  ok &= run_session_state_machine(toolchain_root, self);
  ok &= run_closing_state_retry(toolchain_root, self);
  ok &= run_incomplete_external_cleanup_signal_policy(toolchain_root, self);
  ok &= run_explicit_close_restore_failure(toolchain_root, self);
  ok &= run_cleanup_phase_signal_barrier(*temporary.path(), toolchain_root, self);
  ok &= run_committed_restore_failure(*temporary.path(), toolchain_root, self);
  ok &= run_rolled_back_restore_failure(*temporary.path(), toolchain_root, self);
  ok &= run_transient_snapshot_cleanup_failure(*temporary.path(), toolchain_root, self);
  ok &= run_staging_acquisition_cleanup_failure(*temporary.path(), toolchain_root, self);
  ok &= run_output_lock_acquisition_cleanup_failure(*temporary.path(), toolchain_root, self);
  ok &= run_staging_ownership_transfer_exception(*temporary.path(), toolchain_root, self);
  ok &= run_output_lock_disposition_failure(*temporary.path(), toolchain_root, self);
  ok &= run_guard_disarm_failures(*temporary.path(), toolchain_root, self);

  fixture_detail.clear();
  ok &= expect(signal_fixture.restore(fixture_detail), fixture_detail);
  if (!ok)
    return 1;
  std::cout << "freestanding-toolchain-signal-ok\n";
  return 0;
}
