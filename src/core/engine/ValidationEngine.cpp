#include "engine/ValidationEngine.hpp"

#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"
#include "esp/PluginFile.hpp"
#include "util/Text.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <set>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

void ValidationReport::fail(Severity s, std::string category, std::string message, std::string plugin,
                            std::string formId)
{
    checks++;
    if (s == Severity::Error)
        errors++;
    else if (s == Severity::Warning)
        warnings++;
    else
        passed++; // informational findings do not fail a check
    issues.push_back({s, std::move(category), std::move(message), std::move(plugin), std::move(formId), context});
}

void ValidationReport::merge(const ValidationReport& o)
{
    checks += o.checks;
    passed += o.passed;
    warnings += o.warnings;
    errors += o.errors;
    issues.insert(issues.end(), o.issues.begin(), o.issues.end());
}

ValidationReport ValidationEngine::validate(const Session& session, const CompiledOutput& c, ProgressSink& progress)
{
    progress.stage("Validate");
    ValidationReport v;

    for (const auto& is : c.issues) {
        std::string fid = is.form.valid() ? session.loadOrderFormId(is.form) : std::string();
        v.fail(is.severity, is.category, is.message, is.plugin, fid);
    }

    // Every record of every compiled plugin must be accounted for.
    std::set<FormKey> emittedKeys;
    for (const auto& e : c.records)
        emittedKeys.insert(e.key);
    for (const auto& g : session.groups) {
        auto inst = session.instances(g);
        const auto& first = inst[0];
        if (!first.key.valid() || !c.isCompiled(first.key.origin))
            continue;
        if (emittedKeys.count(first.key) || (session.records[g.end - 1].flags & RecordFlags::Deleted))
            v.pass();
        else
            v.fail(Severity::Error, "Orphaned Records",
                   session.formLabel(first.key) + " from a compiled plugin is missing from the output",
                   session.pluginName(first.key.origin), session.loadOrderFormId(first.key));
    }

    // Output plugins: masters and their order.
    std::set<std::string> loadedNames;
    for (auto pi : session.loadOrder)
        loadedNames.insert(session.plugins[pi].key);
    for (const auto& op : c.outputs) {
        if (op.records.empty())
            continue;
        if (op.masterNames.size() <= 254)
            v.pass();
        else
            v.fail(Severity::Error, "Missing Masters", op.name + " has more than 254 masters", op.name);
        for (auto pi : op.retainedMasters) {
            if (c.isCompiled(pi))
                v.fail(Severity::Error, "Missing Masters",
                       op.name + " depends on " + session.pluginName(pi) + ", which is disabled after compilation",
                       op.name);
            else if (!loadedNames.count(session.plugins[pi].key))
                v.fail(Severity::Error, "Missing Masters", op.name + " depends on inactive " + session.pluginName(pi),
                       op.name);
            else
                v.pass();
        }
        for (int o : op.outputMasters) {
            if (o < op.index && !c.outputs[static_cast<size_t>(o)].records.empty())
                v.pass();
            else
                v.fail(Severity::Error, "Circular Dependencies",
                       op.name + " requires " + c.outputName(o) + ", which does not load before it", op.name);
        }
        if (op.light) {
            for (auto r : op.records) {
                const auto& e = c.records[r];
                if (!e.isNew)
                    continue;
                if (e.objectId >= kFirstObjectId && e.objectId <= kLightPluginMaxObjectId)
                    v.pass();
                else
                    v.fail(Severity::Error, "Broken FormIDs", "Light plugin FormID out of range in " + op.name,
                           op.name, formatFormId(e.rawFormId));
            }
        }
    }

    // Records: identity, references.
    std::set<std::pair<int, uint32_t>> seenNew;
    std::set<FormKey> seenOverride;
    std::map<std::pair<Signature, Signature>, std::pair<uint64_t, uint64_t>> slotStats; // resolved / total
    size_t n = 0;
    for (const auto& op : c.outputs) {
        for (size_t ri = 0; ri < op.records.size(); ++ri) {
            const auto& e = c.records[op.records[ri]];
            v.context = e.key;
            const std::string fid = formatFormId(e.rawFormId);
            const std::string label = session.formLabel(e.key);
            if ((++n & 0xFFF) == 0)
                progress.progress(n, c.records.size(), "validating records");

            if (e.isNew) {
                if (seenNew.insert({op.index, e.objectId}).second)
                    v.pass();
                else
                    v.fail(Severity::Error, "Duplicate IDs", "Duplicate FormID " + fid + " in " + op.name, op.name, fid);
            } else {
                if (seenOverride.insert(e.key).second)
                    v.pass();
                else
                    v.fail(Severity::Error, "Duplicate IDs", label + " is overridden by more than one output plugin",
                           op.name, fid);
            }
            uint32_t selfIndex = static_cast<uint32_t>(op.masterNames.size());
            if (masterIndexOf(e.rawFormId) <= selfIndex && (e.isNew == (masterIndexOf(e.rawFormId) == selfIndex)))
                v.pass();
            else
                v.fail(Severity::Error, "Broken FormIDs", label + " has an invalid FormID " + fid, op.name, fid);

            for (const auto& is : e.issues)
                v.fail(is.severity == Severity::Error ? Severity::Error : is.severity, "Record Decoding",
                       label + " " + is.sub.str() + ": " + is.message, op.name, fid);

            for (const auto& f : e.fields.fields) {
                auto checkSub = [&](const NormSub& s) {
                    for (const auto& [off, k] : s.refs) {
                        auto& stat = slotStats[{e.sig, s.sig}];
                        if (k.valid() || k.objectId)
                            stat.second++; // empty (null) slots say nothing about the layout
                        if (!k.valid()) {
                            if (k.objectId)
                                v.fail(Severity::Error, "Missing References",
                                       label + " " + s.sig.str() + " has an unresolvable FormID", op.name, fid);
                            else
                                v.pass();
                            continue;
                        }
                        bool exists = session.exists(k);
                        if (exists)
                            stat.first++;
                        bool ok = true;
                        uint32_t raw = c.translate(session, k, op.index, &ok);
                        if (!ok)
                            v.fail(Severity::Error, "Missing References",
                                   label + " " + s.sig.str() + " cannot reach " + session.formLabel(k), op.name, fid);
                        else if (masterIndexOf(raw) > selfIndex)
                            v.fail(Severity::Error, "Broken FormIDs", label + " writes an out-of-range FormID",
                                   op.name, fid);
                        else if (!exists && !c.isCompiled(k.origin))
                            v.fail(Severity::Warning, "Missing References",
                                   label + " " + s.sig.str() + " references " + formatFormId(k.objectId) + " in " +
                                       session.pluginName(k.origin) + ", which does not exist (also broken in the source)",
                                   op.name, fid);
                        else
                            v.pass();
                    }
                    if (!c.options.rewriteHeuristicReferences)
                        for (const auto& [off, k] : s.softRefs) {
                            if (c.isCompiled(k.origin)) {
                                v.fail(Severity::Error, "Missing References",
                                       label + " " + s.sig.str() + " +" + std::to_string(off) +
                                           " probably refers to compiled record " + session.formLabel(k) +
                                           " in a layout this profile does not describe",
                                       op.name, fid);
                                continue;
                            }
                            // Undescribed bytes are copied as they are, so the master index they
                            // carry must name the same plugin in the output's master list.
                            uint32_t raw = off + 4 <= s.bytes.size() ? loadLE<uint32_t>(s.bytes.data() + off) : 0;
                            uint32_t mi = masterIndexOf(raw);
                            if (mi < op.retainedMasters.size() && op.retainedMasters[mi] == k.origin)
                                v.pass();
                            else
                                v.fail(Severity::Error, "Missing References",
                                       label + " " + s.sig.str() + " +" + std::to_string(off) + " probably refers to " +
                                           session.formLabel(k) + " in a layout this profile does not describe; in " +
                                           op.name + " its master index would point elsewhere",
                                       op.name, fid);
                        }
                };
                for (const auto& s : f.subs)
                    checkSub(s);
                for (const auto& item : f.items)
                    for (const auto& s : item.subs)
                        checkSub(s);
            }
        }
    }

    v.context = FormKey{};

    // Definition self-check: a described FormID slot that rarely resolves is suspect.
    for (const auto& [key, stat] : slotStats) {
        if (stat.second < 20)
            continue;
        double rate = static_cast<double>(stat.first) / static_cast<double>(stat.second);
        if (rate < 0.8)
            v.fail(Severity::Warning, "Definitions",
                   "Only " + std::to_string(static_cast<int>(rate * 100)) + "% of FormIDs in " + key.first.str() + " " +
                       key.second.str() + " resolve; the field layout for this runtime may be wrong");
        else
            v.pass();
    }

    // Orphan info: new keywords / form lists nothing references.
    std::unordered_set<uint32_t> referenced;
    for (const auto& e : c.records)
        for (auto d : e.deps)
            referenced.insert(d);
    for (uint32_t i = 0; i < c.records.size(); ++i) {
        const auto& e = c.records[i];
        if (e.isNew && (e.sig == sig::KYWD || e.sig == sig::FLST) && !referenced.count(i))
            v.fail(Severity::Info, "Orphaned Records",
                   session.formLabel(e.key) + " is not referenced by any compiled record (it may be used by scripts)",
                   c.outputName(e.output), formatFormId(e.rawFormId));
    }

    spdlog::info("Validation: {} checks, {} passed, {} warnings, {} errors", v.checks, v.passed, v.warnings, v.errors);
    return v;
}

