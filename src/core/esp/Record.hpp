#pragma once

#include "esp/Signature.hpp"
#include "util/Binary.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fo4mx {

namespace RecordFlags {
inline constexpr uint32_t Master = 0x00000001;
inline constexpr uint32_t Deleted = 0x00000020;
inline constexpr uint32_t Localized = 0x00000080;
inline constexpr uint32_t Light = 0x00000200;
inline constexpr uint32_t Persistent = 0x00000400;
inline constexpr uint32_t Ignored = 0x00001000;
inline constexpr uint32_t Compressed = 0x00040000;
} // namespace RecordFlags

struct RecordHeader {
    static constexpr size_t kSize = 24;

    Signature signature;
    uint32_t dataSize = 0;
    uint32_t flags = 0;
    uint32_t formId = 0;
    uint32_t versionControl1 = 0;
    uint16_t formVersion = 0;
    uint16_t versionControl2 = 0;

    bool compressed() const { return (flags & RecordFlags::Compressed) != 0; }
    bool deleted() const { return (flags & RecordFlags::Deleted) != 0; }

    static RecordHeader read(BinaryReader& r);
    void write(BinaryWriter& w) const;
};

struct GroupHeader {
    static constexpr size_t kSize = 24;

    uint32_t groupSize = 0; // includes this header
    uint32_t label = 0;
    int32_t groupType = 0;
    uint16_t timestamp = 0;
    uint16_t versionControl = 0;
    uint32_t unknown = 0;

    static GroupHeader read(BinaryReader& r); // signature already consumed
    void write(BinaryWriter& w) const;
};

struct Subrecord {
    Signature signature;
    std::vector<uint8_t> data;

    Subrecord() = default;
    Subrecord(Signature s, std::vector<uint8_t> d) : signature(s), data(std::move(d)) {}

    std::string asZString() const;
    template <class T>
    std::optional<T> as(size_t offset = 0) const
    {
        if (offset + sizeof(T) > data.size())
            return std::nullopt;
        return loadLE<T>(data.data() + offset);
    }
    static Subrecord fromZString(Signature s, std::string_view text);
    template <class T>
    static Subrecord fromValue(Signature s, const T& v)
    {
        Subrecord sr;
        sr.signature = s;
        sr.data.resize(sizeof(T));
        storeLE<T>(sr.data.data(), v);
        return sr;
    }
};

class Record {
public:
    RecordHeader header;
    std::vector<Subrecord> subrecords;

    // rawData is the on-disk payload (compressed if the header says so).
    static Record parse(const RecordHeader& header, std::span<const uint8_t> rawData);
    static std::vector<uint8_t> decompress(const RecordHeader& header, std::span<const uint8_t> rawData);
    static std::vector<Subrecord> parseSubrecords(std::span<const uint8_t> data);

    std::vector<uint8_t> serializeSubrecords() const;
    // Writes header + payload. dataSize and the compression flag are recomputed.
    void write(BinaryWriter& w, bool compress = false) const;

    Signature signature() const { return header.signature; }
    const Subrecord* find(Signature s) const;
    Subrecord* find(Signature s);
    std::vector<const Subrecord*> findAll(Signature s) const;
    std::string editorId() const;
};

} // namespace fo4mx
