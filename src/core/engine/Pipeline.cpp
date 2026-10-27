#include "engine/Pipeline.hpp"

#include "db/DatabaseWriter.hpp"
#include "report/ReportGenerator.hpp"
#include "util/Text.hpp"

#include <spdlog/spdlog.h>

namespace fo4mx {

Pipeline::Pipeline(PipelineConfig config)
    : config_(std::move(config)), session_(std::make_unique<Session>(RecordDefinitions::get(config_.install.runtime)))
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
                                                     definitionsId(RecordDefinitions::get(config_.install.runtime)),
                                                     std::move(resolutions));
    changes_ = finishChangeReport(build, std::move(current), settings);
    if (db_)
        db::DatabaseWriter(*db_).writeChanges(changes_);
    spdlog::info("Build status: {} ({} changes)", buildStatusName(changes_.status), changes_.items.size());
    return changes_;
}

void Pipeline::scan(ProgressSink& progress)
{
    session_ = std::make_unique<Session>(RecordDefinitions::get(config_.install.runtime));
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
    dependencies_ = DependencyEngine::analyse(*session_, config_.analysis, progress);
    compiled_.reset();
    validation_.reset();
    if (db_)
        db::DatabaseWriter(*db_).writeAnalysis(*session_, *conflicts_, *dependencies_, progress);
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
    compiled_ = CompilationEngine::compile(*session_, config_.compile, progress);
    validation_ = ValidationEngine::validate(*session_, *compiled_, progress);
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
    auto post = ValidationEngine::validateWrittenFiles(*compiled_, result.pluginFiles);
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
    auto files = ReportGenerator::writeAll(ctx, directory);
    spdlog::info("Wrote {} report files to {}", files.size(), pathToUtf8(directory));
    return files;
}

} // namespace fo4mx