ValidationReport ValidationEngine::validateWrittenFiles(const CompiledOutput& c,
                                                        const std::vector<std::filesystem::path>& files)
{
    ValidationReport v;
    for (const auto& path : files) {
        std::string name = pathToUtf8(path.filename());
        const OutputPlugin* plan = nullptr;
        for (const auto& op : c.outputs)
            if (iequals(op.name, name))
                plan = &op;
        if (!plan) {
            v.fail(Severity::Warning, "Export", "Unexpected file " + name);
            continue;
        }
        try {
            auto file = PluginFile::load(path);
            const auto& h = file.header();
            std::vector<std::string> masters;
            for (const auto& m : h.masters)
                masters.push_back(m.name);
            if (masters == plan->masterNames)
                v.pass();
            else
                v.fail(Severity::Error, "Missing Masters", name + ": master list on disk differs from the plan", name);

            std::set<uint32_t> expected;
            for (const auto& r : plan->built)
                expected.insert(r.header.formId);
            size_t count = 0;
            file.forEachRecord([&](const RecordLocation& loc) {
                ++count;
                try {
                    Record::parse(loc.header, loc.rawData);
                    if (expected.count(loc.header.formId))
                        v.pass();
                    else
                        v.fail(Severity::Error, "Broken FormIDs",
                               name + ": unexpected record " + formatFormId(loc.header.formId), name);
                } catch (const std::exception& e) {
                    v.fail(Severity::Error, "Export", name + ": record " + formatFormId(loc.header.formId) +
                                                          " does not parse: " + e.what(),
                           name);
                }
                return true;
            });
            if (count == plan->built.size())
                v.pass();
            else
                v.fail(Severity::Error, "Export",
                       name + ": wrote " + std::to_string(count) + " records, expected " + std::to_string(plan->built.size()),
                       name);
        } catch (const std::exception& e) {
            v.fail(Severity::Error, "Export", name + " cannot be read back: " + e.what(), name);
        }
    }
    return v;
}

