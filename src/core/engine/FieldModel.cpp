#include "engine/FieldModel.hpp"

#include "util/Text.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>

namespace fo4mx {

namespace {

const char* kFlagsKey = "@flags";

NormSub flagsSub(uint32_t flags)
{
    NormSub s;
    s.sig = Signature("HDRF");
    s.bytes.resize(4);
    storeLE<uint32_t>(s.bytes.data(), flags);
    return s;
}

// Split a packed array subrecord into per-element pseudo-subrecords.
std::vector<ListItem> splitPacked(const NormSub& s, uint16_t stride)
{
    std::vector<ListItem> items;
    if (stride == 0 || s.bytes.size() % stride != 0) {
        ListItem whole;
        whole.subs.push_back(s);
        items.push_back(std::move(whole));
        return items;
    }
    for (uint32_t e = 0; e < s.bytes.size(); e += stride) {
        NormSub el;
        el.sig = s.sig;
        el.heuristic = s.heuristic;
        el.bytes.assign(s.bytes.begin() + e, s.bytes.begin() + e + stride);
        for (const auto& [off, key] : s.refs)
            if (off >= e && off < e + stride)
                el.refs.emplace_back(off - e, key);
        for (const auto& [off, key] : s.softRefs)
            if (off >= e && off < e + stride)
                el.softRefs.emplace_back(off - e, key);
        ListItem item;
        item.subs.push_back(std::move(el));
        items.push_back(std::move(item));
    }
    return items;
}

std::string hexBytes(const std::vector<uint8_t>& b, size_t max = 24)
{
    std::string out;
    char buf[4];
    for (size_t i = 0; i < b.size() && i < max; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", b[i]);
        if (i)
            out.push_back(' ');
        out += buf;
    }
    if (b.size() > max)
        out += " …";
    return out;
}

bool printable(const std::vector<uint8_t>& b)
{
    if (b.empty() || b.back() != 0)
        return false;
    for (size_t i = 0; i + 1 < b.size(); ++i)
        if (b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t')
            return false;
    return true;
}

std::string describeSub(const NormSub& s, const std::function<std::string(FormKey)>& label)
{
    if (s.refs.size() == 1 && s.bytes.size() == 4)
        return s.refs[0].second.valid() || s.refs[0].second.objectId ? label(s.refs[0].second) : "NULL";
    if (printable(s.bytes))
        return "\"" + std::string(reinterpret_cast<const char*>(s.bytes.data()), s.bytes.size() - 1) + "\"";
    std::string out;
    if (s.bytes.size() == 4 && s.refs.empty()) {
        uint32_t u = loadLE<uint32_t>(s.bytes.data());
        float f;
        std::memcpy(&f, &u, 4);
        char buf[64];
        if (std::abs(f) > 1e-6f && std::abs(f) < 1e7f && (u & 0x7F800000u) != 0)
            std::snprintf(buf, sizeof(buf), "%u / %g", u, static_cast<double>(f));
        else
            std::snprintf(buf, sizeof(buf), "%u", u);
        return buf;
    }
    if (s.bytes.size() == 1)
        return std::to_string(s.bytes[0]);
    if (s.bytes.size() == 2)
        return std::to_string(loadLE<uint16_t>(s.bytes.data()));
    out = s.bytes.empty() ? std::string("(empty)") : hexBytes(s.bytes);
    for (const auto& [off, key] : s.refs)
        if (key.valid() || key.objectId)
            out += " → " + label(key);
    return out;
}

} // namespace

const Field* FieldSet::find(const std::string& key) const
{
    for (const auto& f : fields)
        if (f.key == key)
            return &f;
    return nullptr;
}

Field* FieldSet::find(const std::string& key)
{
    for (auto& f : fields)
        if (f.key == key)
            return &f;
    return nullptr;
}

uint32_t FieldSet::flags() const
{
    const Field* f = find(kFlagsKey);
    return f && !f->subs.empty() ? loadLE<uint32_t>(f->subs[0].bytes.data()) : 0;
}

FieldSet splitFields(const NormRecord& rec, const RecordDefinitions& defs)
{
    FieldSet set;
    set.key = rec.key;
    set.sig = rec.sig;
    set.plugin = rec.plugin;
    set.formVersion = rec.formVersion;

    Field flags;
    flags.key = kFlagsKey;
    flags.label = "Record Flags";
    flags.sig = Signature("HDRF");
    flags.subs.push_back(flagsSub(rec.flags));
    set.fields.push_back(std::move(flags));

    const RecordDef* rd = defs.record(rec.sig);

    // Count subrecords that belong to a list are regenerated on output.
    std::map<Signature, Signature> countToList;
    if (rd)
        for (const auto& [sub, def] : rd->fields)
            if (!def.countSig.empty())
                countToList[def.countSig] = sub;
    for (Signature s : {sig::KSIZ})
        countToList.emplace(s, sig::KWDA);
    std::map<Signature, bool> sawCount;

    std::map<std::string, int> occurrences;
    const auto& subs = rec.subs;
    size_t i = 0;
    while (i < subs.size()) {
        const NormSub& s = subs[i];
        if (auto it = countToList.find(s.sig); it != countToList.end()) {
            sawCount[it->second] = true;
            ++i;
            continue;
        }

        const RegionDef* region = nullptr;
        if (rd)
            for (const auto& r : rd->regions)
                if (r.startsWith(s.sig)) {
                    region = &r;
                    break;
                }

        if (region) {
            Field f;
            int occ = occurrences["region:" + region->name]++;
            f.key = "region:" + region->name + "#" + std::to_string(occ);
            f.label = occ ? region->name + " " + std::to_string(occ + 1) : region->name;
            f.shape = FieldShape::Region;
            f.sig = s.sig;
            size_t j = i;
            while (j < subs.size() && region->contains(subs[j].sig)) {
                f.subs.push_back(subs[j]);
                if (!region->end.empty() && subs[j].sig == region->end) {
                    ++j;
                    break;
                }
                ++j;
            }
            set.fields.push_back(std::move(f));
            i = j;
            continue;
        }

        const FieldDef& def = defs.field(rec.sig, s.sig);
        // Arrays with a header (arrayStart) stay whole: their items do not start at offset 0.
        if (def.merge != MergeMode::Scalar && def.isPackedArray() && def.arrayStart == 0) {
            std::string key = "array:" + s.sig.str();
            Field* existing = set.find(key);
            if (!existing) {
                Field f;
                f.key = key;
                f.label = s.sig.str();
                f.shape = FieldShape::PackedArray;
                f.sig = s.sig;
                f.merge = def.merge;
                f.countSig = def.countSig;
                f.countWidth = def.countWidth;
                f.stride = def.kind == FieldKind::FormIdArray ? 4 : def.stride;
                f.hadCount = sawCount[s.sig];
                set.fields.push_back(std::move(f));
                existing = &set.fields.back();
            }
            auto items = splitPacked(s, existing->stride);
            existing->items.insert(existing->items.end(), items.begin(), items.end());
            ++i;
            continue;
        }

        if (def.isRepeatedList()) {
            std::string key = "list:" + s.sig.str();
            Field* existing = set.find(key);
            if (!existing) {
                Field f;
                f.key = key;
                f.label = s.sig.str() + " list";
                f.shape = FieldShape::RepeatedList;
                f.sig = s.sig;
                f.merge = def.merge;
                f.countSig = def.countSig;
                f.countWidth = def.countWidth;
                f.hadCount = sawCount[s.sig];
                set.fields.push_back(std::move(f));
                existing = &set.fields.back();
            }
            size_t j = i;
            while (j < subs.size() && (subs[j].sig == s.sig || (!def.companion.empty() && subs[j].sig == def.companion &&
                                                                 !existing->items.empty() && j > i))) {
                if (subs[j].sig == s.sig)
                    existing->items.push_back(ListItem{{subs[j]}});
                else
                    existing->items.back().subs.push_back(subs[j]);
                ++j;
            }
            i = j;
            continue;
        }

        Field f;
        int occ = occurrences[s.sig.str()]++;
        f.key = s.sig.str() + "#" + std::to_string(occ);
        f.label = occ ? s.sig.str() + " " + std::to_string(occ + 1) : s.sig.str();
        f.shape = FieldShape::Scalar;
        f.sig = s.sig;
        f.subs.push_back(s);
        set.fields.push_back(std::move(f));
        ++i;
    }
    return set;
}

std::vector<NormSub> joinFields(const FieldSet& set)
{
    std::vector<NormSub> out;
    auto countSub = [](Signature sig, uint8_t width, size_t count) {
        NormSub c;
        c.sig = sig;
        c.bytes.resize(width);
        if (width == 1)
            c.bytes[0] = static_cast<uint8_t>(std::min<size_t>(count, 255));
        else if (width == 2)
            storeLE<uint16_t>(c.bytes.data(), static_cast<uint16_t>(count));
        else
            storeLE<uint32_t>(c.bytes.data(), static_cast<uint32_t>(count));
        return c;
    };

    for (const auto& f : set.fields) {
        if (f.key == kFlagsKey)
            continue;
        switch (f.shape) {
        case FieldShape::Scalar:
        case FieldShape::Region:
            out.insert(out.end(), f.subs.begin(), f.subs.end());
            break;
        case FieldShape::PackedArray: {
            if (f.items.empty())
                break;
            if (!f.countSig.empty())
                out.push_back(countSub(f.countSig, f.countWidth, f.items.size()));
            NormSub joined;
            joined.sig = f.sig;
            for (const auto& item : f.items) {
                for (const auto& el : item.subs) {
                    uint32_t base = static_cast<uint32_t>(joined.bytes.size());
                    joined.heuristic |= el.heuristic;
                    joined.bytes.insert(joined.bytes.end(), el.bytes.begin(), el.bytes.end());
                    for (const auto& [off, key] : el.refs)
                        joined.refs.emplace_back(base + off, key);
                    for (const auto& [off, key] : el.softRefs)
                        joined.softRefs.emplace_back(base + off, key);
                }
            }
            out.push_back(std::move(joined));
            break;
        }
        case FieldShape::RepeatedList:
            if (!f.countSig.empty() && (!f.items.empty() || f.hadCount))
                out.push_back(countSub(f.countSig, f.countWidth, f.items.size()));
            for (const auto& item : f.items)
                out.insert(out.end(), item.subs.begin(), item.subs.end());
            break;
        }
    }
    return out;
}

bool itemEquivalent(const ListItem& a, const ListItem& b)
{
    return equivalent(a.subs, b.subs);
}

FormKey itemKey(const ListItem& item)
{
    if (!item.subs.empty() && !item.subs[0].refs.empty())
        return item.subs[0].refs[0].second;
    return FormKey{};
}

bool fieldEquivalent(const Field* a, const Field* b)
{
    if (!a || !b) {
        auto empty = [](const Field* f) { return !f || (f->isList() && f->items.empty()); };
        return empty(a) && empty(b);
    }
    if (a->shape != b->shape)
        return false;
    if (!a->isList())
        return equivalent(a->subs, b->subs);
    if (a->items.size() != b->items.size())
        return false;
    for (size_t i = 0; i < a->items.size(); ++i)
        if (!itemEquivalent(a->items[i], b->items[i]))
            return false;
    return true;
}

std::string describeField(const Field* f, const std::function<std::string(FormKey)>& label, size_t maxLength)
{
    if (!f)
        return "(absent)";
    std::string out;
    if (f->key == kFlagsKey) {
        uint32_t flags = loadLE<uint32_t>(f->subs[0].bytes.data());
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", flags);
        out = buf;
        if (flags & RecordFlags::Deleted)
            out += " (Deleted)";
        return out;
    }
    if (f->isList()) {
        out = std::to_string(f->items.size()) + " item" + (f->items.size() == 1 ? "" : "s");
        if (!f->items.empty())
            out += ": ";
        for (size_t i = 0; i < f->items.size() && out.size() < maxLength; ++i) {
            if (i)
                out += "; ";
            const auto& first = f->items[i].subs[0];
            if (!first.refs.empty())
                out += label(first.refs[0].second);
            else
                out += describeSub(first, label);
            if (f->items[i].subs.size() > 1)
                out += " +" + std::to_string(f->items[i].subs.size() - 1);
        }
    } else {
        for (size_t i = 0; i < f->subs.size() && out.size() < maxLength; ++i) {
            if (i)
                out += " | ";
            if (f->shape == FieldShape::Region)
                out += f->subs[i].sig.str() + "=";
            out += describeSub(f->subs[i], label);
        }
    }
    if (out.size() > maxLength)
        out = out.substr(0, maxLength) + " …";
    return out;
}

std::vector<std::pair<FormKey, Signature>> collectRefs(const FieldSet& set, bool includeSoft)
{
    std::vector<std::pair<FormKey, Signature>> out;
    auto addSub = [&](const NormSub& s) {
        for (const auto& [_, k] : s.refs)
            if (k.valid() || k.objectId)
                out.emplace_back(k, s.sig);
        if (includeSoft)
            for (const auto& [_, k] : s.softRefs)
                out.emplace_back(k, s.sig);
    };
    for (const auto& f : set.fields) {
        for (const auto& s : f.subs)
            addSub(s);
        for (const auto& item : f.items)
            for (const auto& s : item.subs)
                addSub(s);
    }
    return out;
}

} // namespace fo4mx
