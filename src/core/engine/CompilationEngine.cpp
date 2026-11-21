#include "engine/CompilationEngine.hpp"
#include "engine/Redirection.hpp"
#include "esp/FieldDecoder.hpp"
#include "engine/PrevisCheck.hpp"

#include "assets/AssetScanner.hpp"
#include "engine/BuildCache.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/Diagnosis.hpp"
#include "engine/MergeEngine.hpp"
#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

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

void CompiledOutput::markCompiled(size_t pluginCount)
{
    compiledFlag_.assign(pluginCount, 0);
    for (auto p : compiledPlugins)
        if (p < compiledFlag_.size())
            compiledFlag_[p] = 1;
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
// Cell record flag "Partial Form": fields the record omits are taken from earlier versions.
constexpr uint32_t kCellPartialFlag = 0x00004000;

// A cell's XPRI (index 0) or XCRI (1) that becomes current when a non-partial version has it: present
// and not empty (XCRI: a mesh or a reference counted).
bool cellListNonEmpty(const Subrecord* sub, int list)
{
    if (!sub)
        return false;
    if (list == 0)
        return !sub->data.empty();
    if (sub->data.size() < 8)
        return false;
    uint32_t meshes = 0, refs = 0;
    std::memcpy(&meshes, sub->data.data(), 4);
    std::memcpy(&refs, sub->data.data() + 4, 4);
    return meshes != 0 || refs != 0;
}

// Whether a cell must be written although the result equals the version that stays: a compiled
// version is partial, or supplies a list the game uses today (see "Cell inheritance").
bool cellInheritanceNeedsRecord(Session& session, std::span<const RecordInstance> inst,
                                const std::vector<uint8_t>& compiled)
{
    static const Signature lists[2] = {Signature("XPRI"), Signature("XCRI")};
    for (const auto& r : inst)
        if (compiled[r.plugin] && (r.flags & kCellPartialFlag))
            return true;
    for (int l = 0; l < 2; ++l)
        for (size_t i = inst.size(); i-- > 0;) {
            if (inst[i].flags & kCellPartialFlag)
                continue;
            if (cellListNonEmpty(session.loadRecord(inst[i]).find(lists[l]), l)) {
                if (compiled[inst[i].plugin])
                    return true;
                break;
            }
        }
    return false;
}


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
        bool planned = false; // the compatibility patch plan carries changes into it
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

        const bool planned = options.applyPatchPlan && options.plannedChanges.count(key.packed());
        if (!inCompiled) {
            if (planned) {
                const auto* entry = session.defs.catalogueEntry(inst[0].signature);
                if (entry && entry->placement == RecordPlacement::Top && !options.skipPatchRecords.count(key.packed())) {
                    Candidate c{gi, false, true, -1, true};
                    c.planned = true;
                    candidates.push_back(c);
                    continue;
                }
            }
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
                if (!out.compiledFlag_[r.plugin] && !session.redirected.count(r.plugin)) // a redirected copy drops it
                    addIssue(Severity::Error, "Dependency",
                             session.pluginName(r.plugin) + " overrides " + session.formLabel(key) +
                                 " from a compiled plugin but stays in the load order",
                             key, session.pluginName(r.plugin));
        } else if (!lastCompiled && (!options.mergeCompatible || winnerOnly) && forced < 0 && !planned) {
            out.retainedWins++;
            continue;
        }
        // Winner-only types: the last plugin's version, taken whole (unless the user chose another).
        if (winnerOnly && forced < 0)
            forced = static_cast<int>(inst.size()) - 1;
        Candidate cand{gi, originCompiled, !lastCompiled, forced, false};
        cand.planned = planned && forced < 0;
        candidates.push_back(cand);
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

                // Compatibility patch plan: the winner plus the planned changes, matched by their stable ids
                // and checked to be held by the result; otherwise the record is written as it would be
                // without the plan.
                std::optional<FieldSet> planned;
                std::vector<std::string> carriedText;
                if (c.planned) {
                    const auto& ids = options.plannedChanges.at(inst[0].key.packed());
                    Normalizer pn(session);
                    std::vector<FieldSet> chain;
                    for (const auto& r : inst)
                        chain.push_back(splitFields(pn.normalize(r), session.defs));
                    const auto rp = traceProvenance(chain, session.overrideParents(inst),
                                                    [](FormKey k) { return formToken(k); }, &session.defs);
                    std::vector<const ChangeEvent*> carry;
                    for (const auto& ev : rp.events)
                        if (isLost(ev.status) &&
                            std::find(ids.begin(), ids.end(), tracedChangeId(session, ev, inst[ev.version].plugin)) != ids.end())
                            carry.push_back(&ev);
                    FieldSet result = chain.back();
                    const auto missing = applyChanges(result, chain, carry);
                    if (carry.size() == ids.size() && missing.empty()) {
                        planned = std::move(result);
                        for (const auto* ev : carry)
                            carriedText.push_back(ev->label + (ev->item.empty() ? "" : " " + renderFormTokens(session, ev->item)) +
                                                  " from " + session.pluginName(inst[ev->version].plugin));
                    } else {
                        mergeIssues[ci].push_back(
                            {Severity::Warning, "Compatibility Patch",
                             session.formLabel(inst[0].key) + ": the plan no longer matches the record (" +
                                 std::to_string(ids.size() - std::min(ids.size(), carry.size() - missing.size())) +
                                 " of " + std::to_string(ids.size()) +
                                 " planned change(s) not found or not held); written as it would be without the plan. "
                                 "Analyse again and review the plan",
                             inst[0].key, session.pluginName(inst.back().plugin)});
                        if (c.patch)
                            return; // only the plan put it in the patch
                    }
                }

                // Once compiled plugins are disabled, the last retained instance is what the game
                // loads. If the result equals it, an override would only be an ITM. Not for a record a
                // compiled plugin defines: with its plugin disabled, the outputs must define it (a redirected
                // plugin's copy leaves its override of it to them).
                for (size_t i = inst.size(); i-- > 0 && !planned && !c.isNew;) {
                    if (out.compiledFlag_[inst[i].plugin])
                        continue;
                    if (m.equalsInstance[i] &&
                        !(inst[0].signature == sig::CELL && cellInheritanceNeedsRecord(session, inst, out.compiledFlag_))) {
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
                // The plugin whose version is written: the user's forced choice, else the last. Its
                // placement and Version Control Info are written with it.
                const auto& source = inst[c.forced >= 0 ? static_cast<size_t>(c.forced) : inst.size() - 1];
                e.winner = source.plugin;
                for (const auto& r : inst)
                    e.chain.push_back(r.plugin);
                for (uint32_t idx : m.contributors)
                    e.contributors.push_back(inst[idx].plugin);
                for (size_t i = 0; i < inst.size() && i < m.issues.size(); ++i)
                    if (out.compiledFlag_[inst[i].plugin])
                        e.issues.insert(e.issues.end(), m.issues[i].begin(), m.issues[i].end());
                e.fields = m.fields;
                if (planned) {
                    e.fields = std::move(*planned);
                    e.merged = true;
                    e.winner = inst.back().plugin;
                    mergeIssues[ci].push_back({Severity::Info, "Compatibility Patch",
                                               session.formLabel(inst[0].key) + " is written as " +
                                                   session.pluginName(inst.back().plugin) +
                                                   "'s version with the planned changes: " + join(carriedText, ", "),
                                               inst[0].key, session.pluginName(inst.back().plugin)});
                }
                e.size = estimateSize(e.fields);
                placeFrom(e, c.planned && planned ? inst.back() : source);
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
        // Encoded in parallel, stored in one transaction.
        std::vector<size_t> which;
        for (size_t ci = 0; ci < candidates.size(); ++ci)
            if (computed[ci])
                which.push_back(ci);
        std::vector<uint64_t> putKeys(which.size());
        std::vector<std::vector<uint8_t>> blobs(which.size());
        parallelFor(
            which.size(),
            [&](size_t k) {
                putKeys[k] = keys[which[k]];
                blobs[k] = serializeMerge(session, *merges[which[k]]);
            },
            nullProgress(), options.threads);
        cache->putMany("merges", putKeys, blobs);
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

    // Cells taken in for the configuration merge -> the records their previs file names (their masters).
    std::map<FormKey, std::vector<FormKey>> configPrevisRecords;
    // ---- Cell configuration merge: cells and worldspaces whose winner stays ----------------
    // A cell or worldspace no output writes, with more than one version and a winning version from a
    // plugin that stays, is taken in as the game loads it now (configOnly). The configuration merge
    // below brings the other versions' changes into it; one it leaves unchanged is dropped again.
    // Partial winners are left as they are, as for written cells.
    if (options.mergeCellConfig) {
        Normalizer norm(session);
        for (uint32_t gi = 0; gi < session.groups.size(); ++gi) {
            const auto inst = session.instances(session.groups[gi]);
            const Signature s = inst[0].signature;
            if ((s != sig::CELL && s != Signature("WRLD")) || inst.size() < 2)
                continue;
            const FormKey key = inst[0].key;
            const auto& last = inst.back();
            if (!key.valid() || out.indexByKey.count(key) || out.compiledFlag_[key.origin] ||
                out.compiledFlag_[last.plugin] || options.skipPatchRecords.count(key.packed()) ||
                (s == sig::CELL && (last.flags & kCellPartialFlag)))
                continue;
            EmittedRecord e;
            e.key = key;
            e.sig = s;
            for (size_t i = inst.size(); i-- > 0;)
                if (inst[i].editorId) {
                    e.editorId = session.strings.get(inst[i].editorId);
                    break;
                }
            e.configOnly = true;
            e.winner = last.plugin;
            for (const auto& r : inst)
                e.chain.push_back(r.plugin);
            e.contributors.push_back(last.plugin);
            try {
                e.fields = splitFields(norm.normalize(last), session.defs);
            } catch (const std::exception& ex) {
                addIssue(Severity::Warning, "Cell Configuration",
                         session.formLabel(key) + ": cannot be read (" + ex.what() + "); left as it is",
                         key, session.pluginName(last.plugin));
                continue;
            }
            e.size = estimateSize(e.fields);
            placeFrom(e, last);
            out.indexByKey[key] = static_cast<uint32_t>(out.records.size());
            out.records.push_back(std::move(e));
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

    // Cell inheritance (PJM's previsbine guide). As the game reads each version of a cell in load order:
    // a non-partial version's record flags, PCMB and VISI become current, even when blank; its XPRI and
    // XCRI become current only when present and not empty; a partial version (flag 0x4000) changes none
    // of these. Compiling removes the compiled versions and puts the written one last, so the lists the
    // game ends up with are worked out from the whole chain as it loads today and written into the
    // record. What cannot be written that way (a partial version written with compiled versions under
    // it) keeps the cell's chain out: an error on the cell keeps every compiled plugin in it.
    {
        Normalizer cellNorm(session);
        const Signature lists[2] = {Signature("XPRI"), Signature("XCRI")};
        const auto nonEmpty = cellListNonEmpty;
        auto fieldIndex = [&](const FieldSet& set, Signature sig) -> int {
            for (size_t i = 0; i < set.fields.size(); ++i) {
                if (set.fields[i].sig == sig)
                    return static_cast<int>(i);
                for (const auto& sub : set.fields[i].subs)
                    if (sub.sig == sig)
                        return static_cast<int>(i);
            }
            return -1;
        };
        for (auto& e : out.records) {
            if (e.sig != sig::CELL)
                continue;
            const auto* g = session.findGroup(e.key);
            if (!g)
                continue;
            const auto chain = session.instances(*g);
            size_t w = chain.size();
            for (size_t i = chain.size(); i-- > 0;)
                if (chain[i].plugin == e.winner) {
                    w = i;
                    break;
                }
            if (w == chain.size())
                continue;
            // Compiled versions besides the one written leave the load order; without any, the cell
            // loads as it does today (the versions that stay, then the written one).
            bool removes = false;
            for (size_t i = 0; i < chain.size(); ++i)
                removes |= i != w && out.isCompiled(chain[i].plugin);
            if (!removes)
                continue;
            auto isPartial = [&](size_t i) { return (chain[i].flags & kCellPartialFlag) != 0; };
            auto stays = [&](size_t i) { return !out.isCompiled(chain[i].plugin); };
            std::vector<std::optional<Record>> loaded(chain.size());
            auto rec = [&](size_t i) -> const Record& {
                if (!loaded[i])
                    loaded[i] = session.loadRecord(chain[i]);
                return *loaded[i];
            };
            // The version whose list l is current after reading versions [0, end) that pass `use`.
            auto supplier = [&](int l, size_t end, const std::function<bool(size_t)>& use) -> int {
                for (size_t i = end; i-- > 0;)
                    if (use(i) && !isPartial(i) && nonEmpty(rec(i).find(lists[l]), l))
                        return static_cast<int>(i);
                return -1;
            };
            const auto all = [](size_t) { return true; };
            std::string problem;
            std::vector<std::string> inherited;
            try {
                if (!isPartial(w)) {
                    // Written non-partial and last: its flags, PCMB and VISI are current, as today. Each list
                    // is the one current today; the versions that stay load before it.
                    for (int l = 0; l < 2 && problem.empty(); ++l) {
                        const int from = supplier(l, w + 1, all);
                        if (from < 0) {
                            // None today; a version that stays and loads after the written one (a forced
                            // winner) would supply one, and no list in the written record can clear it.
                            for (size_t i = w + 1; i < chain.size(); ++i)
                                if (stays(i) && !isPartial(i) && nonEmpty(rec(i).find(lists[l]), l))
                                    problem = session.pluginName(chain[i].plugin) + "'s version has " +
                                              lists[l].str() + ", which the version written cannot clear";
                            continue;
                        }
                        // The written version's own list, or one from a version that stays: it loads before
                        // the written one and stays current, as today.
                        if (static_cast<size_t>(from) == w || stays(static_cast<size_t>(from)))
                            continue;
                        // A compiled version supplies it today and leaves the load order: take it from there.
                        FieldSet source = splitFields(cellNorm.normalize(chain[from]), session.defs);
                        const int si = fieldIndex(source, lists[l]);
                        if (si < 0) {
                            problem = "the " + lists[l].str() + " of " + session.pluginName(chain[from].plugin) +
                                      "'s version could not be read";
                            continue;
                        }
                        Field taken = source.fields[static_cast<size_t>(si)];
                        if (int di = fieldIndex(e.fields, lists[l]); di >= 0) {
                            e.fields.fields[static_cast<size_t>(di)] = std::move(taken);
                        } else {
                            // After the field that precedes it in the source version, else at the end.
                            size_t pos = e.fields.fields.size();
                            for (int p = si; p-- > 0;) {
                                const auto key = source.fields[static_cast<size_t>(p)].key;
                                auto it = std::find_if(e.fields.fields.begin(), e.fields.fields.end(),
                                                       [&](const Field& f) { return f.key == key; });
                                if (it != e.fields.fields.end()) {
                                    pos = static_cast<size_t>(it - e.fields.fields.begin()) + 1;
                                    break;
                                }
                            }
                            e.fields.fields.insert(e.fields.fields.begin() + static_cast<std::ptrdiff_t>(pos),
                                                   std::move(taken));
                        }
                        inherited.push_back(lists[l].str() + " from " + session.pluginName(chain[from].plugin));
                    }
                } else {
                    // Written partial: it changes none of these, so the versions under it must give the game
                    // what they give it today. The last non-partial version before it, and everything after
                    // that, must stay; earlier compiled versions only matter through the lists.
                    int base = -1;
                    for (size_t i = w; i-- > 0;)
                        if (!isPartial(i)) {
                            base = static_cast<int>(i);
                            break;
                        }
                    if (base < 0) {
                        problem = "is a partial cell (flag 0x4000) with no full version under it";
                    } else {
                        for (size_t i = static_cast<size_t>(base); i < chain.size() && problem.empty(); ++i)
                            if (i != w && !stays(i))
                                problem = "is a partial cell (flag 0x4000), which takes the fields it omits from "
                                          "earlier versions, and " + session.pluginName(chain[i].plugin) +
                                          "'s version, which it would take them from, would leave the load order";
                        for (int l = 0; l < 2 && problem.empty(); ++l)
                            if (supplier(l, w, all) != supplier(l, w, stays))
                                problem = "is a partial cell (flag 0x4000), and the " + lists[l].str() +
                                          " current under it today comes from a version that would leave the load order";
                    }
                }
            } catch (const std::exception& ex) {
                problem = std::string("cannot be compared with its earlier versions (") + ex.what() + ")";
            }
            if (!problem.empty() && e.configOnly) {
                // Taken in only for the configuration merge: left out again, so the cell loads as it
                // would have without the merge.
                previsCells.insert(e.key);
                e.configChanged = false;
            } else if (!problem.empty()) {
                previsCells.insert(e.key);
                addIssue(Severity::Error, "Cell Inheritance",
                         session.formLabel(e.key) + " as written from " + session.pluginName(e.winner) + ": " +
                             problem + "; compiling would change what the cell inherits, so the plugins compiling "
                             "the cell stay",
                         e.key, session.pluginName(e.winner));
            } else if (!inherited.empty()) {
                e.configChanged = true;
                e.size = estimateSize(e.fields);
                addIssue(Severity::Info, "Cell Inheritance",
                         session.formLabel(e.key) + " is written from " + session.pluginName(e.winner) +
                             ", which omits its lists; written with the ones the game uses today: " +
                             join(inherited, ", "),
                         e.key, session.pluginName(e.winner));
            }
        }
    }
    // Cell configuration merge (optional, PJM's FO4CheckPrevisbines "Fix only Cell Config Conflicts"): a
    // written cell keeps the winner's precombine and previs data and takes the last real change of each
    // configuration field from the versions loading today. A change is a version whose value is set and
    // differs from the defining plugin's; a version only repeating that value is not one. Regions are
    // the union of every version's list. Never touched: PCMB, XCRI, XPRI, VISI, RVIS, the No Pre Vis
    // flag and the record's identity. Partial cells, and cells the inheritance guard keeps out, as they are.
    if (options.mergeCellConfig) {
        Normalizer cfgNorm(session);
        static const std::vector<Signature> kCellSigs = {
            Signature("EDID"), Signature("MHDT"), Signature("XCLL"), Signature("XLCN"), Signature("XCMO"),
            Signature("XEZN"), Signature("XCAS"), Signature("XCIM"), Signature("XGDR"), Signature("XCCM"),
            Signature("XCWT"), Signature("XWCU"), Signature("XCLW"), Signature("XILW"), Signature("LTMP")};
        // Worldspaces: max height data, water environment map, map image, encounter zone, climate,
        // interior lighting, location, water, LOD water, music. Never the parent worldspace (WNAM, PNAM).
        static const std::vector<Signature> kWorldSigs = {
            Signature("MHDT"), Signature("XWEM"), Signature("ICON"), Signature("XEZN"), Signature("CNAM"),
            Signature("LTMP"), Signature("XLCN"), Signature("NAM2"), Signature("NAM3"), Signature("ZNAM")};
        // xEdit's CELL DATA flags: Has Water, Can't Travel From Here, No LOD Water, Public Area, Hand
        // Changed, Show Sky, Use Sky Lighting, Sunlight Shadows, Distant LOD only, Player Followers Can't
        // Travel Here. Record flags: Off Limits (17), Can't Wait (19); never No Pre Vis (7).
        constexpr uint16_t kDataBits = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) |
                                       (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
        constexpr uint32_t kRecordBits = (1u << 17) | (1u << 19);
        auto sameSubs = [](const std::vector<NormSub>& a, const std::vector<NormSub>& b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].sig != b[i].sig || a[i].bytes != b[i].bytes || a[i].refs != b[i].refs)
                    return false;
            return true;
        };
        auto sameField = [&](const Field* a, const Field* b) {
            if (!a || !b)
                return a == b;
            if (!sameSubs(a->subs, b->subs) || a->items.size() != b->items.size())
                return false;
            for (size_t i = 0; i < a->items.size(); ++i)
                if (!sameSubs(a->items[i].subs, b->items[i].subs))
                    return false;
            return true;
        };
        auto blank = [](const Field* f) {
            if (!f)
                return true;
            auto subsBlank = [](const std::vector<NormSub>& subs) {
                for (const auto& s : subs) {
                    for (const auto& [off, k] : s.refs)
                        if (k.valid())
                            return false;
                    for (uint8_t b : s.bytes)
                        if (b != 0)
                            return false;
                }
                return true;
            };
            if (!subsBlank(f->subs))
                return false;
            for (const auto& it : f->items)
                if (!subsBlank(it.subs))
                    return false;
            return true;
        };
        auto bySig = [](FieldSet& set, Signature sig) -> Field* {
            for (auto& f : set.fields) {
                if (f.sig == sig)
                    return &f;
                for (const auto& s : f.subs)
                    if (s.sig == sig)
                        return &f;
            }
            return nullptr;
        };
        auto u16Of = [](const Field* f) -> uint16_t {
            return f && !f->subs.empty() && f->subs[0].bytes.size() >= 2 ? loadLE<uint16_t>(f->subs[0].bytes.data())
                                                                          : 0;
        };
        // A field the written version lacks goes where the version it comes from has it: after the
        // nearest field before it that the written version has, else before the nearest one after it.
        auto insertLike = [](FieldSet& dst, const FieldSet& from, const Field& field) -> Field* {
            const auto& sf = from.fields;
            size_t at = sf.size();
            for (size_t i = 0; i < sf.size(); ++i)
                if (sf[i].key == field.key) {
                    at = i;
                    break;
                }
            auto find = [&](const std::string& key) {
                return std::find_if(dst.fields.begin(), dst.fields.end(), [&](const Field& f) { return f.key == key; });
            };
            size_t pos = dst.fields.size();
            bool placed = false;
            for (size_t p = std::min(at, sf.size()); p-- > 0 && !placed;)
                if (auto it = find(sf[p].key); it != dst.fields.end()) {
                    pos = static_cast<size_t>(it - dst.fields.begin()) + 1;
                    placed = true;
                }
            for (size_t n = at + 1; n < sf.size() && !placed; ++n)
                if (auto it = find(sf[n].key); it != dst.fields.end()) {
                    pos = static_cast<size_t>(it - dst.fields.begin());
                    placed = true;
                }
            // Step past the written version's fields that xEdit puts before this one (as MergeEngine).
            if (const int rank = subrecordRank(dst.sig, field.sig); rank >= 0)
                while (pos < dst.fields.size() && std::none_of(sf.begin(), sf.end(), [&](const Field& f) {
                           return f.key == dst.fields[pos].key;
                       })) {
                    const int here = subrecordRank(dst.sig, dst.fields[pos].sig);
                    if (here < 0 || here >= rank)
                        break;
                    ++pos;
                }
            return &*dst.fields.insert(dst.fields.begin() + static_cast<std::ptrdiff_t>(pos), field);
        };
        for (auto& e : out.records) {
            const bool isCell = e.sig == sig::CELL;
            if ((!isCell && e.sig != Signature("WRLD")) || previsCells.count(e.key))
                continue;
            const auto* g = session.findGroup(e.key);
            if (!g)
                continue;
            const auto chain = session.instances(*g);
            size_t w = chain.size();
            for (size_t i = chain.size(); i-- > 0;)
                if (chain[i].plugin == e.winner) {
                    w = i;
                    break;
                }
            if (w == chain.size() || w == 0 || (isCell && (chain[w].flags & kCellPartialFlag)))
                continue;
            std::vector<std::string> taken;
            try {
                // The versions loading today up to the written one; partial versions supply nothing here.
                std::vector<FieldSet> versions;
                std::vector<PluginIndex> from;
                for (size_t i = 0; i <= w; ++i) {
                    if (i > 0 && isCell && (chain[i].flags & kCellPartialFlag))
                        continue;
                    versions.push_back(splitFields(cfgNorm.normalize(chain[i]), session.defs));
                    from.push_back(chain[i].plugin);
                }
                const size_t last = versions.size() - 1; // the written version
                auto& base = versions.front();
                for (const Signature sig : isCell ? kCellSigs : kWorldSigs) {
                    const Field* baseField = bySig(base, sig);
                    int change = -1;
                    for (size_t v = versions.size(); v-- > 1;) {
                        const Field* f = bySig(versions[v], sig);
                        if (!blank(f) && !sameField(f, baseField)) {
                            change = static_cast<int>(v);
                            break;
                        }
                    }
                    if (change < 0 || static_cast<size_t>(change) == last)
                        continue;
                    const Field* src = bySig(versions[static_cast<size_t>(change)], sig);
                    Field* dst = bySig(e.fields, sig);
                    if (sameField(src, dst))
                        continue;
                    if (dst)
                        *dst = *src;
                    else
                        insertLike(e.fields, versions[static_cast<size_t>(change)], *src);
                    taken.push_back(sig.str() + " from " + session.pluginName(from[static_cast<size_t>(change)]));
                }
                // Regions: every region any version lists, the written version's first.
                if (isCell) {
                    Field* dst = bySig(e.fields, Signature("XCLR"));
                    std::set<std::string> sources;
                    size_t added = 0;
                    for (size_t v = 0; v < last; ++v) {
                        const Field* f = bySig(versions[v], Signature("XCLR"));
                        if (!f || !f->isList())
                            continue;
                        if (!dst) {
                            // The written version lists none: start from this version's field, in its place.
                            Field copy = *f;
                            copy.items.clear();
                            dst = insertLike(e.fields, versions[v], copy);
                        }
                        if (!dst->isList())
                            break;
                        for (const auto& item : f->items) {
                            const bool real = !item.subs.empty() && !item.subs[0].refs.empty() &&
                                              item.subs[0].refs[0].second.valid();
                            const bool listed = std::any_of(dst->items.begin(), dst->items.end(),
                                                            [&](const ListItem& d) { return sameSubs(d.subs, item.subs); });
                            if (real && !listed) {
                                dst->items.push_back(item);
                                ++added;
                                sources.insert(session.pluginName(from[v]));
                            }
                        }
                    }
                    if (added)
                        taken.push_back("XCLR (" + std::to_string(added) + " region(s)) from " +
                                        join(std::vector<std::string>(sources.begin(), sources.end()), ", "));
                }
                // Flags, bit by bit: the last version whose bit differs from the defining plugin's.
                auto mergeBits = [&](uint32_t mask, auto get, auto set, const char* what) {
                    const uint32_t baseValue = get(base);
                    uint32_t value = get(versions[last]);
                    std::set<std::string> sources;
                    for (uint32_t bit = 0; bit < 32; ++bit) {
                        const uint32_t m = 1u << bit;
                        if (!(mask & m))
                            continue;
                        for (size_t v = versions.size(); v-- > 1;)
                            if ((get(versions[v]) & m) != (baseValue & m)) {
                                if (v != last && (value & m) != (get(versions[v]) & m)) {
                                    value = (value & ~m) | (get(versions[v]) & m);
                                    sources.insert(session.pluginName(from[v]));
                                }
                                break;
                            }
                    }
                    if (!sources.empty() && set(value))
                        taken.push_back(std::string(what) + " from " +
                                        join(std::vector<std::string>(sources.begin(), sources.end()), ", "));
                };
                if (isCell) {
                mergeBits(
                    kDataBits, [&](FieldSet& s) -> uint32_t { return u16Of(bySig(s, Signature("DATA"))); },
                    [&](uint32_t v) {
                        Field* d = bySig(e.fields, Signature("DATA"));
                        if (!d || d->subs.empty() || d->subs[0].bytes.size() < 2)
                            return false;
                        storeLE<uint16_t>(d->subs[0].bytes.data(), static_cast<uint16_t>(v));
                        return true;
                    },
                    "DATA flags");
                mergeBits(
                    kRecordBits, [&](FieldSet& s) -> uint32_t { return s.flags(); },
                    [&](uint32_t v) {
                        Field* f = e.fields.find("@flags");
                        if (!f || f->subs.empty() || f->subs[0].bytes.size() < 4)
                            return false;
                        storeLE<uint32_t>(f->subs[0].bytes.data(), v);
                        return true;
                    },
                    "record flags (Off Limits, Can't Wait)");
                }
            } catch (const std::exception& ex) {
                addIssue(Severity::Warning, "Cell Configuration",
                         session.formLabel(e.key) + ": its versions could not be compared (" + ex.what() +
                             (e.configOnly ? std::string("); left as it is")
                                           : "); written from " + session.pluginName(e.winner) + " as it is"),
                         e.key, session.pluginName(e.winner));
                if (e.configOnly)
                    e.configChanged = false;
                continue;
            }
            if (!taken.empty()) {
                e.configChanged = true;
                e.size = estimateSize(e.fields);
                addIssue(Severity::Info, "Cell Configuration",
                         session.formLabel(e.key) + " is written from " + session.pluginName(e.winner) +
                             " with the last change of each configuration field: " + join(taken, ", "),
                         e.key, session.pluginName(e.winner));
            }
        }
    // ---- Large references (WRLD RNAM) -----------------------------------------------------------------
    // A worldspace's large references (RNAM, one subrecord per cell) are replaced, not added up, by a later
    // plugin: in the owner's build the outputs wrote worldspaces without RNAM, and the game then listed
    // 15,790 more large references than before compiling (FasterStartup's audit), which only replacement
    // explains (adding up could only lose some). Whether the game takes the last plugin with RNAM or the
    // last RNAM of each cell is not documented, so the list in force today is worked out both ways:
    // where they agree and compiling would change it, the written worldspace carries today's list; where
    // they disagree, nothing is changed and a warning says so.
    {
        Normalizer lrNorm(session);
        using CellList = std::map<uint32_t, const NormSub*>; // cell (Y, X) -> its RNAM
        auto cellOf = [](const NormSub& s) { return s.bytes.size() >= 4 ? loadLE<uint32_t>(s.bytes.data()) : 0u; };
        auto sameSub = [](const NormSub* a, const NormSub* b) {
            return a && b && a->bytes == b->bytes && a->refs == b->refs;
        };
        auto sameList = [&](const CellList& a, const CellList& b) {
            if (a.size() != b.size())
                return false;
            for (const auto& [cell, s] : a) {
                auto it = b.find(cell);
                if (it == b.end() || !sameSub(s, it->second))
                    return false;
            }
            return true;
        };
        // In force after loading `versions` in order: the last version with any RNAM (whole), or per cell.
        auto inForce = [&](const std::vector<const std::vector<NormSub>*>& versions, CellList& whole, CellList& perCell) {
            whole.clear();
            perCell.clear();
            for (const auto* subs : versions) {
                CellList mine;
                for (const auto& s : *subs)
                    if (s.sig == Signature("RNAM"))
                        mine[cellOf(s)] = &s;
                if (!mine.empty())
                    whole = mine;
                for (const auto& [cell, s] : mine)
                    perCell[cell] = s;
            }
        };
        for (auto& e : out.records) {
            if (e.sig != Signature("WRLD"))
                continue;
            const auto* g = session.findGroup(e.key);
            if (!g)
                continue;
            const auto chain = session.instances(*g);
            if (chain.size() < 2)
                continue;
            try {
                std::vector<std::vector<NormSub>> subs;
                subs.reserve(chain.size() + 1);
                for (const auto& r : chain)
                    subs.push_back(lrNorm.normalize(r).subs);
                std::vector<const std::vector<NormSub>*> today, after;
                for (size_t i = 0; i < chain.size(); ++i) {
                    today.push_back(&subs[i]);
                    if (!out.compiledFlag_[chain[i].plugin])
                        after.push_back(&subs[i]);
                }
                subs.push_back(joinFields(e.fields)); // the written version loads after every plugin that stays
                after.push_back(&subs.back());
                CellList todayWhole, todayCell, afterWhole, afterCell;
                inForce(today, todayWhole, todayCell);
                inForce(after, afterWhole, afterCell);
                if (sameList(todayWhole, afterWhole) && sameList(todayCell, afterCell))
                    continue;
                const std::string where = session.formLabel(e.key);
                if (!sameList(todayWhole, todayCell)) {
                    addIssue(Severity::Warning, "Large References",
                             where + ": compiling changes which large references (RNAM) the game uses, and today's list "
                                     "differs depending on how the game combines plugins' lists; left as it is (check "
                                     "distant objects in this worldspace in game)",
                             e.key, session.pluginName(e.winner));
                    continue;
                }
                // Today's list comes whole from one version: the written worldspace carries exactly it.
                size_t supplier = chain.size();
                for (size_t i = chain.size(); i-- > 0;)
                    if (std::any_of(subs[i].begin(), subs[i].end(), [](const NormSub& s) { return s.sig == Signature("RNAM"); })) {
                        supplier = i;
                        break;
                    }
                std::set<PluginIndex> origins;
                size_t entries = 0;
                for (const auto& [cell, s] : todayWhole) {
                    for (const auto& [off, k] : s->refs)
                        if (k.valid())
                            origins.insert(k.origin);
                    entries += s->bytes.size() >= 8 ? (s->bytes.size() - 8) / 8 : 0;
                }
                if (origins.size() > 128) {
                    addIssue(Severity::Warning, "Large References",
                             where + ": today's large references (RNAM, from " + session.pluginName(chain[supplier].plugin) +
                                 ") name references of " + std::to_string(origins.size()) +
                                 " plugins, too many masters to add; left as it is",
                             e.key, session.pluginName(e.winner));
                    continue;
                }
                FieldSet source = splitFields(lrNorm.normalize(chain[supplier]), session.defs);
                e.fields.fields.erase(std::remove_if(e.fields.fields.begin(), e.fields.fields.end(),
                                                     [](const Field& f) { return f.sig == Signature("RNAM"); }),
                                      e.fields.fields.end());
                for (const auto& f : source.fields)
                    if (f.sig == Signature("RNAM"))
                        insertLike(e.fields, source, f);
                // Check: after loading, the game now uses today's list, both ways.
                subs.back() = joinFields(e.fields);
                after.back() = &subs.back();
                inForce(after, afterWhole, afterCell);
                if (!sameList(todayWhole, afterWhole) || !sameList(todayCell, afterCell))
                    throw std::runtime_error("the written large references do not reproduce today's");
                if (e.configOnly)
                    e.configChanged = true;
                e.size = estimateSize(e.fields);
                addIssue(Severity::Info, "Large References",
                         where + " is written with the large references (RNAM) the game uses today: " +
                             std::to_string(todayWhole.size()) + " cell(s), " + std::to_string(entries) +
                             " reference(s), from " + session.pluginName(chain[supplier].plugin) +
                             (out.compiledFlag_[chain[supplier].plugin] ? " (compiled)" : ""),
                         e.key, session.pluginName(e.winner));
            } catch (const std::exception& ex) {
                addIssue(Severity::Warning, "Large References",
                         session.formLabel(e.key) + ": large references (RNAM) could not be compared (" + ex.what() +
                             "); left as it is",
                         e.key, session.pluginName(e.winner));
            }
        }
    }
    }
    // Cells and worldspaces taken in only for the configuration merge: kept when it changed them, with
    // the worldspace an exterior cell needs (the game's current version when nothing changed it). They
    // go to the patch (Multiplexer_Patch.esp), which loads last; an exterior cell whose worldspace an
    // output already holds goes with it into that output instead, as a plugin holds a cell only with its
    // worldspace and outputs, too, load after every plugin that stays. A cell with its own previs file
    // makes the plugin holding it the one the game reads the file's FormIDs in, so the file is
    // translated there (Previs Numbering, below) and the records it names become that plugin's masters;
    // a file that cannot be read leaves the cell as it is.
    if (options.mergeCellConfig) {
        for (auto& e : out.records) {
            if (!e.configOnly || !e.configChanged || e.sig != sig::CELL)
                continue;
            std::vector<FormKey> keys;
            std::string problem;
            bool ok = previsRecords(session, e, keys, problem);
            for (const auto& k : keys)
                if (ok && out.compiledFlag_[k.origin] && !out.indexByKey.count(k)) {
                    ok = false;
                    problem = "its previs file names " + session.formLabel(k) + ", which no output writes";
                }
            if (!ok) {
                e.configChanged = false;
                addIssue(Severity::Warning, "Cell Configuration",
                         session.formLabel(e.key) + ": left as it is; " + problem, e.key,
                         session.pluginName(e.winner));
            } else if (!keys.empty()) {
                configPrevisRecords[e.key] = std::move(keys);
            }
        }
        std::set<FormKey> needed;
        for (const auto& e : out.records)
            if (e.configOnly && e.configChanged && e.parent.valid())
                needed.insert(e.parent);
        std::vector<EmittedRecord> kept;
        kept.reserve(out.records.size());
        for (auto& e : out.records) {
            if (e.configOnly && !e.configChanged) {
                if (!needed.count(e.key))
                    continue;
                e.container = true;
                out.containers++;
            } else if (e.configOnly) {
                out.configRecords++;
            }
            kept.push_back(std::move(e));
        }
        out.records = std::move(kept);
        out.indexByKey.clear();
        for (uint32_t i = 0; i < out.records.size(); ++i)
            out.indexByKey[out.records[i].key] = i;
        // A worldspace no output writes and with no other version: copied as the game loads it.
        Normalizer norm(session);
        for (const auto& k : needed) {
            if (out.indexByKey.count(k))
                continue;
            const auto* g = session.findGroup(k);
            if (!g || out.compiledFlag_[k.origin])
                continue; // checked below: the cell is then dropped
            const auto inst = session.instances(*g);
            EmittedRecord w;
            w.key = k;
            w.sig = inst.back().signature;
            for (size_t i = inst.size(); i-- > 0;)
                if (inst[i].editorId) {
                    w.editorId = session.strings.get(inst[i].editorId);
                    break;
                }
            w.container = true;
            w.configOnly = true;
            w.winner = inst.back().plugin;
            for (const auto& r : inst)
                w.chain.push_back(r.plugin);
            w.contributors.push_back(inst.back().plugin);
            try {
                w.fields = splitFields(norm.normalize(inst.back()), session.defs);
            } catch (const std::exception&) {
                continue;
            }
            w.size = estimateSize(w.fields);
            placeFrom(w, inst.back());
            out.indexByKey[k] = static_cast<uint32_t>(out.records.size());
            out.records.push_back(std::move(w));
            out.containers++;
        }
        // Where each goes; a cell whose worldspace could not be copied is left as it is.
        std::vector<uint8_t> drop(out.records.size(), 0);
        bool dropping = false;
        for (auto& e : out.records) {
            if (!e.configOnly)
                continue;
            e.patch = true;
            if (!e.parent.valid())
                continue;
            auto it = out.indexByKey.find(e.parent);
            if (it == out.indexByKey.end()) {
                addIssue(Severity::Warning, "Cell Configuration",
                         session.formLabel(e.key) + ": left as it is; its worldspace " +
                             session.formLabel(e.parent) + " cannot be written with it",
                         e.key, session.pluginName(e.winner));
                drop[out.indexByKey.at(e.key)] = 1;
                dropping = true;
                out.configRecords--;
                continue;
            }
            if (!out.records[it->second].configOnly)
                e.patch = false; // with its worldspace, in an output
        }
        if (dropping) {
            std::vector<EmittedRecord> rest;
            for (uint32_t i = 0; i < out.records.size(); ++i)
                if (!drop[i])
                    rest.push_back(std::move(out.records[i]));
            out.records = std::move(rest);
            out.indexByKey.clear();
            for (uint32_t i = 0; i < out.records.size(); ++i)
                out.indexByKey[out.records[i].key] = i;
        }
        for (const auto& e : out.records)
            if (e.configOnly && !e.container)
                addIssue(Severity::Info, "Cell Configuration",
                         session.formLabel(e.key) + "'s winning version (" + session.pluginName(e.winner) +
                             ") stays in the load order; the merged " + (e.sig == sig::CELL ? "cell" : "worldspace") +
                             " is written to " + (e.patch ? options.patchName : std::string("the output holding its worldspace")) +
                             ", which loads after it",
                         e.key, session.pluginName(e.winner));
    }

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
        if (out.records[it->second].isNew && !session.newCellChecks.count(k)) {
            Session::NewCellAssessment a;
            try {
                a.movable = assessNewCell(session, out.records[it->second], a.text);
            } catch (const std::exception& ex) {
                a.text = "Check of new cell " + session.formLabel(k) + " failed: " + ex.what();
            }
            session.newCellChecks.emplace(k, std::move(a));
        }
    }

    // ---- Rewrite (dependency extraction) ------------------------------------------
    progress.stage("Rewrite");
    // References that cannot be written faithfully: a master missing from the load order, or a record a
    // compiled plugin does not contain. Nothing is guessed. By default the plugin that stores such a
    // reference is kept out (fail closed: it stays in the load order and the game reads its own bytes);
    // with brokenReferencesAsNull (opt-in, lossy) they are written as null and reported.
    for (auto& e : out.records) {
        auto nullBroken = [&](NormSub& s) {
            for (auto& [off, k] : s.refs) {
                // What the source plugin stored here, before resolution: shown so the report says
                // exactly what the plugin contained.
                const auto* stored = s.rawAt(off);
                const std::string evidence =
                    stored && stored->plugin < session.plugins.size()
                        ? " (" + session.pluginName(stored->plugin) + " stores " + formatFormId(stored->value) + ")"
                        : std::string();
                bool badMaster = !k.valid() && k.objectId != 0;
                bool missing = k.valid() && out.compiledFlag_[k.origin] && !out.indexByKey.count(k) && !session.exists(k);
                if (!badMaster && !missing)
                    continue;
                const std::string what =
                    session.formLabel(e.key) + " (" + s.sig.str() + ") refers to " +
                    (badMaster ? "FormID " + formatFormId(k.objectId) + " whose master is missing"
                               : formatFormId(k.objectId) + " in " + session.pluginName(k.origin)) +
                    evidence + ", which does not exist";
                if (!options.brokenReferencesAsNull) {
                    // Fail closed: the value is left as it is (it is never written), and the plugin that
                    // stores it stays in the load order.
                    PluginIndex holder = stored && stored->plugin < session.plugins.size() ? stored->plugin : e.winner;
                    out.brokenReferences.push_back({e.key, holder, what});
                    continue;
                }
                addIssue(Severity::Warning, "Broken in Source",
                         what + "; written as null because \"Write broken references as null\" is on (the "
                                "game's own reading of it is not verified)",
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
            if (auto it = configPrevisRecords.find(e.key); it != configPrevisRecords.end())
                for (const auto& k : it->second)
                    if (!session.plugins[k.origin].baseGame)
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
        // A cell goes into the patch plugin holding its worldspace.
        for (auto v : patchRecords)
            if (const auto& e = out.records[v]; e.parent.valid())
                if (auto it = out.indexByKey.find(e.parent); it != out.indexByKey.end() && out.records[it->second].patch)
                    patchOutput[v] = patchOutput[it->second];
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

    // Each output's light state in the previous build, from its installed plugin: -1 unknown, 0 full, 1 light.
    std::vector<int> previousLight(static_cast<size_t>(N), -1);
    std::vector<uint8_t> hadPrevious(static_cast<size_t>(N), 0);
    if (!previous.empty()) {
        for (int o = 0; o < N; ++o)
            for (const auto& p : session.plugins)
                if (p.generated && iequals(p.name, out.outputs[static_cast<size_t>(o)].name))
                    previousLight[static_cast<size_t>(o)] = p.isLight() ? 1 : 0;
        for (int o : previousOutput)
            if (o >= 0)
                hadPrevious[static_cast<size_t>(o)] = 1;
    }
    constexpr size_t kLightCapacity = kLightPluginMaxObjectId - kFirstObjectId + 1;
    // Append-only: new records that records the previous build placed reference go into the same output
    // or an earlier one, so the placed records need not move (and keep their FormIDs). An output that was
    // light takes them only while it stays within the light range.
    if (!previous.empty()) {
        std::vector<int> cap(out.records.size(), N);
        std::vector<uint32_t> stack;
        auto lower = [&](uint32_t d, int to) {
            const auto& r = out.records[d];
            if (previousOutput[d] >= 0 || r.patch || r.world || to >= cap[d])
                return;
            cap[d] = to;
            stack.push_back(d);
        };
        for (uint32_t v = 0; v < out.records.size(); ++v)
            if (previousOutput[v] >= 0 && !out.records[v].patch)
                for (auto d : out.records[v].deps)
                    lower(d, previousOutput[v]);
        while (!stack.empty()) {
            const uint32_t d = stack.back();
            stack.pop_back();
            for (auto dd : out.records[d].deps)
                lower(dd, cap[d]);
        }
        std::vector<size_t> newIn(static_cast<size_t>(N), 0);
        for (uint32_t v = 0; v < out.records.size(); ++v)
            if (out.records[v].isNew && !out.records[v].patch)
                ++newIn[static_cast<size_t>(pref[v])];
        size_t pulled = 0;
        for (uint32_t v = 0; v < out.records.size(); ++v) {
            if (cap[v] >= N || pref[v] <= cap[v])
                continue;
            const auto to = static_cast<size_t>(cap[v]);
            if (out.records[v].isNew && previousLight[to] == 1 && newIn[to] >= kLightCapacity)
                continue;
            if (out.records[v].isNew) {
                --newIn[static_cast<size_t>(pref[v])];
                ++newIn[to];
            }
            pref[v] = cap[v];
            ++pulled;
        }
        if (pulled)
            spdlog::info("{} new records placed beside the records of the previous build that use them", pulled);
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
                if (auto it = configPrevisRecords.find(e.key); it != configPrevisRecords.end())
                    for (const auto& k : it->second)
                        if (!out.compiledFlag_[k.origin])
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
        const size_t oi = static_cast<size_t>(&op - out.outputs.data());
        // Never light with new cells or worldspaces: cells added by ESL-flagged plugins lose changes
        // across saves.
        const bool newWorld = std::any_of(op.records.begin(), op.records.end(), [&](uint32_t v) {
            return out.records[v].isNew &&
                   (out.records[v].sig == sig::CELL || out.records[v].sig == Signature("WRLD"));
        });
        const bool fits = op.newRecords <= kLightCapacity;
        // A new light output leaves room to grow (it stays light on rebuilds).
        const size_t room = std::min<size_t>(options.lightHeadroom, kLightCapacity);
        const bool fitsWithRoom = op.newRecords + room <= kLightCapacity;
        op.light = op.patch || (options.lightOutputsWhenPossible && !newWorld && fitsWithRoom);
        if (!op.patch && options.lightOutputsWhenPossible && !newWorld && fits && !fitsWithRoom &&
            !(oi < previousLight.size() && previousLight[oi] >= 0 && hadPrevious[oi]))
            addIssue(Severity::Info, "Light Outputs",
                     op.name + " is a full plugin: its " + std::to_string(op.newRecords) +
                         " new records fit a light plugin (" + std::to_string(kLightCapacity) +
                         "), but leave fewer than " + std::to_string(room) +
                         " free. Outputs keep their light or full state on rebuilds, so a light output this full "
                         "would have to become a full plugin, changing its records' FormIDs, when a mod update "
                         "adds a few records. More outputs spread the records thinner");
        // An output keeps the light or full state it had in the previous build: switching changes the
        // FormID the game gives every record in it.
        if (!op.patch && oi < previousLight.size() && previousLight[oi] >= 0 && hadPrevious[oi]) {
            const bool was = previousLight[oi] == 1;
            op.light = was && !newWorld && fits;
            if (op.light != was) {
                size_t n = 0;
                for (auto v : op.records)
                    if (previousOutput[v] == static_cast<int>(oi)) {
                        out.records[v].formIdChanged = true;
                        ++n;
                    }
                addIssue(Severity::Warning, "FormID Stability",
                         op.name + " was " + (was ? "light (ESL)" : "a full plugin") + " in the previous build and is " +
                             (op.light ? "light" : "a full plugin") + " now (" +
                             (newWorld ? "it holds new cells or worldspaces"
                                       : std::to_string(op.newRecords) + " new records, more than a light plugin holds") +
                             "): the game gives its " + std::to_string(n) +
                             " records from the previous build other FormIDs, so saves made with it lose them");
            } else if (op.light != (options.lightOutputsWhenPossible && !newWorld && fitsWithRoom)) {
                addIssue(Severity::Info, "FormID Stability",
                         op.name + " stays " + (op.light ? "light (ESL)" : "a full plugin") +
                             " as in the previous build, so its records keep their FormIDs (the Light outputs "
                             "setting applies to new outputs)");
            }
        }
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
            std::vector<FormKey> refs;
            for (const auto& [k, via] : collectRefs(e.fields))
                refs.push_back(k);
            if (auto it = configPrevisRecords.find(e.key); it != configPrevisRecords.end())
                refs.insert(refs.end(), it->second.begin(), it->second.end());
            for (const auto& k : refs) {
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
            // A Sim Settlements 2 add-on config added by a compiled plugin names that plugin in its
            // sAddonFilename; SS2 registers the pack only when GetFormFromFile(the config's own FormID,
            // that name) returns the config, which now lives in this output: name the output instead.
            if (e.isNew && out.isCompiled(e.key.origin)) {
                const std::string own = session.pluginName(e.key.origin);
                for (auto& sr : rec.subrecords) {
                    if (sr.signature != Signature("VMAD"))
                        continue;
                    if (auto nv = replaceScriptString(sr.data, "SimSettlementsV2:MiscObjects:AddonPackConfiguration",
                                                      "sAddonFilename", own, op.name)) {
                        sr.data = std::move(*nv);
                        addIssue(Severity::Info, "Script Property",
                                 session.formLabel(e.key) + ": the Sim Settlements 2 add-on config's sAddonFilename "
                                 "now names " + op.name + " instead of " + own +
                                     ", where the config is, so SS2 registers the pack",
                                 e.key, own);
                    }
                }
            }
            op.built.push_back(std::move(rec));
            op.placements.push_back(placement);
        }
        // TIFC, a topic's response count, is the number of responses written under it in this plugin (as
        // the Creation Kit and xEdit keep it), not the winning version's count. The writer sets the same
        // value; doing it here keeps the written-content check comparing like with like.
        {
            std::unordered_map<uint32_t, uint32_t> responses;
            for (const auto& pl : op.placements)
                if (pl.kind == Placement::Kind::TopicChild)
                    ++responses[pl.parent];
            for (auto& r : op.built) {
                if (r.signature() != Signature("DIAL"))
                    continue;
                auto it = responses.find(r.header.formId);
                auto* tifc = r.find(Signature("TIFC"));
                if (it == responses.end() || !tifc || tifc->data.size() != 4)
                    continue;
                std::memcpy(tifc->data.data(), &it->second, 4);
            }
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
                if (!options.translatePrevis) {
                    addIssue(Severity::Info, "Previs Numbering",
                             t.path + " (" + session.formLabel(e.key) + "): " + std::to_string(t.changed) + " of " +
                                 std::to_string(t.formIds) + " FormIDs read in " + t.numbering +
                                 "'s numbering would change in " + out.outputName(e.output) +
                                 "; not translated (Translate previs files is off), the original file is used",
                             e.key, t.numbering);
                    continue;
                }
                addIssue(Severity::Info, "Previs Numbering",
                         t.path + " (" + session.formLabel(e.key) + "): " + std::to_string(t.changed) + " of " +
                             std::to_string(t.formIds) + " FormIDs translated from " + t.numbering +
                             "'s numbering into " + out.outputName(e.output) +
                             "'s, which now holds the cell's winning record; the translated copy is written loose" +
                             (t.nowhere ? "; " + std::to_string(t.nowhere) +
                                              " of them point to no record today (e.g. " + join(t.nowhereExamples, ", ") +
                                              ") and were given IDs that point to no record in the output either, "
                                              "so the game finds nothing for them, as now"
                                        : std::string()),
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
            if (e.configOnly) {
                // Written only for the configuration merge: the next round leaves it out (as it is today).
                addIssue(Severity::Error, "Previs Numbering",
                         session.formLabel(e.key) + " (cell configuration merge) is written to " +
                             out.outputName(e.output) + ", so its previs file " + t.path +
                             " would be read in that plugin's numbering, but " + t.problem +
                             "; the merged cell is left out",
                         e.key, session.pluginName(e.winner));
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
        // A master that plugins staying in the load order still list: its name must stay loadable.
        const bool placeholder = session.masterPlaceholders.count(pi) != 0;
        if (archives.empty() && !placeholder)
            continue;
        if (options.archiveLoaders) {
            out.archiveLoaders.push_back(pi);
            if (placeholder)
                out.masterPlaceholders.push_back(pi);
        } else {
            out.archivesToRegister.insert(out.archivesToRegister.end(), archives.begin(), archives.end());
            if (placeholder) // eligibility allows this only where archive loaders are written
                addIssue(Severity::Error, "Master Placeholders",
                         p.name + " is compiled while plugins that stay in the load order list it as a master, but "
                                  "no placeholder plugin can be written here (archive loaders are off): they would not "
                                  "load",
                         FormKey{}, p.name);
        }
    }

    // Master Redirection: copies of the plugins that stay with their compiled masters replaced.
    buildRedirected(session, out);

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
