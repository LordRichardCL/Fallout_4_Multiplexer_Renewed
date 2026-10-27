#pragma once

// Helpers that build synthetic plugins, string tables and installs.

#include "esp/PluginWriter.hpp"
#include "esp/Record.hpp"
#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fo4mx::fixtures {

inline std::filesystem::path tempDir(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / ("fo4mx_test_" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

inline void writeFile(const std::filesystem::path& p, const std::string& text)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

inline void writeBytes(const std::filesystem::path& p, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

inline Subrecord zs(const char* sig, const std::string& text)
{
    return Subrecord::fromZString(Signature::fromString(sig), text);
}

inline Subrecord u32(const char* sig, uint32_t v)
{
    return Subrecord::fromValue<uint32_t>(Signature::fromString(sig), v);
}

inline Subrecord raw(const char* sig, std::vector<uint8_t> bytes)
{
    return Subrecord(Signature::fromString(sig), std::move(bytes));
}

inline Subrecord formIds(const char* sig, const std::vector<uint32_t>& ids)
{
    BinaryWriter w;
    for (auto id : ids)
        w.write(id);
    return Subrecord(Signature::fromString(sig), w.take());
}

inline Record record(const char* sig, uint32_t formId, std::vector<Subrecord> subs, uint32_t flags = 0)
{
    Record r;
    r.header.signature = Signature::fromString(sig);
    r.header.formId = formId;
    r.header.flags = flags;
    r.header.formVersion = 131;
    r.subrecords = std::move(subs);
    return r;
}

inline void writePlugin(const std::filesystem::path& path, const std::vector<std::string>& masters,
                        std::vector<Record> records, uint32_t headerFlags = 0, bool compressLarge = false)
{
    PluginWriter w;
    PluginHeader h;
    h.record.signature = sig::TES4;
    h.record.flags = headerFlags;
    h.record.formVersion = 131;
    h.author = "fixture";
    for (const auto& m : masters)
        h.masters.push_back({m, 0});
    w.setHeader(h);
    for (auto& r : records)
        w.addRecord(std::move(r));
    w.write(path, compressLarge);
}

// .STRINGS (null-terminated) or .DLSTRINGS (length-prefixed) table.
inline std::vector<uint8_t> stringTable(const std::map<uint32_t, std::string>& strings, bool lengthPrefixed)
{
    BinaryWriter dir, data;
    for (const auto& [id, s] : strings) {
        dir.write(id);
        dir.write(static_cast<uint32_t>(data.size()));
        if (lengthPrefixed)
            data.write(static_cast<uint32_t>(s.size() + 1));
        data.writeZString(s);
    }
    BinaryWriter out;
    out.write(static_cast<uint32_t>(strings.size()));
    out.write(static_cast<uint32_t>(data.size()));
    out.writeBytes(dir.buffer());
    out.writeBytes(data.buffer());
    return out.take();
}

// VMAD (version 6, object format 2 as Fallout 4 writes it) with one script and object / object-array properties.
inline Subrecord vmad(const std::vector<uint32_t>& objects, const std::vector<uint32_t>& arrayObjects)
{
    BinaryWriter w;
    w.write<int16_t>(6);
    w.write<int16_t>(2);
    w.write<uint16_t>(1);
    w.writeWString("FixtureScript");
    w.write<uint8_t>(0);
    w.write<uint16_t>(static_cast<uint16_t>(objects.size() + 2));
    for (size_t i = 0; i < objects.size(); ++i) {
        w.writeWString("Obj" + std::to_string(i));
        w.write<uint8_t>(1);
        w.write<uint8_t>(1);
        // Object format 2: unused, alias, FormID
        w.write<uint16_t>(0);
        w.write<int16_t>(-1);
        w.write<uint32_t>(objects[i]);
    }
    w.writeWString("Count");
    w.write<uint8_t>(3);
    w.write<uint8_t>(1);
    w.write<int32_t>(42);
    w.writeWString("Forms");
    w.write<uint8_t>(11);
    w.write<uint8_t>(1);
    w.write<uint32_t>(static_cast<uint32_t>(arrayObjects.size()));
    for (auto id : arrayObjects) {
        w.write<uint16_t>(0);
        w.write<int16_t>(-1);
        w.write<uint32_t>(id);
    }
    return Subrecord(Signature("VMAD"), w.take());
}

// CTDA HasPerk(perk) == 1
inline Subrecord hasPerk(uint32_t perk)
{
    BinaryWriter w;
    w.write<uint8_t>(0);
    w.writeZeros(3);
    w.write<float>(1.0f);
    w.write<uint16_t>(448);
    w.write<uint16_t>(0);
    w.write<uint32_t>(perk);
    w.write<uint32_t>(0);
    w.write<uint32_t>(0);
    w.write<uint32_t>(0);
    w.write<int32_t>(-1);
    return Subrecord(Signature("CTDA"), w.take());
}

inline Subrecord lvlo(uint16_t level, uint32_t form, uint16_t count)
{
    BinaryWriter w;
    w.write(level);
    w.write<uint16_t>(0);
    w.write(form);
    w.write(count);
    w.write<uint8_t>(0);
    w.write<uint8_t>(0);
    return Subrecord(Signature("LVLO"), w.take());
}

inline Subrecord components(const std::vector<std::pair<uint32_t, uint32_t>>& items, const char* sig = "FVPA")
{
    BinaryWriter w;
    for (auto [f, n] : items) {
        w.write(f);
        w.write(n);
    }
    return Subrecord(Signature::fromString(sig), w.take());
}

// WEAP DNAM: ammo FormID followed by opaque data.
inline Subrecord weaponData(uint32_t ammo, uint8_t fill)
{
    std::vector<uint8_t> d(136, fill);
    storeLE<uint32_t>(d.data(), ammo);
    return Subrecord(Signature("DNAM"), std::move(d));
}

} // namespace fo4mx::fixtures
