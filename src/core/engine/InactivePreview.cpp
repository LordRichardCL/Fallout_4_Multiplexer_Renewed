#include "engine/InactivePreview.hpp"

#include "engine/ParallelFor.hpp"
#include "esp/PluginFile.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <map>

namespace fo4mx {

namespace {

// The group of the scanned load order holding `key`, or null when no active plugin has the record.
const FormGroup* findGroup(const Session& s, FormKey key)
{
    const uint64_t packed = key.packed();
    auto it = std::lower_bound(s.groups.begin(), s.groups.end(), packed, [&](const FormGroup& g, uint64_t k) {
        return s.records[g.begin].key.packed() < k;
    });
    if (it == s.groups.end() || s.records[it->begin].key.packed() != packed)
        return nullptr;
    return &*it;
}

} // namespace

std::vector<InactivePreviewRow> InactivePreview::run(const Session& session, ProgressSink& progress, unsigned threads)
{
    std::vector<PluginIndex> todo;
    for (const auto& p : session.plugins)
        if (!p.active && !p.generated && p.error.empty())
            todo.push_back(p.index);
    std::vector<InactivePreviewRow> rows(todo.size());
    if (todo.empty())
        return rows;

    progress.stage("Previewing inactive plugins");
    const auto& defs = session.defs;
    std::atomic<size_t> done{0};
    parallelFor(
        todo.size(),
        [&](size_t i) {
            const auto& p = session.plugins[todo[i]];
            auto& row = rows[i];
            row.plugin = p.index;
            row.name = p.name;
            for (size_t m = 0; m < p.masters.size(); ++m) {
                const PluginIndex mi = p.masters[m];
                if (mi == kInvalidPlugin || !session.plugins[mi].active)
                    row.missingMasters.push_back(mi == kInvalidPlugin
                                                     ? (m < p.header.masters.size() ? p.header.masters[m].name
                                                                                    : std::string("(unknown)"))
                                                     : session.plugins[mi].name);
            }
            std::map<std::string, size_t> against;
            std::map<Signature, size_t> keptTypes; // types it holds that compiling keeps as they are
            try {
                auto file = PluginFile::load(p.path);
                file.forEachRecord([&](const RecordLocation& loc) {
                    if ((row.records & 0xFFFF) == 0 && progress.cancelled())
                        return false;
                    ++row.records;
                    const Signature sig = loc.header.signature;
                    const FormKey key = session.resolve(p, loc.header.formId);
                    const bool isNew = key.origin == p.index;
                    if (isNew)
                        ++row.newRecords;
                    else
                        ++row.overrides;
                    if (!defs.isCompilable(sig) || (isNew && defs.overridesOnly(sig)))
                        ++keptTypes[sig];
                    if (isNew || key.origin == kInvalidPlugin)
                        return true;
                    // Another active plugin than the record's own also changes it: they would conflict.
                    if (const auto* g = findGroup(session, key)) {
                        bool otherEditor = false;
                        for (uint32_t k = g->begin; k < g->end; ++k)
                            otherEditor = otherEditor || session.records[k].plugin != key.origin;
                        if (otherEditor) {
                            ++row.conflicts;
                            ++against[session.plugins[session.records[g->end - 1].plugin].name];
                        }
                    }
                    return true;
                });
            } catch (const std::exception& e) {
                row.error = e.what();
            }
            row.conflictsWith.assign(against.begin(), against.end());
            std::stable_sort(row.conflictsWith.begin(), row.conflictsWith.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });

            if (!row.error.empty()) {
                row.reason = "Could not be read: " + row.error;
            } else if (p.baseGame || p.creationClub) {
                row.reason = "Base game and Creation Club plugins are never compiled.";
            } else if (!row.missingMasters.empty()) {
                row.reason = "Needs " + row.missingMasters.front() +
                             (row.missingMasters.size() > 1
                                  ? " and " + std::to_string(row.missingMasters.size() - 1) + " more master(s)"
                                  : std::string()) +
                             ", not active: enable them too, or it cannot load.";
            } else if (!keptTypes.empty()) {
                const auto worst = std::max_element(keptTypes.begin(), keptTypes.end(),
                                                    [](const auto& a, const auto& b) { return a.second < b.second; });
                row.reason = "Would likely stay in the load order: it holds " + std::to_string(worst->second) + " " +
                             defs.recordName(worst->first) + " (" + worst->first.str() +
                             ") record(s), which compiling keeps as they are" +
                             (keptTypes.size() > 1 ? " (and " + std::to_string(keptTypes.size() - 1) + " more type(s))"
                                                   : std::string()) +
                             ".";
            } else {
                row.likelyCompiles = true;
                row.reason = "Likely to compile: its masters are active and every record type it holds can be "
                             "compiled. Scripts, assets and references are checked once it is enabled and scanned.";
            }
            progress.progress(++done, todo.size(), p.name);
        },
        progress, threads);
    spdlog::info("Previewed {} inactive plugins", rows.size());
    return rows;
}

} // namespace fo4mx
