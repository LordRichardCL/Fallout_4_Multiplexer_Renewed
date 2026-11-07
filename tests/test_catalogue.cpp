#include "Test.hpp"

#include "esp/FieldDecoder.hpp"
#include "esp/PluginWriter.hpp"
#include "esp/RecordCatalogue.hpp"
#include "esp/RecordDefinitions.hpp"
#include "util/Binary.hpp"
#include "util/Compression.hpp"

#include <algorithm>
#include <cstring>
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
    CHECK_EQ(winner, size_t(79)); // stages 4 and 5, world, stage 7a, quest types, LCTN, GMST, navmeshes, packages, WTHR, PKIN and dialogue, confirmed by a real load order; ASTP DLVW DUAL EYES LVSP TREE from xEdit
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
    pos = promoted.find("\nNOCM,");
    auto pres = promoted.find("Preserve", pos);
    promoted.replace(pres, std::string("Preserve").size(), "Specialised");
    auto promotedCat = RecordCatalogue::parse(promoted);
    RecordDefinitions d2 = defs;
    d2.applyCatalogue(promotedCat);
    CHECK(!d2.isCompilable(Signature("NOCM")));
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
    // CELL has a top-level group too (interior cell blocks); SCEN and DLBR live in their quest's group
    // but may also have a top-level group.
    for (Signature s : order) {
        auto p = RecordCatalogue::get().find(s)->placement;
        CHECK(p == RecordPlacement::Top || p == RecordPlacement::TopWithChildren || p == RecordPlacement::QuestChild ||
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
    // Requested types are Candidates, not compiled (the confirmed world types are checked below).
    for (const char* t : {"PBAR", "PBEA", "PCON", "PFLA"}) {
        CHECK(d.strategy(Signature::fromString(t)) == MergeStrategy::Candidate);
        CHECK(d.supportSummary(Signature::fromString(t)).find("not compiled") != std::string::npos);
    }
    CHECK(d.strategy(Signature("NPC_")) == MergeStrategy::Winner); // confirmed
    CHECK(!d.overridesOnly(Signature("NPC_")));                    // new NPCs too (N1)
    CHECK(d.strategy(Signature("LCTN")) == MergeStrategy::Winner); // RCSR matches xEdit; only stale data failed
    // GMST: the value is a localised string for 's' settings, a number otherwise.
    CHECK(d.strategy(Signature("GMST")) == MergeStrategy::Winner); // confirmed
    auto gmst = [&](const char* edid) {
        Subrecord e;
        e.signature = Signature("EDID");
        e.data.assign(edid, edid + std::strlen(edid) + 1);
        Subrecord data;
        data.signature = Signature("DATA");
        data.data = {1, 0, 0, 0};
        return decodeLayout(d, Signature("GMST"), {e, data});
    };
    CHECK_EQ(gmst("sSomeText").localized.size(), size_t(1));
    CHECK(gmst("fSomeValue").localized.empty());
    CHECK(gmst("fSomeValue").heuristic.empty() && gmst("fSomeValue").formIdSlots.empty());
    CHECK(d.strategy(Signature("WTHR")) == MergeStrategy::Winner); // confirmed
    CHECK(d.strategy(Signature("QUST")) == MergeStrategy::Winner); // quest stage, confirmed

    // Navmeshes (xEdit layouts): every FormID found, and the data must end exactly.
    CHECK(d.strategy(Signature("NAVM")) == MergeStrategy::Winner); // confirmed (M2)
    CHECK(d.strategy(Signature("NAVI")) == MergeStrategy::Winner);
    struct Bytes {
        std::vector<uint8_t> v;
        Bytes& u8(uint8_t x) { v.push_back(x); return *this; }
        Bytes& u16(uint16_t x) { return u8(uint8_t(x)).u8(uint8_t(x >> 8)); }
        Bytes& u32(uint32_t x) { return u16(uint16_t(x)).u16(uint16_t(x >> 16)); }
        Bytes& zero(size_t n) { v.insert(v.end(), n, 0); return *this; }
    };
    auto decode = [&](const char* rec, const char* sub, const std::vector<uint8_t>& data) {
        Subrecord sr;
        sr.signature = Signature::fromString(sub);
        sr.data = data;
        return decodeLayout(d, Signature::fromString(rec), {sr});
    };
    auto offsets = [](const DecodedLayout& l) {
        std::vector<uint32_t> o;
        for (const auto& s : l.formIdSlots)
            o.push_back(s.offset);
        return o;
    };
    {
        // Interior navmesh: version, CRC, world 0, parent cell; 1 vertex, 1 triangle, 1 edge link,
        // 1 door link, no cover, no waypoints, grid divisor 1 with one empty cell.
        Bytes b;
        b.u32(15).u32(0).u32(0).u32(0x01000805);
        b.u32(1).zero(12);
        b.u32(1).zero(21);
        b.u32(1).u32(0).u32(0x01000900).u16(0).u8(0); // edge link navmesh at 65
        b.u32(1).u16(0).u32(0).u32(0x01000901);       // door at 82
        b.u32(0).u32(0).u32(0);
        b.u32(1).zero(8 + 24).u32(0);
        auto l = decode("NAVM", "NVNM", b.v);
        CHECK(l.issues.empty() && l.heuristic.empty());
        CHECK((offsets(l) == std::vector<uint32_t>{12, 65, 82}));
        // An exterior navmesh: the world is the FormID, the grid is not.
        auto ext = b.v;
        storeLE<uint32_t>(ext.data() + 8, 0x0000003C);
        CHECK((offsets(decode("NAVM", "NVNM", ext)) == std::vector<uint32_t>{8, 65, 82}));
        // Version 11: no cover data and no waypoints.
        Bytes o;
        o.u32(11).u32(0).u32(0).u32(0x01000805);
        o.u32(0).u32(0).u32(0).u32(0);
        o.u32(1).zero(8 + 24).u32(0);
        auto lo = decode("NAVM", "NVNM", o.v);
        CHECK(lo.issues.empty());
        CHECK((offsets(lo) == std::vector<uint32_t>{12}));
        // Four bytes after the grid (beyond xEdit's layout): kept as they are, undescribed (never
        // taken for a FormID); validation decides whether they can be moved.
        auto extra = b.v;
        extra.insert(extra.end(), {1, 2, 3, 4});
        auto le = decode("NAVM", "NVNM", extra);
        CHECK(le.issues.size() == 1 && le.issues[0].severity == Severity::Info);
        CHECK((offsets(le) == std::vector<uint32_t>{12, 65, 82}));
        CHECK(le.probableSlots.empty());
        CHECK(le.heuristic.size() == 1 && le.heuristic[0].begin == uint32_t(b.v.size()) &&
              le.heuristic[0].end == uint32_t(b.v.size() + 4));
        // A byte too many: reported and scanned, no slots kept.
        auto bad = b.v;
        bad.push_back(0);
        auto lb = decode("NAVM", "NVNM", bad);
        CHECK(!lb.issues.empty() && lb.formIdSlots.empty() && !lb.heuristic.empty());
    }
    {
        // Navmesh info: navmesh, flags, location, preferred %, 1 edge link, 0 preferred, 1 door,
        // island data with 1 triangle and 1 vertex, interior pathing cell.
        Bytes b;
        b.u32(0x01000900).u32(0).zero(12).u32(0);
        b.u32(1).u32(0x01000901);
        b.u32(0);
        b.u32(1).u32(0).u32(0x01000902);
        b.u8(1).zero(24).u32(1).zero(6).u32(1).zero(12);
        b.u32(0).u32(0).u32(0x01000805);
        auto l = decode("NAVI", "NVMI", b.v);
        CHECK(l.issues.empty());
        const uint32_t cell = uint32_t(b.v.size() - 4);
        CHECK((offsets(l) == std::vector<uint32_t>{0, 28, 44, cell}));
        // Precomputed paths: 1 path of 2 navmeshes, 1 road marker.
        Bytes p;
        p.u32(1).u32(2).u32(0x01000900).u32(0x01000901).u32(1).u32(0x01000902).u32(7);
        auto lp = decode("NAVI", "NVPP", p.v);
        CHECK(lp.issues.empty());
        CHECK((offsets(lp) == std::vector<uint32_t>{8, 12, 20}));
        // Packages: CNAM is the combat style before the first data input, a value after it; PLDT and
        // PTDA hold a FormID only for some types; package fragments end exactly.
        CHECK(d.strategy(Signature("PACK")) == MergeStrategy::Winner); // confirmed
        auto sub = [](const char* sig, std::vector<uint8_t> data) {
            Subrecord sr;
            sr.signature = Signature::fromString(sig);
            sr.data = std::move(data);
            return sr;
        };
        auto u32 = [](uint32_t a, uint32_t b = 0, uint32_t c = 0, size_t n = 4) {
            Bytes x;
            x.u32(a);
            if (n > 4) x.u32(b);
            if (n > 8) x.u32(c);
            return x.v;
        };
        auto pack = decodeLayout(d, Signature("PACK"),
                                 {sub("CNAM", u32(0x01000800)), sub("ANAM", {'I', 'n', 't', 0}), sub("CNAM", u32(5)),
                                  sub("PLDT", u32(0, 0x01000801, 0, 12)), sub("PLDT", u32(2, 7, 0, 12)),
                                  sub("PTDA", u32(7, 0x01000802, 1, 12)), sub("PTDA", u32(2, 9, 1, 12))});
        std::vector<std::pair<uint32_t, uint32_t>> packSlots;
        for (const auto& sl : pack.formIdSlots)
            packSlots.emplace_back(sl.sub, sl.offset);
        CHECK((packSlots == std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}, {3, 4}, {5, 4}}));
        CHECK(pack.heuristic.empty() && pack.issues.empty());
        // VMAD: version 6, format 2, no scripts; fragments: bind version, flags OnBegin|OnChange, a script
        // with no properties, two fragments.
        Bytes v;
        v.u16(6).u16(2).u16(0);
        v.u8(3).u8(5).u16(1).u8('S').u8(0).u16(0);
        for (int f = 0; f < 2; ++f)
            v.u8(0).u16(1).u8('S').u16(1).u8('F');
        auto vm = decodeLayout(d, Signature("PACK"), {sub("VMAD", v.v)});
        CHECK(vm.heuristic.empty() && vm.issues.empty());
        auto vbad = v.v;
        vbad.push_back(0);
        CHECK(!decodeLayout(d, Signature("PACK"), {sub("VMAD", vbad)}).issues.empty());

        // INFO TRDA: FFFFFFFF is "no emotion", not a FormID; WTHR IMSP holds 4 or 8 image spaces.
        Bytes t;
        t.u32(0xFFFFFFFF).u8(1).u32(0x01000803).zero(11);
        auto trda = decodeLayout(d, Signature("INFO"), {sub("TRDA", t.v)});
        CHECK(trda.issues.empty() && trda.formIdSlots.size() == 1 && trda.formIdSlots[0].offset == 5);
        storeLE<uint32_t>(t.v.data(), 0x01000804);
        CHECK_EQ(decodeLayout(d, Signature("INFO"), {sub("TRDA", t.v)}).formIdSlots.size(), size_t(2));
        CHECK_EQ(decodeLayout(d, Signature("WTHR"), {sub("IMSP", std::vector<uint8_t>(16, 0))}).formIdSlots.size(), size_t(4));
        CHECK(decodeLayout(d, Signature("WTHR"), {sub("IMSP", std::vector<uint8_t>(32, 0))}).issues.empty());

        // INFO and SCEN script fragments; SCEN HTID is a sound only in a radio action.
        Bytes iv;
        iv.u16(6).u16(2).u16(0).u8(3).u8(2).u16(1).u8('S').u8(0).u16(0).u8(0).u16(1).u8('S').u16(1).u8('F');
        CHECK(decodeLayout(d, Signature("INFO"), {sub("VMAD", iv.v)}).heuristic.empty());
        Bytes sv;
        sv.u16(6).u16(2).u16(0).u8(3).u8(1).u16(1).u8('S').u8(0).u16(0).u8(0).u16(1).u8('S').u16(1).u8('F');
        sv.u16(1).u8(1).u32(2).u8(0).u16(1).u8('S').u16(1).u8('P');
        auto scen = decodeLayout(d, Signature("SCEN"), {sub("VMAD", sv.v)});
        CHECK(scen.heuristic.empty() && scen.issues.empty());
        auto radio = decodeLayout(d, Signature("SCEN"), {sub("ANAM", {6, 0}), sub("HTID", u32(0x01000805))});
        CHECK(radio.formIdSlots.size() == 1 && radio.formIdSlots[0].sub == 1);
        auto talk = decodeLayout(d, Signature("SCEN"), {sub("ANAM", {0, 0}), sub("HTID", u32(3))});
        CHECK(talk.formIdSlots.empty() && talk.heuristic.empty());

        // Precut map: reference, 2 triangles; reference, 0 triangles.
        Bytes m;
        m.u32(0x01000903).u16(2).u16(1).u16(2).u32(0x01000904).u16(0);
        CHECK((offsets(decode("NAVM", "MNAM", m.v)) == std::vector<uint32_t>{0, 10}));
    }

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

    // World types compile only as overrides, written with their cell (stage W2): a cell child set to
    // Winner compiles with the "overrides" scope and not without it; dialogue never compiles.
    std::string text = readText("resources/record_catalogue.csv");
    auto promote = [](std::string t, const char* sig, bool dropScope) {
        auto pos = t.find(std::string("\n") + sig + ",");
        auto cand = t.find("Candidate", pos);
        t.replace(cand, std::string("Candidate").size(), "Winner");
        if (dropScope) {
            auto end = t.find('\n', pos + 1);
            auto scope = t.rfind(",overrides", end);
            if (scope != std::string::npos && scope > pos)
                t.erase(scope, std::string(",overrides").size());
        }
        return t;
    };
    {
        // A placed reference type set to Winner compiles (overrides and new references, W2/W3); dialogue
        // never does.
        RecordDefinitions t = d;
        auto trial = RecordCatalogue::parse(promote(text, "PBAR", false));
        t.applyCatalogue(trial);
        CHECK(t.isCompilable(Signature("PBAR")));
        CHECK(!t.overridesOnly(Signature("PBAR")));
    }
    // Dialogue (D2): topics own child groups like worldspaces, responses are topic children, scenes
    // and branches quest children; all compile.
    for (const char* t : {"DIAL", "INFO", "SCEN", "DLBR"})
        CHECK(d.isCompilable(Signature::fromString(t)));
    for (const char* t : {"REFR", "CELL", "WRLD", "LAND"}) // new records compile (W3, W4)
        CHECK(!d.overridesOnly(Signature::fromString(t)));
    CHECK(d.field(Signature("CELL"), Signature("XCRI")).skipCountAt == 0);
    // Placed references: persistent location, and topic data whose FormID depends on its type.
    CHECK(d.field(Signature("ACHR"), Signature("XLCN")).kind == FieldKind::FormId);
    auto pdto = [&](uint32_t type, uint32_t data) {
        Subrecord sr;
        sr.signature = Signature("PDTO");
        sr.data.resize(8);
        storeLE<uint32_t>(sr.data.data(), type);
        storeLE<uint32_t>(sr.data.data() + 4, data);
        return decodeLayout(d, Signature("REFR"), {sr});
    };
    auto topic = pdto(0, 0x01000800);
    CHECK_EQ(topic.formIdSlots.size(), size_t(1));
    CHECK_EQ(topic.formIdSlots[0].offset, 4u);
    auto subtype = pdto(1, 0x56544341); // "ACTV": text, never a FormID
    CHECK(subtype.formIdSlots.empty() && subtype.heuristic.empty());
    // Confirmed world types compile.
    for (const char* t : {"CELL", "WRLD", "REFR", "ACHR", "LAND", "PHZD", "PGRE", "PMIS", "PARW"}) {
        CHECK(d.strategy(Signature::fromString(t)) == MergeStrategy::Winner);
        CHECK(d.isCompilable(Signature::fromString(t)));
    }
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

