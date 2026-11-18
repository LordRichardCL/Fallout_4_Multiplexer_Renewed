#include "Test.hpp"

#include "engine/Compatibility.hpp"
#include "engine/Provenance.hpp"
#include "esp/Record.hpp"

#include <algorithm>
#include <optional>

using namespace fo4mx;

// Change provenance (docs/COMPATIBILITY_PATCH_RESEARCH.md, milestone 1): the specification's scenarios on
// small override chains. Chain index 0 is the defining version, the last one the winner; `parents` says
// which version each was made against (Session::overrideParents).

namespace {

NormSub sub(const char* sig, std::vector<uint8_t> bytes)
{
    NormSub s;
    s.sig = Signature::fromString(sig);
    s.bytes = std::move(bytes);
    return s;
}

struct Armor {
    std::optional<uint8_t> damage;      // DATA (one value standing in for a field)
    std::optional<uint8_t> weight;      // DNAM
    std::optional<uint8_t> value;       // FNAM
    std::vector<uint32_t> keywords;     // KWDA, object IDs in plugin 0
    std::optional<uint8_t> undescribed; // a field with undescribed bytes
};

FieldSet armor(const Armor& a)
{
    NormRecord r;
    r.key = FormKey{0, 0x1000};
    r.sig = Signature("ARMO");
    r.subs.push_back(sub("EDID", {'R', 0}));
    if (a.damage)
        r.subs.push_back(sub("DATA", {*a.damage, 0, 0, 0}));
    if (a.weight)
        r.subs.push_back(sub("DNAM", {*a.weight, 0, 0, 0}));
    if (a.value)
        r.subs.push_back(sub("FNAM", {*a.value, 0, 0, 0}));
    if (!a.keywords.empty()) {
        r.subs.push_back(sub("KSIZ", {static_cast<uint8_t>(a.keywords.size()), 0, 0, 0}));
        NormSub k = sub("KWDA", std::vector<uint8_t>(a.keywords.size() * 4, 0));
        for (size_t i = 0; i < a.keywords.size(); ++i)
            k.refs.emplace_back(static_cast<uint32_t>(i * 4), FormKey{0, a.keywords[i]});
        r.subs.push_back(k);
    }
    if (a.undescribed) {
        NormSub u = sub("ZZZZ", {*a.undescribed, 0, 0, 0});
        u.heuristic = true;
        u.opaque.emplace_back(0, 4);
        r.subs.push_back(u);
    }
    return splitFields(r, RecordDefinitions::get());
}

FieldSet list(const char* record, const char* entrySig, std::vector<uint32_t> forms)
{
    NormRecord r;
    r.key = FormKey{0, 0x2000};
    r.sig = Signature::fromString(record);
    r.subs.push_back(sub("EDID", {'L', 0}));
    if (std::string(record) == "LVLI")
        r.subs.push_back(sub("LLCT", {static_cast<uint8_t>(forms.size())}));
    for (uint32_t f : forms) {
        NormSub s;
        if (std::string(entrySig) == "LVLO") {
            s = sub("LVLO", std::vector<uint8_t>(12, 0));
            s.bytes[0] = 1;
            s.bytes[8] = 1;
            s.refs.emplace_back(4, FormKey{0, f});
        } else {
            s = sub(entrySig, std::vector<uint8_t>(4, 0));
            s.refs.emplace_back(0, FormKey{0, f});
        }
        r.subs.push_back(s);
    }
    return splitFields(r, RecordDefinitions::get());
}

std::string label(FormKey k)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%X", k.objectId);
    return buf;
}

std::vector<const ChangeEvent*> eventsOf(const RecordProvenance& p, size_t version, const std::string& field = {})
{
    std::vector<const ChangeEvent*> out;
    for (const auto& e : p.events)
        if (e.version == version && (field.empty() || e.label == field || e.fieldKey.find(field) != std::string::npos))
            out.push_back(&e);
    return out;
}

const ChangeEvent* one(const RecordProvenance& p, size_t version, const std::string& field = {})
{
    auto v = eventsOf(p, version, field);
    CHECK_EQ(v.size(), size_t(1));
    return v.size() == 1 ? v[0] : nullptr;
}

} // namespace

