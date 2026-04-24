#include "project.hpp"

#include "frontend/errors.hpp"
#include "frontend/lexer.hpp"
#include "frontend/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <sys/wait.h>

namespace {

using nebula::frontend::Diagnostic;
using nebula::frontend::DiagnosticRisk;
using nebula::frontend::DiagnosticStage;
using nebula::frontend::Lexer;
using nebula::frontend::Parser;
using nebula::frontend::Program;
using nebula::frontend::Severity;
using nebula::frontend::SourceFile;

struct LockedPackageWithManifest {
  LockedPackage locked;
  PackageManifest manifest;
};

constexpr int kProjectLockSchemaVersion = 2;
constexpr int kLockToolchainSchemaVersion = 1;
constexpr int kLockCacheSchemaVersion = 2;

std::string trim(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start += 1;
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end -= 1;
  return std::string(text.substr(start, end - start));
}

std::string strip_inline_comment(std::string_view text) {
  bool in_string = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (ch == '"' && (i == 0 || text[i - 1] != '\\')) in_string = !in_string;
    if (!in_string && ch == '#') return trim(text.substr(0, i));
  }
  return trim(text);
}

bool read_text_file(const fs::path& path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return true;
}

std::string normalize_path_key(const fs::path& path) {
  std::error_code ec;
  const fs::path canon = fs::weakly_canonical(path, ec);
  return (ec ? path.lexically_normal() : canon.lexically_normal()).string();
}

fs::path absolute_normalized(const fs::path& path) {
  std::error_code ec;
  const fs::path canon = fs::weakly_canonical(path, ec);
  return (ec ? fs::absolute(path) : canon).lexically_normal();
}

bool path_is_within(const fs::path& path, const fs::path& root) {
  const fs::path normalized_path = absolute_normalized(path);
  const fs::path normalized_root = absolute_normalized(root);
  std::error_code ec;
  const fs::path rel = fs::relative(normalized_path, normalized_root, ec);
  if (ec) return false;
  if (rel.empty()) return true;
  for (const auto& part : rel) {
    const std::string piece = part.string();
    if (piece == "..") return false;
  }
  return true;
}

bool manifest_is_workspace_container(const PackageManifest& manifest) {
  return !manifest.workspace_members.empty() && !manifest.has_explicit_package &&
         manifest.dependencies.empty() && manifest.host_cxx.empty() && manifest.native.empty();
}

std::vector<std::string> sorted_generic_paths(const std::vector<fs::path>& paths) {
  std::vector<std::string> out;
  out.reserve(paths.size());
  for (const auto& path : paths) out.push_back(path.generic_string());
  std::sort(out.begin(), out.end());
  return out;
}

std::string native_language_name(NativeSourceLanguage language) {
  switch (language) {
  case NativeSourceLanguage::C: return "c";
  case NativeSourceLanguage::Cxx: return "cxx";
  case NativeSourceLanguage::Asm: return "asm";
  }
  return "cxx";
}

std::optional<NativeSourceLanguage> parse_native_language(std::string_view text) {
  if (text == "c") return NativeSourceLanguage::C;
  if (text == "cxx" || text == "cpp") return NativeSourceLanguage::Cxx;
  if (text == "asm" || text == "s") return NativeSourceLanguage::Asm;
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> sorted_string_pairs(
    const std::vector<std::pair<std::string, std::string>>& values) {
  auto out = values;
  std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.first != rhs.first) return lhs.first < rhs.first;
    return lhs.second < rhs.second;
  });
  return out;
}

[[maybe_unused]] std::optional<fs::path> find_manifest_upwards(fs::path start_dir) {
  std::error_code ec;
  fs::path cur = fs::weakly_canonical(start_dir, ec);
  if (ec) cur = start_dir.lexically_normal();
  while (!cur.empty()) {
    const fs::path candidate = cur / "nebula.toml";
    if (fs::exists(candidate)) return candidate;
    if (cur == cur.root_path()) break;
    cur = cur.parent_path();
  }
  return std::nullopt;
}

std::optional<std::string> parse_manifest_string(std::string_view line, std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  if (line.substr(0, prefix.size()) != prefix) return std::nullopt;
  std::string value = trim(line.substr(prefix.size()));
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::nullopt;
  return value.substr(1, value.size() - 2);
}

std::optional<int> parse_manifest_int(std::string_view line, std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  if (line.substr(0, prefix.size()) != prefix) return std::nullopt;
  try {
    return std::stoi(trim(line.substr(prefix.size())));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::vector<std::string>> parse_manifest_string_list(std::string_view line,
                                                                   std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  if (line.substr(0, prefix.size()) != prefix) return std::nullopt;
  std::string value = trim(line.substr(prefix.size()));
  if (value.size() < 2 || value.front() != '[' || value.back() != ']') return std::nullopt;
  value = trim(std::string_view(value).substr(1, value.size() - 2));
  if (value.empty()) return std::vector<std::string>{};

  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < value.size()) {
    std::size_t end = start;
    bool in_string = false;
    while (end < value.size()) {
      const char ch = value[end];
      if (ch == '"' && (end == start || value[end - 1] != '\\')) in_string = !in_string;
      if (!in_string && ch == ',') break;
      end += 1;
    }
    const std::string item = trim(std::string_view(value).substr(start, end - start));
    if (item.size() < 2 || item.front() != '"' || item.back() != '"') return std::nullopt;
    out.push_back(item.substr(1, item.size() - 2));
    start = end + 1;
  }
  return out;
}

std::optional<std::unordered_map<std::string, std::string>> parse_inline_table(std::string_view text) {
  std::string value = trim(text);
  if (value.size() < 2 || value.front() != '{' || value.back() != '}') return std::nullopt;
  value = trim(std::string_view(value).substr(1, value.size() - 2));
  std::unordered_map<std::string, std::string> out;
  std::size_t start = 0;
  while (start < value.size()) {
    std::size_t end = start;
    bool in_string = false;
    while (end < value.size()) {
      const char ch = value[end];
      if (ch == '"' && (end == start || value[end - 1] != '\\')) in_string = !in_string;
      if (!in_string && ch == ',') break;
      end += 1;
    }
    const std::string item = trim(std::string_view(value).substr(start, end - start));
    const std::size_t eq = item.find('=');
    if (eq == std::string::npos) return std::nullopt;
    const std::string key = trim(std::string_view(item).substr(0, eq));
    const std::string raw = trim(std::string_view(item).substr(eq + 1));
    if (key.empty()) return std::nullopt;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
      out[key] = raw.substr(1, raw.size() - 2);
    } else {
      out[key] = raw;
    }
    start = end + 1;
  }
  return out;
}

std::string stable_hash_string(const std::string& text) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : text) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 1099511628211ULL;
  }
  std::ostringstream os;
  os << std::hex << h;
  return os.str();
}

std::string quote_shell_arg(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('\'');
  for (char ch : text) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

std::optional<std::string> capture_command_output(const std::vector<std::string>& args) {
  if (args.empty()) return std::nullopt;
  std::ostringstream cmd;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) cmd << ' ';
    cmd << quote_shell_arg(args[i]);
  }
  cmd << " 2>/dev/null";

  FILE* pipe = popen(cmd.str().c_str(), "r");
  if (pipe == nullptr) return std::nullopt;

  std::string out;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) out += buffer;

  const int status = pclose(pipe);
  if (status == -1) return std::nullopt;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
  return trim(out);
}

std::optional<fs::path> resolve_root_manifest(const fs::path& input) {
  const fs::path abs_input = fs::absolute(input);
  fs::path probe_dir = abs_input;
  if (abs_input.filename() == "nebula.toml") {
    probe_dir = abs_input.parent_path();
  } else if (!fs::is_directory(abs_input)) {
    probe_dir = abs_input.parent_path();
  }

  std::vector<fs::path> manifests;
  std::error_code ec;
  fs::path cur = fs::weakly_canonical(probe_dir, ec);
  if (ec) cur = probe_dir.lexically_normal();
  while (!cur.empty()) {
    const fs::path candidate = cur / "nebula.toml";
    if (fs::exists(candidate)) manifests.push_back(candidate);
    if (cur == cur.root_path()) break;
    cur = cur.parent_path();
  }
  if (manifests.empty()) return std::nullopt;

  fs::path selected = manifests.front();
  for (auto it = manifests.rbegin(); it != manifests.rend(); ++it) {
    PackageManifest manifest;
    std::vector<Diagnostic> scratch;
    if (!read_package_manifest(*it, manifest, scratch, DiagnosticStage::Build)) continue;
    if (manifest.workspace_members.empty()) continue;
    for (const auto& member : manifest.workspace_members) {
      const fs::path member_root = absolute_normalized(it->parent_path() / member);
      const fs::path member_manifest = member_root / "nebula.toml";
      if (path_is_within(abs_input, member_root) ||
          normalize_path_key(abs_input) == normalize_path_key(member_manifest)) {
        selected = *it;
        break;
      }
    }
  }
  return selected;
}

std::string quote_toml(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  out.push_back('"');
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string render_toml_string_list(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ", ";
    out << quote_toml(values[i]);
  }
  out << "]";
  return out.str();
}

std::string render_toml_inline_string_map(
    const std::vector<std::pair<std::string, std::string>>& values) {
  std::ostringstream out;
  out << "{ ";
  const auto ordered = sorted_string_pairs(values);
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (i) out << ", ";
    out << ordered[i].first << " = " << quote_toml(ordered[i].second);
  }
  out << " }";
  return out.str();
}

std::string lock_relative_path_string(const fs::path& lock_path, const fs::path& value) {
  if (value.empty()) return {};
  const fs::path base_dir = lock_path.parent_path();
  const fs::path abs_value = fs::absolute(value).lexically_normal();
  std::error_code ec;
  fs::path rel = fs::relative(abs_value, base_dir, ec);
  if (ec) return abs_value.generic_string();
  return rel.generic_string();
}

fs::path resolve_lock_path(const fs::path& lock_path, const std::string& stored) {
  if (stored.empty()) return {};
  fs::path value = stored;
  if (value.is_absolute()) return value.lexically_normal();
  return fs::absolute(lock_path.parent_path() / value).lexically_normal();
}

std::string manifest_fingerprint(const PackageManifest& manifest) {
  std::ostringstream os;
  os << "schema_version=" << manifest.schema_version << "\n";
  if (!manifest_is_workspace_container(manifest)) {
    os << "name=" << manifest.name << "\n";
  }
  os << "version=" << manifest.version << "\n";
  os << "entry=" << manifest.entry.generic_string() << "\n";
  os << "src_dir=" << manifest.src_dir.generic_string() << "\n";

  const std::vector<std::string> host_cxx = sorted_generic_paths(manifest.host_cxx);
  for (const auto& path : host_cxx) os << "host_cxx=" << path << "\n";

  const std::vector<std::string> native_c = sorted_generic_paths(manifest.native.c_sources);
  for (const auto& path : native_c) os << "native.c=" << path << "\n";
  const std::vector<std::string> native_cxx = sorted_generic_paths(manifest.native.cxx_sources);
  for (const auto& path : native_cxx) os << "native.cxx=" << path << "\n";
  const std::vector<std::string> native_include_dirs = sorted_generic_paths(manifest.native.include_dirs);
  for (const auto& path : native_include_dirs) os << "native.include_dir=" << path << "\n";
  std::vector<std::string> native_defines = manifest.native.defines;
  std::sort(native_defines.begin(), native_defines.end());
  for (const auto& define : native_defines) os << "native.define=" << define << "\n";

  std::vector<std::string> native_sources;
  native_sources.reserve(manifest.native.sources.size());
  for (const auto& source : manifest.native.sources) {
    std::ostringstream line;
    line << "path=" << source.path.generic_string();
    line << "|language=" << native_language_name(source.language);
    for (const auto& include_dir : sorted_generic_paths(source.include_dirs)) {
      line << "|include_dir=" << include_dir;
    }
    auto defines = source.defines;
    std::sort(defines.begin(), defines.end());
    for (const auto& define : defines) line << "|define=" << define;
    auto arch = source.arch;
    std::sort(arch.begin(), arch.end());
    for (const auto& item : arch) line << "|arch=" << item;
    auto cpu_features = source.cpu_features;
    std::sort(cpu_features.begin(), cpu_features.end());
    for (const auto& item : cpu_features) line << "|cpu_feature=" << item;
    native_sources.push_back(line.str());
  }
  std::sort(native_sources.begin(), native_sources.end());
  for (const auto& source : native_sources) os << "native.source=" << source << "\n";

  std::vector<std::string> native_generated_headers;
  native_generated_headers.reserve(manifest.native.generated_headers.size());
  for (const auto& header : manifest.native.generated_headers) {
    std::ostringstream line;
    line << "out=" << header.out.generic_string();
    line << "|template=" << header.template_path.generic_string();
    for (const auto& [key, value] : sorted_string_pairs(header.values)) {
      line << "|value." << key << "=" << value;
    }
    native_generated_headers.push_back(line.str());
  }
  std::sort(native_generated_headers.begin(), native_generated_headers.end());
  for (const auto& header : native_generated_headers) {
    os << "native.generated_header=" << header << "\n";
  }

  const std::vector<std::string> workspace_members = sorted_generic_paths(manifest.workspace_members);
  for (const auto& path : workspace_members) os << "workspace_member=" << path << "\n";

  std::vector<ManifestDependency> deps = manifest.dependencies;
  std::sort(deps.begin(), deps.end(), [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
    if (lhs.alias != rhs.alias) return lhs.alias < rhs.alias;
    if (lhs.kind != rhs.kind) return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
    if (lhs.version != rhs.version) return lhs.version < rhs.version;
    if (lhs.path != rhs.path) return lhs.path.generic_string() < rhs.path.generic_string();
    if (lhs.git != rhs.git) return lhs.git < rhs.git;
    if (lhs.installed != rhs.installed) return lhs.installed < rhs.installed;
    return lhs.rev < rhs.rev;
  });
  for (const auto& dep : deps) {
    os << "dep.alias=" << dep.alias << "\n";
    os << "dep.kind=" << static_cast<int>(dep.kind) << "\n";
    os << "dep.version=" << dep.version << "\n";
    os << "dep.path=" << dep.path.generic_string() << "\n";
    os << "dep.git=" << dep.git << "\n";
    os << "dep.rev=" << dep.rev << "\n";
    os << "dep.installed=" << dep.installed << "\n";
  }

  return stable_hash_string(os.str());
}

fs::path default_registry_root_for_manifest(const fs::path& manifest_path) {
  return manifest_path.parent_path() / ".nebula" / "registry";
}

