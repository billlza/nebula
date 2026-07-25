#include "cli/hosted_artifact_transaction.hpp"
#include "cli/hosted_artifact_transaction_test_hooks.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace {

namespace fs = std::filesystem;
using nebula::cli::begin_hosted_artifact_transaction;
using nebula::cli::HostedArtifactProtectedInput;
using nebula::cli::HostedArtifactTransactionErrorCode;
using nebula::cli::HostedArtifactTransactionPlan;
using nebula::cli::hosted_artifact_transaction_testing::FaultPoint;
using nebula::cli::hosted_artifact_transaction_testing::
  inject_before_final_protected_input_revalidation_once;
using nebula::cli::hosted_artifact_transaction_testing::inject_fault_once;

bool expect(bool condition, const std::string &message) {
  if (!condition)
    std::cerr << "hosted-artifact-transaction-test: " << message << '\n';
  return condition;
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error);
    if (error)
      return;
#if defined(_WIN32)
    const auto process = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const auto process = static_cast<unsigned long>(::getpid());
#endif
    for (unsigned long attempt = 0U; attempt < 128U; ++attempt) {
      path_ = base / ("nebula-hosted-transaction-test-" + std::to_string(process) + "-" +
                      std::to_string(attempt));
      if (fs::create_directory(path_, error))
        return;
      if (error && error != std::errc::file_exists) {
        path_.clear();
        return;
      }
      error.clear();
    }
    path_.clear();
  }

  ~TemporaryDirectory() {
    if (path_.empty())
      return;
    std::error_code error;
    fs::remove_all(path_, error);
    if (error)
      std::cerr << "hosted-artifact-transaction-test: cleanup failed: " << error.message() << '\n';
  }

  [[nodiscard]] const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

bool write_file(const fs::path &path, std::string_view payload) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  output.close();
  return output.good();
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
bool is_expected_windows_delete_denial(DWORD error) {
  return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED;
}

