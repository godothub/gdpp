#include "compiler_service.hpp"

#include "gdpp/compiler/compiler.hpp"
#include "gdpp/core/diagnostic.hpp"
#include "gdpp/core/source.hpp"
#include "gdpp/project/export_worker_snapshot.hpp"
#include "gdpp/project/native_builder.hpp"
#include "gdpp/project/project_compiler.hpp"
#include "gdpp/runtime/attached_script.hpp"
#include "gdpp/semantic/godot_api.hpp"
#include "gdpp/support/path_utf8.hpp"
#include "gdpp/support/sha256.hpp"
#include "gdpp/version.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#ifndef GDPP_SDK_ROOT
#define GDPP_SDK_ROOT ""
#endif
#ifndef GDPP_PLATFORM
#define GDPP_PLATFORM "linux"
#endif
#ifndef GDPP_ARCH
#define GDPP_ARCH "x86_64"
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace gdpp::extension {
namespace {

std::string native_string(const godot::String& value) {
    const auto utf8 = value.utf8();
    return {utf8.get_data(), static_cast<std::size_t>(utf8.length())};
}

void assign_diagnostic_channels(godot::Dictionary& output,
                                const godot::PackedStringArray& diagnostics,
                                const godot::PackedStringArray& errors,
                                const godot::PackedStringArray& warnings = {},
                                const godot::PackedStringArray& notes = {}) {
    output["diagnostics"] = diagnostics;
    output["errors"] = errors;
    output["warnings"] = warnings;
    output["notes"] = notes;
}

godot::Dictionary failure_result(const godot::String& message) {
    godot::Dictionary output;
    output["success"] = false;
    godot::PackedStringArray errors;
    errors.push_back(message);
    assign_diagnostic_channels(output, errors, errors);
    return output;
}

godot::Dictionary result_dictionary(const CompileResult& result, const SourceFile& source) {
    godot::Dictionary output;
    output["success"] = result.success;
    godot::PackedStringArray diagnostics;
    godot::PackedStringArray errors;
    godot::PackedStringArray warnings;
    godot::PackedStringArray notes;
    for (const auto& diagnostic : result.diagnostics) {
        const godot::String message{format_diagnostic(diagnostic, source, false).c_str()};
        diagnostics.push_back(message);
        switch (diagnostic.severity) {
        case DiagnosticSeverity::error:
            errors.push_back(message);
            break;
        case DiagnosticSeverity::warning:
            warnings.push_back(message);
            break;
        case DiagnosticSeverity::note:
            notes.push_back(message);
            break;
        }
    }
    assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
    godot::Dictionary optimization;
    optimization["constants_folded"] = static_cast<int64_t>(result.optimization.constants_folded);
    optimization["statements_removed"] =
        static_cast<int64_t>(result.optimization.statements_removed);
    optimization["hir_branches_simplified"] =
        static_cast<int64_t>(result.optimization.branches_simplified);
    optimization["mir_branches_simplified"] =
        static_cast<int64_t>(result.mir_optimization.branches_simplified);
    optimization["mir_blocks_removed"] =
        static_cast<int64_t>(result.mir_optimization.blocks_removed);
    optimization["mir_instructions_removed"] =
        static_cast<int64_t>(result.mir_optimization.instructions_removed);
    output["optimization"] = optimization;
    if (result.success) {
        output["class_name"] = godot::String{result.unit.script_class_name.c_str()};
        output["native_class_name"] = godot::String{result.unit.class_name.c_str()};
        output["header_name"] = godot::String{result.unit.header_file_name.c_str()};
        output["source_name"] = godot::String{result.unit.source_file_name.c_str()};
        output["header"] = godot::String{result.unit.header.c_str()};
        output["source"] = godot::String{result.unit.source.c_str()};
    }
    return output;
}

bool write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream{path, std::ios::binary};
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return stream.good();
}

bool has_path_component(const std::filesystem::path& path, std::string_view component) {
    return std::any_of(path.begin(), path.end(),
                       [&](const auto& item) { return item == std::filesystem::path{component}; });
}

bool is_xcframework(const std::filesystem::path& path) {
    std::error_code error;
    return path.extension() == ".xcframework" && !std::filesystem::is_symlink(path, error) &&
           !error && std::filesystem::is_directory(path, error) && !error &&
           std::filesystem::is_regular_file(path / "Info.plist", error) && !error;
}

bool commit_xcframework(const std::filesystem::path& pending,
                        const std::filesystem::path& destination, std::string& diagnostic) {
    const auto filename = destination.filename().string();
    if (!is_xcframework(pending) || pending.filename() != destination.filename() ||
        filename.rfind("libgdpp.", 0) != 0 || destination.parent_path().filename() != "binary" ||
        !has_path_component(pending, "native-direct") ||
        !has_path_component(pending, "xcframework-staging")) {
        diagnostic = "refusing to commit an invalid or unsafe iOS XCFramework artifact";
        return false;
    }

    auto backup = destination;
    backup += ".previous";
    std::error_code error;
    bool had_destination = std::filesystem::exists(destination, error);
    if (error) {
        diagnostic = "cannot inspect the current iOS artifact: " + error.message();
        return false;
    }
    const bool has_backup = std::filesystem::exists(backup, error);
    if (error) {
        diagnostic = "cannot inspect the previous iOS artifact backup: " + error.message();
        return false;
    }
    if (has_backup && !had_destination) {
        std::filesystem::rename(backup, destination, error);
        if (error) {
            diagnostic = "cannot recover the previous iOS artifact after an interrupted commit: " +
                         error.message();
            return false;
        }
        had_destination = true;
    } else if (has_backup) {
        std::filesystem::remove_all(backup, error);
        if (error) {
            diagnostic = "cannot clear the obsolete iOS artifact backup: " + error.message();
            return false;
        }
    }
    if (had_destination) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            diagnostic =
                "cannot stage the current iOS artifact for replacement: " + error.message();
            return false;
        }
    }
    std::filesystem::rename(pending, destination, error);
    if (error) {
        const auto commit_error = error.message();
        if (had_destination) {
            error.clear();
            std::filesystem::rename(backup, destination, error);
        }
        diagnostic = "cannot commit the new iOS XCFramework: " + commit_error;
        return false;
    }
    std::filesystem::remove_all(backup, error);
    // A failed cleanup does not invalidate the newly committed XCFramework. The next build will
    // recognize and remove this backup before attempting another replacement.
    return true;
}

NativePlatform native_platform() {
    const std::string platform{GDPP_PLATFORM};
    if (platform == "macos")
        return NativePlatform::macos;
    if (platform == "windows")
        return NativePlatform::windows;
    return NativePlatform::linux;
}

std::string native_platform_name(NativePlatform platform) {
    if (platform == NativePlatform::macos)
        return "macos";
    if (platform == NativePlatform::windows)
        return "windows";
    if (platform == NativePlatform::android)
        return "android";
    if (platform == NativePlatform::ios)
        return "ios";
    if (platform == NativePlatform::web)
        return "web";
    return "linux";
}

