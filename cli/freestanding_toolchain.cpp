#include "freestanding_toolchain.hpp"

#include "artifact_digest.hpp"
#include "host_process.hpp"
#include "log_value.hpp"
#include "path_security.hpp"
#include "termination_signal.hpp"

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
#include "freestanding_toolchain_test_hooks.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <csignal>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::cli {

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
namespace freestanding_toolchain_testing {
namespace {
struct InjectedQuery {
  HostProcessResult result;
  int signal_to_raise = 0;
};

std::optional<InjectedQuery> injected_query;
std::optional<std::string> injected_compiler_snapshot_cleanup_failure;
std::optional<ResolverExceptionSetup> injected_resolver_exception_setup;
} // namespace

void inject_query_result_once(HostProcessResult result, int signal_to_raise) {
  injected_query = InjectedQuery{std::move(result), signal_to_raise};
}

bool query_result_injection_pending() noexcept { return injected_query.has_value(); }

std::optional<InjectedQuery> take_injected_query() {
  std::optional<InjectedQuery> result = std::move(injected_query);
  injected_query.reset();
  return result;
}

void inject_compiler_snapshot_cleanup_failure_once(std::string detail) {
  injected_compiler_snapshot_cleanup_failure = std::move(detail);
}

bool compiler_snapshot_cleanup_failure_injection_pending() noexcept {
  return injected_compiler_snapshot_cleanup_failure.has_value();
}

std::optional<std::string> take_injected_compiler_snapshot_cleanup_failure() {
  std::optional<std::string> result = std::move(injected_compiler_snapshot_cleanup_failure);
  injected_compiler_snapshot_cleanup_failure.reset();
  return result;
}

void inject_resolver_exception_after_lease_once(ResolverExceptionSetup setup) {
  injected_resolver_exception_setup = setup;
}

bool resolver_exception_injection_pending() noexcept {
  return injected_resolver_exception_setup.has_value();
}

ResolverExceptionSetup take_injected_resolver_exception_setup() noexcept {
  const ResolverExceptionSetup result = injected_resolver_exception_setup.value_or(nullptr);
  injected_resolver_exception_setup.reset();
  return result;
}
} // namespace freestanding_toolchain_testing
#endif

namespace {

constexpr std::size_t kQueryStreamLimitBytes = 64U * 1024U;
constexpr std::uint32_t kQueryTimeoutMilliseconds = 5000U;

void append_detail(std::string &detail, std::string_view additional) {
  if (additional.empty())
    return;
  if (!detail.empty())
    detail += "; ";
  detail += additional;
}

FreestandingToolchainResolutionResult failure(FreestandingToolchainErrorCode code,
                                              std::string detail) {
  FreestandingToolchainResolutionResult result;
  result.error = {code, std::move(detail)};
  return result;
}

std::string trim_ascii(std::string value) {
  const auto is_space = [](unsigned char byte) {
    return byte == static_cast<unsigned char>(' ') || byte == static_cast<unsigned char>('\t') ||
           byte == static_cast<unsigned char>('\r') || byte == static_cast<unsigned char>('\n');
  };
  std::size_t first = 0;
  while (first < value.size() && is_space(static_cast<unsigned char>(value[first])))
    ++first;
  std::size_t last = value.size();
  while (last > first && is_space(static_cast<unsigned char>(value[last - 1])))
    --last;
  return value.substr(first, last - first);
}

void append_length_delimited(std::ostringstream &output, std::string_view key,
                             std::string_view value) {
  output << key << "_size=" << value.size() << '\n';
  output << key << '=' << value << '\n';
}

bool revalidate_tool(const ResolvedToolIdentity &expected, std::string_view role,
                     std::string &detail) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(expected.executable, error);
  if (error || canonical != expected.executable) {
    detail = error ? "could not canonicalize " + std::string(role) +
                       " during revalidation: " + error.message()
                   : std::string(role) + " canonical path changed during revalidation";
    return false;
  }
#if !defined(_WIN32)
  if (!validate_owner_controlled_executable(canonical, detail)) {
    detail = std::string(role) + " left the owner-controlled boundary: " + detail;
    return false;
  }
#endif
  const FileDigestResult digest = sha256_file(canonical);
  if (!digest.ok()) {
    detail = "could not re-hash " + std::string(role) + ": " + digest.detail;
    return false;
  }
  if (digest.value->size != expected.size || digest.value->sha256 != expected.sha256) {
    detail = std::string(role) + " content identity changed after resolution";
    return false;
  }
  return true;
}