DefinitionCheck ValidationEngine::checkDefinitions(Session& session, const std::vector<Signature>& types,
                                                   ProgressSink& progress, unsigned threads)
{
    progress.stage("Check definitions");
    DefinitionCheck out;
    auto wanted = [&](Signature sig) {
        if (!types.empty())
            return std::find(types.begin(), types.end(), sig) != types.end();
        return session.defs.isCompilable(sig);
    };
    std::vector<const FormGroup*> groups;
    for (const auto& g : session.groups)
        if (!session.instances(g).empty() && wanted(session.instances(g)[0].signature))
            groups.push_back(&g);

    struct Key {
        Signature record, sub;
        bool operator<(const Key& o) const { return record < o.record || (record == o.record && sub < o.sub); }
    };
    std::map<Key, DefinitionStat> stats;
    std::mutex mutex;
    std::atomic<size_t> done{0};
    parallelFor(
        groups.size(),
        [&](size_t gi) {
            std::map<Key, DefinitionStat> local;
            std::map<Signature, uint64_t> records;
            Normalizer norm(session);
            for (const auto& inst : session.instances(*groups[gi])) {
                if (!session.plugins[inst.plugin].active)
                    continue;
                NormRecord nr;
                try {
                    nr = norm.normalize(inst);
                } catch (const std::exception&) {
                    continue;
                }
                records[nr.sig]++;
                std::string where = session.formLabel(nr.key) + " in " + session.pluginName(inst.plugin);
                for (const auto& sub : nr.subs) {
                    auto& st = local[{nr.sig, sub.sig}];
                    st.record = nr.sig;
                    st.sub = sub.sig;
                    st.described = session.defs.field(nr.sig, sub.sig).kind != FieldKind::Unknown;
                    st.occurrences++;
                    for (const auto& [off, k] : sub.refs) {
                        if (!k.valid() && k.objectId == 0)
                            continue; // null
                        st.formIds++;
                        if (session.exists(k)) {
                            st.resolved++;
                        } else {
                            st.unresolvedOffsets[off]++;
                            if (st.example.empty())
                                st.example = where;
                        }
                    }
                    if (!sub.softRefs.empty()) {
                        st.softRefs += sub.softRefs.size();
                        for (const auto& [off, k] : sub.softRefs)
                            st.softOffsets[off]++;
                        if (st.example.empty())
                            st.example = where;
                    }
                }
                for (const auto& is : nr.issues) {
                    auto& st = local[{nr.sig, is.sub}];
                    st.record = nr.sig;
                    st.sub = is.sub;
                    if (is.message.find("size") != std::string::npos || is.message.find("expected") != std::string::npos) {
                        st.sizeIssues++;
                        if (st.example.empty())
                            st.example = where + ": " + is.message;
                    }
                }
            }
            std::lock_guard lock(mutex);
            for (auto& [k, v] : local) {
                auto& t = stats[k];
                if (t.occurrences == 0 && t.sizeIssues == 0) {
                    t = v;
                    continue;
                }
                t.described |= v.described;
                t.occurrences += v.occurrences;
                t.sizeIssues += v.sizeIssues;
                t.formIds += v.formIds;
                t.resolved += v.resolved;
                t.softRefs += v.softRefs;
                for (auto [o, n] : v.softOffsets)
                    t.softOffsets[o] += n;
                for (auto [o, n] : v.unresolvedOffsets)
                    t.unresolvedOffsets[o] += n;
                if (t.example.empty())
                    t.example = v.example;
            }
            for (auto [sig, n] : records)
                out.recordsByType[sig] += n;
            size_t n = ++done;
            if ((n & 0x3FF) == 0 || n == groups.size())
                progress.progress(n, groups.size(), "decoding records");
        },
        progress, threads);

    auto& v = out.report;
    auto pct = [](uint64_t a, uint64_t b) { return b ? std::to_string((a * 1000 / b) / 10) + "." + std::to_string((a * 1000 / b) % 10) + "%" : std::string("-"); };
    // "at +92 (900), +100 (8)": the offsets with the most hits, so a missing or wrong field can be located.
    auto topOffsets = [](const std::map<uint32_t, uint64_t>& m) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto [o, n] : m)
            v.emplace_back(n, o);
        std::sort(v.rbegin(), v.rend());
        std::string out;
        for (size_t i = 0; i < v.size() && i < 6; ++i)
            out += (i ? ", +" : "at +") + std::to_string(v[i].second) + " (" + std::to_string(v[i].first) + ")";
        if (v.size() > 6)
            out += ", ...";
        return out;
    };
    std::map<Signature, std::pair<uint64_t, uint64_t>> perType; // resolved / FormIDs
    for (auto& [k, st] : stats) {
        out.stats.push_back(st);
        perType[st.record].first += st.resolved;
        perType[st.record].second += st.formIds;
        std::string label = st.record.str() + " " + st.sub.str();
        bool flagged = false;
        // A described FormID slot that rarely names a real record means the offsets are wrong.
        if (st.formIds >= 10 && st.resolved * 10 < st.formIds * 9) {
            v.fail(Severity::Warning, "Definitions",
                   label + ": only " + pct(st.resolved, st.formIds) + " of " + std::to_string(st.formIds) +
                       " FormIDs at the described offsets name an existing record; the layout is probably wrong; "
                       "failing " + topOffsets(st.unresolvedOffsets) + " (e.g. " + st.example + ")");
            flagged = true;
        }
        if (st.sizeIssues) {
            v.fail(Severity::Warning, "Definitions",
                   label + ": unexpected size in " + std::to_string(st.sizeIssues) + " of " +
                       std::to_string(st.occurrences) + " records; these are scanned instead of decoded (e.g. " +
                       st.example + ")");
            flagged = true;
        }
        // Undescribed data that keeps looking like FormIDs is a field the definitions should cover.
        if (st.softRefs && st.softRefs * 4 >= st.occurrences) {
            v.fail(st.described ? Severity::Warning : Severity::Info, "Definitions",
                   label + (st.described ? " (partly described)" : " (not described)") + ": " +
                       std::to_string(st.softRefs) + " values in " + std::to_string(st.occurrences) +
                       " records look like FormIDs " + topOffsets(st.softOffsets) +
                       "; compiled records are protected by validation, but this field should be described (e.g. " +
                       st.example + ")");
            flagged = true;
        }
        if (!flagged)
            v.pass();
    }
    std::sort(out.stats.begin(), out.stats.end(), [](const DefinitionStat& a, const DefinitionStat& b) {
        return a.record.str() != b.record.str() ? a.record.str() < b.record.str() : a.sub.str() < b.sub.str();
    });
    for (const auto& [sig, n] : out.recordsByType) {
        auto [res, total] = perType[sig];
        v.fail(Severity::Info, "Definitions",
               sig.str() + " (" + session.defs.recordName(sig) + "): " + std::to_string(n) + " records decoded, " +
                   (total ? std::to_string(total) + " FormIDs, " + pct(res, total) + " resolve"
                          : std::string("no FormIDs to check")));
    }
    spdlog::info("Definition check: {} record types, {} subrecord layouts, {} warnings", out.recordsByType.size(),
                 out.stats.size(), v.warnings);
    return out;
}

