#include "engine/Provenance.hpp"

#include "esp/RecordDefinitions.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace fo4mx {

const char* changeOpName(ChangeOp op)
{
    switch (op) {
    case ChangeOp::Set: return "changed";
    case ChangeOp::Add: return "added";
    case ChangeOp::Remove: return "removed";
    case ChangeOp::Modify: return "changed";
    case ChangeOp::Reorder: return "reordered";
    }
    return "";
}

const char* changeStatusName(ChangeStatus s)
{
    switch (s) {
    case ChangeStatus::Survives: return "Survives";
    case ChangeStatus::Replaced: return "Replaced";
    case ChangeStatus::Reverted: return "Reverted";
    case ChangeStatus::RemovedLater: return "Removed later";
    case ChangeStatus::OmittedLater: return "Omitted later";
    }
    return "";
}

bool RecordProvenance::madeAgainst(size_t b, size_t a) const
{
    if (a == 0)
        return true;
    for (int guard = 0; guard < 4096 && b != a && b != 0; ++guard)
        b = b < parents.size() ? parents[b] : 0;
    return b == a;
}

size_t RecordProvenance::lostCount() const
{
    return static_cast<size_t>(
        std::count_if(events.begin(), events.end(), [](const ChangeEvent& e) { return isLost(e.status); }));
}

bool listOrderMatters(Signature record, const Field& field)
{
    // xEdit (wbDefinitionsFO4.pas): FLST's FormIDs are not sorted ("Should not be sorted according to
    // Arthmoor and JustinOther"); scripts index them.
    return record == Signature("FLST") && field.sig == Signature("LNAM");
}

