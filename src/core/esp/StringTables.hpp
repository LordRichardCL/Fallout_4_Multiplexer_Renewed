#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace fo4mx {

enum class StringFileKind : uint8_t { Strings, DLStrings, ILStrings };

// Localised string tables (.STRINGS / .DLSTRINGS / .ILSTRINGS) of one plugin.
class StringTables {
public:
    void load(StringFileKind kind, std::span<const uint8_t> bytes);

    // Looks up id in the preferred table, then falls back to the others.
    std::optional<std::string> lookup(uint32_t id, StringFileKind preferred) const;

    bool empty() const { return tables_[0].empty() && tables_[1].empty() && tables_[2].empty(); }
    size_t size() const { return tables_[0].size() + tables_[1].size() + tables_[2].size(); }

    static const char* extension(StringFileKind kind);

private:
    std::unordered_map<uint32_t, std::string> tables_[3];
};

} // namespace fo4mx
