#include "support/test.hpp"

#include "gdpp/project/project_compiler.hpp"
#include "gdpp/support/path_utf8.hpp"
#include "gdpp/support/sha256.hpp"
#include "gdpp/version.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << value;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

const std::string& native_class_for(const gdpp::ProjectCompileResult& result,
                                    const std::string_view file_name) {
    const auto found =
        std::find_if(result.scripts.begin(), result.scripts.end(), [&](const auto& script) {
            return script.relative_path.filename().string() == file_name;
        });
    if (found == result.scripts.end())
        throw std::runtime_error{"missing compiled script " + std::string{file_name}};
    return found->class_name;
}

gdpp::ProjectCompileOptions project_options(const std::filesystem::path& root) {
    gdpp::ProjectCompileOptions options;
    options.project_root = root;
    options.output_directory = root / "addons/gdpp/build/project";
    options.sdk_root = GDPP_TEST_SOURCE_DIR;
    options.godot_cpp_directory = std::filesystem::path{GDPP_TEST_SOURCE_DIR} / "third/godot-cpp";
    return options;
}

std::filesystem::path fixture_root(const std::string& name) {
    return std::filesystem::path{GDPP_TEST_BINARY_DIR} / "test-fixtures" / name;
}

} // namespace

TEST_CASE("SHA-256 matches published empty and abc vectors") {
    REQUIRE_EQ(gdpp::sha256(""), std::string{"e3b0c44298fc1c149afbf4c8996fb924"
                                             "27ae41e4649b934ca495991b7852b855"});
    REQUIRE_EQ(gdpp::sha256("abc"), std::string{"ba7816bf8f01cfea414140de5dae2223"
                                                "b00361a396177a9cb410ff61f20015ad"});
}

TEST_CASE("project compiler reports ordered per-file frontend progress") {
    const auto root = fixture_root("project-progress");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "first.gd", "extends Node\nclass_name ProgressFirst\n");
    write_text(root / "second.gd", "extends Node\nclass_name ProgressSecond\n");

    struct ProgressSample {
        gdpp::ProjectCompilePhase phase;
        std::size_t completed;
        std::size_t total;
    };
    std::vector<ProgressSample> samples;
    auto options = project_options(root);
    options.progress_callback = [&](const gdpp::ProjectCompilePhase phase,
                                    const std::size_t completed, const std::size_t total) {
        samples.push_back({phase, completed, total});
    };

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const std::array expected_phases{
        gdpp::ProjectCompilePhase::scan,     gdpp::ProjectCompilePhase::parse,
        gdpp::ProjectCompilePhase::analyze,  gdpp::ProjectCompilePhase::translate,
        gdpp::ProjectCompilePhase::generate,
    };
    std::size_t sample_index = 0;
    for (const auto phase : expected_phases) {
        REQUIRE(sample_index < samples.size());
        REQUIRE(samples[sample_index].phase == phase);
        REQUIRE_EQ(samples[sample_index].completed, std::size_t{0});
        const auto expected_total =
            phase == gdpp::ProjectCompilePhase::scan || phase == gdpp::ProjectCompilePhase::generate
                ? std::size_t{1}
                : std::size_t{2};
        REQUIRE_EQ(samples[sample_index].total, expected_total);
        std::size_t previous = 0;
        while (sample_index < samples.size() && samples[sample_index].phase == phase) {
            REQUIRE(samples[sample_index].completed >= previous);
            REQUIRE(samples[sample_index].completed <= samples[sample_index].total);
            previous = samples[sample_index].completed;
            ++sample_index;
        }
        REQUIRE_EQ(previous, expected_total);
    }
    REQUIRE_EQ(sample_index, samples.size());
}

TEST_CASE("project compiler incrementally generates a unified native extension") {
    const auto root = fixture_root("project-compiler");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "player.gd",
               "extends Node\nclass_name ProjectPlayer\nfunc ready() -> void:\n    pass\n");
    write_text(root / "actors/enemy.gd",
               "extends Node\nclass_name ProjectEnemy\nfunc attack() -> int:\n    return 1\n");
    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);
    write_text(options.output_directory / "CMakeLists.txt", "retired\n");
    write_text(options.output_directory / "gdpp_project.gdextension",
               "[configuration]\nentry_symbol=\"gdpp_project_library_init\"\n");
    write_text(options.output_directory / "patch_godot_cpp_class_db.cmake", "retired\n");
    write_text(options.output_directory / "prune_stale_development.cmake", "retired\n");

    const auto first = compiler.compile(options);
    REQUIRE(first.success);
    REQUIRE_EQ(first.compiled_count, std::size_t{2});
    REQUIRE_EQ(first.cache_hit_count, std::size_t{0});
    REQUIRE(!std::filesystem::exists(options.output_directory / "CMakeLists.txt"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "gdpp_project.gdextension"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "prune_stale_development.cmake"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "patch_godot_cpp_class_db.cmake"));
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "register_types.cpp"));
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "symbols.map"));
    REQUIRE_EQ(first.native_library_directory, root / "addons/gdpp/binary");
    REQUIRE_EQ(first.build_id.size(), std::size_t{16});
    for (const auto& script : first.scripts) {
        REQUIRE(script.class_name.find("GDPPNative_") == 0);
        REQUIRE(std::filesystem::is_regular_file(options.output_directory / "generated" /
                                                 script.symbol_file_name));
    }
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "build_id.txt"));
    REQUIRE(std::filesystem::is_regular_file(root / "addons/gdpp/build/.gdignore"));
    REQUIRE(read_text(options.output_directory / "manifest.txt")
                .find(std::string{"GDPP_MANIFEST 4 "} + GDPP_VERSION_STRING + " " +
                      GDPP_CODEGEN_FINGERPRINT + "\n") == 0);
    const auto symbol_index = read_text(options.output_directory / "symbols.map");
    REQUIRE(symbol_index.rfind("GDPP_PROJECT_SYMBOL_MAP 1\n", 0) == 0);
    REQUIRE(symbol_index.find(first.build_id) != std::string::npos);
    REQUIRE(symbol_index.find("generated/project_player.gd.symbols") != std::string::npos);
    REQUIRE(read_text(options.output_directory / "generated/project_player.gd.symbols")
                .find("method \"ready\"") != std::string::npos);

    const auto second = compiler.compile(options);
    REQUIRE(second.success);
    REQUIRE_EQ(second.compiled_count, std::size_t{0});
    REQUIRE_EQ(second.cache_hit_count, std::size_t{2});

    write_text(root / "actors/enemy.gd",
               "extends Node\nclass_name ProjectEnemy\nfunc attack() -> int:\n    return 2\n");
    const auto third = compiler.compile(options);
    REQUIRE(third.success);
    REQUIRE_EQ(third.compiled_count, std::size_t{1});
    REQUIRE_EQ(third.cache_hit_count, std::size_t{1});
    REQUIRE(third.build_id != first.build_id);
    REQUIRE_EQ(native_class_for(third, "player.gd"), native_class_for(first, "player.gd"));
    REQUIRE_EQ(native_class_for(third, "enemy.gd"), native_class_for(first, "enemy.gd"));
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find(native_class_for(first, "player.gd")) != std::string::npos);
    REQUIRE(registration.find(native_class_for(first, "enemy.gd")) != std::string::npos);
    REQUIRE(registration.find("::_gdpp_preload_resources();") == std::string::npos);
    REQUIRE(registration.find("::_gdpp_release_preloaded_resources();") != std::string::npos);
    REQUIRE(registration.find("gdpp_library_init(GDExtensionInterfaceGetProcAddress") !=
            std::string::npos);
    REQUIRE(registration.find("gdpp_project_library_init") == std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(gdpp::runtime::CoroutineFunctionState)") !=
            std::string::npos);

    std::filesystem::remove(root / "player.gd", error);
    const auto fourth = compiler.compile(options);
    REQUIRE(fourth.success);
    REQUIRE_EQ(fourth.removed_count, std::size_t{1});
    REQUIRE_EQ(fourth.scripts.size(), std::size_t{1});
}

TEST_CASE("attached descriptors defer every script constant until Godot requests its value") {
    const auto root = fixture_root("project-deferred-attached-constants");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "game_map.gd",
               "extends Node\n"
               "const BALL_SCENE = preload(\"res://component/ball.tscn\")\n"
               "const BALL_SCENES: Array = [preload(\"res://component/ball.tscn\")]\n"
               "const PURE_VALUE = Vector2(2.0, 4.0)\n"
               "@export var default_scene = preload(\"res://component/ball.tscn\")\n"
               "var runtime_tree = get_tree()\n"
               "static var static_scene = preload(\"res://component/ball.tscn\")\n"
               "func scene_resource():\n"
               "    return BALL_SCENE\n"
               "func scene_or_default(scene = preload(\"res://component/ball.tscn\")):\n"
               "    return scene\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    const auto descriptor_begin = source.find("::_gdpp_descriptor() {");
    const auto descriptor_end = source.find("    return descriptor;", descriptor_begin);
    REQUIRE(descriptor_begin != std::string::npos);
    REQUIRE(descriptor_end != std::string::npos);
    const auto descriptor = source.substr(descriptor_begin, descriptor_end - descriptor_begin);
    REQUIRE(descriptor.find(
                "descriptor.deferred_constants.push_back({godot::StringName(\"BALL_SCENE\"), "
                "[]() -> godot::Variant") != std::string::npos);
    REQUIRE(descriptor.find(
                "descriptor.deferred_constants.push_back({godot::StringName(\"PURE_VALUE\"), "
                "[]() -> godot::Variant") != std::string::npos);
    REQUIRE(descriptor.find(
                "descriptor.deferred_constants.push_back({godot::StringName(\"BALL_SCENES\"), "
                "[]() -> godot::Variant") != std::string::npos);
    REQUIRE(descriptor.find("descriptor.constants[godot::StringName(\"BALL_SCENE\")]") ==
            std::string::npos);
    REQUIRE(descriptor.find("gdpp::runtime::load_resource(") == std::string::npos);
    REQUIRE(descriptor.find("get_tree(") == std::string::npos);
    REQUIRE(descriptor.find("_gdpp_preload_resources(") == std::string::npos);
    REQUIRE(descriptor.find("descriptor.method_dispatches.push_back({"
                            "godot::StringName(\"scene_resource\"), &") != std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("::_gdpp_preload_resources();") == std::string::npos);
    REQUIRE(registration.find("gdpp_engine") == std::string::npos);
}

TEST_CASE("project compiler publishes normalized extension class icons") {
    const auto root = fixture_root("project-icons");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "icons/type.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n");
    write_text(root / "actors/enemy.gd", "@icon(\"../icons/type.svg\")\n"
                                         "class_name IconEnemy\n"
                                         "extends Node\n");
    write_text(root / "global_icon.gd", "@icon(\"res://icons/type.svg\")\n"
                                        "class_name GlobalIcon\n"
                                        "extends Resource\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto first = compiler.compile(options);

    REQUIRE(first.success);
    const auto enemy_class = native_class_for(first, "enemy.gd");
    const auto global_class = native_class_for(first, "global_icon.gd");
    REQUIRE(!enemy_class.empty());
    REQUIRE(!global_class.empty());
    REQUIRE(std::all_of(first.scripts.begin(), first.scripts.end(), [](const auto& script) {
        return script.icon_path == std::optional<std::string>{"res://icons/type.svg"};
    }));

    const auto cached = compiler.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{2});
    REQUIRE_EQ(cached.scripts.front().icon_path,
               std::optional<std::string>{"res://icons/type.svg"});
}

TEST_CASE("project compiler rejects icon paths outside project resources") {
    const auto root = fixture_root("project-invalid-icons");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "invalid.gd", "@icon(\"../outside.svg\")\n"
                                    "class_name InvalidIcon\n"
                                    "extends Node\n");
    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "PRJ0027"; }));
}

TEST_CASE("project compiler ignores cross-platform filesystem metadata") {
    const auto root = fixture_root("project-platform-metadata");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "player.gd",
               "extends Node\nclass_name MetadataPlayer\nfunc ready() -> void:\n    pass\n");
    write_text(root / "._player.gd", "\0\5\26\7not gdscript");
    write_text(root / "scenes/._level.tscn", "\0\5\26\7not a scene");
    write_text(root / "__MACOSX/shadow.gd", "this is archive metadata");
    write_text(root / "nested/__MACOSX/shadow.tscn", "this is archive metadata");
    write_text(root / ".DS_Store", "finder metadata");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE_EQ(result.compiled_count, std::size_t{1});
    REQUIRE_EQ(result.scripts.front().relative_path.generic_string(), std::string{"player.gd"});
    REQUIRE(result.diagnostics.empty());
}

TEST_CASE("project compiler preserves tool mode across incremental cache hits") {
    const auto root = fixture_root("project-tool-metadata");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "editor_worker.gd", "@tool\n"
                                          "extends Node\n"
                                          "class_name EditorWorker\n");
    write_text(root / "runtime_worker.gd", "extends Node\n"
                                           "class_name RuntimeWorker\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;

    const auto first = compiler.compile(options);
    REQUIRE(first.success);
    REQUIRE_EQ(first.scripts.size(), std::size_t{2});
    const auto editor =
        std::find_if(first.scripts.begin(), first.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "editor_worker.gd";
        });
    const auto runtime =
        std::find_if(first.scripts.begin(), first.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "runtime_worker.gd";
        });
    REQUIRE(editor != first.scripts.end());
    REQUIRE(runtime != first.scripts.end());
    REQUIRE(editor->is_tool);
    REQUIRE(!runtime->is_tool);

    const auto cached = compiler.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{2});
    REQUIRE_EQ(std::count_if(cached.scripts.begin(), cached.scripts.end(),
                             [](const auto& script) { return script.is_tool; }),
               std::ptrdiff_t{1});
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("GDREGISTER_CLASS(" + editor->class_name + ")") != std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(" + runtime->class_name + ")") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_RUNTIME_CLASS") == std::string::npos);
}

TEST_CASE("project compiler preserves static unload metadata across cache hits") {
    const auto root = fixture_root("project-static-unload-metadata");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "static_state.gd", "@static_unload\n"
                                         "extends Node\n"
                                         "static var value := 42\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;

    const auto first = compiler.compile(options);
    REQUIRE(first.success);
    REQUIRE_EQ(first.scripts.size(), std::size_t{1});
    REQUIRE(first.scripts.front().static_unload);

    const auto cached = compiler.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{1});
    REQUIRE(cached.scripts.front().static_unload);
}

TEST_CASE("project compiler excludes editor class hierarchies from runtime registration") {
    const auto root = fixture_root("project-editor-only-registration");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "editor_plugin.gd", "@tool\n"
                                          "extends EditorPlugin\n"
                                          "class_name CustomerEditorPlugin\n");
    write_text(root / "editor_derived.gd", "@tool\n"
                                           "extends CustomerEditorPlugin\n"
                                           "class_name CustomerEditorDerived\n");
    write_text(root / "runtime_worker.gd", "extends Node\n"
                                           "class_name CustomerRuntimeWorker\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{3});
    const auto find_script = [&](const std::string_view name) {
        return std::find_if(result.scripts.begin(), result.scripts.end(), [&](const auto& script) {
            return script.relative_path.filename().string() == std::string{name};
        });
    };
    const auto plugin = find_script("editor_plugin.gd");
    const auto derived = find_script("editor_derived.gd");
    const auto runtime = find_script("runtime_worker.gd");
    REQUIRE(plugin != result.scripts.end());
    REQUIRE(derived != result.scripts.end());
    REQUIRE(runtime != result.scripts.end());
    REQUIRE(plugin->is_editor_only);
    REQUIRE(derived->is_editor_only);
    REQUIRE(!runtime->is_editor_only);

    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("auto* gdpp_engine = godot::Engine::get_singleton();") !=
            std::string::npos);
    REQUIRE(registration.find("ERR_FAIL_NULL_MSG(gdpp_engine") != std::string::npos);
    REQUIRE(
        registration.find("const bool gdpp_editor_environment = gdpp_engine->is_editor_hint();") !=
        std::string::npos);
    REQUIRE(registration.find("if (gdpp_editor_environment) {\n        GDREGISTER_CLASS(" +
                              plugin->class_name + ");\n    }") != std::string::npos);
    REQUIRE(registration.find("if (gdpp_editor_environment) {\n        GDREGISTER_CLASS(" +
                              derived->class_name + ");\n    }") != std::string::npos);
    REQUIRE(registration.find("    GDREGISTER_CLASS(" + runtime->class_name + ");") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_RUNTIME_CLASS") == std::string::npos);
}

TEST_CASE("project compiler isolates tool access to runtime script state") {
    const auto root = fixture_root("project-tool-runtime-isolation");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "runtime_state.gd", "extends RefCounted\n"
                                          "class_name RuntimeState\n"
                                          "const ANSWER := 42\n"
                                          "static var shared: Variant = \"runtime\"\n"
                                          "static func answer() -> int:\n"
                                          "    return ANSWER\n");
    write_text(root / "tool_consumer.gd", "@tool\n"
                                          "extends RefCounted\n"
                                          "class_name ToolConsumer\n"
                                          "static func inspect() -> Array:\n"
                                          "    var instance := RuntimeState.new()\n"
                                          "    RuntimeState.shared = \"changed\"\n"
                                          "    return [instance, RuntimeState.shared, "
                                          "RuntimeState.answer()]\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto tool =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "tool_consumer.gd";
        });
    const auto runtime =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "runtime_state.gd";
        });
    REQUIRE(tool != result.scripts.end());
    REQUIRE(runtime != result.scripts.end());
    const auto tool_header =
        read_text(options.output_directory / "generated" / tool->header_file_name);
    const auto tool_source =
        read_text(options.output_directory / "generated" / tool->source_file_name);
    const auto runtime_source =
        read_text(options.output_directory / "generated" / runtime->source_file_name);
    REQUIRE(tool_header.find("if (!T::_gdpp_tool_mode && gdpp::runtime::is_editor_hint())") !=
            std::string::npos);
    REQUIRE(tool_source.find("gdpp::runtime::is_editor_hint() ? godot::Variant()") !=
            std::string::npos);
    REQUIRE(tool_source.find("if (!gdpp::runtime::is_editor_hint()) " + runtime->class_name +
                             "::_gdpp_set_shared") != std::string::npos);
    REQUIRE(tool_source.find(runtime->class_name + "::answer()") != std::string::npos);
    REQUIRE(runtime_source.find("static thread_local godot::Variant editor_value{}") !=
            std::string::npos);
}