bool expect_staging_directory_identity_lock(const fs::path &directory) {
  bool ok = true;
  HANDLE deletion = ::CreateFileW(
    directory.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const DWORD deletion_error = deletion == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
  ok &=
    expect(deletion == INVALID_HANDLE_VALUE && is_expected_windows_delete_denial(deletion_error),
           "active staging directory granted a competing delete-capable handle");
  if (deletion != INVALID_HANDLE_VALUE) {
    ok &=
      expect(::CloseHandle(deletion) != 0, "failed to close unexpected staging deletion handle");
  }

  fs::path renamed = directory;
  renamed += L".rename-attempt";
  const BOOL moved = ::MoveFileExW(directory.c_str(), renamed.c_str(), MOVEFILE_WRITE_THROUGH);
  const DWORD move_error = moved == 0 ? ::GetLastError() : ERROR_SUCCESS;
  ok &= expect(moved == 0 && is_expected_windows_delete_denial(move_error),
               "active staging directory could be renamed despite its identity lock");
  if (moved != 0) {
    ok &= expect(::MoveFileExW(renamed.c_str(), directory.c_str(), MOVEFILE_WRITE_THROUGH) != 0,
                 "failed to restore unexpectedly renamed staging directory");
  }
  return ok;
}
#endif

ArtifactBuildKey build_key() {
  ArtifactBuildKey key;
  key.build_inputs_sha256 = std::string(64U, 'a');
  key.mode = "debug";
  key.profile = "auto";
  key.artifact_kind = "executable";
  key.compiler_schema_version = 9;
  key.cache_schema_version = 4;
  key.strict_region = true;
  key.warnings_as_errors = true;
  key.no_std = false;
  key.runtime_profile = "hosted";
  key.target = "host";
  key.panic_policy = "abort";
  return key;
}

bool run_successful_replacement_test(const fs::path &root) {
  const fs::path input = root / "main.nb";
  const fs::path artifact = root / "app.out";
  const fs::path generated = root / "app.cpp";
  const fs::path discovered_tool = root / "tool.bin";
  bool ok = write_file(input, "fn main() -> Void {}\n") && write_file(artifact, "old-artifact") &&
            write_file(generated, "old-generated") &&
            write_file(discovered_tool, "toolchain-identity");
  ok &= expect(ok, "failed to create successful replacement fixtures");

  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto protected_once = begun.transaction->protect_additional_inputs({discovered_tool});
  ok &= expect(protected_once.ok(), "failed to protect a discovered toolchain input");
  const auto protected_twice = begun.transaction->protect_additional_inputs({discovered_tool});
  ok &= expect(protected_twice.ok(), "repeated additional input protection was not idempotent");
  const auto output_conflict = begun.transaction->protect_additional_inputs({artifact});
  ok &= expect(!output_conflict.ok() &&
                 output_conflict.error.code == HostedArtifactTransactionErrorCode::PathConflict,
               "additional input protection accepted a public output");
  const auto staging = begun.transaction->staging_paths();
  ok &= expect(staging.artifact.filename() == artifact.filename(),
               "artifact basename was not preserved in staging");
  ok &= expect(staging.artifact.parent_path() != artifact.parent_path(),
               "artifact was not isolated in a private staging directory");
#if defined(_WIN32)
  ok &= expect_staging_directory_identity_lock(staging.artifact.parent_path());
#endif
  ok &= expect(write_file(staging.artifact, "new-artifact"), "failed to write staged artifact");
  ok &=
    expect(write_file(staging.generated_cpp, "new-generated"), "failed to write generated source");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  const auto late_protection = begun.transaction->protect_additional_inputs({discovered_tool});
  ok &= expect(!late_protection.ok() &&
                 late_protection.error.code == HostedArtifactTransactionErrorCode::InvalidState,
               "additional input protection was accepted after seal");
  const auto committed = begun.transaction->commit();
  ok &= expect(committed.ok(), "commit failed: " + committed.error.detail);
  if (!committed.ok())
    return false;
  ok &= expect(read_file(artifact) == "new-artifact", "artifact replacement was not committed");
  ok &= expect(read_file(generated) == "new-generated", "generated replacement was not committed");
  const ArtifactMetadataResult metadata = read_artifact_metadata(artifact);
  ok &= expect(metadata.ok(), "published v6 metadata could not be parsed");
  if (metadata.ok()) {
    ok &= expect(metadata.value->content.size == 12U, "published artifact size is incorrect");
    ok &= expect(metadata.value->build.build_inputs_sha256 == std::string(64U, 'a'),
                 "published build key was not preserved");
  }
  const auto finished = begun.transaction->finish();
  ok &= expect(finished.ok(), "finish failed: " + finished.error.detail);
  ok &= expect(!fs::exists(staging.artifact.parent_path()), "staging directory survived finish");
  return ok;
}

bool run_conflict_tests(const fs::path &root) {
  const fs::path input = root / "protected.nb";
  bool ok = expect(write_file(input, "protected"), "failed to write protected input");

  HostedArtifactTransactionPlan lexical{
    input, root / "lexical.cpp", std::nullopt, std::nullopt, {input}};
  auto lexical_result = begin_hosted_artifact_transaction(lexical);
  ok &= expect(!lexical_result.ok() &&
                 lexical_result.error.code == HostedArtifactTransactionErrorCode::PathConflict,
               "lexical output/input collision was not rejected");
  ok &= expect(!fs::exists(fs::path(input.string() + ".nebula.lock")),
               "lexical rejection created a lock file");

  const fs::path hardlink = root / "hardlink.out";
  std::error_code error;
  fs::create_hard_link(input, hardlink, error);
  if (!error) {
    HostedArtifactTransactionPlan linked{
      hardlink, root / "linked.cpp", std::nullopt, std::nullopt, {input}};
    auto linked_result = begin_hosted_artifact_transaction(linked);
    ok &= expect(!linked_result.ok() &&
                   linked_result.error.code == HostedArtifactTransactionErrorCode::PathConflict,
                 "hard-link output/input collision was not rejected");
  }

  const fs::path symlink = root / "symlink.out";
  error.clear();
  fs::create_symlink(input.filename(), symlink, error);
  if (!error) {
    HostedArtifactTransactionPlan linked{
      symlink, root / "symlink.cpp", std::nullopt, std::nullopt, {input}};
    auto linked_result = begin_hosted_artifact_transaction(linked);
    ok &= expect(!linked_result.ok() &&
                   linked_result.error.code == HostedArtifactTransactionErrorCode::UnsafePath,
                 "symlink output was not rejected");
  }

  const fs::path reserved_output = root / "protocol.NEBULA.LOCK";
  HostedArtifactTransactionPlan reserved{
    reserved_output, root / "protocol.cpp", std::nullopt, std::nullopt, {input}};
  auto reserved_result = begin_hosted_artifact_transaction(reserved);
  ok &= expect(!reserved_result.ok() &&
                 reserved_result.error.code == HostedArtifactTransactionErrorCode::PathConflict,
               "case-variant reserved transaction-lock suffix was accepted as an output");

  HostedArtifactTransactionPlan digest_mismatch{
    root / "digest.out",
    root / "digest.cpp",
    std::nullopt,
    std::nullopt,
    {HostedArtifactProtectedInput{input, nebula::cli::FileDigest{999U, std::string(64U, '0')}}}};
  auto digest_result = begin_hosted_artifact_transaction(digest_mismatch);
  ok &= expect(!digest_result.ok() && digest_result.error.code ==
                                        HostedArtifactTransactionErrorCode::ConcurrentModification,
               "protected input digest mismatch was accepted at transaction begin");
  return ok;
}

bool run_large_additional_input_index_test(const fs::path &root) {
  const fs::path directory = root / "large-additional-input-index";
  const fs::path inputs = directory / "inputs";
  std::error_code error;
  fs::create_directories(inputs, error);
  bool ok = expect(!error, "failed to create large additional-input fixture directories");
  const fs::path initial_input = directory / "initial.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= expect(write_file(initial_input, "initial-protected-input"),
               "failed to create initial protected-input fixture");
  const fs::path initial_hardlink = directory / "initial-hardlink.nb";
  fs::create_hard_link(initial_input, initial_hardlink, error);
  const bool initial_hardlink_supported = !error;
  HostedArtifactTransactionPlan plan{
    artifact, generated, std::nullopt, std::nullopt, {initial_input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &=
    expect(begun.ok(), "large additional-input transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  if (initial_hardlink_supported) {
    const auto initial_alias = begun.transaction->protect_additional_inputs({initial_hardlink});
    ok &= expect(initial_alias.ok(),
                 "pre-existing hard link to an initial protected input was not idempotent");
  }

  const fs::path atomic_candidate = inputs / "atomic-candidate.hpp";
  ok &= expect(write_file(atomic_candidate, "atomic-original\n"),
               "failed to create atomic additional-input fixture");
  const auto rejected_batch = begun.transaction->protect_additional_inputs(
    {atomic_candidate, inputs / "missing-after-valid.hpp"});
  ok &= expect(!rejected_batch.ok(), "partially invalid additional-input batch was accepted");
  ok &= expect(write_file(atomic_candidate, "atomic-mutated-after-rejection\n"),
               "failed to mutate rejected additional-input fixture");
  const auto atomic_revalidation = begun.transaction->revalidate_protected_inputs();
  ok &= expect(atomic_revalidation.ok(),
               "failed additional-input batch partially changed transaction state");

  constexpr std::size_t kInputCount = 1024U;
  std::vector<HostedArtifactProtectedInput> protected_inputs;
  protected_inputs.reserve(kInputCount + 2U);
  for (std::size_t index = 0U; index < kInputCount; ++index) {
    const fs::path input = inputs / ("header-" + std::to_string(index) + ".hpp");
    ok &= expect(write_file(input, "#define NEBULA_BATCH_VALUE " + std::to_string(index) + "\n"),
                 "failed to create large additional-input file");
    protected_inputs.emplace_back(input);
  }
  protected_inputs.push_back(protected_inputs.front());

  const fs::path hardlink = inputs / "header-hardlink.hpp";
  error.clear();
  fs::create_hard_link(protected_inputs.front().path, hardlink, error);
  const bool hardlink_supported = !error;
  if (hardlink_supported)
    protected_inputs.emplace_back(hardlink);

  const auto protected_batch = begun.transaction->protect_additional_inputs(protected_inputs);
  ok &=
    expect(protected_batch.ok(), "large duplicate/hard-link additional-input batch was rejected: " +
                                   protected_batch.error.detail);
  if (!protected_batch.ok()) {
    (void)begun.transaction->abort();
    return false;
  }
  if (hardlink_supported) {
    const auto repeated_hardlink = begun.transaction->protect_additional_inputs({hardlink});
    ok &= expect(repeated_hardlink.ok(), "repeated protected hard link was not idempotent");
  }

  const HostedArtifactProtectedInput incorrect_digest{
    protected_inputs.front().path, nebula::cli::FileDigest{0U, std::string(64U, '0')}};
  const auto mismatch = begun.transaction->protect_additional_inputs({incorrect_digest});
  ok &= expect(!mismatch.ok() &&
                 mismatch.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "additional-input index bypassed an expected-digest mismatch");

  ok &= expect(write_file(protected_inputs[kInputCount - 1U].path,
                          "mutated-after-protection-with-a-different-size\n"),
               "failed to mutate indexed protected-input fixture");
  const auto changed = begun.transaction->revalidate_protected_inputs();
  ok &= expect(!changed.ok() &&
                 changed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "indexed additional inputs were not retained as the transaction source of truth");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(),
               "large additional-input transaction abort failed: " + aborted.error.detail);
  return ok;
}

bool run_additional_input_hardlink_transition_test(const fs::path &root) {
  const fs::path directory = root / "additional-input-hardlink-transition";
  std::error_code error;
  fs::create_directories(directory, error);
  bool ok = expect(!error, "failed to create hard-link transition fixture directory");
  const fs::path initial_input = directory / "initial.nb";
  const fs::path additional_input = directory / "header.hpp";
  ok &= expect(write_file(initial_input, "initial") &&
                 write_file(additional_input, "#define ANSWER 41\n"),
               "failed to write hard-link transition fixtures");
  HostedArtifactTransactionPlan plan{
    directory / "app.out", directory / "app.cpp", std::nullopt, std::nullopt, {initial_input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "hard-link transition transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto protected_result = begun.transaction->protect_additional_inputs({additional_input});
  ok &= expect(protected_result.ok(), "failed to protect hard-link transition input");
  if (!protected_result.ok()) {
    (void)begun.transaction->abort();
    return false;
  }

  const fs::path late_hardlink = directory / "late-hardlink.hpp";
  fs::create_hard_link(additional_input, late_hardlink, error);
  if (!error) {
    const auto changed = begun.transaction->protect_additional_inputs({late_hardlink});
    ok &=
      expect(!changed.ok() &&
               changed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification &&
               changed.error.operation == "reprotect-input",
             "link-count change was accepted as an idempotent hard-link duplicate");
    const auto revalidated = begun.transaction->revalidate_protected_inputs();
    ok &= expect(!revalidated.ok() && revalidated.error.code ==
                                        HostedArtifactTransactionErrorCode::ConcurrentModification,
                 "protected-input revalidation accepted a post-protection hard link");
  }
  const auto aborted = begun.transaction->abort();
  ok &=
    expect(aborted.ok(), "hard-link transition transaction abort failed: " + aborted.error.detail);
  return ok;
}

#if !defined(_WIN32)
bool run_additional_input_symlink_retarget_test(const fs::path &root) {
  const fs::path directory = root / "additional-input-symlink-retarget";
  const fs::path first = directory / "first";
  const fs::path second = directory / "second";
  std::error_code error;
  fs::create_directories(first, error);
  fs::create_directories(second, error);
  bool ok = expect(!error, "failed to create additional-input retarget fixtures");
  const fs::path initial_input = directory / "initial.nb";
  ok &= expect(write_file(initial_input, "initial") &&
                 write_file(first / "answer.hpp", "#define ANSWER 41\n") &&
                 write_file(second / "answer.hpp", "#define ANSWER 41\n"),
               "failed to write additional-input retarget fixtures");
  const fs::path alias = directory / "current";
  fs::create_directory_symlink(first.filename(), alias, error);
  ok &= expect(!error, "failed to create additional-input directory symlink");

  HostedArtifactTransactionPlan plan{
    directory / "app.out", directory / "app.cpp", std::nullopt, std::nullopt, {initial_input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &=
    expect(begun.ok(), "additional-input retarget transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const fs::path spelling = alias / "answer.hpp";
  const auto protected_result = begun.transaction->protect_additional_inputs({spelling});
  ok &= expect(protected_result.ok(), "failed to protect symlink-spelled additional input");
  if (!protected_result.ok()) {
    (void)begun.transaction->abort();
    return false;
  }

  fs::remove(alias, error);
  ok &= expect(!error, "failed to remove additional-input directory symlink");
  fs::create_directory_symlink(second.filename(), alias, error);
  ok &= expect(!error, "failed to retarget additional-input directory symlink");
  const auto retargeted = begun.transaction->protect_additional_inputs({spelling});
  ok &=
    expect(!retargeted.ok() &&
             retargeted.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification &&
             retargeted.error.operation == "reprotect-input",
           "normalized-spelling index accepted a retargeted protected input");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(),
               "additional-input retarget transaction abort failed: " + aborted.error.detail);
  return ok;
}

bool run_untrusted_output_parent_test(const fs::path &root) {
  const fs::path input = root / "untrusted-parent-input.nb";
  const fs::path shared = root / "untrusted-parent";
  std::error_code error;
  fs::create_directory(shared, error);
  bool ok =
    expect(!error && write_file(input, "protected"), "failed to create untrusted-parent fixtures");
  ok &= expect(::chmod(shared.c_str(), 0777) == 0, "failed to make output parent shared-writable");
  HostedArtifactTransactionPlan plan{
    shared / "app.out", shared / "app.cpp", std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(!begun.ok() && begun.error.code == HostedArtifactTransactionErrorCode::UnsafePath,
               "non-sticky shared-writable output parent was accepted");
  ok &= expect(!fs::exists(shared / "app.out.nebula.lock") &&
                 !fs::exists(shared / "app.cpp.nebula.lock"),
               "untrusted output parent rejection created transaction locks");
  ok &= expect(::chmod(shared.c_str(), S_IRWXU) == 0,
               "failed to restore untrusted-parent fixture permissions");
  return ok;
}

fs::path transaction_lock_path(const fs::path &output) {
  fs::path lock = output;
  lock += ".nebula.lock";
  return lock;
}

std::vector<fs::path> transaction_lock_paths(const fs::path &artifact, const fs::path &generated) {
  return {transaction_lock_path(artifact), transaction_lock_path(generated),
          transaction_lock_path(artifact_metadata_path(artifact))};
}

bool run_lock_file_policy_test(const fs::path &root) {
  const fs::path directory = root / "lock-file-policy";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error && ::chmod(directory.c_str(), 01777) == 0,
                   "failed to create sticky lock-policy directory");
  const fs::path input = root / "lock-file-policy-input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= expect(write_file(input, "protected"), "failed to create lock-policy protected input");
  const std::vector<fs::path> locks = transaction_lock_paths(artifact, generated);
  for (const fs::path &lock : locks) {
    ok &= expect(write_file(lock, "") && ::chmod(lock.c_str(), 0666) == 0,
                 "failed to create an unsafe pre-existing transaction lock");
  }

  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto unsafe = begin_hosted_artifact_transaction(plan);
  ok &= expect(!unsafe.ok() && unsafe.error.code == HostedArtifactTransactionErrorCode::UnsafePath,
               "world-accessible pre-existing transaction lock was accepted");

  for (const fs::path &lock : locks) {
    ok &= expect(::chmod(lock.c_str(), 0600) == 0,
                 "failed to repair pre-existing transaction lock permissions");
  }
  auto valid = begin_hosted_artifact_transaction(plan);
  ok &= expect(valid.ok(),
               "private same-owner pre-existing locks were rejected: " + valid.error.detail);
  if (valid.ok()) {
    for (const fs::path &lock : locks) {
      struct stat status{};
      const bool inspected = ::lstat(lock.c_str(), &status) == 0;
      ok &= expect(inspected && S_ISREG(status.st_mode) && status.st_uid == ::geteuid() &&
                     status.st_nlink == 1 && (status.st_mode & 07777) == 0600,
                   "accepted transaction lock did not retain the secure file policy");
    }
    const auto aborted = valid.transaction->abort();
    ok &= expect(aborted.ok(),
                 "valid pre-existing lock transaction could not abort: " + aborted.error.detail);
    for (const fs::path &lock : locks) {
      ok &= expect(fs::exists(lock), "abort removed a persistent transaction lock entry");
    }
  }

  const fs::path linked_artifact = directory / "linked.out";
  const fs::path linked_generated = directory / "linked.cpp";
  const fs::path linked_lock = transaction_lock_path(linked_generated);
  const fs::path linked_alias = directory / "attacker-lock-alias";
  ok &= expect(write_file(linked_lock, "") && ::chmod(linked_lock.c_str(), 0600) == 0,
               "failed to create hard-linked transaction lock fixture");
  fs::create_hard_link(linked_lock, linked_alias, error);
  ok &= expect(!error, "failed to create transaction lock hard-link fixture");
  HostedArtifactTransactionPlan linked_plan{
    linked_artifact, linked_generated, std::nullopt, std::nullopt, {input}};
  auto linked = begin_hosted_artifact_transaction(linked_plan);
  ok &= expect(!linked.ok() && linked.error.code == HostedArtifactTransactionErrorCode::UnsafePath,
               "multiply-linked pre-existing transaction lock was accepted");
  ok &= expect(::chmod(directory.c_str(), 0700) == 0,
               "failed to restore lock-policy directory permissions");
  return ok;
}

bool run_lock_entry_replacement_test(const fs::path &root) {
  const fs::path directory = root / "lock-entry-replacement";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error && ::chmod(directory.c_str(), 01777) == 0,
                   "failed to create sticky lock-replacement directory");
  const fs::path input = root / "lock-entry-replacement-input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= expect(write_file(input, "protected"), "failed to create lock-replacement protected input");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "lock-replacement transaction begin failed: " + begun.error.detail);
  if (!begun.ok()) {
    (void)::chmod(directory.c_str(), 0700);
    return false;
  }
  const auto staging = begun.transaction->staging_paths();
  ok &= expect(write_file(staging.artifact, "new-artifact") &&
                 write_file(staging.generated_cpp, "new-generated"),
               "failed to create lock-replacement staged outputs");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "lock-replacement transaction seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;

  const fs::path replaced_lock = transaction_lock_path(artifact);
  fs::remove(replaced_lock, error);
  ok &= expect(!error && write_file(replaced_lock, "replacement-lock") &&
                 ::chmod(replaced_lock.c_str(), 0600) == 0,
               "failed to replace the acquired transaction lock entry");
  const auto committed = begun.transaction->commit();
  ok &=
    expect(!committed.ok() &&
             committed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification &&
             committed.error.operation == "revalidate-lock-before-commit",
           "commit did not reject a replaced transaction lock entry");
  ok &= expect(!fs::exists(artifact), "replaced transaction lock allowed artifact publication");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "lock-replacement transaction abort failed: " + aborted.error.detail);
  ok &= expect(read_file(replaced_lock) == "replacement-lock",
               "abort removed the replacement lock entry it did not own");
  ok &= expect(::chmod(directory.c_str(), 0700) == 0,
               "failed to restore lock-replacement directory permissions");
  return ok;
}

bool run_finish_lock_revalidation_test(const fs::path &root) {
  const fs::path directory = root / "finish-lock-revalidation";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error && ::chmod(directory.c_str(), 01777) == 0,
                   "failed to create sticky finish-lock directory");
  const fs::path input = root / "finish-lock-revalidation-input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= expect(write_file(input, "protected"), "failed to create finish-lock protected input");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "finish-lock transaction begin failed: " + begun.error.detail);
  if (!begun.ok()) {
    (void)::chmod(directory.c_str(), 0700);
    return false;
  }
  const auto staging = begun.transaction->staging_paths();
  ok &= expect(write_file(staging.artifact, "new-artifact") &&
                 write_file(staging.generated_cpp, "new-generated"),
               "failed to create finish-lock staged outputs");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "finish-lock transaction seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  const auto committed = begun.transaction->commit();
  ok &= expect(committed.ok(), "finish-lock transaction commit failed: " + committed.error.detail);
  if (!committed.ok())
    return false;

  const fs::path changed_lock = transaction_lock_path(artifact);
  ok &= expect(::chmod(changed_lock.c_str(), 0644) == 0,
               "failed to weaken acquired transaction lock permissions");
  const auto unsafe_finish = begun.transaction->finish();
  ok &= expect(!unsafe_finish.ok() &&
                 unsafe_finish.error.code ==
                   HostedArtifactTransactionErrorCode::ConcurrentModification &&
                 unsafe_finish.error.operation == "revalidate-lock-before-finish",
               "finish did not reject weakened transaction lock permissions");
  ok &= expect(::chmod(changed_lock.c_str(), 0600) == 0,
               "failed to restore transaction lock permissions");
  const auto finished = begun.transaction->finish();
  ok &= expect(finished.ok(), "finish did not recover after lock permissions were restored: " +
                                finished.error.detail);
  ok &= expect(::chmod(directory.c_str(), 0700) == 0,
               "failed to restore finish-lock directory permissions");
  return ok;
}
#endif

#if defined(__APPLE__)
bool refresh_or_add_darwin_provenance(const fs::path &path) {
  constexpr std::string_view name = "com.apple.provenance";
  errno = 0;
  const ssize_t required = ::getxattr(path.c_str(), name.data(), nullptr, 0U, 0U, 0);
  std::vector<std::uint8_t> value;
  if (required < 0) {
    if (errno != ENOATTR)
      return false;
    value = {0x01U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
  } else {
    if (required == 0 || required > 4096)
      return false;
    value.resize(static_cast<std::size_t>(required));
    if (::getxattr(path.c_str(), name.data(), value.data(), value.size(), 0U, 0) != required)
      return false;
  }
  return ::setxattr(path.c_str(), name.data(), value.data(), value.size(), 0U, 0) == 0;
}

bool run_darwin_metadata_security_transition_tests(const fs::path &root) {
  const fs::path directory = root / "darwin-metadata-security";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create Darwin metadata-security directory");
  const fs::path input = directory / "input.nb";
  ok &= write_file(input, "protected");

  {
    const fs::path artifact = directory / "provenance.out";
    HostedArtifactTransactionPlan plan{
      artifact, directory / "provenance.cpp", std::nullopt, std::nullopt, {input}};
    auto begun = begin_hosted_artifact_transaction(plan);
    ok &= expect(begun.ok(), "Darwin provenance transaction begin failed: " + begun.error.detail);
    if (!begun.ok())
      return false;
    const auto staging = begun.transaction->staging_paths();
    ok &=
      write_file(staging.artifact, "artifact") && write_file(staging.generated_cpp, "generated");
    const auto sealed = begun.transaction->seal(build_key());
    ok &= expect(sealed.ok(), "Darwin provenance transaction seal failed: " + sealed.error.detail);
    if (!sealed.ok())
      return false;
    ok &= expect(refresh_or_add_darwin_provenance(staging.metadata),
                 "failed to create a deterministic Darwin provenance transition");
    const auto committed = begun.transaction->commit();
    ok &= expect(committed.ok(),
                 "controlled Darwin provenance transition was rejected: " + committed.error.detail);
    if (!committed.ok())
      return false;
    const auto finished = begun.transaction->finish();
    ok &= expect(finished.ok(),
                 "Darwin provenance transaction finish failed: " + finished.error.detail);
  }

  {
    const fs::path artifact = directory / "unknown-xattr.out";
    HostedArtifactTransactionPlan plan{
      artifact, directory / "unknown-xattr.cpp", std::nullopt, std::nullopt, {input}};
    auto begun = begin_hosted_artifact_transaction(plan);
    ok &= expect(begun.ok(), "unknown-xattr transaction begin failed: " + begun.error.detail);
    if (!begun.ok())
      return false;
    const auto staging = begun.transaction->staging_paths();
    ok &=
      write_file(staging.artifact, "artifact") && write_file(staging.generated_cpp, "generated");
    const auto sealed = begun.transaction->seal(build_key());
    ok &= expect(sealed.ok(), "unknown-xattr transaction seal failed: " + sealed.error.detail);
    if (!sealed.ok())
      return false;
    constexpr std::array<std::uint8_t, 3U> value{0x01U, 0x02U, 0x03U};
    ok &= expect(::setxattr(staging.metadata.c_str(), "user.nebula.transaction-test", value.data(),
                            value.size(), 0U, 0) == 0,
                 "failed to add the unknown metadata xattr fixture");
    const auto committed = begun.transaction->commit();
    ok &= expect(!committed.ok() && committed.error.code ==
                                      HostedArtifactTransactionErrorCode::ConcurrentModification,
                 "metadata accepted an unknown extended-attribute transition");
    const auto aborted = begun.transaction->abort();
    ok &= expect(aborted.ok(), "unknown-xattr transaction abort failed: " + aborted.error.detail);
  }

  {
    const fs::path artifact = directory / "metadata-content.out";
    HostedArtifactTransactionPlan plan{
      artifact, directory / "metadata-content.cpp", std::nullopt, std::nullopt, {input}};
    auto begun = begin_hosted_artifact_transaction(plan);
    ok &= expect(begun.ok(), "metadata-content transaction begin failed: " + begun.error.detail);
    if (!begun.ok())
      return false;
    const auto staging = begun.transaction->staging_paths();
    ok &=
      write_file(staging.artifact, "artifact") && write_file(staging.generated_cpp, "generated");
    const auto sealed = begun.transaction->seal(build_key());
    ok &= expect(sealed.ok(), "metadata-content transaction seal failed: " + sealed.error.detail);
    if (!sealed.ok())
      return false;
    struct stat sealed_status{};
    ok &= expect(::stat(staging.metadata.c_str(), &sealed_status) == 0,
                 "failed to stat sealed metadata content fixture");
    std::string changed = read_file(staging.metadata);
    ok &= expect(!changed.empty(), "sealed metadata content fixture is empty");
    if (changed.empty())
      return false;
    changed.back() = changed.back() == '\n' ? ' ' : static_cast<char>(changed.back() ^ 0x01);
    ok &= expect(write_file(staging.metadata, changed), "failed to mutate sealed metadata content");
    const timespec timestamps[2] = {sealed_status.st_atimespec, sealed_status.st_mtimespec};
    ok &= expect(::utimensat(AT_FDCWD, staging.metadata.c_str(), timestamps, 0) == 0,
                 "failed to restore the metadata fixture timestamps");
    const auto committed = begun.transaction->commit();
    ok &= expect(!committed.ok() && committed.error.code ==
                                      HostedArtifactTransactionErrorCode::ConcurrentModification,
                 "metadata accepted same-size content drift with restored mtime");
    const auto aborted = begun.transaction->abort();
    ok &=
      expect(aborted.ok(), "metadata-content transaction abort failed: " + aborted.error.detail);
  }
  return ok;
}
#endif

bool run_mutation_and_abort_test(const fs::path &root) {
  const fs::path input = root / "mutation.nb";
  const fs::path artifact = root / "mutation.out";
  const fs::path generated = root / "mutation.cpp";
  bool ok = write_file(input, "input");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "mutation transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "sealed") && write_file(staging.generated_cpp, "generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "mutation transaction seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  const fs::path displaced = staging.artifact.parent_path() / "displaced";
  std::error_code error;
  fs::rename(staging.artifact, displaced, error);
  ok &= expect(!error, "failed to displace sealed artifact fixture");
  ok &= expect(write_file(staging.artifact, "replacement"), "failed to create replacement fixture");
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok() &&
                 committed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "sealed staging path replacement was not rejected");
  const auto aborted = begun.transaction->abort();
  ok &= expect(!aborted.ok() &&
                 aborted.error.code == HostedArtifactTransactionErrorCode::CleanupIncomplete,
               "abort did not report the changed staging identity");
  ok &= expect(read_file(staging.artifact) == "replacement",
               "abort removed a path whose identity changed");
  fs::remove_all(staging.artifact.parent_path(), error);
  return ok;
}

bool run_explicit_failed_build_adoption_test(const fs::path &root) {
  const fs::path directory = root / "failed-build-adoption";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create failed-build-adoption directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "protected");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "failed-build-adoption begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.generated_cpp, "generated-before-compiler-failure");
  ok &= write_file(staging.artifact, "partial-compiler-output");
  const auto adopted = begun.transaction->adopt_existing_staged_outputs_for_cleanup();
  ok &= expect(adopted.ok(), "failed-build outputs could not be explicitly adopted");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "explicitly adopted failed build could not be cleaned");
  ok &= expect(!fs::exists(staging.artifact.parent_path()),
               "adopted partial artifact staging directory survived abort");
  ok &= expect(!fs::exists(staging.generated_cpp.parent_path()),
               "adopted generated-source staging directory survived abort");
  return ok;
}