struct QueryResult {
  std::optional<std::string> output;
  std::string detail;
  bool signal_redelivery_safe = true;
};

class ResolverExceptionSignalGuard final {
public:
  explicit ResolverExceptionSignalGuard(CompilerTerminationSignalScope &signals) noexcept
      : signals_(&signals) {}
  ResolverExceptionSignalGuard(const ResolverExceptionSignalGuard &) = delete;
  ResolverExceptionSignalGuard &operator=(const ResolverExceptionSignalGuard &) = delete;
  ~ResolverExceptionSignalGuard() noexcept {
    if (active_)
      signals_->suppress_emergency_redelivery();
  }

  void disarm() noexcept { active_ = false; }

private:
  CompilerTerminationSignalScope *signals_;
  bool active_ = true;
};

void append_redelivery_suppressed(std::string &detail, std::string_view reason) {
  append_detail(detail, "original caller termination signal redelivery was suppressed because " +
                          std::string(reason));
}

void append_redelivery_disabled(std::string &detail, std::string_view reason) {
  append_detail(detail,
                "caller termination signal redelivery is disabled because " + std::string(reason));
}

QueryResult run_query(std::vector<std::string> arguments,
                      const VerifiedExecutableLease &compiler_lease, std::string_view purpose,
                      bool allow_empty_output, CompilerTerminationSignalScope &signals) {
  HostProcessRequest request;
  request.arguments = std::move(arguments);
  request.inherit_environment = false;
  request.environment_overrides = {
    {"LANG", "C"},
    {"LC_ALL", "C"},
    {"TZ", "UTC"},
  };
  request.stdin_mode = HostProcessInputMode::Discard;
  request.stdout_mode = HostProcessStreamMode::Capture;
  request.stderr_mode = HostProcessStreamMode::Capture;
  request.max_stdout_bytes = kQueryStreamLimitBytes;
  request.max_stderr_bytes = kQueryStreamLimitBytes;
  request.timeout_milliseconds = kQueryTimeoutMilliseconds;
  request.termination_signals = &signals;
  HostProcessResult process;
#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
  if (auto injected = freestanding_toolchain_testing::take_injected_query(); injected.has_value()) {
    process = std::move(injected->result);
    if (injected->signal_to_raise != 0 && std::raise(injected->signal_to_raise) != 0) {
      append_detail(process.infrastructure_error,
                    "test injection could not raise the requested caller signal");
    }
  } else {
    process = compiler_lease.execute_request(std::move(request));
  }
#else
  process = compiler_lease.execute_request(std::move(request));
#endif
  QueryResult result;
  if (process.containment == HostProcessContainment::Unconfirmed ||
      (process.parent_interruption_signal != 0 &&
       process.containment != HostProcessContainment::Confirmed)) {
    result.signal_redelivery_safe = false;
    signals.suppress_emergency_redelivery();
  }
  if (!process.succeeded()) {
    result.detail = std::string(purpose) + " failed";
    if (process.timed_out) {
      result.detail += process.containment == HostProcessContainment::Confirmed
                         ? " after its bounded timeout and confirmed cleanup"
                         : " after timeout without confirmed containment cleanup";
    } else if (!process.infrastructure_error.empty()) {
      result.detail += ": " + process.infrastructure_error;
    } else if (process.termination_signal != 0) {
      result.detail += " with signal " + std::to_string(process.termination_signal);
    } else if (process.exited) {
      result.detail += " with exit status " + std::to_string(process.exit_code);
    } else {
      result.detail += " before a complete exit status was available";
    }
    if (!result.signal_redelivery_safe) {
      append_redelivery_disabled(result.detail,
                                 "the compiler query containment cleanup was not confirmed");
    }
    return result;
  }
  result.output = trim_ascii(process.stdout_data + process.stderr_data);
  if (!allow_empty_output && result.output->empty()) {
    result.output.reset();
    result.detail = std::string(purpose) + " produced no bounded identity output";
  }
  return result;
}

