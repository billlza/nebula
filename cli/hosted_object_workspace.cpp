#include "hosted_object_workspace.hpp"
#if defined(_WIN32)
#include "windows_object_identity.hpp"
#include "windows_private_security.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nebula::cli {
namespace {

#if defined(_WIN32)

enum class WindowsDirectoryHandlePhase : std::uint8_t {
  IdentityLocked,
  CleanupTransition,
  DeletionBound,
  DeletionMarked,
};

struct NativeDirectoryBinding {
  HANDLE handle = INVALID_HANDLE_VALUE;
  WindowsObjectIdentity identity;
  WindowsDirectoryHandlePhase phase = WindowsDirectoryHandlePhase::IdentityLocked;

  NativeDirectoryBinding() = default;
  NativeDirectoryBinding(const NativeDirectoryBinding &) = delete;
  NativeDirectoryBinding &operator=(const NativeDirectoryBinding &) = delete;
  NativeDirectoryBinding(NativeDirectoryBinding &&other) noexcept
      : handle(other.handle), identity(other.identity), phase(other.phase) {
    other.handle = INVALID_HANDLE_VALUE;
  }
  NativeDirectoryBinding &operator=(NativeDirectoryBinding &&) = delete;
  ~NativeDirectoryBinding() {
    if (handle != INVALID_HANDLE_VALUE)
      (void)::CloseHandle(handle);
  }
};

std::string windows_error(DWORD error) { return "Windows error " + std::to_string(error); }

bool read_windows_identity(HANDLE handle, NativeDirectoryBinding &binding, std::string &detail) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (::GetFileInformationByHandle(handle, &information) == 0) {
    detail = "could not read hosted object workspace identity: " + windows_error(::GetLastError());
    return false;
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    detail = "hosted object workspace is not a plain directory";
    return false;
  }
  const DWORD identity_error = read_windows_object_identity(handle, binding.identity);
  if (identity_error != ERROR_SUCCESS) {
    detail =
      "could not read stable hosted object workspace identity: " + windows_error(identity_error);
    return false;
  }
  return true;
}

