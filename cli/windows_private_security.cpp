#include "windows_private_security.hpp"

#if !defined(_WIN32)
#error "windows_private_security.cpp must only be compiled for Windows targets"
#endif

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace nebula::cli {
namespace {

std::string windows_error(DWORD error) {
  return std::system_category().message(static_cast<int>(error));
}

void append_detail(std::string &detail, std::string_view addition) {
  if (!detail.empty())
    detail += "; ";
  detail += addition;
}

class UniqueHandle final {
public:
  explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;
  ~UniqueHandle() {
    if (valid())
      (void)::CloseHandle(handle_);
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] bool close(std::string &detail, std::string_view operation) {
    if (!valid())
      return true;
    if (::CloseHandle(handle_) != 0) {
      handle_ = nullptr;
      return true;
    }
    append_detail(detail, std::string(operation) + ": " + windows_error(::GetLastError()));
    return false;
  }

private:
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  HANDLE handle_ = nullptr;
};

class UniqueLocalMemory final {
public:
  explicit UniqueLocalMemory(HLOCAL memory = nullptr) : memory_(memory) {}
  UniqueLocalMemory(const UniqueLocalMemory &) = delete;
  UniqueLocalMemory &operator=(const UniqueLocalMemory &) = delete;
  ~UniqueLocalMemory() {
    if (memory_ != nullptr)
      (void)::LocalFree(memory_);
  }

  void adopt(HLOCAL memory) noexcept { memory_ = memory; }

  [[nodiscard]] bool release(std::string &detail, std::string_view operation) {
    if (memory_ == nullptr)
      return true;
    if (::LocalFree(memory_) == nullptr) {
      memory_ = nullptr;
      return true;
    }
    append_detail(detail, std::string(operation) + ": " + windows_error(::GetLastError()));
    return false;
  }

private:
  HLOCAL memory_ = nullptr;
};

bool read_process_token_record(TOKEN_INFORMATION_CLASS information_class, std::size_t minimum_size,
                               std::string_view record_name, std::vector<std::max_align_t> &buffer,
                               std::string &detail) {
  HANDLE raw_token = nullptr;
  if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) == 0) {
    detail = "failed to open the process token for private object security: " +
             windows_error(::GetLastError());
    return false;
  }
  UniqueHandle token(raw_token);

  DWORD token_bytes = 0U;
  const BOOL sized =
    ::GetTokenInformation(token.get(), information_class, nullptr, 0U, &token_bytes);
  const DWORD sizing_error = ::GetLastError();
  if (sized != 0 || sizing_error != ERROR_INSUFFICIENT_BUFFER) {
    detail = sized != 0 ? "the process token unexpectedly accepted an empty " +
                            std::string(record_name) + " buffer"
                        : "failed to size the process " + std::string(record_name) +
                            " for private object security: " + windows_error(sizing_error);
    (void)token.close(detail, "failed to close the process token");
    return false;
  }
  if (static_cast<std::size_t>(token_bytes) < minimum_size) {
    detail =
      "the process " + std::string(record_name) + " token record is shorter than its fixed header";
    (void)token.close(detail, "failed to close the process token");
    return false;
  }

  const std::size_t token_byte_count = static_cast<std::size_t>(token_bytes);
  const std::size_t token_word_count =
    token_byte_count / sizeof(std::max_align_t) +
    (token_byte_count % sizeof(std::max_align_t) == 0U ? 0U : 1U);
  buffer.resize(token_word_count);
  DWORD returned_bytes = token_bytes;
  if (::GetTokenInformation(token.get(), information_class, buffer.data(), token_bytes,
                            &returned_bytes) == 0) {
    detail = "failed to read the process " + std::string(record_name) +
             " for private object security: " + windows_error(::GetLastError());
    (void)token.close(detail, "failed to close the process token");
    return false;
  }
  if (static_cast<std::size_t>(returned_bytes) < minimum_size) {
    detail = "the returned process " + std::string(record_name) +
             " token record is shorter than its fixed header";
    (void)token.close(detail, "failed to close the process token");
    return false;
  }
  return token.close(detail, "failed to close the process token");
}

bool release_validation_memory(WindowsPrivateSecurityDescriptor &expected,
                               UniqueLocalMemory &actual, std::string &detail) {
  bool released =
    actual.release(detail, "failed to release the inspected private object security descriptor");
  released = expected.release(detail) && released;
  return released;
}

bool fail_private_security_validation(WindowsPrivateSecurityDescriptor &expected,
                                      UniqueLocalMemory &actual, std::string &detail,
                                      std::string reason) {
  detail = std::move(reason);
  (void)release_validation_memory(expected, actual, detail);
  return false;
}

} // namespace

WindowsPrivateSecurityDescriptor::~WindowsPrivateSecurityDescriptor() {
  if (descriptor_ != nullptr)
    (void)::LocalFree(descriptor_);
}

SECURITY_ATTRIBUTES *WindowsPrivateSecurityDescriptor::attributes() noexcept {
  return &attributes_;
}

