#include "assets/AssetPreview.hpp"

#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cmath>

namespace fo4mx::preview {

namespace {

enum class Codec { None, BC1, BC2, BC3, BC4, BC5, RGBA8, BGRA8, BGRX8, R8 };

struct FormatDesc {
    Codec codec;
    const char* name;
};

FormatDesc describeDxgi(uint32_t f)
{
    switch (f) {
    case 28: return {Codec::RGBA8, "R8G8B8A8"};
    case 29: return {Codec::RGBA8, "R8G8B8A8 (sRGB)"};
    case 61: return {Codec::R8, "R8"};
    case 71: return {Codec::BC1, "BC1"};
    case 72: return {Codec::BC1, "BC1 (sRGB)"};
    case 74: return {Codec::BC2, "BC2"};
    case 75: return {Codec::BC2, "BC2 (sRGB)"};
    case 77: return {Codec::BC3, "BC3"};
    case 78: return {Codec::BC3, "BC3 (sRGB)"};
    case 80: return {Codec::BC4, "BC4"};
    case 81: return {Codec::BC4, "BC4 (signed)"};
    case 83: return {Codec::BC5, "BC5"};
    case 84: return {Codec::BC5, "BC5 (signed)"};
    case 87: return {Codec::BGRA8, "B8G8R8A8"};
    case 88: return {Codec::BGRX8, "B8G8R8X8"};
    case 91: return {Codec::BGRA8, "B8G8R8A8 (sRGB)"};
    case 95: return {Codec::None, "BC6H (unsigned)"};
    case 96: return {Codec::None, "BC6H (signed)"};
    case 98: return {Codec::None, "BC7"};
    case 99: return {Codec::None, "BC7 (sRGB)"};
    default: return {Codec::None, "DXGI format"};
    }
}

uint32_t fourCC(const char* s)
{
    return static_cast<uint32_t>(s[0]) | static_cast<uint32_t>(s[1]) << 8 | static_cast<uint32_t>(s[2]) << 16 |
           static_cast<uint32_t>(s[3]) << 24;
}

struct Parsed {
    TextureInfo info;
    Codec codec = Codec::None;
};

std::optional<Parsed> parse(const std::vector<uint8_t>& d)
{
    if (d.size() < 128 || loadLE<uint32_t>(d.data()) != 0x20534444)
        return std::nullopt;
    Parsed p;
    p.info.height = loadLE<uint32_t>(d.data() + 12);
    p.info.width = loadLE<uint32_t>(d.data() + 16);
    p.info.mipCount = std::max<uint32_t>(1, loadLE<uint32_t>(d.data() + 28));
    uint32_t pfFlags = loadLE<uint32_t>(d.data() + 80);
    uint32_t cc = loadLE<uint32_t>(d.data() + 84);
    uint32_t bits = loadLE<uint32_t>(d.data() + 88);
    uint32_t rMask = loadLE<uint32_t>(d.data() + 92);
    uint32_t caps2 = loadLE<uint32_t>(d.data() + 112);
    p.info.cubemap = (caps2 & 0x200) != 0;
    p.info.dataOffset = 128;

    if ((pfFlags & 0x4) && cc == fourCC("DX10")) {
        if (d.size() < 148)
            return std::nullopt;
        p.info.dxgiFormat = loadLE<uint32_t>(d.data() + 128);
        p.info.cubemap = p.info.cubemap || (loadLE<uint32_t>(d.data() + 136) & 0x4);
        p.info.dataOffset = 148;
        auto desc = describeDxgi(p.info.dxgiFormat);
        p.codec = desc.codec;
        p.info.format = desc.name;
        if (desc.codec == Codec::None && std::string(desc.name) == "DXGI format")
            p.info.format = "DXGI format " + std::to_string(p.info.dxgiFormat);
    } else if (pfFlags & 0x4) {
        if (cc == fourCC("DXT1"))
            p.codec = Codec::BC1, p.info.format = "BC1 (DXT1)";
        else if (cc == fourCC("DXT3"))
            p.codec = Codec::BC2, p.info.format = "BC2 (DXT3)";
        else if (cc == fourCC("DXT5"))
            p.codec = Codec::BC3, p.info.format = "BC3 (DXT5)";
        else if (cc == fourCC("ATI1") || cc == fourCC("BC4U"))
            p.codec = Codec::BC4, p.info.format = "BC4 (ATI1)";
        else if (cc == fourCC("ATI2") || cc == fourCC("BC5U"))
            p.codec = Codec::BC5, p.info.format = "BC5 (ATI2)";
        else {
            char s[5] = {static_cast<char>(cc), static_cast<char>(cc >> 8), static_cast<char>(cc >> 16),
                         static_cast<char>(cc >> 24), 0};
            p.info.format = std::string("FourCC ") + s;
        }
    } else if (bits == 32) {
        p.codec = rMask == 0x00FF0000 ? Codec::BGRA8 : Codec::RGBA8;
        p.info.format = rMask == 0x00FF0000 ? "B8G8R8A8 (legacy)" : "R8G8B8A8 (legacy)";
    } else if (bits == 8) {
        p.codec = Codec::R8;
        p.info.format = "L8 (legacy)";
    } else {
        p.info.format = "Uncompressed " + std::to_string(bits) + "-bit";
    }
    p.info.decodable = p.codec != Codec::None;
    return p;
}

void rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
}

void decodeColorBlock(const uint8_t* b, uint8_t px[16][4], bool forceFour)
{
    uint16_t c0 = loadLE<uint16_t>(b), c1 = loadLE<uint16_t>(b + 2);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || forceFour) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = static_cast<uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<uint8_t>((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    uint32_t idx = loadLE<uint32_t>(b + 4);
    for (int i = 0; i < 16; ++i) {
        const uint8_t* c = pal[(idx >> (2 * i)) & 3];
        std::copy(c, c + 4, px[i]);
    }
}

void decodeAlphaBlock(const uint8_t* b, uint8_t out[16])
{
    uint8_t a0 = b[0], a1 = b[1];
    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            pal[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i < 5; ++i)
            pal[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = pal[(bits >> (3 * i)) & 7];
}

} // namespace

std::optional<TextureInfo> readDdsInfo(const std::vector<uint8_t>& dds)
{
    auto p = parse(dds);
    if (!p)
        return std::nullopt;
    return p->info;
}

std::optional<Image> decodeDds(const std::vector<uint8_t>& dds, uint32_t maxDimension)
{
    auto p = parse(dds);
    if (!p || !p->info.decodable || p->info.width == 0 || p->info.height == 0)
        return std::nullopt;
    const uint32_t w = p->info.width, h = p->info.height;
    if (w > 16384 || h > 16384)
        return std::nullopt;
    const uint8_t* data = dds.data() + p->info.dataOffset;
    size_t avail = dds.size() - p->info.dataOffset;

    Image full;
    full.width = w;
    full.height = h;
    full.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
    auto put = [&](uint32_t x, uint32_t y, const uint8_t* c) {
        if (x < w && y < h)
            std::copy(c, c + 4, &full.rgba[(static_cast<size_t>(y) * w + x) * 4]);
    };

    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (p->codec) {
    case Codec::BC1:
    case Codec::BC2:
    case Codec::BC3:
    case Codec::BC4:
    case Codec::BC5: {
        size_t blockSize = (p->codec == Codec::BC1 || p->codec == Codec::BC4) ? 8 : 16;
        if (avail < static_cast<size_t>(bw) * bh * blockSize)
            return std::nullopt;
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t* b = data + (static_cast<size_t>(by) * bw + bx) * blockSize;
                uint8_t px[16][4];
                if (p->codec == Codec::BC1) {
                    decodeColorBlock(b, px, false);
                } else if (p->codec == Codec::BC2) {
                    decodeColorBlock(b + 8, px, true);
                    for (int i = 0; i < 16; ++i) {
                        uint8_t nib = (b[i / 2] >> ((i & 1) * 4)) & 0xF;
                        px[i][3] = static_cast<uint8_t>(nib * 17);
                    }
                } else if (p->codec == Codec::BC3) {
                    decodeColorBlock(b + 8, px, true);
                    uint8_t a[16];
                    decodeAlphaBlock(b, a);
                    for (int i = 0; i < 16; ++i)
                        px[i][3] = a[i];
                } else if (p->codec == Codec::BC4) {
                    uint8_t r[16];
                    decodeAlphaBlock(b, r);
                    for (int i = 0; i < 16; ++i)
                        px[i][0] = px[i][1] = px[i][2] = r[i], px[i][3] = 255;
                } else {
                    uint8_t r[16], g[16];
                    decodeAlphaBlock(b, r);
                    decodeAlphaBlock(b + 8, g);
                    for (int i = 0; i < 16; ++i) {
                        double x = r[i] / 127.5 - 1.0, y = g[i] / 127.5 - 1.0;
                        double z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
                        px[i][0] = r[i];
                        px[i][1] = g[i];
                        px[i][2] = static_cast<uint8_t>(std::clamp((z + 1.0) * 127.5, 0.0, 255.0));
                        px[i][3] = 255;
                    }
                }
                for (int i = 0; i < 16; ++i)
                    put(bx * 4 + (i % 4), by * 4 + (i / 4), px[i]);
            }
        break;
    }
    case Codec::RGBA8:
    case Codec::BGRA8:
    case Codec::BGRX8: {
        if (avail < static_cast<size_t>(w) * h * 4)
            return std::nullopt;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const uint8_t* s = data + i * 4;
            uint8_t c[4];
            if (p->codec == Codec::RGBA8)
                c[0] = s[0], c[1] = s[1], c[2] = s[2], c[3] = s[3];
            else
                c[0] = s[2], c[1] = s[1], c[2] = s[0], c[3] = p->codec == Codec::BGRX8 ? 255 : s[3];
            std::copy(c, c + 4, &full.rgba[i * 4]);
        }
        break;
    }
    case Codec::R8: {
        if (avail < static_cast<size_t>(w) * h)
            return std::nullopt;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            uint8_t c[4] = {data[i], data[i], data[i], 255};
            std::copy(c, c + 4, &full.rgba[i * 4]);
        }
        break;
    }
    case Codec::None: return std::nullopt;
    }

    // Box-filter down to maxDimension for display.
    uint32_t scale = 1;
    while (w / scale > maxDimension || h / scale > maxDimension)
        scale *= 2;
    if (scale == 1)
        return full;
    Image out;
    out.width = std::max<uint32_t>(1, w / scale);
    out.height = std::max<uint32_t>(1, h / scale);
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y)
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (uint32_t dy = 0; dy < scale; ++dy)
                for (uint32_t dx = 0; dx < scale; ++dx) {
                    uint32_t sx = x * scale + dx, sy = y * scale + dy;
                    if (sx >= w || sy >= h)
                        continue;
                    const uint8_t* s = &full.rgba[(static_cast<size_t>(sy) * w + sx) * 4];
                    for (int c = 0; c < 4; ++c)
                        acc[c] += s[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c)
                out.rgba[(static_cast<size_t>(y) * out.width + x) * 4 + c] = static_cast<uint8_t>(acc[c] / std::max(1u, n));
        }
    return out;
}

