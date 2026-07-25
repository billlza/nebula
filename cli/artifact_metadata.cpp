#include "artifact_metadata.hpp"

#include "cli/artifact_digest.hpp"

#include "boot/protocol_abi_contract.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr std::array<std::string_view, 15> kMetadataKeys = {
  "version",
  "build_inputs_sha256",
  "mode",
  "profile",
  "artifact_kind",
  "compiler_schema_version",
  "cache_schema_version",
  "strict_region",
  "warnings_as_errors",
  "no_std",
  "runtime_profile",
  "target",
  "panic_policy",
  "artifact_size",
  "artifact_sha256",
};

enum class MetadataField : std::size_t {
  Version,
  BuildInputsSha256,
  Mode,
  Profile,
  ArtifactKind,
  CompilerSchemaVersion,
  CacheSchemaVersion,
  StrictRegion,
  WarningsAsErrors,
  NoStd,
  RuntimeProfile,
  Target,
  PanicPolicy,
  ArtifactSize,
  ArtifactSha256,
};

constexpr std::size_t field_index(MetadataField field) { return static_cast<std::size_t>(field); }

ArtifactMetadataError make_error(ArtifactMetadataErrorCode code, std::string detail,
                                 std::size_t line = 0U, std::string field = {}) {
  return ArtifactMetadataError{code, line, std::move(field), std::move(detail)};
}

ArtifactMetadataResult parse_error(ArtifactMetadataErrorCode code, std::string detail,
                                   std::size_t line = 0U, std::string field = {}) {
  ArtifactMetadataResult result;
  result.error = make_error(code, std::move(detail), line, std::move(field));
  return result;
}

ArtifactMetadataSerializationResult serialization_error(ArtifactMetadataError error) {
  ArtifactMetadataSerializationResult result;
  result.error = std::move(error);
  return result;
}

std::optional<std::size_t> find_key(std::string_view key) {
  const auto found = std::find(kMetadataKeys.begin(), kMetadataKeys.end(), key);
  if (found == kMetadataKeys.end())
    return std::nullopt;
  return static_cast<std::size_t>(found - kMetadataKeys.begin());
}

bool is_lower_sha256(std::string_view value) {
  return value.size() == 64U && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         });
}

bool is_target(std::string_view value) {
  const auto is_alnum = [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
  };
  return !value.empty() && value.size() <= 128U && is_alnum(value.front()) &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '+';
         });
}