// The fields that kept 42 plugins out on a real load order (build 66), now described from xEdit.
TEST(perk_sound_shader_projectile_and_effect_fields)
{
    const auto& d = RecordDefinitions::get();
    auto sub = [](const char* sig, std::vector<uint8_t> data) {
        Subrecord sr;
        sr.signature = Signature::fromString(sig);
        sr.data = std::move(data);
        return sr;
    };
    auto slots = [](const DecodedLayout& l) {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (const auto& s : l.formIdSlots)
            v.emplace_back(s.sub, s.offset);
        return v;
    };
    using Slots = std::vector<std::pair<uint32_t, uint32_t>>;
    const std::vector<uint8_t> four(4, 0), eight(8, 0);
    // PERK entry point: DATA (entry point, function 12, tab count), then EPFT 2 + EPFD = actor value, float.
    auto perk = [&](uint8_t epft, uint8_t function, const std::vector<uint8_t>& epfd) {
        return decodeLayout(d, Signature("PERK"),
                            {sub("PRKE", {2, 0, 0}), sub("DATA", {7, function, 1}), sub("EPFT", {epft}), sub("EPFD", epfd)});
    };
    CHECK((slots(perk(2, 12, eight)) == Slots{{3, 0}})); // actor value
    CHECK(perk(2, 1, eight).formIdSlots.empty() && perk(2, 1, eight).heuristic.empty()); // two floats
    CHECK((slots(perk(5, 0, four)) == Slots{{3, 0}}));   // spell
    CHECK((slots(perk(8, 0, eight)) == Slots{{3, 0}}));  // actor value, float
    CHECK_EQ(perk(7, 0, four).localized.size(), size_t(1));
    // SNDR BNAM: the base descriptor only in an AutoWeapon descriptor.
    std::vector<uint8_t> autoWeapon = {0xE3, 0x7A, 0x15, 0xED}, standard = {0x0A, 0x54, 0xEF, 0x1E};
    CHECK((slots(decodeLayout(d, Signature("SNDR"), {sub("CNAM", autoWeapon), sub("BNAM", four)})) == Slots{{1, 0}}));
    auto values = decodeLayout(d, Signature("SNDR"), {sub("CNAM", standard), sub("BNAM", {1, 2, 3, 4, 5, 6})});
    CHECK(values.formIdSlots.empty() && values.heuristic.empty());
    // EFSH DNAM: the ambient sound at +108 in the current 157-byte layout.
    CHECK((slots(decodeLayout(d, Signature("EFSH"), {sub("DNAM", std::vector<uint8_t>(157, 0))})) == Slots{{0, 108}}));
    // PROJ DNAM: the VATS projectile at +89, nothing scanned.
    auto proj = decodeLayout(d, Signature("PROJ"), {sub("DNAM", std::vector<uint8_t>(93, 0))});
    CHECK(proj.heuristic.empty());
    CHECK(std::any_of(proj.formIdSlots.begin(), proj.formIdSlots.end(), [](const auto& s) { return s.offset == 89; }));
    // MGEF DATA: complete, the flags at +0 are not scanned.
    auto mgef = decodeLayout(d, Signature("MGEF"), {sub("DATA", std::vector<uint8_t>(152, 0))});
    CHECK(mgef.heuristic.empty());
    CHECK_EQ(mgef.formIdSlots.size(), size_t(20));
}

