#include "report/ReportGenerator.hpp"

#include "engine/Diagnosis.hpp"

#include "util/Json.hpp"
#include "util/Text.hpp"

#include <chrono>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <tuple>

namespace fo4mx {

const char* reportKindName(ReportKind k)
{
    switch (k) {
    case ReportKind::Conflict: return "Conflict";
    case ReportKind::Asset: return "Asset";
    case ReportKind::Dependency: return "Dependency";
    case ReportKind::Compilation: return "Compilation";
    case ReportKind::Validation: return "Validation";
    case ReportKind::AutoMerger: return "AutoMerger";
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
    void h3(const std::string& s)
    {
        out_ += std::string("<h4 style=\"color:") + pal_.text + "\">" + htmlEscape(s) + "</h4>";
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

// Plain text, for reading in any editor: the same sections and tables as the HTML reports, with
// columns padded to line up (the last column is never padded, so long messages stay whole).
class Text {
public:
    Text(const std::string& title, const std::string& subtitle, ReportStyle = ReportStyle::Rich)
    {
        out_ = title + "\n" + std::string(title.size(), '=') + "\n";
        if (!subtitle.empty())
            out_ += subtitle + "\n";
    }
    void h2(const std::string& s) { out_ += "\n" + s + "\n" + std::string(s.size(), '-') + "\n"; }
    void h3(const std::string& s) { out_ += "\n" + s + "\n"; }
    void note(const std::string& s) { out_ += s + "\n"; }
    void cards(const std::vector<std::pair<std::string, std::string>>& items)
    {
        size_t width = 0;
        for (const auto& [l, _] : items)
            width = std::max(width, columns(l));
        out_ += "\n";
        for (const auto& [l, v] : items)
            out_ += "  " + l + ":" + std::string(width - columns(l) + 1, ' ') + v + "\n";
    }
    void tableStart(const std::vector<std::string>& headers)
    {
        rows_.clear();
        rows_.push_back(headers);
    }
    void row(const std::vector<std::string>& cells, const std::vector<std::string>& = {}) { rows_.push_back(cells); }
    void tableEnd(size_t shown, size_t total)
    {
        constexpr size_t kMaxWidth = 48; // wider cells overflow rather than stretch every row
        std::vector<size_t> width;
        for (const auto& r : rows_)
            for (size_t i = 0; i < r.size(); ++i) {
                if (width.size() <= i)
                    width.push_back(0);
                width[i] = std::max(width[i], std::min(kMaxWidth, columns(flat(r[i]))));
            }
        for (size_t ri = 0; ri < rows_.size(); ++ri) {
            std::string line = " ";
            for (size_t i = 0; i < rows_[ri].size(); ++i) {
                std::string cell = flat(rows_[ri][i]);
                line += " " + cell;
                if (i + 1 < rows_[ri].size() && columns(cell) < width[i])
                    line += std::string(width[i] - columns(cell), ' ');
            }
            out_ += line + "\n";
            if (ri == 0) {
                size_t rule = 0;
                for (size_t i = 0; i < width.size(); ++i)
                    rule += width[i] + 1;
                out_ += "  " + std::string(std::min<size_t>(rule, 120), '-') + "\n";
            }
        }
        rows_.clear();
        if (shown < total)
            note("Showing " + formatCount(shown) + " of " + formatCount(total) + " rows. The JSON report contains all rows.");
    }
    std::string finish()
    {
        out_ += "\nGenerated by Fallout 4 Multiplexer Renewed on " + now() + ".\n";
        return out_;
    }

private:
    // Characters, not bytes: UTF-8 continuation bytes (10xxxxxx) take no column.
    static size_t columns(const std::string& s)
    {
        size_t n = 0;
        for (unsigned char c : s)
            n += (c & 0xC0) != 0x80;
        return n;
    }
    static std::string flat(std::string s)
    {
        for (char& c : s)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        return s;
    }
    std::string out_;
    std::vector<std::vector<std::string>> rows_;
};

std::string subtitle(const ReportContext& ctx)
{
    if (!ctx.session)
        return {};
    const auto& s = *ctx.session;
    return std::string(installKindName(s.install.config.kind)) + " · " + pathToUtf8(s.install.gamePath) + " · " +
           s.defs.profileName();
}

template <class W>
std::string conflictReport(const ReportContext& ctx)
{
    W h("Conflict Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    const auto& c = *ctx.conflicts;
    h.cards({{"Total Conflicts", formatCount(c.conflictCount())},
             {"Type 1 · Simple Override", formatCount(c.byType[1])},
             {"Type 2 · Compatible Merge", formatCount(c.byType[2])},
             {"Type 3 · Hard Conflict", formatCount(c.byType[3])},
             {"Conflict · Not Merged", formatCount(c.byType[4])},
             {"ITM-only Chains", formatCount(c.identicalOverrides)},
             {"Changes Not in the Winner", formatCount(c.lostChanges)},
             {"Green · Low Risk", formatCount(c.lostByClass[0])},
             {"Yellow · Review", formatCount(c.lostByClass[1])},
             {"Red · Not Carried", formatCount(c.lostByClass[2])},
             {"Unknown · No Rule", formatCount(c.lostByClass[3])}});

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
               join(chain, " › "), mergeHandlingName(e->handling), e->summary},
              {"mono", "", conflictSeverityName(e->severity)});
    }
    h.tableEnd(shown, list.size());

    // Change provenance: each change a plugin made against the version it was made from that the
    // winning version does not hold, and what the load order shows about why. Facts, not intent.
    h.h2("Changes not in the winning version");
    h.note("Each change is what a plugin's version differs from the version it was made against (its masters). "
           "\"Not made with it as a master\" means the later plugin never had the change to keep; \"no longer "
           "contains it\" means it had it in its masters and does not have it now. Neither says why. The class says "
           "whether carrying the change into a patch matches a documented rule: Green low risk (not a guarantee), "
           "Yellow review needed, Red never carried automatically, Unknown no rule for it "
           "(docs/COMPATIBILITY_PATCH_RESEARCH.md, 8.2).");
    h.tableStart({"Form", "Field", "Change", "Made by", "What became of it", "Class", "Why", "Notes"});
    size_t lostShown = 0, lostTotal = 0;
    auto names = [&](const std::vector<PluginIndex>& ps) {
        std::vector<std::string> out;
        for (auto p : ps)
            out.push_back(s.pluginName(p));
        return join(out, ", ");
    };
    for (auto e : list)
        for (const auto& t : e->changes) {
            if (!isLost(t.status))
                continue;
            ++lostTotal;
            if (lostShown >= ctx.maxRows)
                continue;
            ++lostShown;
            const std::string src = s.pluginName(t.source);
            const std::string at = t.lostAt == kInvalidPlugin ? std::string("the winner") : s.pluginName(t.lostAt);
            std::string change = t.item.empty() ? t.before + " → " + t.after : std::string(changeOpName(t.op)) + " " + t.item;
            if (!t.position.empty())
                change += " (" + t.position + ")";
            std::string fate;
            switch (t.status) {
            case ChangeStatus::OmittedLater: fate = "not in " + at + ", which was not made with " + src + " as a master"; break;
            case ChangeStatus::RemovedLater: fate = at + " (made with " + src + " as a master) no longer contains it"; break;
            case ChangeStatus::Replaced: fate = at + " (made with " + src + " as a master) holds another value"; break;
            case ChangeStatus::Reverted: fate = at + " (made with " + src + " as a master) holds the earlier value again"; break;
            case ChangeStatus::Survives: break;
            }
            std::vector<std::string> notes;
            if (!t.competesWith.empty())
                notes.push_back("competes with " + names(t.competesWith));
            if (t.duplicateOf != kInvalidPlugin)
                notes.push_back("also added by " + s.pluginName(t.duplicateOf));
            if (!t.keptBy.empty())
                notes.push_back("kept by " + names(t.keptBy));
            if (t.opaque)
                notes.push_back("undescribed bytes: not understood");
            if (!t.requiredMasters.empty())
                notes.push_back("a patch would need " + names(t.requiredMasters) + " as master");
            h.row({s.formLabel(e->key), t.field, change, src, fate, compatClassName(t.cls), t.rule, join(notes, "; ")},
                  {"mono"});
        }
    h.tableEnd(lostShown, lostTotal);

    // The reviewable compatibility patch plan (phase 7): nothing is written from it yet.
    if (const auto* plan = ctx.plan) {
        h.h2("Compatibility patch plan");
        h.note("What a compatibility patch would do with each change above, starting from the winning version. "
               "Green changes are carried by default, Yellow ones wait for your decision (kept as the winner has "
               "them until then), Red and Unknown ones are never carried. Your choices are saved and kept across "
               "scans. Nothing is written from this plan yet.");
        h.cards({{"Records Ready", formatCount(plan->byStatus[static_cast<size_t>(PlanStatus::Ready)])},
                 {"Need a Decision", formatCount(plan->byStatus[static_cast<size_t>(PlanStatus::NeedsDecision)])},
                 {"Blocked", formatCount(plan->byStatus[static_cast<size_t>(PlanStatus::Blocked)])},
                 {"Changes Carried", formatCount(plan->carried)},
                 {"Changes Waiting", formatCount(plan->pending)},
                 {"Kept as Winner", formatCount(plan->kept)},
                 {"Never Carried", formatCount(plan->locked)},
                 {"Extra Masters", formatCount(plan->masters.size())}});
        h.tableStart({"Form", "Status", "Carried", "Waiting", "Kept / never carried", "Masters needed", "Problems"});
        size_t planShown = 0;
        for (const auto& pr : plan->records) {
            if (planShown >= ctx.maxRows)
                break;
            ++planShown;
            size_t carried = 0, waiting = 0, other = 0;
            for (const auto& pe : pr.entries)
                (pe.decision == PlanDecision::Pending ? waiting
                 : pe.action == PlanAction::CarryForward ? carried
                                                         : other)++;
            h.row({s.formLabel(pr.conflict->key), planStatusName(pr.status), std::to_string(carried),
                   std::to_string(waiting), std::to_string(other), names(pr.masters), join(pr.problems, "; ")},
                  {"mono"});
        }
        h.tableEnd(planShown, plan->records.size());
        h.h2("Compatibility patch plan, change by change");
        h.tableStart({"Form", "Field", "Change", "Made by", "Class", "Plan", "Decision", "Note"});
        size_t entryShown = 0, entryTotal = 0;
        for (const auto& pr : plan->records)
            for (const auto& pe : pr.entries) {
                ++entryTotal;
                if (entryShown >= ctx.maxRows)
                    continue;
                ++entryShown;
                const auto& t = pr.conflict->changes[pe.change];
                std::string change = t.item.empty() ? t.before + " → " + t.after : std::string(changeOpName(t.op)) + " " + t.item;
                if (!t.position.empty())
                    change += " (" + t.position + ")";
                h.row({s.formLabel(pr.conflict->key), t.field, change, s.pluginName(t.source), compatClassName(t.cls),
                       planActionName(pe.action), planDecisionName(pe.decision), pe.note},
                      {"mono"});
            }
        h.tableEnd(entryShown, entryTotal);
    }
    return h.finish();
}

template <class W>
std::string assetReport(const ReportContext& ctx)
{
    W h("Asset Report", subtitle(ctx), ctx.style);
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
               !ar.archive               ? std::string("unreadable")
               : ar.archive->hasNames() ? formatCount(ar.archive->entries().size())
                                        : formatCount(ar.archive->entries().size()) + " (no name table: matched by hash)"});
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
        h.note("Each place a plugin's file name appears, classified by how it is used. The Dependency Report "
               "has the verdict per plugin.");
        h.tableStart({"Plugin", "Referenced in", "Provided by", "Type", "Use", "Classification"});
        size_t shown = 0;
        for (const auto& r : s.nameReferences) {
            if (shown++ >= ctx.maxRows)
                break;
            h.row({s.pluginName(r.plugin), r.assetPath, r.source, referenceKindName(r.kind), r.context,
                   referenceClassName(r.cls)},
                  {"", "mono", "", "", "", r.cls == ReferenceClass::Safe ? "ok" : ""});
        }
        h.tableEnd(std::min(shown, s.nameReferences.size()), s.nameReferences.size());
    }
    return h.finish();
}

