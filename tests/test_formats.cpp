#include "Fixtures.hpp"
#include "papyrus/Pex.hpp"
#include "Test.hpp"

#include "assets/AssetPreview.hpp"
#include "assets/AssetScanner.hpp"
#include "ba2/Ba2Archive.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"
#include "esp/StringTables.hpp"
#include "util/Compression.hpp"
#include "util/IniFile.hpp"
#include "util/Json.hpp"

using namespace fo4mx;
using namespace fo4mx::fixtures;

TEST(text_utilities)
{
    CHECK_EQ(normalizeAssetPath("Data/Textures//Weapons\\10mm.DDS"), std::string("textures\\weapons\\10mm.dds"));
    CHECK_EQ(formatFormId(0x0001A2B3), std::string("0001A2B3"));
    CHECK_EQ(formatCount(1234567), std::string("1,234,567"));
    CHECK(iequals("Fallout4.ESM", "fallout4.esm"));
    CHECK_EQ(split("a, b ,c", ',').size(), size_t(3));
}

TEST(json_round_trip)
{
    auto v = json::Value::parse(R"({"files":[{"relPath":"meshes\\a.nif","source":"Mod é"}],"n":3.5,"ok":true})");
    CHECK_EQ(v["files"].asArray().size(), size_t(1));
    CHECK_EQ(v["files"].asArray()[0]["relPath"].asString(), std::string("meshes\\a.nif"));
    CHECK_EQ(v["n"].asNumber(), 3.5);
    auto again = json::Value::parse(v.dump());
    CHECK_EQ(again["files"].asArray()[0]["source"].asString(), std::string("Mod \xC3\xA9"));
    CHECK(again["ok"].asBool());
}

TEST(ini_reader)
{
    IniFile ini;
    ini.loadFromString("[Archive]\nSResourceArchiveList2 = A.ba2, B.ba2\n; comment\n[General]\nx=1\n");
    CHECK_EQ(ini.get("archive", "sresourcearchivelist2", ""), std::string("A.ba2, B.ba2"));
    CHECK_EQ(ini.get("General", "X", ""), std::string("1"));
}

TEST(zlib_round_trip)
{
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 7);
    auto packed = zlibDeflate(data);
    CHECK(packed.size() < data.size());
    CHECK(zlibInflate(packed, data.size()) == data);
}

TEST(record_round_trip_with_compression_and_xxxx)
{
    Record r = record("WEAP", 0x00000801, {zs("EDID", "BigGun"), raw("DATA", std::vector<uint8_t>(70000, 0xAB))});
    for (bool compress : {false, true}) {
        BinaryWriter w;
        r.write(w, compress);
        BinaryReader rd(w.buffer());
        auto h = RecordHeader::read(rd);
        CHECK_EQ(h.compressed(), compress);
        auto back = Record::parse(h, rd.readBytes(h.dataSize));
        CHECK_EQ(back.subrecords.size(), size_t(2));
        CHECK_EQ(back.editorId(), std::string("BigGun"));
        CHECK_EQ(back.subrecords[1].data.size(), size_t(70000));
    }
}

TEST(plugin_write_and_scan)
{
    auto dir = tempDir("plugin_rw");
    writePlugin(dir / "Test.esp", {"Fallout4.esm"},
                {record("KYWD", 0x01000800, {zs("EDID", "Kw")}), record("WEAP", 0x01000801, {zs("EDID", "Gun")}),
                 record("KYWD", 0x01000802, {zs("EDID", "Kw2")})});
    auto f = PluginFile::load(dir / "Test.esp");
    CHECK_EQ(f.header().masters.size(), size_t(1));
    CHECK_EQ(f.header().numRecords, 5); // 3 records + 2 groups
    CHECK_EQ(f.header().nextObjectId, 0x803u);
    std::vector<std::string> order;
    f.forEachRecord([&](const RecordLocation& loc) {
        order.push_back(loc.header.signature.str());
        CHECK_EQ(loc.depth, 1u);
        return true;
    });
    CHECK_EQ(order.size(), size_t(3));
    CHECK_EQ(order.front(), std::string("KYWD")); // KYWD group precedes WEAP
    auto h = readPluginHeader(dir / "Test.esp");
    CHECK_EQ(h.masters[0].name, std::string("Fallout4.esm"));
}

TEST(string_tables)
{
    StringTables t;
    t.load(StringFileKind::Strings, stringTable({{1, "Ten Millimeter"}, {7, "Pipe"}}, false));
    t.load(StringFileKind::DLStrings, stringTable({{2, "A long description"}}, true));
    CHECK_EQ(*t.lookup(1, StringFileKind::Strings), std::string("Ten Millimeter"));
    CHECK_EQ(*t.lookup(2, StringFileKind::DLStrings), std::string("A long description"));
    CHECK_EQ(*t.lookup(2, StringFileKind::Strings), std::string("A long description")); // fallback
    CHECK(!t.lookup(99, StringFileKind::Strings));
}

