#include "engine/CompilationEngine.hpp"
#include "engine/PrevisCheck.hpp"

#include "assets/AssetScanner.hpp"
#include "engine/BuildCache.hpp"
#include "engine/Diagnosis.hpp"
#include "engine/MergeEngine.hpp"
#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* allocationStrategyName(AllocationStrategy s)
{
    switch (s) {
    case AllocationStrategy::ByRecordType: return "By Record Type";
    case AllocationStrategy::Balanced: return "Balanced Size";
    case AllocationStrategy::UserDefined: return "User Defined";
    }
    return "Balanced Size";
}

bool CompiledOutput::isCompiled(PluginIndex p) const
{
    return p < compiledFlag_.size() && compiledFlag_[p];
}

bool CompiledOutput::hasArchiveLoader(PluginIndex p) const
{
    return std::find(archiveLoaders.begin(), archiveLoaders.end(), p) != archiveLoaders.end();
}

const OutputPlugin* CompiledOutput::patchOutput() const
{
    for (const auto& o : outputs)
        if (o.patch)
            return &o;
    return nullptr;
}

uint32_t CompiledOutput::translate(const Session& session, FormKey key, int output, bool* ok) const
{
    if (ok)
        *ok = true;
    auto fail = [&]() -> uint32_t {
        if (ok)
            *ok = false;
        return 0;
    };
    if (!key.valid())
        return key.objectId == 0 ? 0 : fail();
    const auto& out = outputs.at(static_cast<size_t>(output));
    if (isCompiled(key.origin)) {
        auto it = indexByKey.find(key);
        if (it == indexByKey.end())
            return fail();
        const auto& rec = records[it->second];
        if (rec.output == output)
            return makeRawFormId(static_cast<uint32_t>(out.masterNames.size()), rec.objectId);
        auto pos = std::find(out.outputMasters.begin(), out.outputMasters.end(), rec.output);
        if (pos == out.outputMasters.end())
            return fail();
        uint32_t mi = static_cast<uint32_t>(out.retainedMasters.size() + (pos - out.outputMasters.begin()));
        return makeRawFormId(mi, rec.objectId);
    }
    auto pos = std::find(out.retainedMasters.begin(), out.retainedMasters.end(), key.origin);
    if (pos == out.retainedMasters.end())
        return fail();
    (void)session;
    return makeRawFormId(static_cast<uint32_t>(pos - out.retainedMasters.begin()), key.objectId);
}

std::string CompilationEngine::patchFileName(const CompileOptions& options, int index)
{
    if (index == 0)
        return options.patchName;
    auto dot = options.patchName.rfind('.');
    std::string stem = dot == std::string::npos ? options.patchName : options.patchName.substr(0, dot);
    std::string ext = dot == std::string::npos ? std::string(".esp") : options.patchName.substr(dot);
    return stem + "_" + std::to_string(index + 1) + ext;
}

std::string CompilationEngine::outputFileName(const CompileOptions& options, int index)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d", index + 1);
    return options.outputPrefix + buf + ".esp";
}

std::vector<FormMapping> CompilationEngine::readFormMap(const std::filesystem::path& path)
{
    std::vector<FormMapping> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto cols = split(line, '\t', false);
        if (cols.size() < 4)
            continue;
        FormMapping m;
        m.sourcePlugin = cols[0];
        m.sourceObjectId = static_cast<uint32_t>(std::strtoul(cols[1].c_str(), nullptr, 16));
        m.outputPlugin = cols[2];
        m.objectId = static_cast<uint32_t>(std::strtoul(cols[3].c_str(), nullptr, 16));
        if (cols.size() > 4)
            m.sig = Signature::fromString(cols[4]);
        if (cols.size() > 5)
            m.editorId = cols[5];
        out.push_back(std::move(m));
    }
    return out;
}

namespace {

// A cell's previs (XPRI) and combined (XCRI) reference lists are sorted by FormID (xEdit keeps them
// sorted, as the Creation Kit writes them). Rewriting their FormIDs into an output's master numbering
// can change the order, so they are sorted again once the output's FormIDs are written.
void sortCellReferenceLists(Subrecord& sr)
{
    auto& b = sr.data;
    if (sr.signature == Signature("XPRI")) {
        const size_t n = b.size() / 4;
        std::vector<uint32_t> ids(n);
        for (size_t i = 0; i < n; ++i)
            ids[i] = loadLE<uint32_t>(b.data() + 4 * i);
        std::sort(ids.begin(), ids.end());
        for (size_t i = 0; i < n; ++i)
            storeLE<uint32_t>(b.data() + 4 * i, ids[i]);
    } else if (sr.signature == Signature("XCRI") && b.size() >= 8) {
        // Meshes count, references count, mesh IDs (sorted, unchanged), then (reference, mesh) pairs.
        const size_t start = 8 + size_t(4) * loadLE<uint32_t>(b.data());
        if (start > b.size())
            return;
        const size_t n = (b.size() - start) / 8;
        std::vector<std::pair<uint32_t, uint32_t>> refs(n);
        for (size_t i = 0; i < n; ++i)
            refs[i] = {loadLE<uint32_t>(b.data() + start + 8 * i), loadLE<uint32_t>(b.data() + start + 8 * i + 4)};
        std::sort(refs.begin(), refs.end());
        for (size_t i = 0; i < n; ++i) {
            storeLE<uint32_t>(b.data() + start + 8 * i, refs[i].first);
            storeLE<uint32_t>(b.data() + start + 8 * i + 4, refs[i].second);
        }
    }
}

bool sameRecord(const FieldSet& a, const FieldSet& b)
{
    if (a.fields.size() != b.fields.size())
        return false;
    for (size_t i = 0; i < a.fields.size(); ++i)
        if (a.fields[i].key != b.fields[i].key || !fieldEquivalent(&a.fields[i], &b.fields[i]))
            return false;
    return true;
}

uint64_t estimateSize(const FieldSet& fs)
{
    uint64_t n = RecordHeader::kSize;
    for (const auto& s : joinFields(fs))
        n += 6 + s.bytes.size() + (s.bytes.size() > 0xFFFF ? 10 : 0);
    return n;
}

// Dependency-first order of strongly connected components (Tarjan, iterative).
std::vector<std::vector<uint32_t>> stronglyConnected(const std::vector<EmittedRecord>& recs)
{
    const uint32_t n = static_cast<uint32_t>(recs.size());
    std::vector<int32_t> index(n, -1), low(n, 0);
    std::vector<bool> onStack(n, false);
    std::vector<uint32_t> stack;
    std::vector<std::vector<uint32_t>> sccs;
    int32_t counter = 0;

    struct Frame {
        uint32_t v;
        size_t edge;
    };
    for (uint32_t start = 0; start < n; ++start) {
        if (index[start] != -1)
            continue;
        std::vector<Frame> call{{start, 0}};
        index[start] = low[start] = counter++;
        stack.push_back(start);
        onStack[start] = true;
        while (!call.empty()) {
            Frame& f = call.back();
            const auto& deps = recs[f.v].deps;
            if (f.edge < deps.size()) {
                uint32_t w = deps[f.edge++];
                if (index[w] == -1) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    onStack[w] = true;
                    call.push_back({w, 0});
                } else if (onStack[w]) {
                    low[f.v] = std::min(low[f.v], index[w]);
                }
                continue;
            }
            uint32_t v = f.v;
            if (low[v] == index[v]) {
                std::vector<uint32_t> comp;
                uint32_t w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = false;
                    comp.push_back(w);
                } while (w != v);
                sccs.push_back(std::move(comp));
            }
            call.pop_back();
            if (!call.empty())
                low[call.back().v] = std::min(low[call.back().v], low[v]);
        }
    }
    return sccs; // Tarjan emits a component only after everything it depends on
}

// Allocation group of a record type (catalogue "Allocation Order"); unranked types share the last group.
int typeRank(const RecordDefinitions& defs, Signature s)
{
    auto e = defs.catalogueEntry(s);
    return e && e->allocationOrder >= 0 ? e->allocationOrder : defs.catalogue().allocationGroups() - 1;
}

