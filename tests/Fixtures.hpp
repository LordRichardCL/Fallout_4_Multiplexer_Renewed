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
    // Actor value and sound slots hold FormIDs (null here), never filler.
    for (size_t off : {40, 44, 73, 77, 81, 85, 89, 93, 97, 101})
        storeLE<uint32_t>(d.data() + off, 0);
    return Subrecord(Signature("DNAM"), std::move(d));
}

} // namespace fo4mx::fixtures

// A compiled Fallout 4 script (layout as written by the Papyrus compiler and Caprica) with one
// function: ::temp0 = Game.GetFormFromFile(formId, plugin); Game.IsPluginInstalled(plugin); return.
// sizeAdjust corrupts the object's recorded size (0 keeps it right).
// Variants: the FormID passed to GetFormFromFile comes from a variable (`runtimeFormId`); the plugin
// name is also assigned to a string variable (`assignName`, a use whose meaning is unknown); `filler`
// unused strings are appended to the string table (a full table has 65535).
struct ScriptOptions {
    int sizeAdjust = 0;
    bool runtimeFormId = false;
    bool assignName = false;
    size_t filler = 0;
    std::string unused; // one more unused string (e.g. another plugin's name)
};

inline std::vector<uint8_t> testScript(uint32_t formId, const std::string& plugin, const ScriptOptions& opt)
{
    using namespace fo4mx;
    std::vector<std::string> strings = {"TestScript", "", "OnInit", "Game", "GetFormFromFile", "::temp0", "Form",
                                        plugin, "IsPluginInstalled", "::temp1", "Bool", "None", "::NoneVar",
                                        "::temp2", "String", "aiFormID", "Int"};
    if (!opt.unused.empty())
        strings.push_back(opt.unused);
    for (size_t i = 0; i < opt.filler; ++i)
        strings.push_back("filler" + std::to_string(i));
    auto idx = [&](const std::string& s) {
        for (size_t i = 0; i < strings.size(); ++i)
            if (strings[i] == s)
                return static_cast<uint16_t>(i);
        return uint16_t(0);
    };
    BinaryWriter w;
    w.write<uint32_t>(0xFA57C0DE);
    w.write<uint8_t>(3);
    w.write<uint8_t>(9);
    w.write<uint16_t>(2);
    w.write<uint64_t>(0);
    w.writeWString("TestScript.psc");
    w.writeWString("user");
    w.writeWString("pc");
    w.write<uint16_t>(static_cast<uint16_t>(strings.size()));
    for (const auto& s : strings)
        w.writeWString(s);
    w.write<uint8_t>(0);  // no debug info
    w.write<uint16_t>(0); // user flags
    w.write<uint16_t>(1); // objects

    BinaryWriter o; // object body after the size field
    o.write<uint16_t>(idx(""));    // parent
    o.write<uint16_t>(idx(""));    // doc
    o.write<uint8_t>(0);           // const
    o.write<uint32_t>(0);          // user flags
    o.write<uint16_t>(idx(""));    // auto state
    o.write<uint16_t>(0);          // structs
    o.write<uint16_t>(0);          // variables
    o.write<uint16_t>(0);          // properties
    o.write<uint16_t>(1);          // states
    o.write<uint16_t>(idx(""));    // default state
    o.write<uint16_t>(1);          // functions
    o.write<uint16_t>(idx("OnInit"));
    o.write<uint16_t>(idx("None")); // return type
    o.write<uint16_t>(idx(""));     // doc
    o.write<uint32_t>(0);
    o.write<uint8_t>(0);
    o.write<uint16_t>(0); // params
    o.write<uint16_t>(4); // locals
    o.write<uint16_t>(idx("::temp0"));
    o.write<uint16_t>(idx("Form"));
    o.write<uint16_t>(idx("::temp1"));
    o.write<uint16_t>(idx("Bool"));
    o.write<uint16_t>(idx("::temp2"));
    o.write<uint16_t>(idx("String"));
    o.write<uint16_t>(idx("aiFormID"));
    o.write<uint16_t>(idx("Int"));
    o.write<uint16_t>(opt.assignName ? 4 : 3); // instructions
    auto ident = [&](const std::string& s) { o.write<uint8_t>(1); o.write<uint16_t>(idx(s)); };
    auto string = [&](const std::string& s) { o.write<uint8_t>(2); o.write<uint16_t>(idx(s)); };
    auto integer = [&](int32_t v) { o.write<uint8_t>(3); o.write<int32_t>(v); };
    o.write<uint8_t>(0x19); // callstatic Game.GetFormFromFile -> ::temp0
    ident("Game");
    ident("GetFormFromFile");
    ident("::temp0");
    integer(2);
    if (opt.runtimeFormId)
        ident("aiFormID");
    else
        integer(static_cast<int32_t>(formId));
    string(plugin);
    o.write<uint8_t>(0x19); // callstatic Game.IsPluginInstalled -> ::temp1
    ident("Game");
    ident("IsPluginInstalled");
    ident("::temp1");
    integer(1);
    string(plugin);
    if (opt.assignName) {
        o.write<uint8_t>(0x0D); // assign ::temp2 = "Plugin.esp"
        ident("::temp2");
        string(plugin);
    }
    o.write<uint8_t>(0x1A); // return none
    o.write<uint8_t>(0);

    w.write<uint16_t>(idx("TestScript"));
    w.write<uint32_t>(static_cast<uint32_t>(static_cast<int>(o.size()) + opt.sizeAdjust));
    w.writeBytes(o.buffer());
    return w.take();
}

inline std::vector<uint8_t> testScript(uint32_t formId, const std::string& plugin, int sizeAdjust = 0)
{
    ScriptOptions opt;
    opt.sizeAdjust = sizeAdjust;
    return testScript(formId, plugin, opt);
}
