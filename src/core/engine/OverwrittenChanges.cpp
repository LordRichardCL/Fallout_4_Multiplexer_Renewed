#include "engine/OverwrittenChanges.hpp"

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

OverwrittenChanges findOverwrittenChanges(const Session& session, const ConflictReport* conflicts,
                                          std::string_view pluginName, bool includeMadeFor)
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
        bool touched = false, lostHere = false;
        for (const auto& t : e.changes) {
            if (!counts(t.source))
                continue;
            touched = true;
            ++r.changes;
            if (!isLost(t.status))
                continue;
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
    if (r.lost == 0) {
        line("The winning version of every one of them holds all of those changes: no later plugin overwrites them.");
        return out;
    }
    line(formatCount(r.lost) + " of those changes, in " + formatCount(r.lostRecords) +
         " records, are not in the winning version: in game those records work as the later plugin has them.");
    line("This describes the load order as scanned. A Multiplexer build keeps the same winning versions (adding "
         "only the changes the Compatibility Patch option carries), so the result is the same with or without it.");
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