[[maybe_unused]] fs::path registry_root_for_manifest(const fs::path& manifest_path) {
  const char* registry_root_env = std::getenv("NEBULA_REGISTRY_ROOT");
  if (registry_root_env != nullptr && *registry_root_env != '\0') {
    return absolute_normalized(fs::path(registry_root_env));
  }
  return absolute_normalized(default_registry_root_for_manifest(manifest_path));
}

bool is_tracked_package_source(const fs::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".nb" || ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
         ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

std::vector<fs::path> collect_package_source_files(const LockedPackage& pkg,
                                                   const PackageManifest& manifest) {
  std::vector<fs::path> files;
  std::set<std::string> seen;
  auto add_file = [&](const fs::path& candidate) {
    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec || !fs::is_regular_file(candidate, ec) || ec) return;
    const fs::path normalized = fs::absolute(candidate).lexically_normal();
    const std::string key = normalized.string();
    if (!seen.insert(key).second) return;
    files.push_back(normalized);
  };
  auto add_dir = [&](const fs::path& dir) {
    std::error_code dir_ec;
    if (!fs::exists(dir, dir_ec) || dir_ec || !fs::is_directory(dir, dir_ec) || dir_ec) return;
    for (const auto& entry : fs::recursive_directory_iterator(dir, dir_ec)) {
      if (dir_ec) break;
      if (!entry.is_regular_file()) continue;
      if (!is_tracked_package_source(entry.path())) continue;
      add_file(entry.path());
    }
  };

  const fs::path src_root = manifest.src_dir.empty() ? pkg.root : (pkg.root / manifest.src_dir);
  add_dir(src_root);

  add_file(pkg.root / manifest.entry);
  for (const auto& rel : manifest.host_cxx) add_file(pkg.root / rel);
  for (const auto& rel : manifest.native.c_sources) add_file(pkg.root / rel);
  for (const auto& rel : manifest.native.cxx_sources) add_file(pkg.root / rel);
  for (const auto& rel : manifest.native.include_dirs) add_dir(pkg.root / rel);
  for (const auto& source : manifest.native.sources) {
    add_file(pkg.root / source.path);
    for (const auto& rel : source.include_dirs) add_dir(pkg.root / rel);
  }
  for (const auto& header : manifest.native.generated_headers) add_file(pkg.root / header.template_path);

  std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return files;
}

std::string package_source_fingerprint(const LockedPackage& pkg, const PackageManifest& manifest) {
  std::ostringstream os;
  for (const auto& file : collect_package_source_files(pkg, manifest)) {
    std::string text;
    if (!read_text_file(file, text)) {
      os << "missing=" << file.generic_string() << "\n";
      continue;
    }
    os << "path=" << file.generic_string() << "\n";
    os << "hash=" << stable_hash_string(text) << "\n";
  }
  return stable_hash_string(os.str());
}

std::vector<fs::path> collect_registry_integrity_files(const LockedPackage& pkg) {
  std::vector<fs::path> files;
  std::set<std::string> seen;
  std::error_code ec;
  if (!fs::exists(pkg.root, ec) || ec || !fs::is_directory(pkg.root, ec) || ec) return files;

  for (const auto& entry : fs::recursive_directory_iterator(pkg.root, ec)) {
    if (ec) break;
    const fs::path rel = fs::relative(entry.path(), pkg.root, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    bool skip = false;
    for (const auto& part : rel) {
      const std::string piece = part.string();
      if (piece == ".git" || piece == ".nebula") {
        skip = true;
        break;
      }
    }
    if (skip) {
      if (entry.is_directory()) {
        ec.clear();
      }
      continue;
    }
    if (!entry.is_regular_file()) continue;
    if (rel.filename() == "nebula.lock") continue;
    const fs::path normalized = absolute_normalized(entry.path());
    if (!seen.insert(normalized.string()).second) continue;
    files.push_back(normalized);
  }

  std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return files;
}

std::string package_integrity_fingerprint(const LockedPackage& pkg, const PackageManifest& manifest) {
  if (pkg.source_kind == "workspace") {
    return stable_hash_string("workspace\n" + manifest_fingerprint(manifest));
  }

  std::ostringstream os;
  os << "name=" << pkg.name << "\n";
  os << "version=" << pkg.version << "\n";
  os << "source_kind=" << pkg.source_kind << "\n";
  if (pkg.source_kind != "path" && pkg.source_kind != "implicit") {
    os << "source_hint=" << pkg.source_hint << "\n";
  }
  os << "resolved_rev=" << pkg.resolved_rev << "\n";
  os << "manifest_hash=" << manifest_fingerprint(manifest) << "\n";
  os << "source_fingerprint=" << package_source_fingerprint(pkg, manifest) << "\n";

  if (pkg.source_kind == "registry") {
    for (const auto& file : collect_registry_integrity_files(pkg)) {
      std::string text;
      if (!read_text_file(file, text)) {
        os << "missing=" << fs::relative(file, pkg.root).generic_string() << "\n";
        continue;
      }
      std::error_code ec;
      fs::path rel = fs::relative(file, pkg.root, ec);
      if (ec) rel = file.filename();
      os << "registry_path=" << rel.generic_string() << "\n";
      os << "registry_hash=" << stable_hash_string(text) << "\n";
    }
  }

  return stable_hash_string(os.str());
}

std::string package_lock_source_fingerprint(const LockedPackage& pkg, const PackageManifest& manifest) {
  if (pkg.source_kind == "workspace") {
    return stable_hash_string("workspace-source\n" + manifest_fingerprint(manifest));
  }
  return package_source_fingerprint(pkg, manifest);
}

std::string directory_tree_fingerprint(const fs::path& root) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (!fs::exists(root, ec) || ec) return {};
  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });

  std::ostringstream os;
  for (const auto& file : files) {
    std::string text;
    if (!read_text_file(file, text)) {
      os << "missing=" << file.generic_string() << "\n";
      continue;
    }
    std::error_code rel_ec;
    fs::path rel = fs::relative(file, root, rel_ec);
    if (rel_ec) rel = file.filename();
    os << "path=" << rel.generic_string() << "\n";
    os << "hash=" << stable_hash_string(text) << "\n";
  }
  return stable_hash_string(os.str());
}

std::string render_manifest_text(const PackageManifest& manifest) {
  auto quote = [](const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    out.push_back('"');
    for (char ch : value) {
      if (ch == '\\' || ch == '"') out.push_back('\\');
      out.push_back(ch);
    }
    out.push_back('"');
    return out;
  };

  auto render_string_list = [&](const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i) out << ", ";
      out << quote(values[i]);
    }
    out << "]";
    return out.str();
  };

  std::ostringstream out;
  out << "schema_version = " << manifest.schema_version << "\n\n";
  out << "[package]\n";
  out << "name = " << quote(manifest.name) << "\n";
  out << "version = " << quote(manifest.version) << "\n";
  out << "entry = " << quote(manifest.entry.lexically_normal().generic_string()) << "\n";
  out << "src_dir = " << quote(manifest.src_dir.lexically_normal().generic_string()) << "\n";
  if (!manifest.host_cxx.empty()) {
    const std::vector<std::string> host_items = sorted_generic_paths(manifest.host_cxx);
    out << "host_cxx = " << render_string_list(host_items) << "\n";
  }
  if (!manifest.native.empty()) {
    out << "\n[native]\n";
    if (!manifest.native.c_sources.empty()) {
      out << "c_sources = " << render_string_list(sorted_generic_paths(manifest.native.c_sources)) << "\n";
    }
    if (!manifest.native.cxx_sources.empty()) {
      out << "cxx_sources = " << render_string_list(sorted_generic_paths(manifest.native.cxx_sources))
          << "\n";
    }
    if (!manifest.native.include_dirs.empty()) {
      out << "include_dirs = "
          << render_string_list(sorted_generic_paths(manifest.native.include_dirs)) << "\n";
    }
    if (!manifest.native.defines.empty()) {
      std::vector<std::string> defines = manifest.native.defines;
      std::sort(defines.begin(), defines.end());
      out << "defines = " << render_string_list(defines) << "\n";
    }
    for (const auto& source : manifest.native.sources) {
      out << "\n[[native.sources]]\n";
      out << "path = " << quote(source.path.lexically_normal().generic_string()) << "\n";
      out << "language = " << quote(native_language_name(source.language)) << "\n";
      if (!source.include_dirs.empty()) {
        out << "include_dirs = " << render_string_list(sorted_generic_paths(source.include_dirs)) << "\n";
      }
      if (!source.defines.empty()) {
        std::vector<std::string> defines = source.defines;
        std::sort(defines.begin(), defines.end());
        out << "defines = " << render_string_list(defines) << "\n";
      }
      if (!source.arch.empty()) {
        std::vector<std::string> arch = source.arch;
        std::sort(arch.begin(), arch.end());
        out << "arch = " << render_string_list(arch) << "\n";
      }
      if (!source.cpu_features.empty()) {
        std::vector<std::string> cpu_features = source.cpu_features;
        std::sort(cpu_features.begin(), cpu_features.end());
        out << "cpu_features = " << render_string_list(cpu_features) << "\n";
      }
    }
    for (const auto& header : manifest.native.generated_headers) {
      out << "\n[[native.generated_headers]]\n";
      out << "out = " << quote(header.out.lexically_normal().generic_string()) << "\n";
      out << "template = " << quote(header.template_path.lexically_normal().generic_string()) << "\n";
      out << "values = " << render_toml_inline_string_map(header.values) << "\n";
    }
  }
  if (!manifest.dependencies.empty()) {
    out << "\n[dependencies]\n";
    std::vector<ManifestDependency> deps = manifest.dependencies;
    std::sort(deps.begin(), deps.end(), [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
      return lhs.alias < rhs.alias;
    });
    for (const auto& dep : deps) {
      out << dep.alias << " = ";
      if (dep.kind == ManifestDependencyKind::Version) {
        out << quote(dep.version) << "\n";
      } else if (dep.kind == ManifestDependencyKind::Path) {
        out << "{ path = " << quote(dep.path.lexically_normal().generic_string()) << " }\n";
      } else if (dep.kind == ManifestDependencyKind::Installed) {
        out << "{ installed = " << quote(dep.installed) << " }\n";
      } else {
        out << "{ git = " << quote(dep.git) << ", rev = " << quote(dep.rev) << " }\n";
      }
    }
  }
  if (!manifest.workspace_members.empty()) {
    out << "\n[workspace]\n";
    std::vector<std::string> members;
    members.reserve(manifest.workspace_members.size());
    for (const auto& member : manifest.workspace_members) {
      members.push_back(member.lexically_normal().generic_string());
    }
    std::sort(members.begin(), members.end());
    out << "members = " << render_string_list(members) << "\n";
  }
  return out.str();
}

std::string render_publish_metadata(const PackageManifest& manifest,
                                    const LockedPackage& pkg,
                                    std::string_view manifest_hash,
                                    std::string_view source_fingerprint) {
  std::ostringstream out;
  out << "schema_version = 1\n";
  out << "name = " << quote_toml(manifest.name) << "\n";
  out << "version = " << quote_toml(manifest.version) << "\n";
  out << "source_kind = " << quote_toml(pkg.source_kind) << "\n";
  out << "source_hint = " << quote_toml(pkg.source_hint) << "\n";
  out << "manifest_hash = " << quote_toml(std::string(manifest_hash)) << "\n";
  out << "source_fingerprint = " << quote_toml(std::string(source_fingerprint)) << "\n";
  out << "dependencies = [";
  std::vector<ManifestDependency> deps = manifest.dependencies;
  std::sort(deps.begin(), deps.end(), [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
    return lhs.alias < rhs.alias;
  });
  for (std::size_t i = 0; i < deps.size(); ++i) {
    if (i) out << ", ";
    std::string encoded = deps[i].alias + "=";
    if (deps[i].kind == ManifestDependencyKind::Version) {
      encoded += "version:" + deps[i].version;
    } else if (deps[i].kind == ManifestDependencyKind::Path) {
      encoded += "path:" + deps[i].path.generic_string();
    } else if (deps[i].kind == ManifestDependencyKind::Installed) {
      encoded += "installed:" + deps[i].installed;
    } else {
      encoded += "git:" + deps[i].git + "#" + deps[i].rev;
    }
    out << quote_toml(encoded);
  }
  out << "]\n";
  return out.str();
}

std::optional<std::string> git_head_revision(const fs::path& repo_root) {
  return capture_command_output({"git", "-C", repo_root.string(), "rev-parse", "HEAD"});
}

bool preflight_git_dependency_environment_impl(const ManifestDependency& dep,
                                               std::vector<Diagnostic>& diags,
                                               DiagnosticStage stage,
                                               bool require_remote_probe,
                                               std::string_view command_hint) {
  const std::string command_text =
      command_hint.empty() ? std::string("fetch/update") : std::string(command_hint);
  if (!find_executable_on_path("git").has_value()) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-DEP-GIT-TOOL",
        "git dependency support requires `git` on PATH", stage, DiagnosticRisk::High,
        "nebula resolves git dependencies by invoking the host git executable",
        "git dependencies cannot be fetched, updated, or pinned",
        {"install git and retry nebula " + command_text,
         "or replace the git dependency with a path/version source"}));
    return false;
  }

  if (!require_remote_probe) return true;
  if (!capture_command_output(
          {"git", "-c", "credential.interactive=never", "-c", "core.askPass=", "ls-remote",
           "--symref", dep.git, "HEAD"})
           .has_value()) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-DEP-GIT-REMOTE",
        "failed to reach git dependency source before checkout: " + dep.git, stage, DiagnosticRisk::High,
        "git ls-remote could not query the dependency source",
        "dependency resolution cannot continue until the source is reachable",
        {"check network/auth access to the git remote before nebula " + command_text,
         "verify the git URL/path exists",
         "or remove the git dependency"}));
    return false;
  }
  return true;
}

fs::path lockfile_cache_root(const fs::path& manifest_path) {
  return manifest_path.parent_path() / ".nebula";
}

