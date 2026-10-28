#include "Test.hpp"

#include "engine/MergeEngine.hpp"

using namespace fo4mx;

namespace {

FormKey K(uint32_t origin, uint32_t id)
{
    return FormKey{origin, id};
}

NormSub scalar(const char* sig, std::vector<uint8_t> bytes)
{
    NormSub s;
    s.sig = Signature::fromString(sig);
    s.bytes = std::move(bytes);
    return s;
}

NormSub keywords(const std::vector<FormKey>& ks)
{
    NormSub s;
    s.sig = Signature("KWDA");
    s.bytes.assign(ks.size() * 4, 0);
    for (size_t i = 0; i < ks.size(); ++i)
        s.refs.emplace_back(static_cast<uint32_t>(i * 4), ks[i]);
    return s;
}

FieldSet weapon(const std::string& name, std::vector<FormKey> kws, uint8_t value)
{
    NormRecord r;
    r.key = K(0, 0x1000);
    r.sig = Signature("WEAP");
    r.subs.push_back(scalar("EDID", {'G', 0}));
    std::vector<uint8_t> full(name.begin(), name.end());
    full.push_back(0);
    r.subs.push_back(scalar("FULL", full));
    NormSub ksiz = scalar("KSIZ", {0, 0, 0, 0});
    r.subs.push_back(ksiz);
    r.subs.push_back(keywords(kws));
    r.subs.push_back(scalar("DATA", {value, 0, 0, 0}));
    return splitFields(r, RecordDefinitions::get());
}

} // namespace

TEST(merge_compatible_fields)
{
    auto base = weapon("Pistol", {K(0, 1)}, 10);
    auto a = weapon("Better Pistol", {K(0, 1)}, 10); // changes name
    auto b = weapon("Pistol", {K(0, 1)}, 25);        // changes value
    auto r = MergeEngine::merge({base, a, b});
    CHECK(!r.analysis.hard);
    CHECK(r.merged); // B (last) alone would lose A's name
    CHECK(r.analysis.winnerLosesChanges);
    auto joined = joinFields(r.fields);
    std::string fullText;
    uint8_t value = 0;
    for (const auto& s : joined) {
        if (s.sig == Signature("FULL"))
            fullText = reinterpret_cast<const char*>(s.bytes.data());
        if (s.sig == Signature("DATA"))
            value = s.bytes[0];
    }
    CHECK_EQ(fullText, std::string("Better Pistol"));
    CHECK_EQ(int(value), 25);
}

TEST(merge_hard_conflict_last_changer_wins)
{
    auto base = weapon("Pistol", {}, 10);
    auto a = weapon("Pistol A", {}, 10);
    auto b = weapon("Pistol B", {}, 10);
    auto r = MergeEngine::merge({base, a, b});
    CHECK(r.analysis.hard);
    bool found = false;
    for (const auto& f : r.analysis.fields)
        if (f.hard) {
            found = true;
            CHECK(f.cells[1] == CellStatus::Replaced);
            CHECK(f.cells[2] == CellStatus::Winning);
        }
    CHECK(found);
}

TEST(merge_keyword_lists_add_and_remove)
{
    auto base = weapon("P", {K(0, 1), K(0, 2)}, 1);
    auto a = weapon("P", {K(0, 1), K(0, 2), K(5, 0x800)}, 1); // adds a mod keyword
    auto b = weapon("P", {K(0, 1), K(6, 0x900)}, 1);          // removes 2, adds another
    auto r = MergeEngine::merge({base, a, b});
    CHECK(!r.analysis.hard);
    const Field* kw = r.fields.find("array:KWDA");
    CHECK(kw != nullptr);
    std::vector<FormKey> got;
    for (const auto& it : kw->items)
        got.push_back(it.subs[0].refs[0].second);
    CHECK_EQ(got.size(), size_t(3));
    CHECK(got[0] == K(0, 1));
    CHECK(std::find(got.begin(), got.end(), K(5, 0x800)) != got.end());
    CHECK(std::find(got.begin(), got.end(), K(6, 0x900)) != got.end());
    CHECK(std::find(got.begin(), got.end(), K(0, 2)) == got.end());
    // KSIZ is regenerated with the merged count.
    auto joined = joinFields(r.fields);
    for (const auto& s : joined)
        if (s.sig == Signature("KSIZ"))
            CHECK_EQ(int(s.bytes[0]), 3);
}

