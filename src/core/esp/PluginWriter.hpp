#pragma once

#include "esp/PluginHeader.hpp"
#include "esp/Record.hpp"

#include <filesystem>
#include <vector>

namespace fo4mx {

// Writes a flat plugin: TES4 header followed by one top-level group per
// record signature. Only top-level record types (no CELL/WRLD children) are
// supported, which covers every record type the compiler emits.
class PluginWriter {
public:
    void setHeader(PluginHeader header) { header_ = std::move(header); }
    PluginHeader& header() { return header_; }
    void addRecord(Record record) { records_.push_back(std::move(record)); }
    size_t recordCount() const { return records_.size(); }

    // Builds the file image. HEDR's record count and next object id are updated.
    std::vector<uint8_t> build(bool compressLargeRecords = false);
    void write(const std::filesystem::path& path, bool compressLargeRecords = false);

    // Canonical Fallout 4 top-level group order (as in Fallout4.esm / xEdit).
    static const std::vector<Signature>& topLevelOrder();

private:
    PluginHeader header_;
    std::vector<Record> records_;
};

} // namespace fo4mx
