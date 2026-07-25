#include "hosted_toolchain.hpp"

#include "artifact_digest.hpp"
#include "host_process.hpp"
#include "hosted_object_workspace.hpp"
#include "path_security.hpp"
#include "termination_signal.hpp"
#include "tool_lookup.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nebula::cli {
namespace {

constexpr std::uint32_t kToolQueryTimeoutMilliseconds = 5000U;
constexpr std::size_t kToolQueryStreamLimitBytes = 64U * 1024U;
constexpr std::uintmax_t kArchiverProbeMaxObjectBytes = 1024U * 1024U;
constexpr std::uintmax_t kArchiverProbeMaxArchiveBytes = 1024U * 1024U;
constexpr std::string_view kArchiverCapabilityIdentity = "nebula-ar-rcs-portable-archive-v1";
constexpr std::array<std::uint8_t, 8U> kArchiveMagic = {
  static_cast<std::uint8_t>('!'), static_cast<std::uint8_t>('<'),  static_cast<std::uint8_t>('a'),
  static_cast<std::uint8_t>('r'), static_cast<std::uint8_t>('c'),  static_cast<std::uint8_t>('h'),
  static_cast<std::uint8_t>('>'), static_cast<std::uint8_t>('\n'),
};

constexpr std::array<std::string_view, 20U> kForwardedCompilationEnvironmentNames = {
  "COMPILER_PATH",
  "CPATH",
  "CPLUS_INCLUDE_PATH",
  "C_INCLUDE_PATH",
  "GCC_EXEC_PREFIX",
  "INCLUDE",
  "LIB",
  "LIBPATH",
  "LIBRARY_PATH",
  "MACOSX_DEPLOYMENT_TARGET",
  "OBJC_INCLUDE_PATH",
  "PATH",
  "SDKROOT",
  "SOURCE_DATE_EPOCH",
  "SystemRoot",
  "UniversalCRTSdkDir",
  "VCINSTALLDIR",
  "VCToolsInstallDir",
  "WindowsSdkDir",
  "ZERO_AR_DATE",
};

std::string digest_text(std::string_view text) {
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  return sha256_hex(std::span<const std::uint8_t>(bytes, text.size()));
}

void append_length_delimited(std::ostringstream &output, std::string_view name,
                             std::string_view value) {
  output << name << "_size=" << value.size() << '\n';
  output << name << '=' << value << '\n';
}

std::string trim_ascii_whitespace(std::string value) {
  const auto is_space = [](unsigned char byte) {
    return byte == static_cast<unsigned char>(' ') || byte == static_cast<unsigned char>('\t') ||
           byte == static_cast<unsigned char>('\r') || byte == static_cast<unsigned char>('\n') ||
           byte == static_cast<unsigned char>('\f') || byte == static_cast<unsigned char>('\v');
  };
  const auto first = std::find_if_not(value.begin(), value.end(), [&](char byte) {
    return is_space(static_cast<unsigned char>(byte));
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [&](char byte) {
                      return is_space(static_cast<unsigned char>(byte));
                    }).base();
  if (first >= last)
    return {};
  return std::string(first, last);
}

struct EnvironmentSnapshotResult {
  std::vector<HostEnvironmentOverride> entries;
  std::string sha256;
  std::string detail;
};

std::string aggregate_environment_identity(const std::vector<HostEnvironmentOverride> &entries) {
  std::ostringstream identity;
  identity << "hosted-compilation-environment-v2\n";
  for (const HostEnvironmentOverride &entry : entries) {
    append_length_delimited(identity, "name", entry.name);
    append_length_delimited(identity, "value", entry.value);
  }
  return digest_text(identity.str());
}

EnvironmentSnapshotResult capture_execution_environment() {
  EnvironmentSnapshotResult result;
  result.entries = {
    HostEnvironmentOverride{"LANG", "C"},
    HostEnvironmentOverride{"LC_ALL", "C"},
    HostEnvironmentOverride{"TZ", "UTC"},
  };
  for (const std::string_view name : kForwardedCompilationEnvironmentNames) {
    const std::string owned_name(name);
    const char *value = std::getenv(owned_name.c_str());
    if (value != nullptr)
      result.entries.push_back(HostEnvironmentOverride{owned_name, value});
  }
  const auto path =
    std::find_if(result.entries.begin(), result.entries.end(),
                 [](const HostEnvironmentOverride &entry) { return entry.name == "PATH"; });
  if (path == result.entries.end() || path->value.empty()) {
    result.detail = "hosted compilation requires a nonempty explicit PATH snapshot";
    return result;
  }
#if defined(_WIN32)
  const auto system_root =
    std::find_if(result.entries.begin(), result.entries.end(),
                 [](const HostEnvironmentOverride &entry) { return entry.name == "SystemRoot"; });
  if (system_root == result.entries.end() || system_root->value.empty()) {
    result.detail = "Windows hosted compilation requires an explicit SystemRoot snapshot";
    return result;
  }
#endif
  std::error_code temporary_error;
  const std::filesystem::path temporary = std::filesystem::temp_directory_path(temporary_error);
  if (temporary_error) {
    result.detail =
      "could not resolve the hosted compiler temporary directory: " + temporary_error.message();
    return result;
  }
#if defined(_WIN32)
  result.entries.push_back(HostEnvironmentOverride{"TEMP", temporary.string()});
  result.entries.push_back(HostEnvironmentOverride{"TMP", temporary.string()});
#else
  result.entries.push_back(HostEnvironmentOverride{"TMPDIR", temporary.string()});
#endif
  std::sort(result.entries.begin(), result.entries.end(),
            [](const HostEnvironmentOverride &lhs, const HostEnvironmentOverride &rhs) {
              return lhs.name < rhs.name;
            });
  result.sha256 = aggregate_environment_identity(result.entries);
  return result;
}
struct ToolQueryResult {
  std::optional<std::string> value;
  std::string stdout_data;
  std::string stderr_data;
  std::string detail;
};

ToolQueryResult
run_bounded_tool_query(const std::vector<std::string> &arguments, std::string_view purpose,
                       const std::vector<HostEnvironmentOverride> &environment,
                       bool allow_empty_output = false,
                       CompilerTerminationSignalScope *termination_signals = nullptr) {
  HostProcessRequest request;
  request.executable_path = arguments.front();
  request.arguments = arguments;
  request.inherit_environment = false;
  request.environment_overrides = environment;
  request.stdin_mode = HostProcessInputMode::Discard;
  request.stdout_mode = HostProcessStreamMode::Capture;
  request.stderr_mode = HostProcessStreamMode::Capture;
  request.max_stdout_bytes = kToolQueryStreamLimitBytes;
  request.max_stderr_bytes = kToolQueryStreamLimitBytes;
  request.timeout_milliseconds = kToolQueryTimeoutMilliseconds;
  request.termination_signals = termination_signals;
  const HostProcessResult result = run_host_process(request);
  if (termination_signals != nullptr && result.parent_interruption_signal != 0 &&
      result.containment != HostProcessContainment::Confirmed) {
    termination_signals->suppress_emergency_redelivery();
  }
  if (result.timed_out) {
    ToolQueryResult query;
    query.detail =
      result.containment == HostProcessContainment::Confirmed
        ? std::string(purpose) + " timed out after confirmed containment-domain cleanup"
        : std::string(purpose) + " timed out and containment-domain cleanup could not be confirmed";
    if (!result.infrastructure_error.empty())
      query.detail += ": " + result.infrastructure_error;
    return query;
  }
  if (!result.succeeded()) {
    ToolQueryResult query;
    query.detail = std::string(purpose) + " failed";
    if (!result.infrastructure_error.empty()) {
      query.detail += ": " + result.infrastructure_error;
    } else if (result.termination_signal != 0) {
      query.detail += " with signal " + std::to_string(result.termination_signal);
    } else if (result.exited) {
      query.detail += " with exit status " + std::to_string(result.exit_code);
    } else {
      query.detail += " before a complete exit status was available";
    }
    return query;
  }
  ToolQueryResult query;
  query.stdout_data = result.stdout_data;
  query.stderr_data = result.stderr_data;
  query.value = trim_ascii_whitespace(query.stdout_data + query.stderr_data);
  if (query.value->empty() && !allow_empty_output) {
    query.value.reset();
    query.detail = std::string(purpose) + " produced no identity text";
  }
  return query;
}

struct ToolResolutionResult {
  std::optional<ResolvedToolIdentity> value;
  std::string detail;
};

bool write_exclusive_probe_source(const std::filesystem::path &path, std::string_view contents,
                                  std::string &detail) {
  detail.clear();
#if defined(_WIN32)
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    detail = "could not exclusively create the host archiver probe source: " +
             std::system_category().message(static_cast<int>(::GetLastError()));
    return false;
  }
  std::size_t offset = 0U;
  while (offset < contents.size()) {
    DWORD written = 0U;
    const DWORD requested = static_cast<DWORD>(contents.size() - offset);
    if (::WriteFile(file, contents.data() + offset, requested, &written, nullptr) == 0 ||
        written == 0U) {
      const DWORD error = written == 0U ? ERROR_WRITE_FAULT : ::GetLastError();
      detail = "could not write the host archiver probe source: " +
               std::system_category().message(static_cast<int>(error));
      if (::CloseHandle(file) == 0) {
        detail += "; could not close the probe source after the write failure: " +
                  std::system_category().message(static_cast<int>(::GetLastError()));
      }
      return false;
    }
    offset += written;
  }
  if (::CloseHandle(file) == 0) {
    detail = "could not close the host archiver probe source: " +
             std::system_category().message(static_cast<int>(::GetLastError()));
    return false;
  }
  return true;
#else
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    detail = "could not exclusively create the host archiver probe source: " +
             std::string(std::strerror(errno));
    return false;
  }
  std::size_t offset = 0U;
  while (offset < contents.size()) {
    const ssize_t written = ::write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      const int error = written == 0 ? EIO : errno;
      detail =
        "could not write the host archiver probe source: " + std::string(std::strerror(error));
      if (::close(descriptor) != 0) {
        detail += "; could not close the probe source after the write failure: " +
                  std::string(std::strerror(errno));
      }
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::close(descriptor) != 0) {
    detail = "could not close the host archiver probe source: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
#endif
}

struct ExecutablePathResolutionResult {
  enum class Error : std::uint8_t {
    None,
    InvalidCommand,
    NotFound,
    InvalidCandidate,
  };

