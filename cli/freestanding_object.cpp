#include "freestanding_object.hpp"

#include "artifact_metadata.hpp"
#include "boot/protocol_abi_contract.hpp"
#include "log_value.hpp"

#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
#include "freestanding_transaction_test_hooks.hpp"
#endif

#include <utility>

namespace fs = std::filesystem;

#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
namespace nebula::cli::freestanding_transaction_testing {
namespace {
PhaseObserver phase_observer = nullptr;
std::uint32_t pending_faults = 0U;
unsigned publication_attempts = 0U;
int injected_output_lock_descriptor = -1;

constexpr std::uint32_t fault_bit(Fault fault) {
  return std::uint32_t{1U} << static_cast<unsigned>(fault);
}
} // namespace

void set_phase_observer(PhaseObserver observer) noexcept { phase_observer = observer; }

void clear_phase_observer() noexcept { phase_observer = nullptr; }

void inject_fault_once(Fault fault) noexcept {
  pending_faults |= fault_bit(fault);
  if (fault == Fault::SecondPublication)
    publication_attempts = 0U;
}

bool fault_pending(Fault fault) noexcept { return (pending_faults & fault_bit(fault)) != 0U; }

void clear_faults() noexcept {
  pending_faults = 0U;
  publication_attempts = 0U;
  injected_output_lock_descriptor = -1;
}

int last_injected_output_lock_descriptor() noexcept { return injected_output_lock_descriptor; }

bool take_fault(Fault fault) noexcept {
  const std::uint32_t bit = fault_bit(fault);
  if ((pending_faults & bit) == 0U)
    return false;
  pending_faults &= ~bit;
  return true;
}

bool fail_this_publication() noexcept {
  if (!fault_pending(Fault::SecondPublication))
    return false;
  ++publication_attempts;
  return publication_attempts == 2U && take_fault(Fault::SecondPublication);
}

void notify_phase(Phase phase) {
  if (phase_observer != nullptr)
    phase_observer(phase);
}

void record_injected_output_lock_descriptor(int descriptor) noexcept {
  injected_output_lock_descriptor = descriptor;
}
} // namespace nebula::cli::freestanding_transaction_testing
#endif

namespace {

nebula::frontend::Diagnostic make_fs_diagnostic(std::string code, std::string message,
                                                std::string cause, std::string impact,
                                                std::vector<std::string> suggestions = {}) {
  nebula::frontend::Diagnostic diagnostic;
  diagnostic.severity = nebula::frontend::Severity::Error;
  diagnostic.code = std::move(code);
  diagnostic.message = std::move(message);
  diagnostic.stage = nebula::frontend::DiagnosticStage::Build;
  diagnostic.risk = nebula::frontend::DiagnosticRisk::High;
  diagnostic.category = "cli";
  diagnostic.cause = std::move(cause);
  diagnostic.impact = std::move(impact);
  diagnostic.suggestions = std::move(suggestions);
  return diagnostic;
}

} // namespace

#if defined(_WIN32)

FreestandingObjectResult
build_freestanding_object(const FreestandingObjectRequest &request,
                          nebula::cli::ResolvedFreestandingToolchain &toolchain,
                          FreestandingCompilerExecutor &compiler_executor) {
  (void)compiler_executor;
  (void)toolchain;
  (void)request;
  FreestandingObjectResult result;
  result.diagnostics.push_back(make_fs_diagnostic(
    "NBL-CLI-FS-HOST-UNSUPPORTED",
    "freestanding object publication is not implemented on Windows hosts",
    "the audited publication path currently requires POSIX file identity and locking primitives",
    "the Windows compiler/tooling release remains available, but this experimental artifact is "
    "unavailable",
    {"run the exact freestanding-object request on a supported macOS or Linux host"}));
  return result;
}

#else

#include "artifact_digest.hpp"
#include "elf_object.hpp"
#include "termination_signal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using nebula::frontend::Diagnostic;
std::optional<fs::path> absolute_lexical_path(const fs::path &path, std::string_view role,
                                              std::string &detail) {
  std::error_code error;
  const fs::path result = fs::absolute(path, error);
  if (error) {
    detail = std::string(role) + ": " + error.message();
    return std::nullopt;
  }
  return result.lexically_normal();
}

struct PosixPathIdentity {
  dev_t device = 0;
  ino_t inode = 0;
  bool valid = false;
};

std::string describe_path_identity(const fs::path &path, const PosixPathIdentity &identity) {
  std::string description = quote_cli_log_value(path.string());
  if (!identity.valid)
    return description + " (identity unavailable)";
  return description + " (device " + std::to_string(static_cast<std::uintmax_t>(identity.device)) +
         ", inode " + std::to_string(static_cast<std::uintmax_t>(identity.inode)) + ")";
}

bool same_path_identity(const struct stat &status, const PosixPathIdentity &identity) noexcept {
  return identity.valid && status.st_dev == identity.device && status.st_ino == identity.inode;
}

enum class LockStatus : std::uint8_t { Acquired, Busy, Error, CleanupIncomplete };

struct OutputLockAcquisitionResult {
  LockStatus status = LockStatus::Error;
  fs::path path;
  PosixPathIdentity identity;
  std::string detail;
  int cleanup_error = 0;
};

static_assert(std::is_nothrow_move_constructible_v<OutputLockAcquisitionResult>);

struct OutputLockReleaseResult {
  int unlock_error = 0;
  int close_error = 0;
  bool injected_confirmation_failure = false;

  [[nodiscard]] bool complete() const noexcept {
    return unlock_error == 0 && close_error == 0 && !injected_confirmation_failure;
  }
};

bool close_descriptor_with_context(int descriptor, std::string_view context, std::string &detail) {
  if (::close(descriptor) == 0)
    return true;
  const int close_error = errno;
  if (!detail.empty())
    detail += "; ";
  detail += std::string(context) + ": " + std::strerror(close_error);
  return false;
}

class OutputLock {
public:
  OutputLock() = default;
  OutputLock(const OutputLock &) = delete;
  OutputLock &operator=(const OutputLock &) = delete;