// Test 1: three plugins change three different fields, none made against another. The two earlier changes
// are omitted from the winner (it never had them); the winner's own survives.
TEST(provenance_independent_scalar_changes)
{
    const Armor base{33, 11, 100, {}, {}};
    Armor a = base, b = base, c = base;
    a.damage = 55;
    b.weight = 7;
    c.value = 250;
    auto p = traceProvenance({armor(base), armor(a), armor(b), armor(c)}, {0, 0, 0, 0}, label);
    CHECK_EQ(p.events.size(), size_t(3));
    const auto* da = one(p, 1, "DATA");
    const auto* wb = one(p, 2, "DNAM");
    const auto* vc = one(p, 3, "FNAM");
    if (!(da && wb && vc))
        return;
    CHECK(da->status == ChangeStatus::OmittedLater);
    CHECK(wb->status == ChangeStatus::OmittedLater);
    CHECK(vc->status == ChangeStatus::Survives);
    CHECK_EQ(da->lostAt, 3);
    CHECK(da->competesWith.empty());
    CHECK_EQ(p.lostCount(), size_t(2));
}

// Test 2: two plugins set the same field to different values: they compete; nothing is chosen.
TEST(provenance_same_field_competition)
{
    const Armor base{33, 11, 100, {}, {}};
    Armor a = base, b = base;
    a.damage = 55;
    b.damage = 80;
    auto p = traceProvenance({armor(base), armor(a), armor(b)}, {0, 0, 0}, label);
    const auto* ea = one(p, 1, "DATA");
    const auto* eb = one(p, 2, "DATA");
    if (!(ea && eb))
        return;
    CHECK(ea->status == ChangeStatus::OmittedLater);
    CHECK(eb->status == ChangeStatus::Survives);
    CHECK((ea->competesWith == std::vector<size_t>{2}));
    CHECK((eb->competesWith == std::vector<size_t>{1}));
}

// Tests 3, 4 and 5: a keyword added by ModB. ModC not made against ModB omits it; ModC made against ModB
// keeps it; ModC made against ModB without it no longer contains it (no intent is claimed either way).
TEST(provenance_added_element_omitted_kept_or_removed)
{
    const Armor base{33, 11, 100, {0x800}, {}};
    Armor b = base, c = base;
    b.keywords = {0x800, 0x900}; // the "laser" slot
    c.value = 250;

    auto omitted = traceProvenance({armor(base), armor(b), armor(c)}, {0, 0, 0}, label);
    const auto* add = one(omitted, 1, "KWDA");
    if (add) {
        CHECK(add->op == ChangeOp::Add);
        CHECK(add->status == ChangeStatus::OmittedLater);
        CHECK_EQ(add->lostAt, 2);
        CHECK(add->item.find("900") != std::string::npos);
    }
    CHECK(eventsOf(omitted, 2, "KWDA").empty()); // ModC changed nothing in the keywords

    Armor kept = c;
    kept.keywords = b.keywords;
    auto preserved = traceProvenance({armor(base), armor(b), armor(kept)}, {0, 0, 1}, label);
    if (const auto* e = one(preserved, 1, "KWDA")) {
        CHECK(e->status == ChangeStatus::Survives);
        CHECK((e->keptBy == std::vector<size_t>{2}));
    }

    auto removed = traceProvenance({armor(base), armor(b), armor(c)}, {0, 0, 1}, label);
    if (const auto* e = one(removed, 1, "KWDA")) {
        CHECK(e->status == ChangeStatus::RemovedLater);
        CHECK_EQ(e->lostAt, 2);
    }
    if (const auto* r = one(removed, 2, "KWDA"))
        CHECK(r->op == ChangeOp::Remove && r->status == ChangeStatus::Survives);
}

