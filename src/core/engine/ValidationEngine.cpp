#include "engine/ValidationEngine.hpp"

#include "esp/PluginFile.hpp"
#include "util/Text.hpp"

#include <map>
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
    issues.push_back({s, std::move(category), std::move(message), std::move(plugin), std::move(formId)});
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
                        stat.second++;
                        if (!k.valid()) {
                            if (k.objectId)
                                v.fail(Severity::Error, "Missing References",
                                       label + " " + s.sig.str() + " has an unresolvable FormID", op.name, fid);
                            else
                                v.pass();
                            continue;
                        }
                        bool exists = session.findGroup(k) != nullptr;
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
                            if (c.isCompiled(k.origin))
                                v.fail(Severity::Error, "Missing References",
                                       label + " " + s.sig.str() + " +" + std::to_string(off) +
                                           " probably refers to compiled record " + session.formLabel(k) +
                                           " in a layout this profile does not describe",
                                       op.name, fid);
                            else
                                v.pass();
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

} // namespace fo4mx