TEST_CASE("project compiler preserves mixed tool inheritance metadata") {
    const auto root = fixture_root("project-tool-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "editor_base.gd", "@tool\n"
                                        "extends Node\n"
                                        "class_name EditorBase\n");
    write_text(root / "runtime_child.gd", "extends EditorBase\n"
                                          "class_name RuntimeChild\n");
    const auto options = project_options(root);

    const auto runtime_child = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(runtime_child.success);
    REQUIRE_EQ(runtime_child.scripts.size(), std::size_t{2});
    const auto editor_base_script =
        std::find_if(runtime_child.scripts.begin(), runtime_child.scripts.end(),
                     [](const auto& script) { return script.global_name == "EditorBase"; });
    const auto runtime_child_script =
        std::find_if(runtime_child.scripts.begin(), runtime_child.scripts.end(),
                     [](const auto& script) { return script.global_name == "RuntimeChild"; });
    REQUIRE(editor_base_script != runtime_child.scripts.end());
    REQUIRE(runtime_child_script != runtime_child.scripts.end());
    REQUIRE(editor_base_script->is_tool);
    REQUIRE(!runtime_child_script->is_tool);

    write_text(root / "editor_base.gd", "extends Node\n"
                                        "class_name EditorBase\n");
    write_text(root / "runtime_child.gd", "@tool\n"
                                          "extends EditorBase\n"
                                          "class_name RuntimeChild\n");
    const auto editor_child = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(editor_child.success);
    REQUIRE_EQ(editor_child.scripts.size(), std::size_t{2});
    const auto runtime_base_script =
        std::find_if(editor_child.scripts.begin(), editor_child.scripts.end(),
                     [](const auto& script) { return script.global_name == "EditorBase"; });
    const auto editor_child_script =
        std::find_if(editor_child.scripts.begin(), editor_child.scripts.end(),
                     [](const auto& script) { return script.global_name == "RuntimeChild"; });
    REQUIRE(runtime_base_script != editor_child.scripts.end());
    REQUIRE(editor_child_script != editor_child.scripts.end());
    REQUIRE(!runtime_base_script->is_tool);
    REQUIRE(editor_child_script->is_tool);
}

TEST_CASE("project compiler includes GDScript embedded in text scenes") {
    const auto root = fixture_root("project-embedded-scripts");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "actors/enemy_pointer.tscn",
               "[gd_scene format=3]\n\n"
               "[sub_resource type=\"GDScript\" id=\"GDScript_pointer\"]\n"
               "script/source = \"extends Node2D\n"
               "var label = \\\"embedded\\\"\n"
               "func value() -> String:\n"
               "\\treturn label\n\"\n\n"
               "[node name=\"EnemyPointer\" type=\"Node2D\"]\n"
               "script = SubResource(\"GDScript_pointer\")\n");
    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);

    const auto result = compiler.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE_EQ(result.compiled_count, std::size_t{1});
    REQUIRE_EQ(result.scripts.front().relative_path.generic_string(),
               std::string{"actors/enemy_pointer.tscn::GDScript_pointer"});
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "generated" /
                                             result.scripts.front().source_file_name));
}

TEST_CASE("project compiler includes source-less GDScript embedded in text resources") {
    const auto root = fixture_root("project-embedded-resource-scripts");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "data/marker.tres",
               "[gd_resource type=\"Resource\" load_steps=2 format=3]\n\n"
               "[sub_resource type=\"GDScript\" id=\"GDScript_marker\"]\n\n"
               "[resource]\n"
               "script = SubResource(\"GDScript_marker\")\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE_EQ(result.scripts.front().relative_path.generic_string(),
               std::string{"data/marker.tres::GDScript_marker"});
    const auto generated =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    REQUIRE(generated.find("public gdpp::runtime::AttachedScriptBehavior") != std::string::npos);
    REQUIRE(generated.find("res://data/marker.tres::GDScript_marker") != std::string::npos);
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(source.find("descriptor.native_base_type = godot::StringName(\"Resource\")") !=
            std::string::npos);
}

TEST_CASE("project compiler rejects source-less embedded scripts with no concrete owner") {
    const auto root = fixture_root("project-unowned-embedded-resource-script");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "data/orphan.tres",
               "[gd_resource type=\"Resource\" load_steps=2 format=3]\n\n"
               "[sub_resource type=\"GDScript\" id=\"GDScript_orphan\"]\n\n"
               "[resource]\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(!result.success);
    REQUIRE(!result.diagnostics.empty());
    REQUIRE_EQ(result.diagnostics.front().diagnostic.code, std::string{"PRJ0017"});
}

TEST_CASE("moving a globally named script preserves reused generated outputs") {
    const auto root = fixture_root("project-moved-global-script");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "old/location.gd",
               "extends Node\nclass_name StableMovedScript\nfunc value() -> int:\n    return 1\n");
    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);

    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.scripts.size(), std::size_t{1});
    const auto header = initial.scripts.front().header_file_name;
    const auto source = initial.scripts.front().source_file_name;

    std::filesystem::create_directories(root / "new");
    std::filesystem::rename(root / "old/location.gd", root / "new/location.gd", error);
    REQUIRE(!error);
    const auto moved = compiler.compile(options);

    REQUIRE(moved.success);
    REQUIRE_EQ(moved.compiled_count, std::size_t{1});
    REQUIRE_EQ(moved.removed_count, std::size_t{1});
    REQUIRE_EQ(moved.scripts.front().header_file_name, header);
    REQUIRE_EQ(moved.scripts.front().source_file_name, source);
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "generated" / header));
    REQUIRE(std::filesystem::is_regular_file(options.output_directory / "generated" / source));
}

TEST_CASE("project compiler transactionally replaces renamed and incompatible generated outputs") {
    const auto root = fixture_root("project-renamed-generated-output");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "widget.gd", "extends Node\nfunc value() -> int:\n    return 1\n");
    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);

    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.scripts.size(), std::size_t{1});
    const auto old_header = initial.scripts.front().header_file_name;
    const auto old_source = initial.scripts.front().source_file_name;
    const auto old_symbols = initial.scripts.front().symbol_file_name;
    REQUIRE(old_header.find("path_widget_") == 0);
    write_text(options.output_directory / "generated/orphan.gd.hpp", "orphan");
    write_text(options.output_directory / "generated/orphan.gd.cpp", "orphan");
    write_text(options.output_directory / "generated/orphan.gd.symbols", "orphan");

    auto incompatible_manifest = read_text(options.output_directory / "manifest.txt");
    const auto header_end = incompatible_manifest.find('\n');
    REQUIRE(header_end != std::string::npos);
    const auto fingerprint_begin = incompatible_manifest.rfind(' ', header_end);
    REQUIRE(fingerprint_begin != std::string::npos);
    incompatible_manifest.replace(fingerprint_begin + 1U, header_end - fingerprint_begin - 1U,
                                  "incompatible-codegen-fingerprint");
    write_text(options.output_directory / "manifest.txt", incompatible_manifest);
    write_text(root / "widget.gd",
               "extends Node\nclass_name RenamedWidget\nfunc value() -> int:\n    return 2\n");

    const auto renamed = compiler.compile(options);

    REQUIRE(renamed.success);
    REQUIRE_EQ(renamed.compiled_count, std::size_t{1});
    REQUIRE_EQ(renamed.cache_hit_count, std::size_t{0});
    REQUIRE_EQ(renamed.removed_count, std::size_t{1});
    REQUIRE_EQ(renamed.scripts.front().header_file_name, std::string{"renamed_widget.gd.hpp"});
    REQUIRE_EQ(renamed.scripts.front().source_file_name, std::string{"renamed_widget.gd.cpp"});
    REQUIRE_EQ(renamed.scripts.front().symbol_file_name, std::string{"renamed_widget.gd.symbols"});
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated" / old_header));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated" / old_source));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated" / old_symbols));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated/orphan.gd.hpp"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated/orphan.gd.cpp"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated/orphan.gd.symbols"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "CMakeLists.txt"));
    const auto manifest = read_text(options.output_directory / "manifest.txt");
    REQUIRE(manifest.find(old_source) == std::string::npos);
    REQUIRE(manifest.find("renamed_widget.gd.cpp") != std::string::npos);
}

TEST_CASE("project compiler never follows generated manifest paths outside its owned directory") {
    const auto root = fixture_root("project-manifest-path-confinement");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "safe.gd", "extends Node\nclass_name SafeManifestScript\n");
    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);

    const auto sentinel = options.output_directory / "sentinel.gd.hpp";
    write_text(sentinel, "must remain");
    auto manifest = read_text(options.output_directory / "manifest.txt");
    const auto safe_name = std::string{"safe_manifest_script.gd.hpp"};
    const auto header = manifest.find(safe_name);
    REQUIRE(header != std::string::npos);
    manifest.replace(header, safe_name.size(), "../sentinel.gd.hpp");
    write_text(options.output_directory / "manifest.txt", manifest);

    const auto rebuilt = compiler.compile(options);

    REQUIRE(rebuilt.success);
    REQUIRE_EQ(rebuilt.compiled_count, std::size_t{1});
    REQUIRE(std::filesystem::is_regular_file(sentinel));
    REQUIRE_EQ(read_text(sentinel), std::string{"must remain"});
}

TEST_CASE("project compiler registers internal classes and includes complete enum owners") {
    const auto root = fixture_root("project-internal-class-lambda-enum");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "chart_data.gd", "class_name CorpusChartData\n"
                                       "enum Chart { FIRST, SECOND }\n");
    write_text(root / "consumer.gd",
               "extends Node\n"
               "class_name CorpusConsumer\n"
               "class Payload:\n"
               "    var value: int\n"
               "    func _init(initial: int) -> void:\n"
               "        value = initial\n"
               "signal changed(value: int)\n"
               "var chart: CorpusChartData.Chart = CorpusChartData.Chart.FIRST\n"
               "func select_chart(index: int) -> CorpusChartData.Chart:\n"
               "    return index as CorpusChartData.Chart\n"
               "func attach() -> void:\n"
               "    changed.connect(\n"
               "        func(value: int) -> void:\n"
               "            print(value))\n"
               "    var payload := Payload.new(1)\n");

    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);
    const auto result = compiler.compile(options);

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(consumer != result.scripts.end());
    REQUIRE_EQ(consumer->inner_class_names.size(), std::size_t{1});
    const auto header =
        read_text(options.output_directory / "generated" / consumer->header_file_name);
    REQUIRE(header.find("#include \"corpus_chart_data.gd.hpp\"") == std::string::npos);
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(source.find("#include \"corpus_chart_data.gd.hpp\"") != std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    const auto inner_position = registration.find(consumer->inner_class_names.front());
    const auto outer_position = registration.find(consumer->class_name, inner_position + 1);
    REQUIRE(inner_position != std::string::npos);
    REQUIRE(outer_position != std::string::npos);
    REQUIRE(inner_position < outer_position);

    const auto cached = compiler.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{2});
    const auto cached_consumer =
        std::find_if(cached.scripts.begin(), cached.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(cached_consumer != cached.scripts.end());
    REQUIRE_EQ(cached_consumer->inner_class_names, consumer->inner_class_names);
}

TEST_CASE("project compiler canonicalizes internal class aliases in typed containers") {
    const auto root = fixture_root("project-internal-container-aliases");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "queue.gd", "extends Node\n"
                                  "class Task extends RefCounted:\n"
                                  "    var value: int\n"
                                  "var pending: Array[Task] = []\n"
                                  "func get_pending() -> Array[Task]:\n"
                                  "    return pending\n"
                                  "func enqueue(task: Task) -> void:\n"
                                  "    get_pending().push_back(task)\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE_EQ(result.scripts.front().inner_class_names.size(), std::size_t{1});
    const auto canonical_tag =
        "ContainerObjectTag_" + result.scripts.front().inner_class_names.front();
    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(header.find("struct " + canonical_tag) != std::string::npos);
    REQUIRE(header.find("struct ContainerObjectTag_Task") == std::string::npos);
    REQUIRE(source.find(canonical_tag) != std::string::npos);
    REQUIRE(source.find("ContainerObjectTag_Task") == std::string::npos);
}

TEST_CASE("project compiler resolves script and nested internal enum identities") {
    const auto root = fixture_root("project-nested-internal-enums");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "messages.gd", "extends RefCounted\n"
                                     "enum Shared { UNKNOWN = 0, ACTIVE = 3 }\n"
                                     "class Packet:\n"
                                     "    enum Status { EMPTY = 0, READY = 7 }\n"
                                     "    var shared: Shared = Shared.ACTIVE\n"
                                     "    var status: Packet.Status = Packet.Status.READY\n"
                                     "    func select(value: Packet.Status) -> Packet.Status:\n"
                                     "        return value\n"
                                     "    func accepts(value: Variant) -> bool:\n"
                                     "        return value is Shared and value is Packet.Status\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(source.find("Shared::_gdpp_enum_ACTIVE") != std::string::npos);
    REQUIRE(source.find("Status::_gdpp_enum_READY") != std::string::npos);
    REQUIRE(source.find(".get_type() == godot::Variant::INT") != std::string::npos);
}

TEST_CASE("project compiler preserves nested internal class identities across cache hits") {
    const auto root = fixture_root("project-nested-internal-classes");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "factory.gd", "extends Node\n"
                                    "class Container:\n"
                                    "    class Base:\n"
                                    "        const base_value := 40\n"
                                    "    class Derived extends Base:\n"
                                    "        func value() -> int:\n"
                                    "            return base_value + 2\n"
                                    "func create() -> int:\n"
                                    "    return Container.Derived.new().value()\n");

    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);
    const auto result = compiler.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE_EQ(result.scripts.front().inner_class_names.size(), std::size_t{3});
    const auto& inner_names = result.scripts.front().inner_class_names;
    const auto has_suffix = [](const std::string& name, const std::string_view suffix) {
        return name.size() >= suffix.size() &&
               name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    const auto base = std::find_if(inner_names.begin(), inner_names.end(),
                                   [&](const auto& name) { return has_suffix(name, "__Base"); });
    const auto derived =
        std::find_if(inner_names.begin(), inner_names.end(),
                     [&](const auto& name) { return has_suffix(name, "__Derived"); });
    REQUIRE(base != inner_names.end());
    REQUIRE(derived != inner_names.end());
    REQUIRE(std::none_of(inner_names.begin(), inner_names.end(),
                         [](const auto& name) { return name.find('.') != std::string::npos; }));

    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    REQUIRE(header.find("class " + *derived + " : public " + *base) != std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("GDREGISTER_CLASS(" + *base + ")") <
            registration.find("GDREGISTER_CLASS(" + *derived + ")"));

    const auto cached = compiler.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.compiled_count, std::size_t{0});
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{1});
    REQUIRE_EQ(cached.scripts.front().inner_class_names, inner_names);
}

TEST_CASE("onready dependencies remain forward declarations and do not create header cycles") {
    const auto root = fixture_root("project-onready-header-cycle");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "header_a.gd", "extends Node\n"
                                     "class_name HeaderA\n"
                                     "const PEER_VALUE = HeaderB.VALUE\n");
    write_text(root / "header_b.gd", "extends Node\n"
                                     "class_name HeaderB\n"
                                     "const VALUE := 1\n"
                                     "@onready var owner := get_parent() as HeaderA\n");

    const gdpp::ProjectCompiler compiler;
    const auto options = project_options(root);
    const auto result = compiler.compile(options);

    REQUIRE(result.success);
    const auto header_a = read_text(options.output_directory / "generated/header_a.gd.hpp");
    const auto source_a = read_text(options.output_directory / "generated/header_a.gd.cpp");
    const auto header_b = read_text(options.output_directory / "generated/header_b.gd.hpp");
    REQUIRE(header_a.find("#include \"header_b.gd.hpp\"") == std::string::npos);
    REQUIRE(source_a.find("#include \"header_b.gd.hpp\"") != std::string::npos);
    REQUIRE(header_b.find("#include \"header_a.gd.hpp\"") == std::string::npos);
    REQUIRE(header_b.find("class GDPPNative_HeaderA_") != std::string::npos);
}

TEST_CASE("attached scripts keep onready initialization independent from user ready methods") {
    const auto root = fixture_root("project-attached-implicit-ready");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "panel.gd", "extends Node\n"
                                  "@onready var label := get_node(\"Label\")\n"
                                  "func initialize_instance() -> void:\n"
                                  "    pass\n"
                                  "func initialize_onready() -> void:\n"
                                  "    pass\n"
                                  "func dispatch_notification(_what: int) -> void:\n"
                                  "    pass\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto& script = result.scripts.front();
    const auto header = read_text(options.output_directory / "generated" / script.header_file_name);
    const auto source = read_text(options.output_directory / "generated" / script.source_file_name);
    const auto native_class = native_class_for(result, "panel.gd");
    REQUIRE(header.find("void _gdpp_initialize_onready() override") != std::string::npos);
    REQUIRE(header.find("void initialize_instance()") != std::string::npos);
    REQUIRE(header.find("void initialize_onready()") != std::string::npos);
    REQUIRE(header.find("void dispatch_notification(int64_t _what)") != std::string::npos);
    REQUIRE(header.find("_gdpp_variant_call__ready") == std::string::npos);
    REQUIRE(header.find("virtual void _ready()") == std::string::npos);
    REQUIRE(source.find("godot::StringName(\"_ready\")") == std::string::npos);
    REQUIRE(source.find("void " + native_class + "::_ready()") == std::string::npos);
    const auto initializer = source.find("void " + native_class + "::_gdpp_initialize_onready() {");
    const auto base_initializer = source.find(
        "gdpp::runtime::AttachedScriptBehavior::_gdpp_initialize_onready()", initializer);
    const auto label_initializer = source.find("get_node<godot::Node>", initializer);
    REQUIRE(initializer != std::string::npos);
    REQUIRE(base_initializer > initializer);
    REQUIRE(label_initializer > base_initializer);
    const auto label_path = source.find("godot::String(\"Label\")", base_initializer);
    REQUIRE(label_path > base_initializer);
    REQUIRE(label_path < label_initializer);
}

