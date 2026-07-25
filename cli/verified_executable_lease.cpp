#include "verified_executable_lease.hpp"
#include "path_security.hpp"
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
#include "verified_executable_lease_test_hooks.hpp"
#endif
#if defined(_WIN32)
#include "windows_object_identity.hpp"
#include "windows_private_security.hpp"
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <span>
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
#include <stdexcept>
#endif
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bcrypt.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdlib.h>
#include <sys/xattr.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace nebula::cli {

#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
namespace verified_executable_lease_testing {
namespace {

struct InjectedAcquisitionException {
  AcquisitionExceptionPoint point;
  AcquisitionExceptionSetup setup = nullptr;
};

std::optional<InjectedAcquisitionException> injected_acquisition_exception;
std::optional<AcquisitionExceptionSetup> injected_acquisition_failure_setup;
bool injected_post_rollback_diagnostic_exception = false;
bool injected_cleanup_failure = false;
std::optional<AcquisitionExceptionSetup> injected_post_deletion_cleanup_setup;

} // namespace

void inject_acquisition_exception_once(AcquisitionExceptionPoint point,
                                       AcquisitionExceptionSetup setup) {
  injected_acquisition_exception = InjectedAcquisitionException{point, setup};
}

bool acquisition_exception_injection_pending() noexcept {
  return injected_acquisition_exception.has_value();
}

void inject_acquisition_failure_once(AcquisitionExceptionSetup setup) {
  injected_acquisition_failure_setup = setup;
}

bool acquisition_failure_injection_pending() noexcept {
  return injected_acquisition_failure_setup.has_value();
}

void inject_post_rollback_diagnostic_exception_once() noexcept {
  injected_post_rollback_diagnostic_exception = true;
}

bool post_rollback_diagnostic_exception_pending() noexcept {
  return injected_post_rollback_diagnostic_exception;
}

void inject_cleanup_failure_once() noexcept { injected_cleanup_failure = true; }

bool cleanup_failure_injection_pending() noexcept { return injected_cleanup_failure; }

void inject_post_deletion_cleanup_setup_once(AcquisitionExceptionSetup setup) {
  injected_post_deletion_cleanup_setup = setup;
}

bool post_deletion_cleanup_setup_pending() noexcept {
  return injected_post_deletion_cleanup_setup.has_value();
}

std::optional<InjectedAcquisitionException>
take_acquisition_exception(AcquisitionExceptionPoint point) noexcept {
  if (!injected_acquisition_exception.has_value() ||
      injected_acquisition_exception->point != point) {
    return std::nullopt;
  }
  std::optional<InjectedAcquisitionException> result = injected_acquisition_exception;
  injected_acquisition_exception.reset();
  return result;
}

std::optional<AcquisitionExceptionSetup> take_acquisition_failure() noexcept {
  const std::optional<AcquisitionExceptionSetup> result = injected_acquisition_failure_setup;
  injected_acquisition_failure_setup.reset();
  return result;
}

bool take_post_rollback_diagnostic_exception() noexcept {
  const bool result = injected_post_rollback_diagnostic_exception;
  injected_post_rollback_diagnostic_exception = false;
  return result;
}

bool take_cleanup_failure() noexcept {
  const bool result = injected_cleanup_failure;
  injected_cleanup_failure = false;
  return result;
}

std::optional<AcquisitionExceptionSetup> take_post_deletion_cleanup_setup() noexcept {
  const std::optional<AcquisitionExceptionSetup> result = injected_post_deletion_cleanup_setup;
  injected_post_deletion_cleanup_setup.reset();
  return result;
}

} // namespace verified_executable_lease_testing
#endif

namespace {

namespace fs = std::filesystem;

static_assert(std::is_nothrow_move_constructible_v<VerifiedExecutableLeaseBeginResult>);
static_assert(std::is_nothrow_move_constructible_v<VerifiedExecutableLeaseResult>);

VerifiedExecutableLeaseError make_error(VerifiedExecutableLeaseErrorCode code, fs::path path,
                                        std::string operation, std::string detail) {
  return {code, std::move(path), std::move(operation), std::move(detail)};
}

VerifiedExecutableLeaseBeginResult begin_failure(VerifiedExecutableLeaseErrorCode code,
                                                 const fs::path &path, std::string operation,
                                                 std::string detail) {
  VerifiedExecutableLeaseBeginResult result;
  result.error = make_error(code, path, std::move(operation), std::move(detail));
  return result;
}

VerifiedExecutableLeaseResult cleanup_failure(VerifiedExecutableLeaseErrorCode code,
                                              const fs::path &path, std::string operation,
                                              std::string detail) {
  return {make_error(code, path, std::move(operation), std::move(detail)),
          VerifiedExecutableLeaseCleanupDisposition::Incomplete};
}

VerifiedExecutableLeaseResult cleanup_conflict_after_owned_cleanup(const fs::path &path,
                                                                   std::string operation,
                                                                   std::string detail) {
  return {make_error(VerifiedExecutableLeaseErrorCode::ConcurrentModification, path,
                     std::move(operation), std::move(detail)),
          VerifiedExecutableLeaseCleanupDisposition::Complete};
}

bool same_digest(const FileDigest &left, const FileDigest &right) {
  return left.size == right.size && left.sha256 == right.sha256;
}

struct NormalizedExecutablePath {
  fs::path parent;
  fs::path path;
  fs::path filename;
};

std::optional<NormalizedExecutablePath> normalize_executable_path(const fs::path &input,
                                                                  std::string &detail) {
  detail.clear();
  if (input.empty()) {
    detail = "executable path is empty";
    return std::nullopt;
  }
  std::error_code error;
  fs::path absolute = fs::absolute(input, error).lexically_normal();
  if (error || absolute.empty() || !absolute.is_absolute()) {
    detail = error ? "could not make executable path absolute: " + error.message()
                   : "executable path did not normalize to an absolute path";
    return std::nullopt;
  }
  const fs::path filename = absolute.filename();
  if (filename.empty() || filename == "." || filename == "..") {
    detail = "executable path must name one file";
    return std::nullopt;
  }
  const fs::path parent_input = absolute.parent_path();
  const fs::path parent = fs::canonical(parent_input, error);
  if (error || parent.empty() || !parent.is_absolute()) {
    detail = error ? "could not canonicalize executable parent: " + error.message()
                   : "executable parent did not resolve to an absolute path";
    return std::nullopt;
  }
  return NormalizedExecutablePath{parent, parent / filename, filename};
}

std::atomic<std::uint64_t> lease_sequence{0U};

std::string lease_filename(std::uint64_t nonce, std::uint64_t attempt,
                           std::string_view suffix = {}) {
  constexpr char hex[] = "0123456789abcdef";
  std::array<char, 16U> encoded{};
  for (std::size_t index = 0U; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - 1U - index) * 4U;
    encoded[index] = hex[(nonce >> shift) & 0x0fU];
  }
  const std::uint64_t sequence = lease_sequence.fetch_add(1U, std::memory_order_relaxed);
  return ".nebula-exec-" + std::string(encoded.data(), encoded.size()) + "-" +
         std::to_string(sequence) + "-" + std::to_string(attempt) + std::string(suffix);
}

enum class AcquisitionRollbackFailure : std::uint8_t {
  None,
  InspectCreatedObject,
  OpenPath,
  InspectPath,
  IdentityChanged,
  CloseAcquisitionHandle,
  BindDeletion,
  InspectDeletion,
  MarkDeletion,
  CloseDeletion,
  VerifyRemoval,
  RemovePath,
};

struct AcquisitionRollbackResult {
  AcquisitionRollbackFailure failure = AcquisitionRollbackFailure::None;
  std::uint64_t native_error = 0U;

  [[nodiscard]] bool ok() const noexcept { return failure == AcquisitionRollbackFailure::None; }
};

