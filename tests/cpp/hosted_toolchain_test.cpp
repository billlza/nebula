#include "cli/hosted_toolchain.hpp"
#include "cli/tool_lookup.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class ReadableStdinScope final {
public:
  ReadableStdinScope() {
#if defined(_WIN32)
    target_ = ::_fileno(stdin);
    saved_ = ::_dup(target_);
    int pipe_descriptors[2] = {-1, -1};
    if (saved_ < 0 || ::_pipe(pipe_descriptors, 256, _O_BINARY) != 0) {
      if (saved_ >= 0)
        (void)::_close(saved_);
      throw std::runtime_error("could not create readable stdin fixture");
    }
    const int written = ::_write(pipe_descriptors[1], "x", 1U);
    const int duplicated = written == 1 ? ::_dup2(pipe_descriptors[0], target_) : -1;
    (void)::_close(pipe_descriptors[0]);
    (void)::_close(pipe_descriptors[1]);
#else
    target_ = STDIN_FILENO;
    saved_ = ::dup(target_);
    int pipe_descriptors[2] = {-1, -1};
    if (saved_ < 0 || ::pipe(pipe_descriptors) != 0) {
      if (saved_ >= 0)
        (void)::close(saved_);
      throw std::runtime_error("could not create readable stdin fixture");
    }
    const ssize_t written = ::write(pipe_descriptors[1], "x", 1U);
    const int duplicated = written == 1 ? ::dup2(pipe_descriptors[0], target_) : -1;
    (void)::close(pipe_descriptors[0]);
    (void)::close(pipe_descriptors[1]);
#endif
    if (duplicated < 0) {
      (void)restore();
      throw std::runtime_error("could not install readable stdin fixture");
    }
  }

  ReadableStdinScope(const ReadableStdinScope &) = delete;
  ReadableStdinScope &operator=(const ReadableStdinScope &) = delete;

  ~ReadableStdinScope() { (void)restore(); }

  bool restore() noexcept {
    if (saved_ < 0)
      return true;
#if defined(_WIN32)
    const bool restored = ::_dup2(saved_, target_) == 0;
    const bool closed = ::_close(saved_) == 0;
#else
    const bool restored = ::dup2(saved_, target_) >= 0;
    const bool closed = ::close(saved_) == 0;
#endif
    saved_ = -1;
    return restored && closed;
  }

private:
  int target_ = -1;
  int saved_ = -1;
};

bool set_environment(const char *name, const std::optional<std::string> &value) {
#if defined(_WIN32)
  return ::_putenv_s(name, value.has_value() ? value->c_str() : "") == 0;
#else
  return value.has_value() ? ::setenv(name, value->c_str(), 1) == 0 : ::unsetenv(name) == 0;
#endif
}

std::optional<std::string> environment_value(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr)
    return std::nullopt;
  return std::string(value);
}

void expect(bool condition, std::string_view message, int &failures) {
  if (condition)
    return;
  std::cerr << "hosted_toolchain_test: " << message << '\n';
  ++failures;
}