std::string host_process_architecture() {
    if (const auto* engine = godot::Engine::get_singleton()) {
        auto architecture = native_string(engine->get_architecture_name());
        std::transform(architecture.begin(), architecture.end(), architecture.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (architecture == "aarch64" || architecture == "arm64" || architecture == "arm64-v8a") {
            return "arm64";
        }
        if (architecture == "amd64" || architecture == "x64" || architecture == "x86_64") {
            return "x86_64";
        }
    }

    // A universal Mach-O contains separate compiler-service slices. These preprocessor checks
    // identify the slice selected by the current process, whereas GDPP_ARCH intentionally names
    // the distributable file as "universal".
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return GDPP_ARCH;
#endif
}

std::optional<NativePlatform> parse_native_platform(const std::string& value) {
    if (value == "macos")
        return NativePlatform::macos;
    if (value == "windows")
        return NativePlatform::windows;
    if (value == "linux")
        return NativePlatform::linux;
    if (value == "android")
        return NativePlatform::android;
    if (value == "ios")
        return NativePlatform::ios;
    if (value == "web")
        return NativePlatform::web;
    return std::nullopt;
}

godot::String default_compiler() {
    return native_platform() == NativePlatform::windows ? godot::String{"cl.exe"}
                                                        : godot::String{"c++"};
}

std::string reflected_type_name(const godot::Dictionary& info, const bool allow_void) {
    if (!info.has("type"))
        return allow_void ? "void" : "Variant";
    const auto raw_type = static_cast<std::int64_t>(info["type"]);
    if (raw_type < godot::Variant::NIL || raw_type >= godot::Variant::VARIANT_MAX)
        return "Variant";
    const auto type = static_cast<godot::Variant::Type>(raw_type);
    const auto usage = info.has("usage")
                           ? static_cast<std::uint64_t>(static_cast<std::int64_t>(info["usage"]))
                           : std::uint64_t{0};
    if (type == godot::Variant::NIL) {
        return (usage & godot::PROPERTY_USAGE_NIL_IS_VARIANT) != 0 || !allow_void ? "Variant"
                                                                                  : "void";
    }
    if ((type == godot::Variant::OBJECT || type == godot::Variant::INT) && info.has("class_name")) {
        const godot::StringName class_name = info["class_name"];
        if (!class_name.is_empty())
            return native_string(godot::String{class_name});
    }
    const auto hint = info.has("hint") ? static_cast<std::int64_t>(info["hint"]) : std::int64_t{-1};
    const auto hint_string = info.has("hint_string")
                                 ? native_string(static_cast<godot::String>(info["hint_string"]))
                                 : std::string{};
    if (type == godot::Variant::ARRAY && hint == godot::PROPERTY_HINT_ARRAY_TYPE) {
        return type_from_godot_api("typedarray::" + hint_string).name;
    }
    if (type == godot::Variant::DICTIONARY && hint == godot::PROPERTY_HINT_DICTIONARY_TYPE) {
        return type_from_godot_api("typeddictionary::" + hint_string).name;
    }
    if (type == godot::Variant::OBJECT && hint == godot::PROPERTY_HINT_RESOURCE_TYPE &&
        !hint_string.empty()) {
        return hint_string;
    }
    return native_string(godot::Variant::get_type_name(type));
}

using ProjectScriptNativeBases = std::unordered_map<std::string, std::string>;

std::string reflected_object_native_base(const Type& type,
                                         const ProjectScriptNativeBases& script_native_bases) {
    if (type.kind != TypeKind::object)
        return {};
    if (const auto found = script_native_bases.find(type.name);
        found != script_native_bases.end()) {
        return found->second;
    }
    return type.name;
}

std::string reflected_container_argument(const std::string& argument,
                                         const ProjectScriptNativeBases& script_native_bases) {
    const auto type = type_from_annotation(argument);
    if (type.kind == TypeKind::object)
        return reflected_object_native_base(type, script_native_bases);
    return argument;
}

godot::PropertyInfo script_property_info(const Type& type, const godot::StringName& name,
                                         const std::uint32_t usage,
                                         const ProjectScriptNativeBases& script_native_bases) {
    godot::PropertyInfo info;
    info.name = name;
    info.usage = usage;
    if (type.kind == TypeKind::void_type) {
        info.type = godot::Variant::NIL;
        info.usage = godot::PROPERTY_USAGE_NONE;
        return info;
    }
    const auto variant_type = variant_type_of(type);
    info.type =
        variant_type ? static_cast<godot::Variant::Type>(*variant_type) : godot::Variant::NIL;
    if (type.kind == TypeKind::variant || type.kind == TypeKind::unknown) {
        info.type = godot::Variant::NIL;
        info.usage |= godot::PROPERTY_USAGE_NIL_IS_VARIANT;
    } else if (type.kind == TypeKind::object || type.kind == TypeKind::script_resource) {
        info.class_name = godot::StringName{type.name.c_str()};
        if (type.kind == TypeKind::object &&
            (usage & godot::PROPERTY_USAGE_EDITOR) != 0U) {
            const auto native_base = reflected_object_native_base(type, script_native_bases);
            const godot::StringName native_base_name{native_base.c_str()};
            const auto* class_db = godot::ClassDBSingleton::get_singleton();
            const auto inherits = [&](const char* base) {
                const godot::StringName base_name{base};
                return native_base_name == base_name ||
                       (class_db && class_db->class_exists(native_base_name) &&
                        class_db->is_parent_class(native_base_name, base_name));
            };
            if (inherits("Node")) {
                info.hint = godot::PROPERTY_HINT_NODE_TYPE;
                info.hint_string = godot::String{type.name.c_str()};
            } else if (inherits("Resource")) {
                info.hint = godot::PROPERTY_HINT_RESOURCE_TYPE;
                info.hint_string = godot::String{type.name.c_str()};
            }
        }
    }
    if (const auto container = describe_container_type(type);
        container && container->has_runtime_constraint()) {
        info.hint = container->kind == ContainerTypeKind::array
                        ? godot::PROPERTY_HINT_ARRAY_TYPE
                        : godot::PROPERTY_HINT_DICTIONARY_TYPE;
        std::string hint;
        for (const auto& argument : container->arguments) {
            if (!hint.empty())
                hint += container->kind == ContainerTypeKind::array ? "," : ";";
            hint += reflected_container_argument(argument, script_native_bases);
        }
        info.hint_string = godot::String{hint.c_str()};
    }
    return info;
}

godot::Dictionary editor_script_descriptor(const CompiledProjectScript& script,
                                           const godot::String& source_path,
                                           const ProjectScriptNativeBases& script_native_bases) {
    godot::Dictionary descriptor;
    descriptor["source_path"] = source_path;
    descriptor["global_name"] = godot::StringName{script.global_name.c_str()};
    descriptor["native_base_type"] = godot::StringName{script.attached_native_base.c_str()};
    descriptor["base_script_path"] = godot::String{script.base_script_path.c_str()};
    descriptor["contract_hash"] = godot::String{script.public_abi_hash.c_str()};
    descriptor["behavior_class"] = godot::StringName{script.class_name.c_str()};
    descriptor["constants"] = godot::Dictionary{};
    descriptor["rpc_config"] = godot::Variant{};
    descriptor["tool"] = script.is_tool;
    descriptor["abstract"] = script.is_abstract;
    descriptor["static_unload"] = script.static_unload;

    godot::Array properties;
    godot::Array methods;
    godot::Array signals;
    for (const auto& member : script.reflection_members) {
        if (member.kind == ScriptMemberKind::field && !member.is_static) {
            std::uint32_t usage = godot::PROPERTY_USAGE_SCRIPT_VARIABLE;
            if (member.property_storage)
                usage |= godot::PROPERTY_USAGE_STORAGE;
            if (member.property_editor)
                usage |= godot::PROPERTY_USAGE_EDITOR;
            godot::Dictionary property;
            property["info"] = static_cast<godot::Dictionary>(
                script_property_info(member.type, godot::StringName{member.name.c_str()}, usage,
                                     script_native_bases));
            // The target behavior constructor owns source-level defaults. This temporary editor
            // instance only needs the serialization surface while stored values are copied.
            property["has_default"] = false;
            properties.push_back(property);
            continue;
        }
        if (member.kind != ScriptMemberKind::function && member.kind != ScriptMemberKind::signal) {
            continue;
        }
        if (member.kind == ScriptMemberKind::function && member.name == "_static_init")
            continue;
        godot::MethodInfo method{script_property_info(member.kind == ScriptMemberKind::signal
                                                          ? Type{TypeKind::void_type, "void"}
                                                          : member.type,
                                                      godot::StringName{},
                                                      godot::PROPERTY_USAGE_DEFAULT,
                                                      script_native_bases),
                                 godot::StringName{member.name.c_str()}};
        for (std::size_t index = 0; index < member.parameters.size(); ++index) {
            const auto argument_name = index < member.parameter_names.size()
                                           ? member.parameter_names[index]
                                           : "argument_" + std::to_string(index);
            method.arguments.push_back(script_property_info(
                member.parameters[index], godot::StringName{argument_name.c_str()},
                godot::PROPERTY_USAGE_DEFAULT, script_native_bases));
            if (index < member.default_parameters.size() && member.default_parameters[index])
                method.default_arguments.push_back(godot::Variant{});
        }
        if (member.is_static)
            method.flags |= GDEXTENSION_METHOD_FLAG_STATIC;
        if (member.is_vararg)
            method.flags |= GDEXTENSION_METHOD_FLAG_VARARG;
        if (member.kind == ScriptMemberKind::signal)
            signals.push_back(static_cast<godot::Dictionary>(method));
        else
            methods.push_back(static_cast<godot::Dictionary>(method));
    }
    descriptor["properties"] = properties;
    descriptor["methods"] = methods;
    descriptor["signals"] = signals;
    return descriptor;
}

bool reflected_nil_is_variant(const godot::Dictionary& info) {
    if (!info.has("type") || static_cast<std::int64_t>(info["type"]) != godot::Variant::NIL) {
        return false;
    }
    const auto usage = info.has("usage")
                           ? static_cast<std::uint64_t>(static_cast<std::int64_t>(info["usage"]))
                           : std::uint64_t{0};
    return (usage & godot::PROPERTY_USAGE_NIL_IS_VARIANT) != 0;
}

std::string reflected_name(const godot::Dictionary& info) {
    if (!info.has("name"))
        return {};
    const godot::StringName value = info["name"];
    return native_string(godot::String{value});
}

std::uint32_t reflected_method_compatibility_hash(const godot::Dictionary& method_dictionary) {
    const auto method = godot::MethodInfo::from_dict(method_dictionary);
    const bool has_return = method.return_val.type != godot::Variant::NIL ||
                            (method.return_val.usage & godot::PROPERTY_USAGE_NIL_IS_VARIANT) != 0;
    auto hash = godot::hash_murmur3_one_32(has_return ? 1U : 0U);
    hash = godot::hash_murmur3_one_32(static_cast<std::uint32_t>(method.arguments.size()), hash);
    if (has_return) {
        hash = godot::hash_murmur3_one_32(static_cast<std::uint32_t>(method.return_val.type), hash);
        if (!method.return_val.class_name.is_empty()) {
            hash = godot::hash_murmur3_one_32(
                static_cast<std::uint32_t>(method.return_val.class_name.hash()), hash);
        }
    }
    for (const auto& argument : method.arguments) {
        hash = godot::hash_murmur3_one_32(static_cast<std::uint32_t>(argument.type), hash);
        if (!argument.class_name.is_empty()) {
            hash = godot::hash_murmur3_one_32(
                static_cast<std::uint32_t>(argument.class_name.hash()), hash);
        }
    }
    hash = godot::hash_murmur3_one_32(static_cast<std::uint32_t>(method.default_arguments.size()),
                                      hash);
    for (const auto& default_argument : method.default_arguments)
        hash = godot::hash_murmur3_one_32(default_argument.hash(), hash);
    hash =
        godot::hash_murmur3_one_32((method.flags & godot::METHOD_FLAG_CONST) != 0 ? 1U : 0U, hash);
    hash =
        godot::hash_murmur3_one_32((method.flags & godot::METHOD_FLAG_VARARG) != 0 ? 1U : 0U, hash);
    return godot::hash_fmix32(hash);
}

std::vector<ExtensionBridge> reflect_extension_contracts() {
    auto* class_db = godot::ClassDBSingleton::get_singleton();
    if (!class_db)
        return {};

    const auto is_extension_class = [&](const godot::StringName& name) {
        if (name.is_empty() || !class_db->class_exists(name))
            return false;
        const auto api = class_db->class_get_api_type(name);
        return api == godot::ClassDBSingleton::API_EXTENSION ||
               api == godot::ClassDBSingleton::API_EDITOR_EXTENSION;
    };
    std::vector<ExtensionBridge> result;
    for (const auto& reflected_name_value : class_db->get_class_list()) {
        const godot::StringName class_name = reflected_name_value;
        const auto class_name_utf8 = native_string(godot::String{class_name});
        if (!is_extension_class(class_name) || class_name_utf8 == "GDPPCompiler" ||
            class_name_utf8.rfind("GDPPNative_", 0) == 0) {
            continue;
        }

        ExtensionBridgeClass contract;
        contract.gdscript_name = class_name_utf8;
        contract.runtime_only = true;
        contract.editor_only = class_db->class_get_api_type(class_name) ==
                               godot::ClassDBSingleton::API_EDITOR_EXTENSION;
        contract.members_complete = true;
        std::unordered_set<std::string> member_keys;
        godot::StringName current = class_name;
        std::unordered_set<std::string> visited;
        while (is_extension_class(current)) {
            const auto current_utf8 = native_string(godot::String{current});
            if (!visited.insert(current_utf8).second)
                break;

            const auto methods = class_db->class_get_method_list(current, true);
            for (std::int64_t index = 0; index < methods.size(); ++index) {
                const godot::Dictionary method = methods[index];
                ExtensionBridgeMember member;
                member.kind = ExtensionBridgeMemberKind::method;
                member.name = reflected_name(method);
                if (member.name.empty())
                    continue;
                const auto flags =
                    method.has("flags")
                        ? static_cast<std::uint64_t>(static_cast<std::int64_t>(method["flags"]))
                        : std::uint64_t{0};
                member.is_static = (flags & godot::METHOD_FLAG_STATIC) != 0;
                member.vararg = (flags & godot::METHOD_FLAG_VARARG) != 0;
                member.method_hash = reflected_method_compatibility_hash(method);
                member.has_method_hash = true;
                if (method.has("return")) {
                    const godot::Dictionary return_info = method["return"];
                    member.type = reflected_type_name(return_info, true);
                } else {
                    member.type = "void";
                }
                godot::Array arguments;
                if (method.has("args"))
                    arguments = method["args"];
                godot::Array defaults;
                if (method.has("default_args"))
                    defaults = method["default_args"];
                const auto required = arguments.size() - defaults.size();
                for (std::int64_t argument_index = 0; argument_index < arguments.size();
                     ++argument_index) {
                    const godot::Dictionary argument = arguments[argument_index];
                    ExtensionBridgeParameter parameter;
                    parameter.name = reflected_name(argument);
                    if (parameter.name.empty())
                        parameter.name = "arg" + std::to_string(argument_index);
                    parameter.type = reflected_type_name(argument, false);
                    parameter.has_default = argument_index >= required;
                    member.parameters.push_back(std::move(parameter));
                }
                const auto key = "m:" + member.name;
                if (member_keys.insert(key).second)
                    contract.members.push_back(std::move(member));
            }

            const auto properties = class_db->class_get_property_list(current, true);
            for (std::int64_t index = 0; index < properties.size(); ++index) {
                const godot::Dictionary property = properties[index];
                ExtensionBridgeMember member;
                member.kind = ExtensionBridgeMemberKind::property;
                member.name = reflected_name(property);
                if (member.name.empty() ||
                    (property.has("type") &&
                     static_cast<std::int64_t>(property["type"]) == godot::Variant::NIL &&
                     !reflected_nil_is_variant(property))) {
                    continue;
                }
                member.type = reflected_type_name(property, false);
                if (member.type == "int") {
                    const auto getter =
                        class_db->class_get_property_getter(current, member.name.c_str());
                    const auto getter_member = std::find_if(
                        contract.members.begin(), contract.members.end(),
                        [&](const auto& candidate) {
                            return candidate.kind == ExtensionBridgeMemberKind::method &&
                                   candidate.name == native_string(godot::String{getter});
                        });
                    if (getter_member != contract.members.end() &&
                        getter_member->type.find('.') != std::string::npos) {
                        member.type = getter_member->type;
                    }
                }
                const auto usage =
                    property.has("usage")
                        ? static_cast<std::uint64_t>(static_cast<std::int64_t>(property["usage"]))
                        : std::uint64_t{0};
                member.read_only =
                    (usage & godot::PROPERTY_USAGE_READ_ONLY) != 0 ||
                    class_db->class_get_property_setter(current, member.name.c_str()).is_empty();
                const auto key = "p:" + member.name;
                if (member_keys.insert(key).second)
                    contract.members.push_back(std::move(member));
            }

            const auto signals = class_db->class_get_signal_list(current, true);
            for (std::int64_t index = 0; index < signals.size(); ++index) {
                const godot::Dictionary signal = signals[index];
                ExtensionBridgeMember member;
                member.kind = ExtensionBridgeMemberKind::signal;
                member.name = reflected_name(signal);
                member.type = "Signal";
                if (member.name.empty())
                    continue;
                godot::Array arguments;
                if (signal.has("args"))
                    arguments = signal["args"];
                for (std::int64_t argument_index = 0; argument_index < arguments.size();
                     ++argument_index) {
                    const godot::Dictionary argument = arguments[argument_index];
                    ExtensionBridgeParameter parameter;
                    parameter.name = reflected_name(argument);
                    if (parameter.name.empty())
                        parameter.name = "arg" + std::to_string(argument_index);
                    parameter.type = reflected_type_name(argument, false);
                    member.parameters.push_back(std::move(parameter));
                }
                const auto key = "s:" + member.name;
                if (member_keys.insert(key).second)
                    contract.members.push_back(std::move(member));
            }

            const auto constants = class_db->class_get_integer_constant_list(current, true);
            for (const auto& constant_name_value : constants) {
                const godot::StringName constant_name = constant_name_value;
                if (!class_db->class_get_integer_constant_enum(current, constant_name, true)
                         .is_empty()) {
                    continue;
                }
                ExtensionBridgeMember member;
                member.kind = ExtensionBridgeMemberKind::constant;
                member.name = native_string(godot::String{constant_name});
                member.type = "int";
                member.read_only = true;
                member.is_static = true;
                member.constant_value =
                    class_db->class_get_integer_constant(current, constant_name);
                const auto key = "c:" + member.name;
                if (!member.name.empty() && member_keys.insert(key).second)
                    contract.members.push_back(std::move(member));
            }

            const auto enum_names = class_db->class_get_enum_list(current, true);
            for (const auto& enum_name_value : enum_names) {
                const godot::StringName enum_name = enum_name_value;
                const auto enum_name_utf8 = native_string(godot::String{enum_name});
                const auto already_present = std::find_if(
                    contract.enums.begin(), contract.enums.end(),
                    [&](const auto& enumeration) { return enumeration.name == enum_name_utf8; });
                if (enum_name_utf8.empty() || already_present != contract.enums.end())
                    continue;
                ExtensionBridgeEnum enumeration;
                enumeration.name = enum_name_utf8;
                enumeration.is_bitfield =
                    class_db->is_class_enum_bitfield(current, enum_name, true);
                const auto enum_constants =
                    class_db->class_get_enum_constants(current, enum_name, true);
                for (const auto& constant_name_value : enum_constants) {
                    const godot::StringName constant_name = constant_name_value;
                    enumeration.entries.push_back(
                        {native_string(godot::String{constant_name}),
                         class_db->class_get_integer_constant(current, constant_name)});
                }
                if (!enumeration.entries.empty())
                    contract.enums.push_back(std::move(enumeration));
            }
            current = class_db->get_parent_class(current);
        }
        contract.godot_base = current.is_empty() ? "Object" : native_string(godot::String{current});
        if (!current.is_empty()) {
            const auto base_api = class_db->class_get_api_type(current);
            contract.editor_only = contract.editor_only ||
                                   base_api == godot::ClassDBSingleton::API_EDITOR ||
                                   base_api == godot::ClassDBSingleton::API_EDITOR_EXTENSION;
        }

        std::sort(contract.members.begin(), contract.members.end(),
                  [](const auto& left, const auto& right) {
                      if (left.kind != right.kind)
                          return left.kind < right.kind;
                      return left.name < right.name;
                  });
        std::sort(contract.enums.begin(), contract.enums.end(),
                  [](const auto& left, const auto& right) { return left.name < right.name; });

        std::string identity = contract.gdscript_name + "\nbase:" + contract.godot_base +
                               "\neditor-only:" + (contract.editor_only ? "true\n" : "false\n");
        for (const auto& member : contract.members) {
            identity += std::to_string(static_cast<int>(member.kind)) + ":" + member.name + ":" +
                        member.type + ":" + (member.read_only ? "ro" : "rw") + ":" +
                        (member.vararg ? "vararg" : "fixed") + ":" +
                        (member.is_static ? "static" : "instance") + ":" +
                        std::to_string(member.constant_value) + ":" +
                        (member.has_method_hash ? std::to_string(member.method_hash) : "no-hash") +
                        "\n";
            for (const auto& parameter : member.parameters)
                identity += "arg:" + parameter.name + ":" + parameter.type + ":" +
                            (parameter.has_default ? "default" : "required") + "\n";
        }
        for (const auto& enumeration : contract.enums) {
            identity += "enum:" + enumeration.name + ":" +
                        (enumeration.is_bitfield ? "bitfield" : "enum") + "\n";
            for (const auto& entry : enumeration.entries)
                identity += "value:" + entry.name + ":" + std::to_string(entry.value) + "\n";
        }

        ExtensionBridge bridge;
        // Reflected contracts have no source file. Their exact ClassDB identity already enters
        // the script/build hashes; leaving the path empty prevents the native object cache from
        // treating a fictional manifest as perpetually stale.
        bridge.manifest_path.clear();
        bridge.provider = "ClassDB";
        bridge.abi = "classdb:" + contract.gdscript_name;
        bridge.contract_hash = sha256(identity);
        bridge.classes.push_back(std::move(contract));
        result.push_back(std::move(bridge));
    }
    return result;
}

godot::Dictionary invalid_version_result(const godot::String& value) {
    return failure_result("unsupported target Godot version '" + value +
                          "'; expected 4.4, 4.5, 4.6, or 4.7");
}

std::filesystem::path versioned_sdk_root(const std::filesystem::path& root, GodotVersion version,
                                         NativePlatform platform, std::string_view architecture,
                                         NativeWebThreadMode web_thread_mode) {
    const auto version_root = std::filesystem::is_regular_file(root / "sdk.manifest")
                                  ? root
                                  : root / godot_version_name(version);
    const auto target_root =
        version_root / native_platform_name(platform) / std::string{architecture};
    if (platform == NativePlatform::web) {
        const auto variant =
            web_thread_mode == NativeWebThreadMode::multi_threaded ? "threads" : "nothreads";
        if (std::filesystem::is_regular_file(target_root / variant / "sdk.manifest"))
            return target_root / variant;
    }
    if (std::filesystem::is_regular_file(target_root / "sdk.manifest"))
        return target_root;
    return version_root;
}

struct NativeProcessResult {
    int64_t exit_code{-1};
    std::string output;
    std::string launch_error;
};

#ifdef _WIN32
struct WindowsProcessOptions {
    const std::vector<wchar_t>* environment{nullptr};
    const std::wstring* desktop_name{nullptr};
    bool utf16_output{false};
};

NativeProcessResult execute_hidden_windows_process(const std::vector<std::wstring>& arguments,
                                                   const WindowsProcessOptions& options = {});

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {};
    const auto length = static_cast<int>(value.size());
    const int required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(),
                            required) != required)
        return {};
    return result;
}

