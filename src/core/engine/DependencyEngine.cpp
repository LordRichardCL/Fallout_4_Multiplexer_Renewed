#include "engine/DependencyEngine.hpp"

#include "engine/BuildCache.hpp"
#include "engine/FieldModel.hpp"
#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

std::vector<PluginIndex> DependencyEngine::dependents(const Session& session, PluginIndex plugin)
{
    std::vector<PluginIndex> out;
    for (const auto& p : session.plugins)
        if (std::find(p.masters.begin(), p.masters.end(), plugin) != p.masters.end())
            out.push_back(p.index);
    return out;
}

namespace {

// What keeps one plugin from compilation, independent of its masters and dependents.
struct IntrinsicBlock {
    bool candidate = false; // active, readable, not base game, Creation Club or excluded
    bool otherReason = false; // blocked by something supporting a record type cannot fix
    std::vector<Signature> types;
};

// Plugins eligible when `supported` types are added to the compilable set,
// including the rule that plugins staying in the load order keep their masters.
size_t countEligible(const Session& session, const std::vector<IntrinsicBlock>& blocks,
                     const std::unordered_set<Signature>& supported)
{
    std::vector<uint8_t> eligible(session.plugins.size(), 0);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& b = blocks[i];
        eligible[i] = b.candidate && !b.otherReason &&
                      std::all_of(b.types.begin(), b.types.end(), [&](Signature s) { return supported.count(s) != 0; });
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& q : session.plugins) {
            if (!q.active || eligible[q.index])
                continue;
            for (auto m : q.masters)
                if (m != kInvalidPlugin && eligible[m]) {
                    eligible[m] = 0;
                    changed = true;
                }
        }
    }
    return static_cast<size_t>(std::count(eligible.begin(), eligible.end(), 1));
}

void reportBlockers(const Session& session, const std::vector<IntrinsicBlock>& blocks, DependencyReport& report)
{
    std::unordered_set<Signature> supported(session.defs.compilableTypes().begin(), session.defs.compilableTypes().end());
    const size_t baseline = countEligible(session, blocks, supported);

    std::map<Signature, TypeBlocker> byType;
    std::unordered_set<Signature> all = supported;
    report.blockedByTypes = 0;
    for (const auto& b : blocks) {
        if (!b.candidate || b.types.empty())
            continue;
        if (!b.otherReason)
            report.blockedByTypes++;
        size_t index = static_cast<size_t>(&b - blocks.data());
        for (Signature s : b.types) {
            auto& t = byType[s];
            t.sig = s;
            t.name = session.defs.recordName(s);
            t.plugins++;
            auto it = session.plugins[index].typeCounts.find(s);
            t.records += it == session.plugins[index].typeCounts.end() ? 0 : it->second;
            if (b.types.size() == 1 && !b.otherReason)
                t.soleBlocker++;
            all.insert(s);
        }
    }
    report.blockers.clear();
    for (auto& [s, t] : byType) {
        auto with = supported;
        with.insert(s);
        t.unlocks = countEligible(session, blocks, with) - baseline;
        report.blockers.push_back(t);
    }
    std::sort(report.blockers.begin(), report.blockers.end(), [](const TypeBlocker& a, const TypeBlocker& b) {
        if (a.unlocks != b.unlocks)
            return a.unlocks > b.unlocks;
        if (a.plugins != b.plugins)
            return a.plugins > b.plugins;
        return a.sig.str() < b.sig.str();
    });
    report.unlockedByAll = countEligible(session, blocks, all) - baseline;
}

} // namespace

