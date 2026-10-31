#pragma once

#include "esp/GroupLayout.hpp"
#include "esp/PluginHeader.hpp"
#include "esp/Record.hpp"

#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// Position of one record inside a plugin file.
struct RecordLocation {
    RecordHeader header;
    uint64_t offset = 0;      // offset of the record header in the file
    Signature topGroup;       // label of the enclosing top-level group
    uint32_t depth = 0;       // group nesting depth (1 = directly in a top group)
    std::span<const uint8_t> rawData; // payload (only valid while the PluginFile is alive)
    int32_t groupType = -1;   // type of the enclosing group (GroupType), -1 outside any group
    uint32_t groupLabel = 0;
    uint32_t world = 0;       // enclosing worldspace (label of a World Children group), 0 if none
    uint32_t cell = 0;        // enclosing cell (label of a Cell Children group), 0 if none
    uint32_t topic = 0;       // enclosing dialogue topic (Topic Children group), 0 if none
    int32_t blockType = -1;   // enclosing cell block group (interior 2 or exterior 4), -1 if none
    uint32_t blockLabel = 0;
    uint32_t subBlockLabel = 0; // enclosing cell sub-block group (3 or 5)
    bool unusualGroup = false; // inside a group type this app does not write (anything above 9)

    // Where the record sits, as the writer takes it.
    Placement placement() const;
};

// A plugin loaded fully into memory for scanning.
class PluginFile {
public:
    static PluginFile load(const std::filesystem::path& path);
    static PluginFile fromBytes(std::vector<uint8_t> bytes);

    const PluginHeader& header() const { return header_; }
    std::span<const uint8_t> bytes() const { return bytes_; }

    // Visits every record (not groups) depth-first in file order. Return false to stop.
    void forEachRecord(const std::function<bool(const RecordLocation&)>& visitor) const;

    Record recordAt(uint64_t offset) const;

    // Visits every group header depth-first in file order (depth 0 = top-level group).
    void forEachGroup(const std::function<void(const GroupHeader&, uint32_t depth)>& visitor) const;

private:
    void parseHeader();

    std::vector<uint8_t> bytes_;
    PluginHeader header_;
    uint64_t firstGroupOffset_ = 0;
};

// Reads just the TES4 header without loading the whole file.
PluginHeader readPluginHeader(const std::filesystem::path& path);

// Random access to records in many plugin files, keeping a bounded set of
// file handles open. Thread-safe.
class RecordReader {
public:
    explicit RecordReader(size_t maxOpenFiles = 48) : maxOpen_(maxOpenFiles) {}

    Record read(const std::filesystem::path& path, uint64_t offset);
    std::vector<uint8_t> readRaw(const std::filesystem::path& path, uint64_t offset, RecordHeader& headerOut);

private:
    struct Handle;
    std::shared_ptr<Handle> acquire(const std::filesystem::path& path);

    size_t maxOpen_;
    std::mutex mutex_;
    std::list<std::filesystem::path> lru_;
    std::unordered_map<std::string, std::shared_ptr<Handle>> handles_;
};

} // namespace fo4mx
