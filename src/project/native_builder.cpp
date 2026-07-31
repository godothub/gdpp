#include "gdpp/project/native_builder.hpp"

#include "gdpp/project/native_contract.hpp"
#include "gdpp/support/path_utf8.hpp"
#include "gdpp/support/sha256.hpp"
#include "gdpp/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gdpp {
namespace {

constexpr std::string_view native_build_revision{"16"};
constexpr std::string_view native_build_engine{"Ninja 1.13.2"};

struct GodotApiContract {
    std::string_view kind;
    std::string_view precision;
    std::string_view sha256;
};

GodotApiContract godot_api_contract(const GodotVersion version) {
    switch (version) {
    case GodotVersion::v4_4:
        return {GDPP_GODOT_API_KIND_4_4, GDPP_GODOT_API_PRECISION_4_4, GDPP_GODOT_API_SHA256_4_4};
    case GodotVersion::v4_5:
        return {GDPP_GODOT_API_KIND_4_5, GDPP_GODOT_API_PRECISION_4_5, GDPP_GODOT_API_SHA256_4_5};
    case GodotVersion::v4_6:
        return {GDPP_GODOT_API_KIND_4_6, GDPP_GODOT_API_PRECISION_4_6, GDPP_GODOT_API_SHA256_4_6};
    case GodotVersion::v4_7:
        return {GDPP_GODOT_API_KIND_4_7, GDPP_GODOT_API_PRECISION_4_7, GDPP_GODOT_API_SHA256_4_7};
    }
    return {};
}

struct BridgeBuildInputs {
    std::vector<std::filesystem::path> include_directories;
    std::vector<std::filesystem::path> link_libraries;
    std::vector<std::filesystem::path> manifests;
};

struct NinjaBuildEdge {
    NativeBuildCommand command;
    std::vector<std::filesystem::path> inputs;
    std::vector<std::filesystem::path> outputs;
    std::string description;
    std::filesystem::path depfile;
    bool compiler_dependencies{false};
};

std::string platform_name(NativePlatform platform);

std::string object_extension(NativePlatform platform) {
    return platform == NativePlatform::windows ? ".obj" : ".o";
}

std::string read_build_id(const std::filesystem::path& output) {
    std::ifstream input{output / "build_id.txt"};
    std::string value;
    input >> value;
    if (value.size() != 16 || !std::all_of(value.begin(), value.end(), [](const char character) {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        })) {
        return {};
    }
    return value;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<BridgeBuildInputs> read_bridge_lock(const std::filesystem::path& path,
                                                  const NativeBuildOptions& options,
                                                  std::vector<std::string>& diagnostics) {
    if (!std::filesystem::is_regular_file(path))
        return BridgeBuildInputs{};
    std::ifstream input{path};
    std::string magic;
    std::string version;
    if (!(input >> magic >> version) || magic != "GDPP_BRIDGE_LOCK" || version != "1") {
        diagnostics.push_back("missing or incompatible third-party bridge lock: " +
                              path_to_utf8(path));
        return std::nullopt;
    }
    BridgeBuildInputs result;
    std::string token;
    std::string current_abi;
    std::filesystem::path current_manifest;
    bool current_matched = true;
    const auto finish_bridge = [&]() {
        if (!current_abi.empty() && !current_matched) {
            diagnostics.push_back("third-party bridge has no target for " +
                                  platform_name(options.platform) + "/" + options.architecture +
                                  "/" + native_build_profile_name(options.profile) + ": " +
                                  path_to_utf8(current_manifest));
        }
    };
    while (input >> token) {
        if (token == "bridge") {
            finish_bridge();
            std::string manifest_value;
            input >> std::quoted(current_abi) >> std::quoted(manifest_value);
            current_manifest = path_from_utf8(manifest_value);
            current_matched = false;
            if (!current_manifest.empty())
                result.manifests.push_back(current_manifest);
            continue;
        }
        if (token == "runtime") {
            if (current_abi.empty()) {
                diagnostics.push_back("orphan runtime record in third-party bridge lock");
                return std::nullopt;
            }
            current_matched = true;
            continue;
        }
        if (token != "target") {
            diagnostics.push_back("invalid record in third-party bridge lock: " + token);
            return std::nullopt;
        }
        std::string platform;
        std::string architecture;
        std::string profile;
        std::size_t include_count = 0;
        if (!(input >> std::quoted(platform) >> std::quoted(architecture) >> std::quoted(profile) >>
              include_count)) {
            diagnostics.push_back("truncated third-party bridge target record");
            return std::nullopt;
        }
        std::vector<std::filesystem::path> includes(include_count);
        for (auto& include : includes) {
            std::string value;
            if (!(input >> std::quoted(value)))
                return std::nullopt;
            include = path_from_utf8(value);
        }
        std::size_t library_count = 0;
        if (!(input >> library_count))
            return std::nullopt;
        std::vector<std::filesystem::path> libraries(library_count);
        for (auto& library : libraries) {
            std::string value;
            if (!(input >> std::quoted(value)))
                return std::nullopt;
            library = path_from_utf8(value);
        }
        if (platform == platform_name(options.platform) && architecture == options.architecture &&
            profile == native_build_profile_name(options.profile)) {
            current_matched = true;
            result.include_directories.insert(result.include_directories.end(), includes.begin(),
                                              includes.end());
            result.link_libraries.insert(result.link_libraries.end(), libraries.begin(),
                                         libraries.end());
        }
    }
    finish_bridge();
    if (!input.eof()) {
        diagnostics.push_back("cannot parse third-party bridge lock: " + path_to_utf8(path));
        return std::nullopt;
    }
    return diagnostics.empty() ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<bool> write_file_if_changed(const std::filesystem::path& path,
                                          const std::string& content) {
    if (const auto existing = read_file(path); existing && *existing == content)
        return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output.good())
            return std::nullopt;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }
    return true;
}

std::string safe_stem(const std::filesystem::path& path) {
    auto value = path_to_utf8(path.filename());
    for (char& character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0)
            character = '_';
    }
    return value;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool valid_msvc_compiler_version(const std::string_view value) {
    const auto separator = value.find('.');
    if (separator == std::string_view::npos || value.substr(0, separator) != "19" ||
        separator + 1 == value.size()) {
        return false;
    }
    bool previous_dot = false;
    for (std::size_t index = separator + 1; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (value[index] == '.') {
            if (previous_dot || index + 1 == value.size())
                return false;
            previous_dot = true;
        } else {
            if (std::isdigit(character) == 0)
                return false;
            previous_dot = false;
        }
    }
    return true;
}

std::string platform_name(NativePlatform platform) {
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

std::string web_thread_mode_name(NativeWebThreadMode mode) {
    if (mode == NativeWebThreadMode::multi_threaded)
        return "threads";
    if (mode == NativeWebThreadMode::single_threaded)
        return "nothreads";
    return "none";
}

std::filesystem::path sdk_manifest_path(const NativeBuildOptions& options) {
    auto target_name = platform_name(options.platform) + "." + options.architecture;
    if (options.platform == NativePlatform::web)
        target_name += "." + web_thread_mode_name(options.web_thread_mode);
    const auto target_manifest = options.sdk_root / "manifests" / (target_name + ".sdk.manifest");
    if (std::filesystem::is_regular_file(target_manifest))
        return target_manifest;
    if (options.platform == NativePlatform::macos && options.architecture != "universal") {
        const auto universal_manifest =
            options.sdk_root / "manifests" / "macos.universal.sdk.manifest";
        if (std::filesystem::is_regular_file(universal_manifest))
            return universal_manifest;
    }
    return options.sdk_root / "sdk.manifest";
}

bool validate_manifest(const NativeBuildOptions& options, std::vector<std::string>& diagnostics) {
    const auto path = sdk_manifest_path(options);
    std::ifstream input{path};
    std::string magic;
    std::string format;
    if (!(input >> magic >> format) || magic != "GDPP_SDK" ||
        format != std::to_string(GDPP_NATIVE_SDK_SCHEMA)) {
        diagnostics.push_back("native SDK format is missing or incompatible: expected GDPP_SDK " +
                              std::to_string(GDPP_NATIVE_SDK_SCHEMA) + " at " + path_to_utf8(path) +
                              "; reinstall the SDK packaged with this GDPP compiler");
        return false;
    }
    std::string key;
    std::string value;
    std::string platform;
    std::string architecture;
    std::string api;
    std::string api_kind;
    std::string api_sha256;
    std::string precision;
    std::string profiles;
    std::string distribution_binding;
    std::string distribution_optimization;
    std::string runtime_abi;
    std::string runtime_header_sha256;
    std::string reference_semantics_header_sha256;
    std::string runtime_source_sha256;
    std::string attached_runtime_header_sha256;
    std::string attached_runtime_registry_source_sha256;
    std::string attached_runtime_instance_source_sha256;
    std::string attached_runtime_language_source_sha256;
    std::string integer_semantics_header_sha256;
    std::string web_threads;
    std::string source_paths;
    std::string ios_deployment_target;
    std::string ios_slices;
    std::string platform_minimum;
    std::string android_api_level;
    std::string android_stl;
    std::string cxx_standard;
    std::string exceptions;
    std::string msvc_runtime;
    std::string compiler;
    std::string compiler_version;
    while (input >> key >> value) {
        if (key == "platform")
            platform = value;
        else if (key == "arch")
            architecture = value;
        else if (key == "api")
            api = value;
        else if (key == "api_kind")
            api_kind = value;
        else if (key == "api_sha256")
            api_sha256 = value;
        else if (key == "precision")
            precision = value;
        else if (key == "profiles")
            profiles = value;
        else if (key == "distribution_binding")
            distribution_binding = value;
        else if (key == "distribution_optimization")
            distribution_optimization = value;
        else if (key == "runtime_abi")
            runtime_abi = value;
        else if (key == "runtime_header_sha256")
            runtime_header_sha256 = value;
        else if (key == "reference_semantics_header_sha256")
            reference_semantics_header_sha256 = value;
        else if (key == "runtime_source_sha256")
            runtime_source_sha256 = value;
        else if (key == "attached_runtime_header_sha256")
            attached_runtime_header_sha256 = value;
        else if (key == "attached_runtime_registry_source_sha256")
            attached_runtime_registry_source_sha256 = value;
        else if (key == "attached_runtime_instance_source_sha256")
            attached_runtime_instance_source_sha256 = value;
        else if (key == "attached_runtime_language_source_sha256")
            attached_runtime_language_source_sha256 = value;
        else if (key == "integer_semantics_header_sha256")
            integer_semantics_header_sha256 = value;
        else if (key == "web_threads")
            web_threads = value;
        else if (key == "source_paths")
            source_paths = value;
        else if (key == "ios_deployment_target")
            ios_deployment_target = value;
        else if (key == "ios_slices")
            ios_slices = value;
        else if (key == "platform_minimum")
            platform_minimum = value;
        else if (key == "android_api_level")
            android_api_level = value;
        else if (key == "android_stl")
            android_stl = value;
        else if (key == "cxx_standard")
            cxx_standard = value;
        else if (key == "exceptions")
            exceptions = value;
        else if (key == "msvc_runtime")
            msvc_runtime = value;
        else if (key == "compiler")
            compiler = value;
        else if (key == "compiler_version")
            compiler_version = value;
    }
    if (platform != platform_name(options.platform))
        diagnostics.push_back("native SDK platform mismatch: expected " +
                              platform_name(options.platform) + ", package contains " + platform);
    const bool architecture_compatible =
        architecture == options.architecture ||
        (options.platform == NativePlatform::macos && architecture == "universal" &&
         (options.architecture == "arm64" || options.architecture == "x86_64"));
    if (!architecture_compatible)
        diagnostics.push_back("native SDK architecture mismatch: expected " + options.architecture +
                              ", package contains " + architecture);
    const std::string expected_minimum = options.platform == NativePlatform::windows ? "Windows_10"
                                         : options.platform == NativePlatform::macos ? "macOS_11.0"
                                         : options.platform == NativePlatform::linux
                                             ? "Ubuntu_22.04"
                                         : options.platform == NativePlatform::android ? "Android_9"
                                         : options.platform == NativePlatform::ios     ? "iOS_16.0"
                                                                                       : "none";
    if (platform_minimum != expected_minimum) {
        diagnostics.push_back(
            "native SDK platform minimum mismatch: expected " + expected_minimum +
            ", package contains " +
            (platform_minimum.empty() ? std::string{"<missing>"} : platform_minimum));
    }
    if (cxx_standard != "17") {
        diagnostics.push_back("native SDK C++ standard mismatch: expected 17, package contains " +
                              (cxx_standard.empty() ? std::string{"<missing>"} : cxx_standard));
    }
    if (exceptions != "disabled") {
        diagnostics.push_back(
            "native SDK exception model mismatch: expected disabled, package contains " +
            (exceptions.empty() ? std::string{"<missing>"} : exceptions));
    }
    const std::string expected_msvc_runtime =
        options.platform == NativePlatform::windows ? "static" : "not_applicable";
    if (msvc_runtime != expected_msvc_runtime) {
        diagnostics.push_back("native SDK MSVC runtime mismatch: expected " +
                              expected_msvc_runtime + ", package contains " +
                              (msvc_runtime.empty() ? std::string{"<missing>"} : msvc_runtime));
    }
    if (options.platform == NativePlatform::windows) {
        auto executable_name =
            std::filesystem::path{options.compiler_executable}.filename().string();
        std::transform(executable_name.begin(), executable_name.end(), executable_name.begin(),
                       [](const char character) {
                           return static_cast<char>(
                               std::tolower(static_cast<unsigned char>(character)));
                       });
        if (executable_name != "cl" && executable_name != "cl.exe") {
            diagnostics.push_back(
                "native Windows builds require the MSVC cl.exe frontend; configured compiler is " +
                options.compiler_executable);
        }
        if (compiler != "MSVC") {
            diagnostics.push_back(
                "native Windows SDK compiler family mismatch: expected MSVC, package contains " +
                (compiler.empty() ? std::string{"<missing>"} : compiler));
        }
        if (!valid_msvc_compiler_version(compiler_version)) {
            diagnostics.push_back(
                "native Windows SDK MSVC toolset is missing or incompatible: expected compiler "
                "version 19.x, package contains " +
                (compiler_version.empty() ? std::string{"<missing>"} : compiler_version));
        }
    }
    if (options.platform == NativePlatform::android) {
        if (android_api_level != "28") {
            diagnostics.push_back(
                "native Android SDK API baseline mismatch: expected 28, package contains " +
                (android_api_level.empty() ? std::string{"<missing>"} : android_api_level));
        }
        if (android_stl != "c++_shared") {
            diagnostics.push_back(
                "native Android SDK STL mismatch: expected c++_shared, package contains " +
                (android_stl.empty() ? std::string{"<missing>"} : android_stl));
        }
    }
    if (options.platform == NativePlatform::web) {
        const auto expected_threads = web_thread_mode_name(options.web_thread_mode);
        if (web_threads != expected_threads) {
            diagnostics.push_back("native SDK Web thread mode mismatch: expected " +
                                  expected_threads + ", package contains " +
                                  (web_threads.empty() ? std::string{"<missing>"} : web_threads));
        }
        if (source_paths != "mapped") {
            diagnostics.emplace_back(
                "native Web SDK does not guarantee reproducible source-path mapping; "
                "reinstall the matching Web target pack");
        }
    }
    if (options.platform == NativePlatform::ios) {
        if (ios_deployment_target != "16.0")
            diagnostics.emplace_back(
                "native iOS SDK deployment target must match the commercial iOS 16.0 baseline");
        const auto slices = "," + ios_slices + ",";
        for (const auto required : {"device-arm64", "simulator-arm64", "simulator-x86_64"}) {
            if (slices.find("," + std::string{required} + ",") == std::string::npos) {
                diagnostics.push_back("native iOS SDK is missing required slice '" +
                                      std::string{required} + "'");
            }
        }
        if (source_paths != "mapped") {
            diagnostics.emplace_back(
                "native iOS SDK does not guarantee reproducible source-path mapping; "
                "reinstall the matching iOS target pack");
        }
    }
    if (api != godot_version_name(options.target_version))
        diagnostics.push_back("native SDK Godot API mismatch: expected " +
                              std::string{godot_version_name(options.target_version)} +
                              ", package contains " + api);
    const auto api_contract = godot_api_contract(options.target_version);
    const auto expected_precision = native_precision_name(options.precision);
    if (api_contract.precision != expected_precision) {
        diagnostics.push_back("compiler Godot API precision mismatch: target requests " +
                              std::string{expected_precision} + ", compiler metadata is " +
                              std::string{api_contract.precision} +
                              "; rebuild GDPP from the exact target engine extension_api.json");
    }
    if (precision != expected_precision) {
        diagnostics.push_back("native SDK precision mismatch: expected " +
                              std::string{expected_precision} + ", package contains " +
                              (precision.empty() ? std::string{"<missing>"} : precision));
    }
    if (api_kind != api_contract.kind || api_sha256 != api_contract.sha256) {
        diagnostics.emplace_back(
            "native SDK Godot API fingerprint does not match this compiler; reinstall the "
            "matching official package or rebuild every component from one custom "
            "extension_api.json");
    }
    if (profiles.empty()) {
        diagnostics.emplace_back("native SDK manifest does not declare supported build profiles");
    } else {
        const std::string expected = native_build_profile_name(options.profile);
        const auto padded = "," + profiles + ",";
        if (padded.find("," + expected + ",") == std::string::npos) {
            diagnostics.push_back("native SDK does not support the required '" + expected +
                                  "' build profile");
        }
    }
    if (distribution_binding != "template_release") {
        diagnostics.push_back(
            "native SDK distribution binding mismatch: expected template_release, package "
            "contains " +
            (distribution_binding.empty() ? std::string{"<missing>"} : distribution_binding));
    }
    if (distribution_optimization != "Release") {
        diagnostics.push_back(
            "native SDK distribution optimization mismatch: expected Release, package contains " +
            (distribution_optimization.empty() ? std::string{"<missing>"}
                                               : distribution_optimization));
    }
    const auto expected_runtime_abi = std::to_string(GDPP_NATIVE_RUNTIME_ABI);
    if (runtime_abi != expected_runtime_abi) {
        diagnostics.push_back("native SDK runtime ABI mismatch: compiler requires " +
                              expected_runtime_abi + ", package contains " +
                              (runtime_abi.empty() ? std::string{"<missing>"} : runtime_abi) +
                              "; reinstall the matching GDPP SDK");
    }
    if (runtime_header_sha256 != GDPP_NATIVE_RUNTIME_HEADER_SHA256 ||
        reference_semantics_header_sha256 != GDPP_REFERENCE_SEMANTICS_HEADER_SHA256 ||
        runtime_source_sha256 != GDPP_NATIVE_RUNTIME_SOURCE_SHA256 ||
        attached_runtime_header_sha256 != GDPP_ATTACHED_RUNTIME_HEADER_SHA256 ||
        attached_runtime_registry_source_sha256 != GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE_SHA256 ||
        attached_runtime_instance_source_sha256 != GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE_SHA256 ||
        attached_runtime_language_source_sha256 != GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE_SHA256 ||
        integer_semantics_header_sha256 != GDPP_INTEGER_SEMANTICS_HEADER_SHA256) {
        diagnostics.emplace_back(
            "native SDK runtime contract does not match this GDPP compiler; reinstall the "
            "matching plugin package");
    }

    const auto verify_runtime_file = [&](const std::filesystem::path& runtime_path,
                                         const std::string& declared_hash,
                                         const std::string_view label) {
        const auto content = read_file(runtime_path);
        if (!content) {
            diagnostics.push_back("missing native SDK " + std::string{label} + ": " +
                                  path_to_utf8(runtime_path));
        } else if (sha256(*content) != declared_hash) {
            diagnostics.push_back("native SDK " + std::string{label} +
                                  " failed integrity validation: " + path_to_utf8(runtime_path));
        }
    };
    verify_runtime_file(options.sdk_root / "include/gdpp/runtime/variant_ops.hpp",
                        runtime_header_sha256, "runtime header");
    verify_runtime_file(options.sdk_root / "include/gdpp/runtime/reference_semantics.hpp",
                        reference_semantics_header_sha256, "reference semantics header");
    verify_runtime_file(options.sdk_root / "src/runtime/variant_ops.cpp", runtime_source_sha256,
                        "runtime source");
    verify_runtime_file(options.sdk_root / "include/gdpp/runtime/attached_script.hpp",
                        attached_runtime_header_sha256, "attached runtime header");
    verify_runtime_file(options.sdk_root / "src/runtime/attached_script_registry.cpp",
                        attached_runtime_registry_source_sha256,
                        "attached registry runtime source");
    verify_runtime_file(options.sdk_root / "src/runtime/attached_script_instance.cpp",
                        attached_runtime_instance_source_sha256,
                        "attached instance runtime source");
    verify_runtime_file(options.sdk_root / "src/runtime/attached_script_language.cpp",
                        attached_runtime_language_source_sha256,
                        "attached language runtime source");
    verify_runtime_file(options.sdk_root / "include/gdpp/numeric/integer_semantics.hpp",
                        integer_semantics_header_sha256, "integer semantics header");
    return diagnostics.empty();
}

std::string manifest_value(const std::filesystem::path& manifest, std::string_view wanted_key) {
    std::ifstream input{manifest};
    std::string key;
    std::string value;
    while (input >> key >> value) {
        if (key == wanted_key)
            return value;
    }
    return {};
}

std::filesystem::path find_binding_library(const std::filesystem::path& directory,
                                           const NativeBuildOptions& options,
                                           const std::string& target,
                                           std::string_view requested_architecture = {}) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
        return {};
    std::vector<std::filesystem::path> candidates;
    std::vector<std::filesystem::path> fallback_candidates;
    const auto platform_marker = "." + platform_name(options.platform) + "." + target + ".";
    const auto architecture = requested_architecture.empty()
                                  ? std::string_view{options.architecture}
                                  : requested_architecture;
    for (std::filesystem::directory_iterator iterator{directory, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file())
            continue;
        const auto extension = iterator->path().extension();
        const auto filename = iterator->path().filename().string();
        const bool thread_variant_matches =
            options.platform != NativePlatform::web ||
            (options.web_thread_mode == NativeWebThreadMode::single_threaded
                 ? filename.find(".nothreads.") != std::string::npos
                 : filename.find(".nothreads.") == std::string::npos);
        const bool architecture_matches =
            filename.find("." + std::string{architecture} + ".") != std::string::npos;
        const bool universal_fallback = options.platform == NativePlatform::macos &&
                                        architecture != "universal" &&
                                        filename.find(".universal.") != std::string::npos;
        if (((options.platform == NativePlatform::windows && extension == ".lib") ||
             (options.platform != NativePlatform::windows && extension == ".a")) &&
            filename.find(platform_marker) != std::string::npos && thread_variant_matches) {
            if (architecture_matches)
                candidates.push_back(iterator->path());
            else if (universal_fallback)
                fallback_candidates.push_back(iterator->path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty())
        return candidates.front();
    std::sort(fallback_candidates.begin(), fallback_candidates.end());
    return fallback_candidates.empty() ? std::filesystem::path{} : fallback_candidates.front();
}

void append_include_arguments(std::vector<std::string>& arguments,
                              const std::vector<std::filesystem::path>& includes,
                              NativePlatform platform) {
    for (const auto& include : includes) {
        if (platform == NativePlatform::windows)
            arguments.push_back("/I" + path_to_utf8(include));
        else {
            arguments.emplace_back("-I");
            arguments.push_back(path_to_utf8(include));
        }
    }
}

void append_macos_architecture_arguments(std::vector<std::string>& arguments,
                                         const NativeBuildOptions& options) {
    if (options.platform != NativePlatform::macos)
        return;
    arguments.emplace_back("-mmacosx-version-min=11.0");
    if (options.architecture == "universal") {
        arguments.insert(arguments.end(), {"-arch", "arm64", "-arch", "x86_64"});
    } else if (options.architecture == "arm64" || options.architecture == "x86_64") {
        arguments.insert(arguments.end(), {"-arch", options.architecture});
    }
}

void append_android_target_arguments(std::vector<std::string>& arguments,
                                     const NativeBuildOptions& options) {
    if (options.platform != NativePlatform::android)
        return;
    const auto triple =
        options.architecture == "arm64" ? "aarch64-linux-android" : "x86_64-linux-android";
    // Keep this baseline aligned with the packaged Android godot-cpp SDK and the documented
    // Android 9 minimum. A single fixed value keeps object-cache signatures deterministic.
    arguments.push_back("--target=" + std::string{triple} + "28");
}

using ReproduciblePathMapping = std::pair<std::string, std::string>;

std::optional<std::string> read_environment_variable(const char* name) {
#if defined(_MSC_VER)
    std::size_t value_size = 0;
    if (getenv_s(&value_size, nullptr, 0, name) != 0 || value_size == 0)
        return std::nullopt;
    std::vector<char> value(value_size);
    if (getenv_s(&value_size, value.data(), value.size(), name) != 0 || value_size == 0)
        return std::nullopt;
    return std::string{value.data(), value_size - 1};
#else
    const auto* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

void add_reproducible_path_mapping(std::vector<ReproduciblePathMapping>& mappings,
                                   const std::filesystem::path& source, std::string replacement) {
    if (source.empty())
        return;
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(source, error);
    if (error) {
        error.clear();
        normalized = std::filesystem::absolute(source, error).lexically_normal();
    }
    if (error || normalized.empty() || normalized == normalized.root_path())
        return;
    const auto value = path_to_utf8(normalized);
    const auto existing = std::find_if(mappings.begin(), mappings.end(),
                                       [&](const auto& mapping) { return mapping.first == value; });
    if (existing == mappings.end())
        mappings.emplace_back(value, std::move(replacement));
}

std::optional<std::filesystem::path> resolve_compiler_path(std::string_view executable) {
    std::filesystem::path value{executable};
    std::error_code error;
    if (value.is_absolute() || value.has_parent_path()) {
        auto resolved = std::filesystem::weakly_canonical(value, error);
        return error ? std::nullopt : std::optional<std::filesystem::path>{resolved};
    }
    const auto path_environment = read_environment_variable("PATH");
    if (!path_environment)
        return std::nullopt;
#if defined(_WIN32)
    constexpr char path_separator = ';';
    const std::vector<std::string> suffixes{"", ".exe", ".bat", ".cmd"};
#else
    constexpr char path_separator = ':';
    const std::vector<std::string> suffixes{""};
#endif
    std::istringstream paths{*path_environment};
    std::string directory;
    while (std::getline(paths, directory, path_separator)) {
        for (const auto& suffix : suffixes) {
            const auto candidate =
                std::filesystem::path{directory} / (std::string{executable} + suffix);
            if (!std::filesystem::is_regular_file(candidate, error)) {
                error.clear();
                continue;
            }
            auto resolved = std::filesystem::weakly_canonical(candidate, error);
            if (!error)
                return resolved;
            error.clear();
        }
    }
    return std::nullopt;
}

std::vector<ReproduciblePathMapping> reproducible_path_mappings(const NativeBuildOptions& options) {
    std::vector<ReproduciblePathMapping> mappings;
    add_reproducible_path_mapping(mappings, options.project_output_directory, "/gdpp/project");
    add_reproducible_path_mapping(mappings, options.sdk_root, "/gdpp/sdk");
    if (options.platform == NativePlatform::web) {
        for (const auto& [name, replacement] :
             {std::pair{"EM_CACHE", "/gdpp/toolchain/cache"},
              std::pair{"EMSDK", "/gdpp/toolchain/emsdk"},
              std::pair{"EMSCRIPTEN_ROOT", "/gdpp/toolchain/emscripten"}}) {
            if (const auto value = read_environment_variable(name); value && !value->empty())
                add_reproducible_path_mapping(mappings, *value, replacement);
        }
        if (const auto compiler = resolve_compiler_path(options.compiler_executable)) {
            add_reproducible_path_mapping(mappings, compiler->parent_path(),
                                          "/gdpp/toolchain/compiler");
        }
    }
    std::sort(mappings.begin(), mappings.end(), [](const auto& left, const auto& right) {
        return left.first.size() > right.first.size();
    });
    return mappings;
}

void append_reproducible_path_arguments(std::vector<std::string>& arguments,
                                        const NativeBuildOptions& options) {
    for (const auto& [source, replacement] : reproducible_path_mappings(options)) {
        if (options.platform == NativePlatform::windows)
            arguments.push_back("/pathmap:" + source + "=" + replacement);
        else
            arguments.push_back("-ffile-prefix-map=" + source + "=" + replacement);
    }
}

NativeBuildCommand compile_command(const NativeBuildOptions& options,
                                   const std::filesystem::path& source,
                                   const std::filesystem::path& object,
                                   const std::vector<std::filesystem::path>& includes) {
    NativeBuildCommand command;
    command.executable = options.compiler_executable;
    command.working_directory = options.project_output_directory;
    auto& arguments = command.arguments;
    if (options.platform == NativePlatform::windows) {
        // Keep these ABI and feature switches aligned with godot-cpp's exported
        // Windows target settings. In particular, TYPED_METHOD_BIND avoids MSVC's
        // incompatible pointer-to-member representation for generated script types,
        // while /MT matches the compiler-only SDK's statically linked CRT.
        arguments = {"/nologo",
                     "/std:c++17",
                     "/utf-8",
                     "/MT",
                     "/EHsc",
                     "/bigobj",
                     "/experimental:deterministic",
                     "/DGDEXTENSION",
                     "/DTHREADS_ENABLED",
                     "/DWINDOWS_ENABLED",
                     "/D_WIN32_WINNT=0x0A00",
                     "/DWINVER=0x0A00",
                     "/DTYPED_METHOD_BIND",
                     "/D_HAS_EXCEPTIONS=0",
                     "/DNOMINMAX"};
        arguments.emplace_back("/O2");
        arguments.emplace_back("/DNDEBUG");
        if (options.precision == NativePrecision::double_precision)
            arguments.emplace_back("/DREAL_T_IS_DOUBLE");
        arguments.emplace_back("/Gy");
        arguments.emplace_back("/Gw");
        if (options.profile == NativeBuildProfile::debug)
            arguments.emplace_back("/DGDPP_SCRIPT_DEBUG_ENABLED");
        append_reproducible_path_arguments(arguments, options);
        append_include_arguments(arguments, includes, options.platform);
        arguments.emplace_back("/c");
        arguments.push_back(path_to_utf8(source));
        arguments.push_back("/Fo" + path_to_utf8(object));
    } else {
        arguments = {"-std=c++17", "-fPIC", "-fno-exceptions", "-DGDEXTENSION"};
        if (options.platform != NativePlatform::web ||
            options.web_thread_mode == NativeWebThreadMode::multi_threaded) {
            arguments.emplace_back("-DTHREADS_ENABLED");
        }
        if (options.platform == NativePlatform::android)
            arguments.insert(arguments.end(), {"-DANDROID_ENABLED", "-DUNIX_ENABLED"});
        if (options.platform == NativePlatform::web) {
            arguments.insert(arguments.end(), {"-DWEB_ENABLED", "-DUNIX_ENABLED", "-sSIDE_MODULE=1",
                                               "-sSUPPORT_LONGJMP=wasm"});
            if (options.web_thread_mode == NativeWebThreadMode::multi_threaded)
                arguments.emplace_back("-sUSE_PTHREADS=1");
        }
        arguments.insert(arguments.end(),
                         {"-O3", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections"});
        arguments.emplace_back("-DNDEBUG");
        if (options.precision == NativePrecision::double_precision)
            arguments.emplace_back("-DREAL_T_IS_DOUBLE");
        if (options.profile == NativeBuildProfile::debug)
            arguments.emplace_back("-DGDPP_SCRIPT_DEBUG_ENABLED");
        append_reproducible_path_arguments(arguments, options);
        append_macos_architecture_arguments(arguments, options);
        append_android_target_arguments(arguments, options);
        append_include_arguments(arguments, includes, options.platform);
        arguments.emplace_back("-c");
        arguments.push_back(path_to_utf8(source));
        arguments.emplace_back("-o");
        arguments.push_back(path_to_utf8(object));
    }
    return command;
}

struct IOSBuildSlice {
    std::string name;
    std::string sdk;
    std::string target_triple;
    std::filesystem::path binding_library;
};

NativeBuildCommand ios_compile_command(const NativeBuildOptions& options,
                                       const IOSBuildSlice& slice,
                                       const std::filesystem::path& source,
                                       const std::filesystem::path& object,
                                       const std::vector<std::filesystem::path>& includes) {
    NativeBuildCommand command;
    command.executable = options.compiler_executable;
    command.working_directory = options.project_output_directory;
    auto& arguments = command.arguments;
    arguments = {
        "--sdk",         slice.sdk,       "clang++",         "-target",       slice.target_triple,
        "-std=c++17",    "-fPIC",         "-fno-exceptions", "-DGDEXTENSION", "-DTHREADS_ENABLED",
        "-DIOS_ENABLED", "-DUNIX_ENABLED"};
    arguments.insert(arguments.end(),
                     {"-O3", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections"});
    arguments.emplace_back("-DNDEBUG");
    if (options.precision == NativePrecision::double_precision)
        arguments.emplace_back("-DREAL_T_IS_DOUBLE");
    if (options.profile == NativeBuildProfile::debug)
        arguments.emplace_back("-DGDPP_SCRIPT_DEBUG_ENABLED");
    append_reproducible_path_arguments(arguments, options);
    append_include_arguments(arguments, includes, options.platform);
    arguments.emplace_back("-c");
    arguments.push_back(path_to_utf8(source));
    arguments.emplace_back("-o");
    arguments.push_back(path_to_utf8(object));
    return command;
}

NativeBuildCommand ios_link_command(const NativeBuildOptions& options, const IOSBuildSlice& slice,
                                    const std::vector<std::filesystem::path>& objects,
                                    const std::vector<std::filesystem::path>& libraries,
                                    const std::filesystem::path& output,
                                    const std::filesystem::path& export_map) {
    NativeBuildCommand command;
    command.executable = options.compiler_executable;
    command.working_directory = options.project_output_directory;
    command.stage = 1;
    auto& arguments = command.arguments;
    arguments = {"--sdk", slice.sdk, "clang++", "-target", slice.target_triple, "-dynamiclib"};
    for (const auto& object : objects)
        arguments.push_back(path_to_utf8(object));
    arguments.push_back(path_to_utf8(slice.binding_library));
    for (const auto& library : libraries)
        arguments.push_back(path_to_utf8(library));
    arguments.push_back("-Wl,-exported_symbols_list," + path_to_utf8(export_map));
    arguments.emplace_back("-Wl,-install_name,@rpath/libgdpp.dylib");
    arguments.emplace_back("-Wl,-dead_strip");
    arguments.emplace_back("-Wl,-x");
    arguments.emplace_back("-o");
    arguments.push_back(path_to_utf8(output));
    return command;
}

bool append_utf16(std::u16string& output, std::string_view input) {
    for (std::size_t index = 0; index < input.size();) {
        const auto lead = static_cast<unsigned char>(input[index]);
        std::uint32_t codepoint = 0;
        std::size_t length = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80U) {
            codepoint = lead;
            length = 1;
        } else if ((lead & 0xe0U) == 0xc0U) {
            codepoint = lead & 0x1fU;
            length = 2;
            minimum = 0x80U;
        } else if ((lead & 0xf0U) == 0xe0U) {
            codepoint = lead & 0x0fU;
            length = 3;
            minimum = 0x800U;
        } else if ((lead & 0xf8U) == 0xf0U) {
            codepoint = lead & 0x07U;
            length = 4;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + length > input.size())
            return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(input[index + offset]);
            if ((continuation & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (continuation & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU))
            return false;
        if (codepoint < 0x10000U) {
            output.push_back(static_cast<char16_t>(codepoint));
        } else {
            codepoint -= 0x10000U;
            output.push_back(static_cast<char16_t>(0xd800U + (codepoint >> 10U)));
            output.push_back(static_cast<char16_t>(0xdc00U + (codepoint & 0x3ffU)));
        }
        index += length;
    }
    return true;
}

bool write_msvc_response_file(const std::filesystem::path& path,
                              const std::vector<std::string>& arguments) {
    std::string content;
    for (const auto& argument : arguments) {
        content.push_back('"');
        for (const char character : argument) {
            if (character == '"')
                content.push_back('\\');
            content.push_back(character);
        }
        content += "\"\r\n";
    }
    std::u16string utf16;
    if (!append_utf16(utf16, content))
        return false;
    std::string bytes;
    bytes.reserve(2 + utf16.size() * 2);
    bytes.push_back(static_cast<char>(0xff));
    bytes.push_back(static_cast<char>(0xfe));
    for (const char16_t code_unit : utf16) {
        bytes.push_back(static_cast<char>(code_unit & 0xffU));
        bytes.push_back(static_cast<char>((code_unit >> 8U) & 0xffU));
    }
    return write_file_if_changed(path, bytes).has_value();
}

#ifdef _WIN32
std::string quote_response_argument(std::string_view argument) {
    std::string output{"\""};
    std::size_t backslashes = 0;
    for (const char character : argument) {
        if (character == '\\') {
            ++backslashes;
            continue;
        }
        if (character == '"') {
            output.append(backslashes * 2 + 1, '\\');
            output.push_back('"');
        } else {
            output.append(backslashes, '\\');
            output.push_back(character);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, '\\');
    output.push_back('"');
    return output;
}

bool write_msvc_compiler_response_file(const std::filesystem::path& path,
                                       const std::vector<std::string>& arguments) {
    // cl.exe accepts Unicode response files as UTF-8 with a BOM. Keep linker response files in
    // UTF-16LE because link.exe has a separate, well-defined Unicode response-file path.
    std::string content{"\xef\xbb\xbf"};
    for (const auto& argument : arguments)
        content += quote_response_argument(argument) + "\r\n";
    return write_file_if_changed(path, content).has_value();
}

bool is_windows_command_script(const std::string_view executable) {
    auto extension = std::filesystem::path{executable}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    });
    return extension == ".bat" || extension == ".cmd";
}

std::string windows_command_invocation(const std::string& executable,
                                       const std::vector<std::string>& arguments) {
    std::string command;
    if (is_windows_command_script(executable))
        command = "cmd.exe /d /s /c \"";
    command += quote_response_argument(executable);
    for (const auto& argument : arguments)
        command += " " + quote_response_argument(argument);
    if (is_windows_command_script(executable))
        command.push_back('"');
    return command;
}
#endif

std::string quote_posix_shell_argument(std::string_view argument) {
    std::string output{"'"};
    for (const char character : argument) {
        if (character == '\'')
            output += "'\\''";
        else
            output.push_back(character);
    }
    output.push_back('\'');
    return output;
}

std::string escape_ninja_value(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        if (character == '$')
            output += "$$";
        else if (character != '\r' && character != '\n')
            output.push_back(character);
    }
    return output;
}

std::string escape_ninja_path(const std::filesystem::path& path) {
    const auto value = generic_path_to_utf8(path);
    std::string output;
    output.reserve(value.size());
    for (const char character : value) {
        if (character == '$')
            output += "$$";
        else if (character == ' ')
            output += "$ ";
        else if (character == ':')
            output += "$:";
        else if (character != '\r' && character != '\n')
            output.push_back(character);
    }
    return output;
}

bool ninja_path_supported(const std::filesystem::path& path) {
    const auto value = generic_path_to_utf8(path);
    return value.find_first_of("|\r\n") == std::string::npos;
}

std::string sanitized_description(std::string value) {
    for (char& character : value) {
        if (character == '\r' || character == '\n')
            character = ' ';
    }
    return value;
}

std::optional<std::vector<std::filesystem::path>>
write_ninja_command_file(const NativeBuildOptions& options,
                         const std::filesystem::path& command_directory,
                         const std::size_t index, NativeBuildCommand& command,
                         std::vector<std::string> extra_arguments,
                         std::string& invocation) {
    auto arguments = command.arguments;
    arguments.insert(arguments.end(), std::make_move_iterator(extra_arguments.begin()),
                     std::make_move_iterator(extra_arguments.end()));
#ifdef _WIN32
    const auto response = command_directory / (std::to_string(index) + ".rsp");
    std::vector<std::filesystem::path> nested_responses;
    for (const auto& argument : arguments) {
        if (!argument.empty() && argument.front() == '@')
            nested_responses.push_back(path_from_utf8(argument.substr(1)));
    }
    if (!nested_responses.empty()) {
        invocation = windows_command_invocation(command.executable, arguments);
        return nested_responses;
    }
    const bool written =
        options.platform == NativePlatform::windows
            ? write_msvc_compiler_response_file(response, arguments)
            : write_file_if_changed(
                  response, [&]() {
                      std::string content;
                      for (const auto& argument : arguments)
                          content += quote_response_argument(argument) + "\n";
                      return content;
                  }())
                  .has_value();
    if (!written)
        return std::nullopt;
    invocation = windows_command_invocation(
        command.executable, {"@" + generic_path_to_utf8(response)});
    return std::vector<std::filesystem::path>{response};
#else
    (void)options;
    const auto script = command_directory / (std::to_string(index) + ".sh");
    std::string content{"#!/bin/sh\nset -eu\n"};
    if (command.stage == 3 && !arguments.empty()) {
        content += "rm -rf -- " + quote_posix_shell_argument(arguments.back()) + "\n";
    }
    content += "exec ";
    content += quote_posix_shell_argument(command.executable);
    for (const auto& argument : arguments)
        content += " " + quote_posix_shell_argument(argument);
    content.push_back('\n');
    if (!write_file_if_changed(script, content))
        return std::nullopt;
    invocation = "/bin/sh " + quote_posix_shell_argument(generic_path_to_utf8(script));
    return std::vector<std::filesystem::path>{script};
#endif
}

bool write_ninja_graph(const NativeBuildOptions& options,
                       const std::filesystem::path& native,
                       std::vector<NinjaBuildEdge> edges,
                       const std::vector<std::filesystem::path>& final_outputs,
                       NativeBuildPlan& plan) {
    if (!ninja_path_supported(native) || !ninja_path_supported(options.build_executor)) {
        plan.diagnostics.emplace_back(
            "Ninja build paths cannot contain a vertical bar or a line break");
        return false;
    }
    for (const auto& edge : edges) {
        const auto invalid_input =
            std::find_if(edge.inputs.begin(), edge.inputs.end(),
                         [](const auto& path) { return !ninja_path_supported(path); });
        const auto invalid_output =
            std::find_if(edge.outputs.begin(), edge.outputs.end(),
                         [](const auto& path) { return !ninja_path_supported(path); });
        if (invalid_input != edge.inputs.end() || invalid_output != edge.outputs.end() ||
            (!edge.depfile.empty() && !ninja_path_supported(edge.depfile))) {
            plan.diagnostics.emplace_back(
                "Ninja build paths cannot contain a vertical bar or a line break");
            return false;
        }
    }
    const auto command_directory = native / "commands";
    std::error_code error;
    std::filesystem::create_directories(command_directory, error);
    if (error) {
        plan.diagnostics.push_back("cannot create Ninja command directory: " + error.message());
        return false;
    }

    std::string graph{"ninja_required_version = 1.13\n"
                      "msvc_deps_prefix = Note: including file:\n\n"
                      "pool gdpp_link_pool\n"
                      "  depth = 1\n\n"};
    std::vector<std::filesystem::path> compile_outputs;
    for (std::size_t index = 0; index < edges.size(); ++index) {
        auto& edge = edges[index];
        if (const auto executable = resolve_compiler_path(edge.command.executable))
            edge.inputs.push_back(*executable);
        std::vector<std::string> dependency_arguments;
        if (edge.compiler_dependencies) {
            if (options.platform == NativePlatform::windows) {
                dependency_arguments.emplace_back("/showIncludes");
            } else {
                dependency_arguments.insert(
                    dependency_arguments.end(),
                    {"-MMD", "-MF", path_to_utf8(edge.depfile), "-MT",
                     path_to_utf8(edge.outputs.front())});
            }
        }
        std::string invocation;
        const auto command_files =
            write_ninja_command_file(options, command_directory, index, edge.command,
                                     std::move(dependency_arguments), invocation);
        if (!command_files) {
            plan.diagnostics.emplace_back("cannot write Ninja command response");
            return false;
        }
        edge.inputs.insert(edge.inputs.end(), command_files->begin(), command_files->end());

        const auto rule = "gdpp_edge_" + std::to_string(index);
        graph += "rule " + rule + "\n";
        graph += "  command = " + escape_ninja_value(invocation) + "\n";
        const auto phase = edge.command.stage == 0 ? "compile" : "link";
        graph += "  description = @@GDPP:" + std::string{phase} + "@@ " +
                 escape_ninja_value(sanitized_description(edge.description)) + "\n";
        if (edge.compiler_dependencies) {
            if (options.platform == NativePlatform::windows) {
                graph += "  deps = msvc\n";
            } else {
                graph += "  depfile = " + escape_ninja_path(edge.depfile) + "\n";
                graph += "  deps = gcc\n";
            }
        }
        if (edge.command.stage != 0)
            graph += "  pool = gdpp_link_pool\n";
        graph.push_back('\n');

        graph += "build";
        for (const auto& output : edge.outputs)
            graph += " " + escape_ninja_path(output);
        graph += ": " + rule;
        for (const auto& input : edge.inputs)
            graph += " " + escape_ninja_path(input);
        if (edge.command.stage != 0)
            graph += " || gdpp_compile_barrier";
        graph += "\n\n";
        if (edge.command.stage == 0) {
            compile_outputs.insert(compile_outputs.end(), edge.outputs.begin(), edge.outputs.end());
            ++plan.compile_edge_count;
        } else {
            ++plan.post_compile_edge_count;
        }
    }

    graph += "build gdpp_compile_barrier: phony";
    for (const auto& output : compile_outputs)
        graph += " " + escape_ninja_path(output);
    graph += "\n\nbuild gdpp: phony";
    for (const auto& output : final_outputs)
        graph += " " + escape_ninja_path(output);
    graph += "\ndefault gdpp\n";

    plan.build_file = native / "build.ninja";
    const auto graph_changed = write_file_if_changed(plan.build_file, graph);
    if (!graph_changed) {
        plan.diagnostics.emplace_back("cannot write the Ninja build graph");
        return false;
    }
    plan.build_executor = options.build_executor;
    plan.build_directory = native;
    return true;
}

std::optional<NativeBuildCommand> link_command(const NativeBuildOptions& options,
                                               const std::vector<std::filesystem::path>& objects,
                                               const std::filesystem::path& binding_library,
                                               const std::vector<std::filesystem::path>& libraries,
                                               const std::filesystem::path& output,
                                               const std::filesystem::path& response_file,
                                               const std::filesystem::path& export_map) {
    NativeBuildCommand command;
    command.working_directory = options.project_output_directory;
    command.stage = 1;
    auto& arguments = command.arguments;
    if (options.platform == NativePlatform::windows) {
        // cl.exe does not decode UTF-16 response files reliably. Invoke the MSVC
        // linker directly so large projects and non-ASCII paths use its native
        // UTF-16 response-file support instead of the Windows command-line limit.
        std::filesystem::path linker = options.compiler_executable;
        linker.replace_filename("link.exe");
        command.executable = linker.u8string();
        const auto import_library = response_file.parent_path() / "gdpp.lib";
        std::vector<std::string> response_arguments{
            "/DLL", options.architecture == "arm64" ? "/MACHINE:ARM64" : "/MACHINE:X64"};
        for (const auto& object : objects)
            response_arguments.push_back(object.u8string());
        response_arguments.push_back(binding_library.u8string());
        for (const auto& library : libraries)
            response_arguments.push_back(library.u8string());
        response_arguments.push_back("/IMPLIB:" + import_library.u8string());
        response_arguments.push_back("/OUT:" + output.u8string());
        response_arguments.emplace_back("/OPT:REF");
        response_arguments.emplace_back("/OPT:ICF");
        response_arguments.emplace_back("/INCREMENTAL:NO");
        if (!write_msvc_response_file(response_file, response_arguments))
            return std::nullopt;
        arguments = {"/NOLOGO", "@" + response_file.u8string()};
    } else if (options.platform == NativePlatform::web) {
        command.executable = options.compiler_executable;
        arguments = {"-shared", "-sSIDE_MODULE=1", "-sWASM_BIGINT", "-sSUPPORT_LONGJMP=wasm",
                     "-fvisibility=hidden"};
        append_reproducible_path_arguments(arguments, options);
        if (options.web_thread_mode == NativeWebThreadMode::multi_threaded)
            arguments.emplace_back("-sUSE_PTHREADS=1");
        for (const auto& object : objects)
            arguments.push_back(path_to_utf8(object));
        arguments.push_back(path_to_utf8(binding_library));
        for (const auto& library : libraries)
            arguments.push_back(path_to_utf8(library));
        arguments.insert(arguments.end(), {"-O3", "-Wl,--gc-sections", "-s"});
        arguments.emplace_back("-o");
        arguments.push_back(path_to_utf8(output));
    } else {
        command.executable = options.compiler_executable;
        arguments.push_back(options.platform == NativePlatform::macos ? "-dynamiclib" : "-shared");
        append_macos_architecture_arguments(arguments, options);
        append_android_target_arguments(arguments, options);
        for (const auto& object : objects)
            arguments.push_back(path_to_utf8(object));
        arguments.push_back(path_to_utf8(binding_library));
        for (const auto& library : libraries)
            arguments.push_back(path_to_utf8(library));
        if (options.platform == NativePlatform::linux ||
            options.platform == NativePlatform::android) {
            // Keep the statically linked godot-cpp/C++ runtime private to each ELF GDExtension.
            // Multiple extensions are loaded into one process and ELF symbol interposition across
            // their standard-library locale/allocator state is undefined and has caused crashes.
            arguments.emplace_back("-Wl,--exclude-libs,ALL");
            arguments.push_back("-Wl,--version-script=" + path_to_utf8(export_map));
        } else if (options.platform == NativePlatform::macos) {
            // Mach-O exports symbols pulled from static archives unless an explicit allow-list
            // is supplied. Only the GDExtension ABI entry point belongs to the customer binary.
            arguments.push_back("-Wl,-exported_symbols_list," + path_to_utf8(export_map));
        }
        if (options.platform == NativePlatform::macos) {
            arguments.emplace_back("-Wl,-dead_strip");
            // Distribution images do not need the local symbol table. Keep the public
            // GDExtension entry point while reducing package size and exposed internals.
            arguments.emplace_back("-Wl,-x");
        } else {
            arguments.insert(arguments.end(), {"-Wl,--gc-sections", "-Wl,-O1", "-Wl,-s"});
            if (options.platform == NativePlatform::android) {
                arguments.insert(arguments.end(), {"-Wl,-z,relro", "-Wl,-z,now"});
            }
        }
        arguments.emplace_back("-o");
        arguments.push_back(path_to_utf8(output));
    }
    return command;
}

} // namespace

const char* native_build_profile_name(NativeBuildProfile profile) noexcept {
    switch (profile) {
    case NativeBuildProfile::debug:
        return "debug";
    case NativeBuildProfile::release:
        return "release";
    }
    return "release";
}

std::optional<NativeBuildProfile> parse_native_build_profile(std::string_view value) noexcept {
    if (value == "debug")
        return NativeBuildProfile::debug;
    if (value == "release")
        return NativeBuildProfile::release;
    return std::nullopt;
}

const char* native_precision_name(const NativePrecision precision) noexcept {
    return precision == NativePrecision::double_precision ? "double" : "single";
}

std::optional<NativePrecision> parse_native_precision(const std::string_view value) noexcept {
    if (value == "single")
        return NativePrecision::single;
    if (value == "double")
        return NativePrecision::double_precision;
    return std::nullopt;
}

bool native_architecture_supported(const NativePlatform platform,
                                   const std::string_view architecture) noexcept {
    switch (platform) {
    case NativePlatform::macos:
        return architecture == "arm64" || architecture == "x86_64" || architecture == "universal";
    case NativePlatform::linux:
    case NativePlatform::windows:
        return architecture == "x86_64";
    case NativePlatform::android:
    case NativePlatform::ios:
        return architecture == "arm64";
    case NativePlatform::web:
        return architecture == "wasm32";
    }
    return false;
}

std::string native_library_name(NativeBuildProfile profile, NativePlatform platform,
                                std::string_view architecture,
                                NativeWebThreadMode web_thread_mode) {
    std::string stem = "gdpp." + std::string{native_build_profile_name(profile)} + "." +
                       platform_name(platform) + "." + std::string{architecture};
    if (platform == NativePlatform::web)
        stem += "." + web_thread_mode_name(web_thread_mode);
    if (platform == NativePlatform::windows)
        return stem + ".dll";
    if (platform == NativePlatform::web)
        return "lib" + stem + ".wasm";
    if (platform == NativePlatform::ios)
        return "lib" + stem + ".xcframework";
    return "lib" + stem + (platform == NativePlatform::macos ? ".dylib" : ".so");
}

NativeBuildPlan NativeBuilder::plan(const NativeBuildOptions& options) const {
    NativeBuildPlan result;
    if (options.compiler_executable.empty()) {
        result.diagnostics.emplace_back("C++ compiler executable is not configured");
        return result;
    }
    if (options.binary_output_directory.empty()) {
        result.diagnostics.emplace_back(
            "project native library output directory is not configured");
        return result;
    }
    if (!native_architecture_supported(options.platform, options.architecture)) {
        result.diagnostics.push_back("unsupported native architecture '" + options.architecture +
                                     "' for " + platform_name(options.platform));
        return result;
    }
    if (options.platform == NativePlatform::web) {
        if (options.web_thread_mode == NativeWebThreadMode::not_applicable) {
            result.diagnostics.emplace_back(
                "Web target must explicitly select threads or nothreads");
            return result;
        }
    } else if (options.web_thread_mode != NativeWebThreadMode::not_applicable) {
        result.diagnostics.emplace_back("Web thread mode is invalid for a non-Web target");
        return result;
    }
    if (!validate_manifest(options, result.diagnostics))
        return result;
    const auto build_id = read_build_id(options.project_output_directory);
    if (build_id.empty()) {
        result.diagnostics.emplace_back("missing or invalid project native build identifier");
        return result;
    }
    const auto generated = options.project_output_directory / "generated";
    const auto registration = options.project_output_directory / "register_types.cpp";
    const auto runtime = options.sdk_root / "src/runtime/variant_ops.cpp";
    const auto registration_source = read_file(registration);
    const bool has_attached_runtime =
        registration_source &&
        registration_source->find("gdpp::runtime::register_attached_script") != std::string::npos;
    const std::vector<std::filesystem::path> attached_runtime_sources{
        options.sdk_root / "src/runtime/attached_script_registry.cpp",
        options.sdk_root / "src/runtime/attached_script_instance.cpp",
        options.sdk_root / "src/runtime/attached_script_language.cpp",
    };
    std::vector<std::filesystem::path> includes{
        generated,
        options.sdk_root / "include",
        options.sdk_root / "godot-cpp/include",
        options.sdk_root / "godot-cpp/gen/include",
    };
    const auto bridge_inputs = read_bridge_lock(options.project_output_directory / "bridge.lock",
                                                options, result.diagnostics);
    if (!bridge_inputs)
        return result;
    includes.insert(includes.end(), bridge_inputs->include_directories.begin(),
                    bridge_inputs->include_directories.end());
    for (const auto& include : bridge_inputs->include_directories) {
        if (!std::filesystem::is_directory(include))
            result.diagnostics.push_back("missing third-party bridge include directory: " +
                                         path_to_utf8(include));
    }
    for (const auto& library : bridge_inputs->link_libraries) {
        if (!std::filesystem::is_regular_file(library))
            result.diagnostics.push_back("missing third-party bridge link library: " +
                                         path_to_utf8(library));
    }
    for (const auto& required :
         {registration, runtime, includes[1] / "gdpp/runtime/variant_ops.hpp",
          includes[1] / "gdpp/runtime/reference_semantics.hpp",
          includes[1] / "gdpp/numeric/integer_semantics.hpp", includes[2] / "godot_cpp/godot.hpp",
          includes[3] / "godot_cpp/core/version.hpp", includes[3] / "gdextension_interface.h"}) {
        if (!std::filesystem::is_regular_file(required))
            result.diagnostics.push_back("missing native SDK input: " + path_to_utf8(required));
    }
    if (has_attached_runtime) {
        if (!std::filesystem::is_regular_file(includes[1] / "gdpp/runtime/attached_script.hpp")) {
            result.diagnostics.push_back(
                "missing native SDK input: " +
                path_to_utf8(includes[1] / "gdpp/runtime/attached_script.hpp"));
        }
        for (const auto& source : attached_runtime_sources) {
            if (!std::filesystem::is_regular_file(source))
                result.diagnostics.push_back("missing native SDK input: " + path_to_utf8(source));
        }
    }
    // Godot-cpp uses upstream ABI target names internally. They are deliberately kept out of
    // GDPP's public build-profile API and artifact names.
    const std::string binding_target = "template_release";
    std::filesystem::path binding_library;
    std::filesystem::path ios_device_binding_library;
    std::filesystem::path ios_simulator_binding_library;
    if (options.platform == NativePlatform::ios) {
        ios_device_binding_library =
            find_binding_library(options.sdk_root / "lib", options, binding_target, "arm64");
        ios_simulator_binding_library =
            find_binding_library(options.sdk_root / "lib", options, binding_target, "universal");
        if (ios_device_binding_library.empty()) {
            ios_device_binding_library =
                find_binding_library(options.sdk_root / "lib/device", options, binding_target);
        }
        if (ios_simulator_binding_library.empty()) {
            ios_simulator_binding_library = find_binding_library(
                options.sdk_root / "lib/simulator", options, binding_target, "universal");
        }
        if (ios_device_binding_library.empty() || ios_simulator_binding_library.empty()) {
            result.diagnostics.emplace_back(
                "missing device or Universal Simulator godot-cpp library in iOS SDK");
        }
    } else {
        binding_library = find_binding_library(options.sdk_root / "lib", options, binding_target);
        if (binding_library.empty())
            result.diagnostics.emplace_back(
                "missing ABI-compatible godot-cpp static library in SDK");
    }
    if (!result.diagnostics.empty())
        return result;

    std::vector<std::filesystem::path> sources{registration, runtime};
    if (has_attached_runtime)
        sources.insert(sources.end(), attached_runtime_sources.begin(),
                       attached_runtime_sources.end());
    std::error_code error;
    bool found_generated_source = false;
    for (std::filesystem::directory_iterator iterator{generated, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file() &&
            ends_with(iterator->path().filename().string(), ".gd.cpp")) {
            sources.push_back(iterator->path());
            found_generated_source = true;
        }
    }
    if (error || !found_generated_source) {
        result.diagnostics.emplace_back("no generated project translation units were found");
        return result;
    }
    std::sort(sources.begin(), sources.end());

    auto native = options.project_output_directory / "native-direct" /
                  godot_version_name(options.target_version) / platform_name(options.platform) /
                  options.architecture;
    if (options.platform == NativePlatform::web)
        native /= web_thread_mode_name(options.web_thread_mode);
    native /= native_build_profile_name(options.profile);
    const auto objects_directory = native / "objects";
    const auto build_configuration = native / "build-configuration.txt";
    const auto export_map = native / "gdpp.exports.map";
    const auto& binary_directory = options.binary_output_directory;
    std::filesystem::create_directories(objects_directory, error);
    std::filesystem::create_directories(binary_directory, error);
    if (error) {
        result.diagnostics.emplace_back("cannot create native build directories: " +
                                        error.message());
        return result;
    }
    std::string build_configuration_contents =
        "GDPP_NATIVE_BUILD " + std::string{native_build_revision} + "\napi " +
        std::string{godot_version_name(options.target_version)} + "\nplatform " +
        platform_name(options.platform) + "\narch " + options.architecture + "\nprofile " +
        native_build_profile_name(options.profile) + "\ncompiler " + options.compiler_executable +
        "\nprecision " + native_precision_name(options.precision) + "\nweb_threads " +
        web_thread_mode_name(options.web_thread_mode) + "\nbuild_engine " +
        std::string{native_build_engine} + "\nbuild_executor " +
        path_to_utf8(options.build_executor) + "\n";
    if (const auto compiler = resolve_compiler_path(options.compiler_executable)) {
        std::error_code compiler_error;
        const auto size = std::filesystem::file_size(*compiler, compiler_error);
        if (!compiler_error) {
            const auto modified = std::filesystem::last_write_time(*compiler, compiler_error);
            if (!compiler_error) {
                build_configuration_contents +=
                    "compiler_resolved " + path_to_utf8(*compiler) + "\ncompiler_size " +
                    std::to_string(size) + "\ncompiler_modified " +
                    std::to_string(
                        static_cast<std::int64_t>(modified.time_since_epoch().count())) +
                    "\n";
            }
        }
    }
    for (const auto& [source, replacement] : reproducible_path_mappings(options))
        build_configuration_contents += "path_map " + source + "=" + replacement + "\n";
    if (!write_file_if_changed(build_configuration, build_configuration_contents)) {
        result.diagnostics.emplace_back("cannot write native build configuration signature");
        return result;
    }
    if (options.platform == NativePlatform::linux || options.platform == NativePlatform::android) {
        const auto contents =
            "{ global: " + std::string{project_library_entry_symbol} + "; local: *; };\n";
        if (!write_file_if_changed(export_map, contents)) {
            result.diagnostics.emplace_back("cannot write ELF project export map");
            return result;
        }
    } else if (options.platform == NativePlatform::macos ||
               options.platform == NativePlatform::ios) {
        const auto contents = "_" + std::string{project_library_entry_symbol} + "\n";
        if (!write_file_if_changed(export_map, contents)) {
            result.diagnostics.emplace_back("cannot write Mach-O project export list");
            return result;
        }
    }
    result.output_library =
        binary_directory / native_library_name(options.profile, options.platform,
                                               options.architecture, options.web_thread_mode);

    std::vector<std::filesystem::path> common_compile_inputs{
        build_configuration,
        sdk_manifest_path(options),
        options.sdk_root / "include/gdpp/runtime/variant_ops.hpp",
        options.sdk_root / "include/gdpp/runtime/reference_semantics.hpp",
        options.sdk_root / "include/gdpp/numeric/integer_semantics.hpp",
    };
    if (has_attached_runtime) {
        common_compile_inputs.push_back(options.sdk_root /
                                        "include/gdpp/runtime/attached_script.hpp");
        common_compile_inputs.insert(common_compile_inputs.end(), attached_runtime_sources.begin(),
                                     attached_runtime_sources.end());
    }
    const auto bridge_lock = options.project_output_directory / "bridge.lock";
    if (std::filesystem::is_regular_file(bridge_lock))
        common_compile_inputs.push_back(bridge_lock);
    common_compile_inputs.insert(common_compile_inputs.end(), bridge_inputs->manifests.begin(),
                                 bridge_inputs->manifests.end());
    std::sort(common_compile_inputs.begin(), common_compile_inputs.end());
    common_compile_inputs.erase(
        std::unique(common_compile_inputs.begin(), common_compile_inputs.end()),
        common_compile_inputs.end());
    std::vector<NinjaBuildEdge> graph_edges;

    if (options.platform == NativePlatform::ios) {
        const auto deployment_target =
            manifest_value(sdk_manifest_path(options), "ios_deployment_target");
        if (deployment_target.empty()) {
            result.diagnostics.emplace_back("native iOS SDK deployment target is unavailable");
            return result;
        }
        const std::vector<IOSBuildSlice> slices{
            {"device-arm64", "iphoneos", "arm64-apple-ios" + deployment_target,
             ios_device_binding_library},
            {"simulator-arm64", "iphonesimulator",
             "arm64-apple-ios" + deployment_target + "-simulator", ios_simulator_binding_library},
            {"simulator-x86_64", "iphonesimulator",
             "x86_64-apple-ios" + deployment_target + "-simulator", ios_simulator_binding_library},
        };

        result.output_library =
            binary_directory / native_library_name(options.profile, options.platform,
                                                   options.architecture, options.web_thread_mode);
        const auto staging_directory = native / "xcframework-staging";
        result.pending_output_library = staging_directory / result.output_library.filename();

        std::vector<std::filesystem::path> slice_libraries;
        for (const auto& slice : slices) {
            const auto slice_directory = native / slice.name;
            const auto slice_objects = slice_directory / "objects";
            std::filesystem::create_directories(slice_objects, error);
            if (error) {
                result.diagnostics.push_back("cannot create iOS slice build directory: " +
                                             error.message());
                return result;
            }
            std::vector<std::filesystem::path> objects;
            for (const auto& source : sources) {
                const auto object = slice_objects / (safe_stem(source) + ".o");
                objects.push_back(object);
                auto inputs = common_compile_inputs;
                inputs.push_back(source);
                auto command = ios_compile_command(options, slice, source, object, includes);
                graph_edges.push_back(
                    {command,
                     inputs,
                     {object},
                     source.filename().string(),
                     std::filesystem::path{path_to_utf8(object) + ".d"},
                     true});
                result.commands.push_back(std::move(command));
            }
            const auto slice_library = slice_directory / "libgdpp.dylib";
            slice_libraries.push_back(slice_library);
            auto link_inputs = objects;
            link_inputs.push_back(slice.binding_library);
            link_inputs.push_back(export_map);
            link_inputs.insert(link_inputs.end(), bridge_inputs->link_libraries.begin(),
                               bridge_inputs->link_libraries.end());
            auto command = ios_link_command(options, slice, objects,
                                            bridge_inputs->link_libraries, slice_library,
                                            export_map);
            graph_edges.push_back(
                {command, link_inputs, {slice_library}, "Linking " + slice.name, {}, false});
            result.commands.push_back(std::move(command));
        }

        const auto simulator_directory = native / "simulator-universal";
        std::filesystem::create_directories(simulator_directory, error);
        if (error) {
            result.diagnostics.push_back("cannot create iOS Simulator build directory: " +
                                         error.message());
            return result;
        }
        const auto simulator_library = simulator_directory / "libgdpp.dylib";
        {
            NativeBuildCommand command;
            command.executable = options.compiler_executable;
            command.working_directory = options.project_output_directory;
            command.stage = 2;
            command.arguments = {"lipo",
                                 "-create",
                                 path_to_utf8(slice_libraries[1]),
                                 path_to_utf8(slice_libraries[2]),
                                 "-output",
                                 path_to_utf8(simulator_library)};
            graph_edges.push_back({command,
                                   {slice_libraries[1], slice_libraries[2]},
                                   {simulator_library},
                                   "Combining Simulator slices",
                                   {},
                                   false});
            result.commands.push_back(std::move(command));
        }

        std::filesystem::create_directories(staging_directory, error);
        if (error) {
            result.diagnostics.push_back("cannot create iOS XCFramework staging directory: " +
                                         error.message());
            return result;
        }
        {
            NativeBuildCommand command;
            command.executable = options.compiler_executable;
            command.working_directory = options.project_output_directory;
            command.stage = 3;
            command.arguments = {"xcodebuild", "-create-xcframework",
                                 "-library",   path_to_utf8(slice_libraries[0]),
                                 "-library",   path_to_utf8(simulator_library),
                                 "-output",    path_to_utf8(result.pending_output_library)};
            graph_edges.push_back(
                {command,
                 {slice_libraries[0], simulator_library},
                 {result.pending_output_library / "Info.plist"},
                 "Packaging XCFramework",
                 {},
                 false});
            result.commands.push_back(std::move(command));
        }
        if (!write_ninja_graph(options, native, std::move(graph_edges),
                               {result.pending_output_library / "Info.plist"}, result))
            return result;
        result.success = true;
        return result;
    }

    std::vector<std::filesystem::path> objects;
    for (const auto& source : sources) {
        const auto object =
            objects_directory / (safe_stem(source) + object_extension(options.platform));
        objects.push_back(object);
        auto inputs = common_compile_inputs;
        inputs.push_back(source);
        auto command = compile_command(options, source, object, includes);
        graph_edges.push_back(
            {command,
             inputs,
             {object},
             source.filename().string(),
             std::filesystem::path{path_to_utf8(object) + ".d"},
             true});
        result.commands.push_back(std::move(command));
    }
    auto link_inputs = objects;
    link_inputs.push_back(binding_library);
    if (std::filesystem::is_regular_file(export_map))
        link_inputs.push_back(export_map);
    link_inputs.insert(link_inputs.end(), bridge_inputs->link_libraries.begin(),
                       bridge_inputs->link_libraries.end());
    auto command =
        link_command(options, objects, binding_library, bridge_inputs->link_libraries,
                     result.output_library, native / "link.rsp", export_map);
    if (!command) {
        result.diagnostics.emplace_back("cannot write MSVC native linker response file");
        return result;
    }
    graph_edges.push_back(
        {*command, link_inputs, {result.output_library}, "Linking native library", {}, false});
    result.commands.push_back(std::move(*command));
    if (!write_ninja_graph(options, native, std::move(graph_edges), {result.output_library},
                           result))
        return result;
    result.success = true;
    return result;
}

} // namespace gdpp
