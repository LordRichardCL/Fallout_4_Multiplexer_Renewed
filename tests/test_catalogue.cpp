#include "Test.hpp"

#include "esp/FieldDecoder.hpp"
#include "esp/PluginWriter.hpp"
#include "esp/RecordCatalogue.hpp"
#include "esp/RecordDefinitions.hpp"
#include "util/Binary.hpp"
#include "util/Compression.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

using namespace fo4mx;

namespace {

std::string readText(const char* relative)
{
    std::ifstream in(std::string(FO4MX_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// The catalogue lists every type of the supplied xEdit record list, under the same name.
TEST(catalogue_covers_the_xedit_record_list)
{
    const auto& cat = RecordCatalogue::get();
    std::istringstream list(readText("resources/fallout4_xedit_record_types.csv"));
    std::string line;
    size_t rows = 0;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0)
            line.erase(0, 3);
        if (line.empty() || line.rfind("Signature,", 0) == 0)
            continue;
        auto comma = line.find(',');
        auto e = cat.find(Signature::fromString(line.substr(0, comma)));
        CHECK(e != nullptr);
        CHECK_EQ(e->name, line.substr(comma + 1));
        ++rows;
    }
    CHECK_EQ(rows, size_t(139));
    CHECK_EQ(cat.entries().size(), size_t(147)); // plus the eight placed projectile reference types
    for (const auto& e : cat.entries())
        CHECK(!e.category.empty());
}

// Compiling needs both a layout and Specialised or Winner in the catalogue; the two must agree.
TEST(catalogue_strategy_matches_the_layouts)
{
    const auto& defs = RecordDefinitions::get();
    CHECK(defs.strategyWithoutLayout().empty());
    size_t specialised = 0, winner = 0;
    for (const auto& e : defs.catalogue().entries()) {
        specialised += e.strategy == MergeStrategy::Specialised;
        winner += e.strategy == MergeStrategy::Winner;
    }
    CHECK_EQ(specialised, size_t(61));
    CHECK_EQ(winner, size_t(23)); // stages 4 and 5, confirmed by a real load order
    CHECK_EQ(specialised + winner, defs.compilableTypes().size());
    CHECK_EQ(defs.recordName(Signature("MISC")), std::string("Misc. Item"));
    CHECK_EQ(defs.recordName(Signature("XYZW")), std::string("XYZW"));

    // Setting Preserve in the catalogue turns compiling a type off.
    std::string text = readText("resources/record_catalogue.csv");
    auto pos = text.find("\nWEAP,");
    CHECK(pos != std::string::npos);
    auto spec = text.find("Specialised", pos);
    text.replace(spec, std::string("Specialised").size(), "Preserve");
    auto edited = RecordCatalogue::parse(text);
    RecordDefinitions d = defs;
    d.applyCatalogue(edited);
    CHECK(!d.isCompilable(Signature("WEAP")));
    CHECK(d.isCompilable(Signature("ARMO")));
    CHECK(d.strategyWithoutLayout().empty());

    // Marking a type without a layout Specialised does not make it compilable; it is reported.
    std::string promoted = readText("resources/record_catalogue.csv");
    pos = promoted.find("\nNPC_,");
    auto pres = promoted.find("Preserve", pos);
    promoted.replace(pres, std::string("Preserve").size(), "Specialised");
    auto promotedCat = RecordCatalogue::parse(promoted);
    RecordDefinitions d2 = defs;
    d2.applyCatalogue(promotedCat);
    CHECK(!d2.isCompilable(Signature("NPC_")));
    CHECK_EQ(d2.strategyWithoutLayout().size(), size_t(1));
}

TEST(catalogue_rejects_malformed_rows)
{
    bool threw = false;
    try {
        RecordCatalogue::parse("Signature,Name,Category,Placement,Strategy\nWEAP,Weapon,Items,somewhere,Preserve\n");
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        RecordCatalogue::parse("Signature,Name,Category,Placement,Strategy\nWEAP,Weapon,Items,top,Preserve\n"
                               "WEAP,Weapon,Items,top,Preserve\n");
    } catch (const FormatError&) {
        threw = true;
    }
    CHECK(threw);
    auto quoted = RecordCatalogue::parse("Signature,Name,Category,Placement,Strategy,Group Order,Allocation Order,Notes\n"
                                         "WEAP,Weapon,Items,top,Preserve,3,,\"Has \"\"quotes\"\", and commas\"\n");
    CHECK_EQ(quoted.find(Signature("WEAP"))->notes, std::string("Has \"quotes\", and commas"));
    CHECK_EQ(quoted.groupOrder().size(), size_t(1));
}

// The written group order is xEdit's Fallout 4 order; allocation keeps its established grouping.
TEST(catalogue_group_and_allocation_order)
{
    const auto& order = PluginWriter::topLevelOrder();
    CHECK_EQ(order.size(), size_t(132));
    CHECK(order.front() == Signature("GMST"));
    CHECK(order.back() == Signature("OVIS"));
    for (Signature s : order) { // CELL has a top-level group too (interior cell blocks)
        auto p = RecordCatalogue::get().find(s)->placement;
        CHECK(p == RecordPlacement::Top || p == RecordPlacement::TopWithChildren ||
              (p == RecordPlacement::Cell && s == Signature("CELL")));
    }
    CHECK(std::find(order.begin(), order.end(), Signature("REFR")) == order.end());

    const auto& cat = RecordCatalogue::get();
    CHECK_EQ(cat.allocationGroups(), 19);
    CHECK_EQ(cat.find(Signature("KYWD"))->allocationOrder, 0);
    CHECK_EQ(cat.find(Signature("COBJ"))->allocationOrder, 17);
    CHECK_EQ(cat.find(Signature("ARMA"))->allocationOrder, -1);
}

TEST(asset_roles_come_from_the_definitions)
{
    const auto& d = RecordDefinitions::get();
    CHECK(d.assetRole(Signature("WEAP"), Signature("MODL")) == AssetRole::Mesh);
    CHECK(d.assetRole(Signature("ARMO"), Signature("MODL")) == AssetRole::None);
    CHECK(d.assetRole(Signature("ARMA"), Signature("MODL")) == AssetRole::None);
    CHECK(d.assetRole(Signature("TXST"), Signature("TX03")) == AssetRole::Texture);
    CHECK(d.assetRole(Signature("TXST"), Signature("MNAM")) == AssetRole::Material);
    CHECK(d.assetRole(Signature("MSWP"), Signature("BNAM")) == AssetRole::None);
    CHECK(d.assetRole(Signature("SNDR"), Signature("ANAM")) == AssetRole::Sound);
    CHECK(d.assetRole(Signature("AMMO"), Signature("NAM1")) == AssetRole::Mesh);
    CHECK(d.assetRole(Signature("MISC"), Signature("MICO")) == AssetRole::Icon);
    CHECK_EQ(std::string(assetRoot(AssetRole::Icon)), std::string("textures\\"));
}

TEST(zlib_inflate_prefix)
{
    std::vector<uint8_t> data(100000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 7);
    auto z = zlibDeflate(data);
    auto head = zlibInflatePrefix(z, 10);
    CHECK_EQ(head.size(), size_t(10));
    CHECK(std::equal(head.begin(), head.end(), data.begin()));
    auto all = zlibInflatePrefix(z, 200000);
    CHECK_EQ(all.size(), data.size());
}

// Stage 5 layouts: binary IMAD signatures and record-specific overrides of common subrecords.
TEST(stage5_layouts)
{
    const auto& d = RecordDefinitions::get();
    for (const char* t : {"ADDN", "HDPT", "IMAD", "LTEX", "GRAS", "IMGS", "BPTD"}) {
        CHECK(d.strategy(Signature::fromString(t)) == MergeStrategy::Winner);
        CHECK(d.isCompilable(Signature::fromString(t)));
        CHECK(!d.mergesFields(Signature::fromString(t)));
    }
    const Signature imad("IMAD");
    // "\x00IAD" and "@IAD" (index 0 and 0x40) up to "\x14IAD" / "TIAD".
    CHECK(d.field(imad, Signature(0x44414900u)).kind == FieldKind::NoRefs);
    CHECK(d.field(imad, Signature(0x44414954u)).kind == FieldKind::NoRefs);
    CHECK(d.field(imad, Signature(0x44414915u)).kind == FieldKind::Unknown); // index 0x15 does not exist
    // YNAM is a FormID in most records, an interpolator in IMAD.
    CHECK(d.field(Signature("WEAP"), Signature("YNAM")).kind == FieldKind::FormId);
    CHECK(d.field(imad, Signature("YNAM")).kind == FieldKind::NoRefs);
    const auto& bpnd = d.field(Signature("BPTD"), Signature("BPND"));
    CHECK_EQ(bpnd.requiredSize, uint16_t(101));
    CHECK_EQ(bpnd.offsets.size(), size_t(11));
}

// Layouts of this update: structural guards, position-dependent subrecords and string tables.
TEST(update_layouts_and_guards)
{
    const auto& d = RecordDefinitions::get();
    // Requested types are Candidates; those stored inside cells, worldspaces or topics can never compile.
    for (const char* t : {"REFR", "ACHR", "PGRE", "PARW", "PMIS", "PHZD", "LAND", "INFO", "WRLD", "DIAL"}) {
        CHECK(d.strategy(Signature::fromString(t)) == MergeStrategy::Candidate);
        CHECK(d.supportSummary(Signature::fromString(t)).find("not compiled") != std::string::npos);
    }
    CHECK(d.strategy(Signature("NPC_")) == MergeStrategy::Candidate);
    CHECK(d.overridesOnly(Signature("NPC_")));
    CHECK(d.strategy(Signature("WTHR")) == MergeStrategy::Candidate);
    CHECK(d.strategy(Signature("QUST")) == MergeStrategy::Preserve);

    // Same subrecord, different meaning per record.
    CHECK(d.field(Signature("RACE"), Signature("ZNAM")).kind == FieldKind::ZString);
    CHECK(d.field(Signature("NPC_"), Signature("ZNAM")).kind == FieldKind::FormId);
    CHECK(d.field(Signature("RACE"), Signature("NAM2")).kind == FieldKind::Struct); // empty marker or FormID
    CHECK(d.field(Signature("ACHR"), Signature("TNAM")).kind == FieldKind::FormId); // topic
    CHECK(d.field(Signature("REFR"), Signature("TNAM")).kind == FieldKind::NoRefs); // map marker
    // String tables as in xEdit: INFO text from ILSTRINGS except RNAM; LSCR DESC from STRINGS.
    CHECK(d.field(Signature("INFO"), Signature("NAM1")).kind == FieldKind::ILString);
    CHECK(d.field(Signature("INFO"), Signature("RNAM")).kind == FieldKind::LString);
    CHECK(d.field(Signature("LSCR"), Signature("DESC")).kind == FieldKind::LString);
    CHECK(d.field(Signature("ARMO"), Signature("DESC")).kind == FieldKind::DLString);
    // WTHR cloud layers 00TX .. O0TX.
    for (const char* sub : {"00TX", "90TX", ":0TX", "@0TX", "A0TX", "O0TX"})
        CHECK(d.field(Signature("WTHR"), Signature::fromString(sub)).kind == FieldKind::ZString);

    // Turning a cell child into Winner in the catalogue still does not compile it.
    std::string text = readText("resources/record_catalogue.csv");
    auto pos = text.find("\nREFR,");
    auto cand = text.find("Candidate", pos);
    text.replace(cand, std::string("Candidate").size(), "Winner");
    auto trial = RecordCatalogue::parse(text);
    RecordDefinitions t = d;
    t.applyCatalogue(trial);
    CHECK(!t.isCompilable(Signature("REFR")));
    CHECK(t.strategy(Signature("REFR")) == MergeStrategy::Candidate);
}

TEST(struct_array_with_header)
{
    // WRLD RNAM: cell Y, X, count, then (reference, Y, X) entries.
    std::vector<uint8_t> rnam(8 + 2 * 8, 0);
    storeLE<uint32_t>(rnam.data() + 4, 2);
    storeLE<uint32_t>(rnam.data() + 8, 0x01000800);
    storeLE<uint32_t>(rnam.data() + 16, 0x01000801);
    Subrecord sr;
    sr.signature = Signature("RNAM");
    sr.data = rnam;
    auto layout = decodeLayout(RecordDefinitions::get(), Signature("WRLD"), {sr});
    CHECK(layout.issues.empty());
    CHECK_EQ(layout.formIdSlots.size(), size_t(2));
    CHECK_EQ(layout.formIdSlots[0].offset, uint32_t(8));
    CHECK_EQ(layout.formIdSlots[1].offset, uint32_t(16));
}