// Test 5 (the specification's): a later plugin made against the first sets the same field to another value.
TEST(provenance_later_change_replaces)
{
    const Armor base{33, 11, 100, {}, {}};
    Armor a = base, b = base;
    a.damage = 55;
    b.damage = 80;
    auto p = traceProvenance({armor(base), armor(a), armor(b)}, {0, 0, 1}, label);
    if (const auto* e = one(p, 1, "DATA")) {
        CHECK(e->status == ChangeStatus::Replaced);
        CHECK(e->competesWith.empty()); // made against each other: a replacement, not a competition
    }
    Armor back = base;
    auto r = traceProvenance({armor(base), armor(a), armor(back)}, {0, 0, 1}, label);
    if (const auto* e = one(r, 1, "DATA"))
        CHECK(e->status == ChangeStatus::Reverted);
}

// Test 6: keywords are sorted by xEdit, so another order is no change; a FormID list keeps its order.
TEST(provenance_order_changes)
{
    const Armor base{33, 11, 100, {0x800, 0x801, 0x802}, {}};
    Armor a = base;
    a.keywords = {0x801, 0x800, 0x802};
    auto p = traceProvenance({armor(base), armor(a)}, {0, 0}, label);
    CHECK(p.events.empty());

    auto f = traceProvenance({list("FLST", "LNAM", {1, 2, 3}), list("FLST", "LNAM", {2, 1, 3})}, {0, 0}, label);
    CHECK_EQ(f.events.size(), size_t(1));
    if (f.events.size() == 1)
        CHECK(f.events[0].op == ChangeOp::Reorder && f.events[0].status == ChangeStatus::Survives);
}

// Test 7: leveled list entries may repeat. Two plugins not made against each other add the same entry:
// both additions are reported, the later one as a duplicate of the earlier.
TEST(provenance_duplicate_elements)
{
    auto base = list("LVLI", "LVLO", {0x10, 0x10});
    auto a = list("LVLI", "LVLO", {0x10, 0x10, 0x20});
    auto b = list("LVLI", "LVLO", {0x10, 0x10, 0x20});
    auto p = traceProvenance({base, a, b}, {0, 0, 0}, label);
    const auto* ea = one(p, 1);
    const auto* eb = one(p, 2);
    if (!(ea && eb))
        return;
    CHECK(ea->op == ChangeOp::Add && eb->op == ChangeOp::Add);
    CHECK_EQ(eb->duplicateOf, 1);
    CHECK(ea->status == ChangeStatus::Survives); // the winner holds an equal entry

    // A third copy of an existing entry is an addition of its own (repeats count).
    auto more = traceProvenance({base, list("LVLI", "LVLO", {0x10, 0x10, 0x10})}, {0, 0}, label);
    CHECK_EQ(more.events.size(), size_t(1));
}

// Test 11: a difference in undescribed bytes is found and marked as not understood.
TEST(provenance_unknown_structure)
{
    Armor base{33, 11, 100, {}, uint8_t(1)};
    Armor a = base;
    a.undescribed = 2;
    auto p = traceProvenance({armor(base), armor(a)}, {0, 0}, label);
    const auto* e = one(p, 1);
    if (e)
        CHECK(e->opaque);
}

// Test 12: the defining version and the winner agree, a version in between added an element: the lost
// change is still found (comparing only the defining version with the winner would miss it).
TEST(provenance_master_and_winner_identical)
{
    const Armor base{33, 11, 100, {0x800}, {}};
    Armor b = base;
    b.keywords = {0x800, 0x900};
    auto p = traceProvenance({armor(base), armor(b), armor(base)}, {0, 0, 0}, label);
    CHECK_EQ(p.lostCount(), size_t(1));
    if (const auto* e = one(p, 1, "KWDA"))
        CHECK(e->status == ChangeStatus::OmittedLater);
    CHECK(eventsOf(p, 2).empty()); // the winner repeats the defining version: it changed nothing
}

