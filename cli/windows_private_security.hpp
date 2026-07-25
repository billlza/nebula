#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace nebula::cli {

enum class WindowsPrivateObjectKind : std::uint8_t {
  File,
  Directory,
};

// Owns a Windows security descriptor whose protected DACL grants full control
// only to LocalSystem, built-in administrators, and the process user. Callers
// must explicitly release it after the CreateFileW/CreateDirectoryW call so a
// cleanup failure can be reported. The destructor is a final best-effort guard.
class WindowsPrivateSecurityDescriptor final {
public:
  WindowsPrivateSecurityDescriptor() = default;
  WindowsPrivateSecurityDescriptor(const WindowsPrivateSecurityDescriptor &) = delete;
  WindowsPrivateSecurityDescriptor &operator=(const WindowsPrivateSecurityDescriptor &) = delete;
  WindowsPrivateSecurityDescriptor(WindowsPrivateSecurityDescriptor &&) = delete;
  WindowsPrivateSecurityDescriptor &operator=(WindowsPrivateSecurityDescriptor &&) = delete;
  ~WindowsPrivateSecurityDescriptor();

  [[nodiscard]] SECURITY_ATTRIBUTES *attributes() noexcept;
  [[nodiscard]] bool release(std::string &detail);

private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};

  friend bool prepare_windows_private_security(WindowsPrivateObjectKind kind,
                                               WindowsPrivateSecurityDescriptor &security,
                                               std::string &detail);
};

[[nodiscard]] bool prepare_windows_private_security(WindowsPrivateObjectKind kind,
                                                    WindowsPrivateSecurityDescriptor &security,
                                                    std::string &detail);

// Verifies that a bound object has the exact protected DACL prepared above and
// that its owner is the process token's default owner. The handle must include
// READ_CONTROL. This closes the CreateDirectoryW/open binding window for
// untrusted parent-directory principals that cannot impersonate that owner.
[[nodiscard]] bool validate_windows_private_object_security(HANDLE handle,
                                                            WindowsPrivateObjectKind kind,
                                                            std::string &detail);

// Windows can accept a security descriptor while silently ignoring its DACL
// on filesystems such as FAT/exFAT. Private object creation must fail closed on
// a volume that does not advertise persistent ACL support.
[[nodiscard]] bool
validate_windows_persistent_acl_support(const std::filesystem::path &existing_path,
                                        std::string &detail);

} // namespace nebula::cli

#endif
