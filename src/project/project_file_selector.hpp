#pragma once

#include <filesystem>

namespace gdpp::project {

enum class PathDisposition {
    project_content,
    version_control,
    engine_cache,
    delivery_output,
    gdpp_plugin,
    compiler_output,
    platform_metadata,
    godot_ignored,
};

// Keeps project discovery and source-protection boundaries in one path-aware policy.  Names such
// as "addons" and "build" are valid inside game content; only known root-level/generated trees
// are excluded.
class ProjectFileSelector final {
  public:
    ProjectFileSelector(std::filesystem::path project_root, std::filesystem::path compiler_output);

    [[nodiscard]] PathDisposition
    classify(const std::filesystem::path& project_relative_path) const;
    [[nodiscard]] bool should_descend(const std::filesystem::path& project_relative_path) const;
    [[nodiscard]] bool should_compile(const std::filesystem::path& project_relative_path) const;

  private:
    [[nodiscard]] bool
    is_godot_ignored(const std::filesystem::path& project_relative_path) const;

    std::filesystem::path project_root_;
    std::filesystem::path output_relative_;
};

} // namespace gdpp::project