void DependencyEngine::evaluateEligibility(Session& session, const AnalysisOptions& options, DependencyReport* report)
{
    std::vector<IntrinsicBlock> blocks(session.plugins.size());
    std::unordered_set<std::string> excluded = session.userRetained;
    for (const auto& e : options.excludedPlugins)
        excluded.insert(toLower(e));

    // Plugin-named asset folders (FaceGen, voice files) tie assets to the file name.
    std::map<std::string, size_t> namedAssets;
    for (const auto& e : session.assets.entries) {
        const auto& p = session.assets.path(e);
        for (const char* prefix : {"meshes\\actors\\character\\facegendata\\facegeom\\",
                                   "textures\\actors\\character\\facecustomization\\", "sound\\voice\\"}) {
            if (!istartsWith(p, prefix))
                continue;
            auto rest = std::string_view(p).substr(std::strlen(prefix));
            auto slash = rest.find('\\');
            if (slash != std::string_view::npos)
                namedAssets[toLower(rest.substr(0, slash))]++;
        }
    }

    std::map<PluginIndex, std::vector<const PluginNameReference*>> nameRefs;
    for (const auto& r : session.nameReferences)
        nameRefs[r.plugin].push_back(&r);

    for (auto& p : session.plugins) {
        p.reasons.clear();
        if (!p.active) {
            p.eligibility = Eligibility::Inactive;
            p.reasons.push_back("Not enabled in the load order");
            continue;
        }
        if (!p.error.empty() || !p.scanned) {
            p.eligibility = Eligibility::Error;
            p.reasons.push_back(p.error.empty() ? "Not scanned" : p.error);
            continue;
        }
        if (!p.missingMasters.empty()) {
            p.eligibility = Eligibility::Error;
            p.reasons.push_back("Missing masters: " + join(p.missingMasters, ", "));
            continue;
        }
        if (p.baseGame) {
            p.eligibility = Eligibility::Retained;
            p.reasons.push_back("Base game master");
            continue;
        }
        if (p.creationClub && !options.compileCreationClub) {
            p.eligibility = Eligibility::Retained;
            p.reasons.push_back("Creation Club content");
            continue;
        }
        if (excluded.count(p.key)) {
            p.eligibility = Eligibility::Retained;
            p.reasons.push_back("Excluded by user");
            continue;
        }

        p.eligibility = Eligibility::Eligible;
        auto& block = blocks[p.index];
        block.candidate = true;
        std::vector<std::string> unsupported;
        for (const auto& [s, n] : p.typeCounts)
            if (!session.defs.isCompilable(s)) {
                unsupported.push_back(s.str() + " (" + std::to_string(n) + ")");
                block.types.push_back(s);
            }
        if (!unsupported.empty()) {
            p.eligibility = Eligibility::Ineligible;
            if (unsupported.size() > 6) {
                size_t extra = unsupported.size() - 6;
                unsupported.resize(6);
                unsupported.push_back("+" + std::to_string(extra) + " more types");
            }
            p.reasons.push_back("Contains record types not yet compiled: " + join(unsupported, ", "));
        }
        if (auto it = nameRefs.find(p.index); it != nameRefs.end()) {
            block.otherReason = true;
            p.eligibility = Eligibility::Ineligible;
            std::string where = it->second.front()->assetPath;
            p.reasons.push_back("Referenced by file name in " + std::to_string(it->second.size()) + " script/config file" +
                                (it->second.size() == 1 ? "" : "s") + " (e.g. " + where + ")");
        }
        if (auto it = namedAssets.find(p.key); it != namedAssets.end()) {
            block.otherReason = true;
            p.eligibility = Eligibility::Ineligible;
            p.reasons.push_back(std::to_string(it->second) + " assets are stored in folders named after the plugin");
        }
    }

    // A plugin that stays in the load order keeps its masters in the load order too.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& q : session.plugins) {
            if (!q.active || q.eligibility == Eligibility::Eligible)
                continue;
            for (auto m : q.masters) {
                if (m == kInvalidPlugin)
                    continue;
                auto& mp = session.plugins[m];
                if (mp.eligibility == Eligibility::Eligible) {
                    mp.eligibility = Eligibility::Ineligible;
                    mp.reasons.push_back("Required as a master by " + q.name + ", which stays in the load order");
                    changed = true;
                }
            }
        }
    }

    if (report) {
        report->eligiblePlugins = 0;
        report->eligibleRecords = 0;
        for (const auto& p : session.plugins)
            if (p.eligibility == Eligibility::Eligible) {
                report->eligiblePlugins++;
                report->eligibleRecords += p.recordCount;
            }
        reportBlockers(session, blocks, *report);
    }
}


