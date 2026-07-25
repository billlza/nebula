#pragma once

#include "artifact_digest.hpp"
#include "host_process.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nebula::cli {

enum class VerifiedExecutableLeaseErrorCode : std::uint8_t {
  None,
  InvalidPath,
  UnsafePath,
  TooLarge,
  Io,
  ContentMismatch,
  ConcurrentModification,
  InvalidState,
  CleanupIncomplete,
};

struct VerifiedExecutableLeaseError {
  VerifiedExecutableLeaseErrorCode code = VerifiedExecutableLeaseErrorCode::None;
  std::filesystem::path path;
  std::string operation;
  std::string detail;
};

enum class VerifiedExecutableLeaseCleanupDisposition : std::uint8_t {
  Complete,
  Incomplete,
};

struct VerifiedExecutableLeaseResult {
  VerifiedExecutableLeaseError error;
  VerifiedExecutableLeaseCleanupDisposition cleanup_disposition =
    VerifiedExecutableLeaseCleanupDisposition::Complete;

  [[nodiscard]] bool ok() const noexcept {
    return error.code == VerifiedExecutableLeaseErrorCode::None;
  }

  [[nodiscard]] bool owned_cleanup_complete() const noexcept {
    return cleanup_disposition == VerifiedExecutableLeaseCleanupDisposition::Complete;
  }
};

class VerifiedExecutableLease;

struct VerifiedExecutableLeaseBeginResult {
  std::unique_ptr<VerifiedExecutableLease> lease;
  VerifiedExecutableLeaseError error;

  [[nodiscard]] bool ok() const noexcept {
    return lease != nullptr && error.code == VerifiedExecutableLeaseErrorCode::None;
  }
};

// Copies one regular executable through an open native handle into an
// exclusive, owner-private file in the same directory. The copy is hashed
// while it is read, checked against expected_content when supplied, and kept
// open through a read-only native handle until cleanup. Keeping the copy beside
// the public artifact preserves loader-relative directory semantics while
// decoupling execution from later atomic replacement of the public path. On
// POSIX, owner-private mode and path checks assume processes running as the
// same effective UID are inside the caller's trust boundary; this is not an
// immutable-file guarantee against a hostile same-UID process.
[[nodiscard]] VerifiedExecutableLeaseBeginResult
begin_verified_executable_lease(const std::filesystem::path &public_artifact,
                                const std::optional<FileDigest> &expected_content = std::nullopt);

class VerifiedExecutableLease final {
public:
  VerifiedExecutableLease(const VerifiedExecutableLease &) = delete;
  VerifiedExecutableLease &operator=(const VerifiedExecutableLease &) = delete;
  VerifiedExecutableLease(VerifiedExecutableLease &&) = delete;
  VerifiedExecutableLease &operator=(VerifiedExecutableLease &&) = delete;
  ~VerifiedExecutableLease() noexcept;

  [[nodiscard]] const std::filesystem::path &public_path() const noexcept;
  [[nodiscard]] const std::filesystem::path &execution_path() const noexcept;
  [[nodiscard]] const FileDigest &content() const noexcept;
  [[nodiscard]] bool active() const noexcept;

  // Revalidates the private directory entry and the native object retained by
  // this lease. This deliberately does not re-open the public path: public
  // replacement must not change which verified bytes are executed.
  [[nodiscard]] bool revalidate(std::string &detail) const;

  // Executes the private copy directly. arguments[0] remains the caller's
  // logical public argv[0]; it is intentionally independent of execution_path.
  [[nodiscard]] HostProcessResult execute(const std::vector<std::string> &arguments) const;
  // Applies an explicit process policy while still forcing executable_path to
  // the lease's private copy. The caller controls logical argv[0], streams,
  // environment, timeout, and signal containment, but cannot redirect the
  // verified execution identity.
  [[nodiscard]] HostProcessResult execute_request(HostProcessRequest request) const;

  // Removes the directory entry only after it still matches the native object
  // created by this lease. A replacement observed at the lease path is
  // preserved and reported. Failed cleanup may be retried. POSIX unlink does
  // not provide an fd-only primitive, so the same-UID trust boundary above
  // also applies to the final identity-check/unlink interval.
  [[nodiscard]] VerifiedExecutableLeaseResult cleanup();

private:
  struct Impl;
  explicit VerifiedExecutableLease(std::unique_ptr<Impl> implementation);

  std::unique_ptr<Impl> impl_;

  friend VerifiedExecutableLeaseBeginResult
  begin_verified_executable_lease(const std::filesystem::path &public_artifact,
                                  const std::optional<FileDigest> &expected_content);
};

} // namespace nebula::cli
