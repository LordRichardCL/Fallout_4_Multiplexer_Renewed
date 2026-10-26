#include "ba2/Ba2Archive.hpp"

#include "util/Binary.hpp"
#include "util/Compression.hpp"
#include "util/Text.hpp"

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
            r.skip(16); // name hash, extension, dir hash, flags
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
            r.skip(12); // name hash, extension, dir hash
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
    if (nameTable == 0 || nameTable >= fileSize)
        throw FormatError(pathToUtf8(path.filename()) + ": missing name table");
    std::vector<uint8_t> names(static_cast<size_t>(fileSize - nameTable));
    in.seekg(static_cast<std::streamoff>(nameTable));
    readExact(in, names.data(), names.size(), "name table");
    BinaryReader nr(names);
    for (auto& e : ar->entries_)
        e.path = normalizeAssetPath(nr.readWString());
    return ar;
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
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, bool compress)
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
        uint64_t h = fnv1a64(files[i].first.data(), files[i].first.size());
        w.write<uint32_t>(static_cast<uint32_t>(h));
        w.write<uint32_t>(0);
        w.write<uint32_t>(static_cast<uint32_t>(h >> 32));
        w.write<uint32_t>(0x00100100);
        w.write<uint64_t>(dataOffset);
        w.write<uint32_t>(compress ? static_cast<uint32_t>(payloads[i].size()) : 0u);
        w.write<uint32_t>(static_cast<uint32_t>(files[i].second.size()));
        w.write<uint32_t>(0xBAADF00D);
        dataOffset += payloads[i].size();
    }
    for (const auto& p : payloads)
        w.writeBytes(p);
    w.patch<uint64_t>(nameTablePos, w.size());
    for (const auto& [name, _] : files)
        w.writeWString(name);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(w.buffer().data()), static_cast<std::streamsize>(w.size()));
    if (!out)
        throw FormatError("cannot write " + pathToUtf8(path));
}

} // namespace fo4mx