  ~OutputLock() {
    if (descriptor_ < 0)
      return;
    const int descriptor = descriptor_;
    descriptor_ = -1;
    int unlock_result = 0;
    do {
      unlock_result = ::flock(descriptor, LOCK_UN);
    } while (unlock_result != 0 && errno == EINTR);
    const bool unlock_failed = unlock_result != 0;
    const bool close_failed = ::close(descriptor) != 0;
    if (!unlock_failed && !close_failed)
      return;
    constexpr char message[] =
      "[NBL-CLI-FS-CLEANUP] output lock cleanup failed during exception cleanup\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }

  OutputLockReleaseResult release() noexcept {
    if (descriptor_ < 0)
      return {};
    const int descriptor = descriptor_;
    descriptor_ = -1;

    int unlock_error = 0;
    while (true) {
      if (::flock(descriptor, LOCK_UN) == 0) {
        unlock_error = 0;
        break;
      }
      const int current_error = errno;
      if (current_error == EINTR)
        continue;
      unlock_error = current_error;
      break;
    }
    const int close_result = ::close(descriptor);
    const int close_error = close_result == 0 ? 0 : errno;
    OutputLockReleaseResult result{unlock_error, close_error, false};
    if (unlock_error == 0 && close_error == 0) {
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
      if (nebula::cli::freestanding_transaction_testing::take_fault(
            nebula::cli::freestanding_transaction_testing::Fault::OutputLockRelease)) {
        result.injected_confirmation_failure = true;
      }
#endif
      return result;
    }
    return result;
  }

  OutputLockAcquisitionResult acquire(const fs::path &path) {
    OutputLockAcquisitionResult result;
    result.path = path;
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor_ = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor_ < 0) {
      result.detail = std::strerror(errno);
      return result;
    }
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::OutputLockOpened);
#endif
    struct stat status{};
    if (::fstat(descriptor_, &status) != 0) {
      result.detail = "failed to inspect output lock: " + std::string(std::strerror(errno));
      return fail_open_acquisition(std::move(result), LockStatus::Error);
    }
    result.identity = {status.st_dev, status.st_ino, true};
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    if (nebula::cli::freestanding_transaction_testing::take_fault(
          nebula::cli::freestanding_transaction_testing::Fault::OutputLockAcquireRollbackClose)) {
      result.detail = "injected output lock inspection failure";
      result.status = LockStatus::CleanupIncomplete;
      result.detail += "; injected output lock acquisition rollback close failure";
      nebula::cli::freestanding_transaction_testing::record_injected_output_lock_descriptor(
        descriptor_);
      return result;
    }
#endif
    if (!S_ISREG(status.st_mode)) {
      result.detail = "lock path is not a regular file";
      return fail_open_acquisition(std::move(result), LockStatus::Error);
    }
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
      const int lock_error = errno;
      result.detail = std::strerror(lock_error);
      const LockStatus primary_status =
        lock_error == EWOULDBLOCK || lock_error == EAGAIN ? LockStatus::Busy : LockStatus::Error;
      return fail_open_acquisition(std::move(result), primary_status);
    }
    result.status = LockStatus::Acquired;
    return result;
  }

private:
  OutputLockAcquisitionResult fail_open_acquisition(OutputLockAcquisitionResult result,
                                                    LockStatus primary_status) noexcept {
    const int descriptor = descriptor_;
    // A real close failure leaves the descriptor number in an indeterminate
    // state on supported POSIX hosts. Never retry that number: another thread
    // could have reused it after the kernel released the original descriptor.
    descriptor_ = -1;
    if (::close(descriptor) == 0) {
      result.status = primary_status;
      return result;
    }
    result.cleanup_error = errno;
    result.status = LockStatus::CleanupIncomplete;
    return result;
  }

  int descriptor_ = -1;
};

class StagingDirectoryGuard {
public:
  explicit StagingDirectoryGuard(const std::optional<fs::path> &path) : path_(path) {}
  StagingDirectoryGuard(const StagingDirectoryGuard &) = delete;
  StagingDirectoryGuard &operator=(const StagingDirectoryGuard &) = delete;

  ~StagingDirectoryGuard() noexcept {
    if (released_ || !path_.has_value())
      return;
    try {
      std::error_code error;
      fs::remove_all(*path_, error);
      if (error) {
        constexpr char message[] =
          "[NBL-CLI-FS-CLEANUP] staging cleanup failed during exception cleanup\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
        ::_exit(125);
      }
    } catch (...) {
      constexpr char message[] =
        "[NBL-CLI-FS-CLEANUP] staging cleanup threw during exception cleanup\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
  }

  void release() noexcept { released_ = true; }

private:
  const std::optional<fs::path> &path_;
  bool released_ = false;
};

bool path_is_absent(const fs::path &path, std::string &detail) {
  std::error_code error;
  const fs::file_status status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory)
    return true;
  if (error) {
    detail = error.message();
    return false;
  }
  if (status.type() == fs::file_type::not_found)
    return true;
  detail = "path already exists";
  return false;
}

enum class StagingAcquisitionDisposition : std::uint8_t {
  Acquired,
  FailedClean,
  InfrastructureFailureClean,
  CleanupIncomplete,
};

enum class StagingRollbackFailure : std::uint8_t {
  None,
  Inspection,
  IdentityChanged,
  Removal,
};

struct StagingRollbackResult {
  StagingRollbackFailure failure = StagingRollbackFailure::None;
  int system_error = 0;

  [[nodiscard]] bool complete() const noexcept { return failure == StagingRollbackFailure::None; }
};

struct StagingAcquisitionResult {
  StagingAcquisitionDisposition disposition = StagingAcquisitionDisposition::FailedClean;
  std::optional<fs::path> path;
  PosixPathIdentity identity;
  std::string detail;
  StagingRollbackResult rollback;
};

static_assert(std::is_nothrow_move_constructible_v<StagingAcquisitionResult>);

StagingRollbackResult rollback_staging_allocation(const fs::path &path,
                                                  const PosixPathIdentity &identity) noexcept {
  struct stat current{};
  if (::lstat(path.c_str(), &current) != 0)
    return {StagingRollbackFailure::Inspection, errno};
  if (!S_ISDIR(current.st_mode) || !same_path_identity(current, identity))
    return {StagingRollbackFailure::IdentityChanged, 0};
  if (::rmdir(path.c_str()) != 0)
    return {StagingRollbackFailure::Removal, errno};
  return {};
}

std::string describe_staging_rollback(const StagingRollbackResult &rollback) {
  switch (rollback.failure) {
  case StagingRollbackFailure::None:
    return {};
  case StagingRollbackFailure::Inspection:
    return "could not re-inspect staging allocation before rollback: " +
           std::string(std::strerror(rollback.system_error));
  case StagingRollbackFailure::IdentityChanged:
    return "staging allocation path no longer identifies the created directory";
  case StagingRollbackFailure::Removal:
    return "could not remove the identity-bound staging allocation: " +
           std::string(std::strerror(rollback.system_error));
  }
  return "staging allocation rollback has an invalid typed failure state";
}

class StagingAllocationOwner final {
public:
  StagingAllocationOwner(const fs::path &path, PosixPathIdentity identity) noexcept
      : path_(path), identity_(identity) {}
  StagingAllocationOwner(const StagingAllocationOwner &) = delete;
  StagingAllocationOwner &operator=(const StagingAllocationOwner &) = delete;

  ~StagingAllocationOwner() noexcept {
    if (!active_)
      return;
    struct stat current{};
    const bool identity_matches = ::lstat(path_.c_str(), &current) == 0 &&
                                  S_ISDIR(current.st_mode) &&
                                  same_path_identity(current, identity_);
    if (identity_matches && ::rmdir(path_.c_str()) == 0)
      return;
    constexpr char message[] =
      "[NBL-CLI-FS-CLEANUP] identity-bound staging allocation rollback failed during exception "
      "cleanup\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
    ::_exit(125);
  }

  StagingRollbackResult rollback() noexcept {
    active_ = false;
    return rollback_staging_allocation(path_, identity_);
  }

  void bind_identity(PosixPathIdentity identity) noexcept { identity_ = identity; }
  void release() noexcept { active_ = false; }

private:
  const fs::path &path_;
  PosixPathIdentity identity_;
  bool active_ = true;
};

