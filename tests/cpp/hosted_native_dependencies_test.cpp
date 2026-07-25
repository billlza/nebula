#include "cli/hosted_native_dependencies.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

void expect(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write_file(const fs::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  expect(static_cast<bool>(output), "could not create test file: " + path.string());
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  expect(!output.fail(), "could not finish test file: " + path.string());
}

fs::path require_canonical(const fs::path &path) {
  std::error_code error;
  fs::path result = fs::canonical(path, error);
  expect(!error, "could not canonicalize test path: " + error.message());
  return result;
}

void parser_contract() {
  const std::string depfile =
    "nebula_dependency_target_0_7: /tmp/source.cpp path\\ with\\ spaces.hpp \\\n"
    " hash\\#name.inc dollar$$name C:/sdk/header.h\n";
  const auto parsed = nebula::cli::detail::parse_make_dependency_rule(
    depfile, "nebula_dependency_target_0_7", 16U, 4096U);
  expect(parsed.ok(), "valid dependency rule was rejected: " + parsed.detail);
  const std::vector<std::string> expected = {"/tmp/source.cpp", "path with spaces.hpp",
                                             "hash#name.inc", "dollar$name", "C:/sdk/header.h"};
  expect(parsed.dependencies == expected, "dependency rule escaping was decoded incorrectly");

  const auto wrong_target =
    nebula::cli::detail::parse_make_dependency_rule(depfile, "other_target", 16U, 4096U);
  expect(!wrong_target.ok(), "mismatched dependency target was accepted");
  const auto multiple_rules = nebula::cli::detail::parse_make_dependency_rule(
    "target: one\nother: two\n", "target", 16U, 4096U);
  expect(!multiple_rules.ok(), "multiple dependency rules were accepted");
  const auto bounded =
    nebula::cli::detail::parse_make_dependency_rule("target: one two\n", "target", 1U, 4096U);
  expect(!bounded.ok(), "dependency count limit was ignored");
  const std::string with_nul("target: one\0two", 15U);
  const auto nul = nebula::cli::detail::parse_make_dependency_rule(with_nul, "target", 16U, 4096U);
  expect(!nul.ok(), "NUL-containing dependency rule was accepted");
}

void discovery_contract(const fs::path &self) {
  const fs::path root =
    fs::temp_directory_path() /
    ("nebula-hosted-dependencies-test-" +
     std::to_string(static_cast<unsigned long long>(std::hash<std::string>{}(self.string()))));
  std::error_code cleanup_error;
  fs::remove_all(root, cleanup_error);
  cleanup_error.clear();
  fs::create_directories(root / "scratch", cleanup_error);
  expect(!cleanup_error, "could not create dependency test directory");
#if !defined(_WIN32)
  fs::permissions(root / "scratch", fs::perms::owner_all, fs::perm_options::replace, cleanup_error);
  expect(!cleanup_error, "could not make dependency scratch directory private");
#endif

  const fs::path source = root / "host.cpp";
  const fs::path header = root / "answer with space.hpp";
  const fs::path fragment = root / "answer_fragment";
  write_file(source, "#include <cstdint>\n"
                     "#include \"answer with space.hpp\"\n"
                     "std::int64_t answer() { return NEBULA_DEPENDENCY_ANSWER; }\n");
  write_file(header, "#pragma once\n#include \"answer_fragment\"\n");
  write_file(fragment, "#define NEBULA_DEPENDENCY_ANSWER 41\n");

  nebula::cli::HostedToolchainRequest request;
  request.self_executable = self;
  const char *configured_compiler = std::getenv("CXX");
  request.compiler_command = configured_compiler != nullptr && *configured_compiler != '\0'
                               ? configured_compiler
                               : "clang++";
  auto resolved = nebula::cli::resolve_hosted_toolchain(request);
  expect(resolved.ok(),
         "could not resolve hosted compiler for dependency test: " + resolved.detail);

  nebula::cli::HostedNativeDependencyUnit unit;
  unit.source = require_canonical(source);
  unit.compiler_arguments = {resolved.value->cxx_standard_flag(),
                             "-I" + require_canonical(root).string()};
  auto discovered =
    nebula::cli::discover_hosted_native_dependencies(*resolved.value, {unit}, root / "scratch");
  expect(discovered.ok(), "dependency discovery failed: " + discovered.detail);
  expect(discovered.snapshot->identity_sha256.size() == 64U,
         "dependency snapshot did not produce a SHA-256 identity");

  const auto contains = [&](const fs::path &wanted) {
    return std::any_of(discovered.snapshot->files.begin(), discovered.snapshot->files.end(),
                       [&](const nebula::cli::HostedNativeDependencyFile &file) {
                         return file.canonical_path == require_canonical(wanted);
                       });
  };
  expect(contains(source), "translation unit is missing from discovered dependency closure");
  expect(contains(header), "space-containing adjacent header is missing from dependency closure");
  expect(contains(fragment), "extensionless transitive include is missing from dependency closure");

  const fs::path compiled_depfile = root / "scratch" / "compiled.d";
  const fs::path compiled_object = root / "compiled.o";
  const std::string compiled_target = "nebula_compiled_dependency_target";
  const std::vector<std::string> compile_command = {
    resolved.value->compiler().executable.string(),
    resolved.value->cxx_standard_flag(),
    "-I" + require_canonical(root).string(),
    "-MD",
    "-MF",
    compiled_depfile.string(),
    "-MT",
    compiled_target,
    "-c",
    unit.source.string(),
    "-o",
    compiled_object.string(),
  };
  const nebula::cli::HostProcessResult compiled = resolved.value->execute(compile_command, 30'000U);
  std::string compile_error;
  const int compile_exit = nebula::cli::host_process_compatible_exit_code(compiled, compile_error);
  expect(compile_exit == 0 && compile_error.empty(),
         "actual dependency-producing compilation failed: " + compile_error);
  const auto compiled_dependencies = nebula::cli::collect_compiled_hosted_native_dependencies(
    {{compiled_depfile, compiled_target, std::nullopt}}, resolved.value->working_directory(),
    root / "scratch");
  expect(compiled_dependencies.ok(),
         "compiled dependency collection failed: " + compiled_dependencies.detail);
  expect(*compiled_dependencies.snapshot == *discovered.snapshot,
         "actual compilation dependencies differ from the stable pre-scan");
  fs::remove(compiled_object, cleanup_error);
  expect(!cleanup_error, "could not remove dependency-test object file");

  for (const std::string &conflicting_flag :
       {std::string("-dependency-file"), std::string("-Xclang=-dependency-file")}) {
    nebula::cli::HostedNativeDependencyUnit conflicting = unit;
    conflicting.compiler_arguments.push_back(conflicting_flag);
    const auto conflicting_result = nebula::cli::discover_hosted_native_dependencies(
      *resolved.value, {conflicting}, root / "scratch");
    expect(!conflicting_result.ok() && conflicting_result.exit_code == 125,
           "dependency-output override flag was accepted: " + conflicting_flag);
  }

  const std::string first_identity = discovered.snapshot->identity_sha256;
  write_file(fragment, "#define NEBULA_DEPENDENCY_ANSWER 42\n");
  discovered =
    nebula::cli::discover_hosted_native_dependencies(*resolved.value, {unit}, root / "scratch");
  expect(discovered.ok(), "dependency rediscovery failed: " + discovered.detail);
  expect(discovered.snapshot->identity_sha256 != first_identity,
         "same-size dependency mutation did not change the closure identity");

  nebula::cli::HostedNativeDependencyLimits limited;
  limited.max_dependencies = 1U;
  const auto rejected = nebula::cli::discover_hosted_native_dependencies(*resolved.value, {unit},
                                                                         root / "scratch", limited);
  expect(!rejected.ok() && rejected.exit_code == 125,
         "dependency capacity limit did not fail explicitly");

#if !defined(_WIN32)
  const fs::path unsafe_directory = root / "shared-writable";
  fs::create_directory(unsafe_directory, cleanup_error);
  expect(!cleanup_error, "could not create unsafe dependency-directory fixture");
  fs::permissions(unsafe_directory, fs::perms::all, fs::perm_options::replace, cleanup_error);
  expect(!cleanup_error, "could not make dependency-directory fixture shared-writable");
  const fs::path unsafe_source = unsafe_directory / "unsafe.cpp";
  write_file(unsafe_source, "int unsafe_dependency_fixture() { return 0; }\n");
  nebula::cli::HostedNativeDependencyUnit unsafe_unit;
  unsafe_unit.source = unsafe_source;
  unsafe_unit.compiler_arguments = {resolved.value->cxx_standard_flag()};
  const auto unsafe_result = nebula::cli::discover_hosted_native_dependencies(
    *resolved.value, {unsafe_unit}, root / "scratch");
  expect(!unsafe_result.ok() && unsafe_result.exit_code == 125 &&
           unsafe_result.detail.find("non-sticky and world-writable") != std::string::npos,
         "dependency discovery accepted an other-UID replaceable path spelling");
  fs::permissions(unsafe_directory, fs::perms::owner_all, fs::perm_options::replace, cleanup_error);
  expect(!cleanup_error, "could not restore dependency-directory fixture permissions");
#endif

  expect(fs::is_empty(root / "scratch"), "dependency discovery left temporary depfiles behind");
  fs::remove_all(root, cleanup_error);
  expect(!cleanup_error, "could not clean dependency test directory");
}

} // namespace

int main(int argc, char **argv) {
  try {
    parser_contract();
    expect(argc > 0 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0',
           "test executable path is unavailable");
    discovery_contract(require_canonical(argv[0]));
  } catch (const std::exception &error) {
    std::cerr << "hosted native dependency test failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