std::optional<MeshInfo> readNifInfo(const std::vector<uint8_t>& nif)
{
    auto nl = std::find(nif.begin(), nif.begin() + static_cast<long>(std::min<size_t>(nif.size(), 128)), '\n');
    if (nif.size() < 40 || nl == nif.end())
        return std::nullopt;
    MeshInfo m;
    m.headerLine.assign(nif.begin(), nl);
    if (m.headerLine.find("Gamebryo") == std::string::npos && m.headerLine.find("NetImmerse") == std::string::npos)
        return std::nullopt;
    try {
        BinaryReader r(nif);
        r.seek(static_cast<size_t>(nl - nif.begin()) + 1);
        m.version = r.read<uint32_t>();
        r.skip(1); // endian
        m.userVersion = r.read<uint32_t>();
        m.blockCount = r.read<uint32_t>();
        m.bsVersion = r.read<uint32_t>();
        r.readBString(); // author
        if (m.bsVersion > 130)
            r.skip(4);
        if (m.bsVersion < 131)
            r.readBString(); // process script
        r.readBString();     // export script
        if (m.bsVersion >= 103)
            r.readBString(); // max filepath
        uint16_t typeCount = r.read<uint16_t>();
        std::vector<std::string> types(typeCount);
        for (auto& t : types) {
            uint32_t len = r.read<uint32_t>();
            if (len > 256)
                throw FormatError("bad block type name");
            t = r.readFixedString(len);
        }
        for (uint32_t i = 0; i < m.blockCount; ++i) {
            uint16_t ti = r.read<uint16_t>() & 0x7FFF;
            if (ti < types.size())
                m.blockTypes[types[ti]]++;
        }
        r.skip(static_cast<size_t>(m.blockCount) * 4); // block sizes
        uint32_t stringCount = r.read<uint32_t>();
        r.skip(4); // max length
        for (uint32_t i = 0; i < stringCount && i < 4096; ++i) {
            uint32_t len = r.read<uint32_t>();
            if (len > 1024)
                break;
            m.strings.push_back(r.readFixedString(len));
        }
        m.complete = true;
    } catch (const FormatError&) {
        // partial information is still useful
    }
    return m;
}

