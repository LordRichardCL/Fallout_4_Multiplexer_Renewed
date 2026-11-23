#pragma once

#include <functional>

#include "pages/Page.hpp"

#include "diagnostics/RecorderLog.hpp"

#include <optional>

class QLabel;
class QTreeWidget;

namespace gui {

// Reads a Diagnostic Recorder session (MultiplexerDiagnosticRecorder.dll) and shows what it
// recognises: likely causes, the evidence in the log, and what to check.
class DiagnosticsPage : public Page {
    Q_OBJECT
public:
    explicit DiagnosticsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void open(const QString& file);
    // Buffout 4 crash log read against the latest build (empty: the newest one, else ask).
    void readCrashLog(QString file);
    bool crashReading(const QString& file, QString* text) const;
    void showCrashReading(const QString& file, const QString& text);
    void showReading(const QString& title, const QString& file, const QString& text, const QString& filter,
                     const std::function<void(const QString&)>& again,
                     const std::function<QString(QWidget*)>& pickAnother = {});
    void readPapyrusLog(QString file);
    bool papyrusReading(const QString& file, QString* text) const;
    // Startup popups and MCM menus of the latest build, from a Papyrus log, MCM.log and the load order the game
    // had (f4se.log's plugin list, else the scanned one).
    void checkStartup(QString file);
    QString startupReading(const QString& papyrusFile) const;
    QString saveCheckReading(const QString& file) const;
    // "A mod doesn't seem to work": the later plugins that overwrite a plugin's changes (conflict analysis).
    void checkOverwritten(QString plugin);
    QString overwrittenReading(const QString& plugin) const;
    QString pickPlugin(QWidget* parent) const;
    QString lastChecked_;
    void checkSaveGame(QString file);
    QLabel* noLog_ = nullptr; // shown until a recorder log is opened
    // One zip with what it takes to look into a problem (Support Bundle…).
    void makeSupportBundle();
    std::filesystem::path packageRoot() const;

    void showInstallStatus();

    QLabel* install_;
    QLabel* summary_;
    QTreeWidget* findings_;
    QString file_;
    fo4mx::diagnostics::RecorderSession session_;
    std::vector<fo4mx::diagnostics::Finding> results_;
    std::optional<fo4mx::diagnostics::PackageContext> package_;
};

} // namespace gui
