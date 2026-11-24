#include "engine/OverwrittenChanges.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/PatchPlan.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <set>

namespace fo4mx {

namespace {

std::string lowerCopy(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string counted(size_t n, const char* one, const char* many)
{
    return formatCount(n) + " " + (n == 1 ? one : many);
}

std::string shorten(std::string s, size_t max)
{
    if (s.size() > max)
        s = s.substr(0, max) + "…";
    return s;
}

} // namespace

std::unordered_set<std::string> changesCarriedByBuild(const PatchPlan* plan, const CompiledOutput* compiled,
                                                      bool applyPatchPlan)
{
    std::unordered_set<std::string> ids;
    if (!applyPatchPlan || !plan || !compiled)
        return ids;
    std::set<uint64_t> withoutPlan; // records written as they would be without the plan
    for (const auto& i : compiled->issues)
        if (i.category == "Compatibility Patch" && i.severity == Severity::Warning)
            withoutPlan.insert(i.form.packed());
    for (const auto& pr : plan->records) {
        if (pr.status != PlanStatus::Ready || withoutPlan.count(pr.conflict->key.packed()))
            continue;
        for (const auto& pe : pr.entries)
            if (pe.action == PlanAction::CarryForward)
                ids.insert(pr.conflict->changes[pe.change].changeId);
    }
    return ids;
}

OverwrittenChanges findOverwrittenChanges(const Session& session, const ConflictReport* conflicts,
                                          std::string_view pluginName, bool includeMadeFor,
                                          const std::unordered_set<std::string>* carried)
{
    OverwrittenChanges r;
    const std::string key = lowerCopy(pluginName);
    for (auto p : session.loadOrder)
        if (session.plugins[p].key == key) {
            r.plugin = p;
            r.found = true;
        }
    if (!r.found)
        return r;
    for (auto p : session.loadOrder) {
        const auto& ms = session.plugins[p].masters;
        if (p != r.plugin && std::find(ms.begin(), ms.end(), r.plugin) != ms.end())
            r.madeFor.push_back(p);
    }
    if (!conflicts)
        return r;
    r.analysed = true;

    const std::set<PluginIndex> madeFor(r.madeFor.begin(), r.madeFor.end());
    auto counts = [&](PluginIndex source) {
        return source == r.plugin || (includeMadeFor && madeFor.count(source));
    };
    std::map<PluginIndex, Overwriter> by;
    std::map<PluginIndex, const ConflictEntry*> lastEntry; // counts each record once per overwriter
    for (const auto& e : conflicts->entries) {
        bool touched = false, lostHere = false, mergedHere = false, carriedHere = false;
        for (const auto& t : e.changes) {
            if (!counts(t.source))
                continue;
            touched = true;
            ++r.changes;
            if (!isLost(t.status))
                continue;
            if (carried && carried->count(t.changeId)) {
                ++r.carried;
                carriedHere = true;
                continue;
            }
            if (t.inMerged) {
                ++r.merged;
                mergedHere = true;
                continue;
            }
            ++r.lost;
            lostHere = true;
            const PluginIndex at = t.lostAt != kInvalidPlugin ? t.lostAt : e.winner;
            auto& o = by[at];
            o.plugin = at;
            if (lastEntry[at] != &e) {
                lastEntry[at] = &e;
                ++o.records;
                ++o.types[e.sig.str()];
            }
            if (t.source == r.plugin)
                ++o.changes;
            else
                ++o.patchChanges;
            if (t.status != ChangeStatus::OmittedLater)
                ++o.deliberate;
            for (auto k : t.keptBy)
                if (k != at)
                    ++o.keptBefore[k];
            if (o.examples.size() < 8) {
                std::string change = t.item.empty() ? t.before + " → " + t.after
                                                    : std::string(changeOpName(t.op)) + " " + t.item;
                o.examples.push_back({session.formLabel(e.key), t.field, shorten(change, 140),
                                      session.pluginName(t.source)});
            }
        }
        r.records += touched;
        r.lostRecords += lostHere;
        r.mergedRecords += mergedHere;
        r.carriedRecords += carriedHere;
    }
    for (auto& [p, o] : by)
        r.overwriters.push_back(std::move(o));
    std::sort(r.overwriters.begin(), r.overwriters.end(), [](const Overwriter& a, const Overwriter& b) {
        const size_t na = a.changes + a.patchChanges, nb = b.changes + b.patchChanges;
        return na != nb ? na > nb : a.plugin < b.plugin;
    });
    return r;
}

std::string formatOverwrittenChanges(const Session& session, const OverwrittenChanges& r, std::string_view pluginName,
                                     size_t examplesPerPlugin)
{
    std::string out;
    auto line = [&](const std::string& s) { out += s + "\n"; };
    auto where = [&](PluginIndex p) {
        const auto& pl = session.plugins[p];
        return pl.name + (pl.loadOrderHex.empty() ? "" : " [" + pl.loadOrderHex + "]");
    };
    if (!r.found) {
        line(std::string(pluginName) + " is not active in the scanned load order.");
        return out;
    }
    const std::string name = session.plugins[r.plugin].name;
    const std::string title = "Changes " + name + " makes that later plugins overwrite";
    line(title);
    line(std::string(title.size(), '='));
    line("Load order: " + where(r.plugin) + ". Plugins made for it (with it as a master): " +
         formatCount(r.madeFor.size()) + (r.madeFor.empty() ? "." : ", counted with it."));
    if (!r.analysed) {
        line("");
        line("The conflict analysis has not run yet: scan and analyse first.");
        return out;
    }
    line("");
    if (r.records == 0) {
        line("No other plugin changes the records it changes: nothing of it can be overwritten by a later plugin.");
        return out;
    }
    line("It and the plugins made for it change " + formatCount(r.records) +
         " records that other plugins change too, with " + formatCount(r.changes) + " changes.");
    const std::string mergedNote =
        r.merged ? counted(r.merged, "more change", "more changes") + ", in " +
                       counted(r.mergedRecords, "record", "records") + " (lists and other fields a build merges), are not in the last plugin's version but are "
                       "in the merged version a Multiplexer build writes: with a build they are kept."
                 : std::string();
    const std::string carriedNote =
        r.carried ? counted(r.carried, "more change", "more changes") + ", in " +
                        counted(r.carriedRecords, "record", "records") +
                        ", are not in the winning version but the last build carried them (Compatibility Patch)."
                  : std::string();
    if (r.lost == 0) {
        line("No later plugin overwrites them: the winning version holds all of those changes" +
             std::string(r.merged || r.carried ? ", or the build keeps them." : "."));
        if (r.merged)
            line(mergedNote);
        if (r.carried)
            line(carriedNote);
        return out;
    }
    line(formatCount(r.lost) + " of those changes, in " + formatCount(r.lostRecords) +
         " records, are not in the winning version: in game those records work as the later plugin has them.");
    line("This holds with or without a Multiplexer build: a build keeps the same winning versions for these "
         "records (adding only the changes the Compatibility Patch option carries).");
    if (r.merged)
        line(mergedNote);
    if (r.carried)
        line(carriedNote);
    line("");

    std::vector<const Overwriter*> unaware, onPurpose;
    for (const auto& o : r.overwriters)
        (o.deliberate * 2 >= o.changes + o.patchChanges ? onPurpose : unaware).push_back(&o);

    auto describe = [&](const Overwriter& o) {
        std::string types;
        for (const auto& [t, n] : o.types)
            types += (types.empty() ? "" : ", ") + t + " " + formatCount(n);
        std::string counts = counted(o.changes + o.patchChanges, "change", "changes") + " in " +
                             counted(o.records, "record", "records") + " (" + types + ")";
        if (o.patchChanges)
            counts += "; " + formatCount(o.patchChanges) + " of them made by plugins made for " + name;
        line("  " + where(o.plugin));
        line("    " + counts);
        if (!o.keptBefore.empty()) {
            std::vector<std::pair<size_t, PluginIndex>> kept;
            for (const auto& [p, n] : o.keptBefore)
                kept.push_back({n, p});
            std::sort(kept.rbegin(), kept.rend());
            std::string k;
            for (size_t i = 0; i < kept.size() && i < 3; ++i)
                k += (i ? ", " : "") + session.plugins[kept[i].second].name;
            line("    Kept until it loads by: " + k);
        }
        for (size_t i = 0; i < o.examples.size() && i < examplesPerPlugin; ++i) {
            const auto& ex = o.examples[i];
            line("    e.g. " + ex.form + "  " + ex.field + ": " + ex.change +
                 (ex.madeBy == name ? "" : "  (by " + ex.madeBy + ")"));
        }
    };
    if (!unaware.empty()) {
        line("Overwritten by plugins made without it (they load later and never had its changes):");
        for (auto* o : unaware)
            describe(*o);
        line("");
    }
    if (!onPurpose.empty()) {
        line("Changed by plugins made with it as a master (usually on purpose: patches that adjust it):");
        for (auto* o : onPurpose)
            describe(*o);
        line("");
    }
    line("What you can do:");
    if (!unaware.empty()) {
        line("  - Load the plugins in the first list before " + name +
             " (and before its patches), then scan and build again. Their own changes to these records are then "
             "lost instead: pick which mod should win.");
        line("  - Or use a compatibility patch made for both mods, loaded after both; or turn one of them off.");
        line("  - The Conflict Report's \"Changes not in the winning version\" lists every change; the Compatibility "
             "Patch option can carry the ones marked Green.");
    }
    if (!onPurpose.empty())
        line("  - The second list is normally fine: check those plugins only if they are patches for another version "
             "of " + name + ".");
    return out;
}

} // namespace fo4mx
