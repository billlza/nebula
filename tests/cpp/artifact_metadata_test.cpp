#include "cli/artifact_digest.hpp"
#include "cli/artifact_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, std::string_view message) {
  if (condition)
    return true;
  std::cerr << "artifact-metadata-test: " << message << '\n';
  return false;
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error);
    if (error)
      return;
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
      const auto candidate = base / ("nebula-artifact-metadata-" + std::to_string(unique_seed_) +
                                     "-" + std::to_string(attempt));
      if (fs::create_directory(candidate, error)) {
        path_ = candidate;
        ++unique_seed_;
        return;
      }
      if (error && error != std::errc::file_exists)
        return;
      error.clear();
    }
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    if (!path_.has_value())
      return;
    std::error_code error;
    fs::remove_all(*path_, error);
    if (error)
      std::cerr << "artifact-metadata-test: cleanup failed: " << error.message() << '\n';
  }

  [[nodiscard]] const std::optional<fs::path> &path() const noexcept { return path_; }

private:
  inline static unsigned long long unique_seed_ = 1U;
  std::optional<fs::path> path_;
};

bool write_binary(const fs::path &path, std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  return !output.fail();
}

ArtifactMetadata sample_metadata(std::string_view artifact_bytes = "abc") {
  ArtifactMetadata metadata;
  metadata.build.build_inputs_sha256 = std::string(64U, '1');
  metadata.build.mode = "debug";
  metadata.build.profile = "fast";
  metadata.build.artifact_kind = "executable";
  metadata.build.compiler_schema_version = 1;
  metadata.build.cache_schema_version = 4;
  metadata.build.strict_region = false;
  metadata.build.warnings_as_errors = false;
  metadata.build.no_std = false;
  metadata.build.runtime_profile = "hosted";
  metadata.build.target = "host";
  metadata.build.panic_policy = "abort";
  metadata.content.size = artifact_bytes.size();
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(artifact_bytes.data());
  metadata.content.sha256 =
    nebula::cli::sha256_hex(std::span<const std::uint8_t>(bytes, artifact_bytes.size()));
  return metadata;
}

std::string replace_once(std::string payload, std::string_view from, std::string_view to) {
  const std::size_t position = payload.find(from);
  if (position != std::string::npos)
    payload.replace(position, from.size(), to);
  return payload;
}

std::string remove_line(std::string payload, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  const std::size_t start = payload.find(prefix);
  if (start == std::string::npos)
    return payload;
  const std::size_t end = payload.find('\n', start);
  payload.erase(start, end - start + 1U);
  return payload;
}

bool expect_parse_error(std::string_view payload, ArtifactMetadataErrorCode code,
                        std::string_view label) {
  const ArtifactMetadataResult result = parse_artifact_metadata(payload);
  return expect(!result.ok(), std::string(label) + " unexpectedly parsed") &&
         expect(result.error.code == code,
                std::string(label) + " returned the wrong structured error code");
}

bool test_sha256_incremental() {
  bool ok = true;
  const std::array<std::pair<std::string_view, std::string_view>, 3> vectors = {{
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"The quick brown fox jumps over the lazy dog",
     "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
  }};
  for (const auto &[input, expected] : vectors) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(input.data());
    ok &= expect(nebula::cli::sha256_hex(std::span<const std::uint8_t>(bytes, input.size())) ==
                   expected,
                 "SHA-256 standard vector mismatch");
  }

  std::string boundary_input(129U, 'x');
  nebula::cli::Sha256Digest incremental;
  const auto *boundary_bytes = reinterpret_cast<const std::uint8_t *>(boundary_input.data());
  incremental.update(std::span<const std::uint8_t>(boundary_bytes, 55U));
  incremental.update({});
  incremental.update(std::span<const std::uint8_t>(boundary_bytes + 55U, 1U));
  incremental.update({});
  incremental.update({});
  incremental.update(std::span<const std::uint8_t>(boundary_bytes + 56U, 8U));
  incremental.update(std::span<const std::uint8_t>(boundary_bytes + 64U, 65U));
  ok &= expect(incremental.finish_hex() == nebula::cli::sha256_hex(std::span<const std::uint8_t>(
                                             boundary_bytes, boundary_input.size())),
               "incremental SHA-256 disagrees across 55/56/64/129-byte boundaries");

  nebula::cli::Sha256Digest empty_updates;
  empty_updates.update({});
  empty_updates.update({});
  ok &= expect(empty_updates.finish_hex() == vectors.front().second,
               "repeated empty SHA-256 updates changed the empty digest");
  return ok;
}

