#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nebula::cli {

// Reuse verification is deliberately bounded. An artifact larger than this is
// not accepted as a reusable compiler output; callers must rebuild or report
// the explicit size error instead of allocating or hashing without a limit.
inline constexpr std::uintmax_t kMaxReusableArtifactBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;

class Sha256Digest final {
public:
  Sha256Digest() = default;

  Sha256Digest(const Sha256Digest &) = delete;
  Sha256Digest &operator=(const Sha256Digest &) = delete;

  // Throws std::length_error if the SHA-256 bit-length field would overflow,
  // and std::logic_error if called after finish_hex().
  void update(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::string finish_hex();

private:
  void process_block(const std::uint8_t *block);

  std::array<std::uint32_t, 8> state_ = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::uint8_t, 64> block_{};
  std::size_t block_size_ = 0U;
  std::uint64_t total_bytes_ = 0U;
  bool finished_ = false;
};

[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);

enum class FileDigestErrorCode : std::uint8_t {
  None,
  Missing,
  Symlink,
  NotRegularFile,
  TooLarge,
  Io,
  Unstable,
};

struct FileDigest {
  std::uintmax_t size = 0U;
  std::string sha256;

  bool operator==(const FileDigest &) const = default;
};

struct FileDigestResult {
  std::optional<FileDigest> value;
  FileDigestErrorCode error = FileDigestErrorCode::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error == FileDigestErrorCode::None;
  }
};

struct StableFilePrefix {
  std::uintmax_t file_size = 0U;
  std::vector<std::uint8_t> bytes;
};

struct StableFilePrefixResult {
  std::optional<StableFilePrefix> value;
  FileDigestErrorCode error = FileDigestErrorCode::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error == FileDigestErrorCode::None;
  }
};

struct DirectoryTreeDigest {
  std::uint64_t entry_count = 0U;
  std::string sha256;
};

struct DirectoryTreeDigestResult {
  std::optional<DirectoryTreeDigest> value;
  FileDigestErrorCode error = FileDigestErrorCode::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error == FileDigestErrorCode::None;
  }
};

struct DirectoryTreeSnapshotLimits {
  std::size_t max_entries = 1'000'000U;
  std::size_t max_encoded_path_bytes = 256U * 1024U * 1024U;
  std::uintmax_t max_file_bytes = kMaxReusableArtifactBytes;
  std::uintmax_t max_total_file_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct DirectoryTreeFileSnapshot {
  // Always a normalized, non-empty path relative to the snapshot root.
  std::filesystem::path relative_path;
  FileDigest content;

  bool operator==(const DirectoryTreeFileSnapshot &) const = default;
};

struct DirectoryTreeSnapshot {
  DirectoryTreeDigest membership;
  // Binds membership plus every sorted relative file path and exact file
  // digest. It deliberately excludes the absolute root spelling so identical
  // immutable trees have the same content identity at different locations.
  std::string content_sha256;
  std::vector<DirectoryTreeFileSnapshot> regular_files;
};

struct DirectoryTreeSnapshotResult {
  std::optional<DirectoryTreeSnapshot> value;
  FileDigestErrorCode error = FileDigestErrorCode::None;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error == FileDigestErrorCode::None;
  }
};

// Hashes a regular, non-symlink file using fixed-size chunks. The file identity
// and timestamps are checked again after the final read so changes during the
// hashing operation are rejected.
[[nodiscard]] FileDigestResult sha256_file(const std::filesystem::path &path,
                                           std::uintmax_t max_bytes = kMaxReusableArtifactBytes);

// Reads at most requested_bytes from the start of one regular, non-link file.
// max_bytes bounds the complete file, not only the returned prefix. The same
// no-follow handle remains open across the read and before/after identity
// checks; a shorter file returns all of its bytes and its exact full size.
[[nodiscard]] StableFilePrefixResult read_stable_file_prefix(const std::filesystem::path &path,
                                                             std::size_t requested_bytes,
                                                             std::uintmax_t max_bytes);

// Produces a stable, length-delimited identity for every regular file and
// directory name below a non-symlink root. File contents are intentionally
// separate build inputs; this digest closes membership changes such as a newly
// added include candidate. Special files, links, unbounded trees, and a tree
// that changes between two complete traversals are rejected.
[[nodiscard]] DirectoryTreeDigestResult sha256_directory_tree(const std::filesystem::path &root);

// Returns one bounded logical content snapshot. The tree is enumerated before,
// between, and after two complete file-digest passes. Each pass rejects links,
// special files, oversized input, and per-file mutations; the two sorted
// observations must agree exactly. This closes the former API gap where callers
// separately observed membership and contents and could accidentally retain
// only the membership identity.
[[nodiscard]] DirectoryTreeSnapshotResult
snapshot_directory_tree(const std::filesystem::path &root,
                        const DirectoryTreeSnapshotLimits &limits = {});

} // namespace nebula::cli
