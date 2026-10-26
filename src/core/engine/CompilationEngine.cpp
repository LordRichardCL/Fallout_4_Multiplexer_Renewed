#include "engine/CompilationEngine.hpp"

#include "assets/AssetScanner.hpp"
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

int typeRank(Signature s)
{
    static const char* order[] = {"KYWD", "MESG", "FACT", "AMMO", "MISC", "OMOD", "WEAP", "ARMO", "INGR",
                                  "ALCH", "BOOK", "NOTE", "KEYM", "SPEL", "PERK", "LVLI", "FLST", "COBJ"};
    for (int i = 0; i < static_cast<int>(std::size(order)); ++i)
        if (s == Signature::fromString(order[i]))
            return i;
    return static_cast<int>(std::size(order));
}

} // namespace

CompiledOutput CompilationEngine::compile(Session& session, const CompileOptions& opts, ProgressSink& progress)
{
    CompiledOutput out;
    out.options = opts;
    CompileOptions& options = out.options;
    options.outputCount = std::clamp(options.outputCount, 1, 64);

    out.compiledFlag_.assign(session.plugins.size(), 0);
    for (auto pi : session.loadOrder) {
        if (session.plugins[pi].eligibility == Eligibility::Eligible) {
            out.compiledFlag_[pi] = 1;
            out.compiledPlugins.push_back(pi);
        } else {
            out.retainedPlugins.push_back(pi);
        }
    }
    auto addIssue = [&](Severity s, std::string cat, std::string msg, FormKey k = {}, std::string plugin = {}) {
        out.issues.push_back({s, std::move(cat), std::move(msg), k, std::move(plugin)});
    };
    if (out.compiledPlugins.empty()) {
        addIssue(Severity::Warning, "Compilation", "No plugins are eligible for compilation");
        return out;
    }

    // ---- Resolve: which records need to be materialised -------------------
    progress.stage("Resolve");
    struct Candidate {
        uint32_t group;
        bool isNew;
        bool retainedWinner;
        int forced;
    };
    std::vector<Candidate> candidates;
    for (uint32_t gi = 0; gi < session.groups.size(); ++gi) {
        auto inst = session.instances(session.groups[gi]);
        size_t inCompiled = 0;
        for (const auto& r : inst)
            inCompiled += out.compiledFlag_[r.plugin];
        if (!inCompiled)
            continue;
        out.sourceRecords += inCompiled;

        FormKey key = inst[0].key;
        bool originCompiled = key.valid() && out.compiledFlag_[key.origin];
        bool lastCompiled = out.compiledFlag_[inst.back().plugin];

        int forced = -1;
        if (auto it = options.forcedWinners.find(key.packed()); it != options.forcedWinners.end())
            for (size_t i = 0; i < inst.size(); ++i)
                if (inst[i].plugin == it->second)
                    forced = static_cast<int>(i);

        if (originCompiled) {
            for (const auto& r : inst)
                if (!out.compiledFlag_[r.plugin])
                    addIssue(Severity::Error, "Dependency",
                             session.pluginName(r.plugin) + " overrides " + session.formLabel(key) +
                                 " from a compiled plugin but stays in the load order",
                             key, session.pluginName(r.plugin));
        } else if (!lastCompiled && !options.mergeCompatible && forced < 0) {
            out.retainedWins++;
            continue;
        }
        candidates.push_back({gi, originCompiled, !lastCompiled, forced});
    }

    // ---- Merge ---------------------------------------------------------------
    progress.stage("Merge");
    std::vector<std::optional<EmittedRecord>> emitted(candidates.size());
    std::vector<std::vector<CompileIssue>> mergeIssues(candidates.size());
    std::atomic<size_t> done{0};
    parallelFor(
        candidates.size(),
        [&](size_t ci) {
            const auto& c = candidates[ci];
            auto inst = session.instances(session.groups[c.group]);
            try {
                Normalizer norm(session);
                std::vector<FieldSet> chain;
                std::vector<DecodeIssue> issues;
                for (const auto& r : inst) {
                    auto nr = norm.normalize(r);
                    if (out.compiledFlag_[r.plugin])
                        issues.insert(issues.end(), nr.issues.begin(), nr.issues.end());
                    chain.push_back(splitFields(nr, session.defs));
                }
                MergeOptions mo;
                mo.mergeCompatible = options.mergeCompatible;
                mo.forcedWinner = c.forced;
                MergeResult mr = MergeEngine::merge(chain, mo);

                // Once compiled plugins are disabled, the last retained instance is what the game
                // loads. If the result equals it, an override would only be an ITM.
                for (size_t i = inst.size(); i-- > 0;) {
                    if (out.compiledFlag_[inst[i].plugin])
                        continue;
                    if (sameRecord(mr.fields, chain[i])) {
                        mergeIssues[ci].push_back({Severity::Info, "Identical", "", inst[0].key, {}});
                        return;
                    }
                    break;
                }
                if (c.isNew && (mr.fields.flags() & RecordFlags::Deleted)) {
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
                e.merged = mr.merged;
                e.winner = inst.back().plugin;
                for (const auto& r : inst)
                    e.chain.push_back(r.plugin);
                for (size_t idx : mr.contributors)
                    e.contributors.push_back(inst[idx].plugin);
                e.fields = std::move(mr.fields);
                e.issues = std::move(issues);
                e.size = estimateSize(e.fields);
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

    // ---- Rewrite (dependency extraction) ------------------------------------------
    progress.stage("Rewrite");
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
    const int N = options.outputCount;
    out.outputs.resize(static_cast<size_t>(N));
    for (int o = 0; o < N; ++o) {
        out.outputs[static_cast<size_t>(o)].index = o;
        out.outputs[static_cast<size_t>(o)].name = outputFileName(options, o);
    }

    auto sccs = stronglyConnected(out.records);
    std::vector<uint32_t> sccOf(out.records.size());
    for (uint32_t s = 0; s < sccs.size(); ++s)
        for (auto v : sccs[s])
            sccOf[v] = s;

    uint64_t totalSize = 0;
    for (const auto& e : out.records)
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
            bucketSize[typeRank(e.sig)] += e.size;
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
                int r = typeRank(out.records[v].sig);
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
            out.records[v].output = o;
    }

    for (uint32_t v = 0; v < out.records.size(); ++v) {
        auto& e = out.records[v];
        auto& op = out.outputs[static_cast<size_t>(e.output)];
        op.records.push_back(v);
        op.bytes += e.size;
        if (e.isNew)
            op.newRecords++;
        else
            op.overrides++;
        if (previousOutput[v] >= 0 && previousOutput[v] != e.output) {
            e.formIdChanged = true;
            addIssue(Severity::Warning, "FormID Stability",
                     session.formLabel(e.key) + " moved from " + out.outputName(previousOutput[v]) + " to " +
                         out.outputName(e.output) + " because of new dependencies; saves referencing it may break",
                     e.key);
        }
    }

    // ---- FormIDs --------------------------------------------------------------
    for (auto& op : out.outputs) {
        op.light = options.lightOutputsWhenPossible && op.newRecords <= (kLightPluginMaxObjectId - kFirstObjectId + 1);
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
                addIssue(Severity::Warning, "FormID Stability",
                         session.formLabel(e.key) + " received a new FormID in " + op.name, e.key);
            }
            e.objectId = next;
            used.insert(next++);
        }
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
            for (const auto& [k, via] : collectRefs(e.fields, options.rewriteHeuristicReferences)) {
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

    // ---- Materialise records -------------------------------------------------
    for (auto& op : out.outputs) {
        op.header = PluginHeader{};
        op.header.record.signature = sig::TES4;
        op.header.record.flags = op.light ? RecordFlags::Light : 0;
        op.header.record.formVersion = 131;
        op.header.version = 1.0f;
        op.header.author = options.author;
        op.header.description = "Compiled game state " + std::to_string(op.index + 1) + "/" + std::to_string(N) +
                                " - generated by Fallout 4 Multiplexer Renewed from " +
                                std::to_string(out.compiledPlugins.size()) + " plugins";
        for (const auto& m : op.masterNames)
            op.header.masters.push_back({m, 0});

        op.built.clear();
        op.built.reserve(op.records.size());
        for (auto v : op.records) {
            auto& e = out.records[v];
            bool ok = true;
            Record rec;
            rec.header.signature = e.sig;
            rec.header.flags = e.fields.flags() & ~RecordFlags::Compressed;
            rec.header.formVersion = e.fields.formVersion ? e.fields.formVersion : 131;
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
                if (options.rewriteHeuristicReferences)
                    for (const auto& [off, k] : ns.softRefs) {
                        bool refOk = true;
                        uint32_t raw = out.translate(session, k, op.index, &refOk);
                        if (refOk)
                            storeLE<uint32_t>(sr.data.data() + off, raw);
                    }
                rec.subrecords.push_back(std::move(sr));
            }
            op.built.push_back(std::move(rec));
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
        for (const auto& a : AssetScanner::pluginArchiveNames(p, lang))
            if (session.vfs.find(toLower(a)))
                out.archivesToRegister.push_back(a);
    }

    size_t used = std::count_if(out.outputs.begin(), out.outputs.end(),
                                [](const OutputPlugin& o) { return !o.records.empty(); });
    spdlog::info("Compiled {} records ({} new, {} overrides, {} merged) from {} plugins into {} of {} output plugins",
                 out.records.size(), out.mappings.size(), out.records.size() - out.mappings.size(), out.mergedRecords,
                 out.compiledPlugins.size(), used, N);
    return out;
}

} // namespace fo4mx