bool test_stable_file_prefix_contract() {
  TemporaryDirectory temporary;
  if (!expect(temporary.path().has_value(), "could not create stable-prefix temporary directory")) {
    return false;
  }

  bool ok = true;
  const fs::path file = *temporary.path() / "prefix.bin";
  constexpr std::string_view payload = "nebula-prefix";
  ok &= expect(write_binary(file, payload), "could not write stable-prefix fixture");
  if (!ok)
    return false;

  const auto bytes_equal = [](const std::vector<std::uint8_t> &bytes, std::string_view expected) {
    return bytes.size() == expected.size() &&
           std::equal(bytes.begin(), bytes.end(), expected.begin(),
                      [](std::uint8_t left, char right) {
                        return left == static_cast<std::uint8_t>(right);
                      });
  };

  const nebula::cli::StableFilePrefixResult short_prefix =
    nebula::cli::read_stable_file_prefix(file, 6U, payload.size());
  ok &= expect(short_prefix.ok(), "stable short prefix read failed");
  if (short_prefix.ok()) {
    ok &= expect(short_prefix.value->file_size == payload.size(),
                 "short prefix read omitted the complete file size");
    ok &= expect(bytes_equal(short_prefix.value->bytes, "nebula"),
                 "short prefix read returned the wrong bytes");
  }

  const nebula::cli::StableFilePrefixResult long_prefix =
    nebula::cli::read_stable_file_prefix(file, payload.size() + 32U, payload.size());
  ok &= expect(long_prefix.ok(), "stable prefix read past EOF failed");
  if (long_prefix.ok()) {
    ok &= expect(long_prefix.value->file_size == payload.size(),
                 "long prefix read omitted the complete file size");
    ok &= expect(bytes_equal(long_prefix.value->bytes, payload),
                 "long prefix read did not stop at the stable EOF");
  }

  const nebula::cli::StableFilePrefixResult oversized =
    nebula::cli::read_stable_file_prefix(file, 1U, payload.size() - 1U);
  ok &= expect(!oversized.ok() && oversized.error == nebula::cli::FileDigestErrorCode::TooLarge &&
                 !oversized.detail.empty(),
               "stable prefix read did not reject a file above its full-file bound");

  const fs::path directory = *temporary.path() / "directory";
  std::error_code error;
  ok &= expect(fs::create_directory(directory, error) && !error,
               "could not create stable-prefix directory fixture");
  if (!error) {
    const nebula::cli::StableFilePrefixResult non_regular =
      nebula::cli::read_stable_file_prefix(directory, 1U, 1U);
    ok &= expect(!non_regular.ok() &&
                   non_regular.error == nebula::cli::FileDigestErrorCode::NotRegularFile,
                 "stable prefix read accepted a directory");
  }

  const fs::path link = *temporary.path() / "prefix-link.bin";
  error.clear();
  fs::create_symlink(file.filename(), link, error);
  if (!error) {
    const nebula::cli::StableFilePrefixResult symlink =
      nebula::cli::read_stable_file_prefix(link, 6U, payload.size());
    ok &= expect(!symlink.ok() && symlink.error == nebula::cli::FileDigestErrorCode::Symlink,
                 "stable prefix read followed a symbolic link or reparse point");
  }
  return ok;
}

