#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace fo4mx {

// Inflate a zlib stream. Throws FormatError if the stream is corrupt or the
// decompressed size does not match expectedSize.
std::vector<uint8_t> zlibInflate(std::span<const uint8_t> compressed, size_t expectedSize);

std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> data, int level = 6);

} // namespace fo4mx
