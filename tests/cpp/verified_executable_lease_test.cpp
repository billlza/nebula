#include "cli/artifact_digest.hpp"
#include "cli/verified_executable_lease.hpp"
#include "cli/verified_executable_lease_test_hooks.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif
#endif

namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr std::string_view kPostDeletionReplacement = "post-deletion-cleanup-replacement";
DWORD post_deletion_replacement_error = ERROR_SUCCESS;

void replace_deleted_lease_before_absence_check(const fs::path &lease_path) noexcept {
  HANDLE replacement = ::CreateFileW(lease_path.c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (replacement == INVALID_HANDLE_VALUE) {
    post_deletion_replacement_error = ::GetLastError();
    return;
  }

  DWORD written = 0U;
  if (::WriteFile(replacement, kPostDeletionReplacement.data(),
                  static_cast<DWORD>(kPostDeletionReplacement.size()), &written, nullptr) == 0) {
    post_deletion_replacement_error = ::GetLastError();
  } else if (written != static_cast<DWORD>(kPostDeletionReplacement.size())) {
    post_deletion_replacement_error = ERROR_WRITE_FAULT;
  }
  if (::CloseHandle(replacement) == 0 && post_deletion_replacement_error == ERROR_SUCCESS)
    post_deletion_replacement_error = ::GetLastError();
}
#endif

bool expect(bool condition, const std::string &message) {
  if (condition)
    return true;
  std::cerr << "verified executable lease test failed: " << message << '\n';
  return false;
}

std::string process_summary(const nebula::cli::HostProcessResult &process) {
  return "started=" + std::to_string(process.started) +
         ", exited=" + std::to_string(process.exited) +
         ", exit_code=" + std::to_string(process.exit_code) +
         ", signal=" + std::to_string(process.termination_signal) +
         ", timed_out=" + std::to_string(process.timed_out) +
         ", infrastructure_error=" + process.infrastructure_error +
         ", stderr=" + process.stderr_data;
}

fs::path unique_test_root() {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("nebula-verified-executable-lease-test-" + std::to_string(tick));
}

bool has_private_lease_file(const fs::path &directory, std::error_code &error) {
  error.clear();
  for (fs::directory_iterator iterator(directory, error), end; !error && iterator != end;
       iterator.increment(error)) {
    if (iterator->path().filename().string().starts_with(".nebula-exec-")) {
      return true;
    }
  }
  return false;
}

std::vector<fs::path> private_lease_files(const fs::path &directory, std::error_code &error) {
  std::vector<fs::path> paths;
  error.clear();
  for (fs::directory_iterator iterator(directory, error), end; !error && iterator != end;
       iterator.increment(error)) {
    if (iterator->path().filename().string().starts_with(".nebula-exec-"))
      paths.push_back(iterator->path());
  }
  return paths;
}

bool run_injected_acquisition_exception(
  const fs::path &public_executable,
  nebula::cli::verified_executable_lease_testing::AcquisitionExceptionPoint point,
  std::string_view phase) {
  using namespace nebula::cli;
  using namespace nebula::cli::verified_executable_lease_testing;

  inject_acquisition_exception_once(point);
  bool caught_expected = false;
  try {
    const VerifiedExecutableLeaseBeginResult unexpected =
      begin_verified_executable_lease(public_executable);
    (void)unexpected;
  } catch (const std::runtime_error &exception) {
    caught_expected = exception.what() == kInjectedAcquisitionExceptionDetail;
  } catch (...) {
    caught_expected = false;
  }

  std::error_code error;
  bool ok = true;
  ok &= expect(caught_expected,
               std::string(phase) + " did not preserve the injected acquisition exception");
  ok &= expect(!acquisition_exception_injection_pending(),
               std::string(phase) + " did not consume its one-shot injection");
  ok &= expect(!has_private_lease_file(public_executable.parent_path(), error) && !error,
               std::string(phase) + " left a private executable lease behind");
  return ok;
}

#if !defined(_WIN32)
constexpr std::string_view kRollbackReplacement = "identity-bound-rollback-replacement";

[[noreturn]] void acquisition_fixture_exit(int status) noexcept { ::_exit(status); }

void replace_private_lease_before_rollback(const fs::path &lease_path) {
  if (::unlink(lease_path.c_str()) != 0)
    acquisition_fixture_exit(91);
  const int descriptor = ::open(
    lease_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    acquisition_fixture_exit(92);
  const char *cursor = kRollbackReplacement.data();
  std::size_t remaining = kRollbackReplacement.size();
  while (remaining != 0U) {
    const ssize_t written = ::write(descriptor, cursor, remaining);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      (void)::close(descriptor);
      acquisition_fixture_exit(93);
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  if (::close(descriptor) != 0)
    acquisition_fixture_exit(94);
}
#endif

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--lease-child") {
    return std::string(argv[0]) == argv[2] ? 0 : 91;
  }
  if (argc == 3 && std::string(argv[1]) == "--lease-cleanup-failure-child") {
    nebula::cli::VerifiedExecutableLeaseBeginResult begun =
      nebula::cli::begin_verified_executable_lease(fs::path(argv[2]));
    if (!begun.ok())
      return 97;
    nebula::cli::verified_executable_lease_testing::inject_cleanup_failure_once();
    return 98;
  }
#if !defined(_WIN32)
  if (argc == 3 && std::string(argv[1]) == "--acquisition-rollback-failure-child") {
    using namespace nebula::cli::verified_executable_lease_testing;
    inject_acquisition_failure_once(replace_private_lease_before_rollback);
    inject_post_rollback_diagnostic_exception_once();
    try {
      const nebula::cli::VerifiedExecutableLeaseBeginResult unexpected =
        nebula::cli::begin_verified_executable_lease(fs::path(argv[2]));
      (void)unexpected;
    } catch (...) {
      return 95;
    }
    return 96;
  }
#endif

  bool ok = true;
  std::error_code error;
  const fs::path self = fs::canonical(argv[0], error);
  ok &= expect(!error && self.is_absolute(), "test executable must canonicalize");
  if (!ok)
    return 1;

  const nebula::cli::FileDigestResult self_digest = nebula::cli::sha256_file(self);
  ok &= expect(self_digest.ok(), "test executable must be hashable");
  if (!self_digest.ok())
    return 1;

  const fs::path root = unique_test_root();
  fs::create_directories(root, error);
  ok &= expect(!error, "test root must be created");
#if !defined(_WIN32)
  ok &= expect(::chmod(root.c_str(), S_IRWXU) == 0, "test root must be owner-private");
#endif

  const fs::path public_executable = root / ("program-under-test" + self.extension().string());
  fs::copy_file(self, public_executable, fs::copy_options::none, error);
  ok &= expect(!error, "public executable fixture must be copied");
#if !defined(_WIN32)
  ok &= expect(::chmod(public_executable.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0,
               "public executable fixture must be executable");
#endif
  const nebula::cli::FileDigestResult public_digest = nebula::cli::sha256_file(public_executable);
  ok &= expect(public_digest.ok(), "public executable fixture must be hashable");
  if (!public_digest.ok())
    return 1;

  using nebula::cli::verified_executable_lease_testing::AcquisitionExceptionPoint;
  ok &= run_injected_acquisition_exception(public_executable,
                                           AcquisitionExceptionPoint::AfterPrivateEntryCreation,
                                           "post-private-entry-creation acquisition exception");
  ok &= run_injected_acquisition_exception(public_executable,
                                           AcquisitionExceptionPoint::AfterWritableSnapshotClosed,
                                           "post-writer-close acquisition exception");
  ok &= run_injected_acquisition_exception(
    public_executable, AcquisitionExceptionPoint::BeforeImplementationAllocation,
    "pre-implementation-allocation acquisition exception");
  ok &= run_injected_acquisition_exception(public_executable,
                                           AcquisitionExceptionPoint::AfterOwnershipTransfer,
                                           "post-ownership-transfer acquisition exception");

#if !defined(_WIN32)
  nebula::cli::HostProcessRequest rollback_failure_request;
  rollback_failure_request.executable_path = self;
  rollback_failure_request.arguments = {self.string(), "--acquisition-rollback-failure-child",
                                        public_executable.string()};
  rollback_failure_request.stdin_mode = nebula::cli::HostProcessInputMode::Discard;
  rollback_failure_request.stdout_mode = nebula::cli::HostProcessStreamMode::Discard;
  rollback_failure_request.stderr_mode = nebula::cli::HostProcessStreamMode::Capture;
  rollback_failure_request.max_stderr_bytes = 4096U;
  rollback_failure_request.timeout_milliseconds = 10000U;
  const nebula::cli::HostProcessResult rollback_failure_process =
    nebula::cli::run_host_process(rollback_failure_request);
  ok &= expect(rollback_failure_process.completed() &&
                 rollback_failure_process.termination_signal == 0 &&
                 rollback_failure_process.exit_code == 125U,
               "incomplete exception rollback must fail fast with exit code 125: " +
                 process_summary(rollback_failure_process));
  ok &= expect(rollback_failure_process.stderr_data.find(
                 "fatal: identity-bound executable lease acquisition rollback failed") !=
                 std::string::npos,
               "incomplete exception rollback must emit its fixed fatal diagnostic");

  std::vector<fs::path> preserved_replacements = private_lease_files(root, error);
  ok &= expect(!error && preserved_replacements.size() == 1U,
               "failed acquisition rollback must preserve exactly one replacement object");
  if (!error && preserved_replacements.size() == 1U) {
    std::ifstream replacement(preserved_replacements.front(), std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(replacement)),
                               std::istreambuf_iterator<char>());
    ok &= expect((replacement.good() || replacement.eof()) && contents == kRollbackReplacement,
                 "failed acquisition rollback changed or deleted the replacement object");
    replacement.close();
    fs::remove(preserved_replacements.front(), error);
    ok &= expect(!error, "preserved acquisition replacement must be removable by the test");
  }
#endif

  nebula::cli::HostProcessRequest cleanup_failure_request;
  cleanup_failure_request.executable_path = self;
  cleanup_failure_request.arguments = {self.string(), "--lease-cleanup-failure-child",
                                       public_executable.string()};
  cleanup_failure_request.stdin_mode = nebula::cli::HostProcessInputMode::Discard;
  cleanup_failure_request.stdout_mode = nebula::cli::HostProcessStreamMode::Discard;
  cleanup_failure_request.stderr_mode = nebula::cli::HostProcessStreamMode::Capture;
  cleanup_failure_request.max_stderr_bytes = 4096U;
  cleanup_failure_request.timeout_milliseconds = 10000U;
  const nebula::cli::HostProcessResult cleanup_failure_process =
    nebula::cli::run_host_process(cleanup_failure_request);
  ok &=
    expect(cleanup_failure_process.completed() && cleanup_failure_process.termination_signal == 0 &&
             cleanup_failure_process.exit_code == 125U,
           "incomplete implicit lease cleanup must fail fast with exit code 125: " +
             process_summary(cleanup_failure_process));
  ok &= expect(cleanup_failure_process.stderr_data.find(
                 "fatal: verified executable lease cleanup failed") != std::string::npos,
               "incomplete implicit lease cleanup must emit its fixed fatal diagnostic");
  std::vector<fs::path> incomplete_cleanup_entries = private_lease_files(root, error);
  ok &= expect(!error && incomplete_cleanup_entries.size() == 1U,
               "incomplete implicit cleanup must leave one observable owned lease entry");
  if (!error && incomplete_cleanup_entries.size() == 1U) {
    fs::remove(incomplete_cleanup_entries.front(), error);
    ok &= expect(!error, "incomplete cleanup lease entry must be removable by the test");
  }

  auto begun =
    nebula::cli::begin_verified_executable_lease(public_executable, *public_digest.value);
  ok &= expect(begun.ok(), "matching executable content must acquire a lease");
  if (begun.ok()) {
    const fs::path execution_path = begun.lease->execution_path();
    const fs::path canonical_public_parent = fs::canonical(public_executable.parent_path(), error);
    ok &= expect(!error && execution_path.parent_path() == canonical_public_parent,
                 "lease must preserve the public executable directory");
    ok &= expect(execution_path != public_executable && fs::exists(execution_path),
                 "lease must use a distinct private file");
#if defined(_WIN32)
    const fs::path blocked_rename(execution_path.native() + L".renamed");
    const BOOL renamed = ::MoveFileExW(execution_path.c_str(), blocked_rename.c_str(), 0U);
    const DWORD rename_error = renamed == 0 ? ::GetLastError() : ERROR_SUCCESS;
    ok &= expect(renamed == 0 &&
                   (rename_error == ERROR_SHARING_VIOLATION || rename_error == ERROR_ACCESS_DENIED),
                 "active Windows lease must block path rename/replacement; error=" +
                   std::to_string(rename_error));
    if (renamed != 0) {
      ok &= expect(::MoveFileExW(blocked_rename.c_str(), execution_path.c_str(), 0U) != 0,
                   "unexpected Windows lease rename must be restored");
    }
    const BOOL deleted = ::DeleteFileW(execution_path.c_str());
    const DWORD deletion_error = deleted == 0 ? ::GetLastError() : ERROR_SUCCESS;
    ok &= expect(
      deleted == 0 &&
        (deletion_error == ERROR_SHARING_VIOLATION || deletion_error == ERROR_ACCESS_DENIED),
      "active Windows lease must block path deletion; error=" + std::to_string(deletion_error));
#endif
    const std::string logical_argv0 = "logical/public/program";
    const nebula::cli::HostProcessResult process =
      begun.lease->execute({logical_argv0, "--lease-child", logical_argv0});
    ok &= expect(process.succeeded(), "lease must execute while preserving logical argv[0]: " +
                                        process_summary(process));
    const auto cleaned = begun.lease->cleanup();
    ok &= expect(cleaned.ok(), "lease cleanup must succeed");
    ok &= expect(!fs::exists(execution_path), "lease cleanup must remove its private file");
  }

  auto public_replaced =
    nebula::cli::begin_verified_executable_lease(public_executable, *public_digest.value);
  ok &= expect(public_replaced.ok(), "public replacement fixture must acquire a lease");
  if (public_replaced.ok()) {
    const fs::path original_public = root / "program-original";
    fs::rename(public_executable, original_public, error);
    ok &= expect(!error, "public artifact must be replaceable after lease acquisition");
    {
      std::ofstream replacement(public_executable, std::ios::binary | std::ios::trunc);
      replacement << "not-the-verified-executable";
      replacement.close();
      ok &= expect(!replacement.fail(), "public replacement must be written");
    }
    const std::string logical_argv0 = "logical/replaced/public/program";
    const nebula::cli::HostProcessResult process =
      public_replaced.lease->execute({logical_argv0, "--lease-child", logical_argv0});
    ok &= expect(process.succeeded(),
                 "public path replacement must not change leased execution bytes: " +
                   process_summary(process));
    ok &= expect(public_replaced.lease->cleanup().ok(), "public replacement lease must clean up");
    fs::remove(public_executable, error);
    ok &= expect(!error, "public replacement fixture must be removed");
    fs::rename(original_public, public_executable, error);
    ok &= expect(!error, "original public executable fixture must be restored");
  }

#if defined(_WIN32)
  auto post_deletion_replaced =
    nebula::cli::begin_verified_executable_lease(public_executable, *public_digest.value);
  ok &=
    expect(post_deletion_replaced.ok(), "post-deletion replacement fixture must acquire a lease");
  if (post_deletion_replaced.ok()) {
    using namespace nebula::cli::verified_executable_lease_testing;
    const fs::path private_path = post_deletion_replaced.lease->execution_path();
    post_deletion_replacement_error = ERROR_SUCCESS;
    inject_post_deletion_cleanup_setup_once(replace_deleted_lease_before_absence_check);
    const auto conflict = post_deletion_replaced.lease->cleanup();
    ok &= expect(!post_deletion_cleanup_setup_pending(),
                 "post-deletion replacement hook must be consumed");
    ok &= expect(post_deletion_replacement_error == ERROR_SUCCESS,
                 "post-deletion replacement must be created; error=" +
                   std::to_string(post_deletion_replacement_error));
    ok &= expect(!conflict.ok() &&
                   conflict.error.code ==
                     nebula::cli::VerifiedExecutableLeaseErrorCode::ConcurrentModification &&
                   conflict.owned_cleanup_complete() && !post_deletion_replaced.lease->active(),
                 "cleanup must classify a post-deletion replacement as an owned-complete "
                 "concurrent modification");
    std::ifstream replacement(private_path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(replacement)),
                               std::istreambuf_iterator<char>());
    ok &= expect((replacement.good() || replacement.eof()) && contents == kPostDeletionReplacement,
                 "cleanup must preserve the post-deletion replacement bytes");
    replacement.close();
    fs::remove(private_path, error);
    ok &= expect(!error, "post-deletion replacement fixture must be removable");
  }
#endif

  nebula::cli::FileDigest wrong_digest = *public_digest.value;
  wrong_digest.sha256.assign(64U, '0');
  auto mismatch = nebula::cli::begin_verified_executable_lease(public_executable, wrong_digest);
  ok &= expect(!mismatch.ok() && mismatch.error.code ==
                                   nebula::cli::VerifiedExecutableLeaseErrorCode::ContentMismatch,
               "content mismatch must fail closed");
  ok &= expect(!has_private_lease_file(root, error) && !error,
               "failed lease acquisition must remove its private snapshot");

#if defined(_WIN32)
  const fs::path zone_identifier(public_executable.native() + L":Zone.Identifier");
  HANDLE zone = ::CreateFileW(zone_identifier.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  ok &= expect(zone != INVALID_HANDLE_VALUE, "Zone.Identifier fixture must be created on Windows");
  if (zone != INVALID_HANDLE_VALUE) {
    constexpr char zone_payload[] = "[ZoneTransfer]\r\nZoneId=3\r\n";
    DWORD written = 0U;
    ok &= expect(::WriteFile(zone, zone_payload, static_cast<DWORD>(sizeof(zone_payload) - 1U),
                             &written, nullptr) != 0 &&
                   written == static_cast<DWORD>(sizeof(zone_payload) - 1U),
                 "Zone.Identifier fixture must be written");
    ok &= expect(::CloseHandle(zone) != 0, "Zone.Identifier fixture handle must close");
    auto labeled = nebula::cli::begin_verified_executable_lease(public_executable);
    ok &= expect(!labeled.ok() &&
                   labeled.error.code == nebula::cli::VerifiedExecutableLeaseErrorCode::UnsafePath,
                 "Zone.Identifier must not be stripped by a private lease");
    ok &= expect(::DeleteFileW(zone_identifier.c_str()) != 0,
                 "Zone.Identifier fixture must be removed");
  }
#endif

#if !defined(_WIN32)
  const fs::path execute_denied = root / "readable-but-not-owner-executable";
  fs::copy_file(self, execute_denied, fs::copy_options::none, error);
  ok &= expect(!error && ::chmod(execute_denied.c_str(), S_IRUSR | S_IXGRP | S_IXOTH) == 0,
               "effective-execute denial fixture must be created");
  auto denied = nebula::cli::begin_verified_executable_lease(execute_denied);
  ok &= expect(!denied.ok() &&
                 denied.error.code == nebula::cli::VerifiedExecutableLeaseErrorCode::UnsafePath,
               "lease must not broaden effective execute permission");

#if defined(__APPLE__)
  const fs::path quarantined = root / "quarantined-program";
  fs::copy_file(self, quarantined, fs::copy_options::none, error);
  ok &= expect(!error, "quarantined executable fixture must be copied");
  constexpr char quarantine_value[] = "0081;00000000;NebulaLeaseTest;";
  const int quarantine_result = ::setxattr(quarantined.c_str(), "com.apple.quarantine",
                                           quarantine_value, sizeof(quarantine_value) - 1U, 0U, 0);
  ok &=
    expect(quarantine_result == 0,
           quarantine_result == 0 ? "quarantine fixture must carry the platform security label"
                                  : "quarantine fixture must carry the platform security label: " +
                                      std::string(std::strerror(errno)));
  ok &= expect(::chmod(quarantined.c_str(), S_IRUSR | S_IXUSR) == 0,
               "quarantined executable fixture must be executable");
  auto quarantined_lease = nebula::cli::begin_verified_executable_lease(quarantined);
  ok &=
    expect(!quarantined_lease.ok() && quarantined_lease.error.code ==
                                        nebula::cli::VerifiedExecutableLeaseErrorCode::UnsafePath,
           "lease must not strip com.apple.quarantine");
#endif

  const fs::path symlink_path = root / "program-link";
  fs::create_symlink(public_executable.filename(), symlink_path, error);
  ok &= expect(!error, "symlink fixture must be created");
  auto symlink_lease = nebula::cli::begin_verified_executable_lease(symlink_path);
  ok &= expect(!symlink_lease.ok() && symlink_lease.error.code ==
                                        nebula::cli::VerifiedExecutableLeaseErrorCode::UnsafePath,
               "symbolic-link executable must be rejected");

  auto replaced =
    nebula::cli::begin_verified_executable_lease(public_executable, *public_digest.value);
  ok &= expect(replaced.ok(), "replacement cleanup fixture must acquire a lease");
  if (replaced.ok()) {
    const fs::path private_path = replaced.lease->execution_path();
    ok &= expect(::unlink(private_path.c_str()) == 0, "test must unlink the original lease name");
    {
      std::ofstream replacement(private_path, std::ios::binary | std::ios::trunc);
      replacement << "replacement";
      replacement.close();
      ok &= expect(!replacement.fail(), "replacement fixture must be written");
    }
    const nebula::cli::HostProcessResult rejected_execution =
      replaced.lease->execute({"logical/replaced/lease"});
    ok &= expect(!rejected_execution.started && !rejected_execution.infrastructure_error.empty(),
                 "replaced lease path must fail before process launch");
    const auto conflict = replaced.lease->cleanup();
    ok &= expect(!conflict.ok() &&
                   conflict.error.code ==
                     nebula::cli::VerifiedExecutableLeaseErrorCode::ConcurrentModification &&
                   conflict.owned_cleanup_complete() && !replaced.lease->active(),
                 "cleanup must preserve a replacement after retiring every owned resource");
    ok &= expect(fs::exists(private_path), "replacement object must remain after failed cleanup");
    fs::remove(private_path, error);
    ok &= expect(!error, "replacement fixture must be removable by the test");
    const auto retry = replaced.lease->cleanup();
    ok &= expect(retry.ok(), "cleanup must be retryable after a path conflict is resolved");
  }

  const fs::path unsafe = root / "shared";
  fs::create_directories(unsafe, error);
  ok &= expect(!error && ::chmod(unsafe.c_str(), 0777) == 0,
               "unsafe parent fixture must be group/world writable");
  const fs::path unsafe_executable = unsafe / "program";
  fs::copy_file(self, unsafe_executable, fs::copy_options::none, error);
  ok &= expect(!error && ::chmod(unsafe_executable.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0,
               "unsafe executable fixture must be created");
  auto unsafe_lease = nebula::cli::begin_verified_executable_lease(unsafe_executable);
  ok &= expect(!unsafe_lease.ok() && unsafe_lease.error.code ==
                                       nebula::cli::VerifiedExecutableLeaseErrorCode::UnsafePath,
               "non-sticky shared-writable parent must be rejected");
  (void)::chmod(unsafe.c_str(), S_IRWXU);
#endif

  fs::remove_all(root, error);
  ok &= expect(!error, "test fixtures must be removed");
  return ok ? 0 : 1;
}
