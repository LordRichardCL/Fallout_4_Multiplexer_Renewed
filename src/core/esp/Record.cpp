#include "esp/Record.hpp"

#include "util/Compression.hpp"

#include <limits>

namespace fo4mx {

RecordHeader RecordHeader::read(BinaryReader& r)
{
    RecordHeader h;
    h.signature = Signature(r.read<uint32_t>());
    h.dataSize = r.read<uint32_t>();
    h.flags = r.read<uint32_t>();
    h.formId = r.read<uint32_t>();
    h.versionControl1 = r.read<uint32_t>();
    h.formVersion = r.read<uint16_t>();
    h.versionControl2 = r.read<uint16_t>();
    return h;
}

void RecordHeader::write(BinaryWriter& w) const
{
    w.write(signature.value);
    w.write(dataSize);
    w.write(flags);
    w.write(formId);
    w.write(versionControl1);
    w.write(formVersion);
    w.write(versionControl2);
}

GroupHeader GroupHeader::read(BinaryReader& r)
{
    GroupHeader g;
    g.groupSize = r.read<uint32_t>();
    g.label = r.read<uint32_t>();
    g.groupType = r.read<int32_t>();
    g.timestamp = r.read<uint16_t>();
    g.versionControl = r.read<uint16_t>();
    g.unknown = r.read<uint32_t>();
    return g;
}

void GroupHeader::write(BinaryWriter& w) const
{
    w.write(sig::GRUP.value);
    w.write(groupSize);
    w.write(label);
    w.write(groupType);
    w.write(timestamp);
    w.write(versionControl);
    w.write(unknown);
}

std::string Subrecord::asZString() const
{
    size_t len = 0;
    while (len < data.size() && data[len] != 0)
        ++len;
    return std::string(reinterpret_cast<const char*>(data.data()), len);
}

Subrecord Subrecord::fromZString(Signature s, std::string_view text)
{
    Subrecord sr;
    sr.signature = s;
    sr.data.assign(text.begin(), text.end());
    sr.data.push_back(0);
    return sr;
}

std::vector<uint8_t> Record::decompress(const RecordHeader& header, std::span<const uint8_t> rawData)
{
    if (!header.compressed())
        return std::vector<uint8_t>(rawData.begin(), rawData.end());
    if (rawData.size() < 4)
        throw FormatError("compressed record " + header.signature.str() + " too small");
    uint32_t decompressedSize = loadLE<uint32_t>(rawData.data());
    return zlibInflate(rawData.subspan(4), decompressedSize);
}

std::vector<Subrecord> Record::parseSubrecords(std::span<const uint8_t> data)
{
    std::vector<Subrecord> out;
    BinaryReader r(data);
    uint32_t extendedSize = 0;
    bool haveExtended = false;
    while (!r.atEnd()) {
        if (r.remaining() < 6)
            throw FormatError("truncated subrecord header");
        Signature s(r.read<uint32_t>());
        uint32_t size = r.read<uint16_t>();
        if (s == sig::XXXX) {
            if (size != 4)
                throw FormatError("XXXX subrecord with unexpected size");
            extendedSize = r.read<uint32_t>();
            haveExtended = true;
            continue;
        }
        if (haveExtended) {
            size = extendedSize;
            haveExtended = false;
        }
        auto bytes = r.readBytes(size);
        out.emplace_back(s, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    return out;
}

Record Record::parse(const RecordHeader& header, std::span<const uint8_t> rawData)
{
    Record rec;
    rec.header = header;
    auto data = decompress(header, rawData);
    rec.subrecords = parseSubrecords(data);
    rec.header.flags &= ~RecordFlags::Compressed;
    rec.header.dataSize = 0; // recomputed on write
    return rec;
}

std::vector<uint8_t> Record::serializeSubrecords() const
{
    BinaryWriter w;
    for (const auto& sr : subrecords) {
        if (sr.data.size() > std::numeric_limits<uint16_t>::max()) {
            w.write(sig::XXXX.value);
            w.write<uint16_t>(4);
            w.write<uint32_t>(static_cast<uint32_t>(sr.data.size()));
            w.write(sr.signature.value);
            w.write<uint16_t>(0);
        } else {
            w.write(sr.signature.value);
            w.write<uint16_t>(static_cast<uint16_t>(sr.data.size()));
        }
        w.writeBytes(sr.data);
    }
    return w.take();
}

void Record::write(BinaryWriter& w, bool compress) const
{
    auto payload = serializeSubrecords();
    RecordHeader h = header;
    if (compress && !payload.empty()) {
        auto packed = zlibDeflate(payload);
        BinaryWriter body;
        body.write<uint32_t>(static_cast<uint32_t>(payload.size()));
        body.writeBytes(packed);
        payload = body.take();
        h.flags |= RecordFlags::Compressed;
    } else {
        h.flags &= ~RecordFlags::Compressed;
    }
    h.dataSize = static_cast<uint32_t>(payload.size());
    h.write(w);
    w.writeBytes(payload);
}

const Subrecord* Record::find(Signature s) const
{
    for (const auto& sr : subrecords)
        if (sr.signature == s)
            return &sr;
    return nullptr;
}

Subrecord* Record::find(Signature s)
{
    for (auto& sr : subrecords)
        if (sr.signature == s)
            return &sr;
    return nullptr;
}

std::vector<const Subrecord*> Record::findAll(Signature s) const
{
    std::vector<const Subrecord*> out;
    for (const auto& sr : subrecords)
        if (sr.signature == s)
            out.push_back(&sr);
    return out;
}

std::string Record::editorId() const
{
    const Subrecord* e = find(sig::EDID);
    return e ? e->asZString() : std::string();
}

} // namespace fo4mx
