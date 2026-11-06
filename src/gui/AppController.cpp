#include "AppController.hpp"

#include "diagnostics/RecorderPackage.hpp"

#include "Format.hpp"

#include "db/Database.hpp"
#include "db/DatabaseWriter.hpp"
#include "engine/BuildCache.hpp"
#include "engine/Exporter.hpp"
#include "engine/CompilationResult.hpp"
#include "engine/PostCompilation.hpp"
#include "engine/VortexSupport.hpp"
#include "engine/Mo2Support.hpp"
#include "util/FileTransaction.hpp"
#include "util/Log.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <QMetaObject>
#include <QCoreApplication>
#include <QSettings>
#include <QSqlError>
#include <QThread>

#include <mutex>
#include <algorithm>
#include <fstream>

#include <spdlog/sinks/base_sink.h>

namespace gui {

using namespace fo4mx;

// Forwards engine progress to the UI thread through queued signals.
class QtProgress : public QObject, public ProgressSink {
public:
    explicit QtProgress(AppController* owner) : owner_(owner) {}
    void stage(std::string_view name) override
    {
        QString n = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
        QMetaObject::invokeMethod(owner_, [o = owner_, n] { emit o->stageStarted(n); }, Qt::QueuedConnection);
    }
    void progress(uint64_t done, uint64_t total, std::string_view detail) override
    {
        // Throttle: at most ~ every 0.5% of progress.
        uint64_t bucket = total ? done * 200 / total : 0;
        if (bucket == last_ && done != total)
            return;
        last_ = bucket;
        QString d = QString::fromUtf8(detail.data(), static_cast<qsizetype>(detail.size()));
        QMetaObject::invokeMethod(
            owner_, [o = owner_, done, total, d] { emit o->progressChanged(qint64(done), qint64(total), d); },
            Qt::QueuedConnection);
    }

private:
    AppController* owner_;
    std::atomic<uint64_t> last_{~0ull};
};

namespace {

class UiLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit UiLogSink(AppController* owner) : owner_(owner) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        if (msg.level < spdlog::level::info)
            return;
        QString text = QString::fromUtf8(msg.payload.data(), static_cast<qsizetype>(msg.payload.size()));
        int level = static_cast<int>(msg.level);
        QMetaObject::invokeMethod(owner_, [o = owner_, level, text] { emit o->logMessage(level, text); },
                                  Qt::QueuedConnection);
    }
    void flush_() override {}

private:
    AppController* owner_;
};

} // namespace

AppController::AppController(QObject* parent) : QObject(parent), progress_(std::make_unique<QtProgress>(this))
{
    auto sink = std::make_shared<UiLogSink>(this);
    log::addSink(sink);
    logSink_ = sink;
    loadSettings();
}

