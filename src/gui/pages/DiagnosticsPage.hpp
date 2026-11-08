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
