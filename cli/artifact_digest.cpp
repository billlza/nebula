#include "cli/artifact_digest.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nebula::cli {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
  0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
  0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
  0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
  0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
  0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
  0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
  0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
  0xc67178f2U,
};

#if defined(_MSC_VER)
#define NEBULA_SHA256_FORCE_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define NEBULA_SHA256_FORCE_INLINE inline __attribute__((always_inline))
#else
#define NEBULA_SHA256_FORCE_INLINE inline
#endif

// These primitives execute hundreds of times per input block. Keeping them
// inline is important even in diagnostic builds, where provenance hashing must
// not dominate every compiler invocation.
NEBULA_SHA256_FORCE_INLINE constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                                unsigned amount) {
  return (value >> amount) | (value << (32U - amount));
}

NEBULA_SHA256_FORCE_INLINE std::uint32_t read_be32(const std::uint8_t *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

#undef NEBULA_SHA256_FORCE_INLINE

char hex_digit(unsigned value) {
  constexpr char kDigits[] = "0123456789abcdef";
  return kDigits[value & 0x0fU];
}

FileDigestResult digest_error(FileDigestErrorCode code, std::string detail) {
  FileDigestResult result;
  result.error = code;
  result.detail = std::move(detail);
  return result;
}

StableFilePrefixResult stable_prefix_error(FileDigestErrorCode code, std::string detail) {
  StableFilePrefixResult result;
  result.error = code;
  result.detail = std::move(detail);
  return result;
}

DirectoryTreeDigestResult directory_digest_error(FileDigestErrorCode code, std::string detail) {
  DirectoryTreeDigestResult result;
  result.error = code;
  result.detail = std::move(detail);
  return result;
}

DirectoryTreeSnapshotResult directory_snapshot_error(FileDigestErrorCode code, std::string detail) {
  DirectoryTreeSnapshotResult result;
  result.error = code;
  result.detail = std::move(detail);
  return result;
}

struct DirectoryEntryIdentity {
  char kind = 'f';
  std::string relative_path;

  bool operator==(const DirectoryEntryIdentity &) const = default;
};

DirectoryTreeDigestResult enumerate_directory_tree(const std::filesystem::path &root,
                                                   std::vector<DirectoryEntryIdentity> &entries,
                                                   std::size_t max_entries,
                                                   std::size_t max_encoded_path_bytes) {
  namespace fs = std::filesystem;

  std::error_code error;
  const fs::file_status root_status = fs::symlink_status(root, error);
  if (error) {
    return directory_digest_error(error == std::errc::no_such_file_or_directory
                                    ? FileDigestErrorCode::Missing
                                    : FileDigestErrorCode::Io,
                                  "failed to inspect directory tree root: " + error.message());
  }
  if (fs::is_symlink(root_status)) {
    return directory_digest_error(FileDigestErrorCode::Symlink,
                                  "directory tree root is a symbolic link");
  }
  if (!fs::is_directory(root_status)) {
    return directory_digest_error(FileDigestErrorCode::NotRegularFile,
                                  "directory tree root is not a directory");
  }

  entries.clear();
  std::size_t encoded_path_bytes = 0U;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, error);
  const fs::recursive_directory_iterator end;
  while (!error && iterator != end) {
    const fs::path entry_path = iterator->path();
    const fs::file_status status = fs::symlink_status(entry_path, error);
    if (error)
      break;
    if (fs::is_symlink(status)) {
      return directory_digest_error(FileDigestErrorCode::Symlink,
                                    "directory tree contains a symbolic link: " +
                                      entry_path.string());
    }
    const char kind = fs::is_directory(status) ? 'd' : fs::is_regular_file(status) ? 'f' : '\0';
    if (kind == '\0') {
      return directory_digest_error(FileDigestErrorCode::NotRegularFile,
                                    "directory tree contains a special file: " +
                                      entry_path.string());
    }
    const fs::path relative = entry_path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
      return directory_digest_error(FileDigestErrorCode::Io,
                                    "directory tree entry could not be normalized");
    }
    for (const fs::path &component : relative) {
      if (component == "..") {
        return directory_digest_error(FileDigestErrorCode::Io,
                                      "directory tree entry escaped its root");
      }
    }
    std::string encoded = relative.generic_string();
    if (encoded.find('\0') != std::string::npos) {
      return directory_digest_error(FileDigestErrorCode::Io,
                                    "directory tree entry contains a NUL byte");
    }
    if (entries.size() >= max_entries || encoded_path_bytes > max_encoded_path_bytes ||
        encoded.size() > max_encoded_path_bytes - encoded_path_bytes) {
      return directory_digest_error(FileDigestErrorCode::TooLarge,
                                    "directory tree exceeds the bounded membership limit");
    }
    encoded_path_bytes += encoded.size();
    entries.push_back(DirectoryEntryIdentity{kind, std::move(encoded)});
    iterator.increment(error);
  }
  if (error) {
    return directory_digest_error(FileDigestErrorCode::Io,
                                  "failed while enumerating directory tree: " + error.message());
  }
  std::sort(entries.begin(), entries.end(),
            [](const DirectoryEntryIdentity &left, const DirectoryEntryIdentity &right) {
              if (left.relative_path != right.relative_path)
                return left.relative_path < right.relative_path;
              return left.kind < right.kind;
            });
  DirectoryTreeDigestResult result;
  result.value = DirectoryTreeDigest{static_cast<std::uint64_t>(entries.size()), {}};
  return result;
}