AppController::~AppController()
{
    if (worker_) {
        progress_->requestCancel();
        worker_->wait();
    }
    log::removeSink(std::static_pointer_cast<spdlog::sinks::sink>(logSink_));
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

QString AppController::databasePath() const
{
    return toQString(config_.databasePath);
}

QSqlDatabase AppController::readDb()
{
    if (QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase db = QSqlDatabase::database(connectionName_);
        if (db.databaseName() == databasePath())
            return db;
        db.close();
    }
    QSqlDatabase db = QSqlDatabase::contains(connectionName_) ? QSqlDatabase::database(connectionName_, false)
                                                              : QSqlDatabase::addDatabase("QSQLITE", connectionName_);
    db.setDatabaseName(databasePath());
    db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
    if (!std::filesystem::exists(config_.databasePath))
        return db; // not open: pages show empty state
    if (!db.open())
        spdlog::warn("Cannot open results database: {}", db.lastError().text().toStdString());
    return db;
}

bool AppController::clearResultsAfterUpgrade(const QString& buildId)
{
    QSettings s;
    QString previous = s.value("app/build").toString();
    s.setValue("app/build", buildId);
    if (previous.isEmpty() || previous == buildId)
        return false;
    spdlog::info("New build ({} -> {}): clearing results and cache from the previous build", previous.toStdString(),
                 buildId.toStdString());
    resetToFreshState(true);
    return true;
}

QStringList AppController::resetToFreshState(bool keepSettings)
{
    QStringList removed;
    if (busy_)
        return removed;
    pipeline_.reset();
    if (QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase::database(connectionName_, false).close();
        QSqlDatabase::removeDatabase(connectionName_);
    }

    namespace fs = std::filesystem;
    auto remove = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::exists(p, ec))
            return;
        if (fs::remove(p, ec)) {
            removed << toQString(p);
            return;
        }
        // The current log file stays open while the app runs: empty it instead.
        std::ofstream(p, std::ios::trunc);
        removed << toQString(p) + " (emptied)";
    };
    std::vector<fs::path> databases = {config_.databasePath, platform::applicationDataDir() / "multiplexer.sqlite"};
    for (const auto& db : databases) {
        if (db.empty())
            continue;
        if (keepSettings && fs::exists(db)) {
            // Keep the conflict resolutions the user chose; empty everything else.
            try {
                db::Database d(db);
                std::vector<std::string> tables;
                auto q = d.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
                while (q.step())
                    tables.push_back(q.columnText(0));
                for (const auto& t : tables)
                    if (t != "UserResolutions")
                        d.exec("DELETE FROM \"" + t + "\";");
                removed << toQString(db) + " (results cleared, conflict resolutions kept)";
            } catch (const std::exception& e) {
                spdlog::warn("Could not clear {}: {}", pathToUtf8(db), e.what());
            }
        } else {
            remove(db);
            for (const char* suffix : {"-wal", "-shm", "-journal"})
                remove(fs::path(db.native() + fs::path(suffix).native()));
        }
        auto cache = config_.cachePath.empty() ? BuildCache::defaultPathFor(db) : config_.cachePath;
        remove(cache);
        for (const char* suffix : {"-wal", "-shm", "-journal"})
            remove(fs::path(cache.native() + fs::path(suffix).native()));
    }
    std::error_code ec;
    if (!keepSettings)
        for (const auto& entry : fs::directory_iterator(platform::applicationDataDir() / "logs", ec))
            remove(entry.path());

    if (!keepSettings) {
        QSettings().clear();
        QSettings().sync();
        removed << "All saved settings, exclusions and conflict resolutions";
        config_ = PipelineConfig{};
        loadSettings(); // defaults
    }
    spdlog::info("Reset to a fresh state: {} items removed", removed.size());
    emit settingsChanged();
    emit dataChanged();
    return removed;
}

void AppController::reopenDatabase()
{
    if (QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        db.close();
    }
    readDb();
}

