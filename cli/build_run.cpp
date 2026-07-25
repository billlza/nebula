#include "cli_shared.hpp"
#include "artifact_digest.hpp"
#include "artifact_metadata.hpp"
#include "hosted_artifact_transaction.hpp"
#include "hosted_object_workspace.hpp"
#include "log_value.hpp"
#include "project.hpp"
#include "termination_signal.hpp"
#include "verified_executable_lease.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

#include "codegen/backend.hpp"
#include "codegen/freestanding_cpp_emitter.hpp"
#include "freestanding_object.hpp"

namespace {
struct CacheReportScope {
  const CliOptions &opt;
  const CompilePipelineCacheStats before;

  explicit CacheReportScope(const CliOptions &options)
      : opt(options), before(get_compile_pipeline_cache_stats()) {}

  ~CacheReportScope() { emit_cache_report(opt, before, std::cerr); }
};
} // namespace

namespace {

static std::string sanitize_artifact_stem(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? std::string("nebula_artifact") : out;
}

static std::string default_header_stem(const LoadedCompileInput &loaded,
                                       const fs::path &effective_file) {
  const std::string base =
    !loaded.project_name.empty() ? loaded.project_name : effective_file.stem().string();
  return sanitize_artifact_stem(base);
}

static fs::path default_build_artifact_path(const LoadedCompileInput &loaded,
                                            const fs::path &effective_file, const CliOptions &opt) {
  if (opt.out_path.has_value())
    return *opt.out_path;
  const std::string stem = default_header_stem(loaded, effective_file);
  switch (opt.artifact_kind) {
  case BuildArtifactKind::Executable:
    return opt.out_dir / (effective_file.stem().string() + ".out");
  case BuildArtifactKind::StaticLib:
    return opt.out_dir / ("lib" + stem + ".a");
  case BuildArtifactKind::SharedLib:
#if defined(_WIN32)
    return opt.out_dir / (stem + ".dll");
#elif defined(__APPLE__)
    return opt.out_dir / ("lib" + stem + ".dylib");
#else
    return opt.out_dir / ("lib" + stem + ".so");
#endif
  case BuildArtifactKind::FreestandingObject:
    return opt.out_dir / (effective_file.stem().string() + ".o");
  }
  return opt.out_dir / (effective_file.stem().string() + ".out");
}

static fs::path header_output_path_for(const LoadedCompileInput &loaded,
                                       const fs::path &effective_file,
                                       const fs::path &artifact_path) {
  return artifact_path.parent_path() / (default_header_stem(loaded, effective_file) + ".h");
}

static std::optional<ArtifactBuildKey> derive_build_key_or_emit(
  const CliOptions &opt, AnalysisProfile resolved_profile, const std::string &source_graph,
  nebula::frontend::DiagnosticStage stage,
  const nebula::cli::ResolvedHostedToolchain *hosted_toolchain,
  const RuntimeHeaderIdentityResult *runtime_headers = nullptr,
  const nebula::cli::HostedNativeDependencySnapshot *native_dependencies = nullptr,
  const nebula::cli::ResolvedFreestandingToolchain *freestanding_toolchain = nullptr) {
  ArtifactBuildKeyResult result;
  if (opt.artifact_kind == BuildArtifactKind::FreestandingObject &&
      freestanding_toolchain != nullptr) {
    result = derive_freestanding_artifact_build_key_for(
      opt, resolved_profile, hash_source(source_graph), *freestanding_toolchain);
  } else if (opt.artifact_kind == BuildArtifactKind::FreestandingObject) {
    result.detail = "freestanding artifact provenance requires one resolved immutable toolchain";
  } else if (hosted_toolchain != nullptr && runtime_headers != nullptr &&
             native_dependencies != nullptr) {
    result =
      derive_artifact_build_key_for(opt, resolved_profile, hash_source(source_graph),
                                    *hosted_toolchain, *runtime_headers, *native_dependencies);
  } else {
    result.detail = "hosted artifact provenance requires one resolved immutable toolchain";
  }
  if (result.ok())
    return std::move(*result.value);
  auto diagnostic = make_cli_diag(
    nebula::frontend::Severity::Error, "NBL-CLI-METADATA-IDENTITY",
    "failed to derive complete artifact build provenance", stage,
    nebula::frontend::DiagnosticRisk::High, result.detail,
    "Nebula refuses to publish or reuse an artifact with incomplete source, runtime, host, or "
    "toolchain identity",
    {"verify the runtime header installation and configured C++ compiler",
     "ensure the compiler supports a bounded --version query"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return std::nullopt;
}

struct NativeDependencySnapshotResult {
  std::optional<nebula::cli::HostedNativeDependencySnapshot> snapshot;
  int exit_code = 125;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept {
    return snapshot.has_value() && exit_code == 0 && detail.empty();
  }
};

static NativeDependencySnapshotResult
discover_native_dependencies(const CliOptions &opt,
                             const nebula::cli::ResolvedHostedToolchain &toolchain,
                             const LoadedCompileInput &loaded, BuildArtifactKind artifact_kind,
                             const fs::path &object_parent) {
  nebula::cli::HostedObjectWorkspaceCreationResult workspace_creation =
    nebula::cli::create_hosted_object_workspace(object_parent);
  if (!workspace_creation.ok()) {
    return {std::nullopt, 125,
            "could not create private native-dependency workspace: " + workspace_creation.detail};
  }
  nebula::cli::HostedObjectWorkspace workspace = std::move(*workspace_creation.workspace);
  const fs::path generated_probe = workspace.path() / "generated_dependency_probe.cpp";
  std::string generated_probe_source;
  for (const std::string_view include : nebula::codegen::hosted_cpp_translation_unit_includes()) {
    generated_probe_source += include;
    generated_probe_source.push_back('\n');
  }
  if (!write_text_file(generated_probe, generated_probe_source)) {
    const nebula::cli::HostedObjectWorkspaceCleanupResult cleaned = workspace.cleanup();
    std::string detail = "could not write generated dependency probe";
    if (!cleaned.ok())
      detail += "; could not clean private native-dependency workspace: " + cleaned.detail;
    return {std::nullopt, 125, std::move(detail)};
  }
  const std::vector<nebula::cli::HostedNativeDependencyUnit> units =
    plan_hosted_compile_units(opt, toolchain, CompileFlavor::Normal, generated_probe,
                              loaded.host_cxx_sources, loaded.native_inputs, artifact_kind);
  nebula::cli::HostedNativeDependencyDiscoveryResult discovered =
    nebula::cli::discover_hosted_native_dependencies_once(toolchain, units, workspace.path());
  const nebula::cli::HostedObjectWorkspaceCleanupResult cleaned = workspace.cleanup();
  if (!cleaned.ok()) {
    std::string detail = discovered.detail;
    if (!detail.empty())
      detail += "; ";
    detail += "could not clean private native-dependency workspace: " + cleaned.detail;
    return {std::nullopt, 125, std::move(detail)};
  }
  return {std::move(discovered.snapshot), discovered.exit_code, std::move(discovered.detail)};
}

static int emit_native_dependency_error(std::string message,
                                        const NativeDependencySnapshotResult &result,
                                        const CliOptions &opt,
                                        nebula::frontend::DiagnosticStage stage) {
  auto diagnostic = make_cli_diag(
    nebula::frontend::Severity::Error, "NBL-CLI-NATIVE-DEPS", std::move(message), stage,
    nebula::frontend::DiagnosticRisk::Critical, result.detail,
    "Nebula refused to derive, reuse, or publish an artifact from an incomplete native "
    "dependency closure",
    {"stop concurrent native-header writers and retry",
     "remove unsupported links or special files from compiler include paths"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return result.exit_code == 0 ? 125 : result.exit_code;
}

static bool protect_native_dependencies(nebula::cli::HostedArtifactTransaction &transaction,
                                        const nebula::cli::HostedNativeDependencySnapshot &snapshot,
                                        nebula::cli::HostedArtifactTransactionError &error) {
  std::vector<nebula::cli::HostedArtifactProtectedInput> inputs;
  inputs.reserve(snapshot.files.size());
  for (const nebula::cli::HostedNativeDependencyFile &file : snapshot.files)
    inputs.emplace_back(file.canonical_path, file.content);
  const nebula::cli::HostedArtifactTransactionResult protected_result =
    transaction.protect_additional_inputs(inputs);
  if (protected_result.ok())
    return true;
  error = protected_result.error;
  return false;
}

static bool ensure_output_parents(const std::vector<fs::path> &outputs, const CliOptions &opt,
                                  nebula::frontend::DiagnosticStage stage) {
  for (const fs::path &output : outputs) {
    const fs::path parent = output.has_parent_path() ? output.parent_path() : fs::path(".");
    std::error_code error;
    fs::create_directories(parent, error);
    if (!error)
      continue;
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-ARTIFACT-PARENT",
      "failed to prepare hosted output directory", stage, nebula::frontend::DiagnosticRisk::High,
      error.message(),
      "hosted artifact publication requires every output parent to exist before its transaction",
      {"check output directory permissions and path components"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return false;
  }
  return true;
}

static std::optional<RuntimeHeaderIdentityResult>
runtime_headers_or_emit(const CliOptions &opt, nebula::frontend::DiagnosticStage stage) {
  if (auto diagnostic = validate_runtime_include_root(opt); diagnostic.has_value()) {
    diagnostic->stage = stage;
    emit_diagnostics({*diagnostic}, opt, std::cerr);
    return std::nullopt;
  }
  RuntimeHeaderIdentityResult result = resolve_runtime_header_identities(opt);
  if (result.ok())
    return result;
  auto diagnostic =
    make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-METADATA-IDENTITY",
                  "failed to freeze hosted runtime-header provenance", stage,
                  nebula::frontend::DiagnosticRisk::High, result.detail,
                  "Nebula refuses to invoke a hosted compiler without a complete runtime input set",
                  {"restore the installed runtime header tree and remove symbolic links"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return std::nullopt;
}

static std::vector<nebula::cli::HostedArtifactProtectedInput>
make_protected_inputs(const LoadedCompileInput &loaded,
                      const RuntimeHeaderIdentityResult &runtime_headers,
                      const nebula::cli::ResolvedHostedToolPaths &tool_paths) {
  std::vector<nebula::cli::HostedArtifactProtectedInput> inputs;
  inputs.reserve(loaded.build_input_identities.size() + runtime_headers.identities.size() + 3U);
  const auto append_digest_inputs = [&](const std::vector<BuildInputFileIdentity> &identities) {
    for (const BuildInputFileIdentity &identity : identities) {
      inputs.emplace_back(identity.path, nebula::cli::FileDigest{identity.size, identity.sha256});
    }
  };
  append_digest_inputs(loaded.build_input_identities);
  append_digest_inputs(runtime_headers.identities);
  inputs.emplace_back(tool_paths.compiler);
  if (tool_paths.archiver.has_value())
    inputs.emplace_back(*tool_paths.archiver);
  inputs.emplace_back(tool_paths.nebula_executable);
  return inputs;
}

static std::vector<nebula::cli::HostedArtifactProtectedDirectory>
make_protected_directories(const LoadedCompileInput &loaded,
                           const RuntimeHeaderIdentityResult &runtime_headers) {
  std::vector<nebula::cli::HostedArtifactProtectedDirectory> directories;
  directories.reserve(loaded.build_input_directory_identities.size() + 1U);
  for (const BuildInputDirectoryIdentity &identity : loaded.build_input_directory_identities) {
    directories.push_back({identity.path, nebula::cli::DirectoryTreeDigest{
                                            identity.entry_count, identity.membership_sha256}});
  }
  directories.push_back(
    {runtime_headers.directory_identity->path,
     nebula::cli::DirectoryTreeDigest{runtime_headers.directory_identity->entry_count,
                                      runtime_headers.directory_identity->membership_sha256}});
  return directories;
}

static int emit_transaction_error(const nebula::cli::HostedArtifactTransactionError &error,
                                  const fs::path &public_artifact, const CliOptions &opt,
                                  nebula::frontend::DiagnosticStage stage) {
  using ErrorCode = nebula::cli::HostedArtifactTransactionErrorCode;
  std::string code = "NBL-CLI-ARTIFACT-TRANSACTION";
  std::string message = "hosted artifact transaction failed";
  int exit_code = 125;
  switch (error.code) {
  case ErrorCode::InvalidPlan:
  case ErrorCode::PathConflict:
  case ErrorCode::UnsafePath:
    code = "NBL-CLI-ARTIFACT-PATH";
    message = "hosted output conflicts with an input, tool, lock, or unsafe path";
    exit_code = 1;
    break;
  case ErrorCode::Busy:
    code = "NBL-CLI-ARTIFACT-BUSY";
    message = "hosted output is already being published by another Nebula process";
    exit_code = 1;
    break;
  case ErrorCode::ConcurrentModification:
    code = "NBL-CLI-ARTIFACT-CONCURRENT";
    message = "a hosted build input or output changed during publication";
    exit_code = 1;
    break;
  case ErrorCode::None:
    return 0;
  case ErrorCode::Io:
  case ErrorCode::DurabilityUnavailable:
  case ErrorCode::InvalidState:
  case ErrorCode::StagedOutputInvalid:
  case ErrorCode::Metadata:
  case ErrorCode::Publication:
  case ErrorCode::RollbackIncomplete:
  case ErrorCode::CleanupIncomplete:
    break;
  }
  auto diagnostic = make_cli_diag(
    nebula::frontend::Severity::Error, std::move(code), std::move(message), stage,
    nebula::frontend::DiagnosticRisk::Critical,
    error.operation + (error.detail.empty() ? std::string{} : ": " + error.detail) +
      (error.path.empty()
         ? std::string{}
         : "; transaction subject: " + quote_cli_log_value(error.path.filename().string())) +
      "; public artifact: " + quote_cli_log_value(public_artifact.string()),
    "no new hosted artifact is reported as successfully published",
    {"inspect the public output directory and retry after resolving the reported conflict"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return exit_code;
}

static int abort_transaction(nebula::cli::HostedArtifactTransaction &transaction, int primary_exit,
                             const fs::path &public_artifact, const CliOptions &opt,
                             nebula::frontend::DiagnosticStage stage) {
  const nebula::cli::HostedArtifactTransactionResult aborted = transaction.abort();
  if (aborted.ok())
    return primary_exit;
  (void)emit_transaction_error(aborted.error, public_artifact, opt, stage);
  return 125;
}

static int adopt_staged_outputs_and_abort(nebula::cli::HostedArtifactTransaction &transaction,
                                          int primary_exit, const fs::path &public_artifact,
                                          const CliOptions &opt,
                                          nebula::frontend::DiagnosticStage stage) {
  const nebula::cli::HostedArtifactTransactionResult adopted =
    transaction.adopt_existing_staged_outputs_for_cleanup();
  if (!adopted.ok()) {
    (void)emit_transaction_error(adopted.error, public_artifact, opt, stage);
    return abort_transaction(transaction, 125, public_artifact, opt, stage);
  }
  return abort_transaction(transaction, primary_exit, public_artifact, opt, stage);
}

static int emit_executable_lease_error(const nebula::cli::VerifiedExecutableLeaseError &error,
                                       const fs::path &public_artifact, const CliOptions &opt,
                                       nebula::frontend::DiagnosticStage stage) {
  using ErrorCode = nebula::cli::VerifiedExecutableLeaseErrorCode;
  std::string code = "NBL-CLI-EXEC-LEASE-INFRA";
  std::string message = "verified executable lease failed";
  int exit_code = 125;
  switch (error.code) {
  case ErrorCode::None:
    return 0;
  case ErrorCode::InvalidPath:
  case ErrorCode::UnsafePath:
    code = "NBL-CLI-EXEC-LEASE-PATH";
    message = "executable path cannot be safely leased";
    exit_code = 1;
    break;
  case ErrorCode::TooLarge:
    code = "NBL-CLI-EXEC-LEASE-SIZE";
    message = "executable exceeds the verified lease bound";
    exit_code = 1;
    break;
  case ErrorCode::ContentMismatch:
  case ErrorCode::ConcurrentModification:
    code = "NBL-CLI-EXEC-LEASE-CONCURRENT";
    message = "executable changed while its execution lease was acquired";
    exit_code = 1;
    break;
  case ErrorCode::Io:
  case ErrorCode::InvalidState:
  case ErrorCode::CleanupIncomplete:
    break;
  }
  const fs::path detail_path = error.path.empty() ? public_artifact : error.path;
  auto diagnostic =
    make_cli_diag(nebula::frontend::Severity::Error, std::move(code), std::move(message), stage,
                  nebula::frontend::DiagnosticRisk::Critical,
                  error.operation + (error.detail.empty() ? std::string{} : ": " + error.detail) +
                    "; path: " + detail_path.string(),
                  "Nebula did not execute an unverified public pathname",
                  {"remove conflicting private .nebula-exec files only after verifying ownership",
                   "retry from a private, owner-controlled output directory"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return exit_code;
}

static int cleanup_executable_lease(nebula::cli::VerifiedExecutableLease &lease, int primary_exit,
                                    const fs::path &public_artifact, const CliOptions &opt,
                                    nebula::frontend::DiagnosticStage stage) {
  const nebula::cli::VerifiedExecutableLeaseResult cleaned = lease.cleanup();
  if (cleaned.ok())
    return primary_exit;
  (void)emit_executable_lease_error(cleaned.error, public_artifact, opt, stage);
  return 125;
}

static int execute_verified_executable(nebula::cli::VerifiedExecutableLease &lease,
                                       const std::vector<std::string> &arguments,
                                       const fs::path &public_artifact, const CliOptions &opt,
                                       nebula::frontend::DiagnosticStage stage) {
  std::cerr << "[cmd] launching verified host process with " << arguments.size() << " arguments\n";
  const nebula::cli::HostProcessResult process = lease.execute(arguments);
  std::string process_error;
  int exit_code = nebula::cli::host_process_compatible_exit_code(process, process_error);
  if (!process_error.empty()) {
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-EXEC-PROCESS",
      "verified host process could not complete", stage, nebula::frontend::DiagnosticRisk::Critical,
      process_error, "the executable lease was not reported as a successful invocation",
      {"inspect host process permissions and platform execution policy"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    exit_code = 125;
  } else if (exit_code != 0) {
    std::cerr << "[cmd] exit=" << exit_code << "\n";
  }
  return cleanup_executable_lease(lease, exit_code, public_artifact, opt, stage);
}

static bool arm_hosted_termination_boundary(CompilerTerminationSignalScope &signals,
                                            const CliOptions &opt,
                                            nebula::frontend::DiagnosticStage stage) {
#if defined(_WIN32)
  (void)signals;
  (void)opt;
  (void)stage;
  return true;
#else
  std::string detail;
  if (signals.arm(detail))
    return true;
  auto diagnostic =
    make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
                  "failed to establish the hosted compiler termination boundary", stage,
                  nebula::frontend::DiagnosticRisk::Critical, detail,
                  "Nebula cannot guarantee compiler-group cleanup before caller signal redelivery",
                  {"retry from a single-threaded Nebula build process"});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return false;
#endif
}

struct HostedTerminationHandoff {
  int exit_code = 0;
  int interrupted_signal = 0;
};

static HostedTerminationHandoff
restore_hosted_termination_boundary(CompilerTerminationSignalScope &signals, bool cleanup_complete,
                                    const CliOptions &opt,
                                    nebula::frontend::DiagnosticStage stage) {
#if defined(_WIN32)
  (void)signals;
  (void)opt;
  (void)stage;
  return {cleanup_complete ? 0 : 125, 0};
#else
  if (!cleanup_complete)
    signals.suppress_emergency_redelivery();
  int restored_signal = 0;
  std::string detail;
  if (!signals.restore(restored_signal, detail)) {
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
      "failed to restore the hosted termination boundary", stage,
      nebula::frontend::DiagnosticRisk::Critical, detail,
      cleanup_complete
        ? "artifact cleanup completed, but caller signal state could not be confirmed"
        : "signal redelivery remains suppressed because artifact cleanup was incomplete",
      {"retry the command in a fresh process"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return {125, 0};
  }
  if (!cleanup_complete)
    return {125, 0};
  if (restored_signal == 0)
    return {};
  return {128 + restored_signal, restored_signal};
#endif
}

static HostedTerminationHandoff
abort_frozen_hosted_publication(CompilerTerminationSignalScope &signals,
                                nebula::cli::HostedArtifactTransaction &transaction,
                                int primary_exit, const fs::path &public_artifact,
                                const CliOptions &opt, nebula::frontend::DiagnosticStage stage) {
  const nebula::cli::HostedArtifactTransactionResult aborted = transaction.abort();
  if (!aborted.ok()) {
    (void)emit_transaction_error(aborted.error, public_artifact, opt, stage);
  }
  const HostedTerminationHandoff handoff =
    restore_hosted_termination_boundary(signals, aborted.ok(), opt, stage);
  if (handoff.exit_code != 0)
    return handoff;
  return {aborted.ok() ? primary_exit : 125, 0};
}

static std::optional<HostedTerminationHandoff> freeze_before_hosted_publication(
  CompilerTerminationSignalScope &signals, nebula::cli::HostedArtifactTransaction &transaction,
  const fs::path &public_artifact, const CliOptions &opt, nebula::frontend::DiagnosticStage stage) {
#if defined(_WIN32)
  (void)signals;
  (void)transaction;
  (void)public_artifact;
  (void)opt;
  (void)stage;
  return std::nullopt;
#else
  // This freeze is the publication linearization point: signals observed by
  // the build before the snapshot cancel the sealed transaction. Signals that
  // arrive afterward stay blocked until commit/cleanup has closed the boundary.
  std::string detail;
  if (!signals.freeze(detail)) {
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
      "failed to freeze termination signals before hosted publication", stage,
      nebula::frontend::DiagnosticRisk::Critical, detail,
      "the sealed output was not accepted for publication while signal ownership was ambiguous",
      {"retry the command in a fresh single-threaded Nebula process"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return abort_frozen_hosted_publication(signals, transaction, 125, public_artifact, opt, stage);
  }
  const int pending_signal = signals.intercepted_signal();
  if (pending_signal == 0)
    return std::nullopt;

  auto diagnostic = make_cli_diag(
    nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-INTERRUPTED",
    "hosted publication was interrupted before commit", stage,
    nebula::frontend::DiagnosticRisk::Critical,
    "received termination signal " + std::to_string(pending_signal),
    "the sealed staging output is discarded before preserving caller termination semantics", {});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return abort_frozen_hosted_publication(signals, transaction, 128 + pending_signal,
                                         public_artifact, opt, stage);
#endif
}

static int finish_hosted_run_termination_handoff(const HostedTerminationHandoff &handoff,
                                                 const CliOptions &opt,
                                                 nebula::frontend::DiagnosticStage stage) {
#if defined(_WIN32)
  (void)opt;
  (void)stage;
  return handoff.exit_code;
#else
  if (handoff.interrupted_signal == 0)
    return handoff.exit_code;
  if (::kill(::getpid(), handoff.interrupted_signal) == 0)
    return 128 + handoff.interrupted_signal;
  auto diagnostic = make_cli_diag(
    nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
    "failed to redeliver caller termination signal", stage,
    nebula::frontend::DiagnosticRisk::Critical,
    std::string("signal redelivery failed: ") + std::strerror(errno),
    "the hosted transaction is closed, but caller termination semantics were not preserved", {});
  emit_diagnostics({diagnostic}, opt, std::cerr);
  return 125;
#endif
}

static std::optional<int> restore_before_user_execution(CompilerTerminationSignalScope &signals,
                                                        nebula::cli::VerifiedExecutableLease &lease,
                                                        const fs::path &public_artifact,
                                                        const CliOptions &opt,
                                                        nebula::frontend::DiagnosticStage stage) {
#if defined(_WIN32)
  (void)signals;
  (void)lease;
  (void)public_artifact;
  (void)opt;
  (void)stage;
  return std::nullopt;
#else
  std::string detail;
  if (!signals.freeze(detail)) {
    (void)cleanup_executable_lease(lease, 125, public_artifact, opt, stage);
    auto diagnostic =
      make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
                    "failed to freeze the hosted termination boundary", stage,
                    nebula::frontend::DiagnosticRisk::Critical, detail,
                    "the user artifact was not started while signal ownership was ambiguous",
                    {"retry the command after checking process signal policy"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return 125;
  }
  const int pending_signal = signals.intercepted_signal();
  bool cleanup_safe_for_redelivery = true;
  if (pending_signal != 0) {
    const int cleanup_exit = cleanup_executable_lease(lease, 0, public_artifact, opt, stage);
    cleanup_safe_for_redelivery = cleanup_exit == 0;
    if (!cleanup_safe_for_redelivery)
      signals.suppress_emergency_redelivery();
  }
  int restored_signal = 0;
  detail.clear();
  if (!signals.restore(restored_signal, detail)) {
    if (pending_signal == 0) {
      (void)cleanup_executable_lease(lease, 125, public_artifact, opt, stage);
    }
    auto diagnostic =
      make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
                    "failed to restore caller signal state before user execution", stage,
                    nebula::frontend::DiagnosticRisk::Critical, detail,
                    "the user artifact was not started under an ambiguous signal disposition",
                    {"retry the command in a fresh process"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return 125;
  }
  if (restored_signal == 0)
    return std::nullopt;
  if (!cleanup_safe_for_redelivery)
    return 125;
  if (::kill(::getpid(), restored_signal) != 0) {
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-HOSTED-SIGNAL",
      "failed to redeliver caller termination signal", stage,
      nebula::frontend::DiagnosticRisk::Critical,
      std::string("signal redelivery failed: ") + std::strerror(errno),
      "the user artifact was not started, but caller termination semantics were not preserved", {});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return 125;
  }
  return 128 + restored_signal;
#endif
}

static bool
protect_resolved_tool_dependencies(nebula::cli::HostedArtifactTransaction &transaction,
                                   const nebula::cli::ResolvedHostedToolchain &toolchain,
                                   nebula::cli::HostedArtifactTransactionError &error) {
  std::vector<nebula::cli::HostedArtifactProtectedInput> dependencies;
  dependencies.reserve(toolchain.compiler_dependencies().size());
  for (const nebula::cli::ResolvedToolDependency &dependency : toolchain.compiler_dependencies()) {
    dependencies.emplace_back(
      dependency.identity.executable,
      nebula::cli::FileDigest{dependency.identity.size, dependency.identity.sha256});
  }
  const nebula::cli::HostedArtifactTransactionResult protected_result =
    transaction.protect_additional_inputs(dependencies);
  if (protected_result.ok())
    return true;
  error = protected_result.error;
  return false;
}

} // namespace

CliCommandResult cmd_build(const fs::path &file, const CliOptions &opt) {
  CacheReportScope cache_scope(opt);
  auto loaded = load_compile_input(file, nebula::frontend::DiagnosticStage::Build,
                                   load_compile_options_from_cli(opt));
  if (!loaded.diags.empty())
    emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) {
    return {1, 0};
  }
  const fs::path effective_file = loaded.entry_file.empty() ? file : loaded.entry_file;
  AnalysisProfile requested = opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Auto;
  const AnalysisProfile resolved = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  const bool freestanding_object = opt.artifact_kind == BuildArtifactKind::FreestandingObject;
  std::optional<nebula::cli::ResolvedFreestandingToolchain> freestanding_toolchain;
  std::optional<ArtifactBuildKey> freestanding_build_key;
  FreestandingArtifactDisposition freestanding_artifact_disposition =
    FreestandingArtifactDisposition::Absent;
  const auto finish_freestanding_session = [&](CliCommandResult primary) {
    if (!freestanding_toolchain.has_value() ||
        (!freestanding_toolchain->session_active() &&
         !freestanding_toolchain->compiler_snapshot_active())) {
      return primary;
    }
    nebula::cli::FreestandingToolchainCloseResult close_result =
      freestanding_toolchain->close_session();
    if (close_result.ok()) {
      if (close_result.interrupted_signal != 0) {
        return CliCommandResult{128 + close_result.interrupted_signal,
                                close_result.interrupted_signal};
      }
      return primary;
    }
    std::string cleanup_impact;
    switch (freestanding_artifact_disposition) {
    case FreestandingArtifactDisposition::Absent:
      cleanup_impact =
        "no freestanding artifact was published, and compiler or signal cleanup is incomplete";
      break;
    case FreestandingArtifactDisposition::Committed:
      cleanup_impact = "the complete freestanding artifact trio remains committed, but compiler "
                       "or caller signal cleanup is incomplete";
      break;
    case FreestandingArtifactDisposition::CleanupIncomplete:
      cleanup_impact = "freestanding artifact cleanup is incomplete, so publication state must "
                       "not be inferred from the command status";
      break;
    }
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-FS-CLEANUP",
      "failed to close the freestanding compiler session", nebula::frontend::DiagnosticStage::Build,
      nebula::frontend::DiagnosticRisk::Critical, close_result.detail, std::move(cleanup_impact),
      {"inspect the owner-controlled toolchain bin directory before retrying"});
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return CliCommandResult{125, 0};
  };
  if (freestanding_object) {
    if (!opt.freestanding_toolchain_root.has_value()) {
      auto diagnostic = make_cli_diag(
        nebula::frontend::Severity::Error, "NBL-CLI-FS-TOOLCHAIN",
        "freestanding build has no explicit toolchain root",
        nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::Critical,
        "the validated CLI request lost its freestanding toolchain root",
        "Nebula cannot bind provenance and execution to one compiler identity");
      emit_diagnostics({diagnostic}, opt, std::cerr);
      return {125, 0};
    }
    nebula::cli::FreestandingToolchainResolutionResult toolchain_resolution =
      nebula::cli::resolve_freestanding_toolchain(
        {*opt.freestanding_toolchain_root, opt.self_executable});
    if (!toolchain_resolution.ok()) {
      const bool host_unsupported = toolchain_resolution.error.code ==
                                    nebula::cli::FreestandingToolchainErrorCode::HostUnsupported;
      auto diagnostic =
        make_cli_diag(nebula::frontend::Severity::Error,
                      host_unsupported ? "NBL-CLI-FS-HOST-UNSUPPORTED" : "NBL-CLI-FS-TOOLCHAIN",
                      host_unsupported ? "freestanding object build is unsupported on this host"
                                       : "failed to resolve the freestanding compiler toolchain",
                      nebula::frontend::DiagnosticStage::Build,
                      nebula::frontend::DiagnosticRisk::Critical, toolchain_resolution.error.detail,
                      "no artifact is emitted without a bounded, immutable compiler identity",
                      {"provide an owner-controlled absolute root containing bin/clang++",
                       "verify that clang++ supports the x86_64-unknown-none ABI contract"});
      emit_diagnostics({diagnostic}, opt, std::cerr);
      return {125, toolchain_resolution.interrupted_signal};
    }
    freestanding_toolchain.emplace(std::move(*toolchain_resolution.value));
    freestanding_build_key = derive_build_key_or_emit(
      opt, resolved, loaded.cache_key_source, nebula::frontend::DiagnosticStage::Build, nullptr,
      nullptr, nullptr, &*freestanding_toolchain);
    if (!freestanding_build_key.has_value())
      return finish_freestanding_session({1, 0});
  }

  CompilePipelineOptions popt;
  popt.mode = opt.mode;
  popt.profile = resolved;
  popt.analysis_tier = tier;
  popt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region, opt.no_std);
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.no_std = effective_no_std(opt.runtime_profile, opt.no_std);
  popt.runtime_profile = opt.runtime_profile;
  popt.panic_policy = opt.panic_policy;
  popt.target = opt.target;
  popt.include_lint = should_include_lint_in_build_stage(opt);
  popt.allow_cross_stage_reuse = false;
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = effective_file.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  auto analysis = run_compile_pipeline(loaded.compile_sources, popt);
  emit_diagnostics(analysis.diags, opt, std::cerr);
  if (analysis.has_error || !analysis.nir_prog || !analysis.rep_owner)
    return finish_freestanding_session({1, 0});

  const fs::path out_bin = default_build_artifact_path(loaded, effective_file, opt);
  const fs::path out_cpp =
    freestanding_object ? out_bin.parent_path() / (out_bin.stem().string() + ".freestanding.cpp")
                        : cpp_output_path(effective_file, opt, "");
  const bool library_mode = opt.artifact_kind == BuildArtifactKind::StaticLib ||
                            opt.artifact_kind == BuildArtifactKind::SharedLib;
  if (library_mode && !loaded.host_cxx_sources.empty()) {
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-CABI-HOSTCXX",
      "C ABI library build does not support host_cxx sources yet",
      nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
      "host_cxx files currently model executable-side host integration, not stable library ABI "
      "surface",
      "generated library could accidentally expose or link unintended host-side symbols",
      {"remove host_cxx from the package for library builds",
       "or keep the host bridge in a separate consumer project"});
    emit_diagnostics({d}, opt, std::cerr);
    return {1, 0};
  }
  if (library_mode && !loaded.native_inputs.empty()) {
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-CABI-NATIVE",
      "C ABI library build does not support [native] package sources yet",
      nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
      "package-native sources currently model executable-side integration, not stable library ABI "
      "surface",
      "generated library could accidentally depend on non-portable native build inputs",
      {"remove [native] from the reachable package graph for library builds",
       "or keep the native bridge in a separate executable consumer"});
    emit_diagnostics({d}, opt, std::cerr);
    return {1, 0};
  }

  if (freestanding_object) {
    nebula::codegen::FreestandingEmitOptions emit_options;
    emit_options.target = opt.target;
    emit_options.panic_policy = opt.panic_policy;
    if (!loaded.project_name.empty())
      emit_options.root_package = loaded.project_name;
    auto emission =
      nebula::codegen::emit_freestanding_cpp(*analysis.nir_prog, *analysis.rep_owner, emit_options);
    if (!emission.diagnostics.empty()) {
      emit_diagnostics(emission.diagnostics, opt, std::cerr);
    }
    if (!emission.ok())
      return finish_freestanding_session({1, 0});

    FreestandingObjectRequest request;
    request.input_path = effective_file;
    request.generated_source_path = out_cpp;
    request.object_path = out_bin;
    request.translation_unit = std::move(*emission.translation_unit);
    request.build_key = *freestanding_build_key;
    request.mode = opt.mode;
    auto object_result = build_freestanding_object(request, *freestanding_toolchain);
    freestanding_artifact_disposition = object_result.artifact_disposition;
    if (!object_result.diagnostics.empty()) {
      emit_diagnostics(object_result.diagnostics, opt, std::cerr);
    }
    if (object_result.interrupted_signal != 0)
      return finish_freestanding_session(
        {object_result.exit_code(), object_result.interrupted_signal});
    if (!object_result.ok())
      return finish_freestanding_session({object_result.exit_code(), 0});

    std::cerr << "wrote: " << quote_cli_log_value(out_cpp.string()) << "\n";
    std::cerr << "wrote artifact: " << quote_cli_log_value(out_bin.string()) << "\n";
    std::cerr << "wrote metadata: " << quote_cli_log_value(out_bin.string() + ".nebmeta") << "\n";
    return finish_freestanding_session({0, 0});
  }

  nebula::codegen::EmitOptions eopt;
  eopt.main_mode =
    library_mode ? nebula::codegen::MainMode::None : nebula::codegen::MainMode::CallMainIfPresent;
  eopt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region, opt.no_std);
  eopt.runtime_profile = opt.runtime_profile;
  eopt.target = opt.target;
  eopt.panic_policy = opt.panic_policy;
  eopt.emit_c_abi_wrappers = library_mode;
  if (library_mode && !loaded.project_name.empty()) {
    eopt.c_abi_export_package = loaded.project_name;
  }
  const auto &backend = nebula::codegen::default_backend();
  const std::string cpp =
    backend.emit_translation_unit(*analysis.nir_prog, *analysis.rep_owner, eopt);
  std::optional<fs::path> header_path;
  std::optional<std::string> header_contents;
  if (library_mode) {
    const auto exports = backend.collect_c_abi_exports(*analysis.nir_prog, loaded.project_name);
    if (exports.empty()) {
      auto d = make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-CABI-NOEXPORT",
                             "library build requested but no @export @abi_c functions were found",
                             nebula::frontend::DiagnosticStage::Build,
                             nebula::frontend::DiagnosticRisk::High,
                             "C ABI library output needs an explicit exported function surface",
                             "generated library would not expose a supported public ABI",
                             {"mark at least one function with @export and @abi_c",
                              "or build the project as an executable"});
      emit_diagnostics({d}, opt, std::cerr);
      return {1, 0};
    }
    std::unordered_set<std::string> seen_exports;
    for (const auto &fn : exports) {
      if (!seen_exports.insert(fn.export_name).second) {
        auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-CABI-CONFLICT",
          "duplicate exported C ABI symbol: " + fn.export_name,
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
          "sanitized package/module/function names collided in the public C ABI surface",
          "generated library would expose ambiguous symbols",
          {"rename one of the exported functions or modules", "or narrow the export set"});
        emit_diagnostics({d}, opt, std::cerr);
        return {1, 0};
      }
    }
    header_path = header_output_path_for(loaded, effective_file, out_bin);
    header_contents = backend.emit_c_abi_header(*analysis.nir_prog, exports,
                                                default_header_stem(loaded, effective_file));
  }

  std::optional<fs::path> import_library_path;
#if defined(_WIN32)
  if (opt.artifact_kind == BuildArtifactKind::SharedLib) {
    import_library_path = out_bin.parent_path() / ("lib" + out_bin.stem().string() + ".dll.a");
  }
#endif
  std::vector<fs::path> public_outputs = {out_bin, out_cpp};
  if (header_path.has_value())
    public_outputs.push_back(*header_path);
  if (import_library_path.has_value())
    public_outputs.push_back(*import_library_path);
  if (!ensure_output_parents(public_outputs, opt, nebula::frontend::DiagnosticStage::Build)) {
    return {1, 0};
  }

  const auto runtime_headers =
    runtime_headers_or_emit(opt, nebula::frontend::DiagnosticStage::Build);
  if (!runtime_headers.has_value())
    return {1, 0};
  const auto tool_paths = resolve_hosted_tool_paths_or_emit(
    opt, opt.artifact_kind, nebula::frontend::DiagnosticStage::Build);
  if (!tool_paths.has_value())
    return {1, 0};
  CompilerTerminationSignalScope termination_signals;
  if (!arm_hosted_termination_boundary(termination_signals, opt,
                                       nebula::frontend::DiagnosticStage::Build)) {
    return {125, 0};
  }
  CompilerTerminationSignalScope *termination_boundary = nullptr;
#if !defined(_WIN32)
  termination_boundary = &termination_signals;
#endif

  nebula::cli::HostedArtifactTransactionPlan transaction_plan{
    out_bin, out_cpp, header_path, import_library_path,
    make_protected_inputs(loaded, *runtime_headers, *tool_paths)};
  transaction_plan.protected_directories = make_protected_directories(loaded, *runtime_headers);
  auto begun = nebula::cli::begin_hosted_artifact_transaction(transaction_plan);
  if (!begun.ok()) {
    return {
      emit_transaction_error(begun.error, out_bin, opt, nebula::frontend::DiagnosticStage::Build),
      0};
  }
  std::unique_ptr<nebula::cli::HostedArtifactTransaction> transaction =
    std::move(begun.transaction);

  const auto hosted_toolchain = resolve_hosted_toolchain_or_emit(
    opt, opt.artifact_kind, nebula::frontend::DiagnosticStage::Build, &*tool_paths,
    termination_boundary);
  if (!hosted_toolchain.has_value()) {
    return {
      abort_transaction(*transaction, 1, out_bin, opt, nebula::frontend::DiagnosticStage::Build),
      0};
  }
  nebula::cli::HostedArtifactTransactionError protection_error;
  if (!protect_resolved_tool_dependencies(*transaction, *hosted_toolchain, protection_error)) {
    const int error_exit = emit_transaction_error(protection_error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return {abort_transaction(*transaction, error_exit, out_bin, opt,
                              nebula::frontend::DiagnosticStage::Build),
            0};
  }
  const fs::path dependency_workspace_parent =
    out_bin.has_parent_path() ? out_bin.parent_path() : fs::path(".");
  NativeDependencySnapshotResult native_dependencies = discover_native_dependencies(
    opt, *hosted_toolchain, loaded, opt.artifact_kind, dependency_workspace_parent);
  if (!native_dependencies.ok()) {
    const int dependency_exit = emit_native_dependency_error(
      "failed to discover the hosted native dependency closure", native_dependencies, opt,
      nebula::frontend::DiagnosticStage::Build);
    return {abort_transaction(*transaction, dependency_exit, out_bin, opt,
                              nebula::frontend::DiagnosticStage::Build),
            0};
  }
  if (!protect_native_dependencies(*transaction, *native_dependencies.snapshot, protection_error)) {
    const int error_exit = emit_transaction_error(protection_error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return {abort_transaction(*transaction, error_exit, out_bin, opt,
                              nebula::frontend::DiagnosticStage::Build),
            0};
  }
  const std::optional<ArtifactBuildKey> build_key = derive_build_key_or_emit(
    opt, resolved, loaded.cache_key_source, nebula::frontend::DiagnosticStage::Build,
    &*hosted_toolchain, &*runtime_headers, &*native_dependencies.snapshot);
  if (!build_key.has_value()) {
    return {
      abort_transaction(*transaction, 1, out_bin, opt, nebula::frontend::DiagnosticStage::Build),
      0};
  }

  const nebula::cli::HostedArtifactStagingPaths &staging = transaction->staging_paths();
  if (!write_text_file(staging.generated_cpp, cpp) ||
      (header_contents.has_value() &&
       (!staging.generated_header.has_value() ||
        !write_text_file(*staging.generated_header, *header_contents)))) {
    auto diagnostic = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-IO002",
      "failed to write a transaction-owned generated source",
      nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::High,
      "private hosted staging output is not writable", "the public output set remains uncommitted");
    emit_diagnostics({diagnostic}, opt, std::cerr);
    return {adopt_staged_outputs_and_abort(*transaction, 1, out_bin, opt,
                                           nebula::frontend::DiagnosticStage::Build),
            0};
  }
  const nebula::cli::HostedArtifactTransactionResult generated_adopted =
    transaction->adopt_existing_staged_outputs_for_cleanup();
  if (!generated_adopted.ok()) {
    const int error_exit = emit_transaction_error(generated_adopted.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return {abort_transaction(*transaction, error_exit, out_bin, opt,
                              nebula::frontend::DiagnosticStage::Build),
            0};
  }

  nebula::cli::HostedNativeDependencySnapshot compiled_native_dependencies;
  const int compile_exit =
    compile_cpp(opt, *hosted_toolchain, staging.generated_cpp, staging.artifact,
                CompileFlavor::Normal, loaded.host_cxx_sources, loaded.native_inputs,
                opt.artifact_kind, out_bin, staging.import_library, &compiled_native_dependencies);
  if (compile_exit != 0) {
    return {adopt_staged_outputs_and_abort(*transaction, compile_exit, out_bin, opt,
                                           nebula::frontend::DiagnosticStage::Build),
            0};
  }
  if (compiled_native_dependencies != *native_dependencies.snapshot) {
    NativeDependencySnapshotResult mismatch;
    mismatch.exit_code = 125;
    mismatch.detail =
      "actual compilation dependency files differ from the stable pre-compilation snapshot";
    const int mismatch_exit =
      emit_native_dependency_error("hosted native dependencies changed during compilation",
                                   mismatch, opt, nebula::frontend::DiagnosticStage::Build);
    return {adopt_staged_outputs_and_abort(*transaction, mismatch_exit, out_bin, opt,
                                           nebula::frontend::DiagnosticStage::Build),
            0};
  }
  const nebula::cli::HostedArtifactTransactionResult inputs_after_compile =
    transaction->revalidate_protected_inputs();
  if (!inputs_after_compile.ok()) {
    const int error_exit = emit_transaction_error(inputs_after_compile.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return {adopt_staged_outputs_and_abort(*transaction, error_exit, out_bin, opt,
                                           nebula::frontend::DiagnosticStage::Build),
            0};
  }

  const nebula::cli::HostedArtifactTransactionResult sealed = transaction->seal(*build_key);
  if (!sealed.ok()) {
    const int error_exit =
      emit_transaction_error(sealed.error, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
    return {abort_transaction(*transaction, error_exit, out_bin, opt,
                              nebula::frontend::DiagnosticStage::Build),
            0};
  }
  if (const auto interrupted = freeze_before_hosted_publication(
        termination_signals, *transaction, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
      interrupted.has_value()) {
    return {interrupted->exit_code, interrupted->interrupted_signal};
  }
  const nebula::cli::HostedArtifactTransactionResult committed = transaction->commit();
  if (!committed.ok()) {
    const int error_exit = emit_transaction_error(committed.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    const HostedTerminationHandoff handoff =
      abort_frozen_hosted_publication(termination_signals, *transaction, error_exit, out_bin, opt,
                                      nebula::frontend::DiagnosticStage::Build);
    return {handoff.exit_code, handoff.interrupted_signal};
  }
  const nebula::cli::HostedArtifactTransactionResult finished = transaction->finish();
  if (!finished.ok()) {
    (void)emit_transaction_error(finished.error, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
    const HostedTerminationHandoff handoff = restore_hosted_termination_boundary(
      termination_signals, false, opt, nebula::frontend::DiagnosticStage::Build);
    return {handoff.exit_code, handoff.interrupted_signal};
  }
  const HostedTerminationHandoff handoff = restore_hosted_termination_boundary(
    termination_signals, true, opt, nebula::frontend::DiagnosticStage::Build);
  if (handoff.exit_code != 0) {
    return {handoff.exit_code, handoff.interrupted_signal};
  }

  std::cerr << "wrote: " << quote_cli_log_value(out_cpp.string()) << "\n";
  if (header_path.has_value())
    std::cerr << "wrote: " << quote_cli_log_value(header_path->string()) << "\n";
  if (import_library_path.has_value())
    std::cerr << "wrote: " << quote_cli_log_value(import_library_path->string()) << "\n";
  std::cerr << "wrote artifact: " << quote_cli_log_value(out_bin.string()) << "\n";
  std::cerr << "wrote metadata: " << quote_cli_log_value(artifact_metadata_path(out_bin).string())
            << "\n";
  return {0, 0};
}

int cmd_run(const fs::path &file, const CliOptions &opt) {
  CacheReportScope cache_scope(opt);
  auto run_args = [&](const fs::path &artifact) {
    std::vector<std::string> cmd;
    cmd.reserve(1 + opt.run_args.size());
    cmd.push_back(artifact.string());
    cmd.insert(cmd.end(), opt.run_args.begin(), opt.run_args.end());
    return cmd;
  };
  if (opt.no_build && opt.reuse) {
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-CONFLICT",
      "--no-build cannot be used with --reuse", nebula::frontend::DiagnosticStage::Build,
      nebula::frontend::DiagnosticRisk::High, "mutually exclusive run artifact policies",
      "command cannot decide whether compilation is allowed",
      {"remove either --no-build or --reuse"});
    emit_diagnostics({d}, opt, std::cerr);
    return 1;
  }

  auto loaded = load_compile_input(file, nebula::frontend::DiagnosticStage::Build,
                                   load_compile_options_from_cli(opt));
  if (!loaded.diags.empty())
    emit_diagnostics(loaded.diags, opt, std::cerr);
  if (!loaded.ok) {
    return 1;
  }
  const fs::path effective_file = loaded.entry_file.empty() ? file : loaded.entry_file;
  if (run_preflight_if_enabled(effective_file, loaded.compile_sources, loaded.cache_key_source,
                               opt) != 0) {
    return 1;
  }

  if (opt.no_build) {
    auto lookup = resolve_no_build_artifact(effective_file, opt);
    emit_diagnostics(lookup.diags, opt, std::cerr);
    if (!lookup.artifact.has_value())
      return 1;
    auto leased = nebula::cli::begin_verified_executable_lease(*lookup.artifact);
    if (!leased.ok()) {
      return emit_executable_lease_error(leased.error, *lookup.artifact, opt,
                                         nebula::frontend::DiagnosticStage::Build);
    }
    return execute_verified_executable(*leased.lease, run_args(*lookup.artifact), *lookup.artifact,
                                       opt, nebula::frontend::DiagnosticStage::Build);
  }

  const fs::path out_bin = chosen_artifact_path(effective_file, opt);
  const fs::path out_cpp = cpp_output_path(effective_file, opt, "");
  const AnalysisProfile requested =
    opt.profile_explicit ? opt.analysis_profile : AnalysisProfile::Auto;
  const AnalysisProfile resolved_profile = resolve_profile(opt.mode, requested);
  const AnalysisTier tier = resolve_analysis_tier(opt.mode, opt.analysis_tier);
  if (!ensure_output_parents({out_bin, out_cpp}, opt, nebula::frontend::DiagnosticStage::Build)) {
    return 1;
  }
  const auto runtime_headers =
    runtime_headers_or_emit(opt, nebula::frontend::DiagnosticStage::Build);
  if (!runtime_headers.has_value())
    return 1;
  const auto tool_paths = resolve_hosted_tool_paths_or_emit(
    opt, BuildArtifactKind::Executable, nebula::frontend::DiagnosticStage::Build);
  if (!tool_paths.has_value())
    return 1;
  CompilerTerminationSignalScope termination_signals;
  if (!arm_hosted_termination_boundary(termination_signals, opt,
                                       nebula::frontend::DiagnosticStage::Build)) {
    return 125;
  }
  CompilerTerminationSignalScope *termination_boundary = nullptr;
#if !defined(_WIN32)
  termination_boundary = &termination_signals;
#endif
  nebula::cli::HostedArtifactTransactionPlan transaction_plan{
    out_bin, out_cpp, std::nullopt, std::nullopt,
    make_protected_inputs(loaded, *runtime_headers, *tool_paths)};
  transaction_plan.protected_directories = make_protected_directories(loaded, *runtime_headers);
  auto begun = nebula::cli::begin_hosted_artifact_transaction(transaction_plan);
  if (!begun.ok()) {
    return emit_transaction_error(begun.error, out_bin, opt,
                                  nebula::frontend::DiagnosticStage::Build);
  }
  std::unique_ptr<nebula::cli::HostedArtifactTransaction> transaction =
    std::move(begun.transaction);
  const auto hosted_toolchain = resolve_hosted_toolchain_or_emit(
    opt, BuildArtifactKind::Executable, nebula::frontend::DiagnosticStage::Build, &*tool_paths,
    termination_boundary);
  if (!hosted_toolchain.has_value()) {
    return abort_transaction(*transaction, 1, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }
  nebula::cli::HostedArtifactTransactionError protection_error;
  if (!protect_resolved_tool_dependencies(*transaction, *hosted_toolchain, protection_error)) {
    const int error_exit = emit_transaction_error(protection_error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return abort_transaction(*transaction, error_exit, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }
  const fs::path dependency_workspace_parent =
    out_bin.has_parent_path() ? out_bin.parent_path() : fs::path(".");
  NativeDependencySnapshotResult native_dependencies = discover_native_dependencies(
    opt, *hosted_toolchain, loaded, BuildArtifactKind::Executable, dependency_workspace_parent);
  if (!native_dependencies.ok()) {
    const int dependency_exit = emit_native_dependency_error(
      "failed to discover the hosted native dependency closure", native_dependencies, opt,
      nebula::frontend::DiagnosticStage::Build);
    return abort_transaction(*transaction, dependency_exit, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }
  if (!protect_native_dependencies(*transaction, *native_dependencies.snapshot, protection_error)) {
    const int error_exit = emit_transaction_error(protection_error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return abort_transaction(*transaction, error_exit, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }
  const std::optional<ArtifactBuildKey> expected = derive_build_key_or_emit(
    opt, resolved_profile, loaded.cache_key_source, nebula::frontend::DiagnosticStage::Build,
    &*hosted_toolchain, &*runtime_headers, &*native_dependencies.snapshot);
  if (!expected.has_value()) {
    return adopt_staged_outputs_and_abort(*transaction, 1, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
  }

  if (opt.reuse) {
    const ArtifactReuseAssessment assessment = assess_artifact_reuse(out_bin, *expected);
    if (assessment.disposition == ArtifactReuseDisposition::Reject) {
      auto d =
        make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-REUSE-VERIFY",
                      "cached artifact could not be safely verified: " + out_bin.string(),
                      nebula::frontend::DiagnosticStage::Build,
                      nebula::frontend::DiagnosticRisk::High, assessment.detail,
                      "run refuses to rebuild over or execute an unsafe or unstable artifact path",
                      {"remove the unsafe artifact and metadata path, then retry without --reuse"});
      emit_diagnostics({d}, opt, std::cerr);
      return abort_transaction(*transaction, 1, out_bin, opt,
                               nebula::frontend::DiagnosticStage::Build);
    }
    if (assessment.disposition == ArtifactReuseDisposition::Reusable) {
      if (!assessment.verified_content.has_value()) {
        auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-REUSE-IDENTITY",
          "reusable artifact assessment omitted its verified content identity",
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::Critical,
          "reuse cannot acquire an execution lease without the exact assessed digest",
          "Nebula refused to execute a public pathname without content binding",
          {"rebuild the artifact without --reuse"});
        emit_diagnostics({d}, opt, std::cerr);
        return abort_transaction(*transaction, 125, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }
      std::string toolchain_detail;
      if (!hosted_toolchain->revalidate(toolchain_detail)) {
        auto d = make_cli_diag(
          nebula::frontend::Severity::Error, "NBL-CLI-REUSE-TOOLCHAIN",
          "hosted toolchain changed before reuse execution",
          nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::Critical,
          toolchain_detail,
          "the cached artifact was not executed against an unstable provenance boundary",
          {"restore the resolved compiler toolchain and retry"});
        emit_diagnostics({d}, opt, std::cerr);
        return abort_transaction(*transaction, 1, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }
      const nebula::cli::HostedArtifactTransactionResult inputs_before_lease =
        transaction->revalidate_protected_inputs();
      if (!inputs_before_lease.ok()) {
        const int error_exit = emit_transaction_error(inputs_before_lease.error, out_bin, opt,
                                                      nebula::frontend::DiagnosticStage::Build);
        return abort_transaction(*transaction, error_exit, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }

      const ArtifactContentIdentity &verified = *assessment.verified_content;
      auto leased = nebula::cli::begin_verified_executable_lease(
        out_bin,
        nebula::cli::FileDigest{static_cast<std::uintmax_t>(verified.size), verified.sha256});
      if (!leased.ok()) {
        const int lease_exit = emit_executable_lease_error(
          leased.error, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
        return abort_transaction(*transaction, lease_exit, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }

      NativeDependencySnapshotResult reuse_dependencies = discover_native_dependencies(
        opt, *hosted_toolchain, loaded, BuildArtifactKind::Executable, dependency_workspace_parent);
      if (!reuse_dependencies.ok()) {
        int dependency_exit = emit_native_dependency_error(
          "failed to revalidate hosted native dependencies before reuse", reuse_dependencies, opt,
          nebula::frontend::DiagnosticStage::Build);
        dependency_exit = cleanup_executable_lease(*leased.lease, dependency_exit, out_bin, opt,
                                                   nebula::frontend::DiagnosticStage::Build);
        return abort_transaction(*transaction, dependency_exit, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }
      if (*reuse_dependencies.snapshot != *native_dependencies.snapshot) {
        NativeDependencySnapshotResult mismatch;
        mismatch.exit_code = 1;
        mismatch.detail =
          "native dependency paths or contents changed after the reusable artifact was assessed";
        int mismatch_exit =
          emit_native_dependency_error("hosted native dependencies changed before reuse execution",
                                       mismatch, opt, nebula::frontend::DiagnosticStage::Build);
        mismatch_exit = cleanup_executable_lease(*leased.lease, mismatch_exit, out_bin, opt,
                                                 nebula::frontend::DiagnosticStage::Build);
        return abort_transaction(*transaction, mismatch_exit, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }

      const nebula::cli::HostedArtifactTransactionResult inputs_after_lease =
        transaction->revalidate_protected_inputs();
      toolchain_detail.clear();
      const bool toolchain_stable = hosted_toolchain->revalidate(toolchain_detail);
      if (!inputs_after_lease.ok() || !toolchain_stable) {
        int primary_exit = 1;
        if (!inputs_after_lease.ok()) {
          primary_exit = emit_transaction_error(inputs_after_lease.error, out_bin, opt,
                                                nebula::frontend::DiagnosticStage::Build);
        }
        if (!toolchain_stable) {
          auto d = make_cli_diag(nebula::frontend::Severity::Error, "NBL-CLI-REUSE-TOOLCHAIN",
                                 "hosted toolchain changed while reuse execution was leased",
                                 nebula::frontend::DiagnosticStage::Build,
                                 nebula::frontend::DiagnosticRisk::Critical, toolchain_detail,
                                 "the leased artifact was not executed with stale provenance",
                                 {"restore the resolved compiler toolchain and retry"});
          emit_diagnostics({d}, opt, std::cerr);
        }
        primary_exit = cleanup_executable_lease(*leased.lease, primary_exit, out_bin, opt,
                                                nebula::frontend::DiagnosticStage::Build);
        return abort_transaction(*transaction, primary_exit, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
      }

      const int abort_exit =
        abort_transaction(*transaction, 0, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
      if (abort_exit != 0) {
        return cleanup_executable_lease(*leased.lease, abort_exit, out_bin, opt,
                                        nebula::frontend::DiagnosticStage::Build);
      }
      if (const std::optional<int> signal_exit =
            restore_before_user_execution(termination_signals, *leased.lease, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
          signal_exit.has_value()) {
        return *signal_exit;
      }
      auto reused = make_cli_diag(
        nebula::frontend::Severity::Note, "NBL-CLI-REUSE",
        "executing a content-verified reusable artifact", nebula::frontend::DiagnosticStage::Build,
        nebula::frontend::DiagnosticRisk::Low,
        "build inputs, toolchain, metadata, artifact digest, and private execution lease matched",
        "hosted compilation was skipped and the public pathname was not executed directly", {});
      emit_diagnostics({reused}, opt, std::cerr);
      return execute_verified_executable(*leased.lease, run_args(out_bin), out_bin, opt,
                                         nebula::frontend::DiagnosticStage::Build);
    }
  }

  CompilePipelineOptions popt;
  popt.mode = opt.mode;
  popt.profile = resolved_profile;
  popt.analysis_tier = tier;
  popt.strict_region = effective_strict_region(opt.runtime_profile, opt.strict_region, opt.no_std);
  popt.warnings_as_errors = opt.warnings_as_errors;
  popt.no_std = effective_no_std(opt.runtime_profile, opt.no_std);
  popt.runtime_profile = opt.runtime_profile;
  popt.panic_policy = opt.panic_policy;
  popt.target = opt.target;
  popt.include_lint = should_include_lint_in_build_stage(opt);
  popt.allow_cross_stage_reuse = (opt.cross_stage_reuse == CrossStageReuseMode::Safe);
  popt.disk_cache_enabled = (opt.disk_cache == DiskCacheMode::On);
  popt.disk_cache_ttl_sec = opt.disk_cache_ttl_sec;
  popt.disk_cache_max_entries = opt.disk_cache_max_entries;
  popt.disk_cache_dir = opt.disk_cache_dir;
  popt.disk_cache_prune = opt.disk_cache_prune;
  popt.budget_ms = opt.diag_budget_ms;
  popt.source_path = effective_file.string();
  popt.cache_key_source = loaded.cache_key_source;
  popt.stage = nebula::frontend::DiagnosticStage::Build;

  auto analysis = run_compile_pipeline(loaded.compile_sources, popt);
  emit_diagnostics(analysis.diags, opt, std::cerr);
  if (analysis.has_error) {
    return abort_transaction(*transaction, 1, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }

  std::string cpp;
  if (analysis.has_cached_cpp) {
    cpp = analysis.cached_cpp;
  } else if (analysis.nir_prog && analysis.rep_owner) {
    nebula::codegen::EmitOptions eopt;
    eopt.main_mode = nebula::codegen::MainMode::CallMainIfPresent;
    eopt.strict_region =
      effective_strict_region(opt.runtime_profile, opt.strict_region, opt.no_std);
    eopt.runtime_profile = opt.runtime_profile;
    eopt.target = opt.target;
    eopt.panic_policy = opt.panic_policy;
    cpp = nebula::codegen::default_backend().emit_translation_unit(*analysis.nir_prog,
                                                                   *analysis.rep_owner, eopt);
  } else {
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-CACHE-STATE",
      "build analysis result missing codegen payload", nebula::frontend::DiagnosticStage::Build,
      nebula::frontend::DiagnosticRisk::High,
      "analysis cache state is incomplete for code generation",
      "run cannot proceed without generated C++",
      {"retry without --disk-cache", "or rebuild cache with --disk-cache-prune"});
    emit_diagnostics({d}, opt, std::cerr);
    return abort_transaction(*transaction, 1, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }

  const nebula::cli::HostedArtifactStagingPaths &staging = transaction->staging_paths();
  if (!write_text_file(staging.generated_cpp, cpp)) {
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-IO002",
      "failed to write transaction-owned generated C++", nebula::frontend::DiagnosticStage::Build,
      nebula::frontend::DiagnosticRisk::High, "private hosted staging output is not writable",
      "the public generated source and artifact remain uncommitted");
    emit_diagnostics({d}, opt, std::cerr);
    return adopt_staged_outputs_and_abort(*transaction, 1, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
  }
  const nebula::cli::HostedArtifactTransactionResult generated_adopted =
    transaction->adopt_existing_staged_outputs_for_cleanup();
  if (!generated_adopted.ok()) {
    const int error_exit = emit_transaction_error(generated_adopted.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return abort_transaction(*transaction, error_exit, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }

  nebula::cli::HostedNativeDependencySnapshot compiled_native_dependencies;
  const int compile_exit = compile_cpp(
    opt, *hosted_toolchain, staging.generated_cpp, staging.artifact, CompileFlavor::Normal,
    loaded.host_cxx_sources, loaded.native_inputs, BuildArtifactKind::Executable, out_bin,
    std::nullopt, &compiled_native_dependencies);
  if (compile_exit != 0) {
    return adopt_staged_outputs_and_abort(*transaction, compile_exit, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
  }
  if (compiled_native_dependencies != *native_dependencies.snapshot) {
    NativeDependencySnapshotResult mismatch;
    mismatch.exit_code = 125;
    mismatch.detail =
      "actual compilation dependency files differ from the stable pre-compilation snapshot";
    const int mismatch_exit =
      emit_native_dependency_error("hosted native dependencies changed during compilation",
                                   mismatch, opt, nebula::frontend::DiagnosticStage::Build);
    return adopt_staged_outputs_and_abort(*transaction, mismatch_exit, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
  }
  const nebula::cli::HostedArtifactTransactionResult inputs_after_compile =
    transaction->revalidate_protected_inputs();
  if (!inputs_after_compile.ok()) {
    const int error_exit = emit_transaction_error(inputs_after_compile.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    return adopt_staged_outputs_and_abort(*transaction, error_exit, out_bin, opt,
                                          nebula::frontend::DiagnosticStage::Build);
  }

  const nebula::cli::HostedArtifactTransactionResult sealed = transaction->seal(*expected);
  if (!sealed.ok()) {
    const int error_exit =
      emit_transaction_error(sealed.error, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
    return abort_transaction(*transaction, error_exit, out_bin, opt,
                             nebula::frontend::DiagnosticStage::Build);
  }
  if (const auto interrupted = freeze_before_hosted_publication(
        termination_signals, *transaction, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
      interrupted.has_value()) {
    return finish_hosted_run_termination_handoff(*interrupted, opt,
                                                 nebula::frontend::DiagnosticStage::Build);
  }
  const nebula::cli::HostedArtifactTransactionResult committed = transaction->commit();
  if (!committed.ok()) {
    const int error_exit = emit_transaction_error(committed.error, out_bin, opt,
                                                  nebula::frontend::DiagnosticStage::Build);
    const HostedTerminationHandoff handoff =
      abort_frozen_hosted_publication(termination_signals, *transaction, error_exit, out_bin, opt,
                                      nebula::frontend::DiagnosticStage::Build);
    return finish_hosted_run_termination_handoff(handoff, opt,
                                                 nebula::frontend::DiagnosticStage::Build);
  }
  const std::optional<nebula::cli::FileDigest> sealed_digest =
    transaction->sealed_artifact_digest();
  if (!sealed_digest.has_value()) {
    const nebula::cli::HostedArtifactTransactionResult finished = transaction->finish();
    if (!finished.ok()) {
      (void)emit_transaction_error(finished.error, out_bin, opt,
                                   nebula::frontend::DiagnosticStage::Build);
    }
    auto d = make_cli_diag(
      nebula::frontend::Severity::Error, "NBL-CLI-EXEC-LEASE-IDENTITY",
      "committed artifact omitted its sealed content identity",
      nebula::frontend::DiagnosticStage::Build, nebula::frontend::DiagnosticRisk::Critical,
      "the hosted publication transaction could not provide the digest needed by execution",
      "Nebula refused to execute the public artifact path directly",
      {"retry the build after checking transaction diagnostics"});
    emit_diagnostics({d}, opt, std::cerr);
    const HostedTerminationHandoff handoff = restore_hosted_termination_boundary(
      termination_signals, finished.ok(), opt, nebula::frontend::DiagnosticStage::Build);
    if (handoff.exit_code != 0) {
      return finish_hosted_run_termination_handoff(handoff, opt,
                                                   nebula::frontend::DiagnosticStage::Build);
    }
    return 125;
  }
  auto leased = nebula::cli::begin_verified_executable_lease(out_bin, sealed_digest);
  if (!leased.ok()) {
    const int lease_exit = emit_executable_lease_error(leased.error, out_bin, opt,
                                                       nebula::frontend::DiagnosticStage::Build);
    const nebula::cli::HostedArtifactTransactionResult finished = transaction->finish();
    if (!finished.ok()) {
      (void)emit_transaction_error(finished.error, out_bin, opt,
                                   nebula::frontend::DiagnosticStage::Build);
    }
    const HostedTerminationHandoff handoff = restore_hosted_termination_boundary(
      termination_signals, finished.ok(), opt, nebula::frontend::DiagnosticStage::Build);
    if (handoff.exit_code != 0) {
      return finish_hosted_run_termination_handoff(handoff, opt,
                                                   nebula::frontend::DiagnosticStage::Build);
    }
    return lease_exit;
  }
  const nebula::cli::HostedArtifactTransactionResult finished = transaction->finish();
  if (!finished.ok()) {
    (void)emit_transaction_error(finished.error, out_bin, opt,
                                 nebula::frontend::DiagnosticStage::Build);
    (void)cleanup_executable_lease(*leased.lease, 125, out_bin, opt,
                                   nebula::frontend::DiagnosticStage::Build);
    const HostedTerminationHandoff handoff = restore_hosted_termination_boundary(
      termination_signals, false, opt, nebula::frontend::DiagnosticStage::Build);
    return finish_hosted_run_termination_handoff(handoff, opt,
                                                 nebula::frontend::DiagnosticStage::Build);
  }
  if (const std::optional<int> signal_exit = restore_before_user_execution(
        termination_signals, *leased.lease, out_bin, opt, nebula::frontend::DiagnosticStage::Build);
      signal_exit.has_value()) {
    return *signal_exit;
  }
  std::cerr << "wrote: " << quote_cli_log_value(out_cpp.string()) << "\n";
  std::cerr << "wrote artifact: " << quote_cli_log_value(out_bin.string()) << "\n";
  std::cerr << "wrote metadata: " << quote_cli_log_value(artifact_metadata_path(out_bin).string())
            << "\n";
  return execute_verified_executable(*leased.lease, run_args(out_bin), out_bin, opt,
                                     nebula::frontend::DiagnosticStage::Build);
}
