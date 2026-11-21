#include "util/Zip.hpp"
#include "diagnostics/CrashLog.hpp"
#include "engine/SupportBundle.hpp"
#include "engine/BuildComparison.hpp"
#include "engine/EngineFiles.hpp"
#include "diagnostics/PapyrusLog.hpp"
#include <fstream>
#include "esp/RecordDefinitions.hpp"
#include "diagnostics/RecorderLog.hpp"
#include "engine/CompilationEngine.hpp"
#include <cstdio>
#include <cstring>
#include "Fixtures.hpp"
#include "papyrus/Pex.hpp"
#include "Test.hpp"

#include "assets/AssetPreview.hpp"
#include "assets/AssetScanner.hpp"
#include "ba2/Ba2Archive.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/GroupLayout.hpp"
#include "esp/PrevisFile.hpp"
#include "esp/LayoutCheck.hpp"
#include "esp/PluginFile.hpp"
#include "esp/PluginWriter.hpp"
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

// Grid math as xEdit does it: sub-block = floor(cell / 8), block = floor(sub-block / 4); labels hold
// Y in the low word and X in the high word; interior blocks are the last two decimal digits.
TEST(group_layout_math)
{
    CHECK(subBlockOf({-1, 9}) == (GridCell{-1, 1}));
    CHECK(blockOf(subBlockOf({-1, 9})) == (GridCell{-1, 0}));
    CHECK(subBlockOf({-9, -8}) == (GridCell{-2, -1}));
    CHECK(blockOf(subBlockOf({-9, -8})) == (GridCell{-1, -1}));
    CHECK(subBlockOf({7, 8}) == (GridCell{0, 1}));
    CHECK(blockOf({4, 3}) == (GridCell{1, 0}));
    CHECK_EQ(gridLabel({-1, 2}), 0xFFFF0002u);
    CHECK(gridFromLabel(gridLabel({-30, 17})) == (GridCell{-30, 17}));
    CHECK_EQ(interiorBlockOf(0x01003039), 5); // object ID 12345
    CHECK_EQ(interiorSubBlockOf(0x01003039), 4);
    CHECK(recordSortPriority(Signature("LAND")) < recordSortPriority(Signature("NAVM")));
    CHECK(recordSortPriority(Signature("NAVM")) < recordSortPriority(Signature("REFR")));
}

namespace {

std::vector<uint8_t> recordBytes(const Record& r)
{
    BinaryWriter w;
    r.write(w, false);
    return w.take();
}

// A group built by hand (independent of PluginWriter).
std::vector<uint8_t> group(uint32_t label, int32_t type, const std::vector<std::vector<uint8_t>>& contents)
{
    BinaryWriter body;
    for (const auto& c : contents)
        body.writeBytes(c);
    GroupHeader g;
    g.label = label;
    g.groupType = type;
    g.groupSize = static_cast<uint32_t>(GroupHeader::kSize + body.size());
    BinaryWriter w;
    g.write(w);
    w.writeBytes(body.buffer());
    return w.take();
}

Record cellAt(uint32_t formId, const char* edid, int32_t x, int32_t y)
{
    std::vector<uint8_t> xclc(12, 0);
    storeLE<int32_t>(xclc.data(), x);
    storeLE<int32_t>(xclc.data() + 4, y);
    return record("CELL", formId, {zs("EDID", edid), raw("XCLC", xclc)});
}

PluginHeader fixtureHeader()
{
    PluginHeader h;
    h.record.signature = sig::TES4;
    h.record.formVersion = 131;
    h.masters.push_back({"Fallout4.esm", 0});
    return h;
}

} // namespace

// The writer's nested groups match a file laid out by hand the way xEdit writes it.
TEST(nested_groups_match_hand_built_layout)
{
    using K = Placement::Kind;
    const auto interior = record("CELL", 0x01003039, {zs("EDID", "Vault")});
    const auto persistentRef = record("REFR", 0x01000801, {zs("EDID", "Door")}, RecordFlags::Persistent);
    const auto tempRef = record("REFR", 0x01000802, {zs("EDID", "Chair")});
    const auto navmesh = record("NAVM", 0x01000803, {raw("NVNM", std::vector<uint8_t>(8, 0))});
    const auto world = record("WRLD", 0x01000810, {zs("EDID", "World")});
    const auto worldCell = record("CELL", 0x01000811, {zs("EDID", "WorldPersistent")}, RecordFlags::Persistent);
    const auto worldRef = record("REFR", 0x01000812, {zs("EDID", "Marker")}, RecordFlags::Persistent);
    const auto exterior = cellAt(0x01000813, "Outside", -1, 9);
    const auto rock = record("REFR", 0x01000814, {zs("EDID", "Rock")});
    const auto land = record("LAND", 0x01000815, {raw("DATA", {1, 2, 3, 4})});

    PluginWriter w;
    w.setHeader(fixtureHeader());
    // Added in no particular order.
    w.addRecord(land, {K::CellChild, 0x01000813, false});
    w.addRecord(rock, {K::CellChild, 0x01000813, false});
    w.addRecord(exterior, {K::WorldCell, 0x01000810, false});
    w.addRecord(tempRef, {K::CellChild, 0x01003039, false});
    w.addRecord(world);
    w.addRecord(navmesh, {K::CellChild, 0x01003039, false});
    w.addRecord(worldRef, {K::CellChild, 0x01000811, true});
    w.addRecord(persistentRef, {K::CellChild, 0x01003039, true});
    w.addRecord(worldCell, {K::WorldCell, 0x01000810, false});
    w.addRecord(interior, {K::InteriorCell, 0, false});
    const auto image = w.build();

    using G = std::vector<std::vector<uint8_t>>;
    const auto cellGroup = group(sig::CELL.value, GroupType::Top, G{group(5, GroupType::InteriorBlock, G{group(
        4, GroupType::InteriorSubBlock,
        G{recordBytes(interior),
          group(0x01003039, GroupType::CellChildren,
                G{group(0x01003039, GroupType::CellPersistent, G{recordBytes(persistentRef)}),
                  group(0x01003039, GroupType::CellTemporary, G{recordBytes(navmesh), recordBytes(tempRef)})})})})});
    const auto worldGroup = group(Signature("WRLD").value, GroupType::Top,
        G{recordBytes(world),
          group(0x01000810, GroupType::WorldChildren,
                G{recordBytes(worldCell),
                  group(0x01000811, GroupType::CellChildren,
                        G{group(0x01000811, GroupType::CellPersistent, G{recordBytes(worldRef)})}),
                  group(gridLabel({-1, 0}), GroupType::ExteriorBlock,
                        G{group(gridLabel({-1, 1}), GroupType::ExteriorSubBlock,
                                G{recordBytes(exterior),
                                  group(0x01000813, GroupType::CellChildren,
                                        G{group(0x01000813, GroupType::CellTemporary,
                                                G{recordBytes(land), recordBytes(rock)})})})})})});
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), cellGroup.begin(), cellGroup.end()); // CELL precedes WRLD
    expected.insert(expected.end(), worldGroup.begin(), worldGroup.end());

    auto f = PluginFile::fromBytes(image);
    const size_t headerSize = RecordHeader::kSize + f.header().record.dataSize;
    CHECK(std::vector<uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(headerSize), image.end()) == expected);
    CHECK_EQ(f.header().numRecords, 10 + 14); // records + groups
}

