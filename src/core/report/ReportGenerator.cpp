#include "report/ReportGenerator.hpp"

#include "engine/Diagnosis.hpp"

#include "util/Json.hpp"
#include "util/Text.hpp"

#include <chrono>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>

namespace fo4mx {

const char* reportKindName(ReportKind k)
{
    switch (k) {
    case ReportKind::Conflict: return "Conflict";
    case ReportKind::Asset: return "Asset";
    case ReportKind::Dependency: return "Dependency";
    case ReportKind::Compilation: return "Compilation";
    case ReportKind::Validation: return "Validation";
    }
    return "Report";
}

namespace {

std::string now()
{
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

const char* kCss = R"CSS(
:root{--bg:#0f1311;--panel:#161c19;--line:#26302b;--text:#d7e4dc;--muted:#8aa196;--accent:#3ddc84;
--green:#3ddc84;--red:#ff5c5c;--yellow:#f5c542;--blue:#5cb8ff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);
font:14px/1.5 "Segoe UI",system-ui,sans-serif}
header{padding:24px 32px;border-bottom:1px solid var(--line);background:linear-gradient(180deg,#14201a,#0f1311)}
header h1{margin:0;font-size:22px;color:var(--accent);font-weight:600}
header p{margin:4px 0 0;color:var(--muted)}
main{padding:24px 32px;max-width:1500px}
h2{font-size:16px;margin:28px 0 10px;color:var(--text);border-left:3px solid var(--accent);padding-left:10px}
.cards{display:flex;flex-wrap:wrap;gap:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:12px 16px;min-width:150px}
.card .v{font-size:22px;font-weight:600;color:var(--accent)}.card .l{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.05em}
table{border-collapse:collapse;width:100%;background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden}
th,td{padding:6px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{background:#1b231f;color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase}
tr:hover td{background:#1a221e}
.mono{font-family:Consolas,"Cascadia Mono",monospace;font-size:12px}
.Winning,.ok{color:var(--green)}.Replaced,.Error,.High,.Critical{color:var(--red)}
.Merged,.Warning,.Medium{color:var(--yellow)}.Info,.Low{color:var(--blue)}.muted{color:var(--muted)}
.note{color:var(--muted);font-size:12px;margin-top:6px}
)CSS";

struct Palette {
    const char* bg;
    const char* panel;
    const char* header;
    const char* text;
    const char* muted;
    const char* accent;
};

class Html {
public:
    Html(const std::string& title, const std::string& subtitle, ReportStyle style = ReportStyle::Rich) : style_(style)
    {
        pal_ = style == ReportStyle::Print ? Palette{"#ffffff", "#f3f6f4", "#e3ebe6", "#1b2420", "#5b6b63", "#1f8a52"}
                                           : Palette{"#0f1311", "#161c19", "#1b231f", "#d7e4dc", "#8aa196", "#3ddc84"};
        if (style == ReportStyle::Rich) {
            out_ = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" "
                   "content=\"width=device-width,initial-scale=1\"><title>" +
                   htmlEscape(title) + "</title><style>" + kCss + "</style></head><body><header><h1>" + htmlEscape(title) +
                   "</h1><p>" + htmlEscape(subtitle) + "</p></header><main>";
        } else {
            out_ = std::string("<html><head><meta charset=\"utf-8\"><title>") + htmlEscape(title) +
                   "</title></head><body style=\"color:" + pal_.text + ";background-color:" + pal_.bg + "\">" +
                   "<h1 style=\"color:" + pal_.accent + "\">" + htmlEscape(title) + "</h1><p style=\"color:" + pal_.muted +
                   "\">" + htmlEscape(subtitle) + "</p>";
        }
    }
    void h2(const std::string& s)
    {
        if (style_ == ReportStyle::Rich)
            out_ += "<h2>" + htmlEscape(s) + "</h2>";
        else
            out_ += std::string("<h3 style=\"color:") + pal_.text + "\">" + htmlEscape(s) + "</h3>";
    }
    void note(const std::string& s)
    {
        if (style_ == ReportStyle::Rich)
            out_ += "<p class=\"note\">" + htmlEscape(s) + "</p>";
        else
            out_ += std::string("<p style=\"color:") + pal_.muted + "\">" + htmlEscape(s) + "</p>";
    }
    void cards(const std::vector<std::pair<std::string, std::string>>& items)
    {
        if (style_ == ReportStyle::Rich) {
            out_ += "<div class=\"cards\">";
            for (const auto& [l, v] : items)
                out_ += "<div class=\"card\"><div class=\"v\">" + htmlEscape(v) + "</div><div class=\"l\">" + htmlEscape(l) +
                        "</div></div>";
            out_ += "</div>";
            return;
        }
        out_ += "<table cellspacing=\"6\" cellpadding=\"8\"><tr>";
        for (const auto& [l, v] : items)
            out_ += std::string("<td style=\"background-color:") + pal_.panel + "\"><span style=\"font-size:16pt;color:" +
                    pal_.accent + "\">" + htmlEscape(v) + "</span><br><span style=\"color:" + pal_.muted + "\">" +
                    htmlEscape(l) + "</span></td>";
        out_ += "</tr></table>";
    }
    void tableStart(const std::vector<std::string>& headers)
    {
        if (style_ == ReportStyle::Rich)
            out_ += "<table><thead><tr>";
        else
            out_ += std::string("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"4\" style=\"background-color:") +
                    pal_.panel + "\"><thead><tr>";
        for (const auto& h : headers)
            out_ += style_ == ReportStyle::Rich
                        ? "<th>" + htmlEscape(h) + "</th>"
                        : std::string("<th align=\"left\" style=\"background-color:") + pal_.header + ";color:" + pal_.muted +
                              "\">" + htmlEscape(h) + "</th>";
        out_ += "</tr></thead><tbody>";
    }
    // classes: CSS class per cell (Error, Warning, ok, mono, ...).
    void row(const std::vector<std::string>& cells, const std::vector<std::string>& classes = {})
    {
        out_ += "<tr>";
        for (size_t i = 0; i < cells.size(); ++i) {
            std::string cls = i < classes.size() ? classes[i] : std::string();
            if (style_ == ReportStyle::Rich) {
                out_ += cls.empty() ? "<td>" : "<td class=\"" + cls + "\">";
            } else {
                std::string color = colorFor(cls);
                out_ += color.empty() ? "<td>" : "<td style=\"color:" + color + "\">";
            }
            out_ += htmlEscape(cells[i]) + "</td>";
        }
        out_ += "</tr>";
    }
    void tableEnd(size_t shown, size_t total)
    {
        out_ += "</tbody></table>";
        if (shown < total)
            note("Showing " + formatCount(shown) + " of " + formatCount(total) + " rows. The JSON report contains all rows.");
    }
    std::string finish()
    {
        note("Generated by Fallout 4 Multiplexer Renewed on " + now() + ".");
        out_ += style_ == ReportStyle::Rich ? "</main></body></html>" : "</body></html>";
        return out_;
    }

private:
    std::string colorFor(const std::string& cls) const
    {
        bool print = style_ == ReportStyle::Print;
        if (cls == "Error" || cls == "High" || cls == "Critical" || cls == "Replaced")
            return print ? "#c62828" : "#ff5c5c";
        if (cls == "Warning" || cls == "Medium" || cls == "Merged")
            return print ? "#a66b00" : "#f5c542";
        if (cls == "ok" || cls == "Winning")
            return pal_.accent;
        if (cls == "Info" || cls == "Low")
            return print ? "#1565c0" : "#5cb8ff";
        return {};
    }

    ReportStyle style_;
    Palette pal_;
    std::string out_;
};

std::string subtitle(const ReportContext& ctx)
{
    if (!ctx.session)
        return {};
    const auto& s = *ctx.session;
    return std::string(installKindName(s.install.config.kind)) + " · " + pathToUtf8(s.install.gamePath) + " · " +
           s.defs.profileName();
}

std::string conflictHtml(const ReportContext& ctx)
{
    Html h("Conflict Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    const auto& c = *ctx.conflicts;
    h.cards({{"Total Conflicts", formatCount(c.conflictCount())},
             {"Type 1 · Simple Override", formatCount(c.byType[1])},
             {"Type 2 · Compatible Merge", formatCount(c.byType[2])},
             {"Type 3 · Hard Conflict", formatCount(c.byType[3])},
             {"Conflict · Not Merged", formatCount(c.byType[4])},
             {"ITM-only Chains", formatCount(c.identicalOverrides)}});

    h.h2("Most conflicting plugins");
    std::vector<const PluginInfo*> plugins;
    for (const auto& p : s.plugins)
        if (p.conflictCount)
            plugins.push_back(&p);
    std::sort(plugins.begin(), plugins.end(),
              [](auto a, auto b) { return a->hardConflictCount * 10 + a->conflictCount > b->hardConflictCount * 10 + b->conflictCount; });
    h.tableStart({"Plugin", "Conflicts", "Hard", "ITMs", "Overrides"});
    size_t shown = 0;
    for (auto p : plugins) {
        if (shown++ >= 50)
            break;
        h.row({p->name, formatCount(p->conflictCount), formatCount(p->hardConflictCount), formatCount(p->itmCount),
               formatCount(p->overrides)});
    }
    h.tableEnd(std::min<size_t>(50, plugins.size()), plugins.size());

    h.h2("Conflicts (hard conflicts first)");
    std::vector<const ConflictEntry*> list;
    for (const auto& e : c.entries)
        list.push_back(&e);
    std::stable_sort(list.begin(), list.end(),
                     [](auto a, auto b) { return conflictPriority(a->type) < conflictPriority(b->type); });
    h.tableStart({"Form", "Type", "Severity", "Winner", "Override chain", "Handling", "Summary"});
    shown = 0;
    for (auto e : list) {
        if (shown >= ctx.maxRows)
            break;
        ++shown;
        std::vector<std::string> chain;
        for (auto p : e->chain)
            chain.push_back(s.pluginName(p));
        h.row({s.formLabel(e->key), conflictTypeName(e->type), conflictSeverityName(e->severity), s.pluginName(e->winner),
               join(chain, " › "), e->preserved ? "Not merged (kept as is)" : "Merged field by field", e->summary},
              {"mono", "", conflictSeverityName(e->severity)});
    }
    h.tableEnd(shown, list.size());
    return h.finish();
}

std::string assetHtml(const ReportContext& ctx)
{
    Html h("Asset Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    const auto& a = s.assets;
    std::map<std::string, std::pair<size_t, size_t>> byType;
    for (const auto& e : a.entries) {
        auto& t = byType[assetTypeName(e.type)];
        t.first++;
        if (e.conflicting())
            t.second++;
    }
    h.cards({{"Assets", formatCount(a.entries.size())},
             {"Conflicting", formatCount(a.conflictCount())},
             {"Archives", formatCount(a.archives.size())},
             {"Loose Files", formatCount(a.looseFileCount())}});
    h.h2("By asset type");
    h.tableStart({"Type", "Assets", "Conflicting"});
    for (const auto& [t, v] : byType)
        h.row({t, formatCount(v.first), formatCount(v.second)});
    h.tableEnd(byType.size(), byType.size());

    h.h2("Archive load order");
    h.tableStart({"#", "Archive", "Mod", "Loaded by", "Files"});
    for (const auto& ar : a.archives)
        h.row({std::to_string(ar.loadSequence), ar.name, ar.modName,
               ar.owner == kInvalidPlugin ? "INI" : s.pluginName(ar.owner),
               ar.archive ? formatCount(ar.archive->entries().size()) : "unreadable"});
    h.tableEnd(a.archives.size(), a.archives.size());

    h.h2("Asset conflicts");
    h.tableStart({"Asset", "Type", "Winning source", "Overridden sources"});
    size_t shown = 0, total = 0;
    for (const auto& e : a.entries) {
        if (!e.conflicting())
            continue;
        ++total;
        if (shown >= ctx.maxRows)
            continue;
        ++shown;
        std::vector<std::string> losers;
        for (size_t i = 0; i + 1 < e.sources.size(); ++i)
            losers.push_back(a.sourceLabel(e.sources[i]));
        h.row({a.path(e), assetTypeName(e.type), a.sourceLabel(e.winner()), join(losers, "; ")}, {"mono", "", "ok"});
    }
    h.tableEnd(shown, total);

    if (!s.nameReferences.empty()) {
        h.h2("Plugins referenced by file name");
        h.note("Scripts and configuration files that name a plugin keep that plugin in the load order.");
        h.tableStart({"Plugin", "Referenced in", "Provided by"});
        for (const auto& r : s.nameReferences)
            h.row({s.pluginName(r.plugin), r.assetPath, r.source});
        h.tableEnd(s.nameReferences.size(), s.nameReferences.size());
    }
    return h.finish();
}

std::string dependencyHtml(const ReportContext& ctx)
{
    Html h("Dependency Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    std::map<Eligibility, size_t> counts;
    for (const auto& p : s.plugins)
        counts[p.eligibility]++;
    std::vector<std::pair<std::string, std::string>> cards = {
        {"Master Links", ctx.dependencies ? formatCount(ctx.dependencies->pluginEdges) : "-"},
        {"Record References", ctx.dependencies ? formatCount(ctx.dependencies->recordEdges) : "-"},
        {"Dangling References", ctx.dependencies ? formatCount(ctx.dependencies->danglingReferences) : "-"}};
    for (auto [e, n] : counts)
        cards.emplace_back(std::string(eligibilityName(e)) + " Plugins", formatCount(n));
    h.cards(cards);

    h.h2("Plugins");
    h.tableStart({"#", "Plugin", "Type", "Masters", "Eligibility", "Reason"});
    for (auto pi : s.loadOrder) {
        const auto& p = s.plugins[pi];
        std::vector<std::string> masters;
        for (const auto& m : p.header.masters)
            masters.push_back(m.name);
        h.row({p.loadOrderHex, p.name, pluginKindName(p.kind), join(masters, ", "), eligibilityName(p.eligibility),
               join(p.reasons, "; ")},
              {"mono", "", "", "", p.eligibility == Eligibility::Eligible ? "ok" : (p.eligibility == Eligibility::Error ? "Error" : "")});
    }
    h.tableEnd(s.loadOrder.size(), s.loadOrder.size());

    for (bool cleaning : {false, true}) {
        if (!ctx.dependencies)
            break;
        std::vector<const DependencyIssue*> list;
        for (const auto& is : ctx.dependencies->issues)
            if (is.cleaning == cleaning)
                list.push_back(&is);
        if (list.empty())
            continue;
        h.h2(cleaning ? "Cleaning report (as in xEdit: reporting only, plugins are never changed)"
                      : "Missing requirements and potential breakages");
        h.tableStart({"Severity", "Category", "Plugin", "Details", "Likely Cause"});
        size_t shown = 0;
        for (const auto* is : list) {
            if (shown >= ctx.maxRows)
                break;
            ++shown;
            h.row({severityName(is->severity), is->category, s.pluginName(is->plugin), is->message,
                   is->cause.empty() ? diagnosis::forCategory(is->category, is->message) : is->cause},
                  {severityName(is->severity)});
        }
        h.tableEnd(shown, list.size());
    }
    return h.finish();
}

std::string compilationHtml(const ReportContext& ctx)
{
    Html h("Compilation Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    const auto& c = *ctx.compiled;
    size_t used = 0;
    for (const auto& op : c.outputs)
        used += op.records.empty() ? 0 : 1;
    h.cards({{"Compiled Plugins", formatCount(c.compiledPlugins.size())},
             {"Retained Plugins", formatCount(c.retainedPlugins.size())},
             {"Output Plugins", formatCount(used)},
             {"Records Written", formatCount(c.records.size())},
             {"New FormIDs", formatCount(c.mappings.size())},
             {"Merged Records", formatCount(c.mergedRecords)}});
    h.note(std::string("Strategy: ") + allocationStrategyName(c.options.strategy) +
           (c.options.mergeCompatible ? " · compatible edits merged" : " · last plugin wins"));

    h.h2("Output plugins");
    h.tableStart({"Plugin", "Records", "New", "Overrides", "Size", "Light", "Masters"});
    for (const auto& op : c.outputs) {
        if (op.records.empty())
            continue;
        h.row({op.name, formatCount(op.records.size()), formatCount(op.newRecords), formatCount(op.overrides),
               formatBytes(op.bytes), op.light ? "Yes" : "No", join(op.masterNames, ", ")});
    }
    h.tableEnd(used, used);

    h.h2("Compiled plugins (disable these)");
    h.tableStart({"Plugin", "Records", "Archives to register"});
    for (auto pi : c.compiledPlugins) {
        const auto& p = s.plugins[pi];
        h.row({p.name, formatCount(p.recordCount), join(p.archives, ", ")});
    }
    h.tableEnd(c.compiledPlugins.size(), c.compiledPlugins.size());

    h.h2("FormID mappings");
    h.tableStart({"Source", "Source FormID", "Output", "New FormID", "Type", "Editor ID"});
    size_t shown = 0;
    for (const auto& m : c.mappings) {
        if (shown++ >= ctx.maxRows)
            break;
        h.row({m.sourcePlugin, formatFormId(m.sourceObjectId), m.outputPlugin, formatFormId(m.objectId), m.sig.str(),
               m.editorId},
              {"", "mono", "", "mono"});
    }
    h.tableEnd(std::min(ctx.maxRows, c.mappings.size()), c.mappings.size());
    return h.finish();
}

std::string validationHtml(const ReportContext& ctx)
{
    Html h("Validation Report", subtitle(ctx), ctx.style);
    const auto& v = *ctx.validation;
    h.cards({{"Checks", formatCount(v.checks)},
             {"Passed", formatCount(v.passed)},
             {"Warnings", formatCount(v.warnings)},
             {"Errors", formatCount(v.errors)}});
    h.note(v.critical() ? "Critical failures were found: compilation output must not be used."
                        : "No critical failures.");
    h.tableStart({"Severity", "Category", "Plugin", "FormID", "Message", "Likely Cause"});
    std::vector<const ValidationIssue*> list;
    for (const auto& is : v.issues)
        list.push_back(&is);
    std::stable_sort(list.begin(), list.end(), [](auto a, auto b) { return a->severity > b->severity; });
    size_t shown = 0;
    for (auto is : list) {
        if (shown++ >= ctx.maxRows)
            break;
        h.row({severityName(is->severity), is->category, is->plugin, is->formId, is->message,
               is->cause.empty() ? diagnosis::forCategory(is->category, is->message) : is->cause},
              {severityName(is->severity), "", "", "mono"});
    }
    h.tableEnd(std::min(ctx.maxRows, list.size()), list.size());
    return h.finish();
}

json::Value pluginJson(const PluginInfo& p)
{
    json::Value o;
    o.set("name", p.name);
    o.set("type", pluginKindName(p.kind));
    o.set("loadOrder", p.loadOrderHex);
    o.set("records", p.recordCount);
    o.set("conflicts", p.conflictCount);
    o.set("eligibility", eligibilityName(p.eligibility));
    json::Value reasons;
    for (const auto& r : p.reasons)
        reasons.push(r);
    o.set("reasons", reasons);
    return o;
}

} // namespace

std::string ReportGenerator::html(ReportKind kind, const ReportContext& ctx)
{
    switch (kind) {
    case ReportKind::Conflict: return ctx.session && ctx.conflicts ? conflictHtml(ctx) : std::string();
    case ReportKind::Asset: return ctx.session ? assetHtml(ctx) : std::string();
    case ReportKind::Dependency: return ctx.session ? dependencyHtml(ctx) : std::string();
    case ReportKind::Compilation: return ctx.session && ctx.compiled ? compilationHtml(ctx) : std::string();
    case ReportKind::Validation: return ctx.validation ? validationHtml(ctx) : std::string();
    }
    return {};
}

std::string ReportGenerator::json(ReportKind kind, const ReportContext& ctx)
{
    json::Value root;
    root.set("report", reportKindName(kind));
    root.set("generated", now());
    const Session* s = ctx.session;
    if (s)
        root.set("runtime", s->defs.profileName());
    switch (kind) {
    case ReportKind::Conflict: {
        if (!s || !ctx.conflicts)
            return {};
        const auto& c = *ctx.conflicts;
        json::Value counts;
        counts.set("simpleOverride", static_cast<uint64_t>(c.byType[1]));
        counts.set("compatibleMerge", static_cast<uint64_t>(c.byType[2]));
        counts.set("hardConflict", static_cast<uint64_t>(c.byType[3]));
        counts.set("notMergedConflict", static_cast<uint64_t>(c.byType[4]));
        counts.set("itmOnly", static_cast<uint64_t>(c.identicalOverrides));
        root.set("counts", counts);
        json::Value list;
        for (const auto& e : c.entries) {
            json::Value o;
            o.set("formId", s->loadOrderFormId(e.key));
            o.set("signature", e.sig.str());
            o.set("editorId", e.editorId);
            o.set("type", static_cast<int>(e.type));
            o.set("typeName", conflictTypeName(e.type));
            o.set("merged", !e.preserved);
            o.set("severity", conflictSeverityName(e.severity));
            o.set("winner", s->pluginName(e.winner));
            json::Value chain;
            for (auto p : e.chain)
                chain.push(s->pluginName(p));
            o.set("chain", chain);
            o.set("summary", e.summary);
            json::Value fields;
            for (const auto& f : e.fields) {
                json::Value fo;
                fo.set("field", f.label);
                fo.set("hard", f.hard);
                json::Value cells;
                for (const auto& cell : f.cells) {
                    json::Value co;
                    co.set("plugin", s->pluginName(cell.plugin));
                    co.set("value", cell.value);
                    co.set("status", cellStatusName(cell.status));
                    cells.push(co);
                }
                fo.set("values", cells);
                fields.push(fo);
            }
            o.set("fields", fields);
            list.push(o);
        }
        root.set("conflicts", list);
        break;
    }
    case ReportKind::Asset: {
        if (!s)
            return {};
        const auto& a = s->assets;
        json::Value list;
        for (const auto& e : a.entries) {
            if (!e.conflicting())
                continue;
            json::Value o;
            o.set("path", a.path(e));
            o.set("type", assetTypeName(e.type));
            json::Value sources;
            for (const auto& src : e.sources)
                sources.push(a.sourceLabel(src));
            o.set("sources", sources);
            o.set("winner", a.sourceLabel(e.winner()));
            list.push(o);
        }
        root.set("assetConflicts", list);
        json::Value refs;
        for (const auto& r : s->nameReferences) {
            json::Value o;
            o.set("plugin", s->pluginName(r.plugin));
            o.set("file", r.assetPath);
            o.set("source", r.source);
            refs.push(o);
        }
        root.set("pluginNameReferences", refs);
        break;
    }
    case ReportKind::Dependency: {
        if (!s)
            return {};
        json::Value plugins;
        for (auto pi : s->loadOrder) {
            auto o = pluginJson(s->plugins[pi]);
            json::Value masters;
            for (const auto& m : s->plugins[pi].header.masters)
                masters.push(m.name);
            o.set("masters", masters);
            plugins.push(o);
        }
        root.set("plugins", plugins);
        if (ctx.dependencies) {
            json::Value issues;
            for (const auto& is : ctx.dependencies->issues) {
                json::Value o;
                o.set("severity", severityName(is.severity));
                o.set("category", is.category);
                o.set("plugin", s->pluginName(is.plugin));
                o.set("message", is.message);
                o.set("cause", is.cause.empty() ? diagnosis::forCategory(is.category, is.message) : is.cause);
                o.set("cleaning", is.cleaning);
                issues.push(o);
            }
            root.set("issues", issues);
        }
        break;
    }
    case ReportKind::Compilation: {
        if (!s || !ctx.compiled)
            return {};
        const auto& c = *ctx.compiled;
        root.set("strategy", allocationStrategyName(c.options.strategy));
        json::Value compiled;
        for (auto pi : c.compiledPlugins)
            compiled.push(s->pluginName(pi));
        root.set("compiledPlugins", compiled);
        json::Value outputs;
        for (const auto& op : c.outputs) {
            if (op.records.empty())
                continue;
            json::Value o;
            o.set("name", op.name);
            o.set("records", static_cast<uint64_t>(op.records.size()));
            o.set("newRecords", op.newRecords);
            o.set("overrides", op.overrides);
            o.set("light", op.light);
            json::Value masters;
            for (const auto& m : op.masterNames)
                masters.push(m);
            o.set("masters", masters);
            outputs.push(o);
        }
        root.set("outputs", outputs);
        json::Value maps;
        for (const auto& m : c.mappings) {
            json::Value o;
            o.set("source", m.sourcePlugin);
            o.set("sourceFormId", formatFormId(m.sourceObjectId));
            o.set("output", m.outputPlugin);
            o.set("formId", formatFormId(m.objectId));
            o.set("signature", m.sig.str());
            o.set("editorId", m.editorId);
            maps.push(o);
        }
        root.set("formMappings", maps);
        json::Value archives;
        for (const auto& a : c.archivesToRegister)
            archives.push(a);
        root.set("archivesToRegister", archives);
        break;
    }
    case ReportKind::Validation: {
        if (!ctx.validation)
            return {};
        const auto& v = *ctx.validation;
        root.set("checks", v.checks);
        root.set("passed", v.passed);
        root.set("warnings", v.warnings);
        root.set("errors", v.errors);
        json::Value issues;
        for (const auto& is : v.issues) {
            json::Value o;
            o.set("severity", severityName(is.severity));
            o.set("category", is.category);
            o.set("message", is.message);
            o.set("plugin", is.plugin);
            o.set("formId", is.formId);
            o.set("cause", is.cause.empty() ? diagnosis::forCategory(is.category, is.message) : is.cause);
            issues.push(o);
        }
        root.set("issues", issues);
        break;
    }
    }
    return root.dump();
}

std::vector<std::filesystem::path> ReportGenerator::writeAll(const ReportContext& ctx, const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);
    std::vector<std::filesystem::path> written;
    for (auto kind : {ReportKind::Conflict, ReportKind::Asset, ReportKind::Dependency, ReportKind::Compilation,
                      ReportKind::Validation}) {
        std::string base = std::string(reportKindName(kind)) + "Report";
        auto h = html(kind, ctx);
        if (h.empty())
            continue;
        auto hp = dir / (base + ".html");
        std::ofstream(hp, std::ios::binary) << h;
        written.push_back(hp);
        auto jp = dir / (base + ".json");
        std::ofstream(jp, std::ios::binary) << json(kind, ctx);
        written.push_back(jp);
    }
    return written;
}

} // namespace fo4mx
