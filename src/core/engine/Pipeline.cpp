#include "engine/Pipeline.hpp"

#include "db/DatabaseWriter.hpp"
#include "engine/CleaningEngine.hpp"
#include "report/ReportGenerator.hpp"
#include "util/Text.hpp"

#include <fstream>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

const RecordDefinitions& definitions(const PipelineConfig& config)
{
    return config.definitions ? *config.definitions : RecordDefinitions::get(config.install.runtime);
}

} // namespace

Pipeline::Pipeline(PipelineConfig config)
    : config_(std::move(config)), session_(std::make_unique<Session>(definitions(config_)))
{
}

Pipeline::~Pipeline() = default;

void Pipeline::openDatabase()
{
    if (config_.databasePath.empty() || db_)
        return;
    db_ = std::make_unique<db::Database>(config_.databasePath);
    db::DatabaseWriter(*db_).initSchema();
}

void Pipeline::openCache()
{
    if (!config_.useCache || cache_)
        return;
    auto path = config_.cachePath;
    if (path.empty()) {
        if (config_.databasePath.empty())
            return;
        path = BuildCache::defaultPathFor(config_.databasePath);
    }
    try {
        cache_ = std::make_unique<BuildCache>(path);
    } catch (const std::exception& e) {
        spdlog::warn("Build cache unavailable ({}); continuing without incremental reuse", e.what());
    }
}

uint64_t Pipeline::currentSettingsHash() const
{
    return ChangeDetector::settingsHash(*session_, config_.compile, config_.analysis.excludedPlugins);
}

ChangeReport Pipeline::finishChangeReport(const std::optional<InstallSnapshot>& build, InstallSnapshot current,
                                          uint64_t settings)
{
    if (build)
        ChangeDetector::fingerprintOutputs(*build, current);
    ChangeReport r = ChangeDetector::compare(build ? &*build : nullptr, current);
    if (build && build->isBuild && build->settingsHash != settings) {
        r.items.push_back({"Settings", "Changed", "Compilation settings", "output count, strategy, rules, exclusions or resolutions", true});
        r.reasons.push_back("Compilation settings changed");
        r.status = BuildStatus::RequiresRebuild;
    }
    return r;
}

ChangeReport Pipeline::checkForChanges(ProgressSink& progress)
{
    openDatabase();
    std::optional<InstallSnapshot> build;
    std::vector<std::string> resolutions;
    if (db_) {
        db::DatabaseWriter w(*db_);
        build = w.loadSnapshot("build");
        resolutions = w.resolutionStrings();
    }
    auto current = ChangeDetector::captureQuick(config_.install, build ? &*build : nullptr, progress);
    uint64_t settings = ChangeDetector::settingsHash(config_.compile, config_.analysis.excludedPlugins,
                                                     definitionsId(definitions(config_)),
                                                     std::move(resolutions));
    changes_ = finishChangeReport(build, std::move(current), settings);
    if (db_)
        db::DatabaseWriter(*db_).writeChanges(changes_);
    spdlog::info("Build status: {} ({} changes)", buildStatusName(changes_.status), changes_.items.size());
    return changes_;
}