// Keyed lists (faction relations): the same relation changed differently by unrelated plugins competes.
TEST(provenance_keyed_competition)
{
    auto fact = [](uint8_t reaction) {
        NormRecord r;
        r.key = FormKey{0, 0x3000};
        r.sig = Signature("FACT");
        r.subs.push_back(sub("EDID", {'F', 0}));
        NormSub s = sub("XNAM", std::vector<uint8_t>(12, 0));
        s.bytes[8] = reaction;
        s.refs.emplace_back(0, FormKey{0, 0x50});
        r.subs.push_back(s);
        return splitFields(r, RecordDefinitions::get());
    };
    auto p = traceProvenance({fact(1), fact(2), fact(3)}, {0, 0, 0}, label);
    const auto* a = one(p, 1);
    const auto* b = one(p, 2);
    if (!(a && b))
        return;
    CHECK(a->op == ChangeOp::Modify);
    CHECK((a->competesWith == std::vector<size_t>{2}));
    CHECK(a->status == ChangeStatus::OmittedLater);
    CHECK(b->status == ChangeStatus::Survives);
}

// ---- Compatibility classes (phase 6: research report 8.2) ----------------------------------------------

namespace {

// Plugins 0..3 are the chain's (0 defines the record, 3 wins); 3 masters 0 only; 9 is outside the chain.
CompatContext context(Signature record, bool fieldsMerge = true)
{
    CompatContext c;
    c.record = record;
    c.fieldsMerge = fieldsMerge;
    c.winner = 3;
    c.chainPlugins = {0, 1, 2, 3};
    c.masters = [](PluginIndex p, PluginIndex other) { return p == 3 && other == 0; };
    c.exists = [](FormKey k) { return k.objectId != 0xDEAD; };
    return c;
}

std::vector<FieldSet> withPlugins(std::vector<FieldSet> chain)
{
    for (size_t i = 0; i < chain.size(); ++i)
        chain[i].plugin = static_cast<PluginIndex>(i);
    return chain;
}

CompatFinding classifyOnly(const std::vector<FieldSet>& chain, const std::vector<size_t>& parents,
                           const CompatContext& ctx, const std::string& field = {})
{
    auto p = traceProvenance(chain, parents, label);
    for (const auto& e : p.events)
        if (isLost(e.status) && (field.empty() || e.fieldKey.find(field) != std::string::npos))
            return classifyChange(e, chain, RecordDefinitions::get(), ctx);
    CHECK(false); // no lost change found
    return {};
}

} // namespace

TEST(compatibility_classes_for_lost_changes)
{
    const auto& defs = RecordDefinitions::get();
    (void)defs;
    const Armor base{33, 11, 100, {0x800}, {}};
    Armor a = base, b = base, c = base;
    a.damage = 55;
    c.value = 250;

    // Green: a field no other plugin changed, omitted by a winner not made against it.
    auto chain = withPlugins({armor(base), armor(a), armor(b), armor(c)});
    auto f = classifyOnly(chain, {0, 0, 0, 0}, context(Signature("ARMO")), "DATA");
    CHECK(f.cls == CompatClass::Green && f.kind == CompatKind::Independent);

    // Yellow: competing values.
    Armor b2 = base;
    b2.damage = 80;
    f = classifyOnly(withPlugins({armor(base), armor(a), armor(b2), armor(c)}), {0, 0, 0, 0},
                     context(Signature("ARMO")), "DATA");
    CHECK(f.cls == CompatClass::Yellow && f.kind == CompatKind::Competing);

    // Yellow: removed by a later plugin made against it.
    Armor k = base;
    k.keywords = {0x800, 0x900};
    f = classifyOnly(withPlugins({armor(base), armor(k), armor(base), armor(base)}), {0, 0, 1, 2},
                     context(Signature("ARMO")), "KWDA");
    CHECK(f.cls == CompatClass::Yellow && f.kind == CompatKind::Ambiguous);

    // Green: an element of a set omitted; it names a record of plugin 0, which the winner masters.
    auto setChain = withPlugins({armor(base), armor(k), armor(base), armor(base)});
    f = classifyOnly(setChain, {0, 0, 0, 0}, context(Signature("ARMO")), "KWDA");
    CHECK(f.cls == CompatClass::Green);
    CHECK(f.requiredMasters.empty());

    // Unknown: a record type with no field rule.
    f = classifyOnly(chain, {0, 0, 0, 0}, context(Signature("ARMO"), false), "DATA");
    CHECK(f.cls == CompatClass::Unknown);

    // Red: a quest, undescribed data, a deleted record.
    f = classifyOnly(chain, {0, 0, 0, 0}, context(Signature("QUST")), "DATA");
    CHECK(f.cls == CompatClass::Red);
    Armor u = base, u2 = base;
    u.undescribed = 1;
    u2.undescribed = 2;
    f = classifyOnly(withPlugins({armor(u), armor(u2), armor(u), armor(u)}), {0, 0, 0, 0}, context(Signature("ARMO")),
                     "ZZZZ");
    CHECK(f.cls == CompatClass::Red && f.kind == CompatKind::Unsupported);
    auto deleted = chain;
    deleted[3].fields[0].subs[0].bytes[0] |= RecordFlags::Deleted;
    f = classifyOnly(deleted, {0, 0, 0, 0}, context(Signature("ARMO")), "DATA");
    CHECK(f.cls == CompatClass::Red);
}

