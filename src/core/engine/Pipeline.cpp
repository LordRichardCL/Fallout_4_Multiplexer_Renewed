#include "engine/Pipeline.hpp"
#include "engine/SlotUsage.hpp"

#include <chrono>

#include "assets/AssetScanner.hpp"
#include "engine/CompilationResult.hpp"
#include "engine/Exporter.hpp"
#include "engine/PluginIni.hpp"
#include "engine/PostCompilation.hpp"
#include "engine/OutputRuns.hpp"
#include "reference/FormIdReference.hpp"
#include "util/FileTransaction.hpp"

#include "diagnostics/RecorderPackage.hpp"

#include "db/DatabaseWriter.hpp"
#include "engine/CleaningEngine.hpp"
#include "report/ReportGenerator.hpp"
#include "util/IniFile.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

const RecordDefinitions& definitions(const PipelineConfig& config)
{
    return config.definitions ? *config.definitions : RecordDefinitions::get(config.install.runtime);
}

std::vector<std::string> conflictingMoves(const Session& s, const std::vector<std::string>& oldOrder, size_t limit);

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
    return ChangeDetector::settingsHash(*session_, config_.compile, config_.analysis.selectionKey());
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
    uint64_t settings = ChangeDetector::settingsHash(config_.compile, config_.analysis.selectionKey(),
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
    plan_.reset();
    dependencies_.reset();
    compiled_.reset();
    validation_.reset();
    exported_.reset();
    restored_.reset();
    restoreProblem_.clear();
    restoreDetails_.clear();

    progress.stage("Scan");
    openDatabase();
    std::optional<BuildRecord> fromDatabase;
    if (db_)
        if (auto build = db::DatabaseWriter(*db_).loadSnapshot("build"); build && build->isBuild) {
            BuildRecord r;
            r.source = "the results database (last build)";
            for (const auto& name : build->compiledPlugins) {
                session_->reactivate.insert(toLower(name));
                r.compiled.push_back(name);
            }
            for (const auto& o : build->outputs)
                r.outputs.push_back(o.name);
            if (!r.outputs.empty())
                fromDatabase = std::move(r);
        }
    spdlog::info("Reading {} configuration", installKindName(config_.install.kind));
    session_->install = resolveInstall(config_.install);
    // What earlier builds compiled, so their outputs in the load order can be matched with it: every
    // finished export (newest first), then the database. The list installed with the outputs is read
    // by the scan and comes before these.
    session_->buildRecords = earlier::recordsFromExports(outputLocation());
    if (fromDatabase)
        session_->buildRecords.push_back(std::move(*fromDatabase));
    for (const auto& name : config_.analysis.excludedPlugins)
        session_->userRetained.insert(toLower(name));

    progress.stage("Building virtual file system");
    populateVfs(session_->install, session_->vfs, progress);
    spdlog::info("Virtual Data folder: {} files from {} sources", session_->vfs.files().size(),
                 session_->vfs.roots().size());

    PluginScanner::scan(*session_, config_.scan, progress);
    // The Auto-Merger's copies in the output folder are never read as originals, and archives an
    // earlier build added to the INI list count as their plugin's. Read from the latest finished export.
    auto outputDir = previousBuildDirectory();
    // The build's own records ship inside the package's Data folder: when the output folder is gone
    // (deleted between builds) but the package is installed, they are read from the game's Data.
    if (outputDir.empty() || !std::filesystem::exists(outputDir / "Multiplexer_FormMap.tsv"))
        for (const auto* f : session_->vfs.rootFiles())
            if (iequals(f->relativePath, "Multiplexer_FormMap.tsv")) {
                outputDir = session_->vfs.winnerPath(*f).parent_path();
                spdlog::info("Reading the previous build's records from the installed package in {}",
                             pathToUtf8(outputDir));
                break;
            }
    session_->buildRecordsDirectory = outputDir;
    session_->relinkedCopies.clear();
    session_->archiveWinnerCopies.clear();
    session_->registeredArchives.clear();
    if (!outputDir.empty()) {
        session_->relinkedCopies = AutoMerger::readManifest(outputDir);
        session_->archiveWinnerCopies = Exporter::archiveWinnerCopies(outputDir);
        for (const auto& a : Exporter::registeredArchives(outputDir)) // written by earlier versions
            session_->registeredArchives.insert(a);
    }
    // Apply to Game keeps its record of the archives it added beside the exports (never inside one).
    for (const auto& a : Exporter::registeredArchives(outputLocation().root))
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
    DependencyEngine::evaluateEligibility(*session_, analysisOptions());

    openDatabase();
    if (db_) {
        db::DatabaseWriter w(*db_);
        auto t0 = std::chrono::steady_clock::now();
        auto since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        };
        // Fingerprint the mod space (it also decides whether the scan tables need writing again).
        auto snapshot = ChangeDetector::capture(*session_);
        // The record, asset and reference tables hold exactly this scan when the mod space, this app's version
        // and the record definitions are what they were written for: then only the plugins are written
        // (their eligibility can differ). About a million rows on a real load order.
        const std::string scanKey =
            [&] {
                auto content = snapshot.toJson();
                content.set("created", 0); // when it was taken is not what it holds
                const std::string text = content.dump(0);
                return std::to_string(fnv1a64(text.data(), text.size()));
            }() + "|" +
            FO4MX_VERSION + "|" + std::to_string(session_->defs.fingerprint()) + "|" +
            std::to_string(session_->records.size()) + "|" + std::to_string(session_->assets.entries.size()) + "|" +
            std::to_string(session_->assets.archives.size()) + "|" + std::to_string(session_->nameReferences.size());
        if (w.meta("scan_key") == scanKey) {
            w.updatePlugins(*session_);
            spdlog::info("Scan results unchanged since the last scan: kept ({:.1f}s)", since(t0));
        } else {
            w.setMeta("scan_key", ""); // cleared until the tables are written in full
            w.writeScan(*session_, progress);
            w.setMeta("scan_key", scanKey);
            spdlog::info("Wrote the scan results in {:.1f}s", since(t0));
        }
        w.saveSnapshot("scan", snapshot);
        auto build = w.loadSnapshot("build");
        auto forced = config_.compile.forcedWinners;
        for (const auto& [k, v] : w.loadResolutions(*session_))
            forced.emplace(k, v);
        CompileOptions effective = config_.compile;
        effective.forcedWinners = std::move(forced);
        changes_ = finishChangeReport(build, std::move(snapshot),
                                      ChangeDetector::settingsHash(*session_, effective, config_.analysis.selectionKey()));
        w.writeChanges(changes_);
    }
    const auto restoreStart = std::chrono::steady_clock::now();
    try {
        restoreLastBuild();
    } catch (const std::exception& e) {
        restored_.reset();
        restoreProblem_ = std::string("The last build could not be reopened: ") + e.what();
        spdlog::warn("{}", restoreProblem_);
    }
    spdlog::info("Checked the last build in {:.1f}s",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - restoreStart).count());
}

