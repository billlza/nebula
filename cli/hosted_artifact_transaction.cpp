#include "hosted_artifact_transaction.hpp"
#include "path_security.hpp"
#if defined(_WIN32)
#include "windows_object_identity.hpp"
#include "windows_private_security.hpp"
#endif

#include "artifact_digest.hpp"

#if defined(NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING)
#include "hosted_artifact_transaction_test_hooks.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#include <sys/acl.h>
#include <sys/xattr.h>
#endif
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace nebula::cli {

#if defined(NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING)
namespace hosted_artifact_transaction_testing {
namespace {
std::atomic<FaultPoint> injected_fault{FaultPoint::None};
std::atomic<BeforeFinalProtectedInputRevalidationHook> injected_final_revalidation_hook{nullptr};
std::atomic<void *> injected_final_revalidation_context{nullptr};
} // namespace

void inject_fault_once(FaultPoint point) { injected_fault.store(point, std::memory_order_release); }

bool consume_fault(FaultPoint point) {
  FaultPoint expected = point;
  return injected_fault.compare_exchange_strong(expected, FaultPoint::None,
                                                std::memory_order_acq_rel);
}

void inject_before_final_protected_input_revalidation_once(
  BeforeFinalProtectedInputRevalidationHook hook, void *context) {
  injected_final_revalidation_context.store(context, std::memory_order_relaxed);
  injected_final_revalidation_hook.store(hook, std::memory_order_release);
}

void run_before_final_protected_input_revalidation_hook_once() {
  const BeforeFinalProtectedInputRevalidationHook hook =
    injected_final_revalidation_hook.exchange(nullptr, std::memory_order_acq_rel);
  if (hook == nullptr)
    return;
  void *context = injected_final_revalidation_context.exchange(nullptr, std::memory_order_acq_rel);
  hook(context);
}
} // namespace hosted_artifact_transaction_testing
#endif

namespace {

namespace fs = std::filesystem;

enum class InternalFaultPoint : std::uint8_t {
  AfterBackup,
  AfterPublishLink,
  BeforePublicationDirectoryFlush,
};

bool consume_test_fault(InternalFaultPoint point) {
#if defined(NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING)
  using hosted_artifact_transaction_testing::FaultPoint;
  switch (point) {
  case InternalFaultPoint::AfterBackup:
    return hosted_artifact_transaction_testing::consume_fault(FaultPoint::AfterBackup);
  case InternalFaultPoint::AfterPublishLink:
    return hosted_artifact_transaction_testing::consume_fault(FaultPoint::AfterPublishLink);
  case InternalFaultPoint::BeforePublicationDirectoryFlush:
    return hosted_artifact_transaction_testing::consume_fault(
      FaultPoint::BeforePublicationDirectoryFlush);
  }
#else
  (void)point;
#endif
  return false;
}

struct NativeIdentity {
  std::uint64_t device = 0U;
  std::uint64_t file = 0U;
  std::uint64_t file_high = 0U;
  std::uint64_t size = 0U;
  std::uint64_t modified_low = 0U;
  std::uint64_t modified_high = 0U;
  std::uint64_t changed_low = 0U;
  std::uint64_t changed_high = 0U;
  std::uint64_t links = 0U;
  std::uint64_t mode = 0U;
  std::uint64_t owner = 0U;
  std::uint64_t group = 0U;
  std::uint64_t flags = 0U;
  std::uint64_t attributes = 0U;
  bool directory = false;
  bool valid = false;
};

struct PlatformSecuritySnapshot {
#if defined(__APPLE__)
  bool extended_attributes_supported = false;
  bool access_control_lists_supported = false;
  bool access_control_list_has_entries = false;
  std::map<std::string, std::vector<std::uint8_t>> extended_attributes;
  std::vector<std::uint8_t> access_control_list;
#endif
};

struct InspectedPath {
  bool exists = false;
  NativeIdentity identity;
  std::string detail;
};

struct LockedPath {
  fs::path path;
  NativeIdentity identity;
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  OVERLAPPED range{};
#else
  int descriptor = -1;
  int parent_descriptor = -1;
  fs::path entry_name;
  NativeIdentity parent_identity;
#endif

  LockedPath() = default;
  LockedPath(const LockedPath &) = delete;
  LockedPath &operator=(const LockedPath &) = delete;
  LockedPath(LockedPath &&other) noexcept
      : path(std::move(other.path)), identity(other.identity)
#if defined(_WIN32)
        ,
        handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)), range(other.range)
#else
        ,
        descriptor(std::exchange(other.descriptor, -1)),
        parent_descriptor(std::exchange(other.parent_descriptor, -1)),
        entry_name(std::move(other.entry_name)), parent_identity(other.parent_identity)
#endif
  {
  }
  LockedPath &operator=(LockedPath &&) = delete;
  ~LockedPath() {
#if defined(_WIN32)
    if (handle != INVALID_HANDLE_VALUE) {
      (void)::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &range);
      (void)::CloseHandle(handle);
    }
#else
    if (descriptor >= 0) {
      (void)::flock(descriptor, LOCK_UN);
      (void)::close(descriptor);
    }
    if (parent_descriptor >= 0)
      (void)::close(parent_descriptor);
#endif
  }
};

#if defined(_WIN32)
enum class WindowsStagingDirectoryHandlePhase : std::uint8_t {
  IdentityLocked,
  CleanupTransition,
  DeletionBound,
  DeletionMarked,
};
#endif

struct StagingDirectory {
  fs::path path;
  NativeIdentity identity;
#if defined(_WIN32)
  HANDLE handle = INVALID_HANDLE_VALUE;
  WindowsStagingDirectoryHandlePhase handle_phase =
    WindowsStagingDirectoryHandlePhase::IdentityLocked;
#endif

#if defined(_WIN32)
  StagingDirectory(fs::path staging_path, NativeIdentity staging_identity, HANDLE staging_handle)
      : path(std::move(staging_path)), identity(staging_identity), handle(staging_handle) {}
#else
  StagingDirectory(fs::path staging_path, NativeIdentity staging_identity)
      : path(std::move(staging_path)), identity(staging_identity) {}
#endif
  StagingDirectory(const StagingDirectory &) = delete;
  StagingDirectory &operator=(const StagingDirectory &) = delete;
  StagingDirectory(StagingDirectory &&other) noexcept
      : path(std::move(other.path)), identity(other.identity)
#if defined(_WIN32)
        ,
        handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)), handle_phase(other.handle_phase)
#endif
  {
  }
  StagingDirectory &operator=(StagingDirectory &&) = delete;
  ~StagingDirectory() {
#if defined(_WIN32)
    if (handle != INVALID_HANDLE_VALUE)
      (void)::CloseHandle(handle);
#endif
  }
};

struct PublicationEntry {
  fs::path destination;
  fs::path staged;
  fs::path backup;
  fs::path rollback_quarantine;
  NativeIdentity staged_identity;
  std::optional<FileDigest> staged_digest;
  PlatformSecuritySnapshot staged_security;
  NativeIdentity initial_identity;
  NativeIdentity prior_identity;
  NativeIdentity backup_identity;
  NativeIdentity rollback_identity;
  bool darwin_metadata_stabilization_available = false;
  bool staged_adopted = false;
  bool initial_existed = false;
  bool prior_existed = false;
  bool backed_up = false;
  bool published = false;
  bool rollback_occupied = false;
};

HostedArtifactTransactionResult success_result() { return {}; }

HostedArtifactTransactionError make_error(HostedArtifactTransactionErrorCode code,
                                          std::string operation, const fs::path &path,
                                          std::string detail) {
  return {code, std::move(operation), path, std::move(detail)};
}

HostedArtifactTransactionResult failure_result(HostedArtifactTransactionErrorCode code,
                                               std::string operation, const fs::path &path,
                                               std::string detail) {
  return {make_error(code, std::move(operation), path, std::move(detail))};
}

bool same_object(const NativeIdentity &left, const NativeIdentity &right) {
  return left.valid && right.valid && left.device == right.device && left.file == right.file &&
         left.file_high == right.file_high && left.directory == right.directory;
}

bool same_stable_security(const NativeIdentity &left, const NativeIdentity &right) {
  return left.mode == right.mode && left.owner == right.owner && left.group == right.group &&
         left.flags == right.flags && left.attributes == right.attributes;
}

bool same_snapshot_except_change_time(const NativeIdentity &left, const NativeIdentity &right) {
  return same_object(left, right) && left.size == right.size &&
         left.modified_low == right.modified_low && left.modified_high == right.modified_high &&
         left.links == right.links && same_stable_security(left, right);
}

bool same_snapshot(const NativeIdentity &left, const NativeIdentity &right) {
  return same_snapshot_except_change_time(left, right) && left.changed_low == right.changed_low &&
         left.changed_high == right.changed_high;
}

bool same_content_snapshot(const NativeIdentity &left, const NativeIdentity &right) {
  return same_object(left, right) && left.size == right.size &&
         left.modified_low == right.modified_low && left.modified_high == right.modified_high &&
         same_stable_security(left, right);
}

bool same_platform_security(const PlatformSecuritySnapshot &left,
                            const PlatformSecuritySnapshot &right) {
#if defined(__APPLE__)
  return left.extended_attributes_supported == right.extended_attributes_supported &&
         left.access_control_lists_supported == right.access_control_lists_supported &&
         left.access_control_list_has_entries == right.access_control_list_has_entries &&
         left.extended_attributes == right.extended_attributes &&
         left.access_control_list == right.access_control_list;
#else
  (void)left;
  (void)right;
  return true;
#endif
}

std::string describe_snapshot_difference(const NativeIdentity &expected,
                                         const NativeIdentity &observed) {
  std::vector<std::string> fields;
  if (!same_object(expected, observed))
    fields.emplace_back("native-object");
  if (expected.size != observed.size)
    fields.emplace_back("size");
  if (expected.modified_low != observed.modified_low ||
      expected.modified_high != observed.modified_high) {
    fields.emplace_back("modification-time");
  }
  if (expected.changed_low != observed.changed_low ||
      expected.changed_high != observed.changed_high) {
    fields.emplace_back("change-time");
  }
  if (expected.links != observed.links)
    fields.emplace_back("link-count");
  if (expected.mode != observed.mode)
    fields.emplace_back("mode");
  if (expected.owner != observed.owner)
    fields.emplace_back("owner");
  if (expected.group != observed.group)
    fields.emplace_back("group");
  if (expected.flags != observed.flags)
    fields.emplace_back("flags");
  if (expected.attributes != observed.attributes)
    fields.emplace_back("attributes");
  if (fields.empty())
    return "staged output identity or content changed";

  std::ostringstream detail;
  detail << "staged output snapshot changed fields=";
  for (std::size_t index = 0U; index < fields.size(); ++index) {
    if (index != 0U)
      detail << ',';
    detail << fields[index];
  }
  return detail.str();
}

#if defined(_WIN32)

std::string windows_error(DWORD code) {
  return std::system_category().message(static_cast<int>(code));
}

void close_handle_with_detail(HANDLE handle, std::string &detail) {
  if (::CloseHandle(handle) == 0) {
    if (!detail.empty())
      detail += "; ";
    detail += "handle cleanup failed: " + windows_error(::GetLastError());
  }
}

NativeIdentity identity_from_info(HANDLE handle, const BY_HANDLE_FILE_INFORMATION &info) {
  NativeIdentity identity;
  WindowsObjectIdentity object_identity;
  FILE_BASIC_INFO basic{};
  const DWORD identity_error = read_windows_object_identity(handle, object_identity);
  if (identity_error != ERROR_SUCCESS ||
      ::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) == 0) {
    if (identity_error != ERROR_SUCCESS)
      ::SetLastError(identity_error);
    return identity;
  }
  identity.device = object_identity.volume_serial_number;
  static_assert(sizeof(object_identity.file_id) == 2U * sizeof(std::uint64_t));
  std::memcpy(&identity.file, object_identity.file_id.data(), sizeof(identity.file));
  std::memcpy(&identity.file_high, object_identity.file_id.data() + sizeof(identity.file),
              sizeof(identity.file_high));
  identity.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
                  static_cast<std::uint64_t>(info.nFileSizeLow);
  identity.modified_low = info.ftLastWriteTime.dwLowDateTime;
  identity.modified_high = info.ftLastWriteTime.dwHighDateTime;
  const std::uint64_t change_time = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
  identity.changed_low = change_time & 0xffffffffULL;
  identity.changed_high = change_time >> 32U;
  identity.links = info.nNumberOfLinks;
  identity.attributes = info.dwFileAttributes;
  identity.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  identity.valid = true;
  return identity;
}

InspectedPath inspect_path(const fs::path &path) {
  HANDLE handle = ::CreateFileW(
    path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return {};
    return {false, {}, "failed to inspect path: " + windows_error(error)};
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(handle, &info) == 0) {
    const DWORD error = ::GetLastError();
    std::string detail = "failed to inspect path identity: " + windows_error(error);
    close_handle_with_detail(handle, detail);
    return {false, {}, std::move(detail)};
  }
  const bool reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  const NativeIdentity identity = identity_from_info(handle, info);
  if (!identity.valid) {
    const DWORD error = ::GetLastError();
    std::string detail = "failed to inspect file ID or change time: " + windows_error(error);
    close_handle_with_detail(handle, detail);
    return {false, {}, std::move(detail)};
  }
  if (::CloseHandle(handle) == 0)
    return {false, {}, "failed to close inspected path: " + windows_error(::GetLastError())};
  if (reparse)
    return {true, {}, "path is a reparse point"};
  return {true, identity, {}};
}

bool read_staging_directory_identity(StagingDirectory &directory, std::string &detail) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(directory.handle, &information) == 0) {
    detail =
      "failed to inspect bound staging directory identity: " + windows_error(::GetLastError());
    return false;
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    detail = "bound staging object is not a plain directory";
    return false;
  }
  directory.identity = identity_from_info(directory.handle, information);
  if (!directory.identity.valid) {
    detail = "failed to read the bound staging directory file ID or change time: " +
             windows_error(::GetLastError());
    return false;
  }
  return true;
}

bool close_staging_directory_handle(StagingDirectory &directory, std::string &detail) {
  if (directory.handle == INVALID_HANDLE_VALUE)
    return true;
  if (::CloseHandle(directory.handle) != 0) {
    directory.handle = INVALID_HANDLE_VALUE;
    return true;
  }
  if (!detail.empty())
    detail += "; ";
  detail +=
    "failed to close the bound staging directory handle: " + windows_error(::GetLastError());
  return false;
}

bool adopt_closed_staging_directory_handle(StagingDirectory &destination, StagingDirectory &source,
                                           std::string &detail) {
  if (destination.handle != INVALID_HANDLE_VALUE || source.handle == INVALID_HANDLE_VALUE) {
    detail = "staging directory handle handoff has an invalid state";
    return false;
  }
  destination.handle = source.handle;
  destination.identity = source.identity;
  destination.handle_phase = source.handle_phase;
  source.handle = INVALID_HANDLE_VALUE;
  return true;
}