void AppController::loadSettings()
{
    QSettings s;
    auto& in = config_.install;
    in.kind = static_cast<InstallKind>(s.value("install/kind", int(InstallKind::ModOrganizer2)).toInt());
    in.gamePath = toPath(s.value("install/gamePath").toString());
    if (in.gamePath.empty())
        if (auto detected = platform::detectGamePath())
            in.gamePath = *detected;
    in.mo2Path = toPath(s.value("install/mo2Path").toString());
    in.mo2Profile = ss(s.value("install/mo2Profile").toString());
    in.pluginsFile = toPath(s.value("install/pluginsFile").toString());
    in.iniDirectory = toPath(s.value("install/iniDirectory").toString());
    in.language = ss(s.value("install/language", "en").toString());
    in.runtime = static_cast<RuntimeProfile>(s.value("install/runtime", 0).toInt());
    in.vortexDownloads = toPath(s.value("vortex/downloadsFolder").toString());

    auto& c = config_.compile;
    c.outputCount = s.value("compile/outputCount", 20).toInt();
    c.autoOutputs = s.value("compile/autoOutputs", false).toBool();
    c.outputPrefix = ss(s.value("compile/prefix", "Dummy_Plugin_").toString());
    c.strategy = static_cast<AllocationStrategy>(s.value("compile/strategy", int(AllocationStrategy::Balanced)).toInt());
    c.mergeCompatible = s.value("compile/merge", true).toBool();
    c.lightOutputsWhenPossible = s.value("compile/light", false).toBool();
    c.buildPatch = s.value("compile/patch", false).toBool();
    c.patchOnly = s.value("compile/patchOnly", false).toBool();
    c.autoMerge = s.value("compile/autoMerge", false).toBool();
    c.rewriteHeuristicReferences = false; // removed: never rewrite values because they look like FormIDs
    c.rules.clear();
    for (const auto& r : s.value("compile/rules").toStringList()) {
        auto parts = r.split('=');
        if (parts.size() == 2)
            c.rules.push_back({ss(parts[0].trimmed()), parts[1].trimmed().toInt()});
    }
    config_.exportOptions.outputDirectory = toPath(s.value("compile/outputDirectory").toString());

    config_.analysis.excludedPlugins.clear();
    for (const auto& p : s.value("analysis/excluded").toStringList())
        config_.analysis.excludedPlugins.push_back(ss(p));
    config_.analysis.compileCreationClub = s.value("analysis/compileCreationClub", false).toBool();
    config_.diagnosticRecorder = s.value("diagnostics/recorder", false).toBool();
    // MultiplexerDiagnosticRecorder.dll ships next to the application.
    config_.diagnosticRecorderDll =
        toPath(QCoreApplication::applicationDirPath()) / fo4mx::diagnostics::kRecorderDllName;
    config_.conflicts.analyseAllRecordTypes = s.value("analysis/allTypes", true).toBool();
    config_.scan.threads = s.value("advanced/threads", 0).toUInt();
    config_.conflicts.threads = config_.analysis.threads = config_.compile.threads = config_.scan.threads;

    QString db = s.value("advanced/database").toString();
    config_.databasePath = db.isEmpty() ? platform::applicationDataDir() / "multiplexer.sqlite" : toPath(db);
    config_.reportDirectory = toPath(s.value("advanced/reportDirectory").toString());
}

void AppController::saveSettings()
{
    QSettings s;
    const auto& in = config_.install;
    s.setValue("install/kind", int(in.kind));
    s.setValue("install/gamePath", toQString(in.gamePath));
    s.setValue("install/mo2Path", toQString(in.mo2Path));
    s.setValue("install/mo2Profile", qs(in.mo2Profile));
    s.setValue("install/pluginsFile", toQString(in.pluginsFile));
    s.setValue("install/iniDirectory", toQString(in.iniDirectory));
    s.setValue("install/language", qs(in.language));
    s.setValue("install/runtime", int(in.runtime));
    s.setValue("vortex/downloadsFolder", toQString(in.vortexDownloads));

    const auto& c = config_.compile;
    s.setValue("compile/outputCount", c.outputCount);
    s.setValue("compile/autoOutputs", c.autoOutputs);
    s.setValue("compile/prefix", qs(c.outputPrefix));
    s.setValue("compile/strategy", int(c.strategy));
    s.setValue("compile/merge", c.mergeCompatible);
    s.setValue("compile/light", c.lightOutputsWhenPossible);
    s.setValue("compile/patch", c.buildPatch);
    s.setValue("compile/patchOnly", c.patchOnly);
    s.setValue("compile/autoMerge", c.autoMerge);
    QStringList rules;
    for (const auto& r : c.rules)
        rules << qs(r.pattern) + "=" + QString::number(r.output);
    s.setValue("compile/rules", rules);
    s.setValue("compile/outputDirectory", toQString(config_.exportOptions.outputDirectory));
    s.setValue("analysis/excluded", excludedPlugins());
    s.setValue("analysis/compileCreationClub", config_.analysis.compileCreationClub);
    s.setValue("diagnostics/recorder", config_.diagnosticRecorder);
    s.setValue("analysis/allTypes", config_.conflicts.analyseAllRecordTypes);
    s.setValue("advanced/threads", config_.scan.threads);
    s.setValue("advanced/database", toQString(config_.databasePath));
    s.setValue("advanced/reportDirectory", toQString(config_.reportDirectory));
    config_.conflicts.threads = config_.analysis.threads = config_.compile.threads = config_.scan.threads;
    if (pipeline_ && !busy_) {
        auto& pc = pipeline_->config();
        auto forced = std::move(pc.compile.forcedWinners);
        pc.compile = config_.compile;
        pc.compile.forcedWinners = std::move(forced);
        pc.exportOptions = config_.exportOptions;
        pc.analysis = config_.analysis;
        pc.reportDirectory = config_.reportDirectory;
    }
    emit settingsChanged();
}

