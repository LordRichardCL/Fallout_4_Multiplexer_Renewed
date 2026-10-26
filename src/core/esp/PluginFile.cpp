#include "esp/PluginFile.hpp"

#include "util/Text.hpp"

#include <fstream>

namespace fo4mx {

namespace {

std::vector<uint8_t> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw FormatError("cannot open " + pathToUtf8(path));
    auto size = static_cast<size_t>(in.tellg());
    std::vector<uint8_t> bytes(size);
    in.seekg(0);
    if (size && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw FormatError("cannot read " + pathToUtf8(path));
    return bytes;
}

} // namespace

PluginFile PluginFile::load(const std::filesystem::path& path)
{
    try {
        return fromBytes(readWholeFile(path));
    } catch (const FormatError& e) {
        throw FormatError(pathToUtf8(path.filename()) + ": " + e.what());
    }
}

PluginFile PluginFile::fromBytes(std::vector<uint8_t> bytes)
{
    PluginFile f;
    f.bytes_ = std::move(bytes);
    f.parseHeader();
    return f;
}

void PluginFile::parseHeader()
{
    BinaryReader r(bytes_);
    auto h = RecordHeader::read(r);
    if (h.signature != sig::TES4)
        throw FormatError("not a Fallout 4 plugin (missing TES4 header)");
    auto payload = r.readBytes(h.dataSize);
    header_ = PluginHeader::fromRecord(Record::parse(h, payload));
    header_.record = h;
    firstGroupOffset_ = r.position();
}

void PluginFile::forEachRecord(const std::function<bool(const RecordLocation&)>& visitor) const
{
    struct Frame {
        uint64_t end;
        Signature top;
    };
    std::vector<Frame> stack;
    BinaryReader r(bytes_);
    r.seek(static_cast<size_t>(firstGroupOffset_));

    while (!r.atEnd()) {
        while (!stack.empty() && r.position() >= stack.back().end)
            stack.pop_back();
        if (r.atEnd())
            break;

        uint64_t offset = r.position();
        if (r.remaining() < RecordHeader::kSize)
            throw FormatError("truncated data at offset " + std::to_string(offset));

        Signature s(r.peek<uint32_t>());
        if (s == sig::GRUP) {
            r.skip(4);
            auto g = GroupHeader::read(r);
            if (g.groupSize < GroupHeader::kSize || offset + g.groupSize > bytes_.size())
                throw FormatError("invalid group size at offset " + std::to_string(offset));
            Signature top = stack.empty() ? Signature(g.label) : stack.back().top;
            stack.push_back({offset + g.groupSize, top});
            continue;
        }

        RecordLocation loc;
        loc.header = RecordHeader::read(r);
        loc.offset = offset;
        loc.topGroup = stack.empty() ? loc.header.signature : stack.back().top;
        loc.depth = static_cast<uint32_t>(stack.size());
        loc.rawData = r.readBytes(loc.header.dataSize);
        if (!visitor(loc))
            return;
    }
}

Record PluginFile::recordAt(uint64_t offset) const
{
    BinaryReader r(bytes_);
    r.seek(static_cast<size_t>(offset));
    auto h = RecordHeader::read(r);
    auto payload = r.readBytes(h.dataSize);
    Record rec = Record::parse(h, payload);
    return rec;
}

PluginHeader readPluginHeader(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FormatError("cannot open " + pathToUtf8(path));
    uint8_t raw[RecordHeader::kSize];
    if (!in.read(reinterpret_cast<char*>(raw), sizeof(raw)))
        throw FormatError(pathToUtf8(path.filename()) + ": file too small");
    BinaryReader r(raw, sizeof(raw));
    auto h = RecordHeader::read(r);
    if (h.signature != sig::TES4)
        throw FormatError(pathToUtf8(path.filename()) + ": missing TES4 header");
    if (h.dataSize > 64 * 1024 * 1024)
        throw FormatError(pathToUtf8(path.filename()) + ": implausible TES4 size");
    std::vector<uint8_t> payload(h.dataSize);
    if (h.dataSize && !in.read(reinterpret_cast<char*>(payload.data()), h.dataSize))
        throw FormatError(pathToUtf8(path.filename()) + ": truncated TES4 header");
    auto header = PluginHeader::fromRecord(Record::parse(h, payload));
    header.record = h;
    return header;
}

struct RecordReader::Handle {
    std::mutex mutex;
    std::ifstream stream;
};

std::shared_ptr<RecordReader::Handle> RecordReader::acquire(const std::filesystem::path& path)
{
    std::lock_guard lock(mutex_);
    std::string key = pathToUtf8(path);
    auto it = handles_.find(key);
    if (it != handles_.end()) {
        lru_.remove(path);
        lru_.push_front(path);
        return it->second;
    }
    auto handle = std::make_shared<Handle>();
    handle->stream.open(path, std::ios::binary);
    if (!handle->stream)
        throw FormatError("cannot open " + key);
    handles_[key] = handle;
    lru_.push_front(path);
    while (lru_.size() > maxOpen_) {
        handles_.erase(pathToUtf8(lru_.back()));
        lru_.pop_back();
    }
    return handle;
}

std::vector<uint8_t> RecordReader::readRaw(const std::filesystem::path& path, uint64_t offset, RecordHeader& headerOut)
{
    auto handle = acquire(path);
    std::lock_guard lock(handle->mutex);
    auto& in = handle->stream;
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    uint8_t raw[RecordHeader::kSize];
    if (!in.read(reinterpret_cast<char*>(raw), sizeof(raw)))
        throw FormatError("cannot read record header in " + pathToUtf8(path.filename()));
    BinaryReader r(raw, sizeof(raw));
    headerOut = RecordHeader::read(r);
    std::vector<uint8_t> payload(headerOut.dataSize);
    if (headerOut.dataSize && !in.read(reinterpret_cast<char*>(payload.data()), headerOut.dataSize))
        throw FormatError("cannot read record data in " + pathToUtf8(path.filename()));
    return payload;
}

Record RecordReader::read(const std::filesystem::path& path, uint64_t offset)
{
    RecordHeader h;
    auto payload = readRaw(path, offset, h);
    return Record::parse(h, payload);
}

} // namespace fo4mx