void Pipeline::scan(ProgressSink& progress)
{
    session_ = std::make_unique<Session>(definitions(config_));
    openCache();
    session_->cache = cache_.get();
    if (cache_)
        cache_->resetCounters();
    conflicts_.reset();
    dependencies_.reset();
    compiled_.reset();
    validation_.reset();
    exported_.reset();

    progress.stage("Scan");
    openDatabase();
    if (db_)
        if (auto build = db::DatabaseWriter(*db_).loadSnapshot("build"); build && build->isBuild)
            for (const auto& name : build->compiledPlugins)
                session_->reactivate.insert(toLower(name));
    spdlog::info("Reading {} configuration", installKindName(config_.install.kind));
    session_->install = resolveInstall(config_.install);
    for (const auto& name : config_.analysis.excludedPlugins)
        session_->userRetained.insert(toLower(name));

    progress.stage("Building virtual file system");
    populateVfs(session_->install, session_->vfs, progress);
    spdlog::info("Virtual Data folder: {} files from {} sources", session_->vfs.files().size(),
                 session_->vfs.roots().size());

    PluginScanner::scan(*session_, config_.scan, progress);
    AssetScanner::scan(*session_, config_.assets, progress);
    DependencyEngine::evaluateEligibility(*session_, config_.analysis);

    openDatabase();
    if (db_) {
        db::DatabaseWriter w(*db_);
        w.writeScan(*session_, progress);
        // Fingerprint the mod space and compare it with the last build.
        auto snapshot = ChangeDetector::capture(*session_);
        w.saveSnapshot("scan", snapshot);
        auto build = w.loadSnapshot("build");
        auto forced = config_.compile.forcedWinners;
        for (const auto& [k, v] : w.loadResolutions(*session_))
            forced.emplace(k, v);
        CompileOptions effective = config_.compile;
        effective.forcedWinners = std::move(forced);
        changes_ = finishChangeReport(build, std::move(snapshot),
                                      ChangeDetector::settingsHash(*session_, effective, config_.analysis.excludedPlugins));
        w.writeChanges(changes_);
    }
}

DefinitionCheck Pipeline::checkDefinitions(const std::vector<Signature>& types, ProgressSink& progress)
{
    if (!session_->scanned)
        scan(progress);
    auto check = ValidationEngine::checkDefinitions(*session_, types, progress, config_.scan.threads);
    openDatabase();
    if (db_)
        db::DatabaseWriter(*db_).writeValidation("definitions", check.report, false);
    return check;
}

void Pipeline::analyse(ProgressSink& progress)
{
    if (!session_->scanned)
        scan(progress);
    progress.stage("Analyse");
    conflicts_ = ConflictEngine::analyse(*session_, config_.conflicts, progress);
    if (config_.compile.autoMerge) {
        progress.stage("Auto-Merger: checking scripts and configs");
        autoMerge_ = AutoMerger::analyse(*session_);
        session_->autoMergeRelinkable = autoMerge_->relinkable;
    } else {
        autoMerge_.reset();
        session_->autoMergeRelinkable.clear();
    }
    dependencies_ = DependencyEngine::analyse(*session_, config_.analysis, progress);
    CleaningEngine::analyse(*session_, *conflicts_, *dependencies_);
    compiled_.reset();
    validation_.reset();
    if (db_)
        db::DatabaseWriter(*db_).writeAnalysis(*session_, *conflicts_, *dependencies_, progress);
    if (db_)
        db::DatabaseWriter(*db_).writeAutoMerge(*session_, autoMerge(), nullptr);
}

void Pipeline::refreshEligibility()
{
    session_->userRetained.clear();
    for (const auto& name : config_.analysis.excludedPlugins)
        session_->userRetained.insert(toLower(name));
    DependencyEngine::evaluateEligibility(*session_, config_.analysis,
                                          dependencies_ ? &*dependencies_ : nullptr);
    if (db_)
        db::DatabaseWriter(*db_).updatePlugins(*session_);
}

