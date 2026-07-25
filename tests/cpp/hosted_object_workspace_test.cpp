#include "cli/hosted_object_workspace.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

void expect(bool condition, std::string_view message, int &failures) {
  if (condition)
    return;
  std::cerr << "hosted_object_workspace_test: " << message << '\n';
  ++failures;
}

#if defined(_WIN32)

std::string windows_error(DWORD error) {
  return std::system_category().message(static_cast<int>(error));
}

void append_detail(std::string &detail, std::string_view addition) {
  if (!detail.empty())
    detail += "; ";
  detail += addition;
}

std::optional<PSID> read_process_user_sid(std::vector<std::max_align_t> &token_buffer,
                                          std::string &detail) {
  HANDLE token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
    detail = "could not open process token for workspace ACL verification: " +
             windows_error(::GetLastError());
    return std::nullopt;
  }
  const auto close_token = [&](bool success) {
    if (::CloseHandle(token) != 0)
      return success;
    append_detail(detail,
                  "could not close ACL verification token: " + windows_error(::GetLastError()));
    return false;
  };

  DWORD required = 0U;
  const BOOL sized = ::GetTokenInformation(token, TokenUser, nullptr, 0U, &required);
  const DWORD sizing_error = ::GetLastError();
  if (sized != 0 || sizing_error != ERROR_INSUFFICIENT_BUFFER) {
    detail =
      "could not size process user for workspace ACL verification: " + windows_error(sizing_error);
    if (!close_token(true))
      return std::nullopt;
    return std::nullopt;
  }
  if (required < sizeof(TOKEN_USER)) {
    detail = "workspace ACL verification token is shorter than TOKEN_USER";
    if (!close_token(true))
      return std::nullopt;
    return std::nullopt;
  }
  const std::size_t token_byte_count = static_cast<std::size_t>(required);
  const std::size_t token_word_count =
    token_byte_count / sizeof(std::max_align_t) +
    (token_byte_count % sizeof(std::max_align_t) == 0U ? 0U : 1U);
  token_buffer.resize(token_word_count);
  DWORD returned = required;
  if (::GetTokenInformation(token, TokenUser, token_buffer.data(), required, &returned) == 0) {
    detail = "could not read process user for workspace ACL verification: " +
             windows_error(::GetLastError());
    if (!close_token(true))
      return std::nullopt;
    return std::nullopt;
  }
  if (returned < sizeof(TOKEN_USER)) {
    detail = "returned workspace ACL verification token is shorter than TOKEN_USER";
    if (!close_token(true))
      return std::nullopt;
    return std::nullopt;
  }
  const auto *token_user = reinterpret_cast<const TOKEN_USER *>(token_buffer.data());
  if (token_user->User.Sid == nullptr || ::IsValidSid(token_user->User.Sid) == 0) {
    detail = "process token exposes an invalid user SID";
    if (!close_token(true))
      return std::nullopt;
    return std::nullopt;
  }
  PSID user_sid = token_user->User.Sid;
  if (!close_token(true))
    return std::nullopt;
  return user_sid;
}

enum class WorkspaceAclExpectation : std::uint8_t {
  ExplicitInheritableDirectory,
  InheritedDirectory,
  InheritedFile,
};

