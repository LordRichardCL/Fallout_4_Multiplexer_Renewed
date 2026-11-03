#pragma once

// Helpers that build synthetic plugins, string tables and installs.

#include "esp/PluginWriter.hpp"
#include "esp/RecordCatalogue.hpp"
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
    // As in a real plugin: a cell with a grid position (XCLC) or the persistent flag that follows a
    // worldspace is that worldspace's cell, any other cell is an interior cell; placed references,
    // landscape and navmeshes go into the cell listed before them (temporary, or persistent when
    // flagged).
    // Dialogue as in Fallout 4: a topic that follows a quest is in that quest's group, a response in
    // the topic listed before it, scenes and branches in the quest listed before them.
    uint32_t firstCell = 0, cell = 0, world = 0, quest = 0, topic = 0;
    for (const auto& r : records)
        if (r.signature() == sig::CELL && !firstCell)
            firstCell = r.header.formId;
    for (auto& r : records) {
        Placement p;
        const auto* entry = RecordCatalogue::get().find(r.signature());
        if (r.signature() == Signature("QUST")) {
            quest = r.header.formId;
        } else if (r.signature() == Signature("DIAL")) {
            topic = r.header.formId;
            if (quest)
                p = {Placement::Kind::QuestChild, quest, false};
        } else if (r.signature() == Signature("INFO")) {
            p = {Placement::Kind::TopicChild, topic, false};
        } else if ((r.signature() == Signature("SCEN") || r.signature() == Signature("DLBR")) && quest) {
            p = {Placement::Kind::QuestChild, quest, false};
        } else if (r.signature() == Signature("WRLD")) {
            world = r.header.formId;
        } else if (r.signature() == sig::CELL) {
            cell = r.header.formId;
            const bool exterior = world && (r.find(Signature("XCLC")) || (r.header.flags & RecordFlags::Persistent));
            p.kind = exterior ? Placement::Kind::WorldCell : Placement::Kind::InteriorCell;
            p.parent = exterior ? world : 0;
        } else if (entry && entry->placement == RecordPlacement::CellChild) {
            p.kind = Placement::Kind::CellChild;
            p.parent = cell ? cell : firstCell;
            p.persistent = (r.header.flags & RecordFlags::Persistent) != 0;
        }
        w.addRecord(std::move(r), p);
    }
    w.write(path, compressLarge);
}

// A Fallout 4 quest VMAD (version 6, object format 2): one script with an object property, the
// fragment script with one stage fragment, and one alias (object: `quest`, alias 0) whose script
// has an object property naming `aliasTarget`.
inline std::vector<uint8_t> questVmad(uint32_t questScriptTarget, uint32_t quest, uint32_t aliasTarget)
{
    using namespace fo4mx;
    BinaryWriter w;
    auto object = [&](int16_t alias, uint32_t formId) {
        w.write<uint16_t>(0);
        w.write<int16_t>(alias);
        w.write<uint32_t>(formId);
    };
    auto objectProperty = [&](const char* name, uint32_t formId) {
        w.writeWString(name);
        w.write<uint8_t>(1); // object
        w.write<uint8_t>(1); // status
        object(-1, formId);
    };
    w.write<int16_t>(6);
    w.write<int16_t>(2);
    w.write<uint16_t>(1);
    w.writeWString("QuestScript");
    w.write<uint8_t>(0);
    w.write<uint16_t>(1);
    objectProperty("Target", questScriptTarget);
    // Fragments.
    w.write<uint8_t>(3);
    w.write<uint16_t>(1);
    w.writeWString("Fragments:Quests:QF_TestQuest");
    w.write<uint8_t>(0);
    w.write<uint16_t>(0);
    w.write<uint32_t>(10);
    w.write<uint32_t>(0);
    w.write<uint8_t>(1);
    w.writeWString("Fragments:Quests:QF_TestQuest");
    w.writeWString("Fragment_Stage_0010_Item_00");
    // Aliases.
    w.write<uint16_t>(1);
    object(0, quest);
    w.write<int16_t>(6);
    w.write<int16_t>(2);
    w.write<uint16_t>(1);
    w.writeWString("AliasScript");
    w.write<uint8_t>(0);
    w.write<uint16_t>(1);
    objectProperty("Item", aliasTarget);
    return w.take();
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

// Two scripts for named constants: "Consts" with a property sPluginModS whose read handler returns
// `plugin`, and "Reader" whose OnInit reads Consts.sPluginModS and passes it to
// Game.GetFormFromFile(formId, ...) ("gfff"), Game.IsPluginInstalled(...) ("presence"), or does
// nothing with it ("none": the property is never read), or reads it by name at run time
// (GetPropertyValue, "byname").
inline std::vector<uint8_t> pexFile(const std::vector<std::string>& strings, const std::string& object,
                                    const std::vector<uint8_t>& body)
{
    using namespace fo4mx;
    BinaryWriter w;
    w.write<uint32_t>(0xFA57C0DE);
    w.write<uint8_t>(3);
    w.write<uint8_t>(9);
    w.write<uint16_t>(2);
    w.write<uint64_t>(0);
    w.writeWString(object + ".psc");
    w.writeWString("user");
    w.writeWString("pc");
    w.write<uint16_t>(static_cast<uint16_t>(strings.size()));
    for (const auto& s : strings)
        w.writeWString(s);
    w.write<uint8_t>(0);  // no debug info
    w.write<uint16_t>(0); // user flags
    w.write<uint16_t>(1); // objects
    uint16_t name = 0;
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i] == object)
            name = static_cast<uint16_t>(i);
    w.write<uint16_t>(name);
    w.write<uint32_t>(static_cast<uint32_t>(body.size() + 4));
    for (uint8_t b : body)
        w.write<uint8_t>(b);
    return w.take();
}

