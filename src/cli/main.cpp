// fo4mx - command line front end for Fallout 4 Multiplexer Renewed.

#include "ba2/Ba2Archive.hpp"
#include "engine/Pipeline.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"
#include "util/Log.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace fo4mx;

namespace {

const char* kUsage = R"(Fallout 4 Multiplexer Renewed - load order compiler

Usage:
  fo4mx scan     <install> [--db FILE]
  fo4mx analyse  <install> [--db FILE] [--reports DIR]
  fo4mx compile  <install> [--db FILE] [compile options] [--dry-run]
  fo4mx info     <plugin.esp>
  fo4mx dump     <plugin.esp> [--formid XXXXXXXX] [--limit N]
  fo4mx ba2      <archive.ba2> [--extract PATH --to FILE]

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
  --outputs N          number of output plugins, 10-20 (default 20)
  --prefix NAME        output plugin name prefix (default Dummy_Plugin_)
  --strategy type|balanced|user   allocation strategy (default balanced)
  --rule PATTERN=N     strategy "user": send records whose signature or plugin
                       matches PATTERN to output N (repeatable)
  --no-merge           last plugin wins instead of merging compatible edits
  --light              ESL-flag outputs that fit the light plugin range
  --exclude PLUGIN     never compile PLUGIN (repeatable)
  --out DIR            output folder (default: MO2 mod folder or Data)
  --reports DIR        report folder
  --dry-run            compile and validate without writing files

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
    static const std::vector<std::string> flags = {"--vortex", "--no-merge", "--light", "--dry-run", "--verbose", "-v"};
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
    c.outputCount = std::stoi(a.get("--outputs", "20"));
    if (c.outputCount < CompileOptions::kMinOutputs || c.outputCount > CompileOptions::kMaxOutputs)
        throw std::runtime_error("--outputs must be between 10 and 20");
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
    cfg.analysis.excludedPlugins = a.all("--exclude");
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
        std::printf("  Record conflicts    %s (type 1: %s, type 2: %s, type 3: %s)\n",
                    formatCount(c->conflictCount()).c_str(), formatCount(c->byType[1]).c_str(),
                    formatCount(c->byType[2]).c_str(), formatCount(c->byType[3]).c_str());
    size_t eligible = 0;
    for (const auto& pl : s.plugins)
        eligible += pl.eligibility == Eligibility::Eligible;
    std::printf("  Eligible plugins    %s\n", formatCount(eligible).c_str());
    if (auto c = p.compiledOutput()) {
        size_t used = 0;
        for (const auto& op : c->outputs)
            used += !op.records.empty();
        std::printf("  Output plugins      %zu\n", used);
    }
    std::printf("\n");
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
    file.forEachRecord([&](const RecordLocation& loc) {
        if (only && loc.header.formId != only)
            return true;
        auto rec = Record::parse(loc.header, loc.rawData);
        auto layout = decodeLayout(defs, rec.signature(), rec.subrecords);
        std::printf("%s %s flags=%08X  %s\n", rec.signature().str().c_str(), formatFormId(rec.header.formId).c_str(),
                    loc.header.flags, rec.editorId().c_str());
        for (uint32_t i = 0; i < rec.subrecords.size(); ++i) {
            const auto& sr = rec.subrecords[i];
            std::string refs;
            for (const auto& slot : layout.slots)
                if (slot.sub == i && slot.offset + 4 <= sr.data.size())
                    refs += " @" + std::to_string(slot.offset) + "=" +
                            formatFormId(loadLE<uint32_t>(sr.data.data() + slot.offset));
            std::printf("   %s %5zu%s\n", sr.signature.str().c_str(), sr.data.size(), refs.c_str());
        }
        for (const auto& is : layout.issues)
            std::printf("   ! %s %s\n", is.sub.str().c_str(), is.message.c_str());
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

int cmdPipeline(const Args& a)
{
    auto cfg = makeConfig(a);
    if (cfg.databasePath.empty())
        cfg.databasePath = platform::applicationDataDir() / "multiplexer.sqlite";
    Pipeline p(cfg);
    ConsoleProgress progress;

    p.scan(progress);
    if (a.command == "scan") {
        printDashboard(p);
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
    std::printf("Output written to %s\n", pathToUtf8(result.directory).c_str());
    for (const auto& f : result.pluginFiles)
        std::printf("  %s\n", pathToUtf8(f.filename()).c_str());
    std::printf("Next steps:\n  1. Enable the output plugins (see %s)\n  2. Disable the compiled plugins (%s)\n"
                "  3. Merge %s into Fallout4Custom.ini so their archives still load\n",
                pathToUtf8(result.pluginsTxt.filename()).c_str(), pathToUtf8(result.disableList.filename()).c_str(),
                pathToUtf8(result.archiveIni.filename()).c_str());
    return p.validation()->critical() ? 2 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        Args a = parseArgs(argc, argv);
        log::init(platform::applicationDataDir() / "logs" / "fo4mx.log", a.has("--verbose") || a.has("-v"));
        if (a.command.empty() || a.command == "help" || a.has("--help")) {
            std::fputs(kUsage, stdout);
            return a.command.empty() ? 1 : 0;
        }
        if (a.command == "info")
            return cmdInfo(a);
        if (a.command == "dump")
            return cmdDump(a);
        if (a.command == "ba2")
            return cmdBa2(a);
        if (a.command == "scan" || a.command == "analyse" || a.command == "analyze" || a.command == "compile")
            return cmdPipeline(a);
        std::fprintf(stderr, "Unknown command '%s'\n\n%s", a.command.c_str(), kUsage);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
