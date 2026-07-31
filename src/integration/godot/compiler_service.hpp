#pragma once

#include "gdpp/project/extension_bridge.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gdpp::extension {

class GDPPCompiler final : public godot::RefCounted {
    GDCLASS(GDPPCompiler, godot::RefCounted)

  public:
    [[nodiscard]] godot::Dictionary compile_source(const godot::String& source,
                                                   const godot::String& virtual_path,
                                                   const godot::String& target_version) const;
    [[nodiscard]] godot::Dictionary compile_file(const godot::String& source_path,
                                                 const godot::String& output_directory,
                                                 const godot::String& target_version) const;
    [[nodiscard]] godot::Dictionary
    compile_project(const godot::String& project_root, const godot::String& output_directory,
                    const godot::String& sdk_root, const godot::String& compiler_executable,
                    const godot::String& target_version, const godot::String& build_profile,
                    const godot::String& target_platform, const godot::String& target_architecture,
                    const godot::String& target_variant,
                    const godot::String& target_precision) const;
    [[nodiscard]] godot::Dictionary
    execute_project_build(const godot::Dictionary& build_plan) const;
    void prepare_project_build();
    [[nodiscard]] godot::Array drain_project_build_progress() const;
    // Installs reflection-only attached script descriptors into the editor bridge. The
    // distribution library owns the executable descriptors and is never loaded into the editor.
    [[nodiscard]] godot::Dictionary
    install_editor_script_descriptors(const godot::Array& descriptors) const;
    [[nodiscard]] bool
    set_editor_script_storage_state(godot::Object* object,
                                    const godot::PackedStringArray& stored_properties) const;
    void clear_editor_script_descriptors() const;
    // Runs the editor-owned resource transformer in a hidden Godot process with a physically
    // independent project snapshot. Customer GDExtension libraries, ResourceFormatLoader and
    // shader diagnostics stay inside that process while its structured result remains fail-closed.
    [[nodiscard]] godot::Dictionary
    run_export_transform_worker(const godot::String& state_path,
                                const godot::String& result_path) const;
    [[nodiscard]] godot::String get_default_sdk_root() const;
    [[nodiscard]] godot::String get_default_compiler_executable() const;
    [[nodiscard]] godot::String get_host_platform() const;
    [[nodiscard]] godot::String get_host_architecture() const;
    [[nodiscard]] bool is_target_supported(const godot::String& platform,
                                           const godot::String& architecture) const;
    [[nodiscard]] godot::PackedStringArray get_supported_godot_versions() const;

  protected:
    static void _bind_methods();

  private:
    struct BuildExecutionResult {
        int64_t exit_code{-1};
        godot::PackedStringArray diagnostics;
    };

    struct BuildProgressEvent {
        std::string phase;
        std::size_t completed{0};
        std::size_t total{0};
    };

    [[nodiscard]] BuildExecutionResult
    execute_ninja_build(const godot::Dictionary& build_plan) const;
    void enqueue_build_progress(const char* phase, std::size_t completed, std::size_t total) const;
    void clear_project_build_progress() const;

    mutable std::mutex prepared_build_mutex_;
    std::optional<std::vector<gdpp::ExtensionBridge>> prepared_extension_bridges_;
    std::string prepared_build_executor_;
    mutable std::mutex build_progress_mutex_;
    mutable std::vector<BuildProgressEvent> build_progress_events_;
};

} // namespace gdpp::extension
