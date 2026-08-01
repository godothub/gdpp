#include "support/test.hpp"

#include "gdpp/project/export_worker_snapshot.hpp"
#include "gdpp/core/path_utf8.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {

void write_snapshot_file(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << value;
}

std::string read_snapshot_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::filesystem::path snapshot_fixture(const std::string& name) {
    return std::filesystem::path{GDPP_TEST_BINARY_DIR} / "test-fixtures" / name;
}

} // namespace

TEST_CASE("export worker snapshot physically isolates project and extension files") {
    const auto root = snapshot_fixture("export-worker-snapshot");
    const auto snapshot = root / "addons/gdpp/build/project/export-worker/transaction/project";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_snapshot_file(root / "project.godot", "[application]\nconfig/name=\"snapshot\"\n");
    write_snapshot_file(root / "scenes/main.tscn", "[gd_scene]\n");
    write_snapshot_file(root / "addons/vendor/vendor.gdextension", "[libraries]\n");
    write_snapshot_file(root / "addons/vendor/bin/vendor.dll", "native-library");
    write_snapshot_file(root / "addons/vendor/bin/~vendor.dll", "locked-editor-copy");
    write_snapshot_file(root / ".godot/.gdignore", "");
    write_snapshot_file(root / ".godot/extension_list.cfg",
                        "res://addons/vendor/vendor.gdextension\n");
    write_snapshot_file(root / ".godot/global_script_class_cache.cfg", "list=[]\n");
    write_snapshot_file(root / ".godot/uid_cache.bin", "uid-cache");
    write_snapshot_file(root / ".godot/imported/texture.ctex", "imported-texture");
    write_snapshot_file(root / ".godot/mono/temp/bin/provider.dll", "managed-provider");
    write_snapshot_file(root / ".godot/editor/filesystem_cache10", "volatile-editor-cache");
    write_snapshot_file(root / ".godot/exported/123/main.scn", "volatile-export-cache");
    write_snapshot_file(root / ".godot/shader_cache/cache.bin", "volatile-shader-cache");
    write_snapshot_file(root / ".git/config", "repository");
    write_snapshot_file(root / ".hidden/private.txt", "hidden");
    write_snapshot_file(root / "generated/.gdignore", "");
    write_snapshot_file(root / "generated/private.txt", "ignored");
    write_snapshot_file(root / "nested/project.godot", "[application]\n");
    write_snapshot_file(root / "nested/private.txt", "nested-project");
    write_snapshot_file(root / "addons/gdpp/sdk/.gdignore", "");
    write_snapshot_file(root / "addons/gdpp/sdk/large.lib", "package-cache");
    write_snapshot_file(root / "addons/gdpp/build/stale.bin", "compiler-cache");

    const auto result = gdpp::create_export_worker_snapshot(root, snapshot);

    REQUIRE(result.success);
    REQUIRE(result.file_count >= std::uint64_t{5});
    REQUIRE(std::filesystem::is_regular_file(snapshot / "project.godot"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / "scenes/main.tscn"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / "addons/vendor/vendor.gdextension"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / "addons/vendor/bin/vendor.dll"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/extension_list.cfg"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/global_script_class_cache.cfg"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/uid_cache.bin"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/imported/texture.ctex"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/mono/temp/bin/provider.dll"));
    REQUIRE(!std::filesystem::exists(snapshot / ".godot/editor"));
    REQUIRE(!std::filesystem::exists(snapshot / ".godot/exported"));
    REQUIRE(!std::filesystem::exists(snapshot / ".godot/shader_cache"));
    REQUIRE(!std::filesystem::exists(snapshot / "addons/vendor/bin/~vendor.dll"));
    REQUIRE(!std::filesystem::exists(snapshot / ".git"));
    REQUIRE(!std::filesystem::exists(snapshot / ".hidden"));
    REQUIRE(!std::filesystem::exists(snapshot / "generated"));
    REQUIRE(!std::filesystem::exists(snapshot / "nested"));
    REQUIRE(!std::filesystem::exists(snapshot / "addons/gdpp/sdk"));
    REQUIRE(!std::filesystem::exists(snapshot / "addons/gdpp/build"));

    write_snapshot_file(snapshot / "addons/vendor/bin/vendor.dll", "worker-change");
    REQUIRE_EQ(read_snapshot_file(root / "addons/vendor/bin/vendor.dll"),
               std::string{"native-library"});
    std::filesystem::remove_all(root, error);
}

TEST_CASE("export worker snapshot ignores concurrently changing editor caches") {
    const auto root = snapshot_fixture("export-worker-volatile-editor-cache");
    const auto snapshot = root / "addons/gdpp/build/project/export-worker/transaction/project";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_snapshot_file(root / "project.godot", "[application]\nconfig/name=\"snapshot\"\n");
    write_snapshot_file(root / "scenes/main.tscn", "[gd_scene]\n");
    write_snapshot_file(root / ".godot/.gdignore", "");
    write_snapshot_file(root / ".godot/extension_list.cfg", "");
    write_snapshot_file(root / ".godot/editor/volatile.cfg", "initial");

    std::atomic_bool stop{false};
    std::thread mutator{[&] {
        std::uint64_t revision = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            write_snapshot_file(root / ".godot/editor/volatile.cfg",
                                "revision-" + std::to_string(revision++));
            std::filesystem::remove(root / ".godot/editor/volatile.cfg", error);
        }
    }};

    bool successful = true;
    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
        const auto result = gdpp::create_export_worker_snapshot(root, snapshot);
        successful = successful && result.success;
        successful = successful && !std::filesystem::exists(snapshot / ".godot/editor");
    }
    stop.store(true, std::memory_order_relaxed);
    mutator.join();

    REQUIRE(successful);
    REQUIRE(std::filesystem::is_regular_file(snapshot / "project.godot"));
    REQUIRE(std::filesystem::is_regular_file(snapshot / ".godot/extension_list.cfg"));
    std::filesystem::remove_all(root, error);
}

