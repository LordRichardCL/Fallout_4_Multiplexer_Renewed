#pragma once

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
