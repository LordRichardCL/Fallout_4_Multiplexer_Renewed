#include "engine/CleaningEngine.hpp"

#include "engine/Diagnosis.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <functional>
#include <map>

namespace fo4mx {

namespace {

bool official(const PluginInfo& p)
{
    return p.baseGame || p.creationClub;
}

std::string recordLabel(const Session& session, const RecordInstance& r)
{
    std::string label = r.signature.str() + " " + session.loadOrderFormId(r.key);
    if (r.editorId)
        label += " (" + std::string(session.strings.get(r.editorId)) + ")";
    return label;
}

DependencyIssue finding(const Session& session, Severity severity, const char* category, PluginIndex plugin,
                        FormKey form, std::string message)
{
    DependencyIssue is{severity, category, plugin, form, std::move(message), {}, true};
    if (plugin < session.plugins.size() && official(session.plugins[plugin]))
        is.severity = Severity::Info;
    return is;
}

// Leveled lists whose entries lead back to themselves, from the winning records' LVLO references.
void circularLeveledLists(const Session& session, DependencyReport& report)
{
    const Signature lvlo("LVLO");
    auto isList = [&](FormKey k) {
        auto w = session.winner(k);
        return w && (w->signature == Signature("LVLI") || w->signature == Signature("LVLN"));
    };
    std::map<FormKey, std::vector<FormKey>> next;
    for (const auto& e : session.recordEdges)
        if (e.via == lvlo && isList(e.from) && isList(e.to))
            next[e.from].push_back(e.to);
    if (next.empty())
        return;

    // Tarjan's strongly connected components.
    std::map<FormKey, int> index, low;
    std::map<FormKey, bool> onStack;
    std::vector<FormKey> stack;
    int counter = 0;
    std::vector<std::vector<FormKey>> cycles;
    std::function<void(FormKey)> visit = [&](FormKey v) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        onStack[v] = true;
        for (FormKey w : next[v]) {
            if (!index.count(w)) {
                visit(w);
                low[v] = std::min(low[v], low[w]);
            } else if (onStack[w]) {
                low[v] = std::min(low[v], index[w]);
            }
        }
        if (low[v] == index[v]) {
            std::vector<FormKey> scc;
            FormKey w;
            do {
                w = stack.back();
                stack.pop_back();
                onStack[w] = false;
                scc.push_back(w);
            } while (!(w == v));
            const auto& out = next[v];
            if (scc.size() > 1 || std::find(out.begin(), out.end(), v) != out.end())
                cycles.push_back(std::move(scc));
        }
    };
    std::vector<FormKey> nodes;
    for (const auto& [k, _] : next)
        nodes.push_back(k);
    std::sort(nodes.begin(), nodes.end());
    for (FormKey k : nodes)
        if (!index.count(k))
            visit(k);

    for (auto& scc : cycles) {
        std::sort(scc.begin(), scc.end());
        std::vector<std::string> names;
        std::vector<std::string> plugins;
        for (FormKey k : scc) {
            const auto* w = session.winner(k);
            names.push_back(recordLabel(session, *w) + " from " + session.pluginName(w->plugin));
            plugins.push_back(session.pluginName(w->plugin));
        }
        std::sort(plugins.begin(), plugins.end());
        plugins.erase(std::unique(plugins.begin(), plugins.end()), plugins.end());
        const auto* first = session.winner(scc.front());
        report.issues.push_back(finding(session, Severity::Error, "Circular Leveled List", first->plugin, scc.front(),
                                        std::to_string(scc.size()) + " leveled list" + (scc.size() == 1 ? "" : "s") +
                                            " refer back to themselves: " + join(names, " -> ") +
                                            ". Plugins involved: " + join(plugins, ", ")));
    }
}

} // namespace