bool ensure_git_checkout(const ManifestDependency& dep,
                         const fs::path& checkout_root,
                         std::vector<Diagnostic>& diags,
                         DiagnosticStage stage,
                         bool refresh) {
  const fs::path git_dir = checkout_root / ".git";
  if (!preflight_git_dependency_environment_impl(
          dep, diags, stage, !fs::exists(git_dir) || refresh, "fetch/update")) {
    return false;
  }
  if (!fs::exists(git_dir)) {
    fs::create_directories(checkout_root.parent_path());
    if (run_command({"git", "clone", dep.git, checkout_root.string()}) != 0) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-DEP-GIT-CLONE",
          "failed to clone git dependency: " + dep.git, stage, DiagnosticRisk::High,
          "git clone did not complete successfully", "dependency source is unavailable",
          {"run nebula fetch again after fixing network/auth", "or remove the git dependency"}));
      return false;
    }
  } else if (refresh) {
    if (run_command({"git", "-C", checkout_root.string(), "fetch", "--all", "--tags", "--force"}) != 0) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-DEP-GIT-FETCH",
          "failed to refresh git dependency: " + dep.git, stage, DiagnosticRisk::High,
          "git fetch did not complete successfully", "dependency revision may be stale",
          {"run nebula update again after fixing network/auth"}));
      return false;
    }
  }

  if (!dep.rev.empty()) {
    if (run_command({"git", "-C", checkout_root.string(), "checkout", "--force", dep.rev}) != 0) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-DEP-GIT-REV",
          "failed to checkout git dependency revision: " + dep.rev, stage, DiagnosticRisk::High,
          "requested git revision was not available", "dependency source cannot be pinned",
          {"verify rev/tag/branch exists", "or refresh with nebula update"}));
      return false;
    }
  }

  return true;
}

std::optional<ManifestDependency> locked_git_dependency_from_package(const LockedPackage& pkg) {
  ManifestDependency dep;
  dep.alias = pkg.name;
  dep.kind = ManifestDependencyKind::Git;
  if (!pkg.git_url.empty()) {
    dep.git = pkg.git_url;
    dep.rev = pkg.resolved_rev.empty() ? pkg.git_requested_rev : pkg.resolved_rev;
  } else {
    if (pkg.source_kind != "git" || pkg.source_hint.empty()) return std::nullopt;
    const std::size_t sep = pkg.source_hint.rfind('#');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= pkg.source_hint.size()) {
      return std::nullopt;
    }
    dep.git = pkg.source_hint.substr(0, sep);
    dep.rev = pkg.resolved_rev.empty() ? pkg.source_hint.substr(sep + 1) : pkg.resolved_rev;
  }
  if (dep.git.empty() || dep.rev.empty()) return std::nullopt;
  return dep;
}

bool restore_missing_locked_git_checkouts(ProjectLock& lock,
                                          std::vector<Diagnostic>& diags,
                                          DiagnosticStage stage) {
  for (auto& pkg : lock.packages) {
    if (pkg.source_kind != "git") continue;
    if (!pkg.manifest_path.empty() && fs::exists(pkg.manifest_path)) continue;

    const auto dep = locked_git_dependency_from_package(pkg);
    if (!dep.has_value()) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-LOCK-GIT-SOURCE",
          "git-locked package has invalid source_hint/revision: " + pkg.name, stage,
          DiagnosticRisk::High,
          "nebula.lock cannot reconstruct the pinned git dependency checkout",
          "dependency graph is no longer reproducible from the lockfile alone",
          {"re-run nebula update to regenerate nebula.lock"}));
      return false;
    }

    if (!ensure_git_checkout(*dep, pkg.root, diags, stage, false)) return false;
  }
  return true;
}

std::optional<std::pair<std::string, std::string>> split_dependency_import(std::string_view import_name) {
  const std::size_t sep = import_name.find("::");
  if (sep == std::string::npos) return std::nullopt;
  const std::string alias(import_name.substr(0, sep));
  const std::string module(import_name.substr(sep + 2));
  if (alias.empty() || module.empty()) return std::nullopt;
  return std::make_pair(alias, module);
}

bool is_reserved_std_name(std::string_view name) {
  return name == "std";
}

bool is_hosted_std_module(std::string_view module_name) {
  return module_name == "env" || module_name == "fs" || module_name == "http" ||
         module_name == "http_json" || module_name == "net" || module_name == "process" ||
         module_name == "task" || module_name == "time";
}

std::string runtime_profile_name(RuntimeProfile profile) {
  switch (profile) {
  case RuntimeProfile::Hosted: return "hosted";
  case RuntimeProfile::System: return "system";
  }
  return "hosted";
}

std::optional<fs::path> resolve_std_module_file(const fs::path& std_root, std::string_view module_name) {
  if (std_root.empty()) return std::nullopt;
  fs::path rel;
  std::string current;
  for (char ch : module_name) {
    if (ch == '.') {
      if (!current.empty()) {
        rel /= current;
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) rel /= current;
  rel += ".nb";
  const fs::path candidate = std_root / rel;
  if (!fs::exists(candidate)) return std::nullopt;
  return absolute_normalized(candidate);
}

bool parse_program_headers(const SourceFile& source,
                          Program& program,
                          std::vector<Diagnostic>& diags,
                          DiagnosticStage stage) {
  try {
    Lexer lex(source.text, source.path);
    auto toks = lex.lex_all();
    Parser parser(std::move(toks));
    program = parser.parse_program();
    return true;
  } catch (const nebula::frontend::FrontendError& e) {
    Diagnostic d;
    d.severity = Severity::Error;
    d.code = "NBL-PAR900";
    d.message = e.what();
    d.span = e.span;
    if (d.span.source_path.empty()) d.span.source_path = source.path;
    d.stage = stage;
    d.category = "parser";
    d.risk = DiagnosticRisk::High;
    diags.push_back(std::move(d));
    return false;
  }
}

fs::path source_root_for_package(const LockedPackage& pkg) {
  return pkg.src_dir.empty() ? pkg.root : (pkg.root / pkg.src_dir);
}

std::string module_name_from_path(const LockedPackage& pkg, const fs::path& file) {
  const fs::path src_root = source_root_for_package(pkg);
  std::error_code ec;
  fs::path rel = fs::relative(file, src_root, ec);
  if (ec) rel = file.filename();
  rel.replace_extension();

  std::string out;
  for (const auto& part : rel) {
    const std::string piece = part.string();
    if (piece.empty() || piece == ".") continue;
    if (!out.empty()) out.push_back('.');
    out += piece;
  }
  return out.empty() ? file.stem().string() : out;
}

std::optional<fs::path> resolve_module_file(const LockedPackage& pkg,
                                            const fs::path& from_dir,
                                            std::string_view module_name) {
  fs::path rel;
  std::string current;
  for (char ch : module_name) {
    if (ch == '.') {
      if (!current.empty()) {
        rel /= current;
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) rel /= current;
  rel += ".nb";

  std::vector<fs::path> roots;
  roots.push_back(source_root_for_package(pkg));
  roots.push_back(from_dir);
  roots.push_back(pkg.root);

  std::set<std::string> seen;
  for (const auto& root : roots) {
    const fs::path candidate = root / rel;
    const std::string key = normalize_path_key(candidate);
    if (!seen.insert(key).second) continue;
    if (fs::exists(candidate)) return candidate;
  }
  return std::nullopt;
}

[[maybe_unused]] std::string workspace_package_name(const fs::path& manifest_path) {
  return "@workspace:" + manifest_path.parent_path().filename().string();
}

[[maybe_unused]] bool collect_workspace_member_manifests(const fs::path& workspace_manifest_path,
                                                         const PackageManifest& workspace_manifest,
                                                         std::vector<fs::path>& out,
                                                         std::vector<Diagnostic>& diags,
                                                         DiagnosticStage stage) {
  out.clear();
  std::set<std::string> seen;
  for (const auto& rel : workspace_manifest.workspace_members) {
    const fs::path member_manifest =
        absolute_normalized(workspace_manifest_path.parent_path() / rel / "nebula.toml");
    const std::string member_key = normalize_path_key(member_manifest);
    if (member_key == normalize_path_key(workspace_manifest_path)) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-WORKSPACE-MEMBER",
          "workspace member points at the workspace root: " + rel.string(), stage,
          DiagnosticRisk::High, "workspace member list must point at package manifests",
          "workspace package graph cannot be constructed"));
      return false;
    }
    if (!seen.insert(member_key).second) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-WORKSPACE-MEMBER",
          "duplicate workspace member detected: " + rel.string(), stage, DiagnosticRisk::High,
          "workspace member list resolves the same package more than once",
          "workspace package graph would become ambiguous"));
      return false;
    }
    if (!fs::exists(member_manifest)) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-WORKSPACE-MEMBER",
          "workspace member manifest not found: " + member_manifest.string(), stage,
          DiagnosticRisk::High, "workspace member list references a missing package manifest",
          "workspace package graph cannot be loaded",
          {"add nebula.toml to the member package", "or remove the bad workspace member entry"}));
      return false;
    }
    out.push_back(member_manifest);
  }
  std::sort(out.begin(), out.end(), [](const fs::path& lhs, const fs::path& rhs) {
    return lhs.generic_string() < rhs.generic_string();
  });
  return true;
}

[[maybe_unused]] std::optional<fs::path> select_workspace_member_manifest(
    const fs::path& workspace_manifest_path,
    const PackageManifest& workspace_manifest,
    const fs::path& input_path) {
  const fs::path abs_input = absolute_normalized(input_path);
  const std::string input_key = normalize_path_key(abs_input);
  const std::string workspace_root_key = normalize_path_key(workspace_manifest_path.parent_path());
  const std::string workspace_manifest_key = normalize_path_key(workspace_manifest_path);
  std::vector<fs::path> member_manifests;
  member_manifests.reserve(workspace_manifest.workspace_members.size());
  std::optional<std::pair<std::size_t, fs::path>> best;
  for (const auto& rel : workspace_manifest.workspace_members) {
    const fs::path member_root = absolute_normalized(workspace_manifest_path.parent_path() / rel);
    const fs::path member_manifest = member_root / "nebula.toml";
    member_manifests.push_back(member_manifest);
    if (!path_is_within(abs_input, member_root) &&
        input_key != normalize_path_key(member_manifest)) {
      continue;
    }

    const std::size_t score = member_root.generic_string().size();
    if (!best.has_value() || score > best->first) best = std::make_pair(score, member_manifest);
  }
  if (!best.has_value() &&
      (input_key == workspace_root_key || input_key == workspace_manifest_key)) {
    std::sort(member_manifests.begin(), member_manifests.end(), [](const fs::path& lhs, const fs::path& rhs) {
      return lhs.generic_string() < rhs.generic_string();
    });
    if (!member_manifests.empty()) return member_manifests.front();
  }
  if (!best.has_value()) return std::nullopt;
  return best->second;
}

std::optional<fs::path> default_workspace_member_manifest(const fs::path& workspace_manifest_path,
                                                          const PackageManifest& workspace_manifest) {
  if (workspace_manifest.workspace_members.empty()) return std::nullopt;
  std::vector<fs::path> ordered_members = workspace_manifest.workspace_members;
  std::sort(ordered_members.begin(), ordered_members.end(),
            [](const fs::path& lhs, const fs::path& rhs) { return lhs.generic_string() < rhs.generic_string(); });
  for (const auto& rel : ordered_members) {
    const fs::path member_manifest =
        absolute_normalized(workspace_manifest_path.parent_path() / rel / "nebula.toml");
    if (fs::exists(member_manifest)) return member_manifest;
  }
  return std::nullopt;
}

bool lockfile_is_stale(const fs::path& lock_path,
                       const ProjectLock& lock,
                       const fs::path& root_manifest_path) {
  if (lock.schema_version != kProjectLockSchemaVersion) return true;
  if (lock.toolchain_schema_version != kLockToolchainSchemaVersion) return true;
  if (lock.cache_schema_version != kLockCacheSchemaVersion) return true;

  if (!root_manifest_path.empty()) {
    std::vector<Diagnostic> diags;
    PackageManifest manifest;
    if (!read_package_manifest(root_manifest_path, manifest, diags, DiagnosticStage::Build)) return true;
    if (lock.root_manifest_hash != manifest_fingerprint(manifest)) return true;
  }

  for (const auto& pkg : lock.packages) {
    if (pkg.manifest_path.empty()) return true;

    std::vector<Diagnostic> diags;
    PackageManifest manifest;
    if (!read_package_manifest(pkg.manifest_path, manifest, diags, DiagnosticStage::Build)) return true;
    if (pkg.manifest_hash != manifest_fingerprint(manifest)) return true;
    if (pkg.source_fingerprint != package_lock_source_fingerprint(pkg, manifest)) return true;
    if (pkg.integrity_fingerprint != package_integrity_fingerprint(pkg, manifest)) return true;
  }

  (void)lock_path;
  return false;
}

