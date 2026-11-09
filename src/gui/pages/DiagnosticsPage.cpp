#include "pages/DiagnosticsPage.hpp"

#include "Format.hpp"
#include "widgets/Widgets.hpp"

#include "engine/Exporter.hpp"
#include "engine/OutputRuns.hpp"
#include "util/Platform.hpp"

#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

namespace dg = fo4mx::diagnostics;

DiagnosticsPage::DiagnosticsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    auto* newest = makeButton("Open Newest Log", "Primary");
    auto* openBtn = makeButton("Open Log…");
    auto* folder = makeButton("Open Log Folder");
    auto* save = makeButton("Save Report…");
    auto* check = makeButton("Check Installation");
    l->addWidget(makePageHeader("Diagnostics",
                                "What the optional Diagnostic Recorder saw while you played, and what it likely means.",
                                {newest, openBtn, folder, check, save}));

    auto* intro = new QLabel(
        "Turn on <b>Prepare the Diagnostic Recorder</b> in Settings, export, and install "
        "<i>Multiplexer Output\\Diagnostic Recorder\\Multiplexer Diagnostic Recorder.zip</i> as a mod. Start the game "
        "through F4SE; each session writes a log. Findings are likely causes with their evidence, not certainties; "
        "for crashes, keep a crash logger such as Buffout 4 as well. When a Multiplexer build is found, the game's "
        "load order is compared with the one it was built for.");
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8aa196");
    l->addWidget(intro);

    install_ = new QLabel;
    install_->setWordWrap(true);
    install_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(install_);

    summary_ = new QLabel("No log opened.");
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(summary_);

    findings_ = new QTreeWidget;
    findings_->setColumnCount(4);
    findings_->setHeaderLabels({"Severity", "Finding", "Likely cause", "Confidence"});
    findings_->setAlternatingRowColors(true);
    findings_->setWordWrap(true);
    findings_->header()->setStretchLastSection(false);
    findings_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    findings_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    findings_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    findings_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    findings_->setColumnWidth(1, 360);
    l->addWidget(findings_, 1);

    connect(newest, &QPushButton::clicked, this, [this] {
        const auto file = dg::newestSession(dg::defaultRecorderFolder());
        if (file.empty()) {
            QMessageBox::information(this, "Diagnostics",
                                     "No recorder log yet in " + toQString(dg::defaultRecorderFolder()) +
                                         ".\n\nInstall the Diagnostic Recorder and start the game through F4SE first.");
            return;
        }
        open(toQString(file));
    });
    connect(openBtn, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, "Open a Diagnostic Recorder log",
                                                       toQString(dg::defaultRecorderFolder()), "Recorder logs (*.jsonl)");
        if (!f.isEmpty())
            open(f);
    });
    connect(check, &QPushButton::clicked, this, [this] {
        showInstallStatus();
        const auto st = dg::checkRecorderInstall(app_->config().install.gamePath);
        QMessageBox::information(this, "Diagnostic Recorder", QString::fromStdString(dg::formatInstallStatus(st)));
    });
    connect(folder, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(dg::defaultRecorderFolder()))); });
    connect(save, &QPushButton::clicked, this, [this] {
        if (file_.isEmpty())
            return;
        const QString f = QFileDialog::getSaveFileName(this, "Save the diagnostics report", file_ + ".txt", "Text (*.txt)");
        if (f.isEmpty())
            return;
        QFile out(f);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(QByteArray::fromStdString(dg::formatReport(session_, results_, package_ ? &*package_ : nullptr)));
    });
}