[[noreturn]] void fail_fast_with_fixed_message(const char *message,
                                               std::size_t message_size) noexcept {
#if defined(_WIN32)
  const HANDLE standard_error = ::GetStdHandle(STD_ERROR_HANDLE);
  if (standard_error != nullptr && standard_error != INVALID_HANDLE_VALUE) {
    DWORD written = 0U;
    (void)::WriteFile(standard_error, message, static_cast<DWORD>(message_size), &written, nullptr);
  }
  (void)::TerminateProcess(::GetCurrentProcess(), 125U);
  ::ExitProcess(125U);
#else
  const char *cursor = message;
  std::size_t remaining = message_size;
  while (remaining != 0U) {
    const ssize_t written = ::write(STDERR_FILENO, cursor, remaining);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      break;
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  ::_exit(125);
#endif
}

[[noreturn]] void fail_fast_after_acquisition_rollback() noexcept {
  constexpr char message[] =
    "nebula: fatal: identity-bound executable lease acquisition rollback failed\n";
  fail_fast_with_fixed_message(message, sizeof(message) - 1U);
}

[[noreturn]] void fail_fast_after_lease_cleanup() noexcept {
  constexpr char message[] = "nebula: fatal: verified executable lease cleanup failed\n";
  fail_fast_with_fixed_message(message, sizeof(message) - 1U);
}

#if defined(_WIN32)

std::string windows_error(DWORD error) {
  return std::system_category().message(static_cast<int>(error));
}

class UniqueHandle final {
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
  [[nodiscard]] bool close(std::string &detail, std::string_view operation) {
    if (!valid())
      return true;
    if (::CloseHandle(handle_) != 0) {
      handle_ = nullptr;
      return true;
    }
    if (!detail.empty())
      detail += "; ";
    detail += std::string(operation) + ": " + windows_error(::GetLastError());
    return false;
  }
  [[nodiscard]] DWORD close_noexcept() noexcept {
    if (!valid())
      return ERROR_SUCCESS;
    if (::CloseHandle(handle_) != 0) {
      handle_ = nullptr;
      return ERROR_SUCCESS;
    }
    return ::GetLastError();
  }
  void reset(HANDLE replacement = nullptr) noexcept {
    if (valid())
      (void)::CloseHandle(handle_);
    handle_ = replacement;
  }

private:
  HANDLE handle_ = nullptr;
};

struct WindowsFileIdentity {
  WindowsObjectIdentity object;
  std::uint64_t size = 0U;
  DWORD attributes = 0U;
};

enum class WindowsFileInspectionFailure : std::uint8_t {
  None,
  Metadata,
  UnsafeType,
  StableIdentity,
};

struct WindowsFileInspection {
  WindowsFileIdentity identity;
  WindowsFileInspectionFailure failure = WindowsFileInspectionFailure::None;
  DWORD native_error = ERROR_SUCCESS;

  [[nodiscard]] bool ok() const noexcept { return failure == WindowsFileInspectionFailure::None; }
};

enum class WindowsLeaseHandlePhase : std::uint8_t {
  ExecutionLocked,
  CleanupTransition,
  DeletionBound,
  DeletionMarked,
  ParentOnly,
};

WindowsFileInspection inspect_windows_file_noexcept(HANDLE handle) noexcept {
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle, &information) == 0) {
    return {{}, WindowsFileInspectionFailure::Metadata, ::GetLastError()};
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return {{}, WindowsFileInspectionFailure::UnsafeType, ERROR_SUCCESS};
  }
  const std::uint64_t size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
                             static_cast<std::uint64_t>(information.nFileSizeLow);
  WindowsObjectIdentity object;
  const DWORD identity_error = read_windows_object_identity(handle, object);
  if (identity_error != ERROR_SUCCESS) {
    return {{}, WindowsFileInspectionFailure::StableIdentity, identity_error};
  }
  return {WindowsFileIdentity{object, size, information.dwFileAttributes},
          WindowsFileInspectionFailure::None, ERROR_SUCCESS};
}

std::optional<WindowsFileIdentity> inspect_windows_file(HANDLE handle, std::string &detail) {
  const WindowsFileInspection inspection = inspect_windows_file_noexcept(handle);
  if (inspection.ok())
    return inspection.identity;
  switch (inspection.failure) {
  case WindowsFileInspectionFailure::Metadata:
    detail = "failed to inspect executable handle: " + windows_error(inspection.native_error);
    break;
  case WindowsFileInspectionFailure::UnsafeType:
    detail = "executable is a directory or reparse point";
    break;
  case WindowsFileInspectionFailure::StableIdentity:
    detail = "failed to read stable executable identity: " + windows_error(inspection.native_error);
    break;
  case WindowsFileInspectionFailure::None:
    break;
  }
  return std::nullopt;
}

bool same_windows_object(const WindowsFileIdentity &left, const WindowsFileIdentity &right) {
  return left.object == right.object;
}

bool random_nonce(std::uint64_t &nonce, std::string &detail) {
  const NTSTATUS status = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce),
                                            sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status == 0)
    return true;
  detail = "Windows system random-number generation failed";
  return false;
}

bool mark_windows_file_for_deletion(HANDLE handle, std::string &detail) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (::SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                   sizeof(disposition)) != 0) {
    return true;
  }
  detail = "failed to mark executable lease for identity-bound deletion: " +
           windows_error(::GetLastError());
  return false;
}

bool windows_path_is_absent(const fs::path &path, std::string &detail) {
  if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
    return false;
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    return true;
  detail = "failed to verify executable lease removal: " + windows_error(error);
  return false;
}