TEST_CASE(
    "export worker snapshot preserves UTF-8 customer paths independent of the ANSI code page") {
    const auto root =
        snapshot_fixture("export-worker-unicode") / gdpp::path_from_utf8("客户项目-é");
    const auto snapshot = root / "addons/gdpp/build/project/export-worker/transaction/project";
    const auto resource = gdpp::path_from_utf8("场景") / gdpp::path_from_utf8("角色-é.tscn");
    std::error_code error;
    std::filesystem::remove_all(root.parent_path(), error);
    write_snapshot_file(root / "project.godot", "[application]\nconfig/name=\"Unicode\"\n");
    write_snapshot_file(root / resource, "客户资源");

    const auto result = gdpp::create_export_worker_snapshot(root, snapshot);

    REQUIRE(result.success);
    REQUIRE(std::filesystem::is_regular_file(snapshot / resource));
    REQUIRE_EQ(read_snapshot_file(snapshot / resource), std::string{"客户资源"});
    auto temporary = snapshot / resource;
    temporary += gdpp::path_from_utf8(".gdpp-snapshot-copy");
    REQUIRE(!std::filesystem::exists(temporary));
    std::filesystem::remove_all(root.parent_path(), error);
}

TEST_CASE("export worker snapshot rejects destinations outside its ignored transaction root") {
    const auto root = snapshot_fixture("export-worker-snapshot-boundary");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    write_snapshot_file(root / "project.godot", "[application]\n");

    const auto outside =
        gdpp::create_export_worker_snapshot(root, root.parent_path() / "escaped-snapshot");
    const auto project_visible =
        gdpp::create_export_worker_snapshot(root, root / "worker-snapshot");

    REQUIRE(!outside.success);
    REQUIRE(!project_visible.success);
    REQUIRE(!std::filesystem::exists(root.parent_path() / "escaped-snapshot"));
    REQUIRE(!std::filesystem::exists(root / "worker-snapshot"));
    std::filesystem::remove_all(root, error);
}