bool finish_empty_bound_staging_directory_deletion(StagingDirectory &directory,
                                                   std::string &detail) {
  if (directory.handle_phase == WindowsStagingDirectoryHandlePhase::IdentityLocked) {
    StagingDirectory transition(
      directory.path, {},
      ::ReOpenFile(directory.handle, FILE_READ_ATTRIBUTES,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    transition.handle_phase = WindowsStagingDirectoryHandlePhase::CleanupTransition;
    if (transition.handle == INVALID_HANDLE_VALUE) {
      detail =
        "failed to reopen staging directory cleanup transition: " + windows_error(::GetLastError());
      return false;
    }
    if (!read_staging_directory_identity(transition, detail) ||
        !same_object(transition.identity, directory.identity)) {
      if (detail.empty())
        detail = "staging directory cleanup transition has the wrong identity";
      std::string close_detail;
      if (!close_staging_directory_handle(transition, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!close_staging_directory_handle(directory, detail)) {
      std::string close_detail;
      if (!close_staging_directory_handle(transition, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!adopt_closed_staging_directory_handle(directory, transition, detail))
      return false;
  }

  if (directory.handle_phase == WindowsStagingDirectoryHandlePhase::CleanupTransition) {
    StagingDirectory deletion(
      directory.path, {},
      ::ReOpenFile(directory.handle, FILE_READ_ATTRIBUTES | DELETE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT));
    deletion.handle_phase = WindowsStagingDirectoryHandlePhase::DeletionBound;
    if (deletion.handle == INVALID_HANDLE_VALUE) {
      detail = "failed to reopen staging directory for handle-bound deletion: " +
               windows_error(::GetLastError());
      return false;
    }
    if (!read_staging_directory_identity(deletion, detail) ||
        !same_object(deletion.identity, directory.identity)) {
      if (detail.empty())
        detail = "staging directory deletion handle has the wrong identity";
      std::string close_detail;
      if (!close_staging_directory_handle(deletion, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!close_staging_directory_handle(directory, detail)) {
      std::string close_detail;
      if (!close_staging_directory_handle(deletion, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!adopt_closed_staging_directory_handle(directory, deletion, detail))
      return false;
  }

  if (directory.handle_phase == WindowsStagingDirectoryHandlePhase::DeletionBound) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (::SetFileInformationByHandle(directory.handle, FileDispositionInfo, &disposition,
                                     sizeof(disposition)) == 0) {
      detail = "failed to mark staging directory for handle-bound deletion: " +
               windows_error(::GetLastError());
      return false;
    }
    directory.handle_phase = WindowsStagingDirectoryHandlePhase::DeletionMarked;
  }
  if (directory.handle_phase == WindowsStagingDirectoryHandlePhase::DeletionMarked &&
      !close_staging_directory_handle(directory, detail)) {
    return false;
  }
  if (directory.handle == INVALID_HANDLE_VALUE)
    return true;
  if (detail.empty())
    detail = "staging directory deletion state did not reach a closed handle";
  return false;
}

bool spelling_equal(const fs::path &left, const fs::path &right) {
  return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool has_reserved_lock_suffix(const fs::path &path) {
  const std::wstring name = path.filename().native();
  constexpr std::wstring_view suffix = L".nebula.lock";
  if (name.size() < suffix.size())
    return false;
  return ::CompareStringOrdinal(name.c_str() + (name.size() - suffix.size()),
                                static_cast<int>(suffix.size()), suffix.data(),
                                static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

std::uint64_t process_id() { return static_cast<std::uint64_t>(::GetCurrentProcessId()); }

#else

void close_descriptor_with_detail(int descriptor, std::string &detail) {
  if (::close(descriptor) != 0) {
    if (!detail.empty())
      detail += "; ";
    detail += "descriptor cleanup failed: " + std::string(std::strerror(errno));
  }
}

NativeIdentity identity_from_stat(const struct stat &status) {
  NativeIdentity identity;
  identity.device = static_cast<std::uint64_t>(status.st_dev);
  identity.file = static_cast<std::uint64_t>(status.st_ino);
  identity.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__APPLE__)
  identity.modified_low = static_cast<std::uint64_t>(status.st_mtimespec.tv_sec);
  identity.modified_high = static_cast<std::uint64_t>(status.st_mtimespec.tv_nsec);
  identity.changed_low = static_cast<std::uint64_t>(status.st_ctimespec.tv_sec);
  identity.changed_high = static_cast<std::uint64_t>(status.st_ctimespec.tv_nsec);
#else
  identity.modified_low = static_cast<std::uint64_t>(status.st_mtim.tv_sec);
  identity.modified_high = static_cast<std::uint64_t>(status.st_mtim.tv_nsec);
  identity.changed_low = static_cast<std::uint64_t>(status.st_ctim.tv_sec);
  identity.changed_high = static_cast<std::uint64_t>(status.st_ctim.tv_nsec);
#endif
  identity.links = static_cast<std::uint64_t>(status.st_nlink);
  identity.mode = static_cast<std::uint64_t>(status.st_mode);
  identity.owner = static_cast<std::uint64_t>(status.st_uid);
  identity.group = static_cast<std::uint64_t>(status.st_gid);
#if defined(__APPLE__)
  identity.flags = static_cast<std::uint64_t>(status.st_flags);
#endif
  identity.directory = S_ISDIR(status.st_mode);
  identity.valid = true;
  return identity;
}

InspectedPath inspect_path(const fs::path &path) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return {};
    return {false, {}, "failed to inspect path: " + std::string(std::strerror(errno))};
  }
  if (S_ISLNK(status.st_mode))
    return {true, {}, "path is a symbolic link"};
  if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))
    return {true, {}, "path is neither a regular file nor a directory"};
  return {true, identity_from_stat(status), {}};
}

bool spelling_equal(const fs::path &left, const fs::path &right) { return left == right; }

bool has_reserved_lock_suffix(const fs::path &path) {
  const std::string name = path.filename().native();
  constexpr std::string_view suffix = ".nebula.lock";
  if (name.size() < suffix.size())
    return false;
  const auto offset = name.size() - suffix.size();
  for (std::size_t index = 0U; index < suffix.size(); ++index) {
    const auto value = static_cast<unsigned char>(name[offset + index]);
    if (static_cast<char>(std::tolower(value)) != suffix[index])
      return false;
  }
  return true;
}

std::uint64_t process_id() { return static_cast<std::uint64_t>(::getpid()); }

bool validate_posix_lock_status(const struct stat &status, std::string_view subject,
                                std::string &detail) {
  if (!S_ISREG(status.st_mode)) {
    detail = std::string(subject) + " is not a regular file";
    return false;
  }
  if (status.st_uid != ::geteuid()) {
    detail = std::string(subject) + " is not owned by the effective user";
    return false;
  }
  if (status.st_nlink != 1) {
    detail = std::string(subject) + " must have exactly one hard link";
    return false;
  }
  constexpr mode_t permission_bits = S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
  constexpr mode_t required_permissions = S_IRUSR | S_IWUSR;
  if ((status.st_mode & permission_bits) != required_permissions) {
    detail = std::string(subject) + " must have exactly 0600 permissions";
    return false;
  }
  return true;
}

#endif

struct SpellingPathLess {
  bool operator()(const fs::path &left, const fs::path &right) const {
#if defined(_WIN32)
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
#else
    return left < right;
#endif
  }
};

struct NativeObjectKey {
  std::uint64_t device = 0U;
  std::uint64_t file = 0U;
  std::uint64_t file_high = 0U;
  bool directory = false;

  bool operator<(const NativeObjectKey &other) const {
    return std::tie(device, file, file_high, directory) <
           std::tie(other.device, other.file, other.file_high, other.directory);
  }
};

NativeObjectKey native_object_key(const NativeIdentity &identity) {
  return {identity.device, identity.file, identity.file_high, identity.directory};
}

bool revalidate_lock(const LockedPath &lock, std::string &detail) {
#if defined(_WIN32)
  const InspectedPath current = inspect_path(lock.path);
  if (!current.detail.empty()) {
    detail = current.detail;
    return false;
  }
  if (!current.exists || !same_object(current.identity, lock.identity)) {
    detail = "transaction lock directory entry no longer names the locked file";
    return false;
  }
  return true;
#else
  if (lock.descriptor < 0 || lock.parent_descriptor < 0 || lock.entry_name.empty() ||
      !lock.identity.valid || !lock.parent_identity.valid) {
    detail = "transaction lock does not have a complete identity binding";
    return false;
  }

  struct stat parent_status{};
  if (::fstat(lock.parent_descriptor, &parent_status) != 0) {
    detail = "failed to inspect transaction lock parent: " + std::string(std::strerror(errno));
    return false;
  }
  const NativeIdentity current_parent = identity_from_stat(parent_status);
  if (!S_ISDIR(parent_status.st_mode) || !same_object(current_parent, lock.parent_identity)) {
    detail = "transaction lock parent descriptor identity changed";
    return false;
  }

  struct stat descriptor_status{};
  if (::fstat(lock.descriptor, &descriptor_status) != 0) {
    detail = "failed to inspect locked transaction file: " + std::string(std::strerror(errno));
    return false;
  }
  if (!validate_posix_lock_status(descriptor_status, "transaction lock", detail))
    return false;
  const NativeIdentity descriptor_identity = identity_from_stat(descriptor_status);
  if (!same_object(descriptor_identity, lock.identity)) {
    detail = "locked transaction file identity changed";
    return false;
  }

  struct stat entry_status{};
  if (::fstatat(lock.parent_descriptor, lock.entry_name.c_str(), &entry_status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    detail =
      "failed to inspect transaction lock directory entry: " + std::string(std::strerror(errno));
    return false;
  }
  if (!validate_posix_lock_status(entry_status, "transaction lock directory entry", detail))
    return false;
  const NativeIdentity entry_identity = identity_from_stat(entry_status);
  if (!same_object(entry_identity, descriptor_identity) ||
      !same_object(entry_identity, lock.identity)) {
    detail = "transaction lock directory entry no longer names the locked file";
    return false;
  }
  return true;
#endif
}

bool revalidate_locks(const std::vector<LockedPath> &locks, fs::path &failed_path,
                      std::string &detail) {
  for (const LockedPath &lock : locks) {
    if (!revalidate_lock(lock, detail)) {
      failed_path = lock.path;
      return false;
    }
  }
  return true;
}

bool path_is_within_or_equal(const fs::path &candidate, const fs::path &directory) {
  auto candidate_component = candidate.begin();
  for (auto directory_component = directory.begin(); directory_component != directory.end();
       ++directory_component, ++candidate_component) {
    if (candidate_component == candidate.end() ||
        !spelling_equal(*candidate_component, *directory_component)) {
      return false;
    }
  }
  return true;
}

bool same_directory_digest(const DirectoryTreeDigest &left, const DirectoryTreeDigest &right) {
  return left.entry_count == right.entry_count && left.sha256 == right.sha256;
}

bool same_file_digest(const FileDigest &left, const FileDigest &right) {
  return left.size == right.size && left.sha256 == right.sha256;
}

std::optional<fs::path> absolute_normalized(const fs::path &path, std::string &detail) {
  if (path.empty()) {
    detail = "path is empty";
    return std::nullopt;
  }
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  if (error) {
    detail = "failed to make path absolute: " + error.message();
    return std::nullopt;
  }
  absolute = absolute.lexically_normal();
  if (absolute.filename().empty()) {
    detail = "path does not name a file";
    return std::nullopt;
  }
  return absolute;
}

std::optional<fs::path> canonical_for_comparison(const fs::path &path, std::string &detail) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (error) {
    detail = "failed to resolve path for comparison: " + error.message();
    return std::nullopt;
  }
  return canonical.lexically_normal();
}

bool remove_if_identity(const fs::path &path, const NativeIdentity &identity, bool directory,
                        std::string &detail) {
  const InspectedPath current = inspect_path(path);
  if (!current.detail.empty()) {
    detail = current.detail;
    return false;
  }
  if (!current.exists)
    return true;
  if (!same_object(current.identity, identity) || current.identity.directory != directory) {
    detail = "path identity changed; refusing cleanup";
    return false;
  }
#if defined(_WIN32)
  const BOOL removed = directory ? ::RemoveDirectoryW(path.c_str()) : ::DeleteFileW(path.c_str());
  if (removed == 0) {
    detail = "identity-bound cleanup failed: " + windows_error(::GetLastError());
    return false;
  }
#else
  const int result = directory ? ::rmdir(path.c_str()) : ::unlink(path.c_str());
  if (result != 0) {
    detail = "identity-bound cleanup failed: " + std::string(std::strerror(errno));
    return false;
  }
#endif
  return true;
}

bool remove_staging_directory(StagingDirectory &directory, std::string &detail) {
#if defined(_WIN32)
  if (directory.handle_phase == WindowsStagingDirectoryHandlePhase::IdentityLocked) {
    const InspectedPath current = inspect_path(directory.path);
    if (!current.detail.empty()) {
      detail = current.detail;
      return false;
    }
    if (!current.exists || !same_object(current.identity, directory.identity) ||
        !current.identity.directory) {
      detail = current.exists ? "staging directory path identity changed; refusing cleanup"
                              : "bound staging directory path disappeared before cleanup";
      return false;
    }

    {
      std::error_code enumeration_error;
      const fs::directory_iterator first(directory.path, enumeration_error);
      if (enumeration_error) {
        detail =
          "failed to enumerate staging directory before cleanup: " + enumeration_error.message();
        return false;
      }
      if (first != fs::directory_iterator{}) {
        detail = "staging directory is not empty; refusing root deletion";
        return false;
      }
    }
  }

  if (!finish_empty_bound_staging_directory_deletion(directory, detail))
    return false;

  const InspectedPath current = inspect_path(directory.path);
  if (!current.detail.empty()) {
    detail = "failed to verify staging directory removal: " + current.detail;
    return false;
  }
  if (current.exists) {
    detail = "staging directory path now names a replacement object";
    return false;
  }
  return true;
#else
  return remove_if_identity(directory.path, directory.identity, true, detail);
#endif
}

bool flush_directory(const fs::path &path, std::string &detail) {
#if defined(_WIN32)
  // MOVEFILE_WRITE_THROUGH is used for every Windows rename/publication. Windows does not
  // provide a portable directory FlushFileBuffers contract for user-mode filesystems.
  (void)path;
  (void)detail;
  return true;
#else
  int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                          | O_DIRECTORY
#endif
  );
  if (descriptor < 0) {
    detail = "failed to open parent directory for fsync: " + std::string(std::strerror(errno));
    return false;
  }
  bool ok = true;
  while (::fsync(descriptor) != 0) {
    if (errno == EINTR)
      continue;
    detail = "parent directory fsync failed: " + std::string(std::strerror(errno));
    ok = false;
    break;
  }
  if (::close(descriptor) != 0) {
    if (!detail.empty())
      detail += "; ";
    detail += "parent directory close failed: " + std::string(std::strerror(errno));
    ok = false;
  }
  return ok;
#endif
}

std::atomic<std::uint64_t> staging_counter{0U};

std::optional<StagingDirectory>
create_staging_directory(const fs::path &parent, std::string &detail, bool &cleanup_incomplete) {
  cleanup_incomplete = false;
#if defined(_WIN32)
  std::string volume_detail;
  if (!validate_windows_persistent_acl_support(parent, volume_detail)) {
    detail = "cannot create a private staging directory: " + volume_detail;
    return std::nullopt;
  }
#endif
  const auto tick =
    static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  for (std::uint64_t attempt = 0U; attempt < 128U; ++attempt) {
    const std::uint64_t sequence = staging_counter.fetch_add(1U, std::memory_order_relaxed);
    const fs::path path =
      parent / (".nebula-txn-" + std::to_string(process_id()) + "-" + std::to_string(tick) + "-" +
                std::to_string(sequence) + "-" + std::to_string(attempt));
#if defined(_WIN32)
    WindowsPrivateSecurityDescriptor security;
    std::string security_detail;
    if (!prepare_windows_private_security(WindowsPrivateObjectKind::Directory, security,
                                          security_detail)) {
      detail = "failed to prepare the private staging ACL: " + security_detail;
      return std::nullopt;
    }
    const BOOL created = ::CreateDirectoryW(path.c_str(), security.attributes());
    const DWORD create_error = created == 0 ? ::GetLastError() : ERROR_SUCCESS;
    if (!security.release(security_detail)) {
      detail = "failed to release the private staging ACL: " + security_detail;
      if (created == 0)
        detail =
          "staging directory creation also failed: " + windows_error(create_error) + "; " + detail;
      cleanup_incomplete = true;
      if (created != 0)
        detail += "; the unbound staging directory was preserved to avoid path-based cleanup";
      return std::nullopt;
    }
    if (created == 0) {
      if (create_error == ERROR_ALREADY_EXISTS || create_error == ERROR_FILE_EXISTS)
        continue;
      detail = "failed to create staging directory: " + windows_error(create_error);
      return std::nullopt;
    }
    HANDLE security_handle = ::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (security_handle == INVALID_HANDLE_VALUE) {
      cleanup_incomplete = true;
      detail = "failed to bind the new staging directory for private owner/DACL validation: " +
               windows_error(::GetLastError());
      return std::nullopt;
    }
    StagingDirectory bound_directory(path, {}, security_handle);
    FILE_ATTRIBUTE_TAG_INFO security_attributes{};
    std::string validation_detail;
    const BOOL attributes_inspected =
      ::GetFileInformationByHandleEx(bound_directory.handle, FileAttributeTagInfo,
                                     &security_attributes, sizeof(security_attributes));
    const DWORD inspection_error = attributes_inspected != 0 ? ERROR_SUCCESS : ::GetLastError();
    const bool plain_directory =
      attributes_inspected != 0 &&
      (security_attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
      (security_attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
    const bool private_security =
      plain_directory &&
      validate_windows_private_object_security(
        bound_directory.handle, WindowsPrivateObjectKind::Directory, validation_detail);
    if (!private_security) {
      cleanup_incomplete = true;
      detail =
        attributes_inspected == 0
          ? "failed to inspect the bound staging directory: " + windows_error(inspection_error)
        : !plain_directory          ? "new staging path is not a bound plain directory"
        : validation_detail.empty() ? "new staging directory did not retain its private owner/DACL"
                                    : validation_detail;
      (void)close_staging_directory_handle(bound_directory, detail);
      return std::nullopt;
    }
    if (!read_staging_directory_identity(bound_directory, validation_detail)) {
      cleanup_incomplete = true;
      detail = std::move(validation_detail);
      (void)close_staging_directory_handle(bound_directory, detail);
      return std::nullopt;
    }
    return std::optional<StagingDirectory>(std::move(bound_directory));
#else
    if (::mkdir(path.c_str(), 0700) != 0) {
      if (errno == EEXIST)
        continue;
      detail = "failed to create staging directory: " + std::string(std::strerror(errno));
      return std::nullopt;
    }
    const InspectedPath inspected = inspect_path(path);
    if (!inspected.detail.empty() || !inspected.exists || !inspected.identity.directory) {
      detail = inspected.detail.empty() ? "new staging path is not a directory" : inspected.detail;
      if (::rmdir(path.c_str()) != 0) {
        detail += "; failed to clean the unvalidated staging directory: " +
                  std::string(std::strerror(errno));
        cleanup_incomplete = true;
      }
      return std::nullopt;
    }
    return StagingDirectory(path, inspected.identity);
#endif
  }
  detail = "failed to allocate a unique staging directory after 128 attempts";
  return std::nullopt;
}

bool release_lock(LockedPath &lock, std::string &detail);

bool acquire_lock(const fs::path &path, LockedPath &lock, HostedArtifactTransactionErrorCode &code,
                  std::string &detail) {
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    code = HostedArtifactTransactionErrorCode::Io;
    detail = "failed to open transaction lock: " + windows_error(::GetLastError());
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (::GetFileInformationByHandle(handle, &info) == 0) {
    detail = "failed to inspect transaction lock: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    code = HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U) {
    detail = "transaction lock is a reparse point or directory";
    close_handle_with_detail(handle, detail);
    code = HostedArtifactTransactionErrorCode::UnsafePath;
    return false;
  }
  OVERLAPPED range{};
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                   MAXDWORD, &range) == 0) {
    const DWORD error = ::GetLastError();
    detail = "transaction output is busy: " + windows_error(error);
    close_handle_with_detail(handle, detail);
    code = error == ERROR_LOCK_VIOLATION ? HostedArtifactTransactionErrorCode::Busy
                                         : HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  lock.path = path;
  lock.identity = identity_from_info(handle, info);
  if (!lock.identity.valid) {
    detail = "failed to capture transaction lock file ID or change time: " +
             windows_error(::GetLastError());
    if (::UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &range) == 0)
      detail += "; lock cleanup failed: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    code = HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  lock.handle = handle;
  lock.range = range;
#else
  std::string parent_detail;
  const auto canonical_parent = canonical_for_comparison(path.parent_path(), parent_detail);
  if (!canonical_parent) {
    code = HostedArtifactTransactionErrorCode::UnsafePath;
    detail = "failed to resolve transaction lock parent: " + parent_detail;
    return false;
  }
  // Keep the canonical parent open for the full transaction so every lock
  // revalidation addresses the same directory object via fstatat().
  const int parent_descriptor = ::open(canonical_parent->c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_DIRECTORY
                                                                    | O_DIRECTORY
#endif
#ifdef O_NOFOLLOW
                                                                    | O_NOFOLLOW
#endif
  );
  if (parent_descriptor < 0) {
    code = HostedArtifactTransactionErrorCode::Io;
    detail = "failed to open transaction lock parent: " + std::string(std::strerror(errno));
    return false;
  }
  struct stat parent_status{};
  if (::fstat(parent_descriptor, &parent_status) != 0 || !S_ISDIR(parent_status.st_mode)) {
    detail = "transaction lock parent is not a stable directory";
    close_descriptor_with_detail(parent_descriptor, detail);
    code = HostedArtifactTransactionErrorCode::UnsafePath;
    return false;
  }

  const fs::path entry_name = path.filename();
  // Lock files are persistent protocol entries. A new entry is created
  // exclusively and normalized to 0600; an existing entry is reusable only
  // when it already satisfies the same owner/mode/single-link policy.
  int descriptor = ::openat(parent_descriptor, entry_name.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC
#ifdef O_NOFOLLOW
                              | O_NOFOLLOW
#endif
                            ,
                            0600);
  const bool created = descriptor >= 0;
  if (descriptor < 0 && errno == EEXIST) {
    descriptor = ::openat(parent_descriptor, entry_name.c_str(),
                          O_RDWR | O_CLOEXEC
#ifdef O_NOFOLLOW
                            | O_NOFOLLOW
#endif
    );
  }
  if (descriptor < 0) {
    const int error = errno;
    code = (error == ELOOP || error == EACCES || error == EPERM)
             ? HostedArtifactTransactionErrorCode::UnsafePath
             : HostedArtifactTransactionErrorCode::Io;
    detail = "failed to open transaction lock: " + std::string(std::strerror(error));
    close_descriptor_with_detail(parent_descriptor, detail);
    return false;
  }

  if (created && ::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
    detail =
      "failed to set private transaction lock permissions: " + std::string(std::strerror(errno));
    close_descriptor_with_detail(descriptor, detail);
    close_descriptor_with_detail(parent_descriptor, detail);
    code = HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    detail = "failed to inspect transaction lock: " + std::string(std::strerror(errno));
    close_descriptor_with_detail(descriptor, detail);
    close_descriptor_with_detail(parent_descriptor, detail);
    code = HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  if (!validate_posix_lock_status(status, "transaction lock", detail)) {
    close_descriptor_with_detail(descriptor, detail);
    close_descriptor_with_detail(parent_descriptor, detail);
    code = HostedArtifactTransactionErrorCode::UnsafePath;
    return false;
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int error = errno;
    detail = "transaction output is busy: " + std::string(std::strerror(error));
    close_descriptor_with_detail(descriptor, detail);
    close_descriptor_with_detail(parent_descriptor, detail);
    code = (error == EWOULDBLOCK || error == EAGAIN) ? HostedArtifactTransactionErrorCode::Busy
                                                     : HostedArtifactTransactionErrorCode::Io;
    return false;
  }
  lock.path = path;
  lock.identity = identity_from_stat(status);
  lock.descriptor = descriptor;
  lock.parent_descriptor = parent_descriptor;
  lock.entry_name = entry_name;
  lock.parent_identity = identity_from_stat(parent_status);
  if (!revalidate_lock(lock, detail)) {
    std::string cleanup_detail;
    if (!release_lock(lock, cleanup_detail)) {
      if (!detail.empty())
        detail += "; ";
      detail += "lock cleanup failed: " + cleanup_detail;
      code = HostedArtifactTransactionErrorCode::CleanupIncomplete;
    } else {
      code = HostedArtifactTransactionErrorCode::ConcurrentModification;
    }
    return false;
  }
#endif
  return true;
}

bool release_lock(LockedPath &lock, std::string &detail) {
  bool ok = true;
#if defined(_WIN32)
  if (lock.handle != INVALID_HANDLE_VALUE) {
    if (::UnlockFileEx(lock.handle, 0, MAXDWORD, MAXDWORD, &lock.range) == 0) {
      detail = "failed to unlock transaction lock: " + windows_error(::GetLastError());
      ok = false;
    }
    if (::CloseHandle(lock.handle) == 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "failed to close transaction lock: " + windows_error(::GetLastError());
      ok = false;
    } else {
      lock.handle = INVALID_HANDLE_VALUE;
    }
  }
#else
  if (lock.descriptor >= 0) {
    if (::flock(lock.descriptor, LOCK_UN) != 0) {
      detail = "failed to unlock transaction lock: " + std::string(std::strerror(errno));
      ok = false;
    }
    if (::close(lock.descriptor) != 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "failed to close transaction lock: " + std::string(std::strerror(errno));
      ok = false;
    }
    lock.descriptor = -1;
  }
  if (lock.parent_descriptor >= 0) {
    if (::close(lock.parent_descriptor) != 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "failed to close transaction lock parent: " + std::string(std::strerror(errno));
      ok = false;
    }
    lock.parent_descriptor = -1;
  }
#endif
  return ok;
}

#if defined(__APPLE__)
constexpr std::size_t kMaxExtendedAttributeNamesBytes = 64U * 1024U;
constexpr std::size_t kMaxExtendedAttributeSnapshotBytes = 1024U * 1024U;
constexpr std::size_t kMaxAccessControlListBytes = 64U * 1024U;
constexpr std::size_t kMaxDarwinProvenanceBytes = 4U * 1024U;
constexpr std::string_view kDarwinProvenanceAttribute = "com.apple.provenance";

bool capture_darwin_extended_attributes(int descriptor, PlatformSecuritySnapshot &snapshot,
                                        std::string &detail) {
  errno = 0;
  const ssize_t required = ::flistxattr(descriptor, nullptr, 0U, XATTR_SHOWCOMPRESSION);
  if (required < 0) {
    if (errno == ENOTSUP) {
      snapshot.extended_attributes_supported = false;
      return true;
    }
    detail =
      "failed to enumerate staged output extended attributes: " + std::string(std::strerror(errno));
    return false;
  }
  if (static_cast<std::uint64_t>(required) > kMaxExtendedAttributeNamesBytes) {
    detail = "staged output extended-attribute name list exceeds the bounded snapshot size";
    return false;
  }

  snapshot.extended_attributes_supported = true;
  std::vector<char> names(static_cast<std::size_t>(required));
  if (required > 0) {
    const ssize_t observed =
      ::flistxattr(descriptor, names.data(), names.size(), XATTR_SHOWCOMPRESSION);
    if (observed != required) {
      detail = observed < 0 ? "failed to read staged output extended-attribute names: " +
                                std::string(std::strerror(errno))
                            : "staged output extended-attribute names changed while being observed";
      return false;
    }
  }

  std::size_t total_bytes = names.size();
  for (std::size_t offset = 0U; offset < names.size();) {
    const void *terminator = std::memchr(names.data() + offset, '\0', names.size() - offset);
    if (terminator == nullptr) {
      detail = "staged output returned a malformed extended-attribute name list";
      return false;
    }
    const auto *end = static_cast<const char *>(terminator);
    const std::size_t length = static_cast<std::size_t>(end - (names.data() + offset));
    if (length == 0U) {
      detail = "staged output returned an empty extended-attribute name";
      return false;
    }
    std::string name(names.data() + offset, length);
    offset += length + 1U;

    errno = 0;
    const ssize_t value_size =
      ::fgetxattr(descriptor, name.c_str(), nullptr, 0U, 0U, XATTR_SHOWCOMPRESSION);
    if (value_size < 0) {
      detail =
        "failed to size a staged output extended attribute: " + std::string(std::strerror(errno));
      return false;
    }
    if (static_cast<std::uint64_t>(value_size) > kMaxExtendedAttributeSnapshotBytes ||
        total_bytes > kMaxExtendedAttributeSnapshotBytes - static_cast<std::size_t>(value_size)) {
      detail = "staged output extended attributes exceed the bounded snapshot size";
      return false;
    }
    std::vector<std::uint8_t> value(static_cast<std::size_t>(value_size));
    if (value_size > 0) {
      const ssize_t observed = ::fgetxattr(descriptor, name.c_str(), value.data(), value.size(), 0U,
                                           XATTR_SHOWCOMPRESSION);
      if (observed != value_size) {
        detail = observed < 0 ? "failed to read a staged output extended attribute: " +
                                  std::string(std::strerror(errno))
                              : "a staged output extended attribute changed while being observed";
        return false;
      }
    }
    total_bytes += value.size();
    if (!snapshot.extended_attributes.emplace(std::move(name), std::move(value)).second) {
      detail = "staged output returned duplicate extended-attribute names";
      return false;
    }
  }
  return true;
}

bool capture_darwin_access_control_list(int descriptor, PlatformSecuritySnapshot &snapshot,
                                        std::string &detail) {
  errno = 0;
  acl_t access_control_list = ::acl_get_fd_np(descriptor, ACL_TYPE_EXTENDED);
  if (access_control_list == nullptr) {
    if (errno == ENOENT) {
      snapshot.access_control_lists_supported = true;
      snapshot.access_control_list_has_entries = false;
      snapshot.access_control_list.clear();
      return true;
    }
    if (errno == EOPNOTSUPP) {
      snapshot.access_control_lists_supported = false;
      return true;
    }
    detail =
      "failed to read staged output access-control list: " + std::string(std::strerror(errno));
    return false;
  }

  snapshot.access_control_lists_supported = true;
  acl_entry_t first_entry = nullptr;
  const int entry_result = ::acl_get_entry(access_control_list, ACL_FIRST_ENTRY, &first_entry);
  if (entry_result < 0) {
    detail = "failed to inspect staged output access-control entries: " +
             std::string(std::strerror(errno));
    (void)::acl_free(access_control_list);
    return false;
  }
  snapshot.access_control_list_has_entries = entry_result == 1;
  const ssize_t acl_size = ::acl_size(access_control_list);
  if (acl_size < 0 || static_cast<std::uint64_t>(acl_size) > kMaxAccessControlListBytes) {
    detail = acl_size < 0 ? "failed to size staged output access-control list: " +
                              std::string(std::strerror(errno))
                          : "staged output access-control list exceeds the bounded snapshot size";
    (void)::acl_free(access_control_list);
    return false;
  }
  snapshot.access_control_list.resize(static_cast<std::size_t>(acl_size));
  if (acl_size > 0 && ::acl_copy_ext(snapshot.access_control_list.data(), access_control_list,
                                     acl_size) != acl_size) {
    detail =
      "failed to serialize staged output access-control list: " + std::string(std::strerror(errno));
    (void)::acl_free(access_control_list);
    return false;
  }
  const int acl_release = ::acl_free(access_control_list);
  if (acl_release != 0) {
    detail = "failed to release staged output access-control snapshot";
    return false;
  }
  return true;
}
#endif

#if defined(_WIN32)
bool capture_platform_security(HANDLE handle, PlatformSecuritySnapshot &snapshot,
                               std::string &detail) {
  (void)handle;
  (void)snapshot;
  (void)detail;
  return true;
}
#else
bool capture_platform_security(int descriptor, PlatformSecuritySnapshot &snapshot,
                               std::string &detail) {
#if defined(__APPLE__)
  return capture_darwin_extended_attributes(descriptor, snapshot, detail) &&
         capture_darwin_access_control_list(descriptor, snapshot, detail);
#else
  (void)descriptor;
  (void)snapshot;
  (void)detail;
  return true;
#endif
}
#endif

bool is_controlled_darwin_provenance_transition(const PlatformSecuritySnapshot &sealed,
                                                const PlatformSecuritySnapshot &current) {
#if defined(__APPLE__)
  if (!sealed.extended_attributes_supported || !current.extended_attributes_supported ||
      !sealed.access_control_lists_supported || !current.access_control_lists_supported ||
      sealed.access_control_list_has_entries != current.access_control_list_has_entries ||
      sealed.access_control_list != current.access_control_list ||
      sealed.extended_attributes.contains(std::string(kDarwinProvenanceAttribute)) ||
      current.extended_attributes.size() != sealed.extended_attributes.size() + 1U) {
    return false;
  }
  const auto provenance = current.extended_attributes.find(std::string(kDarwinProvenanceAttribute));
  if (provenance == current.extended_attributes.end() || provenance->second.empty() ||
      provenance->second.size() > kMaxDarwinProvenanceBytes) {
    return false;
  }
  return std::all_of(sealed.extended_attributes.begin(), sealed.extended_attributes.end(),
                     [&current](const auto &entry) {
                       const auto found = current.extended_attributes.find(entry.first);
                       return found != current.extended_attributes.end() &&
                              found->second == entry.second;
                     });
#else
  (void)sealed;
  (void)current;
  return false;
#endif
}

bool has_bounded_darwin_provenance(const PlatformSecuritySnapshot &snapshot) {
#if defined(__APPLE__)
  if (!snapshot.extended_attributes_supported)
    return false;
  const auto provenance =
    snapshot.extended_attributes.find(std::string(kDarwinProvenanceAttribute));
  return provenance != snapshot.extended_attributes.end() && !provenance->second.empty() &&
         provenance->second.size() <= kMaxDarwinProvenanceBytes;
#else
  (void)snapshot;
  return false;
#endif
}

struct AdoptedFile {
  NativeIdentity identity;
  std::optional<FileDigest> digest;
  PlatformSecuritySnapshot security;
};

bool reconcile_sealed_publication_entry(PublicationEntry &entry, AdoptedFile observation,
                                        bool allow_known_publication_change_time,
                                        bool allow_darwin_provenance_transition,
                                        std::string &detail) {
  if (!observation.digest.has_value() || !entry.staged_digest.has_value()) {
    detail = "sealed output digest is unavailable";
    return false;
  }
  if (!same_file_digest(*observation.digest, *entry.staged_digest)) {
    detail = "staged output content digest changed after seal";
    return false;
  }
  if (!same_snapshot_except_change_time(observation.identity, entry.staged_identity)) {
    detail = describe_snapshot_difference(entry.staged_identity, observation.identity);
    return false;
  }

  const bool exact_security = same_platform_security(observation.security, entry.staged_security);
  const bool exact_change_time =
    observation.identity.changed_low == entry.staged_identity.changed_low &&
    observation.identity.changed_high == entry.staged_identity.changed_high;
  // macOS may add its opaque provenance xattr asynchronously, or rewrite the
  // same bounded value after it first becomes visible. This is not treated as
  // provenance authentication: one metadata-only transition is accepted only
  // after content, object identity, mode/owner/flags, ACL, and every other
  // xattr were independently proven unchanged. The refreshed snapshot then
  // closes this exception for the remainder of the transaction.
  const bool controlled_provenance_transition =
    allow_darwin_provenance_transition && entry.darwin_metadata_stabilization_available &&
    !exact_change_time &&
    is_controlled_darwin_provenance_transition(entry.staged_security, observation.security);
  const bool controlled_provenance_rewrite =
    allow_darwin_provenance_transition && entry.darwin_metadata_stabilization_available &&
    !allow_known_publication_change_time && !exact_change_time && exact_security &&
    has_bounded_darwin_provenance(observation.security);
  if ((!allow_known_publication_change_time && !exact_change_time &&
       !controlled_provenance_transition && !controlled_provenance_rewrite) ||
      (!exact_security && !controlled_provenance_transition)) {
    detail = exact_security ? "staged output change time changed outside a publication operation"
                            : "staged output security metadata changed after seal";
    return false;
  }

  entry.staged_identity = observation.identity;
  entry.staged_security = std::move(observation.security);
  if (controlled_provenance_transition || controlled_provenance_rewrite)
    entry.darwin_metadata_stabilization_available = false;
  return true;
}

bool validate_sealed_metadata_policy(const PublicationEntry &entry, std::string &detail) {
  if (!entry.staged_identity.valid || entry.staged_identity.directory ||
      entry.staged_identity.links != 1U) {
    detail = "staged metadata is not a private single-link regular file";
    return false;
  }
#if !defined(_WIN32)
  constexpr std::uint64_t permission_bits =
    S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX;
  constexpr std::uint64_t required_permissions = S_IRUSR | S_IWUSR;
  if ((entry.staged_identity.mode & permission_bits) != required_permissions ||
      entry.staged_identity.owner != static_cast<std::uint64_t>(::geteuid())) {
    detail = "staged metadata must be owned by the effective user with exactly 0600 permissions";
    return false;
  }
#if defined(__APPLE__)
  if (entry.staged_identity.flags != 0U) {
    detail = "staged metadata carries unexpected file flags";
    return false;
  }
  if (entry.staged_security.access_control_lists_supported &&
      entry.staged_security.access_control_list_has_entries) {
    detail = "staged metadata carries an extended access-control list";
    return false;
  }
  if (entry.staged_security.extended_attributes_supported) {
    for (const auto &[name, value] : entry.staged_security.extended_attributes) {
      if (name != kDarwinProvenanceAttribute || value.empty() ||
          value.size() > kMaxDarwinProvenanceBytes) {
        detail = "staged metadata carries an unexpected extended attribute";
        return false;
      }
    }
  }
#endif
#endif
  return true;
}

std::optional<AdoptedFile> observe_regular_file(const fs::path &path, bool hash,
                                                bool require_private, bool flush_before_observation,
                                                bool capture_security_metadata,
                                                std::string &detail) {
#if defined(_WIN32)
  const DWORD access = GENERIC_READ | (flush_before_observation ? GENERIC_WRITE : 0U);
  HANDLE handle = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    detail = "failed to open staged output: " + windows_error(::GetLastError());
    return std::nullopt;
  }
  BY_HANDLE_FILE_INFORMATION before_info{};
  if (::GetFileInformationByHandle(handle, &before_info) == 0 ||
      (before_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) !=
        0U ||
      (require_private && before_info.nNumberOfLinks != 1U)) {
    detail = require_private ? "staged output is not a private regular file"
                             : "observed input is not a plain regular file";
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  const NativeIdentity before = identity_from_info(handle, before_info);
  if (!before.valid) {
    detail =
      "failed to capture staged output file ID or change time: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  if (before.size > kMaxReusableArtifactBytes) {
    detail = "staged output exceeds the bounded artifact size";
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  if (flush_before_observation && ::FlushFileBuffers(handle) == 0) {
    detail = "staged output flush failed: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  std::optional<FileDigest> digest;
  if (hash) {
    LARGE_INTEGER zero{};
    if (::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) == 0) {
      detail = "failed to rewind staged artifact: " + windows_error(::GetLastError());
      close_handle_with_detail(handle, detail);
      return std::nullopt;
    }
    Sha256Digest sha;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t total = 0U;
    for (;;) {
      DWORD count = 0U;
      if (::ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) ==
          0) {
        detail = "failed to hash staged artifact: " + windows_error(::GetLastError());
        close_handle_with_detail(handle, detail);
        return std::nullopt;
      }
      if (count == 0U)
        break;
      total += count;
      if (total > kMaxReusableArtifactBytes) {
        detail = "staged artifact exceeds the bounded artifact size";
        close_handle_with_detail(handle, detail);
        return std::nullopt;
      }
      sha.update(std::span<const std::uint8_t>(buffer.data(), count));
    }
    digest = FileDigest{total, sha.finish_hex()};
  }
  PlatformSecuritySnapshot security;
  if (capture_security_metadata && !capture_platform_security(handle, security, detail)) {
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  BY_HANDLE_FILE_INFORMATION after_info{};
  if (::GetFileInformationByHandle(handle, &after_info) == 0) {
    detail = "failed to re-inspect staged output: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  const NativeIdentity after = identity_from_info(handle, after_info);
  if (!after.valid) {
    detail = "failed to recapture staged output file ID or change time: " +
             windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  const InspectedPath path_after = inspect_path(path);
  BY_HANDLE_FILE_INFORMATION final_info{};
  if (::GetFileInformationByHandle(handle, &final_info) == 0) {
    detail =
      "failed to inspect staged output after path rebinding: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  const NativeIdentity final_identity = identity_from_info(handle, final_info);
  if (!final_identity.valid) {
    detail =
      "failed to recapture staged output after path rebinding: " + windows_error(::GetLastError());
    close_handle_with_detail(handle, detail);
    return std::nullopt;
  }
  if (::CloseHandle(handle) == 0) {
    detail = "failed to close staged output: " + windows_error(::GetLastError());
    return std::nullopt;
  }
  if (!same_snapshot(before, after) || !path_after.detail.empty() || !path_after.exists ||
      !same_snapshot(after, path_after.identity) ||
      !same_snapshot(path_after.identity, final_identity)) {
    detail = require_private
               ? "staged output changed while it was being sealed"
               : "observed input changed identity or content while it was being hashed";
    return std::nullopt;
  }
  return AdoptedFile{final_identity, std::move(digest), std::move(security)};
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                                                | O_NOFOLLOW
#endif
  );
  if (descriptor < 0) {
    detail = "failed to open staged output: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  struct stat before_status{};
  if (::fstat(descriptor, &before_status) != 0 || !S_ISREG(before_status.st_mode) ||
      (require_private && before_status.st_nlink != 1)) {
    detail = require_private ? "staged output is not a private regular file"
                             : "observed input is not a plain regular file";
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  const NativeIdentity before = identity_from_stat(before_status);
  if (before.size > kMaxReusableArtifactBytes) {
    detail = "staged output exceeds the bounded artifact size";
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  while (flush_before_observation && ::fsync(descriptor) != 0) {
    if (errno == EINTR)
      continue;
    detail = "staged output fsync failed: " + std::string(std::strerror(errno));
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  std::optional<FileDigest> digest;
  if (hash) {
    if (::lseek(descriptor, 0, SEEK_SET) < 0) {
      detail = "failed to rewind staged artifact: " + std::string(std::strerror(errno));
      close_descriptor_with_detail(descriptor, detail);
      return std::nullopt;
    }
    Sha256Digest sha;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t total = 0U;
    for (;;) {
      const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) {
        detail = "failed to hash staged artifact: " + std::string(std::strerror(errno));
        close_descriptor_with_detail(descriptor, detail);
        return std::nullopt;
      }
      if (count == 0)
        break;
      total += static_cast<std::uint64_t>(count);
      if (total > kMaxReusableArtifactBytes) {
        detail = "staged artifact exceeds the bounded artifact size";
        close_descriptor_with_detail(descriptor, detail);
        return std::nullopt;
      }
      sha.update(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(count)));
    }
    digest = FileDigest{total, sha.finish_hex()};
  }
  PlatformSecuritySnapshot security;
  if (capture_security_metadata && !capture_platform_security(descriptor, security, detail)) {
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  struct stat after_status{};
  if (::fstat(descriptor, &after_status) != 0) {
    detail = "failed to re-inspect staged output: " + std::string(std::strerror(errno));
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  const NativeIdentity after = identity_from_stat(after_status);
  const InspectedPath path_after = inspect_path(path);
  struct stat final_status{};
  if (::fstat(descriptor, &final_status) != 0) {
    detail =
      "failed to inspect staged output after path rebinding: " + std::string(std::strerror(errno));
    close_descriptor_with_detail(descriptor, detail);
    return std::nullopt;
  }
  const NativeIdentity final_identity = identity_from_stat(final_status);
  if (::close(descriptor) != 0) {
    detail = "failed to close staged output: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  if (!same_snapshot(before, after) || !path_after.detail.empty() || !path_after.exists ||
      !same_snapshot(after, path_after.identity) ||
      !same_snapshot(path_after.identity, final_identity)) {
    detail = require_private
               ? "staged output changed while it was being sealed"
               : "observed input changed identity or content while it was being hashed";
    return std::nullopt;
  }
  return AdoptedFile{final_identity, std::move(digest), std::move(security)};
#endif
}

std::optional<AdoptedFile> adopt_staged_file(const fs::path &path, bool hash, std::string &detail) {
  return observe_regular_file(path, hash, true, true, false, detail);
}

std::optional<NativeIdentity> write_staged_metadata(const fs::path &path, std::string_view payload,
                                                    std::string &detail) {
#if defined(_WIN32)
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    detail = "failed to create staged metadata exclusively: " + windows_error(::GetLastError());
    return std::nullopt;
  }
  std::size_t offset = 0U;
  bool ok = true;
  while (offset < payload.size()) {
    const DWORD requested = static_cast<DWORD>(
      std::min<std::size_t>(payload.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0U;
    if (::WriteFile(handle, payload.data() + offset, requested, &written, nullptr) == 0 ||
        written == 0U) {
      detail = "staged metadata write failed: " + windows_error(::GetLastError());
      ok = false;
      break;
    }
    offset += written;
  }
  if (ok && ::FlushFileBuffers(handle) == 0) {
    detail = "staged metadata flush failed: " + windows_error(::GetLastError());
    ok = false;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (ok && ::GetFileInformationByHandle(handle, &info) == 0) {
    detail = "failed to inspect staged metadata: " + windows_error(::GetLastError());
    ok = false;
  }
  NativeIdentity identity;
  if (ok) {
    identity = identity_from_info(handle, info);
    if (!identity.valid) {
      detail = "failed to capture staged metadata file ID or change time: " +
               windows_error(::GetLastError());
      ok = false;
    }
  }
  if (::CloseHandle(handle) == 0) {
    if (!detail.empty())
      detail += "; ";
    detail += "failed to close staged metadata: " + windows_error(::GetLastError());
    ok = false;
  }
  if (!ok)
    return std::nullopt;
  return identity;
#else
  const int descriptor = ::open(path.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC
#ifdef O_NOFOLLOW
                                  | O_NOFOLLOW
#endif
                                ,
                                0600);
  if (descriptor < 0) {
    detail = "failed to create staged metadata exclusively: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  std::size_t offset = 0U;
  bool ok = true;
  while (offset < payload.size()) {
    const ssize_t written = ::write(descriptor, payload.data() + offset, payload.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      detail = written == 0 ? "staged metadata write made no progress"
                            : "staged metadata write failed: " + std::string(std::strerror(errno));
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
  while (ok && ::fsync(descriptor) != 0) {
    if (errno == EINTR)
      continue;
    detail = "staged metadata fsync failed: " + std::string(std::strerror(errno));
    ok = false;
  }
  struct stat status{};
  if (ok && ::fstat(descriptor, &status) != 0) {
    detail = "failed to inspect staged metadata: " + std::string(std::strerror(errno));
    ok = false;
  }
  if (::close(descriptor) != 0) {
    if (!detail.empty())
      detail += "; ";
    detail += "failed to close staged metadata: " + std::string(std::strerror(errno));
    ok = false;
  }
  if (!ok)
    return std::nullopt;
  return identity_from_stat(status);
#endif
}

bool move_no_replace(const fs::path &source, const fs::path &destination, std::string &detail) {
#if defined(_WIN32)
  if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
    detail = "no-replace move failed: " + windows_error(::GetLastError());
    return false;
  }
#elif defined(__linux__)
  constexpr unsigned kRenameNoReplace = 1U;
  long result;
  do {
    result = ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                       kRenameNoReplace);
  } while (result != 0 && errno == EINTR);
  if (result != 0) {
    detail = "atomic no-replace move failed: " + std::string(std::strerror(errno));
    return false;
  }
#elif defined(__APPLE__)
  int result;
  do {
    result = ::renameatx_np(AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCL);
  } while (result != 0 && errno == EINTR);
  if (result != 0) {
    detail = "atomic no-replace move failed: " + std::string(std::strerror(errno));
    return false;
  }
#else
  (void)source;
  (void)destination;
  detail = "this host does not provide an atomic no-replace rename primitive";
  return false;
#endif
  return true;
}

bool rename_to_backup(const fs::path &source, const fs::path &backup, std::string &detail) {
  if (move_no_replace(source, backup, detail))
    return true;
  detail = "failed to create identity-bound backup: " + detail;
  return false;
}

struct PublicationProgress {
  bool linked = false;
  bool staging_link_removed = false;
  std::string detail;
};

PublicationProgress publish_no_replace(const fs::path &staged, const fs::path &destination) {
  PublicationProgress progress;
#if defined(_WIN32)
  if (::MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) == 0) {
    progress.detail =
      "failed to publish staged output without replacement: " + windows_error(::GetLastError());
    return progress;
  }
  progress.linked = true;
  progress.staging_link_removed = true;
  if (consume_test_fault(InternalFaultPoint::AfterPublishLink)) {
    progress.detail = "injected failure after publishing the destination";
    return progress;
  }
#else
  if (::link(staged.c_str(), destination.c_str()) != 0) {
    progress.detail =
      "failed to publish staged output without replacement: " + std::string(std::strerror(errno));
    return progress;
  }
  progress.linked = true;
  if (consume_test_fault(InternalFaultPoint::AfterPublishLink)) {
    progress.detail = "injected failure after publishing the destination link";
    return progress;
  }
  if (::unlink(staged.c_str()) != 0) {
    progress.detail = "published output but failed to remove its staging link: " +
                      std::string(std::strerror(errno));
    return progress;
  }
#endif
  progress.staging_link_removed = true;
  return progress;
}

bool restore_backup(const fs::path &backup, const fs::path &destination, std::string &detail) {
  if (move_no_replace(backup, destination, detail))
    return true;
  detail = "failed to restore backup without replacing another file: " + detail;
  return false;
}

void append_detail(std::string &aggregate, const fs::path &path, std::string_view detail) {
  if (!aggregate.empty())
    aggregate += "; ";
  aggregate += path.string() + ": " + std::string(detail);
}

std::string rollback_publication(std::vector<PublicationEntry> &entries) {
  std::string aggregate;
  std::set<fs::path> affected_parents;
  for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
    PublicationEntry &entry = *iterator;
    if (entry.published || entry.backed_up || entry.rollback_occupied)
      affected_parents.insert(entry.destination.parent_path());
    if (entry.rollback_occupied) {
      const InspectedPath quarantined = inspect_path(entry.rollback_quarantine);
      if (!quarantined.detail.empty() || !quarantined.exists ||
          !same_content_snapshot(quarantined.identity, entry.rollback_identity)) {
        append_detail(aggregate, entry.rollback_quarantine,
                      quarantined.detail.empty()
                        ? "rollback quarantine identity changed; preserving it"
                        : quarantined.detail);
      } else if (same_content_snapshot(quarantined.identity, entry.staged_identity)) {
        std::string detail;
        if (!remove_if_identity(entry.rollback_quarantine, entry.rollback_identity, false,
                                detail)) {
          append_detail(aggregate, entry.rollback_quarantine, detail);
        } else {
          entry.rollback_occupied = false;
        }
      } else {
        const InspectedPath destination = inspect_path(entry.destination);
        if (!destination.detail.empty() || destination.exists) {
          append_detail(aggregate, entry.destination,
                        destination.detail.empty()
                          ? "destination occupied while a displaced non-transaction file is "
                            "preserved in rollback quarantine"
                          : destination.detail);
        } else {
          std::string detail;
          if (!restore_backup(entry.rollback_quarantine, entry.destination, detail)) {
            append_detail(aggregate, entry.rollback_quarantine,
                          "failed to restore displaced non-transaction file: " + detail);
          } else {
            entry.rollback_occupied = false;
            append_detail(aggregate, entry.destination,
                          "restored a displaced non-transaction file without replacement");
          }
        }
      }
    }
    if (entry.published) {
      std::string detail;
      if (!move_no_replace(entry.destination, entry.rollback_quarantine, detail)) {
        append_detail(aggregate, entry.destination,
                      "failed to quarantine published output without replacement: " + detail);
      } else {
        entry.published = false;
        const InspectedPath quarantined = inspect_path(entry.rollback_quarantine);
        if (!quarantined.detail.empty() || !quarantined.exists ||
            !same_content_snapshot(quarantined.identity, entry.staged_identity)) {
          if (quarantined.exists && quarantined.identity.valid) {
            entry.rollback_identity = quarantined.identity;
            entry.rollback_occupied = true;
          }
          std::string restore_detail;
          if (!restore_backup(entry.rollback_quarantine, entry.destination, restore_detail)) {
            append_detail(aggregate, entry.rollback_quarantine,
                          "published destination changed and its displaced file could not be "
                          "restored: " +
                            restore_detail);
          } else {
            entry.rollback_occupied = false;
            append_detail(aggregate, entry.destination,
                          "published destination changed during rollback; the displaced file "
                          "was restored without replacement");
          }
        } else {
          entry.rollback_identity = quarantined.identity;
          entry.rollback_occupied = true;
          if (!remove_if_identity(entry.rollback_quarantine, entry.rollback_identity, false,
                                  detail)) {
            append_detail(aggregate, entry.rollback_quarantine, detail);
          } else {
            entry.rollback_occupied = false;
          }
        }
      }
    }
    if (entry.backed_up) {
      const InspectedPath backup = inspect_path(entry.backup);
      if (!backup.detail.empty() || !backup.exists ||
          !same_content_snapshot(backup.identity, entry.backup_identity)) {
        append_detail(aggregate, entry.backup,
                      backup.detail.empty() ? "backup identity changed; refusing restoration"
                                            : backup.detail);
        continue;
      }
      const InspectedPath destination = inspect_path(entry.destination);
      if (!destination.detail.empty() || destination.exists) {
        append_detail(aggregate, entry.destination,
                      destination.detail.empty() ? "destination occupied during rollback"
                                                 : destination.detail);
        continue;
      }
      std::string detail;
      if (!restore_backup(entry.backup, entry.destination, detail)) {
        append_detail(aggregate, entry.backup, detail);
        continue;
      }
      const InspectedPath restored = inspect_path(entry.destination);
      if (!restored.detail.empty() || !restored.exists ||
          !same_content_snapshot(restored.identity, entry.backup_identity)) {
        append_detail(aggregate, entry.destination,
                      restored.detail.empty() ? "restored destination identity mismatch"
                                              : restored.detail);
      } else {
        entry.backed_up = false;
      }
    }
  }
  for (const fs::path &parent : affected_parents) {
    std::string detail;
    if (!flush_directory(parent, detail))
      append_detail(aggregate, parent, detail);
  }
  return aggregate;
}

std::string cleanup_begin_resources(std::vector<StagingDirectory> &directories,
                                    std::vector<LockedPath> &locks) {
  std::string aggregate;
  for (auto iterator = directories.rbegin(); iterator != directories.rend(); ++iterator) {
    std::string detail;
    if (!remove_staging_directory(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  for (auto iterator = locks.rbegin(); iterator != locks.rend(); ++iterator) {
    std::string detail;
    if (!release_lock(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  return aggregate;
}

} // namespace

struct HostedArtifactTransaction::Impl {
  HostedArtifactTransactionState state = HostedArtifactTransactionState::Open;
  HostedArtifactStagingPaths public_staging;
  std::vector<PublicationEntry> entries;
  std::vector<StagingDirectory> staging_directories;
  std::vector<LockedPath> locks;
  std::vector<fs::path> protected_paths;
  std::vector<NativeIdentity> protected_identities;
  std::vector<FileDigest> protected_digests;
  std::vector<fs::path> protected_directory_paths;
  std::vector<DirectoryTreeDigest> protected_directory_digests;
  std::optional<FileDigest> sealed_artifact_digest;
  std::size_t artifact_index = 0U;
  std::size_t metadata_index = 0U;
};

HostedArtifactTransaction::HostedArtifactTransaction(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {}

HostedArtifactTransaction::~HostedArtifactTransaction() {
  if (impl_ && impl_->state != HostedArtifactTransactionState::Closed) {
    const HostedArtifactTransactionResult cleanup =
      impl_->state == HostedArtifactTransactionState::Committed ? finish() : abort();
    if (!cleanup.ok()) {
      std::cerr << "nebula: fatal: hosted artifact transaction cleanup failed during destruction: "
                << cleanup.error.operation;
      if (!cleanup.error.path.empty())
        std::cerr << " (" << cleanup.error.path.string() << ")";
      if (!cleanup.error.detail.empty())
        std::cerr << ": " << cleanup.error.detail;
      std::cerr << '\n';
    }
  }
}

HostedArtifactTransactionState HostedArtifactTransaction::state() const noexcept {
  return impl_ ? impl_->state : HostedArtifactTransactionState::Closed;
}

const HostedArtifactStagingPaths &HostedArtifactTransaction::staging_paths() const noexcept {
  return impl_->public_staging;
}

std::optional<FileDigest> HostedArtifactTransaction::sealed_artifact_digest() const {
  return impl_ ? impl_->sealed_artifact_digest : std::nullopt;
}

HostedArtifactTransactionResult HostedArtifactTransaction::protect_additional_inputs(
  const std::vector<HostedArtifactProtectedInput> &inputs) {
  if (!impl_ || impl_->state != HostedArtifactTransactionState::Open)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState,
                          "protect-additional-inputs", {},
                          "additional inputs may only be protected while the transaction is open");
  if (inputs.empty())
    return success_result();

  using PathIndex = std::map<fs::path, std::size_t, SpellingPathLess>;
  using ObjectIndex = std::map<NativeObjectKey, std::size_t>;
  PathIndex protected_spelling_index;
  PathIndex protected_canonical_index;
  ObjectIndex protected_object_index;
  for (std::size_t index = 0U; index < impl_->protected_paths.size(); ++index) {
    std::string detail;
    const auto canonical = canonical_for_comparison(impl_->protected_paths[index], detail);
    if (!canonical)
      return failure_result(HostedArtifactTransactionErrorCode::Io, "resolve-protected-input",
                            impl_->protected_paths[index], detail);
    protected_spelling_index.try_emplace(impl_->protected_paths[index], index);
    protected_canonical_index.try_emplace(*canonical, index);
    if (impl_->protected_identities[index].valid) {
      protected_object_index.try_emplace(native_object_key(impl_->protected_identities[index]),
                                         index);
    }
  }

  std::vector<fs::path> pending_paths;
  std::vector<NativeIdentity> pending_identities;
  std::vector<FileDigest> pending_digests;
  pending_paths.reserve(inputs.size());
  pending_identities.reserve(inputs.size());
  pending_digests.reserve(inputs.size());
  PathIndex pending_spelling_index;
  PathIndex pending_canonical_index;
  ObjectIndex pending_object_index;
  for (const HostedArtifactProtectedInput &input : inputs) {
    const fs::path &path = input.path;
    std::string detail;
    const auto normalized = absolute_normalized(path, detail);
    if (!normalized)
      return failure_result(HostedArtifactTransactionErrorCode::InvalidPlan,
                            "normalize-additional-input", path, detail);
    const auto canonical = canonical_for_comparison(*normalized, detail);
    if (!canonical)
      return failure_result(HostedArtifactTransactionErrorCode::UnsafePath,
                            "resolve-additional-input", *normalized, detail);
    const auto observed = observe_regular_file(*normalized, true, false, false, false, detail);
    if (!observed.has_value() || !observed->digest.has_value()) {
      return failure_result(
        HostedArtifactTransactionErrorCode::UnsafePath, "inspect-additional-input", *normalized,
        detail.empty() ? "additional input is not a stable regular file" : detail);
    }
    if (input.expected_digest.has_value() &&
        !same_file_digest(*observed->digest, *input.expected_digest)) {
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "verify-additional-input", *normalized,
                            "additional input no longer matches its resolved content identity");
    }

    const auto validate_protected_duplicate =
      [&](std::size_t index,
          std::string_view changed_detail) -> std::optional<HostedArtifactTransactionResult> {
      if (same_snapshot(observed->identity, impl_->protected_identities[index]) &&
          same_file_digest(*observed->digest, impl_->protected_digests[index])) {
        return std::nullopt;
      }
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "reprotect-input", *normalized, std::string(changed_detail));
    };
    std::optional<std::size_t> protected_path_duplicate;
    const auto protected_spelling = protected_spelling_index.find(*normalized);
    if (protected_spelling != protected_spelling_index.end())
      protected_path_duplicate = protected_spelling->second;
    const auto protected_canonical = protected_canonical_index.find(*canonical);
    if (protected_canonical != protected_canonical_index.end() &&
        (!protected_path_duplicate.has_value() ||
         protected_canonical->second < *protected_path_duplicate)) {
      protected_path_duplicate = protected_canonical->second;
    }
    const auto protected_object =
      protected_object_index.find(native_object_key(observed->identity));
    const bool protected_duplicate_is_path =
      protected_path_duplicate.has_value() &&
      (protected_object == protected_object_index.end() ||
       *protected_path_duplicate <= protected_object->second);
    std::optional<std::size_t> protected_duplicate = protected_path_duplicate;
    if (protected_object != protected_object_index.end() &&
        (!protected_duplicate.has_value() || protected_object->second < *protected_duplicate)) {
      protected_duplicate = protected_object->second;
    }
    if (protected_duplicate.has_value()) {
      if (auto changed = validate_protected_duplicate(
            *protected_duplicate, protected_duplicate_is_path
                                    ? "an already protected path changed identity or content"
                                    : "an already protected hard link changed content")) {
        return std::move(*changed);
      }
      continue;
    }

    const auto validate_pending_duplicate =
      [&](std::size_t index) -> std::optional<HostedArtifactTransactionResult> {
      if (same_snapshot(observed->identity, pending_identities[index]) &&
          same_file_digest(*observed->digest, pending_digests[index])) {
        return std::nullopt;
      }
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "deduplicate-additional-input", *normalized,
                            "one additional input changed while it was protected");
    };
    std::optional<std::size_t> pending_duplicate;
    const auto pending_spelling = pending_spelling_index.find(*normalized);
    if (pending_spelling != pending_spelling_index.end())
      pending_duplicate = pending_spelling->second;
    const auto pending_canonical = pending_canonical_index.find(*canonical);
    if (pending_canonical != pending_canonical_index.end() &&
        (!pending_duplicate.has_value() || pending_canonical->second < *pending_duplicate)) {
      pending_duplicate = pending_canonical->second;
    }
    const auto pending_object = pending_object_index.find(native_object_key(observed->identity));
    if (pending_object != pending_object_index.end() &&
        (!pending_duplicate.has_value() || pending_object->second < *pending_duplicate)) {
      pending_duplicate = pending_object->second;
    }
    if (pending_duplicate.has_value()) {
      if (auto changed = validate_pending_duplicate(*pending_duplicate))
        return std::move(*changed);
      continue;
    }

    for (const PublicationEntry &entry : impl_->entries) {
      const auto destination_canonical = canonical_for_comparison(entry.destination, detail);
      if (!destination_canonical)
        return failure_result(HostedArtifactTransactionErrorCode::Io, "resolve-output",
                              entry.destination, detail);
      const InspectedPath destination = inspect_path(entry.destination);
      if (!destination.detail.empty())
        return failure_result(HostedArtifactTransactionErrorCode::Io, "inspect-output",
                              entry.destination, destination.detail);
      if (spelling_equal(*normalized, entry.destination) ||
          spelling_equal(*canonical, *destination_canonical) ||
          (destination.exists && same_object(observed->identity, destination.identity))) {
        return failure_result(HostedArtifactTransactionErrorCode::PathConflict,
                              "protect-additional-input", *normalized,
                              "additional input conflicts with a public output");
      }
    }
    for (const LockedPath &lock : impl_->locks) {
      const auto lock_canonical = canonical_for_comparison(lock.path, detail);
      if (!lock_canonical)
        return failure_result(HostedArtifactTransactionErrorCode::Io, "resolve-lock", lock.path,
                              detail);
      if (spelling_equal(*normalized, lock.path) || spelling_equal(*canonical, *lock_canonical) ||
          same_object(observed->identity, lock.identity)) {
        return failure_result(HostedArtifactTransactionErrorCode::PathConflict,
                              "protect-additional-input", *normalized,
                              "additional input conflicts with a transaction lock");
      }
    }
    const std::size_t pending_index = pending_paths.size();
    pending_paths.push_back(*normalized);
    pending_identities.push_back(observed->identity);
    pending_digests.push_back(*observed->digest);
    pending_spelling_index.emplace(*normalized, pending_index);
    pending_canonical_index.emplace(*canonical, pending_index);
    pending_object_index.emplace(native_object_key(observed->identity), pending_index);
  }
  impl_->protected_paths.insert(impl_->protected_paths.end(), pending_paths.begin(),
                                pending_paths.end());
  impl_->protected_identities.insert(impl_->protected_identities.end(), pending_identities.begin(),
                                     pending_identities.end());
  impl_->protected_digests.insert(impl_->protected_digests.end(), pending_digests.begin(),
                                  pending_digests.end());
  return success_result();
}

HostedArtifactTransactionResult HostedArtifactTransaction::revalidate_protected_inputs() const {
  if (!impl_ || (impl_->state != HostedArtifactTransactionState::Open &&
                 impl_->state != HostedArtifactTransactionState::Sealed)) {
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState,
                          "revalidate-protected-inputs", {},
                          "protected inputs may only be revalidated before publication");
  }

  for (std::size_t index = 0U; index < impl_->protected_paths.size(); ++index) {
    std::string detail;
    const auto observed =
      observe_regular_file(impl_->protected_paths[index], true, false, false, false, detail);
    if (!observed.has_value() || !observed->digest.has_value() ||
        !same_snapshot(observed->identity, impl_->protected_identities[index]) ||
        !same_file_digest(*observed->digest, impl_->protected_digests[index])) {
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "revalidate-protected-input", impl_->protected_paths[index],
                            detail.empty() ? "protected input identity or content changed"
                                           : detail);
    }
  }
  for (std::size_t index = 0U; index < impl_->protected_directory_paths.size(); ++index) {
    const DirectoryTreeDigestResult membership =
      sha256_directory_tree(impl_->protected_directory_paths[index]);
    if (!membership.ok() ||
        !same_directory_digest(*membership.value, impl_->protected_directory_digests[index])) {
      HostedArtifactTransactionErrorCode code =
        HostedArtifactTransactionErrorCode::ConcurrentModification;
      if (!membership.ok()) {
        switch (membership.error) {
        case FileDigestErrorCode::Symlink:
        case FileDigestErrorCode::NotRegularFile:
          code = HostedArtifactTransactionErrorCode::UnsafePath;
          break;
        case FileDigestErrorCode::TooLarge:
          code = HostedArtifactTransactionErrorCode::InvalidPlan;
          break;
        case FileDigestErrorCode::Io:
          code = HostedArtifactTransactionErrorCode::Io;
          break;
        case FileDigestErrorCode::Missing:
        case FileDigestErrorCode::Unstable:
          break;
        case FileDigestErrorCode::None:
          code = HostedArtifactTransactionErrorCode::Io;
          break;
        }
      }
      return failure_result(
        code, "revalidate-protected-directory", impl_->protected_directory_paths[index],
        membership.ok() ? "protected directory membership changed"
                        : "protected directory could not be enumerated: " + membership.detail);
    }
  }
  return success_result();
}

HostedArtifactTransactionResult
HostedArtifactTransaction::adopt_existing_staged_outputs_for_cleanup() {
  if (!impl_ || impl_->state != HostedArtifactTransactionState::Open)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "adopt-staged-outputs",
                          {}, "staged outputs may only be adopted while the transaction is open");

  for (std::size_t index = 0U; index < impl_->entries.size(); ++index) {
    if (index == impl_->metadata_index)
      continue;
    PublicationEntry &entry = impl_->entries[index];
    const auto directory =
      std::find_if(impl_->staging_directories.begin(), impl_->staging_directories.end(),
                   [&](const StagingDirectory &candidate) {
                     return spelling_equal(candidate.path, entry.staged.parent_path());
                   });
    if (directory == impl_->staging_directories.end()) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "adopt-staged-output",
                            entry.staged,
                            "staged output is not mapped to a transaction-owned directory");
    }
    const InspectedPath current_directory = inspect_path(directory->path);
    if (!current_directory.detail.empty() || !current_directory.exists ||
        !same_object(current_directory.identity, directory->identity)) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "adopt-staged-output", entry.staged,
                            current_directory.detail.empty()
                              ? "transaction staging directory identity changed"
                              : current_directory.detail);
    }
    const InspectedPath current = inspect_path(entry.staged);
    if (!current.detail.empty()) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::StagedOutputInvalid,
                            "adopt-staged-output", entry.staged, current.detail);
    }
    if (!current.exists)
      continue;
    if (entry.staged_adopted) {
      // Adoption records cleanup ownership, not immutable build content.
      // Some filesystems update metadata when the compiler merely opens a
      // generated source. Preserve fail-closed ownership by requiring the
      // same native object, then refresh its cleanup snapshot. seal() performs
      // the strict content/flush observation required for publication.
      if (!same_object(current.identity, entry.staged_identity)) {
        impl_->state = HostedArtifactTransactionState::Failed;
        return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                              "adopt-staged-output", entry.staged,
                              "an adopted staged output path changed native identity");
      }
      entry.staged_identity = current.identity;
      continue;
    }
    std::string detail;
    const auto adopted = adopt_staged_file(entry.staged, false, detail);
    if (!adopted) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::StagedOutputInvalid,
                            "adopt-staged-output", entry.staged, detail);
    }
    entry.staged_identity = adopted->identity;
    entry.staged_adopted = true;
  }
  return success_result();
}

HostedArtifactTransactionBeginResult
begin_hosted_artifact_transaction(const HostedArtifactTransactionPlan &plan) {
  HostedArtifactTransactionBeginResult result;
  auto implementation = std::make_unique<HostedArtifactTransaction::Impl>();

  std::vector<fs::path> requested = {plan.artifact, plan.generated_cpp};
  if (plan.generated_header)
    requested.push_back(*plan.generated_header);
  if (plan.import_library)
    requested.push_back(*plan.import_library);
  requested.push_back(artifact_metadata_path(plan.artifact));

  std::vector<fs::path> outputs;
  std::vector<fs::path> canonical_outputs;
  std::vector<InspectedPath> initial_output_states;
  outputs.reserve(requested.size());
  canonical_outputs.reserve(requested.size());
  initial_output_states.reserve(requested.size());
  for (const fs::path &path : requested) {
    std::string detail;
    const auto normalized = absolute_normalized(path, detail);
    if (!normalized) {
      result.error = make_error(HostedArtifactTransactionErrorCode::InvalidPlan, "normalize-output",
                                path, detail);
      return result;
    }
    if (has_reserved_lock_suffix(*normalized)) {
      result.error = make_error(
        HostedArtifactTransactionErrorCode::PathConflict, "validate-output-protocol", *normalized,
        "public output names ending in .nebula.lock are reserved for transaction locks");
      return result;
    }
    const auto canonical = canonical_for_comparison(*normalized, detail);
    if (!canonical) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath, "resolve-output",
                                *normalized, detail);
      return result;
    }
    const InspectedPath existing = inspect_path(*normalized);
    if (!existing.detail.empty() ||
        (existing.exists && (!existing.identity.valid || existing.identity.directory))) {
      result.error =
        make_error(HostedArtifactTransactionErrorCode::UnsafePath, "inspect-output", *normalized,
                   existing.detail.empty() ? "output is not a regular file" : existing.detail);
      return result;
    }
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
      if (spelling_equal(*normalized, outputs[index]) ||
          spelling_equal(*canonical, canonical_outputs[index]) ||
          (existing.exists && inspect_path(outputs[index]).exists &&
           same_object(existing.identity, inspect_path(outputs[index]).identity))) {
        result.error =
          make_error(HostedArtifactTransactionErrorCode::PathConflict, "validate-output-set",
                     *normalized, "public transaction outputs are not distinct");
        return result;
      }
    }
    const InspectedPath parent = inspect_path(normalized->parent_path());
    if (!parent.detail.empty() || !parent.exists || !parent.identity.directory) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath,
                                "inspect-output-parent", normalized->parent_path(),
                                parent.detail.empty() ? "output parent is not an existing directory"
                                                      : parent.detail);
      return result;
    }
#if !defined(_WIN32)
    if (!validate_owner_controlled_directory_chain(normalized->parent_path(), detail)) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath,
                                "validate-output-parent-trust", normalized->parent_path(), detail);
      return result;
    }
#endif
    outputs.push_back(*normalized);
    canonical_outputs.push_back(*canonical);
    initial_output_states.push_back(existing);
  }

  std::vector<fs::path> protected_inputs;
  std::vector<fs::path> canonical_protected_inputs;
  std::vector<NativeIdentity> protected_identities;
  std::vector<FileDigest> protected_digests;
  for (const HostedArtifactProtectedInput &protected_input : plan.protected_inputs) {
    const fs::path &path = protected_input.path;
    std::string detail;
    const auto normalized = absolute_normalized(path, detail);
    if (!normalized) {
      result.error = make_error(HostedArtifactTransactionErrorCode::InvalidPlan, "normalize-input",
                                path, detail);
      return result;
    }
    const auto observed = observe_regular_file(*normalized, true, false, false, false, detail);
    if (!observed.has_value() || !observed->digest.has_value()) {
      result.error = make_error(
        HostedArtifactTransactionErrorCode::UnsafePath, "inspect-protected-input", *normalized,
        detail.empty() ? "protected input is not a stable regular file" : detail);
      return result;
    }
    const auto canonical = canonical_for_comparison(*normalized, detail);
    if (!canonical) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath, "resolve-input",
                                *normalized, detail);
      return result;
    }
    if (protected_input.expected_digest.has_value()) {
      if (!same_file_digest(*observed->digest, *protected_input.expected_digest)) {
        result.error =
          make_error(HostedArtifactTransactionErrorCode::ConcurrentModification,
                     "verify-protected-input-digest", *normalized,
                     "protected input content no longer matches its provenance digest");
        return result;
      }
    }
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
      const InspectedPath output = inspect_path(outputs[index]);
      if (spelling_equal(*normalized, outputs[index]) ||
          spelling_equal(*canonical, canonical_outputs[index]) ||
          (output.exists && same_object(observed->identity, output.identity))) {
        result.error = make_error(
          HostedArtifactTransactionErrorCode::PathConflict, "protect-input", outputs[index],
          "output conflicts with a protected input by path or native identity");
        return result;
      }
    }
    protected_inputs.push_back(*normalized);
    canonical_protected_inputs.push_back(*canonical);
    protected_identities.push_back(observed->identity);
    protected_digests.push_back(*observed->digest);
  }

  std::vector<fs::path> protected_directories;
  std::vector<fs::path> canonical_protected_directories;
  std::vector<DirectoryTreeDigest> protected_directory_digests;
  for (const HostedArtifactProtectedDirectory &protected_directory : plan.protected_directories) {
    std::string detail;
    const auto normalized = absolute_normalized(protected_directory.path, detail);
    if (!normalized) {
      result.error = make_error(HostedArtifactTransactionErrorCode::InvalidPlan,
                                "normalize-protected-directory", protected_directory.path, detail);
      return result;
    }
    const InspectedPath inspected = inspect_path(*normalized);
    if (!inspected.detail.empty() || !inspected.exists || !inspected.identity.valid ||
        !inspected.identity.directory) {
      result.error = make_error(
        HostedArtifactTransactionErrorCode::UnsafePath, "inspect-protected-directory", *normalized,
        inspected.detail.empty() ? "protected directory is not a plain directory"
                                 : inspected.detail);
      return result;
    }
    const auto canonical = canonical_for_comparison(*normalized, detail);
    if (!canonical) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath,
                                "resolve-protected-directory", *normalized, detail);
      return result;
    }
    const DirectoryTreeDigestResult membership = sha256_directory_tree(*normalized);
    if (!membership.ok() ||
        !same_directory_digest(*membership.value, protected_directory.expected_membership)) {
      result.error = make_error(
        HostedArtifactTransactionErrorCode::ConcurrentModification, "verify-protected-directory",
        *normalized,
        membership.ok() ? "protected directory membership no longer matches build provenance"
                        : "protected directory could not be enumerated: " + membership.detail);
      return result;
    }
    for (const fs::path &canonical_output : canonical_outputs) {
      if (path_is_within_or_equal(canonical_output, *canonical)) {
        result.error =
          make_error(HostedArtifactTransactionErrorCode::PathConflict, "protect-directory",
                     canonical_output, "public output is inside a protected build-input directory");
        return result;
      }
    }
    bool duplicate = false;
    for (std::size_t index = 0U; index < canonical_protected_directories.size(); ++index) {
      if (!spelling_equal(*canonical, canonical_protected_directories[index]))
        continue;
      if (!same_directory_digest(protected_directory.expected_membership,
                                 protected_directory_digests[index])) {
        result.error = make_error(HostedArtifactTransactionErrorCode::InvalidPlan,
                                  "deduplicate-protected-directory", *normalized,
                                  "one protected directory has inconsistent expected memberships");
        return result;
      }
      duplicate = true;
      break;
    }
    if (duplicate)
      continue;
    protected_directories.push_back(*normalized);
    canonical_protected_directories.push_back(*canonical);
    protected_directory_digests.push_back(protected_directory.expected_membership);
  }

  std::vector<fs::path> lock_paths;
  std::vector<fs::path> canonical_lock_paths;
  lock_paths.reserve(outputs.size());
  for (const fs::path &output : outputs) {
    fs::path lock_path = output;
    lock_path += ".nebula.lock";
    std::string detail;
    const auto canonical_lock = canonical_for_comparison(lock_path, detail);
    if (!canonical_lock) {
      result.error = make_error(HostedArtifactTransactionErrorCode::UnsafePath, "resolve-lock",
                                lock_path, detail);
      return result;
    }
    const InspectedPath existing_lock = inspect_path(lock_path);
    if (!existing_lock.detail.empty() ||
        (existing_lock.exists &&
         (!existing_lock.identity.valid || existing_lock.identity.directory))) {
      result.error =
        make_error(HostedArtifactTransactionErrorCode::UnsafePath, "inspect-lock", lock_path,
                   existing_lock.detail.empty() ? "transaction lock is not a regular file"
                                                : existing_lock.detail);
      return result;
    }
    for (std::size_t index = 0U; index < protected_inputs.size(); ++index) {
      if (spelling_equal(lock_path, protected_inputs[index]) ||
          spelling_equal(*canonical_lock, canonical_protected_inputs[index]) ||
          (existing_lock.exists &&
           same_object(existing_lock.identity, protected_identities[index]))) {
        result.error = make_error(HostedArtifactTransactionErrorCode::PathConflict, "protect-lock",
                                  lock_path, "transaction lock conflicts with a protected input");
        return result;
      }
    }
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
      const InspectedPath output = inspect_path(outputs[index]);
      if (spelling_equal(lock_path, outputs[index]) ||
          spelling_equal(*canonical_lock, canonical_outputs[index]) ||
          (existing_lock.exists && output.exists &&
           same_object(existing_lock.identity, output.identity))) {
        result.error =
          make_error(HostedArtifactTransactionErrorCode::PathConflict, "validate-lock-output",
                     lock_path, "transaction lock conflicts with a public output");
        return result;
      }
    }
    for (std::size_t index = 0U; index < lock_paths.size(); ++index) {
      if (spelling_equal(lock_path, lock_paths[index]) ||
          spelling_equal(*canonical_lock, canonical_lock_paths[index]) ||
          (existing_lock.exists && inspect_path(lock_paths[index]).exists &&
           same_object(existing_lock.identity, inspect_path(lock_paths[index]).identity))) {
        result.error =
          make_error(HostedArtifactTransactionErrorCode::PathConflict, "validate-lock-set",
                     lock_path, "transaction lock paths are not distinct");
        return result;
      }
    }
    lock_paths.push_back(std::move(lock_path));
    canonical_lock_paths.push_back(*canonical_lock);
  }
  std::sort(lock_paths.begin(), lock_paths.end(), [](const fs::path &left, const fs::path &right) {
    return left.generic_string() < right.generic_string();
  });
  for (const fs::path &lock_path : lock_paths) {
    LockedPath lock;
    HostedArtifactTransactionErrorCode code = HostedArtifactTransactionErrorCode::Io;
    std::string detail;
    if (!acquire_lock(lock_path, lock, code, detail)) {
      const std::string cleanup =
        cleanup_begin_resources(implementation->staging_directories, implementation->locks);
      if (!cleanup.empty()) {
        result.error = make_error(HostedArtifactTransactionErrorCode::CleanupIncomplete,
                                  "cleanup-after-lock-failure", lock_path,
                                  detail + "; cleanup incomplete: " + cleanup);
        return result;
      }
      result.error = make_error(code, "acquire-lock", lock_path, detail);
      return result;
    }
    std::string lock_validation_detail;
    const bool lock_identity_unsafe = !revalidate_lock(lock, lock_validation_detail);
    bool conflict = lock_identity_unsafe;
    for (const NativeIdentity &identity : protected_identities)
      conflict = conflict || same_object(lock.identity, identity);
    for (const fs::path &output_path : outputs) {
      const InspectedPath output = inspect_path(output_path);
      conflict = conflict || (output.exists && same_object(lock.identity, output.identity));
    }
    for (const LockedPath &other_lock : implementation->locks)
      conflict = conflict || same_object(lock.identity, other_lock.identity);
    if (conflict) {
      implementation->locks.push_back(std::move(lock));
      const std::string cleanup =
        cleanup_begin_resources(implementation->staging_directories, implementation->locks);
      result.error =
        make_error(cleanup.empty() ? HostedArtifactTransactionErrorCode::PathConflict
                                   : HostedArtifactTransactionErrorCode::CleanupIncomplete,
                   "revalidate-lock", lock_path,
                   cleanup.empty() ? (lock_identity_unsafe
                                        ? lock_validation_detail
                                        : "lock identity conflicts with another transaction path")
                                   : "lock identity became unsafe; cleanup incomplete: " + cleanup);
      return result;
    }
    implementation->locks.push_back(std::move(lock));
  }

  // Each publication entry gets a distinct private directory. The compiler
  // still sees the public basename (important for library/import-library
  // naming), while staged and backup names can never alias another entry.
  implementation->staging_directories.reserve(outputs.size());
  for (const fs::path &output : outputs) {
    const fs::path parent = output.parent_path();
    std::string detail;
    bool internal_cleanup_incomplete = false;
    auto directory = create_staging_directory(parent, detail, internal_cleanup_incomplete);
    if (!directory) {
      const std::string cleanup =
        cleanup_begin_resources(implementation->staging_directories, implementation->locks);
      if (!cleanup.empty()) {
        result.error = make_error(HostedArtifactTransactionErrorCode::CleanupIncomplete,
                                  "cleanup-after-staging-failure", parent,
                                  detail + "; cleanup incomplete: " + cleanup);
        return result;
      }
      result.error = make_error(internal_cleanup_incomplete
                                  ? HostedArtifactTransactionErrorCode::CleanupIncomplete
                                  : HostedArtifactTransactionErrorCode::Io,
                                "create-staging-directory", parent, detail);
      return result;
    }
    implementation->staging_directories.push_back(std::move(*directory));
  }

  for (std::size_t index = 0U; index < outputs.size(); ++index) {
    if (index >= implementation->staging_directories.size()) {
      const std::string cleanup =
        cleanup_begin_resources(implementation->staging_directories, implementation->locks);
      result.error = make_error(
        cleanup.empty() ? HostedArtifactTransactionErrorCode::Io
                        : HostedArtifactTransactionErrorCode::CleanupIncomplete,
        "map-staging-directory", outputs[index],
        cleanup.empty() ? "internal staging directory mapping failed"
                        : "internal staging mapping failed; cleanup incomplete: " + cleanup);
      return result;
    }
    const StagingDirectory &directory = implementation->staging_directories[index];
    PublicationEntry entry;
    entry.destination = outputs[index];
    entry.initial_existed = initial_output_states[index].exists;
    if (entry.initial_existed)
      entry.initial_identity = initial_output_states[index].identity;
    entry.staged = directory.path / outputs[index].filename();
    entry.backup = directory.path / (outputs[index].filename().string() + ".backup");
    entry.rollback_quarantine = directory.path / (outputs[index].filename().string() + ".rollback");
    implementation->entries.push_back(std::move(entry));
  }

  implementation->artifact_index = 0U;
  implementation->metadata_index = outputs.size() - 1U;
  implementation->protected_paths = std::move(protected_inputs);
  implementation->protected_identities = std::move(protected_identities);
  implementation->protected_digests = std::move(protected_digests);
  implementation->protected_directory_paths = std::move(protected_directories);
  implementation->protected_directory_digests = std::move(protected_directory_digests);
  implementation->public_staging.artifact = implementation->entries[0].staged;
  implementation->public_staging.generated_cpp = implementation->entries[1].staged;
  std::size_t optional_index = 2U;
  if (plan.generated_header)
    implementation->public_staging.generated_header =
      implementation->entries[optional_index++].staged;
  if (plan.import_library)
    implementation->public_staging.import_library =
      implementation->entries[optional_index++].staged;
  implementation->public_staging.metadata = implementation->entries.back().staged;

  result.transaction = std::unique_ptr<HostedArtifactTransaction>(
    new HostedArtifactTransaction(std::move(implementation)));
  return result;
}

