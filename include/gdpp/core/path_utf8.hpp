#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace gdpp {

// std::filesystem::path stores UTF-16 on Windows, but its narrow string()/generic_string()
// conversions use the active ANSI code page.  Godot project files and GDPP manifests are UTF-8,
// so every boundary between those formats and filesystem paths must be explicit.
inline std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::u8path(value.begin(), value.end());
}

inline std::string path_to_utf8(const std::filesystem::path& value) { return value.u8string(); }

inline std::string generic_path_to_utf8(const std::filesystem::path& value) {
    return value.generic_u8string();
}

} // namespace gdpp