std::optional<std::wstring> windows_environment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
        return std::nullopt;
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required)
        return std::nullopt;
    value.resize(static_cast<std::size_t>(copied));
    return value;
}

std::optional<std::filesystem::path> find_vcvars_batch(const std::filesystem::path& compiler) {
    const auto existing_file = [](const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    };
    if (const auto configured = windows_environment(L"GDPP_VCVARS_PATH")) {
        const std::filesystem::path path{*configured};
        if (existing_file(path))
            return path;
    }

    if (!compiler.empty() && compiler.has_parent_path()) {
        auto directory = std::filesystem::absolute(compiler).parent_path();
        while (!directory.empty()) {
            for (const auto& relative :
                 {std::filesystem::path{"Auxiliary/Build/vcvars64.bat"},
                  std::filesystem::path{"VC/Auxiliary/Build/vcvars64.bat"}}) {
                const auto candidate = directory / relative;
                if (existing_file(candidate))
                    return candidate;
            }
            const auto parent = directory.parent_path();
            if (parent == directory)
                break;
            directory = parent;
        }
    }

    std::vector<std::filesystem::path> vswhere_candidates;
    const auto append_vswhere_candidate = [&](const wchar_t* environment_name) {
        if (const auto root = windows_environment(environment_name)) {
            vswhere_candidates.emplace_back(std::filesystem::path{*root} /
                                            "Microsoft Visual Studio/Installer/vswhere.exe");
        }
    };
    append_vswhere_candidate(L"ProgramFiles(x86)");
    append_vswhere_candidate(L"ProgramFiles");
    std::sort(vswhere_candidates.begin(), vswhere_candidates.end());
    vswhere_candidates.erase(std::unique(vswhere_candidates.begin(), vswhere_candidates.end()),
                             vswhere_candidates.end());
    for (const auto& vswhere : vswhere_candidates) {
        if (!existing_file(vswhere))
            continue;
        const auto discovery = execute_hidden_windows_process(
            {vswhere.wstring(), L"-latest", L"-prerelease", L"-products", L"*", L"-requires",
             L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64", L"-property",
             L"installationPath", L"-utf8"});
        if (discovery.exit_code != 0 || !discovery.launch_error.empty())
            continue;
        for (std::size_t begin = 0; begin <= discovery.output.size();) {
            const auto end = discovery.output.find('\n', begin);
            auto line = discovery.output.substr(begin, end == std::string::npos ? std::string::npos
                                                                                : end - begin);
            while (!line.empty() &&
                   (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back()))))
                line.pop_back();
            constexpr char utf8_bom[] = "\xef\xbb\xbf";
            if (line.compare(0, sizeof(utf8_bom) - 1, utf8_bom) == 0)
                line.erase(0, 3);
            const auto first = std::find_if_not(line.begin(), line.end(), [](const char value) {
                return std::isspace(static_cast<unsigned char>(value)) != 0;
            });
            line.erase(line.begin(), first);
            if (!line.empty()) {
                const auto installation = utf8_to_wide(line);
                if (!installation.empty()) {
                    const auto candidate =
                        std::filesystem::path{installation} / "VC/Auxiliary/Build/vcvars64.bat";
                    if (existing_file(candidate))
                        return candidate;
                }
            }
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
    }

    std::vector<std::filesystem::path> roots;
    const auto append_environment_root = [&roots](const wchar_t* name) {
        if (const auto value = windows_environment(name))
            roots.emplace_back(*value);
    };
    append_environment_root(L"VSINSTALLDIR");
    append_environment_root(L"VCINSTALLDIR");

    const auto append_visual_studio_roots = [&roots](const wchar_t* environment_name) {
        const auto value = windows_environment(environment_name);
        if (!value)
            return;
        const auto base = std::filesystem::path{*value} / "Microsoft Visual Studio";
        for (const auto* year : {L"2026", L"2022", L"2019"}) {
            for (const auto* edition :
                 {L"BuildTools", L"Community", L"Professional", L"Enterprise", L"Preview"})
                roots.emplace_back(base / year / edition);
        }
    };
    append_visual_studio_roots(L"ProgramFiles");
    append_visual_studio_roots(L"ProgramFiles(x86)");

    for (const auto& root : roots) {
        for (const auto& relative : {std::filesystem::path{"VC/Auxiliary/Build/vcvars64.bat"},
                                     std::filesystem::path{"Auxiliary/Build/vcvars64.bat"}}) {
            const auto candidate = root / relative;
            if (existing_file(candidate))
                return candidate;
        }
    }
    return std::nullopt;
}