// Where a record's chosen version sits: cells, worldspaces and placed references keep it.
void placeFromImpl(const Session& session, EmittedRecord& e, const RecordInstance& r)
{
    const auto* entry = session.defs.catalogueEntry(r.signature);
    e.world = entry && entry->placement != RecordPlacement::Top;
    e.placement = static_cast<Placement::Kind>(r.placement);
    e.persistent = r.persistent;
    e.parent = e.placement == Placement::Kind::Top || r.parentRaw == 0 || e.placement == Placement::Kind::InteriorCell
                   ? FormKey{}
                   : session.resolve(session.plugins[r.plugin], r.parentRaw);
}

} // namespace

// The navmesh info map (NAVI): the game combines every plugin's version as it loads, one entry per
// navmesh, so each plugin's own entries count, not only the last plugin's. The output carries the
// entries of every compiled plugin (a later plugin's entry for the same navmesh replacing an earlier
// one) and their deleted navmeshes; the version and the precomputed pathing are the last compiled
// plugin's. Retained plugins keep their own NAVI, which the game still combines.
static EmittedRecord combineNavmeshInfos(Session& session, const CompiledOutput& out, std::span<const RecordInstance> inst,
                                  bool isNew)
{
    const Signature nvmi("NVMI"), nvsi("NVSI");
    Normalizer norm(session);
    std::optional<NormRecord> base;
    const RecordInstance* last = nullptr;
    std::vector<NormSub> infos;
    std::map<FormKey, size_t> infoAt;
    std::vector<FormKey> deleted;
    std::set<FormKey> deletedSeen;
    size_t missing = 0;
    EmittedRecord e;
    for (const auto& r : inst) {
        e.chain.push_back(r.plugin);
        if (!out.isCompiled(r.plugin))
            continue;
        auto nr = norm.normalize(r);
        e.issues.insert(e.issues.end(), nr.issues.begin(), nr.issues.end());
        e.contributors.push_back(r.plugin);
        for (auto& sub : nr.subs) {
            if (sub.sig == nvmi) {
                auto ref = std::find_if(sub.refs.begin(), sub.refs.end(), [](const auto& p) { return p.first == 0; });
                if (sub.heuristic || ref == sub.refs.end())
                    throw FormatError("a navmesh info in " + session.pluginName(r.plugin) +
                                      " could not be decoded, so the navmesh infos cannot be combined");
                // An entry for a navmesh that exists nowhere describes nothing the game can find; it is
                // left out rather than written for "none".
                if (!session.exists(ref->second)) {
                    ++missing;
                    continue;
                }
                if (auto it = infoAt.find(ref->second); it != infoAt.end()) {
                    infos[it->second] = std::move(sub);
                } else {
                    infoAt.emplace(ref->second, infos.size());
                    infos.push_back(std::move(sub));
                }
            } else if (sub.sig == nvsi) {
                if (sub.heuristic)
                    throw FormatError("the deleted navmeshes of " + session.pluginName(r.plugin) + " could not be decoded");
                for (const auto& [off, k] : sub.refs)
                    if (deletedSeen.insert(k).second)
                        deleted.push_back(k);
            }
        }
        base = std::move(nr);
        last = &r;
    }
    if (!base)
        throw FormatError("no compiled plugin has a navmesh info map");
    if (missing)
        e.issues.push_back({Severity::Info, Signature("NVMI"),
                            std::to_string(missing) + " navmesh infos for navmeshes that exist in no plugin are left out"});

    // EDID and NVER, the navmesh infos, then the rest (NVPP), then the deleted navmeshes.
    std::vector<NormSub> subs, rest;
    for (auto& sub : base->subs) {
        if (sub.sig == nvmi || sub.sig == nvsi)
            continue;
        if (sub.sig == Signature("EDID") || sub.sig == Signature("NVER"))
            subs.push_back(std::move(sub));
        else
            rest.push_back(std::move(sub));
    }
    for (auto& info : infos)
        subs.push_back(std::move(info));
    for (auto& sub : rest)
        subs.push_back(std::move(sub));
    if (!deleted.empty()) {
        NormSub d;
        d.sig = nvsi;
        d.bytes.assign(deleted.size() * 4, 0);
        for (size_t k = 0; k < deleted.size(); ++k)
            d.refs.emplace_back(static_cast<uint32_t>(k * 4), deleted[k]);
        subs.push_back(std::move(d));
    }
    base->subs = std::move(subs);

    e.key = inst[0].key;
    e.sig = inst[0].signature;
    if (last->editorId)
        e.editorId = session.strings.get(last->editorId);
    e.isNew = isNew;
    e.merged = e.contributors.size() > 1;
    e.winner = last->plugin;
    e.fields = splitFields(*base, session.defs);
    e.size = estimateSize(e.fields);
    placeFromImpl(session, e, *last);
    return e;
}