bool run_unadopted_unknown_output_preservation_test(const fs::path &root) {
  const fs::path directory = root / "unknown-output-preservation";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create unknown-output-preservation directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "protected");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "unknown-output begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "identity-not-adopted-by-transaction");
  const auto aborted = begun.transaction->abort();
  ok &= expect(!aborted.ok() &&
                 aborted.error.code == HostedArtifactTransactionErrorCode::CleanupIncomplete,
               "abort silently removed an unadopted staged output");
  ok &= expect(read_file(staging.artifact) == "identity-not-adopted-by-transaction",
               "abort removed or changed an unadopted staged output");
  fs::remove_all(staging.artifact.parent_path(), error);
  fs::remove_all(staging.generated_cpp.parent_path(), error);
  return ok;
}

bool run_fault_rollback_test(const fs::path &root, FaultPoint point, std::string_view name) {
  const fs::path directory = root / std::string(name);
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create fault-test directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "protected") && write_file(artifact, "old-artifact") &&
        write_file(generated, "old-generated");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "fault transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "fault transaction seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  inject_fault_once(point);
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok(), "injected transaction failure unexpectedly committed");
  ok &=
    expect(read_file(artifact) == "old-artifact", "rollback did not restore the prior artifact");
  ok &= expect(read_file(generated) == "old-generated",
               "rollback did not restore the prior generated source");
  ok &=
    expect(!fs::exists(artifact_metadata_path(artifact)), "rollback left newly published metadata");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "rollback transaction abort failed: " + aborted.error.detail);
  ok &= expect(!fs::exists(staging.artifact.parent_path()),
               "rollback transaction left its staging directory");
  return ok;
}