QStringList AppController::excludedPlugins() const
{
    QStringList out;
    for (const auto& p : config_.analysis.excludedPlugins)
        out << qs(p);
    return out;
}

void AppController::setExcluded(const QString& plugin, bool excluded)
{
    auto& list = config_.analysis.excludedPlugins;
    auto key = toLower(ss(plugin));
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) { return toLower(p) == key; }),
               list.end());
    if (excluded)
        list.push_back(ss(plugin));
    saveSettings();
    if (pipeline_ && pipeline_->scanned() && !busy_) {
        pipeline_->config().analysis = config_.analysis;
        pipeline_->refreshEligibility();
        emit dataChanged();
    }
}

void AppController::saveResolution(const QString& originPlugin, quint32 objectId, const QString& winnerPlugin)
{
    db::Database db(config_.databasePath);
    db::DatabaseWriter(db).saveResolution(ss(originPlugin), objectId, ss(winnerPlugin));
    if (pipeline_) {
        auto& s = pipeline_->session();
        auto o = s.findPlugin(ss(originPlugin));
        auto w = s.findPlugin(ss(winnerPlugin));
        if (o && w)
            pipeline_->config().compile.forcedWinners[FormKey{o->index, objectId}.packed()] = w->index;
    }
}

void AppController::clearResolution(const QString& originPlugin, quint32 objectId)
{
    db::Database db(config_.databasePath);
    db::DatabaseWriter(db).clearResolution(ss(originPlugin), objectId);
    if (pipeline_) {
        auto& s = pipeline_->session();
        if (auto o = s.findPlugin(ss(originPlugin)))
            pipeline_->config().compile.forcedWinners.erase(FormKey{o->index, objectId}.packed());
    }
}

void AppController::ensurePipeline(bool fresh)
{
    if (fresh || !pipeline_) {
        pipeline_ = std::make_unique<Pipeline>(config_);
    } else {
        auto& pc = pipeline_->config();
        auto forced = std::move(pc.compile.forcedWinners);
        pc.compile = config_.compile;
        pc.compile.forcedWinners = std::move(forced);
        pc.exportOptions = config_.exportOptions;
        pc.analysis = config_.analysis;
        pc.conflicts = config_.conflicts;
        pc.reportDirectory = config_.reportDirectory;
    }
}

void AppController::runTask(const QString& name, std::function<QString(ProgressSink&)> work)
{
    if (busy_)
        return;
    busy_ = true;
    progress_ = std::make_unique<QtProgress>(this);
    emit busyChanged(true);
    emit taskStarted(name);

    // Close our read connection so the writer is never blocked by an open read cursor.
    if (QSqlDatabase::contains(connectionName_))
        QSqlDatabase::database(connectionName_, false).close();

    auto ok = std::make_shared<bool>(false);
    auto message = std::make_shared<QString>();
    ProgressSink* progress = progress_.get();
    worker_ = QThread::create([work, ok, message, progress] {
        try {
            *message = work(*progress);
            *ok = true;
        } catch (const CancelledError&) {
            *message = "Cancelled";
        } catch (const std::exception& e) {
            *message = QString::fromUtf8(e.what());
            spdlog::error("{}", e.what());
        }
    });
    connect(worker_, &QThread::finished, this, [this, name, ok, message] {
        worker_->deleteLater();
        worker_ = nullptr;
        busy_ = false;
        lastMessage_ = *message;
        reopenDatabase();
        emit busyChanged(false);
        emit taskFinished(name, *ok, *message);
        emit dataChanged();
    });
    worker_->start();
}