std::vector<ArchivePriorityChange> ValidationEngine::checkArchivePriority(const Session& session,
                                                                         const CompiledOutput& compiled)
{
    const auto& idx = session.assets;
    // Load sequence after switching over: INI archives as today, then compiled plugins' archives
    // (appended to the INI list in load order), then the archives of plugins that stay.
    std::vector<uint64_t> rank(idx.archives.size());
    for (size_t a = 0; a < idx.archives.size(); ++a) {
        const auto& ar = idx.archives[a];
        bool movesToIni = ar.owner != kInvalidPlugin && compiled.isCompiled(ar.owner) && !compiled.hasArchiveLoader(ar.owner);
        uint64_t tier = ar.owner == kInvalidPlugin ? 0 : movesToIni ? 1 : 2;
        rank[a] = (tier << 32) | a;
    }
    std::vector<ArchivePriorityChange> out;
    for (const auto& e : idx.entries) {
        if (e.sources.size() < 2)
            continue;
        const AssetSource& before = e.winner();
        if (before.archive == kLooseFile)
            continue; // a loose file wins either way
        const AssetSource* after = nullptr;
        for (const auto& s : e.sources)
            if (s.archive != kLooseFile && (!after || rank[s.archive] > rank[after->archive]))
                after = &s;
        if (!after || after->archive == before.archive)
            continue;
        out.push_back({idx.path(e), idx.archives[before.archive].name, idx.modName(before),
                       idx.archives[after->archive].name, idx.modName(*after)});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return out;
}

} // namespace fo4mx