template <typename UInt>
bool parse_unsigned(std::string_view value, UInt &output, bool require_positive) {
  static_assert(std::is_unsigned_v<UInt>);
  if (value.empty() || (value.size() > 1U && value.front() == '0'))
    return false;
  if (!std::all_of(value.begin(), value.end(),
                   [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
    return false;
  }
  UInt parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    return false;
  if (require_positive && parsed == 0)
    return false;
  output = parsed;
  return true;
}

bool parse_positive_int(std::string_view value, int &output) {
  unsigned parsed = 0U;
  if (!parse_unsigned(value, parsed, true) ||
      parsed > static_cast<unsigned>(std::numeric_limits<int>::max())) {
    return false;
  }
  output = static_cast<int>(parsed);
  return true;
}

bool parse_bool(std::string_view value, bool &output) {
  if (value == "0") {
    output = false;
    return true;
  }
  if (value == "1") {
    output = true;
    return true;
  }
  return false;
}

bool is_one_of(std::string_view value, std::initializer_list<std::string_view> choices) {
  return std::find(choices.begin(), choices.end(), value) != choices.end();
}

ArtifactMetadataError validate_metadata(const ArtifactMetadata &metadata) {
  const ArtifactBuildKey &build = metadata.build;
  if (!is_lower_sha256(build.build_inputs_sha256)) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "build_inputs_sha256 must be exactly 64 lowercase hexadecimal characters", 0U,
                      "build_inputs_sha256");
  }
  if (!is_one_of(build.mode, {"debug", "release"})) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue, "mode must be debug or release", 0U,
                      "mode");
  }
  if (!is_one_of(build.profile, {"auto", "fast", "deep"})) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "profile must be auto, fast, or deep", 0U, "profile");
  }
  if (!is_one_of(build.artifact_kind,
                 {"executable", "staticlib", "sharedlib", "freestanding-object"})) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "artifact_kind is not a supported compiler artifact kind", 0U,
                      "artifact_kind");
  }
  if (build.compiler_schema_version <= 0) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "compiler_schema_version must be positive", 0U, "compiler_schema_version");
  }
  if (build.cache_schema_version <= 0) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "cache_schema_version must be positive", 0U, "cache_schema_version");
  }
  if (!is_one_of(build.runtime_profile, {"hosted", "system"})) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "runtime_profile must be hosted or system", 0U, "runtime_profile");
  }
  if (!is_target(build.target)) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "target must be 1-128 target-name characters", 0U, "target");
  }
  if (!is_one_of(build.panic_policy, {"abort", "trap", "unwind"})) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "panic_policy must be abort, trap, or unwind", 0U, "panic_policy");
  }
  if (build.runtime_profile == "system" && (!build.strict_region || !build.no_std)) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "system artifacts require strict_region=1 and no_std=1", 0U,
                      "runtime_profile");
  }
  if (build.no_std && !build.strict_region) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "no_std artifacts require strict_region=1", 0U, "no_std");
  }
  if (build.no_std && build.panic_policy == "unwind") {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "no_std artifacts cannot use panic_policy=unwind", 0U, "panic_policy");
  }
  if (build.artifact_kind == "freestanding-object" &&
      (build.runtime_profile != "system" || !build.strict_region || !build.no_std ||
       build.target != nebula::boot::kUosX86_64TargetTriple ||
       build.panic_policy != nebula::boot::kUosX86_64PanicPolicy)) {
    return make_error(
      ArtifactMetadataErrorCode::InvalidValue,
      "freestanding-object requires the exact system/x86_64-unknown-none/trap contract", 0U,
      "artifact_kind");
  }
  if (metadata.content.size == 0U) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue, "artifact_size must be positive", 0U,
                      "artifact_size");
  }
  if (!is_lower_sha256(metadata.content.sha256)) {
    return make_error(ArtifactMetadataErrorCode::InvalidValue,
                      "artifact_sha256 must be exactly 64 lowercase hexadecimal characters", 0U,
                      "artifact_sha256");
  }
  return {};
}

ArtifactMetadataError error_with_parse_line(ArtifactMetadataError error,
                                            const std::array<std::size_t, 15> &lines) {
  if (!error.field.empty()) {
    if (const auto index = find_key(error.field); index.has_value())
      error.line = lines[*index];
  }
  return error;
}

struct BoundedReadResult {
  std::optional<std::string> payload;
  ArtifactMetadataError error;
};