void AppController::runScan()
{
    saveSettings();
    ensurePipeline(true);
    runTask("Scan", [this](ProgressSink& p) {
        pipeline_->scan(p);
        const auto& s = pipeline_->session();
        return QString("Scanned %1 plugins and %2 records").arg(s.loadOrder.size()).arg(formatCount(qint64(s.records.size())));
    });
}

void AppController::runAnalyse()
{
    saveSettings();
    ensurePipeline(false);
    runTask("Analyse", [this](ProgressSink& p) {
        pipeline_->analyse(p);
        const auto* c = pipeline_->conflicts();
        return QString("Found %1 conflicts (%2 hard, %3 not merged)")
            .arg(formatCount(qint64(c->conflictCount())), formatCount(qint64(c->byType[3])),
                 formatCount(qint64(c->byType[4])));
    });
}

void AppController::runDefinitionCheck()
{
    saveSettings();
    ensurePipeline(false);
    runTask("Check Definitions", [this](ProgressSink& p) {
        auto check = pipeline_->checkDefinitions({}, p);
        return QString("Checked %1 record types: %2 layouts, %3 warnings")
            .arg(check.recordsByType.size())
            .arg(check.stats.size())
            .arg(check.report.warnings);
    });
}

void AppController::runCompile()
{
    saveSettings();
    ensurePipeline(false);
    runTask("Compile", [this](ProgressSink& p) {
        bool ok = pipeline_->compile(p);
        const auto* v = pipeline_->validation();
        const auto* c = pipeline_->compiledOutput();
        if (!ok)
            throw std::runtime_error("Validation found " + std::to_string(v->errors) +
                                     " critical errors; review the Validation Centre");
        return QString("Compiled %1 records; %2 checks passed, %3 warnings")
            .arg(formatCount(qint64(c->records.size())))
            .arg(formatCount(qint64(v->passed)))
            .arg(formatCount(qint64(v->warnings)));
    });
}

void AppController::runExport()
{
    ensurePipeline(false);
    runTask("Export", [this](ProgressSink& p) {
        auto r = pipeline_->exportOutputs(p);
        QString recorder;
        if (!r.recorderFolder.empty())
            recorder = QString(". Diagnostic Recorder ready to install (optional, see the Install notes): %1")
                           .arg(toQString(r.recorderFolder));
        else if (!r.recorderProblem.empty())
            recorder = QString(". Diagnostic Recorder not prepared: %1").arg(QString::fromStdString(r.recorderProblem));
        if (!r.zipPath.empty())
            return QString("Wrote %1 plugins; the installable package is %2%3")
                .arg(r.pluginFiles.size())
                .arg(toQString(r.zipPath))
                .arg(recorder);
        return QString("Wrote %1 plugins to %2%3").arg(r.pluginFiles.size()).arg(toQString(r.directory)).arg(recorder);
    });
}

void AppController::runCheck()
{
    saveSettings();
    if (!pipeline_)
        pipeline_ = std::make_unique<Pipeline>(config_);
    else
        ensurePipeline(false);
    runTask("Check", [this](ProgressSink& p) {
        auto r = pipeline_->checkForChanges(p);
        return QString("%1 — %2 change(s) since the last build").arg(qs(buildStatusName(r.status))).arg(r.items.size());
    });
}

void AppController::runRebuild()
{
    saveSettings();
    ensurePipeline(true);
    runTask("Rebuild", [this](ProgressSink& p) {
        pipeline_->scan(p);
        pipeline_->analyse(p);
        if (!pipeline_->compile(p))
            throw std::runtime_error("Validation found " + std::to_string(pipeline_->validation()->errors) +
                                     " critical errors; review the Validation Centre");
        auto r = pipeline_->exportOutputs(p);
        const auto* c = pipeline_->compiledOutput();
        return QString("Rebuilt: %1 chains reused, %2 rebuilt · %3 plugins written, %4 unchanged")
            .arg(c->mergesReused)
            .arg(c->mergesComputed)
            .arg(r.writtenOutputs.size())
            .arg(r.unchangedOutputs.size());
    });
}