AcquisitionRollbackResult
rollback_acquired_snapshot(const fs::path &snapshot_path,
                           const WindowsFileIdentity &expected_identity,
                           std::span<UniqueHandle *const> acquisition_handles) noexcept {
  // path_reader requests only GENERIC_READ, which is permitted by the retained
  // execution handle's FILE_SHARE_READ policy. Its own FILE_SHARE_DELETE flag
  // lets the same verified object remain bound while the non-delete-sharing
  // execution handle is closed and a deletion handle is reopened. Binding
  // before that close prevents a replacement race during the transition.
  UniqueHandle path_reader(::CreateFileW(
    snapshot_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!path_reader.valid()) {
    const DWORD open_error = ::GetLastError();
    if (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND)
      return {};
    return {AcquisitionRollbackFailure::OpenPath, open_error};
  }

  const WindowsFileInspection path_identity = inspect_windows_file_noexcept(path_reader.get());
  if (!path_identity.ok()) {
    const DWORD close_error = path_reader.close_noexcept();
    if (close_error != ERROR_SUCCESS)
      return {AcquisitionRollbackFailure::CloseAcquisitionHandle, close_error};
    return {AcquisitionRollbackFailure::InspectPath, path_identity.native_error};
  }
  if (!same_windows_object(path_identity.identity, expected_identity) ||
      path_identity.identity.size != expected_identity.size) {
    const DWORD close_error = path_reader.close_noexcept();
    if (close_error != ERROR_SUCCESS)
      return {AcquisitionRollbackFailure::CloseAcquisitionHandle, close_error};
    return {AcquisitionRollbackFailure::IdentityChanged, ERROR_SUCCESS};
  }

  DWORD first_close_error = ERROR_SUCCESS;
  for (UniqueHandle *handle : acquisition_handles) {
    if (handle == nullptr || !handle->valid())
      continue;
    const DWORD close_error = handle->close_noexcept();
    if (first_close_error == ERROR_SUCCESS && close_error != ERROR_SUCCESS)
      first_close_error = close_error;
  }
  if (first_close_error != ERROR_SUCCESS) {
    (void)path_reader.close_noexcept();
    return {AcquisitionRollbackFailure::CloseAcquisitionHandle, first_close_error};
  }

  UniqueHandle deletion(::ReOpenFile(path_reader.get(), GENERIC_READ | DELETE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT));
  if (!deletion.valid()) {
    const DWORD deletion_error = ::GetLastError();
    (void)path_reader.close_noexcept();
    return {AcquisitionRollbackFailure::BindDeletion, deletion_error};
  }
  const WindowsFileInspection deletion_identity = inspect_windows_file_noexcept(deletion.get());
  if (!deletion_identity.ok()) {
    (void)deletion.close_noexcept();
    (void)path_reader.close_noexcept();
    return {AcquisitionRollbackFailure::InspectDeletion, deletion_identity.native_error};
  }
  if (!same_windows_object(deletion_identity.identity, expected_identity) ||
      deletion_identity.identity.size != expected_identity.size) {
    (void)deletion.close_noexcept();
    (void)path_reader.close_noexcept();
    return {AcquisitionRollbackFailure::IdentityChanged, ERROR_SUCCESS};
  }

  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (::SetFileInformationByHandle(deletion.get(), FileDispositionInfo, &disposition,
                                   sizeof(disposition)) == 0) {
    const DWORD deletion_error = ::GetLastError();
    (void)deletion.close_noexcept();
    (void)path_reader.close_noexcept();
    return {AcquisitionRollbackFailure::MarkDeletion, deletion_error};
  }

  const DWORD deletion_close_error = deletion.close_noexcept();
  const DWORD reader_close_error = path_reader.close_noexcept();
  if (deletion_close_error != ERROR_SUCCESS)
    return {AcquisitionRollbackFailure::CloseDeletion, deletion_close_error};
  if (reader_close_error != ERROR_SUCCESS)
    return {AcquisitionRollbackFailure::CloseDeletion, reader_close_error};

  if (::GetFileAttributesW(snapshot_path.c_str()) != INVALID_FILE_ATTRIBUTES)
    return {AcquisitionRollbackFailure::IdentityChanged, ERROR_SUCCESS};
  const DWORD absence_error = ::GetLastError();
  if (absence_error != ERROR_FILE_NOT_FOUND && absence_error != ERROR_PATH_NOT_FOUND)
    return {AcquisitionRollbackFailure::VerifyRemoval, absence_error};
  return {};
}

class ExecutableLeaseAcquisitionRollback final {
public:
  explicit ExecutableLeaseAcquisitionRollback(const fs::path &snapshot_path) noexcept
      : snapshot_path_(&snapshot_path) {}
  ExecutableLeaseAcquisitionRollback(const ExecutableLeaseAcquisitionRollback &) = delete;
  ExecutableLeaseAcquisitionRollback &
  operator=(const ExecutableLeaseAcquisitionRollback &) = delete;
  ~ExecutableLeaseAcquisitionRollback() noexcept {
    if (!armed_)
      return;
    if (rollback_attempted_)
      fail_fast_after_acquisition_rollback();
    if (!rollback().ok())
      fail_fast_after_acquisition_rollback();
  }

  [[nodiscard]] AcquisitionRollbackResult arm(UniqueHandle &created_handle) noexcept {
    armed_ = true;
    track(created_handle);
    const WindowsFileInspection inspection = inspect_windows_file_noexcept(created_handle.get());
    if (!inspection.ok()) {
      return {AcquisitionRollbackFailure::InspectCreatedObject, inspection.native_error};
    }
    expected_identity_ = inspection.identity;
    identity_bound_ = true;
    return {};
  }

  void track(UniqueHandle &handle) noexcept {
    for (std::size_t index = 0U; index < handle_count_; ++index) {
      if (handles_[index] == &handle)
        return;
    }
    if (handle_count_ < handles_.size())
      handles_[handle_count_++] = &handle;
  }

  [[nodiscard]] AcquisitionRollbackResult rollback() noexcept {
    if (rollback_attempted_)
      return rollback_result_;
    rollback_attempted_ = true;
    AcquisitionRollbackResult result;
    if (!identity_bound_) {
      result = {AcquisitionRollbackFailure::InspectCreatedObject, ERROR_INVALID_HANDLE};
      for (std::size_t index = 0U; index < handle_count_; ++index) {
        UniqueHandle *handle = handles_[index];
        if (handle == nullptr || !handle->valid())
          continue;
        const WindowsFileInspection inspection = inspect_windows_file_noexcept(handle->get());
        if (!inspection.ok()) {
          result = {AcquisitionRollbackFailure::InspectCreatedObject, inspection.native_error};
          break;
        }
        expected_identity_ = inspection.identity;
        identity_bound_ = true;
        break;
      }
    }
    if (identity_bound_) {
      result =
        rollback_acquired_snapshot(*snapshot_path_, expected_identity_,
                                   std::span<UniqueHandle *const>(handles_.data(), handle_count_));
    }
    rollback_result_ = result;
    if (rollback_result_.ok())
      armed_ = false;
    return rollback_result_;
  }

  void disarm() noexcept { armed_ = false; }

private:
  const fs::path *snapshot_path_ = nullptr;
  std::array<UniqueHandle *, 3U> handles_{};
  std::size_t handle_count_ = 0U;
  WindowsFileIdentity expected_identity_{};
  AcquisitionRollbackResult rollback_result_{};
  bool identity_bound_ = false;
  bool rollback_attempted_ = false;
  bool armed_ = false;
};

bool validate_windows_execution_policy(const fs::path &source_path, std::string &detail) {
  const fs::path zone_identifier(source_path.native() + L":Zone.Identifier");
  UniqueHandle zone(::CreateFileW(zone_identifier.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (zone.valid()) {
    detail = "executable carries a Zone.Identifier security label; use the Windows-approved "
             "unblock flow before requesting a private execution lease";
    if (::CloseHandle(zone.release()) == 0) {
      detail +=
        "; failed to close Zone.Identifier inspection handle: " + windows_error(::GetLastError());
    }
    return false;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
      error == ERROR_INVALID_NAME || error == ERROR_NOT_SUPPORTED) {
    return true;
  }
  detail = "could not verify executable Zone.Identifier policy: " + windows_error(error);
  return false;
}

#else

class UniqueFd final {
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
  [[nodiscard]] bool close(std::string &detail, std::string_view operation) {
    if (!valid())
      return true;
    const int descriptor = release();
    if (::close(descriptor) == 0)
      return true;
    if (!detail.empty())
      detail += "; ";
    detail += std::string(operation) + ": " + std::strerror(errno);
    return false;
  }
  void reset(int replacement = -1) noexcept {
    if (valid())
      (void)::close(descriptor_);
    descriptor_ = replacement;
  }

private:
  int descriptor_ = -1;
};

bool same_timespec(const timespec &left, const timespec &right) {
  return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

const timespec &modified_time(const struct stat &state) {
#if defined(__APPLE__)
  return state.st_mtimespec;
#else
  return state.st_mtim;
#endif
}

const timespec &changed_time(const struct stat &state) {
#if defined(__APPLE__)
  return state.st_ctimespec;
#else
  return state.st_ctim;
#endif
}

bool same_posix_object(const struct stat &left, const struct stat &right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

AcquisitionRollbackResult
rollback_acquired_snapshot(int parent_descriptor, const fs::path &snapshot_filename,
                           const struct stat &expected_identity) noexcept {
  struct stat path_state{};
  if (::fstatat(parent_descriptor, snapshot_filename.c_str(), &path_state, AT_SYMLINK_NOFOLLOW) !=
      0) {
    const int inspection_error = errno;
    if (inspection_error == ENOENT)
      return {};
    return {AcquisitionRollbackFailure::InspectPath, static_cast<std::uint64_t>(inspection_error)};
  }
  if (!same_posix_object(path_state, expected_identity))
    return {AcquisitionRollbackFailure::IdentityChanged, 0U};
  if (::unlinkat(parent_descriptor, snapshot_filename.c_str(), 0) != 0) {
    return {AcquisitionRollbackFailure::RemovePath, static_cast<std::uint64_t>(errno)};
  }
  return {};
}

class ExecutableLeaseAcquisitionRollback final {
public:
  ExecutableLeaseAcquisitionRollback(UniqueFd rollback_parent,
                                     const fs::path &snapshot_filename) noexcept
      : rollback_parent_(std::move(rollback_parent)), snapshot_filename_(&snapshot_filename) {}
  ExecutableLeaseAcquisitionRollback(const ExecutableLeaseAcquisitionRollback &) = delete;
  ExecutableLeaseAcquisitionRollback &
  operator=(const ExecutableLeaseAcquisitionRollback &) = delete;
  ~ExecutableLeaseAcquisitionRollback() noexcept {
    if (!armed_)
      return;
    if (rollback_attempted_)
      fail_fast_after_acquisition_rollback();
    if (!rollback().ok())
      fail_fast_after_acquisition_rollback();
  }

  [[nodiscard]] AcquisitionRollbackResult arm(int created_descriptor) noexcept {
    armed_ = true;
    created_descriptor_ = created_descriptor;
    if (::fstat(created_descriptor_, &expected_identity_) != 0) {
      return {AcquisitionRollbackFailure::InspectCreatedObject, static_cast<std::uint64_t>(errno)};
    }
    identity_bound_ = true;
    return {};
  }

  [[nodiscard]] AcquisitionRollbackResult rollback() noexcept {
    if (rollback_attempted_)
      return rollback_result_;
    rollback_attempted_ = true;
    AcquisitionRollbackResult result;
    if (!identity_bound_) {
      if (::fstat(created_descriptor_, &expected_identity_) != 0) {
        result = {AcquisitionRollbackFailure::InspectCreatedObject,
                  static_cast<std::uint64_t>(errno)};
      } else {
        identity_bound_ = true;
      }
    }
    if (identity_bound_) {
      result =
        rollback_acquired_snapshot(rollback_parent_.get(), *snapshot_filename_, expected_identity_);
    }
    rollback_result_ = result;
    if (rollback_result_.ok())
      armed_ = false;
    return rollback_result_;
  }

  void disarm() noexcept { armed_ = false; }

private:
  UniqueFd rollback_parent_;
  const fs::path *snapshot_filename_ = nullptr;
  int created_descriptor_ = -1;
  struct stat expected_identity_{};
  AcquisitionRollbackResult rollback_result_{};
  bool identity_bound_ = false;
  bool rollback_attempted_ = false;
  bool armed_ = false;
};

bool same_posix_source_state(const struct stat &left, const struct stat &right) {
  return same_posix_object(left, right) && left.st_mode == right.st_mode &&
         left.st_nlink == right.st_nlink && left.st_size == right.st_size &&
         same_timespec(modified_time(left), modified_time(right)) &&
         same_timespec(changed_time(left), changed_time(right));
}

bool random_nonce(std::uint64_t &nonce, std::string &detail) {
#if defined(__APPLE__)
  (void)detail;
  ::arc4random_buf(&nonce, sizeof(nonce));
  return true;
#elif defined(__linux__)
  std::byte *bytes = reinterpret_cast<std::byte *>(&nonce);
  std::size_t offset = 0U;
  while (offset < sizeof(nonce)) {
    const ssize_t count = ::getrandom(bytes + offset, sizeof(nonce) - offset, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      detail = "system random-number generation failed: " + std::string(std::strerror(errno));
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
#else
  try {
    std::random_device random;
    nonce = (static_cast<std::uint64_t>(random()) << 32U) ^ static_cast<std::uint64_t>(random());
    return true;
  } catch (const std::exception &error) {
    detail = "system random-number generation failed: " + std::string(error.what());
    return false;
  }
#endif
}

bool write_all(int descriptor, std::span<const std::uint8_t> bytes, std::string &detail) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      detail = count < 0 ? "failed to write executable lease: " + std::string(std::strerror(errno))
                         : "executable lease write made no progress";
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool validate_posix_execution_policy(int parent_descriptor, const fs::path &filename,
                                     int source_descriptor, std::string &detail) {
  if (::faccessat(parent_descriptor, filename.c_str(), X_OK, AT_EACCESS) != 0) {
    detail = "effective caller is not permitted to execute the source artifact: " +
             std::string(std::strerror(errno));
    return false;
  }
#if defined(__APPLE__)
  errno = 0;
  const ssize_t quarantine_size =
    ::fgetxattr(source_descriptor, "com.apple.quarantine", nullptr, 0U, 0U, 0);
  if (quarantine_size >= 0) {
    detail = "executable carries com.apple.quarantine; use the macOS-approved review or "
             "quarantine-removal flow before requesting a private execution lease";
    return false;
  }
  if (errno != ENOATTR && errno != ENOTSUP) {
    detail = "could not verify executable quarantine policy: " + std::string(std::strerror(errno));
    return false;
  }
#else
  (void)source_descriptor;
#endif
  return true;
}

#endif

std::string acquisition_rollback_detail(const AcquisitionRollbackResult &rollback) {
  std::string detail;
  switch (rollback.failure) {
  case AcquisitionRollbackFailure::None:
    return detail;
  case AcquisitionRollbackFailure::InspectCreatedObject:
    detail = "could not bind the created private lease identity for rollback";
    break;
  case AcquisitionRollbackFailure::OpenPath:
    detail = "could not open the private lease path for identity-bound rollback";
    break;
  case AcquisitionRollbackFailure::InspectPath:
    detail = "could not inspect the private lease path for identity-bound rollback";
    break;
  case AcquisitionRollbackFailure::IdentityChanged:
    return "private lease path was replaced and the replacement was preserved";
  case AcquisitionRollbackFailure::CloseAcquisitionHandle:
    detail = "could not close an acquisition handle before identity-bound rollback";
    break;
  case AcquisitionRollbackFailure::BindDeletion:
    detail = "could not bind the private lease identity for deletion";
    break;
  case AcquisitionRollbackFailure::InspectDeletion:
    detail = "could not revalidate the private lease deletion identity";
    break;
  case AcquisitionRollbackFailure::MarkDeletion:
    detail = "could not mark the identity-bound private lease for deletion";
    break;
  case AcquisitionRollbackFailure::CloseDeletion:
    detail = "could not close an identity-bound private lease deletion handle";
    break;
  case AcquisitionRollbackFailure::VerifyRemoval:
    detail = "could not verify identity-bound private lease removal";
    break;
  case AcquisitionRollbackFailure::RemovePath:
    detail = "identity-checked private lease removal failed";
    break;
  }
  if (rollback.native_error != 0U) {
#if defined(_WIN32)
    detail += ": " + windows_error(static_cast<DWORD>(rollback.native_error));
#else
    detail += ": " + std::string(std::strerror(static_cast<int>(rollback.native_error)));
#endif
  }
  return detail;
}

VerifiedExecutableLeaseBeginResult begin_failure_after_snapshot_cleanup(
  ExecutableLeaseAcquisitionRollback &rollback_guard, const fs::path &snapshot_path,
  VerifiedExecutableLeaseErrorCode primary_code, const fs::path &primary_path,
  std::string primary_operation, std::string primary_detail) {
  const AcquisitionRollbackResult rollback = rollback_guard.rollback();
  if (!rollback.ok()) {
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
    if (verified_executable_lease_testing::take_post_rollback_diagnostic_exception()) {
      throw std::runtime_error(std::string(
        verified_executable_lease_testing::kInjectedPostRollbackDiagnosticExceptionDetail));
    }
#endif
    VerifiedExecutableLeaseBeginResult failure =
      begin_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete, snapshot_path,
                    "rollback-executable-lease",
                    std::move(primary_operation) + " failed: " + std::move(primary_detail) + "; " +
                      acquisition_rollback_detail(rollback));
    rollback_guard.disarm();
    return failure;
  }
  return begin_failure(primary_code, primary_path, std::move(primary_operation),
                       std::move(primary_detail));
}

#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
void throw_injected_acquisition_exception(
  verified_executable_lease_testing::AcquisitionExceptionPoint point,
  const fs::path &snapshot_path) {
  const auto injected = verified_executable_lease_testing::take_acquisition_exception(point);
  if (!injected.has_value())
    return;
  if (injected->setup != nullptr)
    injected->setup(snapshot_path);
  throw std::runtime_error(
    std::string(verified_executable_lease_testing::kInjectedAcquisitionExceptionDetail));
}
#endif

} // namespace

struct VerifiedExecutableLease::Impl {
  fs::path public_path;
  fs::path execution_path;
  fs::path filename;
  FileDigest content;
  bool cleaned = false;
#if defined(_WIN32)
  UniqueHandle parent_handle;
  UniqueHandle snapshot_handle;
  WindowsFileIdentity snapshot_identity;
  WindowsLeaseHandlePhase handle_phase = WindowsLeaseHandlePhase::ExecutionLocked;
#else
  UniqueFd parent_fd;
  UniqueFd snapshot_fd;
  struct stat snapshot_identity{};
#endif
};

VerifiedExecutableLease::VerifiedExecutableLease(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {}

VerifiedExecutableLease::~VerifiedExecutableLease() noexcept {
  if (!impl_ || impl_->cleaned)
    return;
  try {
    if (!cleanup().owned_cleanup_complete())
      fail_fast_after_lease_cleanup();
  } catch (...) {
    fail_fast_after_lease_cleanup();
  }
}

const fs::path &VerifiedExecutableLease::public_path() const noexcept { return impl_->public_path; }

const fs::path &VerifiedExecutableLease::execution_path() const noexcept {
  return impl_->execution_path;
}

const FileDigest &VerifiedExecutableLease::content() const noexcept { return impl_->content; }

bool VerifiedExecutableLease::active() const noexcept {
  return impl_ != nullptr && !impl_->cleaned;
}

bool VerifiedExecutableLease::revalidate(std::string &detail) const {
  detail.clear();
  if (!impl_ || impl_->cleaned) {
    detail = "verified executable lease is not active";
    return false;
  }
#if defined(_WIN32)
  if (impl_->handle_phase != WindowsLeaseHandlePhase::ExecutionLocked) {
    detail = "verified executable lease cleanup has already started";
    return false;
  }
  std::string inspection_detail;
  const auto current = inspect_windows_file(impl_->snapshot_handle.get(), inspection_detail);
  if (!current.has_value() || !same_windows_object(*current, impl_->snapshot_identity) ||
      current->size != impl_->snapshot_identity.size) {
    detail = inspection_detail.empty()
               ? "verified executable lease identity changed before process launch"
               : std::move(inspection_detail);
    return false;
  }
#else
  struct stat descriptor_state{};
  struct stat path_state{};
  if (::fstat(impl_->snapshot_fd.get(), &descriptor_state) != 0 ||
      ::fstatat(impl_->parent_fd.get(), impl_->filename.c_str(), &path_state,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !same_posix_object(descriptor_state, impl_->snapshot_identity) ||
      !same_posix_object(path_state, impl_->snapshot_identity) || !S_ISREG(path_state.st_mode) ||
      path_state.st_size < 0 ||
      static_cast<std::uintmax_t>(path_state.st_size) != impl_->content.size) {
    detail = "verified executable lease identity changed before process launch";
    return false;
  }
#endif
  return true;
}

HostProcessResult
VerifiedExecutableLease::execute(const std::vector<std::string> &arguments) const {
  HostProcessRequest request;
  request.arguments = arguments;
  return execute_request(std::move(request));
}

HostProcessResult VerifiedExecutableLease::execute_request(HostProcessRequest request) const {
  HostProcessResult result;
  if (!revalidate(result.infrastructure_error))
    return result;
  request.executable_path = impl_->execution_path;
  return run_host_process(request);
}

VerifiedExecutableLeaseResult VerifiedExecutableLease::cleanup() {
  if (!impl_ || impl_->cleaned)
    return {};
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
  if (verified_executable_lease_testing::take_cleanup_failure()) {
    return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                           impl_->execution_path, "cleanup-executable-lease",
                           "injected owned executable lease cleanup failure");
  }
#endif
#if defined(_WIN32)
  std::string detail;
  if (impl_->handle_phase == WindowsLeaseHandlePhase::ExecutionLocked) {
    const auto current = inspect_windows_file(impl_->snapshot_handle.get(), detail);
    if (!current.has_value() || !same_windows_object(*current, impl_->snapshot_identity) ||
        current->size != impl_->snapshot_identity.size) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::ConcurrentModification,
                             impl_->execution_path, "cleanup-executable-lease",
                             detail.empty() ? "execution lock no longer binds the lease identity"
                                            : detail);
    }

    UniqueHandle transition(
      ::CreateFileW(impl_->execution_path.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const DWORD transition_error = transition.valid() ? ERROR_SUCCESS : ::GetLastError();
    detail.clear();
    const auto transition_identity =
      transition.valid() ? inspect_windows_file(transition.get(), detail) : std::nullopt;
    if (!transition.valid() || !transition_identity.has_value() ||
        !same_windows_object(*transition_identity, impl_->snapshot_identity) ||
        transition_identity->size != impl_->snapshot_identity.size) {
      VerifiedExecutableLeaseErrorCode code =
        transition.valid() ? VerifiedExecutableLeaseErrorCode::ConcurrentModification
                           : VerifiedExecutableLeaseErrorCode::CleanupIncomplete;
      if (transition.valid() &&
          !transition.close(detail, "failed to close the rejected cleanup transition")) {
        code = VerifiedExecutableLeaseErrorCode::CleanupIncomplete;
      }
      if (detail.empty()) {
        detail = transition_error == ERROR_SUCCESS
                   ? "cleanup transition did not bind the lease path identity"
                   : "failed to open cleanup transition: " + windows_error(transition_error);
      }
      return cleanup_failure(code, impl_->execution_path,
                             "bind-executable-lease-cleanup-transition", detail);
    }

    detail.clear();
    if (!impl_->snapshot_handle.close(detail,
                                      "failed to close the non-delete-shared execution lock")) {
      if (!transition.close(detail, "failed to close the unused cleanup transition")) {
        return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                               impl_->execution_path, "handoff-executable-lease-cleanup", detail);
      }
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "handoff-executable-lease-cleanup", detail);
    }
    impl_->snapshot_handle = std::move(transition);
    impl_->handle_phase = WindowsLeaseHandlePhase::CleanupTransition;
  }

  if (impl_->handle_phase == WindowsLeaseHandlePhase::CleanupTransition) {
    detail.clear();
    const auto current = inspect_windows_file(impl_->snapshot_handle.get(), detail);
    if (!current.has_value() || !same_windows_object(*current, impl_->snapshot_identity) ||
        current->size != impl_->snapshot_identity.size) {
      return cleanup_failure(
        VerifiedExecutableLeaseErrorCode::ConcurrentModification, impl_->execution_path,
        "cleanup-executable-lease",
        detail.empty() ? "cleanup transition no longer binds the lease identity" : detail);
    }

    UniqueHandle deletion(::ReOpenFile(impl_->snapshot_handle.get(), GENERIC_READ | DELETE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT));
    const DWORD deletion_error = deletion.valid() ? ERROR_SUCCESS : ::GetLastError();
    detail.clear();
    const auto deletion_identity =
      deletion.valid() ? inspect_windows_file(deletion.get(), detail) : std::nullopt;
    if (!deletion.valid() || !deletion_identity.has_value() ||
        !same_windows_object(*deletion_identity, impl_->snapshot_identity) ||
        deletion_identity->size != impl_->snapshot_identity.size) {
      VerifiedExecutableLeaseErrorCode code =
        deletion.valid() ? VerifiedExecutableLeaseErrorCode::ConcurrentModification
                         : VerifiedExecutableLeaseErrorCode::CleanupIncomplete;
      if (deletion.valid() &&
          !deletion.close(detail, "failed to close the rejected deletion handle")) {
        code = VerifiedExecutableLeaseErrorCode::CleanupIncomplete;
      }
      if (detail.empty()) {
        detail =
          deletion_error == ERROR_SUCCESS
            ? "lease path was replaced before deletion; replacement was preserved"
            : "failed to reopen the lease identity for deletion: " + windows_error(deletion_error);
      }
      return cleanup_failure(code, impl_->execution_path, "bind-executable-lease-deletion", detail);
    }

    detail.clear();
    if (!impl_->snapshot_handle.close(detail, "failed to close the cleanup transition")) {
      if (!deletion.close(detail, "failed to close the unused deletion handle")) {
        return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                               impl_->execution_path, "handoff-executable-lease-deletion", detail);
      }
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "handoff-executable-lease-deletion", detail);
    }
    impl_->snapshot_handle = std::move(deletion);
    impl_->handle_phase = WindowsLeaseHandlePhase::DeletionBound;
  }

  if (impl_->handle_phase == WindowsLeaseHandlePhase::DeletionBound) {
    detail.clear();
    const auto current = inspect_windows_file(impl_->snapshot_handle.get(), detail);
    if (!current.has_value() || !same_windows_object(*current, impl_->snapshot_identity) ||
        current->size != impl_->snapshot_identity.size) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::ConcurrentModification,
                             impl_->execution_path, "cleanup-executable-lease",
                             detail.empty() ? "deletion handle no longer binds the lease identity"
                                            : detail);
    }
    if (!mark_windows_file_for_deletion(impl_->snapshot_handle.get(), detail)) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "cleanup-executable-lease", detail);
    }
    impl_->handle_phase = WindowsLeaseHandlePhase::DeletionMarked;
  }

  if (impl_->handle_phase == WindowsLeaseHandlePhase::DeletionMarked) {
    detail.clear();
    if (!impl_->snapshot_handle.close(detail, "failed to close the marked executable lease")) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "cleanup-executable-lease", detail);
    }
    impl_->handle_phase = WindowsLeaseHandlePhase::ParentOnly;
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
    const auto post_deletion_setup =
      verified_executable_lease_testing::take_post_deletion_cleanup_setup();
    if (post_deletion_setup.has_value() && *post_deletion_setup != nullptr)
      (*post_deletion_setup)(impl_->execution_path);
