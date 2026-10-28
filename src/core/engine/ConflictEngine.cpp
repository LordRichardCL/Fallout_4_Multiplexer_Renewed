#include "engine/ConflictEngine.hpp"

#include "engine/BuildCache.hpp"
#include "engine/ParallelFor.hpp"

#include <algorithm>
#include <atomic>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* conflictTypeName(ConflictType t)
{
    switch (t) {
    case ConflictType::None: return "None";
    case ConflictType::SimpleOverride: return "Simple Override";
    case ConflictType::CompatibleMerge: return "Compatible Merge";
    case ConflictType::HardConflict: return "Hard Conflict";
    }
    return "None";
}

const char* conflictSeverityName(ConflictSeverity s)
{
    switch (s) {
    case ConflictSeverity::Low: return "Low";
    case ConflictSeverity::Medium: return "Medium";
    case ConflictSeverity::High: return "High";
    case ConflictSeverity::Critical: return "Critical";
    }
    return "Low";
}

const ConflictEntry* ConflictReport::find(FormKey key) const
{
    auto it = std::lower_bound(entries.begin(), entries.end(), key,
                               [](const ConflictEntry& e, FormKey k) { return e.key < k; });
    return it != entries.end() && it->key == key ? &*it : nullptr;
}

ConflictEntry ConflictEngine::analyseGroup(Session& session, uint32_t groupIndex, bool fieldLevel, bool keepDetails)
{
    const FormGroup& g = session.groups[groupIndex];
    auto inst = session.instances(g);
    ConflictEntry e;
    e.group = groupIndex;
    e.key = inst[0].key;
    e.sig = inst[0].signature;
    for (const auto& i : inst)
        e.chain.push_back(i.plugin);
    e.winner = inst.back().plugin;
    for (size_t i = inst.size(); i-- > 0;)
        if (inst[i].editorId) {
            e.editorId = session.strings.get(inst[i].editorId);
            break;
        }

    // Byte-identical overrides need no decoding.
    bool allSame = std::all_of(inst.begin() + 1, inst.end(), [&](const RecordInstance& r) {
        return r.hash == inst[0].hash && r.dataSize == inst[0].dataSize;
    });
    if (allSame) {
        for (size_t i = 1; i < inst.size(); ++i)
            e.itmPlugins.push_back(inst[i].plugin);
        e.type = ConflictType::None;
        e.summary = "Identical to master";
        return e;
    }

    if (!fieldLevel) {
        e.type = inst.size() == 2 ? ConflictType::SimpleOverride : ConflictType::HardConflict;
        e.severity = inst.size() == 2 ? ConflictSeverity::Low : ConflictSeverity::Medium;
        e.summary = "Record-level comparison only";
        return e;
    }

    Normalizer norm(session);
    std::vector<FieldSet> chain;
    chain.reserve(inst.size());
    for (const auto& i : inst)
        chain.push_back(splitFields(norm.normalize(i), session.defs));

    MergeOptions mo;
    mo.parents = session.overrideParents(inst);
    MergeResult merged = MergeEngine::merge(chain, mo);
    const ChainAnalysis& a = merged.analysis;
    e.fieldsAnalysed = true;
    e.winnerLosesChanges = a.winnerLosesChanges;
    for (size_t c : a.itms)
        e.itmPlugins.push_back(inst[c].plugin);

    if (a.changers.empty())
        e.type = ConflictType::None;
    else if (a.hard)
        e.type = ConflictType::HardConflict;
    else if (a.changers.size() == 1 && !a.winnerLosesChanges)
        e.type = ConflictType::SimpleOverride;
    else
        e.type = ConflictType::CompatibleMerge;

    switch (e.type) {
    case ConflictType::None: e.severity = ConflictSeverity::Low; break;
    case ConflictType::SimpleOverride: e.severity = ConflictSeverity::Low; break;
    case ConflictType::CompatibleMerge: e.severity = ConflictSeverity::Medium; break;
    case ConflictType::HardConflict: e.severity = ConflictSeverity::High; break;
    }
    bool baseDeleted = inst.front().flags & RecordFlags::Deleted;
    bool winnerDeleted = inst.back().flags & RecordFlags::Deleted;
    if (winnerDeleted && !baseDeleted && e.type != ConflictType::None)
        e.severity = ConflictSeverity::Critical;

    // Summary.
    size_t hardFields = 0;
    std::string hardNames;
    for (const auto& f : a.fields)
        if (f.hard) {
            if (hardFields < 4)
                hardNames += (hardNames.empty() ? "" : ", ") + f.label;
            ++hardFields;
        }
    e.summary = std::to_string(a.fields.size()) + " field" + (a.fields.size() == 1 ? "" : "s") + " changed by " +
                std::to_string(a.changers.size()) + " plugin" + (a.changers.size() == 1 ? "" : "s");
    if (hardFields)
        e.summary += "; " + std::to_string(hardFields) + " contested (" + hardNames + ")";
    if (a.winnerLosesChanges)
        e.summary += "; last-plugin-wins loses edits";
    if (winnerDeleted && !baseDeleted)
        e.summary += "; winner deletes the record";

    if (keepDetails) {
        // Tokens are rendered against the current load order when the report is finalised.
        auto label = [](FormKey k) { return formToken(k); };
        for (const auto& fa : a.fields) {
            ConflictField cf;
            cf.key = fa.key;
            cf.label = fa.label;
            cf.hard = fa.hard;
            cf.list = fa.listField;
            cf.result = describeField(merged.fields.find(fa.key), label);
            for (size_t c = 0; c < chain.size(); ++c) {
                ConflictCell cell;
                cell.plugin = inst[c].plugin;
                cell.value = describeField(chain[c].find(fa.key), label);
                cell.status = c == 0 && fa.cells[c] == CellStatus::Same ? CellStatus::Base : fa.cells[c];
                cf.cells.push_back(std::move(cell));
            }
            e.fields.push_back(std::move(cf));
        }
    }
    return e;
}

