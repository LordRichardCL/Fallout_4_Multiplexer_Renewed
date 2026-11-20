#include "engine/Redirection.hpp"

#include "engine/FieldModel.hpp"
#include "engine/Normalizer.hpp"
#include "engine/PrevisCheck.hpp"
#include "esp/PluginFile.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

// One record the copy writes: its fields (FormKeys) and where it sits.
struct Planned {
    FieldSet fields;
    Placement::Kind placement = Placement::Kind::Top;
    bool persistent = false;
    FormKey parent;
    uint32_t vc1 = 0;
    uint16_t vc2 = 0;
    bool own = false; // the plugin's own version (else: the game's current version, as a container)
};

FormKey parentOf(const Session& session, const RecordInstance& r)
{
    const auto kind = static_cast<Placement::Kind>(r.placement);
    if (kind == Placement::Kind::Top || kind == Placement::Kind::InteriorCell || r.parentRaw == 0)
        return {};
    return session.resolve(session.plugins[r.plugin], r.parentRaw);
}

void build(Session& session, const CompiledOutput& out, RedirectedPlugin& rp,
           const std::vector<std::pair<uint32_t, uint32_t>>& mine, // (group, index in the group)
           bool translatePrevis)
{
    const PluginIndex q = rp.plugin;
    const auto& qp = session.plugins[q];
    auto fail = [&](std::string why) {
        if (rp.failure.empty())
            rp.failure = std::move(why);
    };
    // An output that needs the plugin as a master would load before it while its copy loads after the
    // outputs: they cannot both hold.
    for (const auto& op : out.outputs)
        if (op.hasRecords() && std::find(op.retainedMasters.begin(), op.retainedMasters.end(), q) != op.retainedMasters.end())
            return fail(op.name + " needs it as a master (records written there use its records), and its copy "
                                  "would need the outputs");

    Normalizer norm(session);
    auto fieldsOf = [&](const RecordInstance& r) {
        auto nr = norm.normalize(r);
        for (const auto& s : nr.subs)
            if (s.heuristic || !s.opaque.empty())
                throw std::runtime_error(session.formLabel(r.key) + " has data the app cannot place");
        return splitFields(nr, session.defs);
    };

    std::map<FormKey, Planned> plan; // by key: one record each
    std::vector<FormKey> order;      // write order (as the plugin has them)
    std::set<FormKey> replaced;      // overrides left to the outputs or later plugins
    try {
        for (const auto& [gi, idx] : mine) {
            const auto inst = session.instances(session.groups[gi]);
            const auto& r = inst[idx];
            // A later plugin that stays wins over it, as today. A later compiled plugin counts only through the
            // outputs: when they do not hold the record, its final version equals the last version of a plugin
            // that stays (the compiler writes no override identical to it), which may be this one.
            bool later = false;
            for (size_t j = idx + 1; j < inst.size(); ++j)
                later |= !out.isCompiled(inst[j].plugin);
            const bool held = out.indexByKey.count(r.key) != 0;
            if (r.key.origin == q) {
                if (later || held)
                    return fail(session.formLabel(r.key) + ", one of its own records, is changed by " +
                                (held ? std::string("the outputs") : session.pluginName(inst.back().plugin)));
            } else if (later || held) {
                replaced.insert(r.key);
                continue;
            }
            Planned p;
            p.fields = fieldsOf(r);
            p.placement = static_cast<Placement::Kind>(r.placement);
            p.persistent = r.persistent;
            p.parent = parentOf(session, r);
            p.vc1 = r.versionControl1;
            p.vc2 = r.versionControl2;
            p.own = true;
            plan.emplace(r.key, std::move(p));
            order.push_back(r.key);
        }
        // Containers: a kept record's cell, worldspace, topic or quest must be in the copy too. One the copy
        // does not keep is written as the game shows it today (the outputs' version, else the last plugin's).
        std::vector<FormKey> todo;
        for (const auto& k : order)
            todo.push_back(plan.at(k).parent);
        while (!todo.empty()) {
            const FormKey k = todo.back();
            todo.pop_back();
            if (!k.valid() || plan.count(k))
                continue;
            Planned p;
            if (auto it = out.indexByKey.find(k); it != out.indexByKey.end()) {
                const auto& e = out.records[it->second];
                p.fields = e.fields;
                p.placement = e.placement;
                p.persistent = e.persistent;
                p.parent = e.parent;
            } else {
                const auto* g = session.findGroup(k);
                if (!g)
                    return fail("the container " + session.formLabel(k) + " of its records is not in the load order");
                // Not in the outputs: the game shows the last version of a plugin that stays (a compiled
                // plugin's identical version was not written).
                const auto chain = session.instances(*g);
                size_t wi = chain.size() - 1;
                while (wi > 0 && out.isCompiled(chain[wi].plugin))
                    --wi;
                const auto& w = chain[wi];
                p.fields = fieldsOf(w);
                p.placement = static_cast<Placement::Kind>(w.placement);
                p.persistent = w.persistent;
                p.parent = parentOf(session, w);
                p.vc1 = w.versionControl1;
                p.vc2 = w.versionControl2;
            }
            if (const auto* g = session.findGroup(k); g && out.indexByKey.count(k)) {
                const auto& w = session.instances(*g).back();
                p.vc1 = w.versionControl1;
                p.vc2 = w.versionControl2;
            }
            todo.push_back(p.parent);
            plan.emplace(k, std::move(p));
            order.push_back(k);
            replaced.erase(k);
            ++rp.winnerCopies;
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    rp.dropped = replaced.size();

    // Master list: its own masters that stay, the plugins today's versions of containers name, then the
    // outputs it refers to. A compiled master it no longer needs is left out.
    std::set<PluginIndex> retained;
    std::set<int> outs;
    auto need = [&](const FormKey& k) -> bool {
        if (!k.valid() || k.origin == q)
            return true;
        if (out.isCompiled(k.origin)) {
            auto it = out.indexByKey.find(k);
            if (it == out.indexByKey.end())
                return false;
            outs.insert(out.records[it->second].output);
        } else {
            retained.insert(k.origin);
        }
        return true;
    };
    for (PluginIndex m : qp.masters)
        if (m != kInvalidPlugin && !out.isCompiled(m))
            retained.insert(m);
    for (const auto& k : order) {
        const auto& p = plan.at(k);
        if (!need(k) || !need(p.parent))
            return fail("the record " + session.formLabel(k) + " is compiled but not in the outputs");
        for (const auto& [rk, via] : collectRefs(p.fields))
            if (!need(rk))
                return fail(session.formLabel(k) + " refers to " + session.formLabel(rk) +
                            ", which is compiled but not in the outputs");
    }
    // A master that is redirected too would load after the outputs as well, in an order the copies cannot
    // both rely on.
    for (PluginIndex m : retained)
        if (session.redirected.count(m) && m != q)
            return fail("its copy would need " + session.plugins[m].name + ", which is redirected too, as a master");
    std::vector<PluginIndex> retainedOrdered(retained.begin(), retained.end());
    std::sort(retainedOrdered.begin(), retainedOrdered.end(), [&](PluginIndex a, PluginIndex b) {
        return session.plugins[a].loadOrder < session.plugins[b].loadOrder;
    });
    std::vector<int> outputs(outs.begin(), outs.end());
    for (auto pi : retainedOrdered)
        rp.masterNames.push_back(session.plugins[pi].name);
    for (int o : outputs)
        rp.masterNames.push_back(out.outputName(o));
    if (rp.masterNames.size() > 254)
        return fail("its copy would need " + std::to_string(rp.masterNames.size()) + " masters (limit 254)");
    for (PluginIndex m : qp.masters)
        if (m != kInvalidPlugin && out.isCompiled(m))
            rp.replacedMasters.push_back(session.plugins[m].name);

    const uint32_t self = static_cast<uint32_t>(rp.masterNames.size());
    auto translate = [&](const FormKey& k, bool& ok) -> uint32_t {
        if (!k.valid())
            return k.objectId == 0 ? 0 : (ok = false, 0u);
        if (k.origin == q)
            return makeRawFormId(self, k.objectId);
        if (out.isCompiled(k.origin)) {
            auto it = out.indexByKey.find(k);
            if (it == out.indexByKey.end())
                return ok = false, 0u;
            const auto& e = out.records[it->second];
            auto pos = std::find(outputs.begin(), outputs.end(), e.output);
            if (pos == outputs.end())
                return ok = false, 0u;
            ++rp.translated;
            return makeRawFormId(static_cast<uint32_t>(retainedOrdered.size() + (pos - outputs.begin())), e.objectId);
        }
        auto pos = std::find(retainedOrdered.begin(), retainedOrdered.end(), k.origin);
        if (pos == retainedOrdered.end())
            return ok = false, 0u;
        return makeRawFormId(static_cast<uint32_t>(pos - retainedOrdered.begin()), k.objectId);
    };

    // The header: the plugin's own (flags, version, author's description kept after the tag), its new
    // master list.
    rp.header = qp.header;
    rp.header.masters.clear();
    for (const auto& m : rp.masterNames)
        rp.header.masters.push_back({m, 0});
    rp.header.overriddenForms.clear();
    rp.header.extra.erase(std::remove_if(rp.header.extra.begin(), rp.header.extra.end(),
                                         [](const Subrecord& s) { return s.signature == Signature("ONAM"); }),
                          rp.header.extra.end());
    rp.header.description = std::string(kRedirectTag) + "; loads after: " + rp.after + "|" +
                            (qp.header.author.empty() ? std::string() : "by " + qp.header.author + ". ") +
                            qp.header.description;
    rp.header.author = kGeneratorAuthor;

    // Containers first, so a reader meets each before what it holds (the writer groups them anyway).
    std::stable_sort(order.begin(), order.end(), [&](const FormKey& a, const FormKey& b) {
        auto rank = [&](const Planned& p) {
            switch (p.placement) {
            case Placement::Kind::Top: return p.fields.sig == Signature("WRLD") || p.fields.sig == Signature("QUST") ? 0 : 3;
            case Placement::Kind::InteriorCell:
            case Placement::Kind::WorldCell: return 1;
            case Placement::Kind::QuestChild: return 2;
            default: return 4;
            }
        };
        return rank(plan.at(a)) < rank(plan.at(b));
    });
    for (const auto& k : order) {
        const auto& p = plan.at(k);
        bool ok = true;
        Record rec;
        rec.header.signature = p.fields.sig;
        rec.header.flags = p.fields.flags() & ~RecordFlags::Compressed;
        rec.header.formVersion = p.fields.formVersion ? p.fields.formVersion : 131;
        rec.header.versionControl1 = p.vc1;
        rec.header.versionControl2 = p.vc2;
        rec.header.formId = translate(k, ok);
        for (const auto& ns : joinFields(p.fields)) {
            Subrecord sr;
            sr.signature = ns.sig;
            sr.data = ns.bytes;
            for (const auto& [off, rk] : ns.refs)
                storeLE<uint32_t>(sr.data.data() + off, translate(rk, ok));
            rec.subrecords.push_back(std::move(sr));
        }
        Placement pl;
        pl.kind = p.placement;
        pl.persistent = p.persistent;
        if (p.parent.valid())
            pl.parent = translate(p.parent, ok);
        if (!ok)
            return fail("a FormID of " + session.formLabel(k) + " cannot be written in its copy");
        rec.header.dataSize = 0;
        rp.built.push_back(std::move(rec));
        rp.placements.push_back(pl);
        rp.ownVersions += p.own;
    }
    // Previs files: a cell whose record the copy holds wins in game, so its previs file is read in the copy's
    // numbering from then on. Each is translated from the numbering it is read in today, as for the outputs;
    // one that cannot be (or translation turned off while IDs would change) stops the redirection.
    {
        uint32_t nextUnused = 0x800;
        for (const auto& k : order)
            if (k.origin == q && k.objectId >= nextUnused)
                nextUnused = k.objectId + 1;
        const uint32_t maxObject = qp.isLight() ? 0xFFFu : 0xFFFFFFu;
        auto express = [&](FormKey k, uint32_t& raw) {
            bool ok = true;
            const size_t before = rp.translated;
            raw = translate(k, ok);
            rp.translated = before;
            return ok;
        };
        for (const auto& k : order) {
            const auto& p = plan.at(k);
            if (p.fields.sig != sig::CELL)
                continue;
            EmittedRecord cell;
            cell.key = k;
            cell.sig = sig::CELL;
            cell.fields = p.fields;
            PrevisTranslation t;
            try {
                t = translatePrevisInto(session, cell, rp.name + " (redirected copy)", express, self, nextUnused,
                                        maxObject);
            } catch (const std::exception& ex) {
                t.hasFile = true;
                t.ok = false;
                t.problem = ex.what();
            }
            if (!t.hasFile)
                continue;
            if (!t.ok)
                return fail("the previs file " + t.path + " of " + session.formLabel(k) +
                            ", whose record the copy holds, cannot be translated into the copy's numbering: " +
                            t.problem);
            // Unchanged files are kept too: an output may have written its own translation of the same file,
            // which the copy's record (loading after it) makes wrong.
            if (t.changed && !translatePrevis)
                return fail("the previs file " + t.path + " of " + session.formLabel(k) +
                            " would be read in the copy's numbering, and Translate previs files is off");
            nextUnused += static_cast<uint32_t>(t.nowhere);
            PrevisRewrite r;
            r.path = t.path;
            r.bytes = std::move(t.bytes);
            r.cell = k;
            r.source = t.source;
            r.numbering = t.numbering;
            r.output = rp.name;
            r.formIds = t.formIds;
            r.changed = t.changed;
            rp.previsFiles.push_back(std::move(r));
        }
    }
    // TIFC, a topic's response count, is the number of responses written under it.
    std::unordered_map<uint32_t, uint32_t> responses;
    for (const auto& pl : rp.placements)
        if (pl.kind == Placement::Kind::TopicChild)
            ++responses[pl.parent];
    for (auto& r : rp.built)
        if (r.signature() == Signature("DIAL"))
            if (auto* tifc = r.find(Signature("TIFC")); tifc && tifc->data.size() == 4) {
                const uint32_t n = responses.count(r.header.formId) ? responses[r.header.formId] : 0;
                std::memcpy(tifc->data.data(), &n, 4);
            }
}

} // namespace

void buildRedirected(Session& session, CompiledOutput& out)
{
    out.redirected.clear();
    if (session.redirected.empty())
        return;
    std::unordered_map<PluginIndex, size_t> slot;
    // In load order: the copies load after the outputs in the order the plugins loaded before.
    std::vector<PluginIndex> ordered(session.redirected.begin(), session.redirected.end());
    std::sort(ordered.begin(), ordered.end(), [&](PluginIndex a, PluginIndex b) {
        return session.plugins[a].loadOrder < session.plugins[b].loadOrder;
    });
    for (PluginIndex q : ordered) {
        RedirectedPlugin rp;
        rp.plugin = q;
        rp.name = session.plugins[q].name;
        // The plugin it loaded after, so a scan puts the original back there.
        const auto list = session.pluginsInListOrder();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i] == q) {
                for (size_t j = i; j-- > 0;)
                    if (!session.plugins[list[j]].generated && !session.redirected.count(list[j])) {
                        rp.after = session.plugins[list[j]].name;
                        break;
                    }
                break;
            }
        slot[q] = out.redirected.size();
        out.redirected.push_back(std::move(rp));
    }
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> mine(out.redirected.size());
    for (uint32_t gi = 0; gi < session.groups.size(); ++gi) {
        const auto inst = session.instances(session.groups[gi]);
        for (uint32_t i = 0; i < inst.size(); ++i)
            if (auto it = slot.find(inst[i].plugin); it != slot.end())
                mine[it->second].push_back({gi, i});
    }
    for (size_t i = 0; i < out.redirected.size(); ++i) {
        auto& rp = out.redirected[i];
        build(session, out, rp, mine[i], out.options.translatePrevis);
        if (!rp.failure.empty()) {
            rp.built.clear();
            rp.placements.clear();
            rp.previsFiles.clear();
            spdlog::info("Master Redirection: {} cannot be redirected: {}", rp.name, rp.failure);
        } else {
            // The copy loads after the outputs: where an output also holds the cell, the copy's record wins, so
            // its translation replaces the output's.
            for (auto& pr : rp.previsFiles) {
                out.previsFiles.erase(std::remove_if(out.previsFiles.begin(), out.previsFiles.end(),
                                                     [&](const PrevisRewrite& o) { return o.path == pr.path; }),
                                      out.previsFiles.end());
                out.previsFiles.push_back(pr);
            }
            spdlog::info("Master Redirection: {} ({} own records, {} containers as today, {} overrides left to "
                         "others, {} FormIDs now naming outputs; replaces {})",
                         rp.name, rp.ownVersions, rp.winnerCopies, rp.dropped, rp.translated,
                         join(rp.replacedMasters, ", "));
        }
    }
}

