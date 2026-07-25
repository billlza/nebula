#include "hosted_native_dependencies.hpp"

#include "host_process.hpp"
#include "path_security.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nebula::cli {
namespace {

namespace fs = std::filesystem;

HostedNativeDependencyDiscoveryResult failure(int exit_code, std::string detail) {
  HostedNativeDependencyDiscoveryResult result;
  result.exit_code = exit_code;
  result.detail = std::move(detail);
  return result;
}

void update_u64_be(Sha256Digest &digest, std::uint64_t value) {
  std::array<std::uint8_t, 8U> encoded{};
  for (std::size_t index = 0U; index < encoded.size(); ++index) {
    const unsigned shift = static_cast<unsigned>((encoded.size() - index - 1U) * 8U);
    encoded[index] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
  digest.update(encoded);
}

void update_text(Sha256Digest &digest, std::string_view text) {
  update_u64_be(digest, static_cast<std::uint64_t>(text.size()));
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(text.data());
  digest.update(std::span<const std::uint8_t>(bytes, text.size()));
}

std::string snapshot_identity(const std::vector<HostedNativeDependencyFile> &files) {
  Sha256Digest digest;
  constexpr std::string_view domain = "nebula-hosted-native-dependencies-v1";
  update_text(digest, domain);
  update_u64_be(digest, static_cast<std::uint64_t>(files.size()));
  for (const HostedNativeDependencyFile &file : files) {
    update_text(digest, file.canonical_path.generic_string());
    update_u64_be(digest, static_cast<std::uint64_t>(file.content.size));
    update_text(digest, file.content.sha256);
  }
  return digest.finish_hex();
}

bool is_forbidden_discovery_argument(std::string_view argument) {
  if (argument.empty() || argument.front() == '@')
    return true;
  if (argument == "-c" || argument == "-E" || argument == "-S" || argument == "-MG" ||
      argument == "-MP" || argument == "-o" || argument == "--dependency-file") {
    return true;
  }
  for (const std::string_view prefix :
       {std::string_view("-MF"), std::string_view("-MT"), std::string_view("-MQ"),
        std::string_view("-MJ"), std::string_view("-Wp,-M"), std::string_view("-dependency-file"),
        std::string_view("--dependency-file="), std::string_view("-Xclang=-M"),
        std::string_view("-Xclang=-dependency-file"), std::string_view("-fdeps-file="),
        std::string_view("-fdep-file=")}) {
    if (argument.starts_with(prefix))
      return true;
  }
  return argument == "-M" || argument == "-MM" || argument == "-MD" || argument == "-MMD";
}

#if !defined(_WIN32)
bool validate_private_scratch_directory(const fs::path &directory, std::string &detail) {
  struct stat state{};
  if (::lstat(directory.c_str(), &state) != 0) {
    detail = "could not inspect dependency scratch-directory ownership: " +
             std::string(std::strerror(errno));
    return false;
  }
  if (!S_ISDIR(state.st_mode) || state.st_uid != ::geteuid()) {
    detail = "dependency scratch directory must be a directory owned by the effective user";
    return false;
  }
  if ((state.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    detail = "dependency scratch directory must not grant group or other access";
    return false;
  }
  return true;
}
#endif

std::optional<fs::path> validate_scratch_directory(const fs::path &scratch_directory,
                                                   std::string &detail) {
  std::error_code directory_error;
  const fs::file_status directory_status = fs::symlink_status(scratch_directory, directory_error);
  if (directory_error) {
    detail = "could not inspect dependency scratch directory: " + directory_error.message();
    return std::nullopt;
  }
  if (fs::is_symlink(directory_status) || !fs::is_directory(directory_status)) {
    detail = "dependency scratch path must be a non-symlink directory";
    return std::nullopt;
  }
  const fs::path canonical_scratch = fs::canonical(scratch_directory, directory_error);
  if (directory_error) {
    detail = "could not canonicalize dependency scratch directory: " + directory_error.message();
    return std::nullopt;
  }
#if !defined(_WIN32)
  if (!validate_owner_controlled_directory_chain(canonical_scratch, detail)) {
    detail =
      "dependency scratch directory is outside the owner-controlled trust boundary: " + detail;
    return std::nullopt;
  }
  if (!validate_private_scratch_directory(canonical_scratch, detail)) {
    return std::nullopt;
  }
#endif
  return canonical_scratch;
}

struct DepfileReadResult {
  std::string text;
  std::uintmax_t size = 0U;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return detail.empty(); }
};

DepfileReadResult read_stable_depfile(const fs::path &path, std::uintmax_t max_bytes) {
  DepfileReadResult result;
  const FileDigestResult expected = sha256_file(path, max_bytes);
  if (!expected.ok()) {
    result.detail = "could not establish dependency-file identity: " + expected.detail;
    return result;
  }
  if (expected.value->size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    result.detail = "dependency file cannot be represented in memory on this host";
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    result.detail = "could not open compiler dependency file";
    return result;
  }
  result.text.resize(static_cast<std::size_t>(expected.value->size));
  if (!result.text.empty()) {
    input.read(result.text.data(), static_cast<std::streamsize>(result.text.size()));
  }
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    result.detail = "compiler dependency file changed or became unreadable while loading";
    return result;
  }
  input.close();
  if (input.fail()) {
    result.detail = "could not close compiler dependency file after loading";
    return result;
  }
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(result.text.data());
  const std::string observed_sha256 =
    sha256_hex(std::span<const std::uint8_t>(bytes, result.text.size()));
  if (result.text.size() != expected.value->size || observed_sha256 != expected.value->sha256) {
    result.detail = "compiler dependency file changed while loading";
    return result;
  }
  result.size = expected.value->size;
  return result;
}

std::optional<fs::path> canonical_regular_dependency(const fs::path &spelling,
                                                     const fs::path &working_directory,
                                                     std::string &detail) {
#if !defined(_WIN32)
  const fs::path candidate = spelling.is_absolute() ? spelling : working_directory / spelling;
  if (candidate.empty() || !candidate.is_absolute()) {
    detail = "compiler-discovered dependency did not resolve to an absolute path";
    return std::nullopt;
  }

  const uid_t effective_user = ::geteuid();
  const auto trusted_owner = [effective_user](const struct stat &state) {
    return state.st_uid == 0 || state.st_uid == effective_user;
  };
  const auto validate_directory = [&](const fs::path &path, const struct stat &state) -> bool {
    if (!S_ISDIR(state.st_mode)) {
      detail = "compiler dependency path contains a non-directory component: " + path.string();
      return false;
    }
    std::string trust_detail;
    if (validate_owner_controlled_directory(path, trust_detail))
      return true;
    detail = "compiler dependency directory is outside the owner-controlled trust boundary: " +
             trust_detail;
    return false;
  };

  fs::path resolved = candidate.root_path();
  struct stat root_state{};
  if (resolved.empty() || ::lstat(resolved.c_str(), &root_state) != 0) {
    detail = "could not inspect compiler dependency root: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  if (!validate_directory(resolved, root_state))
    return std::nullopt;

  std::deque<fs::path> pending;
  for (const fs::path &component : candidate.relative_path())
    pending.push_back(component);
  std::size_t followed_links = 0U;
  while (!pending.empty()) {
    const fs::path component = std::move(pending.front());
    pending.pop_front();
    if (component.empty() || component == ".")
      continue;
    if (component == "..") {
      resolved = resolved.parent_path();
      if (resolved.empty())
        resolved = candidate.root_path();
      continue;
    }

    const fs::path next = resolved / component;
    struct stat state{};
    if (::lstat(next.c_str(), &state) != 0) {
      detail = "could not inspect compiler-discovered dependency " + spelling.string() + ": " +
               std::string(std::strerror(errno));
      return std::nullopt;
    }
    if (S_ISLNK(state.st_mode)) {
      if (!trusted_owner(state)) {
        detail = "compiler dependency path contains a symbolic link owned by another user: " +
                 next.string();
        return std::nullopt;
      }
      if (++followed_links > 64U) {
        detail = "compiler dependency path exceeds the symbolic-link traversal limit";
        return std::nullopt;
      }
      std::error_code link_error;
      const fs::path target = fs::read_symlink(next, link_error);
      if (link_error || target.empty()) {
        detail = link_error
                   ? "could not read compiler dependency symbolic link: " + link_error.message()
                   : "compiler dependency symbolic link has an empty target";
        return std::nullopt;
      }
      std::deque<fs::path> expanded;
      for (const fs::path &target_component : target.relative_path())
        expanded.push_back(target_component);
      expanded.insert(expanded.end(), pending.begin(), pending.end());
      pending = std::move(expanded);
      if (target.is_absolute()) {
        resolved = target.root_path();
        struct stat target_root_state{};
        if (resolved.empty() || ::lstat(resolved.c_str(), &target_root_state) != 0) {
          detail = "could not inspect compiler dependency symbolic-link root: " +
                   std::string(std::strerror(errno));
          return std::nullopt;
        }
        if (!validate_directory(resolved, target_root_state))
          return std::nullopt;
      }
      continue;
    }

    if (!pending.empty()) {
      if (!validate_directory(next, state))
        return std::nullopt;
      resolved = next;
      continue;
    }
    if (!S_ISREG(state.st_mode)) {
      detail = "compiler-discovered dependency is not a regular file: " + next.string();
      return std::nullopt;
    }
    if (!trusted_owner(state)) {
      detail = "compiler-discovered dependency is owned by another user: " + next.string();
      return std::nullopt;
    }
    if ((state.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      detail =
        "compiler-discovered dependency is writable by another trust domain: " + next.string();
      return std::nullopt;
    }
    return next.lexically_normal();
  }
  detail = "compiler-discovered dependency resolves to a directory instead of a regular file";
  return std::nullopt;
#else
  std::error_code error;
  const fs::path candidate = spelling.is_absolute() ? spelling : working_directory / spelling;
  const fs::path canonical = fs::canonical(candidate, error);
  if (error) {
    detail = "could not canonicalize compiler-discovered dependency " + spelling.string() + ": " +
             error.message();
    return std::nullopt;
  }
  const fs::file_status status = fs::symlink_status(canonical, error);
  if (error || !fs::is_regular_file(status)) {
    detail = error ? "could not inspect compiler-discovered dependency " + canonical.string() +
                       ": " + error.message()
                   : "compiler-discovered dependency is not a regular file: " + canonical.string();
    return std::nullopt;
  }
  return canonical;
#endif
}

struct DependencyPathPass {
  std::vector<fs::path> paths;
  std::uintmax_t depfile_bytes = 0U;
  int exit_code = 0;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return exit_code == 0 && detail.empty(); }
};

DependencyPathPass discover_dependency_paths(const ResolvedHostedToolchain &toolchain,
                                             const std::vector<HostedNativeDependencyUnit> &units,
                                             const fs::path &scratch_directory, unsigned pass,
                                             const HostedNativeDependencyLimits &limits,
                                             std::uint32_t timeout_milliseconds) {
  DependencyPathPass result;
  std::size_t encoded_path_bytes = 0U;

  for (std::size_t unit_index = 0U; unit_index < units.size(); ++unit_index) {
    const HostedNativeDependencyUnit &unit = units[unit_index];
    std::optional<fs::path> excluded_source;
    if (unit.exclude_source_from_snapshot) {
      std::string excluded_detail;
      excluded_source =
        canonical_regular_dependency(unit.source, toolchain.working_directory(), excluded_detail);
      if (!excluded_source.has_value()) {
        result.exit_code = 125;
        result.detail = "could not bind excluded generated dependency source: " + excluded_detail;
        return result;
      }
    }
    const std::string target =
      "nebula_dependency_target_" + std::to_string(pass) + "_" + std::to_string(unit_index);
    const fs::path depfile = scratch_directory / (".nebula-dependencies-" + std::to_string(pass) +
                                                  "-" + std::to_string(unit_index) + ".d");

    std::error_code inspect_error;
    const fs::file_status existing = fs::symlink_status(depfile, inspect_error);
    const bool destination_missing =
      inspect_error == std::errc::no_such_file_or_directory ||
      (!inspect_error && existing.type() == fs::file_type::not_found);
    if (!destination_missing) {
      result.exit_code = 125;
      result.detail =
        inspect_error ? "could not inspect dependency-file destination: " + inspect_error.message()
                      : "dependency-file destination already exists";
      return result;
    }

    std::vector<std::string> command;
    command.reserve(unit.compiler_arguments.size() + 8U);
    command.push_back(toolchain.compiler().executable.string());
    for (const std::string &argument : unit.compiler_arguments) {
      if (argument.find('\0') != std::string::npos || is_forbidden_discovery_argument(argument)) {
        result.exit_code = 125;
        result.detail = "dependency discovery received a conflicting or unsafe compiler argument";
        return result;
      }
      command.push_back(argument);
    }
    command.push_back("-M");
    command.push_back("-MF");
    command.push_back(depfile.string());
    command.push_back("-MT");
    command.push_back(target);
    command.push_back(unit.source.string());

    std::string toolchain_detail;
    if (!toolchain.revalidate(toolchain_detail)) {
      result.exit_code = 125;
      result.detail = "hosted toolchain changed before dependency discovery: " + toolchain_detail;
      return result;
    }
    const HostProcessResult execution = toolchain.execute(command, timeout_milliseconds);
    std::string execution_error;
    const int execution_exit = host_process_compatible_exit_code(execution, execution_error);

    std::error_code existence_error;
    const fs::file_status depfile_status = fs::symlink_status(depfile, existence_error);
    const bool depfile_missing =
      existence_error == std::errc::no_such_file_or_directory ||
      (!existence_error && depfile_status.type() == fs::file_type::not_found);
    if (depfile_missing)
      existence_error.clear();
    const bool depfile_exists = !existence_error && !depfile_missing;
    auto remove_depfile = [&]() -> std::string {
      if (existence_error) {
        return "could not inspect dependency file during cleanup: " + existence_error.message();
      }
      if (!depfile_exists)
        return {};
      std::error_code remove_error;
      if (!fs::remove(depfile, remove_error) || remove_error) {
        return remove_error ? "could not remove dependency file: " + remove_error.message()
                            : "dependency file disappeared before identity-bound cleanup";
      }
      return {};
    };

    if (execution_exit != 0) {
      const std::string cleanup_detail = remove_depfile();
      result.exit_code = cleanup_detail.empty() ? execution_exit : 125;
      result.detail = execution_error.empty()
                        ? "host compiler dependency discovery exited with status " +
                            std::to_string(execution_exit)
                        : execution_error;
      if (!cleanup_detail.empty())
        result.detail += "; " + cleanup_detail;
      return result;
    }
    if (!execution_error.empty()) {
      const std::string cleanup_detail = remove_depfile();
      result.exit_code = 125;
      result.detail = execution_error;
      if (!cleanup_detail.empty())
        result.detail += "; " + cleanup_detail;
      return result;
    }
    toolchain_detail.clear();
    if (!toolchain.revalidate(toolchain_detail)) {
      const std::string cleanup_detail = remove_depfile();
      result.exit_code = 125;
      result.detail = "hosted toolchain changed during dependency discovery: " + toolchain_detail;
      if (!cleanup_detail.empty())
        result.detail += "; " + cleanup_detail;
      return result;
    }
    if (existence_error || !depfile_exists) {
      result.exit_code = 125;
      result.detail = existence_error
                        ? "could not inspect compiler dependency file: " + existence_error.message()
                        : "host compiler reported success without producing its dependency file";
      return result;
    }

    DepfileReadResult read = read_stable_depfile(depfile, limits.max_depfile_bytes);
    const std::string cleanup_detail = remove_depfile();
    if (!read.ok() || !cleanup_detail.empty()) {
      result.exit_code = 125;
      result.detail = read.ok() ? cleanup_detail : std::move(read.detail);
      if (!read.ok() && !cleanup_detail.empty())
        result.detail += "; " + cleanup_detail;
      return result;
    }
    if (result.depfile_bytes > limits.max_total_depfile_bytes ||
        read.size > limits.max_total_depfile_bytes - result.depfile_bytes) {
      result.exit_code = 125;
      result.detail = "compiler dependency files exceed the bounded aggregate size";
      return result;
    }
    result.depfile_bytes += read.size;

    const detail::MakeDependencyParseResult parsed = detail::parse_make_dependency_rule(
      read.text, target, limits.max_dependencies, limits.max_encoded_path_bytes);
    if (!parsed.ok()) {
      result.exit_code = 125;
      result.detail = "could not parse compiler dependency file: " + parsed.detail;
      return result;
    }
    for (const std::string &dependency : parsed.dependencies) {
      std::string canonical_detail;
      const std::optional<fs::path> canonical = canonical_regular_dependency(
        fs::path(dependency), toolchain.working_directory(), canonical_detail);
      if (!canonical.has_value()) {
        result.exit_code = 125;
        result.detail = std::move(canonical_detail);
        return result;
      }
      if (excluded_source.has_value() && *canonical == *excluded_source)
        continue;
      const std::string encoded = canonical->generic_string();
      if (result.paths.size() >= limits.max_dependencies ||
          encoded_path_bytes > limits.max_encoded_path_bytes ||
          encoded.size() > limits.max_encoded_path_bytes - encoded_path_bytes) {
        result.exit_code = 125;
        result.detail = "compiler dependency closure exceeds the bounded path count or size";
        return result;
      }
      encoded_path_bytes += encoded.size();
      result.paths.push_back(*canonical);
    }
  }

  std::sort(result.paths.begin(), result.paths.end(),
            [](const fs::path &left, const fs::path &right) {
              return left.generic_string() < right.generic_string();
            });
  result.paths.erase(std::unique(result.paths.begin(), result.paths.end()), result.paths.end());
  return result;
}

HostedNativeDependencyDiscoveryResult snapshot_paths(const std::vector<fs::path> &paths,
                                                     const HostedNativeDependencyLimits &limits) {
  std::vector<HostedNativeDependencyFile> files;
  files.reserve(paths.size());
  std::uintmax_t total_file_bytes = 0U;
  for (const fs::path &path : paths) {
    const std::uintmax_t remaining = total_file_bytes <= limits.max_total_file_bytes
                                       ? limits.max_total_file_bytes - total_file_bytes
                                       : 0U;
    const FileDigestResult digest = sha256_file(path, std::min(limits.max_file_bytes, remaining));
    if (!digest.ok()) {
      return failure(125, "could not hash compiler-discovered dependency " + path.string() + ": " +
                            digest.detail);
    }
    if (digest.value->size > remaining) {
      return failure(125, "compiler dependency closure exceeds the bounded total content size");
    }
    total_file_bytes += digest.value->size;
    files.push_back(HostedNativeDependencyFile{path, *digest.value});
  }
  HostedNativeDependencySnapshot snapshot;
  snapshot.identity_sha256 = snapshot_identity(files);
  snapshot.files = std::move(files);
  HostedNativeDependencyDiscoveryResult result;
  result.snapshot = std::move(snapshot);
  result.exit_code = 0;
  return result;
}

HostedNativeDependencyDiscoveryResult discover_dependency_snapshot_pass(
  const ResolvedHostedToolchain &toolchain, const std::vector<HostedNativeDependencyUnit> &units,
  const fs::path &scratch_directory, const HostedNativeDependencyLimits &limits,
  std::uint32_t timeout_milliseconds, unsigned pass) {
  if (units.size() > limits.max_translation_units) {
    return failure(125, "hosted dependency discovery exceeds the translation-unit limit");
  }
  if (timeout_milliseconds == 0U) {
    return failure(125, "hosted dependency discovery requires a bounded timeout");
  }
  if (units.empty())
    return snapshot_paths({}, limits);

  std::string scratch_detail;
  const std::optional<fs::path> canonical_scratch =
    validate_scratch_directory(scratch_directory, scratch_detail);
  if (!canonical_scratch.has_value())
    return failure(125, std::move(scratch_detail));

  for (const HostedNativeDependencyUnit &unit : units) {
    if (unit.source.empty() || unit.source.string().find('\0') != std::string::npos) {
      return failure(125, "dependency discovery source path is empty or contains NUL");
    }
  }

  const DependencyPathPass paths = discover_dependency_paths(toolchain, units, *canonical_scratch,
                                                             pass, limits, timeout_milliseconds);
  if (!paths.ok())
    return failure(paths.exit_code, paths.detail);
  return snapshot_paths(paths.paths, limits);
}

std::string cleanup_depfiles(const std::vector<HostedNativeDependencyDepfile> &depfiles) {
  for (const HostedNativeDependencyDepfile &depfile : depfiles) {
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(depfile.path, status_error);
    const bool missing = status_error == std::errc::no_such_file_or_directory ||
                         (!status_error && status.type() == fs::file_type::not_found);
    if (missing)
      continue;
    if (status_error) {
      return "could not inspect compiled dependency file during cleanup: " + status_error.message();
    }
    std::error_code remove_error;
    if (!fs::remove(depfile.path, remove_error) || remove_error) {
      return remove_error ? "could not remove compiled dependency file: " + remove_error.message()
                          : "compiled dependency file disappeared before cleanup";
    }
  }
  return {};
}

} // namespace

namespace detail {

MakeDependencyParseResult parse_make_dependency_rule(std::string_view depfile,
                                                     std::string_view expected_target,
                                                     std::size_t max_dependencies,
                                                     std::size_t max_encoded_path_bytes) {
  MakeDependencyParseResult result;
  if (depfile.find('\0') != std::string_view::npos) {
    result.detail = "dependency file contains a NUL byte";
    return result;
  }
  if (expected_target.empty() ||
      expected_target.find_first_of("\\:# \t\r\n") != std::string_view::npos) {
    result.detail = "expected dependency target is not a simple make token";
    return result;
  }

  std::string logical;
  logical.reserve(depfile.size());
  for (std::size_t index = 0U; index < depfile.size();) {
    if (depfile[index] == '\\' && index + 1U < depfile.size() && depfile[index + 1U] == '\n') {
      index += 2U;
      continue;
    }
    if (depfile[index] == '\\' && index + 2U < depfile.size() && depfile[index + 1U] == '\r' &&
        depfile[index + 2U] == '\n') {
      index += 3U;
      continue;
    }
    logical.push_back(depfile[index]);
    ++index;
  }

  const std::size_t first_newline = logical.find_first_of("\r\n");
  std::string_view rule = logical;
  if (first_newline != std::string::npos) {
    rule = std::string_view(logical).substr(0U, first_newline);
    const std::string_view trailing = std::string_view(logical).substr(first_newline);
    if (trailing.find_first_not_of(" \t\r\n") != std::string_view::npos) {
      result.detail = "dependency file contains more than one make rule";
      return result;
    }
  }

  bool escaped = false;
  std::size_t separator = std::string_view::npos;
  for (std::size_t index = 0U; index < rule.size(); ++index) {
    const char byte = rule[index];
    if (escaped) {
      escaped = false;
    } else if (byte == '\\') {
      escaped = true;
    } else if (byte == ':') {
      separator = index;
      break;
    }
  }
  if (escaped || separator == std::string_view::npos) {
    result.detail = escaped ? "dependency rule ends in an incomplete escape"
                            : "dependency rule has no target separator";
    return result;
  }
  if (rule.substr(0U, separator) != expected_target) {
    result.detail = "dependency rule target does not match the requested target";
    return result;
  }

  std::string token;
  std::size_t encoded_path_bytes = 0U;
  const auto finish_token = [&]() -> bool {
    if (token.empty())
      return true;
    if (result.dependencies.size() >= max_dependencies ||
        encoded_path_bytes > max_encoded_path_bytes ||
        token.size() > max_encoded_path_bytes - encoded_path_bytes) {
      result.detail = "dependency rule exceeds the bounded path count or size";
      return false;
    }
    encoded_path_bytes += token.size();
    result.dependencies.push_back(std::move(token));
    token.clear();
    return true;
  };

  const std::string_view dependencies = rule.substr(separator + 1U);
  for (std::size_t index = 0U; index < dependencies.size(); ++index) {
    const char byte = dependencies[index];
    if (byte == ' ' || byte == '\t') {
      if (!finish_token())
        return result;
      continue;
    }
    if (byte == '#')
      break;
    if (byte == '\\') {
      if (index + 1U >= dependencies.size()) {
        result.detail = "dependency path ends in an incomplete escape";
        return result;
      }
      token.push_back(dependencies[++index]);
      continue;
    }
    if (byte == '$' && index + 1U < dependencies.size() && dependencies[index + 1U] == '$') {
      token.push_back('$');
      ++index;
      continue;
    }
    token.push_back(byte);
  }
  if (!finish_token())
    return result;
  if (result.dependencies.empty()) {
    result.detail = "dependency rule contains no input paths";
  }
  return result;
}

} // namespace detail

HostedNativeDependencyDiscoveryResult discover_hosted_native_dependencies(
  const ResolvedHostedToolchain &toolchain, const std::vector<HostedNativeDependencyUnit> &units,
  const fs::path &scratch_directory, const HostedNativeDependencyLimits &limits,
  std::uint32_t timeout_milliseconds) {
  HostedNativeDependencyDiscoveryResult first = discover_dependency_snapshot_pass(
    toolchain, units, scratch_directory, limits, timeout_milliseconds, 0U);
  if (!first.ok())
    return first;

  HostedNativeDependencyDiscoveryResult second = discover_dependency_snapshot_pass(
    toolchain, units, scratch_directory, limits, timeout_milliseconds, 1U);
  if (!second.ok())
    return second;

  if (*first.snapshot != *second.snapshot) {
    return failure(125, "compiler dependency closure changed during stable discovery");
  }
  return second;
}

HostedNativeDependencyDiscoveryResult discover_hosted_native_dependencies_once(
  const ResolvedHostedToolchain &toolchain, const std::vector<HostedNativeDependencyUnit> &units,
  const fs::path &scratch_directory, const HostedNativeDependencyLimits &limits,
  std::uint32_t timeout_milliseconds) {
  return discover_dependency_snapshot_pass(toolchain, units, scratch_directory, limits,
                                           timeout_milliseconds, 0U);
}

HostedNativeDependencyDiscoveryResult collect_compiled_hosted_native_dependencies(
  const std::vector<HostedNativeDependencyDepfile> &depfiles, const fs::path &working_directory,
  const fs::path &scratch_directory, const HostedNativeDependencyLimits &limits) {
  if (depfiles.empty())
    return snapshot_paths({}, limits);
  if (depfiles.size() > limits.max_translation_units) {
    return failure(125, "compiled dependency files exceed the translation-unit limit");
  }
  std::string scratch_detail;
  const std::optional<fs::path> canonical_scratch =
    validate_scratch_directory(scratch_directory, scratch_detail);
  if (!canonical_scratch.has_value())
    return failure(125, std::move(scratch_detail));

  std::error_code working_directory_error;
  const fs::path canonical_working_directory =
    fs::canonical(working_directory, working_directory_error);
  if (working_directory_error || !fs::is_directory(canonical_working_directory)) {
    return failure(125, working_directory_error
                          ? "could not canonicalize dependency working directory: " +
                              working_directory_error.message()
                          : "dependency working directory is not a directory");
  }

  std::vector<HostedNativeDependencyDepfile> validated_depfiles;
  validated_depfiles.reserve(depfiles.size());
  std::vector<fs::path> normalized_paths;
  normalized_paths.reserve(depfiles.size());
  for (const HostedNativeDependencyDepfile &depfile : depfiles) {
    std::error_code absolute_error;
    const fs::path absolute = fs::absolute(depfile.path, absolute_error).lexically_normal();
    std::error_code parent_error;
    const fs::path canonical_parent =
      absolute_error ? fs::path{} : fs::canonical(absolute.parent_path(), parent_error);
    if (absolute_error || parent_error || canonical_parent != *canonical_scratch ||
        absolute.filename().empty()) {
      const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
      std::string detail;
      if (absolute_error) {
        detail = "could not normalize compiled dependency file: " + absolute_error.message();
      } else if (parent_error) {
        detail =
          "could not canonicalize compiled dependency-file parent: " + parent_error.message();
      } else {
        detail = "compiled dependency file is outside its private scratch directory";
      }
      if (!cleanup_detail.empty())
        detail += "; " + cleanup_detail;
      return failure(125, std::move(detail));
    }
    const fs::path normalized = canonical_parent / absolute.filename();
    validated_depfiles.push_back(
      {normalized, depfile.expected_target, depfile.excluded_dependency});
    normalized_paths.push_back(normalized);
  }
  std::sort(normalized_paths.begin(), normalized_paths.end());
  if (std::adjacent_find(normalized_paths.begin(), normalized_paths.end()) !=
      normalized_paths.end()) {
    const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
    return failure(125, "compiled dependency file list contains a duplicate" +
                          (cleanup_detail.empty() ? std::string{} : "; " + cleanup_detail));
  }

  std::vector<fs::path> dependency_paths;
  std::vector<std::optional<fs::path>> excluded_dependencies;
  excluded_dependencies.reserve(validated_depfiles.size());
  for (const HostedNativeDependencyDepfile &depfile : validated_depfiles) {
    if (!depfile.excluded_dependency.has_value()) {
      excluded_dependencies.push_back(std::nullopt);
      continue;
    }
    std::string excluded_detail;
    std::optional<fs::path> excluded = canonical_regular_dependency(
      *depfile.excluded_dependency, canonical_working_directory, excluded_detail);
    if (!excluded.has_value()) {
      const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
      if (!cleanup_detail.empty())
        excluded_detail += "; " + cleanup_detail;
      return failure(125, "could not bind excluded compiled dependency source: " + excluded_detail);
    }
    excluded_dependencies.push_back(std::move(excluded));
  }
  std::size_t encoded_path_bytes = 0U;
  std::uintmax_t total_depfile_bytes = 0U;
  for (std::size_t index = 0U; index < validated_depfiles.size(); ++index) {
    const DepfileReadResult read =
      read_stable_depfile(validated_depfiles[index].path, limits.max_depfile_bytes);
    if (!read.ok()) {
      const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
      return failure(125, read.detail +
                            (cleanup_detail.empty() ? std::string{} : "; " + cleanup_detail));
    }
    if (total_depfile_bytes > limits.max_total_depfile_bytes ||
        read.size > limits.max_total_depfile_bytes - total_depfile_bytes) {
      const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
      return failure(125, "compiled dependency files exceed the bounded aggregate size" +
                            (cleanup_detail.empty() ? std::string{} : "; " + cleanup_detail));
    }
    total_depfile_bytes += read.size;
    const detail::MakeDependencyParseResult parsed =
      detail::parse_make_dependency_rule(read.text, validated_depfiles[index].expected_target,
                                         limits.max_dependencies, limits.max_encoded_path_bytes);
    if (!parsed.ok()) {
      const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
      return failure(125, "could not parse compiled dependency file: " + parsed.detail +
                            (cleanup_detail.empty() ? std::string{} : "; " + cleanup_detail));
    }
    for (const std::string &dependency : parsed.dependencies) {
      std::string canonical_detail;
      const std::optional<fs::path> canonical = canonical_regular_dependency(
        fs::path(dependency), canonical_working_directory, canonical_detail);
      if (!canonical.has_value()) {
        const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
        if (!cleanup_detail.empty())
          canonical_detail += "; " + cleanup_detail;
        return failure(125, std::move(canonical_detail));
      }
      if (excluded_dependencies[index].has_value() && *canonical == *excluded_dependencies[index])
        continue;
      const std::string encoded = canonical->generic_string();
      if (dependency_paths.size() >= limits.max_dependencies ||
          encoded_path_bytes > limits.max_encoded_path_bytes ||
          encoded.size() > limits.max_encoded_path_bytes - encoded_path_bytes) {
        const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
        return failure(125, "compiled dependency closure exceeds the bounded path count or size" +
                              (cleanup_detail.empty() ? std::string{} : "; " + cleanup_detail));
      }
      encoded_path_bytes += encoded.size();
      dependency_paths.push_back(*canonical);
    }
  }

  const std::string cleanup_detail = cleanup_depfiles(validated_depfiles);
  if (!cleanup_detail.empty())
    return failure(125, cleanup_detail);
  std::sort(dependency_paths.begin(), dependency_paths.end(),
            [](const fs::path &left, const fs::path &right) {
              return left.generic_string() < right.generic_string();
            });
  dependency_paths.erase(std::unique(dependency_paths.begin(), dependency_paths.end()),
                         dependency_paths.end());
  return snapshot_paths(dependency_paths, limits);
}

} // namespace nebula::cli