bool test_schema() {
  bool ok = true;
  const ArtifactMetadata metadata = sample_metadata();
  const ArtifactMetadataSerializationResult serialized = serialize_artifact_metadata(metadata);
  ok &= expect(serialized.ok(), "valid metadata did not serialize");
  if (!serialized.ok())
    return false;
  const std::string &canonical = *serialized.payload;
  const std::string expected_prefix = "version=6\n"
                                      "build_inputs_sha256=" +
                                      std::string(64U, '1') +
                                      "\n"
                                      "mode=debug\n"
                                      "profile=fast\n"
                                      "artifact_kind=executable\n";
  ok &= expect(canonical.starts_with(expected_prefix),
               "serializer did not emit the canonical key order");
  const ArtifactMetadataResult parsed = parse_artifact_metadata(canonical);
  ok &= expect(parsed.ok(), "canonical metadata did not parse");
  if (parsed.ok()) {
    ok &= expect(compare_artifact_build_keys(parsed.value->build, metadata.build).status ==
                   ArtifactBuildKeyComparisonStatus::Match,
                 "roundtrip changed the build key");
    ok &= expect(parsed.value->content.size == metadata.content.size &&
                   parsed.value->content.sha256 == metadata.content.sha256,
                 "roundtrip changed the content identity");
  }

  ok &= expect_parse_error(replace_once(canonical, "version=6", "version=5"),
                           ArtifactMetadataErrorCode::UnsupportedVersion, "v5 metadata");
  ok &= expect_parse_error("mode=debug\n" + canonical, ArtifactMetadataErrorCode::VersionNotFirst,
                           "late version");
  ok &= expect_parse_error(canonical + "future_key=1\n", ArtifactMetadataErrorCode::UnknownKey,
                           "unknown key");
  ok &= expect_parse_error(canonical + "mode=debug\n", ArtifactMetadataErrorCode::DuplicateKey,
                           "duplicate key");
  ok &= expect_parse_error(remove_line(canonical, "target"), ArtifactMetadataErrorCode::MissingKey,
                           "missing key");
  ok &= expect_parse_error(replace_once(canonical, "target=host", "targethost"),
                           ArtifactMetadataErrorCode::InvalidLine, "missing separator");
  ok &= expect_parse_error(replace_once(canonical, "target=host", "target=host=extra"),
                           ArtifactMetadataErrorCode::InvalidLine, "multiple separators");
  ok &= expect_parse_error(replace_once(canonical, "mode=debug\n", "mode=debug\n\n"),
                           ArtifactMetadataErrorCode::InvalidLine, "empty line");
  ok &= expect_parse_error(canonical.substr(0U, canonical.size() - 1U),
                           ArtifactMetadataErrorCode::InvalidLine, "missing final LF");
  ok &= expect_parse_error(replace_once(canonical, "mode=debug", "mode=de\tbug"),
                           ArtifactMetadataErrorCode::InvalidEncoding, "TAB encoding");
  ok &= expect_parse_error(replace_once(canonical, "mode=debug\n", "mode=debug\r\n"),
                           ArtifactMetadataErrorCode::InvalidEncoding, "CRLF encoding");
  std::string nul = canonical;
  nul[10U] = '\0';
  ok &= expect_parse_error(nul, ArtifactMetadataErrorCode::InvalidEncoding, "NUL encoding");
  std::string non_ascii = canonical;
  non_ascii[10U] = static_cast<char>(0x80U);
  ok &=
    expect_parse_error(non_ascii, ArtifactMetadataErrorCode::InvalidEncoding, "non-ASCII encoding");
  ok &= expect_parse_error(std::string(kArtifactMetadataMaxBytes + 1U, 'x'),
                           ArtifactMetadataErrorCode::TooLarge, "oversized payload");
  ok &= expect_parse_error("version=6\n" + std::string(257U, 'x') + "=1\n",
                           ArtifactMetadataErrorCode::TooLarge, "oversized line");

  for (const auto &[from, to, label] : std::array<std::array<std::string_view, 3>, 6>{{
         {{"strict_region=0", "strict_region=false", "noncanonical bool"}},
         {{"compiler_schema_version=1", "compiler_schema_version=01", "leading-zero int"}},
         {{"cache_schema_version=4", "cache_schema_version=4x", "trailing int text"}},
         {{"artifact_size=3", "artifact_size=0", "zero size"}},
         {{"mode=debug", "mode=Debug", "invalid enum"}},
         {{"target=host", "target=host/path", "invalid target"}},
       }}) {
    ok &= expect_parse_error(replace_once(canonical, from, to),
                             ArtifactMetadataErrorCode::InvalidValue, label);
  }
  ok &= expect_parse_error(replace_once(canonical, std::string(64U, '1'), std::string(63U, '1')),
                           ArtifactMetadataErrorCode::InvalidValue, "short build digest");
  ok &= expect_parse_error(replace_once(canonical, std::string(64U, '1'), std::string(64U, 'A')),
                           ArtifactMetadataErrorCode::InvalidValue, "uppercase build digest");
  ok &= expect_parse_error(replace_once(canonical, metadata.content.sha256, std::string(64U, 'g')),
                           ArtifactMetadataErrorCode::InvalidValue, "nonhex content digest");

  ArtifactMetadata invalid = metadata;
  invalid.build.runtime_profile = "system";
  const ArtifactMetadataSerializationResult invalid_system = serialize_artifact_metadata(invalid);
  ok &= expect(!invalid_system.ok() &&
                 invalid_system.error.code == ArtifactMetadataErrorCode::InvalidValue,
               "serializer accepted a system artifact without strict/no_std invariants");
  invalid = metadata;
  invalid.build.artifact_kind = "freestanding-object";
  const ArtifactMetadataSerializationResult invalid_freestanding =
    serialize_artifact_metadata(invalid);
  ok &=
    expect(!invalid_freestanding.ok(), "serializer accepted an inconsistent freestanding artifact");
  invalid = metadata;
  invalid.build.no_std = true;
  const ArtifactMetadataSerializationResult invalid_no_std = serialize_artifact_metadata(invalid);
  ok &= expect(!invalid_no_std.ok(), "serializer accepted no_std without strict_region");
  return ok;
}