void CleaningEngine::analyse(const Session& session, const ConflictReport& conflicts, DependencyReport& report)
{
    const size_t before = report.issues.size();

    // Identical to master: one summary per plugin; those that undo other plugins' changes one by one.
    std::map<PluginIndex, std::pair<size_t, std::vector<std::string>>> itmPerPlugin;
    for (const auto& itm : conflicts.itms) {
        // Cells, worldspaces and dialogue topics are copied unchanged into any plugin that edits what they
        // contain (placed objects, dialogue): they are required parents, not dirty edits, and xEdit does not
        // clean them either. Whether such a copy loading last overrides cell settings is a load order
        // conflict, shown in the Conflict Centre.
        if (const auto* entry = session.defs.catalogueEntry(itm.sig);
            entry && (entry->placement == RecordPlacement::Cell || entry->placement == RecordPlacement::TopWithChildren))
            continue;
        const auto* g = session.findGroup(itm.key);
        const RecordInstance* inst = nullptr;
        if (g)
            for (const auto& r : session.instances(*g))
                if (r.plugin == itm.plugin)
                    inst = &r;
        if (!inst)
            continue;
        auto& [count, examples] = itmPerPlugin[itm.plugin];
        ++count;
        if (examples.size() < 5)
            examples.push_back(recordLabel(session, *inst));
        if (itm.cancelsChanges)
            report.issues.push_back(finding(
                session, Severity::Warning, "Identical to Master (undoes changes)", itm.plugin, itm.key,
                recordLabel(session, *inst) + ": " + session.pluginName(itm.plugin) +
                    " repeats its master's version unchanged but loads last, so it undoes other plugins' changes "
                    "to this record (see the Conflict Centre)"));
    }
    for (const auto& [plugin, data] : itmPerPlugin) {
        const auto& [count, examples] = data;
        report.issues.push_back(finding(session, Severity::Info, "Identical to Master (summary)", plugin, {},
                                        session.pluginName(plugin) + ": " + std::to_string(count) +
                                            " record" + (count == 1 ? "" : "s") +
                                            " identical to master, e.g. " + join(examples, ", ")));
    }

    // Deleted and injected records.
    for (const auto& g : session.groups) {
        auto inst = session.instances(g);
        const auto& first = inst.front();
        if (first.plugin != first.key.origin) {
            std::string origin = first.key.origin < session.plugins.size() ? session.pluginName(first.key.origin)
                                                                           : std::string("a missing plugin");
            report.issues.push_back(finding(session, Severity::Info, "Injected Record", first.plugin, first.key,
                                            recordLabel(session, first) + ": " + session.pluginName(first.plugin) +
                                                " adds a new record with a FormID that belongs to " + origin +
                                                ", which does not contain it"));
        }
        const bool baseDeleted = first.flags & RecordFlags::Deleted;
        for (size_t k = 1; k < inst.size() && !baseDeleted; ++k) {
            const auto& r = inst[k];
            if (!(r.flags & RecordFlags::Deleted))
                continue;
            const auto* entry = session.defs.catalogueEntry(r.signature);
            const bool reference = entry && entry->category == "Placed References";
            const bool navmesh = r.signature == Signature("NAVM");
            const char* category = navmesh ? "Deleted Navmesh" : reference ? "Deleted Reference" : "Deleted Record";
            report.issues.push_back(finding(session, navmesh || reference ? Severity::Warning : Severity::Info, category,
                                            r.plugin, r.key,
                                            recordLabel(session, r) + ": " + session.pluginName(r.plugin) +
                                                " deletes a record from " + session.pluginName(first.plugin)));
        }
    }

    circularLeveledLists(session, report);

    // Most serious first, so reports that show only the first rows still show every error.
    std::stable_sort(report.issues.begin() + static_cast<std::ptrdiff_t>(before), report.issues.end(),
                     [](const DependencyIssue& a, const DependencyIssue& b) {
                         if (a.severity != b.severity)
                             return a.severity > b.severity;
                         return a.category < b.category;
                     });
    for (size_t i = before; i < report.issues.size(); ++i) {
        auto& is = report.issues[i];
        const bool fromOfficial = is.plugin < session.plugins.size() && official(session.plugins[is.plugin]);
        is.cause = diagnosis::forCleaning(is.category, fromOfficial);
    }
}

} // namespace fo4mx
