#include "engine/DependencyEngine.hpp"

#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"

#include "engine/BuildCache.hpp"
#include "engine/Diagnosis.hpp"
#include "engine/FieldModel.hpp"
#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"
#include "engine/ReferenceScanner.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <atomic>
#include <set>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

std::vector<std::string> AnalysisOptions::selectionKey() const
{
    std::vector<std::string> key = excludedPlugins;
    if (onlySelected) {
        key.push_back("only:");
        for (const auto& p : selectedPlugins)
            key.push_back("only:" + p);
    }
    return key;
}

std::vector<PluginIndex> DependencyEngine::dependents(const Session& session, PluginIndex plugin)
{
    std::vector<PluginIndex> out;
    for (const auto& p : session.plugins)
        if (std::find(p.masters.begin(), p.masters.end(), plugin) != p.masters.end())
            out.push_back(p.index);
    return out;
}

std::vector<uint8_t> DependencyEngine::mastersInUse(const Session& session, const PluginInfo& p)
{
    const size_t n = p.header.masters.size();
    std::vector<uint8_t> used(n, 0);
    size_t left = n;
    auto mark = [&](uint32_t index) {
        if (index < n && !used[index]) {
            used[index] = 1;
            --left;
        }
    };
    try {
        const auto file = PluginFile::load(p.path);
        file.forEachRecord([&](const RecordLocation& loc) {
            if (loc.header.signature == sig::TES4)
                return true;
            // The record itself, and the records whose groups hold it.
            for (uint32_t id : {loc.header.formId, loc.world, loc.cell, loc.topic, loc.quest})
                if (id)
                    mark(id >> 24);
            std::vector<uint8_t> inflated;
            std::span<const uint8_t> data = loc.rawData;
            if (loc.header.compressed()) {
                inflated = Record::decompress(loc.header, loc.rawData);
                data = inflated;
            }
            const auto subs = Record::parseSubrecords(data);
            const auto layout = decodeLayout(session.defs, loc.header.signature, subs);
            for (const auto* slots : {&layout.formIdSlots, &layout.probableSlots})
                for (const auto& sl : *slots) {
                    if (sl.sub >= subs.size() || sl.offset + 4 > subs[sl.sub].data.size())
                        continue;
                    uint32_t v = 0;
                    std::memcpy(&v, subs[sl.sub].data.data() + sl.offset, 4);
                    if (v)
                        mark(v >> 24);
                }
            // Undescribed data: any byte equal to a master's index could be the top byte of a FormID
            // naming it, so that master counts as used.
            std::set<uint32_t> loose;
            for (const auto& h : layout.heuristic)
                loose.insert(h.sub);
            for (uint32_t i : loose)
                if (i < subs.size())
                    for (uint8_t b : subs[i].data)
                        mark(b);
            return left > 0;
        });
    } catch (const std::exception& e) {
        spdlog::debug("Masters of {} taken as used: {}", p.name, e.what());
        std::fill(used.begin(), used.end(), 1);
    }
    return used;
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
            auto entry = session.defs.catalogueEntry(s);
            t.known = entry != nullptr;
            t.category = entry ? entry->category : std::string("Unknown");
            t.support = session.defs.supportSummary(s);
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

namespace {
// A navmesh whose data does not decode would have its FormIDs rewritten by guesswork, and the navmesh
// info map is combined from every compiled plugin's entries: such a plugin stays in the load order.
void findNavmeshFaults(Session& session)
{
    if (session.navmeshFaults)
        return;
    session.navmeshFaults.emplace();
    const Signature navm("NAVM"), navi("NAVI");
    if (!session.defs.isCompilable(navm) && !session.defs.isCompilable(navi))
        return;
    for (const auto& r : session.records) {
        if ((r.signature != navm && r.signature != navi) || !session.plugins[r.plugin].active ||
            session.navmeshFaults->count(r.plugin))
            continue;
        try {
            const Record rec = session.loadRecord(r);
            for (const auto& is : decodeLayout(session.defs, r.signature, rec.subrecords).issues)
                if (is.severity != Severity::Info) {
                    session.navmeshFaults->emplace(r.plugin, session.formLabel(r.key) + " " + is.sub.str() + ": " +
                                                                 is.message);
                    break;
                }
        } catch (const std::exception& e) {
            session.navmeshFaults->emplace(r.plugin, session.formLabel(r.key) + ": " + e.what());
        }
    }
}
} // namespace

void DependencyEngine::evaluateEligibility(Session& session, const AnalysisOptions& options, DependencyReport* report)
{
    findNavmeshFaults(session);
    std::vector<IntrinsicBlock> blocks(session.plugins.size());
    std::unordered_set<std::string> excluded = session.userRetained;
    for (const auto& e : options.excludedPlugins)
        excluded.insert(toLower(e));

    // New records of types that are compiled only as overrides (per plugin, per type).
    std::map<PluginIndex, std::map<Signature, size_t>> newOverrideOnly;
    {
        std::unordered_set<Signature> scoped;
        for (Signature sig : session.defs.compilableTypes())
            if (session.defs.overridesOnly(sig))
                scoped.insert(sig);
        if (!scoped.empty())
            for (const auto& r : session.records)
                if (r.plugin == r.key.origin && scoped.count(r.signature))
                    ++newOverrideOnly[r.plugin][r.signature];
    }

    // Places that name plugins by file name, grouped per plugin. Their classes decide, never their number.
    std::map<PluginIndex, std::vector<const PluginReference*>> nameRefs;
    for (const auto& r : session.nameReferences)
        nameRefs[r.plugin].push_back(&r);
    session.referenceDecisions.clear();
    for (const auto& [plugin, refs] : nameRefs) {
        auto& d = session.referenceDecisions[plugin] = ReferenceScanner::decide(refs);
        d.reason = "not evaluated: the plugin is not a compilation candidate";
        d.action = "None";
    }

    for (auto& p : session.plugins) {
        p.reasons.clear();
        if (!p.active) {
            p.eligibility = Eligibility::Inactive;
            p.reasons.push_back(p.generated ? "Written by Fallout 4 Multiplexer Renewed (an output, never a source)"
                                            : "Not enabled in the load order");
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
        if (auto it = session.autoRetained.find(p.key); it != session.autoRetained.end()) {
            p.eligibility = Eligibility::Retained;
            p.reasons.push_back("Kept out automatically: " + it->second);
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
        std::vector<std::string> unsupported, unknown, newOfOverrideTypes;
        if (auto it = newOverrideOnly.find(p.index); it != newOverrideOnly.end())
            for (const auto& [s, n] : it->second) {
                newOfOverrideTypes.push_back(s.str() + " (" + std::to_string(n) + ")");
                block.types.push_back(s);
            }
        if (!newOfOverrideTypes.empty()) {
            p.eligibility = Eligibility::Ineligible;
            std::vector<std::string> why;
            for (Signature t : block.types)
                if (t == Signature("NPC_"))
                    why.push_back("new NPCs: FaceGen data is stored under the plugin's name");
            p.reasons.push_back("Adds new records of types compiled only as overrides: " + join(newOfOverrideTypes, ", ") +
                                ". New records of these types are tied to their plugin" +
                                (why.empty() ? std::string() : " (" + join(why, "; ") + ")"));
        }
        for (const auto& [s, n] : p.typeCounts)
            if (!session.defs.isCompilable(s)) {
                (session.defs.catalogueEntry(s) ? unsupported : unknown).push_back(s.str() + " (" + std::to_string(n) + ")");
                block.types.push_back(s);
            }
        if (!unknown.empty()) {
            p.eligibility = Eligibility::Ineligible;
            p.reasons.push_back("Contains record types that are not Fallout 4 record types: " + join(unknown, ", ") +
                                ". The plugin may be for another game, or damaged");
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
        if (auto it = session.navmeshFaults->find(p.index); it != session.navmeshFaults->end()) {
            p.eligibility = Eligibility::Ineligible;
            block.otherReason = true;
            p.reasons.push_back("Navmesh data that does not decode (" + it->second +
                                "): its FormIDs cannot be rewritten with certainty, and the navmesh info map "
                                "combines every compiled plugin's entries");
        }
        if (auto it = nameRefs.find(p.index); it != nameRefs.end()) {
            auto& d = session.referenceDecisions[p.index];
            if (d.cls == ReferenceClass::Safe) {
                d.reason = "none of them depends on the plugin's file name";
                d.action = "None: compiled like any other plugin";
            } else if (d.cls == ReferenceClass::Rewritable && session.autoMergeEnabled) {
                d.reason = "every reference that depends on the file name can be rewritten exactly and checked";
                d.action = "None: the Advanced Users Auto-Merger writes rewritten copies into the output folder "
                           "(Multiplexer_Relinked.tsv lists every change)";
            } else if (d.cls == ReferenceClass::Rewritable) {
                d.reason = "the references can be rewritten, but the Advanced Users Auto-Merger is off";
                d.action = "Turn on the Advanced Users Auto-Merger to compile it, or leave it in the load order";
            } else {
                // The references that decide.
                std::vector<const PluginReference*> deciding;
                for (const auto* r : it->second)
                    if (r->cls == d.cls)
                        deciding.push_back(r);
                std::vector<std::string> examples;
                for (size_t i = 0; i < deciding.size() && i < 3; ++i)
                    examples.push_back(deciding[i]->assetPath + " (" + deciding[i]->context + ": " +
                                       deciding[i]->reason + ")");
                if (deciding.size() > 3)
                    examples.push_back("+" + std::to_string(deciding.size() - 3) + " more");
                d.reason = std::string(referenceClassName(d.cls)) + ": " + join(examples, "; ");
                d.action = d.cls == ReferenceClass::CompatibilityRequired
                               ? "Leave it in the load order: moving plugin-named files is not implemented"
                           : d.cls == ReferenceClass::Unsafe
                               ? "Leave it in the load order: the reference cannot be preserved"
                               : "Leave it in the load order; check what uses the listed files";
            }
            p.reasons.push_back(d.summary + ". " + d.reason);
            if (d.cls != ReferenceClass::Safe && !(d.cls == ReferenceClass::Rewritable && session.autoMergeEnabled)) {
                block.otherReason = true;
                p.eligibility = Eligibility::Ineligible;
                if (d.cls == ReferenceClass::Rewritable)
                    p.reasons.back() += ": turn on the Advanced Users Auto-Merger to compile it";
            }
        }
    }

    // Opt-in: a plugin that could compile but was not chosen stays in the load order (its own checks
    // above still decide what the list shows for the ones that cannot).
    if (options.onlySelected) {
        std::unordered_set<std::string> chosen;
        for (const auto& n : options.selectedPlugins)
            chosen.insert(toLower(n));
        for (auto& p : session.plugins)
            if (p.eligibility == Eligibility::Eligible && !chosen.count(p.key)) {
                p.eligibility = Eligibility::Retained;
                p.reasons.push_back("Not chosen for compilation (only the plugins you choose are compiled)");
                blocks[p.index].candidate = false;
            }
    }

    // A plugin that stays in the load order keeps its masters in the load order too, unless it uses
    // none of a master's records and placeholders are on: then the master compiles and an empty plugin
    // with its name satisfies the master list.
    auto masterUse = [&](const PluginInfo& q) -> const std::vector<uint8_t>& {
        auto it = session.masterUse.find(q.index);
        if (it == session.masterUse.end())
            it = session.masterUse.emplace(q.index, mastersInUse(session, q)).first;
        return it->second;
    };
    {
        // Read the plugins that may need it in parallel first: those that stay and list a plugin that
        // compiles so far.
        std::vector<PluginIndex> todo;
        for (const auto& q : session.plugins) {
            if (!q.active || q.eligibility == Eligibility::Eligible || session.masterUse.count(q.index))
                continue;
            if (std::any_of(q.masters.begin(), q.masters.end(), [&](PluginIndex m) {
                    return m != kInvalidPlugin && session.plugins[m].eligibility == Eligibility::Eligible;
                }))
                todo.push_back(q.index);
        }
        std::vector<std::vector<uint8_t>> use(todo.size());
        parallelFor(
            todo.size(), [&](size_t i) { use[i] = mastersInUse(session, session.plugins[todo[i]]); }, nullProgress(),
            options.threads);
        for (size_t i = 0; i < todo.size(); ++i)
            session.masterUse.emplace(todo[i], std::move(use[i]));
    }
    // Before masters are pinned: the plugins that compile on their own merits.
    std::vector<uint8_t> ownMerits(session.plugins.size(), 0);
    for (const auto& p : session.plugins)
        ownMerits[p.index] = p.eligibility == Eligibility::Eligible;
    session.unusedMasters.clear();
    session.masterPlaceholders.clear();
    // Each plugin's dependents, with the entry of their master list naming it.
    std::vector<std::vector<std::pair<PluginIndex, size_t>>> listedBy(session.plugins.size());
    for (const auto& q : session.plugins)
        if (q.active)
            for (size_t i = 0; i < q.masters.size(); ++i)
                if (q.masters[i] != kInvalidPlugin)
                    listedBy[q.masters[i]].push_back({q.index, i});
    const bool placeholders = options.masterPlaceholders && options.placeholdersOff.empty();
    // The plugins that stay and list `m`, split into those that use it and those that do not.
    auto listers = [&](PluginIndex m, std::vector<PluginIndex>& users, std::vector<PluginIndex>& nonUsers) {
        users.clear();
        nonUsers.clear();
        for (const auto& [q, i] : listedBy[m]) {
            const auto& qp = session.plugins[q];
            if (qp.eligibility == Eligibility::Eligible)
                continue;
            const auto& use = masterUse(qp);
            (i < use.size() && !use[i] ? nonUsers : users).push_back(q);
        }
    };
    std::vector<PluginIndex> users, nonUsers;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& mp : session.plugins) {
            if (mp.eligibility != Eligibility::Eligible || listedBy[mp.index].empty())
                continue;
            listers(mp.index, users, nonUsers);
            if (users.empty() && (nonUsers.empty() || placeholders))
                continue;
            const auto& by = session.plugins[users.empty() ? nonUsers.front() : users.front()];
            std::string why = "Required as a master by " + by.name + ", which stays in the load order";
            if (users.empty())
                why += "; it uses none of its records, so this plugin could compile with an empty placeholder "
                       "keeping its name (" +
                       (options.masterPlaceholders ? "not possible here: " + options.placeholdersOff
                                                   : std::string("turn on \"Placeholders for unused masters\"")) +
                       ")";
            mp.eligibility = Eligibility::Ineligible;
            mp.reasons.push_back(why);
            changed = true;
        }
    }
    // Masters listed but not used by plugins that stay (for the report), and the placeholders needed.
    for (const auto& q : session.plugins) {
        if (!q.active || q.eligibility == Eligibility::Eligible)
            continue;
        auto it = session.masterUse.find(q.index);
        if (it == session.masterUse.end())
            continue;
        for (size_t i = 0; i < q.masters.size() && i < it->second.size(); ++i)
            if (!it->second[i] && q.masters[i] != kInvalidPlugin && !session.plugins[q.masters[i]].baseGame)
                session.unusedMasters.insert({q.index, q.masters[i]});
    }
    for (const auto& [q, m] : session.unusedMasters) {
        auto& mp = session.plugins[m];
        if (mp.eligibility != Eligibility::Eligible)
            continue;
        session.masterPlaceholders.insert(m);
        mp.reasons.push_back("Listed as a master by " + session.plugins[q].name +
                             ", which stays in the load order and uses none of its records: compiled, with an empty "
                             "placeholder plugin of the same name keeping " + session.plugins[q].name + " loading");
    }

    // What each plugin that stays for its own reasons holds back: the pinned plugins only it reaches through
    // the masters that keep plugins in the load order.
    {
        const size_t n = session.plugins.size();
        auto pinned = [&](PluginIndex m) {
            return ownMerits[m] && session.plugins[m].eligibility != Eligibility::Eligible;
        };
        auto isRoot = [&](PluginIndex q) {
            const auto& qp = session.plugins[q];
            return qp.active && qp.eligibility != Eligibility::Eligible && !ownMerits[q];
        };
        // Pin edges: a plugin that stays keeps each master it lists, except an unused one with a placeholder.
        auto targets = [&](PluginIndex q, std::vector<PluginIndex>& out) {
            out.clear();
            const auto& qp = session.plugins[q];
            const auto it = session.masterUse.find(q);
            for (size_t i = 0; i < qp.masters.size(); ++i) {
                const PluginIndex m = qp.masters[i];
                if (m == kInvalidPlugin)
                    continue;
                const bool unused = it != session.masterUse.end() && i < it->second.size() && !it->second[i];
                if (unused && placeholders)
                    continue;
                out.push_back(m);
            }
        };
        constexpr int64_t kNone = -1, kMany = -2;
        std::vector<int64_t> label(n, kNone);
        std::vector<PluginIndex> heldBy(n, kInvalidPlugin);
        std::vector<uint32_t> seen(n, 0);
        uint32_t stamp = 0;
        std::vector<PluginIndex> stack, next;
        for (auto& p : session.plugins) {
            p.holdsBack.clear();
            p.sharedHold = 0;
            p.heldBy = kInvalidPlugin;
        }
        for (const auto& root : session.plugins) {
            if (!isRoot(root.index))
                continue;
            ++stamp;
            stack.assign(1, root.index);
            seen[root.index] = stamp;
            while (!stack.empty()) {
                const PluginIndex q = stack.back();
                stack.pop_back();
                targets(q, next);
                for (PluginIndex m : next) {
                    if (seen[m] == stamp)
                        continue;
                    seen[m] = stamp;
                    if (isRoot(m)) {
                        if (heldBy[m] == kInvalidPlugin)
                            heldBy[m] = root.index;
                        continue; // it stays for its own reasons; its own walk covers its masters
                    }
                    if (!pinned(m))
                        continue;
                    label[m] = label[m] == kNone ? static_cast<int64_t>(root.index) : kMany;
                    stack.push_back(m);
                }
            }
        }
        for (const auto& p : session.plugins)
            if (pinned(p.index) && label[p.index] >= 0)
                session.plugins[static_cast<size_t>(label[p.index])].holdsBack.push_back(p.index);
        for (auto& p : session.plugins) {
            if (!isRoot(p.index))
                continue;
            p.heldBy = heldBy[p.index];
            if (p.heldBy != kInvalidPlugin)
                p.holdsBack.clear(); // it would stay anyway, and keep them with it
        }
        // Shared: pinned plugins reached by this one and by others.
        for (const auto& root : session.plugins) {
            if (!isRoot(root.index))
                continue;
            ++stamp;
            stack.assign(1, root.index);
            seen[root.index] = stamp;
            size_t shared = 0;
            while (!stack.empty()) {
                const PluginIndex q = stack.back();
                stack.pop_back();
                targets(q, next);
                for (PluginIndex m : next) {
                    if (seen[m] == stamp || isRoot(m) || !pinned(m))
                        continue;
                    seen[m] = stamp;
                    shared += label[m] == kMany;
                    stack.push_back(m);
                }
            }
            session.plugins[root.index].sharedHold = shared;
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
        // Masters listed but not used by plugins that stay (xEdit's "Clean Masters" would remove them).
        report->issues.erase(std::remove_if(report->issues.begin(), report->issues.end(),
                                            [](const DependencyIssue& is) { return is.category == "Unused Master"; }),
                             report->issues.end());
        for (const auto& [q, m] : session.unusedMasters) {
            DependencyIssue is;
            is.severity = Severity::Info;
            is.category = "Unused Master";
            is.plugin = q;
            const auto& qn = session.plugins[q].name;
            const auto& mn = session.plugins[m].name;
            is.message = qn + " lists " + mn + " as a master but uses none of its records";
            if (session.masterPlaceholders.count(m))
                is.cause = mn + " compiles; an empty placeholder plugin with its name keeps " + qn + " loading.";
            else if (session.plugins[m].eligibility == Eligibility::Eligible)
                is.cause = mn + " compiles.";
            else if (!options.masterPlaceholders)
                is.cause = mn + " stays in the load order because " + qn + " lists it. Turn on \"Placeholders for "
                           "unused masters\" (Compilation page) to compile it, or remove the master from " + qn +
                           " in xEdit (Clean Masters).";
            else if (!options.placeholdersOff.empty())
                is.cause = mn + " stays in the load order: placeholders are not possible here (" +
                           options.placeholdersOff + ").";
            else
                is.cause = mn + " stays in the load order for another reason (see the Plugins page).";
            report->issues.push_back(std::move(is));
        }
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
    // Likely causes: missing references are judged together per source -> target plugin, since a
    // patch made for another version of its master leaves many of them pointing the same way.
    std::map<std::pair<PluginIndex, PluginIndex>, size_t> perPair;
    std::map<std::pair<PluginIndex, PluginIndex>, FormKey> examplePerPair;
    std::vector<std::vector<FormKey>> missingTargets(targets.size());
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& w = session.records[session.groups[targets[i]].end - 1];
        if (!refs[i])
            continue;
        for (const auto& [to, via] : *refs[i])
            if (to != w.key && (!to.valid() || !session.exists(to))) {
                missingTargets[i].push_back(to);
                std::pair<PluginIndex, PluginIndex> pair{w.plugin, to.valid() ? to.origin : kInvalidPlugin};
                perPair[pair]++;
                examplePerPair.emplace(pair, to);
            }
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        size_t m = 0;
        for (auto& is : issues[i]) {
            if (is.category != "Missing Reference" || m >= missingTargets[i].size())
                continue;
            FormKey to = missingTargets[i][m++];
            is.cause = diagnosis::missingReference(session, is.plugin, to, to.valid() ? 0 : to.objectId,
                                                   perPair[{is.plugin, to.valid() ? to.origin : kInvalidPlugin}]);
        }
    }
    for (const auto& [pair, count] : perPair) {
        if (count < 2)
            continue;
        auto [from, to] = pair;
        std::string target = to == kInvalidPlugin ? std::string("plugins that are not its masters") : session.pluginName(to);
        report.issues.push_back(
            {Severity::Warning, "Missing References (summary)", from, {},
             session.pluginName(from) + " -> " + target + ": " + std::to_string(count) +
                 " references to records that do not exist",
             diagnosis::missingReference(session, from, examplePerPair[pair],
                                         examplePerPair[pair].valid() ? 0 : examplePerPair[pair].objectId, count)});
    }
    for (auto& v : issues)
        for (auto& is : v) {
            if (is.category == "Missing Reference")
                report.danglingReferences++;
            report.issues.push_back(std::move(is));
        }
    for (auto& is : report.issues)
        if (is.cause.empty())
            is.cause = diagnosis::forCategory(is.category, is.message);
    report.recordEdges = session.recordEdges.size();

    evaluateEligibility(session, options, &report);
    session.analysed = true;
    spdlog::info("Dependencies: {} master links, {} record references, {} dangling; {} plugins eligible for compilation",
                 report.pluginEdges, report.recordEdges, report.danglingReferences, report.eligiblePlugins);
    return report;
}

} // namespace fo4mx
