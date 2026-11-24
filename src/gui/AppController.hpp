#pragma once
#include <QTimer>
#include <QFileSystemWatcher>

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
    // The name of the quiet changes check that runs when a watched file changes (watchInstall).
    static constexpr const char* kAutoCheck = "Auto check";
    void loadSettings();
    // Persists the whole settings state (QSettings) and applies it to the pipeline.
    void saveSettings();
    // A page changed the settings state without saving it: marks it unsaved.
    void settingsEdited();
    bool hasUnsavedSettings() const { return unsaved_; }

    fo4mx::Pipeline* pipeline() { return pipeline_.get(); }
    bool busy() const { return busy_; }

    // What a scan would read, from the plugin and archive lists only (no plugin is opened): for the
    // question asked before scanning.
    struct ScanSummary {
        bool ok = false;
        QString error;
        QString manager; // "Vortex", "Mod Organizer 2 (profile Default)", ...
        int activePlugins = 0;
        int inactivePlugins = 0;
        int archives = 0;
    };
    ScanSummary scanSummary() const;
    // Whether Scan asks what to read first (saved at once).
    bool askBeforeScan() const;
    void setAskBeforeScan(bool ask);
    // Also preview inactive plugins when scanning (saved at once).
    bool previewInactive() const { return config_.scan.previewInactive; }
    void setPreviewInactive(bool preview);
    bool hasSession() const { return pipeline_ && pipeline_->scanned(); }
    QString databasePath() const;
    QSqlDatabase readDb();
    void reopenDatabase();
    // Forgets everything this app stored: settings, the results database, the build cache, logs
    // and remembered conflict resolutions. Generated plugins in the output folder are not touched.
    // Returns what was removed.
    QStringList resetToFreshState(bool keepSettings = false);
    // On the first start of a different build: clears the previous build's derived results (settings,
    // conflict resolutions and build snapshots are kept; the build cache checks itself). Returns true when it did.
    bool clearResultsAfterUpgrade(const QString& buildId);
    // Empties the results database's derived tables (what Scan and Analyse compute), keeping the user's
    // conflict resolutions and the build snapshots. The build cache is left to check itself.
    void clearDerivedResults();

    QStringList excludedPlugins() const;
    void setExcluded(const QString& plugin, bool excluded);
    // Master Redirection: plugins kept as they are (never redirected). Saved at once.
    QStringList notRedirected() const;
    void setRedirect(const QString& plugin, bool allowed);
    // Which plugins are compiled (Compilation page): every eligible one not excluded, or only the chosen
    // ones (opt-in). Like exclusions these are saved at once and applied to the current analysis.
    void setExcludedMany(const QStringList& plugins, bool excluded);
    bool onlySelected() const { return config_.analysis.onlySelected; }
    QStringList chosenPlugins() const;
    void setOnlySelected(bool on);
    void setChosen(const QStringList& plugins, bool chosen);
    void applySelection();
    void saveResolution(const QString& originPlugin, quint32 objectId, const QString& winnerPlugin);
    void clearResolution(const QString& originPlugin, quint32 objectId);
    // The same for many records at once (one transaction): `records` holds (origin plugin, object ID).
    void saveResolutions(const QVector<QPair<QString, quint32>>& records, const QString& winnerPlugin);
    void clearResolutions(const QVector<QPair<QString, quint32>>& records);
    // The compatibility patch plan: the user's choice for one change (action -1: back to the default,
    // 0: carry forward, 1: keep the winner's value). Saved next to the database. False while a task runs or
    // before Analyse.
    bool setPlanDecision(const QString& originPlugin, quint32 objectId, const QString& changeId, int action);
    // Accepts the automatic choice for the changes that wait (one record, or every record with
    // originPlugin empty): they keep the winner's values, as compiling does, and stop waiting. Returns how many
    // changes were decided, or -1 when it cannot run now (a task is running, or Analyse has not run).
    int acceptAutomatic(const QString& originPlugin = {}, quint32 objectId = 0);

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
        bool recorderAvailable = false; // this export prepared the Diagnostic Recorder's zip
        QString recorderZipPath;
        bool recorderInDownloads = false; // an older copy is there (it is replaced, backed up)
    };
    struct ApplyChoice {
        bool ghost = false;
        bool copyZip = false;
        bool replaceZip = false; // the archive is already in the downloads folder: replace it (else keep both)
        bool enableOutputMod = false; // MO2: enable the output mod in the profile
        bool copyRecorder = false;    // also copy the Diagnostic Recorder's zip to the downloads folder
    };
    bool previewApply(ApplyPreview* preview, QString* message);
    bool applyToProfile(const ApplyChoice& choice, QString* message);
    // Switch back: undo the last Apply to Game from its backups (recorded in the folder holding the exports).
    bool canSwitchBack() const;
    bool previewSwitchBack(QString* text, QString* message);
    bool switchBack(QString* message);

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
    // Watches plugins.txt, the mod list and the mods and Data folders while the app is open: a few seconds
    // after one changes, the changes check runs again quietly ("Auto check": no summary card), so the
    // Dashboard says a rebuild is needed without a restart. Only once a build exists.
    void watchInstall();
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
    std::filesystem::path switchBackDirectory() const;
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
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* watchDelay_ = nullptr;
    bool watchPending_ = false; // a change came in while a task ran
    void watchedChange(const QString& path);
    void autoCheck();
    bool hasBuild() const;
    QString lastMessage_;
    QString connectionName_ = "fo4mx-ui";
    std::shared_ptr<void> logSink_;
};

} // namespace gui