bool resolve_project_lock_from_manifest(const fs::path& manifest_path,
                                        ProjectLock& out,
                                        std::vector<Diagnostic>& diags,
                                        DiagnosticStage stage,
                                        const fs::path& backend_sdk_root,
                                        bool refresh_git_sources) {
  out = ProjectLock{};
  out.schema_version = kProjectLockSchemaVersion;
  out.toolchain_schema_version = kLockToolchainSchemaVersion;
  out.cache_schema_version = kLockCacheSchemaVersion;

  PackageManifest root_manifest;
  if (!read_package_manifest(manifest_path, root_manifest, diags, stage)) return false;

  std::vector<fs::path> workspace_member_manifests;
  if (!root_manifest.workspace_members.empty() &&
      !collect_workspace_member_manifests(manifest_path, root_manifest, workspace_member_manifests,
                                          diags, stage)) {
    return false;
  }
  const bool workspace_container = manifest_is_workspace_container(root_manifest);

  struct WorkspaceVersionTarget {
    fs::path manifest_path;
    fs::path package_root;
  };
  std::unordered_map<std::string, WorkspaceVersionTarget> workspace_version_targets;
  auto register_workspace_version_target = [&](const fs::path& pkg_manifest_path,
                                               const PackageManifest& pkg_manifest) -> bool {
    const std::string key = pkg_manifest.name + "@" + pkg_manifest.version;
    if (pkg_manifest.name.empty()) return true;
    auto [it, inserted] =
        workspace_version_targets.emplace(key, WorkspaceVersionTarget{pkg_manifest_path, pkg_manifest_path.parent_path()});
    if (!inserted && normalize_path_key(it->second.manifest_path) != normalize_path_key(pkg_manifest_path)) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-WORKSPACE-VERSION-DUP",
          "workspace exact-version target is ambiguous: " + key, stage, DiagnosticRisk::High,
          "multiple workspace manifests provide the same name/version pair",
          "exact-version dependency resolution cannot choose a unique package",
          {"deduplicate workspace package versions", "or switch the dependency to an explicit path/git source"}));
      return false;
    }
    return true;
  };

  if (root_manifest.has_explicit_package && !workspace_container &&
      !register_workspace_version_target(manifest_path, root_manifest)) {
    return false;
  }
  for (const auto& member_manifest_path : workspace_member_manifests) {
    PackageManifest member_manifest;
    if (!read_package_manifest(member_manifest_path, member_manifest, diags, stage)) return false;
    if (!register_workspace_version_target(member_manifest_path, member_manifest)) return false;
  }

  struct VisitState {
    std::unordered_map<std::string, LockedPackageWithManifest> by_name;
    std::unordered_map<std::string, std::string> manifest_to_name;
    std::unordered_set<std::string> visiting;
  };

  VisitState state;
  const fs::path cache_root = lockfile_cache_root(manifest_path) / "git";
  const fs::path registry_root = registry_root_for_manifest(manifest_path);
  const std::string root_manifest_key = normalize_path_key(manifest_path);

  std::function<std::optional<std::string>(const fs::path&, bool)> visit_manifest =
      [&](const fs::path& cur_manifest_path, bool is_root) -> std::optional<std::string> {
        const std::string manifest_key = normalize_path_key(cur_manifest_path);
        auto known = state.manifest_to_name.find(manifest_key);
        if (known != state.manifest_to_name.end()) return known->second;
        if (!state.visiting.insert(manifest_key).second) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-PKG-CYCLE",
              "package dependency cycle detected at: " + cur_manifest_path.string(), stage,
              DiagnosticRisk::High, "package graph contains a cycle",
              "dependency resolution is undefined until the cycle is removed"));
          return std::nullopt;
        }

        PackageManifest manifest;
        if (manifest_key == root_manifest_key) {
          manifest = root_manifest;
        } else if (!read_package_manifest(cur_manifest_path, manifest, diags, stage)) {
          state.visiting.erase(manifest_key);
          return std::nullopt;
        }

        LockedPackageWithManifest node;
        node.locked.is_root = is_root;
        node.locked.name = manifest.name;
        node.locked.version = manifest.version;
        node.locked.root = cur_manifest_path.parent_path();
        node.locked.manifest_path = cur_manifest_path;
        node.locked.entry = manifest.entry;
        node.locked.src_dir = manifest.src_dir;
        node.locked.source_kind = is_root ? "root" : "path";
        node.locked.manifest_hash = manifest_fingerprint(manifest);
        node.locked.host_cxx_sources = manifest.host_cxx;
        node.locked.native = manifest.native;
        node.manifest = manifest;

        if (manifest.name.empty()) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-NAME",
              "manifest missing package.name: " + cur_manifest_path.string(), stage,
              DiagnosticRisk::High, "package manifest did not declare a package name",
              "dependency resolution cannot assign a stable package identity"));
          state.visiting.erase(manifest_key);
          return std::nullopt;
        }

        auto existing_pkg = state.by_name.find(manifest.name);
        if (existing_pkg != state.by_name.end() &&
            normalize_path_key(existing_pkg->second.locked.manifest_path) != manifest_key) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-PKG-DUP",
              "duplicate package name detected: " + manifest.name, stage, DiagnosticRisk::High,
              "multiple manifests resolve to the same package identity",
              "package imports would become ambiguous"));
          state.visiting.erase(manifest_key);
          return std::nullopt;
        }

        for (const auto& dep : manifest.dependencies) {
          fs::path dep_manifest_path;
          std::string source_kind = "path";
          std::string source_hint;
          if (dep.kind == ManifestDependencyKind::Path) {
            dep_manifest_path = fs::absolute(node.locked.root / dep.path) / "nebula.toml";
            source_kind = "path";
            source_hint = fs::absolute(node.locked.root / dep.path).string();
          } else if (dep.kind == ManifestDependencyKind::Git) {
            const std::string checkout_id = dep.alias + "-" + stable_hash_string(dep.git + "#" + dep.rev);
            const fs::path checkout_root = cache_root / checkout_id;
            if (!ensure_git_checkout(dep, checkout_root, diags, stage, refresh_git_sources)) {
              state.visiting.erase(manifest_key);
              return std::nullopt;
            }
            dep_manifest_path = checkout_root / "nebula.toml";
            source_kind = "git";
            source_hint = dep.git + "#" + dep.rev;
          } else if (dep.kind == ManifestDependencyKind::Installed) {
            source_kind = "installed";
            source_hint = dep.installed;
            if (backend_sdk_root.empty()) {
              diags.push_back(make_cli_diag(
                  Severity::Error, "NBL-CLI-DEP-INSTALLED-MISSING",
                  "installed backend SDK dependency is unavailable: " + dep.alias, stage,
                  DiagnosticRisk::High,
                  "nebula could not resolve an installed backend SDK root for this binary",
                  "package graph cannot resolve installed backend service packages",
                  {"install Nebula with --with-backend-sdk on Linux",
                   "or replace the dependency with a path source from a repo checkout"}));
              state.visiting.erase(manifest_key);
              return std::nullopt;
            }
            dep_manifest_path =
                absolute_normalized(backend_sdk_root / dep.installed / "nebula.toml");
          } else {
            const std::string version_key = dep.alias + "@" + dep.version;
            auto workspace_target = workspace_version_targets.find(version_key);
            if (workspace_target != workspace_version_targets.end()) {
              dep_manifest_path = workspace_target->second.manifest_path;
              source_kind = "workspace";
              source_hint = workspace_target->second.package_root.string();
            } else {
              dep_manifest_path = registry_root / dep.alias / dep.version / "nebula.toml";
              source_kind = "registry";
              source_hint = dep.alias + "@" + dep.version;
            }
          }

          if (!fs::exists(dep_manifest_path)) {
            if (dep.kind == ManifestDependencyKind::Installed) {
              diags.push_back(make_cli_diag(
                  Severity::Error, "NBL-CLI-DEP-INSTALLED-NOTFOUND",
                  "installed backend SDK package not found: " + dep.installed, stage,
                  DiagnosticRisk::High,
                  "installed backend SDK root did not contain the requested package manifest",
                  "package graph is incomplete",
                  {"install Nebula with --with-backend-sdk on Linux",
                   "or replace the dependency with a path source from a repo checkout"}));
              state.visiting.erase(manifest_key);
              return std::nullopt;
            }
            diags.push_back(make_cli_diag(
                Severity::Error, "NBL-CLI-DEP-NOTFOUND",
                "dependency manifest not found: " + dep_manifest_path.string(), stage,
                DiagnosticRisk::High, "dependency source could not be resolved",
                "package graph is incomplete",
                {"run nebula fetch/update", "or fix dependency path/source"}));
            state.visiting.erase(manifest_key);
            return std::nullopt;
          }

          auto dep_name = visit_manifest(dep_manifest_path, false);
          if (!dep_name.has_value()) {
            state.visiting.erase(manifest_key);
            return std::nullopt;
          }

          auto dep_it = state.by_name.find(*dep_name);
          if (dep_it != state.by_name.end() && dep_it->second.locked.source_kind == "path") {
            dep_it->second.locked.source_kind = source_kind;
            dep_it->second.locked.source_hint = source_hint;
            if (source_kind == "git") {
              dep_it->second.locked.git_url = dep.git;
              dep_it->second.locked.git_requested_rev = dep.rev;
              dep_it->second.locked.resolved_rev = git_head_revision(dep_it->second.locked.root).value_or("");
            }
            dep_it->second.locked.source_fingerprint =
                package_lock_source_fingerprint(dep_it->second.locked, dep_it->second.manifest);
            dep_it->second.locked.integrity_fingerprint =
                package_integrity_fingerprint(dep_it->second.locked, dep_it->second.manifest);
          }
          node.locked.dependencies.push_back({dep.alias, *dep_name});
        }

        std::sort(node.locked.dependencies.begin(), node.locked.dependencies.end(),
                  [](const LockedPackageDependency& lhs, const LockedPackageDependency& rhs) {
                    if (lhs.alias != rhs.alias) return lhs.alias < rhs.alias;
                    return lhs.package < rhs.package;
                  });

        if (node.locked.source_kind == "git" && node.locked.resolved_rev.empty()) {
          node.locked.resolved_rev = git_head_revision(node.locked.root).value_or("");
        }
        node.locked.source_fingerprint = package_lock_source_fingerprint(node.locked, node.manifest);
        node.locked.integrity_fingerprint = package_integrity_fingerprint(node.locked, node.manifest);

        state.manifest_to_name[manifest_key] = manifest.name;
        state.by_name[manifest.name] = std::move(node);
        state.visiting.erase(manifest_key);
        return manifest.name;
      };

  std::optional<std::string> root_name;
  if (!workspace_container) {
    root_name = visit_manifest(manifest_path, true);
    if (!root_name.has_value()) return false;
  }

  for (const auto& member_manifest_path : workspace_member_manifests) {
    if (!visit_manifest(member_manifest_path, false).has_value()) return false;
  }

  if (workspace_container) {
    LockedPackageWithManifest workspace_node;
    workspace_node.locked.is_root = true;
    workspace_node.locked.name = workspace_package_name(manifest_path);
    workspace_node.locked.version = root_manifest.version;
    workspace_node.locked.root = manifest_path.parent_path();
    workspace_node.locked.manifest_path = manifest_path;
    workspace_node.locked.source_kind = "workspace";
    workspace_node.locked.manifest_hash = manifest_fingerprint(root_manifest);
    workspace_node.manifest = root_manifest;

    std::vector<std::string> member_names;
    for (const auto& member_manifest_path : workspace_member_manifests) {
      auto member_it = state.manifest_to_name.find(normalize_path_key(member_manifest_path));
      if (member_it == state.manifest_to_name.end()) continue;
      member_names.push_back(member_it->second);
    }
    std::sort(member_names.begin(), member_names.end());
    for (const auto& member_name : member_names) {
      workspace_node.locked.dependencies.push_back({member_name, member_name});
    }
    workspace_node.locked.source_fingerprint =
        package_lock_source_fingerprint(workspace_node.locked, workspace_node.manifest);
    workspace_node.locked.integrity_fingerprint =
        package_integrity_fingerprint(workspace_node.locked, workspace_node.manifest);

    root_name = workspace_node.locked.name;
    state.manifest_to_name[root_manifest_key] = workspace_node.locked.name;
    state.by_name[workspace_node.locked.name] = std::move(workspace_node);
  }

  if (!root_name.has_value()) return false;

  out.root_package = *root_name;
  out.root_manifest_hash = manifest_fingerprint(root_manifest);
  out.packages.reserve(state.by_name.size());
  for (auto& [_, node] : state.by_name) {
    out.packages.push_back(std::move(node.locked));
  }
  std::sort(out.packages.begin(), out.packages.end(), [](const LockedPackage& lhs, const LockedPackage& rhs) {
    if (lhs.is_root != rhs.is_root) return lhs.is_root > rhs.is_root;
    return lhs.name < rhs.name;
  });
  return true;
}

bool synthesize_root_lock(const fs::path& input,
                          ProjectLock& out,
                          std::vector<Diagnostic>& diags,
                          DiagnosticStage stage) {
  const auto manifest_path = resolve_root_manifest(input);
  if (manifest_path.has_value()) {
    return resolve_project_lock_from_manifest(*manifest_path, out, diags, stage, {}, false);
  }

  const fs::path abs_input = fs::absolute(input);
  const fs::path entry_path = fs::is_directory(abs_input) ? (abs_input / "main.nb") : abs_input;
  LockedPackage root;
  root.is_root = true;
  root.name = entry_path.stem().string();
  root.version = "0.1.0";
  root.root = entry_path.parent_path();
  root.entry = entry_path.filename();
  root.src_dir = ".";
  root.source_kind = "implicit";
  PackageManifest manifest;
  manifest.name = root.name;
  manifest.entry = root.entry;
  manifest.src_dir = root.src_dir;
  root.manifest_hash = manifest_fingerprint(manifest);
  root.source_fingerprint = package_lock_source_fingerprint(root, manifest);
  root.integrity_fingerprint = package_integrity_fingerprint(root, manifest);
  out = ProjectLock{};
  out.schema_version = kProjectLockSchemaVersion;
  out.toolchain_schema_version = kLockToolchainSchemaVersion;
  out.cache_schema_version = kLockCacheSchemaVersion;
  out.root_package = root.name;
  out.packages.push_back(std::move(root));
  return true;
}

} // namespace

bool preflight_git_dependency_environment(const ManifestDependency& dep,
                                          std::vector<Diagnostic>& diags,
                                          DiagnosticStage stage,
                                          bool require_remote_probe,
                                          std::string_view command_hint) {
  return preflight_git_dependency_environment_impl(
      dep, diags, stage, require_remote_probe, command_hint);
}

