#include "gdpp/project/xcframework_artifact.hpp"

#include "support/test.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void write_artifact_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output << content;
}

std::filesystem::path make_xcframework_fixture(const std::string& name,
                                               const std::string& payload = "library") {
    const auto root =
        std::filesystem::temp_directory_path() / "gdpp-xcframework-artifact-tests" / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto artifact = root / "native-direct/4.6/ios/arm64/release/xcframework-staging/"
                                 "libgdpp.release.ios.arm64.xcframework";
    write_artifact_file(artifact / "Info.plist", "plist");
    write_artifact_file(artifact / "ios-arm64/libgdpp.dylib", payload + "-device");
    write_artifact_file(artifact / "ios-arm64_x86_64-simulator/libgdpp.dylib",
                        payload + "-simulator");
    return artifact;
}

std::string read_artifact_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("XCFramework artifacts require metadata and both runtime libraries") {
    const auto artifact = make_xcframework_fixture("validation");
    REQUIRE(gdpp::is_complete_xcframework(artifact));

    std::filesystem::remove(artifact / "ios-arm64/libgdpp.dylib");
    REQUIRE(!gdpp::is_complete_xcframework(artifact));
}

TEST_CASE("XCFramework commits preserve the extension and replace content transactionally") {
    const auto pending = make_xcframework_fixture("transaction", "first");
    auto root = pending;
    while (root.filename() != "transaction")
        root = root.parent_path();
    const auto destination = root / "addons/gdpp/binary" / pending.filename();
    std::string diagnostic;

    REQUIRE(gdpp::commit_xcframework_artifact(pending, destination, diagnostic));
    REQUIRE(diagnostic.empty());
    REQUIRE(gdpp::is_complete_xcframework(destination));
    REQUIRE_EQ(read_artifact_file(destination / "ios-arm64/libgdpp.dylib"),
               std::string{"first-device"});
    REQUIRE(!std::filesystem::exists(pending.parent_path() / ".gdpp-xcframework-transaction"));

    REQUIRE(gdpp::commit_xcframework_artifact(pending, destination, diagnostic));
    write_artifact_file(pending / "ios-arm64/libgdpp.dylib", "second-device");
    write_artifact_file(pending / "ios-arm64_x86_64-simulator/libgdpp.dylib", "second-simulator");
    REQUIRE(gdpp::commit_xcframework_artifact(pending, destination, diagnostic));
    REQUIRE_EQ(read_artifact_file(destination / "ios-arm64/libgdpp.dylib"),
               std::string{"second-device"});
    REQUIRE_EQ(read_artifact_file(destination / "ios-arm64_x86_64-simulator/libgdpp.dylib"),
               std::string{"second-simulator"});
    REQUIRE(!std::filesystem::exists(pending.parent_path() / ".gdpp-xcframework-transaction"));
}

TEST_CASE("XCFramework commits reject paths outside the native staging contract") {
    const auto pending = make_xcframework_fixture("unsafe");
    auto root = pending;
    while (root.filename() != "unsafe")
        root = root.parent_path();
    const auto destination = root / "not-binary" / pending.filename();
    std::string diagnostic;

    REQUIRE(!gdpp::commit_xcframework_artifact(pending, destination, diagnostic));
    REQUIRE(!diagnostic.empty());
    REQUIRE(!std::filesystem::exists(destination));
}

TEST_CASE("XCFramework commits recover an interrupted destination replacement") {
    const auto pending = make_xcframework_fixture("recovery");
    auto root = pending;
    while (root.filename() != "recovery")
        root = root.parent_path();
    const auto destination = root / "addons/gdpp/binary" / pending.filename();
    const auto transaction_root = pending.parent_path() / ".gdpp-xcframework-transaction";
    const auto backup = transaction_root / "libgdpp.release.ios.arm64.previous.xcframework";
    std::filesystem::create_directories(transaction_root);
    std::filesystem::copy(pending, backup, std::filesystem::copy_options::recursive);
    std::string diagnostic{"stale diagnostic"};

    REQUIRE(gdpp::commit_xcframework_artifact(pending, destination, diagnostic));
    REQUIRE(diagnostic.empty());
    REQUIRE(gdpp::is_complete_xcframework(destination));
    REQUIRE(!std::filesystem::exists(transaction_root));
}