TEST(ba2_general_archive)
{
    auto dir = tempDir("ba2");
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
        {"Meshes\\Weapons\\Gun.nif", std::vector<uint8_t>(5000, 1)}, {"Scripts\\A.pex", {1, 2, 3, 4}}};
    for (bool compress : {false, true}) {
        auto path = dir / (compress ? "c.ba2" : "u.ba2");
        writeGeneralBa2(path, files, compress);
        auto ar = Ba2Archive::open(path);
        CHECK_EQ(ar->entries().size(), size_t(2));
        CHECK_EQ(ar->entries()[0].path, std::string("meshes\\weapons\\gun.nif"));
        CHECK(ar->extract(0) == files[0].second);
        CHECK(ar->extract(1) == files[1].second);
        CHECK_EQ(ar->extractHead(0, 10).size(), size_t(10));
    }
}

TEST(decoder_vmad_condition_lists)
{
    const auto& defs = RecordDefinitions::get();
    std::vector<Subrecord> subs = {zs("EDID", "X"), vmad({0x01000800}, {0x00000014, 0x02000900}),
                                   formIds("KWDA", {0x00000801, 0x01000802}), hasPerk(0x0000ABCD)};
    auto layout = decodeLayout(defs, Signature("WEAP"), subs);
    CHECK(layout.issues.empty());
    // VMAD: 1 object + 2 array objects, KWDA: 2, CTDA: HasPerk param.
    CHECK_EQ(layout.formIdSlots.size(), size_t(6));
    size_t vmadSlots = 0;
    for (const auto& s : layout.formIdSlots)
        if (s.sub == 1) {
            ++vmadSlots;
            uint32_t v = loadLE<uint32_t>(subs[1].data.data() + s.offset);
            CHECK(v == 0x01000800 || v == 0x00000014 || v == 0x02000900);
        }
    CHECK_EQ(vmadSlots, size_t(3));
    // Object format 2 puts unused + alias first: those bytes must never be read as the FormID.
    for (const auto& s : layout.formIdSlots)
        if (s.sub == 1)
            CHECK(loadLE<uint32_t>(subs[1].data.data() + s.offset) != 0xFFFF0000u);
    bool perk = false;
    for (const auto& s : layout.formIdSlots)
        if (s.sub == 3 && s.offset == 12)
            perk = true;
    CHECK(perk);
}

TEST(decoder_object_template_and_omod)
{
    const auto& defs = RecordDefinitions::get();
    // OBTS: 1 include, 1 FormID property, 1 keyword.
    BinaryWriter t;
    t.write<uint32_t>(1);
    t.write<uint32_t>(1);
    t.write<uint8_t>(1);
    t.write<uint8_t>(0);
    t.write<uint8_t>(10);
    t.write<uint8_t>(0);
    t.write<int16_t>(-1);
    t.write<uint8_t>(1);
    t.write<uint8_t>(1);           // keyword count
    t.write<uint32_t>(0x00000AAA); // keyword
    t.write<uint8_t>(0);
    t.write<uint8_t>(0);
    t.write<uint32_t>(0x01000BBB); // include OMOD
    t.write<uint8_t>(0);
    t.write<uint8_t>(0);
    t.write<uint8_t>(0);
    t.write<uint8_t>(4); // value type FormID,Int
    t.writeZeros(3);
    t.write<uint8_t>(0);
    t.writeZeros(3);
    t.write<uint16_t>(5);
    t.writeZeros(2);
    t.write<uint32_t>(0x01000CCC); // property form
    t.write<uint32_t>(1);
    t.write<float>(0);
    auto layout = decodeLayout(defs, Signature("WEAP"), {Subrecord(Signature("OBTS"), t.take())});
    CHECK(layout.issues.empty());
    CHECK_EQ(layout.formIdSlots.size(), size_t(3));

    BinaryWriter d;
    d.write<uint32_t>(1); // includes
    d.write<uint32_t>(1); // properties
    d.write<uint8_t>(0);
    d.write<uint8_t>(0);
    d.write(Signature("WEAP").value);
    d.write<uint8_t>(0);
    d.write<uint8_t>(0);
    d.write<uint32_t>(0x00000111); // attach point
    d.write<uint32_t>(1);
    d.write<uint32_t>(0x00000222); // attach parent slot
    d.write<uint32_t>(0);          // items
    d.write<uint32_t>(0x01000333); // include
    d.write<uint8_t>(0);
    d.write<uint8_t>(0);
    d.write<uint8_t>(0);
    d.write<uint8_t>(6);
    d.writeZeros(3);
    d.write<uint8_t>(0);
    d.writeZeros(3);
    d.write<uint16_t>(9);
    d.writeZeros(2);
    d.write<uint32_t>(0x01000444);
    d.write<float>(1.5f);
    d.write<float>(0);
    auto omod = decodeLayout(defs, Signature("OMOD"), {Subrecord(Signature("DATA"), d.take())});
    CHECK(omod.issues.empty());
    CHECK_EQ(omod.formIdSlots.size(), size_t(4));

    // A malformed OBTS is reported and scanned heuristically, never trusted.
    auto bad = decodeLayout(defs, Signature("WEAP"), {raw("OBTS", {1, 2, 3, 4, 5, 6, 7, 8, 9})});
    CHECK(!bad.issues.empty());
    CHECK(bad.formIdSlots.empty());
    CHECK(!bad.heuristic.empty());
}