BoundedReadResult read_metadata_file(const std::filesystem::path &path) {
#if defined(_WIN32)
  const auto windows_message = [](DWORD error) {
    return std::system_category().message(static_cast<int>(error));
  };
  HANDLE file = ::CreateFileW(
    path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    BoundedReadResult result;
    result.error = make_error(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                                ? ArtifactMetadataErrorCode::Missing
                                : ArtifactMetadataErrorCode::Io,
                              "failed to open artifact metadata: " + windows_message(error));
    return result;
  }
  const auto close_file = [&file]() {
    if (file == INVALID_HANDLE_VALUE)
      return true;
    const bool ok = ::CloseHandle(file) != 0;
    file = INVALID_HANDLE_VALUE;
    return ok;
  };
  FILE_ATTRIBUTE_TAG_INFO tag{};
  BY_HANDLE_FILE_INFORMATION before{};
  if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
      ::GetFileInformationByHandle(file, &before) == 0) {
    const DWORD error = ::GetLastError();
    close_file();
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Io,
                       "failed to inspect artifact metadata: " + windows_message(error))};
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    close_file();
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Symlink, "artifact metadata is a reparse point")};
  }
  if (::GetFileType(file) != FILE_TYPE_DISK) {
    close_file();
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::NotRegularFile,
                                     "artifact metadata handle is not a disk file")};
  }
  if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    close_file();
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::NotRegularFile,
                                     "artifact metadata is not a regular file")};
  }
  const std::uint64_t size =
    (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
  if (size == 0U || size > kArtifactMetadataMaxBytes) {
    close_file();
    return {std::nullopt, make_error(size == 0U ? ArtifactMetadataErrorCode::InvalidLine
                                                : ArtifactMetadataErrorCode::TooLarge,
                                     size == 0U ? "artifact metadata is empty"
                                                : "artifact metadata exceeds 4096 bytes")};
  }
  std::string payload(static_cast<std::size_t>(size), '\0');
  DWORD count = 0U;
  if (::ReadFile(file, payload.data(), static_cast<DWORD>(payload.size()), &count, nullptr) == 0 ||
      count != payload.size()) {
    const DWORD error = ::GetLastError();
    close_file();
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::Unstable,
                                     "artifact metadata changed during its bounded read: " +
                                       windows_message(error))};
  }
  BY_HANDLE_FILE_INFORMATION after{};
  if (::GetFileInformationByHandle(file, &after) == 0) {
    const DWORD error = ::GetLastError();
    close_file();
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Io,
                       "failed to recheck artifact metadata: " + windows_message(error))};
  }
  if (!close_file()) {
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::Io,
                                     "failed to close artifact metadata after reading")};
  }
  if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
      before.nFileIndexHigh != after.nFileIndexHigh ||
      before.nFileIndexLow != after.nFileIndexLow || before.nFileSizeHigh != after.nFileSizeHigh ||
      before.nFileSizeLow != after.nFileSizeLow || before.nNumberOfLinks != after.nNumberOfLinks ||
      before.ftLastWriteTime.dwHighDateTime != after.ftLastWriteTime.dwHighDateTime ||
      before.ftLastWriteTime.dwLowDateTime != after.ftLastWriteTime.dwLowDateTime) {
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::Unstable,
                                     "artifact metadata changed while it was being read")};
  }
  return {std::move(payload), {}};
#else
  int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int file = ::open(path.c_str(), flags);
  if (file < 0) {
    const int error = errno;
    ArtifactMetadataErrorCode code = ArtifactMetadataErrorCode::Io;
    if (error == ENOENT || error == ENOTDIR)
      code = ArtifactMetadataErrorCode::Missing;
#ifdef ELOOP
    else if (error == ELOOP)
      code = ArtifactMetadataErrorCode::Symlink;
#endif
    return {std::nullopt, make_error(code, "failed to open artifact metadata: " +
                                             std::string(std::strerror(error)))};
  }
  struct stat before{};
  if (::fstat(file, &before) != 0) {
    const int error = errno;
    ::close(file);
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Io, "failed to inspect artifact metadata: " +
                                                        std::string(std::strerror(error)))};
  }
  if (!S_ISREG(before.st_mode)) {
    ::close(file);
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::NotRegularFile,
                                     "artifact metadata is not a regular file")};
  }
  if (before.st_size <= 0) {
    ::close(file);
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::InvalidLine, "artifact metadata is empty")};
  }
  if (static_cast<std::uintmax_t>(before.st_size) > kArtifactMetadataMaxBytes) {
    ::close(file);
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::TooLarge,
                                     "artifact metadata exceeds 4096 bytes")};
  }
  std::string payload(static_cast<std::size_t>(before.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < payload.size()) {
    const ssize_t count = ::read(file, payload.data() + offset, payload.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      const int error = errno;
      ::close(file);
      return {std::nullopt,
              make_error(ArtifactMetadataErrorCode::Unstable,
                         count == 0 ? "artifact metadata was truncated during its bounded read"
                                    : "artifact metadata read failed: " +
                                        std::string(std::strerror(error)))};
    }
    offset += static_cast<std::size_t>(count);
  }
  char extra = '\0';
  ssize_t extra_count = 0;
  do {
    extra_count = ::read(file, &extra, 1U);
  } while (extra_count < 0 && errno == EINTR);
  struct stat after{};
  const int stat_result = ::fstat(file, &after);
  const int stat_error = errno;
  struct stat path_after{};
  const int path_stat_result = ::lstat(path.c_str(), &path_after);
  const int close_result = ::close(file);
  const int close_error = errno;
  if (stat_result != 0) {
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Io, "failed to recheck artifact metadata: " +
                                                        std::string(std::strerror(stat_error)))};
  }
  if (close_result != 0) {
    return {std::nullopt,
            make_error(ArtifactMetadataErrorCode::Io, "failed to close artifact metadata: " +
                                                        std::string(std::strerror(close_error)))};
  }
  bool stable = extra_count == 0 && path_stat_result == 0 && S_ISREG(path_after.st_mode) &&
                path_after.st_dev == after.st_dev && path_after.st_ino == after.st_ino &&
                before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                before.st_nlink == after.st_nlink && before.st_size == after.st_size &&
                before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime;
