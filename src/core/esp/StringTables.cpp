#include "esp/StringTables.hpp"

#include "util/Binary.hpp"

namespace fo4mx {

const char* StringTables::extension(StringFileKind kind)
{
    switch (kind) {
    case StringFileKind::Strings: return "STRINGS";
    case StringFileKind::DLStrings: return "DLSTRINGS";
    case StringFileKind::ILStrings: return "ILSTRINGS";
    }
    return "STRINGS";
}

void StringTables::load(StringFileKind kind, std::span<const uint8_t> bytes)
{
    BinaryReader r(bytes);
    uint32_t count = r.read<uint32_t>();
    uint32_t dataSize = r.read<uint32_t>();
    size_t directoryEnd = 8 + static_cast<size_t>(count) * 8;
    if (directoryEnd > bytes.size() || directoryEnd + dataSize > bytes.size())
        throw FormatError("string table directory exceeds file size");
    const uint8_t* data = bytes.data() + directoryEnd;

    auto& table = tables_[static_cast<int>(kind)];
    table.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = r.read<uint32_t>();
        uint32_t offset = r.read<uint32_t>();
        if (offset >= dataSize)
            continue;
        const uint8_t* p = data + offset;
        size_t avail = dataSize - offset;
        if (kind == StringFileKind::Strings) {
            size_t len = 0;
            while (len < avail && p[len] != 0)
                ++len;
            table[id] = std::string(reinterpret_cast<const char*>(p), len);
        } else {
            if (avail < 4)
                continue;
            uint32_t len = loadLE<uint32_t>(p);
            if (len > avail - 4)
                continue;
            std::string s(reinterpret_cast<const char*>(p + 4), len);
            while (!s.empty() && s.back() == '\0')
                s.pop_back();
            table[id] = std::move(s);
        }
    }
}

std::optional<std::string> StringTables::lookup(uint32_t id, StringFileKind preferred) const
{
    if (id == 0)
        return std::string();
    const int first = static_cast<int>(preferred);
    for (int i = -1; i < 3; ++i) {
        int idx = i < 0 ? first : i;
        if (i == first)
            continue;
        auto it = tables_[idx].find(id);
        if (it != tables_[idx].end())
            return it->second;
    }
    return std::nullopt;
}

} // namespace fo4mx