inline std::vector<uint8_t> constantsScript(const std::string& plugin)
{
    using namespace fo4mx;
    const std::vector<std::string> strings = {"Consts", "", "sPluginModS", "String", plugin};
    BinaryWriter o;
    o.write<uint16_t>(1); o.write<uint16_t>(1); o.write<uint8_t>(0); o.write<uint32_t>(0); o.write<uint16_t>(1);
    o.write<uint16_t>(0); // structs
    o.write<uint16_t>(0); // variables
    o.write<uint16_t>(1); // properties
    o.write<uint16_t>(2); o.write<uint16_t>(3); o.write<uint16_t>(1); o.write<uint32_t>(0);
    o.write<uint8_t>(1);  // read handler
    o.write<uint16_t>(3); o.write<uint16_t>(1); o.write<uint32_t>(0); o.write<uint8_t>(0);
    o.write<uint16_t>(0); o.write<uint16_t>(0); // params, locals
    o.write<uint16_t>(1); // instructions
    o.write<uint8_t>(0x1A); o.write<uint8_t>(2); o.write<uint16_t>(4); // return "Plugin.esp"
    o.write<uint16_t>(0); // states
    return pexFile(strings, "Consts", o.take());
}

inline std::vector<uint8_t> readerScript(const std::string& how, uint32_t formId = 0x812)
{
    using namespace fo4mx;
    const std::vector<std::string> strings = {"Reader", "", "OnInit", "None", "sPluginModS", "kConsts", "::temp0",
                                              "Form", "::temp1", "String", "Game", "GetFormFromFile",
                                              "IsPluginInstalled", "GetPropertyValue", "Var"};
    BinaryWriter o;
    o.write<uint16_t>(1); o.write<uint16_t>(1); o.write<uint8_t>(0); o.write<uint32_t>(0); o.write<uint16_t>(1);
    o.write<uint16_t>(0); o.write<uint16_t>(0); o.write<uint16_t>(0); // structs, variables, properties
    o.write<uint16_t>(1); // states
    o.write<uint16_t>(1); // default state
    o.write<uint16_t>(1); // functions
    o.write<uint16_t>(2); // OnInit
    o.write<uint16_t>(3); o.write<uint16_t>(1); o.write<uint32_t>(0); o.write<uint8_t>(0);
    o.write<uint16_t>(0); // params
    o.write<uint16_t>(2); o.write<uint16_t>(6); o.write<uint16_t>(7); o.write<uint16_t>(8); o.write<uint16_t>(9);
    const bool call = how == "gfff" || how == "presence";
    o.write<uint16_t>(call ? 2 : how == "byname" ? 1 : 0);
    auto ident = [&](uint16_t i) { o.write<uint8_t>(1); o.write<uint16_t>(i); };
    if (how == "byname") { // kConsts.GetPropertyValue("sPluginModS") -> ::temp1
        o.write<uint8_t>(0x17); ident(13); ident(5); ident(8);
        o.write<uint8_t>(3); o.write<int32_t>(1);
        o.write<uint8_t>(2); o.write<uint16_t>(4);
    }
    if (call) {
        o.write<uint8_t>(0x1C); ident(4); ident(5); ident(8); // propget sPluginModS kConsts ::temp1
        o.write<uint8_t>(0x19); ident(10); ident(how == "gfff" ? 11 : 12); ident(6);
        o.write<uint8_t>(3); o.write<int32_t>(how == "gfff" ? 2 : 1);
        if (how == "gfff") {
            o.write<uint8_t>(3);
            o.write<int32_t>(static_cast<int32_t>(formId));
        }
        ident(8);
    }
    return pexFile(strings, "Reader", o.take());
}

inline std::vector<uint8_t> testScript(uint32_t formId, const std::string& plugin, int sizeAdjust = 0)
{
    ScriptOptions opt;
    opt.sizeAdjust = sizeAdjust;
    return testScript(formId, plugin, opt);
}