#if defined(__APPLE__)
  stable = stable && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
           before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#elif defined(__linux__)
  stable = stable && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
  if (!stable) {
    return {std::nullopt, make_error(ArtifactMetadataErrorCode::Unstable,
                                     "artifact metadata changed while it was being read")};
  }
  return {std::move(payload), {}};
#endif
}

ArtifactReuseAssessment rebuild_assessment(ArtifactReuseReason reason, std::string detail,
                                           std::string field = {}) {
  return {ArtifactReuseDisposition::Rebuild, reason, std::move(field), std::move(detail),
          std::nullopt};
}

ArtifactReuseAssessment reject_assessment(ArtifactReuseReason reason, std::string detail) {
  return {ArtifactReuseDisposition::Reject, reason, {}, std::move(detail), std::nullopt};
}

} // namespace

std::filesystem::path artifact_metadata_path(const std::filesystem::path &artifact) {
  std::filesystem::path result = artifact;
  result += ".nebmeta";
  return result;
}

ArtifactMetadataResult parse_artifact_metadata(std::string_view payload) {
  if (payload.empty())
    return parse_error(ArtifactMetadataErrorCode::InvalidLine, "artifact metadata is empty");
  if (payload.size() > kArtifactMetadataMaxBytes) {
    return parse_error(ArtifactMetadataErrorCode::TooLarge, "artifact metadata exceeds 4096 bytes");
  }
  if (payload.back() != '\n') {
    return parse_error(ArtifactMetadataErrorCode::InvalidLine,
                       "artifact metadata must end with LF");
  }
  for (const unsigned char byte : payload) {
    if (byte != '\n' && (byte < 0x20U || byte > 0x7eU)) {
      return parse_error(ArtifactMetadataErrorCode::InvalidEncoding,
                         "artifact metadata permits printable ASCII and LF only");
    }
  }

  std::array<std::string_view, 15> values{};
  std::array<std::size_t, 15> lines{};
  std::array<bool, 15> seen{};
  std::size_t position = 0U;
  std::size_t line_number = 0U;
  while (position < payload.size()) {
    ++line_number;
    const std::size_t newline = payload.find('\n', position);
    if (newline == std::string_view::npos)
      return parse_error(ArtifactMetadataErrorCode::InvalidLine,
                         "artifact metadata line is not LF terminated", line_number);
    const std::string_view line = payload.substr(position, newline - position);
    position = newline + 1U;
    if (line.empty()) {
      return parse_error(ArtifactMetadataErrorCode::InvalidLine,
                         "artifact metadata contains an empty line", line_number);
    }
    if (line.size() > kArtifactMetadataMaxLineBytes) {
      return parse_error(ArtifactMetadataErrorCode::TooLarge,
                         "artifact metadata line exceeds 256 bytes", line_number);
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos || equals == 0U ||
        line.find('=', equals + 1U) != std::string_view::npos) {
      return parse_error(ArtifactMetadataErrorCode::InvalidLine,
                         "artifact metadata line must contain exactly one key/value separator",
                         line_number);
    }
    const std::string_view key = line.substr(0U, equals);
    const std::string_view value = line.substr(equals + 1U);
    if (line_number == 1U && key != "version") {
      return parse_error(ArtifactMetadataErrorCode::VersionNotFirst,
                         "version must be the first artifact metadata key", line_number,
                         std::string(key));
    }
    const auto index = find_key(key);
    if (!index.has_value()) {
      return parse_error(ArtifactMetadataErrorCode::UnknownKey, "unknown artifact metadata key",
                         line_number, std::string(key));
    }
    if (seen[*index]) {
      return parse_error(ArtifactMetadataErrorCode::DuplicateKey, "duplicate artifact metadata key",
                         line_number, std::string(key));
    }
    seen[*index] = true;
    values[*index] = value;
    lines[*index] = line_number;

    if (line_number == 1U) {
      unsigned version = 0U;
      if (!parse_unsigned(value, version, true)) {
        return parse_error(ArtifactMetadataErrorCode::InvalidValue,
                           "version must be a canonical positive decimal integer", line_number,
                           "version");
      }
      if (version != static_cast<unsigned>(kArtifactMetadataVersion)) {
        return parse_error(ArtifactMetadataErrorCode::UnsupportedVersion,
                           "artifact metadata version is not supported", line_number, "version");
      }
    }
  }

  for (std::size_t index = 0U; index < seen.size(); ++index) {
    if (!seen[index]) {
      return parse_error(ArtifactMetadataErrorCode::MissingKey,
                         "required artifact metadata key is missing", 0U,
                         std::string(kMetadataKeys[index]));
    }
  }

  ArtifactMetadata metadata;
  metadata.build.build_inputs_sha256 = values[field_index(MetadataField::BuildInputsSha256)];
  metadata.build.mode = values[field_index(MetadataField::Mode)];
  metadata.build.profile = values[field_index(MetadataField::Profile)];
  metadata.build.artifact_kind = values[field_index(MetadataField::ArtifactKind)];
  if (!parse_positive_int(values[field_index(MetadataField::CompilerSchemaVersion)],
                          metadata.build.compiler_schema_version)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue,
                       "compiler_schema_version must be a canonical positive decimal integer",
                       lines[field_index(MetadataField::CompilerSchemaVersion)],
                       "compiler_schema_version");
  }
  if (!parse_positive_int(values[field_index(MetadataField::CacheSchemaVersion)],
                          metadata.build.cache_schema_version)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue,
                       "cache_schema_version must be a canonical positive decimal integer",
                       lines[field_index(MetadataField::CacheSchemaVersion)],
                       "cache_schema_version");
  }
  if (!parse_bool(values[field_index(MetadataField::StrictRegion)], metadata.build.strict_region)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue, "strict_region must be 0 or 1",
                       lines[field_index(MetadataField::StrictRegion)], "strict_region");
  }
  if (!parse_bool(values[field_index(MetadataField::WarningsAsErrors)],
                  metadata.build.warnings_as_errors)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue, "warnings_as_errors must be 0 or 1",
                       lines[field_index(MetadataField::WarningsAsErrors)], "warnings_as_errors");
  }
  if (!parse_bool(values[field_index(MetadataField::NoStd)], metadata.build.no_std)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue, "no_std must be 0 or 1",
                       lines[field_index(MetadataField::NoStd)], "no_std");
  }
  metadata.build.runtime_profile = values[field_index(MetadataField::RuntimeProfile)];
  metadata.build.target = values[field_index(MetadataField::Target)];
  metadata.build.panic_policy = values[field_index(MetadataField::PanicPolicy)];
  if (!parse_unsigned(values[field_index(MetadataField::ArtifactSize)], metadata.content.size,
                      true)) {
    return parse_error(ArtifactMetadataErrorCode::InvalidValue,
                       "artifact_size must be a canonical positive decimal integer",
                       lines[field_index(MetadataField::ArtifactSize)], "artifact_size");
  }
  metadata.content.sha256 = values[field_index(MetadataField::ArtifactSha256)];

  ArtifactMetadataError validation = validate_metadata(metadata);
  if (validation.code != ArtifactMetadataErrorCode::None) {
    ArtifactMetadataResult result;
    result.error = error_with_parse_line(std::move(validation), lines);
    return result;
  }
  ArtifactMetadataResult result;
  result.value = std::move(metadata);
  return result;
}

