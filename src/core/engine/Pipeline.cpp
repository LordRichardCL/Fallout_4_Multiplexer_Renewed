#include "engine/Pipeline.hpp"

#include "engine/CompilationResult.hpp"
#include "engine/PostCompilation.hpp"
#include "engine/OutputBackup.hpp"
#include "reference/FormIdReference.hpp"
#include "util/FileTransaction.hpp"

#include "diagnostics/RecorderPackage.hpp"

#include "db/DatabaseWriter.hpp"
#include "engine/CleaningEngine.hpp"
#include "report/ReportGenerator.hpp"
#include "util/IniFile.hpp"
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
    // The Auto-Merger's copies in the output folder are never read as originals, and archives an
    // earlier build added to the INI list count as their plugin's.
    auto outputDir = config_.exportOptions.outputDirectory.empty() ? Exporter::defaultOutputDirectory(*session_)
                                                                   : config_.exportOptions.outputDirectory;
    // The build's own records ship inside the package's Data folder: when the output folder is gone
    // (deleted between builds) but the package is installed, they are read from the game's Data.
    if (!std::filesystem::exists(outputDir / "Multiplexer_FormMap.tsv"))
        for (const auto* f : session_->vfs.rootFiles())
            if (iequals(f->relativePath, "Multiplexer_FormMap.tsv")) {
                outputDir = session_->vfs.winnerPath(*f).parent_path();
                spdlog::info("Reading the previous build's records from the installed package in {}",
                             pathToUtf8(outputDir));
                break;
            }
    session_->buildRecordsDirectory = outputDir;
    session_->relinkedCopies = AutoMerger::readManifest(outputDir);
    session_->archiveWinnerCopies = Exporter::archiveWinnerCopies(outputDir);
    session_->registeredArchives.clear();
    for (const auto& a : Exporter::registeredArchives(outputDir))
        session_->registeredArchives.insert(a);
    // Also from the oldest backup of Fallout4Custom.ini (written before this app first changed it):
    // archives of active plugins in today's list that were not in that list were added since. This
    // holds when the output folder and the database were deleted between builds.
    if (!session_->install.iniDirectory.empty()) {
        std::error_code ec;
        std::filesystem::path oldest;
        for (const auto& entry : std::filesystem::directory_iterator(session_->install.iniDirectory, ec)) {
            const std::string name = toLower(pathToUtf8(entry.path().filename()));
            if (name.rfind("fallout4custom.ini.multiplexer-backup-", 0) == 0 &&
                (oldest.empty() || name < toLower(pathToUtf8(oldest.filename()))))
                oldest = entry.path();
        }
        if (!oldest.empty()) {
            IniFile before;
            before.load(oldest);
            std::unordered_set<std::string> original;
            for (const auto& a : split(before.get("Archive", "SResourceArchiveList2", ""), ','))
                original.insert(toLower(a));
            for (const auto& a : split(session_->install.ini.get("Archive", "SResourceArchiveList2", ""), ',')) {
                const std::string lower = toLower(a);
                if (lower.empty() || original.count(lower) || !iendsWith(lower, ".ba2"))
                    continue;
                for (size_t sep = lower.find(" - "); sep != std::string::npos; sep = lower.find(" - ", sep + 1))
                    for (const char* ext : {".esp", ".esm", ".esl"})
                        if (const auto* p = session_->findPlugin(lower.substr(0, sep) + ext);
                            p && p->active && !p->baseGame)
                            session_->registeredArchives.insert(lower);
            }
        }
    }
    // Also from the last build in the database (the file lives in a folder a mod manager manages):
    // archives of plugins it compiled that are now in the INI list were put there by it.
    for (const auto& a : session_->install.iniArchives()) {
        const std::string lower = toLower(a);
        if (!iendsWith(lower, ".ba2"))
            continue;
        for (const auto& name : session_->reactivate) {
            const std::string base = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
            if (lower.size() > base.size() + 3 && lower.compare(0, base.size() + 3, base + " - ") == 0)
                session_->registeredArchives.insert(lower);
        }
    }
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
    // Cross-check with the independent reference of the base-game masters, when it is installed.
    try {
        if (auto ref = FormIdReference::tryLoad(config_.formIdReference)) {
            progress.stage("Comparing with the FormID reference");
            const auto cmp = compareWithReference(*ref, *session_);
            reportReferenceComparison(cmp, check.report);
            spdlog::info("FormID reference: {} plugin(s) compared, {}", cmp.plugins.size(),
                         cmp.agrees() ? "all agree" : "differences found (see Reference Check)");
        }
    } catch (const std::exception& e) {
        check.report.fail(Severity::Warning, "Reference Check", std::string("The FormID reference could not be read: ") + e.what());
    }
    openDatabase();
    if (db_)
        db::DatabaseWriter(*db_).writeValidation("definitions", check.report, false);
    definitionCheck_ = check.report;
    return check;
}

