#pragma once

#include "cli_shared.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class ManifestDependencyKind : std::uint8_t { Version, Path, Git, Installed };

struct ManifestDependency {
  std::string alias;
  ManifestDependencyKind kind = ManifestDependencyKind::Version;
  std::string version;
  fs::path path;
  std::string git;
  std::string rev;
  std::string installed;
};

struct PackageManifest {
  int schema_version = 1;
  bool legacy_flat = false;
  bool legacy_project_table = false;
  bool has_explicit_package = false;
  bool has_workspace_section = false;
  std::string name;
  std::string version = "0.1.0";
  fs::path entry = "src/main.nb";
  fs::path src_dir = "src";
  std::vector<fs::path> host_cxx;
  NativeBuildConfig native;
  std::vector<fs::path> workspace_members;
  std::vector<ManifestDependency> dependencies;
};

struct LockedPackageDependency {
  std::string alias;
  std::string package;
};

struct LockedPackage {
  bool is_root = false;
  std::string name;
  std::string version;
  fs::path root;
  fs::path manifest_path;
  fs::path entry;
  fs::path src_dir;
  std::string source_kind;
  std::string source_hint;
  std::string git_url;
  std::string git_requested_rev;
  std::string resolved_rev;
  std::string manifest_hash;
  std::string source_fingerprint;
  std::string integrity_fingerprint;
  std::vector<fs::path> host_cxx_sources;
  NativeBuildConfig native;
  std::vector<LockedPackageDependency> dependencies;
};

struct ProjectLock {
  int schema_version = 2;
  std::string root_package;
  std::string root_manifest_hash;
  int toolchain_schema_version = 1;
  int cache_schema_version = 2;
  std::vector<LockedPackage> packages;
};

struct PublishPackageResult {
  fs::path registry_root;
  fs::path package_root;
  fs::path manifest_path;
  fs::path published_root;
  std::string package_name;
  std::string package_version;
  bool unchanged = false;
  bool replaced = false;
};

struct LoadedCompileInput {
  bool ok = false;
  fs::path manifest_path;
  fs::path project_root;
  fs::path module_root;
  fs::path entry_file;
  std::string project_name;
  std::string cache_key_source;
  std::vector<nebula::frontend::SourceFile> compile_sources;
  std::vector<fs::path> source_files;
  std::vector<fs::path> host_cxx_sources;
  NativeBuildInputs native_inputs;
  std::vector<nebula::frontend::Diagnostic> diags;
};

struct LoadCompileOptions {
  fs::path std_root;
  fs::path backend_sdk_root;
  bool no_std = false;
  RuntimeProfile runtime_profile = RuntimeProfile::Hosted;
  std::string target = "host";
};

inline LoadCompileOptions load_compile_options_from_cli(const CliOptions& opt) {
  LoadCompileOptions out;
  out.std_root = opt.std_root;
  out.backend_sdk_root = opt.backend_sdk_root;
  out.no_std = opt.no_std;
  out.runtime_profile = opt.runtime_profile;
  out.target = opt.target;
  return out;
}

bool read_package_manifest(const fs::path& path,
                           PackageManifest& out,
                           std::vector<nebula::frontend::Diagnostic>& diags,
                           nebula::frontend::DiagnosticStage stage);
fs::path lockfile_path_for_manifest(const fs::path& manifest_path);
bool read_project_lock(const fs::path& path,
                       ProjectLock& out,
                       std::vector<nebula::frontend::Diagnostic>& diags,
                       nebula::frontend::DiagnosticStage stage);
bool write_project_lock(const fs::path& path,
                        const ProjectLock& lock,
                        std::vector<nebula::frontend::Diagnostic>& diags,
                        nebula::frontend::DiagnosticStage stage);
bool resolve_project_lock(const fs::path& input,
                          ProjectLock& out,
                          std::vector<nebula::frontend::Diagnostic>& diags,
                          nebula::frontend::DiagnosticStage stage,
                          const fs::path& backend_sdk_root = {},
                          bool refresh_git_sources = false);
bool publish_package_to_local_registry(const fs::path& input,
                                       PublishPackageResult& out,
                                       std::vector<nebula::frontend::Diagnostic>& diags,
                                       nebula::frontend::DiagnosticStage stage,
                                       bool force = false);
bool preflight_git_dependency_environment(const ManifestDependency& dep,
                                          std::vector<nebula::frontend::Diagnostic>& diags,
                                          nebula::frontend::DiagnosticStage stage,
                                          bool require_remote_probe,
                                          std::string_view command_hint = "fetch/update");
LoadedCompileInput load_compile_input(const fs::path& input,
                                      nebula::frontend::DiagnosticStage stage,
                                      const fs::path& std_root,
                                      const fs::path& backend_sdk_root = {});
LoadedCompileInput load_compile_input(const fs::path& input,
                                      nebula::frontend::DiagnosticStage stage,
                                      const LoadCompileOptions& options);
