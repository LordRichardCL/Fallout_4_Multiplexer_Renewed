#include "AppController.hpp"

#include "Format.hpp"

#include "db/DatabaseWriter.hpp"
#include "engine/Exporter.hpp"
#include "util/Log.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <QMetaObject>
#include <QSettings>
#include <QSqlError>
#include <QThread>

#include <mutex>
#include <algorithm>

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

    auto& c = config_.compile;
    c.outputCount = s.value("compile/outputCount", 20).toInt();
    c.outputPrefix = ss(s.value("compile/prefix", "Dummy_Plugin_").toString());
    c.strategy = static_cast<AllocationStrategy>(s.value("compile/strategy", int(AllocationStrategy::Balanced)).toInt());
    c.mergeCompatible = s.value("compile/merge", true).toBool();
    c.lightOutputsWhenPossible = s.value("compile/light", false).toBool();
    c.rewriteHeuristicReferences = s.value("compile/rewriteHeuristic", false).toBool();
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

    const auto& c = config_.compile;
    s.setValue("compile/outputCount", c.outputCount);
    s.setValue("compile/prefix", qs(c.outputPrefix));
    s.setValue("compile/strategy", int(c.strategy));
    s.setValue("compile/merge", c.mergeCompatible);
    s.setValue("compile/light", c.lightOutputsWhenPossible);
    s.setValue("compile/rewriteHeuristic", c.rewriteHeuristicReferences);
    QStringList rules;
    for (const auto& r : c.rules)
        rules << qs(r.pattern) + "=" + QString::number(r.output);
    s.setValue("compile/rules", rules);
    s.setValue("compile/outputDirectory", toQString(config_.exportOptions.outputDirectory));
    s.setValue("analysis/excluded", excludedPlugins());
    s.setValue("analysis/compileCreationClub", config_.analysis.compileCreationClub);
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
        return QString("Found %1 conflicts (%2 hard)").arg(formatCount(qint64(c->conflictCount()))).arg(
            formatCount(qint64(c->byType[3])));
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
        return QString("Wrote %1 plugins to %2").arg(r.pluginFiles.size()).arg(toQString(r.directory));
    });
}

void AppController::cancel()
{
    if (busy_ && progress_)
        progress_->requestCancel();
}

bool AppController::applyToProfile(QString* message)
{
    if (!pipeline_ || !pipeline_->compiledOutput() || !pipeline_->exportResult()) {
        *message = "Compile and export first.";
        return false;
    }
    const auto& s = pipeline_->session();
    try {
        auto written = Exporter::applyToProfile(s, *pipeline_->compiledOutput(), s.install.pluginsFile);
        QStringList names;
        for (const auto& w : written)
            names << toQString(w);
        *message = "Updated (backups kept next to each file):\n" + names.join("\n");
        return true;
    } catch (const std::exception& e) {
        *message = QString::fromUtf8(e.what());
        return false;
    }
}

} // namespace gui
