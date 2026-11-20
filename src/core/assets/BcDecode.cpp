// BC7 and BC6H block decoders for texture previews, written from the Direct3D 11 block compression
// specification (BC7 and BC6H formats). Checked against an independent decoder on random blocks of every
// mode (tests/test_formats.cpp holds a sample of the results).

#include "assets/BcDecode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace fo4mx::preview {

namespace {

// Bits of a 128-bit block, least significant first.
class Bits {
public:
    explicit Bits(const uint8_t* b) { std::memcpy(bytes_, b, 16); }
    uint32_t read(int count)
    {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i, ++pos_)
            v |= static_cast<uint32_t>((bytes_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
        return v;
    }
    int position() const { return pos_; }

private:
    uint8_t bytes_[16];
    int pos_ = 0;
};

const uint8_t kPartition2[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0}, {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1}, {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0},
    {0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0}, {0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1}, {0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1},
    {0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0},
    {0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0}, {0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1},
    {0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1},
    {0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0}, {0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0},
    {0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1}, {0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1},
    {0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0}, {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1},
};

const uint8_t kPartition3[64][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2},
    {0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2},
    {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2},
    {0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2}, {0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0},
    {0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2}, {0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2}, {0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2}, {0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0},
    {0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0}, {0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2},
    {0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1},
    {0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2}, {0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2},
    {0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1}, {0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1},
    {0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2}, {0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2}, {0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0}, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0},
    {0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0}, {0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1},
    {0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1}, {0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1}, {0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2},
    {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1}, {0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1}, {0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2}, {0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1},
    {0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2}, {0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2}, {0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2},
    {0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2},
    {0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2},
    {0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1}, {0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2},
    {0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, {0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0},
};

// Anchor pixel of subset 1 (two subsets), and of subsets 1 and 2 (three subsets).
const uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                              15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                              15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                              6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
const uint8_t kAnchor3a[64] = {3,  3,  15, 15, 8,  3,  15, 15, 8,  8,  6,  6,  6,  5,  3,  3,
                               3,  3,  8,  15, 3,  3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                               8,  15, 3,  5,  6,  10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                               3,  15, 5,  5,  5,  8,  5,  10, 5,  10, 8,  13, 15, 12, 3,  3};
const uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                               15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                               15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                               15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

const int kWeights2[4] = {0, 21, 43, 64};
const int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

const int* weightsFor(int bits)
{
    return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4;
}

bool isAnchor(int pixel, int subsets, int partition)
{
    if (pixel == 0)
        return true;
    if (subsets == 2)
        return pixel == kAnchor2[partition];
    if (subsets == 3)
        return pixel == kAnchor3a[partition] || pixel == kAnchor3b[partition];
    return false;
}

int subsetOf(int pixel, int subsets, int partition)
{
    return subsets == 2 ? kPartition2[partition][pixel] : subsets == 3 ? kPartition3[partition][pixel] : 0;
}

} // namespace