void AppController::checkOnStartup()
{
    if (!std::filesystem::exists(config_.databasePath))
        return;
    try {
        db::Database db(config_.databasePath);
        db::DatabaseWriter(db).initSchema();
        if (db.scalar("SELECT COUNT(*) FROM Snapshots WHERE kind = 'build'").value_or(0) > 0)
            runCheck();
    } catch (const std::exception& e) {
        spdlog::warn("Startup change check skipped: {}", e.what());
    }
}

void AppController::cancel()
{
    if (busy_ && progress_)
        progress_->requestCancel();
}

namespace {

QString bullet(const std::vector<std::string>& lines, size_t limit = 50)
{
    QStringList out;
    for (size_t i = 0; i < lines.size() && i < limit; ++i)
        out << "  \u2022 " + qs(lines[i]);
    if (lines.size() > limit)
        out << QString("  \u2026 and %1 more").arg(lines.size() - limit);
    return out.join("\n");
}

} // namespace

namespace {

fo4mx::ApplyOptions applyOptions(const fo4mx::Session& s, const AppController::ApplyChoice& choice,
                                 const fo4mx::CompilationResult& result, const fo4mx::PipelineConfig& config,
                                 bool managerRunning)
{
    fo4mx::ApplyOptions o;
    o.ghost = choice.ghost;
    o.managerRunning = managerRunning;
    const auto kind = s.install.config.kind;
    const auto folder = kind == fo4mx::InstallKind::Vortex ? s.install.config.vortexDownloads
                        : kind == fo4mx::InstallKind::ModOrganizer2 ? s.install.downloadsPath
                                                                     : std::filesystem::path();
    if (choice.copyZip && !folder.empty() && !result.zipPath.empty()) {
        const auto name = fo4mx::pathToUtf8(result.zipPath.filename());
        std::error_code ec;
        const bool exists = std::filesystem::exists(folder / result.zipPath.filename(), ec);
        if (exists && choice.replaceZip) {
            o.zipTo = folder / result.zipPath.filename();
            o.replaceZip = true;
        } else {
            o.zipTo = fo4mx::vortex::uniqueName(folder, name);
        }
    }
    if (choice.enableOutputMod && kind == fo4mx::InstallKind::ModOrganizer2) {
        const auto out = config.exportOptions.outputDirectory.empty()
                             ? fo4mx::Exporter::defaultOutputDirectory(s)
                             : config.exportOptions.outputDirectory;
        o.enableOutputMod = fo4mx::mo2::outputModName(s, out);
    }
    return o;
}

AppController::ApplyChoice previewChoice(const AppController::ApplyPreview& p)
{
    AppController::ApplyChoice c;
    c.enableOutputMod = p.mo2 && !p.outputMod.isEmpty() && !p.outputModEnabled;
    return c;
}

bool managerIsRunning(const fo4mx::Session& s)
{
    switch (s.install.config.kind) {
    case fo4mx::InstallKind::Vortex: return fo4mx::platform::isProcessRunning(fo4mx::vortex::kProcessName);
    case fo4mx::InstallKind::ModOrganizer2: return fo4mx::platform::isProcessRunning(fo4mx::mo2::kProcessName);
    case fo4mx::InstallKind::Standalone: break;
    }
    return false;
}

} // namespace