StagingAcquisitionResult create_staging_directory(const fs::path &parent) {
  try {
    std::random_device random;
    for (std::size_t attempt = 0; attempt < 64U; ++attempt) {
      const std::uint64_t token = (static_cast<std::uint64_t>(random()) << 32U) ^
                                  static_cast<std::uint64_t>(random()) ^
                                  static_cast<std::uint64_t>(::getpid()) ^ attempt;
      fs::path created_path = parent / (".nebula-fs-" + std::to_string(token));
      if (::mkdir(created_path.c_str(), S_IRWXU) != 0) {
        const int create_error = errno;
        if (create_error == EEXIST)
          continue;
        return {StagingAcquisitionDisposition::FailedClean,
                std::nullopt,
                {},
                std::strerror(create_error),
                {}};
      }
      // Arm ownership immediately after mkdir. No allocation or diagnostic
      // construction may occur first: if identity binding or result transfer
      // throws, the exception fallback must either remove the exact directory
      // or terminate fail-closed when no safe identity exists.
      StagingAllocationOwner owner(created_path, {});

      struct stat created_status{};
      const int identity_result = ::lstat(created_path.c_str(), &created_status);
      const int identity_error = identity_result == 0 ? 0 : errno;
      const PosixPathIdentity observed_identity =
        identity_result == 0 ? PosixPathIdentity{created_status.st_dev, created_status.st_ino, true}
                             : PosixPathIdentity{};
      if (identity_result != 0 || !S_ISDIR(created_status.st_mode)) {
        std::string identity_detail = "could not establish staging allocation identity after mkdir";
        if (identity_error != 0)
          identity_detail += ": " + std::string(std::strerror(identity_error));
        else
          identity_detail += ": created path is not a directory";
        StagingAcquisitionResult cleanup_incomplete;
        cleanup_incomplete.disposition = StagingAcquisitionDisposition::CleanupIncomplete;
        cleanup_incomplete.path.emplace(std::move(created_path));
        cleanup_incomplete.identity = observed_identity;
        cleanup_incomplete.detail = std::move(identity_detail);
        cleanup_incomplete.rollback = {identity_result != 0
                                         ? StagingRollbackFailure::Inspection
                                         : StagingRollbackFailure::IdentityChanged,
                                       identity_error};
        owner.release();
        return cleanup_incomplete;
      }
      const PosixPathIdentity identity = observed_identity;
      owner.bind_identity(identity);
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
      nebula::cli::freestanding_transaction_testing::notify_phase(
        nebula::cli::freestanding_transaction_testing::Phase::StagingDirectoryCreated);
      const bool inject_permissions_rollback_failure =
        nebula::cli::freestanding_transaction_testing::take_fault(
          nebula::cli::freestanding_transaction_testing::Fault::StagingPermissionsRollbackCleanup);
#else
      constexpr bool inject_permissions_rollback_failure = false;
#endif
      std::error_code error;
      if (!inject_permissions_rollback_failure)
        fs::permissions(created_path, fs::perms::owner_all, fs::perm_options::replace, error);
      if (error) {
        // Fully materialize both the path and diagnostic while the owner is
        // still armed. After the one authoritative rollback attempt, only
        // noexcept typed state changes are allowed before return.
        StagingAcquisitionResult rollback_result;
        rollback_result.disposition = StagingAcquisitionDisposition::CleanupIncomplete;
        rollback_result.path.emplace(created_path);
        rollback_result.identity = identity;
        rollback_result.detail = error.message();
        const StagingRollbackResult rollback = owner.rollback();
        if (rollback.complete()) {
          rollback_result.disposition = StagingAcquisitionDisposition::FailedClean;
          rollback_result.path.reset();
          return rollback_result;
        }
        rollback_result.rollback = rollback;
        return rollback_result;
      }
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
      if (inject_permissions_rollback_failure) {
        StagingAcquisitionResult cleanup_incomplete;
        cleanup_incomplete.disposition = StagingAcquisitionDisposition::CleanupIncomplete;
        cleanup_incomplete.detail =
          "injected staging permissions failure; injected staging allocation rollback cleanup "
          "failure";
        cleanup_incomplete.rollback = {StagingRollbackFailure::Removal, EIO};
        cleanup_incomplete.path.emplace(std::move(created_path));
        cleanup_incomplete.identity = identity;
        owner.release();
        return cleanup_incomplete;
      }
      if (nebula::cli::freestanding_transaction_testing::take_fault(
            nebula::cli::freestanding_transaction_testing::Fault::BeforeStagingOwnershipTransfer)) {
        throw std::runtime_error("injected failure before staging ownership transfer");
      }
#endif
      StagingAcquisitionResult acquired;
      acquired.disposition = StagingAcquisitionDisposition::Acquired;
      acquired.path.emplace(std::move(created_path));
      acquired.identity = identity;
      owner.release();
      return acquired;
    }
  } catch (const std::system_error &error) {
    return {StagingAcquisitionDisposition::InfrastructureFailureClean,
            std::nullopt,
            {},
            error.what(),
            {}};
  } catch (const std::runtime_error &error) {
    return {StagingAcquisitionDisposition::InfrastructureFailureClean,
            std::nullopt,
            {},
            error.what(),
            {}};
  }
  return {StagingAcquisitionDisposition::FailedClean,
          std::nullopt,
          {},
          "could not allocate a unique staging directory after 64 attempts",
          {}};
}

bool write_checked_file(const fs::path &path, std::string_view contents, std::string &detail) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    detail = "could not open staging file";
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.flush();
  if (!output.good()) {
    detail = "write or flush failed";
    return false;
  }
  output.close();
  if (output.fail()) {
    detail = "close failed";
    return false;
  }
  return true;
}

std::optional<std::vector<std::uint8_t>> read_bounded_compiler_object(const fs::path &path,
                                                                      std::string &detail) {
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
  flags |= O_NONBLOCK;
#endif
  const int descriptor = ::open(path.c_str(), flags);
  if (descriptor < 0) {
    detail = std::strerror(errno);
    return std::nullopt;
  }

  struct stat before{};
  if (::fstat(descriptor, &before) != 0) {
    detail = "failed to inspect compiler output: " + std::string(std::strerror(errno));
    close_descriptor_with_context(descriptor, "compiler output close also failed", detail);
    return std::nullopt;
  }
  if (!S_ISREG(before.st_mode) || before.st_uid != ::geteuid() || before.st_nlink != 1 ||
      before.st_size <= 0 ||
      static_cast<std::uintmax_t>(before.st_size) > nebula::cli::kMaxFreestandingObjectBytes) {
    detail = "compiler output must be one owner-written regular inode within the 64 MiB limit";
    close_descriptor_with_context(descriptor, "compiler output close also failed", detail);
    return std::nullopt;
  }

  const std::size_t size = static_cast<std::size_t>(before.st_size);
  std::vector<std::uint8_t> bytes(size);
  std::size_t consumed = 0U;
  while (consumed < bytes.size()) {
    const ssize_t count = ::read(descriptor, bytes.data() + consumed, bytes.size() - consumed);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      detail = count == 0 ? "compiler output ended before its declared size" : std::strerror(errno);
      close_descriptor_with_context(descriptor, "compiler output close also failed", detail);
      return std::nullopt;
    }
    consumed += static_cast<std::size_t>(count);
  }

  struct stat after{};
  const int final_status_result = ::fstat(descriptor, &after);
  const int final_status_error = final_status_result == 0 ? 0 : errno;
  const bool stable = final_status_result == 0 && before.st_dev == after.st_dev &&
                      before.st_ino == after.st_ino && before.st_size == after.st_size &&
                      before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime &&
                      after.st_nlink == 1;
  const int close_result = ::close(descriptor);
  const int close_error = close_result == 0 ? 0 : errno;
  if (!stable || close_result != 0) {
    detail = final_status_error != 0 ? "failed to re-inspect compiler output: " +
                                         std::string(std::strerror(final_status_error))
             : !stable               ? "compiler output inode changed while it was being read"
                                     : "failed to close compiler output after inspection read: " +
                                         std::string(std::strerror(close_error));
    if (!stable && close_error != 0) {
      detail += "; compiler output close also failed: ";
      detail += std::strerror(close_error);
    }
    return std::nullopt;
  }
  return bytes;
}