TEST(compatibility_dependencies_and_structures)
{
    const Armor base{33, 11, 100, {0x800}, {}};
    // A keyword from a plugin outside the chain: a patch would need it as a master.
    auto withKeyword = [&](FormKey kw) {
        auto set = armor(base);
        for (auto& f : set.fields)
            if (f.isList())
                for (auto& item : f.items)
                    for (auto& sub : item.subs)
                        for (auto& [off, k] : sub.refs)
                            k = kw;
        return set;
    };
    auto outside = withPlugins({armor(base), withKeyword(FormKey{9, 0x900}), armor(base), armor(base)});
    auto f = classifyOnly(outside, {0, 0, 0, 0}, context(Signature("ARMO")), "KWDA");
    CHECK(f.cls == CompatClass::Yellow && f.kind == CompatKind::Dependent);
    CHECK((f.requiredMasters == std::vector<PluginIndex>{9}));

    // From a plugin of the chain the winner does not master: Green, the master listed.
    auto inChain = withPlugins({armor(base), withKeyword(FormKey{1, 0x900}), armor(base), armor(base)});
    f = classifyOnly(inChain, {0, 0, 0, 0}, context(Signature("ARMO")), "KWDA");
    CHECK(f.cls == CompatClass::Green);
    CHECK((f.requiredMasters == std::vector<PluginIndex>{1}));

    // A record that is not in the load order: Red.
    auto missing = withPlugins({armor(base), withKeyword(FormKey{0, 0xDEAD}), armor(base), armor(base)});
    f = classifyOnly(missing, {0, 0, 0, 0}, context(Signature("ARMO")), "KWDA");
    CHECK(f.cls == CompatClass::Red);

    // A form list addition: order is data, so review.
    auto flst = withPlugins({list("FLST", "LNAM", {1}), list("FLST", "LNAM", {1, 2}), list("FLST", "LNAM", {1}),
                             list("FLST", "LNAM", {1})});
    f = classifyOnly(flst, {0, 0, 0, 0}, context(Signature("FLST")));
    CHECK(f.cls == CompatClass::Yellow);

    // Leveled list entry: Green (an entry of a list that keeps repeats).
    auto lvli = withPlugins({list("LVLI", "LVLO", {1}), list("LVLI", "LVLO", {1, 2}), list("LVLI", "LVLO", {1}),
                             list("LVLI", "LVLO", {1})});
    f = classifyOnly(lvli, {0, 0, 0, 0}, context(Signature("LVLI")));
    CHECK(f.cls == CompatClass::Green);

    // Scripts: Red.
    auto script = [](uint8_t v) {
        NormRecord r;
        r.key = FormKey{0, 0x1000};
        r.sig = Signature("ARMO");
        r.subs.push_back(sub("EDID", {'R', 0}));
        r.subs.push_back(sub("VMAD", {6, 0, 2, 0, 0, 0, v}));
        return splitFields(r, RecordDefinitions::get());
    };
    auto vmad = withPlugins({script(0), script(1), script(0), script(0)});
    f = classifyOnly(vmad, {0, 0, 0, 0}, context(Signature("ARMO")), "VMAD");
    CHECK(f.cls == CompatClass::Red && f.kind == CompatKind::Complex);
}