bool has_private_workspace_acl(const std::filesystem::path &path,
                               WorkspaceAclExpectation expectation, std::string &detail) {
  detail.clear();
  std::vector<std::max_align_t> token_buffer;
  const std::optional<PSID> user_sid = read_process_user_sid(token_buffer, detail);
  if (!user_sid.has_value())
    return false;

  alignas(std::max_align_t) std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid_storage{};
  DWORD system_sid_bytes = static_cast<DWORD>(system_sid_storage.size());
  if (::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid_storage.data(),
                           &system_sid_bytes) == 0) {
    detail = "could not create LocalSystem SID for workspace ACL verification: " +
             windows_error(::GetLastError());
    return false;
  }
  alignas(std::max_align_t) std::array<std::byte, SECURITY_MAX_SID_SIZE>
    administrators_sid_storage{};
  DWORD administrators_sid_bytes = static_cast<DWORD>(administrators_sid_storage.size());
  if (::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators_sid_storage.data(),
                           &administrators_sid_bytes) == 0) {
    detail = "could not create administrators SID for workspace ACL verification: " +
             windows_error(::GetLastError());
    return false;
  }

  std::wstring mutable_path = path.native();
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD query =
    ::GetNamedSecurityInfoW(mutable_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                            nullptr, &dacl, nullptr, &descriptor);
  if (query != ERROR_SUCCESS) {
    detail = "could not read hosted object workspace ACL: " + windows_error(query);
    return false;
  }
  const auto release_descriptor = [&](bool success) {
    if (::LocalFree(descriptor) == nullptr)
      return success;
    append_detail(detail, "could not release hosted object workspace ACL descriptor");
    return false;
  };

  SECURITY_DESCRIPTOR_CONTROL control = 0U;
  DWORD revision = 0U;
  if (::GetSecurityDescriptorControl(descriptor, &control, &revision) == 0) {
    detail =
      "could not inspect hosted object workspace ACL control: " + windows_error(::GetLastError());
    return release_descriptor(false);
  }
  const bool expected_inherited =
    expectation != WorkspaceAclExpectation::ExplicitInheritableDirectory;
  const bool dacl_protected = (control & SE_DACL_PROTECTED) != 0U;
  const bool protection_matches = expected_inherited ? !dacl_protected : dacl_protected;
  if (dacl == nullptr || !protection_matches) {
    detail = expected_inherited
               ? "hosted object child DACL is absent or unexpectedly protected"
               : "hosted object workspace DACL is absent or inherits from its parent";
    return release_descriptor(false);
  }

  ACL_SIZE_INFORMATION acl_information{};
  if (::GetAclInformation(dacl, &acl_information, sizeof(acl_information), AclSizeInformation) ==
      0) {
    detail =
      "could not inspect hosted object workspace ACE count: " + windows_error(::GetLastError());
    return release_descriptor(false);
  }
  if (acl_information.AceCount != 3U) {
    detail = "hosted object workspace DACL does not contain exactly three private ACEs";
    return release_descriptor(false);
  }

  const std::array<PSID, 3U> expected_sids{*user_sid, system_sid_storage.data(),
                                           administrators_sid_storage.data()};
  std::array<bool, 3U> matched_expected_sids{};
  for (DWORD index = 0U; index < acl_information.AceCount; ++index) {
    void *raw_ace = nullptr;
    if (::GetAce(dacl, index, &raw_ace) == 0 || raw_ace == nullptr) {
      detail = "could not read hosted object workspace ACE: " + windows_error(::GetLastError());
      return release_descriptor(false);
    }
    const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
    const bool inherited = (header->AceFlags & INHERITED_ACE) != 0U;
    const bool inheritable = (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) ==
                             (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
    const bool must_remain_inheritable = expectation != WorkspaceAclExpectation::InheritedFile;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || inherited != expected_inherited ||
        (must_remain_inheritable && !inheritable)) {
      detail = "hosted object ACL has incorrect allow/inheritance flags";
      return release_descriptor(false);
    }
    const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(raw_ace);
    PSID sid = const_cast<void *>(static_cast<const void *>(&allowed->SidStart));
    if (::IsValidSid(sid) == 0 || allowed->Mask != FILE_ALL_ACCESS) {
      detail = "hosted object workspace ACE has an invalid SID or access mask";
      return release_descriptor(false);
    }
    bool matched = false;
    for (std::size_t expected_index = 0U; expected_index < expected_sids.size(); ++expected_index) {
      if (!matched_expected_sids[expected_index] &&
          ::EqualSid(sid, expected_sids[expected_index]) != 0) {
        matched_expected_sids[expected_index] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      detail = "hosted object workspace ACL grants access to an unexpected SID";
      return release_descriptor(false);
    }
  }
  for (bool matched : matched_expected_sids) {
    if (!matched) {
      detail = "hosted object ACL omits a required private principal";
      return release_descriptor(false);
    }
  }
  return release_descriptor(true);
}

#endif

} // namespace