void update_u64_be(Sha256Digest &digest, std::uint64_t value) {
  std::array<std::uint8_t, 8U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    const unsigned shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
    bytes[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  digest.update(bytes);
}

void update_length_delimited_text(Sha256Digest &digest, std::string_view text) {
  update_u64_be(digest, static_cast<std::uint64_t>(text.size()));
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  digest.update(std::span<const std::uint8_t>(bytes, text.size()));
}

DirectoryTreeDigest
directory_membership_digest(const std::vector<DirectoryEntryIdentity> &entries) {
  Sha256Digest digest;
  constexpr char domain[] = "nebula-directory-tree-v1";
  const auto *domain_bytes = reinterpret_cast<const std::uint8_t *>(domain);
  digest.update(std::span<const std::uint8_t>(domain_bytes, sizeof(domain)));
  update_u64_be(digest, static_cast<std::uint64_t>(entries.size()));
  for (const DirectoryEntryIdentity &entry : entries) {
    const std::uint8_t kind = static_cast<std::uint8_t>(entry.kind);
    digest.update(std::span<const std::uint8_t>(&kind, 1U));
    update_length_delimited_text(digest, entry.relative_path);
  }
  return DirectoryTreeDigest{static_cast<std::uint64_t>(entries.size()), digest.finish_hex()};
}

struct DirectoryFileDigestPass {
  std::vector<DirectoryTreeFileSnapshot> files;
  FileDigestErrorCode error = FileDigestErrorCode::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return error == FileDigestErrorCode::None; }
};

DirectoryFileDigestPass digest_directory_files(const std::filesystem::path &root,
                                               const std::vector<DirectoryEntryIdentity> &entries,
                                               const DirectoryTreeSnapshotLimits &limits) {
  DirectoryFileDigestPass result;
  result.files.reserve(static_cast<std::size_t>(
    std::count_if(entries.begin(), entries.end(),
                  [](const DirectoryEntryIdentity &entry) { return entry.kind == 'f'; })));
  std::uintmax_t total_bytes = 0U;
  for (const DirectoryEntryIdentity &entry : entries) {
    if (entry.kind != 'f')
      continue;
    const std::uintmax_t remaining_total =
      total_bytes <= limits.max_total_file_bytes ? limits.max_total_file_bytes - total_bytes : 0U;
    const std::uintmax_t file_limit = std::min(limits.max_file_bytes, remaining_total);
    const std::filesystem::path relative(entry.relative_path);
    FileDigestResult digest = sha256_file(root / relative, file_limit);
    if (!digest.ok()) {
      result.error =
        digest.error == FileDigestErrorCode::Missing ? FileDigestErrorCode::Unstable : digest.error;
      result.detail = "could not establish directory-tree content identity for " +
                      relative.generic_string() + ": " + digest.detail;
      return result;
    }
    if (digest.value->size > remaining_total) {
      result.error = FileDigestErrorCode::TooLarge;
      result.detail = "directory tree exceeds the bounded total content limit";
      return result;
    }
    total_bytes += digest.value->size;
    result.files.push_back(DirectoryTreeFileSnapshot{relative, std::move(*digest.value)});
  }
  return result;
}