// ---- Carrying changes into a patch record (phase 8) ------------------------------------------------------

namespace {
std::vector<const ChangeEvent*> lostOf(const RecordProvenance& p)
{
    std::vector<const ChangeEvent*> out;
    for (const auto& e : p.events)
        if (isLost(e.status))
            out.push_back(&e);
    return out;
}
uint8_t firstByte(const FieldSet& s, const char* key)
{
    const Field* f = s.find(key);
    return f && !f->subs.empty() && !f->subs[0].bytes.empty() ? f->subs[0].bytes[0] : 0;
}
} // namespace

TEST(apply_carries_scalars_and_list_elements)
{
    // ModA changes damage, ModB adds weight (absent before) and a keyword, ModC (the winner) changes value.
    const Armor base{33, std::nullopt, 100, {0x800}, {}};
    Armor a = base, b = base, c = base;
    a.damage = 55;
    b.weight = 7;
    b.keywords = {0x800, 0x900};
    c.value = 250;
    const std::vector<FieldSet> chain{armor(base), armor(a), armor(b), armor(c)};
    auto p = traceProvenance(chain, {0, 0, 0, 0}, label);
    auto lost = lostOf(p);
    CHECK_EQ(lost.size(), size_t(3));
    FieldSet result = chain.back();
    auto missing = applyChanges(result, chain, lost);
    CHECK(missing.empty());
    CHECK_EQ(int(firstByte(result, "DATA#0")), 55);
    CHECK_EQ(int(firstByte(result, "DNAM#0")), 7);
    CHECK_EQ(int(firstByte(result, "FNAM#0")), 250); // the winner's own change stays
    // The added field sits where xEdit expects it (DNAM between DATA and FNAM here, as in ModB).
    std::vector<std::string> order;
    for (const auto& f : result.fields)
        order.push_back(f.sig.str());
    const auto dnam = std::find(order.begin(), order.end(), "DNAM");
    const auto fnam = std::find(order.begin(), order.end(), "FNAM");
    CHECK(dnam != order.end() && fnam != order.end() && dnam < fnam);
}

TEST(apply_removal_reorder_and_contradiction)
{
    // A removal carried: ModB removed keyword 0x801; ModC (winner, not made against ModB) still has it.
    const Armor base{33, 11, 100, {0x800, 0x801}, {}};
    Armor b = base, c = base;
    b.keywords = {0x800};
    c.value = 250;
    std::vector<FieldSet> chain{armor(base), armor(b), armor(c)};
    auto p = traceProvenance(chain, {0, 0, 0}, label);
    FieldSet result = chain.back();
    CHECK(applyChanges(result, chain, lostOf(p)).empty());
    const Field* kw = nullptr;
    for (const auto& f : result.fields)
        if (f.isList())
            kw = &f;
    CHECK(kw && kw->items.size() == 1);

    // A form list's order carried.
    std::vector<FieldSet> fl{list("FLST", "LNAM", {1, 2, 3}), list("FLST", "LNAM", {3, 2, 1}),
                             list("FLST", "LNAM", {1, 2, 3, 4})};
    auto pf = traceProvenance(fl, {0, 0, 0}, label);
    FieldSet fr = fl.back();
    CHECK(applyChanges(fr, fl, lostOf(pf)).empty());
    std::vector<uint32_t> ids;
    for (const auto& f : fr.fields)
        if (f.isList())
            for (const auto& item : f.items)
                ids.push_back(item.subs[0].refs[0].second.objectId);
    CHECK((ids == std::vector<uint32_t>{3, 2, 1, 4}));

    // Two competing values carried together: the result cannot hold both, and says which it lacks.
    const Armor s{33, 11, 100, {}, {}};
    Armor x = s, y = s, w = s;
    x.damage = 55;
    y.damage = 80;
    w.value = 1;
    std::vector<FieldSet> comp{armor(s), armor(x), armor(y), armor(w)};
    auto pc = traceProvenance(comp, {0, 0, 0, 0}, label);
    FieldSet cr = comp.back();
    CHECK_EQ(applyChanges(cr, comp, lostOf(pc)).size(), size_t(1));
}