bool canonicalize_root(const std::filesystem::path &root, std::filesystem::path &canonical,
                       std::string &detail) {
  if (root.empty() || !root.is_absolute()) {
    detail = "freestanding toolchain root must be one explicit absolute path";
    return false;
  }
  std::error_code error;
  canonical = std::filesystem::canonical(root, error);
  if (error) {
    detail = "could not canonicalize freestanding toolchain root: " + error.message();
    return false;
  }
  const std::filesystem::file_status status = std::filesystem::symlink_status(canonical, error);
  if (error || !std::filesystem::is_directory(status)) {
    detail = error ? "could not inspect freestanding toolchain root: " + error.message()
                   : "freestanding toolchain root is not a directory";
    return false;
  }
#if !defined(_WIN32)
  if (!validate_owner_controlled_directory_chain(canonical, detail)) {
    detail = "freestanding toolchain root is outside the owner-controlled boundary: " + detail;
    return false;
  }
#endif
  return true;
}

bool resolve_identity(const std::filesystem::path &path, std::string_view role,
                      bool require_executable, ResolvedToolIdentity &identity,
                      std::string &detail) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(path, error);
  if (error) {
    detail = "could not canonicalize " + std::string(role) + ": " + error.message();
    return false;
  }
  const std::filesystem::file_status status = std::filesystem::symlink_status(canonical, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    detail = error ? "could not inspect " + std::string(role) + ": " + error.message()
                   : std::string(role) + " is not a regular file";
    return false;
  }
#if !defined(_WIN32)
  if (require_executable && !validate_owner_controlled_executable(canonical, detail)) {
    detail = std::string(role) + " is outside the owner-controlled boundary: " + detail;
    return false;
  }
#else
  (void)require_executable;
#endif
  const FileDigestResult digest = sha256_file(canonical);
  if (!digest.ok()) {
    detail = "could not hash " + std::string(role) + ": " + digest.detail;
    return false;
  }
  identity.executable = canonical;
  identity.size = digest.value->size;
  identity.sha256 = digest.value->sha256;
  return true;
}

} // namespace

ResolvedFreestandingToolchain::ResolvedFreestandingToolchain(
  std::filesystem::path root, ResolvedToolIdentity compiler, ResolvedToolIdentity nebula_executable,
  std::string target_triple, bool signal_redelivery_safe,
  std::unique_ptr<CompilerTerminationSignalScope> termination_signals,
  std::unique_ptr<VerifiedExecutableLease> compiler_lease) noexcept
    : root_(std::move(root)), compiler_(std::move(compiler)),
      nebula_executable_(std::move(nebula_executable)), target_triple_(std::move(target_triple)),
      termination_signals_(std::move(termination_signals)),
      compiler_lease_(std::move(compiler_lease)), signal_redelivery_safe_(signal_redelivery_safe) {}

ResolvedFreestandingToolchain::ResolvedFreestandingToolchain(
  ResolvedFreestandingToolchain &&other) noexcept
    : root_(std::move(other.root_)), compiler_(std::move(other.compiler_)),
      nebula_executable_(std::move(other.nebula_executable_)),
      target_triple_(std::move(other.target_triple_)),
      termination_signals_(std::move(other.termination_signals_)),
      compiler_lease_(std::move(other.compiler_lease_)),
      signal_redelivery_safe_(other.signal_redelivery_safe_), session_state_(other.session_state_) {
  other.signal_redelivery_safe_ = false;
  other.session_state_ = FreestandingToolchainSessionState::Closed;
}

ResolvedFreestandingToolchain::~ResolvedFreestandingToolchain() noexcept {
  if (termination_signals_ != nullptr &&
      (session_state_ != FreestandingToolchainSessionState::Closed || compiler_snapshot_active())) {
    mark_signal_redelivery_unsafe();
  }
}

void ResolvedFreestandingToolchain::mark_signal_redelivery_unsafe() noexcept {
  signal_redelivery_safe_ = false;
  termination_signals_->suppress_emergency_redelivery();
}

HostProcessResult
ResolvedFreestandingToolchain::execute_compiler(HostProcessRequest request) const {
  HostProcessResult failure;
  if (!session_executable()) {
    failure.infrastructure_error =
      "resolved freestanding compiler execution is unavailable after session close begins";
    return failure;
  }
  if (request.arguments.empty() || request.arguments.front() != compiler_.executable.string()) {
    failure.infrastructure_error =
      "resolved freestanding compiler execution requires its canonical public argv[0]";
    return failure;
  }
  return compiler_lease_->execute_request(std::move(request));
}