bool Pipeline::compile(ProgressSink& progress)
{
    if (!conflicts_)
        analyse(progress);
    if (db_) {
        auto resolutions = db::DatabaseWriter(*db_).loadResolutions(*session_);
        for (const auto& [k, v] : resolutions)
            config_.compile.forcedWinners.emplace(k, v);
    }
    if (config_.compile.previousFormMap.empty()) {
        auto dir = config_.exportOptions.outputDirectory.empty() ? Exporter::defaultOutputDirectory(*session_)
                                                                 : config_.exportOptions.outputDirectory;
        auto previous = dir / "Multiplexer_FormMap.tsv";
        if (std::filesystem::exists(previous))
            config_.compile.previousFormMap = previous;
    }
    // Compile, and while validation finds records that cannot be written safely, set their source
    // plugins (or patch records) aside and compile again. Each round only ever removes work.
    session_->autoRetained.clear();
    CompileOptions options = config_.compile;

    // Archive loaders need the output mod to override the original plugin files, which only a
    // Mod Organizer 2 mod folder does. Anywhere else they would overwrite the originals.
    const auto outputDir = config_.exportOptions.outputDirectory.empty()
                               ? Exporter::defaultOutputDirectory(*session_)
                               : config_.exportOptions.outputDirectory;
    std::string outputMod;
    std::string loadersOff; // why archive loaders are not used, for the validation report
    if (!config_.archiveLoaders) {
        loadersOff = "archive loaders are turned off";
    } else if (session_->install.config.kind != InstallKind::ModOrganizer2) {
        loadersOff = std::string("the install is managed by ") + installKindName(session_->install.config.kind) +
                     ", where a loader would overwrite the original plugin in Data (Mod Organizer 2 only)";
    } else if (session_->install.modsPath.empty()) {
        loadersOff = "Mod Organizer's mods folder is unknown";
    } else {
        // Windows paths differ in letter case and slash direction between MO2's INI and a folder
        // picked by hand: compare them normalised.
        auto norm = [](const std::filesystem::path& p) {
            std::string g = toLower(pathToUtf8(std::filesystem::path(p).lexically_normal().generic_u8string()));
            while (g.size() > 1 && g.back() == '/')
                g.pop_back();
            return g;
        };
        std::string mods = norm(session_->install.modsPath) + "/";
        std::string out = norm(outputDir);
        if (out.rfind(mods, 0) == 0 && out.size() > mods.size()) {
            std::string rest = pathToUtf8(std::filesystem::path(outputDir).lexically_normal().generic_u8string());
            rest = rest.substr(rest.size() - (out.size() - mods.size())); // original spelling
            outputMod = rest.substr(0, rest.find('/'));
            options.archiveLoaders = true;
        } else {
            loadersOff = "the output folder (" + pathToUtf8(outputDir) + ") is not inside Mod Organizer's mods folder (" +
                         pathToUtf8(session_->install.modsPath) +
                         "); leave Output folder empty in Setup to use the default MO2 output mod";
        }
    }
    std::map<std::string, std::string> isolatedPatchRecords; // record label -> finding
    for (int round = 0;; ++round) {
        DependencyEngine::evaluateEligibility(*session_, config_.analysis, dependencies_ ? &*dependencies_ : nullptr);
        compiled_ = CompilationEngine::compile(*session_, options, progress);
        validation_ = ValidationEngine::validate(*session_, *compiled_, progress);
        if (!validation_->critical() || !config_.autoIsolate || round >= 8)
            break;
        size_t isolated = 0;
        for (const auto& is : validation_->issues) {
            if (is.severity != Severity::Error || !is.record.valid())
                continue;
            auto it = compiled_->indexByKey.find(is.record);
            if (it == compiled_->indexByKey.end())
                continue;
            const auto& e = compiled_->records[it->second];
            if (e.patch) {
                if (options.skipPatchRecords.insert(e.key.packed()).second) {
                    isolatedPatchRecords.emplace(session_->formLabel(e.key), is.message);
                    ++isolated;
                }
                continue;
            }
            std::vector<PluginIndex> sources;
            if (e.isNew)
                sources.push_back(e.key.origin);
            else
                for (auto pi : e.chain)
                    if (compiled_->isCompiled(pi))
                        sources.push_back(pi);
            for (auto pi : sources)
                if (session_->autoRetained.emplace(session_->plugins[pi].key, is.message).second)
                    ++isolated;
        }
        if (isolated == 0)
            break; // the remaining errors are not tied to a plugin or record
        spdlog::info("Validation round {}: kept {} plugins and {} patch records aside; compiling again", round + 1,
                     session_->autoRetained.size(), options.skipPatchRecords.size());
    }
    for (const auto& [key, reason] : session_->autoRetained) {
        const auto* p = session_->findPlugin(key);
        validation_->fail(Severity::Warning, "Kept Out Automatically",
                          (p ? p->name : key) + " stays in the load order because one of its records could not be "
                                                "written safely: " + reason,
                          p ? p->name : key);
    }
    for (const auto& [label, reason] : isolatedPatchRecords)
        validation_->fail(Severity::Warning, "Kept Out Automatically",
                          label + " was left out of the patch (last plugin wins for it): " + reason);
    if (db_ && !session_->autoRetained.empty())
        db::DatabaseWriter(*db_).updatePlugins(*session_);

    if (!compiled_->archiveLoaders.empty()) {
        // The output mod must win over every mod whose plugin it replaces with a loader.
        const ModInfo* out = nullptr;
        for (const auto& m : session_->install.mods)
            if (iequals(m.name, outputMod))
                out = &m;
        std::vector<std::string> above;
        for (auto pi : compiled_->archiveLoaders) {
            const auto& p = session_->plugins[pi];
            for (const auto& m : session_->install.mods)
                if (out && iequals(m.name, p.sourceMod) && m.priority > out->priority)
                    above.push_back(p.name + " (" + m.name + ")");
        }
        if (!out)
            validation_->fail(Severity::Info, "Archive Loaders",
                              std::to_string(compiled_->archiveLoaders.size()) +
                                  " compiled plugins keep their archives through archive loaders in '" + outputMod +
                                  "'. After exporting, refresh Mod Organizer and keep that mod at the bottom of the "
                                  "left pane so it overrides the original plugins.");
        else if (!above.empty())
            validation_->fail(Severity::Warning, "Archive Loaders",
                              "Move '" + outputMod + "' to the bottom of Mod Organizer's left pane: these mods are "
                              "below it, so their original plugins would win over the archive loaders: " +
                                  join(above, ", "));
        else
            validation_->pass();
    }

    archiveChanges_ = ValidationEngine::checkArchivePriority(*session_, *compiled_);
    if (!loadersOff.empty() && !compiled_->archivesToRegister.empty())
        validation_->fail(Severity::Info, "Archive Loaders",
                          "Archive loaders are not used because " + loadersOff + ". Compiled plugins' archives are "
                          "added to the INI list instead (Apply to Game).");
    if (!archiveChanges_.empty()) {
        validation_->fail(Severity::Warning, "Archive Priority",
                          std::to_string(archiveChanges_.size()) +
                              " files would be served by a different archive once compiled plugins' archives load "
                              "from the INI list (they load before every plugin's own archives). The full list is "
                              "written to Multiplexer_ArchivePriority.txt on export. To keep a compiled mod's "
                              "version, exclude that mod from compilation or provide the file loose.");
        size_t shown = 0;
        for (const auto& c : archiveChanges_) {
            if (++shown > 50)
                break;
            validation_->fail(Severity::Warning, "Archive Priority",
                              c.path + ": " + c.before + " (" + c.beforeMod + ") wins today; " + c.after + " (" +
                                  c.afterMod + ") would win after switching");
        }
    } else {
        validation_->pass();
    }
    assetTrace_ = AssetTrace{};
    if (config_.traceAssets) {
        assetTrace_ = AssetTracer::trace(*session_, *compiled_, true, progress);
        for (const auto& a : assetTrace_.assets) {
            if (a.found || !a.via.empty())
                continue;
            const auto& e = compiled_->records[a.record];
            validation_->fail(Severity::Warning, "Missing Assets",
                              session_->formLabel(e.key) + " uses " + a.path + ", which no installed mod provides",
                              compiled_->outputName(e.output), formatFormId(e.rawFormId));
        }
        if (assetTrace_.viaRegisteredArchives)
            validation_->fail(Severity::Info, "Assets",
                              std::to_string(assetTrace_.viaRegisteredArchives) +
                                  " assets come from archives of compiled plugins; they keep loading from their "
                                  "original archives through Multiplexer_Archives.ini");
    }
    exported_.reset();
    if (db_) {
        db::DatabaseWriter w(*db_);
        w.writeCompilation(*session_, *compiled_);
        w.writeAssetTrace(*compiled_, assetTrace_);
        w.writeValidation("pre-export", *validation_, true);
    }
    return !validation_->critical();
}

