#pragma once

#include "gdpp/compiler/compiler.hpp"
#include "gdpp/core/diagnostic.hpp"
#include "gdpp/project/extension_bridge.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gdpp {

enum class ProjectCompilePhase {
    scan,
    parse,
    analyze,
    translate,
    generate,
};

using ProjectCompileProgressCallback =
    std::function<void(ProjectCompilePhase phase, std::size_t completed, std::size_t total)>;

struct ProjectCompileOptions {
    std::filesystem::path project_root;
    std::filesystem::path output_directory;
    std::filesystem::path native_library_directory;
    std::filesystem::path sdk_root;
    std::filesystem::path godot_cpp_directory;
    // Runtime contracts reflected from the live editor ClassDB. The Godot compiler service
    // populates these after every third-party GDExtension has registered its classes; the CLI
    // intentionally leaves the list empty and can still consume checked-in bridge manifests.
    std::vector<ExtensionBridge> reflected_extension_bridges;
    CompileOptions compiler;
    ProjectCompileProgressCallback progress_callback;
};

struct ProjectDiagnostic {
    std::filesystem::path path;
    Diagnostic diagnostic;
};

struct CompiledProjectScript {
    std::filesystem::path relative_path;
    std::string content_hash;
    std::string public_abi_hash;
    std::vector<std::string> dependencies;
    std::string class_name;
    std::string header_file_name;
    std::string source_file_name;
    std::string symbol_file_name;
    std::vector<std::string> inner_class_names;
    std::vector<std::string> abstract_inner_class_names;
    std::vector<std::string> editor_only_inner_class_names;
    std::optional<std::string> icon_path;
    std::string native_base_type;
    std::string external_base_name;
    // Exact ClassDB type instantiated as the owner of this compiled behavior. For ordinary
    // scripts this is the source-level Godot base; for scripts rooted in another GDExtension it
    // remains that provider-owned class rather than its nearest built-in ancestor.
    std::string attached_native_base;
    std::string global_name;
    std::string base_script_path;
    std::vector<ScriptMemberSymbol> reflection_members;
    bool is_abstract{false};
    bool is_tool{false};
    bool static_unload{false};
    bool is_attached{false};
    bool is_editor_only{false};
    bool cache_hit{false};
};

struct ProjectCompileResult {
    bool success{false};
    std::vector<CompiledProjectScript> scripts;
    std::vector<ProjectDiagnostic> diagnostics;
    std::size_t compiled_count{0};
    std::size_t cache_hit_count{0};
    std::size_t removed_count{0};
    std::filesystem::path native_library_directory;
    std::string build_id;
};

class ProjectCompiler final {
  public:
    [[nodiscard]] ProjectCompileResult compile(const ProjectCompileOptions& options) const;

  private:
    [[nodiscard]] ProjectCompileResult compile_impl(const ProjectCompileOptions& options) const;
};

} // namespace gdpp