void decodeBc7Block(const uint8_t* block, uint8_t out[16][4])
{
    struct Mode {
        int subsets, partitionBits, rotationBits, indexSelectionBits, colorBits, alphaBits, endpointPBits,
            sharedPBits, indexBits, indexBits2;
    };
    static const Mode kModes[8] = {
        {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
        {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
        {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
    };
    Bits bits(block);
    int mode = 0;
    while (mode < 8 && bits.read(1) == 0)
        ++mode;
    if (mode >= 8) {
        std::memset(out, 0, 16 * 4);
        return;
    }
    const Mode& m = kModes[mode];
    const int partition = static_cast<int>(bits.read(m.partitionBits));
    const int rotation = static_cast<int>(bits.read(m.rotationBits));
    const int indexSelection = static_cast<int>(bits.read(m.indexSelectionBits));

    int endpoints[6][4] = {}; // [subset * 2 + end][channel]
    const int ends = m.subsets * 2;
    for (int c = 0; c < 3; ++c)
        for (int e = 0; e < ends; ++e)
            endpoints[e][c] = static_cast<int>(bits.read(m.colorBits));
    if (m.alphaBits)
        for (int e = 0; e < ends; ++e)
            endpoints[e][3] = static_cast<int>(bits.read(m.alphaBits));
    int pbits[6] = {};
    if (m.endpointPBits)
        for (int e = 0; e < ends; ++e)
            pbits[e] = static_cast<int>(bits.read(1));
    if (m.sharedPBits)
        for (int s = 0; s < m.subsets; ++s)
            pbits[s * 2] = pbits[s * 2 + 1] = static_cast<int>(bits.read(1));
    const bool hasP = m.endpointPBits || m.sharedPBits;
    for (int e = 0; e < ends; ++e)
        for (int c = 0; c < 4; ++c) {
            int width = c < 3 ? m.colorBits : m.alphaBits;
            if (width == 0) {
                endpoints[e][c] = 255;
                continue;
            }
            int v = endpoints[e][c];
            if (hasP) {
                v = (v << 1) | pbits[e];
                ++width;
            }
            v <<= 8 - width;
            endpoints[e][c] = v | (v >> width);
        }

    int index1[16], index2[16] = {};
    for (int i = 0; i < 16; ++i)
        index1[i] = static_cast<int>(bits.read(isAnchor(i, m.subsets, partition) ? m.indexBits - 1 : m.indexBits));
    if (m.indexBits2)
        for (int i = 0; i < 16; ++i)
            index2[i] = static_cast<int>(bits.read(i == 0 ? m.indexBits2 - 1 : m.indexBits2));

    for (int i = 0; i < 16; ++i) {
        const int s = subsetOf(i, m.subsets, partition);
        const int* e0 = endpoints[s * 2];
        const int* e1 = endpoints[s * 2 + 1];
        int colorIndex = index1[i], colorBits = m.indexBits;
        int alphaIndex = index1[i], alphaBits = m.indexBits;
        if (m.indexBits2) {
            alphaIndex = index2[i];
            alphaBits = m.indexBits2;
            if (indexSelection) {
                std::swap(colorIndex, alphaIndex);
                std::swap(colorBits, alphaBits);
            }
        }
        const int cw = weightsFor(colorBits)[colorIndex];
        const int aw = weightsFor(alphaBits)[alphaIndex];
        int px[4];
        for (int c = 0; c < 3; ++c)
            px[c] = ((64 - cw) * e0[c] + cw * e1[c] + 32) >> 6;
        px[3] = ((64 - aw) * e0[3] + aw * e1[3] + 32) >> 6;
        if (rotation)
            std::swap(px[3], px[rotation - 1]);
        for (int c = 0; c < 4; ++c)
            out[i][c] = static_cast<uint8_t>(px[c]);
    }
}

namespace {

// BC6H header layout: segments of fields in the order the bits come. Fields: 0..11 = r0 g0 b0 r1 g1 b1 r2
// g2 b2 r3 g3 b3 (endpoint 0..3, colour channel); `low` is the first bit of the field the segment fills;
// `reversed`: the first bit read is the segment's highest.
struct Segment {
    uint8_t field, low, count;
    bool reversed;
};
enum : uint8_t { R0, G0, B0, R1, G1, B1, R2, G2, B2, R3, G3, B3 };

struct Bc6Mode {
    bool transformed;
    int regions;
    int precision;
    int delta[3];
    std::vector<Segment> segments;
};

const Bc6Mode& bc6Mode(int index)
{
    static const Bc6Mode modes[14] = {
        {true, 2, 10, {5, 5, 5}, {{G2, 4, 1}, {B2, 4, 1}, {B3, 4, 1}, {R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 5},
                                  {G3, 4, 1}, {G2, 0, 4}, {G1, 0, 5}, {B3, 0, 1}, {G3, 0, 4}, {B1, 0, 5}, {B3, 1, 1},
                                  {B2, 0, 4}, {R2, 0, 5}, {B3, 2, 1}, {R3, 0, 5}, {B3, 3, 1}}},
        {true, 2, 7, {6, 6, 6}, {{G2, 5, 1}, {G3, 4, 1}, {G3, 5, 1}, {R0, 0, 7}, {B3, 0, 1}, {B3, 1, 1}, {B2, 4, 1},
                                 {G0, 0, 7}, {B2, 5, 1}, {B3, 2, 1}, {G2, 4, 1}, {B0, 0, 7}, {B3, 3, 1}, {B3, 5, 1},
                                 {B3, 4, 1}, {R1, 0, 6}, {G2, 0, 4}, {G1, 0, 6}, {G3, 0, 4}, {B1, 0, 6}, {B2, 0, 4},
                                 {R2, 0, 6}, {R3, 0, 6}}},
        {true, 2, 11, {5, 4, 4}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 5}, {R0, 10, 1}, {G2, 0, 4}, {G1, 0, 4},
                                  {G0, 10, 1}, {B3, 0, 1}, {G3, 0, 4}, {B1, 0, 4}, {B0, 10, 1}, {B3, 1, 1}, {B2, 0, 4},
                                  {R2, 0, 5}, {B3, 2, 1}, {R3, 0, 5}, {B3, 3, 1}}},
        {true, 2, 11, {4, 5, 4}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 4}, {R0, 10, 1}, {G3, 4, 1}, {G2, 0, 4},
                                  {G1, 0, 5}, {G0, 10, 1}, {G3, 0, 4}, {B1, 0, 4}, {B0, 10, 1}, {B3, 1, 1}, {B2, 0, 4},
                                  {R2, 0, 4}, {B3, 0, 1}, {B3, 2, 1}, {R3, 0, 4}, {G2, 4, 1}, {B3, 3, 1}}},
        {true, 2, 11, {4, 4, 5}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 4}, {R0, 10, 1}, {B2, 4, 1}, {G2, 0, 4},
                                  {G1, 0, 4}, {G0, 10, 1}, {B3, 0, 1}, {G3, 0, 4}, {B1, 0, 5}, {B0, 10, 1}, {B2, 0, 4},
                                  {R2, 0, 4}, {B3, 1, 1}, {B3, 2, 1}, {R3, 0, 4}, {B3, 4, 1}, {B3, 3, 1}}},
        {true, 2, 9, {5, 5, 5}, {{R0, 0, 9}, {B2, 4, 1}, {G0, 0, 9}, {G2, 4, 1}, {B0, 0, 9}, {B3, 4, 1}, {R1, 0, 5},
                                 {G3, 4, 1}, {G2, 0, 4}, {G1, 0, 5}, {B3, 0, 1}, {G3, 0, 4}, {B1, 0, 5}, {B3, 1, 1},
                                 {B2, 0, 4}, {R2, 0, 5}, {B3, 2, 1}, {R3, 0, 5}, {B3, 3, 1}}},
        {true, 2, 8, {6, 5, 5}, {{R0, 0, 8}, {G3, 4, 1}, {B2, 4, 1}, {G0, 0, 8}, {B3, 2, 1}, {G2, 4, 1}, {B0, 0, 8},
                                 {B3, 3, 1}, {B3, 4, 1}, {R1, 0, 6}, {G2, 0, 4}, {G1, 0, 5}, {B3, 0, 1}, {G3, 0, 4},
                                 {B1, 0, 5}, {B3, 1, 1}, {B2, 0, 4}, {R2, 0, 6}, {R3, 0, 6}}},
        {true, 2, 8, {5, 6, 5}, {{R0, 0, 8}, {B3, 0, 1}, {B2, 4, 1}, {G0, 0, 8}, {G2, 5, 1}, {G2, 4, 1}, {B0, 0, 8},
                                 {G3, 5, 1}, {B3, 4, 1}, {R1, 0, 5}, {G3, 4, 1}, {G2, 0, 4}, {G1, 0, 6}, {G3, 0, 4},
                                 {B1, 0, 5}, {B3, 1, 1}, {B2, 0, 4}, {R2, 0, 5}, {B3, 2, 1}, {R3, 0, 5}, {B3, 3, 1}}},
        {true, 2, 8, {5, 5, 6}, {{R0, 0, 8}, {B3, 1, 1}, {B2, 4, 1}, {G0, 0, 8}, {B2, 5, 1}, {G2, 4, 1}, {B0, 0, 8},
                                 {B3, 5, 1}, {B3, 4, 1}, {R1, 0, 5}, {G3, 4, 1}, {G2, 0, 4}, {G1, 0, 5}, {B3, 0, 1},
                                 {G3, 0, 4}, {B1, 0, 6}, {B2, 0, 4}, {R2, 0, 5}, {B3, 2, 1}, {R3, 0, 5}, {B3, 3, 1}}},
        {false, 2, 6, {6, 6, 6}, {{R0, 0, 6}, {G3, 4, 1}, {B3, 0, 1}, {B3, 1, 1}, {B2, 4, 1}, {G0, 0, 6}, {G2, 5, 1},
                                  {B2, 5, 1}, {B3, 2, 1}, {G2, 4, 1}, {B0, 0, 6}, {G3, 5, 1}, {B3, 3, 1}, {B3, 5, 1},
                                  {B3, 4, 1}, {R1, 0, 6}, {G2, 0, 4}, {G1, 0, 6}, {G3, 0, 4}, {B1, 0, 6}, {B2, 0, 4},
                                  {R2, 0, 6}, {R3, 0, 6}}},
        {false, 1, 10, {10, 10, 10}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 10}, {G1, 0, 10}, {B1, 0, 10}}},
        {true, 1, 11, {9, 9, 9}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 9}, {R0, 10, 1}, {G1, 0, 9}, {G0, 10, 1},
                                  {B1, 0, 9}, {B0, 10, 1}}},
        {true, 1, 12, {8, 8, 8}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 8}, {R0, 10, 2, true}, {G1, 0, 8},
                                  {G0, 10, 2, true}, {B1, 0, 8}, {B0, 10, 2, true}}},
        {true, 1, 16, {4, 4, 4}, {{R0, 0, 10}, {G0, 0, 10}, {B0, 0, 10}, {R1, 0, 4}, {R0, 10, 6, true}, {G1, 0, 4},
                                  {G0, 10, 6, true}, {B1, 0, 4}, {B0, 10, 6, true}}},
    };
    return modes[index];
}

