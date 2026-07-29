#include "gdpp/project/export_worker_snapshot.hpp"

#include "gdpp/support/path_utf8.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <unordered_set>

namespace gdpp {
namespace {

bool starts_with(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    auto path_part = path.begin();
    for (auto prefix_part = prefix.begin(); prefix_part != prefix.end(); ++prefix_part) {
        if (path_part == path.end() || *path_part != *prefix_part)
            return false;
        ++path_part;
    }
    return true;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool is_platform_metadata(const std::filesystem::path& relative) {
    for (const auto& component : relative) {
        const auto name = path_to_utf8(component.filename());
        if (name == ".DS_Store" || name == "__MACOSX" || name.rfind("._", 0) == 0)
            return true;
    }
    return false;
}

bool is_editor_temp_library(const std::filesystem::path& path) {
    const auto name = path_to_utf8(path.filename());
    if (name.empty() || name.front() != '~')
        return false;
    const auto extension = lowercase(path_to_utf8(path.extension()));
    return extension == ".dll" || extension == ".pdb" || extension == ".so" ||
           extension == ".dylib";
}

bool is_reserved_directory(const std::filesystem::path& relative,
                           const std::filesystem::path& source) {
    const auto name = path_to_utf8(relative.filename());
    if (name == ".git" || name == ".hg" || name == ".svn" || name == "__MACOSX")
        return true;
    const std::filesystem::path gdpp_root{"addons/gdpp"};
    if (starts_with(relative, gdpp_root / "build") || starts_with(relative, gdpp_root / "sdk"))
        return true;
    const bool engine_cache = starts_with(relative, std::filesystem::path{".godot"});
    std::error_code error;
    return !engine_cache && std::filesystem::is_regular_file(source / ".gdignore", error);
}

std::string path_error(const std::string& operation, const std::filesystem::path& path,
                       const std::error_code& error) {
    return operation + " '" + path_to_utf8(path) + "': " + error.message();
}

class SnapshotBuilder final {
  public:
    SnapshotBuilder(std::filesystem::path source_root, std::filesystem::path destination_root)
        : source_root_(std::move(source_root)), destination_root_(std::move(destination_root)) {}

    ExportWorkerSnapshotResult run() {
        std::error_code error;
        std::filesystem::remove_all(destination_root_, error);
        if (error)
            return failure(
                path_error("cannot clear export-worker snapshot", destination_root_, error));
        std::filesystem::create_directories(destination_root_, error);
        if (error)
            return failure(
                path_error("cannot create export-worker snapshot", destination_root_, error));
        if (!copy_directory(source_root_, destination_root_, {})) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(destination_root_, cleanup_error);
            return failure(std::move(diagnostic_));
        }
        result_.success = true;
        return result_;
    }

  private:
    ExportWorkerSnapshotResult failure(std::string diagnostic) {
        result_.success = false;
        result_.diagnostic = std::move(diagnostic);
        return result_;
    }

    bool enter_directory(const std::filesystem::path& source) {
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(source, error);
        if (error) {
            diagnostic_ = path_error("cannot resolve project directory", source, error);
            return false;
        }
        const auto key = path_to_utf8(canonical);
        if (!active_directories_.insert(key).second) {
            diagnostic_ = "project directory symlink cycle reaches '" + path_to_utf8(source) + "'";
            return false;
        }
        return true;
    }

    void leave_directory(const std::filesystem::path& source) {
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(source, error);
        if (!error)
            active_directories_.erase(path_to_utf8(canonical));
    }

    bool copy_directory(const std::filesystem::path& source,
                        const std::filesystem::path& destination,
                        const std::filesystem::path& relative) {
        if (!relative.empty() && is_reserved_directory(relative, source))
            return true;
        if (!enter_directory(source))
            return false;

        std::error_code error;
        std::filesystem::create_directories(destination, error);
        if (error) {
            diagnostic_ = path_error("cannot create snapshot directory", destination, error);
            leave_directory(source);
            return false;
        }

        std::filesystem::directory_iterator iterator{source, error};
        if (error) {
            diagnostic_ = path_error("cannot enumerate project directory", source, error);
            leave_directory(source);
            return false;
        }
        const std::filesystem::directory_iterator end;
        while (iterator != end) {
            const auto entry_source = iterator->path();
            const auto name = entry_source.filename();
            const auto entry_relative = relative / name;
            const auto entry_destination = destination / name;
            if (!is_platform_metadata(entry_relative)) {
                const auto status = std::filesystem::status(entry_source, error);
                if (error) {
                    diagnostic_ = path_error("cannot inspect project entry", entry_source, error);
                    leave_directory(source);
                    return false;
                }
                if (std::filesystem::is_directory(status)) {
                    if (!copy_directory(entry_source, entry_destination, entry_relative)) {
                        leave_directory(source);
                        return false;
                    }
                } else if (std::filesystem::is_regular_file(status)) {
                    if (!is_editor_temp_library(entry_source) &&
                        !copy_file(entry_source, entry_destination)) {
                        leave_directory(source);
                        return false;
                    }
                } else {
                    diagnostic_ =
                        "unsupported project filesystem entry '" + path_to_utf8(entry_source) + "'";
                    leave_directory(source);
                    return false;
                }
            }
            iterator.increment(error);
            if (error) {
                diagnostic_ = path_error("cannot continue project enumeration", source, error);
                leave_directory(source);
                return false;
            }
        }
        leave_directory(source);
        return true;
    }

    bool copy_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
        std::error_code error;
        const auto size_before = std::filesystem::file_size(source, error);
        if (error) {
            diagnostic_ = path_error("cannot measure project file", source, error);
            return false;
        }
        const auto time_before = std::filesystem::last_write_time(source, error);
        if (error) {
            diagnostic_ = path_error("cannot inspect project file timestamp", source, error);
            return false;
        }
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            diagnostic_ =
                path_error("cannot copy project file into worker snapshot", source, error);
            return false;
        }
        const auto size_after = std::filesystem::file_size(source, error);
        if (error) {
            diagnostic_ = path_error("cannot remeasure project file", source, error);
            return false;
        }
        const auto time_after = std::filesystem::last_write_time(source, error);
        if (error) {
            diagnostic_ = path_error("cannot recheck project file timestamp", source, error);
            return false;
        }
        if (size_before != size_after || time_before != time_after) {
            diagnostic_ = "project file changed while creating export-worker snapshot: '" +
                          path_to_utf8(source) + "'";
            return false;
        }
        if (result_.file_count == std::numeric_limits<std::uint64_t>::max() ||
            size_before > std::numeric_limits<std::uint64_t>::max() - result_.byte_count) {
            diagnostic_ = "export-worker snapshot accounting overflowed";
            return false;
        }
        ++result_.file_count;
        result_.byte_count += static_cast<std::uint64_t>(size_before);
        return true;
    }