TEST(dds_decode_bc1_and_rgba)
{
    // 4x4 BC1, solid red.
    auto dds = makeDdsHeader(4, 4, 1, 71, false);
    uint16_t red = 0xF800;
    BinaryWriter b;
    b.write(red);
    b.write(red);
    b.write<uint32_t>(0);
    dds.insert(dds.end(), b.buffer().begin(), b.buffer().end());
    auto info = preview::readDdsInfo(dds);
    CHECK(info.has_value());
    CHECK_EQ(info->width, 4u);
    CHECK_EQ(info->format, std::string("BC1"));
    auto img = preview::decodeDds(dds);
    CHECK(img.has_value());
    CHECK_EQ(int(img->rgba[0]), 255);
    CHECK_EQ(int(img->rgba[1]), 0);

    auto bc7 = makeDdsHeader(4, 4, 1, 98, false);
    bc7.resize(bc7.size() + 16);
    CHECK(preview::readDdsInfo(bc7)->format == "BC7");
    CHECK(!preview::decodeDds(bc7).has_value());
}

TEST(pex_and_config_plugin_names)
{
    BinaryWriter w;
    w.write<uint32_t>(0xFA57C0DE);
    w.write<uint8_t>(3);
    w.write<uint8_t>(9);
    w.write<uint16_t>(2);
    w.write<uint64_t>(0);
    w.writeWString("Script.psc");
    w.writeWString("user");
    w.writeWString("pc");
    w.write<uint16_t>(3);
    w.writeWString("OnInit");
    w.writeWString("MyMod.esp");
    w.writeWString("GetFormFromFile");
    auto strings = AssetScanner::pexStrings(w.buffer());
    CHECK_EQ(strings.size(), size_t(3));
    CHECK_EQ(strings[1], std::string("MyMod.esp"));

    auto names = AssetScanner::pluginNamesInText("sPlugin=Some Mod - Patch.esp\nfoo=\"Other.ESM\"|0x800\n");
    CHECK_EQ(names.size(), size_t(2));
    CHECK(std::find(names.begin(), names.end(), "some mod - patch.esp") != names.end());
    CHECK(std::find(names.begin(), names.end(), "other.esm") != names.end());
}

// Compiled Fallout 4 scripts: the scanner walks the whole file and finds GetFormFromFile calls; any
// inconsistency (wrong size, truncation, Skyrim byte order) leaves the script untouched.
TEST(pex_scan_and_rewrite)
{
    auto script = testScript(0x812, "ModS.esp");
    auto scan = pex::scan(script);
    CHECK(scan.ok);
    CHECK_EQ(scan.getFormFromFile.size(), size_t(1));
    const auto& call = scan.getFormFromFile[0];
    CHECK(call.formIdLiteral && call.fileLiteral);
    CHECK_EQ(call.formId, int32_t(0x812));
    CHECK_EQ(scan.strings[call.fileString], std::string("ModS.esp"));
    CHECK(scan.usedElsewhere[call.fileString]); // IsPluginInstalled uses the same string

    // Rewrite: a new string for the call, the old one renamed, the FormID patched in place.
    auto strings = scan.strings;
    strings[call.fileString] = "Dummy_Plugin_01.esp";
    strings.push_back("Dummy_Plugin_02.esp");
    std::vector<pex::Patch> patches{{call.formIdOffset, {0x34, 0x12, 0, 0}},
                                    {call.fileStringOffset, {static_cast<uint8_t>(strings.size() - 1), 0}}};
    auto out = pex::rewrite(script, scan, strings, patches);
    auto again = pex::scan(out);
    CHECK(again.ok);
    CHECK_EQ(again.getFormFromFile[0].formId, int32_t(0x1234));
    CHECK_EQ(again.strings[again.getFormFromFile[0].fileString], std::string("Dummy_Plugin_02.esp"));
    CHECK_EQ(again.strings[call.fileString], std::string("Dummy_Plugin_01.esp"));

    CHECK(!pex::scan(testScript(0x812, "ModS.esp", 1)).ok); // object size disagrees
    auto truncated = script;
    truncated.pop_back();
    CHECK(!pex::scan(truncated).ok);
    auto skyrim = script;
    std::swap(skyrim[0], skyrim[3]);
    std::swap(skyrim[1], skyrim[2]);
    CHECK(!pex::scan(skyrim).ok);
    // Some compilers count the size field itself: accepted.
    CHECK(pex::scan(testScript(0x812, "ModS.esp", 4)).ok);
}