// Context- and position-dependent fields are decoded only in their context (xEdit's deciders):
// condition parameters by xEdit's table of 479 functions and the condition's flags, BOOK's "teaches"
// FormID by its flags. Nothing is taken for a FormID because its value looks like one.
TEST(context_dependent_fields_follow_xedit)
{
    const auto& d = RecordDefinitions::get();
    auto ctda = [](uint8_t flags, uint16_t function, uint32_t p1, uint32_t p2, uint32_t runOn, uint32_t ref) {
        std::vector<uint8_t> v(32, 0);
        v[0] = flags;
        storeLE<float>(v.data() + 4, 1.0f);
        storeLE<uint16_t>(v.data() + 8, function);
        storeLE<uint32_t>(v.data() + 12, p1);
        storeLE<uint32_t>(v.data() + 16, p2);
        storeLE<uint32_t>(v.data() + 20, runOn);
        storeLE<uint32_t>(v.data() + 24, ref);
        storeLE<int32_t>(v.data() + 28, -1);
        Subrecord sr;
        sr.signature = Signature("CTDA");
        sr.data = std::move(v);
        return sr;
    };
    auto offsets = [](const DecodedLayout& l) {
        std::vector<uint32_t> o;
        for (const auto& s : l.formIdSlots)
            o.push_back(s.offset);
        return o;
    };
    const Signature cobj("COBJ");
    // The table covers every function xEdit knows (it had 18 before).
    CHECK(d.conditionFunctionKnown(72) && d.conditionFunctionKnown(817) && d.conditionFunctionKnown(0));
    CHECK(!d.conditionFunctionKnown(9999));
    // GetIsID (72): parameter 1 is a FormID.
    CHECK((offsets(decodeLayout(d, cobj, {ctda(0, 72, 0x01000800, 0, 0, 0)})) == std::vector<uint32_t>{12}));
    // GetDistance (1) takes a reference: a FormID, unless "use aliases" (0x02) makes it an alias index,
    // or "use packdata" (0x08) a package data index.
    CHECK((offsets(decodeLayout(d, cobj, {ctda(0, 1, 0x01000800, 0, 0, 0)})) == std::vector<uint32_t>{12}));
    CHECK(offsets(decodeLayout(d, cobj, {ctda(0x02, 1, 5, 0, 0, 0)})).empty());
    CHECK(offsets(decodeLayout(d, cobj, {ctda(0x08, 1, 5, 0, 0, 0)})).empty());
    // GetIsCurrentPackage (161) keeps its package when it runs on a quest alias (5), even with 0x02.
    CHECK((offsets(decodeLayout(d, cobj, {ctda(0x02, 161, 0x01000800, 0, 5, 0)})) == std::vector<uint32_t>{12}));
    // Run On "Reference" (2): the reference field is a FormID; "Quest Alias" (5): it is not.
    CHECK((offsets(decodeLayout(d, cobj, {ctda(0, 0, 0, 0, 2, 0x00000014)})) == std::vector<uint32_t>{24}));
    CHECK(offsets(decodeLayout(d, cobj, {ctda(0, 0, 0, 0, 5, 3)})).empty());
    // A function xEdit does not know: its parameters are undescribed (and reported), never a guess.
    auto unknown = decodeLayout(d, cobj, {ctda(0, 9999, 0x01000800, 0x01000801, 0, 0)});
    CHECK(offsets(unknown).empty() && unknown.probableSlots.empty());
    CHECK(unknown.heuristic.size() == 1 && unknown.heuristic[0].begin == 12 && unknown.heuristic[0].end == 20);
    CHECK(!unknown.issues.empty());

    // BOOK DNAM: "teaches" (+1) is a FormID only when a flag says what it teaches.
    auto book = [](uint8_t flags) {
        Subrecord sr;
        sr.signature = Signature("DNAM");
        sr.data.assign(13, 0);
        sr.data[0] = flags;
        storeLE<uint32_t>(sr.data.data() + 1, 0x01000800);
        return sr;
    };
    const Signature bookSig("BOOK");
    CHECK(offsets(decodeLayout(d, bookSig, {book(0)})).empty());
    for (uint8_t f : {uint8_t(0x01), uint8_t(0x04), uint8_t(0x10)}) {
        auto l = decodeLayout(d, bookSig, {book(f)});
        CHECK((offsets(l) == std::vector<uint32_t>{1}));
        CHECK(l.heuristic.empty());
    }
    // Layouts completed from xEdit: no undescribed bytes inside the described size.
    auto weap = [](const char* sig, size_t size) {
        Subrecord sr;
        sr.signature = Signature::fromString(sig);
        sr.data.assign(size, 0x33);
        return sr;
    };
    const Signature weapSig("WEAP");
    CHECK(decodeLayout(d, weapSig, {weap("DNAM", 132)}).heuristic.empty());
    CHECK(decodeLayout(d, weapSig, {weap("FNAM", 41)}).heuristic.empty());
    CHECK(decodeLayout(d, weapSig, {weap("CRDT", 12)}).heuristic.empty());
    // Bytes past xEdit's layout stay undescribed.
    auto longer = decodeLayout(d, weapSig, {weap("DNAM", 136)});
    CHECK(longer.heuristic.size() == 1 && longer.heuristic[0].begin == 132 && longer.heuristic[0].end == 136);
    // SNDR DNAM is a descriptor FormID (xEdit), not a guess.
    CHECK((offsets(decodeLayout(d, Signature("SNDR"), {weap("DNAM", 4)})) == std::vector<uint32_t>{0}));
}