    std::filesystem::path source_root_;
    std::filesystem::path destination_root_;
    std::unordered_set<std::string> active_directories_;
    ExportWorkerSnapshotResult result_;
    std::string diagnostic_;
};

} // namespace

ExportWorkerSnapshotResult
create_export_worker_snapshot(const std::filesystem::path& project_root,
                              const std::filesystem::path& snapshot_root) {
    std::error_code error;
    const auto source = std::filesystem::absolute(project_root, error).lexically_normal();
    if (error)
        return {false, 0, 0, path_error("cannot resolve project root", project_root, error)};
    const auto destination = std::filesystem::absolute(snapshot_root, error).lexically_normal();
    if (error)
        return {false, 0, 0,
                path_error("cannot resolve export-worker snapshot", snapshot_root, error)};
    if (!std::filesystem::is_regular_file(source / "project.godot", error) || error)
        return {false, 0, 0, "export-worker snapshot source has no project.godot"};

    const auto destination_relative = destination.lexically_relative(source);
    const std::filesystem::path worker_root{"addons/gdpp/build/project/export-worker"};
    if (destination_relative.empty() || destination_relative.is_absolute() ||
        !starts_with(destination_relative, worker_root) || starts_with(source, destination) ||
        destination == source) {
        return {false, 0, 0,
                "export-worker snapshot must stay below addons/gdpp/build/project/export-worker"};
    }
    return SnapshotBuilder{source, destination}.run();
}

} // namespace gdpp