bool ResolvedFreestandingToolchain::revalidate(std::string &detail) const {
  detail.clear();
  std::error_code error;
  const std::filesystem::path canonical_root = std::filesystem::canonical(root_, error);
  if (error || canonical_root != root_) {
    detail = error ? "could not revalidate freestanding toolchain root: " + error.message()
                   : "freestanding toolchain root canonical identity changed";
    return false;
  }
#if !defined(_WIN32)
  if (!validate_owner_controlled_directory_chain(root_, detail)) {
    detail = "freestanding toolchain root left the owner-controlled boundary: " + detail;
    return false;
  }
#endif
  if (!compiler_lease_->revalidate(detail)) {
    detail = "freestanding clang++ private execution snapshot changed: " + detail;
    return false;
  }
  return revalidate_tool(nebula_executable_, "Nebula executable", detail);
}

bool ResolvedFreestandingToolchain::cleanup_compiler_snapshot(std::string &detail) {
  detail.clear();
#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
  if (auto injected =
        freestanding_toolchain_testing::take_injected_compiler_snapshot_cleanup_failure();
      injected.has_value()) {
    mark_signal_redelivery_unsafe();
    detail = "failed to retire the verified freestanding compiler snapshot: " + *injected;
    return false;
  }
#endif
  VerifiedExecutableLeaseResult cleanup;
  try {
    cleanup = compiler_lease_->cleanup();
  } catch (...) {
    mark_signal_redelivery_unsafe();
    throw;
  }
  if (cleanup.ok())
    return true;
  mark_signal_redelivery_unsafe();
  detail = "failed to retire the verified freestanding compiler snapshot";
  if (!cleanup.error.operation.empty())
    detail += " during " + cleanup.error.operation;
  if (!cleanup.error.detail.empty())
    detail += ": " + cleanup.error.detail;
  return false;
}

FreestandingToolchainPrepareResult ResolvedFreestandingToolchain::prepare_session_close() {
  FreestandingToolchainPrepareResult result;
  if (session_state_ == FreestandingToolchainSessionState::Closed) {
    if (compiler_snapshot_active())
      result.detail = "freestanding compiler snapshot remained active after its session closed";
    return result;
  }
  if (session_state_ == FreestandingToolchainSessionState::PreparedFrozen) {
    result.observed_signal = termination_signals_->intercepted_signal();
    return result;
  }

  // Closing is monotonic. Even a failed freeze or lease retirement can be
  // retried, but compiler execution must never become available again.
  session_state_ = FreestandingToolchainSessionState::Closing;
  std::string freeze_detail;
  const bool freeze_ok = termination_signals_->freeze(freeze_detail);
  if (!freeze_ok) {
    mark_signal_redelivery_unsafe();
    append_detail(result.detail,
                  "failed to freeze the freestanding toolchain signal session: " + freeze_detail);
    if (termination_signals_->intercepted_signal() != 0)
      append_redelivery_suppressed(result.detail,
                                   "the signal session could not be frozen for cleanup");
  }
  result.observed_signal = termination_signals_->intercepted_signal();

  std::string cleanup_detail;
  const bool cleanup_ok = cleanup_compiler_snapshot(cleanup_detail);
  if (!cleanup_ok) {
    mark_signal_redelivery_unsafe();
    append_detail(result.detail, cleanup_detail);
    if (termination_signals_->intercepted_signal() != 0) {
      append_redelivery_suppressed(result.detail,
                                   "the verified compiler snapshot could not be retired");
    }
    // Keep the session Closing and, when freeze succeeded, frozen. The owner
    // may retry preparation after resolving the explicit lease cleanup error.
  }

  if (freeze_ok && cleanup_ok)
    session_state_ = FreestandingToolchainSessionState::PreparedFrozen;
  return result;
}

FreestandingToolchainCloseResult ResolvedFreestandingToolchain::finalize_session_close(
  FreestandingExternalCleanup external_cleanup) {
  FreestandingToolchainCloseResult result;
  if (session_state_ == FreestandingToolchainSessionState::Closed) {
    if (compiler_snapshot_active())
      result.detail = "freestanding compiler snapshot remained active after its session closed";
    return result;
  }
  if (session_state_ != FreestandingToolchainSessionState::PreparedFrozen) {
    result.detail = session_state_ == FreestandingToolchainSessionState::Executable
                      ? "freestanding toolchain session must be prepared before finalization"
                      : "freestanding toolchain session preparation did not complete";
    return result;
  }

  if (external_cleanup == FreestandingExternalCleanup::Incomplete)
    mark_signal_redelivery_unsafe();

  int restored_signal = 0;
  std::string restore_detail;
  if (!termination_signals_->restore(restored_signal, restore_detail)) {
    mark_signal_redelivery_unsafe();
    append_detail(result.detail, restore_detail.empty()
                                   ? "failed to restore the freestanding toolchain signal session"
                                   : restore_detail);
    if (termination_signals_->intercepted_signal() != 0) {
      append_redelivery_suppressed(result.detail,
                                   "the caller signal disposition could not be restored");
    }
    return result;
  }
  session_state_ = FreestandingToolchainSessionState::Closed;
  if (external_cleanup == FreestandingExternalCleanup::Complete && signal_redelivery_safe_)
    result.interrupted_signal = restored_signal;
  return result;
}