  std::optional<std::filesystem::path> value;
  Error error = Error::None;
  std::string detail;
};

ExecutablePathResolutionResult
canonicalize_executable_candidate(const std::filesystem::path &candidate, std::string_view role) {
  ExecutablePathResolutionResult result;
  std::error_code error;
  const std::filesystem::path executable = std::filesystem::canonical(candidate, error);
  if (error) {
    result.error = ExecutablePathResolutionResult::Error::InvalidCandidate;
    result.detail = "could not canonicalize " + std::string(role) + ": " + error.message();
    return result;
  }
  const std::filesystem::file_status status = std::filesystem::symlink_status(executable, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    result.error = ExecutablePathResolutionResult::Error::InvalidCandidate;
    result.detail = error ? "could not inspect " + std::string(role) + ": " + error.message()
                          : std::string(role) + " is not a regular executable file";
    return result;
  }
#if !defined(_WIN32)
  std::string trust_detail;
  if (!validate_owner_controlled_executable(executable, trust_detail)) {
    result.error = ExecutablePathResolutionResult::Error::InvalidCandidate;
    result.detail =
      std::string(role) + " is outside the owner-controlled executable boundary: " + trust_detail;
    return result;
  }
#endif
  result.value = executable;
  return result;
}

ExecutablePathResolutionResult resolve_executable_path(std::string_view command,
                                                       std::string_view role) {
  ExecutablePathResolutionResult result;
  if (command.empty() || command.find('\0') != std::string_view::npos) {
    result.error = ExecutablePathResolutionResult::Error::InvalidCommand;
    result.detail = std::string(role) + " command is empty or contains NUL";
    return result;
  }
  const std::filesystem::path command_path(command);
  const bool explicit_path = command_path.has_parent_path() ||
                             command.find('/') != std::string_view::npos ||
                             command.find('\\') != std::string_view::npos;
  if (!explicit_path && command.find_first_of(" \t\r\n") != std::string_view::npos) {
    result.error = ExecutablePathResolutionResult::Error::InvalidCommand;
    result.detail = std::string(role) + " must name a single executable, not a shell command";
    return result;
  }
  const std::optional<std::filesystem::path> candidate = find_executable_on_path(command);
  if (!candidate.has_value()) {
    result.error = ExecutablePathResolutionResult::Error::NotFound;
    result.detail =
      std::string(role) + " executable could not be resolved: " + std::string(command);
    return result;
  }
  return canonicalize_executable_candidate(*candidate, role);
}

ToolResolutionResult
resolve_executable_identity_at_path(const std::filesystem::path &executable, bool query_version,
                                    std::string_view role,
                                    const std::vector<HostEnvironmentOverride> &environment,
                                    CompilerTerminationSignalScope *termination_signals) {
  ToolResolutionResult result;
  const FileDigestResult digest = sha256_file(executable);
  if (!digest.ok()) {
    result.detail = "could not hash " + std::string(role) + ": " + digest.detail;
    return result;
  }

  std::string version;
  if (query_version) {
    ToolQueryResult query = run_bounded_tool_query({executable.string(), "--version"},
                                                   std::string(role) + " version query",
                                                   environment, false, termination_signals);
    if (!query.value.has_value()) {
      result.detail = std::move(query.detail);
      return result;
    }
    version = std::move(*query.value);
  }
  result.value =
    ResolvedToolIdentity{executable, digest.value->size, digest.value->sha256, std::move(version)};
  return result;
}

ToolResolutionResult
resolve_executable_identity(std::string_view command, bool query_version, std::string_view role,
                            const std::vector<HostEnvironmentOverride> &environment,
                            CompilerTerminationSignalScope *termination_signals) {
  ExecutablePathResolutionResult executable = resolve_executable_path(command, role);
  if (!executable.value.has_value()) {
    ToolResolutionResult result;
    result.detail = std::move(executable.detail);
    return result;
  }
  return resolve_executable_identity_at_path(*executable.value, query_version, role, environment,
                                             termination_signals);
}

ToolQueryResult probe_archiver_capability(const std::filesystem::path &compiler,
                                          std::string_view standard_flag,
                                          const std::filesystem::path &archiver,
                                          const std::vector<HostEnvironmentOverride> &environment,
                                          CompilerTerminationSignalScope *termination_signals) {
  std::error_code error;
  const std::filesystem::path temporary_parent = std::filesystem::temp_directory_path(error);
  if (error) {
    ToolQueryResult result;
    result.detail = "could not resolve the host archiver probe directory: " + error.message();
    return result;
  }

  HostedObjectWorkspaceCreationResult created = create_hosted_object_workspace(temporary_parent);
  if (!created.ok()) {
    ToolQueryResult result;
    result.detail =
      "could not create a private host archiver probe workspace: " + std::move(created.detail);
    return result;
  }
  HostedObjectWorkspace &workspace = *created.workspace;
  const auto fail_with_cleanup = [&](std::string detail) {
    const HostedObjectWorkspaceCleanupResult cleanup = workspace.cleanup();
    if (!cleanup.ok())
      detail += "; host archiver probe cleanup also failed: " + cleanup.detail;
    ToolQueryResult result;
    result.detail = std::move(detail);
    return result;
  };

  const std::filesystem::path source = workspace.path() / "probe.cpp";
  const std::filesystem::path member = workspace.path() / "probe.o";
  const std::filesystem::path archive = workspace.path() / "probe.a";
  std::string source_detail;
  if (!write_exclusive_probe_source(
        source, "extern \"C\" int nebula_archiver_probe_symbol() { return 0; }\n", source_detail)) {
    return fail_with_cleanup(std::move(source_detail));
  }

  ToolQueryResult compilation = run_bounded_tool_query(
    {compiler.string(), std::string(standard_flag), "-c", source.string(), "-o", member.string()},
    "host archiver object probe compilation", environment, true, termination_signals);
  if (!compilation.value.has_value())
    return fail_with_cleanup(std::move(compilation.detail));
  if (!compilation.stdout_data.empty() || !compilation.stderr_data.empty()) {
    return fail_with_cleanup(
      "host archiver object probe compilation produced unexpected diagnostic output");
  }
  const FileDigestResult member_digest = sha256_file(member, kArchiverProbeMaxObjectBytes);
  if (!member_digest.ok()) {
    return fail_with_cleanup("host archiver object probe output is invalid: " +
                             member_digest.detail);
  }
  if (member_digest.value->size == 0U) {
    return fail_with_cleanup("host archiver object probe produced an empty object file");
  }

  ToolQueryResult execution = run_bounded_tool_query(
    {archiver.string(), "rcs", archive.string(), member.string()}, "host archiver capability probe",
    environment, true, termination_signals);
  if (!execution.value.has_value())
    return fail_with_cleanup(std::move(execution.detail));
  if (!execution.stdout_data.empty() || !execution.stderr_data.empty())
    return fail_with_cleanup("host archiver capability probe produced unexpected output");

  const FileDigestResult archive_digest = sha256_file(archive, kArchiverProbeMaxArchiveBytes);
  if (!archive_digest.ok()) {
    return fail_with_cleanup("host archiver capability probe output is invalid: " +
                             archive_digest.detail);
  }
  if (archive_digest.value->size <= kArchiveMagic.size()) {
    return fail_with_cleanup(
      "host archiver capability probe produced an archive without an object member");
  }
  const StableFilePrefixResult archive_prefix =
    read_stable_file_prefix(archive, kArchiveMagic.size(), kArchiverProbeMaxArchiveBytes);
  if (!archive_prefix.ok()) {
    return fail_with_cleanup("could not verify the host archiver probe header: " +
                             archive_prefix.detail);
  }
  if (archive_prefix.value->bytes.size() != kArchiveMagic.size() ||
      !std::equal(archive_prefix.value->bytes.begin(), archive_prefix.value->bytes.end(),
                  kArchiveMagic.begin())) {
    return fail_with_cleanup("host archiver capability probe produced an invalid archive header");
  }

  ToolQueryResult members = run_bounded_tool_query(
    {archiver.string(), "t", archive.string(), member.filename().string()},
    "host archiver member-list probe", environment, false, termination_signals);
  if (!members.value.has_value())
    return fail_with_cleanup(std::move(members.detail));
  if (!members.stderr_data.empty() ||
      trim_ascii_whitespace(members.stdout_data) != member.filename().string()) {
    return fail_with_cleanup(
      "host archiver capability probe did not retain exactly the requested object member");
  }

  const HostedObjectWorkspaceCleanupResult cleanup = workspace.cleanup();
  if (!cleanup.ok()) {
    ToolQueryResult result;
    result.detail = "host archiver capability probe cleanup failed: " + cleanup.detail;
    return result;
  }
  ToolQueryResult result;
  result.value = std::string(kArchiverCapabilityIdentity);
  return result;
}

std::string ascii_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
    return byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')
             ? static_cast<char>(
                 byte + (static_cast<unsigned char>('a') - static_cast<unsigned char>('A')))
             : static_cast<char>(byte);
  });
  return value;
}