template <class W>
std::string dependencyReport(const ReportContext& ctx)
{
    W h("Dependency Report", subtitle(ctx), ctx.style);
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

    // What each plugin that stays holds back (masters it keeps in the load order), most first.
    {
        std::vector<PluginIndex> roots;
        for (auto pi : s.loadOrder)
            if (!s.plugins[pi].holdsBack.empty() || s.plugins[pi].sharedHold)
                roots.push_back(pi);
        std::stable_sort(roots.begin(), roots.end(), [&](PluginIndex a, PluginIndex b) {
            return s.plugins[a].holdsBack.size() > s.plugins[b].holdsBack.size();
        });
        if (!roots.empty()) {
            h.h2("What plugins that stay hold back");
            h.note("A plugin that stays in the load order keeps its masters there too, and theirs. \"Would also "
                   "compile\" counts the plugins only it keeps: they would compile if it did (for example once its "
                   "record types are supported). \"Shared\" counts those it keeps together with other plugins that "
                   "stay. The ones with the most are where support for their record types gains the most.");
            h.tableStart({"Plugin", "Why it stays", "Would also compile", "Shared", "Plugins", "Master Redirection"});
            size_t shown = 0;
            for (auto pi : roots) {
                if (shown++ >= ctx.maxRows)
                    break;
                const auto& p = s.plugins[pi];
                std::vector<std::string> names;
                for (auto h2 : p.holdsBack)
                    names.push_back(s.plugins[h2].name);
                h.row({p.name, p.reasons.empty() ? std::string() : p.reasons.front(),
                       formatCount(p.holdsBack.size()), formatCount(p.sharedHold), join(names, ", "), p.redirectNote});
            }
            h.tableEnd(std::min(shown, roots.size()), roots.size());
        }
    }

    // Master Redirection: plugins that stay with a copy naming the outputs instead of their compiled masters.
    if (!s.redirected.empty() || !s.redirectFailed.empty()) {
        h.h2("Master Redirection");
        h.note("A plugin that stays in the load order normally keeps its masters there too. These plugins stay as a "
               "copy in the output, with the same name, that lists the output plugins instead of their compiled "
               "masters; every FormID naming a compiled record is translated. The copy loads after the outputs. "
               "Its own records keep their FormIDs; overrides the outputs or later plugins also change are left to "
               "them, and a cell holding its references is written as the game shows it today. The original file "
               "is never changed: switching the output mod off restores it.");
        h.tableStart({"Plugin", "Masters replaced", "FormIDs translated", "Own records", "Cells as today",
                      "Left to others", "Status"});
        size_t rows = 0;
        auto compiledEntry = [&](PluginIndex pi) -> const RedirectedPlugin* {
            if (!ctx.compiled)
                return nullptr;
            for (const auto& rp : ctx.compiled->redirected)
                if (rp.plugin == pi)
                    return &rp;
            return nullptr;
        };
        for (auto pi : s.redirected) {
            const auto* rp = compiledEntry(pi);
            std::vector<std::string> masters;
            for (auto m : s.plugins[pi].masters)
                if (m != kInvalidPlugin && s.plugins[m].eligibility == Eligibility::Eligible)
                    masters.push_back(s.plugins[m].name);
            const bool ok = rp && rp->failure.empty();
            h.row({s.plugins[pi].name, rp ? join(rp->replacedMasters, ", ") : join(masters, ", "),
                   ok ? formatCount(rp->translated) : std::string("-"), ok ? formatCount(rp->ownVersions) : std::string("-"),
                   ok ? formatCount(rp->winnerCopies) : std::string("-"), ok ? formatCount(rp->dropped) : std::string("-"),
                   !rp ? std::string("planned (compile to write the copy)") : ok ? std::string("redirected") : rp->failure});
            ++rows;
        }
        for (const auto& [key, why] : s.redirectFailed) {
            const auto* p = s.findPlugin(key);
            h.row({p ? p->name : key, "", "-", "-", "-", "-", "not redirected: " + why});
            ++rows;
        }
        h.tableEnd(rows, rows);
    }

    if (!s.referenceDecisions.empty()) {
        h.h2("Filename references");
        h.note("Every place a plugin's file name appears outside the plugin. The most restrictive classification "
               "decides; the number of references never does. Safe: unaffected. Rewritable: changed exactly by "
               "the Advanced Users Auto-Merger. Compatibility Required, Unsafe, Unknown: the plugin stays.");
        const size_t perPlugin = std::min<size_t>(ctx.maxRows, 200);
        for (auto pi : s.loadOrder) {
            auto it = s.referenceDecisions.find(pi);
            if (it == s.referenceDecisions.end())
                continue;
            const auto& d = it->second;
            h.h3("Plugin: " + s.plugins[pi].name);
            h.note("Filename references: " + std::to_string(d.total));
            h.tableStart({"Reference", "Type", "Use", "Location", "Classification", "Reason"});
            size_t shown = 0;
            for (const auto& r : s.nameReferences) {
                if (r.plugin != pi)
                    continue;
                if (shown < perPlugin) {
                    ++shown;
                    h.row({r.assetPath, referenceKindName(r.kind), r.context, r.location, referenceClassName(r.cls),
                           r.reason},
                          {"mono", "", "", "", r.cls == ReferenceClass::Safe ? "ok" : ""});
                }
            }
            h.tableEnd(shown, d.total);
            h.note(std::string("Overall classification: ") + referenceClassName(d.cls));
            h.note("Reason: " + d.reason);
            h.note("Recommended action: " + d.action);
        }
    }

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

template <class W>
std::string compilationReport(const ReportContext& ctx)
{
    W h("Compilation Report", subtitle(ctx), ctx.style);
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
             {"Merged Records", formatCount(c.mergedRecords)},
             {"Cells Copied", formatCount(c.containers)}});
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

    if (const auto* cmp = ctx.comparison) {
        h.h2("Changes since the previous build");
        h.cards({{"Keep their FormID", formatCount(cmp->kept)},
                 {"New", formatCount(cmp->added.size())},
                 {"Moved to another output", formatCount(cmp->moved.size())},
                 {"Renumbered", formatCount(cmp->renumbered.size())},
                 {"No longer in the build", formatCount(cmp->removed.size())},
                 {"Compiled for the first time", formatCount(cmp->newlyCompiled.size())},
                 {"Back in the load order", formatCount(cmp->returned.size())}});
        h.note(cmp->same() ? "Same as the previous build (" + cmp->previousSource +
                                 "): every record keeps its output plugin and FormID."
                           : "Compared with " + cmp->previousSource +
                                 ". Saves made with that build hold FormIDs: records that move, are renumbered or "
                                 "are no longer in the build are lost to them.");
        if (!cmp->newlyCompiled.empty() || !cmp->returned.empty() || !cmp->outputsAdded.empty() ||
            !cmp->outputsRemoved.empty()) {
            h.tableStart({"Plugin", "Change"});
            size_t n = 0;
            for (const auto& p : cmp->newlyCompiled)
                h.row({p, "Compiled for the first time"}), ++n;
            for (const auto& p : cmp->returned)
                h.row({p, "Back in the load order (no longer compiled)"}), ++n;
            for (const auto& p : cmp->outputsAdded)
                h.row({p, "New output plugin"}), ++n;
            for (const auto& p : cmp->outputsRemoved)
                h.row({p, "Output plugin no longer produced"}), ++n;
            h.tableEnd(n, n);
        }
        const size_t changes = cmp->moved.size() + cmp->renumbered.size() + cmp->removed.size() + cmp->added.size();
        if (changes) {
            h.tableStart({"Change", "Source", "Source FormID", "Type", "Editor ID", "Before", "After"});
            size_t shown = 0;
            auto at = [](const FormMapping& m) { return m.outputPlugin + " " + formatFormId(m.objectId); };
            auto row = [&](const char* what, const FormMapping& m, const std::string& before, const std::string& after) {
                if (shown++ < ctx.maxRows)
                    h.row({what, m.sourcePlugin, formatFormId(m.sourceObjectId), m.sig.str(), m.editorId, before, after},
                          {"", "", "mono", "", "", "mono", "mono"});
            };
            for (const auto& ch : cmp->moved)
                row("Moved", ch.after, at(ch.before), at(ch.after));
            for (const auto& ch : cmp->renumbered)
                row("Renumbered", ch.after, at(ch.before), at(ch.after));
            for (const auto& m : cmp->removed)
                row("No longer in the build", m, at(m), "");
            for (const auto& m : cmp->added)
                row("New", m, "", at(m));
            h.tableEnd(std::min(ctx.maxRows, changes), changes);
        }
    }

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

