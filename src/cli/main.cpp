// fo4mx - command line front end for Fallout 4 Multiplexer Renewed.

#include "diagnostics/RecorderLog.hpp"
#include "diagnostics/RecorderPackage.hpp"
#include "engine/Exporter.hpp"
#include "ba2/Ba2Archive.hpp"
#include "engine/Pipeline.hpp"
#include "reference/FormIdReference.hpp"
#include "engine/ReferenceIndex.hpp"
#include "engine/OutputRuns.hpp"
#include "esp/FieldValues.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"
#include "util/Log.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
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
  --no-previs-translation  do not write translated previs files (the same plugins compile; for testing)
  --patch              also write Multiplexer_Patch.esp, merging conflicts between
                       plugins that stay in the load order (like a Bashed Patch)
  --patch-only         write only the patch; compile no plugins
  --auto-merge         Advanced Users Auto-Merger: also compile plugins named in
                       scripts/configs that can be relinked, and write relinked copies
  --exclude PLUGIN     never compile PLUGIN (repeatable)
  --out DIR            folder holding the exports (default: <game>\\Multiplexer Output); every export
                       goes into "Multiplexer Output DDMMYY HHMMSS" in it, with Multiplexer Output.zip
  --reports DIR        report folder
  --archive-loader  load compiled plugins' archives with the F4SE archive loader (shipped in
                 the package; no 1,023-character limit; the game must start through F4SE)
  --diagnostic-recorder  also prepare the optional Diagnostic Recorder (F4SE plugin) beside
                       the package, in "Multiplexer Output\Diagnostic Recorder"
  --dry-run            compile and validate without writing files
  --no-cache           ignore the build cache (full rescan, analysis and merge)

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
    static const std::vector<std::string> flags = {"--vortex", "--no-merge", "--light", "--no-previs-translation", "--patch", "--patch-only", "--auto-merge", "--dry-run", "--no-cache",
                                                   "--diagnostic-recorder", "--install", "--archive-loader",
                                                   "--values", "--tree", "--help", "--unfinished",
                                                   "--verbose", "-v"};
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
    c.translatePrevis = !a.has("--no-previs-translation");
    c.patchOnly = a.has("--patch-only");
    c.autoMerge = a.has("--auto-merge");
    c.buildPatch = c.patchOnly || a.has("--patch");
    cfg.analysis.excludedPlugins = a.all("--exclude");
    cfg.diagnosticRecorder = a.has("--diagnostic-recorder");
    cfg.diagnosticRecorderDll = g_exeDir / diagnostics::kRecorderDllName;
    cfg.f4seArchiveLoader = a.has("--archive-loader");
    cfg.archiveLoaderDll = g_exeDir / Exporter::kArchiveLoaderDll;
    cfg.formIdReference = g_exeDir / FormIdReference::kFileName;
    cfg.useCache = !a.has("--no-cache");
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
                    const bool children = (t == 6 || t == 8 || t == 9 || t == 10) && n.group.label == cell;
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
        std::printf("Database: %s\n", pathToUtf8(cfg.databasePath).c_str());
        return 0;
    }
    p.analyse(progress);
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
        if (a.command == "clean-outputs")
            return cmdCleanOutputs(a);
        if (a.command == "scan" || a.command == "analyse" || a.command == "analyze" || a.command == "compile" ||
            a.command == "status" || a.command == "check-definitions" || a.command == "references")
            return cmdPipeline(a);
        std::fprintf(stderr, "Unknown command '%s'\n\n%s", a.command.c_str(), kUsage);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
