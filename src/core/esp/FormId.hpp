#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>

namespace fo4mx {

// Index of a plugin in the application's own plugin table. Unlike the game's
// load-order byte this is unbounded: thousands of plugins are fine.
using PluginIndex = uint32_t;
inline constexpr PluginIndex kInvalidPlugin = std::numeric_limits<PluginIndex>::max();

// Load-order independent identity of a form: the plugin that defines the
// FormID namespace plus the 24-bit object id.
struct FormKey {
    PluginIndex origin = kInvalidPlugin;
    uint32_t objectId = 0;

    constexpr bool valid() const { return origin != kInvalidPlugin; }
    constexpr uint64_t packed() const { return (static_cast<uint64_t>(origin) << 32) | objectId; }
    static constexpr FormKey unpack(uint64_t v)
    {
        return FormKey{static_cast<PluginIndex>(v >> 32), static_cast<uint32_t>(v & 0xFFFFFFFFu)};
    }
    constexpr auto operator<=>(const FormKey&) const = default;
};

inline constexpr uint32_t masterIndexOf(uint32_t rawFormId) { return rawFormId >> 24; }
inline constexpr uint32_t objectIdOf(uint32_t rawFormId) { return rawFormId & 0x00FFFFFFu; }
inline constexpr uint32_t makeRawFormId(uint32_t masterIndex, uint32_t objectId)
{
    return (masterIndex << 24) | (objectId & 0x00FFFFFFu);
}

// FO4 1.10.163 light plugins may only define object ids 0x800..0xFFF.
inline constexpr uint32_t kFirstObjectId = 0x800;
inline constexpr uint32_t kLightPluginMaxObjectId = 0xFFF;

} // namespace fo4mx

template <>
struct std::hash<fo4mx::FormKey> {
    size_t operator()(const fo4mx::FormKey& k) const noexcept
    {
        uint64_t x = k.packed();
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};