struct FinalProtectedInputMutationContext {
  fs::path path;
  bool invoked = false;
  bool write_succeeded = false;
};

void mutate_final_protected_input(void *opaque_context) {
  auto &context = *static_cast<FinalProtectedInputMutationContext *>(opaque_context);
  context.invoked = true;
  context.write_succeeded = write_file(context.path, "mutated-after-publication-flush");
}

bool run_final_protected_input_revalidation_test(const fs::path &root) {
  const fs::path directory = root / "final-protected-input-revalidation";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create final protected-input fixture directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "protected") && write_file(artifact, "old-artifact") &&
        write_file(generated, "old-generated");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "final protected-input transaction begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &=
    expect(sealed.ok(), "final protected-input transaction seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;

  FinalProtectedInputMutationContext context{input};
  inject_before_final_protected_input_revalidation_once(mutate_final_protected_input, &context);
  const auto committed = begun.transaction->commit();
  inject_before_final_protected_input_revalidation_once(nullptr, nullptr);
  ok &= expect(context.invoked && context.write_succeeded,
               "final protected-input mutation hook did not update the protected file");
  ok &= expect(!committed.ok() &&
                 committed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "final protected-input mutation was not rejected");
  ok &= expect(read_file(artifact) == "old-artifact" && read_file(generated) == "old-generated",
               "final protected-input rejection did not restore prior outputs");
  ok &= expect(!fs::exists(artifact_metadata_path(artifact)),
               "final protected-input rejection left newly published metadata");
  if (committed.ok()) {
    const auto finished = begun.transaction->finish();
    ok &= expect(finished.ok(), "unexpectedly committed final protected-input transaction could "
                                "not be finished: " +
                                  finished.error.detail);
  } else {
    const auto aborted = begun.transaction->abort();
    ok &= expect(aborted.ok(),
                 "final protected-input transaction abort failed: " + aborted.error.detail);
  }
  return ok;
}