bool read_package_manifest(const fs::path& path,
                           PackageManifest& out,
                           std::vector<Diagnostic>& diags,
                           DiagnosticStage stage) {
  out = PackageManifest{};
  std::string text;
  if (!read_text_file(path, text)) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PROJ-IO",
        "cannot read manifest: " + path.string(), stage, DiagnosticRisk::High,
        "project manifest path is not readable", "project loading cannot continue"));
    return false;
  }

  std::string section;
  NativeSourceSpec* current_native_source = nullptr;
  NativeGeneratedHeaderConfig* current_generated_header = nullptr;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = strip_inline_comment(line);
    if (trimmed.empty()) continue;
    if (trimmed.size() >= 4 && trimmed[0] == '[' && trimmed[1] == '[' &&
        trimmed[trimmed.size() - 2] == ']' && trimmed.back() == ']') {
      section = trim(std::string_view(trimmed).substr(2, trimmed.size() - 4));
      current_native_source = nullptr;
      current_generated_header = nullptr;
      if (section == "native.sources") {
        out.has_explicit_package = true;
        out.native.sources.push_back(NativeSourceSpec{});
        current_native_source = &out.native.sources.back();
      } else if (section == "native.generated_headers") {
        out.has_explicit_package = true;
        out.native.generated_headers.push_back(NativeGeneratedHeaderConfig{});
        current_generated_header = &out.native.generated_headers.back();
      }
      continue;
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      section = trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
      current_native_source = nullptr;
      current_generated_header = nullptr;
      if (section == "package" || section == "project") out.has_explicit_package = true;
      if (section == "workspace") out.has_workspace_section = true;
      continue;
    }

    if (section.empty()) {
      if (auto schema_version = parse_manifest_int(trimmed, "schema_version"); schema_version.has_value()) {
        out.schema_version = *schema_version;
        continue;
      }
    }

    const bool in_package = section.empty() || section == "package" || section == "project";
    if (in_package) {
      if (section.empty()) out.legacy_flat = true;
      if (section == "project") out.legacy_project_table = true;
      if (auto name = parse_manifest_string(trimmed, "name"); name.has_value()) {
        out.has_explicit_package = true;
        out.name = *name;
        continue;
      }
      if (auto version = parse_manifest_string(trimmed, "version"); version.has_value()) {
        out.has_explicit_package = true;
        out.version = *version;
        continue;
      }
      if (auto entry = parse_manifest_string(trimmed, "entry"); entry.has_value()) {
        out.has_explicit_package = true;
        out.entry = *entry;
        continue;
      }
      if (auto src_dir = parse_manifest_string(trimmed, "src_dir"); src_dir.has_value()) {
        out.has_explicit_package = true;
        out.src_dir = *src_dir;
        continue;
      }
      if (auto host_cxx = parse_manifest_string_list(trimmed, "host_cxx"); host_cxx.has_value()) {
        out.has_explicit_package = true;
        out.host_cxx.clear();
        for (const auto& rel : *host_cxx) out.host_cxx.push_back(rel);
        continue;
      }
    }

    if (section == "workspace") {
      out.has_workspace_section = true;
      if (auto members = parse_manifest_string_list(trimmed, "members"); members.has_value()) {
        out.workspace_members.clear();
        for (const auto& rel : *members) out.workspace_members.push_back(rel);
        continue;
      }
    }

    if (section == "native") {
      out.has_explicit_package = true;
      if (auto c_sources = parse_manifest_string_list(trimmed, "c_sources"); c_sources.has_value()) {
        out.native.c_sources.clear();
        for (const auto& rel : *c_sources) out.native.c_sources.push_back(rel);
        continue;
      }
      if (auto cxx_sources = parse_manifest_string_list(trimmed, "cxx_sources");
          cxx_sources.has_value()) {
        out.native.cxx_sources.clear();
        for (const auto& rel : *cxx_sources) out.native.cxx_sources.push_back(rel);
        continue;
      }
      if (auto include_dirs = parse_manifest_string_list(trimmed, "include_dirs");
          include_dirs.has_value()) {
        out.native.include_dirs.clear();
        for (const auto& rel : *include_dirs) out.native.include_dirs.push_back(rel);
        continue;
      }
      if (auto defines = parse_manifest_string_list(trimmed, "defines"); defines.has_value()) {
        out.native.defines = *defines;
        continue;
      }
    }

    if (section == "native.sources" && current_native_source != nullptr) {
      out.has_explicit_package = true;
      if (auto path_value = parse_manifest_string(trimmed, "path"); path_value.has_value()) {
        current_native_source->path = *path_value;
        continue;
      }
      if (auto language = parse_manifest_string(trimmed, "language"); language.has_value()) {
        const auto parsed = parse_native_language(*language);
        if (!parsed.has_value()) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-NATIVE",
              "unsupported native source language: " + *language, stage, DiagnosticRisk::High,
              "native source descriptors only support c, cxx, or asm",
              "package-native source selection cannot continue"));
          return false;
        }
        current_native_source->language = *parsed;
        continue;
      }
      if (auto include_dirs = parse_manifest_string_list(trimmed, "include_dirs");
          include_dirs.has_value()) {
        current_native_source->include_dirs.clear();
        for (const auto& rel : *include_dirs) current_native_source->include_dirs.push_back(rel);
        continue;
      }
      if (auto defines = parse_manifest_string_list(trimmed, "defines"); defines.has_value()) {
        current_native_source->defines = *defines;
        continue;
      }
      if (auto arch = parse_manifest_string_list(trimmed, "arch"); arch.has_value()) {
        current_native_source->arch = *arch;
        continue;
      }
      if (auto cpu_features = parse_manifest_string_list(trimmed, "cpu_features");
          cpu_features.has_value()) {
        current_native_source->cpu_features = *cpu_features;
        continue;
      }
    }

    if (section == "native.generated_headers" && current_generated_header != nullptr) {
      out.has_explicit_package = true;
      if (auto out_path = parse_manifest_string(trimmed, "out"); out_path.has_value()) {
        current_generated_header->out = *out_path;
        continue;
      }
      if (auto template_path = parse_manifest_string(trimmed, "template"); template_path.has_value()) {
        current_generated_header->template_path = *template_path;
        continue;
      }
      const std::string prefix = "values = ";
      if (trimmed.substr(0, prefix.size()) == prefix) {
        auto table = parse_inline_table(std::string_view(trimmed).substr(prefix.size()));
        if (!table.has_value()) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-NATIVE",
              "generated header values must be an inline table in manifest: " + path.string(), stage,
              DiagnosticRisk::High, "generated header descriptor had an invalid values table",
              "native generated-header materialization cannot continue"));
          return false;
        }
        current_generated_header->values.clear();
        for (const auto& [key, value] : *table) {
          current_generated_header->values.push_back({key, value});
        }
        continue;
      }
    }

    if (section == "dependencies") {
      const std::size_t eq = trimmed.find('=');
      if (eq == std::string::npos) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-MANIFEST-DEPS",
            "invalid dependency entry in manifest: " + path.string(), stage, DiagnosticRisk::High,
            "dependency declaration is missing '='", "dependency graph cannot be resolved"));
        return false;
      }
      ManifestDependency dep;
      out.has_explicit_package = true;
      dep.alias = trim(std::string_view(trimmed).substr(0, eq));
      const std::string raw_value = trim(std::string_view(trimmed).substr(eq + 1));
      if (dep.alias.empty()) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-MANIFEST-DEPS",
            "dependency alias is empty in manifest: " + path.string(), stage, DiagnosticRisk::High,
            "dependency declaration does not name a package alias",
            "dependency graph cannot be resolved"));
        return false;
      }

      if (raw_value.size() >= 2 && raw_value.front() == '"' && raw_value.back() == '"') {
        dep.kind = ManifestDependencyKind::Version;
        dep.version = raw_value.substr(1, raw_value.size() - 2);
      } else {
        auto table = parse_inline_table(raw_value);
        if (!table.has_value()) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-DEPS",
              "dependency entry must be a version string or inline table: " + dep.alias, stage,
              DiagnosticRisk::High, "dependency declaration shape is invalid",
              "dependency graph cannot be resolved"));
          return false;
        }
        if (auto it = table->find("path"); it != table->end()) {
          dep.kind = ManifestDependencyKind::Path;
          dep.path = it->second;
        } else if (auto it = table->find("installed"); it != table->end()) {
          dep.kind = ManifestDependencyKind::Installed;
          dep.installed = it->second;
        } else if (auto it = table->find("git"); it != table->end()) {
          dep.kind = ManifestDependencyKind::Git;
          dep.git = it->second;
          auto rev_it = table->find("rev");
          if (rev_it != table->end()) dep.rev = rev_it->second;
        } else {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-DEPS",
              "dependency inline table must declare path, installed, or git: " + dep.alias, stage,
              DiagnosticRisk::High, "dependency source kind was not provided",
              "dependency graph cannot be resolved"));
          return false;
        }
        if (dep.kind == ManifestDependencyKind::Installed && dep.installed.empty()) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MANIFEST-DEPS",
              "installed dependency name must be non-empty: " + dep.alias, stage,
              DiagnosticRisk::High, "installed dependency source omitted its package name",
              "dependency graph cannot be resolved"));
          return false;
        }
      }
      out.dependencies.push_back(std::move(dep));
    }
  }

  if (out.name.empty()) {
    out.name = path.parent_path().filename().string();
  }

  if (is_reserved_std_name(out.name)) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-MANIFEST-STD",
        "package name `std` is reserved in manifest: " + path.string(), stage,
        DiagnosticRisk::High, "Nebula reserves the std package name for bundled standard library modules",
        "package resolution would conflict with the compiler-shipped std package",
        {"rename the package to a non-reserved name"}));
    return false;
  }
  for (const auto& dep : out.dependencies) {
    if (!is_reserved_std_name(dep.alias)) continue;
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-MANIFEST-STD",
        "dependency alias `std` is reserved in manifest: " + path.string(), stage,
        DiagnosticRisk::High, "Nebula reserves the std import alias for bundled standard library modules",
        "dependency imports would become ambiguous",
        {"rename the dependency alias to a non-reserved name"}));
    return false;
  }

  if (out.schema_version != 1) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-MANIFEST-SCHEMA",
        "unsupported manifest schema_version in: " + path.string(), stage, DiagnosticRisk::High,
        "manifest schema_version does not match Nebula 1.0 contract",
        "project loading cannot continue",
        {"set schema_version = 1"}));
    return false;
  }

  for (const auto& source : out.native.sources) {
    if (source.path.empty()) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-MANIFEST-NATIVE",
          "native source descriptor missing path in manifest: " + path.string(), stage,
          DiagnosticRisk::High, "native source descriptor must declare a source path",
          "native source selection cannot continue"));
      return false;
    }
  }
  for (const auto& header : out.native.generated_headers) {
    if (header.out.empty() || header.template_path.empty()) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-MANIFEST-NATIVE",
          "generated header descriptor missing out/template in manifest: " + path.string(), stage,
          DiagnosticRisk::High, "generated header descriptors require both out and template",
          "native generated-header materialization cannot continue"));
      return false;
    }
  }

  return true;
}

fs::path lockfile_path_for_manifest(const fs::path& manifest_path) {
  return manifest_path.parent_path() / "nebula.lock";
}

bool read_project_lock(const fs::path& path,
                       ProjectLock& out,
                       std::vector<Diagnostic>& diags,
                       DiagnosticStage stage) {
  out = ProjectLock{};
  std::string text;
  if (!read_text_file(path, text)) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-LOCK-READ",
        "cannot read lockfile: " + path.string(), stage, DiagnosticRisk::High,
        "lockfile path is not readable", "dependency resolution cannot continue"));
    return false;
  }

  std::string section;
  LockedPackage* current = nullptr;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = strip_inline_comment(line);
    if (trimmed.empty()) continue;
    if (trimmed == "[[package]]") {
      out.packages.push_back(LockedPackage{});
      current = &out.packages.back();
      section = "package";
      continue;
    }
    if (trimmed.front() == '[' && trimmed.back() == ']') {
      section = trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
      current = nullptr;
      continue;
    }

    if (section.empty()) {
      if (auto schema_version = parse_manifest_int(trimmed, "schema_version"); schema_version.has_value()) {
        out.schema_version = *schema_version;
        continue;
      }
      if (auto root_package = parse_manifest_string(trimmed, "root_package"); root_package.has_value()) {
        out.root_package = *root_package;
        continue;
      }
      if (auto root_manifest_hash = parse_manifest_string(trimmed, "root_manifest_hash");
          root_manifest_hash.has_value()) {
        out.root_manifest_hash = *root_manifest_hash;
        continue;
      }
      if (auto toolchain_schema_version =
              parse_manifest_int(trimmed, "toolchain_schema_version");
          toolchain_schema_version.has_value()) {
        out.toolchain_schema_version = *toolchain_schema_version;
        continue;
      }
      if (auto cache_schema_version = parse_manifest_int(trimmed, "cache_schema_version");
          cache_schema_version.has_value()) {
        out.cache_schema_version = *cache_schema_version;
        continue;
      }
    }

    if (section == "package" && current != nullptr) {
      if (auto name = parse_manifest_string(trimmed, "name"); name.has_value()) {
        current->name = *name;
        continue;
      }
      if (auto version = parse_manifest_string(trimmed, "version"); version.has_value()) {
        current->version = *version;
        continue;
      }
      if (auto root = parse_manifest_string(trimmed, "root"); root.has_value()) {
        current->root = *root;
        continue;
      }
      if (auto manifest_path = parse_manifest_string(trimmed, "manifest"); manifest_path.has_value()) {
        current->manifest_path = *manifest_path;
        continue;
      }
      if (auto entry = parse_manifest_string(trimmed, "entry"); entry.has_value()) {
        current->entry = *entry;
        continue;
      }
      if (auto src_dir = parse_manifest_string(trimmed, "src_dir"); src_dir.has_value()) {
        current->src_dir = *src_dir;
        continue;
      }
      if (auto source_kind = parse_manifest_string(trimmed, "source_kind"); source_kind.has_value()) {
        current->source_kind = *source_kind;
        continue;
      }
      if (auto source_hint = parse_manifest_string(trimmed, "source_hint"); source_hint.has_value()) {
        current->source_hint = *source_hint;
        continue;
      }
      if (auto git_url = parse_manifest_string(trimmed, "git_url"); git_url.has_value()) {
        current->git_url = *git_url;
        continue;
      }
      if (auto git_requested_rev =
              parse_manifest_string(trimmed, "git_requested_rev");
          git_requested_rev.has_value()) {
        current->git_requested_rev = *git_requested_rev;
        continue;
      }
      if (auto resolved_rev = parse_manifest_string(trimmed, "resolved_rev"); resolved_rev.has_value()) {
        current->resolved_rev = *resolved_rev;
        continue;
      }
      if (auto manifest_hash = parse_manifest_string(trimmed, "manifest_hash"); manifest_hash.has_value()) {
        current->manifest_hash = *manifest_hash;
        continue;
      }
      if (auto source_fingerprint =
              parse_manifest_string(trimmed, "source_fingerprint");
          source_fingerprint.has_value()) {
        current->source_fingerprint = *source_fingerprint;
        continue;
      }
      if (auto integrity_fingerprint =
              parse_manifest_string(trimmed, "integrity_fingerprint");
          integrity_fingerprint.has_value()) {
        current->integrity_fingerprint = *integrity_fingerprint;
        continue;
      }
      if (auto is_root = parse_manifest_int(trimmed, "is_root"); is_root.has_value()) {
        current->is_root = (*is_root != 0);
        continue;
      }
      if (auto host_cxx = parse_manifest_string_list(trimmed, "host_cxx"); host_cxx.has_value()) {
        current->host_cxx_sources.clear();
        for (const auto& rel : *host_cxx) current->host_cxx_sources.push_back(rel);
        continue;
      }
      if (auto native_c = parse_manifest_string_list(trimmed, "native_c_sources"); native_c.has_value()) {
        current->native.c_sources.clear();
        for (const auto& rel : *native_c) current->native.c_sources.push_back(rel);
        continue;
      }
      if (auto native_cxx = parse_manifest_string_list(trimmed, "native_cxx_sources");
          native_cxx.has_value()) {
        current->native.cxx_sources.clear();
        for (const auto& rel : *native_cxx) current->native.cxx_sources.push_back(rel);
        continue;
      }
      if (auto native_include_dirs = parse_manifest_string_list(trimmed, "native_include_dirs");
          native_include_dirs.has_value()) {
        current->native.include_dirs.clear();
        for (const auto& rel : *native_include_dirs) current->native.include_dirs.push_back(rel);
        continue;
      }
      if (auto native_defines = parse_manifest_string_list(trimmed, "native_defines");
          native_defines.has_value()) {
        current->native.defines = *native_defines;
        continue;
      }
      if (auto deps = parse_manifest_string_list(trimmed, "dependencies"); deps.has_value()) {
        current->dependencies.clear();
        for (const auto& dep : *deps) {
          const std::size_t sep = dep.find('=');
          if (sep == std::string::npos) continue;
          current->dependencies.push_back({dep.substr(0, sep), dep.substr(sep + 1)});
        }
        continue;
      }
    }
  }

  if (out.root_package.empty()) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-LOCK-SCHEMA",
        "lockfile missing root_package: " + path.string(), stage, DiagnosticRisk::High,
        "lockfile schema is incomplete", "dependency resolution cannot continue"));
    return false;
  }
  if (out.schema_version != 1 && out.schema_version != kProjectLockSchemaVersion) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-LOCK-SCHEMA",
        "unsupported lockfile schema_version in: " + path.string(), stage, DiagnosticRisk::High,
        "lockfile schema_version is newer than this compiler understands",
        "dependency resolution cannot continue",
        {"re-run nebula fetch/update with a compatible compiler"}));
    return false;
  }

  for (auto& pkg : out.packages) {
    pkg.root = resolve_lock_path(path, pkg.root.generic_string());
    pkg.manifest_path = resolve_lock_path(path, pkg.manifest_path.generic_string());
    if (pkg.source_kind == "path" || pkg.source_kind == "workspace") {
      pkg.source_hint = resolve_lock_path(path, pkg.source_hint).generic_string();
    }
  }
  return true;
}