std::string directory_content_digest(const DirectoryTreeDigest &membership,
                                     const std::vector<DirectoryTreeFileSnapshot> &files) {
  Sha256Digest digest;
  constexpr char domain[] = "nebula-directory-content-v1";
  const auto *domain_bytes = reinterpret_cast<const std::uint8_t *>(domain);
  digest.update(std::span<const std::uint8_t>(domain_bytes, sizeof(domain)));
  update_u64_be(digest, membership.entry_count);
  update_length_delimited_text(digest, membership.sha256);
  update_u64_be(digest, static_cast<std::uint64_t>(files.size()));
  for (const DirectoryTreeFileSnapshot &file : files) {
    update_length_delimited_text(digest, file.relative_path.generic_string());
    update_u64_be(digest, static_cast<std::uint64_t>(file.content.size));
    update_length_delimited_text(digest, file.content.sha256);
  }
  return digest.finish_hex();
}

#if defined(_WIN32)

class ScopedPrefixFile final {
public:
  explicit ScopedPrefixFile(HANDLE handle) noexcept : handle_(handle) {}
  ScopedPrefixFile(const ScopedPrefixFile &) = delete;
  ScopedPrefixFile &operator=(const ScopedPrefixFile &) = delete;

  ~ScopedPrefixFile() {
    if (handle_ != INVALID_HANDLE_VALUE)
      (void)::CloseHandle(handle_);
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  [[nodiscard]] bool close(DWORD &error) noexcept {
    error = ERROR_SUCCESS;
    if (handle_ == INVALID_HANDLE_VALUE)
      return true;
    const HANDLE closing = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    if (::CloseHandle(closing) != 0)
      return true;
    error = ::GetLastError();
    return false;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::string windows_error_message(DWORD error) {
  return std::system_category().message(static_cast<int>(error));
}

bool same_file_state(const BY_HANDLE_FILE_INFORMATION &left,
                     const BY_HANDLE_FILE_INFORMATION &right) {
  return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
         left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow &&
         left.nFileSizeHigh == right.nFileSizeHigh && left.nFileSizeLow == right.nFileSizeLow &&
         left.nNumberOfLinks == right.nNumberOfLinks &&
         left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
         left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime;
}

#else

class ScopedPrefixFile final {
public:
  explicit ScopedPrefixFile(int descriptor) noexcept : descriptor_(descriptor) {}
  ScopedPrefixFile(const ScopedPrefixFile &) = delete;
  ScopedPrefixFile &operator=(const ScopedPrefixFile &) = delete;

  ~ScopedPrefixFile() {
    if (descriptor_ >= 0)
      (void)::close(descriptor_);
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }

  [[nodiscard]] bool close(int &error) noexcept {
    error = 0;
    if (descriptor_ < 0)
      return true;
    const int closing = descriptor_;
    descriptor_ = -1;
    if (::close(closing) == 0)
      return true;
    error = errno;
    return false;
  }

private:
  int descriptor_ = -1;
};

bool same_file_state(const struct stat &left, const struct stat &right) {
  if (left.st_dev != right.st_dev || left.st_ino != right.st_ino ||
      left.st_nlink != right.st_nlink || left.st_size != right.st_size ||
      left.st_mtime != right.st_mtime || left.st_ctime != right.st_ctime) {
    return false;
  }
#if defined(__APPLE__)
  return left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
         left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#elif defined(__linux__)
  return left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#else
  return true;
#endif
}

#endif

} // namespace

void Sha256Digest::process_block(const std::uint8_t *block) {
  std::uint32_t words[64]{};
  for (std::size_t index = 0U; index < 16U; ++index)
    words[index] = read_be32(block + index * 4U);
  for (std::size_t index = 16U; index < 64U; ++index) {
    const std::uint32_t s0 = rotate_right(words[index - 15U], 7U) ^
                             rotate_right(words[index - 15U], 18U) ^ (words[index - 15U] >> 3U);
    const std::uint32_t s1 = rotate_right(words[index - 2U], 17U) ^
                             rotate_right(words[index - 2U], 19U) ^ (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0U; index < 64U; ++index) {
    const std::uint32_t sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[index] + words[index];
    const std::uint32_t sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Digest::update(std::span<const std::uint8_t> bytes) {
  if (finished_)
    throw std::logic_error("cannot update a finalized SHA-256 digest");
  constexpr std::uint64_t kMaxBytes = std::numeric_limits<std::uint64_t>::max() / 8U;
  if (bytes.size() > kMaxBytes - total_bytes_)
    throw std::length_error("SHA-256 input exceeds its 64-bit bit-length field");
  if (bytes.empty())
    return;
  total_bytes_ += static_cast<std::uint64_t>(bytes.size());

  const std::uint8_t *cursor = bytes.data();
  std::size_t remaining = bytes.size();

  if (block_size_ != 0U) {
    const std::size_t count = std::min(block_.size() - block_size_, remaining);
    std::memcpy(block_.data() + block_size_, cursor, count);
    block_size_ += count;
    cursor += count;
    remaining -= count;
    if (block_size_ != block_.size())
      return;
    process_block(block_.data());
    block_size_ = 0U;
  }

  while (remaining >= block_.size()) {
    process_block(cursor);
    cursor += block_.size();
    remaining -= block_.size();
  }

  if (remaining != 0U) {
    std::memcpy(block_.data(), cursor, remaining);
    block_size_ = remaining;
  }
}

std::string Sha256Digest::finish_hex() {
  if (finished_)
    throw std::logic_error("SHA-256 digest was finalized more than once");
  finished_ = true;

  block_[block_size_++] = 0x80U;
  if (block_size_ > 56U) {
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.end(), 0U);
    process_block(block_.data());
    block_size_ = 0U;
  }
  std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_), block_.begin() + 56, 0U);
  const std::uint64_t bit_length = total_bytes_ * 8U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    const unsigned shift = static_cast<unsigned>((7U - index) * 8U);
    block_[56U + index] = static_cast<std::uint8_t>((bit_length >> shift) & 0xffU);
  }
  process_block(block_.data());

  std::string output(64U, '0');
  std::size_t offset = 0U;
  for (const std::uint32_t word : state_) {
    for (int shift = 28; shift >= 0; shift -= 4)
      output[offset++] = hex_digit(word >> static_cast<unsigned>(shift));
  }
  return output;
}

std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  Sha256Digest digest;
  digest.update(bytes);
  return digest.finish_hex();
}

