#include "engine/Normalizer.hpp"

#include "util/Text.hpp"

#include <algorithm>

namespace fo4mx {

namespace {

std::vector<uint8_t> maskSoft(const NormSub& s)
{
    std::vector<uint8_t> b = s.bytes;
    for (const auto& [off, _] : s.softRefs)
        if (off + 4 <= b.size())
            std::fill(b.begin() + off, b.begin() + off + 4, 0);
    return b;
}

} // namespace

bool equivalent(const NormSub& a, const NormSub& b)
{
    if (a.sig != b.sig || a.refs != b.refs || a.bytes.size() != b.bytes.size())
        return false;
    if (a.bytes == b.bytes)
        return a.softRefs == b.softRefs || (a.softRefs.empty() && b.softRefs.empty());
    if (!a.heuristic && !b.heuristic)
        return false;
    // Raw bytes differ: equal only if the differences are soft references to the same forms.
    if (a.softRefs != b.softRefs)
        return false;
    return maskSoft(a) == maskSoft(b);
}

bool equivalent(const std::vector<NormSub>& a, const std::vector<NormSub>& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!equivalent(a[i], b[i]))
            return false;
    return true;
}

NormRecord Normalizer::normalize(const RecordInstance& inst)
{
    Record rec = session_.loadRecord(inst);
    return normalize(rec, inst.plugin);
}

NormRecord Normalizer::normalize(const Record& rec, PluginIndex plugin)
{
    const auto& p = session_.plugins[plugin];
    NormRecord out;
    out.sig = rec.signature();
    out.plugin = plugin;
    out.flags = rec.header.flags & ~RecordFlags::Compressed;
    out.formVersion = rec.header.formVersion;
    out.key = session_.resolve(p, rec.header.formId);

    DecodedLayout layout = decodeLayout(session_.defs, out.sig, rec.subrecords);
    out.issues = std::move(layout.issues);

    out.subs.resize(rec.subrecords.size());
    for (size_t i = 0; i < rec.subrecords.size(); ++i) {
        out.subs[i].sig = rec.subrecords[i].signature;
        out.subs[i].bytes = rec.subrecords[i].data;
    }

    // Described FormIDs: resolve and zero.
    for (const auto& slot : layout.formIdSlots) {
        auto& s = out.subs[slot.sub];
        if (slot.offset + 4 > s.bytes.size())
            continue;
        uint32_t raw = loadLE<uint32_t>(s.bytes.data() + slot.offset);
        FormKey key;
        if (raw != 0) {
            key = session_.resolve(p, raw);
            if (!key.valid())
                key.objectId = raw; // keep the raw value for diagnostics
            if (masterIndexOf(raw) > p.masters.size())
                out.issues.push_back({Severity::Warning, s.sig,
                                      "FormID " + formatFormId(raw) + " uses a master index beyond the master list"});
        } else {
            key = FormKey{kInvalidPlugin, 0};
        }
        storeLE<uint32_t>(s.bytes.data() + slot.offset, 0);
        s.refs.emplace_back(slot.offset, key);
    }
    for (const auto& slot : layout.probableSlots) {
        auto& s = out.subs[slot.sub];
        if (slot.offset + 4 > s.bytes.size())
            continue;
        uint32_t raw = loadLE<uint32_t>(s.bytes.data() + slot.offset);
        if (raw == 0 || masterIndexOf(raw) > p.masters.size())
            continue;
        FormKey key = session_.resolve(p, raw);
        if (!session_.exists(key))
            continue;
        storeLE<uint32_t>(s.bytes.data() + slot.offset, 0);
        s.refs.emplace_back(slot.offset, key);
    }
    for (auto& s : out.subs)
        std::sort(s.refs.begin(), s.refs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // Undescribed bytes: look for 4-byte aligned values that resolve to existing forms.
    for (const auto& span : layout.heuristic) {
        auto& s = out.subs[span.sub];
        s.heuristic = true;
        for (uint32_t off = span.begin; off + 4 <= span.end && off + 4 <= s.bytes.size(); off += 4) {
            if (layout.slotCovers(span.sub, off))
                continue;
            uint32_t raw = loadLE<uint32_t>(s.bytes.data() + off);
            if (objectIdOf(raw) < 0x800 || masterIndexOf(raw) > p.masters.size())
                continue;
            FormKey key = session_.resolve(p, raw);
            if (session_.exists(key) && key.objectId >= 0x800)
                s.softRefs.emplace_back(off, key);
        }
        // FormIDs are not always 4-byte aligned (weapon sounds sit at +73). At the other offsets
        // only values naming a record in a non-base master count: those are the ones whose master
        // index can change in the output, and requiring it keeps chance matches rare.
        for (uint32_t off = span.begin; off + 4 <= span.end && off + 4 <= s.bytes.size(); ++off) {
            if ((off - span.begin) % 4 == 0 || layout.slotCovers(span.sub, off))
                continue;
            bool overlaps = false;
            for (const auto& [o, _] : s.softRefs)
                overlaps |= off < o + 4 && o < off + 4;
            if (overlaps)
                continue;
            uint32_t raw = loadLE<uint32_t>(s.bytes.data() + off);
            uint32_t mi = masterIndexOf(raw);
            if (mi == 0 || mi > p.masters.size() || objectIdOf(raw) < 0x800)
                continue;
            FormKey key = session_.resolve(p, raw);
            if (key.valid() && !session_.plugins[key.origin].baseGame && session_.exists(key))
                s.softRefs.emplace_back(off, key);
        }
        std::sort(s.softRefs.begin(), s.softRefs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    // Localised strings become inline text so output plugins need no string tables.
    if (p.header.isLocalized()) {
        const StringTables* tables = nullptr;
        for (const auto& lf : layout.localized) {
            auto& s = out.subs[lf.sub];
            if (s.bytes.size() != 4)
                continue;
            if (!tables)
                tables = session_.stringTables(plugin);
            uint32_t id = loadLE<uint32_t>(s.bytes.data());
            auto text = tables->lookup(id, lf.file);
            if (!text) {
                out.issues.push_back({Severity::Warning, s.sig,
                                      "localised string " + formatFormId(id) + " not found in " + p.baseName() +
                                          " string tables"});
                text = std::string();
            }
            s.bytes.assign(text->begin(), text->end());
            s.bytes.push_back(0);
        }
    }
    return out;
}

} // namespace fo4mx