bool write_project_lock(const fs::path& path,
                        const ProjectLock& lock,
                        std::vector<Diagnostic>& diags,
                        DiagnosticStage stage) {
  std::ostringstream out;
  out << "schema_version = " << lock.schema_version << "\n";
  out << "root_package = " << quote_toml(lock.root_package) << "\n";
  out << "root_manifest_hash = " << quote_toml(lock.root_manifest_hash) << "\n";
  out << "toolchain_schema_version = " << lock.toolchain_schema_version << "\n";
  out << "cache_schema_version = " << lock.cache_schema_version << "\n";
  for (const auto& pkg : lock.packages) {
    out << "\n[[package]]\n";
    out << "name = " << quote_toml(pkg.name) << "\n";
    out << "version = " << quote_toml(pkg.version) << "\n";
    out << "root = " << quote_toml(lock_relative_path_string(path, pkg.root)) << "\n";
    out << "manifest = " << quote_toml(lock_relative_path_string(path, pkg.manifest_path)) << "\n";
    out << "entry = " << quote_toml(pkg.entry.generic_string()) << "\n";
    out << "src_dir = " << quote_toml(pkg.src_dir.generic_string()) << "\n";
    out << "source_kind = " << quote_toml(pkg.source_kind) << "\n";
    const std::string source_hint = (pkg.source_kind == "path" || pkg.source_kind == "workspace")
                                        ? lock_relative_path_string(path, pkg.source_hint)
                                        : pkg.source_hint;
    out << "source_hint = " << quote_toml(source_hint) << "\n";
    if (pkg.source_kind == "git") {
      out << "git_url = " << quote_toml(pkg.git_url) << "\n";
      out << "git_requested_rev = " << quote_toml(pkg.git_requested_rev) << "\n";
    }
    out << "resolved_rev = " << quote_toml(pkg.resolved_rev) << "\n";
    out << "manifest_hash = " << quote_toml(pkg.manifest_hash) << "\n";
    out << "source_fingerprint = " << quote_toml(pkg.source_fingerprint) << "\n";
    out << "integrity_fingerprint = " << quote_toml(pkg.integrity_fingerprint) << "\n";
    out << "is_root = " << (pkg.is_root ? 1 : 0) << "\n";
    if (!pkg.host_cxx_sources.empty()) {
      out << "host_cxx = " << render_toml_string_list(sorted_generic_paths(pkg.host_cxx_sources))
          << "\n";
    }
    if (!pkg.native.c_sources.empty()) {
      out << "native_c_sources = "
          << render_toml_string_list(sorted_generic_paths(pkg.native.c_sources)) << "\n";
    }
    if (!pkg.native.cxx_sources.empty()) {
      out << "native_cxx_sources = "
          << render_toml_string_list(sorted_generic_paths(pkg.native.cxx_sources)) << "\n";
    }
    if (!pkg.native.include_dirs.empty()) {
      out << "native_include_dirs = "
          << render_toml_string_list(sorted_generic_paths(pkg.native.include_dirs)) << "\n";
    }
    if (!pkg.native.defines.empty()) {
      std::vector<std::string> defines = pkg.native.defines;
      std::sort(defines.begin(), defines.end());
      out << "native_defines = " << render_toml_string_list(defines) << "\n";
    }
    out << "dependencies = [";
    for (std::size_t i = 0; i < pkg.dependencies.size(); ++i) {
      if (i) out << ", ";
      out << quote_toml(pkg.dependencies[i].alias + "=" + pkg.dependencies[i].package);
    }
    out << "]\n";
  }

  if (!write_text_file(path, out.str())) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-LOCK-WRITE",
        "failed to write lockfile: " + path.string(), stage, DiagnosticRisk::High,
        "lockfile path is not writable", "dependency resolution state could not be persisted"));
    return false;
  }
  return true;
}

bool resolve_project_lock(const fs::path& input,
                          ProjectLock& out,
                          std::vector<Diagnostic>& diags,
                          DiagnosticStage stage,
                          const fs::path& backend_sdk_root,
                          bool refresh_git_sources) {
  const auto manifest_path = resolve_root_manifest(input);
  if (!manifest_path.has_value()) return synthesize_root_lock(input, out, diags, stage);
  return resolve_project_lock_from_manifest(*manifest_path, out, diags, stage, backend_sdk_root,
                                            refresh_git_sources);
}

bool publish_package_to_local_registry(const fs::path& input,
                                       PublishPackageResult& out,
                                       std::vector<Diagnostic>& diags,
                                       DiagnosticStage stage,
                                       bool force) {
  out = PublishPackageResult{};

  const fs::path abs_input = fs::absolute(input);
  fs::path search_dir = abs_input;
  if (abs_input.filename() == "nebula.toml") {
    search_dir = abs_input.parent_path();
  } else if (!fs::is_directory(abs_input)) {
    search_dir = abs_input.parent_path();
  }

  const auto target_manifest_path = find_manifest_upwards(search_dir);
  if (!target_manifest_path.has_value()) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-MANIFEST",
        "publish target has no nebula.toml: " + abs_input.string(), stage, DiagnosticRisk::High,
        "package publish requires a package directory or manifest path",
        "registry publish cannot determine package identity"));
    return false;
  }

  const auto project_manifest_path = resolve_root_manifest(abs_input);
  const fs::path registry_manifest_path =
      project_manifest_path.has_value() ? *project_manifest_path : *target_manifest_path;

  PackageManifest source_manifest;
  if (!read_package_manifest(*target_manifest_path, source_manifest, diags, stage)) return false;
  if (!source_manifest.has_explicit_package) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-WORKSPACE",
        "publish target is not a package manifest: " + target_manifest_path->string(), stage,
        DiagnosticRisk::High, "workspace-only manifests cannot be published as packages",
        "registry publish needs an explicit [package] manifest",
        {"publish a workspace member directory instead"}));
    return false;
  }
  if (!source_manifest.workspace_members.empty() && manifest_is_workspace_container(source_manifest)) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-WORKSPACE",
        "publish target is a workspace root without a package: " + target_manifest_path->string(), stage,
        DiagnosticRisk::High, "workspace-only manifests cannot be published as packages",
        "registry publish needs an explicit [package] manifest",
        {"publish a workspace member directory instead"}));
    return false;
  }

  const fs::path package_root = target_manifest_path->parent_path();
  PackageManifest published_manifest = source_manifest;
  published_manifest.workspace_members.clear();
  published_manifest.has_workspace_section = false;
  published_manifest.has_explicit_package = true;
  for (auto& dep : published_manifest.dependencies) {
    if (dep.kind == ManifestDependencyKind::Installed) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-INSTALLED",
          "installed backend SDK dependency cannot be published: " + dep.alias, stage,
          DiagnosticRisk::High,
          "installed backend SDK dependencies are local install contracts, not reproducible publish sources",
          "published package would depend on host-local Nebula installation state",
          {"replace the dependency with an exact version or path source before publishing"}));
      return false;
    }
    if (dep.kind != ManifestDependencyKind::Path) continue;
    const fs::path dep_manifest_path = absolute_normalized(package_root / dep.path / "nebula.toml");
    PackageManifest dep_manifest;
    if (!read_package_manifest(dep_manifest_path, dep_manifest, diags, stage)) return false;
    if (!dep_manifest.has_explicit_package) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-DEPS",
          "path dependency cannot be published as an exact-version dependency: " + dep.alias, stage,
          DiagnosticRisk::High, "path dependency resolves to a workspace-only or invalid manifest",
          "published package would not be reproducible",
          {"publish the dependency as a package first", "or replace the dependency with an exact version"}));
      return false;
    }
    dep.kind = ManifestDependencyKind::Version;
    dep.version = dep_manifest.version;
    dep.path.clear();
    dep.git.clear();
    dep.rev.clear();
  }

  LockedPackage source_pkg;
  source_pkg.name = source_manifest.name;
  source_pkg.version = source_manifest.version;
  source_pkg.root = package_root;
  source_pkg.manifest_path = *target_manifest_path;
  source_pkg.entry = source_manifest.entry;
  source_pkg.src_dir = source_manifest.src_dir;
  source_pkg.host_cxx_sources = source_manifest.host_cxx;
  source_pkg.native = source_manifest.native;

  const fs::path registry_root = registry_root_for_manifest(registry_manifest_path);
  const fs::path published_root = registry_root / source_manifest.name / source_manifest.version;
  const fs::path staging_root = registry_root / ".publish-tmp" /
                                (source_manifest.name + "-" + source_manifest.version + "-" +
                                 stable_hash_string(target_manifest_path->string()));

  std::error_code ec;
  fs::remove_all(staging_root, ec);
  ec.clear();
  if (!fs::create_directories(staging_root, ec) && ec) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-IO",
        "failed to create publish staging directory: " + staging_root.string(), stage, DiagnosticRisk::High,
        "local registry staging directory is not writable", "package publish cannot continue"));
    return false;
  }

  auto cleanup_staging = [&]() {
    std::error_code cleanup_ec;
    fs::remove_all(staging_root, cleanup_ec);
  };

  const auto tracked_files = collect_package_source_files(source_pkg, source_manifest);
  for (const auto& file : tracked_files) {
    if (!path_is_within(file, package_root)) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-FILE",
          "published file escapes package root: " + file.string(), stage, DiagnosticRisk::High,
          "published package contents must stay within the package directory",
          "local registry artifact would not be self-contained",
          {"move the file under the package root", "or stop referencing it from host_cxx/src_dir"}));
      return false;
    }
    fs::path relative = fs::relative(file, package_root, ec);
    if (ec) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-FILE",
          "failed to map published file into package root: " + file.string(), stage, DiagnosticRisk::High,
          "published package contents could not be normalized",
          "local registry artifact would be incomplete"));
      return false;
    }
    const fs::path dest = staging_root / relative;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-IO",
          "failed to create published file directory: " + dest.parent_path().string(), stage,
          DiagnosticRisk::High, "local registry target directory is not writable",
          "package publish cannot continue"));
      return false;
    }
    fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-IO",
          "failed to copy published file: " + file.string(), stage, DiagnosticRisk::High,
          "source file could not be copied into the local registry",
          "published package would be incomplete"));
      return false;
    }
  }

  const fs::path staged_manifest_path = staging_root / "nebula.toml";
  if (!write_text_file(staged_manifest_path, render_manifest_text(published_manifest))) {
    cleanup_staging();
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-IO",
        "failed to write published manifest: " + staged_manifest_path.string(), stage,
        DiagnosticRisk::High, "local registry manifest path is not writable",
        "package publish cannot continue"));
    return false;
  }

  LockedPackage staged_pkg;
  staged_pkg.name = published_manifest.name;
  staged_pkg.version = published_manifest.version;
  staged_pkg.root = staging_root;
  staged_pkg.manifest_path = staged_manifest_path;
  staged_pkg.entry = published_manifest.entry;
  staged_pkg.src_dir = published_manifest.src_dir;
  staged_pkg.source_kind = "registry";
  staged_pkg.source_hint = published_manifest.name + "@" + published_manifest.version;
  staged_pkg.host_cxx_sources = published_manifest.host_cxx;
  staged_pkg.native = published_manifest.native;
  staged_pkg.manifest_hash = manifest_fingerprint(published_manifest);
  staged_pkg.source_fingerprint = package_lock_source_fingerprint(staged_pkg, published_manifest);

  const fs::path staged_metadata_path = staging_root / "nebula-package.toml";
  if (!write_text_file(staged_metadata_path,
                       render_publish_metadata(published_manifest, staged_pkg, staged_pkg.manifest_hash,
                                               staged_pkg.source_fingerprint))) {
    cleanup_staging();
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-IO",
        "failed to write published metadata: " + staged_metadata_path.string(), stage,
        DiagnosticRisk::High, "local registry metadata path is not writable",
        "package publish cannot continue"));
    return false;
  }
  staged_pkg.integrity_fingerprint = package_integrity_fingerprint(staged_pkg, published_manifest);

  if (fs::exists(published_root, ec) && !ec) {
    const std::string existing_tree = directory_tree_fingerprint(published_root);
    const std::string staged_tree = directory_tree_fingerprint(staging_root);
    if (!existing_tree.empty() && existing_tree == staged_tree) {
      cleanup_staging();
      out.registry_root = registry_root;
      out.package_root = package_root;
      out.manifest_path = *target_manifest_path;
      out.published_root = published_root;
      out.package_name = source_manifest.name;
      out.package_version = source_manifest.version;
      out.unchanged = true;
      return true;
    }
    if (!force) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-IMMUTABLE",
          "registry package already exists with different contents: " + published_root.string(), stage,
          DiagnosticRisk::High, "published package versions are immutable by default",
          "consumers would not get deterministic exact-version replay",
          {"bump package.version before publishing", "or rerun nebula publish --force"}));
      return false;
    }
    fs::remove_all(published_root, ec);
    if (ec) {
      cleanup_staging();
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-PUBLISH-IO",
          "failed to replace published package: " + published_root.string(), stage, DiagnosticRisk::High,
          "existing local registry package could not be removed",
          "forced publish cannot continue"));
      return false;
    }
    out.replaced = true;
  }

  fs::create_directories(published_root.parent_path(), ec);
  if (ec) {
    cleanup_staging();
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-IO",
        "failed to create registry package directory: " + published_root.parent_path().string(), stage,
        DiagnosticRisk::High, "local registry package directory is not writable",
        "package publish cannot continue"));
    return false;
  }

  fs::rename(staging_root, published_root, ec);
  if (ec) {
    cleanup_staging();
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PUBLISH-IO",
        "failed to finalize local registry publish: " + published_root.string(), stage,
        DiagnosticRisk::High, "published package staging directory could not be committed",
        "package publish did not complete"));
    return false;
  }

  out.registry_root = registry_root;
  out.package_root = package_root;
  out.manifest_path = *target_manifest_path;
  out.published_root = published_root;
  out.package_name = source_manifest.name;
  out.package_version = source_manifest.version;
  return true;
}