FileDigestResult sha256_file(const std::filesystem::path &path, std::uintmax_t max_bytes) {
#if defined(_WIN32)
  HANDLE file = ::CreateFileW(
    path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return digest_error(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                          ? FileDigestErrorCode::Missing
                          : FileDigestErrorCode::Io,
                        "failed to open artifact for hashing: " + windows_error_message(error));
  }
  const auto close_file = [&file]() {
    if (file == INVALID_HANDLE_VALUE)
      return true;
    const bool closed = ::CloseHandle(file) != 0;
    file = INVALID_HANDLE_VALUE;
    return closed;
  };

  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag_info, sizeof(tag_info)) ==
      0) {
    const DWORD error = ::GetLastError();
    close_file();
    return digest_error(FileDigestErrorCode::Io,
                        "failed to inspect artifact attributes: " + windows_error_message(error));
  }
  if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    close_file();
    return digest_error(FileDigestErrorCode::Symlink,
                        "artifact is a reparse point and cannot be reused");
  }
  if (::GetFileType(file) != FILE_TYPE_DISK) {
    close_file();
    return digest_error(FileDigestErrorCode::NotRegularFile, "artifact handle is not a disk file");
  }
  BY_HANDLE_FILE_INFORMATION before{};
  if (::GetFileInformationByHandle(file, &before) == 0) {
    const DWORD error = ::GetLastError();
    close_file();
    return digest_error(FileDigestErrorCode::Io,
                        "failed to inspect artifact identity: " + windows_error_message(error));
  }
  if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    close_file();
    return digest_error(FileDigestErrorCode::NotRegularFile, "artifact is not a regular file");
  }
  const std::uint64_t expected_size =
    (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
  if (expected_size > max_bytes) {
    close_file();
    return digest_error(FileDigestErrorCode::TooLarge,
                        "artifact exceeds the configured reuse digest limit");
  }
  Sha256Digest digest;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  std::uint64_t total = 0U;
  while (true) {
    DWORD count = 0U;
    if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) == 0) {
      const DWORD error = ::GetLastError();
      close_file();
      return digest_error(FileDigestErrorCode::Io,
                          "failed while reading artifact: " + windows_error_message(error));
    }
    if (count == 0U)
      break;
    if (total > expected_size || count > expected_size - total) {
      close_file();
      return digest_error(FileDigestErrorCode::Unstable, "artifact grew while it was being hashed");
    }
    const std::span<const std::uint8_t> chunk(buffer.data(), count);
    digest.update(chunk);
    total += count;
  }
  BY_HANDLE_FILE_INFORMATION after{};
  if (::GetFileInformationByHandle(file, &after) == 0) {
    const DWORD error = ::GetLastError();
    close_file();
    return digest_error(FileDigestErrorCode::Io,
                        "failed to recheck artifact identity: " + windows_error_message(error));
  }
  if (!close_file()) {
    return digest_error(FileDigestErrorCode::Io, "failed to close artifact after hashing: " +
                                                   windows_error_message(::GetLastError()));
  }
  if (total != expected_size || !same_file_state(before, after)) {
    return digest_error(FileDigestErrorCode::Unstable,
                        "artifact changed while it was being hashed");
  }
  FileDigestResult result;
  result.value = FileDigest{static_cast<std::uintmax_t>(total), digest.finish_hex()};
  return result;
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
    FileDigestErrorCode code = FileDigestErrorCode::Io;
    if (error == ENOENT || error == ENOTDIR)
      code = FileDigestErrorCode::Missing;