std::string checkRedirectedImage(const RedirectedPlugin& r, const std::vector<uint8_t>& image)
{
    try {
        const auto file = PluginFile::fromBytes(image);
        if (file.header().masters.size() != r.masterNames.size())
            return "its master list was not written as planned";
        for (size_t i = 0; i < r.masterNames.size(); ++i)
            if (!iequals(file.header().masters[i].name, r.masterNames[i]))
                return "its master list was not written as planned";
        std::unordered_map<uint32_t, const Record*> planned;
        for (const auto& rec : r.built)
            planned[rec.header.formId] = &rec;
        size_t seen = 0;
        std::string diff;
        file.forEachRecord([&](const RecordLocation& loc) {
            if (loc.header.signature == sig::TES4)
                return true;
            auto it = planned.find(loc.header.formId);
            if (it == planned.end()) {
                diff = "a record that was not planned was written";
                return false;
            }
            ++seen;
            const Record back = file.recordAt(loc.offset);
            const auto& want = *it->second;
            // TIFC and the cell reference lists are the writer's to keep consistent; everything else must be
            // exactly as planned.
            if (back.subrecords.size() != want.subrecords.size()) {
                diff = "a record was not written as planned";
                return false;
            }
            for (size_t i = 0; i < want.subrecords.size(); ++i)
                if (back.subrecords[i].signature != want.subrecords[i].signature ||
                    (back.subrecords[i].data != want.subrecords[i].data &&
                     want.subrecords[i].signature != Signature("TIFC"))) {
                    diff = "a record was not written as planned (" + want.subrecords[i].signature.str() + ")";
                    return false;
                }
            return true;
        });
        if (!diff.empty())
            return diff;
        if (seen != r.built.size())
            return "records are missing from the written copy";
    } catch (const std::exception& e) {
        return std::string("it cannot be read back (") + e.what() + ")";
    }
    return {};
}

std::optional<std::string> redirectedAfter(const std::string& description)
{
    if (description.rfind(kRedirectTag, 0) != 0)
        return std::nullopt;
    const std::string marker = "; loads after: ";
    const auto at = description.find(marker);
    if (at == std::string::npos)
        return std::string();
    const auto end = description.find('|', at + marker.size());
    return description.substr(at + marker.size(), end == std::string::npos ? std::string::npos : end - at - marker.size());
}

} // namespace fo4mx
