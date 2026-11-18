#include "Test.hpp"

#include "engine/Provenance.hpp"

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