ArtifactMetadataResult read_artifact_metadata(const std::filesystem::path &artifact) {
  BoundedReadResult read = read_metadata_file(artifact_metadata_path(artifact));
  if (!read.payload.has_value()) {
    ArtifactMetadataResult result;
    result.error = std::move(read.error);
    return result;
  }
  return parse_artifact_metadata(*read.payload);
}

ArtifactMetadataSerializationResult serialize_artifact_metadata(const ArtifactMetadata &metadata) {
  ArtifactMetadataError validation = validate_metadata(metadata);
  if (validation.code != ArtifactMetadataErrorCode::None)
    return serialization_error(std::move(validation));

  std::ostringstream output;
  output << "version=" << kArtifactMetadataVersion << '\n';
  output << "build_inputs_sha256=" << metadata.build.build_inputs_sha256 << '\n';
  output << "mode=" << metadata.build.mode << '\n';
  output << "profile=" << metadata.build.profile << '\n';
  output << "artifact_kind=" << metadata.build.artifact_kind << '\n';
  output << "compiler_schema_version=" << metadata.build.compiler_schema_version << '\n';
  output << "cache_schema_version=" << metadata.build.cache_schema_version << '\n';
  output << "strict_region=" << (metadata.build.strict_region ? '1' : '0') << '\n';
  output << "warnings_as_errors=" << (metadata.build.warnings_as_errors ? '1' : '0') << '\n';
  output << "no_std=" << (metadata.build.no_std ? '1' : '0') << '\n';
  output << "runtime_profile=" << metadata.build.runtime_profile << '\n';
  output << "target=" << metadata.build.target << '\n';
  output << "panic_policy=" << metadata.build.panic_policy << '\n';
  output << "artifact_size=" << metadata.content.size << '\n';
  output << "artifact_sha256=" << metadata.content.sha256 << '\n';
  if (!output.good()) {
    return serialization_error(
      make_error(ArtifactMetadataErrorCode::Io, "failed to serialize artifact metadata"));
  }
  std::string payload = output.str();
  if (payload.size() > kArtifactMetadataMaxBytes) {
    return serialization_error(make_error(ArtifactMetadataErrorCode::TooLarge,
                                          "serialized artifact metadata exceeds 4096 bytes"));
  }
  ArtifactMetadataSerializationResult result;
  result.payload = std::move(payload);
  return result;
}

