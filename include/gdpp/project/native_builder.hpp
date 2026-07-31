#pragma once

#include "gdpp/core/godot_version.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gdpp {

enum class NativePlatform { macos, linux, windows, android, ios, web };
enum class NativeBuildProfile { debug, release };
enum class NativeWebThreadMode { not_applicable, single_threaded, multi_threaded };
enum class NativePrecision { single, double_precision };

[[nodiscard]] const char* native_build_profile_name(NativeBuildProfile profile) noexcept;
[[nodiscard]] std::optional<NativeBuildProfile>
parse_native_build_profile(std::string_view value) noexcept;
[[nodiscard]] const char* native_precision_name(NativePrecision precision) noexcept;
[[nodiscard]] std::optional<NativePrecision>
parse_native_precision(std::string_view value) noexcept;
[[nodiscard]] bool native_architecture_supported(NativePlatform platform,
                                                 std::string_view architecture) noexcept;
[[nodiscard]] std::string
native_library_name(NativeBuildProfile profile, NativePlatform platform,
                    std::string_view architecture,
                    NativeWebThreadMode web_thread_mode = NativeWebThreadMode::not_applicable);

struct NativeBuildOptions {
    std::filesystem::path project_output_directory;
    std::filesystem::path binary_output_directory;
    std::filesystem::path sdk_root;
    std::filesystem::path build_executor;
    std::string compiler_executable;
    NativePlatform platform{NativePlatform::linux};
    std::string architecture{"x86_64"};
    NativeBuildProfile profile{NativeBuildProfile::release};
    NativeWebThreadMode web_thread_mode{NativeWebThreadMode::not_applicable};
    NativePrecision precision{NativePrecision::single};
    GodotVersion target_version{minimum_godot_version};
};

struct NativeBuildCommand {
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    // Logical stage metadata is retained for graph validation and progress accounting. Ninja owns
    // dependency scheduling and bounded parallel execution; these commands are never serialized
    // into the Godot-facing build plan.
    std::size_t stage{0};
};

struct NativeBuildPlan {
    bool success{false};
    // Complete logical edge descriptions used by native tests and graph construction. Customer
    // builds receive only the generated Ninja graph contract.
    std::vector<NativeBuildCommand> commands;
    std::vector<std::string> diagnostics;
    std::filesystem::path build_executor;
    std::filesystem::path build_directory;
    std::filesystem::path build_file;
    std::string build_target{"gdpp"};
    std::size_t compile_edge_count{0};
    std::size_t post_compile_edge_count{0};
    std::filesystem::path output_library;
    // Directory artifacts such as an Apple XCFramework are assembled away from the customer
    // output and committed only after the complete build succeeds.
    std::filesystem::path pending_output_library;
};

class NativeBuilder final {
  public:
    [[nodiscard]] NativeBuildPlan plan(const NativeBuildOptions& options) const;
};

} // namespace gdpp