std::optional<std::vector<ResolvedToolDependency>> resolve_compiler_dependencies(
  const ResolvedToolIdentity &compiler, const std::vector<HostEnvironmentOverride> &environment,
  std::string &detail, CompilerTerminationSignalScope *termination_signals) {
  const std::string version = ascii_lower(compiler.version);
  std::vector<std::string_view> program_names;
  if (version.find("clang") != std::string::npos) {
    // Clang's frontend and integrated assembler live in the driver binary.
    program_names = {"ld"};
  } else if (version.find("gcc") != std::string::npos || version.find("g++") != std::string::npos ||
             version.find("gnu") != std::string::npos) {
    program_names = {"cc1plus", "cc1", "as", "ld", "collect2"};
  } else {
    detail = "unsupported host compiler family; complete child-tool provenance is available only "
             "for Clang and GCC drivers";
    return std::nullopt;
  }

  std::vector<ResolvedToolDependency> dependencies;
  dependencies.reserve(program_names.size());
  for (const std::string_view program_name : program_names) {
    ToolQueryResult query = run_bounded_tool_query(
      {compiler.executable.string(), "-print-prog-name=" + std::string(program_name)},
      "compiler child-tool query for " + std::string(program_name), environment, false,
      termination_signals);
    if (!query.value.has_value()) {
      detail = std::move(query.detail);
      return std::nullopt;
    }
    ToolResolutionResult dependency = resolve_executable_identity(
      *query.value, false, "compiler child tool " + std::string(program_name), environment,
      termination_signals);
    if (!dependency.value.has_value()) {
      detail = std::move(dependency.detail);
      return std::nullopt;
    }
    dependencies.push_back(
      ResolvedToolDependency{std::string(program_name), std::move(*dependency.value)});
  }
  return dependencies;
}

