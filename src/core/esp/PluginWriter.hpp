#pragma once

#include "esp/GroupLayout.hpp"
#include "esp/PluginHeader.hpp"
#include "esp/Record.hpp"

#include <filesystem>
#include <vector>

namespace fo4mx {

// Writes a plugin: the TES4 header, then one top-level group per record signature in the game's
// order. Cells, worldspaces and dialogue topics get their nested groups, as xEdit writes them:
//   CELL group -> interior block -> sub-block -> CELL, then its children group
//   WRLD group -> WRLD, then its children group -> persistent CELL and its children,
//                 then exterior block -> sub-block -> CELL and its children
//   cell children group -> persistent group (8), temporary group (9)
//   DIAL group -> DIAL, then its topic children group (INFO, in the order added)
// Records in a group are ordered ROAD/LAND, CELL/PGRD/NAVM, then the rest, each by FormID.
// A record whose parent (cell, worldspace, topic) is not in the plugin cannot be placed: build()
// throws, nothing is written.
class PluginWriter {
public:
    void setHeader(PluginHeader header) { header_ = std::move(header); }
    PluginHeader& header() { return header_; }
    void addRecord(Record record, Placement placement = {});
    size_t recordCount() const { return records_.size(); }

    // Builds the file image. HEDR's record count (records and groups) and next object id are updated.
    std::vector<uint8_t> build(bool compressLargeRecords = false);
    void write(const std::filesystem::path& path, bool compressLargeRecords = false);

    // Canonical Fallout 4 top-level group order (as in Fallout4.esm / xEdit).
    static const std::vector<Signature>& topLevelOrder();

private:
    struct Entry {
        Record record;
        Placement placement;
    };
    PluginHeader header_;
    std::vector<Entry> records_;
};

} // namespace fo4mx
