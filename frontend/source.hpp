#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebula::frontend {

struct QualifiedName {
  std::string package_name;
  std::string module_name;
  std::string local_name;

  bool empty() const { return local_name.empty(); }

  std::string display_name() const {
    if (package_name.empty() && module_name.empty()) return local_name;
    if (package_name.empty()) return module_name + "::" + local_name;
    if (module_name.empty()) return package_name + "::" + local_name;
    return package_name + "::" + module_name + "::" + local_name;
  }
};

inline bool operator==(const QualifiedName& lhs, const QualifiedName& rhs) {
  return lhs.package_name == rhs.package_name && lhs.module_name == rhs.module_name &&
         lhs.local_name == rhs.local_name;
}

struct QualifiedNameHash {
  std::size_t operator()(const QualifiedName& name) const noexcept {
    const auto mix = [](std::size_t seed, const std::string& part) {
      const std::size_t value = std::hash<std::string>{}(part);
      return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    };

    std::size_t seed = 0;
    seed = mix(seed, name.package_name);
    seed = mix(seed, name.module_name);
    seed = mix(seed, name.local_name);
    return seed;
  }
};

struct SourcePos {
  std::size_t offset = 0;
  int line = 1;
  int col = 1;
};

struct Span {
  SourcePos start{};
  SourcePos end{};
  std::string source_path;

  Span() = default;
  Span(SourcePos s, SourcePos e, std::string path = {})
      : start(s), end(e), source_path(std::move(path)) {}
};

struct SourceFile {
  std::string path;
  std::string text;
  std::string package_name;
  std::string module_name;
  std::vector<std::string> resolved_imports;

  std::string_view view() const { return std::string_view{text}; }
};

} // namespace nebula::frontend
