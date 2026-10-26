#pragma once

#include "esp/Record.hpp"

#include <string>
#include <vector>

namespace fo4mx {

struct MasterEntry {
    std::string name;
    uint64_t size = 0;
};

// Decoded TES4 record.
struct PluginHeader {
    RecordHeader record;
    float version = 1.0f;
    int32_t numRecords = 0;
    uint32_t nextObjectId = kDefaultNextObjectId;
    std::string author;
    std::string description;
    std::vector<MasterEntry> masters;
    std::vector<Subrecord> extra; // ONAM, INTV, INCC and anything unknown, preserved

    static constexpr uint32_t kDefaultNextObjectId = 0x800;

    bool isMasterFlagged() const { return (record.flags & RecordFlags::Master) != 0; }
    bool isLightFlagged() const { return (record.flags & RecordFlags::Light) != 0; }
    bool isLocalized() const { return (record.flags & RecordFlags::Localized) != 0; }

    static PluginHeader fromRecord(const Record& rec);
    Record toRecord() const;
};

} // namespace fo4mx
