#pragma once

#include "artifact_digest.hpp"
#include "artifact_metadata.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nebula::cli {

struct HostedArtifactProtectedInput {
  std::filesystem::path path;
  std::optional<FileDigest> expected_digest;

  HostedArtifactProtectedInput(std::filesystem::path input_path) : path(std::move(input_path)) {}
  HostedArtifactProtectedInput(std::filesystem::path input_path, FileDigest digest)
      : path(std::move(input_path)), expected_digest(std::move(digest)) {}
};

struct HostedArtifactProtectedDirectory {
  std::filesystem::path path;
  DirectoryTreeDigest expected_membership;
};

struct HostedArtifactTransactionPlan {
  std::filesystem::path artifact;
  std::filesystem::path generated_cpp;
  std::optional<std::filesystem::path> generated_header;
  std::optional<std::filesystem::path> import_library;
  std::vector<HostedArtifactProtectedInput> protected_inputs;
  std::vector<HostedArtifactProtectedDirectory> protected_directories;

  HostedArtifactTransactionPlan(std::filesystem::path artifact_path,
                                std::filesystem::path generated_cpp_path,
                                std::optional<std::filesystem::path> generated_header_path,
                                std::optional<std::filesystem::path> import_library_path,
                                std::vector<HostedArtifactProtectedInput> inputs,
                                std::vector<HostedArtifactProtectedDirectory> directories = {})
      : artifact(std::move(artifact_path)), generated_cpp(std::move(generated_cpp_path)),
        generated_header(std::move(generated_header_path)),
        import_library(std::move(import_library_path)), protected_inputs(std::move(inputs)),
        protected_directories(std::move(directories)) {}
};

struct HostedArtifactStagingPaths {
  std::filesystem::path artifact;
  std::filesystem::path generated_cpp;
  std::optional<std::filesystem::path> generated_header;
  std::optional<std::filesystem::path> import_library;
  std::filesystem::path metadata;
};

enum class HostedArtifactTransactionState : std::uint8_t {
  Open,
  Sealed,
  Committed,
  Failed,
  Closed,
};

enum class HostedArtifactTransactionErrorCode : std::uint8_t {
  None,
  InvalidPlan,
  PathConflict,
  UnsafePath,
  Busy,
  Io,
  DurabilityUnavailable,
  InvalidState,
  StagedOutputInvalid,
  ConcurrentModification,
  Metadata,
  Publication,
  RollbackIncomplete,
  CleanupIncomplete,
};

struct HostedArtifactTransactionError {
  HostedArtifactTransactionErrorCode code = HostedArtifactTransactionErrorCode::None;
  std::string operation;
  std::filesystem::path path;
  std::string detail;
};

struct HostedArtifactTransactionResult {
  HostedArtifactTransactionError error;

  [[nodiscard]] bool ok() const noexcept {
    return error.code == HostedArtifactTransactionErrorCode::None;
  }
};

class HostedArtifactTransaction;

struct HostedArtifactTransactionBeginResult {
  std::unique_ptr<HostedArtifactTransaction> transaction;
  HostedArtifactTransactionError error;

  [[nodiscard]] bool ok() const noexcept {
    return transaction != nullptr && error.code == HostedArtifactTransactionErrorCode::None;
  }
};

// Starts an isolated multi-output publication transaction. It exclusively
// creates private staging directories; the caller/compiler must create each
// returned output path for the first time. seal() then adopts the regular files
// by native identity, and later replacement or mutation is rejected. POSIX
// transaction locks are persistent sibling entries: new locks are created as
// private 0600 single-link files, and existing locks are reusable only when
// they already have that shape and are owned by the effective user.
[[nodiscard]] HostedArtifactTransactionBeginResult
begin_hosted_artifact_transaction(const HostedArtifactTransactionPlan &plan);

class HostedArtifactTransaction final {
public:
  HostedArtifactTransaction(const HostedArtifactTransaction &) = delete;
  HostedArtifactTransaction &operator=(const HostedArtifactTransaction &) = delete;
  HostedArtifactTransaction(HostedArtifactTransaction &&) = delete;
  HostedArtifactTransaction &operator=(HostedArtifactTransaction &&) = delete;
  ~HostedArtifactTransaction();

  [[nodiscard]] HostedArtifactTransactionState state() const noexcept;
  [[nodiscard]] const HostedArtifactStagingPaths &staging_paths() const noexcept;
  [[nodiscard]] std::optional<FileDigest> sealed_artifact_digest() const;

  // Adds toolchain/runtime files discovered after the transaction acquired its
  // output locks. Repeated protection of the same unchanged file is idempotent.
  // Every path is revalidated again by commit().
  [[nodiscard]] HostedArtifactTransactionResult
  protect_additional_inputs(const std::vector<HostedArtifactProtectedInput> &inputs);

  // Revalidates every protected file and directory without publishing output.
  // Reuse execution calls this after metadata assessment and immediately
  // before acquiring an executable lease, while output locks are still held.
  [[nodiscard]] HostedArtifactTransactionResult revalidate_protected_inputs() const;

  // Explicitly adopts regular files already written by the trusted caller or
  // resolved compiler so a later failed build can clean only those identities.
  // Unknown files are never implicitly adopted by abort().
  [[nodiscard]] HostedArtifactTransactionResult adopt_existing_staged_outputs_for_cleanup();

  // Freezes and flushes every caller-written staging file, hashes the artifact,
  // and writes canonical version-6 metadata into the transaction-owned sidecar.
  [[nodiscard]] HostedArtifactTransactionResult seal(const ArtifactBuildKey &build_key);

  // Replaces the locked destination set with process-level rollback. Existing
  // files are first backed up by native identity; any ordinary failure triggers
  // reverse-order restoration. This is not a journaled crash/power-loss atomic
  // multi-file commit.
  [[nodiscard]] HostedArtifactTransactionResult commit();

  // Explicitly abandons an open, sealed, or failed transaction. It never
  // removes a path whose native identity was not recorded by this transaction.
  [[nodiscard]] HostedArtifactTransactionResult abort();

  // Removes identity-bound backups/staging state and releases locks after a
  // successful commit. A cleanup uncertainty is returned as infrastructure
  // failure instead of being silently ignored.
  [[nodiscard]] HostedArtifactTransactionResult finish();

private:
  struct Impl;
  explicit HostedArtifactTransaction(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> impl_;

  friend HostedArtifactTransactionBeginResult
  begin_hosted_artifact_transaction(const HostedArtifactTransactionPlan &plan);
};

} // namespace nebula::cli