bool run_protected_swap_test(const fs::path &root) {
  const fs::path directory = root / "protected-swap";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create protected-swap directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "protected") && write_file(artifact, "old-artifact");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "protected-swap begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "protected-swap seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  fs::remove(artifact, error);
  ok &= expect(!error, "failed to remove destination before protected swap");
  fs::create_hard_link(input, artifact, error);
  ok &= expect(!error, "failed to install protected hard-link swap");
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok() &&
                 committed.error.code == HostedArtifactTransactionErrorCode::PathConflict,
               "commit did not reject a destination swapped to a protected hard link");
  ok &= expect(read_file(input) == "protected" && read_file(artifact) == "protected",
               "protected hard-link swap corrupted its input");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "protected-swap abort failed: " + aborted.error.detail);
  return ok;
}

bool run_protected_content_mutation_test(const fs::path &root) {
  const fs::path directory = root / "protected-content-mutation";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create protected-content-mutation directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  const fs::path generated = directory / "app.cpp";
  ok &= write_file(input, "original-input") && write_file(artifact, "old-artifact");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "protected-content-mutation begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "protected-content-mutation seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  ok &= expect(write_file(input, "mutated-input-with-a-different-size"),
               "failed to mutate a protected input in place");
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok() &&
                 committed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "commit did not reject in-place protected input mutation");
  ok &= expect(read_file(artifact) == "old-artifact",
               "protected input mutation changed the public artifact");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "protected-content-mutation abort failed: " + aborted.error.detail);
  return ok;
}

