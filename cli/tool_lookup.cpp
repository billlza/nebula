#include "tool_lookup.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::vector<std::string> executable_probe_candidates(std::string_view command) {
  std::vector<std::string> candidates;
  candidates.emplace_back(command);
#if defined(_WIN32)
  const std::filesystem::path base(command);
  if (base.extension().empty()) {
    for (const char *extension : {".exe", ".com"})
      candidates.push_back(base.string() + extension);
  }
#endif
  return candidates;
}

bool is_executable_candidate(const std::filesystem::path &candidate) {
#if defined(_WIN32)
  return ::_waccess(candidate.c_str(), 0) == 0;
#else
  return ::access(candidate.c_str(), X_OK) == 0;
#endif
}

} // namespace

std::optional<std::filesystem::path> find_executable_on_path(std::string_view command) {
  if (command.empty())
    return std::nullopt;
  for (const std::string &candidate_name : executable_probe_candidates(command)) {
    const std::filesystem::path candidate(candidate_name);
    if (candidate.has_parent_path()) {
      if (is_executable_candidate(candidate))
        return std::filesystem::absolute(candidate).lexically_normal();
      continue;
    }

    const char *path_environment = std::getenv("PATH");
    if (path_environment == nullptr || *path_environment == '\0')
      continue;
    std::stringstream stream(path_environment);
    std::string segment;
#if defined(_WIN32)
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    while (std::getline(stream, segment, kSeparator)) {
      if (segment.empty())
        continue;
      const std::filesystem::path entry = std::filesystem::path(segment) / candidate;
      if (is_executable_candidate(entry))
        return std::filesystem::absolute(entry).lexically_normal();
    }
  }
  return std::nullopt;
}