int signExtend(int v, int bits)
{
    const int shift = 32 - bits;
    return static_cast<int>(static_cast<uint32_t>(v) << shift) >> shift;
}

int unquantize(int v, int bits, bool isSigned)
{
    if (!isSigned) {
        if (bits >= 15)
            return v;
        if (v == 0)
            return 0;
        if (v == (1 << bits) - 1)
            return 0xFFFF;
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16)
        return v;
    const bool negative = v < 0;
    int x = negative ? -v : v;
    if (x == 0)
        x = 0;
    else if (x >= (1 << (bits - 1)) - 1)
        x = 0x7FFF;
    else
        x = ((x << 15) + 0x4000) >> (bits - 1);
    return negative ? -x : x;
}

float halfToFloat(uint16_t h)
{
    const int sign = h >> 15, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? 0.0f : 65504.0f; // NaN shows as 0, infinity as the largest value
    else
        v = std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
    return sign ? -v : v;
}

} // namespace

void decodeBc6hBlock(const uint8_t* block, bool isSigned, uint8_t out[16][4])
{
    Bits bits(block);
    int index = -1;
    const uint32_t two = bits.read(2);
    if (two < 2) {
        index = static_cast<int>(two);
    } else {
        const uint32_t five = two | (bits.read(3) << 2);
        static const int kFive[32] = {-1, -1, 2,  10, -1, -1, 3,  11, -1, -1, 4, 12, -1, -1, 5, 13,
                                      -1, -1, 6,  -1, -1, -1, 7,  -1, -1, -1, 8, -1, -1, -1, 9, -1};
        index = kFive[five];
    }
    if (index < 0) {
        for (int i = 0; i < 16; ++i)
            out[i][0] = out[i][1] = out[i][2] = 0, out[i][3] = 255;
        return;
    }
    const Bc6Mode& m = bc6Mode(index);
    int fields[12] = {};
    for (const auto& s : m.segments) {
        const uint32_t v = bits.read(s.count);
        for (int b = 0; b < s.count; ++b) {
            const int bit = static_cast<int>((v >> b) & 1);
            const int to = s.reversed ? s.low + s.count - 1 - b : s.low + b;
            fields[s.field] |= bit << to;
        }
    }
    const int partition = m.regions == 2 ? static_cast<int>(bits.read(5)) : 0;
    const int ends = m.regions * 2;
    int e[4][3];
    for (int p = 0; p < ends; ++p)
        for (int c = 0; c < 3; ++c)
            e[p][c] = fields[p * 3 + c];
    for (int c = 0; c < 3; ++c) {
        if (isSigned)
            e[0][c] = signExtend(e[0][c], m.precision);
        for (int p = 1; p < ends; ++p) {
            if (m.transformed) {
                int v = (e[0][c] + signExtend(e[p][c], m.delta[c])) & ((1 << m.precision) - 1);
                e[p][c] = isSigned ? signExtend(v, m.precision) : v;
            } else if (isSigned) {
                e[p][c] = signExtend(e[p][c], m.precision);
            }
        }
        for (int p = 0; p < ends; ++p)
            e[p][c] = unquantize(e[p][c], m.precision, isSigned);
    }
    const int indexBits = m.regions == 2 ? 3 : 4;
    for (int i = 0; i < 16; ++i) {
        const bool anchor = i == 0 || (m.regions == 2 && i == kAnchor2[partition]);
        const int idx = static_cast<int>(bits.read(anchor ? indexBits - 1 : indexBits));
        const int region = m.regions == 2 ? kPartition2[partition][i] : 0;
        const int w = weightsFor(indexBits)[idx];
        for (int c = 0; c < 3; ++c) {
            const int a = e[region * 2][c], b = e[region * 2 + 1][c];
            int v = (a * (64 - w) + b * w + 32) >> 6;
            uint16_t h;
            if (isSigned)
                h = static_cast<uint16_t>(v < 0 ? (((-v) * 31) >> 5) | 0x8000 : (v * 31) >> 5);
            else
                h = static_cast<uint16_t>((v * 31) >> 6);
            const float f = std::clamp(halfToFloat(h), 0.0f, 1.0f);
            out[i][c] = static_cast<uint8_t>(f * 255.0f + 0.5f);
        }
        out[i][3] = 255;
    }
}

} // namespace fo4mx::preview