// Written and read back: every record keeps its bytes and its place, and writing what was read
// gives the same file.
TEST(nested_groups_round_trip)
{
    using K = Placement::Kind;
    std::vector<std::pair<Record, Placement>> in = {
        {record("KYWD", 0x01000800, {zs("EDID", "Kw")}), {}},
        {record("CELL", 0x01000801, {zs("EDID", "InteriorA")}), {K::InteriorCell, 0, false}},
        {record("CELL", 0x01000902, {zs("EDID", "InteriorB")}), {K::InteriorCell, 0, false}},
        {record("REFR", 0x01000803, {zs("EDID", "A1")}), {K::CellChild, 0x01000801, false}},
        {record("ACHR", 0x01000804, {zs("EDID", "A2")}, RecordFlags::Persistent), {K::CellChild, 0x01000801, true}},
        {record("REFR", 0x01000805, {zs("EDID", "B1")}), {K::CellChild, 0x01000902, false}},
        {record("WRLD", 0x01000810, {zs("EDID", "W1")}), {}},
        {record("WRLD", 0x01000820, {zs("EDID", "W2")}), {}},
        {cellAt(0x01000811, "W1a", 0, 0), {K::WorldCell, 0x01000810, false}},
        {cellAt(0x01000812, "W1b", 1, 1), {K::WorldCell, 0x01000810, false}},     // same sub-block
        {cellAt(0x01000813, "W1c", -33, -40), {K::WorldCell, 0x01000810, false}}, // another block
        {cellAt(0x01000821, "W2a", 50, -3), {K::WorldCell, 0x01000820, false}},
        {record("CELL", 0x01000822, {zs("EDID", "W2p")}, RecordFlags::Persistent), {K::WorldCell, 0x01000820, false}},
        {record("LAND", 0x01000814, {raw("DATA", {1})}), {K::CellChild, 0x01000811, false}},
        {record("REFR", 0x01000815, {zs("EDID", "W1aRef")}), {K::CellChild, 0x01000811, false}},
        {record("REFR", 0x01000823, {zs("EDID", "W2pRef")}, RecordFlags::Persistent), {K::CellChild, 0x01000822, true}},
        {record("DIAL", 0x01000830, {zs("EDID", "Topic")}), {}},
        {record("INFO", 0x01000832, {zs("EDID", "Second")}), {K::TopicChild, 0x01000830, false}},
        {record("INFO", 0x01000831, {zs("EDID", "First")}), {K::TopicChild, 0x01000830, false}},
        // Fallout 4 quest children: the quest's own topics (with their responses), branches and scenes.
        {record("QUST", 0x01000840, {zs("EDID", "Quest")}), {}},
        {record("DIAL", 0x01000841, {zs("EDID", "QuestTopic")}), {K::QuestChild, 0x01000840, false}},
        {record("INFO", 0x01000842, {zs("EDID", "QuestLine")}), {K::TopicChild, 0x01000841, false}},
        {record("SCEN", 0x01000843, {zs("EDID", "Scene")}), {K::QuestChild, 0x01000840, false}},
        {record("DLBR", 0x01000844, {zs("EDID", "Branch")}), {K::QuestChild, 0x01000840, false}},
        // A worldspace cell with neither a grid position nor the persistent flag stays in the worldspace.
        {record("CELL", 0x01000824, {zs("EDID", "W2odd")}), {K::WorldCell, 0x01000820, false}},
    };
    PluginWriter w;
    w.setHeader(fixtureHeader());
    for (const auto& [r, p] : in)
        w.addRecord(r, p);
    const auto image = w.build();

    auto f = PluginFile::fromBytes(image);
    std::map<uint32_t, std::pair<std::vector<uint8_t>, Placement>> read;
    std::vector<uint32_t> infoOrder;
    PluginWriter again;
    again.setHeader(f.header());
    f.forEachRecord([&](const RecordLocation& loc) {
        CHECK(!loc.unusualGroup);
        auto rec = f.recordAt(loc.offset);
        read[loc.header.formId] = {recordBytes(rec), loc.placement()};
        if (loc.header.signature == Signature("INFO") && loc.topic == 0x01000830)
            infoOrder.push_back(loc.header.formId);
        again.addRecord(rec, loc.placement());
        return true;
    });
    CHECK_EQ(read.size(), in.size());
    for (const auto& [r, p] : in) {
        auto it = read.find(r.header.formId);
        CHECK(it != read.end());
        CHECK(it->second.first == recordBytes(r));
        CHECK(it->second.second == p);
    }
    CHECK(infoOrder == (std::vector<uint32_t>{0x01000832, 0x01000831})); // topic responses keep their order
    CHECK(again.build() == image);
}

// The game resolves an INFO's previous INFO (PNAM) when it loads the INFO: one that names another INFO of
// the same topic group is written after it (a compiled topic can hold one plugin's override of a response
// whose PNAM names another plugin's new response, owner's crash in INFO loading). TIFC, the topic's
// response count, is the number written.
TEST(topic_responses_follow_their_previous_info_and_tifc_counts_them)
{
    using K = Placement::Kind;
    PluginWriter w;
    w.setHeader(fixtureHeader());
    w.addRecord(record("DIAL", 0x00000830, {zs("EDID", "Topic"), u32("TIFC", 1)}), {});
    w.addRecord(record("INFO", 0x00000831, {u32("PNAM", 0x01000900)}), {K::TopicChild, 0x00000830, false});
    w.addRecord(record("INFO", 0x01000901, {}), {K::TopicChild, 0x00000830, false}); // no PNAM: keeps its place
    w.addRecord(record("INFO", 0x01000900, {u32("PNAM", 0x00000500)}), {K::TopicChild, 0x00000830, false});
    const auto image = w.build();
    auto f = PluginFile::fromBytes(image);
    std::vector<uint32_t> order;
    uint32_t tifc = 0;
    f.forEachRecord([&](const RecordLocation& loc) {
        if (loc.header.signature == Signature("INFO"))
            order.push_back(loc.header.formId);
        if (loc.header.signature == Signature("DIAL")) {
            auto rec = f.recordAt(loc.offset);
            if (const auto* t = rec.find(Signature("TIFC")))
                std::memcpy(&tifc, t->data.data(), 4);
        }
        return true;
    });
    CHECK(order == (std::vector<uint32_t>{0x01000900, 0x00000831, 0x01000901}));
    CHECK_EQ(tifc, 3u);
}

// A record that cannot be placed stops the build: nothing is written.
TEST(nested_groups_refuse_misplaced_records)
{
    using K = Placement::Kind;
    auto fails = [](std::vector<std::pair<Record, Placement>> records, const std::string& expect) {
        PluginWriter w;
        w.setHeader(fixtureHeader());
        for (auto& [r, p] : records)
            w.addRecord(r, p);
        try {
            w.build();
        } catch (const FormatError& e) {
            return std::string(e.what()).find(expect) != std::string::npos;
        }
        return false;
    };
    CHECK(fails({{record("REFR", 0x01000801, {}), {K::CellChild, 0x01000800, false}}}, "its cell 01000800 is not in the plugin"));
    CHECK(fails({{record("REFR", 0x01000801, {}), {}}}, "cannot be written as a top-level group"));
    CHECK(fails({{record("CELL", 0x01000801, {}), {}}}, "cannot be placed as a top"));
    CHECK(fails({{record("KYWD", 0x01000801, {}), {K::InteriorCell, 0, false}}}, "cannot be placed as a interior cell"));
    CHECK(fails({{record("CELL", 0x01000801, {}), {K::WorldCell, 0x01000800, false}}}, "its worldspace 01000800"));
    CHECK(fails({{record("INFO", 0x01000801, {}), {K::TopicChild, 0x01000800, false}}}, "its topic 01000800"));
    // Topics added to another plugin's quest: the quest's group is written without the quest.
    {
        PluginWriter w;
        w.setHeader(fixtureHeader());
        w.addRecord(record("DIAL", 0x01000801, {zs("EDID", "AddedTopic")}), {K::QuestChild, 0x00001234, false});
        auto f = PluginFile::fromBytes(w.build());
        size_t topics = 0;
        f.forEachRecord([&](const RecordLocation& loc) {
            CHECK(loc.placement() == (Placement{K::QuestChild, 0x00001234, false}));
            ++topics;
            return true;
        });
        CHECK_EQ(topics, size_t(1));
    }
    CHECK(fails({{record("CELL", 0x01000801, {}), {K::InteriorCell, 0, false}},
                 {record("CELL", 0x01000801, {}), {K::InteriorCell, 0, false}}},
                "in the plugin twice"));
}