bool valid_standard_flag(std::string_view flag) {
  return flag.starts_with("-std=") && flag.size() > 5U &&
         flag.find('\0') == std::string_view::npos &&
         flag.find_first_of(" \t\r\n") == std::string_view::npos;
}

ToolQueryResult probe_standard_flag(const std::filesystem::path &compiler, std::string_view flag,
                                    const std::vector<HostEnvironmentOverride> &environment,
                                    CompilerTerminationSignalScope *termination_signals) {
#if defined(_WIN32)
  constexpr std::string_view kNullDevice = "NUL";
#else
  constexpr std::string_view kNullDevice = "/dev/null";
#endif
  return run_bounded_tool_query(
    {compiler.string(), std::string(flag), "-x", "c++", "-fsyntax-only", std::string(kNullDevice)},
    "C++ standard capability query", environment, true, termination_signals);
}

std::optional<std::string>
select_standard_flag(const std::filesystem::path &compiler,
                     const std::optional<std::string> &requested,
                     const std::vector<HostEnvironmentOverride> &environment, std::string &detail,
                     CompilerTerminationSignalScope *termination_signals) {
  if (requested.has_value()) {
    if (!valid_standard_flag(*requested)) {
      detail = "NEBULA_CXX_STD_FLAG must be one NUL-free -std=<dialect> argument";
      return std::nullopt;
    }
    ToolQueryResult probe =
      probe_standard_flag(compiler, *requested, environment, termination_signals);
    if (!probe.value.has_value()) {
      detail = "configured C++ standard flag is unsupported: " + probe.detail;
      return std::nullopt;
    }
    return *requested;
  }
  std::string failures;
  for (const std::string_view candidate :
       {std::string_view("-std=c++23"), std::string_view("-std=c++2b")}) {
    ToolQueryResult probe =
      probe_standard_flag(compiler, candidate, environment, termination_signals);
    if (probe.value.has_value())
      return std::string(candidate);
    if (!failures.empty())
      failures += "; ";
    failures += std::string(candidate) + ": " + probe.detail;
  }
  detail = "compiler does not provide a verified C++23 dialect: " + failures;
  return std::nullopt;
}