TEST_CASE("attached onready initialization follows inheritance and internal class lifecycles") {
    const auto root = fixture_root("project-attached-onready-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\n"
                                 "@onready var base_marker := get_node(\"BaseMarker\")\n"
                                 "func _ready() -> void:\n"
                                 "    assert(base_marker != null)\n");
    write_text(root / "child.gd", "extends \"res://base.gd\"\n"
                                  "@onready var child_marker := get_node(\"ChildMarker\")\n");
    write_text(root / "inner.gd", "extends Node\n"
                                  "class InnerBase extends Node:\n"
                                  "    @onready var base_marker := get_node(\"BaseMarker\")\n"
                                  "class InnerChild extends InnerBase:\n"
                                  "    @onready var child_marker := get_node(\"ChildMarker\")\n"
                                  "    func _ready() -> void:\n"
                                  "        assert(base_marker != null and child_marker != null)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto base_class = native_class_for(result, "base.gd");
    const auto child_class = native_class_for(result, "child.gd");
    const auto child_script =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "child.gd";
        });
    REQUIRE(child_script != result.scripts.end());
    const auto child_source =
        read_text(options.output_directory / "generated" / child_script->source_file_name);
    const auto child_initializer =
        child_source.find("void " + child_class + "::_gdpp_initialize_onready() {");
    const auto inherited_initializer =
        child_source.find(base_class + "::_gdpp_initialize_onready()", child_initializer);
    const auto child_marker =
        child_source.find("godot::String(\"ChildMarker\")", child_initializer);
    REQUIRE(child_initializer != std::string::npos);
    REQUIRE(inherited_initializer > child_initializer);
    REQUIRE(child_marker > inherited_initializer);
    REQUIRE(child_source.find("void " + child_class + "::_ready()") == std::string::npos);
    REQUIRE(child_source.find("_gdpp_variant_call__ready") == std::string::npos);
    const auto child_header =
        read_text(options.output_directory / "generated" / child_script->header_file_name);
    REQUIRE(child_header.find("virtual void _ready()") == std::string::npos);

    const auto inner_script =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "inner.gd";
        });
    REQUIRE(inner_script != result.scripts.end());
    REQUIRE_EQ(inner_script->inner_class_names.size(), std::size_t{2});
    const auto inner_source =
        read_text(options.output_directory / "generated" / inner_script->source_file_name);
    const auto& inner_base = inner_script->inner_class_names[0];
    const auto& inner_child = inner_script->inner_class_names[1];
    REQUIRE(inner_source.find("void " + inner_base + "::_gdpp_initialize_onready() {") !=
            std::string::npos);
    const auto inner_child_initializer =
        inner_source.find("void " + inner_child + "::_gdpp_initialize_onready() {");
    REQUIRE(inner_child_initializer != std::string::npos);
    REQUIRE(inner_source.find(inner_base + "::_gdpp_initialize_onready()",
                              inner_child_initializer) > inner_child_initializer);
    REQUIRE(inner_source.find("void " + inner_base + "::_ready()") == std::string::npos);
    REQUIRE(inner_source.find(inner_base + "::_gdpp_variant_call__ready") == std::string::npos);
}

TEST_CASE("project target version changes build identity") {
    const auto root = fixture_root("project-version");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "versioned.gd", "extends Node\nclass_name VersionedProject\n");
    const gdpp::ProjectCompiler compiler;
    auto options = project_options(root);
    const auto baseline = compiler.compile(options);
    REQUIRE(baseline.success);

    options.compiler.target_version = gdpp::GodotVersion::v4_7;
    const auto latest = compiler.compile(options);
    REQUIRE(latest.success);
    REQUIRE(latest.build_id != baseline.build_id);
}

TEST_CASE("project compiler lowers internal classes derived from preloaded scripts") {
    const auto root = fixture_root("project-cross-script-inner-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "message.gd", "extends RefCounted\n"
                                    "class_name MessageBase\n"
                                    "var cleared: bool = false\n"
                                    "func clear() -> void:\n"
                                    "    cleared = true\n"
                                    "func value() -> int:\n"
                                    "    return 40\n");
    write_text(root / "packets.gd", "extends Node\n"
                                    "const Message = preload(\"message.gd\")\n"
                                    "class AliasPacket extends Message:\n"
                                    "    func reset() -> bool:\n"
                                    "        clear()\n"
                                    "        return cleared\n"
                                    "    func value() -> int:\n"
                                    "        return super() + 2\n"
                                    "class GlobalPacket extends MessageBase:\n"
                                    "    func reset() -> bool:\n"
                                    "        clear()\n"
                                    "        return cleared\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto base =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"message.gd"};
        });
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"packets.gd"};
        });
    REQUIRE(base != result.scripts.end());
    REQUIRE(consumer != result.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"message.gd"});
    REQUIRE_EQ(consumer->inner_class_names.size(), std::size_t{2});
    const auto header =
        read_text(options.output_directory / "generated" / consumer->header_file_name);
    REQUIRE(header.find("#include \"" + base->header_file_name + "\"") != std::string::npos);
    for (const auto& inner : consumer->inner_class_names) {
        REQUIRE(header.find("class " + inner + " : public " + base->class_name) !=
                std::string::npos);
    }
    REQUIRE(header.find("virtual int64_t value() override;") != std::string::npos);
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(source.find(base->class_name + "::value()") != std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    const auto base_position = registration.find(base->class_name);
    const auto inner_position = registration.find(consumer->inner_class_names.front());
    REQUIRE(base_position != std::string::npos);
    REQUIRE(inner_position != std::string::npos);
    REQUIRE(base_position < inner_position);
}

TEST_CASE("project compiler exposes preloaded scripts as typed namespaces") {
    const auto root = fixture_root("project-script-resource-namespaces");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "library.gd", "extends RefCounted\n"
                                    "const LIMIT: int = 40\n"
                                    "static var TAG: String = \"stable\"\n"
                                    "enum State { IDLE = 0, READY = 7 }\n"
                                    "static func answer(value: int = 2) -> int:\n"
                                    "    return LIMIT + value\n"
                                    "class Item:\n"
                                    "    enum Mode { COLD = 0, HOT = 3 }\n"
                                    "    var mode: Mode = Mode.HOT\n"
                                    "    var child: Child = null\n"
                                    "    class Child:\n"
                                    "        var id: int = 7\n"
                                    "    func value() -> int:\n"
                                    "        return 42\n"
                                    "    func root_state() -> int:\n"
                                    "        return State.READY\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "const library = preload(\"library.gd\")\n"
                                     "const LibraryScript = preload(\"library.gd\")\n"
                                     "var root_value: LibraryScript = null\n"
                                     "var item: library.Item = null\n"
                                     "var items: Array[library.Item] = []\n"
                                     "var state: library.State = library.State.READY\n"
                                     "var mode: library.Item.Mode = library.Item.Mode.HOT\n"
                                     "func make() -> library.Item:\n"
                                     "    return library.Item.new()\n"
                                     "func cast_root(value: Variant) -> LibraryScript:\n"
                                     "    return value as LibraryScript\n"
                                     "func is_root(value: LibraryScript) -> bool:\n"
                                     "    return value is LibraryScript\n"
                                     "func total() -> int:\n"
                                     "    return library.answer() + library.LIMIT\n"
                                     "func tag() -> String:\n"
                                     "    return library.TAG\n"
                                     "func set_tag(value: String) -> void:\n"
                                     "    library.TAG = value\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto library =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"library.gd"};
        });
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(library != result.scripts.end());
    REQUIRE(consumer != result.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"library.gd"});
    REQUIRE_EQ(library->inner_class_names.size(), std::size_t{2});
    const auto item = std::find_if(
        library->inner_class_names.begin(), library->inner_class_names.end(),
        [](const auto& name) { return name.find("__Item__Child") == std::string::npos; });
    REQUIRE(item != library->inner_class_names.end());
    const auto& item_class = *item;
    const auto library_header =
        read_text(options.output_directory / "generated" / library->header_file_name);
    for (const auto& inner : library->inner_class_names)
        REQUIRE(library_header.find("class " + inner + ";") != std::string::npos);
    const auto library_source =
        read_text(options.output_directory / "generated" / library->source_file_name);
    REQUIRE(library_source.find(library->class_name + "::State::_gdpp_enum_READY") !=
            std::string::npos);
    const auto header =
        read_text(options.output_directory / "generated" / consumer->header_file_name);
    REQUIRE(header.find("#include \"" + library->header_file_name + "\"") != std::string::npos);
    REQUIRE(header.find("godot::Ref<godot::RefCounted> item") != std::string::npos);
    REQUIRE(header.find("godot::Ref<godot::RefCounted> root_value") != std::string::npos);
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(source.find("InternalClassResource<" + item_class + ">{}.instantiate()") !=
            std::string::npos);
    REQUIRE(source.find(library->class_name + "::answer(") != std::string::npos);
    REQUIRE(source.find(library->class_name + "::_gdpp_get_TAG()") != std::string::npos);
    REQUIRE(source.find(library->class_name +
                        "::_gdpp_set_TAG(std::move(_gdpp_assignment_result_") != std::string::npos);
    REQUIRE(source.find("cast_attached_script(gdpp::runtime::to_variant(value), "
                        "godot::String(\"res://library.gd\"))") != std::string::npos);
    REQUIRE(source.find("is_attached_script_instance") != std::string::npos);
    REQUIRE(source.find("Object::cast_to<" + library->class_name) == std::string::npos);
    REQUIRE(source.find(library->class_name + "::LIMIT") != std::string::npos);
    REQUIRE(source.find(library->class_name + "::State::_gdpp_enum_READY") != std::string::npos);
    REQUIRE(source.find(item_class + "::Mode::_gdpp_enum_HOT") != std::string::npos);
}

TEST_CASE("project compiler rejects cross-script native class collisions transactionally") {
    const auto root = fixture_root("project-collision");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "first.gd", "extends Node\nclass_name DuplicateNative\n");
    write_text(root / "second.gd", "extends Node\nclass_name DuplicateNative\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto result = compiler.compile(options);

    REQUIRE(!result.success);
    REQUIRE(!result.diagnostics.empty());
    REQUIRE(!std::filesystem::exists(options.output_directory / "manifest.txt"));
}

TEST_CASE("project compiler resolves class and path inheritance in parent-first order") {
    const auto root = fixture_root("project-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\n"
                                 "class_name ProjectBase\n"
                                 "enum { ANONYMOUS_LIMIT = 11 }\n"
                                 "var base_value: int = 40\n"
                                 "func inherited_answer() -> int:\n"
                                 "    return base_value + 2\n"
                                 "static func static_answer(value: int = 42) -> int:\n"
                                 "    return value\n");
    write_text(root / "actors/middle.gd", "extends ProjectBase\n"
                                          "class_name ProjectMiddle\n"
                                          "func middle_answer() -> int:\n"
                                          "    return inherited_answer()\n");
    write_text(root / "actors/child.gd", "extends \"middle.gd\"\n"
                                         "class_name ProjectChild\n"
                                         "var linked: ProjectBase\n"
                                         "func answer() -> int:\n"
                                         "    return ProjectBase.static_answer() + "
                                         "ProjectBase.ANONYMOUS_LIMIT\n"
                                         "func inherited_limit() -> int:\n"
                                         "    return ANONYMOUS_LIMIT\n"
                                         "func typed_identity(value: ProjectBase) -> ProjectBase:\n"
                                         "    linked = value\n"
                                         "    return linked\n"
                                         "func read_base(value: ProjectBase) -> int:\n"
                                         "    value.base_value = 40\n"
                                         "    return value.base_value + 2\n"
                                         "func is_base(value: Variant) -> bool:\n"
                                         "    return value is ProjectBase\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{3});
    REQUIRE(result.scripts[0].class_name.find("GDPPNative_ProjectBase_") == 0);
    REQUIRE(result.scripts[1].class_name.find("GDPPNative_ProjectMiddle_") == 0);
    REQUIRE(result.scripts[2].class_name.find("GDPPNative_ProjectChild_") == 0);
    const auto base_source = read_text(options.output_directory / "generated/project_base.gd.cpp");
    REQUIRE(base_source.find("godot::StringName(\"static_answer\")") != std::string::npos);
    REQUIRE(base_source.find("gdpp::runtime::bind_variant_method(") != std::string::npos);
    REQUIRE(base_source.find("method.flags |= GDEXTENSION_METHOD_FLAG_STATIC") !=
            std::string::npos);
    const auto child_source =
        read_text(options.output_directory / "generated/project_child.gd.cpp");
    REQUIRE(child_source.find("::_gdpp_enum_ANONYMOUS_LIMIT") != std::string::npos);
    const auto middle_header =
        read_text(options.output_directory / "generated/project_middle.gd.hpp");
    const auto& base_class = native_class_for(result, "base.gd");
    const auto& middle_class = native_class_for(result, "middle.gd");
    const auto& child_class = native_class_for(result, "child.gd");
    REQUIRE(middle_header.find("#include \"project_base.gd.hpp\"") != std::string::npos);
    REQUIRE(middle_header.find("public " + base_class) != std::string::npos);
    const auto child_header =
        read_text(options.output_directory / "generated/project_child.gd.hpp");
    REQUIRE(child_header.find("#include \"project_middle.gd.hpp\"") != std::string::npos);
    REQUIRE(child_header.find("GDCLASS(" + child_class + ", " + middle_class + ")") !=
            std::string::npos);
    REQUIRE(child_header.find("gdpp::runtime::ObjectStorage<godot::Object> linked") !=
            std::string::npos);
    REQUIRE(child_header.find("gdpp::runtime::ObjectStorage<godot::Object> typed_identity("
                              "gdpp::runtime::ObjectStorage<godot::Object> value)") !=
            std::string::npos);
    REQUIRE(child_source.find(base_class + "::static_answer()") != std::string::npos);
    REQUIRE(child_source.find("get_named(value, godot::StringName(\"base_value\"), "
                              "gdpp::runtime::ScriptSourceLocation{") != std::string::npos);
    REQUIRE(child_source.find("set_named(_gdpp_dynamic_root_") != std::string::npos);
    REQUIRE(child_source.find("godot::StringName(\"base_value\")") != std::string::npos);
    REQUIRE(child_source.find("is_attached_script_instance") != std::string::npos);
    REQUIRE(child_source.find("godot::String(\"res://base.gd\")") != std::string::npos);
    REQUIRE(child_source.find("Object::cast_to<" + base_class) == std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    const auto base_position = registration.find("GDREGISTER_CLASS(GDPPNative_ProjectBase_");
    const auto middle_position = registration.find("GDREGISTER_CLASS(GDPPNative_ProjectMiddle_");
    const auto child_position = registration.find("GDREGISTER_CLASS(GDPPNative_ProjectChild_");
    REQUIRE(base_position < middle_position);
    REQUIRE(middle_position < child_position);
}

TEST_CASE("project compiler preserves Object free lifetime semantics for script classes") {
    const auto root = fixture_root("project-script-free");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "disposable_node.gd", "extends Node\n"
                                            "class_name DisposableNode\n");
    write_text(root / "disposable_ref.gd", "extends RefCounted\n"
                                           "class_name DisposableRef\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "class_name ScriptFreeConsumer\n"
                                     "func free_node() -> void:\n"
                                     "    var value: DisposableNode = DisposableNode.new()\n"
                                     "    value.free()\n"
                                     "func reject_ref_counted() -> void:\n"
                                     "    var value: DisposableRef = DisposableRef.new()\n"
                                     "    value.free()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    const std::string lowering = "gdpp::runtime::free_object_at(gdpp::runtime::to_variant(value), "
                                 "gdpp::runtime::ScriptSourceLocation{";
    const auto first = source.find(lowering);
    REQUIRE(first != std::string::npos);
    REQUIRE(source.find(lowering, first + lowering.size()) != std::string::npos);
    REQUIRE(source.find("memdelete(value)") == std::string::npos);
}

TEST_CASE("project compiler lowers super calls to the resolved native base") {
    const auto root = fixture_root("project-super-dispatch");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name SuperBase\n"
                                 "var value: int = 0\n"
                                 "func set_value(next: int) -> void:\n"
                                 "    value = next\n");
    write_text(root / "child.gd", "extends SuperBase\nclass_name SuperChild\n"
                                  "func set_value(next: int) -> void:\n"
                                  "    super.set_value(next)\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto source = read_text(options.output_directory / "generated/super_child.gd.cpp");
    const auto header = read_text(options.output_directory / "generated/super_child.gd.hpp");
    REQUIRE(source.find(native_class_for(result, "base.gd") + "::set_value(") != std::string::npos);
    REQUIRE(header.find("set_value(int64_t next) override") != std::string::npos);
}

TEST_CASE("project compiler dynamically dispatches script overrides with a different native ABI") {
    const auto root = fixture_root("project-override-signature");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name OverrideBase\n"
                                 "func edit(value, attribute, extra):\n"
                                 "    return value\n"
                                 "func invoke(target: OverrideBase):\n"
                                 "    return target.edit(1, 2, 3)\n");
    write_text(root / "child.gd", "extends OverrideBase\nclass_name OverrideChild\n"
                                  "func edit(value, attribute: int, extra = null):\n"
                                  "    return value\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto header = read_text(options.output_directory / "generated/override_child.gd.hpp");
    REQUIRE(header.find("edit(godot::Variant value, int64_t attribute, "
                        "godot::Variant _gdpp_argument_extra") != std::string::npos);
    REQUIRE(header.find("_gdpp_argument_extra = gdpp::runtime::default_argument()) override") ==
            std::string::npos);
    const auto base_source = read_text(options.output_directory / "generated/override_base.gd.cpp");
    REQUIRE(base_source.find("gdpp::runtime::call_dynamic") != std::string::npos);
}

TEST_CASE("project compiler isolates fixed and variadic override ABIs") {
    const auto root = fixture_root("project-vararg-override-abi");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends RefCounted\nclass_name VarargOverrideBase\n"
                                 "func collect(value) -> int:\n    return value\n"
                                 "func invoke(value: VarargOverrideBase) -> int:\n"
                                 "    return value.collect(1)\n");
    write_text(root / "child.gd", "extends VarargOverrideBase\n"
                                  "class_name VarargOverrideChild\n"
                                  "func collect(value, ...extras: Array) -> int:\n"
                                  "    return value + extras.size()\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto child_header =
        read_text(options.output_directory / "generated/vararg_override_child.gd.hpp");
    const auto base_source =
        read_text(options.output_directory / "generated/vararg_override_base.gd.cpp");
    REQUIRE(child_header.find("_gdpp_native_override_collect(godot::Variant value, "
                              "godot::Array extras)") != std::string::npos);
    REQUIRE(child_header.find("godot::Array extras) override") == std::string::npos);
    REQUIRE(base_source.find("gdpp::runtime::call_dynamic") != std::string::npos);
}

TEST_CASE("project compiler accepts variance-safe script override contracts") {
    const auto root = fixture_root("project-override-variance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd",
               "extends Node\nclass_name VarianceBase\n"
               "func transform(value: Node, policy: Variant, limit: int = 1) -> Node:\n"
               "    return value\n");
    write_text(root / "child.gd", "extends VarianceBase\nclass_name VarianceChild\n"
                                  "func transform(value: Object, policy: Variant, limit: int = 1, "
                                  "context = null) -> Node2D:\n"
                                  "    return null\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(result.success);
    const auto header =
        read_text(project_options(root).output_directory / "generated/variance_child.gd.hpp");
    REQUIRE(header.find("_gdpp_native_override_transform") != std::string::npos);
}

TEST_CASE("project compiler refines inferred cross-script enum override parameters") {
    const auto root = fixture_root("project-inferred-enum-override");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "contract.gd", "extends RefCounted\n"
                                     "class_name InferredEnumContract\n"
                                     "enum Mode { NORMAL, RESET }\n");
    write_text(root / "project.godot", "[application]\nconfig/name=\"Inferred enum override\"\n"
                                       "[autoload]\n"
                                       "InferredEnumContract=\"*res://contract.gd\"\n");
    write_text(root / "base.gd", "extends Node\n"
                                 "class_name InferredEnumBase\n"
                                 "func reset(mode := InferredEnumContract.Mode.NORMAL) -> void:\n"
                                 "    pass\n");
    write_text(root / "child.gd", "extends InferredEnumBase\n"
                                  "class_name InferredEnumChild\n"
                                  "func reset(mode: InferredEnumContract.Mode = "
                                  "InferredEnumContract.Mode.RESET) -> void:\n"
                                  "    pass\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto base_source =
        read_text(options.output_directory / "generated/inferred_enum_base.gd.cpp");
    const auto child_header =
        read_text(options.output_directory / "generated/inferred_enum_child.gd.hpp");
    const auto child_source =
        read_text(options.output_directory / "generated/inferred_enum_child.gd.cpp");
    REQUIRE(base_source.find("int64_t mode =") != std::string::npos);
    REQUIRE(child_source.find("int64_t mode =") != std::string::npos);
    REQUIRE(child_header.find("_gdpp_native_override_reset") == std::string::npos);
}

TEST_CASE("project compiler compares internal script overrides by emitted native ABI") {
    const auto root = fixture_root("project-inner-native-override-abi");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "message.gd",
               "extends RefCounted\nclass_name NativeMessageContract\n"
               "func New() -> NativeMessageContract:\n    return null\n"
               "func MergeFrom(other: NativeMessageContract) -> void:\n    pass\n");
    write_text(root / "generated.gd",
               "extends RefCounted\n"
               "class Packet extends NativeMessageContract:\n"
               "    func New() -> NativeMessageContract:\n        return Packet.new()\n"
               "    func MergeFrom(other: NativeMessageContract) -> void:\n        pass\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto generated =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "generated.gd";
        });
    REQUIRE(generated != result.scripts.end());
    const auto header =
        read_text(options.output_directory / "generated" / generated->header_file_name);
    REQUIRE(header.find("New() override;") != std::string::npos);
    REQUIRE(header.find("MergeFrom(godot::Ref<godot::RefCounted> other) override;") !=
            std::string::npos);
    REQUIRE(header.find("_gdpp_native_override_New") == std::string::npos);
    REQUIRE(header.find("_gdpp_native_override_MergeFrom") == std::string::npos);
}

TEST_CASE("project compiler dynamically dispatches ABI-changing internal overrides") {
    const auto root = fixture_root("project-inner-dynamic-dispatch");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "strategies.gd",
               "extends RefCounted\n"
               "class Base:\n"
               "    func transform(value: Node) -> Node:\n        return value\n"
               "class Derived extends Base:\n"
               "    func transform(value: Object) -> Node2D:\n        return null\n"
               "    func local_transform(value: Object) -> Node2D:\n"
               "        return transform(value)\n"
               "func dispatch(value: Base, input: Node) -> Node:\n"
               "    return value.transform(input)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(header.find("_gdpp_native_override_transform("
                        "gdpp::runtime::ObjectStorage<godot::Object> value)") != std::string::npos);
    REQUIRE(header.find("gdpp::runtime::ObjectStorage<godot::Object> value) override") ==
            std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("_gdpp_native_override_transform(") != std::string::npos);
}

TEST_CASE("project compiler preserves internal default vararg and super call ABIs") {
    const auto root = fixture_root("project-inner-call-abi");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "calls.gd", "extends RefCounted\n"
                                  "class Base:\n"
                                  "    func combine(value: int = 1) -> int:\n        return value\n"
                                  "    func collect(value: int = 2, ...extras: Array) -> int:\n"
                                  "        return value + extras.size()\n"
                                  "class Derived extends Base:\n"
                                  "    func combine(value: int = 1) -> int:\n"
                                  "        return super.combine(value) + 1\n"
                                  "    func collect(value: int = 2, ...extras: Array) -> int:\n"
                                  "        return value + extras.size() + 1\n"
                                  "    func local_collect() -> int:\n        return collect()\n"
                                  "func dispatch(value: Base) -> int:\n"
                                  "    return value.collect(3, 4, 5)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(header.find("combine(godot::Variant _gdpp_argument_value") != std::string::npos);
    REQUIRE(header.find("godot::Array extras) override;") != std::string::npos);
    REQUIRE(source.find("__Base::combine(") != std::string::npos);
    REQUIRE(source.find("godot::Array _gdpp_call_rest_") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"collect\")") != std::string::npos);
}

TEST_CASE("project compiler normalizes nested enum identities across typed containers") {
    const auto root = fixture_root("project-inner-enum-container-identity");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "messages.gd",
               "extends RefCounted\n"
               "class Message:\n"
               "    enum Status { UNKNOWN, READY }\n"
               "class Collection:\n"
               "    var values: Array[Message.Status] = []\n"
               "    func first() -> Message.Status:\n"
               "        if values.is_empty():\n            return Message.Status.UNKNOWN\n"
               "        return values[0]\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(source.find("int64_t ") != std::string::npos);
    REQUIRE(source.find("::first()") != std::string::npos);
}

TEST_CASE("project compiler rejects incompatible script override contracts") {
    const auto root = fixture_root("project-invalid-overrides");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name InvalidOverrideBase\n"
                                 "func arity(value: int, optional: int = 1) -> int: return value\n"
                                 "func input(value: Node) -> void: pass\n"
                                 "func output() -> Node2D: return null\n"
                                 "func qualifier(value: int) -> int: return value\n");
    write_text(root / "child.gd",
               "extends InvalidOverrideBase\nclass_name InvalidOverrideChild\n"
               "func arity(value: int, optional: int, required: int) -> int: return value\n"
               "func input(value: Node2D) -> void: pass\n"
               "func output() -> Node: return null\n"
               "static func qualifier(value: int) -> int: return value\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));
    const auto has_code = [&](const std::string_view code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto& item) { return item.diagnostic.code == code; });
    };

    REQUIRE(!result.success);
    REQUIRE(has_code("GDS4102"));
    REQUIRE(has_code("GDS4120"));
    REQUIRE(has_code("GDS4121"));
    REQUIRE(has_code("GDS4143"));
}

