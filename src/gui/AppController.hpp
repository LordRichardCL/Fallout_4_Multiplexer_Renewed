#pragma once

#include "engine/Pipeline.hpp"

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class QThread;

namespace gui {

class QtProgress;

// Owns the compiler pipeline and runs its stages on a worker thread.
// Pages read results from the SQLite database through readDb().
class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    fo4mx::PipelineConfig& config() { return config_; }
    void loadSettings();
    void saveSettings();

    fo4mx::Pipeline* pipeline() { return pipeline_.get(); }
    bool busy() const { return busy_; }
    bool hasSession() const { return pipeline_ && pipeline_->scanned(); }
    QString databasePath() const;
    QSqlDatabase readDb();
    void reopenDatabase();
    // Forgets everything this app stored: settings, the results database, the build cache, logs
    // and remembered conflict resolutions. Generated plugins in the output folder are not touched.
    // Returns what was removed.
    QStringList resetToFreshState(bool keepSettings = false);
    // On the first start of a different build: clears results and the cache from the previous one
    // (settings are kept). Returns true when it did.
    bool clearResultsAfterUpgrade(const QString& buildId);

    QStringList excludedPlugins() const;
    void setExcluded(const QString& plugin, bool excluded);
    void saveResolution(const QString& originPlugin, quint32 objectId, const QString& winnerPlugin);
    void clearResolution(const QString& originPlugin, quint32 objectId);

    // Last task's outcome, for the status bar.
    QString lastMessage() const { return lastMessage_; }

public slots:
    void runScan();
    void runAnalyse();
    void runCompile();
    void runExport();
    // Compares the mod space with the last build (no full scan).
    void runCheck();
    // Scan, analyse, compile and export in one go, reusing cached work.
    void runRebuild();
    // Decodes every record of the supported types and reports layouts that look wrong.
    void runDefinitionCheck();
    // On startup: check for changes when a previous build exists.
    void checkOnStartup();
    void cancel();
    bool applyToProfile(QString* message);

signals:
    void busyChanged(bool busy);
    void taskStarted(const QString& task);
    void stageStarted(const QString& stage);
    void progressChanged(qint64 done, qint64 total, const QString& detail);
    void taskFinished(const QString& task, bool ok, const QString& message);
    void dataChanged();
    void logMessage(int level, const QString& text);
    void settingsChanged();

private:
    void runTask(const QString& name, std::function<QString(fo4mx::ProgressSink&)> work);
    void ensurePipeline(bool fresh);

    fo4mx::PipelineConfig config_;
    std::unique_ptr<fo4mx::Pipeline> pipeline_;
    std::unique_ptr<QtProgress> progress_;
    QThread* worker_ = nullptr;
    bool busy_ = false;
    QString lastMessage_;
    QString connectionName_ = "fo4mx-ui";
    std::shared_ptr<void> logSink_;
};

} // namespace gui