std::optional<NativeDirectoryBinding> bind_directory_identity(const std::filesystem::path &path,
                                                              std::string &detail) {
  NativeDirectoryBinding binding;
  binding.handle = ::CreateFileW(
    path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (binding.handle == INVALID_HANDLE_VALUE) {
    detail = "could not bind hosted object workspace identity: " + windows_error(::GetLastError());
    return std::nullopt;
  }
  if (!read_windows_identity(binding.handle, binding, detail)) {
    if (::CloseHandle(binding.handle) == 0) {
      detail += "; could not close rejected hosted object workspace handle: " +
                windows_error(::GetLastError());
    } else {
      binding.handle = INVALID_HANDLE_VALUE;
    }
    return std::nullopt;
  }
  return std::optional<NativeDirectoryBinding>(std::move(binding));
}

bool same_path_identity(const std::filesystem::path &path, const NativeDirectoryBinding &expected,
                        std::string &detail) {
  NativeDirectoryBinding current;
  current.handle = ::CreateFileW(
    path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (current.handle == INVALID_HANDLE_VALUE) {
    detail =
      "could not bind hosted object workspace path identity: " + windows_error(::GetLastError());
    return false;
  }
  if (!read_windows_identity(current.handle, current, detail)) {
    if (::CloseHandle(current.handle) == 0) {
      detail += "; could not close rejected hosted object workspace verification handle: " +
                windows_error(::GetLastError());
    } else {
      current.handle = INVALID_HANDLE_VALUE;
    }
    return false;
  }
  const bool same = current.identity == expected.identity;
  if (::CloseHandle(current.handle) == 0) {
    detail = "could not close hosted object workspace verification handle: " +
             windows_error(::GetLastError());
    return false;
  }
  current.handle = INVALID_HANDLE_VALUE;
  if (!same)
    detail = "hosted object workspace path was replaced before cleanup";
  return same;
}

bool mark_bound_directory_for_deletion(const NativeDirectoryBinding &binding, std::string &detail) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (::SetFileInformationByHandle(binding.handle, FileDispositionInfo, &disposition,
                                   sizeof(disposition)) != 0) {
    return true;
  }
  detail = "could not mark hosted object workspace for handle-bound deletion: " +
           windows_error(::GetLastError());
  return false;
}

bool close_binding(NativeDirectoryBinding &binding, std::string &detail) {
  if (binding.handle == INVALID_HANDLE_VALUE)
    return true;
  if (::CloseHandle(binding.handle) != 0) {
    binding.handle = INVALID_HANDLE_VALUE;
    return true;
  }
  detail =
    "could not close hosted object workspace identity handle: " + windows_error(::GetLastError());
  return false;
}

bool adopt_closed_binding(NativeDirectoryBinding &destination, NativeDirectoryBinding &source,
                          std::string &detail) {
  if (destination.handle != INVALID_HANDLE_VALUE || source.handle == INVALID_HANDLE_VALUE) {
    detail = "hosted object workspace handle handoff has an invalid state";
    return false;
  }
  destination.handle = source.handle;
  destination.identity = source.identity;
  destination.phase = source.phase;
  source.handle = INVALID_HANDLE_VALUE;
  return true;
}

bool finish_empty_bound_directory_deletion(NativeDirectoryBinding &binding, std::string &detail) {
  if (binding.phase == WindowsDirectoryHandlePhase::IdentityLocked) {
    NativeDirectoryBinding transition;
    transition.phase = WindowsDirectoryHandlePhase::CleanupTransition;
    transition.handle = ::ReOpenFile(binding.handle, FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (transition.handle == INVALID_HANDLE_VALUE) {
      detail = "could not reopen hosted object workspace cleanup transition: " +
               windows_error(::GetLastError());
      return false;
    }
    if (!read_windows_identity(transition.handle, transition, detail) ||
        transition.identity != binding.identity) {
      if (detail.empty())
        detail = "hosted object workspace cleanup transition has the wrong identity";
      std::string close_detail;
      if (!close_binding(transition, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!close_binding(binding, detail)) {
      std::string close_detail;
      if (!close_binding(transition, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!adopt_closed_binding(binding, transition, detail))
      return false;
  }

  if (binding.phase == WindowsDirectoryHandlePhase::CleanupTransition) {
    NativeDirectoryBinding deletion;
    deletion.phase = WindowsDirectoryHandlePhase::DeletionBound;
    deletion.handle = ::ReOpenFile(binding.handle, FILE_READ_ATTRIBUTES | DELETE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (deletion.handle == INVALID_HANDLE_VALUE) {
      detail = "could not reopen hosted object workspace identity for deletion: " +
               windows_error(::GetLastError());
      return false;
    }
    if (!read_windows_identity(deletion.handle, deletion, detail) ||
        deletion.identity != binding.identity) {
      if (detail.empty())
        detail = "hosted object workspace deletion handle has the wrong identity";
      std::string close_detail;
      if (!close_binding(deletion, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!close_binding(binding, detail)) {
      std::string close_detail;
      if (!close_binding(deletion, close_detail))
        detail += "; " + close_detail;
      return false;
    }
    if (!adopt_closed_binding(binding, deletion, detail))
      return false;
  }

  if (binding.phase == WindowsDirectoryHandlePhase::DeletionBound) {
    if (!mark_bound_directory_for_deletion(binding, detail))
      return false;
    binding.phase = WindowsDirectoryHandlePhase::DeletionMarked;
  }
  if (binding.phase == WindowsDirectoryHandlePhase::DeletionMarked &&
      !close_binding(binding, detail)) {
    return false;
  }
  if (binding.handle == INVALID_HANDLE_VALUE)
    return true;
  if (detail.empty())
    detail = "hosted object workspace deletion state did not reach a closed handle";
  return false;
}

std::uint64_t process_identifier() { return static_cast<std::uint64_t>(::GetCurrentProcessId()); }

#else

struct NativeDirectoryBinding {
  int descriptor = -1;
  int parent_descriptor = -1;
  std::string filename;
  dev_t device = 0;
  ino_t inode = 0;
};

std::optional<NativeDirectoryBinding> bind_directory_identity_at(int parent_descriptor,
                                                                 const std::string &filename,
                                                                 std::string &detail) {
  NativeDirectoryBinding binding;
  binding.descriptor =
    ::openat(parent_descriptor, filename.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (binding.descriptor < 0) {
    detail =
      "could not bind hosted object workspace identity: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  struct stat status{};
  if (::fstat(binding.descriptor, &status) != 0) {
    const int inspect_error = errno;
    detail = "could not read hosted object workspace identity: " +
             std::string(std::strerror(inspect_error));
    if (::close(binding.descriptor) != 0) {
      detail += "; could not close rejected hosted object workspace descriptor: " +
                std::string(std::strerror(errno));
    }
    return std::nullopt;
  }
  if (!S_ISDIR(status.st_mode)) {
    detail = "hosted object workspace is not a directory";
    if (::close(binding.descriptor) != 0) {
      detail += "; could not close rejected hosted object workspace descriptor: " +
                std::string(std::strerror(errno));
    }
    return std::nullopt;
  }
  binding.device = status.st_dev;
  binding.inode = status.st_ino;
  binding.filename = filename;
  return binding;
}

enum class BoundDirectoryEntryState : std::uint8_t {
  Same,
  Absent,
  Replaced,
  Error,
};

BoundDirectoryEntryState inspect_bound_directory_entry(const NativeDirectoryBinding &expected,
                                                       std::string &detail) {
  detail.clear();
  if (expected.parent_descriptor < 0 || expected.filename.empty()) {
    detail = "hosted object workspace parent identity is unavailable";
    return BoundDirectoryEntryState::Error;
  }
  struct stat status{};
  if (::fstatat(expected.parent_descriptor, expected.filename.c_str(), &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT)
      return BoundDirectoryEntryState::Absent;
    detail = "could not revalidate hosted object workspace parent entry: " +
             std::string(std::strerror(errno));
    return BoundDirectoryEntryState::Error;
  }
  if (!S_ISDIR(status.st_mode) || status.st_dev != expected.device ||
      status.st_ino != expected.inode) {
    detail = "hosted object workspace path was replaced before cleanup";
    return BoundDirectoryEntryState::Replaced;
  }
  return BoundDirectoryEntryState::Same;
}

bool remove_bound_directory_contents(int directory, std::string &detail) {
  const int iteration_descriptor =
    ::openat(directory, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (iteration_descriptor < 0) {
    detail = "could not open a fresh hosted object workspace enumeration: " +
             std::string(std::strerror(errno));
    return false;
  }
  DIR *stream = ::fdopendir(iteration_descriptor);
  if (stream == nullptr) {
    const int error = errno;
    detail = "could not enumerate hosted object workspace: " + std::string(std::strerror(error));
    if (::close(iteration_descriptor) != 0) {
      detail +=
        "; could not close rejected enumeration descriptor: " + std::string(std::strerror(errno));
    }
    return false;
  }

  bool ok = true;
  errno = 0;
  while (dirent *entry = ::readdir(stream)) {
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..")
      continue;
    struct stat status{};
    if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
      detail =
        "could not inspect hosted object workspace entry: " + std::string(std::strerror(errno));
      ok = false;
      break;
    }
    if (S_ISDIR(status.st_mode)) {
      const int child =
        ::openat(directory, entry->d_name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
      if (child < 0) {
        detail = "could not bind hosted object workspace child directory: " +
                 std::string(std::strerror(errno));
        ok = false;
        break;
      }
      struct stat opened_child{};
      if (::fstat(child, &opened_child) != 0) {
        const int inspect_error = errno;
        detail = "could not inspect the opened hosted object workspace child: " +
                 std::string(std::strerror(inspect_error));
        if (::close(child) != 0) {
          detail +=
            "; could not close rejected child directory: " + std::string(std::strerror(errno));
        }
        ok = false;
        break;
      }
      if (!S_ISDIR(opened_child.st_mode) || opened_child.st_dev != status.st_dev ||
          opened_child.st_ino != status.st_ino) {
        detail = "hosted object workspace child identity changed before recursion";
        if (::close(child) != 0) {
          detail +=
            "; could not close rejected child directory: " + std::string(std::strerror(errno));
        }
        ok = false;
        break;
      }
      std::string child_detail;
      const bool child_removed = remove_bound_directory_contents(child, child_detail);
      const int close_result = ::close(child);
      if (!child_removed || close_result != 0) {
        detail = child_removed ? "could not close hosted object workspace child directory: " +
                                   std::string(std::strerror(errno))
                               : std::move(child_detail);
        ok = false;
        break;
      }
      struct stat current{};
      if (::fstatat(directory, entry->d_name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISDIR(current.st_mode) || current.st_dev != status.st_dev ||
          current.st_ino != status.st_ino) {
        detail = "hosted object workspace child identity changed during cleanup";
        ok = false;
        break;
      }
      if (::unlinkat(directory, entry->d_name, AT_REMOVEDIR) != 0) {
        detail = "could not remove hosted object workspace child directory: " +
                 std::string(std::strerror(errno));
        ok = false;
        break;
      }
    } else if (S_ISREG(status.st_mode) || S_ISLNK(status.st_mode)) {
      if (::unlinkat(directory, entry->d_name, 0) != 0) {
        detail =
          "could not remove hosted object workspace file: " + std::string(std::strerror(errno));
        ok = false;
        break;
      }
    } else {
      detail = "hosted object workspace contains an unsupported special file";
      ok = false;
      break;
    }
    errno = 0;
  }
  if (ok && errno != 0) {
    detail =
      "could not finish enumerating hosted object workspace: " + std::string(std::strerror(errno));
    ok = false;
  }
  if (::closedir(stream) != 0) {
    if (!detail.empty())
      detail += "; ";
    detail +=
      "could not close hosted object workspace enumeration: " + std::string(std::strerror(errno));
    ok = false;
  }
  return ok;
}

bool close_binding(NativeDirectoryBinding &binding, std::string &detail) {
  bool ok = true;
  if (binding.descriptor >= 0) {
    const int descriptor = binding.descriptor;
    binding.descriptor = -1;
    if (::close(descriptor) != 0) {
      detail = "could not close hosted object workspace identity descriptor: " +
               std::string(std::strerror(errno));
      ok = false;
    }
  }
  if (binding.parent_descriptor >= 0) {
    const int parent_descriptor = binding.parent_descriptor;
    binding.parent_descriptor = -1;
    if (::close(parent_descriptor) != 0) {
      if (!detail.empty())
        detail += "; ";
      detail += "could not close hosted object workspace parent descriptor: " +
                std::string(std::strerror(errno));
      ok = false;
    }
  }
  return ok;
}

std::uint64_t process_identifier() { return static_cast<std::uint64_t>(::getpid()); }

#endif

std::atomic<std::uint64_t> workspace_sequence{0U};

std::string unique_workspace_name(std::uint64_t sequence) {
  const auto epoch = std::chrono::steady_clock::now().time_since_epoch().count();
  return ".nebula-obj-" + std::to_string(process_identifier()) + "-" + std::to_string(epoch) + "-" +
         std::to_string(sequence);
}

#if defined(_WIN32)
bool path_is_absent(const std::filesystem::path &path, std::string &detail) {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
  if (!error)
    return !std::filesystem::exists(status);
  if (error == std::errc::no_such_file_or_directory)
    return true;
  detail = "could not verify hosted object workspace removal: " + error.message();
  return false;
}

bool remove_windows_directory_contents(const std::filesystem::path &path, std::string &detail) {
  std::error_code error;
  std::vector<std::filesystem::path> entries;
  for (std::filesystem::directory_iterator iterator(path, error), end; !error && iterator != end;
       iterator.increment(error)) {
    entries.push_back(iterator->path());
  }
  if (error) {
    detail = "could not enumerate hosted object workspace: " + error.message();
    return false;
  }
  for (const std::filesystem::path &entry : entries) {
    std::filesystem::remove_all(entry, error);
    if (error) {
      detail = "could not remove hosted object workspace entry " + entry.filename().string() +
               ": " + error.message();
      return false;
    }
  }
  return true;
}
#endif

} // namespace

struct HostedObjectWorkspace::Impl {
  std::filesystem::path path;
  NativeDirectoryBinding binding;
  bool root_removed = false;
  bool cleaned = false;
};

HostedObjectWorkspace::HostedObjectWorkspace(std::unique_ptr<Impl> implementation)
    : impl_(std::move(implementation)) {}

HostedObjectWorkspace::HostedObjectWorkspace(HostedObjectWorkspace &&) noexcept = default;

HostedObjectWorkspace::~HostedObjectWorkspace() {
  if (!impl_)
    return;
  if (!impl_->cleaned) {
    const HostedObjectWorkspaceCleanupResult cleaned = cleanup();
    if (!cleaned.ok()) {
      std::cerr << "nebula: fatal: hosted object workspace cleanup failed";
      if (!impl_->path.empty())
        std::cerr << " (" << impl_->path.string() << ")";
      if (!cleaned.detail.empty())
        std::cerr << ": " << cleaned.detail;
      std::cerr << '\n';
    }
  }
}

const std::filesystem::path &HostedObjectWorkspace::path() const noexcept { return impl_->path; }

HostedObjectWorkspaceCleanupResult HostedObjectWorkspace::cleanup() {
  HostedObjectWorkspaceCleanupResult result;
  if (!impl_) {
    result.detail = "hosted object workspace is moved-from";
    return result;
  }
  if (impl_->cleaned)
    return result;

#if defined(_WIN32)
  if (!impl_->root_removed) {
    if (impl_->binding.phase == WindowsDirectoryHandlePhase::IdentityLocked) {
      if (!same_path_identity(impl_->path, impl_->binding, result.detail))
        return result;
      if (!remove_windows_directory_contents(impl_->path, result.detail))
        return result;
    }
    if (!finish_empty_bound_directory_deletion(impl_->binding, result.detail))
      return result;
    impl_->root_removed = true;
  }
  if (!path_is_absent(impl_->path, result.detail)) {
    if (result.detail.empty())
      result.detail = "hosted object workspace path now names a replacement object";
    return result;
  }
#else
  if (!impl_->root_removed) {
    const BoundDirectoryEntryState initial_entry =
      inspect_bound_directory_entry(impl_->binding, result.detail);
    if (initial_entry == BoundDirectoryEntryState::Replaced ||
        initial_entry == BoundDirectoryEntryState::Error) {
      return result;
    }
    if (!remove_bound_directory_contents(impl_->binding.descriptor, result.detail)) {
      return result;
    }
    const BoundDirectoryEntryState final_entry =
      inspect_bound_directory_entry(impl_->binding, result.detail);
    if (final_entry == BoundDirectoryEntryState::Replaced ||
        final_entry == BoundDirectoryEntryState::Error) {
      return result;
    }
    if (final_entry == BoundDirectoryEntryState::Same &&
        ::unlinkat(impl_->binding.parent_descriptor, impl_->binding.filename.c_str(),
                   AT_REMOVEDIR) != 0) {
      result.detail =
        "could not remove hosted object workspace root: " + std::string(std::strerror(errno));
      return result;
    }
    impl_->root_removed = true;
  }
#endif
  if (!close_binding(impl_->binding, result.detail))
    return result;
  impl_->cleaned = true;
  return result;
}

HostedObjectWorkspaceCreationResult
create_hosted_object_workspace(const std::filesystem::path &parent) {
  HostedObjectWorkspaceCreationResult result;
  if (parent.empty()) {
    result.detail = "hosted object workspace requires a nonempty parent directory";
    return result;
  }
#if defined(_WIN32)
  std::string volume_detail;
  if (!validate_windows_persistent_acl_support(parent, volume_detail)) {
    result.detail = "cannot create a private hosted object workspace: " + volume_detail;
    return result;
  }
#else
  int parent_descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
  if (parent_descriptor < 0) {
    result.detail =
      "could not bind hosted object workspace parent: " + std::string(std::strerror(errno));
    return result;
  }
  const auto close_parent_after_failure = [&](std::string detail) {
    const int close_result = ::close(parent_descriptor);
    parent_descriptor = -1;
    if (close_result != 0) {
      if (!detail.empty())
        detail += "; ";
      detail +=
        "could not close hosted object workspace parent: " + std::string(std::strerror(errno));
    }
    return detail;
  };
#endif
  for (std::uint32_t attempt = 0U; attempt < 128U; ++attempt) {
    const std::uint64_t sequence = workspace_sequence.fetch_add(1U, std::memory_order_relaxed);
    const std::filesystem::path candidate = parent / unique_workspace_name(sequence);
#if defined(_WIN32)
    WindowsPrivateSecurityDescriptor security;
    std::string security_detail;
    if (!prepare_windows_private_security(WindowsPrivateObjectKind::Directory, security,
                                          security_detail)) {
      result.detail = "could not prepare private hosted object workspace ACL: " + security_detail;
      return result;
    }
    const BOOL created = ::CreateDirectoryW(candidate.c_str(), security.attributes());
    const DWORD creation_error = created == 0 ? ::GetLastError() : ERROR_SUCCESS;
    if (!security.release(security_detail)) {
      result.detail = "could not release private hosted object workspace ACL: " + security_detail;
      if (created == 0) {
        result.detail =
          "hosted object workspace creation also failed: " + windows_error(creation_error) + "; " +
          result.detail;
      }
      if (created != 0)
        result.detail += "; the unbound workspace was preserved to avoid path-based cleanup";
      return result;
    }
    if (created == 0) {
      if (creation_error == ERROR_ALREADY_EXISTS || creation_error == ERROR_FILE_EXISTS) {
        continue;
      }
      result.detail =
        "could not create private hosted object workspace: " + windows_error(creation_error);
      return result;
    }
#else
    const std::string filename = candidate.filename().string();
    if (::mkdirat(parent_descriptor, filename.c_str(), 0700) != 0) {
      if (errno == EEXIST)
        continue;
      result.detail =
        "could not create private hosted object workspace: " + std::string(std::strerror(errno));
      result.detail = close_parent_after_failure(std::move(result.detail));
      return result;
    }
#endif

    std::string identity_detail;
#if defined(_WIN32)
    std::optional<NativeDirectoryBinding> binding =
      bind_directory_identity(candidate, identity_detail);
#else
    std::optional<NativeDirectoryBinding> binding =
      bind_directory_identity_at(parent_descriptor, filename, identity_detail);
#endif
    if (!binding.has_value()) {
#if defined(_WIN32)
      result.detail = std::move(identity_detail);
      result.detail += "; the unbound workspace was preserved to avoid path-based cleanup";
#else
      const int cleanup_error =
        ::unlinkat(parent_descriptor, filename.c_str(), AT_REMOVEDIR) == 0 ? 0 : errno;
      result.detail = std::move(identity_detail);
      if (cleanup_error != 0) {
        result.detail += "; could not roll back unbound hosted object workspace: " +
                         std::string(std::strerror(cleanup_error));
      }
      result.detail = close_parent_after_failure(std::move(result.detail));
#endif
      return result;
    }
#if defined(_WIN32)
    if (!validate_windows_private_object_security(
          binding->handle, WindowsPrivateObjectKind::Directory, identity_detail)) {
      result.detail =
        "hosted object workspace did not retain its private owner/DACL: " + identity_detail;
      std::string close_detail;
      if (!close_binding(*binding, close_detail))
        result.detail += "; " + close_detail;
      return result;
    }
    if (!same_path_identity(candidate, *binding, identity_detail)) {
      result.detail = std::move(identity_detail);
      std::string deletion_detail;
      if (!finish_empty_bound_directory_deletion(*binding, deletion_detail)) {
        result.detail += "; " + deletion_detail;
      }
      return result;
    }
#else
    binding->parent_descriptor = parent_descriptor;
    parent_descriptor = -1;
    const BoundDirectoryEntryState entry = inspect_bound_directory_entry(*binding, identity_detail);
    if (entry != BoundDirectoryEntryState::Same) {
      if (entry == BoundDirectoryEntryState::Absent && identity_detail.empty()) {
        identity_detail = "hosted object workspace disappeared while its identity was bound";
      }
      std::string close_detail;
      (void)close_binding(*binding, close_detail);
      result.detail = std::move(identity_detail);
      if (!close_detail.empty()) {
        result.detail += "; " + close_detail;
      }
      return result;
    }
#endif

    auto implementation = std::make_unique<HostedObjectWorkspace::Impl>();
    implementation->path = candidate;
#if defined(_WIN32)
    if (!adopt_closed_binding(implementation->binding, *binding, result.detail)) {
      return result;
    }
#else
    implementation->binding = std::move(*binding);
#endif
    HostedObjectWorkspace workspace(std::move(implementation));
    result.workspace.emplace(std::move(workspace));
    return result;
  }
  result.detail = "could not allocate a unique hosted object workspace after 128 attempts";
#if !defined(_WIN32)
  result.detail = close_parent_after_failure(std::move(result.detail));
#endif
  return result;
}

} // namespace nebula::cli