bool run_protected_directory_membership_test(const fs::path &root) {
  const fs::path directory = root / "protected-directory-membership";
  const fs::path include_directory = directory / "include";
  const fs::path output_directory = directory / "output";
  std::error_code error;
  fs::create_directories(include_directory, error);
  fs::create_directories(output_directory, error);
  bool ok = expect(!error, "failed to create protected-directory fixtures");
  const fs::path input = directory / "input.nb";
  const fs::path header = include_directory / "answer.hpp";
  const fs::path artifact = output_directory / "app.out";
  const fs::path generated = output_directory / "app.cpp";
  ok &= write_file(input, "protected") && write_file(header, "#define ANSWER 41\n");
  const nebula::cli::DirectoryTreeDigestResult membership =
    nebula::cli::sha256_directory_tree(include_directory);
  ok &= expect(membership.ok(), "failed to snapshot protected directory membership");
  if (!membership.ok())
    return false;
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  plan.protected_directories.push_back({include_directory, *membership.value});
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "protected-directory begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  ok &= write_file(include_directory / "shadow.hpp", "#define ANSWER 42\n");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "protected-directory seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok() &&
                 committed.error.code == HostedArtifactTransactionErrorCode::ConcurrentModification,
               "commit accepted changed protected directory membership");
  ok &= expect(!fs::exists(artifact), "protected directory mutation published an artifact");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "protected-directory abort failed: " + aborted.error.detail);
  return ok;
}