bool write_exclusive_bytes(const fs::path &path, std::span<const std::uint8_t> bytes,
                           std::string &detail) {
  int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    detail = std::strerror(errno);
    return false;
  }
  std::size_t written = 0U;
  while (written < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      detail = count == 0 ? "verified object write made no progress" : std::strerror(errno);
      close_descriptor_with_context(descriptor, "verified object close also failed", detail);
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    detail = std::strerror(errno);
    close_descriptor_with_context(descriptor, "verified object close also failed", detail);
    return false;
  }
  if (::close(descriptor) != 0) {
    detail = std::strerror(errno);
    return false;
  }
  return true;
}

bool contains_forbidden_source_surface(std::string_view source, std::string &token) {
  for (const std::string_view forbidden : {"#include", "std::", "nebula::rt", " throw ", " new "}) {
    if (source.find(forbidden) != std::string_view::npos) {
      token = std::string(forbidden);
      return true;
    }
  }
  return false;
}

enum class StagingCleanupFailure : std::uint8_t { None, Injected, Filesystem };

struct StagingCleanupResult {
  StagingCleanupFailure failure = StagingCleanupFailure::None;
  std::error_code error;

  [[nodiscard]] bool complete() const noexcept { return failure == StagingCleanupFailure::None; }
};

StagingCleanupResult cleanup_staging(const fs::path &staging) {
  if (staging.empty())
    return {};
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
  if (nebula::cli::freestanding_transaction_testing::take_fault(
        nebula::cli::freestanding_transaction_testing::Fault::StagingCleanup)) {
    return {StagingCleanupFailure::Injected, {}};
  }
#endif
  std::error_code error;
  fs::remove_all(staging, error);
  return error ? StagingCleanupResult{StagingCleanupFailure::Filesystem, error}
               : StagingCleanupResult{};
}

Diagnostic make_staging_cleanup_diagnostic(const StagingCleanupResult &cleanup) {
  return make_fs_diagnostic(
    "NBL-CLI-FS-CLEANUP", "failed to clean freestanding build state after staging cleanup",
    cleanup.failure == StagingCleanupFailure::Injected ? "injected staging cleanup failure"
                                                       : cleanup.error.message(),
    cleanup.failure == StagingCleanupFailure::Injected
      ? "a private staging path remains for deterministic cleanup-failure validation"
      : "a private staging path may require manual removal");
}

struct PublishedPath {
  fs::path destination;
  dev_t device = 0;
  ino_t inode = 0;
  bool created = false;
};

class PublishedRollbackGuard {
public:
  explicit PublishedRollbackGuard(const std::vector<PublishedPath> &paths) : paths_(paths) {}
  PublishedRollbackGuard(const PublishedRollbackGuard &) = delete;
  PublishedRollbackGuard &operator=(const PublishedRollbackGuard &) = delete;

  ~PublishedRollbackGuard() noexcept {
    if (!active_)
      return;
    bool cleanup_failed = false;
    for (auto it = paths_.rbegin(); it != paths_.rend(); ++it) {
      if (!it->created)
        continue;
      struct stat current{};
      if (::lstat(it->destination.c_str(), &current) != 0) {
        if (errno != ENOENT)
          cleanup_failed = true;
        continue;
      }
      if (current.st_dev != it->device || current.st_ino != it->inode) {
        cleanup_failed = true;
        continue;
      }
      if (::unlink(it->destination.c_str()) != 0 && errno != ENOENT)
        cleanup_failed = true;
    }
    if (cleanup_failed) {
      constexpr char message[] =
        "[NBL-CLI-FS-CLEANUP] publication rollback was incomplete during exception cleanup\n";
      (void)::write(STDERR_FILENO, message, sizeof(message) - 1U);
      ::_exit(125);
    }
  }

  void release() noexcept { active_ = false; }

private:
  const std::vector<PublishedPath> &paths_;
  bool active_ = true;
};

enum class PublicationRollbackFailure : std::uint8_t {
  None,
  Injected,
  Inspection,
  IdentityChanged,
  Removal,
};

struct PublicationRollbackResult {
  PublicationRollbackFailure failure = PublicationRollbackFailure::None;
  std::size_t path_index = 0U;
  int system_error = 0;

  [[nodiscard]] bool complete() const noexcept {
    return failure == PublicationRollbackFailure::None;
  }
};

PublicationRollbackResult rollback_published(const std::vector<PublishedPath> &paths) noexcept {
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
  if (nebula::cli::freestanding_transaction_testing::take_fault(
        nebula::cli::freestanding_transaction_testing::Fault::PublicationRollback)) {
    return {PublicationRollbackFailure::Injected, 0U, 0};
  }
#endif
  PublicationRollbackResult result;
  for (std::size_t index = paths.size(); index > 0U; --index) {
    const PublishedPath &path = paths[index - 1U];
    if (!path.created)
      continue;
    struct stat current{};
    if (::lstat(path.destination.c_str(), &current) != 0) {
      const int status_error = errno;
      if (status_error == ENOENT)
        continue;
      if (result.complete())
        result = {PublicationRollbackFailure::Inspection, index - 1U, status_error};
      continue;
    }
    if (current.st_dev != path.device || current.st_ino != path.inode) {
      if (result.complete())
        result = {PublicationRollbackFailure::IdentityChanged, index - 1U, 0};
      continue;
    }
    if (::unlink(path.destination.c_str()) != 0 && errno != ENOENT && result.complete())
      result = {PublicationRollbackFailure::Removal, index - 1U, errno};
  }
  return result;
}

Diagnostic make_publication_rollback_diagnostic(const PublicationRollbackResult &rollback,
                                                const std::vector<PublishedPath> &paths) {
  switch (rollback.failure) {
  case PublicationRollbackFailure::None:
    return {};
  case PublicationRollbackFailure::Injected:
    return make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP", "failed to roll back freestanding publication",
      "injected publication rollback failure",
      "a published path remains for deterministic no-second-retry validation");
  case PublicationRollbackFailure::Inspection:
    return make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP", "failed to inspect a freestanding publication during rollback",
      std::strerror(rollback.system_error),
      "a published path may remain because its identity could not be re-established");
  case PublicationRollbackFailure::IdentityChanged:
    return make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP",
      "refused to remove a changed path during freestanding publication rollback",
      "destination no longer identifies the published inode: " +
        quote_cli_log_value(paths.at(rollback.path_index).destination.string()),
      "a concurrently replaced path was preserved instead of being deleted");
  case PublicationRollbackFailure::Removal:
    return make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP", "failed to remove a freestanding publication during rollback",
      std::strerror(rollback.system_error), "a published path may require manual removal");
  }
  return make_fs_diagnostic("NBL-CLI-FS-CLEANUP",
                            "freestanding publication rollback has an invalid typed state",
                            "the cleanup result enum was outside its closed set",
                            "artifact cleanup must be treated as incomplete");
}