CompiledOutput CompilationEngine::compile(Session& session, const CompileOptions& opts, ProgressSink& progress)
{
    CompiledOutput out;
    out.options = opts;
    CompileOptions& options = out.options;
    options.outputCount = std::clamp(options.outputCount, CompileOptions::kMinOutputs, CompileOptions::kMaxOutputs);

    out.compiledFlag_.assign(session.plugins.size(), 0);
    if (options.patchOnly)
        options.buildPatch = true;
    for (auto pi : session.loadOrder) {
        if (!options.patchOnly && session.plugins[pi].eligibility == Eligibility::Eligible) {
            out.compiledFlag_[pi] = 1;
            out.compiledPlugins.push_back(pi);
        } else {
            out.retainedPlugins.push_back(pi);
        }
    }
    auto addIssue = [&](Severity s, std::string cat, std::string msg, FormKey k = {}, std::string plugin = {}) {
        out.issues.push_back({s, std::move(cat), std::move(msg), k, std::move(plugin)});
    };
    auto placeFrom = [&](EmittedRecord& e, const RecordInstance& r) { placeFromImpl(session, e, r); };
    if (out.compiledPlugins.empty() && !options.buildPatch) {
        addIssue(Severity::Warning, "Compilation", "No plugins are eligible for compilation");
        return out;
    }

    // Overrides of another record type (left out by the scan, as the game ignores them).
    for (const auto& m : session.typeMismatches) {
        if (m.plugin >= out.compiledFlag_.size() || !out.compiledFlag_[m.plugin])
            continue;
        const std::string edid = m.editorId ? session.strings.get(m.editorId) : std::string();
        addIssue(Severity::Warning, "Broken in Source",
                 (edid.empty() ? std::string() : edid + " ") + "[" + m.signature.str() + ":" +
                     session.loadOrderFormId(m.key) + "] in " + session.pluginName(m.plugin) + " overrides a " +
                     m.original.str() + " of " + session.pluginName(m.definer) +
                     " with a record of another type; the game ignores it and keeps the " + m.original.str() +
                     ", so it is not written",
                 m.key, session.pluginName(m.plugin));
        out.issues.back().cause =
            session.pluginName(m.plugin) + " was made for another version of " + session.pluginName(m.definer) +
            ", in which this FormID was a " + m.signature.str() + "; it has since been renumbered (for example by ESL "
            "compaction). Use a version of " + session.pluginName(m.plugin) + " made for your version of " +
            session.pluginName(m.definer) + ".";
    }

    // ---- Resolve: which records need to be materialised -------------------
    progress.stage("Resolve");
    struct Candidate {
        uint32_t group;
        bool isNew;
        bool retainedWinner;
        int forced;
        bool patch;
        bool combine = false; // the navmesh info map: the compiled plugins' versions combined
    };
    std::vector<Candidate> candidates;
    for (uint32_t gi = 0; gi < session.groups.size(); ++gi) {
        auto inst = session.instances(session.groups[gi]);
        size_t inCompiled = 0;
        for (const auto& r : inst)
            inCompiled += out.compiledFlag_[r.plugin];
        FormKey key = inst[0].key;
        int forced = -1;
        if (auto it = options.forcedWinners.find(key.packed()); it != options.forcedWinners.end())
            for (size_t i = 0; i < inst.size(); ++i)
                if (inst[i].plugin == it->second)
                    forced = static_cast<int>(i);

        if (!inCompiled) {
            // Only retained plugins: last-plugin-wins already applies unless the patch
            // merges the chain. Only types whose fields merge are patched (a winner-only type would just
            // repeat the winner), unless the user chose another plugin's version.
            const auto* entry = session.defs.catalogueEntry(inst[0].signature);
            if (options.buildPatch && inst.size() > 1 && session.defs.isCompilable(inst[0].signature) &&
                entry && entry->placement == RecordPlacement::Top && // the patch holds no cells
                (session.defs.mergesFields(inst[0].signature) || forced >= 0) &&
                (options.mergeCompatible || forced >= 0) && !options.skipPatchRecords.count(key.packed()))
                candidates.push_back({gi, false, true, forced, true});
            continue;
        }
        out.sourceRecords += inCompiled;
        const bool winnerOnly = !session.defs.mergesFields(inst[0].signature);

        bool originCompiled = key.valid() && out.compiledFlag_[key.origin];
        bool lastCompiled = out.compiledFlag_[inst.back().plugin];
        if (inst[0].signature == Signature("NAVI") && !originCompiled) {
            candidates.push_back({gi, false, !lastCompiled, -1, false, true});
            continue;
        }

        if (originCompiled) {
            for (const auto& r : inst)
                if (!out.compiledFlag_[r.plugin])
                    addIssue(Severity::Error, "Dependency",
                             session.pluginName(r.plugin) + " overrides " + session.formLabel(key) +
                                 " from a compiled plugin but stays in the load order",
                             key, session.pluginName(r.plugin));
        } else if (!lastCompiled && (!options.mergeCompatible || winnerOnly) && forced < 0) {
            out.retainedWins++;
            continue;
        }
        // Winner-only types: the last plugin's version, taken whole (unless the user chose another).
        if (winnerOnly && forced < 0)
            forced = static_cast<int>(inst.size()) - 1;
        candidates.push_back({gi, originCompiled, !lastCompiled, forced, false});
    }

    // ---- Merge ---------------------------------------------------------------
    progress.stage("Merge");
    std::vector<std::optional<EmittedRecord>> emitted(candidates.size());
    std::vector<std::vector<CompileIssue>> mergeIssues(candidates.size());

    // Merging an override chain depends only on its content and the merge options,
    // so unchanged chains reuse the previous result (incremental recompilation).
    std::vector<std::optional<CachedMerge>> merges(candidates.size());
    std::vector<uint64_t> keys(candidates.size(), 0);
    BuildCache* cache = session.cache;
    if (cache) {
        for (size_t ci = 0; ci < candidates.size(); ++ci) {
            const auto& c = candidates[ci];
            if (c.combine)
                continue; // built from the compiled plugins' versions every time
            uint64_t opts = (options.mergeCompatible ? 1ull : 0ull) | (static_cast<uint64_t>(c.forced + 1) << 8);
            keys[ci] = chainSignature(session, session.groups[c.group]) ^ ((opts + 1) * 0xC2B2AE3D27D4EB4FULL);
            if (auto blob = cache->get("merges", keys[ci]))
                merges[ci] = deserializeMerge(session, *blob);
            (merges[ci] ? cache->merges.hits : cache->merges.misses)++;
        }
    }
    std::vector<uint8_t> computed(candidates.size(), 0);

    std::atomic<size_t> done{0};
    parallelFor(
        candidates.size(),
        [&](size_t ci) {
            const auto& c = candidates[ci];
            auto inst = session.instances(session.groups[c.group]);
            try {
                if (c.combine) {
                    emitted[ci] = combineNavmeshInfos(session, out, inst, c.isNew);
                    size_t n = ++done;
                    if ((n & 0x3FF) == 0 || n == candidates.size())
                        progress.progress(n, candidates.size(), "merging records");
                    return;
                }
                if (!merges[ci]) {
                    Normalizer norm(session);
                    std::vector<FieldSet> chain;
                    CachedMerge m;
                    for (const auto& r : inst) {
                        auto nr = norm.normalize(r);
                        m.issues.push_back(std::move(nr.issues));
                        chain.push_back(splitFields(nr, session.defs));
                    }
                    MergeOptions mo;
                    mo.mergeCompatible = options.mergeCompatible;
                    mo.forcedWinner = c.forced;
                    mo.parents = session.overrideParents(inst);
                    MergeResult mr = MergeEngine::merge(chain, mo);
                    for (const auto& link : chain)
                        m.equalsInstance.push_back(sameRecord(mr.fields, link) ? 1 : 0);
                    for (size_t idx : mr.contributors)
                        m.contributors.push_back(static_cast<uint32_t>(idx));
                    m.merged = mr.merged;
                    m.fields = std::move(mr.fields);
                    merges[ci] = std::move(m);
                    computed[ci] = 1;
                }
                const CachedMerge& m = *merges[ci];

                // Once compiled plugins are disabled, the last retained instance is what the game
                // loads. If the result equals it, an override would only be an ITM.
                for (size_t i = inst.size(); i-- > 0;) {
                    if (out.compiledFlag_[inst[i].plugin])
                        continue;
                    if (m.equalsInstance[i]) {
                        mergeIssues[ci].push_back({Severity::Info, "Identical", "", inst[0].key, {}});
                        return;
                    }
                    break;
                }
                if (c.isNew && (m.fields.flags() & RecordFlags::Deleted)) {
                    mergeIssues[ci].push_back({Severity::Info, "Deleted",
                                               session.formLabel(inst[0].key) + " is a deleted new record and is dropped",
                                               inst[0].key, session.pluginName(inst.back().plugin)});
                    return;
                }

                EmittedRecord e;
                e.key = inst[0].key;
                e.sig = inst[0].signature;
                for (size_t i = inst.size(); i-- > 0;)
                    if (inst[i].editorId) {
                        e.editorId = session.strings.get(inst[i].editorId);
                        break;
                    }
                e.isNew = c.isNew;
                e.patch = c.patch;
                e.merged = m.merged;
                e.winner = inst.back().plugin;
                for (const auto& r : inst)
                    e.chain.push_back(r.plugin);
                for (uint32_t idx : m.contributors)
                    e.contributors.push_back(inst[idx].plugin);
                for (size_t i = 0; i < inst.size() && i < m.issues.size(); ++i)
                    if (out.compiledFlag_[inst[i].plugin])
                        e.issues.insert(e.issues.end(), m.issues[i].begin(), m.issues[i].end());
                e.fields = m.fields;
                e.size = estimateSize(e.fields);
                placeFrom(e, inst[c.forced >= 0 ? static_cast<size_t>(c.forced) : inst.size() - 1]);
                emitted[ci] = std::move(e);
            } catch (const std::exception& ex) {
                mergeIssues[ci].push_back({Severity::Error, "Unreadable Record",
                                           session.formLabel(inst[0].key) + ": " + ex.what(), inst[0].key,
                                           session.pluginName(inst.back().plugin)});
            }
            size_t n = ++done;
            if ((n & 0x3FF) == 0 || n == candidates.size())
                progress.progress(n, candidates.size(), "merging records");
        },
        progress, options.threads);

    if (cache) {
        cache->begin();
        for (size_t ci = 0; ci < candidates.size(); ++ci)
            if (computed[ci])
                cache->put("merges", keys[ci], serializeMerge(session, *merges[ci]));
        cache->commit();
    }
    for (size_t ci = 0; ci < candidates.size(); ++ci)
        (computed[ci] ? out.mergesComputed : out.mergesReused)++;
    merges.clear();
    spdlog::info("Merge: {} chains reused from the build cache, {} recomputed", out.mergesReused, out.mergesComputed);

    for (size_t ci = 0; ci < candidates.size(); ++ci) {
        for (auto& is : mergeIssues[ci]) {
            if (is.category == "Identical") {
                out.identicalToRetained++;
                continue;
            }
            if (is.category == "Deleted")
                out.droppedDeleted++;
            out.issues.push_back(std::move(is));
        }
        if (emitted[ci]) {
            out.indexByKey[emitted[ci]->key] = static_cast<uint32_t>(out.records.size());
            if (emitted[ci]->merged)
                out.mergedRecords++;
            out.records.push_back(std::move(*emitted[ci]));
        }
    }
    emitted.clear();

    // ---- Containers ---------------------------------------------------------------
    // A compiled placed reference is written inside its cell, and an exterior cell inside its
    // worldspace. Where no compiled plugin provides that cell or worldspace, it is copied as the game
    // loads it now (the last plugin's version, whoever that is), so the output changes nothing about
    // it. Cells and worldspaces are only ever overrides here: the plugin that defines them stays.
    {
        Normalizer norm(session);
        std::vector<std::pair<FormKey, FormKey>> todo; // (needed, needed by)
        for (const auto& e : out.records)
            if (e.parent.valid())
                todo.emplace_back(e.parent, e.key);
        while (!todo.empty()) {
            auto [k, by] = todo.back();
            todo.pop_back();
            if (out.indexByKey.count(k))
                continue;
            const auto* g = session.findGroup(k);
            if (!g || out.compiledFlag_[k.origin]) {
                addIssue(Severity::Error, "World Layout",
                         session.formLabel(by) + " cannot be placed: its " + (g ? "cell or worldspace " : "parent ") +
                             session.formLabel(k) + (g ? " is defined by a compiled plugin" : " is not in the load order"),
                         by, session.pluginName(out.records[out.indexByKey.at(by)].winner));
                continue;
            }
            auto inst = session.instances(*g);
            EmittedRecord e;
            e.key = k;
            e.sig = inst.back().signature;
            for (size_t i = inst.size(); i-- > 0;)
                if (inst[i].editorId) {
                    e.editorId = session.strings.get(inst[i].editorId);
                    break;
                }
            e.container = true;
            e.winner = inst.back().plugin;
            for (const auto& r : inst)
                e.chain.push_back(r.plugin);
            e.contributors.push_back(inst.back().plugin);
            try {
                e.fields = splitFields(norm.normalize(inst.back()), session.defs);
            } catch (const std::exception& ex) {
                addIssue(Severity::Error, "Unreadable Record", session.formLabel(k) + ": " + ex.what(), by,
                         session.pluginName(e.winner));
                continue;
            }
            e.size = estimateSize(e.fields);
            placeFrom(e, inst.back());
            if (e.parent.valid())
                todo.emplace_back(e.parent, k);
            out.indexByKey[k] = static_cast<uint32_t>(out.records.size());
            out.records.push_back(std::move(e));
            out.containers++;
        }
    }

    // A quest holding compiled dialogue (topics, scenes, branches) is written with it, in one output.
    {
        std::set<FormKey> quests;
        for (const auto& e : out.records)
            if (e.parent.valid() && (e.placement == Placement::Kind::QuestChild))
                quests.insert(e.parent);
        for (const auto& k : quests)
            if (auto it = out.indexByKey.find(k); it != out.indexByKey.end())
                out.records[it->second].world = true;
    }

    // ---- Records stored outside their parent ------------------------------------
    // A placed reference (or another record that belongs in a cell or a dialogue topic) stored in a
    // top-level group by its plugin (a malformed plugin) cannot be written where it belongs: its
    // plugin stays in the load order, as the game loads it today.
    for (const auto& e : out.records) {
        if (e.placement != Placement::Kind::Top)
            continue;
        const auto* c = RecordCatalogue::get().find(e.sig);
        if (!c || (c->placement != RecordPlacement::CellChild && c->placement != RecordPlacement::TopicChild))
            continue;
        addIssue(Severity::Error, "World Layout",
                 session.formLabel(e.key) + " is stored outside any " +
                     (c->placement == RecordPlacement::CellChild ? "cell" : "dialogue topic") + " in " +
                     session.pluginName(e.winner) +
                     " (a top-level group): it cannot be written where it belongs, so its plugin stays",
                 e.key, session.pluginName(e.winner));
    }

    // ---- Precombines and previs --------------------------------------------------
    std::set<FormKey> previsCells; // cells whose precombine or previs data keeps a plugin out
    // Previs (XPRI): a new reference listed there is in a previs file (.uvd), which holds its FormID in
    // the numbering of the plugin whose cell record wins. A cell's own previs file is translated into
    // the output's numbering when the cell is written (P2b, "Previs Numbering" below), or keeps the
    // plugins out when it cannot be. A cell in another cell's previs file (RVIS, an exterior cluster)
    // keeps the reference's plugin out: that file is read in the other cell's numbering.
    // Precombine (XCRI) only (P2a): the precombined meshes hold no FormIDs and are found by the cell's
    // own plugin and FormID, which compiling an override of the cell does not change; the previs file
    // does not list combined references (checked on 946 cells of a real load order: none was in a
    // .uvd without also being in XPRI). XCRI is rewritten with the new FormID (and sorted again), so
    // the reference stays combined into the same mesh. A new cell keeps its plugin out (below).
    for (const auto& e : out.records) {
        if (e.sig != sig::CELL)
            continue;
        std::set<FormKey> xpri;
        for (const auto& f : e.fields.fields)
            for (const auto& sub : f.subs)
                if (sub.sig == Signature("XPRI"))
                    for (const auto& [off, k] : sub.refs)
                        xpri.insert(k);
        size_t combinedOnly = 0;
        for (const auto& f : e.fields.fields)
            for (const auto& sub : f.subs) {
                if (sub.sig != Signature("XCRI") && sub.sig != Signature("XPRI"))
                    continue;
                for (const auto& [off, k] : sub.refs) {
                    if (!k.valid() || !out.compiledFlag_[k.origin])
                        continue;
                    auto it = out.indexByKey.find(k);
                    if (it == out.indexByKey.end() || !out.records[it->second].isNew)
                        continue;
                    if (sub.sig == Signature("XCRI") && !xpri.count(k)) {
                        ++combinedOnly;
                        continue;
                    }
                    if (sub.sig == Signature("XPRI") && !e.isNew && ownsPrevisFile(e))
                        continue; // its previs file is translated with the cell (checked when written)
                    previsCells.insert(e.key);
                    addIssue(Severity::Error, "Precombined Reference",
                             session.formLabel(k) + " is a new reference in the previs data of " +
                                 session.formLabel(e.key) +
                                 ", which is in the previs file of another cell (RVIS): that file holds its FormID in "
                                 "that cell's numbering, which compiling would change, so its plugin stays",
                             k, session.pluginName(k.origin));
                }
            }
        if (combinedOnly && !e.isNew)
            addIssue(Severity::Info, "Precombined Reference",
                     std::to_string(combinedOnly) + " new reference" + (combinedOnly == 1 ? "" : "s") +
                         " in the precombine data (XCRI) of " + session.formLabel(e.key) +
                         " compile with new FormIDs: the precombined meshes hold no FormIDs and are found by the "
                         "cell, which keeps its FormID; XCRI is rewritten so each stays combined into its mesh",
                     e.key, session.pluginName(e.winner));
    }

    // New cells (W4). Precombined meshes and previs files are found by the cell's defining plugin and
    // its FormID (Precombined\<plugin>\<cell>_<mesh>_OC.nif), both of which compiling changes: a new
    // cell with precombine or previs data, or whose previs file is such a cell (RVIS), keeps its plugin
    // out. Two new cells at the same grid position of one worldspace would collide in one plugin.
    {
        auto subOf = [](const EmittedRecord& e, Signature sig) -> const NormSub* {
            for (const auto& f : e.fields.fields)
                for (const auto& sub : f.subs)
                    if (sub.sig == sig)
                        return &sub;
            return nullptr;
        };
        auto isNewCompiledCell = [&](FormKey k) {
            auto it = out.indexByKey.find(k);
            return it != out.indexByKey.end() && out.records[it->second].isNew &&
                   out.records[it->second].sig == sig::CELL;
        };
        std::map<std::tuple<uint64_t, int32_t, int32_t>, FormKey> grids; // (worldspace, x, y) -> cell
        for (const auto& e : out.records) {
            if (e.sig != sig::CELL)
                continue;
            if (e.isNew)
                for (const char* data : {"PCMB", "VISI", "XCRI", "XPRI"})
                    if (subOf(e, Signature::fromString(data))) {
                        previsCells.insert(e.key);
                        addIssue(Severity::Error, "Precombined Cell",
                                 session.formLabel(e.key) + " is a new cell with precombine or previs data (" + data +
                                     "): those files are found by its plugin's name and its FormID, which compiling "
                                     "would change, so its plugin stays",
                                 e.key, session.pluginName(e.key.origin));
                        break;
                    }
            if (const auto* rvis = subOf(e, Signature("RVIS")))
                for (const auto& [off, k] : rvis->refs)
                    if (k.valid() && isNewCompiledCell(k)) {
                        // Always reported (each RVIS names the plugin to keep out); the cell is
                        // checked once.
                        previsCells.insert(e.key);
                        addIssue(Severity::Error, "Precombined Cell",
                                 session.formLabel(e.key) + " is in the previs file of " + session.formLabel(k) +
                                     ", a new cell: that file is found by its plugin's name and FormID, so its plugin "
                                     "stays",
                                 k, session.pluginName(k.origin));
                    }
            const auto* xclc = subOf(e, Signature("XCLC"));
            if (e.placement != Placement::Kind::WorldCell || !e.parent.valid() || !xclc || xclc->bytes.size() < 8 ||
                (e.fields.flags() & RecordFlags::Persistent))
                continue;
            const auto key = std::make_tuple(e.parent.packed(), loadLE<int32_t>(xclc->bytes.data()),
                                             loadLE<int32_t>(xclc->bytes.data() + 4));
            auto [it, added] = grids.emplace(key, e.key);
            if (!added) {
                const FormKey clash = e.isNew ? e.key : it->second;
                addIssue(Severity::Error, "World Layout",
                         session.formLabel(e.key) + " and " + session.formLabel(it->second) +
                             " are both at grid " + std::to_string(std::get<1>(key)) + "," +
                             std::to_string(std::get<2>(key)) + " of " + session.formLabel(e.parent) +
                             ": a new cell there would collide in one plugin, so its plugin stays",
                         clash, session.pluginName(clash.origin));
            }
        }
    }

    // Evidence for the cells above: what their previs files and precombined meshes hold (read only).
    for (const auto& k : previsCells) {
        if (session.previsChecks.count(k))
            continue; // checked in an earlier round, while the plugin was still compiled
        auto it = out.indexByKey.find(k);
        if (it == out.indexByKey.end())
            continue;
        try {
            session.previsChecks.emplace(k, checkPrevis(session, out.records[it->second]));
        } catch (const std::exception& ex) {
            session.previsChecks.emplace(k, "Previs check of " + session.formLabel(k) + " failed: " + ex.what());
        }
    }

    // ---- Rewrite (dependency extraction) ------------------------------------------
    progress.stage("Rewrite");
    // References that are already broken in the source (a master index past the master list, or
    // a record that exists nowhere in the load order) read as "none" in game. Write them as none
    // instead of refusing the record: the compiled plugin then behaves exactly like the source.
    for (auto& e : out.records) {
        auto nullBroken = [&](NormSub& s) {
            for (auto& [off, k] : s.refs) {
                bool badMaster = !k.valid() && k.objectId != 0;
                bool missing = k.valid() && out.compiledFlag_[k.origin] && !out.indexByKey.count(k) && !session.exists(k);
                if (!badMaster && !missing)
                    continue;
                addIssue(Severity::Warning, "Broken in Source",
                         session.formLabel(e.key) + " (" + s.sig.str() + ") refers to " +
                             (badMaster ? "FormID " + formatFormId(k.objectId) + " with a master index past its master list"
                                        : formatFormId(k.objectId) + " in " + session.pluginName(k.origin)) +
                             ", which does not exist; the game reads it as none, so it is written as none",
                         e.key, session.pluginName(e.winner));
                out.issues.back().cause = diagnosis::missingReference(session, e.winner, badMaster ? FormKey{} : k,
                                                                      badMaster ? k.objectId : 0);
                k = FormKey{kInvalidPlugin, 0};
            }
        };
        for (auto& f : e.fields.fields) {
            for (auto& sub : f.subs)
                nullBroken(sub);
            for (auto& item : f.items)
                for (auto& sub : item.subs)
                    nullBroken(sub);
        }
    }
    for (uint32_t i = 0; i < out.records.size(); ++i) {
        auto& e = out.records[i];
        std::set<uint32_t> deps;
        for (const auto& [k, via] : collectRefs(e.fields)) {
            if (!k.valid()) {
                addIssue(Severity::Error, "Broken FormID",
                         session.formLabel(e.key) + " (" + via.str() + ") contains FormID " + formatFormId(k.objectId) +
                             " whose master is missing",
                         e.key, session.pluginName(e.winner));
                continue;
            }
            if (!out.compiledFlag_[k.origin] || k == e.key)
                continue;
            auto it = out.indexByKey.find(k);
            if (it == out.indexByKey.end()) {
                addIssue(Severity::Error, "Missing Reference",
                         session.formLabel(e.key) + " (" + via.str() + ") references " + session.formLabel(k) +
                             ", which is not part of the compiled output",
                         e.key, session.pluginName(e.winner));
                continue;
            }
            deps.insert(it->second);
        }
        e.deps.assign(deps.begin(), deps.end());
    }

    // ---- Allocation -------------------------------------------------------------
    progress.stage("Compile");
    // Allocation, FormIDs and master lists for N output plugins. Automatic: the fewest outputs whose
    // master lists all fit (tried from 1 up, doubling, then narrowed down); each try starts over.
    const size_t issuesBeforeAllocation = out.issues.size();
    auto allocate = [&](const int N) -> bool {
    out.issues.resize(issuesBeforeAllocation);
    out.outputs.clear();
    out.patchedRecords = 0;
    for (auto& e : out.records) {
        e.output = -1;
        e.objectId = 0;
        e.formIdChanged = false;
    }

    // Patch records go to as many patch plugins as the master limit requires. Records are
    // taken in load order and a new patch starts when the next record's masters would not fit.
    std::vector<int> patchOutput(out.records.size(), -1);
    int patchCount = 0;
    {
        size_t baseCount = 0;
        for (auto pi : session.loadOrder)
            baseCount += session.plugins[pi].baseGame ? 1 : 0;
        const size_t cap = std::min<size_t>(options.patchMasterLimit, 254);
        const size_t limit = cap > baseCount ? cap - baseCount : 1;
        std::vector<uint32_t> patchRecords;
        for (uint32_t v = 0; v < out.records.size(); ++v)
            if (out.records[v].patch)
                patchRecords.push_back(v);
        std::stable_sort(patchRecords.begin(), patchRecords.end(), [&](uint32_t a, uint32_t b) {
            return session.plugins[out.records[a].key.origin].loadOrder < session.plugins[out.records[b].key.origin].loadOrder;
        });
        std::set<PluginIndex> current;
        for (auto v : patchRecords) {
            const auto& e = out.records[v];
            std::set<PluginIndex> need;
            if (e.key.valid() && !session.plugins[e.key.origin].baseGame)
                need.insert(e.key.origin);
            for (const auto& [k, via] : collectRefs(e.fields))
                if (k.valid() && !session.plugins[k.origin].baseGame)
                    need.insert(k.origin);
            std::set<PluginIndex> merged = current;
            merged.insert(need.begin(), need.end());
            if (patchCount == 0 || merged.size() > limit) {
                ++patchCount;
                current = need;
            } else {
                current = std::move(merged);
            }
            patchOutput[v] = N + patchCount - 1;
        }
    }

    out.outputs.resize(static_cast<size_t>(N + patchCount));
    for (int o = 0; o < N; ++o) {
        out.outputs[static_cast<size_t>(o)].index = o;
        out.outputs[static_cast<size_t>(o)].name = outputFileName(options, o);
    }
    for (int k = 0; k < patchCount; ++k) {
        // Patches add no records, so they never need a full load order slot.
        auto& patch = out.outputs[static_cast<size_t>(N + k)];
        patch.index = N + k;
        patch.name = patchFileName(options, k);
        patch.patch = true;
    }

    auto sccs = stronglyConnected(out.records);
    std::vector<uint32_t> sccOf(out.records.size());
    for (uint32_t s = 0; s < sccs.size(); ++s)
        for (auto v : sccs[s])
            sccOf[v] = s;

    uint64_t totalSize = 0;
    for (const auto& e : out.records)
        if (!e.patch)
            totalSize += e.size;
    totalSize = std::max<uint64_t>(totalSize, 1);

    std::vector<int> pref(out.records.size(), 0);
    // Fill outputs in order; small load orders use few plugins instead of
    // spreading a handful of records over every output slot.
    constexpr uint64_t kMinBytesPerOutput = 2ull * 1024 * 1024;
    const uint64_t capacity = std::max<uint64_t>((totalSize + N - 1) / N, kMinBytesPerOutput);
    auto balanced = [&](std::vector<int>& target) {
        uint64_t acc = 0;
        for (const auto& comp : sccs) {
            if (out.records[comp.front()].patch)
                continue;
            uint64_t compSize = 0;
            for (auto v : comp)
                compSize += out.records[v].size;
            int o = static_cast<int>(std::min<uint64_t>(acc / capacity, static_cast<uint64_t>(N - 1)));
            for (auto v : comp)
                target[v] = o;
            acc += compSize;
        }
    };

    switch (options.strategy) {
    case AllocationStrategy::Balanced:
        balanced(pref);
        break;
    case AllocationStrategy::ByRecordType: {
        std::map<int, uint64_t> bucketSize;
        for (const auto& e : out.records)
            if (!e.patch)
                bucketSize[typeRank(session.defs, e.sig)] += e.size;
        // Outputs per bucket: one each, the rest proportional to size.
        std::map<int, std::pair<int, int>> range; // rank -> [first, count)
        int buckets = static_cast<int>(bucketSize.size());
        if (buckets <= N) {
            std::map<int, int> count;
            for (auto& [r, _] : bucketSize)
                count[r] = 1;
            int spare = N - buckets;
            while (spare-- > 0) {
                int best = bucketSize.begin()->first;
                double bestLoad = -1;
                for (auto& [r, sz] : bucketSize) {
                    double load = static_cast<double>(sz) / count[r];
                    if (load > bestLoad) {
                        bestLoad = load;
                        best = r;
                    }
                }
                count[best]++;
            }
            int first = 0;
            for (auto& [r, c] : count) {
                range[r] = {first, c};
                first += c;
            }
        } else {
            uint64_t acc = 0;
            for (auto& [r, sz] : bucketSize) {
                int o = static_cast<int>(std::min<uint64_t>((acc * static_cast<uint64_t>(N)) / totalSize,
                                                            static_cast<uint64_t>(N - 1)));
                range[r] = {o, 1};
                acc += sz;
            }
        }
        std::map<int, uint64_t> filled;
        for (const auto& comp : sccs) {
            for (auto v : comp) {
                if (out.records[v].patch)
                    continue;
                int r = typeRank(session.defs, out.records[v].sig);
                auto [first, cnt] = range[r];
                uint64_t sz = std::max<uint64_t>(bucketSize[r], 1);
                int o = first + static_cast<int>(std::min<uint64_t>((filled[r] * static_cast<uint64_t>(cnt)) / sz,
                                                                    static_cast<uint64_t>(cnt - 1)));
                pref[v] = o;
                filled[r] += out.records[v].size;
            }
        }
        break;
    }
    case AllocationStrategy::UserDefined: {
        balanced(pref);
        for (uint32_t v = 0; v < out.records.size(); ++v) {
            const auto& e = out.records[v];
            if (e.patch)
                continue;
            for (const auto& rule : options.rules) {
                bool match = iequals(rule.pattern, e.sig.str()) ||
                             (e.key.valid() && iequals(rule.pattern, session.pluginName(e.key.origin))) ||
                             std::any_of(e.chain.begin(), e.chain.end(), [&](PluginIndex p) {
                                 return out.compiledFlag_[p] && iequals(rule.pattern, session.pluginName(p));
                             });
                if (match) {
                    pref[v] = std::clamp(rule.output - 1, 0, N - 1);
                    break;
                }
            }
        }
        break;
    }
    }

    // Previous compilation: keep records where they were (save compatibility).
    std::map<std::pair<std::string, uint32_t>, FormMapping> previous;
    if (!options.previousFormMap.empty() && std::filesystem::exists(options.previousFormMap)) {
        for (auto& m : readFormMap(options.previousFormMap))
            previous[{toLower(m.sourcePlugin), m.sourceObjectId}] = m;
        spdlog::info("Loaded {} FormID mappings from the previous compilation", previous.size());
    }
    auto previousFor = [&](const EmittedRecord& e) -> const FormMapping* {
        if (!e.isNew)
            return nullptr;
        auto it = previous.find({session.plugins[e.key.origin].key, e.key.objectId});
        return it == previous.end() ? nullptr : &it->second;
    };
    std::map<std::string, int> outputByName;
    for (int o = 0; o < N; ++o)
        outputByName[toLower(out.outputs[static_cast<size_t>(o)].name)] = o;
    std::vector<int> previousOutput(out.records.size(), -1);
    for (uint32_t v = 0; v < out.records.size(); ++v)
        if (auto m = previousFor(out.records[v])) {
            auto it = outputByName.find(toLower(m->outputPlugin));
            if (it != outputByName.end()) {
                previousOutput[v] = it->second;
                pref[v] = it->second;
            }
        }

    std::vector<std::vector<uint32_t>> worldClusters; // records that must share an output
    // Cells, worldspaces and placed references go into the last output in use (so everything they
    // reference comes before), in clusters that are never split: an interior cell with everything in
    // it, or a worldspace with all its cells. When a cluster's masters would not fit in that output,
    // the next one takes it. A cluster that needs more masters than any plugin may have keeps its
    // compiled plugins out (validation isolates them).
    {
        int worldOutput = 0;
        bool anyWorld = false;
        for (uint32_t v = 0; v < out.records.size(); ++v) {
            if (out.records[v].patch)
                continue;
            if (out.records[v].world)
                anyWorld = true;
            else
                worldOutput = std::max(worldOutput, pref[v]);
        }
        if (anyWorld) {
            auto retainedMastersOf = [&](const EmittedRecord& e, std::set<PluginIndex>& m) {
                if (!e.isNew && e.key.valid())
                    m.insert(e.key.origin);
                for (const auto& [k, via] : collectRefs(e.fields))
                    if (k.valid() && !out.compiledFlag_[k.origin])
                        m.insert(k.origin);
            };
            // Room for other outputs as masters.
            const size_t limit = 254 > static_cast<size_t>(N) ? 254 - static_cast<size_t>(N) : 1;
            std::map<FormKey, std::vector<uint32_t>> clusters; // root (interior cell or worldspace) -> records
            for (uint32_t v = 0; v < out.records.size(); ++v) {
                if (!out.records[v].world || out.records[v].patch)
                    continue;
                FormKey root = out.records[v].key;
                for (int guard = 0; guard < 8; ++guard) {
                    auto it = out.indexByKey.find(root);
                    if (it == out.indexByKey.end() || !out.records[it->second].parent.valid())
                        break;
                    root = out.records[it->second].parent;
                }
                clusters[root].push_back(v);
            }
            std::set<PluginIndex> current;
            for (uint32_t v = 0; v < out.records.size(); ++v)
                if (!out.records[v].world && !out.records[v].patch && pref[v] == worldOutput)
                    retainedMastersOf(out.records[v], current);
            for (const auto& [root, members] : clusters)
                worldClusters.push_back(members);
            int o = worldOutput;
            for (const auto& [root, members] : clusters) {
                std::set<PluginIndex> need;
                for (auto v : members)
                    retainedMastersOf(out.records[v], need);
                std::set<PluginIndex> merged = current;
                merged.insert(need.begin(), need.end());
                if (merged.size() > limit && o + 1 < N) {
                    ++o;
                    merged = need;
                }
                if (merged.size() > limit)
                    addIssue(Severity::Error, "World Layout",
                             session.formLabel(root) + " and everything compiled in it need " +
                                 std::to_string(need.size()) + " masters, more than an output plugin can have",
                             root);
                current = std::move(merged);
                for (auto v : members)
                    pref[v] = o;
            }
        }
    }

    // Constraint: a record may only reference new records in the same or an earlier output.
    for (const auto& comp : sccs) {
        int o = 0;
        for (auto v : comp) {
            o = std::max(o, pref[v]);
            for (auto d : out.records[v].deps)
                if (sccOf[d] != sccOf[v])
                    o = std::max(o, out.records[d].output);
        }
        for (auto v : comp)
            out.records[v].output = out.records[v].patch ? patchOutput[v] : o;
    }
    // A placed reference that links to a new reference in a later output moves there, and its whole
    // cluster (its cell, its worldspace, everything in them) with it; repeat until nothing moves.
    for (bool moved = !worldClusters.empty(); moved;) {
        moved = false;
        for (const auto& members : worldClusters) {
            int o = 0;
            for (auto v : members)
                o = std::max(o, out.records[v].output);
            for (auto v : members)
                if (out.records[v].output != o) {
                    out.records[v].output = o;
                    moved = true;
                }
        }
        for (auto& e : out.records) {
            if (e.patch)
                continue;
            for (auto d : e.deps)
                if (out.records[d].output > e.output) {
                    e.output = out.records[d].output;
                    moved = true;
                }
        }
    }

    // Records whose FormID differs from the previous compilation's (saves made with it may break):
    // listed one by one when few, otherwise summed up per change.
    struct StabilityChange {
        uint32_t record;
        std::string change;
        std::string message;
    };
    std::vector<StabilityChange> stability;
    for (uint32_t v = 0; v < out.records.size(); ++v) {
        auto& e = out.records[v];
        auto& op = out.outputs[static_cast<size_t>(e.output)];
        op.records.push_back(v);
        op.bytes += e.size;
        if (e.patch)
            out.patchedRecords++;
        if (e.isNew)
            op.newRecords++;
        else
            op.overrides++;
        if (previousOutput[v] >= 0 && previousOutput[v] != e.output) {
            e.formIdChanged = true;
            stability.push_back({v, out.outputName(previousOutput[v]) + " to " + out.outputName(e.output),
                                 session.formLabel(e.key) + " moved from " + out.outputName(previousOutput[v]) +
                                     " to " + out.outputName(e.output) +
                                     " because of new dependencies; saves referencing it may break"});
        }
    }

    // ---- FormIDs --------------------------------------------------------------
    for (auto& op : out.outputs) {
        // Never light with new cells or worldspaces: cells added by ESL-flagged plugins lose changes
        // across saves.
        const bool newWorld = std::any_of(op.records.begin(), op.records.end(), [&](uint32_t v) {
            return out.records[v].isNew &&
                   (out.records[v].sig == sig::CELL || out.records[v].sig == Signature("WRLD"));
        });
        op.light = op.patch || (options.lightOutputsWhenPossible && !newWorld &&
                                op.newRecords <= (kLightPluginMaxObjectId - kFirstObjectId + 1));
        const uint32_t maxId = op.light ? kLightPluginMaxObjectId : 0x00FFFFFFu;
        std::set<uint32_t> used;
        for (auto v : op.records) {
            auto& e = out.records[v];
            if (!e.isNew) {
                e.objectId = e.key.objectId;
                continue;
            }
            if (auto m = previousFor(e); m && previousOutput[v] == e.output && m->objectId >= kFirstObjectId &&
                                         m->objectId <= maxId && !used.count(m->objectId)) {
                e.objectId = m->objectId;
                used.insert(e.objectId);
            } else {
                e.objectId = 0;
            }
        }
        uint32_t next = kFirstObjectId;
        for (auto v : op.records) {
            auto& e = out.records[v];
            if (!e.isNew || e.objectId)
                continue;
            while (used.count(next))
                ++next;
            if (next > maxId) {
                addIssue(Severity::Error, "FormID Allocation", op.name + " ran out of FormIDs");
                break;
            }
            if (previousFor(e)) {
                e.formIdChanged = true;
                stability.push_back({v, "a new FormID in " + op.name,
                                     session.formLabel(e.key) + " received a new FormID in " + op.name});
            }
            e.objectId = next;
            used.insert(next++);
        }
    }

    constexpr size_t kStabilityListed = 100;
    if (stability.size() <= kStabilityListed) {
        for (const auto& c : stability)
            addIssue(Severity::Warning, "FormID Stability", c.message, out.records[c.record].key);
    } else {
        std::map<std::string, std::pair<size_t, std::string>> byChange; // change -> (count, example)
        for (const auto& c : stability) {
            auto& entry = byChange[c.change];
            if (entry.first++ == 0)
                entry.second = session.formLabel(out.records[c.record].key);
        }
        for (const auto& [change, entry] : byChange)
            addIssue(Severity::Warning, "FormID Stability",
                     std::to_string(entry.first) + " records " +
                         (change.rfind("a new FormID", 0) == 0 ? "received " + change : "moved from " + change) +
                         " since the previous compilation (e.g. " + entry.second +
                         "); saves made with the previous outputs may lose them. Start a new game, or keep the "
                         "previous outputs");
    }

    // ---- Master lists ---------------------------------------------------------
    std::vector<PluginIndex> basePrefix;
    for (auto pi : session.loadOrder)
        if (session.plugins[pi].baseGame)
            basePrefix.push_back(pi);

    for (auto& op : out.outputs) {
        std::set<PluginIndex> retained(basePrefix.begin(), basePrefix.end());
        std::set<int> outs;
        for (auto v : op.records) {
            const auto& e = out.records[v];
            if (!e.isNew && e.key.valid())
                retained.insert(e.key.origin);
            for (const auto& [k, via] : collectRefs(e.fields)) {
                if (!k.valid())
                    continue;
                if (out.compiledFlag_[k.origin]) {
                    auto it = out.indexByKey.find(k);
                    if (it != out.indexByKey.end() && out.records[it->second].output != op.index)
                        outs.insert(out.records[it->second].output);
                } else {
                    retained.insert(k.origin);
                }
            }
        }
        std::vector<PluginIndex> ordered(retained.begin(), retained.end());
        std::sort(ordered.begin(), ordered.end(), [&](PluginIndex a, PluginIndex b) {
            return session.plugins[a].loadOrder < session.plugins[b].loadOrder;
        });
        op.retainedMasters = ordered;
        op.outputMasters.assign(outs.begin(), outs.end());
        op.masterNames.clear();
        for (auto pi : op.retainedMasters)
            op.masterNames.push_back(session.plugins[pi].name);
        for (int o : op.outputMasters)
            op.masterNames.push_back(out.outputName(o));
        if (op.masterNames.size() > 254)
            addIssue(Severity::Error, "Missing Masters",
                     op.name + " would need " + std::to_string(op.masterNames.size()) + " masters (limit 254)");
    }

    for (size_t i = issuesBeforeAllocation; i < out.issues.size(); ++i)
        if (out.issues[i].severity == Severity::Error &&
            (out.issues[i].category == "Missing Masters" || out.issues[i].category == "World Layout"))
            return false;
    return true;
    };
    int N = options.outputCount;
    if (options.autoOutputs) {
        // Fewest outputs: 1, 2, 4, ... until the masters fit, then the smallest count in between.
        int fits = -1, fails = 0;
        for (int n = 1; n <= CompileOptions::kMaxOutputs; n = std::min(n * 2, CompileOptions::kMaxOutputs + 1)) {
            if (allocate(n)) {
                fits = n;
                break;
            }
            fails = n;
            if (n == CompileOptions::kMaxOutputs)
                break;
        }
        if (fits < 0) {
            N = CompileOptions::kMaxOutputs; // nothing fits: report the errors of the largest count
        } else {
            int lo = fails + 1, hi = fits;
            while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (allocate(mid))
                    hi = mid;
                else
                    lo = mid + 1;
            }
            N = hi;
        }
        options.outputCount = N;
        spdlog::info("Automatic output count: {} output plugins", N);
    }
    allocate(N);

    // ---- Materialise records -------------------------------------------------
    for (auto& op : out.outputs) {
        op.header = PluginHeader{};
        op.header.record.signature = sig::TES4;
        op.header.record.flags = op.light ? RecordFlags::Light : 0;
        op.header.record.formVersion = 131;
        op.header.version = 1.0f;
        op.header.author = options.author;
        if (op.patch)
            op.header.description = "Conflict resolution patch - " + std::to_string(op.records.size()) +
                                    " merged records - generated by Fallout 4 Multiplexer Renewed";
        else
            op.header.description = "Compiled game state " + std::to_string(op.index + 1) + "/" + std::to_string(N) +
                                    " - generated by Fallout 4 Multiplexer Renewed from " +
                                    std::to_string(out.compiledPlugins.size()) + " plugins";
        for (const auto& m : op.masterNames)
            op.header.masters.push_back({m, 0});

        op.built.clear();
        op.built.reserve(op.records.size());
        op.placements.clear();
        op.placements.reserve(op.records.size());
        for (auto v : op.records) {
            auto& e = out.records[v];
            bool ok = true;
            Record rec;
            rec.header.signature = e.sig;
            rec.header.flags = e.fields.flags() & ~RecordFlags::Compressed;
            rec.header.formVersion = e.fields.formVersion ? e.fields.formVersion : 131;
            // Version Control Info of the version written (the winner's; the last version in the load
            // order otherwise), as the game sees it today: the game compares cells' and references'
            // timestamps with the cells' precombine and previs data, and treats a missing one ("none")
            // as changed, disabling them (PJM's previsbine tools keep these values for that reason).
            if (const auto* g = session.findGroup(e.key)) {
                const auto chain = session.instances(*g);
                const RecordInstance* src = chain.empty() ? nullptr : &chain.back();
                for (const auto& inst : chain)
                    if (inst.plugin == e.winner)
                        src = &inst;
                if (src) {
                    rec.header.versionControl1 = src->versionControl1;
                    rec.header.versionControl2 = src->versionControl2;
                }
            }
            rec.header.formId = out.translate(session, e.key, op.index, &ok);
            e.rawFormId = rec.header.formId;
            if (!ok)
                addIssue(Severity::Error, "Broken FormID", "Cannot express the FormID of " + session.formLabel(e.key),
                         e.key, op.name);
            for (const auto& ns : joinFields(e.fields)) {
                Subrecord sr;
                sr.signature = ns.sig;
                sr.data = ns.bytes;
                for (const auto& [off, k] : ns.refs) {
                    bool refOk = true;
                    uint32_t raw = out.translate(session, k, op.index, &refOk);
                    if (!refOk)
                        addIssue(Severity::Error, "Missing Reference",
                                 session.formLabel(e.key) + " (" + ns.sig.str() + ") cannot reference " +
                                     session.formLabel(k) + " from " + op.name,
                                 e.key, op.name);
                    storeLE<uint32_t>(sr.data.data() + off, raw);
                }
                if (e.sig == sig::CELL)
                    sortCellReferenceLists(sr);
                // Undescribed bytes are written exactly as their source wrote them: never rewritten
                // because a value looks like a FormID (validation proves they mean the same here).
                rec.subrecords.push_back(std::move(sr));
            }
            Placement placement;
            placement.kind = e.placement;
            placement.persistent = e.persistent;
            if (e.parent.valid()) {
                bool parentOk = true;
                placement.parent = out.translate(session, e.parent, op.index, &parentOk);
                const auto pit = out.indexByKey.find(e.parent);
                if (!parentOk || pit == out.indexByKey.end() || out.records[pit->second].output != op.index)
                    addIssue(Severity::Error, "World Layout",
                             session.formLabel(e.key) + " is not in the same output as its cell or worldspace " +
                                 session.formLabel(e.parent),
                             e.key, op.name);
            }
            op.built.push_back(std::move(rec));
            op.placements.push_back(placement);
        }
        // SEQ: as zEdit and xEdit build it.
        op.seqFormIds.clear();
        auto startsWithGame = [](const Record& r) {
            const auto* dnam = r.find(Signature("DNAM"));
            return dnam && dnam->data.size() >= 2 && (loadLE<uint16_t>(dnam->data.data()) & 0x0001);
        };
        for (size_t k = 0; k < op.records.size(); ++k) {
            const auto& e = out.records[op.records[k]];
            if (e.sig != Signature("QUST") || !startsWithGame(op.built[k]))
                continue;
            if (!e.isNew) {
                // An override: the defining plugin's SEQ already lists the quest if it starts with the game there.
                const auto* g = session.findGroup(e.key);
                if (!g)
                    continue;
                try {
                    if (startsWithGame(session.loadRecord(session.instances(*g)[0])))
                        continue;
                } catch (const std::exception&) {
                }
            }
            op.seqFormIds.push_back(op.built[k].header.formId);
        }
    }

    // ---- Previs numbering (P2b) ------------------------------------------------
    // Every cell written to an output wins in game (outputs load last), so its previs file is read in the
    // output's numbering from then on. Each FormID is translated from the numbering it is read in today;
    // a file that changes is written loose as a translated copy. One that cannot be translated keeps out
    // the plugins that put the cell in the output (the cell's compiled versions and its compiled
    // references), so the cell keeps today's winning record and file.
    progress.stage("Previs");
    {
        std::map<FormKey, std::vector<uint32_t>> children; // cell -> its compiled references (indices)
        for (uint32_t i = 0; i < out.records.size(); ++i)
            if (out.records[i].parent.valid())
                children[out.records[i].parent].push_back(i);
        for (const auto& e : out.records) {
            if (e.sig != sig::CELL || e.output < 0 || e.isNew)
                continue; // a new cell with previs data keeps its plugin out (above)
            PrevisTranslation t;
            try {
                t = translatePrevis(session, out, e);
            } catch (const std::exception& ex) {
                t.hasFile = true;
                t.ok = false;
                t.problem = ex.what();
            }
            if (!t.hasFile)
                continue;
            if (t.ok) {
                if (t.changed == 0)
                    continue;
                addIssue(Severity::Info, "Previs Numbering",
                         t.path + " (" + session.formLabel(e.key) + "): " + std::to_string(t.changed) + " of " +
                             std::to_string(t.formIds) + " FormIDs translated from " + t.numbering +
                             "'s numbering into " + out.outputName(e.output) +
                             "'s, which now holds the cell's winning record; the translated copy is written loose",
                         e.key, t.numbering);
                PrevisRewrite r;
                r.path = t.path;
                r.bytes = std::move(t.bytes);
                r.cell = e.key;
                r.source = t.source;
                r.numbering = t.numbering;
                r.output = out.outputName(e.output);
                r.formIds = t.formIds;
                r.changed = t.changed;
                out.previsFiles.push_back(std::move(r));
                continue;
            }
            const std::string why = session.formLabel(e.key) + " is written to " + out.outputName(e.output) +
                                    ", so its previs file " + t.path + " would be read in that plugin's numbering, but " +
                                    t.problem + "; the plugins that put the cell there stay, so it keeps today's record";
            // Keep out what puts the cell in the output: its compiled versions, then its compiled references.
            std::set<PluginIndex> named;
            if (std::any_of(e.chain.begin(), e.chain.end(), [&](PluginIndex p) { return out.isCompiled(p); }))
                addIssue(Severity::Error, "Previs Numbering", why, e.key, session.pluginName(e.winner));
            for (uint32_t ci : children[e.key]) {
                const auto& c = out.records[ci];
                const PluginIndex p = c.isNew ? c.key.origin : c.winner;
                if (p == kInvalidPlugin || !out.isCompiled(p) || !named.insert(p).second)
                    continue;
                addIssue(Severity::Error, "Previs Numbering", why, c.key, session.pluginName(p));
            }
        }
    }

    // ---- Mappings and archives ------------------------------------------------
    for (const auto& e : out.records) {
        if (!e.isNew)
            continue;
        FormMapping m;
        m.source = e.key;
        m.sourcePlugin = session.pluginName(e.key.origin);
        m.sourceObjectId = e.key.objectId;
        m.output = e.output;
        m.outputPlugin = out.outputName(e.output);
        m.objectId = e.objectId;
        m.sig = e.sig;
        m.editorId = e.editorId;
        out.mappings.push_back(std::move(m));
    }
    std::string lang = session.install.config.language.empty() ? "en" : session.install.config.language;
    for (auto pi : out.compiledPlugins) {
        const auto& p = session.plugins[pi];
        std::vector<std::string> archives;
        for (const auto& a : AssetScanner::pluginArchiveNames(p, lang))
            if (session.vfs.find(toLower(a)))
                archives.push_back(a);
        if (archives.empty())
            continue;
        if (options.archiveLoaders)
            out.archiveLoaders.push_back(pi);
        else
            out.archivesToRegister.insert(out.archivesToRegister.end(), archives.begin(), archives.end());
    }

    size_t used = std::count_if(out.outputs.begin(), out.outputs.end(),
                                [](const OutputPlugin& o) { return !o.patch && !o.records.empty(); });
    spdlog::info("Compiled {} records ({} new, {} overrides, {} merged) from {} plugins into {} of {} output plugins",
                 out.records.size(), out.mappings.size(), out.records.size() - out.mappings.size(), out.mergedRecords,
                 out.compiledPlugins.size(), used, N);
    if (options.buildPatch)
        spdlog::info("Conflict resolution patch: {} records in {}", out.patchedRecords, options.patchName);
    return out;
}

} // namespace fo4mx