// The layout check: a plugin laid out like the writer passes in the same order; one laid out in
// another valid order passes with the order noted; a cell in the wrong block is a difference.
TEST(layout_check_against_plugins)
{
    using G = std::vector<std::vector<uint8_t>>;
    const auto world = record("WRLD", 0x01000810, {zs("EDID", "World")});
    const auto a = cellAt(0x01000811, "A", 0, 0);
    const auto b = cellAt(0x01000812, "B", -40, 3);
    const auto refA = record("REFR", 0x01000813, {zs("EDID", "RefA")});
    const auto persistentA = record("REFR", 0x01000814, {zs("EDID", "DoorA")}, RecordFlags::Persistent);
    auto image = [&](bool writerOrder, uint32_t bSubBlock) {
        auto cellGroupOf = [&](const Record& c, const G& children) {
            return G{recordBytes(c), group(c.header.formId, GroupType::CellChildren, children)};
        };
        const G aChildren = writerOrder
            ? G{group(0x01000811, GroupType::CellPersistent, G{recordBytes(persistentA)}),
                group(0x01000811, GroupType::CellTemporary, G{recordBytes(refA)})}
            : G{group(0x01000811, GroupType::CellTemporary, G{recordBytes(refA)}),
                group(0x01000811, GroupType::CellPersistent, G{recordBytes(persistentA)})};
        auto aCell = cellGroupOf(a, aChildren);
        auto blockA = group(gridLabel({0, 0}), GroupType::ExteriorBlock,
                            G{group(gridLabel({0, 0}), GroupType::ExteriorSubBlock, G{aCell[0], aCell[1]})});
        auto blockB = group(gridLabel(blockOf(subBlockOf({-40, 3}))), GroupType::ExteriorBlock,
                            G{group(bSubBlock, GroupType::ExteriorSubBlock, G{recordBytes(b)})});
        auto children = group(0x01000810, GroupType::WorldChildren, writerOrder ? G{blockB, blockA} : G{blockA, blockB});
        BinaryWriter w;
        fixtureHeader().toRecord().write(w, false);
        w.writeBytes(group(Signature("WRLD").value, GroupType::Top, G{recordBytes(world), children}));
        return w.take();
    };
    const uint32_t right = gridLabel(subBlockOf({-40, 3}));

    auto same = checkLayout(PluginFile::fromBytes(image(true, right)), true);
    CHECK(same.nested && same.rebuilt);
    CHECK(same.problems.empty());
    CHECK(same.sameOrder);
    CHECK_EQ(same.cells, size_t(2));
    CHECK_EQ(same.children, size_t(2));

    auto other = checkLayout(PluginFile::fromBytes(image(false, right)), true);
    CHECK(other.problems.empty());
    CHECK(!other.sameOrder);

    auto wrong = checkLayout(PluginFile::fromBytes(image(true, gridLabel({-4, 0}))), true);
    CHECK_EQ(wrong.problems.size(), size_t(1));
    CHECK(wrong.problems[0].find("CELL 01000812 at -40,3") != std::string::npos);
    CHECK(wrong.problems[0].find("the writer puts it in") != std::string::npos);
    CHECK(!wrong.rebuilt); // a placement difference stops before the rebuild

    // An empty group (the Creation Kit leaves some) is not a difference; a FormID twice is a fault of
    // the plugin, reported apart.
    {
        BinaryWriter w;
        fixtureHeader().toRecord().write(w, false);
        w.writeBytes(group(Signature("WRLD").value, GroupType::Top,
                           G{recordBytes(world),
                             group(0x01000810, GroupType::WorldChildren,
                                   G{group(gridLabel({0, 0}), GroupType::ExteriorBlock,
                                           G{group(gridLabel({0, 0}), GroupType::ExteriorSubBlock,
                                                   G{recordBytes(a), group(0x01000811, GroupType::CellChildren, G{})})})})}));
        auto empty = checkLayout(PluginFile::fromBytes(w.take()), true);
        CHECK(empty.problems.empty() && empty.defects.empty() && empty.rebuilt);
        CHECK_EQ(empty.emptyGroups, size_t(1));
    }
    {
        BinaryWriter w;
        fixtureHeader().toRecord().write(w, false);
        auto aCell = G{recordBytes(a), group(0x01000811, GroupType::CellChildren,
                                             G{group(0x01000811, GroupType::CellTemporary,
                                                     G{recordBytes(refA), recordBytes(refA)})})};
        w.writeBytes(group(Signature("WRLD").value, GroupType::Top,
                           G{recordBytes(world),
                             group(0x01000810, GroupType::WorldChildren,
                                   G{group(gridLabel({0, 0}), GroupType::ExteriorBlock,
                                           G{group(gridLabel({0, 0}), GroupType::ExteriorSubBlock, aCell)})})}));
        auto twice = checkLayout(PluginFile::fromBytes(w.take()), true);
        CHECK(twice.problems.empty());
        CHECK_EQ(twice.defects.size(), size_t(1));
        CHECK(twice.defects[0].find("REFR 01000813 is in the plugin twice") != std::string::npos);
        CHECK(!twice.rebuilt);
    }

    // Placement only (large plugins): no rebuild, same verdict.
    auto quick = checkLayout(PluginFile::fromBytes(image(false, right)), false);
    CHECK(quick.problems.empty() && !quick.rebuilt);

    // A plugin without cells, worldspaces or topics is not looked at.
    PluginWriter flat;
    flat.setHeader(fixtureHeader());
    flat.addRecord(record("KYWD", 0x01000800, {zs("EDID", "Kw")}));
    CHECK(!checkLayout(PluginFile::fromBytes(flat.build()), true).nested);
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
    // The game's hash (xEdit TwbHash.FO4): CRC32 table, start 0, no final xor, lower case.
    const auto h = ba2Hash("Textures\\Foo\\Bar.DDS");
    CHECK_EQ(h.dirHash, 0xc5fb816du);
    CHECK_EQ(h.nameHash, 0x89be55b8u);
    CHECK_EQ(h.extension, uint32_t('d') | uint32_t('d') << 8 | uint32_t('s') << 16);
    // An archive without a name table (Archive2 -nostrings) is read; its files keep their hashes.
    auto bare = dir / "nostrings.ba2";
    writeGeneralBa2(bare, files, false, false);
    auto ar = Ba2Archive::open(bare);
    CHECK(!ar->hasNames());
    CHECK_EQ(ar->entries().size(), size_t(2));
    CHECK(ar->entries()[0].path.empty());
    CHECK(ar->entries()[0].dirHash == ba2Hash("meshes\\weapons\\gun.nif").dirHash);
    CHECK(ar->extract(1) == files[1].second);
    CHECK_EQ(unnamedBa2Path(ar->entries()[1]), [&] {
        char b[40];
        std::snprintf(b, sizeof(b), "%08x\\%08x.pex", ba2Hash("scripts\\a.pex").dirHash, ba2Hash("scripts\\a.pex").nameHash);
        return std::string(b);
    }());
}

