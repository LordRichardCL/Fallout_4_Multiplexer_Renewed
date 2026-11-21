#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace fo4mx {

// The record a file found by its plugin's name belongs to, from the file name: exactly one token of 8 hex
// digits, between the start or '_' and '_' or '.', with load order byte 00 (the FormID without its load
// order index, as voice and FaceGen files write it), such as LOD billboards "<model>_0001A2B3.dds" and
// "0001A2B3_n.dds". Anything else (no token, several, another load order byte) is not recognised.
struct FileFormIdToken {
    size_t pos = 0;      // where the 8 digits start in the file name
    uint32_t objectId = 0;
};

inline std::optional<FileFormIdToken> fileFormIdToken(std::string_view name)
{
    std::optional<FileFormIdToken> found;
    const size_t dot = name.rfind('.');
    const size_t stemEnd = dot == std::string_view::npos ? name.size() : dot;
    for (size_t i = 0; i + 8 <= stemEnd; ++i) {
        if (i > 0 && name[i - 1] != '_')
            continue;
        const size_t end = i + 8;
        if (end < stemEnd && name[end] != '_' && name[end] != '.')
            continue;
        bool hex = true;
        for (size_t k = i; k < end && hex; ++k)
            hex = std::isxdigit(static_cast<unsigned char>(name[k])) != 0;
        if (!hex)
            continue;
        if (found)
            return std::nullopt; // two candidates: not sure which
        const uint32_t v = static_cast<uint32_t>(std::strtoul(std::string(name.substr(i, 8)).c_str(), nullptr, 16));
        if (v >> 24)
            return std::nullopt;
        found = FileFormIdToken{i, v};
    }
    return found;
}

} // namespace fo4mx
