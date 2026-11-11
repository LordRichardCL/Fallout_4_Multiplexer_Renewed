#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fo4mx {

// A previs file (vis\<plugin>\<cell FormID>.uvd): the Creation Kit's precomputed visibility for a
// cell (an interior, or a 3x3 cluster of exterior cells). The visibility data itself is an Umbra
// tome nobody has published a format for; what this reads is the file's table of object IDs, which
// the header points to. Each entry is either a reference's FormID, in the numbering of the plugin
// the file was made for, or a combined object (0xFD000000 + index), which is not a FormID.
// Known layout (checked against a mod's file and 1,413 vanilla files examined by others):
//   0x00 magic 0xD6000012, 0x08 file size, 0x40 ID count, 0x50 ID table offset.
struct PrevisFile {
    bool recognised = false;
    std::string problem; // why the layout was not recognised
    uint32_t idCount = 0;
    uint32_t idOffset = 0;
    std::vector<uint32_t> ids;

    static constexpr uint32_t kMagic = 0xD6000012;
    static constexpr uint32_t kCombinedObject = 0xFD; // top byte of a combined object's ID

    static bool isCombinedObject(uint32_t id) { return (id >> 24) == kCombinedObject; }
};

PrevisFile readPrevisFile(std::span<const uint8_t> bytes);

} // namespace fo4mx