FreestandingToolchainCloseResult ResolvedFreestandingToolchain::close_session() {
  FreestandingToolchainCloseResult result;
  const FreestandingToolchainPrepareResult prepare = prepare_session_close();
  if (!prepare.ok()) {
    result.detail = prepare.detail;
    return result;
  }
  return finalize_session_close(FreestandingExternalCleanup::Complete);
}

std::string ResolvedFreestandingToolchain::provenance_identity() const {
  std::ostringstream identity;
  identity << "resolved-freestanding-toolchain-v1\n";
  append_length_delimited(identity, "toolchain_root", root_.generic_string());
  append_length_delimited(identity, "compiler_path", compiler_.executable.generic_string());
  identity << "compiler_size=" << compiler_.size << '\n';
  identity << "compiler_sha256=" << compiler_.sha256 << '\n';
  append_length_delimited(identity, "compiler_version", compiler_.version);
  append_length_delimited(identity, "target_triple", target_triple_);
  append_length_delimited(identity, "command_schema", kFreestandingCommandSchema);
  append_length_delimited(identity, "nebula_executable_path",
                          nebula_executable_.executable.generic_string());
  identity << "nebula_executable_size=" << nebula_executable_.size << '\n';
  identity << "nebula_executable_sha256=" << nebula_executable_.sha256 << '\n';
  return identity.str();
}

