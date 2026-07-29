#include "support/test.hpp"

#include "gdpp/project/export_worker_snapshot.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
    write_snapshot_file(root / ".git/config", "repository");
    write_snapshot_file(root / "generated/.gdignore", "");
    write_snapshot_file(root / "generated/private.txt", "ignored");
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
    REQUIRE(!std::filesystem::exists(snapshot / "addons/vendor/bin/~vendor.dll"));
    REQUIRE(!std::filesystem::exists(snapshot / ".git"));
    REQUIRE(!std::filesystem::exists(snapshot / "generated"));
    REQUIRE(!std::filesystem::exists(snapshot / "addons/gdpp/sdk"));
    REQUIRE(!std::filesystem::exists(snapshot / "addons/gdpp/build"));

    write_snapshot_file(snapshot / "addons/vendor/bin/vendor.dll", "worker-change");
    REQUIRE_EQ(read_snapshot_file(root / "addons/vendor/bin/vendor.dll"),
               std::string{"native-library"});
    std::filesystem::remove_all(root, error);
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
