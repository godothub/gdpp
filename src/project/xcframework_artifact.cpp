#include "gdpp/project/xcframework_artifact.hpp"

#include "gdpp/core/path_utf8.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>

namespace gdpp {
namespace {

bool has_path_component(const std::filesystem::path& path, std::string_view component) {
    return std::any_of(path.begin(), path.end(),
                       [&](const auto& item) { return item == path_from_utf8(component); });
}

bool equal_files(const std::filesystem::path& left, const std::filesystem::path& right,
                 std::error_code& error) {
    const auto left_size = std::filesystem::file_size(left, error);
    if (error)
        return false;
    const auto right_size = std::filesystem::file_size(right, error);
    if (error || left_size != right_size)
        return false;

    std::ifstream left_stream{left, std::ios::binary};
    std::ifstream right_stream{right, std::ios::binary};
    if (!left_stream || !right_stream) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    while (left_stream && right_stream) {
        left_stream.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right_stream.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        if (left_stream.gcount() != right_stream.gcount() ||
            !std::equal(left_buffer.begin(), left_buffer.begin() + left_stream.gcount(),
                        right_buffer.begin()))
            return false;
    }
    if (left_stream.bad() || right_stream.bad()) {
        error = std::make_error_code(std::errc::io_error);
        return false;
    }
    return left_stream.eof() && right_stream.eof();
}

using ArtifactEntries = std::map<std::string, bool>;

bool collect_entries(const std::filesystem::path& root, ArtifactEntries& entries,
                     std::error_code& error) {
    for (std::filesystem::recursive_directory_iterator iterator{root, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        const auto relative = std::filesystem::relative(iterator->path(), root, error);
        if (error)
            return false;
        const bool is_directory = iterator->is_directory(error);
        if (error)
            return false;
        const bool is_regular_file = !is_directory && iterator->is_regular_file(error);
        if (error || (!is_directory && !is_regular_file))
            return false;
        entries.emplace(generic_path_to_utf8(relative), is_directory);
    }
    return !error;
}

bool equal_xcframeworks(const std::filesystem::path& left, const std::filesystem::path& right,
                        std::error_code& error) {
    ArtifactEntries left_entries;
    ArtifactEntries right_entries;
    if (!collect_entries(left, left_entries, error) ||
        !collect_entries(right, right_entries, error) || left_entries != right_entries)
        return false;
    for (const auto& [relative, is_directory] : left_entries) {
        if (!is_directory && !equal_files(left / relative, right / relative, error))
            return false;
    }
    return true;
}

std::filesystem::path transaction_artifact(const std::filesystem::path& root,
                                           const std::filesystem::path& destination,
                                           std::string_view state) {
    return root / path_from_utf8(path_to_utf8(destination.stem()) + "." + std::string{state} +
                                 path_to_utf8(destination.extension()));
}

void remove_empty_transaction_root(const std::filesystem::path& root) {
    std::error_code ignored;
    std::filesystem::remove(root, ignored);
}

} // namespace

bool is_complete_xcframework(const std::filesystem::path& artifact) {
    std::error_code error;
    if (artifact.extension() != ".xcframework" || std::filesystem::is_symlink(artifact, error) ||
        error || !std::filesystem::is_directory(artifact, error) || error ||
        std::filesystem::is_symlink(artifact / "Info.plist", error) || error ||
        !std::filesystem::is_regular_file(artifact / "Info.plist", error) || error ||
        std::filesystem::file_size(artifact / "Info.plist", error) == 0 || error)
        return false;

    std::size_t library_count = 0;
    for (std::filesystem::recursive_directory_iterator iterator{artifact, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_symlink(error) || error)
            return false;
        if (iterator->is_regular_file(error) && !error &&
            iterator->path().filename() == "libgdpp.dylib") {
            if (std::filesystem::file_size(iterator->path(), error) == 0 || error)
                return false;
            ++library_count;
        }
    }
    return !error && library_count >= 2;
}

bool commit_xcframework_artifact(const std::filesystem::path& pending,
                                 const std::filesystem::path& destination,
                                 std::string& diagnostic) {
    diagnostic.clear();
    const auto filename = path_to_utf8(destination.filename());
    if (!is_complete_xcframework(pending) || pending.filename() != destination.filename() ||
        filename.rfind("libgdpp.", 0) != 0 || destination.parent_path().filename() != "binary" ||
        !has_path_component(pending, "native-direct") ||
        !has_path_component(pending, "xcframework-staging")) {
        diagnostic = "refusing to commit an invalid or unsafe iOS XCFramework artifact";
        return false;
    }

    const auto transaction_root = pending.parent_path() / ".gdpp-xcframework-transaction";
    const auto backup = transaction_artifact(transaction_root, destination, "previous");
    const auto incoming = transaction_artifact(transaction_root, destination, "incoming");
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        diagnostic = "cannot create the iOS artifact destination directory: " + error.message();
        return false;
    }
    std::filesystem::create_directories(transaction_root, error);
    if (error) {
        diagnostic = "cannot create the iOS artifact transaction directory: " + error.message();
        return false;
    }

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

    if (had_destination && is_complete_xcframework(destination)) {
        if (equal_xcframeworks(pending, destination, error)) {
            remove_empty_transaction_root(transaction_root);
            return true;
        }
        if (error) {
            diagnostic = "cannot compare the current iOS artifact: " + error.message();
            return false;
        }
    }

    std::filesystem::remove_all(incoming, error);
    if (error) {
        diagnostic = "cannot clear the interrupted iOS incoming artifact: " + error.message();
        return false;
    }
    std::filesystem::copy(pending, incoming, std::filesystem::copy_options::recursive, error);
    if (error || !is_complete_xcframework(incoming)) {
        const auto copy_error = error ? error.message() : "copied artifact is incomplete";
        error.clear();
        std::filesystem::remove_all(incoming, error);
        remove_empty_transaction_root(transaction_root);
        diagnostic = "cannot stage the completed iOS artifact for commit: " + copy_error;
        return false;
    }
    if (had_destination) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            diagnostic =
                "cannot stage the current iOS artifact for replacement: " + error.message();
            return false;
        }
    }
    std::filesystem::rename(incoming, destination, error);
    if (error) {
        const auto commit_error = error.message();
        if (had_destination) {
            error.clear();
            std::filesystem::rename(backup, destination, error);
        }
        error.clear();
        std::filesystem::remove_all(incoming, error);
        remove_empty_transaction_root(transaction_root);
        diagnostic = "cannot commit the new iOS XCFramework: " + commit_error;
        return false;
    }
    std::filesystem::remove_all(backup, error);
    remove_empty_transaction_root(transaction_root);
    return true;
}

} // namespace gdpp
