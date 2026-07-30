#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace gdpp {

struct ExportWorkerSnapshotResult {
    bool success{};
    std::uint64_t file_count{};
    std::uint64_t byte_count{};
    std::string diagnostic;
};

// Creates a physically independent project view for an editor subprocess. The snapshot stays
// below GDPP's ignored export-worker directory, excludes compiler/package and volatile editor
// caches, follows project symlinks into private copies, and never hard-links customer files.
[[nodiscard]] ExportWorkerSnapshotResult
create_export_worker_snapshot(const std::filesystem::path& project_root,
                              const std::filesystem::path& snapshot_root);

} // namespace gdpp