DependencyReport DependencyEngine::analyse(Session& session, const AnalysisOptions& options, ProgressSink& progress)
{
    progress.stage("Analysing dependencies");
    DependencyReport report;

    for (const auto& p : session.plugins) {
        if (!p.active)
            continue;
        report.pluginEdges += p.masters.size();
        for (const auto& m : p.missingMasters)
            report.issues.push_back({Severity::Error, "Missing Master", p.index, {}, p.name + " requires " + m});
        if (!p.error.empty())
            report.issues.push_back({Severity::Error, "Unreadable Plugin", p.index, {}, p.name + ": " + p.error});
    }

    // Record-level references of every winning compilable record.
    std::vector<uint32_t> targets;
    for (uint32_t gi = 0; gi < session.groups.size(); ++gi) {
        const auto& w = session.records[session.groups[gi].end - 1];
        if (session.defs.isCompilable(w.signature) && !(w.flags & RecordFlags::Deleted))
            targets.push_back(gi);
    }

    // References of each winning record: restored from the cache when unchanged.
    std::vector<std::optional<std::vector<std::pair<FormKey, Signature>>>> refs(targets.size());
    std::vector<uint64_t> keys(targets.size(), 0);
    std::vector<std::string> unreadable(targets.size());
    BuildCache* cache = session.cache;
    if (cache) {
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& g = session.groups[targets[i]];
            keys[i] = chainSignature(session, FormGroup{g.end - 1, g.end}) ^ 0xED6E5ULL;
            if (auto blob = cache->get("edges", keys[i]))
                refs[i] = deserializeEdges(session, *blob);
            (refs[i] ? cache->edges.hits : cache->edges.misses)++;
        }
    }
    std::vector<uint8_t> computed(targets.size(), 0);
    std::atomic<size_t> done{0};
    parallelFor(
        targets.size(),
        [&](size_t i) {
            if (refs[i])
                return;
            const auto& g = session.groups[targets[i]];
            try {
                Normalizer norm(session);
                FieldSet fs = splitFields(norm.normalize(session.records[g.end - 1]), session.defs);
                refs[i] = collectRefs(fs);
                computed[i] = 1;
            } catch (const std::exception& e) {
                unreadable[i] = e.what();
            }
            size_t n = ++done;
            if ((n & 0x3FF) == 0 || n == targets.size())
                progress.progress(n, targets.size(), "record references");
        },
        progress, options.threads);
    if (cache) {
        cache->begin();
        for (size_t i = 0; i < targets.size(); ++i)
            if (computed[i])
                cache->put("edges", keys[i], serializeEdges(session, *refs[i]));
        cache->commit();
    }

    std::vector<std::vector<RecordEdge>> edges(targets.size());
    std::vector<std::vector<DependencyIssue>> issues(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& w = session.records[session.groups[targets[i]].end - 1];
        if (!refs[i]) {
            issues[i].push_back({Severity::Warning, "Unreadable Record", w.plugin, w.key,
                                 session.formLabel(w.key) + ": " + unreadable[i]});
            continue;
        }
        for (const auto& [to, via] : *refs[i]) {
            if (to == w.key)
                continue;
            edges[i].push_back({w.key, to, via});
            if (!to.valid()) {
                issues[i].push_back({Severity::Error, "Missing Reference", w.plugin, w.key,
                                     session.formLabel(w.key) + " references a FormID whose master is missing"});
            } else if (!session.exists(to)) {
                issues[i].push_back({Severity::Warning, "Missing Reference", w.plugin, w.key,
                                     session.formLabel(w.key) + " (" + via.str() + ") references " +
                                         formatFormId(to.objectId) + " in " + session.pluginName(to.origin) +
                                         ", which does not exist"});
            }
        }
    }

    session.recordEdges.clear();
    for (auto& v : edges) {
        session.recordEdges.insert(session.recordEdges.end(), v.begin(), v.end());
        std::vector<RecordEdge>().swap(v);
    }
    for (auto& v : issues)
        for (auto& is : v) {
            if (is.category == "Missing Reference")
                report.danglingReferences++;
            report.issues.push_back(std::move(is));
        }
    report.recordEdges = session.recordEdges.size();

    evaluateEligibility(session, options, &report);
    session.analysed = true;
    spdlog::info("Dependencies: {} master links, {} record references, {} dangling; {} plugins eligible for compilation",
                 report.pluginEdges, report.recordEdges, report.danglingReferences, report.eligiblePlugins);
    return report;
}

} // namespace fo4mx
