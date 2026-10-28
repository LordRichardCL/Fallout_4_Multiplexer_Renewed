#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace fo4mx {

// Inflate a zlib stream. Throws FormatError if the stream is corrupt or the
// decompressed size does not match expectedSize.
std::vector<uint8_t> zlibInflate(std::span<const uint8_t> compressed, size_t expectedSize);

// Inflate at most the first maxBytes of a zlib stream (fewer if the stream is shorter). Throws
// FormatError if that part of the stream is corrupt.
std::vector<uint8_t> zlibInflatePrefix(std::span<const uint8_t> compressed, size_t maxBytes);

std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> data, int level = 6);

} // namespace fo4mx