TEST_CASE("project compiler rejects missing and cyclic script bases transactionally") {
    const auto missing_root = fixture_root("project-missing-base");
    std::error_code error;
    std::filesystem::remove_all(missing_root, error);
    write_text(missing_root / "child.gd",
               "extends MissingProjectBase\nclass_name MissingBaseChild\n");
    auto options = project_options(missing_root);
    const auto missing = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!missing.success);
    REQUIRE(!missing.diagnostics.empty());
    REQUIRE(!std::filesystem::exists(options.output_directory / "manifest.txt"));

    const auto cycle_root = fixture_root("project-inheritance-cycle");
    std::filesystem::remove_all(cycle_root, error);
    write_text(cycle_root / "first.gd", "extends SecondCycle\nclass_name FirstCycle\n");
    write_text(cycle_root / "second.gd", "extends FirstCycle\nclass_name SecondCycle\n");
    options = project_options(cycle_root);
    const auto cycle = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!cycle.success);
    REQUIRE(!cycle.diagnostics.empty());
    REQUIRE(!std::filesystem::exists(options.output_directory / "manifest.txt"));
}

TEST_CASE("project compiler rejects third-party bases without runtime metadata") {
    const auto root = fixture_root("project-external-native-base");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension",
               "[configuration]\nentry_symbol=\"vendor_init\"\n");
    write_text(root / "child.gd", "extends VendorNativeType\nclass_name VendorNativeChild\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) {
        return item.diagnostic.code == "PRJ0018" &&
               item.diagnostic.message.find("active ClassDB snapshot or gdpp_bridge.json") !=
                   std::string::npos;
    }));
}

TEST_CASE("project compiler maps super calls through inherited engine virtual implementations") {
    const auto root = fixture_root("project-inherited-engine-virtual-super");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\n"
                                 "class_name EngineVirtualBase\n"
                                 "func _ready() -> void:\n"
                                 "    pass\n");
    write_text(root / "middle.gd", "extends EngineVirtualBase\n"
                                   "class_name EngineVirtualMiddle\n");
    write_text(root / "leaf.gd", "extends EngineVirtualMiddle\n"
                                 "class_name EngineVirtualLeaf\n"
                                 "func _ready() -> void:\n"
                                 "    super._ready()\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto leaf =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "leaf.gd";
        });
    REQUIRE(leaf != result.scripts.end());
    const auto source = read_text(options.output_directory / "generated" / leaf->source_file_name);
    const auto middle_native = native_class_for(result, "middle.gd");
    REQUIRE(source.find(middle_native + "::_gdpp_virtual_impl__ready()") != std::string::npos);
    REQUIRE(source.find(middle_native + "::_ready()") == std::string::npos);
}

TEST_CASE("project compiler attaches scripts to third-party GDExtension instances") {
    const auto root = fixture_root("project-extension-bridge");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension",
               "[configuration]\nentry_symbol=\"vendor_init\"\n");
    write_text(root / "addons/vendor/include/vendor_base.hpp",
               "#pragma once\nnamespace vendor { class VendorBase {}; }\n");
    write_text(root / "addons/vendor/lib/libvendor.a", "bridge library fixture\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\n"
               "  \"schema\": 1,\n"
               "  \"provider\": \"vendor.gdextension\",\n"
               "  \"abi\": \"vendor-abi-v1\",\n"
               "  \"godot_minimum\": \"4.4\",\n"
               "  \"classes\": [{\"gdscript_name\": \"VendorBase\", "
               "\"cpp_type\": \"vendor::VendorBase\", "
               "\"header\": \"include/vendor_base.hpp\", \"godot_base\": \"Node\", "
               "\"methods\": [{\"name\": \"answer\", \"return_type\": \"int\", "
               "\"hash\": 305419896}, {\"name\": \"native_answer\", "
               "\"return_type\": \"int\", \"hash\": 2271560481}]}],\n"
               "  \"targets\": []\n"
               "}\n");
    write_text(root / "derived.gd", "extends VendorBase\nclass_name BridgedDerived\n"
                                    "class VendorWorker:\n"
                                    "    extends VendorBase\n"
                                    "    func answer() -> int:\n"
                                    "        return super.answer() + 2\n"
                                    "func answer() -> int:\n"
                                    "    return super.answer() + 1\n"
                                    "func call_native() -> int:\n"
                                    "    return native_answer()\n"
                                    "func call_native_explicit() -> int:\n"
                                    "    return self.native_answer()\n"
                                    "func make_worker() -> VendorWorker:\n"
                                    "    return VendorWorker.new()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE(result.scripts.front().is_attached);
    REQUIRE_EQ(result.scripts.front().external_base_name, "VendorBase");
    const auto header = read_text(options.output_directory / "generated/bridged_derived.gd.hpp");
    const auto source = read_text(options.output_directory / "generated/bridged_derived.gd.cpp");
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(header.find("public gdpp::runtime::AttachedScriptBehavior") != std::string::npos);
    REQUIRE(header.find("vendor_base.hpp") == std::string::npos);
    REQUIRE(source.find("descriptor.native_base_type = godot::StringName(\"VendorBase\")") !=
            std::string::npos);
    REQUIRE(source.find("res://derived.gd::VendorWorker") != std::string::npos);
    const auto first_native_base =
        source.find("descriptor.native_base_type = godot::StringName(\"VendorBase\")");
    REQUIRE(first_native_base != std::string::npos);
    REQUIRE(source.find("descriptor.native_base_type = godot::StringName(\"VendorBase\")",
                        first_native_base + 1) != std::string::npos);
    REQUIRE(source.find("godot::Ref<gdpp::runtime::AttachedScriptBehavior>") != std::string::npos);
    REQUIRE(source.find("call_attached_native_base") != std::string::npos);
    REQUIRE(source.find("static_cast<std::uint32_t>(305419896)") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::to_variant(owner())") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"native_answer\")") != std::string::npos);
    REQUIRE(!std::filesystem::exists(options.output_directory / "CMakeLists.txt"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "gdpp_project.gdextension"));
    REQUIRE(registration.find("register_attached_script") != std::string::npos);
    REQUIRE(registration.find("register_singleton") != std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledLanguage)") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(gdpp::runtime::AttachedCompiledScript)") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(gdpp::runtime::AttachedScriptResourceLoader)") !=
            std::string::npos);
    REQUIRE(registration.find("register_attached_script_resource_loader") != std::string::npos);
    REQUIRE(registration.find("unregister_attached_script_resource_loader") != std::string::npos);
    REQUIRE(registration.find("unregister_attached_script_resource_loader") <
            registration.find("unregister_all_attached_scripts"));
    REQUIRE(registration.find("GDREGISTER_CLASS(gdpp::runtime::CoroutineFunctionState)") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(" + result.scripts.front().class_name + ")") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(" +
                              result.scripts.front().inner_class_names.front() + ")") !=
            std::string::npos);
}

TEST_CASE("project compiler dynamically bridges a binary-only GDExtension class") {
    const auto root = fixture_root("project-runtime-extension-bridge");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension",
               "[configuration]\nentry_symbol=\"vendor_init\"\n");
    const auto write_bridge = [&](const std::string& abi, const std::string& count_type = "int",
                                  const std::int64_t format_pcm = 1) {
        write_text(root / "addons/vendor/gdpp_bridge.json",
                   "{\n"
                   "  \"schema\": 1,\n"
                   "  \"provider\": \"vendor\\u002egdextension\",\n"
                   "  \"abi\": \"" +
                       abi +
                       "\",\n"
                       "  \"godot_minimum\": \"4.4\",\n"
                       "  \"classes\": [{\"gdscript_name\": \"Vendor\\u0044ata\", "
                       "\"godot_base\": \"Ref\\u0043ounted\", \"mode\": \"runtime\", "
                       "\"members_complete\": true, "
                       "\"properties\": [{\"name\":\"label\",\"type\":\"String\"},"
                       "{\"name\":\"sample_count\",\"type\":\"int\","
                       "\"read_only\":true}], "
                       "\"methods\": [{\"name\":\"clear_data\","
                       "\"return_type\":\"void\"},{\"name\":\"get_count\","
                       "\"return_type\":\"" +
                       count_type +
                       "\",\"parameters\":[{\"name\":\"scale\",\"type\":\"int\"}]}], "
                       "\"signals\": [{\"name\":\"changed\",\"parameters\":[]}], "
                       "\"enums\": [{\"name\":\"Format\",\"bitfield\":false,"
                       "\"values\":[{\"name\":\"FORMAT_PCM\",\"value\":" +
                       std::to_string(format_pcm) +
                       "},"
                       "{\"name\":\"FORMAT_FLOAT\",\"value\":2},"
                       "{\"name\":\"FORMAT_EXACT\",\"value\":9007199254740993}]}] }]\n"
                       "}\n");
    };
    write_bridge("vendor-runtime-v1");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "class_name RuntimeBridgeConsumer\n"
                                     "var data: VendorData\n"
                                     "var format: VendorData.Format = "
                                     "VendorData.Format.FORMAT_PCM\n"
                                     "var exact: VendorData.Format = VendorData.FORMAT_EXACT\n"
                                     "func create() -> bool:\n"
                                     "    data = VendorData.new()\n"
                                     "    if data is VendorData:\n"
                                     "        data.label = \"ready\"\n"
                                     "        data.clear_data()\n"
                                     "        return true\n"
                                     "    return false\n"
                                     "func count() -> int:\n"
                                     "    return data.get_count(2)\n"
                                     "func method_reference() -> Callable:\n"
                                     "    return data.clear_data\n"
                                     "func change_signal() -> Signal:\n"
                                     "    return data.changed\n");
    const auto options = project_options(root);

    const auto first = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(first.success);
    const auto source =
        read_text(options.output_directory / "generated/runtime_bridge_consumer.gd.cpp");
    REQUIRE(source.find("instantiate_external_class_at") != std::string::npos);
    REQUIRE(source.find("is_external_instance") != std::string::npos);
    REQUIRE(source.find("call_dynamic") != std::string::npos);
    REQUIRE(source.find("external_callable_at") != std::string::npos);
    REQUIRE(source.find("external_signal_at") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::ScriptSourceLocation{_gdpp_source_path") !=
            std::string::npos);
    REQUIRE(source.find("format = 1;") != std::string::npos);
    REQUIRE(source.find("exact = 9007199254740993;") != std::string::npos);
    const auto lock = read_text(options.output_directory / "bridge.lock");
    REQUIRE(lock.find("runtime\n") != std::string::npos);

    const auto cached = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(cached.success);
    REQUIRE_EQ(cached.cache_hit_count, std::size_t{1});

    write_bridge("vendor-runtime-v1", "int", 7);
    const auto enum_changed = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(enum_changed.success);
    REQUIRE_EQ(enum_changed.compiled_count, std::size_t{1});
    REQUIRE(read_text(options.output_directory / "generated/runtime_bridge_consumer.gd.cpp")
                .find("format = 7;") != std::string::npos);

    write_bridge("vendor-runtime-v1", "float");
    const auto contract_changed = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(contract_changed.success);
    REQUIRE_EQ(contract_changed.compiled_count, std::size_t{1});
    REQUIRE_EQ(contract_changed.cache_hit_count, std::size_t{0});

    write_bridge("vendor-runtime-v2");
    const auto abi_changed = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(abi_changed.success);
    REQUIRE_EQ(abi_changed.compiled_count, std::size_t{1});
    REQUIRE_EQ(abi_changed.cache_hit_count, std::size_t{0});
}