ArtifactBuildKeyComparison compare_artifact_build_keys(const ArtifactBuildKey &actual,
                                                       const ArtifactBuildKey &expected) {
#define NEBULA_COMPARE_BUILD_FIELD(name)                                                           \
  if (actual.name != expected.name)                                                                \
  return {ArtifactBuildKeyComparisonStatus::Mismatch, #name}
  NEBULA_COMPARE_BUILD_FIELD(build_inputs_sha256);
  NEBULA_COMPARE_BUILD_FIELD(mode);
  NEBULA_COMPARE_BUILD_FIELD(profile);
  NEBULA_COMPARE_BUILD_FIELD(artifact_kind);
  NEBULA_COMPARE_BUILD_FIELD(compiler_schema_version);
  NEBULA_COMPARE_BUILD_FIELD(cache_schema_version);
  NEBULA_COMPARE_BUILD_FIELD(strict_region);
  NEBULA_COMPARE_BUILD_FIELD(warnings_as_errors);
  NEBULA_COMPARE_BUILD_FIELD(no_std);
  NEBULA_COMPARE_BUILD_FIELD(runtime_profile);
  NEBULA_COMPARE_BUILD_FIELD(target);
  NEBULA_COMPARE_BUILD_FIELD(panic_policy);
#undef NEBULA_COMPARE_BUILD_FIELD
  return {ArtifactBuildKeyComparisonStatus::Match, {}};
}

ArtifactReuseAssessment assess_artifact_reuse(const std::filesystem::path &artifact,
                                              const ArtifactBuildKey &expected) {
  const nebula::cli::FileDigestResult digest = nebula::cli::sha256_file(artifact);
  if (!digest.ok()) {
    switch (digest.error) {
    case nebula::cli::FileDigestErrorCode::Missing:
      return rebuild_assessment(ArtifactReuseReason::ArtifactMissing, digest.detail);
    case nebula::cli::FileDigestErrorCode::Symlink:
    case nebula::cli::FileDigestErrorCode::NotRegularFile:
    case nebula::cli::FileDigestErrorCode::TooLarge:
      return reject_assessment(ArtifactReuseReason::UnsafeArtifact, digest.detail);
    case nebula::cli::FileDigestErrorCode::Io:
    case nebula::cli::FileDigestErrorCode::Unstable:
      return reject_assessment(ArtifactReuseReason::IoFailure, digest.detail);
    case nebula::cli::FileDigestErrorCode::None:
      return reject_assessment(ArtifactReuseReason::IoFailure,
                               "artifact digest result had no value and no error");
    }
  }
  ArtifactMetadataResult metadata = read_artifact_metadata(artifact);
  if (!metadata.ok()) {
    switch (metadata.error.code) {
    case ArtifactMetadataErrorCode::Missing:
      return rebuild_assessment(ArtifactReuseReason::MetadataMissing, metadata.error.detail);
    case ArtifactMetadataErrorCode::UnsupportedVersion:
      return rebuild_assessment(ArtifactReuseReason::UnsupportedMetadataVersion,
                                metadata.error.detail, metadata.error.field);
    case ArtifactMetadataErrorCode::Symlink:
    case ArtifactMetadataErrorCode::NotRegularFile:
    case ArtifactMetadataErrorCode::TooLarge:
    case ArtifactMetadataErrorCode::Io:
    case ArtifactMetadataErrorCode::Unstable:
      return reject_assessment(ArtifactReuseReason::IoFailure, metadata.error.detail);
    case ArtifactMetadataErrorCode::InvalidEncoding:
    case ArtifactMetadataErrorCode::InvalidLine:
    case ArtifactMetadataErrorCode::VersionNotFirst:
    case ArtifactMetadataErrorCode::UnknownKey:
    case ArtifactMetadataErrorCode::DuplicateKey:
    case ArtifactMetadataErrorCode::MissingKey:
    case ArtifactMetadataErrorCode::InvalidValue:
      return rebuild_assessment(ArtifactReuseReason::InvalidMetadata, metadata.error.detail,
                                metadata.error.field);
    case ArtifactMetadataErrorCode::None:
      return reject_assessment(ArtifactReuseReason::IoFailure,
                               "artifact metadata result had no value and no error");
    }
  }

  const ArtifactBuildKeyComparison comparison =
    compare_artifact_build_keys(metadata.value->build, expected);
  if (comparison.status == ArtifactBuildKeyComparisonStatus::Mismatch) {
    return rebuild_assessment(ArtifactReuseReason::BuildKeyMismatch,
                              "artifact build key does not match the requested build",
                              comparison.field);
  }

  if (digest.value->size != metadata.value->content.size ||
      digest.value->sha256 != metadata.value->content.sha256) {
    return rebuild_assessment(ArtifactReuseReason::ContentMismatch,
                              "artifact bytes do not match their metadata content identity");
  }
  return {
    ArtifactReuseDisposition::Reusable,
    ArtifactReuseReason::ContentIdentityObserved,
    {},
    "artifact build key and content identity matched during bounded verification",
    ArtifactContentIdentity{static_cast<std::uint64_t>(digest.value->size), digest.value->sha256}};
}

bool write_artifact_metadata(const std::filesystem::path &artifact,
                             const ArtifactMetadata &metadata, std::string &detail) {
  detail.clear();
  ArtifactMetadataSerializationResult serialized = serialize_artifact_metadata(metadata);
  if (!serialized.ok()) {
    detail = serialized.error.detail;
    if (!serialized.error.field.empty())
      detail += " (field: " + serialized.error.field + ")";
    return false;
  }
  const std::string &payload = *serialized.payload;
  const std::filesystem::path metadata_path = artifact_metadata_path(artifact);
#if defined(_WIN32)
  const auto windows_error_message = [](DWORD error) {
    return std::system_category().message(static_cast<int>(error));
  };
  const auto append_windows_cleanup_error = [&](std::string_view operation, DWORD error) {
    if (!detail.empty())
      detail += "; ";
    detail += std::string(operation) + ": " + windows_error_message(error);
  };
  HANDLE descriptor = INVALID_HANDLE_VALUE;
  std::filesystem::path temporary_path;
  DWORD temporary_create_error = ERROR_SUCCESS;
  for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::wstring temporary_name =
      metadata_path.filename().wstring() + L".tmp." +
      std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) + L"." +
      std::to_wstring(tick) + L"." + std::to_wstring(attempt);
    temporary_path = metadata_path.parent_path() / temporary_name;
    descriptor = ::CreateFileW(temporary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (descriptor != INVALID_HANDLE_VALUE)
      break;
    temporary_create_error = ::GetLastError();
    if (temporary_create_error != ERROR_FILE_EXISTS &&
        temporary_create_error != ERROR_ALREADY_EXISTS) {
      detail = "failed to create an exclusive metadata staging file: " +
               windows_error_message(temporary_create_error);
      return false;
    }
  }
  if (descriptor == INVALID_HANDLE_VALUE) {
    detail = "failed to allocate a unique metadata staging file after 64 attempts: " +
             windows_error_message(temporary_create_error);
    return false;
  }

  bool complete = true;
  std::size_t written = 0U;
  while (written < payload.size()) {
    const std::size_t remaining = payload.size() - written;
    const DWORD requested = static_cast<DWORD>(
      std::min(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD count = 0U;
    if (::WriteFile(descriptor, payload.data() + written, requested, &count, nullptr) == 0) {
      detail = "artifact metadata write failed: " + windows_error_message(::GetLastError());
      complete = false;
      break;
    }
    if (count == 0U) {
      detail = "artifact metadata write made no progress";
      complete = false;
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  if (complete && ::FlushFileBuffers(descriptor) == 0) {
    detail = "artifact metadata flush failed: " + windows_error_message(::GetLastError());
    complete = false;
  }
  if (::CloseHandle(descriptor) == 0) {
    append_windows_cleanup_error("artifact metadata close failed", ::GetLastError());
    complete = false;
  }
  if (complete && ::MoveFileExW(temporary_path.c_str(), metadata_path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    detail = "artifact metadata publication failed: " + windows_error_message(::GetLastError());
    complete = false;
  }
  if (!complete) {
    if (::DeleteFileW(temporary_path.c_str()) == 0) {
      const DWORD cleanup_error = ::GetLastError();
      if (cleanup_error != ERROR_FILE_NOT_FOUND)
        append_windows_cleanup_error("metadata staging cleanup failed", cleanup_error);
    }
    return false;
  }
  return true;
#else
  std::string temporary_template = metadata_path.string() + ".tmp.XXXXXX";
  std::vector<char> temporary_path(temporary_template.begin(), temporary_template.end());
  temporary_path.push_back('\0');
  const int descriptor = ::mkstemp(temporary_path.data());
  if (descriptor < 0) {
    detail =
      "failed to create an exclusive metadata staging file: " + std::string(std::strerror(errno));
    return false;
  }

  bool complete = true;
  std::size_t written = 0U;
  while (written < payload.size()) {
    const ssize_t count = ::write(descriptor, payload.data() + written, payload.size() - written);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      detail = count == 0 ? "artifact metadata write made no progress"
                          : "artifact metadata write failed: " + std::string(std::strerror(errno));
      complete = false;
      break;
    }
    written += static_cast<std::size_t>(count);
  }
  while (complete && ::fsync(descriptor) != 0) {
    if (errno == EINTR)
      continue;
    detail = "artifact metadata flush failed: " + std::string(std::strerror(errno));
    complete = false;
  }
  if (::close(descriptor) != 0) {
    const int close_error = errno;
    if (!detail.empty())
      detail += "; ";
    detail += "artifact metadata close failed: " + std::string(std::strerror(close_error));
    complete = false;
  }
  if (complete && ::rename(temporary_path.data(), metadata_path.c_str()) != 0) {
    detail = "artifact metadata publication failed: " + std::string(std::strerror(errno));
    complete = false;
  }
  if (!complete) {
    if (::unlink(temporary_path.data()) != 0 && errno != ENOENT) {
      const int cleanup_error = errno;
      if (!detail.empty())
        detail += "; ";
      detail += "metadata staging cleanup failed: " + std::string(std::strerror(cleanup_error));
    }
    return false;
  }
  return true;
#endif
}
