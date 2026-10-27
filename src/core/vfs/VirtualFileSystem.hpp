#pragma once

#include "util/Progress.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// One folder that contributes loose files to the game's Data directory:
// the real Data folder, an MO2 mod, or MO2's overwrite folder.
struct FileSourceRoot {
    std::string name;
    std::filesystem::path root;
    int32_t priority = 0;
    bool baseGame = false;
};

// Unix seconds of a file's last write time.
int64_t toUnixTime(std::filesystem::file_time_type t);

// Merged view of the Data directory, mirroring how Mod Organizer 2's USVFS
// presents files to the game: a file from a higher priority source hides the
// same file from lower priority ones.
class VirtualFileSystem {
public:
    struct File {
        std::string relativePath;        // original case, '\' separated
        std::vector<uint32_t> sources;   // indices into roots(), ascending priority (last = winner)
        std::vector<uint64_t> sizes;     // parallel to sources
        std::vector<int64_t> mtimes;     // parallel to sources (Unix seconds)
    };

    // Roots must be added in ascending priority.
    uint32_t addRoot(FileSourceRoot root);
    void scan(ProgressSink& progress = nullProgress());
    // Adds a single file manually (used for tests and synthetic sources).
    void addFile(uint32_t root, const std::string& relativePath, uint64_t size, int64_t mtime = 0);

    const std::vector<FileSourceRoot>& roots() const { return roots_; }
    const std::vector<File>& files() const { return files_; }
    const File* find(std::string_view normalizedPath) const;
    std::filesystem::path physicalPath(const File& f, size_t sourceSlot) const;
    std::filesystem::path winnerPath(const File& f) const { return physicalPath(f, f.sources.size() - 1); }
    const std::string& winnerName(const File& f) const { return roots_[f.sources.back()].name; }

    // Files directly in Data (plugins, archives).
    std::vector<const File*> rootFiles() const;

private:
    std::vector<FileSourceRoot> roots_;
    std::vector<File> files_;
    std::unordered_map<std::string, uint32_t> index_; // normalised path -> files_ index
};

} // namespace fo4mx