HostedArtifactTransactionResult HostedArtifactTransaction::seal(const ArtifactBuildKey &build_key) {
  if (!impl_ || impl_->state != HostedArtifactTransactionState::Open)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "seal", {},
                          "transaction is not open");

  std::optional<FileDigest> artifact_digest;
  for (std::size_t index = 0U; index < impl_->entries.size(); ++index) {
    if (index == impl_->metadata_index)
      continue;
    std::string detail;
    auto adopted = adopt_staged_file(impl_->entries[index].staged, true, detail);
    if (!adopted || !adopted->digest.has_value()) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::StagedOutputInvalid,
                            "seal-staged-output", impl_->entries[index].staged,
                            detail.empty() ? "staged output digest was not produced" : detail);
    }
    impl_->entries[index].staged_identity = adopted->identity;
    impl_->entries[index].staged_digest = *adopted->digest;
    impl_->entries[index].staged_adopted = true;
    if (index == impl_->artifact_index)
      artifact_digest = *adopted->digest;
  }
  if (!artifact_digest) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::StagedOutputInvalid, "hash-artifact",
                          impl_->entries[impl_->artifact_index].staged,
                          "artifact digest was not produced");
  }
  ArtifactMetadata metadata;
  metadata.build = build_key;
  metadata.content.size = artifact_digest->size;
  metadata.content.sha256 = artifact_digest->sha256;
  impl_->sealed_artifact_digest = artifact_digest;
  ArtifactMetadataSerializationResult serialized = serialize_artifact_metadata(metadata);
  if (!serialized.ok()) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::Metadata, "serialize-metadata",
                          impl_->entries[impl_->metadata_index].staged, serialized.error.detail);
  }
  std::string detail;
  auto identity = write_staged_metadata(impl_->entries[impl_->metadata_index].staged,
                                        *serialized.payload, detail);
  if (!identity) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::Metadata, "write-metadata",
                          impl_->entries[impl_->metadata_index].staged, detail);
  }
  impl_->entries[impl_->metadata_index].staged_identity = *identity;
  const auto metadata_bytes = std::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t *>(serialized.payload->data()), serialized.payload->size());
  impl_->entries[impl_->metadata_index].staged_digest =
    FileDigest{serialized.payload->size(), sha256_hex(metadata_bytes)};
  impl_->entries[impl_->metadata_index].staged_adopted = true;
  for (const StagingDirectory &directory : impl_->staging_directories) {
    if (!flush_directory(directory.path, detail)) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::DurabilityUnavailable,
                            "flush-staging-directory", directory.path, detail);
    }
  }

  // Directory durability can make a writer-close timestamp transition visible
  // after the preliminary file snapshot (notably for freshly created APFS
  // files). Establish the immutable publication boundary only after every
  // staging directory is durable. Rehashing every output prevents a ctime
  // refresh from accepting same-size content drift as an internal transition.
  for (PublicationEntry &entry : impl_->entries) {
    std::string detail;
    auto stable = observe_regular_file(entry.staged, true, true, false, true, detail);
    if (!stable || !stable->digest.has_value() || !entry.staged_digest.has_value()) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(
        HostedArtifactTransactionErrorCode::StagedOutputInvalid, "stabilize-staged-output",
        entry.staged, detail.empty() ? "stable staged output digest was not produced" : detail);
    }
    if (!same_object(stable->identity, entry.staged_identity) ||
        !same_file_digest(*stable->digest, *entry.staged_digest)) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "stabilize-staged-output", entry.staged,
                            "staged output identity or content changed before the durable seal");
    }
    entry.staged_identity = stable->identity;
    entry.staged_security = std::move(stable->security);
  }
  std::string metadata_policy_detail;
  if (!validate_sealed_metadata_policy(impl_->entries[impl_->metadata_index],
                                       metadata_policy_detail)) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::StagedOutputInvalid,
                          "seal-metadata-policy", impl_->entries[impl_->metadata_index].staged,
                          std::move(metadata_policy_detail));
  }