bool publish_no_replace(const fs::path &staged, const fs::path &destination,
                        PublishedPath &published, std::string &detail) {
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
  if (nebula::cli::freestanding_transaction_testing::fail_this_publication()) {
    detail = "injected failure before the second publication path";
    return false;
  }
#endif
  struct stat staged_status{};
  if (::lstat(staged.c_str(), &staged_status) != 0) {
    detail = "failed to inspect staged publication source: " + std::string(std::strerror(errno));
    return false;
  }
  if (!S_ISREG(staged_status.st_mode)) {
    detail = "staged publication source is not a regular file";
    return false;
  }
  published = {destination, staged_status.st_dev, staged_status.st_ino, false};
  std::error_code error;
  fs::create_hard_link(staged, destination, error);
  if (error) {
    detail = error.message();
    return false;
  }
  published.created = true;
  struct stat destination_status{};
  if (::lstat(destination.c_str(), &destination_status) != 0) {
    detail = "failed to inspect published destination: " + std::string(std::strerror(errno));
    return false;
  }
  if (staged_status.st_dev != destination_status.st_dev ||
      staged_status.st_ino != destination_status.st_ino) {
    detail = "published hard link identity could not be verified";
    return false;
  }
  return true;
}

enum class CompilerExecutionDisposition : std::uint8_t {
  Success,
  BuildFailure,
  Timeout,
  InfrastructureFailure,
};

struct CompilerExecutionAssessment {
  CompilerExecutionDisposition disposition = CompilerExecutionDisposition::InfrastructureFailure;
  std::string detail;
};

bool is_owned_termination_signal(int signal_number) {
  return signal_number == SIGHUP || signal_number == SIGINT || signal_number == SIGQUIT ||
         signal_number == SIGTERM;
}

constexpr std::size_t kCompilerDiagnosticStreamBytes = 1024U;

void append_bounded_compiler_output(std::string &detail, const CommandExecutionResult &execution) {
  const auto append_stream = [&](std::string_view label, std::string_view bytes) {
    if (bytes.empty())
      return;
    const std::string_view rendered = bytes.substr(0U, kCompilerDiagnosticStreamBytes);
    detail += "; bounded compiler ";
    detail += label;
    detail += '=';
    detail += quote_cli_log_value(rendered);
    if (rendered.size() != bytes.size()) {
      detail += " (diagnostic truncated after " + std::to_string(rendered.size()) + " of " +
                std::to_string(bytes.size()) + " captured bytes)";
    }
  };
  append_stream("stdout", execution.stdout_summary);
  append_stream("stderr", execution.stderr_summary);
}

CompilerExecutionAssessment assess_compiler_execution(const CommandExecutionResult &execution,
                                                      int transaction_signal) {
  if (!execution.infrastructure_error.empty()) {
    return {
      CompilerExecutionDisposition::InfrastructureFailure,
      execution.infrastructure_error,
    };
  }
  if (execution.containment == CompilerProcessContainment::Unconfirmed) {
    return {
      CompilerExecutionDisposition::InfrastructureFailure,
      "compiler execution returned Unconfirmed containment without the required root-cause "
      "detail",
    };
  }
  if (execution.containment == CompilerProcessContainment::NotStarted) {
    return {
      CompilerExecutionDisposition::InfrastructureFailure,
      "compiler execution returned NotStarted containment without the required root-cause detail",
    };
  }
  if (execution.timed_out) {
    if (execution.exit_code == 124 && execution.interrupted_signal == 0) {
      return {
        CompilerExecutionDisposition::Timeout,
        "the fixed x86_64-unknown-none command exceeded its 30 second timeout",
      };
    }
    return {
      CompilerExecutionDisposition::InfrastructureFailure,
      "compiler execution returned an inconsistent timeout result; timeout requires exit 124, "
      "Confirmed containment, and no interruption signal",
    };
  }
  if (execution.interrupted_signal != 0) {
    if (is_owned_termination_signal(execution.interrupted_signal) &&
        execution.interrupted_signal == transaction_signal &&
        execution.exit_code == 128 + execution.interrupted_signal) {
      return {
        CompilerExecutionDisposition::BuildFailure,
        "compiler execution was interrupted by the caller termination signal",
      };
    }
    return {
      CompilerExecutionDisposition::InfrastructureFailure,
      "compiler execution returned an inconsistent interruption result; interruption requires a "
      "signal observed by the active transaction, exit 128 + signal, and Confirmed containment",
    };
  }
  if (execution.exit_code == 0)
    return {CompilerExecutionDisposition::Success, {}};
  CompilerExecutionAssessment assessment{
    CompilerExecutionDisposition::BuildFailure,
    "the fixed x86_64-unknown-none command returned a nonzero status",
  };
  append_bounded_compiler_output(assessment.detail, execution);
  return assessment;
}

} // namespace