std::wstring quote_windows_argument(const std::wstring& value) {
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::string windows_text_to_utf8(const std::string& input) {
    if (input.empty())
        return {};
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int wide_length = MultiByteToWideChar(code_page, flags, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (wide_length == 0) {
        code_page = CP_ACP;
        flags = 0;
        wide_length = MultiByteToWideChar(code_page, flags, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    }
    if (wide_length == 0)
        return input;
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(code_page, flags, input.data(), static_cast<int>(input.size()),
                            wide.data(), wide_length) != wide_length)
        return input;
    const int utf8_length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, nullptr, 0, nullptr, nullptr);
    if (utf8_length == 0)
        return input;
    std::string output(static_cast<std::size_t>(utf8_length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, output.data(), utf8_length,
                            nullptr, nullptr) != utf8_length)
        return input;
    return output;
}

std::string windows_wide_text_to_utf8(const std::wstring& input) {
    if (input.empty())
        return {};
    const int length = static_cast<int>(
        std::min(input.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    const int required =
        WideCharToMultiByte(CP_UTF8, 0, input.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, input.data(), length, output.data(), required, nullptr,
                            nullptr) != required)
        return {};
    return output;
}

std::string windows_utf16_output_to_utf8(const std::string& input) {
    if (input.empty())
        return {};
    const auto code_units = input.size() / sizeof(wchar_t);
    if (code_units == 0)
        return {};
    std::wstring wide(code_units, L'\0');
    std::memcpy(wide.data(), input.data(), code_units * sizeof(wchar_t));
    if (!wide.empty() && wide.front() == wchar_t{0xfeff})
        wide.erase(wide.begin());
    while (!wide.empty() && wide.back() == L'\0')
        wide.pop_back();
    return windows_wide_text_to_utf8(wide);
}

bool is_msvc_tool(const std::filesystem::path& executable) {
    auto filename = executable.filename().wstring();
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return filename == L"cl" || filename == L"cl.exe" || filename == L"link" ||
           filename == L"link.exe";
}

class HiddenToolchainDesktop final {
  public:
    HiddenToolchainDesktop()
        : name_{L"GDPPToolchain-" + std::to_wstring(GetCurrentProcessId())},
          handle_{CreateDesktopW(name_.data(), nullptr, nullptr, 0, GENERIC_ALL, nullptr)},
          creation_error_{handle_ == nullptr ? GetLastError() : DWORD{0}} {}

    ~HiddenToolchainDesktop() {
        if (handle_ != nullptr)
            CloseDesktop(handle_);
    }

    HiddenToolchainDesktop(const HiddenToolchainDesktop&) = delete;
    HiddenToolchainDesktop& operator=(const HiddenToolchainDesktop&) = delete;

    [[nodiscard]] bool available() const { return handle_ != nullptr; }
    [[nodiscard]] const std::wstring& name() const { return name_; }
    [[nodiscard]] DWORD creation_error() const { return creation_error_; }

  private:
    std::wstring name_;
    HDESK handle_{nullptr};
    DWORD creation_error_{0};
};

HiddenToolchainDesktop& hidden_toolchain_desktop() {
    static HiddenToolchainDesktop desktop;
    return desktop;
}

NativeProcessResult execute_hidden_windows_command_line(std::wstring command_line,
                                                        const WindowsProcessOptions& options = {}) {
    NativeProcessResult result;
    if (command_line.empty())
        return result;
    std::vector<wchar_t> mutable_command_line{command_line.begin(), command_line.end()};
    mutable_command_line.push_back(L'\0');

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (CreatePipe(&output_read, &output_write, &security_attributes, 0) == FALSE) {
        result.launch_error = "cannot create the toolchain output pipe (Windows error " +
                              std::to_string(GetLastError()) + ")";
        return result;
    }
    if (SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        result.launch_error = "cannot isolate the toolchain output pipe (Windows error " +
                              std::to_string(GetLastError()) + ")";
        CloseHandle(output_read);
        CloseHandle(output_write);
        return result;
    }
    HANDLE input_handle =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input_handle == INVALID_HANDLE_VALUE) {
        result.launch_error = "cannot open the null toolchain input (Windows error " +
                              std::to_string(GetLastError()) + ")";
        CloseHandle(output_read);
        CloseHandle(output_write);
        return result;
    }

    STARTUPINFOW startup_information{};
    startup_information.cb = sizeof(startup_information);
    startup_information.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup_information.wShowWindow = SW_HIDE;
    startup_information.hStdOutput = output_write;
    startup_information.hStdError = output_write;
    startup_information.hStdInput = input_handle;
    startup_information.lpDesktop = options.desktop_name == nullptr
                                        ? nullptr
                                        : const_cast<wchar_t*>(options.desktop_name->data());
    DWORD creation_flags = CREATE_NO_WINDOW;
    if (options.environment != nullptr)
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    void* environment = options.environment == nullptr
                            ? nullptr
                            : const_cast<wchar_t*>(options.environment->data());
    PROCESS_INFORMATION process_information{};
    if (CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, creation_flags,
                       environment, nullptr, &startup_information, &process_information) == FALSE) {
        result.launch_error =
            "cannot start the C++ toolchain (Windows error " + std::to_string(GetLastError()) + ")";
        CloseHandle(input_handle);
        CloseHandle(output_read);
        CloseHandle(output_write);
        return result;
    }

    CloseHandle(input_handle);
    CloseHandle(process_information.hThread);
    CloseHandle(output_write);
    constexpr std::size_t maximum_captured_output = 512U * 1024U;
    bool output_truncated = false;
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(output_read, buffer, sizeof(buffer), &bytes_read, nullptr) != FALSE &&
           bytes_read != 0) {
        const auto remaining = maximum_captured_output - result.output.size();
        const auto captured = std::min(remaining, static_cast<std::size_t>(bytes_read));
        result.output.append(buffer, captured);
        output_truncated = output_truncated || captured != bytes_read;
    }
    CloseHandle(output_read);
    const auto wait_result = WaitForSingleObject(process_information.hProcess, INFINITE);
    DWORD exit_code = 0;
    const bool completed = wait_result == WAIT_OBJECT_0 &&
                           GetExitCodeProcess(process_information.hProcess, &exit_code) != FALSE;
    CloseHandle(process_information.hProcess);
    result.exit_code = completed ? static_cast<int64_t>(exit_code) : int64_t{-1};
    result.output = options.utf16_output ? windows_utf16_output_to_utf8(result.output)
                                         : windows_text_to_utf8(result.output);
    if (output_truncated)
        result.output += "\n[toolchain output truncated after 512 KiB]";
    return result;
}

NativeProcessResult execute_hidden_windows_process(const std::vector<std::wstring>& arguments,
                                                   const WindowsProcessOptions& options) {
    if (arguments.empty() || arguments.front().empty())
        return {};
    std::wstring command_line;
    for (const auto& argument : arguments) {
        if (!command_line.empty())
            command_line.push_back(L' ');
        command_line += quote_windows_argument(argument);
    }
    return execute_hidden_windows_command_line(std::move(command_line), options);
}

struct MsvcEnvironmentSnapshot {
    std::vector<std::wstring> entries;
    std::vector<wchar_t> block;
    std::string diagnostic;

    [[nodiscard]] bool valid() const { return !block.empty() && diagnostic.empty(); }
};

std::wstring environment_entry_name(const std::wstring& entry) {
    const auto separator =
        entry.empty() || entry.front() != L'=' ? entry.find(L'=') : entry.find(L'=', 1);
    return separator == std::wstring::npos ? std::wstring{} : entry.substr(0, separator);
}