std::filesystem::path DiagnosticsPage::packageRoot() const
{
    // The export to compare with: the one exported in this session, else the latest finished export
    // in the output folder (loadPackageContext resolves the folder holding the exports to it).
    auto* p = app_->pipeline();
    if (p && p->exportResult())
        return p->exportResult()->packageRoot;
    const auto& c = app_->config();
    const auto mods = p && p->scanned() ? p->session().install.modsPath : std::filesystem::path();
    const auto loc = fo4mx::output::locate(c.exportOptions.outputDirectory, c.install.gamePath,
                                           c.install.gamePath.empty() ? std::filesystem::path() : c.install.gamePath / "Data",
                                           mods);
    if (auto latest = fo4mx::output::latestRun(loc))
        return *latest;
    return loc.root;
}

void DiagnosticsPage::open(const QString& file)
{
    file_ = file;
    session_ = dg::loadRecorderLog(toPath(file));
    std::optional<dg::PackageContext> package;
    if (const auto root = packageRoot(); !root.empty())
        package = dg::loadPackageContext(root);
    if (package)
        package->expectedProfile =
            app_->config().install.runtime == fo4mx::RuntimeProfile::NextGen ? "nextgen" : "oldgen";
    results_ = dg::analyse(session_, package ? &*package : nullptr);
    package_ = package;
    const auto archives = dg::checkArchives(session_, package ? &*package : nullptr);

    size_t active = 0;
    for (const auto& p : session_.plugins)
        active += p.second;
    summary_->setText(QString("<b>%1</b><br>Started %2 · Fallout 4 %3 · F4SE %4 · recorder %5 · %6 active plugins · "
                              "%7<br>%8")
                          .arg(file.toHtmlEscaped())
                          .arg(QString::fromStdString(session_.startTime))
                          .arg(QString::fromStdString(session_.runtimeVersion))
                          .arg(QString::fromStdString(session_.f4seVersion))
                          .arg(QString::fromStdString(session_.recorderVersion))
                          .arg(active)
                          .arg(session_.ended                       ? "ended normally"
                               : dg::quitAfterExitSave(session_) ? "quit after the exit save"
                                                                 : "<b>no normal exit recorded</b>")
                          .arg((package ? "Compared with the build in " + toQString(package->root).toHtmlEscaped()
                                        : QString("No Multiplexer build found to compare with.")) +
                               (archives.available
                                    ? QString("<br><b>Archives:</b> %1 of %2 requested loaded%3 · %4")
                                          .arg(archives.loaded.size())
                                          .arg(archives.requested.size())
                                          .arg(archives.truncated
                                                   ? QString(" · list cut at %1 characters").arg(archives.received)
                                                   : QString())
                                          .arg(QString::fromStdString(archives.verdict).toHtmlEscaped())
                                    : QString())));

    findings_->clear();
    for (const auto& f : results_) {
        auto* item = new QTreeWidgetItem(findings_, {dg::findingSeverityName(f.severity), QString::fromStdString(f.title),
                                                     QString::fromStdString(f.cause), QString::fromStdString(f.confidence)});
        for (int c = 1; c <= 2; ++c)
            item->setToolTip(c, item->text(c));
        for (const auto& e : f.evidence)
            new QTreeWidgetItem(item, {"Evidence", QString::fromStdString(e)});
        for (const auto& c : f.checks)
            new QTreeWidgetItem(item, {"Check", QString::fromStdString(c)});
        item->setExpanded(f.severity == dg::FindingSeverity::Error);
    }
    if (results_.empty())
        new QTreeWidgetItem(findings_, {"", "No recognised problems in this session."});
}

void DiagnosticsPage::refresh()
{
    showInstallStatus();
}

void DiagnosticsPage::showInstallStatus()
{
    // Read from f4se.log of the last game start: whether F4SE loaded the recorder, or why not.
    const auto st = dg::checkRecorderInstall(app_->config().install.gamePath);
    install_->setText(QString("<b>Recorder:</b> <span style=\"color:%1\">%2</span> %3")
                          .arg(st.ok ? "#6fbf73" : "#e0a040")
                          .arg(st.ok ? "loaded" : "not loaded")
                          .arg(QString::fromStdString(st.verdict).toHtmlEscaped()));
}

} // namespace gui