std::optional<MaterialInfo> readMaterialInfo(const std::vector<uint8_t>& d)
{
    if (d.size() < 8)
        return std::nullopt;
    MaterialInfo m;
    m.kind.assign(reinterpret_cast<const char*>(d.data()), 4);
    if (m.kind != "BGSM" && m.kind != "BGEM")
        return std::nullopt;
    m.version = loadLE<uint32_t>(d.data() + 4);
    // Texture slots are length-prefixed, null-terminated strings.
    for (size_t off = 8; off + 4 < d.size(); ++off) {
        uint32_t len = loadLE<uint32_t>(d.data() + off);
        if (len < 2 || len > 260 || off + 4 + len > d.size())
            continue;
        const uint8_t* s = d.data() + off + 4;
        if (s[len - 1] != 0)
            continue;
        bool ok = true;
        for (uint32_t i = 0; i + 1 < len; ++i)
            if (s[i] < 0x20 || s[i] > 0x7E) {
                ok = false;
                break;
            }
        if (!ok)
            continue;
        std::string str(reinterpret_cast<const char*>(s), len - 1);
        if (iendsWith(str, ".dds") || iendsWith(str, ".bgsm") || str.find('\\') != std::string::npos ||
            str.find('/') != std::string::npos) {
            m.textures.push_back(str);
            off += 3 + len;
        }
    }
    return m;
}

} // namespace fo4mx::preview