bool revalidate_tool(const ResolvedToolIdentity &expected, std::string_view role,
                     std::string &detail) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(expected.executable, error);
  if (error || canonical != expected.executable) {
    detail = error ? "could not canonicalize " + std::string(role) +
                       " during revalidation: " + error.message()
                   : std::string(role) + " canonical path changed during revalidation";
    return false;
  }
  const FileDigestResult digest = sha256_file(expected.executable);
  if (!digest.ok()) {
    detail = "could not re-hash " + std::string(role) + ": " + digest.detail;
    return false;
  }
  if (digest.value->size != expected.size || digest.value->sha256 != expected.sha256) {
    detail = std::string(role) + " content identity changed after resolution";
    return false;
  }
  return true;
}

HostedToolchainResolutionResult resolution_failure(std::string detail) {
  HostedToolchainResolutionResult result;
  result.detail = std::move(detail);
  return result;
}

HostedToolPathResolutionResult path_resolution_failure(HostedToolPathFailureKind failure,
                                                       std::string detail) {
  HostedToolPathResolutionResult result;
  result.failure = failure;
  result.detail = std::move(detail);
  return result;
}

bool same_hosted_tool_paths(const ResolvedHostedToolPaths &lhs,
                            const ResolvedHostedToolPaths &rhs) {
  return lhs.compiler == rhs.compiler && lhs.archiver == rhs.archiver &&
         lhs.nebula_executable == rhs.nebula_executable;
}

} // namespace