TEST(merge_leveled_list_multiset)
{
    auto make = [](std::vector<std::pair<FormKey, uint8_t>> entries) {
        NormRecord r;
        r.key = K(0, 0x2000);
        r.sig = Signature("LVLI");
        r.subs.push_back(scalar("EDID", {'L', 0}));
        r.subs.push_back(scalar("LLCT", {static_cast<uint8_t>(entries.size())}));
        for (auto [k, level] : entries) {
            NormSub s = scalar("LVLO", std::vector<uint8_t>(12, 0));
            s.bytes[0] = level;
            s.bytes[8] = 1;
            s.refs.emplace_back(4, k);
            r.subs.push_back(s);
        }
        return splitFields(r, RecordDefinitions::get());
    };
    auto base = make({{K(0, 1), 1}, {K(0, 1), 1}});
    auto a = make({{K(0, 1), 1}, {K(0, 1), 1}, {K(7, 0x800), 5}});
    auto b = make({{K(0, 1), 1}, {K(0, 1), 1}, {K(8, 0x800), 10}});
    auto r = MergeEngine::merge({base, a, b});
    const Field* list = r.fields.find("list:LVLO");
    CHECK(list != nullptr);
    CHECK_EQ(list->items.size(), size_t(4)); // duplicates kept + both additions
    auto joined = joinFields(r.fields);
    for (const auto& s : joined)
        if (s.sig == Signature("LLCT"))
            CHECK_EQ(int(s.bytes[0]), 4);
}

TEST(merge_forced_winner_and_no_merge_mode)
{
    auto base = weapon("Pistol", {}, 10);
    auto a = weapon("Better Pistol", {}, 10);
    auto b = weapon("Pistol", {}, 25);
    MergeOptions lastWins;
    lastWins.mergeCompatible = false;
    auto r1 = MergeEngine::merge({base, a, b}, lastWins);
    CHECK(!r1.merged);
    MergeOptions forced;
    forced.forcedWinner = 1;
    auto r2 = MergeEngine::merge({base, a, b}, forced);
    CHECK(r2.merged);
    for (const auto& s : joinFields(r2.fields))
        if (s.sig == Signature("DATA"))
            CHECK_EQ(int(s.bytes[0]), 10);
}

// Overrides are compared with the version they were made against (their own masters), as in xEdit.
TEST(merge_against_own_master_version)
{
    auto base = weapon("P", {K(0, 1)}, 10);
    auto modX = weapon("P", {K(0, 1), K(5, 0x800)}, 20);   // adds a keyword, raises the value
    auto other = weapon("Other", {K(0, 1)}, 10);           // unrelated plugin renames it
    auto patch = weapon("P", {K(0, 1)}, 10);               // has modX as master: removes and reverts both
    MergeOptions mo;
    mo.parents = {0, 0, 0, 1};

    auto r = MergeEngine::merge({base, modX, other, patch}, mo);
    CHECK(!r.analysis.hard); // the patch supersedes modX deliberately
    const Field* kw = r.fields.find("array:KWDA");
    CHECK(kw != nullptr);
    CHECK_EQ(kw->items.size(), size_t(1)); // the patch's removal is kept
    std::string fullText;
    uint8_t value = 0;
    for (const auto& s : joinFields(r.fields)) {
        if (s.sig == Signature("FULL"))
            fullText = reinterpret_cast<const char*>(s.bytes.data());
        if (s.sig == Signature("DATA"))
            value = s.bytes[0];
    }
    CHECK_EQ(int(value), 10);                    // the deliberate revert is kept
    CHECK_EQ(fullText, std::string("Other"));    // and the unrelated change still merges in

    // Without lineage everything is compared with the base: the patch looks like it changed nothing.
    auto flat = MergeEngine::merge({base, modX, other, patch});
    CHECK(std::find(flat.analysis.itms.begin(), flat.analysis.itms.end(), size_t(3)) != flat.analysis.itms.end());
}

TEST(merge_repeating_master_edit_is_itm)
{
    auto base = weapon("P", {}, 10);
    auto dlc = weapon("P", {}, 20);
    auto mod = weapon("P", {}, 20); // made with the DLC as master and changes nothing of its own
    MergeOptions mo;
    mo.parents = {0, 0, 1};
    auto r = MergeEngine::merge({base, dlc, mod}, mo);
    CHECK_EQ(r.analysis.itms.size(), size_t(1));
    CHECK_EQ(r.analysis.itms[0], size_t(2));
    CHECK_EQ(r.analysis.changers.size(), size_t(1));
}