int main() {
  int failures = 0;
  const std::string suffix =
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::filesystem::path root =
    std::filesystem::temp_directory_path() / ("nebula-object-workspace-" + suffix);
  std::error_code error;
  std::filesystem::create_directory(root, error);
  expect(!error, "create test root", failures);
  if (error)
    return 1;

  auto first = nebula::cli::create_hosted_object_workspace(root);
  auto second = nebula::cli::create_hosted_object_workspace(root);
  expect(first.ok(), first.detail.empty() ? "create first workspace" : first.detail, failures);
  expect(second.ok(), second.detail.empty() ? "create second workspace" : second.detail, failures);
  if (first.ok() && second.ok()) {
    const std::filesystem::path first_path = first.workspace->path();
    const std::filesystem::path second_path = second.workspace->path();
    expect(first_path != second_path, "each call must own a unique directory", failures);
    expect(first_path.parent_path() == root && second_path.parent_path() == root,
           "workspace directories must stay below the requested parent", failures);
    expect(first_path.filename().string().starts_with(".nebula-obj-") &&
             second_path.filename().string().starts_with(".nebula-obj-"),
           "workspace names must use the private object prefix", failures);

#if defined(_WIN32)
    std::string acl_detail;
    expect(has_private_workspace_acl(
             first_path, WorkspaceAclExpectation::ExplicitInheritableDirectory, acl_detail),
           acl_detail.empty() ? "workspace must have a private inheritable DACL" : acl_detail,
           failures);
    const std::filesystem::path blocked_rename(first_path.native() + L".renamed");
    const BOOL renamed = ::MoveFileExW(first_path.c_str(), blocked_rename.c_str(), 0U);
    const DWORD rename_error = renamed == 0 ? ::GetLastError() : ERROR_SUCCESS;
    expect(renamed == 0 &&
             (rename_error == ERROR_SHARING_VIOLATION || rename_error == ERROR_ACCESS_DENIED),
           "active Windows workspace must block root rename/replacement; error=" +
             std::to_string(rename_error),
           failures);
    if (renamed != 0) {
      expect(::MoveFileExW(blocked_rename.c_str(), first_path.c_str(), 0U) != 0,
             "unexpected Windows workspace rename must be restored", failures);
    }
    const BOOL removed = ::RemoveDirectoryW(first_path.c_str());
    const DWORD removal_error = removed == 0 ? ::GetLastError() : ERROR_SUCCESS;
    expect(removed == 0 &&
             (removal_error == ERROR_SHARING_VIOLATION || removal_error == ERROR_ACCESS_DENIED),
           "active Windows workspace must block root deletion; error=" +
             std::to_string(removal_error),
           failures);
#endif

    std::filesystem::create_directory(first_path / "nested", error);
    expect(!error, "create nested object directory", failures);
    std::ofstream object(first_path / "nested" / "unit.o", std::ios::binary);
    object << "object";
    object.close();
    expect(!object.fail(), "write staged object fixture", failures);

#if defined(_WIN32)
    expect(has_private_workspace_acl(first_path / "nested",
                                     WorkspaceAclExpectation::InheritedDirectory, acl_detail),
           acl_detail.empty() ? "workspace child directory must inherit the private DACL"
                              : acl_detail,
           failures);
    expect(has_private_workspace_acl(first_path / "nested" / "unit.o",
                                     WorkspaceAclExpectation::InheritedFile, acl_detail),
           acl_detail.empty() ? "workspace child file must inherit the private DACL" : acl_detail,
           failures);
#endif

    const auto first_cleanup = first.workspace->cleanup();
    expect(first_cleanup.ok(),
           first_cleanup.detail.empty() ? "clean first workspace" : first_cleanup.detail, failures);
    expect(!std::filesystem::exists(first_path),
           "successful cleanup must remove the owned object tree", failures);
    const auto first_cleanup_again = first.workspace->cleanup();
    expect(first_cleanup_again.ok(), "successful cleanup must be idempotent", failures);

    const auto second_cleanup = second.workspace->cleanup();
    expect(second_cleanup.ok(),
           second_cleanup.detail.empty() ? "clean second workspace" : second_cleanup.detail,
           failures);
    expect(!std::filesystem::exists(second_path),
           "successful cleanup must remove the second owned tree", failures);
  }

#if !defined(_WIN32)
  auto replaced = nebula::cli::create_hosted_object_workspace(root);
  expect(replaced.ok(), replaced.detail.empty() ? "create replacement workspace" : replaced.detail,
         failures);
  if (replaced.ok()) {
    const std::filesystem::path replaced_path = replaced.workspace->path();
    std::filesystem::remove(replaced_path, error);
    expect(!error, "unlink owned directory while its identity descriptor is open", failures);
    std::filesystem::create_directory(replaced_path, error);
    expect(!error, "replace workspace path with a different directory", failures);
    const auto cleanup = replaced.workspace->cleanup();
    expect(!cleanup.ok() && cleanup.detail.find("replaced") != std::string::npos,
           "cleanup must reject a replaced workspace identity", failures);
    expect(std::filesystem::exists(replaced_path),
           "cleanup must not delete the replacement directory", failures);
    std::filesystem::remove(replaced_path, error);
    expect(!error, "remove replacement fixture", failures);
    const auto retry = replaced.workspace->cleanup();
    expect(retry.ok(),
           retry.detail.empty() ? "retry cleanup after replacement disappears" : retry.detail,
           failures);
  }

  auto retryable = nebula::cli::create_hosted_object_workspace(root);
  expect(retryable.ok(), retryable.detail.empty() ? "create retryable workspace" : retryable.detail,
         failures);
  if (retryable.ok()) {
    const std::filesystem::path fifo = retryable.workspace->path() / "unexpected.fifo";
    expect(::mkfifo(fifo.c_str(), 0600) == 0, "create unsupported special-file fixture", failures);
    const auto rejected = retryable.workspace->cleanup();
    expect(!rejected.ok() && rejected.detail.find("special file") != std::string::npos,
           "cleanup must reject an unsupported special file", failures);
    std::filesystem::remove(fifo, error);
    expect(!error, "remove unsupported special-file fixture", failures);
    const auto retried = retryable.workspace->cleanup();
    expect(retried.ok(),
           retried.detail.empty() ? "retry cleanup after a recoverable failure" : retried.detail,
           failures);
  }
#endif

  const auto missing_parent =
    nebula::cli::create_hosted_object_workspace(root / "missing" / "parent");
  expect(!missing_parent.ok(), "missing parent must fail instead of creating a directory tree",
         failures);

  std::filesystem::remove_all(root, error);
  expect(!error, "remove test root", failures);
  if (failures != 0)
    return 1;
  std::cout << "hosted-object-workspace-tests-ok\n";
  return 0;
}