ExportResult Pipeline::exportOutputs(ProgressSink& progress)
{
    if (!compiled_)
        throw std::runtime_error("nothing compiled yet");
    if (validation_ && validation_->critical())
        throw std::runtime_error("validation found " + std::to_string(validation_->errors) +
                                 " critical errors; export is blocked");
    progress.stage("Export");
    std::vector<std::string> previousOutputs;
    std::optional<InstallSnapshot> build;
    if (db_)
        build = db::DatabaseWriter(*db_).loadSnapshot("build");
    auto directory = config_.exportOptions.outputDirectory.empty() ? Exporter::defaultOutputDirectory(*session_)
                                                                   : config_.exportOptions.outputDirectory;
    if (build && build->isBuild && utf8ToPath(build->outputDirectory) == directory)
        for (const auto& o : build->outputs)
            previousOutputs.push_back(o.name);
    auto result = Exporter::write(*session_, *compiled_, config_.exportOptions, previousOutputs);
    {
        auto list = result.directory / "Multiplexer_ArchivePriority.txt";
        std::error_code ec;
        if (archiveChanges_.empty()) {
            std::filesystem::remove(list, ec);
        } else {
            std::ofstream out(list, std::ios::binary | std::ios::trunc);
            out << "# Files whose winning archive changes after switching to the compiled output\n"
                   "# path\twins today (mod)\twould win (mod)\n";
            for (const auto& c : archiveChanges_)
                out << c.path << '\t' << c.before << " (" << c.beforeMod << ")\t" << c.after << " (" << c.afterMod
                    << ")\n";
        }
    }
    auto post = ValidationEngine::validateWrittenFiles(*compiled_, result.pluginFiles);
    {
        // Relinked scripts and configs (or removal of an earlier export's copies when it is off).
        AutoMergeReport none;
        AutoMergeReport& am = config_.compile.autoMerge && autoMerge_ ? *autoMerge_ : none;
        AutoMerger::relink(*session_, *compiled_, result.directory, am);
        for (const auto& e : am.errors)
            post.fail(Severity::Error, "Auto-Merger",
                      e + ". The plugin was compiled but this file still names it: re-enable it or turn the "
                          "Auto-Merger off and compile again");
        if (am.enabled)
            post.pass(am.written.size());
        if (db_)
            db::DatabaseWriter(*db_).writeAutoMerge(*session_, autoMerge(), &*compiled_);
    }
    validation_->merge(post);
    if (db_)
        db::DatabaseWriter(*db_).writeValidation("post-export", post, false);
    exported_ = result;
    if (db_ && !post.critical()) {
        // This build becomes the reference state for change detection.
        db::DatabaseWriter w(*db_);
        auto snapshot = ChangeDetector::capture(*session_);
        ChangeDetector::addBuild(snapshot, *session_, *compiled_, result, currentSettingsHash());
        w.saveSnapshot("build", snapshot);
        changes_ = ChangeReport{};
        changes_.status = BuildStatus::UpToDate;
        changes_.referenceTime = changes_.checkedTime = snapshot.created;
        w.writeChanges(changes_);
        w.setMeta("export_written", std::to_string(result.writtenOutputs.size()));
        w.setMeta("export_unchanged", std::to_string(result.unchangedOutputs.size()));
        w.setMeta("export_removed", std::to_string(result.removedOutputs.size()));
    }
    auto reportDir = config_.reportDirectory.empty() ? result.directory / "Multiplexer Reports" : config_.reportDirectory;
    writeReports(reportDir);
    if (post.critical())
        spdlog::error("Written files failed verification ({} errors)", post.errors);
    return result;
}

std::vector<std::filesystem::path> Pipeline::writeReports(const std::filesystem::path& directory)
{
    ReportContext ctx;
    ctx.session = session_.get();
    ctx.conflicts = conflicts();
    ctx.dependencies = dependencies();
    ctx.compiled = compiledOutput();
    ctx.validation = validation();
    ctx.autoMerge = autoMerge();
    auto files = ReportGenerator::writeAll(ctx, directory);
    spdlog::info("Wrote {} report files to {}", files.size(), pathToUtf8(directory));
    return files;
}

} // namespace fo4mx