TEST_CASE("project compiler propagates provider editor-only contracts") {
    const auto root = fixture_root("project-editor-only-extension-bridge");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension",
               "[configuration]\nentry_symbol=\"vendor_init\"\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\","
               "\"abi\":\"vendor-editor-v1\",\"godot_minimum\":\"4.4\","
               "\"classes\":[{\"gdscript_name\":\"VendorEditorBase\","
               "\"godot_base\":\"Node\",\"mode\":\"runtime\","
               "\"editor_only\":true}]}\n");
    write_text(root / "consumer.gd", "@tool\nextends VendorEditorBase\n"
                                     "class_name VendorEditorConsumer\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    REQUIRE(result.scripts.front().is_attached);
    REQUIRE(result.scripts.front().is_editor_only);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("if (gdpp_editor_environment) {\n        GDREGISTER_CLASS(" +
                              result.scripts.front().class_name + ");\n    }") !=
            std::string::npos);
    REQUIRE(registration.find("if (gdpp_editor_environment) {\n        {\n            "
                              "godot::String error;") != std::string::npos);

    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\","
               "\"abi\":\"vendor-editor-v1\",\"godot_minimum\":\"4.4\","
               "\"classes\":[{\"gdscript_name\":\"VendorEditorBase\","
               "\"godot_base\":\"Node\",\"mode\":\"runtime\","
               "\"editor_only\":\"yes\"}]}\n");
    const auto malformed = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!malformed.success);
    REQUIRE(std::any_of(
        malformed.diagnostics.begin(), malformed.diagnostics.end(), [](const auto& item) {
            return item.diagnostic.message.find("field 'editor_only' must be boolean") !=
                   std::string::npos;
        }));
}

TEST_CASE("project compiler consumes runtime contracts reflected from ClassDB") {
    const auto root = fixture_root("project-reflected-classdb-contract");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "waveform_consumer.gd",
               "extends Node\n"
               "class_name WaveformConsumer\n"
               "var data: WaveformData\n"
               "var data_items: Array[WaveformData] = []\n"
               "var data_by_name: Dictionary[String, WaveformData] = {}\n"
               "@export var format: WaveformData.Format = WaveformData.Format.FORMAT_PCM\n"
               "@export var channels: WaveformData.Channels = "
               "WaveformData.Channels.CHANNEL_LEFT | WaveformData.CHANNEL_RIGHT\n"
               "func prepare() -> int:\n"
               "    data = WaveformData.new()\n"
               "    data.label = \"ready\"\n"
               "    data.clear_data()\n"
               "    format = data.get_format()\n"
               "    return WaveformData.FORMAT_BIAS + WaveformData.get_format_version() + "
               "data.get_sample_count()\n"
               "func preserve(values: Array[WaveformData]) -> Array[WaveformData]:\n"
               "    data_items = values\n"
               "    return data_items\n");

    gdpp::ExtensionBridge reflected;
    reflected.manifest_path = root / ".godot/gdpp_classdb/WaveformData.runtime";
    reflected.provider = "ClassDB";
    reflected.abi = "classdb:WaveformData";
    reflected.contract_hash = gdpp::sha256("WaveformData-v1");
    gdpp::ExtensionBridgeClass waveform;
    waveform.gdscript_name = "WaveformData";
    waveform.godot_base = "RefCounted";
    waveform.runtime_only = true;
    waveform.members_complete = true;
    waveform.members.push_back(
        {gdpp::ExtensionBridgeMemberKind::property, "label", "String", {}, false, false});
    waveform.members.push_back(
        {gdpp::ExtensionBridgeMemberKind::method, "clear_data", "void", {}, false, false});
    waveform.members.push_back(
        {gdpp::ExtensionBridgeMemberKind::method, "get_sample_count", "int", {}, false, false});
    waveform.members.push_back({gdpp::ExtensionBridgeMemberKind::method,
                                "get_format",
                                "WaveformData.Format",
                                {},
                                false,
                                false});
    waveform.members.push_back({gdpp::ExtensionBridgeMemberKind::method,
                                "get_format_version",
                                "int",
                                {},
                                false,
                                false,
                                true});
    waveform.members.push_back({gdpp::ExtensionBridgeMemberKind::constant,
                                "FORMAT_BIAS",
                                "int",
                                {},
                                true,
                                false,
                                true,
                                40});
    waveform.enums.push_back({"Format", false, {{"FORMAT_PCM", 1}, {"FORMAT_FLOAT", 2}}});
    waveform.enums.push_back({"Channels", true, {{"CHANNEL_LEFT", 1}, {"CHANNEL_RIGHT", 2}}});
    reflected.classes.push_back(std::move(waveform));

    auto options = project_options(root);
    options.reflected_extension_bridges.push_back(std::move(reflected));
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto source = read_text(options.output_directory / "generated/waveform_consumer.gd.cpp");
    const auto header = read_text(options.output_directory / "generated/waveform_consumer.gd.hpp");
    REQUIRE(source.find("instantiate_external_class_at") != std::string::npos);
    REQUIRE(source.find("call_dynamic") != std::string::npos);
    REQUIRE(source.find("call_external_static_at") != std::string::npos);
    REQUIRE(source.find("gdpp::integer::add(") != std::string::npos);
    REQUIRE(source.find(" = 40;") != std::string::npos);
    REQUIRE(source.find("FORMAT_PCM:1,FORMAT_FLOAT:2") != std::string::npos);
    REQUIRE(source.find("CHANNEL_LEFT:1,CHANNEL_RIGHT:2") != std::string::npos);
    REQUIRE(source.find("godot::PROPERTY_HINT_FLAGS") != std::string::npos);
    REQUIRE(header.find("struct ContainerObjectTag_WaveformData") != std::string::npos);
    REQUIRE(header.find("godot::StringName(\"WaveformData\")") != std::string::npos);
    REQUIRE(header.find("godot::TypedArray<waveform_consumer_gdpp_detail::"
                        "ContainerObjectTag_WaveformData>") != std::string::npos);
    REQUIRE(header.find("godot::TypedDictionary<godot::String, "
                        "waveform_consumer_gdpp_detail::ContainerObjectTag_WaveformData>") !=
            std::string::npos);
    const auto lock = read_text(options.output_directory / "bridge.lock");
    REQUIRE(lock.find("classdb:WaveformData") != std::string::npos);

    write_text(root / "waveform_consumer.gd", "extends Node\nvar format: WaveformData.Format = "
                                              "WaveformData.Format.MISSING\n");
    const auto invalid = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!invalid.success);
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "GDS4041"; }));
}

TEST_CASE("project compiler rejects third-party bridge namespace collisions") {
    const auto root = fixture_root("project-extension-bridge-collision");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension", "[configuration]\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\","
               "\"abi\":\"vendor-runtime-v1\",\"godot_minimum\":\"4.4\","
               "\"classes\":[{\"gdscript_name\":\"VendorData\","
               "\"godot_base\":\"RefCounted\",\"mode\":\"runtime\"}]}\n");
    write_text(root / "consumer.gd", "extends Node\nclass_name VendorData\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) {
        return item.diagnostic.code == "PRJ0023" &&
               item.diagnostic.message.find("global script class") != std::string::npos;
    }));
    REQUIRE(!std::filesystem::exists(project_options(root).output_directory / "manifest.txt"));
}

TEST_CASE("runtime bridge rejects malformed third-party enum contracts") {
    const auto root = fixture_root("project-runtime-extension-enum-errors");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension", "[configuration]\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\","
               "\"abi\":\"vendor-runtime-v1\",\"godot_minimum\":\"4.4\","
               "\"classes\":[{\"gdscript_name\":\"VendorData\","
               "\"godot_base\":\"RefCounted\",\"mode\":\"runtime\","
               "\"enums\":[{\"name\":\"Format\",\"values\":["
               "{\"name\":\"PCM\",\"value\":1},{\"name\":\"PCM\",\"value\":2}]}]}]}\n");
    write_text(root / "consumer.gd", "extends Node\nvar data: VendorData\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& item) {
        return item.diagnostic.code == "PRJ0020" &&
               item.diagnostic.message.find("invalid or duplicate value") != std::string::npos;
    }));
    REQUIRE(!std::filesystem::exists(project_options(root).output_directory / "manifest.txt"));
}

TEST_CASE("complete runtime bridge contracts reject typos and read-only writes") {
    const auto root = fixture_root("project-runtime-extension-contract-errors");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension", "[configuration]\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\","
               "\"abi\":\"vendor-runtime-v1\",\"godot_minimum\":\"4.4\","
               "\"classes\":[{\"gdscript_name\":\"VendorData\","
               "\"godot_base\":\"RefCounted\",\"mode\":\"runtime\","
               "\"members_complete\":true,\"properties\":[{\"name\":\"count\","
               "\"type\":\"int\",\"read_only\":true}]}]}\n");
    write_text(root / "consumer.gd", "extends Node\nvar data: VendorData\n"
                                     "func invalid() -> void:\n"
                                     "    data.count = 2\n"
                                     "    data.misspelled()\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    const auto has_code = [&](const std::string& code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&](const auto& item) { return item.diagnostic.code == code; });
    };
    REQUIRE(has_code("GDS4112"));
    REQUIRE(has_code("GDS4113"));
}

TEST_CASE("project compiler hoists a script-local class used as the root base") {
    const auto root = fixture_root("project-local-root-base");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "renderer.gd", "class_name LocalRootDerived\n"
                                     "extends LocalRootBase\n"
                                     "func answer() -> int:\n    return base_value + 2\n"
                                     "@abstract class LocalRootBase:\n"
                                     "    extends Node2D\n"
                                     "    var base_value: int = 40\n"
                                     "    @abstract func answer() -> int\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.front().inner_class_names.size(), std::size_t{1});
    const auto header = read_text(options.output_directory / "generated/local_root_derived.gd.hpp");
    const auto source = read_text(options.output_directory / "generated/local_root_derived.gd.cpp");
    const auto& inner = result.scripts.front().inner_class_names.front();
    REQUIRE(header.find("class " + inner + " : public gdpp::runtime::AttachedScriptBehavior") !=
            std::string::npos);
    REQUIRE(header.find("class " + result.scripts.front().class_name + " : public " + inner) !=
            std::string::npos);
    REQUIRE(header.find("#include \"\"") == std::string::npos);
    REQUIRE(source.find("descriptor.native_base_type = godot::StringName(\"Node2D\")") !=
            std::string::npos);
    REQUIRE(source.find("descriptor.base_script_path = "
                        "godot::String(\"res://renderer.gd::LocalRootBase\")") !=
            std::string::npos);
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("GDREGISTER_ABSTRACT_CLASS(" + inner + ")") <
            registration.find("GDREGISTER_CLASS(" + result.scripts.front().class_name + ")"));

    write_text(root / "renderer.gd", "class_name LocalRootDerived\n"
                                     "extends LocalRootBase\n"
                                     "@abstract class LocalRootBase:\n"
                                     "    extends Node2D\n"
                                     "    @abstract func answer() -> int\n");
    const auto invalid = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!invalid.success);
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "GDS4149"; }));
}

TEST_CASE("attached bridges ignore provider build-system paths") {
    const auto root = fixture_root("project-extension-bridge-invalid");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/vendor/vendor.gdextension", "[configuration]\n");
    write_text(root / "outside.hpp", "// must not be reachable through ..\n");
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\",\"abi\":\"bad\","
               "\"godot_minimum\":\"4.4\",\"classes\":[{"
               "\"gdscript_name\":\"VendorBase\",\"cpp_type\":\"vendor::VendorBase\","
               "\"header\":\"../../../outside.hpp\",\"godot_base\":\"Node\"}],"
               "\"targets\":[]}\n");
    write_text(root / "derived.gd", "extends VendorBase\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    REQUIRE(header.find("outside.hpp") == std::string::npos);
}

#ifndef _WIN32
TEST_CASE("attached bridges never traverse provider header symlinks") {
    const auto root = fixture_root("project-extension-bridge-symlink");
    const auto outside = fixture_root("project-extension-bridge-symlink-outside");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(outside, error);
    write_text(root / "addons/vendor/vendor.gdextension", "[configuration]\n");
    write_text(outside / "vendor_base.hpp", "#pragma once\n");
    std::filesystem::create_directories(root / "addons/vendor");
    std::filesystem::create_directory_symlink(outside, root / "addons/vendor/include", error);
    REQUIRE(!error);
    write_text(root / "addons/vendor/gdpp_bridge.json",
               "{\"schema\":1,\"provider\":\"vendor.gdextension\",\"abi\":\"bad\","
               "\"godot_minimum\":\"4.4\",\"classes\":[{"
               "\"gdscript_name\":\"VendorBase\",\"cpp_type\":\"vendor::VendorBase\","
               "\"header\":\"include/vendor_base.hpp\",\"godot_base\":\"Node\"}],"
               "\"targets\":[]}\n");
    write_text(root / "derived.gd", "extends VendorBase\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto header =
        read_text(options.output_directory / "generated" / result.scripts.front().header_file_name);
    REQUIRE(header.find("vendor_base.hpp") == std::string::npos);
}
#endif

TEST_CASE("unnamed scripts inherit by path without becoming global classes") {
    const auto root = fixture_root("project-unnamed-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "node.gd", "extends Node\nfunc path_answer() -> int:\n    return 42\n");
    write_text(root / "child.gd", "extends \"node.gd\"\nclass_name PathInheritanceChild\n"
                                  "func answer() -> int:\n    return path_answer()\n");
    auto options = project_options(root);
    const auto path_result = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(path_result.success);
    REQUIRE_EQ(path_result.scripts.size(), std::size_t{2});
    REQUIRE(read_text(options.output_directory / "generated/path_inheritance_child.gd.hpp")
                .find("#include \"path_node_") != std::string::npos);

    const auto invalid_root = fixture_root("project-unnamed-global-base");
    std::filesystem::remove_all(invalid_root, error);
    write_text(invalid_root / "unnamed_base.gd", "extends Node\n");
    write_text(invalid_root / "child.gd", "extends UnnamedBase\nclass_name InvalidUnnamedChild\n");
    options = project_options(invalid_root);
    const auto global_result = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(!global_result.success);
    REQUIRE(!std::filesystem::exists(options.output_directory / "manifest.txt"));
}

TEST_CASE("project compiler gives same-named unnamed scripts path-stable native identities") {
    const auto root = fixture_root("project-unnamed-native-identities");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "combat/opponent.gd", "extends Node\nfunc role() -> int:\n    return 1\n");
    write_text(root / "movement/opponent.gd", "extends Node\nfunc role() -> int:\n    return 2\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{2});
    REQUIRE(result.scripts.at(0).class_name != result.scripts.at(1).class_name);
    REQUIRE(result.scripts.at(0).class_name.find("GDPPNative_Path_") == 0);
    REQUIRE(result.scripts.at(1).class_name.find("GDPPNative_Path_") == 0);
    REQUIRE(result.scripts.at(0).header_file_name != result.scripts.at(1).header_file_name);
}

TEST_CASE("global class types win over same-stem embedded scripts in typed containers") {
    const auto root = fixture_root("project-global-container-shadow");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "timeline_action.gd",
               "extends Resource\nclass_name TimelineAction\n"
               "@export var progress: float\n@export var action: String\n");
    write_text(root / "timeline_action.tres",
               "[gd_resource type=\"Resource\" load_steps=2 format=3]\n\n"
               "[sub_resource type=\"GDScript\" id=\"GDScript_shadow\"]\n\n"
               "[resource]\nscript = SubResource(\"GDScript_shadow\")\n");
    write_text(root / "enemy.gd",
               "extends Node\nclass_name TypedContainerEnemy\n"
               "@export var actions: Array[TimelineAction]\n"
               "func next_progress() -> float:\n    return actions[0].progress\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;

    const auto initial = compiler.compile(options);

    REQUIRE(initial.success);
    REQUIRE_EQ(initial.scripts.size(), std::size_t{3});
    const auto consumer =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"enemy.gd"};
        });
    REQUIRE(consumer != initial.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"timeline_action.gd"});
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(source.find("cast_to<godot::Variant>") == std::string::npos);
    REQUIRE(source.find("strict_attached_script_storage") != std::string::npos);
    REQUIRE(source.find("godot::String(\"res://timeline_action.gd\")") != std::string::npos);
    REQUIRE(source.find("get_named(") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"progress\")") != std::string::npos);
    const auto timeline =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"timeline_action.gd"};
        });
    REQUIRE(timeline != initial.scripts.end());
    REQUIRE(source.find("#include \"" + timeline->header_file_name + "\"") != std::string::npos);

    write_text(root / "timeline_action.gd",
               "extends Resource\nclass_name TimelineAction\n"
               "@export var progress: int\n@export var action: String\n");
    const auto abi_change = compiler.compile(options);
    REQUIRE(abi_change.success);
    REQUIRE_EQ(abi_change.compiled_count, std::size_t{2});
    REQUIRE_EQ(abi_change.cache_hit_count, std::size_t{1});
}

TEST_CASE("project script member graph reports invalid static and typed calls") {
    const auto root = fixture_root("project-member-diagnostics");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name MemberBase\n"
                                 "func instance_answer() -> int:\n    return 42\n"
                                 "static func compute(value: int) -> int:\n    return value\n");
    write_text(root / "child.gd", "extends MemberBase\nclass_name MemberChild\n"
                                  "var invalid_type: MissingProjectType\n"
                                  "func invalid_calls() -> int:\n"
                                  "    MemberBase.instance_answer()\n"
                                  "    MemberBase.compute()\n"
                                  "    MemberBase.missing()\n"
                                  "    return MemberBase.compute(\"bad\")\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    const auto has_code = [&result](const std::string& code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&code](const gdpp::ProjectDiagnostic& diagnostic) {
                               return diagnostic.diagnostic.code == code;
                           });
    };
    REQUIRE(has_code("GDS4055"));
    REQUIRE(has_code("GDS4056"));
    REQUIRE(has_code("GDS4054"));
    REQUIRE(has_code("GDS4059"));
    REQUIRE(has_code("GDS4002"));
}