ConflictReport ConflictEngine::analyse(Session& session, const ConflictOptions& options, ProgressSink& progress)
{
    progress.stage("Analysing conflicts");
    ConflictReport report;

    std::vector<uint32_t> overridden;
    for (uint32_t i = 0; i < session.groups.size(); ++i)
        if (session.groups[i].size() > 1)
            overridden.push_back(i);

    std::vector<ConflictEntry> results(overridden.size());
    std::vector<uint8_t> failed(overridden.size(), 0);
    std::vector<uint8_t> reused(overridden.size(), 0);
    std::vector<uint64_t> keys(overridden.size(), 0);

    // Reuse analyses of override chains whose content has not changed.
    BuildCache* cache = session.cache;
    if (cache) {
        const uint64_t optionBits = (options.analyseAllRecordTypes ? 1 : 0) | (options.keepFieldDetails ? 2 : 0);
        for (size_t i = 0; i < overridden.size(); ++i) {
            keys[i] = chainSignature(session, session.groups[overridden[i]]) ^ (optionBits * 0x9E3779B97F4A7C15ULL);
            auto blob = cache->get("conflicts", keys[i]);
            auto entry = blob ? deserializeConflict(session, *blob) : std::nullopt;
            if (!entry) {
                cache->conflicts.misses++;
                continue;
            }
            cache->conflicts.hits++;
            const auto& g = session.groups[overridden[i]];
            auto inst = session.instances(g);
            entry->group = overridden[i];
            entry->key = inst[0].key;
            entry->sig = inst[0].signature;
            entry->winner = inst.back().plugin;
            for (const auto& r : inst)
                entry->chain.push_back(r.plugin);
            for (size_t k = inst.size(); k-- > 0;)
                if (inst[k].editorId) {
                    entry->editorId = session.strings.get(inst[k].editorId);
                    break;
                }
            results[i] = std::move(*entry);
            reused[i] = 1;
        }
    }

    std::atomic<size_t> done{0};
    parallelFor(
        overridden.size(),
        [&](size_t i) {
            if (reused[i])
                return;
            uint32_t gi = overridden[i];
            Signature s = session.records[session.groups[gi].begin].signature;
            bool fieldLevel = options.analyseAllRecordTypes || session.defs.isCompilable(s);
            try {
                results[i] = analyseGroup(session, gi, fieldLevel, options.keepFieldDetails);
            } catch (const std::exception& ex) {
                failed[i] = 1;
                results[i] = analyseGroup(session, gi, false, false);
                results[i].summary = std::string("Analysis failed: ") + ex.what();
                spdlog::debug("Conflict analysis failed for {}: {}", session.formLabel(results[i].key), ex.what());
            }
            size_t n = ++done;
            if ((n & 0x3FF) == 0 || n == overridden.size())
                progress.progress(n, overridden.size(), "override chains");
        },
        progress, options.threads);

    if (cache) {
        cache->begin();
        for (size_t i = 0; i < results.size(); ++i)
            if (!reused[i] && !failed[i])
                cache->put("conflicts", keys[i], serializeConflict(session, results[i]));
        cache->commit();
        spdlog::info("Conflict cache: {} chains reused, {} analysed", cache->conflicts.hits.load(),
                     cache->conflicts.misses.load());
    }
    for (auto& e : results)
        for (auto& f : e.fields) {
            f.result = renderFormTokens(session, f.result);
            for (auto& c : f.cells)
                c.value = renderFormTokens(session, c.value);
        }
    report.reusedGroups = static_cast<size_t>(std::count(reused.begin(), reused.end(), 1));

    for (auto& p : session.plugins) {
        p.conflictCount = 0;
        p.hardConflictCount = 0;
        p.itmCount = 0;
    }
    report.entries.reserve(results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        auto& e = results[i];
        report.analysedGroups++;
        report.failedGroups += failed[i];
        for (auto p : e.itmPlugins)
            session.plugins[p].itmCount++;
        if (e.type == ConflictType::None) {
            report.identicalOverrides++;
            continue;
        }
        report.byType[static_cast<size_t>(e.type)]++;
        for (auto p : e.chain) {
            session.plugins[p].conflictCount++;
            if (e.type == ConflictType::HardConflict)
                session.plugins[p].hardConflictCount++;
        }
        report.entries.push_back(std::move(e));
    }
    std::sort(report.entries.begin(), report.entries.end(),
              [](const ConflictEntry& a, const ConflictEntry& b) { return a.key < b.key; });
    spdlog::info("Conflicts: {} simple overrides, {} compatible merges, {} hard conflicts ({} ITM-only chains)",
                 report.byType[1], report.byType[2], report.byType[3], report.identicalOverrides);
    return report;
}

} // namespace fo4mx