bool equal_windows_environment_name(const std::wstring& left, const std::wstring& right) {
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::wstring> snapshot_environment_value(const MsvcEnvironmentSnapshot& snapshot,
                                                       const std::wstring& name) {
    for (const auto& entry : snapshot.entries) {
        if (!equal_windows_environment_name(environment_entry_name(entry), name))
            continue;
        const auto separator = entry.find(L'=');
        if (separator != std::wstring::npos)
            return entry.substr(separator + 1);
    }
    return std::nullopt;
}

MsvcEnvironmentSnapshot parse_msvc_environment(std::string output) {
    MsvcEnvironmentSnapshot snapshot;
    for (std::size_t begin = 0; begin <= output.size();) {
        const auto end = output.find('\n', begin);
        auto line = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty() && line.find('=') != std::string::npos) {
            auto wide = utf8_to_wide(line);
            if (!wide.empty())
                snapshot.entries.push_back(std::move(wide));
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    std::sort(snapshot.entries.begin(), snapshot.entries.end(),
              [](const auto& left, const auto& right) {
                  return _wcsicmp(left.c_str(), right.c_str()) < 0;
              });
    snapshot.entries.erase(std::unique(snapshot.entries.begin(), snapshot.entries.end(),
                                       [](const auto& left, const auto& right) {
                                           return equal_windows_environment_name(
                                               environment_entry_name(left),
                                               environment_entry_name(right));
                                       }),
                           snapshot.entries.end());
    if (snapshot.entries.empty()) {
        snapshot.diagnostic = "Visual Studio environment bootstrap returned no variables";
        return snapshot;
    }
    std::size_t block_size = 1;
    for (const auto& entry : snapshot.entries)
        block_size += entry.size() + 1;
    snapshot.block.reserve(block_size);
    for (const auto& entry : snapshot.entries) {
        snapshot.block.insert(snapshot.block.end(), entry.begin(), entry.end());
        snapshot.block.push_back(L'\0');
    }
    snapshot.block.push_back(L'\0');
    return snapshot;
}

MsvcEnvironmentSnapshot capture_msvc_environment(const std::filesystem::path& vcvars) {
    MsvcEnvironmentSnapshot snapshot;
    auto& desktop = hidden_toolchain_desktop();
    if (!desktop.available()) {
        snapshot.diagnostic = "cannot create the isolated MSVC bootstrap desktop (Windows error " +
                              std::to_string(desktop.creation_error()) + ")";
        return snapshot;
    }
    std::wstring command_line = quote_windows_argument(L"cmd.exe") + L" /d /s /u /c ";
    command_line += L"set \"VSCMD_SKIP_SENDTELEMETRY=1\" && "
                    L"set \"VSCMD_SKIP_VCPKG_ACTIVATION=1\" && call ";
    command_line += quote_windows_argument(vcvars.wstring());
    command_line += L" >nul && set";
    WindowsProcessOptions options;
    options.desktop_name = &desktop.name();
    options.utf16_output = true;
    auto process_result = execute_hidden_windows_command_line(std::move(command_line), options);
    if (process_result.exit_code != 0) {
        snapshot.diagnostic = !process_result.launch_error.empty()
                                  ? std::move(process_result.launch_error)
                                  : "Visual Studio environment bootstrap failed with exit code " +
                                        std::to_string(process_result.exit_code);
        return snapshot;
    }
    return parse_msvc_environment(std::move(process_result.output));
}

std::shared_ptr<const MsvcEnvironmentSnapshot>
cached_msvc_environment(const std::filesystem::path& vcvars) {
    auto key = vcvars.wstring();
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    static std::mutex cache_mutex;
    static std::unordered_map<std::wstring, std::shared_ptr<const MsvcEnvironmentSnapshot>> cache;
    std::lock_guard<std::mutex> lock{cache_mutex};
    const auto existing = cache.find(key);
    if (existing != cache.end())
        return existing->second;
    auto snapshot =
        std::make_shared<const MsvcEnvironmentSnapshot>(capture_msvc_environment(vcvars));
    cache.emplace(std::move(key), snapshot);
    return snapshot;
}

std::optional<std::wstring> resolve_msvc_executable(const std::wstring& executable,
                                                    const MsvcEnvironmentSnapshot& environment) {
    const std::filesystem::path requested{executable};
    std::error_code error;
    if (requested.has_parent_path() && std::filesystem::is_regular_file(requested, error))
        return requested.wstring();
    const auto path = snapshot_environment_value(environment, L"PATH");
    if (!path)
        return std::nullopt;
    for (std::size_t begin = 0; begin <= path->size();) {
        const auto end = path->find(L';', begin);
        auto directory =
            path->substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (directory.size() >= 2 && directory.front() == L'"' && directory.back() == L'"')
            directory = directory.substr(1, directory.size() - 2);
        if (!directory.empty()) {
            const auto candidate = std::filesystem::path{directory} / requested;
            error.clear();
            if (std::filesystem::is_regular_file(candidate, error))
                return candidate.wstring();
        }
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return std::nullopt;
}

struct ResolvedMsvcCompiler {
    std::string executable;
    std::string diagnostic;

    [[nodiscard]] bool valid() const { return !executable.empty() && diagnostic.empty(); }
};

ResolvedMsvcCompiler resolve_msvc_compiler_for_plan(const std::string& configured_executable) {
    const auto requested_utf8 =
        configured_executable.empty() ? std::string{"cl.exe"} : configured_executable;
    const auto requested_wide = utf8_to_wide(requested_utf8);
    if (requested_wide.empty())
        return {{}, "configured MSVC compiler path is not valid UTF-8"};

    const std::filesystem::path requested{requested_wide};
    if (!is_msvc_tool(requested))
        return {requested_utf8, {}};

    std::error_code error;
    if (requested.has_parent_path() && std::filesystem::is_regular_file(requested, error)) {
        error.clear();
        const auto absolute = std::filesystem::absolute(requested, error);
        const auto resolved = error ? requested.wstring() : absolute.wstring();
        const auto executable = windows_wide_text_to_utf8(resolved);
        return executable.empty()
                   ? ResolvedMsvcCompiler{{}, "configured MSVC compiler path is not valid UTF-8"}
                   : ResolvedMsvcCompiler{executable, {}};
    }

    const auto vcvars = find_vcvars_batch(requested);
    if (!vcvars) {
        return {{},
                "cannot locate vcvars64.bat after checking GDPP_VCVARS_PATH, the configured "
                "compiler, vswhere, Visual Studio Build Tools, and standard installations; "
                "install the x64 C++ tools component or configure gdpp/build/cpp_compiler"};
    }
    const auto environment = cached_msvc_environment(*vcvars);
    if (!environment->valid())
        return {{}, environment->diagnostic};
    const auto resolved = resolve_msvc_executable(requested_wide, *environment);
    if (!resolved)
        return {{},
                "cannot resolve '" + requested_utf8 +
                    "' from the initialized Visual Studio environment"};
    const auto executable = windows_wide_text_to_utf8(*resolved);
    return executable.empty()
               ? ResolvedMsvcCompiler{{}, "resolved MSVC compiler path is not valid UTF-8"}
               : ResolvedMsvcCompiler{executable, {}};
}
#endif

NativeProcessResult execute_native_process(const std::string& executable,
                                           const std::vector<std::string>& arguments) {
#ifdef _WIN32
    auto wide_executable = utf8_to_wide(executable);
    if (wide_executable.empty())
        return {};
    std::vector<std::wstring> wide_arguments;
    wide_arguments.reserve(arguments.size() + 1);
    wide_arguments.push_back(wide_executable);
    for (const auto& argument : arguments) {
        auto converted = utf8_to_wide(argument);
        if (!argument.empty() && converted.empty())
            return {};
        wide_arguments.push_back(std::move(converted));
    }
    if (is_msvc_tool(std::filesystem::path{wide_executable})) {
        const std::filesystem::path requested{wide_executable};
        const bool requires_resolution = !requested.has_parent_path();
        const bool requires_environment = !windows_environment(L"INCLUDE");
        if (requires_resolution || requires_environment) {
            const auto vcvars = find_vcvars_batch(std::filesystem::path{wide_executable});
            if (!vcvars) {
                NativeProcessResult result;
                result.launch_error =
                    "cannot locate vcvars64.bat after checking GDPP_VCVARS_PATH, the configured "
                    "compiler, vswhere, Visual Studio Build Tools, and standard installations; "
                    "install the x64 C++ tools component or configure gdpp/build/cpp_compiler";
                return result;
            }
            const auto environment = cached_msvc_environment(*vcvars);
            if (!environment->valid()) {
                NativeProcessResult result;
                result.launch_error = environment->diagnostic;
                return result;
            }
            if (requires_resolution) {
                const auto resolved = resolve_msvc_executable(wide_executable, *environment);
                if (!resolved) {
                    NativeProcessResult result;
                    result.launch_error = "cannot resolve '" + executable +
                                          "' from the initialized Visual Studio environment";
                    return result;
                }
                wide_arguments.front() = *resolved;
            }
            WindowsProcessOptions options;
            options.environment = &environment->block;
            return execute_hidden_windows_process(wide_arguments, options);
        }
    }
    return execute_hidden_windows_process(wide_arguments);
#else
    NativeProcessResult result;
    if (executable.empty())
        return result;
    int output_pipe[2]{};
    if (pipe(output_pipe) != 0) {
        result.launch_error =
            "cannot create the toolchain output pipe: " + std::string{std::strerror(errno)};
        return result;
    }
    std::vector<char*> process_arguments;
    process_arguments.reserve(arguments.size() + 2);
    process_arguments.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments)
        process_arguments.push_back(const_cast<char*>(argument.c_str()));
    process_arguments.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        result.launch_error = "cannot initialize toolchain process redirection";
        return result;
    }
    (void)posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
    pid_t process = 0;
    const int spawn_error = posix_spawnp(&process, executable.c_str(), &actions, nullptr,
                                         process_arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(output_pipe[1]);
    if (spawn_error != 0) {
        close(output_pipe[0]);
        result.launch_error =
            "cannot start the C++ toolchain: " + std::string{std::strerror(spawn_error)};
        return result;
    }
    constexpr std::size_t maximum_captured_output = 512U * 1024U;
    bool output_truncated = false;
    char buffer[4096];
    ssize_t bytes_read = 0;
    while ((bytes_read = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
        const auto remaining = maximum_captured_output - result.output.size();
        const auto captured = std::min(remaining, static_cast<std::size_t>(bytes_read));
        result.output.append(buffer, captured);
        output_truncated = output_truncated || captured != static_cast<std::size_t>(bytes_read);
    }
    close(output_pipe[0]);
    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(process, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        result.launch_error =
            "cannot wait for the C++ toolchain: " + std::string{std::strerror(errno)};
        return result;
    }
    if (WIFEXITED(status))
        result.exit_code = static_cast<int64_t>(WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        result.exit_code = static_cast<int64_t>(128 + WTERMSIG(status));
    if (output_truncated)
        result.output += "\n[toolchain output truncated after 512 KiB]";
    return result;
#endif
}

struct NativeInvocation {
    std::string executable;
    std::vector<std::string> arguments;
    std::size_t stage{0};
};

std::string native_invocation_label(const NativeInvocation& invocation) {
    for (std::size_t index = 0; index + 1 < invocation.arguments.size(); ++index) {
        if (invocation.arguments[index] == "/c" || invocation.arguments[index] == "-c")
            return std::filesystem::path{invocation.arguments[index + 1]}.filename().string();
    }
    const auto executable = std::filesystem::path{invocation.executable}.filename().string();
    return executable.empty() ? invocation.executable : executable;
}

std::string trimmed_toolchain_output(std::string output) {
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' ||
                               output.back() == ' ' || output.back() == '\t'))
        output.pop_back();
    return output;
}

std::vector<std::string> export_worker_diagnostics(const std::string& output) {
    std::vector<std::string> diagnostics;
    for (std::size_t begin = 0; begin <= output.size();) {
        const auto end = output.find('\n', begin);
        auto line =
            output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos)
            line.erase(0, first);
        const bool diagnostic = line.rfind("ERROR:", 0) == 0 ||
                                line.rfind("SCRIPT ERROR:", 0) == 0 ||
                                line.rfind("WARNING:", 0) == 0 ||
                                line.rfind("Unable to open", 0) == 0 ||
                                line.find("[toolchain output truncated") != std::string::npos;
        // Godot's shader compiler reports this known non-fatal continuation for custom samplers
        // in valid Pixelorama shaders on both source and exported runs. Keep the allowlist exact;
        // every other child-process diagnostic remains a release-blocking transform failure.
        const bool known_custom_sampler_continuation =
            line ==
            "ERROR: Condition "
            "\"!actions.custom_samplers.has(function->arguments[j].tex_builtin)\" is true. "
            "Continuing.";
        if (diagnostic && !known_custom_sampler_continuation)
            diagnostics.push_back(std::move(line));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return diagnostics;
}

void report_build_progress(const godot::Callable& callback, const char* phase,
                           const std::size_t completed, const std::size_t total) {
    if (!callback.is_valid())
        return;
    (void)callback.call(godot::String{phase}, static_cast<int64_t>(completed),
                        static_cast<int64_t>(total));
}

} // namespace

void GDPPCompiler::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("compile_source", "source", "virtual_path", "target_version"),
        &GDPPCompiler::compile_source, DEFVAL("4.4"));
    godot::ClassDB::bind_method(
        godot::D_METHOD("compile_file", "source_path", "output_directory", "target_version"),
        &GDPPCompiler::compile_file, DEFVAL("4.4"));
    godot::ClassDB::bind_method(
        godot::D_METHOD("compile_project", "project_root", "output_directory", "sdk_root",
                        "compiler_executable", "target_version", "build_profile", "target_platform",
                        "target_architecture", "target_variant", "target_precision",
                        "progress_callback"),
        &GDPPCompiler::compile_project, DEFVAL("4.4"), DEFVAL("release"), DEFVAL(""), DEFVAL(""),
        DEFVAL(""), DEFVAL(GDPP_GODOT_PRECISION), DEFVAL(godot::Callable{}));
    godot::ClassDB::bind_method(godot::D_METHOD("get_default_sdk_root"),
                                &GDPPCompiler::get_default_sdk_root);
    godot::ClassDB::bind_method(godot::D_METHOD("get_default_compiler_executable"),
                                &GDPPCompiler::get_default_compiler_executable);
    godot::ClassDB::bind_method(godot::D_METHOD("get_host_platform"),
                                &GDPPCompiler::get_host_platform);
    godot::ClassDB::bind_method(godot::D_METHOD("get_host_architecture"),
                                &GDPPCompiler::get_host_architecture);
    godot::ClassDB::bind_method(godot::D_METHOD("is_target_supported", "platform", "architecture"),
                                &GDPPCompiler::is_target_supported);
    godot::ClassDB::bind_method(godot::D_METHOD("get_supported_godot_versions"),
                                &GDPPCompiler::get_supported_godot_versions);
    godot::ClassDB::bind_method(
        godot::D_METHOD("execute_project_build", "build_plan", "progress_callback"),
        &GDPPCompiler::execute_project_build, DEFVAL(godot::Callable{}));
    godot::ClassDB::bind_method(godot::D_METHOD("prepare_project_build"),
                                &GDPPCompiler::prepare_project_build);
    godot::ClassDB::bind_method(godot::D_METHOD("install_editor_script_descriptors", "descriptors"),
                                &GDPPCompiler::install_editor_script_descriptors);
    godot::ClassDB::bind_method(
        godot::D_METHOD("set_editor_script_storage_state", "object", "stored_properties"),
        &GDPPCompiler::set_editor_script_storage_state);
    godot::ClassDB::bind_method(godot::D_METHOD("clear_editor_script_descriptors"),
                                &GDPPCompiler::clear_editor_script_descriptors);
    godot::ClassDB::bind_method(
        godot::D_METHOD("run_export_transform_worker", "state_path", "result_path"),
        &GDPPCompiler::run_export_transform_worker);
}

