#include "support/test.hpp"

#include "gdpp/project/native_builder.hpp"
#include "gdpp/version.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void write_input(const std::filesystem::path& path, const std::string& value = "// fixture\n") {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << value;
}

std::string read_input(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void replace_manifest_field(const std::filesystem::path& path, const std::string& current,
                            const std::string& replacement) {
    auto content = read_input(path);
    const auto offset = content.find(current);
    if (offset == std::string::npos)
        return;
    content.replace(offset, current.size(), replacement);
    write_input(path, content);
}

std::string attached_runtime_manifest_fields() {
    return "attached_runtime_header_sha256 " + std::string{GDPP_ATTACHED_RUNTIME_HEADER_SHA256} +
           "\nattached_runtime_registry_source_sha256 " +
           GDPP_ATTACHED_RUNTIME_REGISTRY_SOURCE_SHA256 +
           "\nattached_runtime_instance_source_sha256 " +
           GDPP_ATTACHED_RUNTIME_INSTANCE_SOURCE_SHA256 +
           "\nattached_runtime_language_source_sha256 " +
           GDPP_ATTACHED_RUNTIME_LANGUAGE_SOURCE_SHA256 + "\n";
}

std::string native_abi_manifest_fields(const std::string& platform) {
    return "cxx_standard 17\nexceptions disabled\nmsvc_runtime " +
           std::string{platform == "windows" ? "static" : "not_applicable"} + "\n" +
           (platform == "android" ? "android_stl c++_shared\n" : "") +
           (platform == "windows" ? "compiler MSVC\ncompiler_version 19.44.35207.1\n" : "");
}

std::string binding_manifest_fields(const std::string& platform) {
    (void)platform;
    return "distribution_binding template_release\ndistribution_optimization Release\n";
}

std::string api_manifest_fields() {
    return "api_kind " + std::string{GDPP_GODOT_API_KIND_4_4} + "\napi_sha256 " +
           GDPP_GODOT_API_SHA256_4_4 + "\nprecision " + GDPP_GODOT_API_PRECISION_4_4 + "\n";
}

std::filesystem::path make_sdk_fixture(const std::string& name, const std::string& library_name) {
    const auto root = std::filesystem::path{GDPP_TEST_BINARY_DIR} / "test-fixtures" / name;
    const std::string platform = name.find("windows") != std::string::npos   ? "windows"
                                 : name.find("android") != std::string::npos ? "android"
                                 : name.find("linux") != std::string::npos   ? "linux"
                                 : name.find("web") != std::string::npos     ? "web"
                                 : name.find("ios") != std::string::npos     ? "ios"
                                                                             : "macos";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_input(root / "project/generated/example.gd.cpp");
    write_input(root / "project/generated/example.gd.hpp");
    write_input(root / "project/register_types.cpp");
    write_input(root / "project/build_id.txt", "0123456789abcdef\n");
    std::filesystem::create_directories(root / "sdk/src/runtime");
    std::filesystem::create_directories(root / "sdk/include/gdpp/runtime");
    std::filesystem::create_directories(root / "sdk/include/gdpp/numeric");
    std::filesystem::copy_file(std::filesystem::path{GDPP_TEST_SOURCE_DIR} /
                                   "src/runtime/variant_ops.cpp",
                               root / "sdk/src/runtime/variant_ops.cpp",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(std::filesystem::path{GDPP_TEST_SOURCE_DIR} /
                                   "include/gdpp/runtime/variant_ops.hpp",
                               root / "sdk/include/gdpp/runtime/variant_ops.hpp",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(std::filesystem::path{GDPP_TEST_SOURCE_DIR} /
                                   "include/gdpp/runtime/reference_semantics.hpp",
                               root / "sdk/include/gdpp/runtime/reference_semantics.hpp",
                               std::filesystem::copy_options::overwrite_existing);
    for (const auto& relative :
         {"src/runtime/attached_script_registry.cpp", "src/runtime/attached_script_instance.cpp",
          "src/runtime/attached_script_language.cpp", "include/gdpp/runtime/attached_script.hpp"}) {
        std::filesystem::copy_file(std::filesystem::path{GDPP_TEST_SOURCE_DIR} / relative,
                                   root / "sdk" / relative,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    std::filesystem::copy_file(std::filesystem::path{GDPP_TEST_SOURCE_DIR} /
                                   "include/gdpp/numeric/integer_semantics.hpp",
                               root / "sdk/include/gdpp/numeric/integer_semantics.hpp",
                               std::filesystem::copy_options::overwrite_existing);
    write_input(root / "sdk/godot-cpp/include/godot_cpp/godot.hpp");
    write_input(root / "sdk/godot-cpp/gen/include/godot_cpp/core/version.hpp");
    write_input(root / "sdk/godot-cpp/gen/include/gdextension_interface.h");
    if (platform == "ios") {
        write_input(root / "sdk/lib/device" / library_name);
        auto simulator_library = library_name;
        const auto arm64 = simulator_library.rfind(".arm64.a");
        if (arm64 != std::string::npos)
            simulator_library.replace(arm64, std::string{".arm64.a"}.size(), ".universal.a");
        write_input(root / "sdk/lib/simulator" / simulator_library);
    } else {
        write_input(root / "sdk/lib" / library_name);
    }
    const std::string architecture = platform == "web"                              ? "wasm32"
                                     : platform == "windows" || platform == "linux" ? "x86_64"
                                                                                    : "arm64";
    const std::string web_threads =
        platform == "web"
            ? "web_threads " +
                  std::string{name.find("nothreads") != std::string::npos ? "nothreads"
                                                                          : "threads"} +
                  "\nsource_paths mapped\n"
            : "";
    const std::string ios_contract =
        platform == "ios" ? "ios_deployment_target 16.0\n"
                            "ios_slices device-arm64,simulator-arm64,simulator-x86_64\n"
                            "source_paths mapped\n"
                          : "";
    const std::string platform_contract = platform == "windows" ? "platform_minimum Windows_10\n"
                                          : platform == "macos" ? "platform_minimum macOS_11.0\n"
                                          : platform == "linux" ? "platform_minimum Ubuntu_22.04\n"
                                          : platform == "android"
                                              ? "platform_minimum Android_9\nandroid_api_level 28\n"
                                          : platform == "ios" ? "platform_minimum iOS_16.0\n"
                                                              : "platform_minimum none\n";
    write_input(root / "sdk/sdk.manifest",
                "GDPP_SDK " + std::to_string(GDPP_NATIVE_SDK_SCHEMA) + "\napi 4.4\n" +
                    api_manifest_fields() + "platform " + platform + "\narch " + architecture +
                    "\nprofiles debug,release\n" + binding_manifest_fields(platform) +
                    "runtime_abi " +
                    std::to_string(GDPP_NATIVE_RUNTIME_ABI) + "\nruntime_header_sha256 " +
                    GDPP_NATIVE_RUNTIME_HEADER_SHA256 + "\nreference_semantics_header_sha256 " +
                    GDPP_REFERENCE_SEMANTICS_HEADER_SHA256 + "\nruntime_source_sha256 " +
                    GDPP_NATIVE_RUNTIME_SOURCE_SHA256 + "\n" + attached_runtime_manifest_fields() +
                    "integer_semantics_header_sha256 " + GDPP_INTEGER_SEMANTICS_HEADER_SHA256 +
                    "\n" + native_abi_manifest_fields(platform) + platform_contract + web_threads +
                    ios_contract + (platform == "windows" ? "" : "compiler fixture\n"));
    return root;
}

bool diagnostic_contains(const gdpp::NativeBuildPlan& plan, const std::string& expected) {
    return std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(),
                       [&](const std::string& diagnostic) {
                           return diagnostic.find(expected) != std::string::npos;
                       });
}

bool contains_path(const std::vector<std::string>& arguments,
                   const std::filesystem::path& expected) {
    const auto normalized = expected.lexically_normal();
    return std::any_of(arguments.begin(), arguments.end(), [&](const std::string& argument) {
        return std::filesystem::path{argument}.lexically_normal() == normalized;
    });
}

bool contains_utf16le_ascii(const std::filesystem::path& path, const std::string& expected) {
    std::ifstream input{path, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::string needle;
    for (const char character : expected) {
        needle.push_back(character);
        needle.push_back(0);
    }
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xffU &&
           static_cast<unsigned char>(bytes[1]) == 0xfeU &&
           std::search(bytes.begin() + 2, bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

TEST_CASE("native builder rejects an SDK for a different Godot target") {
    const auto root =
        make_sdk_fixture("native-builder-version", "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    options.target_version = gdpp::GodotVersion::v4_7;

    const auto plan = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!plan.success);
    REQUIRE(!plan.diagnostics.empty());
}

TEST_CASE("native builder rejects a legacy SDK before creating compiler commands") {
    const auto root = make_sdk_fixture("native-builder-legacy-sdk",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "sdk/sdk.manifest", "GDPP_SDK 2\napi 4.4\nplatform macos\narch arm64\n"
                                           "profiles debug,release\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(plan.commands.empty());
    REQUIRE(diagnostic_contains(plan, "reinstall the SDK packaged with this GDPP compiler"));
}

TEST_CASE("native builder rejects native ABI manifest drift before creating commands") {
    const auto plan_for = [](const std::filesystem::path& root, const gdpp::NativePlatform platform,
                             const std::string& architecture) {
        gdpp::NativeBuildOptions options;
        options.project_output_directory = root / "project";
        options.binary_output_directory = root / "addons/gdpp/binary";
        options.sdk_root = root / "sdk";
        options.compiler_executable = platform == gdpp::NativePlatform::windows ? "cl" : "clang++";
        options.platform = platform;
        options.architecture = architecture;
        return gdpp::NativeBuilder{}.plan(options);
    };

    const auto standard_root = make_sdk_fixture("native-builder-standard-contract",
                                                "libgodot-cpp.macos.template_release.arm64.a");
    replace_manifest_field(standard_root / "sdk/sdk.manifest", "cxx_standard 17",
                           "cxx_standard 20");
    const auto standard = plan_for(standard_root, gdpp::NativePlatform::macos, "arm64");
    REQUIRE(!standard.success);
    REQUIRE(standard.commands.empty());
    REQUIRE(diagnostic_contains(standard, "C++ standard mismatch"));

    const auto windows_root = make_sdk_fixture("native-builder-windows-crt-contract",
                                               "godot-cpp.windows.template_release.x86_64.lib");
    replace_manifest_field(windows_root / "sdk/sdk.manifest", "msvc_runtime static",
                           "msvc_runtime dynamic");
    const auto windows = plan_for(windows_root, gdpp::NativePlatform::windows, "x86_64");
    REQUIRE(!windows.success);
    REQUIRE(windows.commands.empty());
    REQUIRE(diagnostic_contains(windows, "MSVC runtime mismatch"));

    const auto compiler_root = make_sdk_fixture("native-builder-windows-compiler-contract",
                                                "godot-cpp.windows.template_release.x86_64.lib");
    replace_manifest_field(compiler_root / "sdk/sdk.manifest", "compiler MSVC", "compiler Clang");
    const auto compiler = plan_for(compiler_root, gdpp::NativePlatform::windows, "x86_64");
    REQUIRE(!compiler.success);
    REQUIRE(compiler.commands.empty());
    REQUIRE(diagnostic_contains(compiler, "compiler family mismatch"));

    const auto toolset_root = make_sdk_fixture("native-builder-windows-toolset-contract",
                                               "godot-cpp.windows.template_release.x86_64.lib");
    replace_manifest_field(toolset_root / "sdk/sdk.manifest", "compiler_version 19.44.35207.1",
                           "compiler_version 18.0");
    const auto toolset = plan_for(toolset_root, gdpp::NativePlatform::windows, "x86_64");
    REQUIRE(!toolset.success);
    REQUIRE(toolset.commands.empty());
    REQUIRE(diagnostic_contains(toolset, "MSVC toolset"));

    const auto frontend_root = make_sdk_fixture("native-builder-windows-frontend-contract",
                                                "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions frontend_options;
    frontend_options.project_output_directory = frontend_root / "project";
    frontend_options.binary_output_directory = frontend_root / "addons/gdpp/binary";
    frontend_options.sdk_root = frontend_root / "sdk";
    frontend_options.compiler_executable = "clang-cl.exe";
    frontend_options.platform = gdpp::NativePlatform::windows;
    frontend_options.architecture = "x86_64";
    const auto frontend = gdpp::NativeBuilder{}.plan(frontend_options);
    REQUIRE(!frontend.success);
    REQUIRE(frontend.commands.empty());
    REQUIRE(diagnostic_contains(frontend, "cl.exe frontend"));

    const auto android_root = make_sdk_fixture("native-builder-android-stl-contract",
                                               "libgodot-cpp.android.template_release.arm64.a");
    replace_manifest_field(android_root / "sdk/sdk.manifest", "android_stl c++_shared",
                           "android_stl c++_static");
    const auto android = plan_for(android_root, gdpp::NativePlatform::android, "arm64");
    REQUIRE(!android.success);
    REQUIRE(android.commands.empty());
    REQUIRE(diagnostic_contains(android, "Android SDK STL mismatch"));
}

TEST_CASE("native builder rejects Godot API and precision contract drift") {
    const auto root = make_sdk_fixture("native-builder-api-contract",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    auto manifest = root / "sdk/sdk.manifest";
    replace_manifest_field(manifest, "precision single", "precision double");
    const auto wrong_sdk_precision = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!wrong_sdk_precision.success);
    REQUIRE(wrong_sdk_precision.commands.empty());
    REQUIRE(diagnostic_contains(wrong_sdk_precision, "native SDK precision mismatch"));

    replace_manifest_field(manifest, "precision double", "precision single");
    replace_manifest_field(manifest, std::string{"api_sha256 "} + GDPP_GODOT_API_SHA256_4_4,
                           "api_sha256 deadbeef");
    const auto wrong_api = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!wrong_api.success);
    REQUIRE(wrong_api.commands.empty());
    REQUIRE(diagnostic_contains(wrong_api, "API fingerprint"));

    replace_manifest_field(manifest, "api_sha256 deadbeef",
                           std::string{"api_sha256 "} + GDPP_GODOT_API_SHA256_4_4);
    options.precision = gdpp::NativePrecision::double_precision;
    const auto wrong_compiler_precision = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!wrong_compiler_precision.success);
    REQUIRE(wrong_compiler_precision.commands.empty());
    REQUIRE(diagnostic_contains(wrong_compiler_precision, "compiler Godot API precision"));
}

TEST_CASE("native precision names reject ambiguous ABI values") {
    REQUIRE_EQ(std::string{gdpp::native_precision_name(gdpp::NativePrecision::single)}, "single");
    REQUIRE_EQ(
        std::string{gdpp::native_precision_name(gdpp::NativePrecision::double_precision)},
        "double");
    REQUIRE(gdpp::parse_native_precision("single") == gdpp::NativePrecision::single);
    REQUIRE(gdpp::parse_native_precision("double") ==
            gdpp::NativePrecision::double_precision);
    REQUIRE(!gdpp::parse_native_precision(""));
    REQUIRE(!gdpp::parse_native_precision("float"));
    REQUIRE(!gdpp::parse_native_precision("double_precision"));
}

TEST_CASE("native builder rejects a modified SDK runtime before creating compiler commands") {
    const auto root = make_sdk_fixture("native-builder-corrupt-sdk",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "sdk/include/gdpp/runtime/variant_ops.hpp", "// modified after packaging\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(plan.commands.empty());
    REQUIRE(diagnostic_contains(plan, "runtime header failed integrity validation"));
}

TEST_CASE("native builder rejects a modified attached runtime before creating compiler commands") {
    const auto root = make_sdk_fixture("native-builder-corrupt-attached",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "sdk/src/runtime/attached_script_instance.cpp",
                "// modified after packaging\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(plan.commands.empty());
    REQUIRE(
        diagnostic_contains(plan, "attached instance runtime source failed integrity validation"));
}

TEST_CASE("native builder rejects modified integer semantics before creating commands") {
    const auto root = make_sdk_fixture("native-builder-corrupt-integers",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "sdk/include/gdpp/numeric/integer_semantics.hpp",
                "// modified after packaging\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(plan.commands.empty());
    REQUIRE(diagnostic_contains(plan, "integer semantics header failed integrity validation"));
}

TEST_CASE("native builder rejects unsupported architecture names before planning commands") {
    const auto root = make_sdk_fixture("native-builder-invalid-arch",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "mips64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(!plan.diagnostics.empty());
    REQUIRE(plan.commands.empty());
}

TEST_CASE("native builder rejects non-Release distribution binding contracts") {
    const auto root = make_sdk_fixture("native-builder-binding-contract",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    const auto manifest = root / "sdk/sdk.manifest";
    replace_manifest_field(manifest, "distribution_binding template_release",
                           "distribution_binding template_debug");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto wrong_binding = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!wrong_binding.success);
    REQUIRE(diagnostic_contains(wrong_binding, "distribution binding mismatch"));

    replace_manifest_field(manifest, "distribution_binding template_debug",
                           "distribution_binding template_release");
    replace_manifest_field(manifest, "distribution_optimization Release",
                           "distribution_optimization Debug");
    const auto wrong_optimization = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!wrong_optimization.success);
    REQUIRE(diagnostic_contains(wrong_optimization, "distribution optimization mismatch"));
}

TEST_CASE("native architecture support matches the shipped target matrix") {
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::macos, "arm64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::macos, "x86_64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::macos, "universal"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::linux, "x86_64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::windows, "x86_64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::android, "arm64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::ios, "arm64"));
    REQUIRE(gdpp::native_architecture_supported(gdpp::NativePlatform::web, "wasm32"));

    REQUIRE(!gdpp::native_architecture_supported(gdpp::NativePlatform::windows, "arm64"));
    REQUIRE(!gdpp::native_architecture_supported(gdpp::NativePlatform::linux, "arm64"));
    REQUIRE(!gdpp::native_architecture_supported(gdpp::NativePlatform::android, "x86_64"));
    REQUIRE(!gdpp::native_architecture_supported(gdpp::NativePlatform::ios, "x86_64"));
    REQUIRE(!gdpp::native_architecture_supported(gdpp::NativePlatform::web, "x86_64"));
}

TEST_CASE("native builder rejects unshipped Windows arm64 before touching the SDK") {
    const auto root = make_sdk_fixture("native-builder-windows-arm64",
                                       "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "cl.exe";
    options.platform = gdpp::NativePlatform::windows;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(plan.commands.empty());
    REQUIRE(diagnostic_contains(plan, "unsupported native architecture 'arm64' for windows"));
}

TEST_CASE("native builder requires an explicit project library output directory") {
    const auto root = make_sdk_fixture("native-builder-output-directory",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(!plan.success);
    REQUIRE(!plan.diagnostics.empty());
    REQUIRE(plan.commands.empty());
    REQUIRE(!std::filesystem::exists(root / "project/bin"));
}

bool contains(const std::vector<std::string>& arguments, const std::string& value) {
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}

} // namespace

TEST_CASE("native build profiles use product roles instead of Godot ABI target names") {
    REQUIRE_EQ(*gdpp::parse_native_build_profile("debug"), gdpp::NativeBuildProfile::debug);
    REQUIRE_EQ(*gdpp::parse_native_build_profile("release"), gdpp::NativeBuildProfile::release);
    REQUIRE(!gdpp::parse_native_build_profile("development").has_value());
    REQUIRE(!gdpp::parse_native_build_profile("editor").has_value());
    REQUIRE(!gdpp::parse_native_build_profile("template_release").has_value());
}

TEST_CASE("native builder creates release macOS commands and reuses fresh objects") {
    const auto root =
        make_sdk_fixture("native-builder-macos", "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    const gdpp::NativeBuilder builder;
    const auto first = builder.plan(options);

    REQUIRE(first.success);
    REQUIRE_EQ(first.commands.size(), std::size_t{4});
    REQUIRE_EQ(first.commands.front().executable, std::string{"clang++"});
    REQUIRE(contains(first.commands.front().arguments, "-std=c++17"));
    REQUIRE(contains(first.commands.back().arguments, "-dynamiclib"));
    REQUIRE_EQ(first.output_library.filename().string(),
               std::string{"libgdpp.release.macos.arm64.dylib"});
    REQUIRE_EQ(first.output_library.parent_path(), options.binary_output_directory);

    const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::seconds{5};
    for (const auto& name : {"example_gd_cpp.o", "register_types_cpp.o", "variant_ops_cpp.o"}) {
        const auto object = root / "project/native-direct/4.4/macos/arm64/release/objects" / name;
        write_input(object);
        std::filesystem::last_write_time(object, future);
    }
    write_input(first.output_library);
    std::filesystem::last_write_time(first.output_library, future + std::chrono::seconds{1});
    const auto second = builder.plan(options);
    REQUIRE(second.success);
    REQUIRE(second.up_to_date);
    REQUIRE(second.commands.empty());

    options.compiler_executable = "clang++-commercial-upgrade";
    const auto toolchain_change = builder.plan(options);
    REQUIRE(toolchain_change.success);
    REQUIRE(!toolchain_change.up_to_date);
    REQUIRE_EQ(toolchain_change.commands.size(), std::size_t{4});
}

TEST_CASE("native builder recompiles only translation units affected by a generated header") {
    const auto root = make_sdk_fixture("native-builder-precise-objects",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    const auto generated = root / "project/generated";
    write_input(generated / "example.gd.cpp", "#include \"example.gd.hpp\"\n");
    write_input(generated / "example.gd.hpp", "#pragma once\n#include \"shared.hpp\"\n");
    write_input(generated / "shared.hpp", "#pragma once\n");
    write_input(generated / "other.gd.cpp", "#include \"other.gd.hpp\"\n");
    write_input(generated / "other.gd.hpp", "#pragma once\n");
    write_input(root / "project/register_types.cpp",
                "#include \"example.gd.hpp\"\n#include \"other.gd.hpp\"\n");

    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    const gdpp::NativeBuilder builder;
    const auto initial = builder.plan(options);
    REQUIRE(initial.success);
    REQUIRE_EQ(initial.commands.size(), std::size_t{5});

    const auto objects = root / "project/native-direct/4.4/macos/arm64/release/objects";
    const auto fresh = std::filesystem::file_time_type::clock::now() + std::chrono::seconds{5};
    for (const auto& name :
         {"example_gd_cpp.o", "other_gd_cpp.o", "register_types_cpp.o", "variant_ops_cpp.o"}) {
        write_input(objects / name);
        std::filesystem::last_write_time(objects / name, fresh);
    }
    write_input(initial.output_library);
    std::filesystem::last_write_time(initial.output_library, fresh + std::chrono::seconds{1});
    std::filesystem::last_write_time(generated / "shared.hpp", fresh + std::chrono::seconds{2});

    const auto changed = builder.plan(options);

    REQUIRE(changed.success);
    REQUIRE_EQ(changed.commands.size(), std::size_t{3});
    REQUIRE(contains_path(changed.commands[0].arguments, generated / "example.gd.cpp"));
    REQUIRE(contains_path(changed.commands[1].arguments, root / "project/register_types.cpp"));
    REQUIRE(!contains_path(changed.commands[0].arguments, generated / "other.gd.cpp"));
    REQUIRE(
        !contains_path(changed.commands[1].arguments, root / "sdk/src/runtime/variant_ops.cpp"));
    REQUIRE(contains(changed.commands.back().arguments, "-dynamiclib"));

    std::filesystem::last_write_time(objects / "example_gd_cpp.o", fresh + std::chrono::seconds{3});
    std::filesystem::last_write_time(objects / "register_types_cpp.o",
                                     fresh + std::chrono::seconds{3});
    std::filesystem::last_write_time(initial.output_library, fresh + std::chrono::seconds{3});
    std::filesystem::last_write_time(generated / "other.gd.cpp", fresh + std::chrono::seconds{4});

    const auto implementation_changed = builder.plan(options);

    REQUIRE(implementation_changed.success);
    REQUIRE_EQ(implementation_changed.commands.size(), std::size_t{2});
    REQUIRE(contains_path(implementation_changed.commands.front().arguments,
                          generated / "other.gd.cpp"));
    REQUIRE(contains(implementation_changed.commands.back().arguments, "-dynamiclib"));
}

TEST_CASE("native builder relinks without recompiling when a static library changes") {
    const auto root = make_sdk_fixture("native-builder-link-only-change",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    const gdpp::NativeBuilder builder;
    const auto first = builder.plan(options);
    REQUIRE(first.success);

    const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::seconds{5};
    for (const auto& name : {"example_gd_cpp.o", "register_types_cpp.o", "variant_ops_cpp.o"}) {
        const auto object = root / "project/native-direct/4.4/macos/arm64/release/objects" / name;
        write_input(object);
        std::filesystem::last_write_time(object, future);
    }
    write_input(first.output_library);
    std::filesystem::last_write_time(first.output_library, future + std::chrono::seconds{1});
    const auto binding_library = root / "sdk/lib/libgodot-cpp.macos.template_release.arm64.a";
    std::filesystem::last_write_time(binding_library, future + std::chrono::seconds{2});

    const auto relink = builder.plan(options);

    REQUIRE(relink.success);
    REQUIRE(!relink.up_to_date);
    REQUIRE_EQ(relink.commands.size(), std::size_t{1});
    REQUIRE(contains(relink.commands.front().arguments, "-dynamiclib"));
    REQUIRE(contains_path(relink.commands.front().arguments, binding_library));
}

TEST_CASE("native builder injects the selected third-party bridge target") {
    const auto root = make_sdk_fixture("native-builder-extension-bridge",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    const auto include = root / "provider/include";
    const auto library = root / "provider/lib/libprovider.a";
    const auto manifest = root / "provider/gdpp_bridge.json";
    std::filesystem::create_directories(include);
    write_input(library);
    write_input(manifest, "{}\n");
    write_input(root / "project/bridge.lock",
                "GDPP_BRIDGE_LOCK 1\nbridge \"provider-v1\" \"" + manifest.generic_string() +
                    "\"\ntarget \"macos\" \"arm64\" \"release\" 1 \"" + include.generic_string() +
                    "\" 1 \"" + library.generic_string() + "\"\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains_path(plan.commands.front().arguments, include));
    REQUIRE(contains_path(plan.commands.back().arguments, library));
}

TEST_CASE("native builder accepts a runtime-only bridge without native link inputs") {
    const auto root = make_sdk_fixture("native-builder-runtime-bridge",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "project/bridge.lock",
                "GDPP_BRIDGE_LOCK 1\nbridge \"classdb:ProviderRuntime\" \"\"\nruntime\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(std::none_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
        return contains_path(command.arguments, root / "provider/lib/libprovider.a");
    }));
}

TEST_CASE("native builder compiles the complete attached runtime on demand") {
    const auto root = make_sdk_fixture("native-builder-attached-runtime",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "project/register_types.cpp",
                "void probe() { gdpp::runtime::register_attached_script({}); }\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    for (const auto& name : {"attached_script_registry.cpp", "attached_script_instance.cpp",
                             "attached_script_language.cpp"}) {
        REQUIRE(std::any_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
            return contains_path(command.arguments, root / "sdk/src/runtime" / name);
        }));
    }
}

TEST_CASE("native builder emits MSVC compile and link arguments") {
    const auto root =
        make_sdk_fixture("native-builder-windows", "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "cl.exe";
    options.platform = gdpp::NativePlatform::windows;
    options.architecture = "x86_64";
    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.commands.size(), std::size_t{4});
    REQUIRE(contains(plan.commands.front().arguments, "/std:c++17"));
    REQUIRE(contains(plan.commands.front().arguments, "/utf-8"));
    REQUIRE(contains(plan.commands.front().arguments, "/MT"));
    REQUIRE(contains(plan.commands.front().arguments, "/bigobj"));
    REQUIRE(contains(plan.commands.front().arguments, "/experimental:deterministic"));
    REQUIRE(contains(plan.commands.front().arguments, "/DWINDOWS_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "/DTYPED_METHOD_BIND"));
    REQUIRE(contains(plan.commands.front().arguments, "/D_HAS_EXCEPTIONS=0"));
    REQUIRE(contains(plan.commands.front().arguments, "/D_WIN32_WINNT=0x0A00"));
    REQUIRE(contains(plan.commands.front().arguments, "/DWINVER=0x0A00"));
    REQUIRE_EQ(plan.commands.back().executable, std::string{"link.exe"});
    REQUIRE_EQ(plan.commands.back().arguments.size(), std::size_t{2});
    REQUIRE(plan.commands.back().arguments.back().front() == '@');
    const std::filesystem::path response_file = plan.commands.back().arguments.back().substr(1);
    REQUIRE(response_file.filename() == "link.rsp");
    REQUIRE(contains_utf16le_ascii(response_file, "/DLL"));
    REQUIRE(contains_utf16le_ascii(response_file, "/MACHINE:X64"));
    REQUIRE(contains_utf16le_ascii(response_file, "/IMPLIB:"));
    REQUIRE(contains_utf16le_ascii(response_file, "gdpp.lib"));
    REQUIRE(!contains_utf16le_ascii(response_file, "gdpp_project.lib"));
    REQUIRE(contains_utf16le_ascii(response_file, "/OUT:"));
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"gdpp.release.windows.x86_64.dll"});
    REQUIRE_EQ(plan.output_library.parent_path(), options.binary_output_directory);
}

TEST_CASE("native builder pins the MSVC linker beside an absolute compiler") {
    const auto root = make_sdk_fixture("native-builder-windows-absolute-toolchain",
                                       "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = (root / "toolchain/Hostx64/x64/cl.exe").string();
    options.platform = gdpp::NativePlatform::windows;
    options.architecture = "x86_64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.commands.front().executable, options.compiler_executable);
    REQUIRE_EQ(plan.commands.back().executable, (root / "toolchain/Hostx64/x64/link.exe").string());
}

TEST_CASE("native builder creates a stable release library with release bindings") {
    const auto root =
        make_sdk_fixture("native-builder-release", "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.macos.arm64.dylib"});
    REQUIRE(contains(plan.commands.front().arguments, "-DNDEBUG"));
    REQUIRE(contains(plan.commands.front().arguments, "-fvisibility=hidden"));
    REQUIRE(contains(plan.commands.front().arguments, "-mmacosx-version-min=11.0"));
    REQUIRE(contains(plan.commands.front().arguments, "-ffunction-sections"));
    REQUIRE(contains(plan.commands.front().arguments, "-fdata-sections"));
    REQUIRE(!contains(plan.commands.front().arguments, "-DDEBUG_ENABLED"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-dead_strip"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-x"));
    REQUIRE(std::any_of(plan.commands.back().arguments.begin(),
                        plan.commands.back().arguments.end(), [](const std::string& argument) {
                            return argument.find("-Wl,-exported_symbols_list,") == 0;
                        }));
    REQUIRE_EQ(read_input(root / "project/native-direct/4.4/macos/arm64/release/gdpp.exports.map"),
               std::string{"_gdpp_library_init\n"});
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.macos.template_release.arm64.a"));
}

TEST_CASE("native builder enables release dead-code elimination for MSVC") {
    const auto root = make_sdk_fixture("native-builder-windows-release",
                                       "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "cl.exe";
    options.platform = gdpp::NativePlatform::windows;
    options.architecture = "x86_64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains(plan.commands.front().arguments, "/Gy"));
    REQUIRE(contains(plan.commands.front().arguments, "/Gw"));
    const std::filesystem::path response_file = plan.commands.back().arguments.back().substr(1);
    REQUIRE(contains_utf16le_ascii(response_file, "/OPT:REF"));
    REQUIRE(contains_utf16le_ascii(response_file, "/OPT:ICF"));
    REQUIRE(contains_utf16le_ascii(response_file, "/INCREMENTAL:NO"));
}

TEST_CASE("native builder optimizes MSVC debug exports against release bindings") {
    const auto root = make_sdk_fixture("native-builder-windows-debug",
                                       "godot-cpp.windows.template_release.x86_64.lib");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "cl.exe";
    options.platform = gdpp::NativePlatform::windows;
    options.architecture = "x86_64";
    options.profile = gdpp::NativeBuildProfile::debug;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains(plan.commands.front().arguments, "/O2"));
    REQUIRE(contains(plan.commands.front().arguments, "/DNDEBUG"));
    REQUIRE(contains(plan.commands.front().arguments, "/DGDPP_SCRIPT_DEBUG_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "/Od"));
    REQUIRE(!contains(plan.commands.front().arguments, "/DDEBUG_ENABLED"));
    const std::filesystem::path response_file = plan.commands.back().arguments.back().substr(1);
    REQUIRE(contains_utf16le_ascii(response_file, "godot-cpp.windows.template_release.x86_64.lib"));
    REQUIRE(contains_utf16le_ascii(response_file, "/OPT:REF"));
    REQUIRE(contains_utf16le_ascii(response_file, "/OPT:ICF"));
    REQUIRE(contains_utf16le_ascii(response_file, "/INCREMENTAL:NO"));
}

TEST_CASE("native builder keeps optimized debug exports in an isolated object cache") {
    const auto root = make_sdk_fixture("native-builder-template-debug",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";
    options.profile = gdpp::NativeBuildProfile::debug;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.debug.macos.arm64.dylib"});
    REQUIRE(contains(plan.commands.front().arguments, "-O3"));
    REQUIRE(contains(plan.commands.front().arguments, "-DNDEBUG"));
    REQUIRE(contains(plan.commands.front().arguments, "-DGDPP_SCRIPT_DEBUG_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-DDEBUG_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-g"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-dead_strip"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-x"));
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.macos.template_release.arm64.a"));
    REQUIRE(std::filesystem::path{plan.commands.front().arguments.back()}.generic_string().find(
                "native-direct/4.4/macos/arm64/debug/objects") != std::string::npos);
}

TEST_CASE("native builder emits Android NDK compile and hardened release link arguments") {
    const auto root =
        make_sdk_fixture("native-builder-android", "libgodot-cpp.android.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "/opt/android-ndk/toolchains/llvm/bin/clang++";
    options.platform = gdpp::NativePlatform::android;
    options.architecture = "arm64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.commands.size(), std::size_t{4});
    REQUIRE(contains(plan.commands.front().arguments, "--target=aarch64-linux-android28"));
    REQUIRE(contains(plan.commands.front().arguments, "-DANDROID_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "-DUNIX_ENABLED"));
    REQUIRE(std::any_of(plan.commands.front().arguments.begin(),
                        plan.commands.front().arguments.end(), [](const std::string& argument) {
                            return argument.find("-ffile-prefix-map=") == 0 &&
                                   argument.find("=/gdpp/project") != std::string::npos;
                        }));
    REQUIRE(std::any_of(plan.commands.front().arguments.begin(),
                        plan.commands.front().arguments.end(), [](const std::string& argument) {
                            return argument.find("-ffile-prefix-map=") == 0 &&
                                   argument.find("=/gdpp/sdk") != std::string::npos;
                        }));
    REQUIRE(contains(plan.commands.back().arguments, "--target=aarch64-linux-android28"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--gc-sections"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-s"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-z,relro"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-z,now"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--exclude-libs,ALL"));
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.android.arm64.so"});
}

TEST_CASE("native builder selects an Android manifest and library from a shared SDK") {
    const auto root = make_sdk_fixture("native-builder-shared-android-host",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    const auto android = make_sdk_fixture("native-builder-shared-android-target",
                                          "libgodot-cpp.android.template_release.arm64.a");
    std::filesystem::create_directories(root / "sdk/manifests");
    std::filesystem::copy_file(android / "sdk/sdk.manifest",
                               root / "sdk/manifests/android.arm64.sdk.manifest",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(android / "sdk/lib/libgodot-cpp.android.template_release.arm64.a",
                               root / "sdk/lib/libgodot-cpp.android.template_release.arm64.a",
                               std::filesystem::copy_options::overwrite_existing);

    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "/opt/android-ndk/toolchains/llvm/bin/clang++";
    options.platform = gdpp::NativePlatform::android;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.android.template_release.arm64.a"));
    REQUIRE(!contains_path(plan.commands.back().arguments,
                           root / "sdk/lib/libgodot-cpp.macos.template_release.arm64.a"));
}

TEST_CASE("native builder emits a transactional device and Universal Simulator XCFramework") {
    const auto root =
        make_sdk_fixture("native-builder-ios", "libgodot-cpp.ios.template_release.arm64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "xcrun";
    options.platform = gdpp::NativePlatform::ios;
    options.architecture = "arm64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.commands.size(), std::size_t{14});
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.ios.arm64.xcframework"});
    REQUIRE_EQ(plan.pending_output_library.filename(), plan.output_library.filename());
    REQUIRE(plan.pending_output_library.generic_string().find("xcframework-staging") !=
            std::string::npos);
    REQUIRE_EQ(plan.commands.front().stage, std::size_t{0});
    REQUIRE(contains(plan.commands.front().arguments, "iphoneos"));
    REQUIRE(contains(plan.commands.front().arguments, "arm64-apple-ios16.0"));
    REQUIRE(contains(plan.commands.front().arguments, "-DIOS_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "-DUNIX_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "-DTHREADS_ENABLED"));
    REQUIRE_EQ(plan.commands.back().stage, std::size_t{3});
    REQUIRE(contains(plan.commands.back().arguments, "xcodebuild"));
    REQUIRE(contains(plan.commands.back().arguments, "-create-xcframework"));
    REQUIRE(std::count_if(plan.commands.begin(), plan.commands.end(),
                          [](const auto& command) { return command.stage == 0; }) == 9);
    REQUIRE(std::count_if(plan.commands.begin(), plan.commands.end(),
                          [](const auto& command) { return command.stage == 1; }) == 3);
    REQUIRE(std::count_if(plan.commands.begin(), plan.commands.end(), [](const auto& command) {
                return command.stage == 2 && contains(command.arguments, "lipo");
            }) == 1);
    REQUIRE(std::all_of(plan.commands.begin(), plan.commands.end(), [](const auto& command) {
        return command.stage != 1 ||
               contains(command.arguments, "-Wl,-install_name,@rpath/libgdpp.dylib");
    }));
    REQUIRE(std::any_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
        return command.stage == 1 &&
               contains_path(command.arguments, root /
                                                    "project/native-direct/4.4/ios/arm64/release/"
                                                    "device-arm64/libgdpp.dylib");
    }));
    REQUIRE(std::none_of(plan.commands.begin(), plan.commands.end(), [](const auto& command) {
        return std::any_of(command.arguments.begin(), command.arguments.end(),
                           [](const auto& argument) {
                               return argument.find("libgdpp_project.dylib") != std::string::npos;
                           });
    }));
    REQUIRE(std::any_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
        return command.stage == 1 &&
               contains_path(command.arguments,
                             root / "sdk/lib/simulator/"
                                    "libgodot-cpp.ios.template_release.universal.a") &&
               contains(command.arguments, "x86_64-apple-ios16.0-simulator");
    }));
}

TEST_CASE("native builder selects flattened iOS slices from a shared SDK") {
    const auto root = make_sdk_fixture("native-builder-shared-ios-host",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    const auto ios = make_sdk_fixture("native-builder-shared-ios-target",
                                      "libgodot-cpp.ios.template_release.arm64.a");
    std::filesystem::create_directories(root / "sdk/manifests");
    std::filesystem::copy_file(ios / "sdk/sdk.manifest",
                               root / "sdk/manifests/ios.arm64.sdk.manifest",
                               std::filesystem::copy_options::overwrite_existing);
    for (const auto& library : {"libgodot-cpp.ios.template_release.arm64.a",
                                "libgodot-cpp.ios.template_release.universal.a"}) {
        const auto source_directory =
            std::string{library}.find(".universal.") == std::string::npos ? "device" : "simulator";
        std::filesystem::copy_file(ios / "sdk/lib" / source_directory / library,
                                   root / "sdk/lib" / library,
                                   std::filesystem::copy_options::overwrite_existing);
    }

    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "xcrun";
    options.platform = gdpp::NativePlatform::ios;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(std::any_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
        return contains_path(command.arguments,
                             root / "sdk/lib/libgodot-cpp.ios.template_release.arm64.a");
    }));
    REQUIRE(std::any_of(plan.commands.begin(), plan.commands.end(), [&](const auto& command) {
        return contains_path(command.arguments,
                             root / "sdk/lib/libgodot-cpp.ios.template_release.universal.a");
    }));
}

TEST_CASE("native builder fails closed for incomplete iOS target contracts") {
    const auto root = make_sdk_fixture("native-builder-ios-contract",
                                       "libgodot-cpp.ios.template_release.arm64.a");
    write_input(root / "sdk/sdk.manifest",
                "GDPP_SDK " + std::to_string(GDPP_NATIVE_SDK_SCHEMA) +
                    "\napi 4.4\n" + api_manifest_fields() +
                    "platform ios\narch arm64\nprofiles debug,release\n" +
                    binding_manifest_fields("ios") + "runtime_abi " +
                    std::to_string(GDPP_NATIVE_RUNTIME_ABI) + "\nruntime_header_sha256 " +
                    GDPP_NATIVE_RUNTIME_HEADER_SHA256 + "\nreference_semantics_header_sha256 " +
                    GDPP_REFERENCE_SEMANTICS_HEADER_SHA256 + "\nruntime_source_sha256 " +
                    GDPP_NATIVE_RUNTIME_SOURCE_SHA256 + "\n" + attached_runtime_manifest_fields() +
                    "integer_semantics_header_sha256 " + GDPP_INTEGER_SEMANTICS_HEADER_SHA256 +
                    "\n" + native_abi_manifest_fields("ios") +
                    "platform_minimum iOS_16.0\nios_deployment_target 16.0\n"
                    "ios_slices device-arm64\n"
                    "source_paths mapped\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "xcrun";
    options.platform = gdpp::NativePlatform::ios;
    options.architecture = "arm64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto missing_slices = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!missing_slices.success);
    REQUIRE(diagnostic_contains(missing_slices, "simulator-arm64"));
}

TEST_CASE("native builder emits a single-threaded WebAssembly side module") {
    const auto root = make_sdk_fixture("native-builder-web-nothreads",
                                       "libgodot-cpp.web.template_release.wasm32.nothreads.a");
    const auto compiler = root / "toolchain/bin/em++";
    write_input(compiler);
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = compiler.string();
    options.platform = gdpp::NativePlatform::web;
    options.architecture = "wasm32";
    options.profile = gdpp::NativeBuildProfile::release;
    options.web_thread_mode = gdpp::NativeWebThreadMode::single_threaded;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.commands.size(), std::size_t{4});
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.web.wasm32.nothreads.wasm"});
    REQUIRE(contains(plan.commands.front().arguments, "-sSIDE_MODULE=1"));
    REQUIRE(contains(plan.commands.front().arguments, "-sSUPPORT_LONGJMP=wasm"));
    REQUIRE(contains(plan.commands.front().arguments, "-DWEB_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "-DUNIX_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-DTHREADS_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-sUSE_PTHREADS=1"));
    REQUIRE(std::any_of(plan.commands.front().arguments.begin(),
                        plan.commands.front().arguments.end(), [](const std::string& argument) {
                            return argument.find("=/gdpp/toolchain/compiler") != std::string::npos;
                        }));
    REQUIRE(std::any_of(plan.commands.back().arguments.begin(),
                        plan.commands.back().arguments.end(), [](const std::string& argument) {
                            return argument.find("=/gdpp/toolchain/compiler") != std::string::npos;
                        }));
    REQUIRE(contains(plan.commands.back().arguments, "-sWASM_BIGINT"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--gc-sections"));
    REQUIRE(!contains(plan.commands.back().arguments, "-sUSE_PTHREADS=1"));
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.web.template_release.wasm32.nothreads.a"));
    REQUIRE(std::filesystem::path{plan.commands.front().arguments.back()}.generic_string().find(
                "native-direct/4.4/web/wasm32/nothreads/release/objects") != std::string::npos);
}

TEST_CASE("native builder isolates threaded WebAssembly flags and artifacts") {
    const auto root = make_sdk_fixture("native-builder-web-threads",
                                       "libgodot-cpp.web.template_release.wasm32.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "em++";
    options.platform = gdpp::NativePlatform::web;
    options.architecture = "wasm32";
    options.profile = gdpp::NativeBuildProfile::debug;
    options.web_thread_mode = gdpp::NativeWebThreadMode::multi_threaded;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.debug.web.wasm32.threads.wasm"});
    REQUIRE(contains(plan.commands.front().arguments, "-DTHREADS_ENABLED"));
    REQUIRE(contains(plan.commands.front().arguments, "-sUSE_PTHREADS=1"));
    REQUIRE(contains(plan.commands.front().arguments, "-O3"));
    REQUIRE(contains(plan.commands.front().arguments, "-DNDEBUG"));
    REQUIRE(contains(plan.commands.front().arguments, "-DGDPP_SCRIPT_DEBUG_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-DDEBUG_ENABLED"));
    REQUIRE(!contains(plan.commands.front().arguments, "-g2"));
    REQUIRE(!contains(plan.commands.front().arguments, "-g"));
    REQUIRE(contains(plan.commands.back().arguments, "-sUSE_PTHREADS=1"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--gc-sections"));
    REQUIRE(contains(plan.commands.back().arguments, "-s"));
    REQUIRE(!contains(plan.commands.back().arguments, "-sASSERTIONS=1"));
    REQUIRE(!contains(plan.commands.back().arguments, "-g2"));
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.web.template_release.wasm32.a"));
}

TEST_CASE("native builder selects both Web variants from one shared SDK library directory") {
    const auto root = make_sdk_fixture("native-builder-shared-web-threads",
                                       "libgodot-cpp.web.template_release.wasm32.a");
    const auto nothreads = make_sdk_fixture("native-builder-shared-web-nothreads",
                                            "libgodot-cpp.web.template_release.wasm32.nothreads.a");
    std::filesystem::create_directories(root / "sdk/manifests");
    std::filesystem::copy_file(root / "sdk/sdk.manifest",
                               root / "sdk/manifests/web.wasm32.threads.sdk.manifest",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(nothreads / "sdk/sdk.manifest",
                               root / "sdk/manifests/web.wasm32.nothreads.sdk.manifest",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(
        nothreads / "sdk/lib/libgodot-cpp.web.template_release.wasm32.nothreads.a",
        root / "sdk/lib/libgodot-cpp.web.template_release.wasm32.nothreads.a",
        std::filesystem::copy_options::overwrite_existing);

    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "em++";
    options.platform = gdpp::NativePlatform::web;
    options.architecture = "wasm32";

    options.web_thread_mode = gdpp::NativeWebThreadMode::single_threaded;
    const auto single_threaded = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(single_threaded.success);
    REQUIRE(contains_path(single_threaded.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.web.template_release.wasm32.nothreads.a"));

    options.web_thread_mode = gdpp::NativeWebThreadMode::multi_threaded;
    const auto multi_threaded = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(multi_threaded.success);
    REQUIRE(contains_path(multi_threaded.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.web.template_release.wasm32.a"));
}

TEST_CASE("native builder fails closed for incomplete Web target contracts") {
    const auto root = make_sdk_fixture("native-builder-web-nothreads-contract",
                                       "libgodot-cpp.web.template_release.wasm32.nothreads.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "em++";
    options.platform = gdpp::NativePlatform::web;
    options.architecture = "wasm32";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto missing_variant = gdpp::NativeBuilder{}.plan(options);
    REQUIRE(!missing_variant.success);
    REQUIRE(diagnostic_contains(missing_variant, "explicitly select threads or nothreads"));
}

TEST_CASE("native builder strips Linux release symbols") {
    const auto root = make_sdk_fixture("native-builder-linux-release",
                                       "libgodot-cpp.linux.template_release.x86_64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "g++";
    options.platform = gdpp::NativePlatform::linux;
    options.architecture = "x86_64";
    options.profile = gdpp::NativeBuildProfile::release;

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,-s"));
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--gc-sections"));
}

TEST_CASE("native builder hides static archive symbols in every Linux project extension") {
    const auto root =
        make_sdk_fixture("native-builder-linux", "libgodot-cpp.linux.template_release.x86_64.a");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "g++";
    options.platform = gdpp::NativePlatform::linux;
    options.architecture = "x86_64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains(plan.commands.back().arguments, "-Wl,--exclude-libs,ALL"));
    REQUIRE(std::any_of(
        plan.commands.back().arguments.begin(), plan.commands.back().arguments.end(),
        [](const std::string& argument) { return argument.find("-Wl,--version-script=") == 0; }));
    REQUIRE_EQ(read_input(root / "project/native-direct/4.4/linux/x86_64/release/gdpp.exports.map"),
               std::string{"{ global: gdpp_library_init; local: *; };\n"});
}

TEST_CASE("native builder emits a macOS universal compile and link plan") {
    const auto root = make_sdk_fixture("native-builder-universal",
                                       "libgodot-cpp.macos.template_release.universal.a");
    write_input(root / "sdk/sdk.manifest",
                "GDPP_SDK " + std::to_string(GDPP_NATIVE_SDK_SCHEMA) +
                    "\napi 4.4\n" + api_manifest_fields() +
                    "platform macos\narch universal\n"
                    "profiles debug,release\n" +
                    binding_manifest_fields("macos") +
                    "platform_minimum macOS_11.0\n"
                    "runtime_abi " +
                    std::to_string(GDPP_NATIVE_RUNTIME_ABI) + "\nruntime_header_sha256 " +
                    GDPP_NATIVE_RUNTIME_HEADER_SHA256 + "\nreference_semantics_header_sha256 " +
                    GDPP_REFERENCE_SEMANTICS_HEADER_SHA256 + "\nruntime_source_sha256 " +
                    GDPP_NATIVE_RUNTIME_SOURCE_SHA256 + "\n" + attached_runtime_manifest_fields() +
                    "integer_semantics_header_sha256 " + GDPP_INTEGER_SEMANTICS_HEADER_SHA256 +
                    "\n" + native_abi_manifest_fields("macos") + "compiler fixture\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "universal";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.macos.universal.dylib"});
    REQUIRE(contains(plan.commands.front().arguments, "arm64"));
    REQUIRE(contains(plan.commands.front().arguments, "x86_64"));
    REQUIRE(contains(plan.commands.back().arguments, "arm64"));
    REQUIRE(contains(plan.commands.back().arguments, "x86_64"));
}

TEST_CASE("macOS universal SDK can build a thin host release library") {
    const auto root = make_sdk_fixture("native-builder-universal-host",
                                       "libgodot-cpp.macos.template_release.universal.a");
    write_input(root / "sdk/sdk.manifest",
                "GDPP_SDK " + std::to_string(GDPP_NATIVE_SDK_SCHEMA) +
                    "\napi 4.4\n" + api_manifest_fields() +
                    "platform macos\narch universal\n"
                    "profiles debug,release\n" +
                    binding_manifest_fields("macos") +
                    "platform_minimum macOS_11.0\n"
                    "runtime_abi " +
                    std::to_string(GDPP_NATIVE_RUNTIME_ABI) + "\nruntime_header_sha256 " +
                    GDPP_NATIVE_RUNTIME_HEADER_SHA256 + "\nreference_semantics_header_sha256 " +
                    GDPP_REFERENCE_SEMANTICS_HEADER_SHA256 + "\nruntime_source_sha256 " +
                    GDPP_NATIVE_RUNTIME_SOURCE_SHA256 + "\n" + attached_runtime_manifest_fields() +
                    "integer_semantics_header_sha256 " + GDPP_INTEGER_SEMANTICS_HEADER_SHA256 +
                    "\n" + native_abi_manifest_fields("macos") + "compiler fixture\n");
    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE_EQ(plan.output_library.filename().string(),
               std::string{"libgdpp.release.macos.arm64.dylib"});
    REQUIRE(contains(plan.commands.front().arguments, "arm64"));
    REQUIRE(!contains(plan.commands.front().arguments, "x86_64"));
}

TEST_CASE("native builder prefers an exact macOS architecture over a Universal fallback") {
    const auto root = make_sdk_fixture("native-builder-exact-host",
                                       "libgodot-cpp.macos.template_release.arm64.a");
    write_input(root / "sdk/lib/libgodot-cpp.macos.template_release.universal.a");

    gdpp::NativeBuildOptions options;
    options.project_output_directory = root / "project";
    options.binary_output_directory = root / "addons/gdpp/binary";
    options.sdk_root = root / "sdk";
    options.compiler_executable = "clang++";
    options.platform = gdpp::NativePlatform::macos;
    options.architecture = "arm64";

    const auto plan = gdpp::NativeBuilder{}.plan(options);

    REQUIRE(plan.success);
    REQUIRE(contains_path(plan.commands.back().arguments,
                          root / "sdk/lib/libgodot-cpp.macos.template_release.arm64.a"));
    REQUIRE(!contains_path(plan.commands.back().arguments,
                           root / "sdk/lib/libgodot-cpp.macos.template_release.universal.a"));
}