#ifdef ELOOP
    else if (error == ELOOP)
      code = FileDigestErrorCode::Symlink;
#endif
    return digest_error(code, "failed to open artifact for hashing: " +
                                std::string(std::strerror(error)));
  }
  const auto close_file = [file]() { return ::close(file); };
  struct stat before{};
  if (::fstat(file, &before) != 0) {
    const int error = errno;
    close_file();
    return digest_error(FileDigestErrorCode::Io, "failed to inspect artifact identity: " +
                                                   std::string(std::strerror(error)));
  }
  if (!S_ISREG(before.st_mode)) {
    close_file();
    return digest_error(FileDigestErrorCode::NotRegularFile, "artifact is not a regular file");
  }
  if (before.st_size < 0) {
    close_file();
    return digest_error(FileDigestErrorCode::Io, "artifact reported a negative byte size");
  }
  const auto expected_size = static_cast<std::uintmax_t>(before.st_size);
  if (expected_size > max_bytes) {
    close_file();
    return digest_error(FileDigestErrorCode::TooLarge,
                        "artifact exceeds the configured reuse digest limit");
  }
  Sha256Digest digest;
  std::array<std::uint8_t, 64U * 1024U> buffer{};
  std::uintmax_t total = 0U;
  while (true) {
    const ssize_t count = ::read(file, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      const int error = errno;
      close_file();
      return digest_error(FileDigestErrorCode::Io,
                          "failed while reading artifact: " + std::string(std::strerror(error)));
    }
    if (count == 0)
      break;
    const auto chunk_size = static_cast<std::uintmax_t>(count);
    if (total > expected_size || chunk_size > expected_size - total) {
      close_file();
      return digest_error(FileDigestErrorCode::Unstable, "artifact grew while it was being hashed");
    }
    const std::span<const std::uint8_t> chunk(buffer.data(), static_cast<std::size_t>(count));
    digest.update(chunk);
    total += chunk_size;
  }
  struct stat after{};
  if (::fstat(file, &after) != 0) {
    const int error = errno;
    close_file();
    return digest_error(FileDigestErrorCode::Io, "failed to recheck artifact identity: " +
                                                   std::string(std::strerror(error)));
  }
  struct stat path_after{};
  const int path_stat_result = ::lstat(path.c_str(), &path_after);
  const int close_result = close_file();
  if (close_result != 0)
    return digest_error(FileDigestErrorCode::Io, "failed to close artifact after hashing: " +
                                                   std::string(std::strerror(errno)));
  if (path_stat_result != 0 || !S_ISREG(path_after.st_mode) || path_after.st_dev != after.st_dev ||
      path_after.st_ino != after.st_ino || total != expected_size ||
      !same_file_state(before, after)) {
    return digest_error(FileDigestErrorCode::Unstable,
                        "artifact changed while it was being hashed");
  }
  FileDigestResult result;
  result.value = FileDigest{total, digest.finish_hex()};
  return result;
