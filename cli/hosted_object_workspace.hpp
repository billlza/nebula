#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace nebula::cli {

struct HostedObjectWorkspaceCleanupResult {
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return detail.empty(); }
};

struct HostedObjectWorkspaceCreationResult;

class HostedObjectWorkspace final {
public:
  HostedObjectWorkspace(const HostedObjectWorkspace &) = delete;
  HostedObjectWorkspace &operator=(const HostedObjectWorkspace &) = delete;
  HostedObjectWorkspace(HostedObjectWorkspace &&) noexcept;
  HostedObjectWorkspace &operator=(HostedObjectWorkspace &&) = delete;
  ~HostedObjectWorkspace();

  [[nodiscard]] const std::filesystem::path &path() const noexcept;

  // Removes only the unique directory whose native identity was captured at
  // creation. A replaced path is reported and never removed. Recoverable
  // failures retain the identity binding so cleanup can be retried.
  [[nodiscard]] HostedObjectWorkspaceCleanupResult cleanup();

private:
  struct Impl;

  explicit HostedObjectWorkspace(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> impl_;

  friend struct HostedObjectWorkspaceCreationResult;
  friend HostedObjectWorkspaceCreationResult
  create_hosted_object_workspace(const std::filesystem::path &parent);
};

struct HostedObjectWorkspaceCreationResult {
  std::optional<HostedObjectWorkspace> workspace;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return workspace.has_value() && detail.empty(); }
};

// Atomically creates a new, call-owned workspace below parent. Existing names
// are never adopted or removed.
[[nodiscard]] HostedObjectWorkspaceCreationResult
create_hosted_object_workspace(const std::filesystem::path &parent);

} // namespace nebula::cli