#if defined(__APPLE__)
  PublicationEntry &metadata_entry = impl_->entries[impl_->metadata_index];
  metadata_entry.darwin_metadata_stabilization_available =
    metadata_entry.staged_security.extended_attributes_supported &&
    metadata_entry.staged_security.access_control_lists_supported;
#endif
  impl_->state = HostedArtifactTransactionState::Sealed;
  return success_result();
}

HostedArtifactTransactionResult HostedArtifactTransaction::commit() {
  if (!impl_ || impl_->state != HostedArtifactTransactionState::Sealed)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "commit", {},
                          "transaction is not sealed");

  fs::path invalid_lock_path;
  std::string invalid_lock_detail;
  if (!revalidate_locks(impl_->locks, invalid_lock_path, invalid_lock_detail)) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "revalidate-lock-before-commit", invalid_lock_path, invalid_lock_detail);
  }

  std::vector<NativeIdentity> current_protected;
  current_protected.reserve(impl_->protected_paths.size());
  for (std::size_t index = 0U; index < impl_->protected_paths.size(); ++index) {
    std::string detail;
    const auto input =
      observe_regular_file(impl_->protected_paths[index], true, false, false, false, detail);
    if (!input.has_value() || !input->digest.has_value() ||
        !same_content_snapshot(input->identity, impl_->protected_identities[index]) ||
        !same_file_digest(*input->digest, impl_->protected_digests[index])) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "revalidate-protected-input", impl_->protected_paths[index],
                            detail.empty() ? "protected input identity or content changed"
                                           : detail);
    }
    current_protected.push_back(input->identity);
  }
  for (std::size_t index = 0U; index < impl_->protected_directory_paths.size(); ++index) {
    const DirectoryTreeDigestResult membership =
      sha256_directory_tree(impl_->protected_directory_paths[index]);
    if (!membership.ok() ||
        !same_directory_digest(*membership.value, impl_->protected_directory_digests[index])) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(
        HostedArtifactTransactionErrorCode::ConcurrentModification,
        "revalidate-protected-directory", impl_->protected_directory_paths[index],
        membership.ok() ? "protected directory membership changed before publication"
                        : "protected directory could not be enumerated: " + membership.detail);
    }
  }

  for (std::size_t entry_index = 0U; entry_index < impl_->entries.size(); ++entry_index) {
    PublicationEntry &entry = impl_->entries[entry_index];
    std::string staged_detail;
    auto staged = observe_regular_file(entry.staged, true, true, false, true, staged_detail);
    if (!staged.has_value() ||
        !reconcile_sealed_publication_entry(entry, std::move(*staged), false,
                                            entry_index == impl_->metadata_index, staged_detail)) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(
        HostedArtifactTransactionErrorCode::ConcurrentModification, "preflight-staged-output",
        entry.staged, staged_detail.empty() ? "staged output changed after seal" : staged_detail);
    }
    const InspectedPath destination = inspect_path(entry.destination);
    if (!destination.detail.empty() ||
        (destination.exists && (!destination.identity.valid || destination.identity.directory))) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(
        HostedArtifactTransactionErrorCode::UnsafePath, "preflight-destination", entry.destination,
        destination.detail.empty() ? "destination is not a regular file" : destination.detail);
    }
    if (destination.exists) {
      for (std::size_t index = 0U; index < current_protected.size(); ++index) {
        if (same_object(destination.identity, current_protected[index]) ||
            same_object(destination.identity, impl_->protected_identities[index])) {
          impl_->state = HostedArtifactTransactionState::Failed;
          return failure_result(HostedArtifactTransactionErrorCode::PathConflict,
                                "revalidate-protected-output", entry.destination,
                                "destination became a hard link to a protected input");
        }
      }
    }
    if (destination.exists != entry.initial_existed ||
        (destination.exists && !same_snapshot(destination.identity, entry.initial_identity))) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "revalidate-destination", entry.destination,
                            entry.initial_existed
                              ? "destination disappeared or changed after the transaction began"
                              : "destination appeared after the transaction began");
    }
    entry.prior_existed = entry.initial_existed;
    if (destination.exists)
      entry.prior_identity = destination.identity;
  }

  for (std::size_t index = 0U; index < current_protected.size(); ++index) {
    if (!same_snapshot(current_protected[index], impl_->protected_identities[index])) {
      impl_->state = HostedArtifactTransactionState::Failed;
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "revalidate-protected-input", impl_->protected_paths[index],
                            "protected input identity or content changed during the transaction");
    }
  }

  if (!impl_->sealed_artifact_digest) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "reverify-artifact",
                          impl_->entries[impl_->artifact_index].staged,
                          "sealed artifact digest is missing");
  }
  std::string digest_detail;
  auto reverification =
    adopt_staged_file(impl_->entries[impl_->artifact_index].staged, true, digest_detail);
  if (!reverification || !reverification->digest ||
      reverification->digest->size != impl_->sealed_artifact_digest->size ||
      reverification->digest->sha256 != impl_->sealed_artifact_digest->sha256) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "reverify-artifact", impl_->entries[impl_->artifact_index].staged,
                          digest_detail.empty() ? "artifact content changed after seal"
                                                : digest_detail);
  }

  std::string failure_detail;
  fs::path failure_path;
  HostedArtifactTransactionErrorCode failure_code = HostedArtifactTransactionErrorCode::Publication;
  for (std::size_t entry_index = 0U; entry_index < impl_->entries.size(); ++entry_index) {
    PublicationEntry &entry = impl_->entries[entry_index];
    if (entry.prior_existed) {
      if (!rename_to_backup(entry.destination, entry.backup, failure_detail)) {
        failure_path = entry.destination;
        break;
      }
      entry.backed_up = true;
      const InspectedPath backup = inspect_path(entry.backup);
      if (!backup.detail.empty() || !backup.exists || !backup.identity.valid) {
        failure_detail = backup.detail.empty()
                           ? "backup identity could not be captured after replacement"
                           : backup.detail;
        failure_path = entry.backup;
        break;
      }
      entry.backup_identity = backup.identity;
      if (!same_content_snapshot(backup.identity, entry.prior_identity)) {
        failure_detail = "destination changed between publication preflight and backup";
        failure_path = entry.backup;
        failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
        break;
      }
      for (std::size_t index = 0U; index < current_protected.size(); ++index) {
        if (same_object(backup.identity, current_protected[index]) ||
            same_object(backup.identity, impl_->protected_identities[index])) {
          failure_detail = "destination became a hard link to a protected input during publication";
          failure_path = entry.backup;
          failure_code = HostedArtifactTransactionErrorCode::PathConflict;
          break;
        }
      }
      if (!failure_detail.empty())
        break;
      if (consume_test_fault(InternalFaultPoint::AfterBackup)) {
        failure_detail = "injected failure after backing up the destination";
        failure_path = entry.backup;
        break;
      }
    }
    PublicationProgress publication = publish_no_replace(entry.staged, entry.destination);
    entry.published = publication.linked;
    if (!publication.detail.empty()) {
      failure_detail = std::move(publication.detail);
      failure_path = entry.destination;
      break;
    }
    std::string published_detail;
    auto published =
      observe_regular_file(entry.destination, true, true, false, true, published_detail);
    if (!published.has_value() || !reconcile_sealed_publication_entry(
                                    entry, std::move(*published), true,
                                    entry_index == impl_->metadata_index, published_detail)) {
      failure_detail = published_detail.empty()
                         ? "published path does not retain the sealed content and security identity"
                         : published_detail;
      failure_path = entry.destination;
      failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
      break;
    }
  }

  const auto revalidate_protected_after_publication = [&](std::string_view timing) {
    for (std::size_t index = 0U; index < impl_->protected_paths.size(); ++index) {
      std::string detail;
      const auto input =
        observe_regular_file(impl_->protected_paths[index], true, false, false, false, detail);
      if (!input.has_value() || !input->digest.has_value() ||
          !same_snapshot(input->identity, impl_->protected_identities[index]) ||
          !same_file_digest(*input->digest, impl_->protected_digests[index])) {
        failure_detail = detail.empty()
                           ? "protected input identity or content changed " + std::string(timing)
                           : detail;
        failure_path = impl_->protected_paths[index];
        failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
        return false;
      }
    }
    for (std::size_t index = 0U; index < impl_->protected_directory_paths.size(); ++index) {
      const DirectoryTreeDigestResult membership =
        sha256_directory_tree(impl_->protected_directory_paths[index]);
      if (!membership.ok() ||
          !same_directory_digest(*membership.value, impl_->protected_directory_digests[index])) {
        failure_detail = membership.ok()
                           ? "protected directory membership changed " + std::string(timing)
                           : "protected directory could not be enumerated: " + membership.detail;
        failure_path = impl_->protected_directory_paths[index];
        failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
        return false;
      }
    }
    return true;
  };

  if (failure_detail.empty()) {
    const FileDigestResult published_digest =
      sha256_file(impl_->entries[impl_->artifact_index].destination);
    if (!published_digest.ok() ||
        published_digest.value->size != impl_->sealed_artifact_digest->size ||
        published_digest.value->sha256 != impl_->sealed_artifact_digest->sha256) {
      failure_detail = published_digest.ok()
                         ? "published artifact content no longer matches its sealed metadata digest"
                         : "published artifact could not be re-hashed: " + published_digest.detail;
      failure_path = impl_->entries[impl_->artifact_index].destination;
      failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
    }
  }

  if (failure_detail.empty())
    (void)revalidate_protected_after_publication("while outputs were published");

  if (failure_detail.empty() && !revalidate_locks(impl_->locks, failure_path, failure_detail)) {
    failure_code = HostedArtifactTransactionErrorCode::ConcurrentModification;
  }

  if (!failure_detail.empty()) {
    const std::string rollback_detail = rollback_publication(impl_->entries);
    impl_->state = HostedArtifactTransactionState::Failed;
    if (!rollback_detail.empty()) {
      return failure_result(HostedArtifactTransactionErrorCode::RollbackIncomplete,
                            "rollback-publication", failure_path,
                            failure_detail + "; rollback incomplete: " + rollback_detail);
    }
    return failure_result(failure_code, "publish-transaction", failure_path, failure_detail);
  }

  std::set<fs::path> flushed_parents;
  for (const PublicationEntry &entry : impl_->entries)
    flushed_parents.insert(entry.destination.parent_path());
  for (const fs::path &parent : flushed_parents) {
    std::string detail;
    const bool injected_flush_failure =
      consume_test_fault(InternalFaultPoint::BeforePublicationDirectoryFlush);
    if (injected_flush_failure || !flush_directory(parent, detail)) {
      if (injected_flush_failure)
        detail = "injected publication directory flush failure";
      const std::string rollback_detail = rollback_publication(impl_->entries);
      impl_->state = HostedArtifactTransactionState::Failed;
      if (!rollback_detail.empty())
        return failure_result(HostedArtifactTransactionErrorCode::RollbackIncomplete,
                              "rollback-after-publication-flush", parent,
                              detail + "; rollback incomplete: " + rollback_detail);
      return failure_result(HostedArtifactTransactionErrorCode::DurabilityUnavailable,
                            "flush-publication-directory", parent,
                            detail + "; publication was rolled back");
    }
  }
  for (std::size_t entry_index = 0U; entry_index < impl_->entries.size(); ++entry_index) {
    PublicationEntry &entry = impl_->entries[entry_index];
    std::string detail;
    auto published = observe_regular_file(entry.destination, true, true, false, true, detail);
    if (!published.has_value() ||
        !reconcile_sealed_publication_entry(entry, std::move(*published), false,
                                            entry_index == impl_->metadata_index, detail)) {
      const std::string rollback_detail = rollback_publication(impl_->entries);
      const std::string verification_detail =
        detail.empty() ? "published output changed after directory flush" : detail;
      impl_->state = HostedArtifactTransactionState::Failed;
      if (!rollback_detail.empty()) {
        return failure_result(HostedArtifactTransactionErrorCode::RollbackIncomplete,
                              "rollback-after-final-publication-verification", entry.destination,
                              verification_detail + "; rollback incomplete: " + rollback_detail);
      }
      return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                            "revalidate-published-output-after-flush", entry.destination,
                            verification_detail + "; publication was rolled back");
    }
  }
