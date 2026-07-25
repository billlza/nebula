#include "path_security.hpp"

#if !defined(_WIN32)

#include <cerrno>
#include <cstring>
#if defined(__APPLE__)
#include <grp.h>
#include <optional>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace nebula::cli {

#if defined(__APPLE__)
namespace {

bool is_macos_administrator_group(gid_t group_id) {
  static const std::optional<gid_t> administrator_group = []() -> std::optional<gid_t> {
    const struct group *entry = ::getgrnam("admin");
    if (entry == nullptr)
      return std::nullopt;
    return entry->gr_gid;
  }();
  return administrator_group.has_value() && group_id == *administrator_group;
}

} // namespace
#endif

namespace {

bool validate_owner_controlled_directory_state(const std::filesystem::path &path,
                                               const struct stat &state, std::string &detail) {
  const uid_t effective_user = ::geteuid();
  if (!S_ISDIR(state.st_mode)) {
    detail = "trusted directory is a non-directory or symbolic link: " + path.string();
    return false;
  }
  if (state.st_uid != 0 && state.st_uid != effective_user) {
    detail = "trusted directory is owned by another user: " + path.string();
    return false;
  }
  const bool group_write = (state.st_mode & S_IWGRP) != 0;
  const bool world_write = (state.st_mode & S_IWOTH) != 0;
  if (world_write && (state.st_mode & S_ISVTX) == 0) {
    detail = "trusted directory is non-sticky and world-writable: " + path.string();
    return false;
  }
  if (group_write && !world_write && (state.st_mode & S_ISVTX) == 0) {
#if defined(__APPLE__)
    if (!is_macos_administrator_group(state.st_gid)) {
      detail = "trusted directory is non-sticky and group-writable outside the macOS "
               "administrator trust boundary: " +
               path.string();
      return false;
    }
#else
    detail = "trusted directory is non-sticky and group-writable: " + path.string();
    return false;
#endif
  }
  return true;
}

} // namespace

bool validate_owner_controlled_directory(const std::filesystem::path &path, std::string &detail) {
  detail.clear();
  if (path.empty() || !path.is_absolute()) {
    detail = "trusted directory must be an absolute path";
    return false;
  }
  struct stat state{};
  if (::lstat(path.c_str(), &state) != 0) {
    detail = "failed to inspect trusted directory: " + std::string(std::strerror(errno));
    return false;
  }
  return validate_owner_controlled_directory_state(path, state, detail);
}

bool validate_owner_controlled_directory_chain(const std::filesystem::path &path,
                                               std::string &detail) {
  detail.clear();
  if (path.empty() || !path.is_absolute()) {
    detail = "trusted directory chain must be an absolute path";
    return false;
  }
  std::error_code canonical_error;
  const std::filesystem::path canonical_path = std::filesystem::canonical(path, canonical_error);
  if (canonical_error || canonical_path.empty() || !canonical_path.is_absolute()) {
    detail = canonical_error
               ? "failed to canonicalize directory trust chain: " + canonical_error.message()
               : "directory trust chain did not canonicalize to an absolute path";
    return false;
  }
  std::filesystem::path current;
  for (const std::filesystem::path &component : canonical_path) {
    current /= component;
    struct stat state{};
    if (::lstat(current.c_str(), &state) != 0) {
      detail = "failed to inspect directory trust chain: " + std::string(std::strerror(errno));
      return false;
    }
    if (!validate_owner_controlled_directory_state(current, state, detail))
      return false;
  }
  return true;
}

bool validate_owner_controlled_executable(const std::filesystem::path &path, std::string &detail) {
  detail.clear();
  if (path.empty() || !path.is_absolute() || path.filename().empty()) {
    detail = "trusted executable must be an absolute file path";
    return false;
  }
  std::error_code canonical_error;
  const std::filesystem::path canonical = std::filesystem::canonical(path, canonical_error);
  if (canonical_error || canonical != path) {
    detail = canonical_error
               ? "failed to canonicalize trusted executable: " + canonical_error.message()
               : "trusted executable path must already be canonical";
    return false;
  }
  if (!validate_owner_controlled_directory_chain(canonical.parent_path(), detail)) {
    return false;
  }
  struct stat state{};
  if (::lstat(canonical.c_str(), &state) != 0) {
    detail = "failed to inspect trusted executable: " + std::string(std::strerror(errno));
    return false;
  }
  const uid_t effective_user = ::geteuid();
  if (!S_ISREG(state.st_mode) || (state.st_uid != 0 && state.st_uid != effective_user)) {
    detail = "trusted executable must be a regular file owned by root or the effective user";
    return false;
  }
  if ((state.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    detail = "trusted executable must not be group/world writable";
    return false;
  }
  if (::faccessat(AT_FDCWD, canonical.c_str(), X_OK, AT_EACCESS) != 0) {
    detail =
      "effective user cannot execute trusted executable: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
}

} // namespace nebula::cli

#endif