struct HostedToolchainResolverAccess {
  static ResolvedHostedToolchain
  create(ResolvedToolIdentity compiler, std::optional<ResolvedToolIdentity> archiver,
         ResolvedToolIdentity nebula_executable, std::string target_triple,
         std::string cxx_standard_flag, std::filesystem::path working_directory,
         std::vector<ResolvedToolDependency> compiler_dependencies,
         std::vector<HostEnvironmentOverride> execution_environment, std::string environment_sha256,
         CompilerTerminationSignalScope *termination_signals) {
    return ResolvedHostedToolchain(
      std::move(compiler), std::move(archiver), std::move(nebula_executable),
      std::move(target_triple), std::move(cxx_standard_flag), std::move(working_directory),
      std::move(compiler_dependencies), std::move(execution_environment),
      std::move(environment_sha256), termination_signals);
  }
};

ResolvedHostedToolchain::ResolvedHostedToolchain(
  ResolvedToolIdentity compiler, std::optional<ResolvedToolIdentity> archiver,
  ResolvedToolIdentity nebula_executable, std::string target_triple, std::string cxx_standard_flag,
  std::filesystem::path working_directory,
  std::vector<ResolvedToolDependency> compiler_dependencies,
  std::vector<HostEnvironmentOverride> execution_environment, std::string environment_sha256,
  CompilerTerminationSignalScope *termination_signals)
    : compiler_(std::move(compiler)), archiver_(std::move(archiver)),
      nebula_executable_(std::move(nebula_executable)), target_triple_(std::move(target_triple)),
      cxx_standard_flag_(std::move(cxx_standard_flag)),
      working_directory_(std::move(working_directory)),
      compiler_dependencies_(std::move(compiler_dependencies)),
      execution_environment_(std::move(execution_environment)),
      environment_sha256_(std::move(environment_sha256)),
      termination_signals_(termination_signals) {}

HostProcessResult ResolvedHostedToolchain::execute(const std::vector<std::string> &arguments,
                                                   std::uint32_t timeout_milliseconds) const {
  HostProcessResult failure;
  if (arguments.empty()) {
    failure.infrastructure_error = "resolved hosted tool execution requires logical argv[0]";
    return failure;
  }
  const std::filesystem::path executable(arguments.front());
  const bool is_compiler = executable == compiler_.executable;
  const bool is_archiver = archiver_.has_value() && executable == archiver_->executable;
  if (!is_compiler && !is_archiver) {
    failure.infrastructure_error =
      "resolved hosted tool execution rejected an executable outside its bounded snapshot";
    return failure;
  }
  HostProcessRequest request;
  request.executable_path = executable;
  request.arguments = arguments;
  request.inherit_environment = false;
  request.environment_overrides = execution_environment_;
  request.stdin_mode = HostProcessInputMode::Discard;
  request.timeout_milliseconds = timeout_milliseconds;
  request.termination_signals = termination_signals_;
  HostProcessResult result = run_host_process(request);
  if (termination_signals_ != nullptr && result.parent_interruption_signal != 0 &&
      result.containment != HostProcessContainment::Confirmed) {
    termination_signals_->suppress_emergency_redelivery();
  }
  return result;
}

bool ResolvedHostedToolchain::revalidate(std::string &detail) const {
  detail.clear();
  if (!revalidate_tool(compiler_, "host C++ compiler", detail))
    return false;
  for (const ResolvedToolDependency &dependency : compiler_dependencies_) {
    if (!revalidate_tool(dependency.identity, "compiler child tool " + dependency.role, detail)) {
      return false;
    }
  }
  if (archiver_.has_value() && !revalidate_tool(*archiver_, "host archiver", detail))
    return false;
  if (!revalidate_tool(nebula_executable_, "Nebula executable", detail))
    return false;
  std::error_code working_directory_error;
  const std::filesystem::path current_path = std::filesystem::current_path(working_directory_error);
  std::filesystem::path current_working_directory;
  if (!working_directory_error) {
    current_working_directory = std::filesystem::canonical(current_path, working_directory_error);
  }
  if (working_directory_error || current_working_directory != working_directory_) {
    detail = working_directory_error
               ? "could not revalidate the compilation working directory: " +
                   working_directory_error.message()
               : "compilation working directory changed after toolchain resolution";
    return false;
  }
  if (aggregate_environment_identity(execution_environment_) != environment_sha256_) {
    detail = "bounded compilation environment snapshot was corrupted";
    return false;
  }
  return true;
}

std::string ResolvedHostedToolchain::provenance_identity() const {
  std::ostringstream identity;
  identity << "resolved-hosted-toolchain-v3\n";
  append_length_delimited(identity, "compiler_path", compiler_.executable.generic_string());
  identity << "compiler_size=" << compiler_.size << '\n';
  identity << "compiler_sha256=" << compiler_.sha256 << '\n';
  append_length_delimited(identity, "compiler_version", compiler_.version);
  append_length_delimited(identity, "target_triple", target_triple_);
  append_length_delimited(identity, "cxx_standard_flag", cxx_standard_flag_);
  append_length_delimited(identity, "working_directory", working_directory_.generic_string());
  identity << "environment_sha256=" << environment_sha256_ << '\n';
  append_length_delimited(identity, "nebula_executable_path",
                          nebula_executable_.executable.generic_string());
  identity << "nebula_executable_size=" << nebula_executable_.size << '\n';
  identity << "nebula_executable_sha256=" << nebula_executable_.sha256 << '\n';
  identity << "archiver_present=" << (archiver_.has_value() ? '1' : '0') << '\n';
  if (archiver_.has_value()) {
    append_length_delimited(identity, "archiver_path", archiver_->executable.generic_string());
    identity << "archiver_size=" << archiver_->size << '\n';
    identity << "archiver_sha256=" << archiver_->sha256 << '\n';
    append_length_delimited(identity, "archiver_identity_evidence", archiver_->version);
  }
  identity << "compiler_dependency_count=" << compiler_dependencies_.size() << '\n';
  for (const ResolvedToolDependency &dependency : compiler_dependencies_) {
    append_length_delimited(identity, "compiler_dependency_role", dependency.role);
    append_length_delimited(identity, "compiler_dependency_path",
                            dependency.identity.executable.generic_string());
    identity << "compiler_dependency_size=" << dependency.identity.size << '\n';
    identity << "compiler_dependency_sha256=" << dependency.identity.sha256 << '\n';
  }
  return identity.str();
}