bool run_internal_name_collision_regression_test(const fs::path &root) {
  const fs::path directory = root / "internal-name-collision";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create internal-name-collision directory");
  const fs::path input = directory / "input.nb";
  const fs::path artifact = directory / "app.out";
  // This basename collided with the old shared-directory backup convention.
  const fs::path generated = directory / ".backup-0-app.out";
  ok &= write_file(input, "protected") && write_file(artifact, "old-artifact") &&
        write_file(generated, "old-generated");
  HostedArtifactTransactionPlan plan{artifact, generated, std::nullopt, std::nullopt, {input}};
  auto begun = begin_hosted_artifact_transaction(plan);
  ok &= expect(begun.ok(), "internal-name-collision begin failed: " + begun.error.detail);
  if (!begun.ok())
    return false;
  const auto staging = begun.transaction->staging_paths();
  ok &= expect(staging.artifact.parent_path() != staging.generated_cpp.parent_path(),
               "distinct outputs shared an internal staging directory");
  ok &= write_file(staging.artifact, "new-artifact") &&
        write_file(staging.generated_cpp, "new-generated");
  const auto sealed = begun.transaction->seal(build_key());
  ok &= expect(sealed.ok(), "internal-name-collision seal failed: " + sealed.error.detail);
  if (!sealed.ok())
    return false;
  inject_fault_once(FaultPoint::AfterPublishLink);
  const auto committed = begun.transaction->commit();
  ok &= expect(!committed.ok(), "internal-name-collision fault unexpectedly committed");
  ok &= expect(read_file(artifact) == "old-artifact",
               "internal backup collision lost the prior artifact");
  ok &= expect(read_file(generated) == "old-generated",
               "internal backup collision replaced the generated output");
  ok &= expect(!fs::exists(artifact_metadata_path(artifact)),
               "internal backup collision left metadata behind");
  const auto aborted = begun.transaction->abort();
  ok &= expect(aborted.ok(), "internal-name-collision abort failed: " + aborted.error.detail);
  return ok;
}

