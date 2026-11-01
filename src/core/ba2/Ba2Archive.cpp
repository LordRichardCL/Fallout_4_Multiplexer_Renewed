#include "ba2/Ba2Archive.hpp"

#include "util/Binary.hpp"
#include "util/Compression.hpp"
#include "util/Text.hpp"

#include <array>
#include <cstdio>
#include <fstream>

namespace fo4mx {

namespace {

constexpr uint32_t kMagic = 0x58445442;   // "BTDX"
constexpr uint32_t kGeneral = 0x4C524E47; // "GNRL"
constexpr uint32_t kTexture = 0x30315844; // "DX10"

void readExact(std::ifstream& in, void* dst, size_t size, const char* what)
{
    if (size && !in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size)))
        throw FormatError(std::string("BA2: cannot read ") + what);
}

std::vector<uint8_t> readBlock(std::ifstream& in, uint64_t offset, uint32_t packed, uint32_t unpacked)
{
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (packed == 0) {
        std::vector<uint8_t> raw(unpacked);
        readExact(in, raw.data(), raw.size(), "file data");
        return raw;
    }
    std::vector<uint8_t> comp(packed);
    readExact(in, comp.data(), comp.size(), "compressed data");
    return zlibInflate(comp, unpacked);
}

} // namespace

uint64_t Ba2Entry::size() const
{
    if (chunks.empty())
        return unpackedSize;
    uint64_t total = 148;
    for (const auto& c : chunks)
        total += c.unpackedSize;
    return total;
}

std::string Ba2Archive::name() const
{
    return pathToUtf8(path_.filename());
}

std::unique_ptr<Ba2Archive> Ba2Archive::open(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FormatError("cannot open archive " + pathToUtf8(path));

    auto ar = std::make_unique<Ba2Archive>();
    ar->path_ = path;

    uint8_t headerRaw[24];
    readExact(in, headerRaw, sizeof(headerRaw), "header");
    BinaryReader h(headerRaw, sizeof(headerRaw));
    if (h.read<uint32_t>() != kMagic)
        throw FormatError(pathToUtf8(path.filename()) + " is not a BA2 archive");
    ar->version_ = h.read<uint32_t>();
    uint32_t type = h.read<uint32_t>();
    uint32_t count = h.read<uint32_t>();
    uint64_t nameTable = h.read<uint64_t>();

    // Starfield-era versions 2/3 add header fields; Fallout 4 uses 1, 7 and 8.
    if (ar->version_ == 2 || ar->version_ == 3) {
        uint8_t extra[8];
        readExact(in, extra, ar->version_ == 3 ? 12 : 8, "extended header");
    }

    if (type == kGeneral)
        ar->type_ = Ba2Type::General;
    else if (type == kTexture)
        ar->type_ = Ba2Type::Texture;
    else
        throw FormatError(pathToUtf8(path.filename()) + ": unknown BA2 type");

    ar->entries_.resize(count);
    if (ar->type_ == Ba2Type::General) {
        std::vector<uint8_t> table(static_cast<size_t>(count) * 36);
        readExact(in, table.data(), table.size(), "file table");
        BinaryReader r(table);
        for (auto& e : ar->entries_) {
            e.nameHash = r.read<uint32_t>();
            e.extension = r.read<uint32_t>();
            e.dirHash = r.read<uint32_t>();
            r.skip(4); // flags
            e.offset = r.read<uint64_t>();
            e.packedSize = r.read<uint32_t>();
            e.unpackedSize = r.read<uint32_t>();
            r.skip(4); // 0xBAADF00D
        }
    } else {
        for (auto& e : ar->entries_) {
            uint8_t fixed[24];
            readExact(in, fixed, sizeof(fixed), "texture table");
            BinaryReader r(fixed, sizeof(fixed));
            e.nameHash = r.read<uint32_t>();
            e.extension = r.read<uint32_t>();
            e.dirHash = r.read<uint32_t>();
            r.skip(1);
            uint8_t chunkCount = r.read<uint8_t>();
            uint16_t chunkHeaderSize = r.read<uint16_t>();
            e.height = r.read<uint16_t>();
            e.width = r.read<uint16_t>();
            e.mipCount = r.read<uint8_t>();
            e.dxgiFormat = r.read<uint8_t>();
            e.cubemap = r.read<uint16_t>() == 2049;
            if (chunkHeaderSize != 24)
                throw FormatError("BA2: unexpected texture chunk header size");
            for (uint8_t c = 0; c < chunkCount; ++c) {
                uint8_t chunkRaw[24];
                readExact(in, chunkRaw, sizeof(chunkRaw), "texture chunk");
                BinaryReader cr(chunkRaw, sizeof(chunkRaw));
                Ba2TextureChunk chunk;
                chunk.offset = cr.read<uint64_t>();
                chunk.packedSize = cr.read<uint32_t>();
                chunk.unpackedSize = cr.read<uint32_t>();
                chunk.startMip = cr.read<uint16_t>();
                chunk.endMip = cr.read<uint16_t>();
                e.chunks.push_back(chunk);
            }
            if (!e.chunks.empty()) {
                e.offset = e.chunks.front().offset;
                e.unpackedSize = static_cast<uint32_t>(e.size());
            }
        }
    }

    // Name table.
    in.clear();
    in.seekg(0, std::ios::end);
    auto fileSize = static_cast<uint64_t>(in.tellg());
    if (nameTable == 0 || nameTable >= fileSize) {
        ar->hasNames_ = false; // written without names (Archive2 -nostrings): matched by hash
        return ar;
    }
    std::vector<uint8_t> names(static_cast<size_t>(fileSize - nameTable));
    in.seekg(static_cast<std::streamoff>(nameTable));
    readExact(in, names.data(), names.size(), "name table");
    BinaryReader nr(names);
    for (auto& e : ar->entries_)
        e.path = normalizeAssetPath(nr.readWString());
    return ar;
}