FreestandingToolchainResolutionResult
resolve_freestanding_toolchain(const FreestandingToolchainRequest &request) {
#if defined(_WIN32)
  (void)request;
  return failure(FreestandingToolchainErrorCode::HostUnsupported,
                 "freestanding object toolchain resolution is unsupported on Windows hosts");
#else
  std::filesystem::path root;
  std::string detail;
  if (!canonicalize_root(request.toolchain_root, root, detail))
    return failure(FreestandingToolchainErrorCode::InvalidRoot, std::move(detail));

  ResolvedToolIdentity compiler;
  const std::filesystem::path compiler_path = root / "bin" / "clang++";
  if (!resolve_identity(compiler_path, "freestanding clang++", true, compiler, detail)) {
    std::error_code exists_error;
    const bool compiler_path_exists = std::filesystem::exists(compiler_path, exists_error);
    return failure(!exists_error && compiler_path_exists
                     ? FreestandingToolchainErrorCode::UntrustedCompiler
                     : FreestandingToolchainErrorCode::MissingCompiler,
                   std::move(detail));
  }
  ResolvedToolIdentity nebula_executable;
  if (!resolve_identity(request.self_executable, "Nebula executable", true, nebula_executable,
                        detail)) {
    return failure(FreestandingToolchainErrorCode::Identity, std::move(detail));
  }

  auto signals = std::make_unique<CompilerTerminationSignalScope>();
  if (!signals->arm(detail))
    return failure(FreestandingToolchainErrorCode::SignalBoundary, std::move(detail));
  bool signal_redelivery_safe = true;

  VerifiedExecutableLeaseBeginResult compiler_lease;
  // Declaration order is a cleanup invariant: on an unexpected exception the
  // guard is destroyed first, then compiler_lease, then signals. The guard is
  // active before lease acquisition so construction failures cannot let the
  // signal scope hand an intercepted signal to the caller after implicit lease
  // cleanup has become unobservable.
  ResolverExceptionSignalGuard resolver_exception_guard(*signals);

  auto finish_without_snapshot = [&](FreestandingToolchainResolutionResult result) {
    std::string freeze_detail;
    const bool freeze_ok = signals->freeze(freeze_detail);
    if (!freeze_ok) {
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
      result.error.code = FreestandingToolchainErrorCode::SignalBoundary;
      append_detail(result.error.detail,
                    "failed to freeze the freestanding resolver signal boundary: " + freeze_detail);
    }
    const int observed_signal = signals->intercepted_signal();
    if (observed_signal != 0) {
      append_detail(result.error.detail,
                    "freestanding toolchain resolution was interrupted by signal " +
                      std::to_string(observed_signal));
      if (!freeze_ok) {
        append_redelivery_suppressed(result.error.detail,
                                     "the resolver signal boundary could not be frozen");
      }
    }
    result.interrupted_signal = 0;
    int restored_signal = 0;
    std::string restore_detail;
    const bool restore_ok = signals->restore(restored_signal, restore_detail);
    if (!restore_ok) {
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
      result.error.code = FreestandingToolchainErrorCode::SignalBoundary;
      append_detail(result.error.detail,
                    restore_detail.empty()
                      ? "failed to restore the freestanding resolver signal boundary"
                      : restore_detail);
      if (signals->intercepted_signal() != 0) {
        append_redelivery_suppressed(result.error.detail,
                                     "the caller signal disposition could not be restored");
      }
    } else if (restored_signal != 0 && signal_redelivery_safe) {
      result.interrupted_signal = restored_signal;
      result.error.code = FreestandingToolchainErrorCode::Interrupted;
    } else if (restored_signal != 0) {
      append_redelivery_suppressed(result.error.detail,
                                   "the resolver lifecycle did not complete its signal boundary");
    }
    resolver_exception_guard.disarm();
    return result;
  };

  const FileDigest compiler_digest{compiler.size, compiler.sha256};
  compiler_lease = begin_verified_executable_lease(compiler.executable, compiler_digest);
  if (!compiler_lease.ok()) {
    const bool cleanup_incomplete =
      compiler_lease.error.code == VerifiedExecutableLeaseErrorCode::CleanupIncomplete;
    if (cleanup_incomplete) {
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
    }
    std::string lease_detail = "could not create the verified freestanding compiler snapshot";
    if (!compiler_lease.error.operation.empty())
      lease_detail += " during " + compiler_lease.error.operation;
    if (!compiler_lease.error.detail.empty())
      lease_detail += ": " + compiler_lease.error.detail;
    FreestandingToolchainResolutionResult lease_failure =
      failure(cleanup_incomplete ? FreestandingToolchainErrorCode::Cleanup
                                 : FreestandingToolchainErrorCode::Identity,
              std::move(lease_detail));
    if (cleanup_incomplete && signals->intercepted_signal() != 0) {
      append_redelivery_suppressed(
        lease_failure.error.detail,
        "the verified compiler snapshot acquisition rollback did not complete");
    }
    return finish_without_snapshot(std::move(lease_failure));
  }

#if defined(NEBULA_FREESTANDING_TOOLCHAIN_TESTING)
  if (const auto setup = freestanding_toolchain_testing::take_injected_resolver_exception_setup();
      setup != nullptr) {
    setup(compiler_lease.lease->execution_path());
    throw std::runtime_error(
      std::string(freestanding_toolchain_testing::kInjectedResolverExceptionDetail));
  }
#endif

  auto finish = [&](FreestandingToolchainResolutionResult result) {
    std::string freeze_detail;
    const bool freeze_ok = signals->freeze(freeze_detail);
    if (!freeze_ok) {
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
      result.error.code = FreestandingToolchainErrorCode::SignalBoundary;
      append_detail(result.error.detail,
                    "failed to freeze the freestanding query signal boundary: " + freeze_detail);
    }
    const int observed_signal = signals->intercepted_signal();
    if (observed_signal != 0) {
      append_detail(result.error.detail,
                    "freestanding toolchain resolution was interrupted by signal " +
                      std::to_string(observed_signal));
      if (!freeze_ok) {
        append_redelivery_suppressed(result.error.detail,
                                     "the query signal boundary could not be frozen");
      }
    }
    result.interrupted_signal = 0;

    bool cleanup_ok = false;
    std::string cleanup_detail;
    if (compiler_lease.lease != nullptr) {
      const VerifiedExecutableLeaseResult cleanup = compiler_lease.lease->cleanup();
      cleanup_ok = cleanup.ok();
      if (!cleanup_ok) {
        cleanup_detail = "failed to retire the verified compiler snapshot";
        if (!cleanup.error.operation.empty())
          cleanup_detail += " during " + cleanup.error.operation;
        if (!cleanup.error.detail.empty())
          cleanup_detail += ": " + cleanup.error.detail;
      }
      // Release native handles while termination signals are still frozen.
      // A failed explicit cleanup is retried only by this exception-safe
      // destructor boundary and remains an observable Cleanup error.
      compiler_lease.lease.reset();
    }
    if (!cleanup_ok) {
      result.error.code = FreestandingToolchainErrorCode::Cleanup;
      append_detail(result.error.detail,
                    cleanup_detail.empty()
                      ? "compiler snapshot cleanup was unavailable after query failure"
                      : cleanup_detail);
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
      if (signals->intercepted_signal() != 0) {
        append_redelivery_suppressed(result.error.detail,
                                     "the verified compiler snapshot could not be retired");
      }
    }

    int restored_signal = 0;
    std::string restore_detail;
    const bool restore_ok = signals->restore(restored_signal, restore_detail);
    if (!restore_ok) {
      signal_redelivery_safe = false;
      signals->suppress_emergency_redelivery();
      result.error.code = FreestandingToolchainErrorCode::SignalBoundary;
      append_detail(result.error.detail,
                    restore_detail.empty()
                      ? "failed to restore the freestanding query signal boundary"
                      : restore_detail);
      if (signals->intercepted_signal() != 0) {
        append_redelivery_suppressed(result.error.detail,
                                     "the caller signal disposition could not be restored");
      }
    }
    if (restored_signal != 0 && signal_redelivery_safe && cleanup_ok && restore_ok && freeze_ok) {
      result.interrupted_signal = restored_signal;
      result.error.code = FreestandingToolchainErrorCode::Interrupted;
    } else {
      result.interrupted_signal = 0;
    }
    resolver_exception_guard.disarm();
    return result;
  };

  QueryResult version =
    run_query({compiler.executable.string(), "--version"}, *compiler_lease.lease,
              "freestanding clang++ version query", false, *signals);
  signal_redelivery_safe = signal_redelivery_safe && version.signal_redelivery_safe;
  if (!version.output.has_value())
    return finish(failure(FreestandingToolchainErrorCode::Query, std::move(version.detail)));
  compiler.version = std::move(*version.output);

  QueryResult target =
    run_query({compiler.executable.string(), "--target=" + std::string(kFreestandingTargetTriple),
               "--no-default-config", "-dumpmachine"},
              *compiler_lease.lease, "freestanding clang++ target query", false, *signals);
  signal_redelivery_safe = signal_redelivery_safe && target.signal_redelivery_safe;
  if (!target.output.has_value())
    return finish(failure(FreestandingToolchainErrorCode::Query, std::move(target.detail)));
  if (*target.output != kFreestandingTargetTriple) {
    return finish(failure(FreestandingToolchainErrorCode::Capability,
                          "freestanding clang++ returned unexpected target triple: " +
                            quote_cli_log_value(*target.output)));
  }

  std::vector<std::string> capability_arguments = {
    compiler.executable.string(),
    "--target=" + std::string(kFreestandingTargetTriple),
    "--no-default-config",
    "-std=c++20",
    "-x",
    "c++",
    "-ffreestanding",
    "-nostdinc",
    "-nostdinc++",
  };
  for (const std::string_view argument : nebula::boot::kUosX86_64RequiredCompilerAbiArguments)
    capability_arguments.emplace_back(argument);
  capability_arguments.insert(capability_arguments.end(), {"-fsyntax-only", "/dev/null"});
  QueryResult capability = run_query(std::move(capability_arguments), *compiler_lease.lease,
                                     "freestanding clang++ ABI capability query", true, *signals);
  signal_redelivery_safe = signal_redelivery_safe && capability.signal_redelivery_safe;
  if (!capability.output.has_value())
    return finish(
      failure(FreestandingToolchainErrorCode::Capability, std::move(capability.detail)));

  if (signals->intercepted_signal() != 0) {
    return finish(failure(FreestandingToolchainErrorCode::Interrupted,
                          "freestanding toolchain resolution observed caller interruption"));
  }

  FreestandingToolchainResolutionResult result;
  ResolvedFreestandingToolchain resolved(
    std::move(root), std::move(compiler), std::move(nebula_executable), std::move(*target.output),
    signal_redelivery_safe, std::move(signals), std::move(compiler_lease.lease));
  result.value.emplace(std::move(resolved));
  resolver_exception_guard.disarm();
  return result;
#endif
}

} // namespace nebula::cli
