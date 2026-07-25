#pragma once

#include "artifact_metadata.hpp"
#include "build_types.hpp"
#include "compiler_execution.hpp"
#include "freestanding_toolchain.hpp"
#include "frontend/diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct FreestandingObjectRequest {
  std::filesystem::path input_path;
  std::filesystem::path generated_source_path;
  std::filesystem::path object_path;
  std::string translation_unit;
  ArtifactBuildKey build_key;
  BuildMode mode = BuildMode::Debug;
};

enum class FreestandingObjectFailure : std::uint8_t {
  None,
  Build,
  Timeout,
  Infrastructure,
};

enum class FreestandingArtifactDisposition : std::uint8_t {
  Absent,
  Committed,
  CleanupIncomplete,
};

struct FreestandingObjectResult {
  std::vector<nebula::frontend::Diagnostic> diagnostics;
  int interrupted_signal = 0;
  FreestandingObjectFailure failure = FreestandingObjectFailure::Build;
  FreestandingArtifactDisposition artifact_disposition = FreestandingArtifactDisposition::Absent;

  [[nodiscard]] bool ok() const {
    return failure == FreestandingObjectFailure::None && diagnostics.empty() &&
           artifact_disposition == FreestandingArtifactDisposition::Committed;
  }

  [[nodiscard]] int exit_code() const {
    switch (failure) {
    case FreestandingObjectFailure::None:
      return 0;
    case FreestandingObjectFailure::Build:
      return 1;
    case FreestandingObjectFailure::Timeout:
      return 124;
    case FreestandingObjectFailure::Infrastructure:
      return 125;
    }
    return 125;
  }
};

class FreestandingCompilerExecutor {
public:
  FreestandingCompilerExecutor() = default;
  FreestandingCompilerExecutor(const FreestandingCompilerExecutor &) = delete;
  FreestandingCompilerExecutor &operator=(const FreestandingCompilerExecutor &) = delete;
  virtual ~FreestandingCompilerExecutor() = default;

  [[nodiscard]] virtual CommandExecutionResult
  execute(const std::vector<std::string> &command, const std::vector<std::string> &environment,
          int timeout_seconds, const CompilerTerminationSignalScope &termination_signals) = 0;
};

FreestandingObjectResult
build_freestanding_object(const FreestandingObjectRequest &request,
                          nebula::cli::ResolvedFreestandingToolchain &toolchain);
FreestandingObjectResult
build_freestanding_object(const FreestandingObjectRequest &request,
                          nebula::cli::ResolvedFreestandingToolchain &toolchain,
                          FreestandingCompilerExecutor &compiler_executor);