GDPPCompiler::BuildExecutionResult
GDPPCompiler::execute_build_commands(const godot::Array& commands,
                                     const godot::Callable& progress_callback) const {
    BuildExecutionResult result;
    std::vector<NativeInvocation> invocations;
    invocations.reserve(static_cast<std::size_t>(commands.size()));
    for (int64_t index = 0; index < commands.size(); ++index) {
        const godot::Dictionary item = commands[index];
        NativeInvocation invocation;
        invocation.executable = native_string(item.get("executable", godot::String{}));
        const godot::PackedStringArray arguments =
            item.get("arguments", godot::PackedStringArray{});
        invocation.arguments.reserve(static_cast<std::size_t>(arguments.size()));
        for (int64_t argument = 0; argument < arguments.size(); ++argument)
            invocation.arguments.push_back(native_string(arguments[argument]));
        if (invocation.executable.empty()) {
            result.diagnostics.push_back("native build command has no executable");
            return result;
        }
        const auto stage = static_cast<int64_t>(item.get("stage", int64_t{0}));
        if (stage < 0) {
            result.diagnostics.push_back("native build command has an invalid stage");
            return result;
        }
        invocation.stage = static_cast<std::size_t>(stage);
        invocations.push_back(std::move(invocation));
    }
    if (invocations.empty()) {
        result.exit_code = 0;
        return result;
    }

    std::stable_sort(invocations.begin(), invocations.end(),
                     [](const auto& left, const auto& right) { return left.stage < right.stage; });
    for (std::size_t begin = 0; begin < invocations.size();) {
        std::size_t end = begin + 1;
        while (end < invocations.size() && invocations[end].stage == invocations[begin].stage)
            ++end;
        const auto count = end - begin;
        const auto* phase = invocations[begin].stage == 0 ? "compile" : "link";
        report_build_progress(progress_callback, phase, 0, count);
        for (std::size_t index = begin; index < end; ++index) {
            const auto& invocation = invocations[index];
            auto process_result =
                execute_native_process(invocation.executable, invocation.arguments);
            report_build_progress(progress_callback, phase, index - begin + 1, count);
            if (process_result.exit_code != 0) {
                result.exit_code = process_result.exit_code;
                const auto label = native_invocation_label(invocations[index]);
                const auto action = invocations[index].stage == 0 ? "compile" : "link";
                result.diagnostics.push_back(godot::String::utf8(
                    ("C++ " + std::string{action} + " command for '" + label +
                     "' failed with exit code " + std::to_string(process_result.exit_code))
                        .c_str()));
                if (!process_result.launch_error.empty()) {
                    result.diagnostics.push_back(
                        godot::String::utf8(process_result.launch_error.c_str()));
                }
                const auto output = trimmed_toolchain_output(std::move(process_result.output));
                if (!output.empty()) {
                    result.diagnostics.push_back(godot::String::utf8(
                        ("toolchain output for '" + label + "':\n" + output).c_str()));
                }
                return result;
            }
        }
        begin = end;
    }
    result.exit_code = 0;
    return result;
}

godot::Dictionary
GDPPCompiler::execute_project_build(const godot::Dictionary& build_plan,
                                    const godot::Callable& progress_callback) const {
    godot::Dictionary output;
    output["success"] = false;
    output["exit_code"] = int64_t{-1};
    godot::PackedStringArray diagnostics =
        build_plan.get("diagnostics", godot::PackedStringArray{});
    godot::PackedStringArray errors;
    godot::PackedStringArray warnings;
    godot::PackedStringArray notes;
    const bool has_severity_channels =
        build_plan.has("errors") || build_plan.has("warnings") || build_plan.has("notes");
    if (has_severity_channels) {
        errors = build_plan.get("errors", godot::PackedStringArray{});
        warnings = build_plan.get("warnings", godot::PackedStringArray{});
        notes = build_plan.get("notes", godot::PackedStringArray{});
    } else if (static_cast<bool>(build_plan.get("success", false))) {
        // Plans produced before severity channels were introduced only reached execution when
        // their diagnostics were non-fatal.
        warnings = diagnostics;
    } else {
        errors = diagnostics;
    }

    if (!static_cast<bool>(build_plan.get("success", false))) {
        if (errors.is_empty()) {
            const godot::String message{"native build plan failed without an error diagnostic"};
            diagnostics.push_back(message);
            errors.push_back(message);
        }
        assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
        return output;
    }

    const godot::Array commands = build_plan.get("build_commands", godot::Array{});
    const auto execution = execute_build_commands(commands, progress_callback);
    output["exit_code"] = execution.exit_code;
    if (execution.exit_code != 0) {
        for (const auto& diagnostic : execution.diagnostics) {
            diagnostics.push_back(diagnostic);
            errors.push_back(diagnostic);
        }
        if (execution.diagnostics.is_empty()) {
            const godot::String message = "C++ toolchain failed with exit code " +
                                          godot::String::num_int64(execution.exit_code);
            diagnostics.push_back(message);
            errors.push_back(message);
        }
        assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
        return output;
    }

    const godot::String output_library = build_plan.get("output_library", godot::String{});
    auto* settings = godot::ProjectSettings::get_singleton();
    const auto output_path = native_string(settings->globalize_path(output_library));
    const godot::String pending_output = build_plan.get("pending_output_library", godot::String{});
    if (!pending_output.is_empty()) {
        const auto pending_path = native_string(settings->globalize_path(pending_output));
        std::string commit_diagnostic;
        if (!commit_xcframework(path_from_utf8(pending_path), path_from_utf8(output_path),
                                commit_diagnostic)) {
            const godot::String message{commit_diagnostic.c_str()};
            diagnostics.push_back(message);
            errors.push_back(message);
            assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
            return output;
        }
    }
    std::error_code file_error;
    const auto output_filesystem_path = path_from_utf8(output_path);
    const bool artifact_exists =
        std::filesystem::is_regular_file(output_filesystem_path, file_error) ||
        is_xcframework(output_filesystem_path);
    if (output_path.empty() || !artifact_exists || file_error) {
        const godot::String message =
            "native build completed without producing the planned library '" + output_library + "'";
        diagnostics.push_back(message);
        errors.push_back(message);
        assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
        return output;
    }

    output["success"] = true;
    assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
    report_build_progress(progress_callback, "complete", static_cast<std::size_t>(commands.size()),
                          static_cast<std::size_t>(commands.size()));
    return output;
}

void GDPPCompiler::prepare_project_build() {
    auto reflected = reflect_extension_contracts();
    const std::lock_guard lock{reflected_bridges_mutex_};
    reflected_bridges_ = std::move(reflected);
}