FreestandingObjectResult
build_freestanding_object(const FreestandingObjectRequest &request,
                          nebula::cli::ResolvedFreestandingToolchain &toolchain,
                          FreestandingCompilerExecutor &compiler_executor) {
  FreestandingObjectResult result;
  CompilerTerminationSignalScope &termination_signals = toolchain.termination_signals();
  bool interruption_recorded = false;
  std::optional<std::size_t> interruption_diagnostic_index;
  const auto record_interruption = [&](int signal_number, bool preserve_for_redelivery) {
    if (signal_number == 0 || interruption_recorded)
      return;
    interruption_recorded = true;
    if (preserve_for_redelivery)
      result.interrupted_signal = signal_number;
    interruption_diagnostic_index = result.diagnostics.size();
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-INTERRUPTED", "freestanding object build was interrupted",
      "received termination signal " + std::to_string(signal_number),
      preserve_for_redelivery
        ? "the original signal is retained until the compiler process group and artifact "
          "transaction have left their cleanup boundary"
        : "original signal redelivery remains suppressed because compiler containment or "
          "toolchain lifecycle cleanup was not confirmed"));
  };
  const auto suppress_signal_redelivery = [&](std::string_view reason) {
    toolchain.mark_signal_redelivery_unsafe();
    result.interrupted_signal = 0;
    if (!interruption_recorded)
      record_interruption(termination_signals.intercepted_signal(), false);
    if (interruption_diagnostic_index.has_value()) {
      result.diagnostics[*interruption_diagnostic_index].impact =
        "original signal redelivery remains suppressed because " + std::string(reason);
    }
  };

  bool compiler_snapshot_retirement_attempted = false;
  auto retire_compiler_snapshot = [&]() {
    if (!toolchain.compiler_snapshot_active())
      return true;
    if (compiler_snapshot_retirement_attempted)
      return false;
    compiler_snapshot_retirement_attempted = true;
    std::string cleanup_detail;
    if (toolchain.cleanup_compiler_snapshot(cleanup_detail))
      return true;
    suppress_signal_redelivery(
      "the verified compiler snapshot did not retire on its first explicit cleanup attempt");
    result.failure = FreestandingObjectFailure::Infrastructure;
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP", "failed to retire the verified compiler snapshot", cleanup_detail,
      "no object or metadata was published because compiler lifecycle cleanup was incomplete"));
    return false;
  };
  const auto close_session_without_external_resources = [&]() {
    nebula::cli::FreestandingToolchainCloseResult close_result = toolchain.close_session();
    const int lifecycle_signal =
      close_result.interrupted_signal != 0 ? close_result.interrupted_signal
      : toolchain.session_active()         ? termination_signals.intercepted_signal()
                                           : 0;
    record_interruption(lifecycle_signal, close_result.ok() && toolchain.signal_redelivery_safe());
    if (!close_result.ok()) {
      suppress_signal_redelivery("the compiler or signal session did not close completely");
      result.failure = FreestandingObjectFailure::Infrastructure;
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP", "failed to close the freestanding toolchain session",
        close_result.detail,
        "no artifact was published, and compiler or caller signal cleanup is incomplete"));
    }
  };
  const auto finish_result = [&]() -> FreestandingObjectResult {
    if (!result.diagnostics.empty() && result.failure == FreestandingObjectFailure::None)
      result.failure = FreestandingObjectFailure::Build;
    return std::move(result);
  };
  const auto close_session_and_finish = [&]() -> FreestandingObjectResult {
    close_session_without_external_resources();
    return finish_result();
  };
  if (!toolchain.session_executable() || !termination_signals.ready_for_execution()) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-SIGNAL", "freestanding toolchain signal session is not executable",
      "the resolver-owned signal session closed or froze before the object transaction",
      "the build cannot guarantee cleanup before preserving caller termination semantics"));
    return close_session_and_finish();
  }
  if (termination_signals.intercepted_signal() != 0)
    return close_session_and_finish();

  const std::string expected_mode = request.mode == BuildMode::Release ? "release" : "debug";
  if (request.build_key.artifact_kind != "freestanding-object" ||
      request.build_key.runtime_profile != "system" || !request.build_key.no_std ||
      !request.build_key.strict_region ||
      request.build_key.target != nebula::cli::kFreestandingTargetTriple ||
      request.build_key.panic_policy != "trap" || request.build_key.mode != expected_mode) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-METADATA", "freestanding object request carries inconsistent artifact metadata",
      "artifact kind, system/no-std/strict profile, target, panic, and mode must match the request",
      "publishing mislabeled low-level output would break provenance and reuse checks"));
    return close_session_and_finish();
  }
  if (request.object_path.extension() != ".o") {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-OUTPUT", "freestanding object output must use the .o extension",
      "the first object gate has one explicit artifact naming contract",
      "an ambiguous output could be mistaken for a linked executable",
      {"choose an -o/--out path ending in .o"}));
    return close_session_and_finish();
  }

  std::string detail;
  const auto object_path_result =
    absolute_lexical_path(request.object_path, "object output path", detail);
  if (!object_path_result.has_value()) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-IO", "failed to resolve an absolute freestanding object path", detail,
      "path conflict and same-filesystem publication invariants could not be proven"));
    return close_session_and_finish();
  }
  const auto source_path_result =
    absolute_lexical_path(request.generated_source_path, "generated source path", detail);
  if (!source_path_result.has_value()) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-IO", "failed to resolve an absolute freestanding source path", detail,
      "path conflict and same-filesystem publication invariants could not be proven"));
    return close_session_and_finish();
  }
  const auto input_path_result =
    absolute_lexical_path(request.input_path, "input source path", detail);
  if (!input_path_result.has_value()) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-IO", "failed to resolve the absolute freestanding input path", detail,
      "the transaction could not prove that output paths are distinct from the input"));
    return close_session_and_finish();
  }
  const fs::path &object_path = *object_path_result;
  const fs::path &source_path = *source_path_result;
  const fs::path &input_path = *input_path_result;
  const fs::path metadata_path = artifact_metadata_path(object_path);
  if (object_path == source_path || object_path == input_path || source_path == input_path ||
      metadata_path == input_path || metadata_path == source_path) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-OUTPUT",
      "freestanding output paths conflict with each other or with the input source",
      "object, generated source, metadata, and input must be distinct files",
      "publishing the build could overwrite source or corrupt its own transaction"));
    return close_session_and_finish();
  }

  const fs::path parent = object_path.parent_path();
  if (parent.empty()) {
    result.diagnostics.push_back(
      make_fs_diagnostic("NBL-CLI-FS-IO", "freestanding output has no parent directory",
                         "the output path could not be normalized",
                         "a private same-filesystem staging area cannot be created"));
    return close_session_and_finish();
  }
  if (source_path.parent_path() != parent || metadata_path.parent_path() != parent) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-OUTPUT",
      "freestanding source, object, and metadata must share one output directory",
      "same-filesystem transactional publication is required",
      "cross-directory publication could expose a partial artifact set"));
    return close_session_and_finish();
  }

  std::error_code directory_error;
  fs::create_directories(parent, directory_error);
  if (directory_error) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-IO", "failed to create freestanding output directory", directory_error.message(),
      "the object build cannot create a private staging area"));
    return close_session_and_finish();
  }

  OutputLock output_lock;
  const fs::path lock_path = fs::path(object_path.string() + ".nebula.lock");
  OutputLockAcquisitionResult lock_acquisition = output_lock.acquire(lock_path);

  std::optional<fs::path> staging;
  std::vector<PublishedPath> published;
  published.reserve(3U);
  bool acquisition_cleanup_incomplete = false;
  StagingDirectoryGuard staging_guard(staging);
  PublishedRollbackGuard published_guard(published);
  auto finish_locked = [&]() -> FreestandingObjectResult {
    const nebula::cli::FreestandingToolchainPrepareResult prepare_result =
      toolchain.prepare_session_close();
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    if (toolchain.session_state() ==
        nebula::cli::FreestandingToolchainSessionState::PreparedFrozen) {
      nebula::cli::freestanding_transaction_testing::notify_phase(
        nebula::cli::freestanding_transaction_testing::Phase::SessionPrepared);
    }
#endif
    record_interruption(prepare_result.observed_signal,
                        prepare_result.ok() && toolchain.signal_redelivery_safe());
    if (!prepare_result.ok()) {
      suppress_signal_redelivery("the toolchain session could not be prepared for final cleanup");
      result.failure = FreestandingObjectFailure::Infrastructure;
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP", "failed to prepare the freestanding toolchain session for cleanup",
        prepare_result.detail,
        "artifact cleanup continues, but caller signal restoration cannot begin until compiler "
        "lifecycle preparation succeeds"));
    }

    bool external_cleanup_complete = !acquisition_cleanup_incomplete;
    StagingCleanupResult staging_cleanup;
    if (staging.has_value()) {
      staging_cleanup = cleanup_staging(*staging);
      // Once explicit cleanup has run its result is authoritative. Disarm the
      // exception fallback before any diagnostic or signal-policy allocation
      // can throw and accidentally trigger a second removal attempt.
      staging_guard.release();
      if (!staging_cleanup.complete()) {
        external_cleanup_complete = false;
        result.failure = FreestandingObjectFailure::Infrastructure;
      }
    } else {
      staging_guard.release();
    }
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::StagingCleanupFinished);
#endif

    const bool publication_complete =
      published.size() == 3U && std::all_of(published.begin(), published.end(),
                                            [](const PublishedPath &path) { return path.created; });
    const bool publication_state_inconsistent = result.failure == FreestandingObjectFailure::None &&
                                                result.diagnostics.empty() && !publication_complete;
    if (publication_state_inconsistent)
      result.failure = FreestandingObjectFailure::Infrastructure;
    const bool accept_publication = result.failure == FreestandingObjectFailure::None &&
                                    result.diagnostics.empty() && publication_complete;
    PublicationRollbackResult publication_rollback;
    if (accept_publication) {
      result.artifact_disposition = FreestandingArtifactDisposition::Committed;
    } else if (!published.empty()) {
      publication_rollback = rollback_published(published);
      if (!publication_rollback.complete()) {
        external_cleanup_complete = false;
        result.artifact_disposition = FreestandingArtifactDisposition::CleanupIncomplete;
        result.failure = FreestandingObjectFailure::Infrastructure;
      }
    }
    // Publication is either committed or its one explicit rollback attempt is
    // complete. Never leave the rollback guard armed across diagnostic work.
    published_guard.release();
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::PublicationFinalized);
#endif
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::GuardsDisarmed);
#endif

    if (!external_cleanup_complete)
      result.artifact_disposition = FreestandingArtifactDisposition::CleanupIncomplete;
    if (!staging_cleanup.complete())
      suppress_signal_redelivery("private staging cleanup did not complete");
    if (!publication_rollback.complete())
      suppress_signal_redelivery("published-path rollback did not complete");

    try {
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
      if (nebula::cli::freestanding_transaction_testing::take_fault(
            nebula::cli::freestanding_transaction_testing::Fault::PostCleanupDiagnostic)) {
        throw std::runtime_error("injected post-cleanup diagnostic rendering failure");
      }
#endif
      if (!staging_cleanup.complete())
        result.diagnostics.push_back(make_staging_cleanup_diagnostic(staging_cleanup));
      if (publication_state_inconsistent) {
        result.diagnostics.push_back(make_fs_diagnostic(
          "NBL-CLI-FS-PUBLISH", "freestanding publication ended in an inconsistent state",
          "the build reported success without all three source, metadata, and object paths",
          "any partial publication is rolled back instead of being reported as an absent "
          "success"));
      }
      if (!publication_rollback.complete()) {
        result.diagnostics.push_back(
          make_publication_rollback_diagnostic(publication_rollback, published));
      }
    } catch (const std::runtime_error &error) {
      suppress_signal_redelivery("post-cleanup diagnostic rendering did not complete");
      result.failure = FreestandingObjectFailure::Infrastructure;
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP", "failed to render freestanding cleanup diagnostics", error.what(),
        "cleanup guards were already disarmed, so no cleanup operation was retried"));
    }

    const OutputLockReleaseResult lock_release = output_lock.release();
    if (!lock_release.complete()) {
      external_cleanup_complete = false;
      result.artifact_disposition = FreestandingArtifactDisposition::CleanupIncomplete;
      suppress_signal_redelivery("the freestanding output lock release was not confirmed");
      result.failure = FreestandingObjectFailure::Infrastructure;
      std::string release_detail;
      if (lock_release.injected_confirmation_failure)
        release_detail = "injected output lock release confirmation failure";
      if (lock_release.unlock_error != 0) {
        release_detail =
          "output lock unlock failed: " + std::string(std::strerror(lock_release.unlock_error));
      }
      if (lock_release.close_error != 0) {
        if (!release_detail.empty())
          release_detail += "; ";
        release_detail +=
          "output lock close failed: " + std::string(std::strerror(lock_release.close_error));
      }
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP", "failed to release the freestanding output lock", release_detail,
        accept_publication
          ? "the artifact trio was published, but cleanup is indeterminate because the output "
            "lock release was not confirmed"
          : "artifact rollback may have completed, but cleanup is indeterminate because the "
            "output lock release was not confirmed"));
    }
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::OutputLockReleased);
#endif

    const nebula::cli::FreestandingExternalCleanup cleanup_disposition =
      external_cleanup_complete ? nebula::cli::FreestandingExternalCleanup::Complete
                                : nebula::cli::FreestandingExternalCleanup::Incomplete;
