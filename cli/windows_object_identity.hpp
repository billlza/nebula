#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstdint>

namespace nebula::cli {

// A filesystem object identity that remains valid on both NTFS and ReFS.
// BY_HANDLE_FILE_INFORMATION exposes only a 64-bit file index, which is not
// guaranteed to be unique on ReFS; FILE_ID_INFO supplies the full 128-bit ID.
struct WindowsObjectIdentity final {
  std::uint64_t volume_serial_number = 0U;
  std::array<std::uint8_t, 16U> file_id{};

  friend bool operator==(const WindowsObjectIdentity &, const WindowsObjectIdentity &) = default;
};

// Returns ERROR_SUCCESS or the exact Win32 error from FileIdInfo inspection.
// The output is updated only after a complete identity has been obtained.
[[nodiscard]] DWORD read_windows_object_identity(HANDLE handle,
                                                 WindowsObjectIdentity &identity) noexcept;

} // namespace nebula::cli

#endif
