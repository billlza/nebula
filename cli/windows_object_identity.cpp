#include "windows_object_identity.hpp"

#if !defined(_WIN32)
#error "windows_object_identity.cpp must only be compiled for Windows targets"
#endif

#include <cstring>

namespace nebula::cli {
namespace {

constexpr bool all_file_id_bytes(const std::array<std::uint8_t, 16U> &file_id,
                                 std::uint8_t expected) noexcept {
  for (std::uint8_t byte : file_id) {
    if (byte != expected)
      return false;
  }
  return true;
}

constexpr bool usable_file_id(const std::array<std::uint8_t, 16U> &file_id) noexcept {
  return !all_file_id_bytes(file_id, 0U) && !all_file_id_bytes(file_id, 0xffU);
}

constexpr std::array<std::uint8_t, 16U> kUnsupportedFileId{};
constexpr std::array<std::uint8_t, 16U> kUnavailableFileId{0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
                                                           0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
                                                           0xffU, 0xffU, 0xffU, 0xffU};
constexpr std::array<std::uint8_t, 16U> kUsableFileId{1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                                      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
static_assert(!usable_file_id(kUnsupportedFileId));
static_assert(!usable_file_id(kUnavailableFileId));
static_assert(usable_file_id(kUsableFileId));

} // namespace

DWORD read_windows_object_identity(HANDLE handle, WindowsObjectIdentity &identity) noexcept {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    return ERROR_INVALID_HANDLE;

  FILE_ID_INFO information{};
  if (::GetFileInformationByHandleEx(handle, FileIdInfo, &information, sizeof(information)) == 0) {
    return ::GetLastError();
  }

  WindowsObjectIdentity inspected;
  inspected.volume_serial_number = static_cast<std::uint64_t>(information.VolumeSerialNumber);
  static_assert(sizeof(information.FileId.Identifier) == sizeof(inspected.file_id));
  std::memcpy(inspected.file_id.data(), information.FileId.Identifier, inspected.file_id.size());
  if (!usable_file_id(inspected.file_id))
    return ERROR_NOT_SUPPORTED;
  identity = inspected;
  return ERROR_SUCCESS;
}

} // namespace nebula::cli