godot::Dictionary
GDPPCompiler::install_editor_script_descriptors(const godot::Array& descriptors) const {
    godot::Dictionary output;
    output["success"] = false;
    godot::PackedStringArray diagnostics;
    gdpp::runtime::unregister_all_attached_scripts();

    std::vector<godot::String> registered_paths;
    registered_paths.reserve(static_cast<std::size_t>(descriptors.size()));
    for (std::int64_t index = 0; index < descriptors.size(); ++index) {
        if (descriptors[index].get_type() != godot::Variant::DICTIONARY) {
            diagnostics.push_back("editor script descriptor at index " +
                                  godot::String::num_int64(index) + " is not a Dictionary");
            gdpp::runtime::unregister_all_attached_scripts();
            output["diagnostics"] = diagnostics;
            return output;
        }

        const godot::Dictionary input = descriptors[index];
        gdpp::runtime::AttachedScriptDescriptor descriptor;
        descriptor.source_path = input.get("source_path", godot::String{});
        descriptor.global_name = input.get("global_name", godot::StringName{});
        descriptor.native_base_type = input.get("native_base_type", godot::StringName{});
        descriptor.base_script_path = input.get("base_script_path", godot::String{});
        descriptor.contract_hash = input.get("contract_hash", godot::String{});
        descriptor.behavior_class = input.get("behavior_class", godot::StringName{});
        descriptor.tool = input.get("tool", false);
        descriptor.abstract = input.get("abstract", false);
        descriptor.editor_metadata_only = true;
        descriptor.constants = input.get("constants", godot::Dictionary{});
        descriptor.rpc_config = input.get("rpc_config", godot::Variant{});

        const godot::Array properties = input.get("properties", godot::Array{});
        descriptor.properties.reserve(static_cast<std::size_t>(properties.size()));
        for (std::int64_t property_index = 0; property_index < properties.size();
             ++property_index) {
            if (properties[property_index].get_type() != godot::Variant::DICTIONARY) {
                diagnostics.push_back("property metadata at index " +
                                      godot::String::num_int64(property_index) + " for '" +
                                      descriptor.source_path + "' is not a Dictionary");
                gdpp::runtime::unregister_all_attached_scripts();
                output["diagnostics"] = diagnostics;
                return output;
            }
            const godot::Dictionary property_input = properties[property_index];
            const godot::Dictionary property_info = property_input.get("info", godot::Dictionary{});
            gdpp::runtime::AttachedScriptProperty property;
            property.info = godot::PropertyInfo::from_dict(property_info);
            property.has_default = property_input.get("has_default", false);
            if (property.has_default)
                property.default_value = property_input.get("default_value", godot::Variant{});
            descriptor.properties.push_back(std::move(property));
        }

        const auto append_methods = [&](const char* key,
                                        std::vector<godot::MethodInfo>& destination) -> bool {
            const godot::Array methods = input.get(key, godot::Array{});
            destination.reserve(static_cast<std::size_t>(methods.size()));
            for (std::int64_t method_index = 0; method_index < methods.size(); ++method_index) {
                if (methods[method_index].get_type() != godot::Variant::DICTIONARY) {
                    diagnostics.push_back(godot::String{key} + " metadata at index " +
                                          godot::String::num_int64(method_index) + " for '" +
                                          descriptor.source_path + "' is not a Dictionary");
                    return false;
                }
                destination.push_back(
                    godot::MethodInfo::from_dict(godot::Dictionary{methods[method_index]}));
            }
            return true;
        };
        if (!append_methods("methods", descriptor.methods) ||
            !append_methods("signals", descriptor.signals)) {
            gdpp::runtime::unregister_all_attached_scripts();
            output["diagnostics"] = diagnostics;
            return output;
        }

        godot::String error;
        const auto source_path = descriptor.source_path;
        if (!gdpp::runtime::register_attached_script(std::move(descriptor), &error)) {
            diagnostics.push_back(error);
            gdpp::runtime::unregister_all_attached_scripts();
            output["diagnostics"] = diagnostics;
            return output;
        }
        registered_paths.push_back(source_path);
    }

    for (const auto& path : registered_paths) {
        godot::String error;
        if (!gdpp::runtime::resolve_attached_script(path, &error)) {
            diagnostics.push_back(error);
            gdpp::runtime::unregister_all_attached_scripts();
            output["diagnostics"] = diagnostics;
            return output;
        }
    }
    output["success"] = true;
    output["registered_count"] = static_cast<std::int64_t>(registered_paths.size());
    output["diagnostics"] = diagnostics;
    return output;
}

void GDPPCompiler::clear_editor_script_descriptors() const {
    gdpp::runtime::unregister_all_attached_scripts();
}

godot::Dictionary
GDPPCompiler::run_export_transform_worker(const godot::String& state_path,
                                          const godot::String& result_path) const {
    constexpr std::string_view worker_prefix{"res://addons/gdpp/build/project/export-worker/"};
    const auto state_resource = native_string(state_path);
    const auto result_resource = native_string(result_path);
    const auto safe_path = [&](const std::string& path) {
        return path.size() >= worker_prefix.size() &&
               std::equal(worker_prefix.begin(), worker_prefix.end(), path.begin()) &&
               path.find("..") == std::string::npos &&
               path.find('\\') == std::string::npos;
    };
    if (!safe_path(state_resource) || !safe_path(result_resource))
        return failure_result("export transform worker paths must stay inside its transaction");

    auto* settings = godot::ProjectSettings::get_singleton();
    const auto project_root = native_string(settings->globalize_path("res://"));
    const auto state_absolute = native_string(settings->globalize_path(state_path));
    const auto result_absolute = native_string(settings->globalize_path(result_path));
    if (project_root.empty() || state_absolute.empty() || result_absolute.empty() ||
        !std::filesystem::is_regular_file(path_from_utf8(state_absolute)))
        return failure_result("export transform worker state is unavailable");

    const auto state_native = path_from_utf8(state_absolute);
    const auto result_native = path_from_utf8(result_absolute);
    const auto transaction_root = state_native.parent_path();
    if (result_native.parent_path() != transaction_root)
        return failure_result("export transform worker state and result must share a transaction");
    const auto snapshot_root = transaction_root / "project-snapshot";
    const auto snapshot =
        create_export_worker_snapshot(path_from_utf8(project_root), snapshot_root);
    if (!snapshot.success)
        return failure_result("cannot isolate the export transform worker: " +
                              godot::String::utf8(snapshot.diagnostic.c_str()));

    const auto executable =
        native_string(godot::OS::get_singleton()->get_executable_path());
    const std::vector<std::string> arguments{
        "--headless",
        "--editor",
        "--path",
        path_to_utf8(snapshot_root),
        "--script",
        "res://addons/gdpp/scene_transform_worker.gd",
        "--",
        state_absolute,
        result_absolute,
    };
    auto process = execute_native_process(executable, arguments);
    std::error_code snapshot_cleanup_error;
    std::filesystem::remove_all(snapshot_root, snapshot_cleanup_error);
    const auto captured = trimmed_toolchain_output(std::move(process.output));
    constexpr std::string_view committed_marker{"GDPP_EXPORT_TRANSFORM_WORKER_COMMITTED"};
    const auto committed_offset = captured.find(committed_marker);
    const bool worker_committed = committed_offset != std::string::npos;
    // A custom editor SceneTree owns the worker lifecycle instead of EditorNode. Godot can report
    // editor-only server teardown diagnostics after SceneTree.quit(), so audit only the
    // transaction output before the worker's authenticated commit marker. Every transform,
    // serialization and cleanup diagnostic occurs before that marker and remains fail-closed.
    const auto audited_output =
        worker_committed ? captured.substr(0, committed_offset) : captured;
    const auto child_diagnostics = export_worker_diagnostics(audited_output);
    godot::Dictionary output;
    output["success"] =
        process.exit_code == 0 &&
        std::filesystem::is_regular_file(path_from_utf8(result_absolute)) &&
        worker_committed && child_diagnostics.empty();
    output["exit_code"] = process.exit_code;
    godot::PackedStringArray diagnostics;
    if (!process.launch_error.empty())
        diagnostics.push_back(godot::String::utf8(process.launch_error.c_str()));
    if (!worker_committed)
        diagnostics.push_back("isolated Godot resource transformer did not commit its transaction");
    for (const auto& diagnostic : child_diagnostics)
        diagnostics.push_back(godot::String::utf8(diagnostic.c_str()));
    if (!static_cast<bool>(output["success"])) {
        diagnostics.push_back("isolated Godot resource transformer failed with exit code " +
                              godot::String::num_int64(process.exit_code));
        if (!captured.empty())
            diagnostics.push_back(godot::String::utf8(captured.c_str()));
    }
    if (snapshot_cleanup_error)
        diagnostics.push_back(
            "isolated project snapshot cleanup will be retried on the next export: " +
            godot::String::utf8(snapshot_cleanup_error.message().c_str()));
    output["diagnostics"] = diagnostics;
    return output;
}

bool GDPPCompiler::set_editor_script_storage_state(
    godot::Object* object, const godot::PackedStringArray& stored_properties) const {
    return gdpp::runtime::set_attached_editor_storage_state(object, stored_properties);
}