#if defined(NEBULA_HOSTED_ARTIFACT_TRANSACTION_TESTING)
  hosted_artifact_transaction_testing::run_before_final_protected_input_revalidation_hook_once();
#endif
  if (!revalidate_protected_after_publication("after publication directory flush")) {
    const std::string rollback_detail = rollback_publication(impl_->entries);
    impl_->state = HostedArtifactTransactionState::Failed;
    if (!rollback_detail.empty()) {
      return failure_result(HostedArtifactTransactionErrorCode::RollbackIncomplete,
                            "rollback-after-final-protected-input-verification", failure_path,
                            failure_detail + "; rollback incomplete: " + rollback_detail);
    }
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "revalidate-protected-input-after-publication-flush", failure_path,
                          failure_detail + "; publication was rolled back");
  }
  if (!revalidate_locks(impl_->locks, failure_path, failure_detail)) {
    const std::string rollback_detail = rollback_publication(impl_->entries);
    impl_->state = HostedArtifactTransactionState::Failed;
    if (!rollback_detail.empty()) {
      return failure_result(HostedArtifactTransactionErrorCode::RollbackIncomplete,
                            "rollback-after-lock-revalidation", failure_path,
                            failure_detail + "; rollback incomplete: " + rollback_detail);
    }
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "revalidate-lock-after-publication", failure_path,
                          failure_detail + "; publication was rolled back");
  }
  impl_->state = HostedArtifactTransactionState::Committed;
  return success_result();
}