bool load_project_input(const fs::path& input,
                        LoadedProjectInput& out,
                        std::vector<Diagnostic>& diags,
                        DiagnosticStage stage,
                        const LoadCompileOptions& options) {
  out = LoadedProjectInput{};
  const bool system_profile = is_system_profile(options.runtime_profile);
  const bool no_std = effective_no_std(options.runtime_profile, options.no_std);
  const fs::path& std_root = options.std_root;
  const fs::path& backend_sdk_root = options.backend_sdk_root;
  const fs::path abs_input = fs::absolute(input);

  ProjectLock lock;
  PackageManifest root_manifest;
  fs::path root_manifest_path;
  if (auto manifest_path = resolve_root_manifest(abs_input); manifest_path.has_value()) {
    root_manifest_path = *manifest_path;
    if (!read_package_manifest(root_manifest_path, root_manifest, diags, stage)) return false;

    const fs::path lock_path = lockfile_path_for_manifest(root_manifest_path);
    const bool requires_lock = !root_manifest.dependencies.empty() || !root_manifest.workspace_members.empty();
    if (requires_lock) {
      if (!fs::exists(lock_path)) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-LOCK-MISSING",
            "dependency lockfile missing: " + lock_path.string(), stage, DiagnosticRisk::High,
            "manifest declares dependencies or workspace members but no lockfile is present",
            "package graph cannot be reproduced",
            {"run nebula fetch to generate nebula.lock"}));
        return false;
      }
      if (!read_project_lock(lock_path, lock, diags, stage)) return false;
      if (!restore_missing_locked_git_checkouts(lock, diags, stage)) return false;
      if (lockfile_is_stale(lock_path, lock, root_manifest_path)) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-LOCK-STALE",
            "dependency lockfile is stale: " + lock_path.string(), stage, DiagnosticRisk::High,
            "manifest, workspace member set, or dependency manifests changed after lockfile generation",
            "package graph may no longer match source state",
            {"run nebula update to refresh nebula.lock"}));
        return false;
      }
    } else {
      if (!resolve_project_lock_from_manifest(root_manifest_path, lock, diags, stage,
                                              backend_sdk_root, false)) {
        return false;
      }
    }
  } else {
    if (!synthesize_root_lock(abs_input, lock, diags, stage)) return false;
  }

  std::unordered_map<std::string, LockedPackage> package_by_name;
  std::unordered_map<std::string, std::string> package_by_manifest;
  std::unordered_map<std::string, PackageManifest> package_manifest_by_name;
  for (const auto& pkg : lock.packages) {
    package_by_name.insert({pkg.name, pkg});
    if (!pkg.manifest_path.empty()) {
      package_by_manifest.insert({normalize_path_key(pkg.manifest_path), pkg.name});
      PackageManifest manifest;
      if (!read_package_manifest(pkg.manifest_path, manifest, diags, stage)) return false;
      package_manifest_by_name.insert({pkg.name, std::move(manifest)});
    }
  }
  auto root_it = package_by_name.find(lock.root_package);
  if (root_it == package_by_name.end()) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-LOCK-ROOT",
        "root package missing from lockfile: " + lock.root_package, stage, DiagnosticRisk::High,
        "lockfile root_package does not match a package entry",
        "package graph cannot be loaded"));
    return false;
  }

  const bool workspace_container = manifest_is_workspace_container(root_manifest);
  const LockedPackage* target_pkg = &root_it->second;
  if (!root_manifest_path.empty() && !root_manifest.workspace_members.empty()) {
    auto target_manifest = select_workspace_member_manifest(root_manifest_path, root_manifest, abs_input);
    if (!target_manifest.has_value() && workspace_container) {
      target_manifest = default_workspace_member_manifest(root_manifest_path, root_manifest);
    }
    if (target_manifest.has_value()) {
      const auto pkg_name_it = package_by_manifest.find(normalize_path_key(*target_manifest));
      if (pkg_name_it == package_by_manifest.end()) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-WORKSPACE-LOCK",
            "workspace member missing from lockfile: " + target_manifest->string(), stage,
            DiagnosticRisk::High, "workspace lockfile did not capture the targeted member package",
            "project loading cannot continue",
            {"run nebula update to regenerate nebula.lock"}));
        return false;
      }
      target_pkg = &package_by_name.at(pkg_name_it->second);
    } else if (workspace_container) {
      diags.push_back(make_cli_diag(
          Severity::Error, "NBL-CLI-WORKSPACE-TARGET",
          "workspace root does not identify a runnable package target: " + root_manifest_path.string(), stage,
          DiagnosticRisk::High, "workspace member list did not resolve to a concrete package manifest",
          "compilation cannot choose an entry package",
          {"add a valid first workspace member entry", "or run nebula on an explicit workspace member path"}));
      return false;
    }
  }

  const fs::path target_entry = target_pkg->root / target_pkg->entry;
  if (!fs::exists(target_entry)) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-PROJ-ENTRY",
        "entry source not found: " + target_entry.string(), stage, DiagnosticRisk::High,
        "project entry path does not exist", "compilation cannot start"));
    return false;
  }

  std::unordered_set<std::string> reachable_packages;
  std::function<void(const std::string&)> mark_reachable = [&](const std::string& pkg_name) {
    if (!reachable_packages.insert(pkg_name).second) return;
    auto pkg_it = package_by_name.find(pkg_name);
    if (pkg_it == package_by_name.end()) return;
    for (const auto& dep : pkg_it->second.dependencies) {
      mark_reachable(dep.package);
    }
  };
  mark_reachable(target_pkg->name);

  std::vector<std::string> ordered_reachable_packages(reachable_packages.begin(), reachable_packages.end());
  std::sort(ordered_reachable_packages.begin(), ordered_reachable_packages.end());

  out.project_root = target_pkg->root;
  out.manifest_path = target_pkg->manifest_path.empty() ? root_manifest_path : target_pkg->manifest_path;
  out.entry_path = target_entry;
  out.project_name = target_pkg->name;
  out.host_cxx_sources.clear();
  out.native_inputs = NativeBuildInputs{};
  std::set<std::string> host_seen;
  auto require_regular_file = [&](const fs::path& candidate,
                                  std::string_view code,
                                  std::string_view label,
                                  bool must_stay_within_root,
                                  const LockedPackage& pkg) -> std::optional<fs::path> {
    const fs::path normalized = absolute_normalized(candidate);
    if (must_stay_within_root && !path_is_within(normalized, pkg.root)) {
      diags.push_back(make_cli_diag(
          Severity::Error, std::string(code),
          std::string(label) + " escapes package root: " + normalized.string(), stage,
          DiagnosticRisk::High, "package-native inputs must stay within the package root",
          "build would no longer be reproducible from the package checkout alone",
          {"move the file under the package root", "or remove it from the package manifest"}));
      return std::nullopt;
    }
    std::error_code ec;
    if (fs::exists(normalized, ec) && !ec && fs::is_regular_file(normalized, ec) && !ec) {
      return normalized;
    }
    diags.push_back(make_cli_diag(
        Severity::Error, std::string(code),
        std::string(label) + " not found: " + normalized.string(), stage, DiagnosticRisk::High,
        "package manifest references a missing native or host source file",
        "build cannot reproduce the package-native bridge for " + pkg.name,
        {"restore the missing file", "or remove it from the package manifest"}));
    return std::nullopt;
  };
  auto require_directory = [&](const fs::path& candidate,
                               std::string_view code,
                               std::string_view label,
                               bool must_stay_within_root,
                               const LockedPackage& pkg) -> std::optional<fs::path> {
    const fs::path normalized = absolute_normalized(candidate);
    if (must_stay_within_root && !path_is_within(normalized, pkg.root)) {
      diags.push_back(make_cli_diag(
          Severity::Error, std::string(code),
          std::string(label) + " escapes package root: " + normalized.string(), stage,
          DiagnosticRisk::High, "package-native include dirs must stay within the package root",
          "build would no longer be reproducible from the package checkout alone",
          {"move the include dir under the package root", "or remove it from the package manifest"}));
      return std::nullopt;
    }
    std::error_code ec;
    if (fs::exists(normalized, ec) && !ec && fs::is_directory(normalized, ec) && !ec) {
      return normalized;
    }
    diags.push_back(make_cli_diag(
        Severity::Error, std::string(code),
        std::string(label) + " not found: " + normalized.string(), stage, DiagnosticRisk::High,
        "package manifest references a missing native include directory",
        "build cannot compile package-native sources for " + pkg.name,
        {"restore the missing include directory", "or remove it from the package manifest"}));
    return std::nullopt;
  };
  for (const auto& pkg_name : ordered_reachable_packages) {
    const auto pkg_it = package_by_name.find(pkg_name);
    if (pkg_it == package_by_name.end()) continue;
    const auto& pkg = pkg_it->second;
    const auto manifest_it = package_manifest_by_name.find(pkg.name);
    const PackageManifest* pkg_manifest = manifest_it == package_manifest_by_name.end()
                                              ? nullptr
                                              : &manifest_it->second;
    const auto& host_cxx_sources =
        pkg_manifest != nullptr ? pkg_manifest->host_cxx : pkg_it->second.host_cxx_sources;
    const NativeBuildConfig& native_cfg =
        pkg_manifest != nullptr ? pkg_manifest->native : pkg.native;
    for (const auto& rel : host_cxx_sources) {
      const auto host_path = require_regular_file(pkg.root / rel, "NBL-CLI-HOSTCXX-MISSING",
                                                  "host_cxx source", true, pkg);
      if (!host_path.has_value()) return false;
      if (!host_seen.insert(host_path->string()).second) continue;
      out.host_cxx_sources.push_back(*host_path);
    }
    std::vector<fs::path> package_include_dirs;
    for (const auto& rel : native_cfg.include_dirs) {
      const auto include_dir = require_directory(pkg.root / rel, "NBL-CLI-NATIVE-INCLUDE",
                                                 "native include dir", true, pkg);
      if (!include_dir.has_value()) return false;
      package_include_dirs.push_back(*include_dir);
    }
    std::sort(package_include_dirs.begin(), package_include_dirs.end(),
              [](const fs::path& lhs, const fs::path& rhs) {
                return lhs.generic_string() < rhs.generic_string();
              });
    package_include_dirs.erase(
        std::unique(package_include_dirs.begin(), package_include_dirs.end(),
                    [](const fs::path& lhs, const fs::path& rhs) { return lhs == rhs; }),
        package_include_dirs.end());

    std::vector<std::string> package_defines = native_cfg.defines;
    std::sort(package_defines.begin(), package_defines.end());
    package_defines.erase(std::unique(package_defines.begin(), package_defines.end()),
                          package_defines.end());

    std::vector<fs::path> package_generated_include_dirs;
    auto materialize_generated_header = [&](const NativeGeneratedHeaderConfig& header) -> std::optional<fs::path> {
      const auto template_path = require_regular_file(pkg.root / header.template_path,
                                                      "NBL-CLI-NATIVE-TEMPLATE",
                                                      "native generated-header template", true, pkg);
      if (!template_path.has_value()) return std::nullopt;
      const fs::path generated_root =
          lockfile_cache_root(out.manifest_path) / "native-generated" /
          (pkg.name + "-" + stable_hash_string(pkg.manifest_hash.empty() ? pkg.name : pkg.manifest_hash));
      const fs::path output_path = absolute_normalized(generated_root / header.out);
      if (!path_is_within(output_path, generated_root)) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-NATIVE-GENERATED",
            "generated header output escapes generated root: " + output_path.string(), stage,
            DiagnosticRisk::High, "generated header out path must stay within Nebula's generated include root",
            "native generated-header materialization cannot continue",
            {"use a relative out path inside the generated include tree"}));
        return std::nullopt;
      }
      std::string rendered;
      if (!read_text_file(*template_path, rendered)) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-NATIVE-TEMPLATE",
            "cannot read native generated-header template: " + template_path->string(), stage,
            DiagnosticRisk::High, "generated header template path is not readable",
            "native generated-header materialization cannot continue"));
        return std::nullopt;
      }
      for (const auto& [key, value] : header.values) {
        const std::string needle = "@" + key + "@";
        std::size_t pos = 0;
        while ((pos = rendered.find(needle, pos)) != std::string::npos) {
          rendered.replace(pos, needle.size(), value);
          pos += value.size();
        }
      }
      if (!write_text_file(output_path, rendered)) {
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-NATIVE-GENERATED",
            "failed to write generated native header: " + output_path.string(), stage,
            DiagnosticRisk::High, "generated include output path is not writable",
            "native generated-header materialization cannot continue"));
        return std::nullopt;
      }
      return generated_root;
    };
    for (const auto& header : native_cfg.generated_headers) {
      const auto generated_root = materialize_generated_header(header);
      if (!generated_root.has_value()) return false;
      package_generated_include_dirs.push_back(*generated_root);
    }
    std::sort(package_generated_include_dirs.begin(), package_generated_include_dirs.end(),
              [](const fs::path& lhs, const fs::path& rhs) {
                return lhs.generic_string() < rhs.generic_string();
              });
    package_generated_include_dirs.erase(
        std::unique(package_generated_include_dirs.begin(), package_generated_include_dirs.end(),
                    [](const fs::path& lhs, const fs::path& rhs) { return lhs == rhs; }),
        package_generated_include_dirs.end());

    auto append_native_source = [&](const fs::path& rel,
                                    NativeSourceLanguage language,
                                    std::string_view label,
                                    std::vector<fs::path> include_dirs,
                                    std::vector<std::string> defines,
                                    std::vector<std::string> extra_flags) -> bool {
      const auto native_path =
          require_regular_file(pkg.root / rel, "NBL-CLI-NATIVE-FILE", label, true, pkg);
      if (!native_path.has_value()) return false;
      NativeSourceInput input;
      input.path = *native_path;
      input.language = language;
      input.include_dirs = std::move(include_dirs);
      input.defines = std::move(defines);
      input.extra_flags = std::move(extra_flags);
      input.package_name = pkg.name;
      out.native_inputs.sources.push_back(std::move(input));
      return true;
    };

    auto merged_include_dirs = package_include_dirs;
    merged_include_dirs.insert(merged_include_dirs.end(), package_generated_include_dirs.begin(),
                               package_generated_include_dirs.end());
    std::sort(merged_include_dirs.begin(), merged_include_dirs.end(),
              [](const fs::path& lhs, const fs::path& rhs) {
                return lhs.generic_string() < rhs.generic_string();
              });
    merged_include_dirs.erase(
        std::unique(merged_include_dirs.begin(), merged_include_dirs.end(),
                    [](const fs::path& lhs, const fs::path& rhs) { return lhs == rhs; }),
        merged_include_dirs.end());

    for (const auto& rel : native_cfg.c_sources) {
      if (!append_native_source(rel, NativeSourceLanguage::C, "native C source", merged_include_dirs,
                                package_defines, {})) {
        return false;
      }
    }
    for (const auto& rel : native_cfg.cxx_sources) {
      if (!append_native_source(rel, NativeSourceLanguage::Cxx, "native C++ source",
                                merged_include_dirs, package_defines, {})) {
        return false;
      }
    }

    for (const auto& source : native_cfg.sources) {
      std::vector<fs::path> source_include_dirs = merged_include_dirs;
      for (const auto& rel : source.include_dirs) {
        const auto include_dir = require_directory(pkg.root / rel, "NBL-CLI-NATIVE-INCLUDE",
                                                   "native source include dir", true, pkg);
        if (!include_dir.has_value()) return false;
        source_include_dirs.push_back(*include_dir);
      }
      std::sort(source_include_dirs.begin(), source_include_dirs.end(),
                [](const fs::path& lhs, const fs::path& rhs) {
                  return lhs.generic_string() < rhs.generic_string();
                });
      source_include_dirs.erase(
          std::unique(source_include_dirs.begin(), source_include_dirs.end(),
                      [](const fs::path& lhs, const fs::path& rhs) { return lhs == rhs; }),
          source_include_dirs.end());

      std::vector<std::string> source_defines = package_defines;
      source_defines.insert(source_defines.end(), source.defines.begin(), source.defines.end());
      std::sort(source_defines.begin(), source_defines.end());
      source_defines.erase(std::unique(source_defines.begin(), source_defines.end()),
                           source_defines.end());

      auto host_arch_matches = [&](const std::vector<std::string>& wanted) {
        if (wanted.empty()) return true;
#if defined(__x86_64__) || defined(_M_X64)
        static const std::vector<std::string> host_arch = {"x86_64", "amd64"};
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
        static const std::vector<std::string> host_arch = {"arm64", "aarch64"};
#else
        static const std::vector<std::string> host_arch = {};
#endif
        for (const auto& item : wanted) {
          if (std::find(host_arch.begin(), host_arch.end(), item) != host_arch.end()) return true;
        }
        return false;
      };
      if (!host_arch_matches(source.arch)) continue;

      std::vector<std::string> extra_flags;
      [[maybe_unused]] auto add_flag = [&](std::string flag) {
        if (std::find(extra_flags.begin(), extra_flags.end(), flag) == extra_flags.end()) {
          extra_flags.push_back(std::move(flag));
        }
      };
      for (const auto& feature : source.cpu_features) {
#if defined(__x86_64__) || defined(_M_X64)
        if (feature == "sse2") {
          add_flag("-msse2");
        } else if (feature == "sse41") {
          add_flag("-msse4.1");
        } else if (feature == "avx2") {
          add_flag("-mavx2");
        } else if (feature == "avx512f") {
          add_flag("-mavx512f");
        } else if (feature == "avx512vl") {
          add_flag("-mavx512vl");
        } else if (feature == "bmi2") {
          add_flag("-mbmi2");
        } else if (feature == "popcnt") {
          add_flag("-mpopcnt");
        } else if (feature == "aes") {
          add_flag("-maes");
        } else if (feature == "sha") {
          add_flag("-msha");
        } else {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-NATIVE-FEATURE",
              "unsupported x86_64 native cpu_features entry: " + feature, stage,
              DiagnosticRisk::High, "Nebula does not know how to translate that native cpu feature yet",
              "source-local native compilation cannot continue"));
          return false;
        }
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
        if (!(feature == "neon" || feature == "crypto" || feature == "aes" || feature == "sha2" ||
              feature == "sha3" || feature == "sha512")) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-NATIVE-FEATURE",
              "unsupported arm64 native cpu_features entry: " + feature, stage,
              DiagnosticRisk::High, "Nebula does not know how to translate that native cpu feature yet",
              "source-local native compilation cannot continue"));
          return false;
        }