bool test_file_and_reuse_contract() {
  TemporaryDirectory temporary;
  if (!expect(temporary.path().has_value(), "could not create temporary directory"))
    return false;
  bool ok = true;
  const fs::path root = *temporary.path();
  const fs::path artifact = root / "sample.out";
  ok &= expect(write_binary(artifact, "abc"), "could not write artifact fixture");
  const ArtifactMetadata metadata = sample_metadata();
  std::string detail;
  ok &= expect(write_artifact_metadata(artifact, metadata, detail),
               detail.empty() ? "could not write metadata fixture" : detail);

  const ArtifactMetadataResult read = read_artifact_metadata(artifact);
  ok &= expect(read.ok(), "safe regular metadata was not readable");
  ArtifactReuseAssessment assessment = assess_artifact_reuse(artifact, metadata.build);
  ok &= expect(assessment.disposition == ArtifactReuseDisposition::Reusable &&
                 assessment.reason == ArtifactReuseReason::ContentIdentityObserved,
               "matching build key and content identity were not observed");

  ArtifactBuildKey changed_key = metadata.build;
  changed_key.mode = "release";
  assessment = assess_artifact_reuse(artifact, changed_key);
  ok &= expect(assessment.disposition == ArtifactReuseDisposition::Rebuild &&
                 assessment.reason == ArtifactReuseReason::BuildKeyMismatch &&
                 assessment.field == "mode",
               "build-key mismatch did not produce a field-specific rebuild decision");
  ok &= expect(write_binary(artifact, "abd"), "could not mutate artifact fixture");
  assessment = assess_artifact_reuse(artifact, metadata.build);
  ok &= expect(assessment.disposition == ArtifactReuseDisposition::Rebuild &&
                 assessment.reason == ArtifactReuseReason::ContentMismatch,
               "same-size content mutation was incorrectly reusable");

  const nebula::cli::FileDigestResult limited = nebula::cli::sha256_file(artifact, 2U);
  ok &= expect(!limited.ok() && limited.error == nebula::cli::FileDigestErrorCode::TooLarge,
               "explicit file digest bound did not reject an oversized artifact");
  const nebula::cli::FileDigestResult file_digest = nebula::cli::sha256_file(artifact);
  ok &= expect(file_digest.ok() && file_digest.value->size == 3U,
               "streaming file digest did not report the exact byte count");
  const fs::path empty_file = root / "empty.bin";
  ok &= expect(write_binary(empty_file, {}), "could not write empty digest fixture");
  const nebula::cli::FileDigestResult empty_digest = nebula::cli::sha256_file(empty_file);
  ok &= expect(empty_digest.ok() && empty_digest.value->size == 0U &&
                 empty_digest.value->sha256 ==
                   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "generic file digest did not accept the empty SHA-256 domain value");

  const ArtifactMetadataResult missing = read_artifact_metadata(root / "missing.out");
  ok &= expect(!missing.ok() && missing.error.code == ArtifactMetadataErrorCode::Missing,
               "missing metadata did not return Missing");

  const fs::path directory_artifact = root / "directory.out";
  fs::create_directory(artifact_metadata_path(directory_artifact));
  const ArtifactMetadataResult directory = read_artifact_metadata(directory_artifact);
  ok &= expect(!directory.ok() && directory.error.code == ArtifactMetadataErrorCode::NotRegularFile,
               "metadata directory was not rejected as non-regular");

  const fs::path invalid_artifact = root / "missing-parent" / "invalid.out";
  ArtifactMetadata invalid = metadata;
  invalid.content.size = 0U;
  detail.clear();
  ok &= expect(!write_artifact_metadata(invalid_artifact, invalid, detail) &&
                 detail.find("artifact_size") != std::string::npos,
               "writer performed I/O before rejecting an invalid schema value");

  const fs::path symlink_artifact = root / "symlink.out";
  const fs::path symlink_metadata = artifact_metadata_path(symlink_artifact);
  std::error_code symlink_error;
  fs::create_symlink(artifact_metadata_path(artifact), symlink_metadata, symlink_error);
  if (!symlink_error) {
    const ArtifactMetadataResult symlink = read_artifact_metadata(symlink_artifact);
    ok &= expect(!symlink.ok() && symlink.error.code == ArtifactMetadataErrorCode::Symlink,
                 "metadata symlink was not rejected");
  }

#if !defined(_WIN32)
  const fs::path fifo_artifact = root / "fifo.out";
  const fs::path fifo_metadata = artifact_metadata_path(fifo_artifact);
  if (::mkfifo(fifo_metadata.c_str(), 0600) == 0) {
    const ArtifactMetadataResult fifo = read_artifact_metadata(fifo_artifact);
    ok &= expect(!fifo.ok() && fifo.error.code == ArtifactMetadataErrorCode::NotRegularFile,
                 "metadata FIFO was not rejected without blocking");
  }
#endif
  return ok;
}