Pipeline::ApplySource Pipeline::applySource() const
{
    ApplySource a;
    if (compiled_ && exported_) {
        a.compiled = &*compiled_;
        a.exported = &*exported_;
        a.validation = validation_ ? &*validation_ : nullptr;
    } else if (restored_) {
        a.compiled = &restored_->compiled;
        a.exported = &restored_->exported;
        a.restored = true;
    }
    return a;
}

bool Pipeline::restoreLastBuild()
{
    restored_.reset();
    restoreProblem_.clear();
    restoreDetails_.clear();
    if (!session_ || !session_->scanned)
        return false;
    const auto location = outputLocation();
    if (location.layout == output::Layout::GameData)
        return false; // written straight into Data: no export folder to reopen
    const auto latest = output::latestRun(location);
    if (!latest)
        return false; // never exported
    const std::string run = pathToUtf8(latest->filename());
    auto refuse = [&](std::string why) {
        restoreProblem_ = "The last build (" + run + ") was not reopened: " + std::move(why);
        spdlog::info("{}", restoreProblem_);
        for (const auto& d : restoreDetails_)
            spdlog::info("  {}", d);
        return false;
    };
    std::error_code ec;
    if (!std::filesystem::exists(*latest / buildrecord::kFile, ec))
        return refuse("it was exported by an earlier version of this app, which did not record what Apply to Game "
                      "needs. Compile and export again");
    buildrecord::Record rec;
    try {
        rec = buildrecord::read(*latest);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    if (std::filesystem::exists(*latest / kBuildFailedMarker, ec))
        return refuse("the export did not finish successfully");

    // 1. The load order and settings: the same comparison as the change check, against the build's own
    //    snapshot (the results database may have been cleared since).
    auto report = finishChangeReport(rec.snapshot, ChangeDetector::capture(*session_), currentSettingsHash());
    if (rec.settingsHash != currentSettingsHash() && report.status != BuildStatus::RequiresRebuild) {
        report.reasons.push_back("Compilation settings changed");
        report.status = BuildStatus::RequiresRebuild;
    }
    // A re-sort (LOOT, the mod manager) moves plugins; only a move between plugins that edit the same
    // record changes what the build decided. When moves are the only change, they are checked record by record.
    if (report.status == BuildStatus::RequiresRebuild && rec.settingsHash == currentSettingsHash() &&
        std::all_of(report.items.begin(), report.items.end(),
                    [](const ChangeItem& i) { return !i.affectsBuild || (i.category == "Load Order" && i.kind == "Moved"); })) {
        const auto conflicts = conflictingMoves(*session_, rec.snapshot.loadOrder, 20);
        if (conflicts.empty()) {
            size_t moved = 0;
            for (auto& i : report.items)
                if (i.affectsBuild) {
                    i.affectsBuild = false;
                    ++moved;
                }
            report.reasons.clear();
            report.status = BuildStatus::ChangesDetected;
            spdlog::info("{} plugins moved since the build; none now loads before a plugin it shares a record with, "
                         "so the build still holds", moved);
        } else {
            report.reasons = conflicts;
        }
    }
    if (report.status == BuildStatus::RequiresRebuild) {
        for (const auto& r : report.reasons)
            restoreDetails_.push_back(r);
        for (const auto& i : report.items)
            if (i.affectsBuild && restoreDetails_.size() < 50)
                restoreDetails_.push_back(i.category + " " + i.kind + ": " + i.name +
                                          (i.detail.empty() ? std::string() : " (" + i.detail + ")"));
        return refuse("the load order or the settings changed since it was built. Compile again");
    }

    // 2. Its own files: every output exactly as written, and the archive loader when used.
    RestoredBuild rb;
    rb.run = run;
    auto at = [&](const std::string& rel) { return rel.empty() ? std::filesystem::path() : *latest / rel; };
    for (const auto& o : rec.outputs) {
        const auto file = at(o.file);
        if (file.empty() || !std::filesystem::is_regular_file(file, ec)) {
            restoreDetails_.push_back(o.name + ": missing");
            continue;
        }
        if (std::filesystem::file_size(file, ec) != o.size || buildrecord::hashFile(file).value_or(0) != o.hash) {
            restoreDetails_.push_back(o.name + ": changed since it was exported");
            continue;
        }
        rb.exported.pluginFiles.push_back(file);
    }
    // The F4SE archive loader and its list are always at Data\F4SE\Plugins in the export (exports made
    // before this was recorded correctly listed them without a path).
    const auto dataDir = rec.directory.empty() ? *latest : at(rec.directory);
    std::vector<std::filesystem::path> loaderFiles;
    if (rec.f4seArchiveLoader)
        for (const char* name : {Exporter::kArchiveLoaderDll, Exporter::kArchiveManifest}) {
            const auto f = dataDir / "F4SE" / "Plugins" / name;
            if (!std::filesystem::is_regular_file(f, ec))
                restoreDetails_.push_back(std::string("F4SE\\Plugins\\") + name + ": missing");
            loaderFiles.push_back(f);
        }
    if (!restoreDetails_.empty())
        return refuse("its files are missing or were changed. Compile and export again");

    // 3. The compiled plugins, as this scan knows them.
    for (const auto& name : rec.compiled) {
        const auto* p = session_->findPlugin(name);
        if (!p) {
            restoreDetails_.push_back(name + ": not in the load order");
            continue;
        }
        rb.compiled.compiledPlugins.push_back(p->index);
    }
    for (const auto& name : rec.archiveLoaders)
        if (const auto* p = session_->findPlugin(name))
            rb.compiled.archiveLoaders.push_back(p->index);
    for (const auto& entry : rec.redirected) {
        const auto bar = entry.find('|');
        const std::string name = entry.substr(0, bar);
        const auto* p = session_->findPlugin(name);
        if (!p) {
            restoreDetails_.push_back(name + " (redirected): not in the load order");
            continue;
        }
        RedirectedPlugin rp;
        rp.plugin = p->index;
        rp.name = p->name;
        rp.after = bar == std::string::npos ? std::string() : entry.substr(bar + 1);
        rb.compiled.redirected.push_back(std::move(rp));
    }
    if (!restoreDetails_.empty())
        return refuse("plugins it compiled are no longer found. Compile again");

    rb.compiled.options = config_.compile;
    rb.compiled.restored = true;
    rb.compiled.f4seArchiveLoader = rec.f4seArchiveLoader;
    rb.compiled.archivesToRegister = rec.archivesToRegister;
    rb.compiled.markCompiled(session_->plugins.size());
    int index = 0;
    for (const auto& o : rec.outputs) {
        OutputPlugin op;
        op.index = index++;
        op.name = o.name;
        op.light = o.light;
        op.patch = o.patch;
        op.newRecords = o.newRecords;
        op.overrides = o.overrides;
        op.restoredWritten = true;
        rb.compiled.outputs.push_back(std::move(op));
    }
    auto& e = rb.exported;
    e.packageRoot = *latest;
    e.directory = rec.directory.empty() ? *latest : at(rec.directory);
    e.zipPath = at(rec.zip);
    if (!e.zipPath.empty() && !std::filesystem::is_regular_file(e.zipPath, ec))
        e.zipPath.clear();
    e.disableList = at(rec.disableList);
    e.recorderFolder = at(rec.recorderFolder);
    e.archiveLoaderFiles = loaderFiles;
    for (const auto& f : rec.loaderPlugins)
        e.archiveLoaders.push_back(at(f));
    e.runName = run;
    e.outputRoot = location.root;
    e.latestRecorded = true;
    if (report.status == BuildStatus::ChangesDetected)
        for (const auto& i : report.items)
            if (rb.notes.size() < 50)
                rb.notes.push_back(i.category + " " + i.kind + ": " + i.name);
    restored_ = std::move(rb);
    spdlog::info("Reopened the last build ({}): {} compiled plugins, {} outputs, verified; Apply to Game is available",
                 run, restored_->compiled.compiledPlugins.size(), restored_->compiled.outputs.size());
    return true;
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
    planDecisions_ = PlanDecisions::load(planDecisionsPath());
    plan_ = buildPatchPlan(*session_, *conflicts_, planDecisions_);
    session_->autoMergeEnabled = config_.compile.autoMerge;
    if (config_.compile.autoMerge) {
        progress.stage("Auto-Merger: checking scripts and configs");
        autoMerge_ = AutoMerger::analyse(*session_);
    } else {
        autoMerge_.reset();
    }
    dependencies_ = DependencyEngine::analyse(*session_, analysisOptions(), progress);
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

std::filesystem::path Pipeline::planDecisionsPath() const
{
    if (!config_.patchPlanDecisions.empty())
        return config_.patchPlanDecisions;
    if (!config_.databasePath.empty())
        return config_.databasePath.parent_path() / "patch_plan_decisions.json";
    return {};
}

void Pipeline::setPlanDecision(FormKey record, const std::string& changeId, std::optional<PlanAction> action)
{
    planDecisions_.set(PlanDecisions::recordId(*session_, record), changeId, action);
    if (const auto path = planDecisionsPath(); !path.empty())
        planDecisions_.save(path);
    if (conflicts_)
        plan_ = buildPatchPlan(*session_, *conflicts_, planDecisions_);
}

std::string Pipeline::loaderMod(std::string* off) const
{
    std::string why;
    std::string mod;
    if (!config_.archiveLoaders) {
        why = "archive loaders are turned off";
    } else if (session_->install.config.kind != InstallKind::ModOrganizer2) {
        why = std::string("the output is the installable package, the same for every mod manager (") +
              installKindName(session_->install.config.kind) +
              " here); archive loaders are only written when the output folder is a mod inside Mod Organizer "
              "2's mods folder";
    } else if (session_->install.modsPath.empty()) {
        why = "Mod Organizer's mods folder is unknown";
    } else {
        const auto outputDir = pendingOutputDirectory();
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
            mod = rest.substr(0, rest.find('/'));
        } else {
            why = "the output is the installable package (" + pathToUtf8(outputDir) +
                  "), the same for every mod manager; archive loaders are only written when the output folder "
                  "is a mod inside Mod Organizer's mods folder (" + pathToUtf8(session_->install.modsPath) + ")";
        }
    }
    if (off)
        *off = why;
    return mod;
}

std::filesystem::path Pipeline::outputRoot() const
{
    if (!session_)
        return {};
    return outputLocation().root;
}

AnalysisOptions Pipeline::analysisOptions() const
{
    AnalysisOptions a = config_.analysis;
    a.masterPlaceholders = config_.compile.unusedMasterPlaceholders;
    a.newQuestsAndNpcs = config_.compile.compileNewQuestsAndNpcs;
    a.placeholdersOff.clear();
    a.redirectMasters = config_.compile.redirectMasters;
    a.noRedirect = config_.compile.noRedirect;
    a.redirectOff.clear();
    if (session_) { // also with both off: the Holding Back view says whether redirection would be possible
        std::string off;
        if (loaderMod(&off).empty()) {
            a.redirectOff = off;
            if (a.masterPlaceholders)
                a.placeholdersOff = off;
        }
    }
    return a;
}

void Pipeline::refreshEligibility()
{
    session_->userRetained.clear();
    for (const auto& name : config_.analysis.excludedPlugins)
        session_->userRetained.insert(toLower(name));
    DependencyEngine::evaluateEligibility(*session_, analysisOptions(),
                                          dependencies_ ? &*dependencies_ : nullptr);
    if (db_)
        db::DatabaseWriter(*db_).updatePlugins(*session_);
}

namespace {

// Outputs of earlier builds in the load order, and the plugins they compiled that are switched off.
void reportEarlierBuilds(const Session& session, ValidationReport& v)
{
    auto sample = [](const std::vector<std::string>& names) {
        std::string t;
        for (size_t i = 0; i < names.size() && i < 30; ++i)
            t += (i ? ", " : "") + names[i];
        if (names.size() > 30)
            t += ", ... and " + std::to_string(names.size() - 30) + " more";
        return t;
    };
    // Installed plugins switched off in plugins.txt that no earlier build's record accounts for.
    std::vector<std::string> unaccounted;
    for (const auto& p : session.plugins)
        if (!p.active && !p.generated && !p.implicit && !p.ghosted && p.error.empty())
            unaccounted.push_back(p.name);
    for (const auto& b : session.earlierBuilds) {
        const std::string what = b.output + (b.active ? " (enabled)" : " (switched off)") +
                                 " is the output of an earlier build";
        if (b.known() && !b.switchedOff.empty()) {
            v.fail(Severity::Info, "Earlier Build",
                   what + ". " + std::to_string(b.switchedOff.size()) +
                       " plugin(s) it compiled are switched off in plugins.txt; they are read as sources in their "
                       "place in the load order, so this build does not leave them out (from " + b.source + "): " +
                       sample(b.switchedOff),
                   b.output);
        } else if (b.known()) {
            v.fail(Severity::Info, "Earlier Build",
                   what + "; every plugin it compiled is enabled again (from " + b.source + ")", b.output);
        } else if (!unaccounted.empty()) {
            v.fail(Severity::Error, "Earlier Build",
                   what + ", but the list of plugins it compiled was not found (no " + earlier::kCompiledListFile +
                       " installed with it, no export folder holding it, nothing in the results database). Plugins it "
                       "compiled that are still switched off would be read as uninstalled and left out of this build. "
                       "Switch them back on (they are in that build's Multiplexer_DisablePlugins.txt), or remove " +
                       b.output + " from the game if it is no longer used, then scan again. Switched off now: " +
                       sample(unaccounted),
                   b.output);
        } else {
            v.fail(Severity::Info, "Earlier Build",
                   what + "; no installed plugin is switched off, so nothing it compiled can be missing", b.output);
        }
    }
}

} // namespace

bool Pipeline::compile(ProgressSink& progress)
{
    if (!conflicts_)
        analyse(progress);
    if (db_) {
        auto resolutions = db::DatabaseWriter(*db_).loadResolutions(*session_);
        for (const auto& [k, v] : resolutions)
            config_.compile.forcedWinners.emplace(k, v);
    }
    if (config_.compile.previousFormMap.empty() && !session_->buildRecordsDirectory.empty()) {
        // The latest finished export's FormID map: FormIDs stay the same from build to build.
        auto previous = session_->buildRecordsDirectory / "Multiplexer_FormMap.tsv";
        if (std::filesystem::exists(previous))
            config_.compile.previousFormMap = previous;
    }
    // Compile, and while validation finds records that cannot be written safely, set their source
    // plugins (or patch records) aside and compile again. Each round only ever removes work.
    session_->autoRetained.clear();
    session_->previsChecks.clear();
    session_->newCellChecks.clear();
    session_->redirectFailed.clear();
    CompileOptions options = config_.compile;
    // The reviewed compatibility patch plan: the changes to carry, for the records that are Ready.
    options.plannedChanges.clear();
    if (options.applyPatchPlan && plan_)
        for (const auto& pr : plan_->records) {
            if (pr.status != PlanStatus::Ready)
                continue;
            auto& ids = options.plannedChanges[pr.conflict->key.packed()];
            for (const auto& pe : pr.entries)
                if (pe.action == PlanAction::CarryForward)
                    ids.push_back(pr.conflict->changes[pe.change].changeId);
        }

    // Archive loaders need the output mod to override the original plugin files, which only a
    // Mod Organizer 2 mod folder does. Anywhere else they would overwrite the originals.
    // Where the next export will write (it does not exist yet: every export gets a new folder).
    const auto outputDir = pendingOutputDirectory();
    std::string loadersOff; // why archive loaders are not used, for the validation report
    const std::string outputMod = loaderMod(&loadersOff);
    options.archiveLoaders = !outputMod.empty();
    // The F4SE archive loader: the archive list is not limited to what the game reads of the INI.
    std::string archiveLoaderOff;
    if (config_.f4seArchiveLoader) {
        std::error_code ec;
        if (session_->install.config.runtime != RuntimeProfile::OldGen_1_10_163)
            archiveLoaderOff = "it has been tested on Fallout 4 1.10.163 only";
        else if (config_.archiveLoaderDll.empty() || !std::filesystem::is_regular_file(config_.archiveLoaderDll, ec))
            archiveLoaderOff = std::string(Exporter::kArchiveLoaderDll) + " was not found next to the application (" +
                               pathToUtf8(config_.archiveLoaderDll) + ")";
    }
    const bool archiveLoaderOn = config_.f4seArchiveLoader && archiveLoaderOff.empty();
    spdlog::info("F4SE archive loader: {}", archiveLoaderOn                 ? std::string("on")
                                            : config_.f4seArchiveLoader     ? "turned on but not used: " + archiveLoaderOff
                                                                            : std::string("off (Settings > Archives)"));
    std::map<std::string, std::string> isolatedPatchRecords; // record label -> finding
    if (autoMerge_)
        autoMerge_->keptOut.clear();
    for (int round = 0;; ++round) {
        DependencyEngine::evaluateEligibility(*session_, analysisOptions(), dependencies_ ? &*dependencies_ : nullptr);
        compiled_ = CompilationEngine::compile(*session_, options, progress);
        validation_ = ValidationEngine::validate(*session_, *compiled_, progress);
        size_t isolated = 0;
        // Master Redirection: a plugin whose copy cannot be written faithfully is not redirected, so its
        // masters stay in the load order (fail closed) and the compilation runs again.
        for (const auto& rp : compiled_->redirected)
            if (!rp.failure.empty() &&
                session_->redirectFailed.emplace(session_->plugins[rp.plugin].key, rp.failure).second)
                ++isolated;
        // References that cannot be written faithfully (fail closed, unless written as null on request):
        // the plugin that stores each stays in the load order, so the game reads its own data unchanged.
        for (const auto& br : compiled_->brokenReferences)
            if (br.plugin < session_->plugins.size() && compiled_->isCompiled(br.plugin) &&
                session_->autoRetained
                    .emplace(session_->plugins[br.plugin].key,
                             "Broken reference: " + br.message +
                                 ". It stays in the load order, so the game reads its own data unchanged; to compile it "
                                 "anyway with such references written as null (lossy), turn on \"Write broken "
                                 "references as null\" (Compilation page, or --broken-references-as-null)")
                    .second)
                ++isolated;
        // Data\<plugin>.ini of compiled plugins: carried into an output's INI, read after every plugin that
        // stays; a plugin whose setting would end with another value stays in the load order.
        {
            std::vector<uint8_t> flag(session_->plugins.size(), 0);
            for (auto pi : compiled_->compiledPlugins)
                flag[pi] = 1;
            for (const auto& [pi, why] : plugin_ini::conflicts(*session_, flag))
                if (session_->autoRetained.emplace(session_->plugins[pi].key, "Plugin INI: " + why).second)
                    ++isolated;
        }
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
        // The INI archive list must fit the 1,024-byte buffer the game reads it into: past it, archives
        // never load. Full plugins first (compiling them frees the slots that matter), then light ones,
        // each in load order; a plugin whose archives no longer fit stays in the load order.
        if (!compiled_->archivesToRegister.empty() && !archiveLoaderOn) {
            const auto list = Exporter::archiveList(*session_, *compiled_, outputDir);
            if (join(list, ", ").size() > Exporter::kArchiveListLimit) {
                std::unordered_set<std::string> ours, present;
                for (const auto& a : compiled_->archivesToRegister)
                    ours.insert(toLower(a));
                size_t used = 0;
                for (const auto& a : list)
                    if (!ours.count(toLower(a))) {
                        used += (used ? 2 : 0) + a.size();
                        present.insert(toLower(a));
                    }
                const std::string lang =
                    session_->install.config.language.empty() ? "en" : session_->install.config.language;
                std::vector<PluginIndex> owners;
                for (auto pi : compiled_->compiledPlugins)
                    if (!compiled_->hasArchiveLoader(pi))
                        owners.push_back(pi);
                std::stable_sort(owners.begin(), owners.end(), [&](PluginIndex a, PluginIndex b) {
                    const auto& pa = session_->plugins[a];
                    const auto& pb = session_->plugins[b];
                    if (pa.isLight() != pb.isLight())
                        return !pa.isLight();
                    return pa.loadOrder < pb.loadOrder;
                });
                for (auto pi : owners) {
                    const auto& p = session_->plugins[pi];
                    std::vector<std::string> archives;
                    for (const auto& a : AssetScanner::pluginArchiveNames(p, lang))
                        if (session_->vfs.find(toLower(a)) && !present.count(toLower(a)))
                            archives.push_back(a);
                    if (archives.empty())
                        continue;
                    size_t cost = 0;
                    for (const auto& a : archives)
                        cost += 2 + a.size();
                    if (used == 0)
                        cost -= 2;
                    if (used + cost <= Exporter::kArchiveListLimit) {
                        used += cost;
                        for (const auto& a : archives)
                            present.insert(toLower(a));
                    } else if (session_->autoRetained
                                   .emplace(p.key, "Archive list full: Fallout 4 reads at most " +
                                                       std::to_string(Exporter::kArchiveListLimit) +
                                                       " characters of SResourceArchiveList2, and its archives (" +
                                                       join(archives, ", ") +
                                                       ") no longer fit, so they would never load. Turn on the F4SE "
                                                       "archive loader (Settings > Archives, or --archive-loader) "
                                                       "to compile it; on Mod Organizer 2, with the output inside "
                                                       "the mods folder, archive loaders need no INI list either")
                                   .second) {
                        ++isolated;
                    }
                }
            }
        }
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
            if (e.patch || e.configOnly) { // a record written only to merge: left out next round
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
                                       reason.rfind("Archive priority", 0) == 0 ||
                                       reason.rfind("Archive list full", 0) == 0
                                   ? " stays in the load order: " + reason
                                   : " stays in the load order because one of its records could not be written "
                                     "safely: " + reason),
                          p ? p->name : key);
    }
    reportEarlierBuilds(*session_, *validation_);
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
    for (const auto& [cell, text] : session_->previsChecks)
        validation_->fail(Severity::Info, "Previs Check", text, session_->pluginName(cell.origin),
                          session_->loadOrderFormId(cell));
    // Stage PC1 (read only): which new cells' files could move, and the plugins that would no longer be kept
    // out by their cells.
    if (!session_->newCellChecks.empty()) {
        std::map<PluginIndex, std::pair<size_t, size_t>> perPlugin; // movable, cells
        for (const auto& [cell, a] : session_->newCellChecks) {
            validation_->fail(Severity::Info, "Precombined Cell Check", a.text, session_->pluginName(cell.origin),
                              session_->loadOrderFormId(cell));
            auto& [movable, cells] = perPlugin[cell.origin];
            movable += a.movable;
            ++cells;
        }
        size_t cells = 0, movableCells = 0, plugins = 0;
        for (const auto& [p, counts] : perPlugin) {
            movableCells += counts.first;
            cells += counts.second;
            plugins += counts.first == counts.second;
        }
        validation_->fail(Severity::Info, "Precombined Cell Check",
                          std::to_string(movableCells) + " of " + std::to_string(cells) +
                              " new cells kept out for their precombine or previs data could have their files moved "
                              "(stage PC2); " + std::to_string(plugins) + " of " + std::to_string(perPlugin.size()) +
                              " plugins would then no longer be kept out by their cells. Nothing is moved yet.");
    }
    for (const auto& [label, reason] : isolatedPatchRecords)
        validation_->fail(Severity::Warning, "Kept Out Automatically",
                          label + " was left out of the patch (last plugin wins for it): " + reason);
    if (db_ && !session_->autoRetained.empty())
        db::DatabaseWriter(*db_).updatePlugins(*session_);

    // The rounds stopped while a plugin's copy still could not be written: its compiled masters would leave
    // it without them. Never export that.
    for (const auto& rp : compiled_->redirected)
        if (!rp.failure.empty())
            for (PluginIndex m : session_->plugins[rp.plugin].masters)
                if (m != kInvalidPlugin && compiled_->isCompiled(m))
                    validation_->fail(Severity::Error, "Master Redirection",
                                      rp.name + " cannot be redirected (" + rp.failure + "), but its master " +
                                          session_->plugins[m].name + " was compiled. Compile again",
                                      rp.name);
    for (const auto& [key, why] : session_->redirectFailed) {
        const auto* p = session_->findPlugin(key);
        validation_->fail(Severity::Warning, "Master Redirection",
                          (p ? p->name : key) + " is not redirected: " + why +
                              ". Its masters stay in the load order with it.",
                          p ? p->name : key);
    }
    for (const auto& rp : compiled_->redirected)
        if (rp.failure.empty())
            validation_->fail(Severity::Info, "Master Redirection",
                              rp.name + " stays in the load order as a copy in the output that lists the outputs "
                                        "instead of " + join(rp.replacedMasters, ", ") + " (" +
                                  std::to_string(rp.translated) + " FormIDs translated, " +
                                  std::to_string(rp.dropped) + " overrides left to the outputs or later plugins as "
                                  "today, " + std::to_string(rp.winnerCopies) +
                                  " cells or other containers written as the game shows them today). It loads after "
                                  "the outputs; switching the output mod off restores the original.",
                              rp.name);
    for (auto pi : compiled_->masterPlaceholders) {
        std::vector<std::string> by;
        for (const auto& [q, m] : session_->unusedMasters)
            if (m == pi)
                by.push_back(session_->plugins[q].name);
        validation_->fail(Severity::Info, "Master Placeholders",
                          session_->plugins[pi].name + " is compiled; an empty placeholder plugin with its name keeps " +
                              join(by, ", ") + " loading (they list it as a master but use none of its records). "
                              "Keep " + session_->plugins[pi].name + " enabled.");
    }
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
    compiled_->f4seArchiveLoader = archiveLoaderOn && !compiled_->archivesToRegister.empty();
    if (compiled_->f4seArchiveLoader)
        validation_->fail(Severity::Info, "Archive Loader",
                          std::to_string(compiled_->archivesToRegister.size()) +
                              " archives of compiled plugins load through the F4SE archive loader (" +
                              Exporter::kArchiveLoaderDll + ", shipped in the package): the archive list is not limited "
                              "to the 1,023 characters the game reads. Start the game through F4SE");
    else if (!archiveLoaderOff.empty())
        validation_->fail(Severity::Warning, "Archive Loader",
                          std::string("The F4SE archive loader is turned on but not used: ") + archiveLoaderOff +
                              ". The archive list is kept within the 1,023 characters the game reads");
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
    // This build compared with the previous one: what moves, what is new, what goes.
    comparison_.reset();
    if (!config_.compile.previousFormMap.empty() && !config_.compile.patchOnly) { // patch only: nothing compiles
        BuildSide before;
        if (readBuildSide(config_.compile.previousFormMap.parent_path(), before)) {
            BuildSide after;
            after.mappings = compiled_->mappings;
            for (auto pi : compiled_->compiledPlugins)
                after.compiled.push_back(session_->plugins[pi].name);
            for (const auto& op : compiled_->outputs)
                if (!op.records.empty())
                    after.outputs.push_back(op.name);
            comparison_ = compareBuilds(before, after);
            comparison_->previousSource = pathToUtf8(config_.compile.previousFormMap.parent_path());
            const auto& cmp = *comparison_;
            validation_->fail(Severity::Info, "Build Comparison",
                              cmp.same() ? std::string("Same as the previous build: every record keeps its FormID")
                                         : std::to_string(cmp.kept) + " record(s) keep their FormID, " +
                                               std::to_string(cmp.added.size()) + " new, " +
                                               std::to_string(cmp.moved.size() + cmp.renumbered.size()) +
                                               " get another FormID, " + std::to_string(cmp.removed.size()) +
                                               " no longer in the build; " + std::to_string(cmp.newlyCompiled.size()) +
                                               " plugin(s) compiled for the first time, " +
                                               std::to_string(cmp.returned.size()) +
                                               " back in the load order (Compilation Report → Changes since the "
                                               "previous build)");
        }
    }
    // Plugins the last build compiled (switched off since, so saves may hold their records under the
    // outputs' FormIDs) that this build would return to the load order, without the user asking for it:
    // stop and ask instead of doing it silently.
    {
        std::unordered_set<std::string> allowed;
        for (const auto& n : config_.compile.allowReturn)
            allowed.insert(toLower(n));
        for (const auto& p : session_->plugins) {
            if (config_.compile.patchOnly || !p.reactivated || compiled_->isCompiled(p.index) ||
                session_->userRetained.count(p.key))
                continue;
            if (config_.analysis.onlySelected && // not chosen: the user's choice too
                std::none_of(config_.analysis.selectedPlugins.begin(), config_.analysis.selectedPlugins.end(),
                             [&](const std::string& n) { return toLower(n) == p.key; }))
                continue;
            if (allowed.count(p.key)) {
                validation_->fail(Severity::Warning, "Returning Plugins",
                                  p.name + " returns to the load order (you allowed it): " + join(p.reasons, "; "),
                                  p.name);
                continue;
            }
            validation_->fail(
                Severity::Error, "Returning Plugins",
                p.name + " was compiled by the last build and is switched off, but this build would return it to the "
                         "load order: " + (p.reasons.empty() ? std::string("not compiled") : join(p.reasons, "; ")) +
                    ". Saves made with the last build hold its records under the outputs' FormIDs; with it back, the "
                    "outputs no longer have them, so those saves lose what they used of it. Allow it back (Compilation "
                    "page, or --allow-return), exclude it yourself (Plugins page), or remove or switch off what keeps "
                    "it out (when a new or updated plugin needs it as a master), then compile again.",
                p.name);
        }
    }
    // Records the previous build placed that would get other FormIDs while that build is in use (its
    // outputs enabled, or plugins it compiled switched off): saves made with it would lose them.
    {
        const bool inUse = std::any_of(session_->earlierBuilds.begin(), session_->earlierBuilds.end(),
                                       [](const EarlierBuild& b) { return b.active; }) ||
                           std::any_of(session_->plugins.begin(), session_->plugins.end(),
                                       [](const PluginInfo& p) { return p.reactivated; });
        size_t changed = 0;
        std::string example;
        for (const auto& e : compiled_->records)
            if (e.formIdChanged && ++changed == 1)
                example = session_->formLabel(e.key);
        if (inUse && changed) {
            const std::string what = std::to_string(changed) + " record(s) the previous build placed would get " +
                                     "other FormIDs (e.g. " + example +
                                     "; the FormID Stability warnings say why). Saves made with the previous build "
                                     "would lose them.";
            if (config_.compile.acceptFormIdChanges)
                validation_->fail(Severity::Warning, "FormID Stability", what + " Accepted for this build.");
            else
                validation_->fail(Severity::Error, "FormID Stability",
                                  what + " Compile anyway only if you start a new game or accept that loss (the "
                                         "Compilation page asks; --accept-formid-changes on the command line).");
        }
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

output::Location Pipeline::outputLocation() const
{
    return output::locate(*session_, config_.exportOptions.outputDirectory);
}

std::filesystem::path Pipeline::previousBuildDirectory() const
{
    const auto loc = outputLocation();
    if (loc.layout == output::Layout::GameData)
        return loc.root;
    if (auto latest = output::latestRun(loc))
        return output::dataDir(*latest, loc.layout);
    // Before export folders: the single output folder earlier versions wrote (only ever read).
    std::vector<std::filesystem::path> legacy;
    if (!config_.exportOptions.outputDirectory.empty())
        legacy.push_back(config_.exportOptions.outputDirectory);
    legacy.push_back(Exporter::defaultOutputDirectory(*session_));
    for (const auto& d : legacy)
        if (std::filesystem::exists(d / "Multiplexer_FormMap.tsv"))
            return d;
    return {};
}

std::filesystem::path Pipeline::pendingOutputDirectory() const
{
    const auto loc = outputLocation();
    switch (loc.layout) {
    case output::Layout::GameData: return loc.root;
    case output::Layout::ModFolder: return loc.root / "Multiplexer Output (newest export)";
    case output::Layout::Package: break;
    }
    return loc.root / "Multiplexer Output (newest export)" / "Data";
}

namespace {

// Pairs of plugins that edit the same record and whose order differs from `oldOrder` (active plugins at
// the build): only such a move can change which version of a record wins. A move between plugins that
// share no record changes nothing the build decided. Up to `limit` pairs, as "A before B (now after)".
std::vector<std::string> conflictingMoves(const Session& s, const std::vector<std::string>& oldOrder, size_t limit)
{
    std::unordered_map<std::string, int> oldPos;
    for (size_t i = 0; i < oldOrder.size(); ++i)
        oldPos.emplace(toLower(oldOrder[i]), static_cast<int>(i));
    std::vector<int> posOf(s.plugins.size(), -1);
    for (const auto& p : s.plugins)
        if (p.active)
            if (auto it = oldPos.find(toLower(p.name)); it != oldPos.end())
                posOf[p.index] = it->second;
    std::set<std::pair<PluginIndex, PluginIndex>> pairs;
    for (const auto& g : s.groups) {
        if (g.size() < 2)
            continue;
        int maxOld = -1;
        PluginIndex maxPlugin = kInvalidPlugin;
        for (const auto& inst : s.instances(g)) { // in today's load order
            const int op = inst.plugin < posOf.size() ? posOf[inst.plugin] : -1;
            if (op < 0)
                continue; // not in the build's load order (an output, or new since)
            if (op < maxOld)
                pairs.insert({maxPlugin, inst.plugin});
            else {
                maxOld = op;
                maxPlugin = inst.plugin;
            }
        }
        if (pairs.size() >= limit)
            break;
    }
    std::vector<std::string> out;
    for (const auto& [a, b] : pairs)
        // `a` comes first in today's order but loaded after `b` at the build.
        out.push_back(s.plugins[a].name + " now loads before " + s.plugins[b].name +
                      " (they edit the same records; it loaded after it at the build)");
    return out;
}

// After the export folder is renamed from "<name>.tmp" to "<name>": the same paths in the new place.
void rebase(std::filesystem::path& p, const std::string& from, const std::filesystem::path& to)
{
    if (p.empty())
        return;
    const std::string s = pathToUtf8(p.lexically_normal().generic_u8string());
    if (s.size() >= from.size() && s.compare(0, from.size(), from) == 0 && (s.size() == from.size() || s[from.size()] == '/'))
        p = s.size() == from.size() ? to : to / utf8ToPath(s.substr(from.size() + 1));
}

} // namespace

ExportResult Pipeline::exportOutputs(ProgressSink& progress)
{
    if (!compiled_)
        throw std::runtime_error("nothing compiled yet");
    if (validation_ && validation_->critical())
        throw std::runtime_error("validation found " + std::to_string(validation_->errors) +
                                 " critical errors; export is blocked");
    progress.stage("Export");
    // Every export writes into a new folder of its own, "<name>.tmp" until it is complete; an earlier
    // export is never moved, renamed, deleted or changed. (Writing straight into the game's Data folder
    // is the one exception: there the files are replaced in place, as before.)
    const auto location = outputLocation();
    std::optional<output::Run> run;
    std::filesystem::path directory;
    std::vector<std::string> previousOutputs;
    if (location.layout == output::Layout::GameData) {
        directory = location.root;
        if (db_)
            if (auto build = db::DatabaseWriter(*db_).loadSnapshot("build");
                build && build->isBuild && utf8ToPath(build->outputDirectory) == directory)
                for (const auto& o : build->outputs)
                    previousOutputs.push_back(o.name);
    } else {
        run = output::beginRun(location);
        directory = run->dataDir();
    }
    auto exportOptions = config_.exportOptions;
    exportOptions.archiveWinnerCopies = config_.archiveWinnerCopies;
    exportOptions.outputDirectory = directory;
    if (config_.beforeExportWrite)
        config_.beforeExportWrite(directory);
    ExportResult result;
    try {
        result = Exporter::write(*session_, *compiled_, exportOptions, previousOutputs);
    } catch (const std::exception& e) {
        markBuildFailed(directory, Exporter::packageRoot(directory), {}, e.what());
        throw;
    }
    result.outputRoot = location.root;
    if (run)
        result.runName = run->name;
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
    // A failed export stays "<name>.tmp" with a note saying why: never the latest export, and the
    // latest one is left as it is (also in this session: exportResult() keeps naming it).
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
        return result;
    }
    // The F4SE archive loader and the full archive list, in the package's Data\F4SE\Plugins. Where an earlier
    // build wrote into the game's Data folder itself, a list of ours no longer needed is removed.
    {
        const auto plugins = result.directory / "F4SE" / "Plugins";
        const auto manifest = plugins / Exporter::kArchiveManifest;
        const std::string header = "# Written by Fallout 4 Multiplexer Renewed";
        std::error_code ec;
        if (compiled_->f4seArchiveLoader) {
            std::string text = header + ": the archive list MultiplexerArchiveLoader.dll supplies\n"
                                        "# to the game in full (SResourceArchiveList2, in this order). It changes nothing\n"
                                        "# unless what the game read from the INI is the start of this list.\n";
            for (const auto& a : Exporter::archiveList(*session_, *compiled_, result.directory))
                text += a + "\n";
            std::filesystem::create_directories(plugins, ec);
            atomicWriteText(manifest, text);
            std::filesystem::copy_file(config_.archiveLoaderDll, plugins / Exporter::kArchiveLoaderDll,
                                       std::filesystem::copy_options::overwrite_existing);
            result.archiveLoaderFiles = {plugins / Exporter::kArchiveLoaderDll, manifest};
        } else if (std::filesystem::exists(manifest, ec)) {
            std::ifstream in(manifest, std::ios::binary);
            std::string first;
            std::getline(in, first);
            in.close();
            if (first.rfind(header, 0) == 0)
                std::filesystem::remove(manifest, ec);
        }
    }
    // Every file is in Data now: zip it into the installable package. Not when the output folder is the
    // game's own Data folder: a package of it would hold the whole game.
    if (location.layout == output::Layout::GameData)
        spdlog::warn("No Multiplexer Output.zip: the output folder is the game's Data folder");
    else if (config_.exportOptions.writeZip) {
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
    // Reports go into the export (unless a report folder is set) before it is finished.
    const auto reportDir =
        config_.reportDirectory.empty() ? result.packageRoot / "Multiplexer Reports" : config_.reportDirectory;
    writeReports(reportDir);

    // Finish: "<name>.tmp" becomes "<name>" in one rename, then it is the latest export.
    if (run) {
        const std::string from = pathToUtf8(run->folder.lexically_normal().generic_u8string());
        try {
            output::finishRun(*run);
        } catch (const std::exception& e) {
            // Complete but not finished: left as "<name>.tmp" (never the latest), with a note.
            std::ofstream out(result.packageRoot / kBuildFailedMarker, std::ios::binary | std::ios::trunc);
            out << "This export was written completely but could not be finished: " << e.what()
                << "\nIt is not the latest export. Export again, or delete this folder.\n";
            spdlog::error("{}", e.what());
            throw;
        }
        result.runName = run->name;
        for (auto* p : {&result.directory, &result.packageRoot, &result.zipPath, &result.formMap, &result.archiveIni,
                        &result.disableList, &result.pluginsTxt, &result.voiceManifest, &result.recorderFolder})
            rebase(*p, from, run->folder);
        for (auto* list : {&result.pluginFiles, &result.archiveLoaders, &result.seqFiles, &result.archiveLoaderFiles,
                           &result.redirectedCopies})
            for (auto& p : *list)
                rebase(p, from, run->folder);
        if (autoMerge_)
            for (auto& p : autoMerge_->written)
                rebase(p, from, run->folder);
        result.latestRecorded = output::setLatest(*run);
    }
    exported_ = result;
    restored_.reset();
    restoreProblem_.clear();
    restoreDetails_.clear();
    // This build becomes the reference state for change detection, in the results database and in the
    // export itself (Multiplexer_Build.json: what reopening it after a restart needs).
    auto buildSnapshot = ChangeDetector::capture(*session_);
    ChangeDetector::addBuild(buildSnapshot, *session_, *compiled_, result, currentSettingsHash());
    if (!result.runName.empty()) {
        try {
            buildrecord::write(result.packageRoot.empty() ? result.directory : result.packageRoot,
                               buildrecord::make(*session_, *compiled_, result, buildSnapshot, currentSettingsHash()));
        } catch (const std::exception& e) {
            spdlog::warn("Could not write {}: {} (the build cannot be reopened after a restart)", buildrecord::kFile,
                         e.what());
        }
    }
    if (db_) {
        db::DatabaseWriter w(*db_);
        auto snapshot = buildSnapshot;
        w.saveSnapshot("build", snapshot);
        changes_ = ChangeReport{};
        changes_.status = BuildStatus::UpToDate;
        changes_.referenceTime = changes_.checkedTime = snapshot.created;
        w.writeChanges(changes_);
        w.setMeta("export_written", std::to_string(result.writtenOutputs.size()));
        w.setMeta("export_unchanged", std::to_string(result.unchangedOutputs.size()));
        w.setMeta("export_removed", std::to_string(result.removedOutputs.size()));
    }
    return result;
}

std::vector<std::filesystem::path> Pipeline::writeReports(const std::filesystem::path& directory)
{
    ReportContext ctx;
    ctx.session = session_.get();
    ctx.conflicts = conflicts();
    ctx.plan = patchPlan();
    ctx.dependencies = dependencies();
    ctx.compiled = compiledOutput();
    ctx.validation = reportValidation();
    ctx.autoMerge = autoMerge();
    ctx.comparison = comparison();
    auto files = ReportGenerator::writeAll(ctx, directory);
    spdlog::info("Wrote {} report files to {}", files.size(), pathToUtf8(directory));
    return files;
}

} // namespace fo4mx

namespace fo4mx {

diagnostics::SaveCheckInput Pipeline::saveCheckInput() const
{
    diagnostics::SaveCheckInput in;
    const auto& s = *session_;
    for (const auto& p : s.plugins) {
        if (p.generated)
            in.outputs.insert(p.name);
        if (p.reactivated || p.behindLoader) // compiled by the last build
            in.compiledBefore.insert(p.name);
    }
    if (!compiled_) {
        for (auto pi : s.loadOrder)
            in.loadOrder.push_back(s.plugins[pi].name);
        return in;
    }
    in.nextBuild = true;
    in.comparison = comparison();
    for (auto pi : s.loadOrder)
        if (!s.plugins[pi].generated && !compiled_->isCompiled(pi))
            in.loadOrder.push_back(s.plugins[pi].name);
    for (const auto& op : compiled_->outputs)
        if (!op.records.empty()) {
            in.loadOrder.push_back(op.name);
            in.outputs.insert(op.name);
        }
    for (auto pi : compiled_->compiledPlugins)
        in.compiledAway.push_back(s.plugins[pi].name);
    return in;
}

} // namespace fo4mx

namespace fo4mx {

Pipeline::Readiness Pipeline::readiness(const std::filesystem::path& save) const
{
    using S = ReadinessItem::State;
    Readiness r;
    auto add = [&](S state, std::string title, std::string detail) {
        r.items.push_back({state, std::move(title), std::move(detail)});
    };
    if (!session_->scanned) {
        add(S::Fail, "Scan", "Nothing is scanned yet: Scan, Analyse and Compile first.");
        return r;
    }
    // 1. Compiled, and validation found no errors.
    if ((!compiled_ || !validation_) && restored_) {
        add(S::Pass, "Validation", "The latest export (" + restored_->run +
                                       ") was validated when it was written, and still fits this load order.");
    } else if (!compiled_ || !validation_) {
        add(S::Fail, "Compile", "Not compiled in this session: Compile (and Export) first.");
    } else if (validation_->errors) {
        add(S::Fail, "Validation", std::to_string(validation_->errors) +
                                      " error(s): the Validation page says which and why. They block the export.");
    } else {
        add(S::Pass, "Validation",
            "0 errors, " + std::to_string(validation_->warnings) + " warning(s)" +
                (validation_->warnings ? ": worth reading before you play (Validation page)" : ""));
    }
    // 2. Exported: Apply switches to an export.
    if (exported_ && !exported_->directory.empty())
        add(S::Pass, "Export", "Exported to " + pathToUtf8(exported_->directory) + ".");
    else if (restored_)
        add(S::Pass, "Export", "The latest export (" + restored_->run + ") fits this load order.");
    else
        add(S::Fail, "Export", "Export this build before Apply to Game.");
    // 3. Plugin slots.
    if (const CompiledOutput* c = compiled_ ? &*compiled_ : restored_ ? &restored_->compiled : nullptr) {
        const auto after = slotsAfter(*session_, *c);
        const std::string counts = std::to_string(after.full) + " of 254 full plugins and " +
                                   std::to_string(after.light) + " of 4,096 light plugins after switching over";
        if (after.full > SlotUsage::kFullLimit || after.light > SlotUsage::kLightLimit)
            add(S::Fail, "Plugin slots", counts + ": over the game's limit.");
        else if (after.full > 240)
            add(S::Warn, "Plugin slots", counts + ": close to the limit.");
        else
            add(S::Pass, "Plugin slots", counts + ".");
    }
    // 4. The save to test with.
    if (save.empty())
        add(S::Pass, "Your newest save", "No save found: testing with a new game is what the testing guide asks for.");
    else {
        try {
            const auto info = diagnostics::readSaveGame(save);
            const auto in = saveCheckInput();
            const auto check = diagnostics::checkSave(info, in);
            if (check.safe)
                add(S::Pass, "Your newest save", pathToUtf8(save.filename()) + " loses nothing with this build.");
            else
                add(S::Warn, "Your newest save",
                    pathToUtf8(save.filename()) + " would lose content (" +
                        std::to_string(check.missing.size() + check.compiledAway.size()) + " plugin(s)" +
                        (check.recordsChanged ? ", " + std::to_string(check.recordsChanged) + " records" : "") +
                        "): test with a new game, or read Diagnostics > Check a Save.");
        } catch (const std::exception& e) {
            add(S::Warn, "Your newest save", "Could not read " + pathToUtf8(save.filename()) + ": " + e.what());
        }
    }
    // 5. What the game needs for the switches that are on, and a crash logger for the test.
    const auto& install = session_->install;
    std::error_code ec;
    const bool f4se = std::filesystem::exists(install.gamePath / "f4se_loader.exe", ec);
    if (config_.f4seArchiveLoader || config_.diagnosticRecorder)
        add(f4se ? S::Pass : S::Fail, "F4SE",
            f4se ? std::string("Installed: start the game through f4se_loader.exe or your mod manager's F4SE entry.")
                 : std::string("Not found in the game folder, but the F4SE archive loader or the Diagnostic Recorder "
                               "is on: install F4SE, or turn them off."));
    if (config_.diagnosticRecorder && exported_)
        add(exported_->recorderFolder.empty() ? S::Warn : S::Pass, "Diagnostic Recorder",
            exported_->recorderFolder.empty()
                ? "Not prepared: " + exported_->recorderProblem
                : "Ready to install: " + pathToUtf8(exported_->recorderFolder) + ".");
    bool buffout = false;
    for (const char* dll : {"f4se\\plugins\\buffout4.dll", "f4se\\plugins\\crashlogger.dll"})
        buffout |= session_->vfs.find(dll) != nullptr || session_->assets.find(dll) != nullptr;
    add(buffout ? S::Pass : S::Warn, "Crash logger",
        buffout ? std::string("Installed: a crash writes a log Diagnostics can read.")
                : std::string("No Buffout 4 found: install one so a crash leaves a log to read."));
    return r;
}

std::string Pipeline::Readiness::format() const
{
    std::string out = ready() ? "Ready to test.\n\n" : "Not ready yet: fix what is marked FAIL first.\n\n";
    for (const auto& i : items) {
        const char* mark = i.state == ReadinessItem::State::Pass ? "OK  " : i.state == ReadinessItem::State::Warn ? "NOTE" : "FAIL";
        out += std::string(mark) + "  " + i.title + ": " + i.detail + "\n";
    }
    out += "\nThen: Apply to Game, start a new game, and follow the testing guide. Switch back undoes Apply.\n";
    return out;
}

} // namespace fo4mx