uint32_t bsCrc32(std::string_view text)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (char ch : text) {
        auto c = static_cast<unsigned char>(ch == '/' ? '\\' : ch);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        crc = (crc >> 8) ^ table[(crc ^ c) & 0xFF];
    }
    return crc;
}

Ba2Hash ba2Hash(std::string_view path)
{
    Ba2Hash h;
    const size_t slash = path.find_last_of("\\/");
    const std::string_view dir = slash == std::string_view::npos ? std::string_view() : path.substr(0, slash);
    std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::string_view ext;
    if (const size_t dot = file.find_last_of('.'); dot != std::string_view::npos) {
        ext = file.substr(dot + 1);
        file = file.substr(0, dot);
    }
    h.dirHash = bsCrc32(dir);
    h.nameHash = bsCrc32(file);
    for (size_t i = 0; i < 4 && i < ext.size(); ++i) {
        auto c = static_cast<unsigned char>(ext[i]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char>(c - 'A' + 'a');
        h.extension |= static_cast<uint32_t>(c) << (8 * i);
    }
    return h;
}

std::string unnamedBa2Path(const Ba2Entry& e)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08x\\%08x.", e.dirHash, e.nameHash);
    std::string out = buf;
    for (int i = 0; i < 4; ++i)
        if (const char c = static_cast<char>((e.extension >> (8 * i)) & 0xFF))
            out += c;
    return out;
}