// A quest's script data is decoded in full: script properties, the fragment list and every alias
// with its object and scripts. Nothing is left to scanning.
TEST(decoder_quest_fragments_and_aliases)
{
    Subrecord vmad;
    vmad.signature = Signature("VMAD");
    vmad.data = questVmad(0x01000801, 0x01000800, 0x00000802);
    auto layout = decodeLayout(RecordDefinitions::get(), Signature("QUST"), {vmad});
    CHECK(layout.heuristic.empty());
    CHECK(layout.issues.empty());
    std::vector<uint32_t> values;
    for (const auto& slot : layout.formIdSlots)
        values.push_back(loadLE<uint32_t>(vmad.data.data() + slot.offset));
    CHECK(values == (std::vector<uint32_t>{0x01000801, 0x01000800, 0x00000802}));
    // One byte too many: not trusted, scanned and reported.
    vmad.data.push_back(0);
    auto bad = decodeLayout(RecordDefinitions::get(), Signature("QUST"), {vmad});
    CHECK(!bad.issues.empty());
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
    CHECK(preview::decodeDds(bc7).has_value()); // decoded now (an all-zero block: reserved mode, transparent)
    CHECK_EQ(int(preview::decodeDds(bc7)->rgba[3]), 0);
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

    // Apostrophes may be part of the name (owner's build: RobCo's "Tina's Candy Land.esp.ini" was
    // read as "s candy land.esp", so the gate was missed and the config skipped after compiling).
    names = AssetScanner::pluginNamesInText("Tina's Candy Land.esp.ini");
    CHECK(std::find(names.begin(), names.end(), "tina's candy land.esp") != names.end());
    names = AssetScanner::pluginNamesInText("x='Dr. Phibbs' Limbs.esp'");
    CHECK(std::find(names.begin(), names.end(), "dr. phibbs' limbs.esp") != names.end());
    names = AssetScanner::pluginNamesInText("sPlugin='Quoted.esp'");
    CHECK_EQ(names.size(), size_t(1));
    CHECK_EQ(names[0], std::string("quoted.esp"));
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

// The installable package's zip: names under Data/, deflated or stored, and zip64 records once there
// are more than 65,535 files.
TEST(zip_package_with_zip64)
{
    auto dir = tempDir("zip");
    auto data = dir / "Data";
    std::filesystem::create_directories(data / "many");
    for (int i = 0; i < 66000; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "%05d.txt", i);
        std::ofstream(data / "many" / name) << i;
    }
    std::ofstream(data / "Plugin.esp") << std::string(5000, 'x');
    const auto stats = writeZip(dir / "Package.zip", data, "Data/");
    CHECK_EQ(stats.files, size_t(66001));
    std::ifstream in(dir / "Package.zip", std::ios::binary);
    std::string zip((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(zip.find(std::string("\x50\x4b\x06\x06", 4)) != std::string::npos); // zip64 end of central directory
    CHECK(zip.find("Data/Plugin.esp") != std::string::npos);
    CHECK(zip.find("Data/many/65999.txt") != std::string::npos);
    CHECK(zip.size() < 66001u * 200u);
}

// A previs file's object ID table, found through its header (0x40 count, 0x50 offset); anything that
// does not match the known layout is reported as not recognised, never guessed at.
TEST(previs_file_id_table_is_read_from_its_header)
{
    std::vector<uint8_t> b(0x150 + 12, 0);
    storeLE<uint32_t>(b.data(), 0xD6000012);
    storeLE<uint32_t>(b.data() + 0x08, uint32_t(b.size()));
    storeLE<uint32_t>(b.data() + 0x40, 3);
    storeLE<uint32_t>(b.data() + 0x50, 0x150);
    storeLE<uint32_t>(b.data() + 0x150, 0xFD000026);
    storeLE<uint32_t>(b.data() + 0x154, 0x0700213D);
    storeLE<uint32_t>(b.data() + 0x158, 0xFD000000);
    auto f = readPrevisFile(b);
    CHECK(f.recognised);
    CHECK_EQ(f.ids.size(), size_t(3));
    CHECK(PrevisFile::isCombinedObject(f.ids[0]));
    CHECK_EQ(f.ids[1], 0x0700213Du);
    CHECK(!PrevisFile::isCombinedObject(f.ids[1]));

    auto broken = b;
    storeLE<uint32_t>(broken.data() + 0x40, 4); // one ID past the end
    CHECK(!readPrevisFile(broken).recognised);
    broken = b;
    storeLE<uint32_t>(broken.data() + 0x08, 5); // wrong size field
    CHECK(!readPrevisFile(broken).recognised);
    broken = b;
    broken[0] = 0;
    CHECK(!readPrevisFile(broken).recognised);
    CHECK(!readPrevisFile(std::vector<uint8_t>(16, 0)).recognised);

    // No IDs and no ID table (offset 0), as Diamond City's 00000FE1: nothing to read in a numbering.
    std::vector<uint8_t> empty(0x150, 0);
    storeLE<uint32_t>(empty.data(), 0xD6000012);
    storeLE<uint32_t>(empty.data() + 0x08, uint32_t(empty.size()));
    auto e = readPrevisFile(empty);
    CHECK(e.recognised);
    CHECK(e.ids.empty());
}

namespace {

// A minimal Fallout 4 NIF: a header (string table naming a material) and two blocks, a node and a texture
// set naming two textures directly.
std::vector<uint8_t> nifWithTextureSet()
{
    std::vector<uint8_t> out;
    auto u8 = [&](uint8_t v) { out.push_back(v); };
    auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto bstr = [&](const std::string& s) { u8(static_cast<uint8_t>(s.size() + 1)); out.insert(out.end(), s.begin(), s.end()); u8(0); };
    auto sized = [&](std::vector<uint8_t>& to, const std::string& s) {
        for (int i = 0; i < 4; ++i)
            to.push_back(static_cast<uint8_t>(s.size() >> (8 * i)));
        to.insert(to.end(), s.begin(), s.end());
    };
    const std::string line = "Gamebryo File Format, Version 20.2.0.7\n";
    out.insert(out.end(), line.begin(), line.end());
    u32(0x14020007);
    u8(1);
    u32(12);  // user version
    u32(2);   // blocks
    u32(130); // Fallout 4
    bstr("author");
    bstr("process");
    bstr("export");
    bstr("C:\\max\\file.max");
    const std::vector<std::string> types = {"NiNode", "BSShaderTextureSet"};
    u16(static_cast<uint16_t>(types.size()));
    for (const auto& t : types) {
        u32(static_cast<uint32_t>(t.size()));
        out.insert(out.end(), t.begin(), t.end());
    }
    std::vector<uint8_t> node(8, 0), set;
    for (int i = 0; i < 4; ++i)
        set.push_back(static_cast<uint8_t>(3u >> (8 * i)));
    sized(set, "textures\\Mod\\Rock_d.dds");
    sized(set, "Mod\\Rock_n.dds");
    sized(set, "");
    u16(0);
    u16(1);
    u32(static_cast<uint32_t>(node.size()));
    u32(static_cast<uint32_t>(set.size()));
    const std::vector<std::string> strings = {"Rock", "Materials\\Mod\\Rock.bgsm"};
    u32(static_cast<uint32_t>(strings.size()));
    u32(64);
    for (const auto& s : strings) {
        u32(static_cast<uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    u32(0); // groups
    out.insert(out.end(), node.begin(), node.end());
    out.insert(out.end(), set.begin(), set.end());
    return out;
}

} // namespace

// Textures named in a mesh's texture set blocks (not its string table) are read too.
TEST(nif_texture_sets_are_read)
{
    const auto nif = nifWithTextureSet();
    auto m = preview::readNifInfo(nif);
    CHECK(m.has_value());
    if (!m)
        return;
    CHECK(m->complete);
    CHECK_EQ(m->blockTypes["BSShaderTextureSet"], 1u);
    CHECK(m->strings == (std::vector<std::string>{"Rock", "Materials\\Mod\\Rock.bgsm"}));
    CHECK(m->embeddedTextures == (std::vector<std::string>{"textures\\Mod\\Rock_d.dds", "Mod\\Rock_n.dds"}));
    // Truncated: what was read is kept, the texture set is not invented.
    auto cut = nif;
    cut.resize(cut.size() - 10);
    auto partial = preview::readNifInfo(cut);
    CHECK(partial && partial->complete && partial->embeddedTextures.empty());
}

namespace {

// A DDS file (DX10 header) holding `blocks` 16-byte blocks in one row, 4 pixels high.
std::vector<uint8_t> ddsRow(uint32_t dxgi, const std::vector<uint8_t>& blocks)
{
    const uint32_t w = static_cast<uint32_t>(blocks.size() / 16) * 4, h = 4;
    std::vector<uint8_t> d(148, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
    put(12, h);
    put(16, w);
    put(20, static_cast<uint32_t>(blocks.size()));
    put(28, 1);
    put(76, 32);
    put(80, 4);
    std::memcpy(d.data() + 84, "DX10", 4);
    put(108, 0x1000);
    put(128, dxgi);
    put(132, 3);
    put(140, 1);
    d.insert(d.end(), blocks.begin(), blocks.end());
    return d;
}

// The same pseudo-random blocks the references were made from, one per mode.
std::vector<uint8_t> modeBlocks(size_t n, bool bc7, size_t skipBlocks = 0)
{
    uint32_t state = 12345;
    for (size_t i = 0; i < skipBlocks * 16; ++i)
        state = (state * 1103515245u + 12345u) & 0x7fffffffu;
    std::vector<uint8_t> b(16 * n);
    for (auto& v : b) {
        state = (state * 1103515245u + 12345u) & 0x7fffffffu;
        v = static_cast<uint8_t>((state >> 16) & 0xff);
    }
    static const uint8_t five[12] = {2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
    for (size_t k = 0; k < n; ++k) {
        uint8_t& first = b[k * 16];
        if (bc7)
            first = static_cast<uint8_t>((first & ~((1u << (k + 1)) - 1)) | (1u << k));
        else if (k < 2)
            first = static_cast<uint8_t>((first & ~3u) | k);
        else
            first = static_cast<uint8_t>((first & ~31u) | five[k - 2]);
    }
    return b;
}

std::vector<uint8_t> fromHex(const std::string& h)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

} // namespace

// BC7 and BC6H previews: one block of every mode, compared with an independent decoder's output (Pillow
// 12.3). BC7 is exact; BC6H (unsigned) within one step of rounding. Signed BC6H follows the Direct3D
// specification (transformed endpoints sign-extended), which that decoder does not do, so it is not compared.
TEST(dds_bc7_and_bc6h_decode)
{
    const std::string bc7 =
        "62e326ff3cf6a2ff5f7b72ff5f7b72ffc0d5a4ff30be04ff36c204ff30be04ffa3b968ff4a29b5ffceff42ffceff42ff80ab"
        "5effcdf163ff80ab5effa8cf60ff7a7da66da594e7524c33628a4c1c628a3e7fcdad5a74cded5a74e4ed3e7ff9ad81ad7df7"
        "5ca2515959a14d4a4d9d401b9054479da37e42c7b6a63cef7d2c4d756bde08ff3069beff5f7b72ff49e479ffc0d5a4ffc0d5"
        "a4ffd59dd5ff3cc504ffffb5bdff57d02effffb5bdff94f76bff12de5cffcdf163ffcdf163ffcdf163ff7a4ba66d4c33628a"
        "7a62a66d7a4ba66d208ce46a3e7fb8ad208ccd6a5a74f9ed76aa70c760a3566b6ba6639879ab74d67d2c4d759054479d7d2c"
        "4d757d2c4d752163d6ff3f6fa5ff54eb44ff49e479ff30be04ffb1fd81ffc6c6b2ffcbb8bdffffb5bdff57d02effd36864ff"
        "57d02eff12de5cff80ab5effcdf163ff5b895bff7a94a66da54be752a51ce752a533e752208ce46a0497cd2a5a74f9ed3e7f"
        "b8ad72a86cb94d9d401b79ab74d66ba66398b6a63cef7d2c4d75a37e42c7ba3041c38c8c29ff31d6e7ff54eb44ff37daccff"
        "30be04ff23b704ff36c204ffbbe299ffffb5bdff57d02effd36864ff39bd10ff43bf67ffa6807cffcdf163ffcdf163ff2133"
        "21a521c321a54c7d628aa54be752208ccd6a5a74f9ed5a74cded208ccd6a79ab74d67eac79e86fa768aa529e452db6a63cef"
        "59aa9a69ba3041c39a585ea5";
    const std::string bc6u =
        "ff1043ffff12ffffff1100ffff12ffffe2ff09ff9cff0dffffff01ffffff02ff0b0016ff0b0016ff0a0015ff0a0015ff10ff"
        "ffff10ffffff0fffffff0fffffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffff"
        "ff00ffffff00ffffff00ffff00ffffff00ffffff00ffffff00ffffffff00ceffff008dffff00a2ff0002ffff02ffffffffff"
        "ffffffffffff00ffffffffffffffffffffff55ff29ff55ff29ffff1013ffff0f11ff000c0bffff0f0ffffa0603ffff0505ff"
        "f90603fff80603ffff00b3ffff00b3ffff00b3ffff00b3ffff13ffffff0f00ffff0f00ffff1200ffff0000ffffff02ffffff"
        "0affff0000ff0b0016ff0b0014ff0a0015ff0d0016ff10ffffff10ffffff10ffffff0fffffffff00ffffff00ffffff00ffff"
        "ff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffff00ffffff00ffffff00ffffff00ff"
        "ffffff00a2ff0001ffff0000ffff0002ffffffffffff00ffffff1effffffffffffffffffffffffffffff2bff10ffffffffff"
        "ff1012ffff0f11ff010d0cff010d0cfff90603fffd0504ffff0505fffb0503ffff00b3ffff00b3ffff00b3ffff00b3ffff12"
        "04ffff13ffffff1043ffff1200ffff0000ffff0200ffff0000ffffff05ff0c0016ff0a0015ff0a0015ff0c0016ff10ffffff"
        "10ffffff10ffffff0fffffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff00ffffff02ffffff00"
        "ffffff00ffffff00ffff00ffffff00ffffff00e9ffff00ffffff0002ffff0002ffff0000ffff0002ffffffffffffffffffff"
        "02ffffff37ffffffffffffffffffffff55ff29ffffffffff190e0dff050d0dff520e0effff1215fffa0603fff90603ffff04"
        "05fffd0504ffff00b3ffff00b3ffff00b3ffff00b3ffff12ffffff1200ffff14ffffff14ffffffff01ffffff02ffffff03ff"
        "ffff01ff0a0015ff0b0014ff0c0016ff0c0016ff10ffffff10ffffff0fffffff10ffffffff00ffffff00ffffff00ffffff00"
        "ffffff00ffffff00ffffff00ffffff00ffffff02ffffff02ffffff00ffffff00ffff00ffffff00ffffff00ffffff00ffffff"
        "0001ffff0002ffff0000ffff0000ffffffffffff00ffffff1dffffffffffffff55ff29ffffffffffffffffffffffffffff0e"
        "0effff1114ffff0f0fff000c0bfff90603ffff0505fff90603ffff0405ffff00b3ffff00b3ffff00b3ffff00b3ff";
    auto img = preview::decodeDds(ddsRow(98, modeBlocks(8, true)));
    CHECK(img.has_value());
    if (img)
        CHECK(img->rgba == fromHex(bc7));
    auto hdr = preview::decodeDds(ddsRow(95, modeBlocks(14, false, 8))); // the stream continues after BC7
    CHECK(hdr.has_value());
    if (hdr) {
        const auto want = fromHex(bc6u);
        CHECK_EQ(hdr->rgba.size(), want.size());
        int worst = 0;
        for (size_t i = 0; i < want.size() && i < hdr->rgba.size(); ++i)
            worst = std::max(worst, std::abs(int(hdr->rgba[i]) - int(want[i])));
        CHECK(worst <= 1);
    }
    CHECK(preview::readDdsInfo(ddsRow(99, modeBlocks(8, true)))->decodable);
    CHECK(preview::readDdsInfo(ddsRow(96, modeBlocks(14, false)))->decodable);
}

// A Buffout 4 crash log read against a build: FormIDs in an output are translated to the original mod through
// the FormID map, using the log's own load order (full and light slots); a record an output changes is
// marked; one in an output but not in the map is counted apart.
TEST(crash_log_names_the_original_mods)
{
    const std::string log =
        "Fallout 4 v1.10.163\n"
        "Buffout 4 v1.28.6\n"
        "\n"
        "Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF6A1B2C3D4 Fallout4.exe+1234567\n"
        "\n"
        "PROBABLE CALL STACK:\n"
        "\t[ 0] 0x7FF6A1B2C3D4 Fallout4.exe+1234567\n"
        "\n"
        "REGISTERS:\n"
        "\tRAX 0x1F2E3D4C5B60 (TESObjectWEAP*)\n"
        "\t\tName: \"Laser Musket\"\n"
        "\t\tFlags: 0x00000000\n"
        "\t\tFormID: 0x0A000805\n"
        "\t\tFormType: kWEAP (43)\n"
        "\t\tFile: \"Dummy_Plugin_01.esp\"\n"
        "\tRBX 0x1F2E3D4C5B70 (TESObjectREFR*)\n"
        "\t\tName: \"Sanctuary Hills\"\n"
        "\t\tFormID: 0x0001F669\n"
        "\t\tFormType: kREFR (64)\n"
        "\t\tModified by: \"Fallout4.esm\" -> \"Dummy_Plugin_01.esp\"\n"
        "\tRCX 0x1F2E3D4C5B80 (BGSKeyword*)\n"
        "\t\tFormID: 0xFE001801\n"
        "\t\tFormType: kKYWD (4)\n"
        "\tRDX 0x1F2E3D4C5B90 (TESNPC*)\n"
        "\t\tFormID: 0x0A000999\n"
        "\n"
        "STACK:\n"
        "\t[RSP+38  ] 0x1F2E3D4C5BA0 (TESObjectWEAP*)\n"
        "\t\tFormID: 0x0A000805\n"
        "\n"
        "PLUGINS:\n"
        "\t[00]     Fallout4.esm\n"
        "\t[0A]     Dummy_Plugin_01.esp\n"
        "\t[FE:001] Dummy_Plugin_02.esp\n";
    std::vector<FormMapping> map(2);
    map[0].sourcePlugin = "LaserMod.esp";
    map[0].sourceObjectId = 0x1234;
    map[0].outputPlugin = "Dummy_Plugin_01.esp";
    map[0].objectId = 0x805;
    map[0].sig = Signature("WEAP");
    map[0].editorId = "LaserMusketMk2";
    map[1].sourcePlugin = "KeywordPack.esp";
    map[1].sourceObjectId = 0x810;
    map[1].outputPlugin = "Dummy_Plugin_02.esp";
    map[1].objectId = 0x801;
    map[1].sig = Signature("KYWD");
    const auto r = diagnostics::translateCrashLog(log, map, {}, {});
    CHECK(r.recognised);
    CHECK_EQ(r.pluginsListed, size_t(3));
    CHECK_EQ(r.outputsLoaded.size(), size_t(2));
    CHECK_EQ(r.refs.size(), size_t(4)); // the weapon once
    CHECK_EQ(r.translated, size_t(2));
    CHECK_EQ(r.untranslated, size_t(1)); // 0A000999: an output, not in the map
    if (r.refs.size() == 4) {
        CHECK_EQ(r.refs[0].source, std::string("LaserMod.esp"));
        CHECK_EQ(r.refs[0].sourceObjectId, 0x1234u);
        CHECK_EQ(r.refs[0].name, std::string("Laser Musket"));
        CHECK_EQ(r.refs[0].type, std::string("kWEAP (43)"));
        CHECK(r.refs[0].where.find("RAX") != std::string::npos);
        CHECK_EQ(r.refs[1].plugin, std::string("Fallout4.esm"));
        CHECK(!r.refs[1].inOutput);
        CHECK(r.refs[1].changedByOutput);
        CHECK_EQ(r.refs[2].plugin, std::string("Dummy_Plugin_02.esp")); // light slot FE:001
        CHECK_EQ(r.refs[2].objectId, 0x801u);
        CHECK_EQ(r.refs[2].source, std::string("KeywordPack.esp"));
    }
    const std::string text = diagnostics::formatCrashReport(r);
    CHECK(text.find("compiled from LaserMod.esp 001234 (LaserMusketMk2) [WEAP]") != std::string::npos);
    CHECK(text.find("changed by the compiled outputs") != std::string::npos);
    CHECK(!diagnostics::translateCrashLog("just some text", map, {}, {}).recognised);
}

namespace {

// Deterministic damage: flipped bytes, huge or zero 32-bit values (sizes, counts, offsets), truncation and
// repeated chunks, a few at a time.
struct Damage {
    uint32_t state;
    uint32_t next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    std::vector<uint8_t> apply(std::vector<uint8_t> b)
    {
        const int edits = 1 + static_cast<int>(next() % 6);
        for (int e = 0; e < edits && !b.empty(); ++e) {
            const size_t at = next() % b.size();
            switch (next() % 6) {
            case 0: b[at] ^= static_cast<uint8_t>(1u << (next() % 8)); break;
            case 1: b[at] = static_cast<uint8_t>(next()); break;
            case 2:
            case 3: {
                static const uint32_t values[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0, 0x10000, 0xFFFF};
                const uint32_t v = values[next() % 6];
                for (size_t k = 0; k < 4 && at + k < b.size(); ++k)
                    b[at + k] = static_cast<uint8_t>(v >> (8 * k));
                break;
            }
            case 4: b.resize(at); break;
            default: {
                const size_t len = std::min<size_t>(1 + next() % 32, b.size() - at);
                std::vector<uint8_t> chunk(b.begin() + long(at), b.begin() + long(at + len));
                b.insert(b.begin() + long(at), chunk.begin(), chunk.end());
            }
            }
        }
        return b;
    }
};

// Runs `read` on damaged variants. An exception is a fine answer; running out of memory is not (a damaged
// count or size must be checked against the data, not allocated): those are reported with the reader's name.
template <class F>
size_t survive(const char* what, const std::vector<uint8_t>& valid, uint32_t seed, size_t rounds, F&& read)
{
    Damage d{seed};
    size_t rejected = 0, hugeAllocations = 0;
    for (size_t i = 0; i < rounds; ++i) {
        const auto bytes = d.apply(valid);
        try {
            read(bytes);
        } catch (const std::bad_alloc&) {
            if (hugeAllocations++ == 0)
                std::printf("  %s: variant %zu asked for more memory than there is\n", what, i);
        } catch (const std::length_error&) {
            if (hugeAllocations++ == 0)
                std::printf("  %s: variant %zu asked for an impossible size\n", what, i);
        } catch (const std::exception&) {
            ++rejected;
        }
    }
    CHECK_EQ(hugeAllocations, size_t(0));
    return rejected;
}

std::vector<uint8_t> readBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

} // namespace

// Every reader of mod files meets damaged input with an error, never a crash: plugins (records, compressed
// records, subrecords, field layouts), BA2 archives, meshes, textures, materials, scripts, previs files,
// string tables, crash logs and recorder logs. Hundreds of deterministic variants of a valid file each.
TEST(readers_survive_damaged_files)
{
    const auto dir = std::filesystem::temp_directory_path() / "fo4mx_fuzz";
    std::filesystem::create_directories(dir);
    const auto& defs = RecordDefinitions::get();

    // A plugin with a compressed record, a weapon, a list and a cell with a reference.
    std::vector<uint8_t> big(9000, 'x');
    big.back() = 0;
    writePlugin(dir / "Fuzz.esp", {"Fallout4.esm"},
                {record("KYWD", 0x01000800, {zs("EDID", "FuzzKeyword")}),
                 record("FLST", 0x01000801, {zs("EDID", "FuzzList"), u32("LNAM", 0x01000800), u32("LNAM", 0x00000801)}),
                 record("BOOK", 0x01000802, {zs("EDID", "FuzzBook"), raw("DESC", big)}),
                 record("CELL", 0x01000803, {zs("EDID", "FuzzCell")}),
                 record("REFR", 0x01000804, {zs("EDID", "FuzzRef"), u32("NAME", 0x01000802)})},
                0, true);
    const auto plugin = readBytes(dir / "Fuzz.esp");
    CHECK(plugin.size() > 100);
    survive("plugin", plugin, 1, 600, [&](const std::vector<uint8_t>& b) {
        const auto file = PluginFile::fromBytes(b);
        file.forEachRecord([&](const RecordLocation& loc) {
            const Record rec = file.recordAt(loc.offset);
            (void)decodeLayout(defs, rec.signature(), rec.subrecords);
            return true;
        });
    });

    std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
        {"meshes\\fuzz\\a.nif", std::vector<uint8_t>(300, 7)}, {"scripts\\fuzz.pex", std::vector<uint8_t>(64, 1)}};
    for (const bool compressed : {false, true}) {
        writeGeneralBa2(dir / "Fuzz - Main.ba2", files, compressed);
        const auto ba2 = readBytes(dir / "Fuzz - Main.ba2");
        survive("BA2", ba2, compressed ? 3 : 2, 300, [&](const std::vector<uint8_t>& b) {
            const auto p = dir / "Damaged - Main.ba2";
            {
                std::ofstream out(p, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
            }
            auto a = Ba2Archive::open(p);
            for (size_t i = 0; a && i < a->entries().size(); ++i)
                (void)a->extract(i);
        });
    }

    survive("NIF", nifWithTextureSet(), 4, 600, [](const std::vector<uint8_t>& b) { (void)preview::readNifInfo(b); });
    for (uint32_t fmt : {71u, 77u, 83u, 95u, 98u, 28u})
        survive("DDS", ddsRow(fmt, modeBlocks(8, fmt == 98)), 5 + fmt, 200, [](const std::vector<uint8_t>& b) {
            (void)preview::readDdsInfo(b);
            (void)preview::decodeDds(b);
        });
    {
        std::vector<uint8_t> bgsm = {'B', 'G', 'S', 'M', 2, 0, 0, 0};
        const std::string tex = "textures\\fuzz\\a_d.dds";
        bgsm.push_back(static_cast<uint8_t>(tex.size() + 1));
        bgsm.insert(bgsm.end(), {0, 0, 0});
        bgsm.insert(bgsm.end(), tex.begin(), tex.end());
        bgsm.push_back(0);
        survive("material", bgsm, 6, 300, [](const std::vector<uint8_t>& b) { (void)preview::readMaterialInfo(b); });
    }
    survive("PEX", testScript(0x800, "Fuzz.esp", {}), 7, 600, [](const std::vector<uint8_t>& b) { (void)pex::scan(b); });
    {
        std::vector<uint8_t> uvd(0x150 + 12, 0);
        storeLE<uint32_t>(uvd.data(), 0xD6000012);
        storeLE<uint32_t>(uvd.data() + 0x08, uint32_t(uvd.size()));
        storeLE<uint32_t>(uvd.data() + 0x40, 3);
        storeLE<uint32_t>(uvd.data() + 0x50, 0x150);
        survive("previs", uvd, 8, 300, [](const std::vector<uint8_t>& b) { (void)readPrevisFile(b); });
    }
    {
        // A .STRINGS file: count, data size, directory (id, offset), zero-terminated strings.
        std::vector<uint8_t> st(8 + 16, 0);
        storeLE<uint32_t>(st.data(), 2);
        const std::string a = "Laser Musket", b2 = "Fuzz";
        storeLE<uint32_t>(st.data() + 4, uint32_t(a.size() + b2.size() + 2));
        storeLE<uint32_t>(st.data() + 8, 1);
        storeLE<uint32_t>(st.data() + 12, 0);
        storeLE<uint32_t>(st.data() + 16, 2);
        storeLE<uint32_t>(st.data() + 20, uint32_t(a.size() + 1));
        st.insert(st.end(), a.begin(), a.end());
        st.push_back(0);
        st.insert(st.end(), b2.begin(), b2.end());
        st.push_back(0);
        survive("strings", st, 9, 300, [](const std::vector<uint8_t>& b) {
            StringTables t;
            t.load(StringFileKind::Strings, b);
        });
    }
    {
        const std::string crash = "Unhandled exception \"X\" at 0x1\nREGISTERS:\n\tRAX 0x1 (TESForm*)\n\t\tFormID: "
                                  "0x0A000805\n\t\tName: \"A\"\nPLUGINS:\n\t[00] Fallout4.esm\n\t[FE:001] B.esl\n";
        survive("crash log", std::vector<uint8_t>(crash.begin(), crash.end()), 10, 400, [](const std::vector<uint8_t>& b) {
            (void)diagnostics::translateCrashLog(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), {},
                                                 {}, {});
        });
        const std::string rec = "{\"type\":\"session\",\"game\":\"1.10.163\"}\n{\"type\":\"plugin\",\"name\":\"A.esp\"}\n";
        survive("recorder log", std::vector<uint8_t>(rec.begin(), rec.end()), 11, 400, [](const std::vector<uint8_t>& b) {
            (void)diagnostics::parseRecorderLog(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
        });
    }
    std::filesystem::remove_all(dir);
}

// PathLookup finds files whatever their letter case, like findPathCaseInsensitive, listing each folder once.
TEST(path_lookup_is_case_insensitive)
{
    const auto dir = std::filesystem::temp_directory_path() / "fo4mx_pathlookup";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "Scripts" / "Sub");
    std::ofstream(dir / "Scripts" / "Sub" / "MyQuest.pex") << "x";
    PathLookup lookup;
    CHECK_EQ(lookup.find(dir, "scripts\\sub\\myquest.pex"), dir / "Scripts" / "Sub" / "MyQuest.pex");
    // The same file (on Windows findPathCaseInsensitive may keep the spelling it was given).
    CHECK(std::filesystem::equivalent(lookup.find(dir, "SCRIPTS/SUB/MYQUEST.PEX"),
                                      findPathCaseInsensitive(dir, "scripts\\sub\\myquest.pex")));
    CHECK(lookup.find(dir, "scripts\\other.pex").empty());
    CHECK(lookup.find(dir, "scripts\\..\\x").empty());
    std::filesystem::remove_all(dir);
}

// Support bundle: the export's notes, reports and build records, logs, load order, crash log and its reading,
// settings; never plugins or archives; the home folder and user name taken out of every text file.
TEST(support_bundle_collects_and_redacts)
{
    const auto dir = std::filesystem::temp_directory_path() / "fo4mx_bundle";
    std::filesystem::remove_all(dir);
    const auto pkg = dir / "Multiplexer Output 211126 120000";
    std::filesystem::create_directories(pkg / "Data");
    std::filesystem::create_directories(pkg / "Multiplexer Reports");
    const std::string home = "C:\\Users\\Romewolf";
    std::ofstream(pkg / "Multiplexer_Instructions.txt") << "Install from " << home << "\\Downloads\\x.zip\n";
    std::ofstream(pkg / "Data" / "Multiplexer_FormMap.tsv") << "# map\nModA.esp\t000800\tDummy_Plugin_01.esp\t000800\n";
    std::ofstream(pkg / "Data" / "Dummy_Plugin_01.esp") << "TES4";
    std::ofstream(pkg / "Data" / "ModA - Main.ba2") << "BTDX";
    std::ofstream(pkg / "Multiplexer Reports" / "ValidationReport.txt") << "path C:/Users/Romewolf/Games\n";
    std::ofstream(dir / "multiplexer.log") << "scanning " << home << "\\Fallout 4\\Data\n";
    std::ofstream(dir / "plugins.txt") << "*ModA.esp\n";
    SupportBundleInputs in;
    in.packageRoot = pkg;
    in.logs = {dir / "multiplexer.log", dir / "missing.log"};
    in.loadOrder = {dir / "plugins.txt"};
    in.settings = {{"install/gamePath", home + "\\Fallout 4"}};
    in.redact = {home};
    const auto r = writeSupportBundle(in, dir / "bundle.zip");
    CHECK(std::filesystem::file_size(dir / "bundle.zip") > 100);
    auto has = [&](const std::string& f) { return std::find(r.included.begin(), r.included.end(), f) != r.included.end(); };
    CHECK(has("export/Multiplexer_Instructions.txt"));
    CHECK(has("export/Multiplexer_FormMap.tsv"));
    CHECK(has("reports/ValidationReport.txt"));
    CHECK(has("logs/multiplexer.log"));
    CHECK(has("load order/plugins.txt"));
    CHECK(has("settings.txt"));
    CHECK(has("README.txt"));
    for (const auto& f : r.included)
        CHECK(!iendsWith(f, ".esp") && !iendsWith(f, ".ba2"));
    CHECK(std::find(r.missing.begin(), r.missing.end(), "logs/missing.log") != r.missing.end());
    CHECK_EQ(redactText("at " + home + "\\Data and C:/Users/Romewolf/x", {home}), std::string("at <user>\\Data and <user>/x"));
    CHECK_EQ(redactText("mod by Romewolf in \\Romewolf\\", {"\\Romewolf\\"}), std::string("mod by Romewolf in \\<user>\\"));
    std::filesystem::remove_all(dir);
}

// Build comparison: kept, new, moved to another output, renumbered, gone; plugins compiled for the first
// time and back in the load order.
TEST(build_comparison_sorts_every_record)
{
    auto m = [](const char* src, uint32_t sid, const char* out, uint32_t oid) {
        FormMapping f;
        f.sourcePlugin = src;
        f.sourceObjectId = sid;
        f.outputPlugin = out;
        f.objectId = oid;
        return f;
    };
    BuildSide before{{m("A.esp", 0x800, "Dummy_1.esp", 0x800), m("A.esp", 0x801, "Dummy_1.esp", 0x801),
                      m("B.esp", 0x800, "Dummy_1.esp", 0x802), m("C.esp", 0x800, "Dummy_2.esp", 0x800)},
                     {"A.esp", "B.esp", "C.esp"},
                     {"Dummy_1.esp", "Dummy_2.esp"}};
    BuildSide after{{m("a.esp", 0x800, "dummy_1.esp", 0x800), m("A.esp", 0x801, "Dummy_1.esp", 0x810),
                     m("B.esp", 0x800, "Dummy_3.esp", 0x800), m("D.esp", 0x800, "Dummy_1.esp", 0x803)},
                    {"A.esp", "B.esp", "D.esp"},
                    {"Dummy_1.esp", "Dummy_3.esp"}};
    const auto c = compareBuilds(before, after);
    CHECK_EQ(c.kept, size_t(1)); // letter case does not count
    CHECK_EQ(c.renumbered.size(), size_t(1));
    CHECK_EQ(c.moved.size(), size_t(1));
    CHECK_EQ(c.moved[0].after.outputPlugin, std::string("Dummy_3.esp"));
    CHECK_EQ(c.removed.size(), size_t(1));
    CHECK_EQ(c.removed[0].sourcePlugin, std::string("C.esp"));
    CHECK_EQ(c.added.size(), size_t(1));
    CHECK(c.newlyCompiled == std::vector<std::string>{"D.esp"});
    CHECK(c.returned == std::vector<std::string>{"C.esp"});
    CHECK(c.outputsAdded == std::vector<std::string>{"Dummy_3.esp"});
    CHECK(c.outputsRemoved == std::vector<std::string>{"Dummy_2.esp"});
    CHECK_EQ(c.formIdChanges(), size_t(3));
    const auto text = formatBuildComparison(c, 10);
    CHECK(text.find("back in the load order") != std::string::npos || text.find("Back in the load order") != std::string::npos);
    CHECK(text.find("C.esp") != std::string::npos);
    CHECK(compareBuilds(before, before).same());
}

// The FormID in a file name the game finds by plugin name: one 8-digit token, load order byte 00.
TEST(file_formid_token_is_strict)
{
    CHECK(fileFormIdToken("tree_0001a2b3.dds") && fileFormIdToken("tree_0001a2b3.dds")->objectId == 0x1a2b3u);
    CHECK(fileFormIdToken("0001A2B3_n.dds") && fileFormIdToken("0001A2B3_n.dds")->pos == 0);
    CHECK(fileFormIdToken("tree_0001a2b3_n.dds")->objectId == 0x1a2b3u);
    CHECK(!fileFormIdToken("tree.dds"));
    CHECK(!fileFormIdToken("01000800.dds"));            // a load order byte
    CHECK(!fileFormIdToken("00000800_00000801.dds"));   // two: not sure which
    CHECK(!fileFormIdToken("tree00000800.dds"));        // not its own word
    CHECK(!fileFormIdToken("tree_000008001.dds"));      // nine digits
}

// Papyrus log: FormIDs read with the load order, compiled records translated, errors counted by message.
TEST(papyrus_log_names_the_original_mods)
{
    diagnostics::RuntimeLoadOrder lo;
    lo.full = {"Fallout4.esm", "ModA.esp", "Dummy_Plugin_01.esp"};
    lo.light = {"Small.esl"};
    FormMapping m;
    m.sourcePlugin = "Weapons.esp";
    m.sourceObjectId = 0x812;
    m.outputPlugin = "Dummy_Plugin_01.esp";
    m.objectId = 0x000ABC;
    m.editorId = "W_Gun";
    m.sig = Signature("WEAP");
    const std::string log =
        "[11/21/2026 - 04:00:00PM] Papyrus log opened (PC)\n"
        "[11/21/2026 - 04:00:01PM] error: Cannot call GetValue() on a None object, aborting function call\n"
        "stack:\n"
        "\t[W_Quest (02000ABC)].W_Script.OnInit() - \"W_Script.psc\" Line 12\n"
        "[11/21/2026 - 04:00:02PM] error: Cannot call GetValue() on a None object, aborting function call\n"
        "stack:\n"
        "\t[Actor < (00000014)>].Actor.OnLoad() - \"<native>\" Line ?\n"
        "[11/21/2026 - 04:00:03PM] warning: Property Foo on script Bar attached to  (FE000800) cannot be bound\n"
        "[11/21/2026 - 04:00:04PM] error: Unable to find form 0x12 in Missing.esp\n";
    const auto r = diagnostics::translatePapyrusLog(log, lo, {m}, {});
    CHECK(r.recognised);
    CHECK_EQ(r.errors, size_t(3));
    CHECK_EQ(r.warnings, size_t(1));
    CHECK_EQ(r.topErrors.size(), size_t(2));
    CHECK_EQ(r.topErrors[0].count, size_t(2));
    CHECK_EQ(r.translated, size_t(1));
    CHECK(!r.refs.empty());
    if (!r.refs.empty()) {
        CHECK_EQ(r.refs[0].formId, std::string("02000ABC"));
        CHECK_EQ(r.refs[0].source, std::string("Weapons.esp"));
        CHECK_EQ(r.refs[0].name, std::string("W_Quest"));
        CHECK_EQ(r.refs[0].inErrors, size_t(1));
    }
    bool player = false, light = false;
    for (const auto& ref : r.refs) {
        player |= ref.formId == "00000014" && ref.plugin == "Fallout4.esm" && ref.name == "Actor";
        light |= ref.formId == "FE000800" && ref.plugin == "Small.esl" && ref.objectId == 0x800;
    }
    CHECK(player);
    CHECK(light);
    const auto text = diagnostics::formatPapyrusReport(r);
    CHECK(text.find("compiled from Weapons.esp 000812 (W_Gun) [WEAP]") != std::string::npos);
    CHECK(!diagnostics::translatePapyrusLog("hello\n", lo, {}, {}).recognised);
}
