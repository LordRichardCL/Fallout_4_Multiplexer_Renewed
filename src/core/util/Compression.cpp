#include "util/Compression.hpp"

#include "util/Binary.hpp"

#include <limits>
#include <string>
#include <zlib.h>

namespace fo4mx {

std::vector<uint8_t> zlibInflate(std::span<const uint8_t> compressed, size_t expectedSize)
{
    if (compressed.size() > std::numeric_limits<uInt>::max() || expectedSize > std::numeric_limits<uInt>::max())
        throw FormatError("compressed block too large");

    std::vector<uint8_t> out(expectedSize);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    if (inflateInit(&stream) != Z_OK)
        throw FormatError("inflateInit failed");
    int rc = inflate(&stream, Z_FINISH);
    size_t produced = stream.total_out;
    inflateEnd(&stream);

    if (rc != Z_STREAM_END)
        throw FormatError("zlib inflate failed (code " + std::to_string(rc) + ")");
    if (produced != expectedSize)
        throw FormatError("zlib size mismatch: expected " + std::to_string(expectedSize) + ", got " +
                          std::to_string(produced));
    return out;
}

std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> data, int level)
{
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> out(bound);
    int rc = compress2(out.data(), &bound, data.data(), static_cast<uLong>(data.size()), level);
    if (rc != Z_OK)
        throw FormatError("zlib deflate failed (code " + std::to_string(rc) + ")");
    out.resize(bound);
    return out;
}

} // namespace fo4mx