HostedToolPathResolutionResult resolve_hosted_tool_paths(const HostedToolchainRequest &request) {
  std::string compiler_command = request.compiler_command;
  if (compiler_command.empty()) {
    const char *configured = std::getenv("CXX");
    compiler_command = configured != nullptr && *configured != '\0' ? configured : "clang++";
  }
  ExecutablePathResolutionResult compiler =
    resolve_executable_path(compiler_command, "host C++ compiler");
  if (!compiler.value.has_value()) {
    const HostedToolPathFailureKind failure =
      compiler.error == ExecutablePathResolutionResult::Error::InvalidCommand
        ? HostedToolPathFailureKind::InvalidCompilerCommand
      : compiler.error == ExecutablePathResolutionResult::Error::NotFound
        ? HostedToolPathFailureKind::CompilerUnavailable
        : HostedToolPathFailureKind::CompilerUnsafe;
    return path_resolution_failure(failure, std::move(compiler.detail));
  }

  std::optional<std::filesystem::path> archiver;
  if (request.require_archiver) {
    ExecutablePathResolutionResult resolved_archiver;
    if (!request.archiver_command.empty()) {
      resolved_archiver = resolve_executable_path(request.archiver_command, "host archiver");
    } else {
      const std::optional<std::filesystem::path> llvm_ar = find_executable_on_path("llvm-ar");
      resolved_archiver = llvm_ar.has_value()
                            ? canonicalize_executable_candidate(*llvm_ar, "host archiver")
                            : resolve_executable_path("ar", "host archiver");
    }
    if (!resolved_archiver.value.has_value()) {
      const HostedToolPathFailureKind failure =
        resolved_archiver.error == ExecutablePathResolutionResult::Error::NotFound
          ? HostedToolPathFailureKind::ArchiverUnavailable
          : HostedToolPathFailureKind::ArchiverUnsafe;
      return path_resolution_failure(failure, std::move(resolved_archiver.detail));
    }
    archiver = std::move(*resolved_archiver.value);
  }

  if (request.self_executable.empty()) {
    return path_resolution_failure(HostedToolPathFailureKind::NebulaExecutableUnavailable,
                                   "Nebula executable path is unavailable for build provenance");
  }
  ExecutablePathResolutionResult self =
    resolve_executable_path(request.self_executable.string(), "Nebula executable");
  if (!self.value.has_value()) {
    const HostedToolPathFailureKind failure =
      self.error == ExecutablePathResolutionResult::Error::NotFound
        ? HostedToolPathFailureKind::NebulaExecutableUnavailable
        : HostedToolPathFailureKind::NebulaExecutableUnsafe;
    return path_resolution_failure(failure, std::move(self.detail));
  }

  HostedToolPathResolutionResult result;
  result.value = ResolvedHostedToolPaths{std::move(*compiler.value), std::move(archiver),
                                         std::move(*self.value)};
  return result;
}

