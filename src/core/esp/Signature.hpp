#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fo4mx {

// Four-character record / subrecord type code, stored as its little-endian uint32.
struct Signature {
    uint32_t value = 0;

    constexpr Signature() = default;
    constexpr explicit Signature(uint32_t v) : value(v) {}
    constexpr Signature(const char (&s)[5])
        : value(static_cast<uint32_t>(static_cast<uint8_t>(s[0])) |
                (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
                (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
                (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24))
    {
    }

    static Signature fromString(std::string_view s)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < 4; ++i) {
            uint8_t c = i < s.size() ? static_cast<uint8_t>(s[i]) : static_cast<uint8_t>(' ');
            v |= static_cast<uint32_t>(c) << (8 * i);
        }
        return Signature(v);
    }

    std::string str() const
    {
        std::string s(4, ' ');
        for (int i = 0; i < 4; ++i) {
            char c = static_cast<char>((value >> (8 * i)) & 0xFF);
            s[i] = (c >= 32 && c < 127) ? c : '?';
        }
        return s;
    }

    constexpr bool empty() const { return value == 0; }
    constexpr auto operator<=>(const Signature&) const = default;
};

namespace sig {
inline constexpr Signature TES4{"TES4"};
inline constexpr Signature GRUP{"GRUP"};
inline constexpr Signature HEDR{"HEDR"};
inline constexpr Signature CNAM{"CNAM"};
inline constexpr Signature SNAM{"SNAM"};
inline constexpr Signature MAST{"MAST"};
inline constexpr Signature DATA{"DATA"};
inline constexpr Signature ONAM{"ONAM"};
inline constexpr Signature INTV{"INTV"};
inline constexpr Signature INCC{"INCC"};
inline constexpr Signature XXXX{"XXXX"};
inline constexpr Signature EDID{"EDID"};
inline constexpr Signature FULL{"FULL"};
inline constexpr Signature DESC{"DESC"};
inline constexpr Signature VMAD{"VMAD"};
inline constexpr Signature KSIZ{"KSIZ"};
inline constexpr Signature KWDA{"KWDA"};
inline constexpr Signature LLCT{"LLCT"};
inline constexpr Signature LVLO{"LVLO"};
inline constexpr Signature COED{"COED"};
inline constexpr Signature LNAM{"LNAM"};
inline constexpr Signature CTDA{"CTDA"};
inline constexpr Signature CITC{"CITC"};

inline constexpr Signature WEAP{"WEAP"};
inline constexpr Signature ARMO{"ARMO"};
inline constexpr Signature AMMO{"AMMO"};
inline constexpr Signature OMOD{"OMOD"};
inline constexpr Signature COBJ{"COBJ"};
inline constexpr Signature KYWD{"KYWD"};
inline constexpr Signature LVLI{"LVLI"};
inline constexpr Signature FLST{"FLST"};
inline constexpr Signature INGR{"INGR"};
inline constexpr Signature ALCH{"ALCH"};
inline constexpr Signature MISC{"MISC"};
inline constexpr Signature BOOK{"BOOK"};
inline constexpr Signature NOTE{"NOTE"};
inline constexpr Signature KEYM{"KEYM"};
inline constexpr Signature FACT{"FACT"};
inline constexpr Signature PERK{"PERK"};
inline constexpr Signature SPEL{"SPEL"};
inline constexpr Signature MESG{"MESG"};
inline constexpr Signature CELL{"CELL"};
inline constexpr Signature WRLD{"WRLD"};
inline constexpr Signature REFR{"REFR"};
inline constexpr Signature NPC_{"NPC_"};
} // namespace sig

} // namespace fo4mx

template <>
struct std::hash<fo4mx::Signature> {
    size_t operator()(const fo4mx::Signature& s) const noexcept { return std::hash<uint32_t>{}(s.value); }
};