template <class W>
std::string validationReport(const ReportContext& ctx)
{
    W h("Validation Report", subtitle(ctx), ctx.style);
    const auto& v = *ctx.validation;
    h.cards({{"Checks", formatCount(v.checks)},
             {"Passed", formatCount(v.passed)},
             {"Warnings", formatCount(v.warnings)},
             {"Errors", formatCount(v.errors)}});
    h.note(v.critical() ? "Critical failures were found: compilation output must not be used."
                        : "No critical failures.");
    // Every category with its count first, so none is hidden by the row limit.
    {
        std::map<std::pair<int, std::string>, size_t> counts;
        for (const auto& is : v.issues)
            ++counts[{-static_cast<int>(is.severity), is.category}];
        h.h2("By category");
        h.tableStart({"Severity", "Category", "Rows"});
        for (const auto& [k, n] : counts)
            h.row({severityName(static_cast<Severity>(-k.first)), k.second, formatCount(n)},
                  {severityName(static_cast<Severity>(-k.first))});
        h.tableEnd(counts.size(), counts.size());
        h.h2("Findings");
    }
    h.tableStart({"Severity", "Category", "Plugin", "FormID", "Message", "Likely Cause"});
    std::vector<const ValidationIssue*> list;
    for (const auto& is : v.issues)
        list.push_back(&is);
    // Errors first, then the definition and group layout check (a few summary lines that must not
    // fall past the row limit behind thousands of decoding warnings), then the rest by severity.
    auto rank = [](const ValidationIssue* is) {
        if (is->severity == Severity::Error)
            return 0;
        if (is->category == "Definitions" || is->category == "Group Layout")
            return 1;
        return 4 - static_cast<int>(is->severity);
    };
    std::stable_sort(list.begin(), list.end(), [&](auto a, auto b) { return rank(a) < rank(b); });
    // At most a tenth of the rows per category, so one large category cannot hide the others.
    const size_t perCategory = std::max<size_t>(ctx.maxRows / 10, 50);
    std::map<std::string, size_t> perCat;
    size_t shown = 0;
    for (auto is : list) {
        if (shown >= ctx.maxRows)
            break;
        if (++perCat[is->category] > perCategory && is->severity != Severity::Error)
            continue;
        ++shown;
        h.row({severityName(is->severity), is->category, is->plugin, is->formId, is->message,
               is->cause.empty() ? diagnosis::forCategory(is->category, is->message) : is->cause},
              {severityName(is->severity), "", "", "mono"});
    }
    h.tableEnd(shown, list.size());
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

// Advanced Users Auto-Merger: which plugins named in scripts/configs can be relinked, and what was changed.
std::string pluginStatus(const ReportContext& ctx, PluginIndex p)
{
    const auto& am = *ctx.autoMerge;
    if (!am.relinkable.count(p))
        return "Kept out";
    if (am.keptOut.count(p))
        return "Kept out by a failed check";
    return ctx.compiled && ctx.compiled->isCompiled(p) ? "Compiled and relinked" : "Relinkable";
}

// Plugin name and FormID pairs (evidence only): the plugins kept out by nothing but pairs, and the verdict.
std::vector<PluginIndex> pairOnlyPlugins(const Session& s)
{
    std::vector<PluginIndex> out;
    for (const auto& [p, n] : s.pairEvidence.pairsNaming) {
        auto d = s.referenceDecisions.find(p);
        if (d == s.referenceDecisions.end() || d->second.cls != ReferenceClass::CompatibilityRequired)
            continue;
        if (d->second.counts[static_cast<size_t>(ReferenceClass::CompatibilityRequired)] <= n)
            out.push_back(p);
    }
    return out;
}

std::string pairVerdict(const PairEvidence& ev)
{
    if (!ev.scanned)
        return "Not scanned";
    if (!ev.pairs)
        return "No pairs in the load order";
    if (ev.clean())
        return "Clean: every read keeps working when a pair is rewritten";
    std::string why = std::to_string(ev.notOk()) + " reads in a use a rewrite might not keep";
    if (!ev.unreadable.empty())
        why += ", " + std::to_string(ev.unreadable.size()) + " scripts could not be read";
    return "Not clean: " + why;
}

template <class W>
void pairEvidenceSection(W& h, const ReportContext& ctx)
{
    const auto& s = *ctx.session;
    const auto& ev = s.pairEvidence;
    if (!ev.scanned || (!ev.pairs && ev.reads.empty()))
        return;
    h.h2("Plugin name and FormID pairs (evidence, report only)");
    const auto only = pairOnlyPlugins(s);
    h.cards({{"Pairs in Records", formatCount(ev.pairs)},
             {"Plugins Named", formatCount(ev.pairsNaming.size())},
             {"Kept Out Only by Pairs", formatCount(only.size())},
             {"Script Reads", formatCount(ev.reads.size())},
             {"Reads Not Kept", formatCount(ev.notOk())}});
    h.note("Script properties holding a struct with sPluginName and iFormID (as Sim Settlements 2 writes them) keep "
           "the plugin they name out of the build. Rewriting a pair (the name and the FormID together) is safe only "
           "if every script uses the two members for nothing but Game.GetFormFromFile(iFormID, sPluginName) of the "
           "same struct, Game.IsPluginInstalled or logging. This section lists every read found; nothing here "
           "changes what is compiled.");
    h.note("Verdict: " + pairVerdict(ev));
    h.tableStart({"Script property", "Pairs", "Plugins named", "In plugins"});
    size_t shown = 0;
    for (const auto& [holder, x] : ev.holders) {
        if (shown++ >= ctx.maxRows)
            break;
        h.row({holder, formatCount(x.pairs), formatCount(x.named.size()), formatCount(x.inPlugins.size())}, {"mono"});
    }
    h.tableEnd(std::min(shown, ev.holders.size()), ev.holders.size());
    if (!only.empty()) {
        h.tableStart({"Plugin kept out only by pairs", "Pairs naming it"});
        shown = 0;
        for (PluginIndex p : only)
            if (shown++ < ctx.maxRows)
                h.row({s.pluginName(p), formatCount(ev.pairsNaming.at(p))});
        h.tableEnd(std::min(shown, only.size()), only.size());
    }
    // Reads, the ones a rewrite might not keep first, one row per place and use.
    std::map<std::tuple<bool, std::string, std::string, std::string, std::string>, size_t> grouped;
    for (const auto& r : ev.reads)
        ++grouped[{r.ok, r.script, r.where, r.member, r.use}];
    h.tableStart({"Script", "Function", "Member", "Use", "Count", "Kept"});
    shown = 0;
    for (const auto& [k, n] : grouped) {
        if (shown++ >= ctx.maxRows)
            break;
        const auto& [ok, script, where, member, use] = k;
        h.row({script, where, member, use, formatCount(n), ok ? "Yes" : "No"},
              {"mono", "mono", "", "", "", ok ? "ok" : "Warning"});
    }
    h.tableEnd(std::min(shown, grouped.size()), grouped.size());
    if (!ev.unreadable.empty()) {
        h.tableStart({"Script naming a member that could not be read"});
        for (const auto& u : ev.unreadable)
            h.row({u}, {"Warning"});
        h.tableEnd(ev.unreadable.size(), ev.unreadable.size());
    }
}

template <class W>
std::string autoMergerReport(const ReportContext& ctx)
{
    W h("Advanced Users Auto-Merger Report", subtitle(ctx), ctx.style);
    const auto& s = *ctx.session;
    const auto& am = *ctx.autoMerge;
    size_t written = 0;
    for (const auto& f : am.files)
        written += f.written;
    h.cards({{"Files Naming Plugins", formatCount(am.files.size())},
             {"Plugins Named", formatCount(am.filesOf.size())},
             {"Relinkable Plugins", formatCount(am.relinkable.size())},
             {"Files Relinked", formatCount(written)},
             {"Relink Errors", formatCount(am.errors.size())}});
    h.note("A plugin whose file name appears in other files is compiled only when every such reference is Safe or "
           "Rewritable. Exactly the Rewritable references are changed, in copies written into the output folder "
           "that override the originals; each copy is scanned again and checked before anything is written, and a "
           "plugin whose copy fails a check stays in the load order. Multiplexer_Relinked.tsv lists every change.");

    h.h2("Plugins");
    h.tableStart({"Plugin", "Files", "Status", "Reason"});
    size_t shown = 0;
    for (const auto& [p, files] : am.filesOf) {
        if (shown++ >= ctx.maxRows)
            break;
        std::string reason;
        for (size_t i : files)
            if (!am.files[i].problem.empty()) {
                reason = am.files[i].path + ": " + am.files[i].problem;
                break;
            }
        if (auto k = am.keptOut.find(p); k != am.keptOut.end())
            reason = k->second;
        std::string status = pluginStatus(ctx, p);
        h.row({s.pluginName(p), formatCount(files.size()), status, reason},
              {"", "", status.rfind("Kept out", 0) == 0 ? "Warning" : "ok", ""});
    }
    h.tableEnd(std::min(shown, am.filesOf.size()), am.filesOf.size());

    h.h2("Files");
    h.tableStart({"File", "Mod", "Kind", "Plugins", "Classification", "GetFormFromFile", "Pairs", "Presence Checks",
                  "Result"});
    shown = 0;
    for (const auto& f : am.files) {
        if (shown++ >= ctx.maxRows)
            break;
        std::vector<std::string> names;
        for (PluginIndex p : f.plugins)
            names.push_back(s.pluginName(p));
        std::string result;
        if (!f.problem.empty())
            result = "Not relinkable: " + f.problem;
        else if (!f.validation.empty())
            result = "Check failed, plugins kept out: " + f.validation;
        else if (f.cls == ReferenceClass::Safe)
            result = "Nothing to change (safe)";
        else if (f.written)
            result = "Relinked: " + std::to_string(f.changedCalls) + " calls, " + std::to_string(f.changedPairs) +
                     " pairs, " + std::to_string(f.renamed) + " names";
        else
            result = "Relinkable";
        for (const auto& n : f.notes)
            result += "; " + n;
        h.row({f.path, f.mod, relinkKindName(f.kind), join(names, ", "), referenceClassName(f.cls),
               std::to_string(f.formCalls), std::to_string(f.pairs), std::to_string(f.mentions), result},
              {"mono", "", "", "", f.problem.empty() && f.validation.empty() ? "ok" : "Warning"});
    }
    h.tableEnd(std::min(shown, am.files.size()), am.files.size());

    size_t changes = 0;
    for (const auto& f : am.files)
        changes += f.planned ? f.changes.size() : 0;
    if (changes) {
        h.h2("Changes");
        h.tableStart({"File", "Plugin", "Location", "Use", "Before", "After"});
        shown = 0;
        for (const auto& f : am.files)
            for (const auto& c : f.planned ? f.changes : std::vector<RelinkChange>{})
                if (shown++ < ctx.maxRows)
                    h.row({f.path, s.pluginName(c.plugin), c.location, c.context, c.before, c.after}, {"mono"});
        h.tableEnd(std::min(shown, changes), changes);
    }
    pairEvidenceSection(h, ctx);
    if (!am.errors.empty()) {
        h.h2("Relink errors");
        h.tableStart({"File and error"});
        for (const auto& e : am.errors)
            h.row({e}, {"Error"});
        h.tableEnd(am.errors.size(), am.errors.size());
    }
    return h.finish();
}

} // namespace