bool AppController::previewApply(ApplyPreview* preview, QString* message)
{
    if (!pipeline_ || !pipeline_->compiledOutput() || !pipeline_->exportResult()) {
        *message = "Compile and export first.";
        return false;
    }
    const auto& s = pipeline_->session();
    try {
        const auto result = fo4mx::makeCompilationResult(s, pipeline_->compiledOutput(), &*pipeline_->exportResult(),
                                                         pipeline_->validation());
        const bool isVortex = s.install.config.kind == fo4mx::InstallKind::Vortex;
        const bool isMo2 = s.install.config.kind == fo4mx::InstallKind::ModOrganizer2;
        *preview = ApplyPreview{};
        preview->vortex = isVortex;
        preview->mo2 = isMo2;
        preview->managerRunning = managerIsRunning(s);
        std::error_code ec;
        preview->zipAvailable = !result.zipPath.empty() && std::filesystem::is_regular_file(result.zipPath, ec);
        preview->zipPath = toQString(result.zipPath);
        if (isMo2) {
            const auto out = config_.exportOptions.outputDirectory.empty()
                                 ? Exporter::defaultOutputDirectory(s)
                                 : config_.exportOptions.outputDirectory;
            const auto mod = fo4mx::mo2::outputModName(s, out);
            preview->outputMod = qs(mod);
            preview->outputModEnabled = !mod.empty() && fo4mx::mo2::modEnabled(s.install.profilePath, mod);
            std::vector<std::string> compiledNames;
            for (const auto& i : result.included)
                compiledNames.push_back(i.name);
            const auto own = fo4mx::mo2::ownership(s, compiledNames);
            preview->ownership = QString("The %1 compiled plugins come from %2 mods. Only the plugins are switched "
                                         "off; the mods stay enabled, because their meshes, textures, sounds and "
                                         "scripts are still used.%3\n")
                                     .arg(own.plugins)
                                     .arg(own.mods)
                                     .arg(own.providedTwice
                                              ? QString(" %1 of the plugins are also in a lower-priority mod; MO2 "
                                                        "loads the winning copy, which is the one compiled.")
                                                    .arg(own.providedTwice)
                                              : QString()) +
                                 bullet(own.lines, 31);
        }
        if (isVortex || isMo2) {
            const auto& folder = isVortex ? s.install.config.vortexDownloads : s.install.downloadsPath;
            preview->downloadsFolder = toQString(folder);
            if (!folder.empty()) {
                const auto fc = fo4mx::vortex::checkDownloadsFolder(folder);
                if (!fc.ok())
                    preview->downloadsProblem = qs(fo4mx::join(fc.problems, "; "));
                if (!fc.notes.empty() && isVortex) // the "fallout4" subfolder is Vortex's convention
                    preview->downloadsNote = qs(fo4mx::join(fc.notes, "; "));
                preview->zipInDownloads = preview->zipAvailable &&
                                          std::filesystem::exists(folder / result.zipPath.filename(), ec);
            }
        }

        fo4mx::FileTransaction tx("Apply to Game (preview)");
        const auto plan = fo4mx::PostCompilation::planApply(tx, s, *pipeline_->compiledOutput(), result,
                                                            pipeline_->exportResult()->directory,
                                                            applyOptions(s, previewChoice(*preview), result, config_,
                                                                         preview->managerRunning));
        const auto& d = plan.check;
        preview->ready = d.blockers.empty();
        QStringList text;
        text << QString("Mod manager: %1").arg(fo4mx::installKindName(s.install.config.kind));
        text << QString("Compiled plugins: %1    Output plugins: %2").arg(result.included.size()).arg(result.outputPlugins.size());
        text << QString("Active after switching: %1 full plugins (limit %2), %3 light")
                    .arg(d.fullPluginsAfter)
                    .arg(fo4mx::PostCompilation::kMaxFullPlugins)
                    .arg(d.lightPluginsAfter);
        if (!d.blockers.empty())
            text << "" << "Your load order will not be switched over until this is fixed:" << bullet(d.blockers);
        if (!d.warnings.empty())
            text << "" << "Check:" << bullet(d.warnings);
        if (preview->ready) {
            std::vector<std::string> lines = tx.summary();
            text << "" << "These files will change (each is backed up first; all or nothing):" << bullet(lines);
            if (isVortex)
                text << "" << "After this, start Vortex and check its Plugins page: the compiled plugins are disabled "
                              "and the outputs enabled.";
            if (isMo2)
                text << "" << "After this, start Mod Organizer 2: the compiled plugins are unticked in the right pane "
                              "and the outputs ticked; every mod stays enabled.";
        }
        for (const auto& g : plan.ghosts)
            (g.ok() ? preview->ghostable : preview->ghostRefused)++;
        std::vector<std::string> refused;
        for (const auto& g : plan.ghosts)
            if (!g.ok())
                refused.push_back(g.plugin + ": " + g.problem);
        preview->ghostDetails = bullet(refused, 200);
        if (isMo2)
            text << "" << preview->ownership;
        preview->offerGhosting = preview->ready && s.install.config.kind != fo4mx::InstallKind::ModOrganizer2 &&
                                 preview->ghostable > 0;
        preview->summary = text.join("\n");
        return true;
    } catch (const std::exception& e) {
        *message = QString::fromUtf8(e.what());
        return false;
    }
}