#endif
  }

  if (impl_->handle_phase == WindowsLeaseHandlePhase::ParentOnly) {
    std::optional<VerifiedExecutableLeaseResult> replacement_conflict;
    detail.clear();
    if (!windows_path_is_absent(impl_->execution_path, detail)) {
      if (!detail.empty()) {
        return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                               impl_->execution_path, "verify-executable-lease-removal", detail);
      }
      replacement_conflict.emplace(cleanup_conflict_after_owned_cleanup(
        impl_->execution_path, "cleanup-executable-lease",
        "lease path now names a replacement object; replacement was preserved"));
    }
    if (!impl_->parent_handle.close(detail, "failed to close the executable lease parent")) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "cleanup-executable-lease", detail);
    }
    if (replacement_conflict.has_value()) {
      impl_->cleaned = true;
      return std::move(*replacement_conflict);
    }
  }
#else
  struct stat path_state{};
  bool lease_path_absent = false;
  bool replacement_preserved = false;
  std::optional<VerifiedExecutableLeaseResult> replacement_conflict;
  if (::fstatat(impl_->parent_fd.get(), impl_->filename.c_str(), &path_state,
                AT_SYMLINK_NOFOLLOW) != 0) {
    const int inspection_error = errno;
    if (inspection_error != ENOENT) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "inspect-executable-lease-for-cleanup",
                             std::string("failed to inspect executable lease: ") +
                               std::strerror(inspection_error));
    }
    lease_path_absent = true;
  } else {
    if (!same_posix_object(path_state, impl_->snapshot_identity)) {
      replacement_preserved = true;
    } else if (::unlinkat(impl_->parent_fd.get(), impl_->filename.c_str(), 0) != 0) {
      return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                             impl_->execution_path, "cleanup-executable-lease",
                             std::string("identity-checked executable lease unlink failed: ") +
                               std::strerror(errno));
    }
  }

  if (replacement_preserved || lease_path_absent) {
    struct stat retained_state{};
    if (::fstat(impl_->snapshot_fd.get(), &retained_state) != 0 ||
        !same_posix_object(retained_state, impl_->snapshot_identity)) {
      return cleanup_failure(
        VerifiedExecutableLeaseErrorCode::CleanupIncomplete, impl_->execution_path,
        "inspect-retained-executable-lease-for-cleanup",
        "could not confirm the retained executable lease identity after its path changed");
    }
    if (retained_state.st_nlink != 0) {
      return cleanup_failure(
        VerifiedExecutableLeaseErrorCode::ConcurrentModification, impl_->execution_path,
        "cleanup-executable-lease",
        "the original executable lease inode still has an unknown directory entry");
    }
    if (replacement_preserved) {
      replacement_conflict.emplace(cleanup_conflict_after_owned_cleanup(
        impl_->execution_path, "cleanup-executable-lease",
        "lease path now names a different object; replacement was preserved"));
    }
  }

  std::string close_detail;
  const bool snapshot_closed =
    impl_->snapshot_fd.close(close_detail, "failed to close the executable lease snapshot");
  const bool parent_closed =
    impl_->parent_fd.close(close_detail, "failed to close the executable lease parent");
  if (!snapshot_closed || !parent_closed) {
    return cleanup_failure(VerifiedExecutableLeaseErrorCode::CleanupIncomplete,
                           impl_->execution_path, "close-executable-lease", close_detail);
  }
  impl_->cleaned = true;
  if (replacement_conflict.has_value())
    return std::move(*replacement_conflict);