godot::Dictionary GDPPCompiler::compile_project(
    const godot::String& project_root, const godot::String& output_directory,
    const godot::String& sdk_root, const godot::String& compiler_executable,
    const godot::String& target_version, const godot::String& build_profile,
    const godot::String& target_platform, const godot::String& target_architecture,
    const godot::String& target_variant, const godot::String& target_precision,
    const godot::Callable& progress_callback) const {
    const auto version = parse_godot_version(native_string(target_version));
    if (!version)
        return invalid_version_result(target_version);
    const auto profile_value = native_string(build_profile);
    const auto profile = parse_native_build_profile(profile_value);
    if (!profile)
        return failure_result("unsupported build profile '" + build_profile +
                              "'; expected debug or release");
    const auto platform_value = target_platform.is_empty() ? native_platform_name(native_platform())
                                                           : native_string(target_platform);
    const auto platform = parse_native_platform(platform_value);
    if (!platform)
        return failure_result("unsupported native platform '" + target_platform +
                              "'; expected macos, linux, windows, android, ios, or web");
    const auto architecture = target_architecture.is_empty() ? std::string{GDPP_ARCH}
                                                             : native_string(target_architecture);
    if (!native_architecture_supported(*platform, architecture))
        return failure_result("unsupported native architecture '" +
                              godot::String{architecture.c_str()} + "' for " +
                              godot::String{platform_value.c_str()});
    NativeWebThreadMode web_thread_mode = NativeWebThreadMode::not_applicable;
    const auto variant = native_string(target_variant);
    if (*platform == NativePlatform::web) {
        if (variant == "threads")
            web_thread_mode = NativeWebThreadMode::multi_threaded;
        else if (variant == "nothreads")
            web_thread_mode = NativeWebThreadMode::single_threaded;
        else
            return failure_result("Web target variant must be 'threads' or 'nothreads'");
    } else if (!variant.empty()) {
        return failure_result("target variant is only valid for the Web platform");
    }
    const auto precision_value = native_string(target_precision);
    const auto precision = parse_native_precision(precision_value);
    if (!precision)
        return failure_result("target precision must be exactly 'single' or 'double'");
    if (precision_value != GDPP_GODOT_PRECISION)
        return failure_result(
            "target Godot precision is '" + target_precision +
            "', but this GDPP compiler was built for '" GDPP_GODOT_PRECISION
            "'; use the package rebuilt from the target engine extension_api.json");
    auto resolved_compiler_executable = native_string(compiler_executable);
#ifdef _WIN32
    if (*platform == NativePlatform::windows) {
        const auto resolved = resolve_msvc_compiler_for_plan(resolved_compiler_executable);
        if (!resolved.valid())
            return failure_result(godot::String{resolved.diagnostic.c_str()});
        resolved_compiler_executable = resolved.executable;
    }
#endif
    auto* settings = godot::ProjectSettings::get_singleton();
    ProjectCompileOptions options;
    options.project_root = path_from_utf8(native_string(settings->globalize_path(project_root)));
    options.output_directory =
        path_from_utf8(native_string(settings->globalize_path(output_directory)));
    options.sdk_root =
        versioned_sdk_root(path_from_utf8(native_string(settings->globalize_path(sdk_root))),
                           *version, *platform, architecture, web_thread_mode);
    bool has_reflected_snapshot = false;
    {
        const std::lock_guard lock{reflected_bridges_mutex_};
        if (reflected_bridges_) {
            options.reflected_extension_bridges = *reflected_bridges_;
            has_reflected_snapshot = true;
        }
    }
    if (!has_reflected_snapshot)
        options.reflected_extension_bridges = reflect_extension_contracts();
    options.compiler.target_version = *version;
    if (progress_callback.is_valid()) {
        options.progress_callback = [&](const ProjectCompilePhase phase,
                                        const std::size_t completed, const std::size_t total) {
            const char* phase_name = "analyze";
            switch (phase) {
            case ProjectCompilePhase::scan:
                phase_name = "scan";
                break;
            case ProjectCompilePhase::parse:
                phase_name = "parse";
                break;
            case ProjectCompilePhase::analyze:
                phase_name = "analyze";
                break;
            case ProjectCompilePhase::translate:
                phase_name = "translate";
                break;
            case ProjectCompilePhase::generate:
                phase_name = "generate";
                break;
            }
            report_build_progress(progress_callback, phase_name, completed, total);
        };
    }
    const ProjectCompiler compiler;
    const auto result = compiler.compile(options);

    godot::Dictionary output;
    output["success"] = result.success;
    output["compiled_count"] = static_cast<int64_t>(result.compiled_count);
    output["cache_hit_count"] = static_cast<int64_t>(result.cache_hit_count);
    output["removed_count"] = static_cast<int64_t>(result.removed_count);
    godot::PackedStringArray scripts;
    godot::PackedStringArray abstract_scripts;
    godot::PackedStringArray editor_only_scripts;
    godot::Dictionary script_classes;
    godot::Dictionary attached_script_bases;
    godot::Dictionary script_contract_hashes;
    godot::Array editor_script_descriptors;
    ProjectScriptNativeBases script_native_bases;
    script_native_bases.reserve(result.scripts.size() * 3U);
    for (const auto& script : result.scripts) {
        const auto relative_path = generic_path_to_utf8(script.relative_path);
        script_native_bases.insert_or_assign(relative_path, script.attached_native_base);
        script_native_bases.insert_or_assign("res://" + relative_path,
                                             script.attached_native_base);
        script_native_bases.insert_or_assign(script.class_name, script.attached_native_base);
        if (!script.global_name.empty()) {
            script_native_bases.insert_or_assign(script.global_name,
                                                 script.attached_native_base);
        }
    }
    for (const auto& script : result.scripts) {
        const auto relative_path = generic_path_to_utf8(script.relative_path);
        scripts.push_back(godot::String::utf8(relative_path.c_str()));
        const auto resource_path = "res://" + relative_path;
        script_classes[godot::String{resource_path.c_str()}] =
            godot::String{script.class_name.c_str()};
        script_contract_hashes[godot::String{resource_path.c_str()}] =
            godot::String{script.public_abi_hash.c_str()};
        editor_script_descriptors.push_back(editor_script_descriptor(
            script, godot::String{resource_path.c_str()}, script_native_bases));
        if (script.is_attached) {
            attached_script_bases[godot::String{resource_path.c_str()}] =
                godot::String{script.attached_native_base.c_str()};
        }
        if (script.is_abstract)
            abstract_scripts.push_back(godot::String{resource_path.c_str()});
        if (script.is_editor_only)
            editor_only_scripts.push_back(godot::String{resource_path.c_str()});
    }
    output["scripts"] = scripts;
    output["abstract_scripts"] = abstract_scripts;
    output["editor_only_scripts"] = editor_only_scripts;
    output["script_classes"] = script_classes;
    output["attached_script_bases"] = attached_script_bases;
    output["script_contract_hashes"] = script_contract_hashes;
    output["editor_script_descriptors"] = editor_script_descriptors;
    godot::PackedStringArray diagnostics;
    godot::PackedStringArray errors;
    godot::PackedStringArray warnings;
    godot::PackedStringArray notes;
    for (const auto& item : result.diagnostics) {
        const auto message = generic_path_to_utf8(item.path) + ":" +
                             std::to_string(item.diagnostic.span.begin.line) + ":" +
                             std::to_string(item.diagnostic.span.begin.column) + ": " +
                             item.diagnostic.code + ": " + item.diagnostic.message;
        const godot::String formatted{message.c_str()};
        diagnostics.push_back(formatted);
        switch (item.diagnostic.severity) {
        case DiagnosticSeverity::error:
            errors.push_back(formatted);
            break;
        case DiagnosticSeverity::warning:
            warnings.push_back(formatted);
            break;
        case DiagnosticSeverity::note:
            notes.push_back(formatted);
            break;
        }
    }
    assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
    if (result.success) {
        output["build_id"] = godot::String{result.build_id.c_str()};
        NativeBuildOptions build_options;
        build_options.project_output_directory = options.output_directory;
        build_options.binary_output_directory = result.native_library_directory;
        build_options.sdk_root = options.sdk_root;
        build_options.compiler_executable = resolved_compiler_executable;
        build_options.platform = *platform;
        build_options.architecture = architecture;
        build_options.profile = *profile;
        build_options.web_thread_mode = web_thread_mode;
        build_options.precision = *precision;
        build_options.target_version = *version;
        const NativeBuilder builder;
        const auto plan = builder.plan(build_options);
        if (!plan.success) {
            output["success"] = false;
            for (const auto& message : plan.diagnostics) {
                const godot::String formatted{message.c_str()};
                diagnostics.push_back(formatted);
                errors.push_back(formatted);
            }
            assign_diagnostic_channels(output, diagnostics, errors, warnings, notes);
            return output;
        }
        godot::Array commands;
        for (const auto& command : plan.commands) {
            godot::Dictionary item;
            item["executable"] = godot::String{command.executable.c_str()};
            godot::PackedStringArray arguments;
            for (const auto& argument : command.arguments)
                arguments.push_back(godot::String{argument.c_str()});
            item["arguments"] = arguments;
            item["stage"] = static_cast<int64_t>(command.stage);
            commands.push_back(item);
        }
        output["build_commands"] = commands;
        output["native_up_to_date"] = plan.up_to_date;
        output["output_library"] =
            godot::String::utf8(generic_path_to_utf8(plan.output_library).c_str());
        if (!plan.pending_output_library.empty()) {
            output["pending_output_library"] =
                godot::String::utf8(generic_path_to_utf8(plan.pending_output_library).c_str());
        }
        output["build_profile"] = build_profile;
        output["target_platform"] = godot::String{platform_value.c_str()};
        output["target_architecture"] = godot::String{architecture.c_str()};
        output["target_precision"] = target_precision;
    }
    return output;
}

godot::String GDPPCompiler::get_default_sdk_root() const { return godot::String{GDPP_SDK_ROOT}; }

godot::String GDPPCompiler::get_default_compiler_executable() const { return default_compiler(); }

godot::String GDPPCompiler::get_host_platform() const {
    return godot::String{native_platform_name(native_platform()).c_str()};
}

godot::String GDPPCompiler::get_host_architecture() const {
    return godot::String{host_process_architecture().c_str()};
}

bool GDPPCompiler::is_target_supported(const godot::String& platform,
                                       const godot::String& architecture) const {
    const auto parsed_platform = parse_native_platform(native_string(platform));
    return parsed_platform &&
           native_architecture_supported(*parsed_platform, native_string(architecture));
}

godot::PackedStringArray GDPPCompiler::get_supported_godot_versions() const {
    godot::PackedStringArray versions;
    versions.push_back("4.4");
    versions.push_back("4.5");
    versions.push_back("4.6");
    versions.push_back("4.7");
    return versions;
}

godot::Dictionary GDPPCompiler::compile_source(const godot::String& source,
                                               const godot::String& virtual_path,
                                               const godot::String& target_version) const {
    const auto version = parse_godot_version(native_string(target_version));
    if (!version)
        return invalid_version_result(target_version);
    const auto path = native_string(virtual_path);
    const auto text = native_string(source);
    const SourceFile source_file{path, text};
    const Compiler compiler;
    CompileOptions options;
    options.target_version = *version;
    return result_dictionary(compiler.compile(path, text, options), source_file);
}

godot::Dictionary GDPPCompiler::compile_file(const godot::String& source_path,
                                             const godot::String& output_directory,
                                             const godot::String& target_version) const {
    const auto version = parse_godot_version(native_string(target_version));
    if (!version)
        return invalid_version_result(target_version);
    auto* settings = godot::ProjectSettings::get_singleton();
    const auto native_source_name = native_string(settings->globalize_path(source_path));
    const auto native_output_name = native_string(settings->globalize_path(output_directory));
    const auto native_source_path = path_from_utf8(native_source_name);
    const auto native_output_path = path_from_utf8(native_output_name);

    std::ifstream input{native_source_path, std::ios::binary};
    if (!input)
        return failure_result("cannot open source file: " + source_path);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const SourceFile source_file{native_source_name, text};
    const Compiler compiler;
    CompileOptions options;
    options.target_version = *version;
    const auto result = compiler.compile(native_source_name, text, options);
    auto output = result_dictionary(result, source_file);
    if (!result.success)
        return output;

    std::error_code error;
    std::filesystem::create_directories(native_output_path, error);
    if (error ||
        !write_file(native_output_path / result.unit.header_file_name, result.unit.header) ||
        !write_file(native_output_path / result.unit.source_file_name, result.unit.source)) {
        output["success"] = false;
        auto diagnostics = static_cast<godot::PackedStringArray>(output["diagnostics"]);
        auto errors = static_cast<godot::PackedStringArray>(output["errors"]);
        const godot::String message = "cannot write generated files to: " + output_directory;
        diagnostics.push_back(message);
        errors.push_back(message);
        assign_diagnostic_channels(output, diagnostics, errors,
                                   static_cast<godot::PackedStringArray>(output["warnings"]),
                                   static_cast<godot::PackedStringArray>(output["notes"]));
    }
    return output;
}

} // namespace gdpp::extension
