#pragma once

#include <filesystem>
#include <string>

namespace gdpp {

[[nodiscard]] bool is_complete_xcframework(const std::filesystem::path& artifact);

[[nodiscard]] bool commit_xcframework_artifact(const std::filesystem::path& pending,
                                               const std::filesystem::path& destination,
                                               std::string& diagnostic);

} // namespace gdpp
