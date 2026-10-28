#include "Test.hpp"

#include "esp/PluginWriter.hpp"
#include "esp/RecordCatalogue.hpp"
#include "esp/RecordDefinitions.hpp"
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

// Compiling needs both a layout and Specialised in the catalogue; the two must agree.
TEST(catalogue_strategy_matches_the_layouts)
{
    const auto& defs = RecordDefinitions::get();
    CHECK(defs.specialisedWithoutLayout().empty());
    size_t specialised = 0;
    for (const auto& e : defs.catalogue().entries())
        specialised += e.strategy == MergeStrategy::Specialised;
    CHECK_EQ(specialised, defs.compilableTypes().size());
    CHECK_EQ(defs.compilableTypes().size(), size_t(61));
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
    CHECK(d.specialisedWithoutLayout().empty());

    // Marking a type without a layout Specialised does not make it compilable; it is reported.
    std::string promoted = readText("resources/record_catalogue.csv");
    pos = promoted.find("\nNPC_,");
    auto pres = promoted.find("Preserve", pos);
    promoted.replace(pres, std::string("Preserve").size(), "Specialised");
    auto promotedCat = RecordCatalogue::parse(promoted);
    RecordDefinitions d2 = defs;
    d2.applyCatalogue(promotedCat);
    CHECK(!d2.isCompilable(Signature("NPC_")));
    CHECK_EQ(d2.specialisedWithoutLayout().size(), size_t(1));
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
        CHECK(p == RecordPlacement::Top || (p == RecordPlacement::Cell && s == Signature("CELL")));
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