#endif
  impl_->cleaned = true;
  return {};
}

VerifiedExecutableLeaseBeginResult
begin_verified_executable_lease(const fs::path &public_artifact,
                                const std::optional<FileDigest> &expected_content) {
  std::string detail;
  const auto normalized = normalize_executable_path(public_artifact, detail);
  if (!normalized.has_value()) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::InvalidPath, public_artifact,
                         "normalize-executable", detail);
  }

#if defined(_WIN32)
  UniqueHandle parent(
    ::CreateFileW(normalized->parent.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!parent.valid()) {
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->parent, "open-executable-parent",
      "failed to bind executable parent directory: " + windows_error(::GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO parent_attributes{};
  if (::GetFileInformationByHandleEx(parent.get(), FileAttributeTagInfo, &parent_attributes,
                                     sizeof(parent_attributes)) == 0 ||
      (parent_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (parent_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->parent,
                         "inspect-executable-parent", "executable parent is not a plain directory");
  }
  if (!validate_windows_persistent_acl_support(normalized->parent, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->parent,
                         "validate-executable-parent-acl", detail);
  }

  UniqueHandle source(::CreateFileW(
    normalized->path.c_str(), GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, nullptr,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
    nullptr));
  if (!source.valid()) {
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path, "open-executable",
      "failed to open a stable executable source: " + windows_error(::GetLastError()));
  }
  const auto source_identity = inspect_windows_file(source.get(), detail);
  if (!source_identity.has_value()) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path,
                         "inspect-executable", detail);
  }
  if (source_identity->size > kMaxReusableArtifactBytes) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::TooLarge, normalized->path,
                         "inspect-executable",
                         "executable exceeds the configured lease size limit");
  }
  if (!validate_windows_execution_policy(normalized->path, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path,
                         "validate-executable-policy", detail);
  }

  std::uint64_t nonce = 0U;
  if (!random_nonce(nonce, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
                         "allocate-executable-lease-name", detail);
  }
  UniqueHandle snapshot;
  UniqueHandle handoff_reader;
  UniqueHandle execution_lock;
  fs::path snapshot_path;
  fs::path snapshot_filename;
  ExecutableLeaseAcquisitionRollback rollback_guard(snapshot_path);
  const std::string executable_suffix = normalized->filename.extension().string();
  if (executable_suffix.find_first_of("/\\:") != std::string::npos) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::InvalidPath, normalized->path,
                         "validate-executable-suffix",
                         "Windows executable suffix contains a path or alternate-stream delimiter");
  }
  WindowsPrivateSecurityDescriptor security;
  if (!prepare_windows_private_security(WindowsPrivateObjectKind::File, security, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
                         "prepare-executable-lease-acl", detail);
  }
  DWORD create_error = ERROR_SUCCESS;
  bool fatal_create_error = false;
  for (std::uint64_t attempt = 0U; attempt < 128U; ++attempt) {
    snapshot_filename = fs::path(lease_filename(nonce, attempt, executable_suffix));
    snapshot_path = normalized->parent / snapshot_filename;
    snapshot.reset(::CreateFileW(
      snapshot_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, security.attributes(), CREATE_NEW,
      FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH,
      nullptr));
    if (snapshot.valid()) {
      const AcquisitionRollbackResult binding = rollback_guard.arm(snapshot);
      if (!binding.ok()) {
        return begin_failure_after_snapshot_cleanup(
          rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
          "bind-executable-lease-rollback", acquisition_rollback_detail(binding));
      }
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
      throw_injected_acquisition_exception(
        verified_executable_lease_testing::AcquisitionExceptionPoint::AfterPrivateEntryCreation,
        snapshot_path);
#endif
      break;
    }
    create_error = ::GetLastError();
    if (create_error != ERROR_FILE_EXISTS && create_error != ERROR_ALREADY_EXISTS) {
      fatal_create_error = true;
      break;
    }
  }
  std::string security_release_detail;
  if (!security.release(security_release_detail)) {
    if (snapshot.valid()) {
      return begin_failure_after_snapshot_cleanup(
        rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
        "release-executable-lease-acl", security_release_detail);
    }
    if (fatal_create_error) {
      security_release_detail =
        "executable lease creation also failed: " + windows_error(create_error) + "; " +
        security_release_detail;
    }
    return begin_failure(VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
                         "release-executable-lease-acl", security_release_detail);
  }
  if (!snapshot.valid()) {
    if (fatal_create_error) {
      return begin_failure(
        VerifiedExecutableLeaseErrorCode::Io, snapshot_path, "create-executable-lease",
        "failed to create private executable lease: " + windows_error(create_error));
    }
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::Io, normalized->parent, "create-executable-lease",
      "failed to allocate an exclusive executable lease name after 128 attempts");
  }
  if (!validate_windows_private_object_security(snapshot.get(), WindowsPrivateObjectKind::File,
                                                detail)) {
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::UnsafePath, snapshot_path,
      "validate-executable-lease-security", detail);
  }

  Sha256Digest digest_builder;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  std::uint64_t total = 0U;
  while (true) {
    DWORD read_count = 0U;
    if (::ReadFile(source.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read_count,
                   nullptr) == 0) {
      const DWORD read_error = ::GetLastError();
      return begin_failure_after_snapshot_cleanup(
        rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, normalized->path,
        "copy-executable-lease", "failed to read executable source: " + windows_error(read_error));
    }
    if (read_count == 0U)
      break;
    if (total > source_identity->size ||
        static_cast<std::uint64_t>(read_count) > source_identity->size - total) {
      return begin_failure_after_snapshot_cleanup(
        rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ConcurrentModification,
        normalized->path, "copy-executable-lease",
        "executable source grew while the lease was copied");
    }
    digest_builder.update(std::span<const std::uint8_t>(buffer.data(), read_count));
    DWORD offset = 0U;
    while (offset < read_count) {
      DWORD written = 0U;
      if (::WriteFile(snapshot.get(), buffer.data() + offset, read_count - offset, &written,
                      nullptr) == 0 ||
          written == 0U) {
        const DWORD write_error = ::GetLastError();
        return begin_failure_after_snapshot_cleanup(
          rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
          "copy-executable-lease",
          "failed to write executable lease: " + windows_error(write_error));
      }
      offset += written;
    }
    total += read_count;
  }
  if (total != source_identity->size) {
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ConcurrentModification,
      normalized->path, "copy-executable-lease",
      "executable source size changed while the lease was copied");
  }
  if (::FlushFileBuffers(snapshot.get()) == 0) {
    const DWORD flush_error = ::GetLastError();
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
      "flush-executable-lease", "failed to flush executable lease: " + windows_error(flush_error));
  }
  const auto snapshot_identity = inspect_windows_file(snapshot.get(), detail);
  if (!snapshot_identity.has_value() || snapshot_identity->size != total) {
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ConcurrentModification,
      snapshot_path, "inspect-executable-lease",
      detail.empty() ? "executable lease size changed while it was created" : detail);
  }
  if (!validate_windows_execution_policy(normalized->path, detail)) {
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path,
      "revalidate-executable-policy", detail);
  }
  FileDigest content{static_cast<std::uintmax_t>(total), digest_builder.finish_hex()};
  if (expected_content.has_value() && !same_digest(content, *expected_content)) {
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ContentMismatch,
      normalized->path, "verify-executable-content",
      "executable bytes no longer match the expected content identity");
  }

  // Bind the verified object through a share-delete reader before releasing
  // the writer. Then reopen it without FILE_SHARE_DELETE and retain that
  // read-only handle through execution. The final handle prevents a path
  // delete/rename/replacement while remaining compatible with CreateProcessW.
  detail.clear();
  handoff_reader.reset(::CreateFileW(
    snapshot_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  rollback_guard.track(handoff_reader);
  const DWORD handoff_error = handoff_reader.valid() ? ERROR_SUCCESS : ::GetLastError();
  const auto handoff_identity =
    handoff_reader.valid() ? inspect_windows_file(handoff_reader.get(), detail) : std::nullopt;
  if (!handoff_reader.valid() || !handoff_identity.has_value() ||
      !same_windows_object(*handoff_identity, *snapshot_identity) ||
      handoff_identity->size != snapshot_identity->size) {
    std::string failure_detail =
      !detail.empty() ? detail
      : handoff_error == ERROR_SUCCESS
        ? "handoff reader did not bind the verified file identity"
        : "failed to bind the executable handoff reader: " + windows_error(handoff_error);
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ConcurrentModification,
      snapshot_path, "bind-executable-lease-handoff", std::move(failure_detail));
  }

  detail.clear();
  if (!snapshot.close(detail, "failed to close the writable executable lease")) {
    return begin_failure_after_snapshot_cleanup(rollback_guard, snapshot_path,
                                                VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
                                                "close-executable-lease-writer", detail);
  }

#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
  throw_injected_acquisition_exception(
    verified_executable_lease_testing::AcquisitionExceptionPoint::AfterWritableSnapshotClosed,
    snapshot_path);
#endif

  execution_lock.reset(::CreateFileW(
    snapshot_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  rollback_guard.track(execution_lock);
  const DWORD execution_lock_error = execution_lock.valid() ? ERROR_SUCCESS : ::GetLastError();
  detail.clear();
  const auto executable_identity =
    execution_lock.valid() ? inspect_windows_file(execution_lock.get(), detail) : std::nullopt;
  if (!execution_lock.valid() || !executable_identity.has_value() ||
      !same_windows_object(*executable_identity, *snapshot_identity) ||
      executable_identity->size != snapshot_identity->size) {
    std::string failure_detail = !detail.empty() ? detail
                                 : execution_lock_error == ERROR_SUCCESS
                                   ? "execution lock did not bind the verified file identity"
                                   : "failed to bind the non-delete-shared execution lock: " +
                                       windows_error(execution_lock_error);
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::ConcurrentModification,
      snapshot_path, "bind-executable-lease-execution-lock", std::move(failure_detail));
  }

  detail.clear();
  if (!handoff_reader.close(detail, "failed to close the executable handoff reader")) {
    return begin_failure_after_snapshot_cleanup(rollback_guard, snapshot_path,
                                                VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
                                                "close-executable-lease-handoff", detail);
  }

#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
  if (const auto injected = verified_executable_lease_testing::take_acquisition_failure();
      injected.has_value()) {
    if (*injected != nullptr)
      (*injected)(snapshot_path);
    return begin_failure_after_snapshot_cleanup(
      rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
      "injected-executable-lease-acquisition",
      std::string(verified_executable_lease_testing::kInjectedAcquisitionFailureDetail));
  }
  throw_injected_acquisition_exception(
    verified_executable_lease_testing::AcquisitionExceptionPoint::BeforeImplementationAllocation,
    snapshot_path);
#endif
  auto implementation = std::make_unique<VerifiedExecutableLease::Impl>();
  implementation->public_path = normalized->path;
  implementation->execution_path = snapshot_path;
  implementation->filename = snapshot_filename;
  implementation->content = std::move(content);
  implementation->parent_handle = std::move(parent);
  implementation->snapshot_handle = std::move(execution_lock);
  implementation->snapshot_identity = *executable_identity;
#else
  if (!validate_owner_controlled_directory_chain(normalized->parent, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->parent,
                         "validate-executable-parent", detail);
  }
  UniqueFd parent(
    ::open(normalized->parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!parent.valid()) {
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->parent, "open-executable-parent",
      "failed to bind executable parent directory: " + std::string(std::strerror(errno)));
  }
  struct stat parent_state{};
  struct stat parent_path_state{};
  if (::fstat(parent.get(), &parent_state) != 0 ||
      ::lstat(normalized->parent.c_str(), &parent_path_state) != 0 ||
      !S_ISDIR(parent_state.st_mode) || !same_posix_object(parent_state, parent_path_state)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::ConcurrentModification,
                         normalized->parent, "bind-executable-parent",
                         "executable parent changed while its directory handle was acquired");
  }

  UniqueFd source(
    ::openat(parent.get(), normalized->filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!source.valid()) {
    const int open_error = errno;
    return begin_failure(open_error == ELOOP ? VerifiedExecutableLeaseErrorCode::UnsafePath
                                             : VerifiedExecutableLeaseErrorCode::Io,
                         normalized->path, "open-executable",
                         "failed to open executable source: " +
                           std::string(std::strerror(open_error)));
  }
  struct stat source_before{};
  if (::fstat(source.get(), &source_before) != 0) {
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::Io, normalized->path, "inspect-executable",
      "failed to inspect executable source: " + std::string(std::strerror(errno)));
  }
  if (!S_ISREG(source_before.st_mode)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path,
                         "inspect-executable", "executable source is not a regular file");
  }
  if (!validate_posix_execution_policy(parent.get(), normalized->filename, source.get(), detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::UnsafePath, normalized->path,
                         "validate-executable-policy", detail);
  }
  if (source_before.st_size < 0 ||
      static_cast<std::uintmax_t>(source_before.st_size) > kMaxReusableArtifactBytes) {
    return begin_failure(source_before.st_size < 0 ? VerifiedExecutableLeaseErrorCode::Io
                                                   : VerifiedExecutableLeaseErrorCode::TooLarge,
                         normalized->path, "inspect-executable",
                         source_before.st_size < 0
                           ? "executable source reported a negative size"
                           : "executable exceeds the configured lease size limit");
  }

  std::uint64_t nonce = 0U;
  if (!random_nonce(nonce, detail)) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
                         "allocate-executable-lease-name", detail);
  }
  UniqueFd snapshot;
  fs::path snapshot_filename;
  fs::path snapshot_path;
  UniqueFd rollback_parent(::fcntl(parent.get(), F_DUPFD_CLOEXEC, 0));
  if (!rollback_parent.valid()) {
    return begin_failure(VerifiedExecutableLeaseErrorCode::Io, normalized->parent,
                         "bind-executable-lease-rollback-parent",
                         "failed to duplicate the executable parent for acquisition rollback: " +
                           std::string(std::strerror(errno)));
  }
  ExecutableLeaseAcquisitionRollback rollback_guard(std::move(rollback_parent), snapshot_filename);
  int create_error = 0;
  for (std::uint64_t attempt = 0U; attempt < 128U; ++attempt) {
    snapshot_filename = fs::path(lease_filename(nonce, attempt));
    snapshot_path = normalized->parent / snapshot_filename;
    snapshot.reset(::openat(parent.get(), snapshot_filename.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IXUSR));
    if (snapshot.valid()) {
      const AcquisitionRollbackResult binding = rollback_guard.arm(snapshot.get());
      if (!binding.ok()) {
        return begin_failure_after_snapshot_cleanup(
          rollback_guard, snapshot_path, VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
          "bind-executable-lease-rollback", acquisition_rollback_detail(binding));
      }
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
      throw_injected_acquisition_exception(
        verified_executable_lease_testing::AcquisitionExceptionPoint::AfterPrivateEntryCreation,
        snapshot_path);
#endif
      break;
    }
    create_error = errno;
    if (create_error != EEXIST) {
      return begin_failure(
        VerifiedExecutableLeaseErrorCode::Io, snapshot_path, "create-executable-lease",
        "failed to create private executable lease: " + std::string(std::strerror(create_error)));
    }
  }
  if (!snapshot.valid()) {
    return begin_failure(
      VerifiedExecutableLeaseErrorCode::Io, normalized->parent, "create-executable-lease",
      "failed to allocate an exclusive executable lease name after 128 attempts");
  }
  const auto fail_after_snapshot_cleanup = [&](VerifiedExecutableLeaseErrorCode code,
                                               const fs::path &path, std::string operation,
                                               std::string failure_detail) {
    return begin_failure_after_snapshot_cleanup(rollback_guard, snapshot_path, code, path,
                                                std::move(operation), std::move(failure_detail));
  };

  Sha256Digest digest_builder;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  std::uintmax_t total = 0U;
  const std::uintmax_t expected_size = static_cast<std::uintmax_t>(source_before.st_size);
  while (true) {
    const ssize_t count = ::read(source.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      const int read_error = errno;
      return fail_after_snapshot_cleanup(
        VerifiedExecutableLeaseErrorCode::Io, normalized->path, "copy-executable-lease",
        "failed to read executable source: " + std::string(std::strerror(read_error)));
    }
    if (count == 0)
      break;
    const auto chunk_size = static_cast<std::uintmax_t>(count);
    if (total > expected_size || chunk_size > expected_size - total) {
      return fail_after_snapshot_cleanup(VerifiedExecutableLeaseErrorCode::ConcurrentModification,
                                         normalized->path, "copy-executable-lease",
                                         "executable source grew while the lease was copied");
    }
    const std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(count));
    digest_builder.update(chunk);
    if (!write_all(snapshot.get(), chunk, detail)) {
      return fail_after_snapshot_cleanup(VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
                                         "copy-executable-lease", detail);
    }
    total += chunk_size;
  }
  if (total != expected_size) {
    return fail_after_snapshot_cleanup(VerifiedExecutableLeaseErrorCode::ConcurrentModification,
                                       normalized->path, "copy-executable-lease",
                                       "executable source size changed while the lease was copied");
  }
  if (::fsync(snapshot.get()) != 0) {
    const int flush_error = errno;
    return fail_after_snapshot_cleanup(
      VerifiedExecutableLeaseErrorCode::Io, snapshot_path, "flush-executable-lease",
      "failed to flush executable lease: " + std::string(std::strerror(flush_error)));
  }

  struct stat source_after{};
  struct stat source_path_after{};
  struct stat snapshot_state{};
  if (::fstat(source.get(), &source_after) != 0 ||
      ::fstatat(parent.get(), normalized->filename.c_str(), &source_path_after,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      ::fstat(snapshot.get(), &snapshot_state) != 0 ||
      !same_posix_source_state(source_before, source_after) ||
      !same_posix_object(source_after, source_path_after) || !S_ISREG(source_path_after.st_mode) ||
      !S_ISREG(snapshot_state.st_mode) || snapshot_state.st_size < 0 ||
      static_cast<std::uintmax_t>(snapshot_state.st_size) != total) {
    return fail_after_snapshot_cleanup(
      VerifiedExecutableLeaseErrorCode::ConcurrentModification, normalized->path,
      "verify-executable-lease-copy",
      "executable source or private copy changed while the lease was acquired");
  }
  if (!validate_posix_execution_policy(parent.get(), normalized->filename, source.get(), detail)) {
    return fail_after_snapshot_cleanup(VerifiedExecutableLeaseErrorCode::UnsafePath,
                                       normalized->path, "revalidate-executable-policy", detail);
  }
  FileDigest content{total, digest_builder.finish_hex()};
  if (expected_content.has_value() && !same_digest(content, *expected_content)) {
    return fail_after_snapshot_cleanup(
      VerifiedExecutableLeaseErrorCode::ContentMismatch, normalized->path,
      "verify-executable-content",
      "executable bytes no longer match the expected content identity");
  }

  // Linux rejects exec of an inode that is still open for writing (ETXTBSY).
  // Acquire the read-only execution descriptor before closing the writer, then
  // prove that reader, writer, and directory entry are the same regular inode.
  // This preserves the no-reopen replacement guarantee while making the lease
  // executable on every supported POSIX host.
  UniqueFd executable_snapshot(
    ::openat(parent.get(), snapshot_filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  struct stat executable_state{};
  struct stat executable_path_state{};
  if (!executable_snapshot.valid() || ::fstat(executable_snapshot.get(), &executable_state) != 0 ||
      ::fstatat(parent.get(), snapshot_filename.c_str(), &executable_path_state,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(executable_state.st_mode) || !S_ISREG(executable_path_state.st_mode) ||
      !same_posix_object(executable_state, snapshot_state) ||
      !same_posix_object(executable_path_state, snapshot_state) || executable_state.st_size < 0 ||
      static_cast<std::uintmax_t>(executable_state.st_size) != total) {
    return fail_after_snapshot_cleanup(
      VerifiedExecutableLeaseErrorCode::ConcurrentModification, snapshot_path,
      "handoff-executable-lease",
      "could not bind a read-only descriptor to the verified executable inode");
  }
  detail.clear();
  if (!snapshot.close(detail, "failed to close the writable executable lease")) {
    return fail_after_snapshot_cleanup(VerifiedExecutableLeaseErrorCode::Io, snapshot_path,
                                       "close-executable-lease-writer", detail);
  }

#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
  throw_injected_acquisition_exception(
    verified_executable_lease_testing::AcquisitionExceptionPoint::AfterWritableSnapshotClosed,
    snapshot_path);
  if (const auto injected = verified_executable_lease_testing::take_acquisition_failure();
      injected.has_value()) {
    if (*injected != nullptr)
      (*injected)(snapshot_path);
    return fail_after_snapshot_cleanup(
      VerifiedExecutableLeaseErrorCode::Io, snapshot_path, "injected-executable-lease-acquisition",
      std::string(verified_executable_lease_testing::kInjectedAcquisitionFailureDetail));
  }
  throw_injected_acquisition_exception(
    verified_executable_lease_testing::AcquisitionExceptionPoint::BeforeImplementationAllocation,
    snapshot_path);
#endif

  auto implementation = std::make_unique<VerifiedExecutableLease::Impl>();
  implementation->public_path = normalized->path;
  implementation->execution_path = snapshot_path;
  implementation->filename = snapshot_filename;
  implementation->content = std::move(content);
  implementation->parent_fd = std::move(parent);
  implementation->snapshot_fd = std::move(executable_snapshot);
  implementation->snapshot_identity = executable_state;
#endif

  VerifiedExecutableLeaseBeginResult result;
  result.lease = std::unique_ptr<VerifiedExecutableLease>(
    new VerifiedExecutableLease(std::move(implementation)));
  rollback_guard.disarm();
#if defined(NEBULA_VERIFIED_EXECUTABLE_LEASE_TESTING)
  throw_injected_acquisition_exception(
    verified_executable_lease_testing::AcquisitionExceptionPoint::AfterOwnershipTransfer,
    result.lease->execution_path());
#endif
  return result;
}

} // namespace nebula::cli
