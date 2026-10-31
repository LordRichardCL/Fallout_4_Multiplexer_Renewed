#include "Fixtures.hpp"
#include "papyrus/Pex.hpp"
#include "Test.hpp"

#include "assets/AssetPreview.hpp"
#include "assets/AssetScanner.hpp"
#include "ba2/Ba2Archive.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/GroupLayout.hpp"
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