#endif
}

StableFilePrefixResult read_stable_file_prefix(const std::filesystem::path &path,
                                               std::size_t requested_bytes,
                                               std::uintmax_t max_bytes) {
#if defined(_WIN32)
  ScopedPrefixFile file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS |
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                      nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return stable_prefix_error(
      error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? FileDigestErrorCode::Missing
                                                                     : FileDigestErrorCode::Io,
      "failed to open file for stable prefix read: " + windows_error_message(error));
  }
  const auto fail = [&file](FileDigestErrorCode code, std::string detail) {
    DWORD close_error = ERROR_SUCCESS;
    if (!file.close(close_error)) {
      detail += "; failed to close file after stable prefix read failure: " +
                windows_error_message(close_error);
    }
    return stable_prefix_error(code, std::move(detail));
  };

  FILE_ATTRIBUTE_TAG_INFO tag_info{};
  if (::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag_info,
                                     sizeof(tag_info)) == 0) {
    return fail(FileDigestErrorCode::Io, "failed to inspect stable-prefix file attributes: " +
                                           windows_error_message(::GetLastError()));
  }
  if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return fail(FileDigestErrorCode::Symlink, "stable-prefix file is a reparse point");
  }

  ::SetLastError(ERROR_SUCCESS);
  const DWORD file_type = ::GetFileType(file.get());
  if (file_type != FILE_TYPE_DISK) {
    const DWORD type_error = ::GetLastError();
    if (file_type == FILE_TYPE_UNKNOWN && type_error != ERROR_SUCCESS) {
      return fail(FileDigestErrorCode::Io, "failed to inspect stable-prefix file type: " +
                                             windows_error_message(type_error));
    }
    return fail(FileDigestErrorCode::NotRegularFile, "stable-prefix handle is not a disk file");
  }

  BY_HANDLE_FILE_INFORMATION before{};
  if (::GetFileInformationByHandle(file.get(), &before) == 0) {
    return fail(FileDigestErrorCode::Io, "failed to inspect stable-prefix file identity: " +
                                           windows_error_message(::GetLastError()));
  }
  if ((before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    return fail(FileDigestErrorCode::NotRegularFile, "stable-prefix file is not a regular file");
  }
  const std::uint64_t expected_size =
    (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
  if (expected_size > max_bytes) {
    return fail(FileDigestErrorCode::TooLarge,
                "stable-prefix file exceeds the configured byte limit");
  }

  StableFilePrefix prefix;
  prefix.file_size = static_cast<std::uintmax_t>(expected_size);
  const std::uintmax_t prefix_size = std::min<std::uintmax_t>(prefix.file_size, requested_bytes);
  if (prefix_size > prefix.bytes.max_size()) {
    return fail(FileDigestErrorCode::TooLarge,
                "requested stable file prefix cannot be represented in memory");
  }
  prefix.bytes.resize(static_cast<std::size_t>(prefix_size));

  std::size_t total = 0U;
  constexpr std::size_t kReadChunkBytes = 64U * 1024U;
  while (total < prefix.bytes.size()) {
    const std::size_t remaining = prefix.bytes.size() - total;
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(remaining, kReadChunkBytes));
    DWORD count = 0U;
    if (::ReadFile(file.get(), prefix.bytes.data() + total, requested, &count, nullptr) == 0) {
      return fail(FileDigestErrorCode::Io, "failed while reading stable file prefix: " +
                                             windows_error_message(::GetLastError()));
    }
    if (count == 0U || count > remaining) {
      return fail(FileDigestErrorCode::Unstable,
                  "file changed while its stable prefix was being read");
    }
    total += count;
  }

  BY_HANDLE_FILE_INFORMATION after{};
  if (::GetFileInformationByHandle(file.get(), &after) == 0) {
    return fail(FileDigestErrorCode::Io, "failed to recheck stable-prefix file identity: " +
                                           windows_error_message(::GetLastError()));
  }
  DWORD close_error = ERROR_SUCCESS;
  if (!file.close(close_error)) {
    return stable_prefix_error(FileDigestErrorCode::Io,
                               "failed to close file after stable prefix read: " +
                                 windows_error_message(close_error));
  }
  if (!same_file_state(before, after)) {
    return stable_prefix_error(FileDigestErrorCode::Unstable,
                               "file changed while its stable prefix was being read");
  }

  StableFilePrefixResult result;
  result.value = std::move(prefix);
  return result;
#else
#ifndef O_NOFOLLOW
  (void)path;
  (void)requested_bytes;
  (void)max_bytes;
  return stable_prefix_error(FileDigestErrorCode::Io,
                             "stable file prefix reads require O_NOFOLLOW support");
#else
  int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  ScopedPrefixFile file(::open(path.c_str(), flags));
  if (file.get() < 0) {
    const int error = errno;
    FileDigestErrorCode code = FileDigestErrorCode::Io;
    if (error == ENOENT || error == ENOTDIR)
      code = FileDigestErrorCode::Missing;
#ifdef ELOOP
    else if (error == ELOOP)
      code = FileDigestErrorCode::Symlink;
#endif
    return stable_prefix_error(code, "failed to open file for stable prefix read: " +
                                       std::string(std::strerror(error)));
  }
  const auto fail = [&file](FileDigestErrorCode code, std::string detail) {
    int close_error = 0;
    if (!file.close(close_error)) {
      detail += "; failed to close file after stable prefix read failure: " +
                std::string(std::strerror(close_error));
    }
    return stable_prefix_error(code, std::move(detail));
  };

  struct stat before{};
  if (::fstat(file.get(), &before) != 0) {
    return fail(FileDigestErrorCode::Io, "failed to inspect stable-prefix file identity: " +
                                           std::string(std::strerror(errno)));
  }
  if (!S_ISREG(before.st_mode)) {
    return fail(FileDigestErrorCode::NotRegularFile, "stable-prefix file is not a regular file");
  }
  if (before.st_size < 0) {
    return fail(FileDigestErrorCode::Io, "stable-prefix file reported a negative byte size");
  }
  const std::uintmax_t expected_size = static_cast<std::uintmax_t>(before.st_size);
  if (expected_size > max_bytes) {
    return fail(FileDigestErrorCode::TooLarge,
                "stable-prefix file exceeds the configured byte limit");
  }

  StableFilePrefix prefix;
  prefix.file_size = expected_size;
  const std::uintmax_t prefix_size = std::min<std::uintmax_t>(expected_size, requested_bytes);
  if (prefix_size > prefix.bytes.max_size()) {
    return fail(FileDigestErrorCode::TooLarge,
                "requested stable file prefix cannot be represented in memory");
  }
  prefix.bytes.resize(static_cast<std::size_t>(prefix_size));

  std::size_t total = 0U;
  constexpr std::size_t kReadChunkBytes = 64U * 1024U;
  while (total < prefix.bytes.size()) {
    const std::size_t remaining = prefix.bytes.size() - total;
    const std::size_t requested = std::min(remaining, kReadChunkBytes);
    const ssize_t count = ::read(file.get(), prefix.bytes.data() + total, requested);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      return fail(FileDigestErrorCode::Io,
                  "failed while reading stable file prefix: " + std::string(std::strerror(errno)));
    }
    if (count == 0 || static_cast<std::size_t>(count) > remaining) {
      return fail(FileDigestErrorCode::Unstable,
                  "file changed while its stable prefix was being read");
    }
    total += static_cast<std::size_t>(count);
  }

  struct stat after{};
  if (::fstat(file.get(), &after) != 0) {
    return fail(FileDigestErrorCode::Io, "failed to recheck stable-prefix file identity: " +
                                           std::string(std::strerror(errno)));
  }
  struct stat path_after{};
  const int path_stat_result = ::lstat(path.c_str(), &path_after);
  const int path_stat_error = path_stat_result == 0 ? 0 : errno;
  int close_error = 0;
  if (!file.close(close_error)) {
    return stable_prefix_error(FileDigestErrorCode::Io,
                               "failed to close file after stable prefix read: " +
                                 std::string(std::strerror(close_error)));
  }
  if (!same_file_state(before, after)) {
    return stable_prefix_error(FileDigestErrorCode::Unstable,
                               "file changed while its stable prefix was being read");
  }
  if (path_stat_result != 0) {
    return stable_prefix_error(FileDigestErrorCode::Unstable,
                               "stable-prefix path changed while it was being read: " +
                                 std::string(std::strerror(path_stat_error)));
  }
  if (!S_ISREG(path_after.st_mode) || path_after.st_dev != after.st_dev ||
      path_after.st_ino != after.st_ino) {
    return stable_prefix_error(FileDigestErrorCode::Unstable,
                               "stable-prefix path no longer names the opened regular file");
  }

  StableFilePrefixResult result;
  result.value = std::move(prefix);
  return result;