bool WindowsPrivateSecurityDescriptor::release(std::string &detail) {
  if (descriptor_ == nullptr)
    return true;
  if (::LocalFree(descriptor_) != nullptr) {
    append_detail(detail, "failed to release the private Windows security descriptor: " +
                            windows_error(::GetLastError()));
    return false;
  }
  descriptor_ = nullptr;
  attributes_.lpSecurityDescriptor = nullptr;
  return true;
}

bool prepare_windows_private_security(WindowsPrivateObjectKind kind,
                                      WindowsPrivateSecurityDescriptor &security,
                                      std::string &detail) {
  detail.clear();
  if (security.descriptor_ != nullptr) {
    detail = "private Windows security descriptor is already initialized";
    return false;
  }

  std::vector<std::max_align_t> token_buffer;
  if (!read_process_token_record(TokenUser, sizeof(TOKEN_USER), "user", token_buffer, detail)) {
    return false;
  }
  const auto *token_user = reinterpret_cast<const TOKEN_USER *>(token_buffer.data());
  if (token_user->User.Sid == nullptr || ::IsValidSid(token_user->User.Sid) == 0) {
    detail = "the process token contains an invalid user SID";
    return false;
  }

  LPWSTR raw_sid_text = nullptr;
  if (::ConvertSidToStringSidW(token_user->User.Sid, &raw_sid_text) == 0) {
    detail =
      "failed to encode the process user for a private ACL: " + windows_error(::GetLastError());
    return false;
  }
  UniqueLocalMemory sid_text(raw_sid_text);
  const std::wstring inheritance = kind == WindowsPrivateObjectKind::Directory ? L"OICI" : L"";
  const std::wstring sddl = L"D:P(A;" + inheritance + L";FA;;;SY)(A;" + inheritance +
                            L";FA;;;BA)(A;" + inheritance + L";FA;;;" + raw_sid_text + L")";

  if (!sid_text.release(detail, "failed to release the private ACL user SID"))
    return false;

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                             &descriptor, nullptr) == 0) {
    detail = "failed to create the private Windows security descriptor: " +
             windows_error(::GetLastError());
    return false;
  }
  security.descriptor_ = descriptor;
  if (::IsValidSecurityDescriptor(security.descriptor_) == 0) {
    detail = "the generated private Windows security descriptor is invalid";
    if (!security.release(detail))
      return false;
    return false;
  }
  security.attributes_.nLength = sizeof(security.attributes_);
  security.attributes_.lpSecurityDescriptor = security.descriptor_;
  security.attributes_.bInheritHandle = FALSE;
  return true;
}

