// fo4mx - command line front end for Fallout 4 Multiplexer Renewed.

#include "diagnostics/RecorderLog.hpp"
#include "diagnostics/RecorderPackage.hpp"
#include "engine/Exporter.hpp"
#include "ba2/Ba2Archive.hpp"
#include "engine/Pipeline.hpp"
#include "engine/SwitchBack.hpp"
#include "engine/SupportBundle.hpp"
#include "engine/BuildComparison.hpp"
#include <set>
#include "diagnostics/CrashLog.hpp"
#include "diagnostics/PapyrusLog.hpp"
#include "diagnostics/SaveGame.hpp"
#include "diagnostics/StartupCheck.hpp"
#include "util/FileTransaction.hpp"
#include "reference/FormIdReference.hpp"
#include "engine/ReferenceIndex.hpp"
#include "engine/OutputRuns.hpp"
#include "esp/FieldValues.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/GroupLayout.hpp"
#include "esp/PluginFile.hpp"
#include "util/Log.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <limits>
#include <unordered_set>
#include <vector>

using namespace fo4mx;

namespace {

std::filesystem::path g_exeDir; // folder of fo4mx(.exe): MultiplexerDiagnosticRecorder.dll ships beside it

const char* kUsage = R"(Fallout 4 Multiplexer Renewed - load order compiler

Usage:
  fo4mx status   <install> [--db FILE]
  fo4mx scan     <install> [--db FILE]
  fo4mx analyse  <install> [--db FILE] [--reports DIR]
  fo4mx plan     <install> [--db FILE] [carry|keep|default <Plugin.esp:OBJECTID> <change id>]
                 the compatibility patch plan: what a patch would carry of the
                 changes the winning versions lose, and what waits for your
                 decision. With carry/keep/default, records your choice for one
                 change (ids as listed, or from the Conflict Report's JSON) first.
                 Choices are saved next to the database. Nothing is written
  fo4mx compile  <install> [--db FILE] [compile options] [--dry-run]
  fo4mx check-definitions <install> [--types ARMA,MGEF,...]
                 decode every record of the supported types in the load order
                 and report layouts that look wrong
  fo4mx record-types [--runtime oldgen|nextgen]
                 list every Fallout 4 record type with its category, placement
                 and how the compiler handles it (tab separated)
  fo4mx info     <plugin.esp>
  fo4mx dump     <plugin.esp> [--formid XXXXXXXX | --cell XXXXXXXX] [--limit N] [--values] [--tree]
                 --cell: the cell and every record in it (FormIDs as the plugin writes them)
                 --values: every field by name and value (where defined), else bytes
                 --tree: the group / record tree, as xEdit shows it
  fo4mx references <install> <plugin> <formid>
                 every version of every record in the load order that refers to
                 the record (xEdit's "Referenced By")
  fo4mx ba2      <archive.ba2> [--extract PATH --to FILE]
  fo4mx clean-outputs <install> [--keep N] [--unfinished]
  fo4mx switch-back <install> [--yes]       undo the last Apply to Game from its backups
  fo4mx compare-builds <earlier export> <later export>
                                            what the later build changes: records new, moved,
                                            renumbered or gone (FormIDs saves lose), plugins
                                            compiled for the first time or back in the load order
  fo4mx support-bundle <install> [--out FILE.zip]
                                            one zip to send with a problem report: the latest
                                            export's reports and records, logs, load order, the
                                            newest crash log and its reading, recorder log. No mod
                                            files; home folder and user name taken out
  fo4mx ready    <install> [--save save.fos]
                                            before Apply to Game: validation, export, plugin
                                            slots, the newest save, F4SE and a crash logger
                                            (the latest export, or compiles when it does not fit)
  fo4mx check-save <install> [save.fos] [--next]
                                            whether a save would lose anything: its plugins against
                                            the load order now, or with --next after compiling the
                                            next build (analyses and compiles; nothing is written)
  fo4mx papyrus-log <install> [Papyrus.0.log] [--limit N]
                                            the Papyrus log read with the current load order: the
                                            original mod behind every compiled record it names
  fo4mx startup-check <install> [Papyrus.0.log] [--mcm-log MCM.log] [--build <export>] [--f4se-log f4se.log]
                                            startup popups and MCM menus: which start-game quests of
                                            the latest build started or could not, popups that found
                                            no message, and the MCM menus whose requirement was
                                            relinked. With --build and --f4se-log (the load order the
                                            game had) no scan is needed and <install> may be "none"
  fo4mx crash-log [Crash-*.log] [--formmap <export>\\Data\\Multiplexer_FormMap.tsv]
                                            name the original mods behind a Buffout 4 crash log
                 delete exports in the output folder beyond the newest N (default
                 5; never the latest), and with --unfinished also unfinished ones
                 (".tmp"). Only Multiplexer's own export folders; one in use is
                 skipped and left as it is
  fo4mx formid   <plugin> <formid>
                 look up a base-game record (Fallout4.esm, the DLC masters) in
                 the bundled reference: its Editor ID, without scanning anything.
                 <formid>: hex, the master index is ignored (01000800 = 000800)
  fo4mx diagnose [session.jsonl] [--package DIR] [--runtime oldgen|nextgen]
                 read a Diagnostic Recorder log (default: the newest in
                 Documents\My Games\Fallout4\F4SE\MultiplexerDiagnosticRecorder) and
                 list what it recognises: likely causes, evidence, checks.
                 --package: the "Multiplexer Output" folder, to compare the
                 game's load order with the one Multiplexer built for
  fo4mx diagnose --install [--game DIR]
                 check the recorder itself: is the DLL in Data\F4SE\Plugins,
                 did F4SE load it (f4se.log), and its own status log

Install selection:
  --mo2 DIR            Mod Organizer 2 instance folder (contains ModOrganizer.ini)
  --profile NAME       MO2 profile (default: the selected profile)
  --vortex             Vortex-managed install (reads vortex.deployment.json)
  --game DIR           Fallout 4 folder (required for Vortex / standalone)
  --plugins FILE       plugins.txt to use
  --ini-dir DIR        folder containing Fallout4.ini / Fallout4Custom.ini
  --lang CODE          string table language (default: en)
  --runtime oldgen|nextgen   (default: oldgen = 1.10.163)

Compile options:
  --outputs N|auto     most output plugins to use, 1-64 (default 20); auto: the fewest that fit
  --prefix NAME        output plugin name prefix (default Dummy_Plugin_)
  --strategy type|balanced|user   allocation strategy (default balanced)
  --rule PATTERN=N     strategy "user": send records whose signature or plugin
                       matches PATTERN to output N (repeatable)
  --no-merge           last plugin wins instead of merging compatible edits
  --light              ESL-flag outputs that fit the light plugin range
  --light-headroom N   new records a new light output leaves free (default 200)
  --no-previs-translation  do not write translated previs files (only the export changes: the same
                       checks run and the same plugins compile; for testing)
  --apply-patch-plan   write the records the compatibility patch plan marks Ready (see "plan") as
                       their winner plus the planned changes (to the patch when only plugins
                       that stay hold them)
  --merge-cell-config  cells and worldspaces several plugins change take the last real change of each
                       configuration field (lighting, location, water, regions...) and keep the
                       winner's previs data; those whose winner stays go to the patch
  --broken-references-as-null  write references that cannot be written faithfully (missing master,
                       or a record a compiled plugin does not contain) as null and compile their
                       plugin (lossy); default: that plugin stays in the load order
  --no-new-quests-npcs  keep plugins that add new quests, dialogue or NPCs in the load order
                       (overrides of those still compile)
  --master-placeholders  compile masters that plugins staying in the load order list but use none
                       of the records of, keeping their names with empty placeholder plugins
                       (Mod Organizer 2 output inside the mods folder only)
  --accept-formid-changes  compile even though records of the build in use would get other FormIDs
                       (saves made with it lose them); otherwise compiling stops
  --redirect-masters   Master Redirection: a plugin that stays gets a copy in the output listing
                       the outputs instead of its compiled masters, so they compile (MO2, output
                       inside the mods folder)
  --no-redirect PLUGIN keep this plugin as it is with --redirect-masters (repeatable)
  --allow-return PLUGIN  a plugin the last build compiled may return to the load order when this
                       build cannot compile it (repeatable); otherwise compiling stops and names it
  --patch              also write Multiplexer_Patch.esp, merging conflicts between
                       plugins that stay in the load order (like a Bashed Patch)
  --patch-only         write only the patch; compile no plugins
  --auto-merge         Advanced Users Auto-Merger: also compile plugins named in
                       scripts/configs that can be relinked, and write relinked copies
  --exclude PLUGIN     never compile PLUGIN (repeatable)
  --only PLUGIN        compile only the plugins named (repeatable); every other plugin stays in the
                       load order
  --out DIR            folder holding the exports (default: <game>\\Multiplexer Output); every export
                       goes into "Multiplexer Output DDMMYY HHMMSS" in it, with Multiplexer Output.zip
  --reports DIR        report folder
  --archive-loader  load compiled plugins' archives with the F4SE archive loader (shipped in
                 the package; no 1,023-character limit; the game must start through F4SE)
  --diagnostic-recorder  also prepare the optional Diagnostic Recorder (F4SE plugin) beside
                       the package, in "Multiplexer Output\Diagnostic Recorder"
  --dry-run            compile and validate without writing files
  --no-cache           ignore the build cache (full rescan, analysis and merge)
  --scan-scope game|preview-inactive   game (default): read the plugins the game loads; preview-inactive:
                       also read inactive plugins apart from the load order and say what each would
                       bring if enabled (never part of the build)

  -v, --verbose        debug logging
)";

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::multimap<std::string, std::string> options;
    bool has(const std::string& k) const { return options.count(k) != 0; }
    std::string get(const std::string& k, const std::string& def = {}) const
    {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
    std::vector<std::string> all(const std::string& k) const
    {
        std::vector<std::string> out;
        auto [b, e] = options.equal_range(k);
        for (auto it = b; it != e; ++it)
            out.push_back(it->second);
        return out;
    }
};

Args parseArgs(int argc, char** argv)
{
    static const std::vector<std::string> flags = {"--vortex", "--no-merge", "--light", "--no-previs-translation", "--merge-cell-config", "--apply-patch-plan", "--broken-references-as-null", "--master-placeholders", "--redirect-masters", "--no-new-quests-npcs", "--accept-formid-changes", "--patch", "--patch-only", "--auto-merge", "--dry-run", "--no-cache",
                                                   "--diagnostic-recorder", "--install", "--archive-loader",
                                                   "--values", "--tree", "--help", "--unfinished",
                                                   "--yes", "--verbose", "-v"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("-", 0) == 0) {
            if (std::find(flags.begin(), flags.end(), s) != flags.end()) {
                a.options.emplace(s, "1");
            } else if (i + 1 < argc) {
                a.options.emplace(s, argv[++i]);
            } else {
                throw std::runtime_error("missing value for " + s);
            }
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

class ConsoleProgress : public ProgressSink {
public:
    void stage(std::string_view name) override
    {
        clearLine();
        std::fprintf(stderr, "== %.*s\n", static_cast<int>(name.size()), name.data());
    }
    void progress(uint64_t done, uint64_t total, std::string_view detail) override
    {
        if (total == 0)
            return;
        int pct = static_cast<int>(done * 100 / total);
        if (pct == last_ && done != total)
            return;
        last_ = pct;
        std::fprintf(stderr, "\r   %3d%%  %-60.60s", pct, std::string(detail).c_str());
        if (done == total) {
            std::fprintf(stderr, "\n");
            last_ = -1;
        }
    }

private:
    void clearLine()
    {
        if (last_ >= 0)
            std::fprintf(stderr, "\n");
        last_ = -1;
    }
    int last_ = -1;
};

PipelineConfig makeConfig(const Args& a)
{
    PipelineConfig cfg;
    auto& in = cfg.install;
    if (a.has("--mo2")) {
        in.kind = InstallKind::ModOrganizer2;
        in.mo2Path = utf8ToPath(a.get("--mo2"));
        in.mo2Profile = a.get("--profile");
    } else if (a.has("--vortex")) {
        in.kind = InstallKind::Vortex;
    } else {
        in.kind = InstallKind::Standalone;
    }
    if (a.has("--game"))
        in.gamePath = utf8ToPath(a.get("--game"));
    if (a.has("--plugins"))
        in.pluginsFile = utf8ToPath(a.get("--plugins"));
    if (a.has("--ini-dir"))
        in.iniDirectory = utf8ToPath(a.get("--ini-dir"));
    in.language = a.get("--lang", "en");
    in.runtime = a.get("--runtime", "oldgen") == "nextgen" ? RuntimeProfile::NextGen : RuntimeProfile::OldGen_1_10_163;

    if (a.has("--db"))
        cfg.databasePath = utf8ToPath(a.get("--db"));
    if (a.has("--reports"))
        cfg.reportDirectory = utf8ToPath(a.get("--reports"));
    if (a.has("--out"))
        cfg.exportOptions.outputDirectory = utf8ToPath(a.get("--out"));

    auto& c = cfg.compile;
    const std::string outputs = a.get("--outputs", "20");
    c.autoOutputs = iequals(outputs, "auto");
    c.outputCount = c.autoOutputs ? CompileOptions::kMaxOutputs : std::stoi(outputs);
    if (c.outputCount < CompileOptions::kMinOutputs || c.outputCount > CompileOptions::kMaxOutputs)
        throw std::runtime_error("--outputs must be between " + std::to_string(CompileOptions::kMinOutputs) + " and " +
                                 std::to_string(CompileOptions::kMaxOutputs));
    c.outputPrefix = a.get("--prefix", c.outputPrefix);
    auto strategy = a.get("--strategy", "balanced");
    c.strategy = strategy == "type"   ? AllocationStrategy::ByRecordType
                 : strategy == "user" ? AllocationStrategy::UserDefined
                                      : AllocationStrategy::Balanced;
    for (const auto& r : a.all("--rule")) {
        auto eq = r.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("--rule expects PATTERN=N");
        c.rules.push_back({r.substr(0, eq), std::stoi(r.substr(eq + 1))});
    }
    c.mergeCompatible = !a.has("--no-merge");
    c.lightOutputsWhenPossible = a.has("--light");
    if (a.has("--light-headroom"))
        c.lightHeadroom = static_cast<uint32_t>(std::stoul(a.get("--light-headroom")));
    c.translatePrevis = !a.has("--no-previs-translation");
    c.mergeCellConfig = a.has("--merge-cell-config");
    c.applyPatchPlan = a.has("--apply-patch-plan");
    c.brokenReferencesAsNull = a.has("--broken-references-as-null");
    c.unusedMasterPlaceholders = a.has("--master-placeholders");
    c.redirectMasters = a.has("--redirect-masters");
    c.noRedirect = a.all("--no-redirect");
    c.compileNewQuestsAndNpcs = !a.has("--no-new-quests-npcs");
    c.allowReturn = a.all("--allow-return");
    c.acceptFormIdChanges = a.has("--accept-formid-changes");
    c.patchOnly = a.has("--patch-only");
    c.autoMerge = a.has("--auto-merge");
    c.buildPatch = c.patchOnly || a.has("--patch");
    cfg.analysis.excludedPlugins = a.all("--exclude");
    cfg.analysis.selectedPlugins = a.all("--only");
    cfg.analysis.onlySelected = !cfg.analysis.selectedPlugins.empty();
    cfg.diagnosticRecorder = a.has("--diagnostic-recorder");
    cfg.diagnosticRecorderDll = g_exeDir / diagnostics::kRecorderDllName;
    cfg.f4seArchiveLoader = a.has("--archive-loader");
    cfg.archiveLoaderDll = g_exeDir / Exporter::kArchiveLoaderDll;
    cfg.formIdReference = g_exeDir / FormIdReference::kFileName;
    cfg.useCache = !a.has("--no-cache");
    {
        const std::string scope = a.get("--scan-scope", "game");
        if (scope != "game" && scope != "preview-inactive")
            throw std::runtime_error("--scan-scope must be game or preview-inactive");
        cfg.scan.previewInactive = scope == "preview-inactive";
    }
    return cfg;
}

void printDashboard(const Pipeline& p)
{
    const auto& s = p.session();
    size_t esm = 0, esp = 0, esl = 0;
    for (auto pi : s.loadOrder) {
        const auto& pl = s.plugins[pi];
        if (pl.isLight())
            ++esl;
        else if (pl.isMaster())
            ++esm;
        else
            ++esp;
    }
    std::printf("\n");
    std::printf("  Mods installed      %s\n", formatCount(s.install.enabledModCount()).c_str());
    std::printf("  Plugins (active)    %s of %s\n", formatCount(s.loadOrder.size()).c_str(),
                formatCount(s.plugins.size()).c_str());
    std::printf("  ESM / ESP / ESL     %zu / %zu / %zu\n", esm, esp, esl);
    std::printf("  Records             %s (%s unique forms)\n", formatCount(s.records.size()).c_str(),
                formatCount(s.groups.size()).c_str());
    std::printf("  Archives            %s\n", formatCount(s.assets.archives.size()).c_str());
    std::printf("  Loose files         %s\n", formatCount(s.assets.looseFileCount()).c_str());
    std::printf("  Asset conflicts     %s\n", formatCount(s.assets.conflictCount()).c_str());
    if (auto c = p.conflicts())
        std::printf("  Record conflicts    %s (type 1: %s, type 2: %s, type 3: %s, not merged: %s)\n",
                    formatCount(c->conflictCount()).c_str(), formatCount(c->byType[1]).c_str(),
                    formatCount(c->byType[2]).c_str(), formatCount(c->byType[3]).c_str(),
                    formatCount(c->byType[4]).c_str());
    size_t eligible = 0;
    for (const auto& pl : s.plugins)
        eligible += pl.eligibility == Eligibility::Eligible;
    std::printf("  Eligible plugins    %s\n", formatCount(eligible).c_str());
    if (auto d = p.dependencies(); d && !d->blockers.empty()) {
        std::printf("\n  Record types that keep plugins out of compilation (%zu plugins held back; supporting all\n"
                    "  of them would unlock %zu):\n",
                    d->blockedByTypes, d->unlockedByAll);
        std::printf("    %-6s %-28s %-20s %8s %10s %8s %8s\n", "Type", "Name", "Category", "Plugins", "Records", "Only",
                    "Unlocks");
        size_t shown = 0;
        for (const auto& b : d->blockers) {
            if (++shown > 20)
                break;
            std::printf("    %-6s %-28s %-20s %8zu %10zu %8zu %8zu\n", b.sig.str().c_str(), b.name.substr(0, 28).c_str(),
                        b.category.substr(0, 20).c_str(), b.plugins, b.records, b.soleBlocker, b.unlocks);
        }
    }
    if (auto d = p.dependencies()) {
        std::map<std::string, size_t> cleaning; // category -> findings
        for (const auto& is : d->issues)
            if (is.cleaning)
                ++cleaning[is.category];
        if (!cleaning.empty()) {
            std::printf("\n  Cleaning report (as in xEdit; reporting only):\n");
            for (const auto& [category, count] : cleaning)
                std::printf("    %-40s %8zu\n", category.c_str(), count);
        }
    }
    if (auto c = p.compiledOutput()) {
        size_t used = 0;
        for (const auto& op : c->outputs)
            used += !op.patch && !op.records.empty();
        std::printf("  Output plugins      %zu\n", used);
        for (const auto& op : c->outputs)
            if (op.patch)
                std::printf("  Patch               %s (%s merged records, %zu masters)\n", op.name.c_str(),
                            formatCount(op.records.size()).c_str(), op.masterNames.size());
    }
    std::printf("\n");
}

int cmdRecordTypes(const Args& a)
{
    const auto& defs =
        RecordDefinitions::get(a.get("--runtime") == "nextgen" ? RuntimeProfile::NextGen : RuntimeProfile::OldGen_1_10_163);
    std::printf("Signature\tName\tCategory\tPlacement\tStrategy\tSupport\tNotes\n");
    for (const auto& e : defs.catalogue().entries())
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", e.sig.str().c_str(), e.name.c_str(), e.category.c_str(),
                    placementName(e.placement), strategyName(defs.strategy(e.sig)), defs.supportSummary(e.sig).c_str(),
                    e.notes.c_str());
    return 0;
}

int cmdInfo(const Args& a)
{
    if (a.positional.empty())
        throw std::runtime_error("info: plugin path required");
    auto file = PluginFile::load(utf8ToPath(a.positional[0]));
    const auto& h = file.header();
    std::printf("Plugin:      %s\n", a.positional[0].c_str());
    std::printf("Flags:       0x%08X%s%s%s\n", h.record.flags, h.isMasterFlagged() ? " ESM" : "",
                h.isLightFlagged() ? " ESL" : "", h.isLocalized() ? " Localized" : "");
    std::printf("Version:     %.2f  form version %u\n", static_cast<double>(h.version), h.record.formVersion);
    std::printf("Records:     %d  next object id %06X\n", h.numRecords, h.nextObjectId);
    std::printf("Author:      %s\n", h.author.c_str());
    std::printf("Description: %s\n", h.description.c_str());
    for (size_t i = 0; i < h.masters.size(); ++i)
        std::printf("Master %02zX:   %s\n", i, h.masters[i].name.c_str());
    std::map<std::string, size_t> counts;
    size_t total = 0;
    file.forEachRecord([&](const RecordLocation& loc) {
        counts[loc.header.signature.str()]++;
        ++total;
        return true;
    });
    std::printf("\n%zu records:\n", total);
    for (const auto& [s, n] : counts)
        std::printf("  %s  %zu\n", s.c_str(), n);
    return 0;
}

int cmdDump(const Args& a)
{
    if (a.positional.empty())
        throw std::runtime_error("dump: plugin path required");
    auto file = PluginFile::load(utf8ToPath(a.positional[0]));
    uint32_t only = a.has("--formid") ? static_cast<uint32_t>(std::stoul(a.get("--formid"), nullptr, 16)) : 0;
    size_t limit = std::stoul(a.get("--limit", "50"));
    size_t shown = 0;
    const auto& defs = RecordDefinitions::get();
    if (a.has("--tree")) {
        const std::function<void(const std::vector<PluginNode>&, int)> print = [&](const std::vector<PluginNode>& nodes,
                                                                                   int depth) {
            for (const auto& n : nodes) {
                if (shown >= limit)
                    return;
                ++shown;
                if (n.kind == PluginNode::Kind::Group)
                    std::printf("%*sGRUP type %d label %08X (%zu entries)\n", depth * 2, "", n.group.groupType,
                                n.group.label, n.children.size());
                else
                    std::printf("%*s%s %s\n", depth * 2, "", n.record.signature.str().c_str(),
                                formatFormId(n.record.formId).c_str());
                print(n.children, depth + 1);
            }
        };
        print(file.tree(), 0);
        return 0;
    }
    // --cell: the cell record and every record in its children groups (persistent, temporary, distant),
    // FormIDs as the plugin writes them.
    std::unordered_set<uint32_t> inCell;
    const bool cellMode = a.has("--cell");
    if (cellMode) {
        const uint32_t cell = static_cast<uint32_t>(std::stoul(a.get("--cell"), nullptr, 16));
        inCell.insert(cell);
        const std::function<void(const std::vector<PluginNode>&, bool)> walk = [&](const std::vector<PluginNode>& nodes,
                                                                                  bool inside) {
            for (const auto& n : nodes) {
                if (n.kind == PluginNode::Kind::Group) {
                    const auto t = n.group.groupType;
                    // The cell's own groups (labelled with its FormID). QuestChildren (10) is labelled with a
                    // quest's FormID, so it is not one of them.
                    const bool children =
                        (t == GroupType::CellChildren || t == GroupType::CellPersistent || t == GroupType::CellTemporary) &&
                        n.group.label == cell;
                    walk(n.children, inside || children);
                } else {
                    if (inside)
                        inCell.insert(n.record.formId);
                    walk(n.children, inside);
                }
            }
        };
        walk(file.tree(), false);
        if (!a.has("--limit"))
            limit = std::numeric_limits<size_t>::max();
        std::printf("# cell %08X: %zu records (the cell and its references)\n", cell, inCell.size());
    }
    file.forEachRecord([&](const RecordLocation& loc) {
        if (only && loc.header.formId != only)
            return true;
        if (cellMode && !inCell.count(loc.header.formId))
            return true;
        auto rec = Record::parse(loc.header, loc.rawData);
        auto layout = decodeLayout(defs, rec.signature(), rec.subrecords);
        // Version Control Info 1: its low 16 bits are a date (day, month, year since 2000), "none" when 0.
        const uint32_t vc = loc.header.versionControl1 & 0xFFFF;
        char date[16] = "none";
        if (vc)
            std::snprintf(date, sizeof(date), "%04u-%02u-%02u", 2000 + ((vc >> 9) & 0x7F), (vc >> 5) & 0xF, vc & 0x1F);
        std::printf("%s %s flags=%08X vc=%08X/%04X (%s)  %s\n", rec.signature().str().c_str(),
                    formatFormId(rec.header.formId).c_str(), loc.header.flags, loc.header.versionControl1,
                    loc.header.versionControl2, date, rec.editorId().c_str());
        for (uint32_t i = 0; i < rec.subrecords.size(); ++i) {
            const auto& sr = rec.subrecords[i];
            std::string refs;
            for (const auto& slot : layout.formIdSlots)
                if (slot.sub == i && slot.offset + 4 <= sr.data.size())
                    refs += " @" + std::to_string(slot.offset) + "=" +
                            formatFormId(loadLE<uint32_t>(sr.data.data() + slot.offset));
            std::printf("   %s %5zu%s\n", sr.signature.str().c_str(), sr.data.size(), refs.c_str());
        }
        for (const auto& is : layout.issues)
            std::printf("   ! %s %s\n", is.sub.str().c_str(), is.message.c_str());
        if (a.has("--values"))
            for (const auto& v : decodeValues(defs, rec.signature(), rec.subrecords, file.header().isLocalized()))
                std::printf("     %-40s %s%s\n", v.path.c_str(), v.text.c_str(), v.described ? "" : "  (undescribed)");
        return ++shown < limit;
    });
    return 0;
}

int cmdBa2(const Args& a)
{
    if (a.positional.empty())
        throw std::runtime_error("ba2: archive path required");
    auto ar = Ba2Archive::open(utf8ToPath(a.positional[0]));
    if (a.has("--extract")) {
        std::string want = normalizeAssetPath(a.get("--extract"));
        for (size_t i = 0; i < ar->entries().size(); ++i)
            if (ar->entries()[i].path == want) {
                auto data = ar->extract(i);
                std::ofstream out(utf8ToPath(a.get("--to", "extracted.bin")), std::ios::binary);
                out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                std::printf("Extracted %s (%zu bytes)\n", want.c_str(), data.size());
                return 0;
            }
        throw std::runtime_error("not found in archive: " + want);
    }
    std::printf("%s: version %u, %s, %zu files\n", ar->name().c_str(), ar->version(),
                ar->type() == Ba2Type::General ? "general" : "textures", ar->entries().size());
    for (const auto& e : ar->entries())
        std::printf("  %10llu  %s\n", static_cast<unsigned long long>(e.size()), e.path.c_str());
    return 0;
}

void printChanges(const ChangeReport& r)
{
    const char* mark = r.status == BuildStatus::UpToDate          ? "[OK]"
                       : r.status == BuildStatus::ChangesDetected ? "[!]"
                       : r.status == BuildStatus::RequiresRebuild ? "[X]"
                                                                  : "[-]";
    std::printf("\nBuild status: %s %s\n", mark, buildStatusName(r.status));
    if (r.status == BuildStatus::NotBuilt) {
        std::printf("  No compiled output has been exported yet.\n");
        return;
    }
    std::printf("  New mods %zu · removed %zu · updated %zu · plugins +%zu / -%zu / ~%zu · archives %zu · load order %zu\n",
                r.newMods, r.removedMods, r.updatedMods, r.addedPlugins, r.removedPlugins, r.updatedPlugins,
                r.archiveChanges, r.loadOrderChanges);
    for (const auto& i : r.items)
        std::printf("  %s %-10s %-9s %s%s%s\n", i.affectsBuild ? "*" : " ", i.category.c_str(), i.kind.c_str(),
                    i.name.c_str(), i.detail.empty() ? "" : "  — ", i.detail.c_str());
    if (!r.affectedRecordTypes.empty())
        std::printf("  Affected record types:   %s\n", join(r.affectedRecordTypes, " ").c_str());
    if (!r.unaffectedRecordTypes.empty())
        std::printf("  Unaffected record types: %s\n", join(r.unaffectedRecordTypes, " ").c_str());
    if (r.status == BuildStatus::RequiresRebuild)
        std::printf("  (*) requires recompilation. Run: fo4mx compile ...\n");
}

int cmdCleanOutputs(const Args& a)
{
    auto cfg = makeConfig(a);
    const auto install = resolveInstall(cfg.install);
    const auto loc = output::locate(cfg.exportOptions.outputDirectory, install.gamePath, install.dataPath, install.modsPath);
    const size_t keep = std::stoul(a.get("--keep", "5"));
    std::printf("Exports in %s (keeping the newest %zu)\n", pathToUtf8(loc.root).c_str(), keep);
    const auto r = output::cleanupRuns(loc, keep, a.has("--unfinished"));
    for (const auto& p : r.removed)
        std::printf("  removed  %s\n", pathToUtf8(p.filename()).c_str());
    for (const auto& [p, why] : r.skipped)
        std::printf("  kept     %s (%s)\n", pathToUtf8(p.filename()).c_str(), why.c_str());
    return 0;
}

// A Buffout 4 crash log read against a build's FormID map: the original mod behind each output FormID.
int cmdCrashLog(const Args& a)
{
    std::filesystem::path file = a.positional.empty() ? diagnostics::newestCrashLog() : utf8ToPath(a.positional[0]);
    if (file.empty())
        throw std::runtime_error("crash-log: no crash log given, and none in Documents\\My Games\\Fallout4\\F4SE");
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("crash-log: cannot read " + pathToUtf8(file));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<FormMapping> map;
    std::set<std::string> redirected;
    if (a.has("--formmap")) {
        const auto fm = utf8ToPath(a.get("--formmap"));
        map = CompilationEngine::readFormMap(fm);
        if (const auto warn = diagnostics::logBuildWarning(file, fm.parent_path()); !warn.empty())
            std::printf("%s\n", warn.c_str());
        std::ifstream r(fm.parent_path() / "Multiplexer_Redirected.tsv");
        for (std::string line; std::getline(r, line);)
            if (!line.empty() && line[0] != '#')
                redirected.insert(line.substr(0, line.find('\t')));
    }
    std::printf("%s\n%s", pathToUtf8(file).c_str(),
                diagnostics::formatCrashReport(diagnostics::translateCrashLog(text, map, {}, redirected)).c_str());
    return 0;
}

// "Ready to test?" before Apply to Game: the latest export when it still fits, else a fresh compile.
int cmdReady(const Args& a)
{
    auto cfg = makeConfig(a);
    Pipeline p(cfg);
    p.scan();
    if (!p.restoreLastBuild()) {
        p.analyse();
        p.compile();
    }
    const auto save = a.has("--save") ? utf8ToPath(a.get("--save")) : diagnostics::newestSave();
    const auto r = p.readiness(save);
    std::fputs(r.format().c_str(), stdout);
    return r.ready() ? 0 : 2;
}

// A save's plugins against the load order now, or after the next build (--next).
int cmdCheckSave(const Args& a)
{
    auto cfg = makeConfig(a);
    const auto file = a.positional.size() > 1 ? utf8ToPath(a.positional[1]) : diagnostics::newestSave();
    if (file.empty())
        throw std::runtime_error("check-save: no save given, and none in Documents\\My Games\\Fallout4\\Saves");
    const auto save = diagnostics::readSaveGame(file);
    Pipeline p(cfg);
    p.scan();
    if (a.has("--next")) {
        p.analyse();
        p.compile();
    }
    const auto in = p.saveCheckInput();
    const auto check = diagnostics::checkSave(save, in);
    std::printf("%s\n%s", pathToUtf8(file).c_str(), diagnostics::formatSaveCheck(save, check, in.nextBuild).c_str());
    return check.safe ? 0 : 2;
}

// The Papyrus log read with the install's current load order and its latest build.
int cmdPapyrusLog(const Args& a)
{
    auto cfg = makeConfig(a);
    const auto file = a.positional.size() > 1 ? utf8ToPath(a.positional[1]) : diagnostics::defaultPapyrusLog();
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("papyrus-log: cannot read " + pathToUtf8(file));
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Pipeline p(cfg);
    p.scan();
    std::vector<FormMapping> map;
    std::set<std::string> redirected;
    const auto& install = p.session().install;
    const auto loc = output::locate(cfg.exportOptions.outputDirectory, install.gamePath, install.dataPath, install.modsPath);
    const auto root = output::latestRun(loc).value_or(loc.root);
    std::string note = "No build found: compiled records are not translated.";
    if (diagnostics::readBuildMap(root, map, redirected)) {
        note = "Read against the build in " + pathToUtf8(root);
        if (const auto warn = diagnostics::logBuildWarning(file, root); !warn.empty())
            note += "\n" + warn;
    }
    std::printf("%s\n%s\n\n", pathToUtf8(file).c_str(), note.c_str());
    std::fputs(diagnostics::formatPapyrusReport(
                   diagnostics::translatePapyrusLog(text, diagnostics::runtimeLoadOrder(p.session()), map, redirected,
                                                   diagnostics::scriptOwnerOf(p.session())),
                   a.has("--limit") ? std::stoul(a.get("--limit")) : 100)
                   .c_str(),
               stdout);
    return 0;
}

// Startup popups and MCM menus of a build, against the Papyrus log and MCM.log of a session.
int cmdStartupCheck(const Args& a)
{
    auto slurp = [](const std::filesystem::path& f) {
        std::ifstream in(f, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    diagnostics::StartupCheckInputs in;
    const auto papyrus = a.positional.size() > 1 ? utf8ToPath(a.positional[1]) : diagnostics::defaultPapyrusLog();
    in.papyrusLog = slurp(papyrus);
    in.mcmLog = slurp(a.has("--mcm-log") ? utf8ToPath(a.get("--mcm-log")) : diagnostics::defaultMcmLog());
    if (a.has("--build"))
        in.exportRoot = utf8ToPath(a.get("--build"));
    if (a.has("--f4se-log"))
        in.loadOrder = diagnostics::loadOrderFromF4seLog(slurp(utf8ToPath(a.get("--f4se-log"))));
    if (in.exportRoot.empty() || !a.has("--f4se-log")) {
        auto cfg = makeConfig(a);
        Pipeline p(cfg);
        p.scan();
        const auto& install = p.session().install;
        if (in.exportRoot.empty()) {
            const auto loc = output::locate(cfg.exportOptions.outputDirectory, install.gamePath, install.dataPath,
                                            install.modsPath);
            in.exportRoot = output::latestRun(loc).value_or(loc.root);
        }
        if (!a.has("--f4se-log"))
            in.loadOrder = diagnostics::runtimeLoadOrder(p.session());
    }
    std::printf("%s\nRead against the build in %s\n\n", pathToUtf8(papyrus).c_str(), pathToUtf8(in.exportRoot).c_str());
    std::fputs(diagnostics::formatStartupCheck(diagnostics::checkStartup(in)).c_str(), stdout);
    return 0;
}

// The crash log read against a build (its Data folder or root holding Multiplexer_FormMap.tsv).
std::string crashReadingAgainst(const std::filesystem::path& crashLog, const std::filesystem::path& build)
{
    std::ifstream in(crashLog, std::ios::binary);
    if (!in)
        return {};
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<FormMapping> map;
    std::set<std::string> redirected;
    std::string note = "No build to read it against (no Multiplexer_FormMap.tsv found).";
    for (const auto& dir : {build / "Data", build}) {
        if (build.empty() || !std::filesystem::exists(dir / "Multiplexer_FormMap.tsv"))
            continue;
        map = CompilationEngine::readFormMap(dir / "Multiplexer_FormMap.tsv");
        std::ifstream r(dir / "Multiplexer_Redirected.tsv");
        for (std::string line; std::getline(r, line);)
            if (!line.empty() && line[0] != '#')
                redirected.insert(line.substr(0, line.find('\t')));
        note = "Read against the build in " + pathToUtf8(build) + " (" + std::to_string(map.size()) +
               " FormID mappings).";
        if (const auto warn = diagnostics::logBuildWarning(crashLog, build); !warn.empty())
            note += "\n" + warn;
        break;
    }
    return pathToUtf8(crashLog.filename()) + "\n" + note + "\n\n" +
           diagnostics::formatCrashReport(diagnostics::translateCrashLog(text, map, {}, redirected));
}

// Two exports compared: what the later one changes for saves made with the earlier one.
int cmdCompareBuilds(const Args& a)
{
    if (a.positional.size() < 2)
        throw std::runtime_error("compare-builds: two export folders required (earlier, later)");
    auto side = [](const std::string& arg, BuildSide& out) {
        const auto root = utf8ToPath(arg);
        for (const auto& dir : {root / "Data", root})
            if (readBuildSide(dir, out))
                return;
        throw std::runtime_error("compare-builds: no Multiplexer_FormMap.tsv in " + arg);
    };
    BuildSide before, after;
    side(a.positional[0], before);
    side(a.positional[1], after);
    auto c = compareBuilds(before, after);
    c.previousSource = a.positional[0];
    std::fputs(formatBuildComparison(c, a.has("--limit") ? std::stoul(a.get("--limit")) : 200).c_str(), stdout);
    return 0;
}

// One zip to send with a problem report (Diagnostics → Support Bundle… in the app).
int cmdSupportBundle(const Args& a)
{
    SupportBundleInputs in;
    auto cfg = makeConfig(a);
    try {
        const auto install = resolveInstall(cfg.install);
        const auto loc = output::locate(cfg.exportOptions.outputDirectory, install.gamePath, install.dataPath,
                                        install.modsPath);
        in.packageRoot = output::latestRun(loc).value_or(loc.root);
        std::error_code ec;
        for (const auto& f : {install.pluginsFile, install.profilePath / "loadorder.txt", install.profilePath / "modlist.txt"})
            if (!f.empty() && std::filesystem::is_regular_file(f, ec))
                in.loadOrder.push_back(f);
    } catch (const std::exception& e) {
        std::printf("No install found (%s): the bundle holds the logs and crash log only.\n", e.what());
    }
    in.reportDirectory = cfg.reportDirectory;
    std::error_code ec;
    const auto logs = platform::applicationDataDir() / "logs";
    for (auto it = std::filesystem::directory_iterator(logs, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec))
            in.logs.push_back(it->path());
    in.crashLog = diagnostics::newestCrashLog();
    if (!in.crashLog.empty())
        in.crashReading = crashReadingAgainst(in.crashLog, in.packageRoot);
    in.recorderSession = diagnostics::newestSession(diagnostics::defaultRecorderFolder());
    if (const auto papyrus = diagnostics::defaultPapyrusLog(); std::filesystem::exists(papyrus, ec))
        in.extraFiles.push_back({papyrus, "papyrus/" + pathToUtf8(papyrus.filename())});
    const auto out = a.has("--out") ? utf8ToPath(a.get("--out"))
                                    : std::filesystem::current_path() /
                                          ("Multiplexer Support " + output::runStamp() + ".zip");
    const auto r = writeSupportBundle(in, out);
    std::printf("Wrote %s (%zu files). No mod files; home folder and user name taken out.\n", pathToUtf8(out).c_str(),
                r.included.size());
    for (const auto& m : r.missing)
        std::printf("  not found: %s\n", m.c_str());
    return 0;
}

// Undoes the last Apply to Game from its backups: shows the plan, applies it with --yes.
int cmdSwitchBack(const Args& a)
{
    auto cfg = makeConfig(a);
    const auto install = resolveInstall(cfg.install);
    const auto loc = output::locate(cfg.exportOptions.outputDirectory, install.gamePath, install.dataPath, install.modsPath);
    FileTransaction tx("Switch back");
    const auto plan = switch_back::plan(tx, loc.root);
    if (!plan.nothing.empty()) {
        std::printf("%s.\n", plan.nothing.c_str());
        return 0;
    }
    for (const auto& p : plan.problems)
        std::printf("Problem: %s\n", p.c_str());
    if (!plan.possible)
        return 1;
    std::printf("Undo Apply to Game of %s (build %s):\n", plan.record.when.c_str(), plan.record.build.c_str());
    for (const auto& l : tx.summary())
        std::printf("  %s\n", l.c_str());
    for (const auto& c : plan.changedSince)
        std::printf("  changed since Apply (switching back replaces that change too): %s\n", c.c_str());
    if (!a.has("--yes")) {
        std::printf("Nothing changed. Run again with --yes to switch back (close your mod manager first).\n");
        return 0;
    }
    tx.apply();
    switch_back::markSwitchedBack(loc.root);
    std::printf("Switched back; backups of what was replaced are kept next to each file.\n");
    return 0;
}

int cmdFormId(const Args& a)
{
    if (a.positional.size() < 2)
        throw std::runtime_error("formid: plugin and FormID required (e.g. fo4mx formid Fallout4.esm 0C38F9)");
    const auto file = g_exeDir / FormIdReference::kFileName;
    auto ref = FormIdReference::tryLoad(file);
    if (!ref)
        throw std::runtime_error("the FormID reference is not installed: " + pathToUtf8(file));
    const uint32_t id = static_cast<uint32_t>(std::stoul(a.positional[1], nullptr, 16)) & 0xFFFFFF;
    const auto found = ref->find(a.positional[0], id);
    if (found.empty()) {
        std::printf("%s %06X: no Editor ID in the reference (the record has none, or it is not in this plugin)\n",
                    a.positional[0].c_str(), id);
        return 1;
    }
    for (const auto& e : found)
        std::printf("%s %06X  %.*s%s\n", a.positional[0].c_str(), id, static_cast<int>(e.editorId.size()),
                    e.editorId.data(), e.baseGameOverride ? "  (override of the Fallout4.esm record)" : "");
    return 0;
}

int cmdPipeline(const Args& a)
{
    auto cfg = makeConfig(a);
    if (cfg.databasePath.empty())
        cfg.databasePath = platform::applicationDataDir() / "multiplexer.sqlite";
    Pipeline p(cfg);
    ConsoleProgress progress;

    if (a.command == "status") {
        printChanges(p.checkForChanges(progress));
        return p.changes().status == BuildStatus::RequiresRebuild ? 3 : 0;
    }
    p.scan(progress);
    if (a.command == "references") {
        if (a.positional.size() < 2)
            throw std::runtime_error("references: <plugin> <formid> required (with the install options)");
        auto& session = p.session();
        const auto* plugin = session.findPlugin(a.positional[0]);
        if (!plugin)
            throw std::runtime_error(a.positional[0] + " is not in the load order");
        FormKey target{plugin->index, static_cast<uint32_t>(std::stoul(a.positional[1], nullptr, 16)) & 0xFFFFFF};
        const auto found = refs::referencedBy(session, target, progress);
        std::printf("\n%s is referenced by %zu record version(s):\n", session.formLabel(target).c_str(), found.size());
        for (const auto& r : found) {
            const auto& inst = session.records[r.record];
            std::printf("  %-40s %s  %s  (in %s)\n", session.formLabel(inst.key).c_str(), inst.signature.str().c_str(),
                        r.via.str().c_str(), session.pluginName(inst.plugin).c_str());
        }
        return 0;
    }
    if (a.command == "check-definitions") {
        std::vector<Signature> types;
        for (const auto& t : split(a.get("--types", ""), ','))
            if (t.size() == 4)
                types.push_back(Signature::fromString(t.c_str()));
        auto check = p.checkDefinitions(types, progress);
        std::printf("\n%-6s %-6s %-11s %10s %10s %9s %8s %8s\n", "Record", "Sub", "Described", "Seen", "FormIDs",
                    "Resolve", "Sizes", "Undesc.");
        for (const auto& st : check.stats) {
            char rate[16] = "-";
            if (st.formIds)
                std::snprintf(rate, sizeof(rate), "%.1f%%", 100.0 * double(st.resolved) / double(st.formIds));
            std::printf("%-6s %-6s %-11s %10llu %10llu %9s %8llu %8llu\n", st.record.str().c_str(), st.sub.str().c_str(),
                        st.described ? "yes" : "no", static_cast<unsigned long long>(st.occurrences),
                        static_cast<unsigned long long>(st.formIds), rate,
                        static_cast<unsigned long long>(st.sizeIssues), static_cast<unsigned long long>(st.softRefs));
        }
        std::printf("\nFindings:\n");
        for (const auto& is : check.report.issues)
            if (is.severity != Severity::Info)
                std::printf("  [%s] %s\n", severityName(is.severity), is.message.c_str());
        std::printf("%llu warnings\n", static_cast<unsigned long long>(check.report.warnings));
        return check.report.warnings ? 4 : 0;
    }
    if (a.command == "scan") {
        printDashboard(p);
        printChanges(p.changes());
        if (cfg.scan.previewInactive) {
            std::printf("\nInactive plugins (preview, not part of the build): %zu\n", p.inactivePreview().size());
            for (const auto& r : p.inactivePreview())
                std::printf("  %-40s %s  %zu records, conflicts with active mods on %zu. %s\n", r.name.c_str(),
                            r.likelyCompiles ? "likely compiles " : "likely stays out", r.records, r.conflicts,
                            r.reason.c_str());
        }
        std::printf("Database: %s\n", pathToUtf8(cfg.databasePath).c_str());
        return 0;
    }
    p.analyse(progress);
    if (a.command == "plan") {
        auto& session = p.session();
        if (a.positional.size() >= 3) {
            const std::string& verb = a.positional[0];
            const std::string& rec = a.positional[1];
            const auto colon = rec.rfind(':');
            const auto* plugin = colon == std::string::npos ? nullptr : session.findPlugin(rec.substr(0, colon));
            if (!plugin)
                throw std::runtime_error("plan: the record must be given as <Plugin.esp:OBJECTID> of the plugin that defines it");
            const FormKey key{plugin->index, static_cast<uint32_t>(std::stoul(rec.substr(colon + 1), nullptr, 16)) & 0xFFFFFF};
            std::optional<PlanAction> action;
            if (verb == "carry")
                action = PlanAction::CarryForward;
            else if (verb == "keep")
                action = PlanAction::KeepWinner;
            else if (verb != "default")
                throw std::runtime_error("plan: use carry, keep or default");
            p.setPlanDecision(key, a.positional[2], action);
            std::printf("Saved: %s %s for %s (%s)\n", verb.c_str(), a.positional[2].c_str(), session.formLabel(key).c_str(),
                        pathToUtf8(p.planDecisionsPath()).c_str());
        }
        const auto* plan = p.patchPlan();
        std::printf("\nCompatibility patch plan (nothing is written from it yet)\n");
        std::printf("  records: %zu ready, %zu need a decision, %zu blocked, %zu nothing to carry\n",
                    plan->byStatus[0], plan->byStatus[1], plan->byStatus[2], plan->byStatus[3]);
        std::printf("  changes: %zu carried, %zu waiting, %zu kept as the winner, %zu never carried\n", plan->carried,
                    plan->pending, plan->kept, plan->locked);
        size_t shown = 0;
        for (const auto& pr : plan->records) {
            if (pr.status != PlanStatus::NeedsDecision && pr.status != PlanStatus::Blocked)
                continue;
            if (shown++ >= 50) {
                std::printf("  ... more in the Conflict Report\n");
                break;
            }
            std::printf("\n%s  [%s]  record id %s\n", session.formLabel(pr.conflict->key).c_str(),
                        planStatusName(pr.status), PlanDecisions::recordId(session, pr.conflict->key).c_str());
            for (const auto& problem : pr.problems)
                std::printf("    problem: %s\n", problem.c_str());
            for (const auto& pe : pr.entries) {
                if (pe.decision == PlanDecision::Locked)
                    continue;
                const auto& t = pr.conflict->changes[pe.change];
                std::printf("    %s  %-13s %-20s %s by %s (%s: %s)\n", t.changeId.c_str(), planActionName(pe.action),
                            planDecisionName(pe.decision), t.field.c_str(), session.pluginName(t.source).c_str(),
                            compatClassName(t.cls), t.rule.c_str());
            }
        }
        return 0;
    }
    if (a.command == "analyse" || a.command == "analyze") {
        printDashboard(p);
        auto dir = cfg.reportDirectory.empty() ? platform::applicationDataDir() / "Reports" : cfg.reportDirectory;
        p.writeReports(dir);
        std::printf("Reports: %s\nDatabase: %s\n", pathToUtf8(dir).c_str(), pathToUtf8(cfg.databasePath).c_str());
        return 0;
    }

    bool ok = p.compile(progress);
    printDashboard(p);
    const auto* compiled = p.compiledOutput();
    std::printf("Incremental: %zu override chains reused, %zu rebuilt\n", compiled->mergesReused,
                compiled->mergesComputed);
    const auto& trace = p.assetTrace();
    std::printf("Assets: %zu referenced by compiled records (%zu missing, %zu served from archives of compiled "
                "plugins); nothing is copied\n",
                trace.assets.size(), trace.missing, trace.viaRegisteredArchives);
    const auto* v = p.validation();
    std::printf("Validation: %s checks, %s passed, %s warnings, %s errors\n", formatCount(v->checks).c_str(),
                formatCount(v->passed).c_str(), formatCount(v->warnings).c_str(), formatCount(v->errors).c_str());
    int shown = 0;
    for (const auto& is : v->issues) {
        if (is.severity != Severity::Error || shown++ >= 20)
            continue;
        std::printf("  ERROR [%s] %s\n", is.category.c_str(), is.message.c_str());
    }
    if (!ok) {
        std::printf("Compilation stopped: critical validation errors.\n");
        auto dir = cfg.reportDirectory.empty() ? platform::applicationDataDir() / "Reports" : cfg.reportDirectory;
        p.writeReports(dir);
        std::printf("Reports: %s\n", pathToUtf8(dir).c_str());
        return 2;
    }
    if (a.has("--dry-run")) {
        std::printf("Dry run: nothing written.\n");
        return 0;
    }
    auto result = p.exportOutputs(progress);
    std::printf("Output in %s\n", pathToUtf8(result.directory).c_str());
    if (!result.runName.empty())
        std::printf("Export %s in %s%s\n", result.runName.c_str(), pathToUtf8(result.outputRoot).c_str(),
                    result.latestRecorded ? "" : " (Multiplexer Latest.txt could not be updated)");
    for (const auto& n : result.writtenOutputs)
        std::printf("  written    %s\n", n.c_str());
    for (const auto& n : result.unchangedOutputs)
        std::printf("  unchanged  %s\n", n.c_str());
    for (const auto& n : result.removedOutputs)
        std::printf("  removed    %s\n", n.c_str());
    std::printf("Next steps:\n  1. Enable the output plugins (see %s)\n  2. Disable the compiled plugins (%s)\n"
                "  3. Merge %s into Fallout4Custom.ini so their archives still load\n",
                pathToUtf8(result.pluginsTxt.filename()).c_str(), pathToUtf8(result.disableList.filename()).c_str(),
                pathToUtf8(result.archiveIni.filename()).c_str());
    return p.validation()->critical() ? 2 : 0;
}

int cmdDiagnose(const Args& a)
{
    namespace fs = std::filesystem;
    if (a.has("--install")) {
        const auto st = diagnostics::checkRecorderInstall(a.has("--game") ? utf8ToPath(a.get("--game")) : fs::path());
        std::fputs(diagnostics::formatInstallStatus(st).c_str(), stdout);
        return st.ok ? 0 : 2;
    }
    fs::path file = a.positional.empty() ? diagnostics::newestSession(diagnostics::defaultRecorderFolder())
                                         : utf8ToPath(a.positional[0]);
    if (file.empty() || !fs::exists(file)) {
        std::fprintf(stderr, "No Diagnostic Recorder log found%s%s\n", file.empty() ? " in " : ": ",
                     pathToUtf8(file.empty() ? diagnostics::defaultRecorderFolder() : file).c_str());
        return 1;
    }
    const auto session = diagnostics::loadRecorderLog(file);
    std::optional<diagnostics::PackageContext> package;
    if (a.has("--package")) {
        package = diagnostics::loadPackageContext(utf8ToPath(a.get("--package")));
        if (!package)
            std::fprintf(stderr, "warning: no Multiplexer_plugins.txt in %s; the build is not compared\n",
                         a.get("--package").c_str());
        else if (a.has("--runtime"))
            package->expectedProfile = a.get("--runtime") == "nextgen" ? "nextgen" : "oldgen";
    }
    const auto findings = diagnostics::analyse(session, package ? &*package : nullptr);
    std::fputs(diagnostics::formatReport(session, findings, package ? &*package : nullptr).c_str(), stdout);
    for (const auto& f : findings)
        if (f.severity == diagnostics::FindingSeverity::Error)
            return 2;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 0)
            g_exeDir = std::filesystem::absolute(utf8ToPath(argv[0])).parent_path();
        Args a = parseArgs(argc, argv);
        log::init(platform::applicationDataDir() / "logs" / "fo4mx.log", a.has("--verbose") || a.has("-v"));
        if (a.command.empty() || a.command == "help" || a.has("--help")) {
            std::fputs(kUsage, stdout);
            return a.command.empty() ? 1 : 0;
        }
        if (a.command == "record-types")
            return cmdRecordTypes(a);
        if (a.command == "info")
            return cmdInfo(a);
        if (a.command == "dump")
            return cmdDump(a);
        if (a.command == "ba2")
            return cmdBa2(a);
        if (a.command == "diagnose")
            return cmdDiagnose(a);
        if (a.command == "formid")
            return cmdFormId(a);
        if (a.command == "compare-builds")
            return cmdCompareBuilds(a);
        if (a.command == "support-bundle")
            return cmdSupportBundle(a);
        if (a.command == "ready")
            return cmdReady(a);
        if (a.command == "check-save")
            return cmdCheckSave(a);
        if (a.command == "papyrus-log")
            return cmdPapyrusLog(a);
        if (a.command == "startup-check")
            return cmdStartupCheck(a);
        if (a.command == "crash-log")
            return cmdCrashLog(a);
        if (a.command == "switch-back")
            return cmdSwitchBack(a);
        if (a.command == "clean-outputs")
            return cmdCleanOutputs(a);
        if (a.command == "scan" || a.command == "analyse" || a.command == "analyze" || a.command == "plan" || a.command == "compile" ||
            a.command == "status" || a.command == "check-definitions" || a.command == "references")
            return cmdPipeline(a);
        std::fprintf(stderr, "Unknown command '%s'\n\n%s", a.command.c_str(), kUsage);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