#else
        diags.push_back(make_cli_diag(
            Severity::Error, "NBL-CLI-NATIVE-FEATURE",
            "native cpu_features are not supported on this host architecture yet", stage,
            DiagnosticRisk::High, "Nebula does not have a native cpu-feature mapping for this host architecture",
            "source-local native compilation cannot continue"));
        return false;
#endif
      }

      std::string label;
      switch (source.language) {
      case NativeSourceLanguage::C: label = "native descriptor C source"; break;
      case NativeSourceLanguage::Cxx: label = "native descriptor C++ source"; break;
      case NativeSourceLanguage::Asm: label = "native descriptor asm source"; break;
      }
      if (!append_native_source(source.path, source.language, label, source_include_dirs,
                                source_defines, extra_flags)) {
        return false;
      }
    }
  }

  if ((system_profile || no_std) && (!out.host_cxx_sources.empty() || !out.native_inputs.empty())) {
    diags.push_back(make_cli_diag(
        Severity::Error, "NBL-CLI-SYSTEM-NATIVE",
        "system/no-std profile does not support host_cxx or [native] sources yet", stage,
        DiagnosticRisk::High,
        "package-native and host_cxx bridges compile against the hosted C/C++ toolchain profile",
        "system-profile builds would silently inherit host runtime dependencies",
        {"remove host_cxx/[native] from the reachable package graph",
         "or build with --profile hosted while using host bridge code"}));
    return false;
  }

  std::unordered_map<std::string, std::string> module_to_path;
  std::unordered_set<std::string> visited_modules;
  std::unordered_set<std::string> visiting_modules;

  std::function<bool(const LockedPackage&, const fs::path&)> visit_file =
      [&](const LockedPackage& pkg, const fs::path& file) -> bool {
        const std::string file_key = pkg.name + "::" + normalize_path_key(file);
        if (visited_modules.count(file_key)) return true;
        if (!visiting_modules.insert(file_key).second) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MOD-CYCLE",
              "import cycle detected at: " + file.string(), stage, DiagnosticRisk::High,
              "module dependency graph contains a cycle",
              "project load order is undefined until the cycle is removed"));
          return false;
        }

        SourceFile source;
        source.path = file.string();
        if (!read_text_file(file, source.text)) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-IO001",
              "cannot open file: " + file.string(), stage, DiagnosticRisk::High,
              "source file path is not readable", "compilation cannot start"));
          visiting_modules.erase(file_key);
          return false;
        }

        Program program;
        if (!parse_program_headers(source, program, diags, stage)) {
          visiting_modules.erase(file_key);
          return false;
        }

        const std::string module_name = program.module_name.value_or(module_name_from_path(pkg, file));
        const std::string qualified_module = pkg.name + "::" + module_name;
        auto existing = module_to_path.find(qualified_module);
        if (existing != module_to_path.end() && existing->second != normalize_path_key(file)) {
          diags.push_back(make_cli_diag(
              Severity::Error, "NBL-CLI-MOD-DUP",
              "duplicate module name: " + qualified_module, stage, DiagnosticRisk::High,
              "multiple files resolve to the same package-qualified module name",
              "imports would become ambiguous"));
          visiting_modules.erase(file_key);
          return false;
        }
        module_to_path[qualified_module] = normalize_path_key(file);

        source.package_name = pkg.name;
        source.module_name = module_name;
        source.resolved_imports.clear();
        for (const auto& import_name : program.imports) {
          const auto dep_import = split_dependency_import(import_name);
          const LockedPackage* target_pkg = &pkg;
          std::string target_module = import_name;
          if (dep_import.has_value()) {
            if (dep_import->first == "std") {
              if (no_std || (system_profile && is_hosted_std_module(dep_import->second))) {
                diags.push_back(make_cli_diag(
                    Severity::Error, "NBL-CLI-SYSTEM-STD",
                    "std module is not available for this profile: std::" + dep_import->second,
                    stage, DiagnosticRisk::High,
                    "runtime profile `" + runtime_profile_name(options.runtime_profile) +
                        "` with target `" + options.target +
                        "` forbids hosted standard-library imports",
                    "system/no-std code would otherwise depend on host OS services",
                    {"remove the hosted std import",
                     "or build with --profile hosted when host OS APIs are intentional"}));
                visiting_modules.erase(file_key);
                return false;
              }
              const auto std_module_path = resolve_std_module_file(std_root, dep_import->second);
              if (!std_module_path.has_value()) {
                const std::string cause =
                    std_root.empty()
                        ? "nebula could not resolve its bundled std root for this binary"
                        : "the requested std module was not found under " + std_root.string();
                diags.push_back(make_cli_diag(
                    Severity::Error, "NBL-CLI-STD-MODULE",
                    "std module not found: std::" + dep_import->second, stage, DiagnosticRisk::High,
                    cause, "compilation cannot resolve the requested bundled std module",
                    {"reinstall Nebula so share/nebula/std is present",
                     "or use a build-tree/install-tree binary with bundled std assets"}));
                visiting_modules.erase(file_key);
                return false;
              }
              source.resolved_imports.push_back("std::" + dep_import->second);
              LockedPackage std_pkg;
              std_pkg.name = "std";
              std_pkg.root = std_root;
              std_pkg.src_dir = ".";
              if (!visit_file(std_pkg, *std_module_path)) {
                visiting_modules.erase(file_key);
                return false;
              }
              continue;
            }
            const auto dep_it =
                std::find_if(pkg.dependencies.begin(), pkg.dependencies.end(),
                             [&](const LockedPackageDependency& dep) { return dep.alias == dep_import->first; });
            if (dep_it == pkg.dependencies.end()) {
              diags.push_back(make_cli_diag(
                  Severity::Error, "NBL-CLI-PKG-IMPORT",
                  "package import alias not found: " + dep_import->first, stage, DiagnosticRisk::High,
                  "module import references an undeclared dependency alias",
                  "package graph cannot resolve the requested module"));
              visiting_modules.erase(file_key);
              return false;
            }
            auto pkg_it = package_by_name.find(dep_it->package);
            if (pkg_it == package_by_name.end()) {
              diags.push_back(make_cli_diag(
                  Severity::Error, "NBL-CLI-PKG-IMPORT",
                  "dependency package missing from lockfile: " + dep_it->package, stage,
                  DiagnosticRisk::High, "lockfile dependency entry is incomplete",
                  "package import cannot be resolved"));
              visiting_modules.erase(file_key);
              return false;
            }
            target_pkg = &pkg_it->second;
            target_module = dep_import->second;
          }

          auto import_path = resolve_module_file(*target_pkg, file.parent_path(), target_module);
          if (!import_path.has_value()) {
            diags.push_back(make_cli_diag(
                Severity::Error, "NBL-CLI-MOD-NOTFOUND",
                "import not found: " + import_name, stage, DiagnosticRisk::High,
                "module file could not be resolved from package source roots",
                "project loading cannot continue"));
            visiting_modules.erase(file_key);
            return false;
          }
          source.resolved_imports.push_back(target_pkg->name + "::" + target_module);
          if (!visit_file(*target_pkg, *import_path)) {
            visiting_modules.erase(file_key);
            return false;
          }
        }

        visiting_modules.erase(file_key);
        visited_modules.insert(file_key);
        out.sources.push_back(std::move(source));
        return true;
      };

  if (!visit_file(*target_pkg, target_entry)) return false;

  std::ostringstream cache_seed;
  cache_seed << "root=" << target_pkg->name << "\n";
  for (const auto& pkg_name : ordered_reachable_packages) {
    const auto pkg_it = package_by_name.find(pkg_name);
    if (pkg_it == package_by_name.end()) continue;
    const auto& pkg = pkg_it->second;
    cache_seed << "pkg " << pkg.name << " " << pkg.version << " " << pkg.source_kind << " "
               << pkg.source_hint << " " << pkg.integrity_fingerprint << "\n";
  }
  for (const auto& host : out.host_cxx_sources) cache_seed << "host_cxx=" << host.string() << "\n";
  for (const auto& input : out.native_inputs.sources) {
    cache_seed << "native.pkg=" << input.package_name << "\n";
    cache_seed << "native.lang="
               << (input.language == NativeSourceLanguage::C ? "c" : "cxx") << "\n";
    cache_seed << "native.path=" << input.path.string() << "\n";
    for (const auto& dir : input.include_dirs) {
      cache_seed << "native.include_dir=" << dir.string() << "\n";
    }
    for (const auto& define : input.defines) {
      cache_seed << "native.define=" << define << "\n";
    }
    for (const auto& flag : input.extra_flags) {
      cache_seed << "native.flag=" << flag << "\n";
    }
  }
  for (const auto& source : out.sources) {
    cache_seed << source.path << "\n";
    cache_seed << "pkg=" << source.package_name << "\n";
    cache_seed << "mod=" << source.module_name << "\n";
    for (const auto& import_name : source.resolved_imports) {
      cache_seed << "import=" << import_name << "\n";
    }
    cache_seed << source.text << "\n---\n";
  }
  out.cache_key_source = cache_seed.str();
  return true;
}

LoadedCompileInput load_compile_input(const fs::path& input,
                                      nebula::frontend::DiagnosticStage stage,
                                      const fs::path& std_root,
                                      const fs::path& backend_sdk_root) {
  LoadCompileOptions options;
  options.std_root = std_root;
  options.backend_sdk_root = backend_sdk_root;
  return load_compile_input(input, stage, options);
}

LoadedCompileInput load_compile_input(const fs::path& input,
                                      nebula::frontend::DiagnosticStage stage,
                                      const LoadCompileOptions& options) {
  LoadedCompileInput out;

  LoadedProjectInput loaded;
  if (!load_project_input(input, loaded, out.diags, stage, options)) {
    out.ok = false;
    return out;
  }

  for (const auto& source : loaded.sources) {
    out.compile_sources.push_back(source);
    out.source_files.push_back(source.path);
  }

  out.ok = true;
  out.manifest_path = loaded.manifest_path;
  out.project_root = loaded.project_root;
  out.module_root = fs::exists(loaded.project_root / "src") ? (loaded.project_root / "src")
                                                            : loaded.entry_path.parent_path();
  out.entry_file = loaded.entry_path;
  out.project_name = loaded.project_name;
  out.cache_key_source = loaded.cache_key_source;
  out.host_cxx_sources = loaded.host_cxx_sources;
  out.native_inputs = loaded.native_inputs;
  return out;
}