bool validate_windows_private_object_security(HANDLE handle, WindowsPrivateObjectKind kind,
                                              std::string &detail) {
  detail.clear();
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    detail = "private object security validation requires a valid handle";
    return false;
  }

  std::vector<std::max_align_t> owner_buffer;
  if (!read_process_token_record(TokenOwner, sizeof(TOKEN_OWNER), "default owner", owner_buffer,
                                 detail)) {
    return false;
  }
  const auto *token_owner = reinterpret_cast<const TOKEN_OWNER *>(owner_buffer.data());
  if (token_owner->Owner == nullptr || ::IsValidSid(token_owner->Owner) == 0) {
    detail = "the process token contains an invalid default owner SID";
    return false;
  }

  WindowsPrivateSecurityDescriptor expected;
  if (!prepare_windows_private_security(kind, expected, detail))
    return false;
  UniqueLocalMemory actual;

  PACL expected_dacl = nullptr;
  BOOL expected_dacl_present = FALSE;
  BOOL expected_dacl_defaulted = FALSE;
  if (::GetSecurityDescriptorDacl(
        static_cast<PSECURITY_DESCRIPTOR>(expected.attributes()->lpSecurityDescriptor),
        &expected_dacl_present, &expected_dacl, &expected_dacl_defaulted) == 0 ||
      expected_dacl_present == FALSE || expected_dacl == nullptr) {
    return fail_private_security_validation(
      expected, actual, detail,
      "the generated private security descriptor has no inspectable DACL");
  }

  PSID actual_owner = nullptr;
  PACL actual_dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_actual = nullptr;
  const DWORD query_error = ::GetSecurityInfo(
    handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &actual_owner,
    nullptr, &actual_dacl, nullptr, &raw_actual);
  actual.adopt(raw_actual);
  if (query_error != ERROR_SUCCESS) {
    return fail_private_security_validation(expected, actual, detail,
                                            "failed to inspect private object security: " +
                                              windows_error(query_error));
  }
  if (actual_owner == nullptr || ::IsValidSid(actual_owner) == 0 ||
      ::EqualSid(actual_owner, token_owner->Owner) == 0) {
    return fail_private_security_validation(
      expected, actual, detail,
      "private object owner does not match the process token default owner");
  }

  SECURITY_DESCRIPTOR_CONTROL control = 0U;
  DWORD revision = 0U;
  if (::GetSecurityDescriptorControl(raw_actual, &control, &revision) == 0) {
    return fail_private_security_validation(expected, actual, detail,
                                            "failed to inspect private object DACL control: " +
                                              windows_error(::GetLastError()));
  }
  if (actual_dacl == nullptr || (control & SE_DACL_PROTECTED) == 0U) {
    return fail_private_security_validation(
      expected, actual, detail, "private object DACL is absent or inherits from its parent");
  }

  ACL_SIZE_INFORMATION expected_acl_information{};
  ACL_SIZE_INFORMATION actual_acl_information{};
  if (::GetAclInformation(expected_dacl, &expected_acl_information,
                          sizeof(expected_acl_information), AclSizeInformation) == 0 ||
      ::GetAclInformation(actual_dacl, &actual_acl_information, sizeof(actual_acl_information),
                          AclSizeInformation) == 0) {
    return fail_private_security_validation(expected, actual, detail,
                                            "failed to inspect private object ACL entries: " +
                                              windows_error(::GetLastError()));
  }
  if (expected_acl_information.AceCount != 3U ||
      actual_acl_information.AceCount != expected_acl_information.AceCount) {
    return fail_private_security_validation(
      expected, actual, detail,
      "private object DACL does not contain exactly the three required ACEs");
  }

  std::array<bool, 3U> matched_expected{};
  for (DWORD actual_index = 0U; actual_index < actual_acl_information.AceCount; ++actual_index) {
    void *raw_actual_ace = nullptr;
    if (::GetAce(actual_dacl, actual_index, &raw_actual_ace) == 0 || raw_actual_ace == nullptr) {
      return fail_private_security_validation(expected, actual, detail,
                                              "failed to read a private object ACE: " +
                                                windows_error(::GetLastError()));
    }
    const auto *actual_header = static_cast<const ACE_HEADER *>(raw_actual_ace);
    if (actual_header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
        actual_header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) {
      return fail_private_security_validation(
        expected, actual, detail, "private object DACL contains a non-allow or malformed ACE");
    }
    const auto *actual_allowed = static_cast<const ACCESS_ALLOWED_ACE *>(raw_actual_ace);
    PSID actual_sid = const_cast<DWORD *>(&actual_allowed->SidStart);
    if (::IsValidSid(actual_sid) == 0) {
      return fail_private_security_validation(expected, actual, detail,
                                              "private object DACL contains an invalid SID");
    }

    bool matched = false;
    for (DWORD expected_index = 0U; expected_index < expected_acl_information.AceCount;
         ++expected_index) {
      if (matched_expected[expected_index])
        continue;
      void *raw_expected_ace = nullptr;
      if (::GetAce(expected_dacl, expected_index, &raw_expected_ace) == 0 ||
          raw_expected_ace == nullptr) {
        return fail_private_security_validation(expected, actual, detail,
                                                "failed to read a generated private object ACE: " +
                                                  windows_error(::GetLastError()));
      }
      const auto *expected_header = static_cast<const ACE_HEADER *>(raw_expected_ace);
      if (expected_header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
          expected_header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) {
        return fail_private_security_validation(
          expected, actual, detail, "generated private object DACL contains a malformed ACE");
      }
      const auto *expected_allowed = static_cast<const ACCESS_ALLOWED_ACE *>(raw_expected_ace);
      PSID expected_sid = const_cast<DWORD *>(&expected_allowed->SidStart);
      if (actual_header->AceFlags == expected_header->AceFlags &&
          actual_allowed->Mask == expected_allowed->Mask && ::IsValidSid(expected_sid) != 0 &&
          ::EqualSid(actual_sid, expected_sid) != 0) {
        matched_expected[expected_index] = true;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return fail_private_security_validation(
        expected, actual, detail, "private object DACL grants unexpected access or inheritance");
    }
  }

  for (bool matched : matched_expected) {
    if (!matched) {
      return fail_private_security_validation(
        expected, actual, detail, "private object DACL omits a required private principal");
    }
  }
  return release_validation_memory(expected, actual, detail);
}

bool validate_windows_persistent_acl_support(const std::filesystem::path &existing_path,
                                             std::string &detail) {
  detail.clear();
  if (existing_path.empty()) {
    detail = "persistent ACL validation requires a nonempty existing path";
    return false;
  }

  constexpr DWORD kMaximumVolumePathCharacters = 32768U;
  std::vector<wchar_t> volume_path(kMaximumVolumePathCharacters, L'\0');
  if (::GetVolumePathNameW(existing_path.c_str(), volume_path.data(),
                           static_cast<DWORD>(volume_path.size())) == 0) {
    detail = "failed to resolve the Windows volume for private object creation: " +
             windows_error(::GetLastError());
    return false;
  }
  DWORD filesystem_flags = 0U;
  if (::GetVolumeInformationW(volume_path.data(), nullptr, 0U, nullptr, nullptr, &filesystem_flags,
                              nullptr, 0U) == 0) {
    detail =
      "failed to inspect Windows volume security capabilities: " + windows_error(::GetLastError());
    return false;
  }
  if ((filesystem_flags & FS_PERSISTENT_ACLS) == 0U) {
    detail = "the target Windows volume does not support persistent ACLs";
    return false;
  }
  return true;
}

} // namespace nebula::cli
