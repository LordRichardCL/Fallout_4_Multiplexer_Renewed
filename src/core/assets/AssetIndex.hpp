#pragma once

#include "ba2/Ba2Archive.hpp"
#include "esp/FormId.hpp"
#include "util/StringPool.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo4mx {

class VirtualFileSystem;

enum class AssetType : uint8_t {
    Texture,
    Mesh,
    Script,
    Material,
    Audio,
    Animation,
    Interface,
    FaceGen,
    Precombined,
    Previs,
    Strings,
    Other,
};
const char* assetTypeName(AssetType t);
AssetType classifyAsset(std::string_view normalizedPath);

inline constexpr uint32_t kLooseFile = 0xFFFFFFFFu;

struct ArchiveInfo {
    std::string name;
    std::string modName;            // mod providing the archive file
    PluginIndex owner = kInvalidPlugin; // plugin that loads it (kInvalidPlugin = INI list)
    uint32_t loadSequence = 0;
    std::unique_ptr<Ba2Archive> archive;
};

struct AssetSource {
    uint32_t archive = kLooseFile; // archive index, or kLooseFile
    uint32_t entry = 0;            // archive entry index, or VFS file index for loose files
    uint32_t slot = 0;             // VFS source slot for loose files
    uint32_t mod = 0;              // StringPool id of the providing mod
    uint32_t priority = 0;         // higher wins
    uint64_t size = 0;
};

struct AssetEntry {
    uint32_t path = 0; // StringPool id of the normalised path
    AssetType type = AssetType::Other;
    std::vector<AssetSource> sources; // ascending priority, last = winner

    const AssetSource& winner() const { return sources.back(); }
    bool conflicting() const { return sources.size() > 1; }
};

// Every asset the game can see, with all versions from loose files and BA2s.
class AssetIndex {
public:
    StringPool strings;
    std::vector<ArchiveInfo> archives;
    std::vector<AssetEntry> entries;

    AssetEntry& getOrAdd(const std::string& normalizedPath);
    const AssetEntry* find(std::string_view normalizedPath) const;
    const std::string& path(const AssetEntry& e) const { return strings.get(e.path); }
    const std::string& modName(const AssetSource& s) const { return strings.get(s.mod); }
    std::string sourceLabel(const AssetSource& s) const;

    // Reads the bytes of one version of an asset (maxBytes = 0 reads everything).
    std::vector<uint8_t> read(const AssetEntry& e, size_t sourceSlot, const VirtualFileSystem& vfs,
                              size_t maxBytes = 0) const;

    size_t conflictCount() const;
    size_t looseFileCount() const;
    void clear();

private:
    std::unordered_map<std::string, uint32_t> byPath_;
};

} // namespace fo4mx