#endif
#endif
}

DirectoryTreeSnapshotResult snapshot_directory_tree(const std::filesystem::path &root,
                                                    const DirectoryTreeSnapshotLimits &limits) {
  std::vector<DirectoryEntryIdentity> first;
  DirectoryTreeDigestResult first_result =
    enumerate_directory_tree(root, first, limits.max_entries, limits.max_encoded_path_bytes);
  if (!first_result.ok()) {
    return directory_snapshot_error(first_result.error, std::move(first_result.detail));
  }
  DirectoryFileDigestPass first_files = digest_directory_files(root, first, limits);
  if (!first_files.ok())
    return directory_snapshot_error(first_files.error, std::move(first_files.detail));

  std::vector<DirectoryEntryIdentity> middle;
  DirectoryTreeDigestResult middle_result =
    enumerate_directory_tree(root, middle, limits.max_entries, limits.max_encoded_path_bytes);
  if (!middle_result.ok()) {
    return directory_snapshot_error(middle_result.error, std::move(middle_result.detail));
  }
  if (first != middle) {
    return directory_snapshot_error(
      FileDigestErrorCode::Unstable,
      "directory tree membership changed while contents were observed");
  }
  DirectoryFileDigestPass second_files = digest_directory_files(root, middle, limits);
  if (!second_files.ok())
    return directory_snapshot_error(second_files.error, std::move(second_files.detail));

  std::vector<DirectoryEntryIdentity> final;
  DirectoryTreeDigestResult final_result =
    enumerate_directory_tree(root, final, limits.max_entries, limits.max_encoded_path_bytes);
  if (!final_result.ok()) {
    return directory_snapshot_error(final_result.error, std::move(final_result.detail));
  }
  if (middle != final || first_files.files != second_files.files) {
    return directory_snapshot_error(
      FileDigestErrorCode::Unstable,
      "directory tree membership or file contents changed while the snapshot was observed");
  }

  DirectoryTreeSnapshotResult result;
  DirectoryTreeSnapshot snapshot;
  snapshot.membership = directory_membership_digest(final);
  snapshot.regular_files = std::move(second_files.files);
  snapshot.content_sha256 = directory_content_digest(snapshot.membership, snapshot.regular_files);
  result.value = std::move(snapshot);
  return result;
}

DirectoryTreeDigestResult sha256_directory_tree(const std::filesystem::path &root) {
  const DirectoryTreeSnapshotLimits limits;
  std::vector<DirectoryEntryIdentity> first;
  DirectoryTreeDigestResult first_result =
    enumerate_directory_tree(root, first, limits.max_entries, limits.max_encoded_path_bytes);
  if (!first_result.ok())
    return first_result;
  std::vector<DirectoryEntryIdentity> second;
  DirectoryTreeDigestResult second_result =
    enumerate_directory_tree(root, second, limits.max_entries, limits.max_encoded_path_bytes);
  if (!second_result.ok())
    return second_result;
  if (first != second) {
    return directory_digest_error(FileDigestErrorCode::Unstable,
                                  "directory tree membership changed while it was observed");
  }
  DirectoryTreeDigestResult result;
  result.value = directory_membership_digest(second);
  return result;
}

} // namespace nebula::cli