std::string ReportGenerator::html(ReportKind kind, const ReportContext& ctx)
{
    switch (kind) {
    case ReportKind::Conflict: return ctx.session && ctx.conflicts ? conflictReport<Html>(ctx) : std::string();
    case ReportKind::Asset: return ctx.session ? assetReport<Html>(ctx) : std::string();
    case ReportKind::Dependency: return ctx.session ? dependencyReport<Html>(ctx) : std::string();
    case ReportKind::Compilation: return ctx.session && ctx.compiled ? compilationReport<Html>(ctx) : std::string();
    case ReportKind::Validation: return ctx.validation ? validationReport<Html>(ctx) : std::string();
    case ReportKind::AutoMerger: return ctx.session && ctx.autoMerge ? autoMergerReport<Html>(ctx) : std::string();
    }
    return {};
}

std::string ReportGenerator::text(ReportKind kind, const ReportContext& ctx)
{
    switch (kind) {
    case ReportKind::Conflict: return ctx.session && ctx.conflicts ? conflictReport<Text>(ctx) : std::string();
    case ReportKind::Asset: return ctx.session ? assetReport<Text>(ctx) : std::string();
    case ReportKind::Dependency: return ctx.session ? dependencyReport<Text>(ctx) : std::string();
    case ReportKind::Compilation: return ctx.session && ctx.compiled ? compilationReport<Text>(ctx) : std::string();
    case ReportKind::Validation: return ctx.validation ? validationReport<Text>(ctx) : std::string();
    case ReportKind::AutoMerger: return ctx.session && ctx.autoMerge ? autoMergerReport<Text>(ctx) : std::string();
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
        counts.set("changesNotInWinner", static_cast<uint64_t>(c.lostChanges));
        counts.set("notInWinnerGreen", static_cast<uint64_t>(c.lostByClass[0]));
        counts.set("notInWinnerYellow", static_cast<uint64_t>(c.lostByClass[1]));
        counts.set("notInWinnerRed", static_cast<uint64_t>(c.lostByClass[2]));
        counts.set("notInWinnerUnknown", static_cast<uint64_t>(c.lostByClass[3]));
        root.set("counts", counts);
        json::Value list;
        for (const auto& e : c.entries) {
            json::Value o;
            o.set("formId", s->loadOrderFormId(e.key));
            o.set("signature", e.sig.str());
            o.set("editorId", e.editorId);
            o.set("type", static_cast<int>(e.type));
            o.set("typeName", conflictTypeName(e.type));
            o.set("merged", e.fieldsMerged());
            o.set("handling", mergeHandlingName(e.handling));
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
            // Change provenance: every change a version made against the one it was made from.
            json::Value changes;
            for (const auto& t : e.changes) {
                json::Value to;
                to.set("id", t.changeId);
                to.set("field", t.field);
                if (!t.item.empty())
                    to.set("element", t.item);
                to.set("operation", changeOpName(t.op));
                to.set("before", t.before);
                to.set("after", t.after);
                if (!t.position.empty())
                    to.set("position", t.position);
                to.set("madeBy", s->pluginName(t.source));
                to.set("status", changeStatusName(t.status));
                if (t.lostAt != kInvalidPlugin)
                    to.set("lostAt", s->pluginName(t.lostAt));
                json::Value kept, competing;
                for (auto p : t.keptBy)
                    kept.push(s->pluginName(p));
                for (auto p : t.competesWith)
                    competing.push(s->pluginName(p));
                if (!t.keptBy.empty())
                    to.set("keptBy", kept);
                if (!t.competesWith.empty())
                    to.set("competesWith", competing);
                if (t.duplicateOf != kInvalidPlugin)
                    to.set("alsoAddedBy", s->pluginName(t.duplicateOf));
                if (t.opaque)
                    to.set("notUnderstood", true);
                to.set("class", compatClassName(t.cls));
                to.set("kind", compatKindName(t.kind));
                to.set("rule", t.rule);
                if (!t.requiredMasters.empty()) {
                    json::Value req;
                    for (auto p : t.requiredMasters)
                        req.push(s->pluginName(p));
                    to.set("requiredMasters", req);
                }
                changes.push(to);
            }
            o.set("changes", changes);
            list.push(o);
        }
        root.set("conflicts", list);
        if (const auto* plan = ctx.plan) {
            json::Value po;
            po.set("carried", static_cast<uint64_t>(plan->carried));
            po.set("waiting", static_cast<uint64_t>(plan->pending));
            po.set("keptAsWinner", static_cast<uint64_t>(plan->kept));
            po.set("neverCarried", static_cast<uint64_t>(plan->locked));
            json::Value masters;
            for (auto p : plan->masters)
                masters.push(s->pluginName(p));
            po.set("extraMasters", masters);
            json::Value records;
            for (const auto& pr : plan->records) {
                json::Value ro;
                ro.set("formId", s->loadOrderFormId(pr.conflict->key));
                ro.set("record", PlanDecisions::recordId(*s, pr.conflict->key));
                ro.set("status", planStatusName(pr.status));
                json::Value entries;
                for (const auto& pe : pr.entries) {
                    const auto& t = pr.conflict->changes[pe.change];
                    json::Value eo;
                    eo.set("change", t.changeId);
                    eo.set("field", t.field);
                    eo.set("madeBy", s->pluginName(t.source));
                    eo.set("class", compatClassName(t.cls));
                    eo.set("plan", planActionName(pe.action));
                    eo.set("decision", planDecisionName(pe.decision));
                    if (!pe.note.empty())
                        eo.set("note", pe.note);
                    entries.push(eo);
                }
                ro.set("changes", entries);
                json::Value problems;
                for (const auto& p : pr.problems)
                    problems.push(p);
                if (!pr.problems.empty())
                    ro.set("problems", problems);
                records.push(ro);
            }
            po.set("records", records);
            root.set("patchPlan", po);
        }
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
            o.set("type", referenceKindName(r.kind));
            o.set("use", r.context);
            o.set("classification", referenceClassName(r.cls));
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
            const auto& p = s->plugins[pi];
            if (!p.holdsBack.empty() || p.sharedHold || p.heldBy != kInvalidPlugin) {
                json::Value held;
                for (auto h2 : p.holdsBack)
                    held.push(s->plugins[h2].name);
                o.set("holdsBack", held);
                o.set("sharedHold", static_cast<uint64_t>(p.sharedHold));
                if (p.heldBy != kInvalidPlugin)
                    o.set("heldBy", s->plugins[p.heldBy].name);
            }
            plugins.push(o);
        }
        root.set("plugins", plugins);
        json::Value decisions;
        for (auto pi : s->loadOrder) {
            auto it = s->referenceDecisions.find(pi);
            if (it == s->referenceDecisions.end())
                continue;
            json::Value o;
            o.set("plugin", s->plugins[pi].name);
            o.set("filenameReferences", static_cast<uint64_t>(it->second.total));
            o.set("classification", referenceClassName(it->second.cls));
            o.set("reason", it->second.reason);
            o.set("recommendedAction", it->second.action);
            json::Value refs;
            for (const auto& r : s->nameReferences) {
                if (r.plugin != pi)
                    continue;
                json::Value x;
                x.set("file", r.assetPath);
                x.set("source", r.source);
                x.set("type", referenceKindName(r.kind));
                x.set("format", r.format);
                x.set("text", r.text);
                x.set("use", r.context);
                x.set("location", r.location);
                x.set("classification", referenceClassName(r.cls));
                x.set("reason", r.reason);
                refs.push(x);
            }
            o.set("references", refs);
            decisions.push(o);
        }
        root.set("filenameReferences", decisions);
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
        if (const auto* cmp = ctx.comparison) {
            json::Value o;
            o.set("previous", cmp->previousSource);
            o.set("kept", static_cast<uint64_t>(cmp->kept));
            o.set("added", static_cast<uint64_t>(cmp->added.size()));
            o.set("moved", static_cast<uint64_t>(cmp->moved.size()));
            o.set("renumbered", static_cast<uint64_t>(cmp->renumbered.size()));
            o.set("removed", static_cast<uint64_t>(cmp->removed.size()));
            json::Value first, back;
            for (const auto& n : cmp->newlyCompiled)
                first.push(n);
            for (const auto& n : cmp->returned)
                back.push(n);
            o.set("compiledForTheFirstTime", first);
            o.set("backInTheLoadOrder", back);
            root.set("comparedWithPreviousBuild", o);
        }
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
    case ReportKind::AutoMerger: {
        if (!s || !ctx.autoMerge)
            return {};
        const auto& am = *ctx.autoMerge;
        json::Value plugins;
        for (const auto& [p, files] : am.filesOf) {
            json::Value o;
            o.set("plugin", s->pluginName(p));
            o.set("files", static_cast<uint64_t>(files.size()));
            o.set("status", pluginStatus(ctx, p));
            if (auto k = am.keptOut.find(p); k != am.keptOut.end())
                o.set("reason", k->second);
            plugins.push(o);
        }
        root.set("plugins", plugins);
        {
            const auto& ev = s->pairEvidence;
            json::Value pe;
            pe.set("verdict", pairVerdict(ev));
            pe.set("clean", ev.clean());
            pe.set("pairs", static_cast<uint64_t>(ev.pairs));
            json::Value holders;
            for (const auto& [holder, x] : ev.holders) {
                json::Value o;
                o.set("property", holder);
                o.set("pairs", static_cast<uint64_t>(x.pairs));
                o.set("pluginsNamed", static_cast<uint64_t>(x.named.size()));
                holders.push(o);
            }
            pe.set("holders", holders);
            json::Value only;
            for (PluginIndex p : pairOnlyPlugins(*s))
                only.push(s->pluginName(p));
            pe.set("keptOutOnlyByPairs", only);
            json::Value reads;
            for (const auto& r : ev.reads) {
                json::Value o;
                o.set("script", r.script);
                o.set("where", r.where);
                o.set("member", r.member);
                o.set("object", r.object);
                o.set("use", r.use);
                o.set("kept", r.ok);
                reads.push(o);
            }
            pe.set("reads", reads);
            json::Value unreadable;
            for (const auto& u : ev.unreadable)
                unreadable.push(u);
            pe.set("unreadable", unreadable);
            root.set("pairEvidence", pe);
        }
        json::Value files;
        for (const auto& f : am.files) {
            json::Value o;
            o.set("path", f.path);
            o.set("mod", f.mod);
            o.set("kind", relinkKindName(f.kind));
            json::Value names;
            for (PluginIndex p : f.plugins)
                names.push(s->pluginName(p));
            o.set("plugins", names);
            o.set("getFormFromFileCalls", static_cast<uint64_t>(f.formCalls));
            o.set("pairs", static_cast<uint64_t>(f.pairs));
            o.set("mentions", static_cast<uint64_t>(f.mentions));
            o.set("classification", referenceClassName(f.cls));
            o.set("problem", f.problem);
            o.set("checkFailed", f.validation);
            json::Value changes;
            for (const auto& c : f.changes) {
                json::Value x;
                x.set("plugin", s->pluginName(c.plugin));
                x.set("location", c.location);
                x.set("use", c.context);
                x.set("before", c.before);
                x.set("after", c.after);
                changes.push(x);
            }
            o.set("changes", changes);
            o.set("relinked", f.written);
            o.set("changedCalls", static_cast<uint64_t>(f.changedCalls));
            o.set("changedPairs", static_cast<uint64_t>(f.changedPairs));
            o.set("renamed", static_cast<uint64_t>(f.renamed));
            json::Value notes;
            for (const auto& n : f.notes)
                notes.push(n);
            o.set("notes", notes);
            files.push(o);
        }
        root.set("files", files);
        json::Value errors;
        for (const auto& e : am.errors)
            errors.push(e);
        root.set("errors", errors);
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
                      ReportKind::Validation, ReportKind::AutoMerger}) {
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
        auto tp = dir / (base + ".txt");
        std::ofstream(tp, std::ios::binary) << text(kind, ctx);
        written.push_back(tp);
    }
    return written;
}

} // namespace fo4mx