bool test_directory_content_snapshot_contract() {
  TemporaryDirectory temporary;
  if (!expect(temporary.path().has_value(),
              "could not create directory-snapshot temporary directory")) {
    return false;
  }
  bool ok = true;
  const fs::path first_root = *temporary.path() / "tree-a";
  const fs::path second_root = *temporary.path() / "tree-b";
  std::error_code error;
  fs::create_directories(first_root / "nested", error);
  ok &= expect(!error, "could not create first directory-snapshot tree");
  error.clear();
  fs::create_directories(second_root / "nested", error);
  ok &= expect(!error, "could not create second directory-snapshot tree");
  ok &= expect(write_binary(first_root / "a.txt", "abc") &&
                 write_binary(first_root / "nested" / "b.txt", "xyz") &&
                 write_binary(second_root / "a.txt", "abc") &&
                 write_binary(second_root / "nested" / "b.txt", "xyz"),
               "could not write directory-snapshot fixtures");
  if (!ok)
    return false;

  const nebula::cli::DirectoryTreeSnapshotResult first =
    nebula::cli::snapshot_directory_tree(first_root);
  const nebula::cli::DirectoryTreeSnapshotResult relocated =
    nebula::cli::snapshot_directory_tree(second_root);
  ok &= expect(first.ok() && relocated.ok(),
               "stable regular directory tree did not produce a content snapshot");
  if (!first.ok() || !relocated.ok())
    return false;
  ok &= expect(first.value->membership.entry_count == 3U,
               "directory snapshot did not count directories and files exactly");
  ok &= expect(first.value->regular_files.size() == 2U &&
                 first.value->regular_files[0].relative_path.generic_string() == "a.txt" &&
                 first.value->regular_files[1].relative_path.generic_string() == "nested/b.txt",
               "directory snapshot did not return sorted root-relative file identities");
  ok &= expect(first.value->regular_files[0].content.size == 3U &&
                 first.value->regular_files[0].content.sha256 ==
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "directory snapshot did not retain the exact per-file digest");
  ok &= expect(first.value->membership.sha256 == relocated.value->membership.sha256 &&
                 first.value->content_sha256 == relocated.value->content_sha256,
               "directory snapshot identity unexpectedly depended on its absolute root");

  ok &= expect(write_binary(first_root / "a.txt", "abd"),
               "could not perform same-size directory content mutation");
  const nebula::cli::DirectoryTreeSnapshotResult content_changed =
    nebula::cli::snapshot_directory_tree(first_root);
  ok &= expect(content_changed.ok(), "mutated stable tree could not be snapshotted");
  if (content_changed.ok()) {
    ok &= expect(content_changed.value->membership.sha256 == first.value->membership.sha256,
                 "same-membership content mutation changed the membership identity");
    ok &= expect(content_changed.value->content_sha256 != first.value->content_sha256,
                 "same-size file mutation did not change the directory content identity");
  }

  ok &= expect(write_binary(first_root / "new.txt", "new"),
               "could not add directory membership fixture");
  const nebula::cli::DirectoryTreeSnapshotResult membership_changed =
    nebula::cli::snapshot_directory_tree(first_root);
  ok &= expect(membership_changed.ok() &&
                 membership_changed.value->membership.sha256 != first.value->membership.sha256,
               "added file did not change the directory membership identity");

  nebula::cli::DirectoryTreeSnapshotLimits limits;
  limits.max_entries = 2U;
  const nebula::cli::DirectoryTreeSnapshotResult too_many_entries =
    nebula::cli::snapshot_directory_tree(second_root, limits);
  ok &= expect(!too_many_entries.ok() &&
                 too_many_entries.error == nebula::cli::FileDigestErrorCode::TooLarge,
               "directory snapshot entry bound did not fail closed");
  limits = {};
  limits.max_total_file_bytes = 5U;
  const nebula::cli::DirectoryTreeSnapshotResult too_many_bytes =
    nebula::cli::snapshot_directory_tree(second_root, limits);
  ok &= expect(!too_many_bytes.ok() &&
                 too_many_bytes.error == nebula::cli::FileDigestErrorCode::TooLarge,
               "directory snapshot aggregate byte bound did not fail closed");
  limits = {};
  limits.max_file_bytes = 2U;
  const nebula::cli::DirectoryTreeSnapshotResult oversized_file =
    nebula::cli::snapshot_directory_tree(second_root, limits);
  ok &= expect(!oversized_file.ok() &&
                 oversized_file.error == nebula::cli::FileDigestErrorCode::TooLarge,
               "directory snapshot per-file byte bound did not fail closed");

  const fs::path link = second_root / "link.txt";
  error.clear();
  fs::create_symlink("a.txt", link, error);
  if (!error) {
    const nebula::cli::DirectoryTreeSnapshotResult symlink =
      nebula::cli::snapshot_directory_tree(second_root);
    ok &= expect(!symlink.ok() && symlink.error == nebula::cli::FileDigestErrorCode::Symlink,
                 "directory snapshot accepted a symbolic-link member");
    error.clear();
    fs::remove(link, error);
    ok &= expect(!error, "could not remove directory-snapshot symlink fixture");
  }

#if !defined(_WIN32)
  const fs::path fifo = second_root / "pipe";
  if (::mkfifo(fifo.c_str(), 0600) == 0) {
    const nebula::cli::DirectoryTreeSnapshotResult special =
      nebula::cli::snapshot_directory_tree(second_root);
    ok &= expect(!special.ok() && special.error == nebula::cli::FileDigestErrorCode::NotRegularFile,
                 "directory snapshot accepted a special-file member");
    ok &= expect(::unlink(fifo.c_str()) == 0, "could not remove directory-snapshot FIFO fixture");
  }
#endif
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= test_sha256_incremental();
  ok &= test_stable_file_prefix_contract();
  ok &= test_schema();
  ok &= test_file_and_reuse_contract();
  ok &= test_directory_content_snapshot_contract();
  return ok ? 0 : 1;
}