namespace {

std::string canonical(const ListItem& item)
{
    std::string s;
    for (const auto& sub : item.subs) {
        s.append(reinterpret_cast<const char*>(&sub.sig.value), 4);
        const uint32_t n = static_cast<uint32_t>(sub.bytes.size());
        s.append(reinterpret_cast<const char*>(&n), 4);
        s.append(sub.bytes.begin(), sub.bytes.end());
        for (const auto& [off, k] : sub.refs) {
            s.append(reinterpret_cast<const char*>(&off), sizeof(off));
            const uint64_t p = k.packed();
            s.append(reinterpret_cast<const char*>(&p), 8);
        }
    }
    return s;
}

std::vector<FormKey> refsOf(const std::vector<NormSub>& subs)
{
    std::vector<FormKey> out;
    for (const auto& s : subs)
        for (const auto& [off, k] : s.refs)
            if (k.valid() && std::find(out.begin(), out.end(), k) == out.end())
                out.push_back(k);
    return out;
}

std::vector<FormKey> refsOf(const Field* f)
{
    if (!f)
        return {};
    auto out = refsOf(f->subs);
    for (const auto& item : f->items)
        for (const auto& k : refsOf(item.subs))
            if (std::find(out.begin(), out.end(), k) == out.end())
                out.push_back(k);
    return out;
}

bool opaqueField(const Field* f)
{
    if (!f)
        return false;
    auto subsOpaque = [](const std::vector<NormSub>& subs) {
        return std::any_of(subs.begin(), subs.end(), [](const NormSub& s) { return s.heuristic || !s.opaque.empty(); });
    };
    if (subsOpaque(f->subs))
        return true;
    return std::any_of(f->items.begin(), f->items.end(), [&](const ListItem& i) { return subsOpaque(i.subs); });
}

// One version's list: element identity -> element, in order.
struct ListView {
    std::vector<std::string> order;
    std::unordered_map<std::string, const ListItem*> items;
    bool has(const std::string& id) const { return items.count(id) != 0; }
};

ListView viewOf(const Field* f, bool keyed)
{
    ListView v;
    if (!f)
        return v;
    std::unordered_map<std::string, int> seen;
    for (const auto& item : f->items) {
        std::string id;
        if (keyed) {
            const uint64_t p = itemKey(item).packed();
            id.assign(reinterpret_cast<const char*>(&p), 8);
        } else {
            id = canonical(item);
        }
        // Repeated elements count (leveled lists): A, A is A#0, A#1, as xEdit numbers them.
        id += "#" + std::to_string(seen[id]++);
        if (v.items.emplace(id, &item).second)
            v.order.push_back(id);
    }
    return v;
}

// Phase 10: struct members. A field is traced member by member only when its value layer is the whole
// layout (xEdit's members, checked complete), it is one struct (not an array), no bytes of it may hold
// undescribed FormIDs, and every version holds it as one subrecord of exactly that size. Anything else
// stays a whole value, as before: a member is never guessed from a layout that does not fit.
std::vector<ValueMember> memberLayout(const RecordDefinitions* defs, Signature record, const Field& shape,
                                      const std::vector<const Field*>& versions, bool opaque)
{
    if (!defs || opaque || shape.shape != FieldShape::Scalar || shape.key == "@flags")
        return {};
    const FieldDef& def = defs->field(record, shape.sig);
    if (def.values.empty() || !def.valuesComplete || def.valueStride || !def.valueSize ||
        def.kind == FieldKind::Unknown)
        return {};
    for (const Field* f : versions)
        if (!f || f->subs.size() != 1 || f->subs[0].sig != shape.sig || f->subs[0].bytes.size() != def.valueSize)
            return {};
    // One member is the whole field: nothing to separate (and its changes keep their ids).
    if (def.values.size() < 2)
        return {};
    std::vector<ValueMember> members = def.values;
    std::sort(members.begin(), members.end(), [](const ValueMember& a, const ValueMember& b) { return a.offset < b.offset; });
    // Members must tile the struct: no overlap, nothing past its end.
    uint32_t end = 0;
    for (const auto& m : members) {
        if (m.offset < end || m.offset + valueMemberSize(m) > def.valueSize || valueMemberSize(m) == 0)
            return {};
        end = m.offset + valueMemberSize(m);
    }
    return members;
}

std::vector<std::pair<uint32_t, FormKey>> refsIn(const NormSub& s, uint32_t off, uint32_t size)
{
    std::vector<std::pair<uint32_t, FormKey>> out;
    for (const auto& r : s.refs)
        if (r.first >= off && r.first < off + size)
            out.push_back(r);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

bool fitsMember(const Field* f, uint32_t off, uint32_t size)
{
    return f && f->subs.size() == 1 && off + size <= f->subs[0].bytes.size();
}

bool memberEquivalent(const Field* a, const Field* b, uint32_t off, uint32_t size)
{
    if (!fitsMember(a, off, size) || !fitsMember(b, off, size))
        return false;
    const auto& x = a->subs[0];
    const auto& y = b->subs[0];
    return std::equal(x.bytes.begin() + off, x.bytes.begin() + off + size, y.bytes.begin() + off) &&
           refsIn(x, off, size) == refsIn(y, off, size);
}

std::string describeMember(const ValueMember& m, const Field* f, const std::function<std::string(FormKey)>& label)
{
    const uint32_t size = valueMemberSize(m);
    if (!fitsMember(f, m.offset, size))
        return "(absent)";
    const auto& s = f->subs[0];
    const uint8_t* p = s.bytes.data() + m.offset;
    uint32_t u = 0;
    std::memcpy(&u, p, std::min<uint32_t>(size, 4));
    char buf[64];
    switch (m.type) {
    case ValueType::FormId: {
        const auto refs = refsIn(s, m.offset, size);
        if (refs.empty() || !refs[0].second.valid())
            return "NULL";
        return label ? label(refs[0].second) : std::string("(form)");
    }
    case ValueType::Float: {
        float v;
        std::memcpy(&v, p, 4);
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        return buf;
    }
    case ValueType::I8: return std::to_string(static_cast<int8_t>(u));
    case ValueType::I16: return std::to_string(static_cast<int16_t>(u));
    case ValueType::I32: return std::to_string(static_cast<int32_t>(u));
    case ValueType::Enum8:
    case ValueType::Enum32:
        if (u < m.labels.size() && !m.labels[u].empty())
            return m.labels[u];
        return std::to_string(u);
    case ValueType::Flags8:
    case ValueType::Flags16:
    case ValueType::Flags32: {
        std::snprintf(buf, sizeof(buf), "0x%0*X", static_cast<int>(size * 2), u);
        std::string out = buf, names;
        for (size_t bit = 0; bit < 32 && bit < m.labels.size(); ++bit)
            if ((u >> bit) & 1u)
                names += (names.empty() ? "" : ", ") + (m.labels[bit].empty() ? "bit " + std::to_string(bit) : m.labels[bit]);
        return names.empty() ? out : out + " (" + names + ")";
    }
    case ValueType::Bytes: {
        std::string out;
        for (uint32_t i = 0; i < size && i < 32; ++i) {
            std::snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", p[i]);
            out += buf;
        }
        return out + (size > 32 ? " ..." : "");
    }
    default: return std::to_string(u);
    }
}

std::vector<FormKey> keysOf(const std::vector<std::pair<uint32_t, FormKey>>& refs)
{
    std::vector<FormKey> out;
    for (const auto& [off, k] : refs)
        if (k.valid() && std::find(out.begin(), out.end(), k) == out.end())
            out.push_back(k);
    return out;
}

} // namespace

RecordProvenance traceProvenance(const std::vector<FieldSet>& chain, const std::vector<size_t>& givenParents,
                                 const std::function<std::string(FormKey)>& label, const RecordDefinitions* defs)
{
    RecordProvenance rp;
    const size_t n = chain.size();
    rp.parents = givenParents;
    if (rp.parents.size() != n)
        rp.parents.assign(n, 0);
    for (size_t c = 0; c < n; ++c)
        if (rp.parents[c] >= c)
            rp.parents[c] = 0;
    if (n < 2)
        return rp;
    const size_t w = n - 1;
    // The winner's line: the versions it was made against, in load order.
    std::vector<size_t> line;
    for (size_t b = w;; b = rp.parents[b]) {
        line.push_back(b);
        if (b == 0)
            break;
    }
    std::reverse(line.begin(), line.end());
    auto firstOnLineAfter = [&](size_t a, auto lacks) -> int {
        for (size_t j : line)
            if (j > a && lacks(j))
                return static_cast<int>(j);
        return -1;
    };

    // Every field key any version has, in the winner's order first.
    std::vector<std::string> keys;
    std::set<std::string> known;
    for (size_t c = n; c-- > 0;)
        for (const auto& f : chain[c].fields)
            if (known.insert(f.key).second)
                keys.push_back(f.key);

    auto describeItem = [&](const Field& shape, const ListItem* item) {
        if (!item)
            return std::string("(absent)");
        Field one = shape;
        one.items = {*item};
        return describeField(&one, label);
    };

    for (const auto& key : keys) {
        std::vector<const Field*> v(n);
        const Field* shape = nullptr;
        bool opaque = false;
        for (size_t c = 0; c < n; ++c) {
            v[c] = chain[c].find(key);
            if (v[c] && !shape)
                shape = v[c];
            opaque |= opaqueField(v[c]);
        }
        if (!shape)
            continue;
        const size_t first = rp.events.size();

        if (const auto members = memberLayout(defs, chain[0].sig, *shape, v, opaque); !members.empty()) {
            // A struct, member by member: each member is a value of its own.
            for (const auto& m : members) {
                const uint32_t off = m.offset, size = valueMemberSize(m);
                auto same = [&](size_t x, size_t y) { return memberEquivalent(v[x], v[y], off, size); };
                const size_t memberFirst = rp.events.size();
                for (size_t a = 1; a < n; ++a) {
                    const size_t p = rp.parents[a];
                    if (same(a, p))
                        continue;
                    ChangeEvent e;
                    e.fieldKey = key;
                    e.label = shape->label;
                    e.member = m.name;
                    e.memberOffset = static_cast<uint16_t>(off);
                    e.memberSize = static_cast<uint16_t>(size);
                    e.item = m.name;
                    e.identity = "member:" + m.name;
                    e.version = a;
                    e.op = ChangeOp::Set;
                    e.before = describeMember(m, v[p], label);
                    e.after = describeMember(m, v[a], label);
                    e.references = keysOf(refsIn(v[a]->subs[0], off, size));
                    auto holds = [&](size_t j) { return same(j, a); };
                    for (size_t j = a + 1; j < n; ++j)
                        if (rp.madeAgainst(j, a) && holds(j))
                            e.keptBy.push_back(j);
                    if (holds(w)) {
                        e.status = ChangeStatus::Survives;
                    } else if (rp.madeAgainst(w, a)) {
                        e.lostAt = firstOnLineAfter(a, [&](size_t j) { return !holds(j); });
                        e.status = same(static_cast<size_t>(e.lostAt), 0) ? ChangeStatus::Reverted : ChangeStatus::Replaced;
                    } else {
                        e.status = ChangeStatus::OmittedLater;
                        e.lostAt = firstOnLineAfter(a, [](size_t) { return true; });
                    }
                    rp.events.push_back(std::move(e));
                }
                for (size_t i = memberFirst; i < rp.events.size(); ++i)
                    for (size_t k = memberFirst; k < rp.events.size(); ++k) {
                        const size_t a = rp.events[i].version, b = rp.events[k].version;
                        if (a != b && !rp.madeAgainst(a, b) && !rp.madeAgainst(b, a) && !same(a, b))
                            rp.events[i].competesWith.push_back(b);
                    }
            }
            continue;
        }

        if (!shape->isList()) {
            // A whole value.
            for (size_t a = 1; a < n; ++a) {
                const size_t p = rp.parents[a];
                if (fieldEquivalent(v[a], v[p]))
                    continue;
                ChangeEvent e;
                e.fieldKey = key;
                e.label = shape->label;
                e.version = a;
                e.op = !v[p] ? ChangeOp::Add : !v[a] ? ChangeOp::Remove : ChangeOp::Set;
                e.before = v[p] ? describeField(v[p], label) : "(absent)";
                e.after = v[a] ? describeField(v[a], label) : "(absent)";
                e.opaque = opaque;
                e.references = refsOf(v[a]);
                auto holds = [&](size_t j) { return fieldEquivalent(v[j], v[a]); };
                for (size_t j = a + 1; j < n; ++j)
                    if (rp.madeAgainst(j, a) && holds(j))
                        e.keptBy.push_back(j);
                if (holds(w)) {
                    e.status = ChangeStatus::Survives;
                } else if (rp.madeAgainst(w, a)) {
                    e.lostAt = firstOnLineAfter(a, [&](size_t j) { return !holds(j); });
                    const size_t j = static_cast<size_t>(e.lostAt);
                    e.status = fieldEquivalent(v[j], v[0]) ? ChangeStatus::Reverted
                               : !v[j]                     ? ChangeStatus::RemovedLater
                                                           : ChangeStatus::Replaced;
                } else {
                    e.status = ChangeStatus::OmittedLater;
                    e.lostAt = firstOnLineAfter(a, [](size_t) { return true; });
                }
                rp.events.push_back(std::move(e));
            }
            // Competing: versions not made against each other that set different values.
            for (size_t i = first; i < rp.events.size(); ++i)
                for (size_t k = first; k < rp.events.size(); ++k) {
                    const size_t a = rp.events[i].version, b = rp.events[k].version;
                    if (a != b && !rp.madeAgainst(a, b) && !rp.madeAgainst(b, a) && !fieldEquivalent(v[a], v[b]))
                        rp.events[i].competesWith.push_back(b);
                }
            continue;
        }

        // A list: elements by identity (keyed lists by their key).
        const bool keyed = shape->merge == MergeMode::Keyed;
        const bool ordered = listOrderMatters(chain[0].sig, *shape);
        std::vector<ListView> views;
        views.reserve(n);
        for (size_t c = 0; c < n; ++c)
            views.push_back(viewOf(v[c], keyed));
        auto sameItem = [&](size_t j, size_t a, const std::string& id) {
            return views[j].has(id) && (!keyed || itemEquivalent(*views[j].items.at(id), *views[a].items.at(id)));
        };
        for (size_t a = 1; a < n; ++a) {
            const size_t p = rp.parents[a];
            auto add = [&](ChangeOp op, const std::string& id, const ListItem* before, const ListItem* after) {
                ChangeEvent e;
                e.fieldKey = key;
                e.label = shape->label;
                e.version = a;
                e.op = op;
                e.identity = id;
                e.item = describeItem(*shape, after ? after : before);
                e.before = describeItem(*shape, before);
                e.after = describeItem(*shape, after);
                e.opaque = opaque;
                if (after)
                    e.references = refsOf(after->subs);
                auto holds = [&](size_t j) {
                    return op == ChangeOp::Remove ? !views[j].has(id) : sameItem(j, a, id);
                };
                for (size_t j = a + 1; j < n; ++j)
                    if (rp.madeAgainst(j, a) && holds(j))
                        e.keptBy.push_back(j);
                if (holds(w)) {
                    e.status = ChangeStatus::Survives;
                } else if (rp.madeAgainst(w, a)) {
                    e.lostAt = firstOnLineAfter(a, [&](size_t j) { return !holds(j); });
                    const size_t j = static_cast<size_t>(e.lostAt);
                    if (op == ChangeOp::Remove)
                        e.status = ChangeStatus::Reverted; // the element is there again
                    else if (!views[j].has(id))
                        e.status = ChangeStatus::RemovedLater;
                    else if (views[0].has(id) && itemEquivalent(*views[j].items.at(id), *views[0].items.at(id)))
                        e.status = ChangeStatus::Reverted;
                    else
                        e.status = ChangeStatus::Replaced;
                } else {
                    e.status = ChangeStatus::OmittedLater;
                    e.lostAt = firstOnLineAfter(a, [](size_t) { return true; });
                }
                rp.events.push_back(std::move(e));
            };
            for (size_t k = 0; k < views[a].order.size(); ++k) {
                const auto& id = views[a].order[k];
                if (!views[p].has(id)) {
                    add(ChangeOp::Add, id, nullptr, views[a].items.at(id));
                    if (ordered)
                        rp.events.back().position =
                            k == 0 ? std::string("first")
                                   : "after " + describeItem(*shape, views[a].items.at(views[a].order[k - 1]));
                }
                else if (keyed && !itemEquivalent(*views[a].items.at(id), *views[p].items.at(id)))
                    add(ChangeOp::Modify, id, views[p].items.at(id), views[a].items.at(id));
            }
            for (const auto& id : views[p].order)
                if (!views[a].has(id))
                    add(ChangeOp::Remove, id, views[p].items.at(id), nullptr);
            if (ordered) {
                // Elements both versions have, in another order.
                auto common = [&](const ListView& x, const ListView& y) {
                    std::vector<std::string> out;
                    for (const auto& id : x.order)
                        if (y.has(id))
                            out.push_back(id);
                    return out;
                };
                if (common(views[a], views[p]) != common(views[p], views[a])) {
                    ChangeEvent e;
                    e.fieldKey = key;
                    e.label = shape->label;
                    e.version = a;
                    e.op = ChangeOp::Reorder;
                    e.before = describeField(v[p], label);
                    e.after = describeField(v[a], label);
                    e.opaque = opaque;
                    auto holds = [&](size_t j) { return common(views[j], views[a]) == common(views[a], views[j]); };
                    for (size_t j = a + 1; j < n; ++j)
                        if (rp.madeAgainst(j, a) && holds(j))
                            e.keptBy.push_back(j);
                    if (holds(w)) {
                        e.status = ChangeStatus::Survives;
                    } else if (rp.madeAgainst(w, a)) {
                        e.lostAt = firstOnLineAfter(a, [&](size_t j) { return !holds(j); });
                        e.status = ChangeStatus::Replaced;
                    } else {
                        e.status = ChangeStatus::OmittedLater;
                        e.lostAt = firstOnLineAfter(a, [](size_t) { return true; });
                    }
                    rp.events.push_back(std::move(e));
                }
            }
        }
        // Keyed elements changed differently by versions not made against each other compete; the same
        // element added by such versions is a duplicate (both copies are kept where the list allows it).
        std::map<std::string, std::vector<size_t>> byElement; // element identity -> its events
        for (size_t i = first; i < rp.events.size(); ++i)
            if (rp.events[i].op == ChangeOp::Add || rp.events[i].op == ChangeOp::Modify)
                byElement[rp.events[i].identity].push_back(i);
        for (const auto& [element, list] : byElement)
            for (size_t x = 0; x < list.size(); ++x)
                for (size_t y = 0; y < list.size(); ++y) {
                    auto& ex = rp.events[list[x]];
                    const auto& ey = rp.events[list[y]];
                    const size_t a = ex.version, b = ey.version;
                    if (a == b || rp.madeAgainst(a, b) || rp.madeAgainst(b, a))
                        continue;
                    if (keyed) {
                        const auto ia = views[a].items.find(ex.identity), ib = views[b].items.find(ey.identity);
                        if (ia != views[a].items.end() && ib != views[b].items.end() &&
                            !itemEquivalent(*ia->second, *ib->second))
                            ex.competesWith.push_back(b);
                    } else if (b < a && ex.duplicateOf < 0) {
                        ex.duplicateOf = static_cast<int>(b);
                    }
                }
    }
    return rp;
}

namespace {

// Puts `field` into `result` where `from` (the version it comes from) has it: after the nearest earlier
// field `result` holds, then past the result's own fields xEdit orders before it.
void insertField(FieldSet& result, const FieldSet& from, const Field& field)
{
    const auto& sf = from.fields;
    size_t at = 0;
    while (at < sf.size() && sf[at].key != field.key)
        ++at;
    size_t pos = 0;
    for (size_t p = std::min(at, sf.size()); p-- > 0;) {
        auto it = std::find_if(result.fields.begin(), result.fields.end(), [&](const Field& f) { return f.key == sf[p].key; });
        if (it != result.fields.end()) {
            pos = static_cast<size_t>(it - result.fields.begin()) + 1;
            break;
        }
    }
    if (const int rank = subrecordRank(result.sig, field.sig); rank >= 0)
        while (pos < result.fields.size() && !from.find(result.fields[pos].key)) {
            const int here = subrecordRank(result.sig, result.fields[pos].sig);
            if (here < 0 || here >= rank)
                break;
            ++pos;
        }
    result.fields.insert(result.fields.begin() + static_cast<std::ptrdiff_t>(pos), field);
}

const Field* shapeOf(const std::vector<FieldSet>& chain, const std::string& key)
{
    for (const auto& set : chain)
        if (const Field* f = set.find(key))
            return f;
    return nullptr;
}

std::vector<std::string> commonOrder(const ListView& x, const ListView& y)
{
    std::vector<std::string> out;
    for (const auto& id : x.order)
        if (y.has(id))
            out.push_back(id);
    return out;
}

size_t positionIn(const ListView& v, const std::string& id)
{
    return static_cast<size_t>(std::find(v.order.begin(), v.order.end(), id) - v.order.begin());
}

// The nearest element before `id` in `source` that `target` has (other than `id`), or "" when none.
std::string anchorBefore(const ListView& source, const ListView& target, const std::string& id)
{
    const size_t at = positionIn(source, id);
    for (size_t k = at; k-- > 0;)
        if (source.order[k] != id && target.has(source.order[k]))
            return source.order[k];
    return {};
}

// The nearest element after `id` in `source` that `target` has, or "" when none.
std::string anchorAfter(const ListView& source, const ListView& target, const std::string& id)
{
    for (size_t k = positionIn(source, id) + 1; k < source.order.size(); ++k)
        if (target.has(source.order[k]))
            return source.order[k];
    return {};
}

bool holds(const FieldSet& result, const std::vector<FieldSet>& chain, const ChangeEvent& c)
{
    const Field* src = c.version < chain.size() ? chain[c.version].find(c.fieldKey) : nullptr;
    const Field* have = result.find(c.fieldKey);
    const Field* shape = shapeOf(chain, c.fieldKey);
    if (!shape)
        return false;
    if (c.isMember())
        return memberEquivalent(have, src, c.memberOffset, c.memberSize);
    if (!shape->isList())
        return c.op == ChangeOp::Remove ? have == nullptr : fieldEquivalent(have, src);
    const bool keyed = shape->merge == MergeMode::Keyed;
    const ListView mine = viewOf(have, keyed), theirs = viewOf(src, keyed);
    switch (c.op) {
    case ChangeOp::Remove: return !mine.has(c.identity);
    case ChangeOp::Add:
    case ChangeOp::Modify:
        if (!mine.has(c.identity) || !theirs.has(c.identity) ||
            (keyed && !itemEquivalent(*mine.items.at(c.identity), *theirs.items.at(c.identity))))
            return false;
        if (c.op == ChangeOp::Add && listOrderMatters(chain[0].sig, *shape)) {
            // In an ordered list the element must follow the nearest element before it in the source that
            // the result has.
            const auto anchor = anchorBefore(theirs, mine, c.identity);
            if (!anchor.empty())
                return positionIn(mine, c.identity) > positionIn(mine, anchor);
        }
        return true;
    case ChangeOp::Reorder: return commonOrder(mine, theirs) == commonOrder(theirs, mine);
    case ChangeOp::Set: break;
    }
    return false;
}

} // namespace

bool changeHeldBy(const FieldSet& result, const std::vector<FieldSet>& chain, const ChangeEvent& c)
{
    return holds(result, chain, c);
}

std::vector<size_t> applyChanges(FieldSet& result, const std::vector<FieldSet>& chain,
                                 const std::vector<const ChangeEvent*>& changes)
{
    for (const ChangeEvent* c : changes) {
        if (c->version >= chain.size())
            continue;
        const FieldSet& from = chain[c->version];
        const Field* src = from.find(c->fieldKey);
        const Field* shape = shapeOf(chain, c->fieldKey);
        if (!shape)
            continue;
        if (c->isMember()) {
            // The member's bytes and references only; the result must hold the field at the same size
            // (otherwise the change is not held and the record is refused by the check below).
            Field* have = result.find(c->fieldKey);
            const uint32_t off = c->memberOffset, size = c->memberSize;
            if (!fitsMember(have, off, size) || !fitsMember(src, off, size) ||
                have->subs[0].bytes.size() != src->subs[0].bytes.size())
                continue;
            auto& to = have->subs[0];
            const auto& in = src->subs[0];
            std::copy(in.bytes.begin() + off, in.bytes.begin() + off + size, to.bytes.begin() + off);
            to.refs.erase(std::remove_if(to.refs.begin(), to.refs.end(),
                                         [&](const auto& r) { return r.first >= off && r.first < off + size; }),
                          to.refs.end());
            for (const auto& r : refsIn(in, off, size))
                to.refs.push_back(r);
            std::sort(to.refs.begin(), to.refs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            // The source values stored at those offsets (diagnostics) follow the references they belong to.
            to.rawRefs.erase(std::remove_if(to.rawRefs.begin(), to.rawRefs.end(),
                                            [&](const auto& r) { return r.offset >= off && r.offset < off + size; }),
                             to.rawRefs.end());
            for (const auto& r : in.rawRefs)
                if (r.offset >= off && r.offset < off + size)
                    to.rawRefs.push_back(r);
            std::sort(to.rawRefs.begin(), to.rawRefs.end());
            continue;
        }
        if (!shape->isList()) {
            Field* have = result.find(c->fieldKey);
            if (c->op == ChangeOp::Remove) {
                result.fields.erase(std::remove_if(result.fields.begin(), result.fields.end(),
                                                   [&](const Field& f) { return f.key == c->fieldKey; }),
                                    result.fields.end());
            } else if (src) {
                if (have)
                    *have = *src;
                else
                    insertField(result, from, *src);
            }
            continue;
        }
        const bool keyed = shape->merge == MergeMode::Keyed;
        if (!result.find(c->fieldKey)) {
            if (c->op == ChangeOp::Remove || c->op == ChangeOp::Reorder || !src)
                continue;
            Field empty = *src;
            empty.items.clear();
            insertField(result, from, empty);
        }
        Field& list = *result.find(c->fieldKey);
        const ListView theirs = viewOf(src, keyed);
        const ListView mine = viewOf(&list, keyed);
        auto indexOf = [&](const std::string& id) -> std::ptrdiff_t {
            auto it = mine.items.find(id);
            return it == mine.items.end() ? -1 : it->second - list.items.data();
        };
        switch (c->op) {
        case ChangeOp::Add:
        case ChangeOp::Modify: {
            auto it = theirs.items.find(c->identity);
            if (it == theirs.items.end())
                break;
            if (const auto i = indexOf(c->identity); i >= 0) {
                list.items[static_cast<size_t>(i)] = *it->second;
            } else if (c->op == ChangeOp::Add && listOrderMatters(chain[0].sig, *shape)) {
                // An ordered list: after the nearest element before it in the source that the list has, else
                // before the nearest one after it, else at the end.
                const auto before = anchorBefore(theirs, mine, c->identity);
                const auto after = before.empty() ? anchorAfter(theirs, mine, c->identity) : std::string();
                size_t at = list.items.size();
                if (!before.empty())
                    at = static_cast<size_t>(indexOf(before)) + 1;
                else if (!after.empty())
                    at = static_cast<size_t>(indexOf(after));
                list.items.insert(list.items.begin() + static_cast<std::ptrdiff_t>(at), *it->second);
            } else {
                list.items.push_back(*it->second);
            }
            break;
        }
        case ChangeOp::Remove:
            if (const auto i = indexOf(c->identity); i >= 0)
                list.items.erase(list.items.begin() + i);
            break;
        case ChangeOp::Reorder: {
            // The slots the shared elements occupy keep their places; they are filled in the change's order.
            const auto wanted = commonOrder(theirs, mine);
            std::vector<size_t> slots;
            for (const auto& id : commonOrder(mine, theirs))
                slots.push_back(static_cast<size_t>(indexOf(id)));
            std::vector<ListItem> items = list.items;
            for (size_t k = 0; k < slots.size() && k < wanted.size(); ++k)
                items[slots[k]] = list.items[static_cast<size_t>(indexOf(wanted[k]))];
            list.items = std::move(items);
            break;
        }
        case ChangeOp::Set: break;
        }
    }
    std::vector<size_t> missing;
    for (size_t i = 0; i < changes.size(); ++i)
        if (!holds(result, chain, *changes[i]))
            missing.push_back(i);
    return missing;
}

} // namespace fo4mx
