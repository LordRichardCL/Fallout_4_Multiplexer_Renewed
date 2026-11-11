#include "esp/PrevisFile.hpp"

#include "util/Binary.hpp"

namespace fo4mx {

PrevisFile readPrevisFile(std::span<const uint8_t> b)
{
    PrevisFile f;
    constexpr size_t kHeaderSize = 0x150; // the section table starts here
    if (b.size() < kHeaderSize) {
        f.problem = "shorter than its header (" + std::to_string(b.size()) + " bytes)";
        return f;
    }
    if (loadLE<uint32_t>(b.data()) != PrevisFile::kMagic) {
        f.problem = "unknown magic number";
        return f;
    }
    if (loadLE<uint32_t>(b.data() + 0x08) != b.size()) {
        f.problem = "the size in its header is not its size";
        return f;
    }
    f.idCount = loadLE<uint32_t>(b.data() + 0x40);
    f.idOffset = loadLE<uint32_t>(b.data() + 0x50);
    if (f.idOffset < kHeaderSize || f.idOffset % 4 != 0 || f.idCount > (b.size() - f.idOffset) / 4) {
        f.problem = "its ID table (" + std::to_string(f.idCount) + " IDs at offset " + std::to_string(f.idOffset) +
                    ") does not fit in the file";
        return f;
    }
    f.ids.reserve(f.idCount);
    for (uint32_t i = 0; i < f.idCount; ++i)
        f.ids.push_back(loadLE<uint32_t>(b.data() + f.idOffset + 4 * i));
    f.recognised = true;
    return f;
}

} // namespace fo4mx
