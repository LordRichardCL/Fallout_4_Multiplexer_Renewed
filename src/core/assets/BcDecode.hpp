#pragma once

#include <cstdint>

namespace fo4mx::preview {

// One 16-byte BC7 block to 4x4 RGBA8 pixels (row by row). A reserved mode gives transparent black.
void decodeBc7Block(const uint8_t* block, uint8_t out[16][4]);

// One 16-byte BC6H block to 4x4 RGBA8 pixels for display: each half-float channel clamped to [0, 1] and
// scaled to 0..255 (linear), alpha 255. A reserved mode gives black.
void decodeBc6hBlock(const uint8_t* block, bool isSigned, uint8_t out[16][4]);

} // namespace fo4mx::preview
