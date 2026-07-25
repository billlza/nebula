#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

inline constexpr int kArtifactMetadataVersion = 6;
inline constexpr std::size_t kArtifactMetadataMaxBytes = 4U * 1024U;
inline constexpr std::size_t kArtifactMetadataMaxLineBytes = 256U;

struct ArtifactBuildKey {
  std::string build_inputs_sha256;
  std::string mode;
  std::string profile;
  std::string artifact_kind;
  int compiler_schema_version = 0;
  int cache_schema_version = 0;
  bool strict_region = false;
  bool warnings_as_errors = false;
  bool no_std = false;
  std::string runtime_profile;
  std::string target;
  std::string panic_policy;
};

struct ArtifactContentIdentity {
  std::uint64_t size = 0U;
  std::string sha256;
};

struct ArtifactMetadata {
  ArtifactBuildKey build;
  ArtifactContentIdentity content;
};

enum class ArtifactMetadataErrorCode : std::uint8_t {
  None,
  Missing,
  Symlink,
  NotRegularFile,
  TooLarge,
  Io,
  Unstable,
  InvalidEncoding,
  InvalidLine,
  VersionNotFirst,
  UnsupportedVersion,
  UnknownKey,
  DuplicateKey,
  MissingKey,
  InvalidValue,
};

struct ArtifactMetadataError {
  ArtifactMetadataErrorCode code = ArtifactMetadataErrorCode::None;
  std::size_t line = 0U;
  std::string field;
  std::string detail;
};

struct ArtifactMetadataResult {
  std::optional<ArtifactMetadata> value;
  ArtifactMetadataError error;

  [[nodiscard]] bool ok() const noexcept {
    return value.has_value() && error.code == ArtifactMetadataErrorCode::None;
  }
};

struct ArtifactMetadataSerializationResult {
  std::optional<std::string> payload;
  ArtifactMetadataError error;

  [[nodiscard]] bool ok() const noexcept {
    return payload.has_value() && error.code == ArtifactMetadataErrorCode::None;
  }
};

enum class ArtifactBuildKeyComparisonStatus : std::uint8_t {
  Match,
  Mismatch,
};

struct ArtifactBuildKeyComparison {
  ArtifactBuildKeyComparisonStatus status = ArtifactBuildKeyComparisonStatus::Mismatch;
  std::string field;
};

enum class ArtifactReuseDisposition : std::uint8_t {
  Reusable,
  Rebuild,
  Reject,
};

enum class ArtifactReuseReason : std::uint8_t {
  ContentIdentityObserved,
  ArtifactMissing,
  MetadataMissing,
  UnsupportedMetadataVersion,
  InvalidMetadata,
  BuildKeyMismatch,
  ContentMismatch,
  UnsafeArtifact,
  IoFailure,
};

struct ArtifactReuseAssessment {
  ArtifactReuseDisposition disposition = ArtifactReuseDisposition::Reject;
  ArtifactReuseReason reason = ArtifactReuseReason::IoFailure;
  std::string field;
  std::string detail;
  std::optional<ArtifactContentIdentity> verified_content;
};

[[nodiscard]] std::filesystem::path artifact_metadata_path(const std::filesystem::path &artifact);

[[nodiscard]] ArtifactMetadataResult parse_artifact_metadata(std::string_view payload);
[[nodiscard]] ArtifactMetadataResult read_artifact_metadata(const std::filesystem::path &artifact);
[[nodiscard]] ArtifactMetadataSerializationResult
serialize_artifact_metadata(const ArtifactMetadata &metadata);
[[nodiscard]] ArtifactBuildKeyComparison
compare_artifact_build_keys(const ArtifactBuildKey &actual, const ArtifactBuildKey &expected);
[[nodiscard]] ArtifactReuseAssessment assess_artifact_reuse(const std::filesystem::path &artifact,
                                                            const ArtifactBuildKey &expected);

bool write_artifact_metadata(const std::filesystem::path &artifact,
                             const ArtifactMetadata &metadata, std::string &detail);