TEST_CASE("project compiler preserves cross-script call contracts through cache invalidation") {
    const auto root = fixture_root("project-cross-script-call-contract");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "parser.gd", "extends RefCounted\n"
                                   "class_name ProjectPacketParser\n"
                                   "func parse(value: PackedByteArray) -> PackedByteArray:\n"
                                   "    return value\n");
    write_text(root / "consumer.gd", "extends RefCounted\n"
                                     "class_name ProjectPacketConsumer\n"
                                     "var parser := ProjectPacketParser.new()\n"
                                     "func parse(value: Variant) -> PackedByteArray:\n"
                                     "    return parser.parse(value)\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;

    const auto initial = compiler.compile(options);

    REQUIRE(initial.success);
    REQUIRE_EQ(initial.compiled_count, std::size_t{2});
    const auto consumer =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(consumer != initial.scripts.end());
    const auto initial_source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(initial_source.find(
                "gdpp::runtime::strict_packed_array_storage<godot::PackedByteArray>(") !=
            std::string::npos);
    REQUIRE(initial_source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(initial_source.find("godot::StringName(\"parse\")") != std::string::npos);
    REQUIRE(initial_source.find("gdpp::runtime::to_variant(value)") != std::string::npos);

    write_text(root / "parser.gd", "extends RefCounted\n"
                                   "class_name ProjectPacketParser\n"
                                   "func parse(value: PackedInt32Array) -> PackedByteArray:\n"
                                   "    return PackedByteArray()\n");
    const auto changed = compiler.compile(options);

    REQUIRE(changed.success);
    REQUIRE_EQ(changed.compiled_count, std::size_t{2});
    REQUIRE_EQ(changed.cache_hit_count, std::size_t{0});
    const auto changed_consumer =
        std::find_if(changed.scripts.begin(), changed.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(changed_consumer != changed.scripts.end());
    const auto changed_source =
        read_text(options.output_directory / "generated" / changed_consumer->source_file_name);
    REQUIRE(changed_source.find("godot::StringName(\"parse\")") != std::string::npos);
    REQUIRE(changed_source.find("gdpp::runtime::to_variant(value)") != std::string::npos);
}

TEST_CASE("attached cross-script properties recover their semantic native value types") {
    const auto root = fixture_root("project-attached-cross-script-property-types");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "record.gd", "extends Node\n"
                                   "class_name AttachedPropertyRecord\n"
                                   "var values: Dictionary[Vector3i, int] = {}\n");
    write_text(root / "consumer.gd",
               "extends Node\n"
               "class_name AttachedPropertyConsumer\n"
               "func contains(record: AttachedPropertyRecord, key: Vector3i) -> bool:\n"
               "    return record.values.has(key)\n"
               "func read(record: AttachedPropertyRecord, key: Vector3i) -> int:\n"
               "    return record.values[key]\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(consumer != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(source.find("gdpp::runtime::get_named(") != std::string::npos);
    REQUIRE(source.find("strict_typed_storage<"
                        "godot::TypedDictionary<godot::Vector3i, int64_t>>"
                        "(gdpp::runtime::to_variant(") != std::string::npos);
    REQUIRE(source.find(".has(") != std::string::npos);
}

TEST_CASE("attached internal classes dispatch self locally and other instances through scripts") {
    const auto root = fixture_root("project-attached-inner-dispatch");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "records.gd", "extends Node\n"
                                    "class Record:\n"
                                    "    var value: int\n"
                                    "    func _init(initial: int) -> void:\n"
                                    "        self.value = initial\n"
                                    "    func increment() -> int:\n"
                                    "        self.value += 1\n"
                                    "        return self.value\n"
                                    "func read(record: Record) -> int:\n"
                                    "    return record.value\n"
                                    "func write(record: Record, next: int) -> void:\n"
                                    "    record.value = next\n"
                                    "func invoke(record: Record) -> int:\n"
                                    "    return record.increment()\n"
                                    "func create(initial: int) -> Record:\n"
                                    "    return Record.new(initial)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(source.find("owner()->_gdpp_") == std::string::npos);
    REQUIRE(source.find("record->_gdpp_get_value") == std::string::npos);
    REQUIRE(source.find("record->_gdpp_set_value") == std::string::npos);
    REQUIRE(source.find("gdpp::runtime::get_named(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::set_named(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("-> godot::Ref<godot::RefCounted>") != std::string::npos);
    REQUIRE(source.find("property.getter = [](") != std::string::npos);
    REQUIRE(source.find("typed->_gdpp_get_value()") != std::string::npos);
    REQUIRE(source.find("property.setter = [](") != std::string::npos);
    const auto setter = source.find("property.setter = [](");
    const auto scope = source.find("ScriptFaultPolicy::inherit_existing", setter);
    const auto conversion = source.find("const auto converted =", scope);
    const auto rejected = source.find("if (storage_scope.failed()) return false", conversion);
    const auto write = source.find("typed->_gdpp_set_value(converted)", rejected);
    REQUIRE(scope > setter);
    REQUIRE(conversion > scope);
    REQUIRE(rejected > conversion);
    REQUIRE(write > rejected);
    REQUIRE(source.find("typed->_gdpp_set_value(") != std::string::npos);
}

TEST_CASE("attached ref-counted self arguments retain a typed strong reference") {
    const auto root = fixture_root("project-attached-ref-self-argument");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends RefCounted\n"
                                 "class_name AttachedRefBase\n"
                                 "func consume(value: AttachedRefBase) -> bool:\n"
                                 "    return value == self\n");
    write_text(root / "records.gd", "extends Node\n"
                                    "class Record extends AttachedRefBase:\n"
                                    "    func verify() -> bool:\n"
                                    "        return consume(self)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto script =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& candidate) {
            return candidate.relative_path == std::filesystem::path{"records.gd"};
        });
    REQUIRE(script != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / script->source_file_name);
    REQUIRE(source.find("godot::Ref<godot::RefCounted>(godot::Object::cast_to<godot::RefCounted>("
                        "owner()))") != std::string::npos);
    REQUIRE(source.find("godot::Ref<godot::RefCounted> _gdpp_call_argument_") != std::string::npos);
    REQUIRE(source.find("godot::Ref<godot::RefCounted> _gdpp_call_argument_") <
            source.find("godot::Ref<godot::RefCounted>(godot::Object::cast_to<"
                        "godot::RefCounted>(owner()))"));
    const auto base_script =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& candidate) {
            return candidate.relative_path == std::filesystem::path{"base.gd"};
        });
    REQUIRE(base_script != result.scripts.end());
    const auto base_source =
        read_text(options.output_directory / "generated" / base_script->source_file_name);
    REQUIRE(base_source.find("gdpp::runtime::binary(godot::Variant::OP_EQUAL") !=
            std::string::npos);
}

TEST_CASE("attached breakpoint snapshots include inherited script members") {
    const auto root = fixture_root("project-attached-breakpoint-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\n"
                                 "var base_state := 4\n");
    write_text(root / "child.gd", "extends \"res://base.gd\"\n"
                                  "var child_state := 5\n"
                                  "func inspect(value: int) -> void:\n"
                                  "    breakpoint\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto child =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "child.gd";
        });
    REQUIRE(child != result.scripts.end());
    const auto source = read_text(options.output_directory / "generated" / child->source_file_name);
    REQUIRE(source.find("godot::StringName(\"inspect\"), 4, owner()") != std::string::npos);
    REQUIRE(source.find(".push_back(godot::String(\"child_state\"));") != std::string::npos);
    REQUIRE(source.find(".push_back(gdpp::runtime::to_variant(this->child_state));") !=
            std::string::npos);
    REQUIRE(source.find(".push_back(godot::String(\"base_state\"));") != std::string::npos);
    REQUIRE(source.find(".push_back(gdpp::runtime::to_variant(this->base_state));") !=
            std::string::npos);
}

TEST_CASE("attached script self calls use native virtual dispatch without bypassing peer scripts") {
    const auto root = fixture_root("project-attached-self-dispatch");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "worker.gd",
               "extends Node\n"
               "class_name AttachedSelfDispatch\n"
               "func mix(value: int) -> int:\n"
               "    return value * 3\n"
               "func implicit_call(value: int) -> int:\n"
               "    return mix(value)\n"
               "func explicit_call(value: int) -> int:\n"
               "    return self.mix(value)\n"
               "func peer_call(peer: AttachedSelfDispatch, value: int) -> int:\n"
               "    return peer.mix(value)\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{1});
    const auto source =
        read_text(options.output_directory / "generated" / result.scripts.front().source_file_name);
    REQUIRE(source.find("this->mix(") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::to_variant(this)") == std::string::npos);
    REQUIRE(source.find("gdpp::runtime::to_variant(peer)") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::to_variant(owner())") == std::string::npos);
}

TEST_CASE("project symbol signature changes invalidate dependent script caches") {
    const auto root = fixture_root("project-symbol-cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name SignatureBase\n"
                                 "static func value() -> int:\n    return 42\n");
    write_text(root / "child.gd", "extends Node\nclass_name SignatureChild\n"
                                  "func answer() -> int:\n    return SignatureBase.value()\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    const auto manifest_before = read_text(options.output_directory / "manifest.txt");

    write_text(root / "base.gd", "extends Node\nclass_name SignatureBase\n"
                                 "static func value() -> String:\n    return \"changed\"\n");
    const auto incompatible = compiler.compile(options);

    REQUIRE(!incompatible.success);
    REQUIRE_EQ(read_text(options.output_directory / "manifest.txt"), manifest_before);
    REQUIRE(std::any_of(incompatible.diagnostics.begin(), incompatible.diagnostics.end(),
                        [](const gdpp::ProjectDiagnostic& diagnostic) {
                            return diagnostic.diagnostic.code == "GDS4002";
                        }));
}

TEST_CASE("project variadic ABI changes invalidate dependent script caches") {
    const auto root = fixture_root("project-vararg-abi-cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends RefCounted\nclass_name VarargCacheBase\n"
                                 "func collect(value: int) -> int:\n    return value\n");
    write_text(root / "consumer.gd", "extends RefCounted\nclass_name VarargCacheConsumer\n"
                                     "var source: VarargCacheBase\n"
                                     "func read() -> int:\n    return source.collect(1)\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.compiled_count, std::size_t{2});

    write_text(root / "base.gd", "extends RefCounted\nclass_name VarargCacheBase\n"
                                 "func collect(value: int, ...extras: Array) -> int:\n"
                                 "    return value + extras.size()\n");
    const auto changed = compiler.compile(options);

    REQUIRE(changed.success);
    REQUIRE_EQ(changed.compiled_count, std::size_t{2});
    REQUIRE_EQ(changed.cache_hit_count, std::size_t{0});
    REQUIRE(native_class_for(changed, "base.gd") != native_class_for(initial, "base.gd"));
}

TEST_CASE("project coroutine ABI changes invalidate callers and require cross-script await") {
    const auto root = fixture_root("project-coroutine-abi-cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto synchronous_producer = "extends RefCounted\nclass_name CoroutineProducer\n"
                                      "signal resumed\n"
                                      "func produce() -> int:\n    return 1\n";
    const auto asynchronous_producer = "extends RefCounted\nclass_name CoroutineProducer\n"
                                       "signal resumed\n"
                                       "func produce() -> int:\n    await resumed\n    return 1\n";
    const auto direct_consumer = "extends RefCounted\nclass_name CoroutineConsumer\n"
                                 "var producer: CoroutineProducer\n"
                                 "func consume() -> int:\n    return producer.produce()\n";
    const auto awaited_consumer = "extends RefCounted\nclass_name CoroutineConsumer\n"
                                  "var producer: CoroutineProducer\n"
                                  "func consume() -> int:\n    return await producer.produce()\n";
    write_text(root / "producer.gd", synchronous_producer);
    write_text(root / "consumer.gd", direct_consumer);
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.compiled_count, std::size_t{2});
    const auto committed_manifest = read_text(options.output_directory / "manifest.txt");

    write_text(root / "producer.gd", asynchronous_producer);
    const auto missing_await = compiler.compile(options);
    REQUIRE(!missing_await.success);
    REQUIRE_EQ(read_text(options.output_directory / "manifest.txt"), committed_manifest);
    REQUIRE(std::any_of(missing_await.diagnostics.begin(), missing_await.diagnostics.end(),
                        [](const gdpp::ProjectDiagnostic& diagnostic) {
                            return diagnostic.diagnostic.code == "GDS4132";
                        }));

    write_text(root / "consumer.gd", awaited_consumer);
    const auto migrated = compiler.compile(options);
    REQUIRE(migrated.success);
    REQUIRE_EQ(migrated.compiled_count, std::size_t{2});
    REQUIRE_EQ(migrated.cache_hit_count, std::size_t{0});
    REQUIRE(native_class_for(migrated, "producer.gd") != native_class_for(initial, "producer.gd"));
    const auto consumer =
        std::find_if(migrated.scripts.begin(), migrated.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != migrated.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"producer.gd"});
    const auto generated =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(generated.find("gdpp::runtime::is_awaitable(") != std::string::npos);
    REQUIRE(generated.find("gdpp::runtime::await_result(") != std::string::npos);

    write_text(root / "producer.gd", "extends RefCounted\nclass_name CoroutineProducer\n"
                                     "signal resumed\n"
                                     "func produce() -> int:\n    await resumed\n    return 2\n");
    const auto implementation_change = compiler.compile(options);
    REQUIRE(implementation_change.success);
    REQUIRE_EQ(implementation_change.compiled_count, std::size_t{1});
    REQUIRE_EQ(implementation_change.cache_hit_count, std::size_t{1});
}

TEST_CASE("project symbols classify awaited defaults as coroutine ABI") {
    const auto root = fixture_root("project-awaited-default-coroutine");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "producer.gd", "extends RefCounted\n"
                                     "class_name DefaultCoroutineProducer\n"
                                     "signal resumed(value)\n"
                                     "func produce(value: int = await resumed) -> int:\n"
                                     "    return value\n");
    write_text(root / "consumer.gd", "extends RefCounted\n"
                                     "class_name DefaultCoroutineConsumer\n"
                                     "var producer: DefaultCoroutineProducer\n"
                                     "func consume() -> int:\n"
                                     "    return producer.produce()\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto direct = compiler.compile(options);
    REQUIRE(!direct.success);
    REQUIRE(std::any_of(direct.diagnostics.begin(), direct.diagnostics.end(),
                        [](const gdpp::ProjectDiagnostic& diagnostic) {
                            return diagnostic.diagnostic.code == "GDS4132";
                        }));

    write_text(root / "consumer.gd", "extends RefCounted\n"
                                     "class_name DefaultCoroutineConsumer\n"
                                     "var producer: DefaultCoroutineProducer\n"
                                     "func consume() -> int:\n"
                                     "    return await producer.produce()\n");
    const auto awaited = compiler.compile(options);
    REQUIRE(awaited.success);
    const auto consumer =
        std::find_if(awaited.scripts.begin(), awaited.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != awaited.scripts.end());
    const auto generated =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(generated.find("gdpp::runtime::is_awaitable(") != std::string::npos);
    REQUIRE(generated.find("gdpp::runtime::await_result(") != std::string::npos);
}

TEST_CASE("project symbols propagate coroutine property accessors across scripts") {
    const auto root = fixture_root("project-coroutine-property");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends RefCounted\n"
                                 "class_name CoroutinePropertyBase\n"
                                 "signal resumed(value)\n"
                                 "func read_value() -> int:\n"
                                 "    return await resumed\n");
    write_text(root / "producer.gd", "extends CoroutinePropertyBase\n"
                                     "class_name CoroutinePropertyProducer\n"
                                     "var value: int: get = read_value\n"
                                     "func read_value() -> int:\n"
                                     "    return await resumed\n");
    write_text(root / "consumer.gd", "extends RefCounted\n"
                                     "class_name CoroutinePropertyConsumer\n"
                                     "var producer: CoroutinePropertyProducer\n"
                                     "func consume() -> int:\n"
                                     "    return await producer.value\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);
    REQUIRE(result.success);
    const auto producer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "producer.gd";
        });
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(producer != result.scripts.end());
    REQUIRE(consumer != result.scripts.end());
    const auto producer_header =
        read_text(options.output_directory / "generated" / producer->header_file_name);
    const auto consumer_source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(producer_header.find("godot::Variant _gdpp_get_value()") != std::string::npos);
    REQUIRE(consumer_source.find("gdpp::runtime::is_awaitable(") != std::string::npos);
    REQUIRE(consumer_source.find("gdpp::runtime::await_result(") != std::string::npos);
}

TEST_CASE("preload alias casts preserve void coroutine ABI at call sites") {
    const auto root = fixture_root("project-preload-alias-void-coroutine");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "producer.gd", "extends Node\n"
                                     "signal resumed\n"
                                     "func run() -> void:\n"
                                     "    await resumed\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "const Producer = preload(\"producer.gd\")\n"
                                     "func consume(value: Variant) -> void:\n"
                                     "    var producer := value as Producer\n"
                                     "    await producer.run()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto producer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"producer.gd"};
        });
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"consumer.gd"};
        });
    REQUIRE(producer != result.scripts.end());
    REQUIRE(consumer != result.scripts.end());
    const auto producer_header =
        read_text(options.output_directory / "generated" / producer->header_file_name);
    const auto consumer_source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    REQUIRE(producer_header.find("virtual godot::Variant run();") != std::string::npos);
    REQUIRE(consumer_source.find("[&]() -> godot::Variant") != std::string::npos);
    REQUIRE(consumer_source.find("cast_attached_script") != std::string::npos);
    REQUIRE(consumer_source.find("godot::String(\"res://producer.gd\")") != std::string::npos);
    REQUIRE(consumer_source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(consumer_source.find("godot::StringName(\"run\")") != std::string::npos);
}

TEST_CASE("project compiler isolates coroutine overrides behind dynamic script dispatch") {
    const auto root = fixture_root("project-coroutine-override");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends RefCounted\nclass_name CoroutineOverrideBase\n"
                                 "signal resumed\n"
                                 "func answer() -> int:\n    return -1\n");
    write_text(root / "child.gd",
               "extends CoroutineOverrideBase\nclass_name CoroutineOverrideChild\n"
               "func answer() -> int:\n    await resumed\n    return 42\n"
               "func local_answer() -> int:\n    return await answer()\n"
               "func base_typed_answer(value: CoroutineOverrideBase) -> int:\n"
               "    return await value.answer()\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto child =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "child.gd";
        });
    REQUIRE(child != result.scripts.end());
    const auto header = read_text(options.output_directory / "generated" / child->header_file_name);
    const auto source = read_text(options.output_directory / "generated" / child->source_file_name);
    REQUIRE(header.find("godot::Variant _gdpp_native_override_answer()") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"answer\")") != std::string::npos);
    REQUIRE(source.find("&" + child->class_name + "::_gdpp_variant_call_answer") !=
            std::string::npos);
    REQUIRE(source.find("_gdpp_native_override_answer()") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
}

