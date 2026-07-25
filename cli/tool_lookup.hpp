#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

[[nodiscard]] std::optional<std::filesystem::path>
find_executable_on_path(std::string_view command);
