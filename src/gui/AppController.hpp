#pragma once

#include "engine/Pipeline.hpp"

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <atomic>
#include <filesystem>
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

    // The application's settings state, shared by every page. Pages write option changes here as they
    // are made (and call settingsEdited()), so they survive navigating between pages; saveSettings()
    // persists them and survives a restart. Pages show what is here, never keep their own copy.
    fo4mx::PipelineConfig& config() { return config_; }
    void loadSettings();
    // Persists the whole settings state (QSettings) and applies it to the pipeline.
    void saveSettings();
    // A page changed the settings state without saving it: marks it unsaved.
    void settingsEdited();
    bool hasUnsavedSettings() const { return unsaved_; }

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

    // Apply to Game, in two steps: what it will do (nothing changes yet), then doing it.
    struct ApplyPreview {
        bool ready = false;         // false: switching over must wait (see summary)
        QString summary;            // the planned changes and the checks, in plain language
        bool offerGhosting = false; // plugins that may be ghosted (manual install, or not deployed by Vortex)
        int ghostable = 0;
        int ghostRefused = 0;
        QString ghostDetails;       // which plugins cannot be ghosted, and why
        // Vortex and Mod Organizer 2
        bool vortex = false;
        bool mo2 = false;
        bool managerRunning = false; // Vortex.exe / ModOrganizer.exe is running
        QString outputMod;           // MO2: the output folder is this mod in MO2's mods folder
        bool outputModEnabled = false;
        QString ownership;           // MO2: which mods provide the compiled plugins
        bool zipAvailable = false;  // this export wrote Multiplexer Output.zip
        QString zipPath;
        QString downloadsFolder;    // Vortex: from Settings; MO2: from ModOrganizer.ini; empty when not set
        QString downloadsProblem;   // why it cannot be used (empty: usable)
        QString downloadsNote;
        bool zipInDownloads = false; // a file of the same name is already there
    };
    struct ApplyChoice {
        bool ghost = false;
        bool copyZip = false;
        bool replaceZip = false; // the archive is already in the downloads folder: replace it (else keep both)
        bool enableOutputMod = false; // MO2: enable the output mod in the profile
    };
    bool previewApply(ApplyPreview* preview, QString* message);
    bool applyToProfile(const ApplyChoice& choice, QString* message);

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

signals:
    void busyChanged(bool busy);
    void taskStarted(const QString& task);
    void stageStarted(const QString& stage);
    void progressChanged(qint64 done, qint64 total, const QString& detail);
    void taskFinished(const QString& task, bool ok, const QString& message);
    void dataChanged();
    void logMessage(int level, const QString& text);
    // The settings state was loaded, saved or reset: pages show it again.
    void settingsChanged();
    // Whether the settings state has changes that are not saved yet.
    void unsavedSettingsChanged(bool unsaved);

private:
    void runTask(const QString& name, std::function<QString(fo4mx::ProgressSink&)> work);
    void ensurePipeline(bool fresh);

    // The pipeline's configuration from the settings state. The results database is the saved one:
    // a path typed on the Settings page takes effect when saved.
    fo4mx::PipelineConfig pipelineConfig() const;
    void setUnsaved(bool unsaved);

    fo4mx::PipelineConfig config_;
    std::filesystem::path activeDatabase_; // the saved results database (what readDb() opens)
    bool unsaved_ = false;
    std::unique_ptr<fo4mx::Pipeline> pipeline_;
    std::unique_ptr<QtProgress> progress_;
    QThread* worker_ = nullptr;
    bool busy_ = false;
    QString lastMessage_;
    QString connectionName_ = "fo4mx-ui";
    std::shared_ptr<void> logSink_;
};

} // namespace gui