std::vector<uint8_t> Ba2Archive::extract(size_t index) const
{
    const auto& e = entries_.at(index);
    std::lock_guard lock(mutex_);
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        throw FormatError("cannot open archive " + pathToUtf8(path_));
    if (type_ == Ba2Type::General)
        return readBlock(in, e.offset, e.packedSize, e.unpackedSize);

    std::vector<uint8_t> out = makeDdsHeader(e.width, e.height, e.mipCount, e.dxgiFormat, e.cubemap);
    for (const auto& c : e.chunks) {
        auto data = readBlock(in, c.offset, c.packedSize, c.unpackedSize);
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

std::vector<uint8_t> Ba2Archive::extractHead(size_t index, size_t maxBytes) const
{
    const auto& e = entries_.at(index);
    if (type_ != Ba2Type::General || e.packedSize != 0) {
        auto all = extract(index);
        if (all.size() > maxBytes)
            all.resize(maxBytes);
        return all;
    }
    std::lock_guard lock(mutex_);
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        throw FormatError("cannot open archive " + pathToUtf8(path_));
    return readBlock(in, e.offset, 0, static_cast<uint32_t>(std::min<uint64_t>(maxBytes, e.unpackedSize)));
}

std::vector<uint8_t> makeDdsHeader(uint32_t width, uint32_t height, uint32_t mipCount, uint32_t dxgiFormat, bool cubemap)
{
    BinaryWriter w;
    w.write<uint32_t>(0x20534444); // "DDS "
    w.write<uint32_t>(124);
    w.write<uint32_t>(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
    w.write<uint32_t>(height);
    w.write<uint32_t>(width);
    w.write<uint32_t>(0); // pitch / linear size
    w.write<uint32_t>(0); // depth
    w.write<uint32_t>(mipCount);
    w.writeZeros(11 * 4);
    // Pixel format: FourCC DX10, real format in the extension header.
    w.write<uint32_t>(32);
    w.write<uint32_t>(0x4);
    w.write<uint32_t>(0x30315844); // "DX10"
    w.writeZeros(5 * 4);
    uint32_t caps = 0x1000;
    if (mipCount > 1)
        caps |= 0x400000 | 0x8;
    if (cubemap)
        caps |= 0x8;
    w.write<uint32_t>(caps);
    w.write<uint32_t>(cubemap ? 0xFE00u : 0u);
    w.writeZeros(3 * 4);
    // DX10 extension
    w.write<uint32_t>(dxgiFormat);
    w.write<uint32_t>(3); // texture2D
    w.write<uint32_t>(cubemap ? 0x4u : 0u);
    w.write<uint32_t>(1);
    w.write<uint32_t>(0);
    return w.take();
}

void writeGeneralBa2(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, bool compress,
                     bool names)
{
    std::vector<std::vector<uint8_t>> payloads;
    for (const auto& [_, data] : files)
        payloads.push_back(compress ? zlibDeflate(data) : data);

    BinaryWriter w;
    w.write<uint32_t>(kMagic);
    w.write<uint32_t>(1);
    w.write<uint32_t>(kGeneral);
    w.write<uint32_t>(static_cast<uint32_t>(files.size()));
    size_t nameTablePos = w.size();
    w.write<uint64_t>(0);

    uint64_t dataOffset = 24 + files.size() * 36ull;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto h = ba2Hash(files[i].first);
        w.write<uint32_t>(h.nameHash);
        w.write<uint32_t>(h.extension);
        w.write<uint32_t>(h.dirHash);
        w.write<uint32_t>(0x00100100);
        w.write<uint64_t>(dataOffset);
        w.write<uint32_t>(compress ? static_cast<uint32_t>(payloads[i].size()) : 0u);
        w.write<uint32_t>(static_cast<uint32_t>(files[i].second.size()));
        w.write<uint32_t>(0xBAADF00D);
        dataOffset += payloads[i].size();
    }
    for (const auto& p : payloads)
        w.writeBytes(p);
    if (names) {
        w.patch<uint64_t>(nameTablePos, w.size());
        for (const auto& [name, _] : files)
            w.writeWString(name);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(w.buffer().data()), static_cast<std::streamsize>(w.size()));
    if (!out)
        throw FormatError("cannot write " + pathToUtf8(path));
}

} // namespace fo4mx