bool AppController::applyToProfile(const ApplyChoice& choice, QString* message)
{
    if (!pipeline_ || !pipeline_->compiledOutput() || !pipeline_->exportResult()) {
        *message = "Compile and export first.";
        return false;
    }
    auto& s = pipeline_->session();
    try {
        const auto result = fo4mx::makeCompilationResult(s, pipeline_->compiledOutput(), &*pipeline_->exportResult(),
                                                         pipeline_->validation());
        const bool isVortex = s.install.config.kind == fo4mx::InstallKind::Vortex;
        const bool isMo2 = s.install.config.kind == fo4mx::InstallKind::ModOrganizer2;
        const bool running = managerIsRunning(s);
        // One transaction: zip copy, load order, INI, notes and ghosting change together or not at all.
        fo4mx::FileTransaction tx("Apply to Game");
        const auto plan = fo4mx::PostCompilation::planApply(tx, s, *pipeline_->compiledOutput(), result,
                                                            pipeline_->exportResult()->directory,
                                                            applyOptions(s, choice, result, config_, running));
        if (tx.empty()) {
            QStringList why;
            if (!plan.zipProblem.empty())
                why << "  \u2022 " + qs(plan.zipProblem);
            why << bullet(plan.check.blockers);
            *message = "Nothing was changed:\n" + why.join("\n");
            return false;
        }
        s.releaseFiles(); // Windows cannot rename a plugin this app has open
        tx.apply();
        // Report only what was actually done.
        QStringList done;
        size_t ghosted = 0;
        for (const auto& op : tx.operations()) {
            if (!op.applied || op.skipped)
                continue;
            if (op.kind == fo4mx::FileTransaction::Kind::Rename)
                ++ghosted;
            else if (op.kind == fo4mx::FileTransaction::Kind::Copy)
                done << "  \u2022 copied the package to " + toQString(op.target);
            else
                done << "  \u2022 " + toQString(op.target);
        }
        if (ghosted)
            done << QString("  \u2022 %1 compiled plugin(s) ghosted (renamed to .ghost)").arg(ghosted);
        *message = done.isEmpty() ? QString("Nothing needed to change.")
                                  : "Done (backups kept next to each changed file):\n" + done.join("\n");
        if (!plan.check.blockers.empty() && isMo2) {
            *message += "\n\nNext, in Mod Organizer 2: install the package (Downloads tab, or Install a new mod from "
                        "an archive), enable it and keep it at the bottom of the left pane. Then close Mod Organizer 2 "
                        "and press Apply to Game again. Still to do:\n" +
                        bullet(plan.check.blockers);
        } else if (!plan.check.blockers.empty()) {
            // Only the package was copied: switching over is the next step.
            *message += QString(plan.copiesZip ? "\n\nNext, in Vortex: install and enable the package from the Downloads "
                                                 "page (restart Vortex if it is not listed), "
                                               : "\n\nNext, in Vortex: install and enable Multiplexer Output.zip, ") +
                        "let it win every file conflict and Deploy. Then close Vortex and press Apply to Game again to "
                        "switch your load order over. Still to do:\n" +
                        bullet(plan.check.blockers);
        } else if (isMo2) {
            *message += "\n\nStart Mod Organizer 2: the compiled plugins are unticked and the outputs ticked; every "
                        "mod stays enabled.";
        } else if (isVortex) {
            *message += "\n\nStart Vortex and check its Plugins page: the plugins in Multiplexer_DisablePlugins.txt "
                        "are disabled and the outputs enabled.";
        }
        if (plan.check.blockers.empty())
            *message += "\n\nStart a new game with this build.";
        return true;
    } catch (const std::exception& e) {
        *message = "Nothing was changed (every completed step was undone): " + QString::fromUtf8(e.what());
        return false;
    }
}

} // namespace gui