bool run_destination_snapshot_regression_tests(const fs::path &root) {
  const fs::path directory = root / "destination-snapshot";
  std::error_code error;
  fs::create_directory(directory, error);
  bool ok = expect(!error, "failed to create destination-snapshot directory");
  const fs::path input = directory / "input.nb";
  ok &= write_file(input, "protected");

  const fs::path appeared_artifact = directory / "appeared.out";
  HostedArtifactTransactionPlan appeared_plan{
    appeared_artifact, directory / "appeared.cpp", std::nullopt, std::nullopt, {input}};
  auto appeared = begin_hosted_artifact_transaction(appeared_plan);
  ok &= expect(appeared.ok(), "appeared-destination begin failed: " + appeared.error.detail);
  if (!appeared.ok())
    return false;
  const auto appeared_staging = appeared.transaction->staging_paths();
  ok &= write_file(appeared_artifact, "external-appeared") &&
        write_file(appeared_staging.artifact, "transaction-artifact") &&
        write_file(appeared_staging.generated_cpp, "transaction-generated");
  const auto appeared_sealed = appeared.transaction->seal(build_key());
  ok &= expect(appeared_sealed.ok(),
               "appeared-destination seal failed: " + appeared_sealed.error.detail);
  if (!appeared_sealed.ok())
    return false;
  const auto appeared_commit = appeared.transaction->commit();
  ok &=
    expect(!appeared_commit.ok() && appeared_commit.error.code ==
                                      HostedArtifactTransactionErrorCode::ConcurrentModification,
           "destination that appeared after begin was adopted and replaced");
  ok &= expect(read_file(appeared_artifact) == "external-appeared",
               "destination that appeared after begin was modified");
  const auto appeared_abort = appeared.transaction->abort();
  ok &= expect(appeared_abort.ok(),
               "appeared-destination abort failed: " + appeared_abort.error.detail);

  const fs::path modified_artifact = directory / "modified.out";
  const fs::path modified_generated = directory / "modified.cpp";
  ok &= write_file(modified_artifact, "initial-artifact");
  HostedArtifactTransactionPlan modified_plan{
    modified_artifact, modified_generated, std::nullopt, std::nullopt, {input}};
  auto modified = begin_hosted_artifact_transaction(modified_plan);
  ok &= expect(modified.ok(), "modified-destination begin failed: " + modified.error.detail);
  if (!modified.ok())
    return false;
  const auto modified_staging = modified.transaction->staging_paths();
  ok &= write_file(modified_artifact, "external-modification-with-new-size") &&
        write_file(modified_staging.artifact, "transaction-artifact") &&
        write_file(modified_staging.generated_cpp, "transaction-generated");
  const auto modified_sealed = modified.transaction->seal(build_key());
  ok &= expect(modified_sealed.ok(),
               "modified-destination seal failed: " + modified_sealed.error.detail);
  if (!modified_sealed.ok())
    return false;
  const auto modified_commit = modified.transaction->commit();
  ok &=
    expect(!modified_commit.ok() && modified_commit.error.code ==
                                      HostedArtifactTransactionErrorCode::ConcurrentModification,
           "destination modified after begin was adopted and replaced");
  ok &= expect(read_file(modified_artifact) == "external-modification-with-new-size",
               "destination modified after begin was not preserved");
  const auto modified_abort = modified.transaction->abort();
  ok &= expect(modified_abort.ok(),
               "modified-destination abort failed: " + modified_abort.error.detail);
  return ok;
}

} // namespace

int main() {
  TemporaryDirectory temporary;
  if (!expect(!temporary.path().empty(), "failed to allocate temporary directory"))
    return 1;
  bool ok = run_successful_replacement_test(temporary.path());
  ok &= run_conflict_tests(temporary.path());
  ok &= run_large_additional_input_index_test(temporary.path());
  ok &= run_additional_input_hardlink_transition_test(temporary.path());
#if !defined(_WIN32)
  ok &= run_additional_input_symlink_retarget_test(temporary.path());
  ok &= run_untrusted_output_parent_test(temporary.path());
  ok &= run_lock_file_policy_test(temporary.path());
  ok &= run_lock_entry_replacement_test(temporary.path());
  ok &= run_finish_lock_revalidation_test(temporary.path());
#endif
#if defined(__APPLE__)
  ok &= run_darwin_metadata_security_transition_tests(temporary.path());
#endif
  ok &= run_mutation_and_abort_test(temporary.path());
  ok &= run_explicit_failed_build_adoption_test(temporary.path());
  ok &= run_unadopted_unknown_output_preservation_test(temporary.path());
  ok &= run_fault_rollback_test(temporary.path(), FaultPoint::AfterBackup, "fault-after-backup");
  ok &= run_fault_rollback_test(temporary.path(), FaultPoint::AfterPublishLink, "fault-after-link");
  ok &= run_fault_rollback_test(temporary.path(), FaultPoint::BeforePublicationDirectoryFlush,
                                "fault-before-fsync");
  ok &= run_final_protected_input_revalidation_test(temporary.path());
  ok &= run_protected_swap_test(temporary.path());
  ok &= run_protected_content_mutation_test(temporary.path());
  ok &= run_protected_directory_membership_test(temporary.path());
  ok &= run_internal_name_collision_regression_test(temporary.path());
  ok &= run_destination_snapshot_regression_tests(temporary.path());
  return ok ? 0 : 1;
}