HostedArtifactTransactionResult HostedArtifactTransaction::abort() {
  if (!impl_ || impl_->state == HostedArtifactTransactionState::Closed)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "abort", {},
                          "transaction is already closed");
  if (impl_->state == HostedArtifactTransactionState::Committed)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "abort", {},
                          "a committed transaction must be finished");

  std::string aggregate = rollback_publication(impl_->entries);
  for (PublicationEntry &entry : impl_->entries) {
    const InspectedPath current = inspect_path(entry.staged);
    if (!current.detail.empty()) {
      append_detail(aggregate, entry.staged, current.detail);
      continue;
    }
    if (!current.exists)
      continue;
    if (!entry.staged_adopted) {
      append_detail(aggregate, entry.staged,
                    "unadopted staged path is preserved; refusing to infer cleanup ownership");
      continue;
    }
    std::string detail;
    if (!remove_if_identity(entry.staged, entry.staged_identity, false, detail))
      append_detail(aggregate, entry.staged, detail);
  }
  for (auto iterator = impl_->staging_directories.rbegin();
       iterator != impl_->staging_directories.rend(); ++iterator) {
    std::string detail;
    if (!remove_staging_directory(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  if (!aggregate.empty()) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::CleanupIncomplete, "abort-cleanup",
                          {}, aggregate);
  }
  for (auto iterator = impl_->locks.rbegin(); iterator != impl_->locks.rend(); ++iterator) {
    std::string detail;
    if (!release_lock(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  if (!aggregate.empty()) {
    impl_->state = HostedArtifactTransactionState::Failed;
    return failure_result(HostedArtifactTransactionErrorCode::CleanupIncomplete,
                          "abort-lock-release", {}, aggregate);
  }
  impl_->state = HostedArtifactTransactionState::Closed;
  return success_result();
}

HostedArtifactTransactionResult HostedArtifactTransaction::finish() {
  if (!impl_ || impl_->state != HostedArtifactTransactionState::Committed)
    return failure_result(HostedArtifactTransactionErrorCode::InvalidState, "finish", {},
                          "transaction is not committed");

  fs::path invalid_lock_path;
  std::string invalid_lock_detail;
  if (!revalidate_locks(impl_->locks, invalid_lock_path, invalid_lock_detail)) {
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "revalidate-lock-before-finish", invalid_lock_path, invalid_lock_detail);
  }

  std::string aggregate;
  for (PublicationEntry &entry : impl_->entries) {
    if (!entry.backed_up)
      continue;
    std::string detail;
    if (!remove_if_identity(entry.backup, entry.backup_identity, false, detail))
      append_detail(aggregate, entry.backup, detail);
    else
      entry.backed_up = false;
  }
  for (auto iterator = impl_->staging_directories.rbegin();
       iterator != impl_->staging_directories.rend(); ++iterator) {
    std::string detail;
    if (!remove_staging_directory(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  if (!aggregate.empty())
    return failure_result(HostedArtifactTransactionErrorCode::CleanupIncomplete, "finish-cleanup",
                          {}, aggregate);
  if (!revalidate_locks(impl_->locks, invalid_lock_path, invalid_lock_detail)) {
    return failure_result(HostedArtifactTransactionErrorCode::ConcurrentModification,
                          "revalidate-lock-before-release", invalid_lock_path, invalid_lock_detail);
  }
  for (auto iterator = impl_->locks.rbegin(); iterator != impl_->locks.rend(); ++iterator) {
    std::string detail;
    if (!release_lock(*iterator, detail))
      append_detail(aggregate, iterator->path, detail);
  }
  if (!aggregate.empty())
    return failure_result(HostedArtifactTransactionErrorCode::CleanupIncomplete,
                          "finish-lock-release", {}, aggregate);
  impl_->state = HostedArtifactTransactionState::Closed;
  return success_result();
}

} // namespace nebula::cli