TEST_CASE("project symbol refinement keeps immediate await functions on the coroutine ABI") {
    const auto root = fixture_root("project-immediate-await-abi");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "producer.gd", "extends RefCounted\nclass_name ImmediateAwaitProducer\n"
                                     "func answer() -> int:\n    return await 42\n");
    write_text(root / "consumer.gd", "extends RefCounted\nclass_name ImmediateAwaitConsumer\n"
                                     "var producer: ImmediateAwaitProducer\n"
                                     "func answer() -> int:\n    return await producer.answer()\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto producer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "producer.gd";
        });
    REQUIRE(producer != result.scripts.end());
    const auto header =
        read_text(options.output_directory / "generated" / producer->header_file_name);
    REQUIRE(header.find("virtual godot::Variant answer()") != std::string::npos);
}

TEST_CASE("project compilation permits detached coroutine calls") {
    const auto root = fixture_root("project-detached-coroutine");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "worker.gd", "extends RefCounted\nclass_name DetachedWorker\n"
                                   "signal resumed\n"
                                   "func run() -> void:\n    await resumed\n");
    write_text(root / "launcher.gd", "extends RefCounted\nclass_name DetachedLauncher\n"
                                     "var worker: DetachedWorker\n"
                                     "func launch() -> void:\n    worker.run()\n");
    const auto options = project_options(root);

    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.compiled_count, std::size_t{2});
}

TEST_CASE("project cache invalidates only direct ABI dependents") {
    const auto root = fixture_root("project-precise-cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name PreciseBase\n"
                                 "static func value() -> int:\n    return 1\n");
    write_text(root / "consumer.gd", "extends Node\nclass_name PreciseConsumer\n"
                                     "func read():\n    return PreciseBase.value()\n");
    write_text(root / "unrelated.gd", "extends Node\nclass_name PreciseUnrelated\n"
                                      "func read() -> int:\n    return 9\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.compiled_count, std::size_t{3});
    REQUIRE_EQ(native_class_for(initial, "base.gd").find("GDPPNative_PreciseBase_"),
               std::size_t{0});
    const auto initial_base_class = native_class_for(initial, "base.gd");
    const auto initial_consumer_class = native_class_for(initial, "consumer.gd");
    const auto initial_unrelated_hash =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "unrelated.gd";
        })->content_hash;

    write_text(root / "base.gd", "extends Node\nclass_name PreciseBase\n"
                                 "static func value() -> int:\n    return 2\n");
    const auto implementation_change = compiler.compile(options);
    REQUIRE(implementation_change.success);
    REQUIRE_EQ(implementation_change.compiled_count, std::size_t{1});
    REQUIRE_EQ(implementation_change.cache_hit_count, std::size_t{2});
    REQUIRE_EQ(native_class_for(implementation_change, "base.gd"), initial_base_class);
    REQUIRE_EQ(native_class_for(implementation_change, "consumer.gd"), initial_consumer_class);

    write_text(root / "base.gd", "extends Node\nclass_name PreciseBase\n"
                                 "static func value() -> float:\n    return 2.0\n");
    const auto abi_change = compiler.compile(options);
    REQUIRE(abi_change.success);
    REQUIRE_EQ(abi_change.compiled_count, std::size_t{2});
    REQUIRE_EQ(abi_change.cache_hit_count, std::size_t{1});
    REQUIRE(native_class_for(abi_change, "base.gd") != initial_base_class);
    REQUIRE_EQ(native_class_for(abi_change, "consumer.gd"), initial_consumer_class);
    const auto consumer =
        std::find_if(abi_change.scripts.begin(), abi_change.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != abi_change.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"base.gd"});
    const auto unrelated =
        std::find_if(abi_change.scripts.begin(), abi_change.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "unrelated.gd";
        });
    REQUIRE(unrelated != abi_change.scripts.end());
    REQUIRE(unrelated->cache_hit);
    REQUIRE_EQ(unrelated->content_hash, initial_unrelated_hash);
}

TEST_CASE("typed container object arguments participate in precise dependency invalidation") {
    const auto root = fixture_root("project-typed-container-dependency");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "item.gd", "extends RefCounted\nclass_name ContainerItem\n"
                                 "func value() -> int:\n    return 1\n");
    write_text(root / "consumer.gd", "extends Node\nclass_name ContainerConsumer\n"
                                     "var items: Array[ContainerItem] = []\n"
                                     "func replace(values: Array[ContainerItem]) -> void:\n"
                                     "    items = values\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;

    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.compiled_count, std::size_t{2});
    const auto initial_item_class = native_class_for(initial, "item.gd");
    const auto initial_consumer_class = native_class_for(initial, "consumer.gd");
    const auto initial_consumer =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(initial_consumer != initial.scripts.end());
    REQUIRE_EQ(initial_consumer->dependencies, std::vector<std::string>{"item.gd"});
    const auto initial_header =
        read_text(options.output_directory / "generated" / initial_consumer->header_file_name);
    REQUIRE(initial_header.find("gdpp::runtime::ScriptTypedArray<") != std::string::npos);
    REQUIRE(initial_header.find("godot::StringName(\"RefCounted\")") != std::string::npos);
    REQUIRE(initial_header.find("_gdpp_attached_script_path = \"res://item.gd\"") !=
            std::string::npos);
    REQUIRE(initial_header.find(initial_item_class) != std::string::npos);

    write_text(root / "item.gd", "extends RefCounted\nclass_name ContainerItem\n"
                                 "func value() -> float:\n    return 1.0\n");
    const auto changed = compiler.compile(options);
    REQUIRE(changed.success);
    REQUIRE_EQ(changed.compiled_count, std::size_t{2});
    REQUIRE_EQ(changed.cache_hit_count, std::size_t{0});
    REQUIRE(native_class_for(changed, "item.gd") != initial_item_class);
    REQUIRE_EQ(native_class_for(changed, "consumer.gd"), initial_consumer_class);
    const auto changed_consumer =
        std::find_if(changed.scripts.begin(), changed.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(changed_consumer != changed.scripts.end());
    const auto changed_header =
        read_text(options.output_directory / "generated" / changed_consumer->header_file_name);
    REQUIRE(changed_header.find("gdpp::runtime::ScriptTypedArray<") != std::string::npos);
    REQUIRE(changed_header.find("_gdpp_attached_script_path = \"res://item.gd\"") !=
            std::string::npos);
    REQUIRE(changed_header.find(native_class_for(changed, "item.gd")) != std::string::npos);
    REQUIRE(changed_header.find(initial_item_class) == std::string::npos);
}

TEST_CASE("project cache treats inspector annotations as public ABI") {
    const auto root = fixture_root("project-inspector-abi-cache");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto write_base = [&](const int step) {
        write_text(root / "base.gd", "extends Node\nclass_name InspectorAbiBase\n"
                                     "@export_range(0, 10, " +
                                         std::to_string(step) + ") var amount: int = 2\n");
    };
    write_base(1);
    write_text(root / "consumer.gd", "extends Node\nclass_name InspectorAbiConsumer\n"
                                     "var source: InspectorAbiBase\n");
    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);
    REQUIRE(initial.success);
    const auto initial_base_class = native_class_for(initial, "base.gd");

    write_base(2);
    const auto changed = compiler.compile(options);

    REQUIRE(changed.success);
    REQUIRE_EQ(changed.compiled_count, std::size_t{2});
    REQUIRE_EQ(changed.cache_hit_count, std::size_t{0});
    REQUIRE(native_class_for(changed, "base.gd") != initial_base_class);
}

TEST_CASE("project script resource loading rejects dynamic missing and unsupported construction") {
    const auto root = fixture_root("project-script-resource-diagnostics");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name ResourceFactoryBase\n");
    write_text(root / "factory.gd", "extends Node\nclass_name InvalidResourceFactory\n"
                                    "func invalid(path: String) -> void:\n"
                                    "    preload(path)\n"
                                    "    load(\"missing.gd\")\n"
                                    "    preload(\"base.gd\").new(1)\n"
                                    "    preload(\"base.gd\").invalid()\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    const auto has_code = [&result](const std::string& code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&code](const gdpp::ProjectDiagnostic& diagnostic) {
                               return diagnostic.diagnostic.code == code;
                           });
    };
    REQUIRE(has_code("GDS4060"));
    REQUIRE(has_code("GDS4061"));
    REQUIRE(has_code("GDS4055"));
    REQUIRE(has_code("GDS4063"));
}

TEST_CASE("project script resources preserve Script APIs and nullability") {
    const auto root = fixture_root("project-script-resource-api");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "service.gd", "extends Node\n"
                                    "class_name ResourceApiService\n"
                                    "signal executed\n"
                                    "func execute() -> void:\n"
                                    "    pass\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "class_name ResourceApiConsumer\n"
                                     "const Service = preload(\"service.gd\")\n"
                                     "func validate() -> bool:\n"
                                     "    var script := load(\"service.gd\")\n"
                                     "    if script == null:\n"
                                     "        return false\n"
                                     "    return script != null and script.can_instantiate() and "
                                     "script.has_script_signal(&\"executed\")\n"
                                     "func global_name() -> StringName:\n"
                                     "    return Service.get_global_name()\n"
                                     "func rename_resource() -> String:\n"
                                     "    var script := load(\"service.gd\")\n"
                                     "    script.resource_name = \"compiled\"\n"
                                     "    return script.resource_name\n"
                                     "func changed_signal() -> Signal:\n"
                                     "    return Service.changed\n"
                                     "func accepts_script(script: Script) -> bool:\n"
                                     "    return script != null\n"
                                     "func passes_as_script() -> bool:\n"
                                     "    return accepts_script(Service)\n"
                                     "func has_script_type() -> bool:\n"
                                     "    return Service is Script\n"
                                     "func clear_resource() -> bool:\n"
                                     "    var script := load(\"service.gd\")\n"
                                     "    script = null\n"
                                     "    return not script\n"
                                     "func conditional_resource(enabled: bool) -> bool:\n"
                                     "    var script := load(\"service.gd\") if enabled else null\n"
                                     "    return script != null\n"
                                     "func create() -> ResourceApiService:\n"
                                     "    return Service.new()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto source =
        read_text(options.output_directory / "generated/resource_api_consumer.gd.cpp");
    REQUIRE(source.find(".resource()") != std::string::npos);
    REQUIRE(source.find("->can_instantiate()") != std::string::npos);
    REQUIRE(source.find("->has_script_signal(") != std::string::npos);
    REQUIRE(source.find(".resource().is_null()") != std::string::npos);
    REQUIRE(source.find("->set_name(") != std::string::npos);
    REQUIRE(source.find("->get_name(") != std::string::npos);
    REQUIRE(source.find("godot::Signal(") != std::string::npos);
    REQUIRE(source.find("strict_native_ref_storage<godot::Script>") != std::string::npos);
    REQUIRE(source.find(".resource().ptr()") != std::string::npos);
    REQUIRE(source.find("::missing()") != std::string::npos);
}

TEST_CASE("project compiler lowers cross-script constants enums and resource factories") {
    const auto root = fixture_root("project-cross-symbol-values");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name SharedValues\n"
                                 "const LIMIT: int = 5\n"
                                 "const MASK = 1 << 2\n"
                                 "enum State { IDLE, ACTIVE = 4, BOOST = ACTIVE * 2 }\n"
                                 "enum { ANONYMOUS = 11 }\n"
                                 "var constructed: int = 0\n"
                                 "func _init(value: int = 5) -> void:\n"
                                 "    constructed = value\n");
    write_text(
        root / "consumer.gd",
        "extends Node\nclass_name SharedConsumer\n"
        "const Factory = preload(\"base.gd\")\n"
        "static var initialization_marker: int = 1\n"
        "class Holder:\n"
        "    const InnerFactory = preload(\"base.gd\")\n"
        "class TransactionalHolder:\n"
        "    const TransactionalFactory = preload(\"base.gd\")\n"
        "    static var marker: int = 1\n"
        "@export var state: SharedValues.State = SharedValues.State.BOOST\n"
        "func answer() -> int:\n"
        "    match state:\n"
        "        SharedValues.State.BOOST:\n"
        "            return SharedValues.LIMIT + SharedValues.ANONYMOUS + SharedValues.MASK\n"
        "        _:\n"
        "            return 0\n"
        "func create() -> SharedValues:\n"
        "    return Factory.new(5)\n"
        "func type_token():\n"
        "    return SharedValues\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto base_header = read_text(options.output_directory / "generated/shared_values.gd.hpp");
    REQUIRE(base_header.find("static const int64_t& LIMIT();") != std::string::npos);
    REQUIRE(base_header.find("static const int64_t& MASK();") != std::string::npos);
    const auto& base_class = native_class_for(result, "base.gd");
    REQUIRE(base_header.find("virtual void _init(godot::Variant _gdpp_argument_value = "
                             "gdpp::runtime::default_argument())") != std::string::npos);
    REQUIRE(base_header.find("public gdpp::runtime::AttachedScriptBehavior") != std::string::npos);
    const auto consumer_header =
        read_text(options.output_directory / "generated/shared_consumer.gd.hpp");
    REQUIRE(consumer_header.find("#include <gdpp/runtime/attached_script.hpp>") !=
            std::string::npos);
    REQUIRE(consumer_header.find("ScriptResource<GDPPNative_SharedValues_") != std::string::npos);
    REQUIRE(consumer_header.find("operator godot::Variant() const") != std::string::npos);
    REQUIRE(consumer_header.find("attached_script_resource(") != std::string::npos);
    REQUIRE(consumer_header.find("godot::Ref<godot::Script> resource() const") !=
            std::string::npos);
    REQUIRE(consumer_header.find("return gdpp::runtime::to_variant(resource())") !=
            std::string::npos);
    REQUIRE(consumer_header.find("return script;\n        } else {\n"
                                 "            return {};\n        }") != std::string::npos);
    const auto consumer_source =
        read_text(options.output_directory / "generated/shared_consumer.gd.cpp");
    REQUIRE(consumer_source.find("SharedValues_") != std::string::npos);
    REQUIRE(consumer_source.find("::State::_gdpp_enum_BOOST") != std::string::npos);
    REQUIRE(consumer_source.find("::LIMIT()") != std::string::npos);
    REQUIRE(consumer_source.find("::_gdpp_enum_ANONYMOUS") != std::string::npos);
    REQUIRE(consumer_source.find("::MASK()") != std::string::npos);
    REQUIRE(consumer_source.find("ScriptResource<GDPPNative_SharedValues_") != std::string::npos);
    REQUIRE(consumer_source.find(".instantiate(") != std::string::npos);
    REQUIRE(consumer_source.find("godot::StringName(\"" + base_class + "\")") != std::string::npos);
    REQUIRE(consumer_source.find("_gdpp_call_argument_") != std::string::npos);
    REQUIRE(consumer_source.find("IDLE:0,ACTIVE:4,BOOST:8") != std::string::npos);
    REQUIRE(consumer_source.find("_gdpp_constant_Factory_storage())>::missing()") !=
            std::string::npos);
    REQUIRE(consumer_source.find("value = " + std::string{"shared_consumer_gdpp_detail::"} +
                                 "ScriptResource<") != std::string::npos);
    REQUIRE(consumer_source.find(">::missing();\n    return value;") != std::string::npos);
    const auto count_occurrences = [](const std::string_view text, const std::string_view needle) {
        std::size_t count = 0;
        for (auto position = text.find(needle); position != std::string_view::npos;
             position = text.find(needle, position + needle.size()))
            ++count;
        return count;
    };
    REQUIRE_EQ(count_occurrences(consumer_source, "_gdpp_constant_Factory_storage())>::missing()"),
               std::size_t{2});
    REQUIRE_EQ(
        count_occurrences(consumer_source, "_gdpp_constant_InnerFactory_storage())>::missing()"),
        std::size_t{1});
    REQUIRE_EQ(count_occurrences(consumer_source,
                                 "_gdpp_constant_TransactionalFactory_storage())>::missing()"),
               std::size_t{2});
}

TEST_CASE("project compiler lowers cross-script static Callable values") {
    const auto root = fixture_root("project-cross-static-callable");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "sorter.gd", "extends RefCounted\n"
                                   "class_name ProjectSorter\n"
                                   "static func compare(left: int, right: int = 0) -> bool:\n"
                                   "    return left < right\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "func sort_values(values: Array) -> void:\n"
                                     "    values.sort_custom(ProjectSorter.compare)\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    const auto& sorter_class = native_class_for(result, "sorter.gd");
    REQUIRE(source.find("gdpp::runtime::make_callable(nullptr, 1, 2") != std::string::npos);
    REQUIRE(source.find(sorter_class + "::compare(") != std::string::npos);
    REQUIRE(source.find("godot::Callable(") == std::string::npos);
}

TEST_CASE("project compiler resolves globally named script inner types") {
    const auto root = fixture_root("project-global-inner-types");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "ast.gd", "extends RefCounted\n"
                                "class_name ProjectAst\n"
                                "class NodeBase extends RefCounted:\n"
                                "    pass\n"
                                "class ScriptNode extends NodeBase:\n"
                                "    enum State { READY, COMPLETE }\n");
    write_text(root / "consumer.gd", "extends RefCounted\n"
                                     "class_name ProjectAstConsumer\n"
                                     "var current: ProjectAst.ScriptNode\n"
                                     "func create() -> ProjectAst.ScriptNode:\n"
                                     "    return ProjectAst.ScriptNode.new()\n"
                                     "func accepts(value: ProjectAst.ScriptNode) -> bool:\n"
                                     "    return value is ProjectAst.ScriptNode\n"
                                     "func state() -> ProjectAst.ScriptNode.State:\n"
                                     "    return ProjectAst.ScriptNode.State.COMPLETE\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != result.scripts.end());
    REQUIRE_EQ(consumer->dependencies, std::vector<std::string>{"ast.gd"});
}