#if defined(NEBULA_FREESTANDING_TRANSACTION_TESTING)
    nebula::cli::freestanding_transaction_testing::notify_phase(
      nebula::cli::freestanding_transaction_testing::Phase::BeforeSessionFinalize);
#endif
    const nebula::cli::FreestandingToolchainCloseResult close_result =
      toolchain.finalize_session_close(cleanup_disposition);
    record_interruption(close_result.interrupted_signal,
                        close_result.ok() && toolchain.signal_redelivery_safe());
    if (!close_result.ok()) {
      suppress_signal_redelivery("the caller signal disposition could not be restored");
      result.failure = FreestandingObjectFailure::Infrastructure;
      std::string impact;
      switch (result.artifact_disposition) {
      case FreestandingArtifactDisposition::Absent:
        impact = "the publication was rolled back before caller signal restoration failed";
        break;
      case FreestandingArtifactDisposition::Committed:
        impact = "the complete artifact trio remains committed, but caller signal restoration "
                 "failed after publication cleanup completed";
        break;
      case FreestandingArtifactDisposition::CleanupIncomplete:
        impact = "artifact cleanup is incomplete and caller signal restoration also failed";
        break;
      }
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP", "failed to finalize the freestanding toolchain session",
        close_result.detail, std::move(impact)));
    }
    return finish_result();
  };

  if (lock_acquisition.status != LockStatus::Acquired) {
    if (lock_acquisition.status == LockStatus::CleanupIncomplete) {
      acquisition_cleanup_incomplete = true;
      result.failure = FreestandingObjectFailure::Infrastructure;
      result.artifact_disposition = FreestandingArtifactDisposition::CleanupIncomplete;
      suppress_signal_redelivery(
        "output lock acquisition rollback did not confirm descriptor cleanup");
      if (lock_acquisition.cleanup_error != 0) {
        lock_acquisition.detail += "; output lock close also failed: " +
                                   std::string(std::strerror(lock_acquisition.cleanup_error));
      }
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-CLEANUP",
        "failed to clean the freestanding output lock after acquisition was rejected",
        lock_acquisition.detail + "; lock path identity: " +
          describe_path_identity(lock_acquisition.path, lock_acquisition.identity),
        "no artifact was published, but output-lock descriptor cleanup is indeterminate"));
    } else {
      const bool busy = lock_acquisition.status == LockStatus::Busy;
      result.diagnostics.push_back(make_fs_diagnostic(
        busy ? "NBL-CLI-FS-BUSY" : "NBL-CLI-FS-LOCK",
        busy ? "another build owns this freestanding output"
             : "failed to acquire the freestanding output lock",
        lock_acquisition.detail,
        busy ? "concurrent publication is rejected instead of mixing object and metadata"
             : "the build cannot guarantee single-writer publication"));
    }
    return finish_locked();
  }

  for (const fs::path &output : {source_path, metadata_path, object_path}) {
    detail.clear();
    if (!path_is_absent(output, detail)) {
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-OUTPUT-EXISTS",
        "refusing to replace existing freestanding output: " + quote_cli_log_value(output.string()),
        detail,
        "the experimental transaction is no-replace so a failed rebuild cannot invalidate a prior "
        "object",
        {"choose a fresh --out path",
         "or explicitly remove the prior object, source, and metadata"}));
      return finish_locked();
    }
  }

  std::string forbidden_token;
  if (contains_forbidden_source_surface(request.translation_unit, forbidden_token)) {
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-SOURCE", "freestanding generated source contains a hosted or allocation surface",
      "generated token rejected by defense-in-depth scan: " + forbidden_token,
      "invoking clang could introduce an undeclared hosted dependency"));
    return finish_locked();
  }

  StagingAcquisitionResult staging_acquisition = create_staging_directory(parent);
  if (staging_acquisition.disposition != StagingAcquisitionDisposition::Acquired) {
    const bool cleanup_incomplete =
      staging_acquisition.disposition == StagingAcquisitionDisposition::CleanupIncomplete;
    const bool infrastructure_failure =
      cleanup_incomplete ||
      staging_acquisition.disposition == StagingAcquisitionDisposition::InfrastructureFailureClean;
    if (infrastructure_failure)
      result.failure = FreestandingObjectFailure::Infrastructure;
    if (cleanup_incomplete) {
      acquisition_cleanup_incomplete = true;
      result.artifact_disposition = FreestandingArtifactDisposition::CleanupIncomplete;
      suppress_signal_redelivery("private staging allocation rollback did not complete");
    }
    std::string cause = std::move(staging_acquisition.detail);
    if (!staging_acquisition.rollback.complete()) {
      cause += "; staging allocation rollback failed: " +
               describe_staging_rollback(staging_acquisition.rollback);
    }
    if (staging_acquisition.path.has_value()) {
      cause += "; staging path identity: " +
               describe_path_identity(*staging_acquisition.path, staging_acquisition.identity);
    }
    result.diagnostics.push_back(make_fs_diagnostic(
      cleanup_incomplete ? "NBL-CLI-FS-CLEANUP" : "NBL-CLI-FS-IO",
      cleanup_incomplete
        ? "failed to clean private freestanding staging after allocation was rejected"
        : "failed to create private freestanding staging directory",
      std::move(cause),
      cleanup_incomplete
        ? "no artifact was published, but the identity-bound private staging directory remains"
        : "the object cannot be built and inspected before publication"));
    return finish_locked();
  }
  staging = std::move(staging_acquisition.path);
  const fs::path staged_source = *staging / "unit.cpp";
  const fs::path compiler_object = *staging / "compiler-output.o";
  const fs::path staged_object = *staging / "verified.o";
  const fs::path staged_metadata = artifact_metadata_path(staged_object);

  auto fail_with_cleanup = [&](Diagnostic diagnostic) {
    result.diagnostics.push_back(std::move(diagnostic));
    return finish_locked();
  };

  if (!write_checked_file(staged_source, request.translation_unit, detail)) {
    return fail_with_cleanup(
      make_fs_diagnostic("NBL-CLI-FS-IO", "failed to write staged freestanding C++", detail,
                         "clang cannot compile a complete, closed generated source"));
  }
  if (termination_signals.intercepted_signal() != 0)
    return finish_locked();

  detail.clear();
  if (!toolchain.revalidate(detail)) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-TOOLCHAIN", "freestanding toolchain changed before compiler execution", detail,
      "the build refuses to execute a compiler outside the resolved immutable snapshot"));
  }

  std::vector<std::string> command = {
    toolchain.compiler().executable.string(),
    "--target=" + std::string(nebula::cli::kFreestandingTargetTriple),
    "--no-default-config",
    "-std=c++20",
    "-x",
    "c++",
    "-ffreestanding",
    "-nostdinc",
    "-nostdinc++",
  };
  for (const std::string_view argument : nebula::boot::kUosX86_64RequiredCompilerAbiArguments)
    command.emplace_back(argument);
  const std::vector<std::string> fixed_codegen_arguments = {
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
  };
  command.insert(command.end(), fixed_codegen_arguments.begin(), fixed_codegen_arguments.end());
  command.insert(command.end(), {
                                  request.mode == BuildMode::Release ? "-O2" : "-O0",
                                  "-c",
                                  "-o",
                                  compiler_object.string(),
                                  "--",
                                  staged_source.string(),
                                });
  const std::vector<std::string> compiler_environment = {
    "LC_ALL=C",
    "LANG=C",
    "TZ=UTC",
    "TMPDIR=" + staging->string(),
  };
  constexpr int kCompilerTimeoutSeconds = 30;
  const CommandExecutionResult compiler_result = compiler_executor.execute(
    command, compiler_environment, kCompilerTimeoutSeconds, termination_signals);
  if (compiler_result.containment == CompilerProcessContainment::Unconfirmed ||
      (termination_signals.intercepted_signal() != 0 &&
       compiler_result.containment != CompilerProcessContainment::Confirmed)) {
    toolchain.mark_signal_redelivery_unsafe();
  }
  const CompilerExecutionAssessment compiler_assessment =
    assess_compiler_execution(compiler_result, termination_signals.intercepted_signal());
  if (compiler_assessment.disposition != CompilerExecutionDisposition::Success) {
    if (compiler_assessment.disposition == CompilerExecutionDisposition::InfrastructureFailure) {
      result.failure = FreestandingObjectFailure::Infrastructure;
    } else if (compiler_assessment.disposition == CompilerExecutionDisposition::Timeout) {
      result.failure = FreestandingObjectFailure::Timeout;
    }
    if (compiler_assessment.disposition == CompilerExecutionDisposition::BuildFailure &&
        compiler_result.interrupted_signal != 0) {
      result.interrupted_signal = compiler_result.interrupted_signal;
    }
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-TOOLCHAIN", "clang++ failed to compile the freestanding object",
      compiler_assessment.detail, "no object or metadata was published"));
  }

  std::string revalidation_detail;
  const bool toolchain_revalidated = toolchain.revalidate(revalidation_detail);
  if (!toolchain_revalidated) {
    result.failure = FreestandingObjectFailure::Infrastructure;
    result.diagnostics.push_back(make_fs_diagnostic(
      "NBL-CLI-FS-TOOLCHAIN", "freestanding toolchain changed during compiler execution",
      revalidation_detail,
      "compiler output is rejected because its producer identity is no longer provable"));
  }

  const bool compiler_snapshot_cleaned = retire_compiler_snapshot();
  if (compiler_assessment.disposition != CompilerExecutionDisposition::Success ||
      !toolchain_revalidated || !compiler_snapshot_cleaned) {
    return finish_locked();
  }

  auto object_bytes = read_bounded_compiler_object(compiler_object, detail);
  if (!object_bytes.has_value()) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-OBJECT", "clang++ did not produce an inspectable regular object", detail,
      "no unverified compiler output was published"));
  }
  const auto inspection = nebula::cli::inspect_freestanding_elf64_x86_64(*object_bytes);
  if (!inspection.ok()) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-OBJECT", "freestanding ELF audit rejected compiler output",
      inspection.reason + ": " + inspection.detail,
      "the object did not prove the exact ELF64/x86_64/no-host-dependency contract"));
  }
  std::error_code compiler_output_remove_error;
  fs::remove(compiler_object, compiler_output_remove_error);
  if (compiler_output_remove_error) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-CLEANUP", "failed to retire untrusted compiler output",
      compiler_output_remove_error.message(),
      "the audited bytes were not copied into a compiler-independent staged inode"));
  }
  detail.clear();
  if (!write_exclusive_bytes(staged_object, *object_bytes, detail)) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-IO", "failed to create the verified freestanding object inode", detail,
      "the audited bytes could not be bound to an exclusive staged file"));
  }

  ArtifactMetadata metadata;
  metadata.build = request.build_key;
  metadata.content.size = static_cast<std::uint64_t>(object_bytes->size());
  metadata.content.sha256 = nebula::cli::sha256_hex(
    std::span<const std::uint8_t>(object_bytes->data(), object_bytes->size()));
  detail.clear();
  const bool metadata_written = write_artifact_metadata(staged_object, metadata, detail);
  if (!metadata_written) {
    return fail_with_cleanup(make_fs_diagnostic(
      "NBL-CLI-FS-METADATA", "failed to write complete freestanding artifact metadata", detail,
      "an object without content-bound metadata was not published"));
  }
  if (termination_signals.intercepted_signal() != 0)
    return finish_locked();

  for (const auto &[staged, destination] :
       std::vector<std::pair<fs::path, fs::path>>{{staged_source, source_path},
                                                  {staged_metadata, metadata_path},
                                                  {staged_object, object_path}}) {
    detail.clear();
    published.emplace_back();
    if (!publish_no_replace(staged, destination, published.back(), detail)) {
      result.diagnostics.push_back(make_fs_diagnostic(
        "NBL-CLI-FS-PUBLISH", "failed to publish freestanding artifact transaction", detail,
        "no final object commit was accepted"));
      return finish_locked();
    }
  }

  result.failure = FreestandingObjectFailure::None;
  return finish_locked();
}

#endif