namespace {

HostedToolchainResolutionResult
resolve_hosted_toolchain_from_paths(const HostedToolchainRequest &request,
                                    const ResolvedHostedToolPaths &paths) {
  EnvironmentSnapshotResult environment = capture_execution_environment();
  if (!environment.detail.empty())
    return resolution_failure(std::move(environment.detail));

  ToolResolutionResult compiler = resolve_executable_identity_at_path(
    paths.compiler, true, "host C++ compiler", environment.entries, request.termination_signals);
  if (!compiler.value.has_value())
    return resolution_failure(std::move(compiler.detail));

  std::string dependency_detail;
  std::optional<std::vector<ResolvedToolDependency>> compiler_dependencies =
    resolve_compiler_dependencies(*compiler.value, environment.entries, dependency_detail,
                                  request.termination_signals);
  if (!compiler_dependencies.has_value()) {
    return resolution_failure(std::move(dependency_detail));
  }

  ToolQueryResult target = run_bounded_tool_query(
    {compiler.value->executable.string(), "-dumpmachine"}, "compiler target-triple query",
    environment.entries, false, request.termination_signals);
  if (!target.value.has_value())
    return resolution_failure(std::move(target.detail));
  if (target.value->find_first_of(" \t\r\n") != std::string::npos ||
      target.value->find('\0') != std::string::npos) {
    return resolution_failure("compiler target-triple query returned malformed identity text");
  }

  std::optional<std::string> standard_override = request.cxx_standard_override;
  if (!standard_override.has_value()) {
    const char *configured = std::getenv("NEBULA_CXX_STD_FLAG");
    if (configured != nullptr && *configured != '\0')
      standard_override = configured;
  }
  std::string standard_detail;
  std::optional<std::string> standard =
    select_standard_flag(compiler.value->executable, standard_override, environment.entries,
                         standard_detail, request.termination_signals);
  if (!standard.has_value())
    return resolution_failure(std::move(standard_detail));

  std::optional<ResolvedToolIdentity> archiver;
  if (request.require_archiver) {
    if (!paths.archiver.has_value()) {
      return resolution_failure("hosted tool path preview omitted the required archiver");
    }
    ToolResolutionResult resolved_archiver = resolve_executable_identity_at_path(
      *paths.archiver, false, "host archiver", environment.entries, request.termination_signals);
    if (!resolved_archiver.value.has_value()) {
      return resolution_failure(std::move(resolved_archiver.detail));
    }
    const auto revalidate_archiver_probe_inputs = [&](std::string &detail) {
      if (!revalidate_tool(*compiler.value, "host C++ compiler", detail))
        return false;
      for (const ResolvedToolDependency &dependency : *compiler_dependencies) {
        if (!revalidate_tool(dependency.identity, "compiler child tool " + dependency.role,
                             detail)) {
          return false;
        }
      }
      return revalidate_tool(*resolved_archiver.value, "host archiver", detail);
    };
    std::string pre_probe_detail;
    if (!revalidate_archiver_probe_inputs(pre_probe_detail)) {
      return resolution_failure("hosted tool identity changed before archiver capability probe: " +
                                pre_probe_detail);
    }
    ToolQueryResult capability = probe_archiver_capability(
      compiler.value->executable, *standard, resolved_archiver.value->executable,
      environment.entries, request.termination_signals);
    std::string post_probe_detail;
    const bool inputs_unchanged = revalidate_archiver_probe_inputs(post_probe_detail);
    if (!capability.value.has_value()) {
      std::string detail = std::move(capability.detail);
      if (!inputs_unchanged)
        detail +=
          "; hosted tool identity also changed during the failed probe: " + post_probe_detail;
      return resolution_failure(std::move(detail));
    }
    if (!inputs_unchanged)
      return resolution_failure("hosted tool identity changed during archiver capability probe: " +
                                post_probe_detail);
    resolved_archiver.value->version = std::move(*capability.value);
    archiver = std::move(*resolved_archiver.value);
  }

  ToolResolutionResult self =
    resolve_executable_identity_at_path(paths.nebula_executable, false, "Nebula executable",
                                        environment.entries, request.termination_signals);
  if (!self.value.has_value())
    return resolution_failure(std::move(self.detail));

  std::error_code working_directory_error;
  const std::filesystem::path current_path = std::filesystem::current_path(working_directory_error);
  if (working_directory_error) {
    return resolution_failure("could not inspect the compilation working directory: " +
                              working_directory_error.message());
  }
  const std::filesystem::path working_directory =
    std::filesystem::canonical(current_path, working_directory_error);
  if (working_directory_error) {
    return resolution_failure("could not canonicalize the compilation working directory: " +
                              working_directory_error.message());
  }

  ResolvedHostedToolchain resolved = HostedToolchainResolverAccess::create(
    std::move(*compiler.value), std::move(archiver), std::move(*self.value),
    std::move(*target.value), std::move(*standard), working_directory,
    std::move(*compiler_dependencies), std::move(environment.entries),
    std::move(environment.sha256), request.termination_signals);
  std::string revalidation_detail;
  if (!resolved.revalidate(revalidation_detail)) {
    return resolution_failure("hosted toolchain changed during resolution: " + revalidation_detail);
  }
  HostedToolchainResolutionResult result;
  result.value = std::move(resolved);
  return result;
}

} // namespace

HostedToolchainResolutionResult resolve_hosted_toolchain(const HostedToolchainRequest &request) {
  HostedToolPathResolutionResult paths = resolve_hosted_tool_paths(request);
  if (!paths.value.has_value())
    return resolution_failure(std::move(paths.detail));
  return resolve_hosted_toolchain_from_paths(request, *paths.value);
}

HostedToolchainResolutionResult
resolve_hosted_toolchain(const HostedToolchainRequest &request,
                         const ResolvedHostedToolPaths &expected_paths) {
  HostedToolPathResolutionResult current_paths = resolve_hosted_tool_paths(request);
  if (!current_paths.value.has_value()) {
    return resolution_failure(std::move(current_paths.detail));
  }
  if (!same_hosted_tool_paths(*current_paths.value, expected_paths)) {
    return resolution_failure(
      "hosted tool paths changed after output-conflict preflight and before identity queries");
  }
  return resolve_hosted_toolchain_from_paths(request, expected_paths);
}

} // namespace nebula::cli