int fake_tool_main(int argc, char **argv) {
  if (argc <= 0 || argv == nullptr || argv[0] == nullptr)
    return 85;
  const std::filesystem::path self_path{std::string(argv[0])};
  std::ofstream query_marker(self_path.string() + ".query-ran", std::ios::binary | std::ios::app);
  query_marker.put('q');
  query_marker.close();
  if (query_marker.fail())
    return 84;
  char stdin_byte = '\0';
  if (std::cin.get(stdin_byte))
    return 90;
  if (!std::cin.eof())
    return 91;
  if (argc == 4 && std::string_view(argv[1]) == "t" && std::string_view(argv[3]) == "probe.o") {
    std::cout << (self_path.filename().string().starts_with("missing-member-archiver")
                    ? "other.o\n"
                    : "probe.o\n");
    return 0;
  }
  if (argc == 4 && std::string_view(argv[1]) == "rcs") {
    if (self_path.filename().string().starts_with("broken-archiver"))
      return 86;
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path(argv[3]), error) || error)
      return 87;
    std::ofstream archive(std::filesystem::path(argv[2]), std::ios::binary | std::ios::trunc);
    archive << (self_path.filename().string().starts_with("bad-magic-archiver") ? "not-archive"
                                                                                : "!<arch>\nprobe");
    archive.close();
    if (self_path.filename().string().starts_with("warning-archiver"))
      std::cerr << "unexpected archiver warning\n";
    return archive.fail() ? 88 : 0;
  }
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) != "-o")
      continue;
    const bool compile_only = std::any_of(argv + 1, argv + argc, [](const char *argument) {
      return std::string_view(argument) == "-c";
    });
    if (!compile_only)
      break;
    std::ofstream object(std::filesystem::path(argv[index + 1]),
                         std::ios::binary | std::ios::trunc);
    if (!self_path.filename().string().starts_with("empty-object-compiler"))
      object << "fake-object";
    object.close();
    return object.fail() ? 89 : 0;
  }
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--version") {
      if (self_path.filename().string().starts_with("fake-archiver"))
        return 92;
      std::cout << "fake clang version 1.0\n";
      return 0;
    }
    if (argument == "-dumpmachine") {
      std::cout << "x86_64-fake-nebula\n";
      return 0;
    }
    if (argument == "-print-prog-name=ld") {
      const std::optional<std::filesystem::path> linker = find_executable_on_path("ld");
      if (!linker.has_value())
        return 81;
      std::cout << linker->string() << '\n';
      return 0;
    }
    if (argument == "--verify-epoch") {
      const char *epoch = std::getenv("SOURCE_DATE_EPOCH");
      return epoch != nullptr && std::string_view(epoch) == "1" ? 0 : 83;
    }
    if (argument == "-fsyntax-only")
      return 0;
  }
  return 82;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1)
    return fake_tool_main(argc, argv);

  int failures = 0;
  const std::optional<std::filesystem::path> linker = find_executable_on_path("ld");
  expect(linker.has_value(), "test linker must be available", failures);
  if (!linker.has_value())
    return 1;

  const std::filesystem::path self = std::filesystem::canonical(std::filesystem::absolute(argv[0]));
  const std::string suffix =
    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::filesystem::path temporary =
    std::filesystem::temp_directory_path() / ("nebula-hosted-toolchain-" + suffix);
  std::error_code error;
  std::filesystem::create_directory(temporary, error);
  expect(!error, "create private toolchain test directory", failures);
  if (error)
    return 1;
  const std::filesystem::path fake_compiler = temporary / self.filename();
  std::filesystem::copy_file(self, fake_compiler, std::filesystem::copy_options::overwrite_existing,
                             error);
  expect(!error, "copy fake compiler executable", failures);
  std::filesystem::permissions(fake_compiler, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add, error);
  expect(!error, "make fake compiler executable", failures);
  const std::filesystem::path fake_archiver = temporary / "fake-archiver";
  std::filesystem::copy_file(self, fake_archiver, std::filesystem::copy_options::overwrite_existing,
                             error);
  expect(!error, "copy fake archiver executable", failures);
  std::filesystem::permissions(fake_archiver, std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::add, error);
  expect(!error, "make fake archiver executable", failures);

  const std::optional<std::string> prior_epoch = environment_value("SOURCE_DATE_EPOCH");
  expect(set_environment("SOURCE_DATE_EPOCH", std::string("1")),
         "set deterministic compiler environment marker", failures);

  nebula::cli::HostedToolchainRequest request;
  request.self_executable = self;
  request.compiler_command = fake_compiler.string();
  request.require_archiver = true;
  request.archiver_command = fake_archiver.string();
  const std::filesystem::path compiler_query_marker = fake_compiler.string() + ".query-ran";
  const std::filesystem::path archiver_query_marker = fake_archiver.string() + ".query-ran";
  const nebula::cli::HostedToolPathResolutionResult path_resolution =
    nebula::cli::resolve_hosted_tool_paths(request);
  expect(path_resolution.ok(),
         path_resolution.detail.empty() ? "preview fake hosted tool paths" : path_resolution.detail,
         failures);
  expect(!std::filesystem::exists(compiler_query_marker) &&
           !std::filesystem::exists(archiver_query_marker),
         "tool-path preview must not execute the compiler or archiver", failures);

  nebula::cli::HostedToolchainRequest invalid_command_request = request;
  invalid_command_request.compiler_command = "clang++ -fno-rtti";
  const nebula::cli::HostedToolPathResolutionResult invalid_command_paths =
    nebula::cli::resolve_hosted_tool_paths(invalid_command_request);
  expect(!invalid_command_paths.ok() &&
           invalid_command_paths.failure ==
             nebula::cli::HostedToolPathFailureKind::InvalidCompilerCommand,
         "invalid compiler command must retain its typed failure", failures);

  nebula::cli::HostedToolchainRequest missing_archiver_request = request;
  missing_archiver_request.archiver_command = (temporary / "missing-archiver").string();
  const nebula::cli::HostedToolPathResolutionResult missing_archiver_paths =
    nebula::cli::resolve_hosted_tool_paths(missing_archiver_request);
  expect(!missing_archiver_paths.ok() &&
           missing_archiver_paths.failure ==
             nebula::cli::HostedToolPathFailureKind::ArchiverUnavailable,
         "missing archiver must retain its typed failure", failures);

  if (path_resolution.ok()) {
    expect(path_resolution.value->compiler == std::filesystem::canonical(fake_compiler) &&
             path_resolution.value->archiver ==
               std::optional<std::filesystem::path>(std::filesystem::canonical(fake_archiver)) &&
             path_resolution.value->nebula_executable == self,
           "tool-path preview must return all canonical protected inputs", failures);
    nebula::cli::ResolvedHostedToolPaths mismatched_paths = *path_resolution.value;
    mismatched_paths.compiler = self;
    const nebula::cli::HostedToolchainResolutionResult mismatch =
      nebula::cli::resolve_hosted_toolchain(request, mismatched_paths);
    expect(!mismatch.ok() &&
             mismatch.detail.find("changed after output-conflict preflight") != std::string::npos,
           "full resolver must reject a mismatched path preview", failures);
    expect(!std::filesystem::exists(compiler_query_marker) &&
             !std::filesystem::exists(archiver_query_marker),
           "path-preview mismatch must fail before compiler queries", failures);
  }
  ReadableStdinScope readable_stdin;
  const nebula::cli::HostedToolchainResolutionResult resolution =
    path_resolution.ok() ? nebula::cli::resolve_hosted_toolchain(request, *path_resolution.value)
                         : nebula::cli::resolve_hosted_toolchain(request);
  expect(resolution.ok(),
         resolution.detail.empty() ? "resolve fake hosted toolchain" : resolution.detail, failures);
  expect(std::filesystem::exists(compiler_query_marker) &&
           std::filesystem::exists(archiver_query_marker),
         "full toolchain resolution must perform bounded identity queries", failures);

  if (resolution.ok()) {
    const nebula::cli::ResolvedHostedToolchain &toolchain = *resolution.value;
    expect(toolchain.compiler().executable == std::filesystem::canonical(fake_compiler),
           "compiler path must be canonical and absolute", failures);
    expect(toolchain.compiler().sha256.size() == 64U, "compiler binary SHA-256", failures);
    expect(toolchain.nebula_executable().sha256.size() == 64U, "Nebula executable SHA-256",
           failures);
    expect(toolchain.target_triple() == "x86_64-fake-nebula", "bounded target triple query",
           failures);
    expect(toolchain.cxx_standard_flag() == "-std=c++23", "verified preferred C++23 dialect",
           failures);
    expect(toolchain.archiver().has_value() && toolchain.archiver()->sha256.size() == 64U,
           "optional archiver identity", failures);
    expect(toolchain.archiver().has_value() &&
             toolchain.archiver()->version == "nebula-ar-rcs-portable-archive-v1",
           "archiver identity must record the verified capability contract", failures);
    expect(toolchain.compiler_dependencies().size() == 1U &&
             toolchain.compiler_dependencies().front().role == "ld",
           "Clang linker dependency identity", failures);
    const std::string provenance = toolchain.provenance_identity();
    expect(provenance.starts_with("resolved-hosted-toolchain-v3\n") &&
             provenance.find("archiver_identity_evidence=") != std::string::npos &&
             provenance.find("nebula_executable_sha256=") != std::string::npos &&
             provenance.find("working_directory_size=") != std::string::npos &&
             provenance.find("compiler_dependency_sha256=") != std::string::npos,
           "provenance must bind self, cwd, and compiler child tools", failures);
    std::string detail;
    expect(toolchain.revalidate(detail), "unchanged toolchain revalidation", failures);

    const std::filesystem::path original_working_directory = std::filesystem::current_path();
    const std::filesystem::path alternate_working_directory = temporary / "cwd";
    std::filesystem::create_directory(alternate_working_directory, error);
    expect(!error, "create alternate working directory", failures);
    std::filesystem::current_path(alternate_working_directory, error);
    expect(!error, "enter alternate working directory", failures);
    detail.clear();
    expect(!toolchain.revalidate(detail) && detail.find("working directory") != std::string::npos,
           "working-directory changes must invalidate toolchain identity", failures);
    std::filesystem::current_path(original_working_directory, error);
    expect(!error, "restore working directory", failures);

    expect(set_environment("SOURCE_DATE_EPOCH", std::string("2")),
           "mutate compiler environment marker", failures);
    detail.clear();
    expect(toolchain.revalidate(detail),
           "external environment changes cannot mutate the immutable snapshot", failures);
    const nebula::cli::HostProcessResult snapshot_execution =
      toolchain.execute({toolchain.compiler().executable.string(), "--verify-epoch"}, 1000U);
    expect(snapshot_execution.succeeded(),
           "compiler execution must receive the resolved environment snapshot", failures);
    expect(set_environment("SOURCE_DATE_EPOCH", std::string("1")),
           "restore compiler environment marker", failures);

    nebula::cli::HostedToolchainRequest invalid_standard = request;
    invalid_standard.cxx_standard_override = std::string("-std=c++23 -fno-rtti");
    const nebula::cli::HostedToolchainResolutionResult invalid_resolution =
      nebula::cli::resolve_hosted_toolchain(invalid_standard);
    expect(!invalid_resolution.ok() &&
             invalid_resolution.detail.find("one NUL-free") != std::string::npos,
           "multi-argument standard override must fail explicitly", failures);

    const std::filesystem::path broken_archiver = temporary / "broken-archiver";
    std::filesystem::copy_file(self, broken_archiver,
                               std::filesystem::copy_options::overwrite_existing, error);
    expect(!error, "copy failing archiver fixture", failures);
    std::filesystem::permissions(broken_archiver, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add, error);
    expect(!error, "make failing archiver fixture executable", failures);
    nebula::cli::HostedToolchainRequest broken_archiver_request = request;
    broken_archiver_request.archiver_command = broken_archiver.string();
    const nebula::cli::HostedToolchainResolutionResult broken_archiver_resolution =
      nebula::cli::resolve_hosted_toolchain(broken_archiver_request);
    expect(!broken_archiver_resolution.ok() &&
             broken_archiver_resolution.detail.find(
               "host archiver capability probe failed with exit status 86") != std::string::npos,
           "archiver capability failure must remain explicit", failures);

    const auto verify_rejected_archiver = [&](std::string_view filename,
                                              std::string_view expected_detail) {
      const std::filesystem::path fixture = temporary / filename;
      std::filesystem::copy_file(self, fixture, std::filesystem::copy_options::overwrite_existing,
                                 error);
      expect(!error, "copy rejected archiver fixture", failures);
      std::filesystem::permissions(fixture, std::filesystem::perms::owner_exec,
                                   std::filesystem::perm_options::add, error);
      expect(!error, "make rejected archiver fixture executable", failures);
      nebula::cli::HostedToolchainRequest rejected_request = request;
      rejected_request.archiver_command = fixture.string();
      const nebula::cli::HostedToolchainResolutionResult rejected =
        nebula::cli::resolve_hosted_toolchain(rejected_request);
      expect(!rejected.ok() && rejected.detail.find(expected_detail) != std::string::npos,
             "invalid archiver capability output must fail closed", failures);
    };
    verify_rejected_archiver("bad-magic-archiver", "invalid archive header");
    verify_rejected_archiver("warning-archiver", "produced unexpected output");
    verify_rejected_archiver("missing-member-archiver", "did not retain exactly");

    const std::filesystem::path empty_object_compiler = temporary / "empty-object-compiler";
    std::filesystem::copy_file(self, empty_object_compiler,
                               std::filesystem::copy_options::overwrite_existing, error);
    expect(!error, "copy empty-object compiler fixture", failures);
    std::filesystem::permissions(empty_object_compiler, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add, error);
    expect(!error, "make empty-object compiler fixture executable", failures);
    nebula::cli::HostedToolchainRequest empty_object_request = request;
    empty_object_request.compiler_command = empty_object_compiler.string();
    const nebula::cli::HostedToolchainResolutionResult empty_object_resolution =
      nebula::cli::resolve_hosted_toolchain(empty_object_request);
    expect(!empty_object_resolution.ok() && empty_object_resolution.detail.find(
                                              "produced an empty object file") != std::string::npos,
           "empty compiler probe output must fail closed", failures);

#if !defined(_WIN32)
    const std::filesystem::path unsafe_directory = temporary / "unsafe-tool-parent";
    std::filesystem::create_directory(unsafe_directory, error);
    expect(!error && ::chmod(unsafe_directory.c_str(), 0777) == 0,
           "create non-sticky shared-writable tool parent", failures);
    const std::filesystem::path unsafe_compiler = unsafe_directory / self.filename();
    std::filesystem::copy_file(self, unsafe_compiler, std::filesystem::copy_options::none, error);
    expect(!error && ::chmod(unsafe_compiler.c_str(), 0755) == 0, "create unsafe compiler fixture",
           failures);
    nebula::cli::HostedToolchainRequest unsafe_request = request;
    unsafe_request.compiler_command = unsafe_compiler.string();
    const nebula::cli::HostedToolPathResolutionResult unsafe_paths =
      nebula::cli::resolve_hosted_tool_paths(unsafe_request);
    expect(!unsafe_paths.ok() &&
             unsafe_paths.detail.find("owner-controlled executable boundary") != std::string::npos,
           "compiler in a non-sticky shared-writable parent must be rejected", failures);
    expect(::chmod(unsafe_directory.c_str(), 0700) == 0,
           "restore unsafe compiler fixture permissions", failures);
#endif

    std::ofstream mutation(fake_compiler, std::ios::binary | std::ios::app);
    mutation.put('\0');
    mutation.close();
    expect(!mutation.fail(), "mutate fake compiler after resolution", failures);
    detail.clear();
    expect(!toolchain.revalidate(detail) && detail.find("compiler") != std::string::npos,
           "compiler replacement must fail revalidation", failures);
  }

  expect(readable_stdin.restore(), "restore readable stdin fixture", failures);

  expect(set_environment("SOURCE_DATE_EPOCH", prior_epoch), "restore compiler environment marker",
         failures);
  std::filesystem::remove_all(temporary, error);
  expect(!error, "remove toolchain test directory", failures);

  if (failures != 0)
    return 1;
  std::cout << "hosted-toolchain-tests-ok\n";
  return 0;
}
