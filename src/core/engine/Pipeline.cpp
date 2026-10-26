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

void Pipeline::scan(ProgressSink& progress)
{
    session_ = std::make_unique<Session>(RecordDefinitions::get(config_.install.runtime));
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
    if (db_)
        db::DatabaseWriter(*db_).writeScan(*session_, progress);
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
    exported_.reset();
    if (db_) {
        db::DatabaseWriter w(*db_);
        w.writeCompilation(*session_, *compiled_);
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
    auto result = Exporter::write(*session_, *compiled_, config_.exportOptions);
    auto post = ValidationEngine::validateWrittenFiles(*compiled_, result.pluginFiles);
    validation_->merge(post);
    if (db_)
        db::DatabaseWriter(*db_).writeValidation("post-export", post, false);
    exported_ = result;
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