const ValidationReport* Pipeline::reportValidation() const
{
    // A definition check run before the last scan (which starts a fresh pipeline in the app) is
    // still in the database.
    if (!definitionCheck_ && db_)
        if (auto stored = db::DatabaseWriter(*db_).loadValidation("definitions"))
            definitionCheck_ = std::move(*stored);
    // This analysis's candidate check is current; a full definition check (with the group layout
    // check) is used when there is no candidate check.
    const ValidationReport* extra = candidateCheck_ ? &*candidateCheck_ : definitionCheck_ ? &*definitionCheck_ : nullptr;
    if (!extra)
        return validation();
    reportValidation_ = validation_ ? *validation_ : ValidationReport{};
    reportValidation_->merge(*extra);
    if (candidateCheck_ && definitionCheck_)
        for (const auto& is : definitionCheck_->issues) // the group layout result is only in the full check
            if (is.category == "Group Layout")
                reportValidation_->issues.push_back(is);
    return &*reportValidation_;
}

void Pipeline::analyse(ProgressSink& progress)
{
    if (!session_->scanned)
        scan(progress);
    progress.stage("Analyse");
    conflicts_ = ConflictEngine::analyse(*session_, config_.conflicts, progress);
    session_->autoMergeEnabled = config_.compile.autoMerge;
    if (config_.compile.autoMerge) {
        progress.stage("Auto-Merger: checking scripts and configs");
        autoMerge_ = AutoMerger::analyse(*session_);
    } else {
        autoMerge_.reset();
    }
    dependencies_ = DependencyEngine::analyse(*session_, config_.analysis, progress);
    CleaningEngine::analyse(*session_, *conflicts_, *dependencies_);
    // The Candidate types (described, not compiled yet) are checked against this load order every
    // time, so the Validation Report always says whether they are confirmed.
    {
        std::vector<Signature> candidates;
        for (const auto& e : session_->defs.catalogue().entries())
            if (session_->defs.strategy(e.sig) == MergeStrategy::Candidate)
                candidates.push_back(e.sig);
        candidateCheck_.reset();
        if (!candidates.empty()) {
            progress.stage("Checking candidate record types");
            candidateCheck_ =
                ValidationEngine::checkDefinitions(*session_, candidates, progress, config_.scan.threads, false).report;
        }
    }
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
        auto dir = session_->buildRecordsDirectory.empty()
                       ? (config_.exportOptions.outputDirectory.empty() ? Exporter::defaultOutputDirectory(*session_)
                                                                        : config_.exportOptions.outputDirectory)
                       : session_->buildRecordsDirectory;
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
        loadersOff = std::string("the output is the installable package, the same for every mod manager (") +
                     installKindName(session_->install.config.kind) +
                     " here); archive loaders are only written when the output folder is a mod inside Mod Organizer "
                     "2's mods folder";
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
            loadersOff = "the output is the installable package (" + pathToUtf8(outputDir) +
                         "), the same for every mod manager; archive loaders are only written when the output folder "
                         "is a mod inside Mod Organizer's mods folder (" + pathToUtf8(session_->install.modsPath) + ")";
        }
    }
    std::map<std::string, std::string> isolatedPatchRecords; // record label -> finding
    if (autoMerge_)
        autoMerge_->keptOut.clear();
    for (int round = 0;; ++round) {
        DependencyEngine::evaluateEligibility(*session_, config_.analysis, dependencies_ ? &*dependencies_ : nullptr);
        compiled_ = CompilationEngine::compile(*session_, options, progress);
        validation_ = ValidationEngine::validate(*session_, *compiled_, progress);
        size_t isolated = 0;
        // Advanced Users Auto-Merger: plan and check the rewritten copies; a plugin whose copy fails a
        // check stays in the load order (fail closed) and the compilation runs again.
        if (autoMerge_)
            for (const auto& [pi, why] : AutoMerger::plan(*session_, *compiled_, outputDir, *autoMerge_))
                if (session_->autoRetained.emplace(session_->plugins[pi].key, why).second)
                    ++isolated;
        // A compiled plugin whose archives would lose files to other archives (INI list) changes what
        // the game shows: it stays in the load order instead.
        if (config_.keepArchivePriority)
            for (const auto& [pi, files] : ValidationEngine::archiveLosers(*session_, *compiled_))
                if ((!config_.archiveWinnerCopies || files > config_.archiveWinnerCopyLimit) &&
                    session_->autoRetained
                        .emplace(session_->plugins[pi].key,
                                 "Archive priority: " + std::to_string(files) +
                                     " files its archives provide today would be served by other archives once "
                                     "they load from the INI list (before every plugin's own archives)" +
                                     (config_.archiveWinnerCopies
                                          ? ", more than the " + std::to_string(config_.archiveWinnerCopyLimit) +
                                                " kept as loose copies"
                                          : std::string()) +
                                     ". On Mod Organizer 2, with the output inside the mods folder, archive "
                                     "loaders keep their place")
                        .second)
                    ++isolated;
        if (isolated && round < 8)
            continue;
        if (!validation_->critical() || !config_.autoIsolate || round >= 8)
            break;
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
                          (p ? p->name : key) +
                              (reason.rfind("Advanced Users Auto-Merger", 0) == 0 ||
                                       reason.rfind("Archive priority", 0) == 0
                                   ? " stays in the load order: " + reason
                                   : " stays in the load order because one of its records could not be written "
                                     "safely: " + reason),
                          p ? p->name : key);
    }
    if (autoMerge_)
        for (const auto& f : autoMerge_->files) {
            if (f.validation.empty())
                continue;
            for (auto pi : f.plugins)
                if (compiled_->isCompiled(pi))
                    validation_->fail(Severity::Error, "Auto-Merger",
                                      session_->plugins[pi].name + " is compiled but " + f.path +
                                          " could not be rewritten safely: " + f.validation,
                                      session_->plugins[pi].name);
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

    // Make the baseline visible: archives an earlier build put in the INI list count as their plugin's.
    if (!session_->registeredArchives.empty() || session_->restoredArchives)
        validation_->fail(Severity::Info, "Archive Priority",
                          std::to_string(session_->restoredArchives) + " archives in the INI list were added by an "
                          "earlier build (" + std::to_string(session_->registeredArchives.size()) +
                          " listed in Multiplexer_RegisteredArchives.txt); they are compared as their plugin's own "
                          "archives, where the game loaded them before");
    archiveChanges_ = ValidationEngine::checkArchivePriority(*session_, *compiled_);
    if (!loadersOff.empty() && !compiled_->archivesToRegister.empty())
        validation_->fail(Severity::Info, "Archive Loaders",
                          "Archive loaders are not used because " + loadersOff + ". Compiled plugins' archives are "
                          "added to the INI list instead (Apply to Game).");
    if (!archiveChanges_.empty() && config_.archiveWinnerCopies) {
        validation_->fail(Severity::Info, "Archive Priority",
                          std::to_string(archiveChanges_.size()) +
                              " files a compiled plugin's archive serves today would be served by another archive "
                              "once compiled plugins' archives load from the INI list; they are written loose into "
                              "the output folder on export (Multiplexer_ArchiveWinners.tsv), so the same version "
                              "wins");
        validation_->pass();
    } else if (!archiveChanges_.empty()) {
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
        // Compiling never changes asset paths: a file no installed mod provides was already missing for
        // the source plugin (the game shows the same result). Summed up, with examples.
        size_t missing = 0;
        for (const auto& a : assetTrace_.assets) {
            if (a.found || !a.via.empty())
                continue;
            if (++missing > 50)
                continue;
            const auto& e = compiled_->records[a.record];
            validation_->fail(Severity::Info, "Missing Assets",
                              session_->formLabel(e.key) + " uses " + a.path +
                                  ", which no installed mod provides (as for its source plugin)",
                              compiled_->outputName(e.output), formatFormId(e.rawFormId));
        }
        if (missing)
            validation_->fail(Severity::Info, "Missing Assets",
                              std::to_string(missing) + " asset paths named by compiled records are provided by no "
                              "installed mod. Compiling does not change asset paths, so they were missing for the "
                              "source plugins too (the game shows the same result); the first 50 are listed");
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

// A failed export leaves nothing that looks like a result: the plugins it wrote and the package
// zip are removed, relinked copies are rolled back, and a note says why.
void Pipeline::markBuildFailed(const std::filesystem::path& directory, const std::filesystem::path& packageRoot,
                               const std::vector<std::filesystem::path>& written, const std::string& reason)
{
    std::error_code ec;
    for (const auto& f : written)
        std::filesystem::remove(f, ec);
    std::filesystem::remove(packageRoot / "Multiplexer Output.zip", ec);
    AutoMerger::rollback(directory);
    std::filesystem::create_directories(packageRoot, ec);
    std::ofstream out(packageRoot / kBuildFailedMarker, std::ios::binary | std::ios::trunc);
    out << "This build FAILED and must not be installed.\n\n"
        << "Reason: " << reason << "\n\n"
        << "The output plugins of this build and the package zip were removed; relinked scripts and configs were "
           "rolled back. Anything else in this folder is left over and is not a current result. Fix the problem "
           "named above (the Validation page lists every finding) and build again; a successful export removes "
           "this file.\n";
    spdlog::error("Export failed: {}", reason);
}

std::vector<std::filesystem::path> Pipeline::workingFiles() const
{
    // The results database and the build cache stay open while the app runs (with SQLite's side files).
    std::vector<std::filesystem::path> out;
    auto add = [&](const std::filesystem::path& p) {
        if (p.empty())
            return;
        for (const char* suffix : {"", "-journal", "-wal", "-shm"}) {
            auto f = p;
            f += suffix;
            out.push_back(f);
        }
    };
    add(config_.databasePath);
    add(config_.cachePath.empty() && !config_.databasePath.empty() ? BuildCache::defaultPathFor(config_.databasePath)
                                                                   : config_.cachePath);
    return out;
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
    auto exportOptions = config_.exportOptions;
    exportOptions.archiveWinnerCopies = config_.archiveWinnerCopies;
    // Every export starts from an empty output folder: the previous output is moved, unchanged, into
    // "Multiplexer Outputs Backup DDMMYY HHMMSS" beside it first. If that cannot be done, nothing is
    // changed and the export stops (the previous output is never deleted or overwritten).
    session_->releaseFiles(); // Windows cannot move files this app has open
    const auto ownFiles = workingFiles();
    const auto backup = output::backupPreviousOutput(directory, session_->install.dataPath, {}, ownFiles);
    if (backup.moved)
        previousOutputs.clear(); // nothing of the last build is left to remove or compare with
    if (config_.beforeExportWrite)
        config_.beforeExportWrite(directory);
    ExportResult result;
    try {
        result = Exporter::write(*session_, *compiled_, exportOptions, previousOutputs);
        result.backupFolder = backup.backupFolder;
    } catch (const std::exception& e) {
        markBuildFailed(directory, Exporter::packageRoot(directory), {}, e.what());
        throw;
    }
    {
        auto list = result.packageRoot / "Multiplexer_ArchivePriority.txt";
        std::error_code ec;
        // With loose copies the winners do not change: the list would only mislead.
        if (archiveChanges_.empty() || config_.archiveWinnerCopies) {
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
    if (post.critical()) {
        // The files on disk are not what was compiled: nothing of this build may look installable,
        // and no script or config is relinked to it.
        std::string first;
        for (const auto& is : post.issues)
            if (is.severity == Severity::Error) {
                first = is.message;
                break;
            }
        markBuildFailed(result.directory, result.packageRoot, result.pluginFiles,
                        "the written plugins failed verification (" + std::to_string(post.errors) + " errors; first: " +
                            first + ")");
        validation_->merge(post);
        if (db_)
            db::DatabaseWriter(*db_).writeValidation("post-export", post, false);
        spdlog::error("Written files failed verification ({} errors); the build was marked failed", post.errors);
        exported_ = result;
        return result;
    }
    {
        std::error_code ec;
        std::filesystem::remove(result.packageRoot / kBuildFailedMarker, ec);
    }
    {
        // Relinked scripts and configs (or removal of an earlier export's copies when it is off).
        AutoMergeReport none;
        AutoMergeReport& am = config_.compile.autoMerge && autoMerge_ ? *autoMerge_ : none;
        AutoMerger::relink(*session_, result.directory, am);
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
    if (post.critical()) {
        // A compiled plugin is still named by a file that could not be relinked: not a usable build.
        markBuildFailed(result.directory, result.packageRoot, result.pluginFiles,
                        "the Advanced Users Auto-Merger could not relink every file naming a compiled plugin (" +
                            std::to_string(post.errors) + " errors)");
        exported_ = result;
        return result;
    }
    // Every file is in Data now: zip it into the installable package. Not when the output folder is the
    // game's own Data folder: a package of it would hold the whole game.
    if (backup.gameDataOutput)
        spdlog::warn("No Multiplexer Output.zip: the output folder is the game's Data folder");
    if (config_.exportOptions.writeZip && !post.critical() && !backup.gameDataOutput) {
        try {
            Exporter::writePackageZip(result, workingFiles());
        } catch (const std::exception& e) {
            spdlog::error("Could not write Multiplexer Output.zip: {}", e.what());
        }
    }
    // The optional Diagnostic Recorder, beside the package (installing the package never installs it).
    {
        const auto base = result.packageRoot == result.directory ? result.directory.parent_path() : result.packageRoot;
        if (config_.diagnosticRecorder) {
            const auto rp = diagnostics::writeRecorderPackage(config_.diagnosticRecorderDll, base);
            if (rp.written) {
                result.recorderFolder = rp.folder;
                spdlog::info("Diagnostic Recorder ready to install: {}", pathToUtf8(rp.zip));
            } else {
                result.recorderProblem = rp.problem;
                spdlog::warn("Diagnostic Recorder not prepared: {}", rp.problem);
            }
        } else {
            diagnostics::removeRecorderPackage(base);
        }
    }
    // The guide to switching over to this build, beside the package.
    try {
        const auto cr = makeCompilationResult(*session_, &*compiled_, &result, &*validation_);
        const auto check = PostCompilation::check(*session_, *compiled_, cr);
        atomicWriteText(result.packageRoot / PostCompilation::kInstructionsFile,
                        PostCompilation::instructions(*session_, cr, check));
        for (const auto& b : check.blockers)
            spdlog::error("Before switching over: {}", b);
        for (const auto& w : check.warnings)
            spdlog::warn("Before switching over: {}", w);
    } catch (const std::exception& e) {
        spdlog::warn("Could not write {}: {}", PostCompilation::kInstructionsFile, e.what());
    }
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
    auto reportDir = config_.reportDirectory.empty() ? result.packageRoot / "Multiplexer Reports" : config_.reportDirectory;
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
    ctx.validation = reportValidation();
    ctx.autoMerge = autoMerge();
    auto files = ReportGenerator::writeAll(ctx, directory);
    spdlog::info("Wrote {} report files to {}", files.size(), pathToUtf8(directory));
    return files;
}

} // namespace fo4mx