// Fields build 83 refused as undescribed, now described from xEdit: furniture markers (keyword at
// +16 of each 24- or 20-byte marker), PERK/TERM script fragments, and number or text fields.
TEST(fields_described_from_xedit_after_build_83)
{
    const auto& d = RecordDefinitions::get();
    auto sub = [](const char* sig, std::vector<uint8_t> data) {
        Subrecord sr;
        sr.signature = Signature::fromString(sig);
        sr.data = std::move(data);
        return sr;
    };
    auto offsets = [](const DecodedLayout& l) {
        std::vector<uint32_t> o;
        for (const auto& s : l.formIdSlots)
            o.push_back(s.offset);
        return o;
    };
    // Markers: one or two of 24 bytes (form version 125+), or 20 bytes (older).
    CHECK((offsets(decodeLayout(d, Signature("FURN"), {sub("SNAM", std::vector<uint8_t>(24, 0x41))})) ==
           std::vector<uint32_t>{16}));
    CHECK((offsets(decodeLayout(d, Signature("FURN"), {sub("SNAM", std::vector<uint8_t>(48, 0x41))})) ==
           std::vector<uint32_t>{16, 40}));
    CHECK((offsets(decodeLayout(d, Signature("TERM"), {sub("SNAM", std::vector<uint8_t>(40, 0x41))})) ==
           std::vector<uint32_t>{16, 36}));
    CHECK(decodeLayout(d, Signature("FURN"), {sub("SNAM", std::vector<uint8_t>(24, 0x41))}).heuristic.empty());
    // 120 bytes: five old markers or six new ones; undecided, so undescribed.
    auto both = decodeLayout(d, Signature("FURN"), {sub("SNAM", std::vector<uint8_t>(120, 0))});
    CHECK(offsets(both).empty() && both.heuristic.size() == 1);

    // PERK / TERM script fragments: version 6, object format 2, no scripts; then the fragments.
    std::vector<uint8_t> v;
    auto u8 = [&](uint8_t x) { v.push_back(x); };
    auto u16 = [&](uint16_t x) { u8(uint8_t(x)); u8(uint8_t(x >> 8)); };
    auto u32 = [&](uint32_t x) { u16(uint16_t(x)); u16(uint16_t(x >> 16)); };
    auto str = [&](const std::string& s) { u16(uint16_t(s.size())); v.insert(v.end(), s.begin(), s.end()); };
    u16(6); u16(2); u16(0);       // version, object format, scripts
    u8(3);                         // extra bind data version
    str("Fragments:Perks:P"); u8(0); u16(0); // fragment script: name, status, no properties
    u16(1); u32(0); u8(0); str("Fragments:Perks:P"); str("Fragment_Entry_00");
    for (const char* rec : {"PERK", "TERM"}) {
        auto l = decodeLayout(d, Signature::fromString(rec), {sub("VMAD", v)});
        CHECK(l.heuristic.empty() && l.issues.empty());
    }
    auto junk = v;
    junk.push_back(7);
    auto lj = decodeLayout(d, Signature("TERM"), {sub("VMAD", junk)});
    CHECK(!lj.heuristic.empty() && !lj.issues.empty()); // not as described: undescribed, reported

    // Numbers and text: nothing undescribed.
    const std::pair<const char*, const char*> plain[] = {
        {"FURN", "ENAM"}, {"TERM", "MNAM"}, {"LCTN", "ANAM"}, {"LSCR", "ZNAM"}, {"LSCR", "ONAM"}, {"IPCT", "DATA"},
        {"CSTY", "CSRA"}, {"ARMA", "BSMB"}, {"PROJ", "NAM2"}, {"SNDR", "FNAM"}, {"SNDR", "NNAM"}, {"WEAP", "MASE"}};
    for (auto [rec, s] : plain) {
        auto l = decodeLayout(d, Signature::fromString(rec), {sub(s, std::vector<uint8_t>(24, 0x41))});
        CHECK(l.heuristic.empty() && l.formIdSlots.empty());
    }
}