TEST_CASE("project compiler resolves Godot resource UIDs before semantic typing") {
    const auto root = fixture_root("project-resource-uids");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "factory.gd", "extends RefCounted\nclass_name UidFactory\n");
    write_text(root / "factory.gd.uid", "uid://gdppfactory\n");
    write_text(root / "level.tscn", "[gd_scene format=3 uid=\"uid://gdppscene\"]\n\n"
                                    "[ext_resource type=\"Resource\" uid=\"uid://gdppdata\" "
                                    "path=\"res://data.tres\" id=\"1_data\"]\n\n"
                                    "[ext_resource type=\"Texture2D\" uid=\"uid://gdppunicode\" "
                                    "path=\"res://素材/破坏石英.png\" id=\"2_unicode\"]\n\n"
                                    "[node name=\"Level\" type=\"Node\"]\n");
    write_text(root / "data.tres", "[gd_resource type=\"Resource\" format=3]\n");
    write_text(root / "icon.png", "not decoded by the compiler\n");
    write_text(root / "icon.png.import", "[remap]\nuid=\"uid://gdppicon\"\n"
                                         "[deps]\nsource_file=\"res://icon.png\"\n");
    const auto unicode_asset = gdpp::path_from_utf8("素材/破坏石英.png");
    write_text(root / unicode_asset, "not decoded by the compiler\n");
    auto unicode_sidecar = unicode_asset;
    unicode_sidecar += ".import";
    write_text(root / unicode_sidecar, "[remap]\nuid=\"uid://gdppunicode\"\n"
                                       "[deps]\nsource_file=\"res://素材/破坏石英.png\"\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "class_name UidConsumer\n"
                                     "const Factory = preload(\"uid://gdppfactory\")\n"
                                     "const Level = preload(\"uid://gdppscene\")\n"
                                     "const Data = preload(\"uid://gdppdata\")\n"
                                     "const Icon = load(\"uid://gdppicon\")\n"
                                     "const UnicodeIcon = load(\"uid://gdppunicode\")\n"
                                     "func create() -> UidFactory:\n"
                                     "    return Factory.new()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto source = read_text(options.output_directory / "generated/uid_consumer.gd.cpp");
    REQUIRE(source.find("ScriptResource<GDPPNative_UidFactory_") != std::string::npos);
    REQUIRE(source.find("uid://gdppscene") != std::string::npos);
}

TEST_CASE("project compiler rejects invalid _init declarations") {
    const auto root = fixture_root("project-invalid-init");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "invalid.gd", "extends Node\nclass_name InvalidInitializer\n"
                                    "static func _init() -> int:\n"
                                    "    return 1\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    const auto has_code = [&result](const std::string& code) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [&code](const gdpp::ProjectDiagnostic& diagnostic) {
                               return diagnostic.diagnostic.code == code;
                           });
    };
    REQUIRE(has_code("GDS4065"));
    REQUIRE(has_code("GDS4066"));
}

TEST_CASE("project compiler resolves autoloads and invalidates their cached symbol graph") {
    const auto root = fixture_root("project-autoload");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[application]\nconfig/name=\"Autoload Test\"\n\n"
                                       "[autoload]\nSettings=\"*res://settings.gd\"\n");
    write_text(root / "settings.gd",
               "extends Node\nconst DEFAULT_QUALITY: int = 4\nvar quality: int = 3\n");
    write_text(root / "consumer.gd", "extends Node\nclass_name AutoloadConsumer\n"
                                     "func quality() -> int:\n"
                                     "    return Settings.quality\n"
                                     "func default_quality() -> int:\n"
                                     "    return Settings.DEFAULT_QUALITY\n");

    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto initial = compiler.compile(options);

    REQUIRE(initial.success);
    const auto source = read_text(options.output_directory / "generated/autoload_consumer.gd.cpp");
    REQUIRE(source.find("gdpp::runtime::find_autoload(godot::StringName(\"Settings\"))") !=
            std::string::npos);
    REQUIRE(source.find("gdpp::runtime::get_named(") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"quality\")") != std::string::npos);
    REQUIRE(source.find("::DEFAULT_QUALITY") != std::string::npos);
    const auto settings_script =
        std::find_if(initial.scripts.begin(), initial.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "settings.gd";
        });
    REQUIRE(settings_script != initial.scripts.end());
    const auto settings_source =
        read_text(options.output_directory / "generated" / settings_script->source_file_name);
    REQUIRE(settings_source.find(
                "gdpp::runtime::register_autoload(godot::StringName(\"Settings\"), owner())") !=
            std::string::npos);

    write_text(root / "project.godot", "[application]\nconfig/name=\"Autoload Test\"\n\n"
                                       "[autoload]\nConfig=\"*res://settings.gd\"\n");
    const auto renamed = compiler.compile(options);
    REQUIRE(!renamed.success);
    REQUIRE(std::any_of(renamed.diagnostics.begin(), renamed.diagnostics.end(),
                        [](const gdpp::ProjectDiagnostic& diagnostic) {
                            return diagnostic.diagnostic.code == "GDS4058";
                        }));
}

TEST_CASE("project compiler resolves UID script autoloads into native globals") {
    const auto root = fixture_root("project-uid-autoload");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[autoload]\nAudioManager=\"*uid://gdpp-audio-manager\"\n");
    write_text(root / "audio_manager.gd", "extends Node\nvar volume: float = 0.5\n");
    write_text(root / "audio_manager.gd.uid", "uid://gdpp-audio-manager\n");
    write_text(root / "consumer.gd", "extends Node\n"
                                     "func volume() -> float:\n"
                                     "    return AudioManager.volume\n");
    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto find_script = [&](const std::string_view file_name) {
        return std::find_if(result.scripts.begin(), result.scripts.end(), [&](const auto& script) {
            return script.relative_path.filename().string() == std::string{file_name};
        });
    };
    const auto consumer_script = find_script("consumer.gd");
    const auto autoload_script = find_script("audio_manager.gd");
    REQUIRE(consumer_script != result.scripts.end());
    REQUIRE(autoload_script != result.scripts.end());
    const auto consumer =
        read_text(options.output_directory / "generated" / consumer_script->source_file_name);
    const auto autoload =
        read_text(options.output_directory / "generated" / autoload_script->source_file_name);
    REQUIRE(consumer.find("gdpp::runtime::find_autoload(godot::StringName(\"AudioManager\"))") !=
            std::string::npos);
    REQUIRE(consumer.find("get_singleton_object") == std::string::npos);
    REQUIRE(autoload.find(
                "gdpp::runtime::register_autoload(godot::StringName(\"AudioManager\"), owner())") !=
            std::string::npos);
}

TEST_CASE("project compiler rejects unresolved UID autoloads") {
    const auto root = fixture_root("project-unresolved-uid-autoload");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[autoload]\nMissing=\"*uid://gdpp-missing\"\n");
    write_text(root / "consumer.gd", "extends Node\nfunc read():\n    return Missing.value\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(!result.success);
    REQUIRE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "PRJ0030"; }));
}

TEST_CASE("project compiler resolves the root script of a scene autoload") {
    const auto root = fixture_root("project-scene-autoload");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[application]\nconfig/name=\"Scene Autoload Test\"\n\n"
                                       "[autoload]\nTransition=\"*res://transition.tscn\"\n");
    write_text(root / "transition.tscn", "[gd_scene load_steps=2 format=3]\n\n"
                                         "[ext_resource type=\"Script\" uid=\"uid://scene_script\" "
                                         "path=\"res://transition.gd\" id=\"1_script\"]\n\n"
                                         "[node name=\"Transition\" type=\"CanvasLayer\"]\n"
                                         "script = ExtResource(\"1_script\")\n");
    write_text(root / "transition.gd",
               "extends CanvasLayer\nfunc change_scene(path: String) -> void:\n"
               "    get_tree().change_scene_to_file(path)\n");
    write_text(root / "consumer.gd", "extends Node\nfunc enter() -> void:\n"
                                     "    Transition.change_scene(\"res://level.tscn\")\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto generated_source =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(generated_source != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / generated_source->source_file_name);
    REQUIRE(source.find("gdpp::runtime::find_autoload(godot::StringName(\"Transition\"))") !=
            std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"change_scene\")") != std::string::npos);
    REQUIRE(source.find("Object::cast_to<GDPPNative_") == std::string::npos);
}

TEST_CASE("project autoloads shadow same-named engine globals") {
    const auto root = fixture_root("project-autoload-engine-name");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[application]\nconfig/name=\"Autoload Priority\"\n\n"
                                       "[autoload]\nSetting=\"*res://setting.gd\"\n");
    write_text(root / "setting.gd", "extends Node\nfunc save_project() -> void:\n    pass\n");
    write_text(root / "nested/setting.gd", "extends Node\nfunc unrelated() -> void:\n    pass\n");
    write_text(root / "consumer.gd", "extends Node\nfunc save() -> void:\n"
                                     "    Setting.save_project()\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    const auto consumer =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path.filename() == "consumer.gd";
        });
    REQUIRE(consumer != result.scripts.end());
    const auto source =
        read_text(options.output_directory / "generated" / consumer->source_file_name);
    const auto setting =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"setting.gd"};
        });
    REQUIRE(setting != result.scripts.end());
    REQUIRE(source.find("#include \"" + setting->header_file_name + "\"") != std::string::npos);
    REQUIRE(source.find("gdpp::runtime::find_autoload(godot::StringName(\"Setting\"))") !=
            std::string::npos);
    REQUIRE(source.find("gdpp::runtime::call_dynamic_at(") != std::string::npos);
    REQUIRE(source.find("godot::StringName(\"save_project\")") != std::string::npos);
    REQUIRE(source.find("Object::cast_to<GDPPNative_") == std::string::npos);
}

TEST_CASE("project symbol graph preserves untyped fields as Variant") {
    const auto root = fixture_root("project-dynamic-autoload-field");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "project.godot", "[application]\nconfig/name=\"Dynamic Field Test\"\n\n"
                                       "[autoload]\nMain=\"*res://main.gd\"\n");
    write_text(root / "main.gd", "extends Node\nvar focus_enemy = null\n");
    write_text(root / "enemy.gd", "extends CharacterBody2D\n"
                                  "func select() -> void:\n"
                                  "    Main.focus_enemy = self\n");

    const auto result = gdpp::ProjectCompiler{}.compile(project_options(root));

    REQUIRE(result.success);
}

TEST_CASE("project source selection compiles scripts beside native addons and nested builds") {
    const auto root = fixture_root("project-source-selection");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "addons/runtime/base.gd", "@abstract\n"
                                                "extends Node\n"
                                                "class_name RuntimeAddonBase\n"
                                                "func execute() -> void:\n"
                                                "    pass\n");
    write_text(root / "game/feature.gd", "extends RuntimeAddonBase\n"
                                         "class_name RuntimeFeature\n"
                                         "func execute() -> void:\n"
                                         "    pass\n");
    write_text(root / "game/build/generated_feature.gd", "extends Node\n"
                                                         "class_name NestedBuildFeature\n");
    write_text(root / "build/root_build_artifact.gd", "extends Node\n"
                                                      "class_name RootBuildArtifact\n");
    write_text(root / "addons/gdpp/internal.gd", "extends Node\n"
                                                 "class_name GdppInternalArtifact\n");
    write_text(root / "addons/vendor/vendor.gdextension",
               "[configuration]\nentry_symbol=\"vendor_init\"\n"
               "compatibility_minimum=\"4.4\"\n");
    write_text(root / "addons/vendor/runtime_helper.gd",
               "extends Node\nclass_name VendorRuntimeHelper\n");
    write_text(root / "addons/vendor/vendor_scene.tscn",
               "[gd_scene load_steps=2 format=3]\n\n"
               "[sub_resource type=\"GDScript\" id=\"GDScript_vendor\"]\n"
               "script/source = \"extends Node\\nclass_name EmbeddedVendorHelper\\n\"\n\n"
               "[node name=\"Vendor\" type=\"Node\"]\n"
               "script = SubResource(\"GDScript_vendor\")\n");

    const auto options = project_options(root);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(result.success);
    REQUIRE_EQ(result.scripts.size(), std::size_t{5});
    REQUIRE(std::any_of(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
        return script.relative_path.generic_string() == "addons/runtime/base.gd" &&
               script.is_abstract;
    }));
    REQUIRE(std::any_of(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
        return script.relative_path.generic_string() == "game/build/generated_feature.gd";
    }));
    REQUIRE(std::none_of(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
        return script.relative_path.generic_string() == "build/root_build_artifact.gd" ||
               script.relative_path.generic_string() == "addons/gdpp/internal.gd";
    }));
    REQUIRE(std::any_of(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
        return script.relative_path.generic_string() == "addons/vendor/runtime_helper.gd";
    }));
    REQUIRE(std::any_of(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
        return script.relative_path.generic_string() ==
               "addons/vendor/vendor_scene.tscn::GDScript_vendor";
    }));
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("GDREGISTER_ABSTRACT_CLASS(GDPPNative_RuntimeAddonBase_") !=
            std::string::npos);
}

TEST_CASE("project compiler enforces cross-script abstract method obligations") {
    const auto root = fixture_root("project-abstract-inheritance");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "contract.gd", "@abstract\n"
                                     "extends RefCounted\n"
                                     "class_name WorkContract\n"
                                     "@abstract\n"
                                     "func execute(value: int) -> String\n");
    write_text(root / "deferred.gd", "@abstract\n"
                                     "extends WorkContract\n"
                                     "class_name DeferredWork\n");
    write_text(root / "implementation.gd", "extends DeferredWork\n"
                                           "class_name ConcreteWork\n"
                                           "func execute(value: int) -> String:\n"
                                           "    return str(value)\n");
    write_text(root / "inner_types.gd", "@tool\n"
                                        "class_name InnerContracts\n"
                                        "@abstract class Contract:\n"
                                        "    @abstract func execute() -> void\n"
                                        "class Implementation extends Contract:\n"
                                        "    func execute() -> void:\n"
                                        "        pass\n");

    const auto options = project_options(root);
    const gdpp::ProjectCompiler compiler;
    const auto valid = compiler.compile(options);

    REQUIRE(valid.success);
    REQUIRE_EQ(std::count_if(valid.scripts.begin(), valid.scripts.end(),
                             [](const auto& script) { return script.is_abstract; }),
               std::ptrdiff_t{2});
    const auto registration = read_text(options.output_directory / "register_types.cpp");
    REQUIRE(registration.find("GDREGISTER_ABSTRACT_CLASS(GDPPNative_WorkContract_") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_ABSTRACT_CLASS(GDPPNative_DeferredWork_") !=
            std::string::npos);
    REQUIRE(registration.find("GDREGISTER_ABSTRACT_CLASS(GDPPNative_InnerContracts_") !=
            std::string::npos);
    REQUIRE(registration.find("__Contract);") != std::string::npos);
    REQUIRE(registration.find("GDREGISTER_CLASS(GDPPNative_InnerContracts_") != std::string::npos);
    REQUIRE(registration.find("__Implementation);") != std::string::npos);

    write_text(root / "missing.gd", "extends DeferredWork\n"
                                    "class_name MissingWork\n");
    const auto invalid = compiler.compile(options);

    REQUIRE(!invalid.success);
    REQUIRE(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "GDS4149"; }));
}

TEST_CASE("project frontend limit failures never commit generated state") {
    const auto root = fixture_root("project-frontend-limits");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "oversized.gd",
               "extends Node\nclass_name OversizedInput\nfunc answer() -> int:\n    return 42\n");
    auto options = project_options(root);
    options.compiler.frontend_limits.max_source_bytes = 32;

    const auto first = gdpp::ProjectCompiler{}.compile(options);
    const auto second = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(!first.success);
    REQUIRE(!second.success);
    REQUIRE(std::any_of(first.diagnostics.begin(), first.diagnostics.end(),
                        [](const auto& item) { return item.diagnostic.code == "GDS1010"; }));
    REQUIRE_EQ(first.diagnostics.size(), second.diagnostics.size());
    REQUIRE(!std::filesystem::exists(options.output_directory / "manifest.txt"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "generated"));
    REQUIRE(!std::filesystem::exists(options.output_directory / "register_types.cpp"));
}

TEST_CASE("project compiler emits source-free editor reflection on cache hits") {
    const auto root = fixture_root("project-editor-reflection");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_text(root / "base.gd", "extends Node\nclass_name ReflectionBase\n"
                                 "signal inherited_signal\n");
    write_text(root / "child.gd", "extends ReflectionBase\n"
                                  "class_name ReflectionChild\n"
                                  "@export var score: int = 7\n"
                                  "var transient: String = \"ready\"\n"
                                  "static var shared: int = 1\n"
                                  "signal completed(value: int)\n"
                                  "func execute(value: int = 3) -> String:\n"
                                  "    return str(value)\n");
    const auto options = project_options(root);
    const auto first = gdpp::ProjectCompiler{}.compile(options);
    const auto result = gdpp::ProjectCompiler{}.compile(options);

    REQUIRE(first.success);
    REQUIRE(result.success);
    REQUIRE_EQ(result.cache_hit_count, std::size_t{2});
    const auto child =
        std::find_if(result.scripts.begin(), result.scripts.end(), [](const auto& script) {
            return script.relative_path == std::filesystem::path{"child.gd"};
        });
    REQUIRE(child != result.scripts.end());
    REQUIRE_EQ(child->global_name, std::string{"ReflectionChild"});
    REQUIRE_EQ(child->base_script_path, std::string{"res://base.gd"});
    const auto member = [&](const std::string_view name) {
        return std::find_if(child->reflection_members.begin(), child->reflection_members.end(),
                            [&](const auto& value) { return value.name == name; });
    };
    const auto score = member("score");
    const auto transient = member("transient");
    const auto shared = member("shared");
    const auto completed = member("completed");
    const auto execute = member("execute");
    REQUIRE(score != child->reflection_members.end());
    REQUIRE(score->property_storage);
    REQUIRE(score->property_editor);
    REQUIRE(transient != child->reflection_members.end());
    REQUIRE(!transient->property_storage);
    REQUIRE(shared != child->reflection_members.end());
    REQUIRE(shared->is_static);
    REQUIRE(completed != child->reflection_members.end());
    REQUIRE_EQ(completed->parameter_names, std::vector<std::string>{"value"});
    REQUIRE(execute != child->reflection_members.end());
    REQUIRE_EQ(execute->parameter_names, std::vector<std::string>{"value"});
    REQUIRE_EQ(execute->default_parameters, std::vector<bool>{true});
}
