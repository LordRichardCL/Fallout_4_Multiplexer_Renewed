#include "pages/DiagnosticsPage.hpp"
#include "diagnostics/CrashLog.hpp"
#include <functional>
#include "diagnostics/PapyrusLog.hpp"
#include <QStandardPaths>
#include <QSettings>
#include <QDate>
#include "engine/SupportBundle.hpp"
#include "engine/CompilationEngine.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"
#include <QDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <set>

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
    auto* newest = makeButton("Open Newest Log", "Primary", "diagnostics");
    auto* openBtn = makeButton("Open Log…", nullptr, "folder");
    auto* folder = makeButton("Log Folder", nullptr, "folder");
    folder->setToolTip("Open the folder the Diagnostic Recorder writes its logs to");
    folder->setAccessibleName("Open Log Folder");
    auto* save = makeButton("Save Report…", nullptr, "export");
    auto* check = makeButton("Check Installation", nullptr, "check");
    auto* bundle = makeButton("Support Bundle…", nullptr, "export");
    bundle->setToolTip("One zip with what it takes to look into a problem: the latest build's reports and records, "
                       "this app's logs, your load order, the newest crash log with its reading, the newest recorder "
                       "log and the settings. No mod files; your home folder and user name are taken out.");
    auto* papyrus = makeButton("Read Papyrus Log", nullptr, "warning");
    papyrus->setToolTip("Reads Papyrus.0.log with your current load order and names the original mod behind every "
                        "compiled record it mentions, with its most frequent errors.");
    auto* crash = makeButton("Read Crash Log", nullptr, "warning");
    crash->setToolTip("Read the newest Buffout 4 crash log (Documents\\My Games\\Fallout4\\F4SE\\Crash-*.log) and "
                      "name the original mod behind every output plugin FormID it mentions");
    l->addWidget(makePageHeader("Diagnostics", "What happened while you played, and what it likely means.",
                                {newest, openBtn, folder, check, save, crash, papyrus, bundle}, "diagnostics",
                                "To record: turn on \"Prepare the Diagnostic Recorder\" in Settings, export, and "
                                "install Multiplexer Output\\Diagnostic Recorder\\Multiplexer Diagnostic Recorder.zip "
                                "as a mod. Start the game through F4SE; each session writes a log. Findings are likely "
                                "causes with their evidence, not certainties; for crashes, keep a crash logger such as "
                                "Buffout 4 as well. When a Multiplexer build is found, the game's load order is compared "
                                "with the one it was built for."));

    install_ = new QLabel;
    install_->setWordWrap(true);
    install_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(install_);

    summary_ = new QLabel("No log opened yet. <b>Open Newest Log</b> reads your latest session; the <b>?</b> above says how to record one.");
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
    connect(crash, &QPushButton::clicked, this, [this] { readCrashLog({}); });
    connect(bundle, &QPushButton::clicked, this, [this] { makeSupportBundle(); });
    connect(papyrus, &QPushButton::clicked, this, [this] { readPapyrusLog({}); });
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

void DiagnosticsPage::readCrashLog(QString file)
{
    if (file.isEmpty()) {
        const auto newest = dg::newestCrashLog();
        file = newest.empty() ? QFileDialog::getOpenFileName(this, "Open a Buffout 4 crash log",
                                                             toQString(fo4mx::platform::fallout4DocumentsDir() / "F4SE"),
                                                             "Crash logs (*.log)")
                              : toQString(newest);
        if (file.isEmpty())
            return;
    }
    QString text;
    if (!crashReading(file, &text)) {
        QMessageBox::warning(this, "Crash log", "Cannot read " + file);
        return;
    }
    showCrashReading(file, text);
}

bool DiagnosticsPage::crashReading(const QString& file, QString* text) const
{
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = in.readAll();
    // The latest build's FormID map and redirected copies (its Data folder, or the folder itself).
    std::vector<fo4mx::FormMapping> map;
    std::set<std::string> redirected;
    std::string buildNote = "No Multiplexer build found: FormIDs are shown with their plugins only.";
    if (const auto root = packageRoot(); !root.empty())
        for (const auto& dir : {root / "Data", root}) {
            const auto formMap = dir / "Multiplexer_FormMap.tsv";
            if (!std::filesystem::exists(formMap))
                continue;
            map = fo4mx::CompilationEngine::readFormMap(formMap);
            QFile r(toQString(dir / "Multiplexer_Redirected.tsv"));
            if (r.open(QIODevice::ReadOnly))
                for (const auto& line : QString::fromUtf8(r.readAll()).split('\n'))
                    if (!line.startsWith('#') && !line.trimmed().isEmpty())
                        redirected.insert(ss(line.section('\t', 0, 0).trimmed()));
            buildNote = "Read against the build in " + fo4mx::pathToUtf8(root) + " (" + std::to_string(map.size()) +
                        " FormID mappings).";
            break;
        }
    const auto report = dg::translateCrashLog(std::string_view(bytes.constData(), size_t(bytes.size())), map, {}, redirected);
    *text = toQString(std::filesystem::path(ss(file)).filename()) + "\n" + qs(buildNote) + "\n\n" +
            qs(dg::formatCrashReport(report));
    return true;
}

void DiagnosticsPage::showCrashReading(const QString& file, const QString& text)
{
    showReading("Crash log", file, text, "Crash logs (*.log)", [this](const QString& f) { readCrashLog(f); });
}

void DiagnosticsPage::showReading(const QString& title, const QString& file, const QString& text,
                                  const QString& filter, const std::function<void(const QString&)>& again)
{
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(820, 620);
    auto* v = new QVBoxLayout(&dlg);
    auto* view = new QPlainTextEdit(text);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    v->addWidget(view, 1);
    auto* buttons = new QHBoxLayout;
    auto* other = makeButton("Open Another…", nullptr, "folder");
    auto* saveText = makeButton("Save…", nullptr, "export");
    auto* close = makeButton("Close");
    buttons->addWidget(other);
    buttons->addStretch();
    buttons->addWidget(saveText);
    buttons->addWidget(close);
    v->addLayout(buttons);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(saveText, &QPushButton::clicked, &dlg, [&] {
        const QString f = QFileDialog::getSaveFileName(&dlg, "Save the reading", file + ".multiplexer.txt",
                                                       "Text (*.txt)");
        QFile out(f);
        if (!f.isEmpty() && out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            out.write(text.toUtf8());
    });
    QString next;
    connect(other, &QPushButton::clicked, &dlg, [&] {
        next = QFileDialog::getOpenFileName(&dlg, "Open another " + title.toLower(),
                                            QFileInfo(file).absolutePath(), filter);
        if (!next.isEmpty())
            dlg.accept();
    });
    dlg.exec();
    if (!next.isEmpty())
        again(next);
}

void DiagnosticsPage::readPapyrusLog(QString file)
{
    auto* p = app_->pipeline();
    if (!p || !p->scanned()) {
        QMessageBox::information(this, "Papyrus log",
                                 "Scan first: a Papyrus log has no plugin list, so it is read with your current load "
                                 "order.");
        return;
    }
    if (file.isEmpty()) {
        const auto def = dg::defaultPapyrusLog();
        file = std::filesystem::exists(def)
                   ? toQString(def)
                   : QFileDialog::getOpenFileName(this, "Open a Papyrus log", toQString(def.parent_path()),
                                                  "Papyrus logs (*.log)");
        if (file.isEmpty())
            return;
    }
    QFile in(file);
    if (!in.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Papyrus log", "Cannot read " + file);
        return;
    }
    const QByteArray bytes = in.readAll();
    std::vector<fo4mx::FormMapping> map;
    std::set<std::string> redirected;
    std::string buildNote = "No build found to read it against: compiled records are not translated.";
    if (const auto root = packageRoot(); dg::readBuildMap(root, map, redirected))
        buildNote = "Read against the build in " + fo4mx::pathToUtf8(root) + " (" + std::to_string(map.size()) +
                    " FormID mappings).";
    const auto report = dg::translatePapyrusLog(std::string_view(bytes.constData(), size_t(bytes.size())),
                                                dg::runtimeLoadOrder(p->session()), map, redirected);
    const QString text = QFileInfo(file).fileName() + "\n" + qs(buildNote) + "\n\n" + qs(dg::formatPapyrusReport(report));
    showReading("Papyrus log", file, text, "Papyrus logs (*.log)", [this](const QString& f) { readPapyrusLog(f); });
}

void DiagnosticsPage::makeSupportBundle()
{
    const QString name = "Multiplexer Support " + QDate::currentDate().toString("ddMMyy") + ".zip";
    const QString where = QFileDialog::getSaveFileName(
        this, "Save the support bundle", QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) + "/" + name,
        "Zip (*.zip)");
    if (where.isEmpty())
        return;
    fo4mx::SupportBundleInputs in;
    in.packageRoot = packageRoot();
    in.reportDirectory = app_->config().reportDirectory;
    std::error_code ec;
    const auto logs = fo4mx::platform::applicationDataDir() / "logs";
    for (auto it = std::filesystem::directory_iterator(logs, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec))
            in.logs.push_back(it->path());
    if (auto* p = app_->pipeline(); p && p->scanned()) {
        const auto& install = p->session().install;
        for (const auto& f : {install.pluginsFile, install.profilePath / "loadorder.txt", install.profilePath / "modlist.txt"})
            if (!f.empty() && std::filesystem::is_regular_file(f, ec))
                in.loadOrder.push_back(f);
    }
    in.crashLog = dg::newestCrashLog();
    if (!in.crashLog.empty()) {
        QString text;
        if (crashReading(toQString(in.crashLog), &text))
            in.crashReading = ss(text);
    }
    in.recorderSession = dg::newestSession(dg::defaultRecorderFolder());
    QSettings settings;
    for (const auto& k : settings.allKeys())
        in.settings.push_back({ss(k), ss(settings.value(k).toStringList().join(", ").isEmpty()
                                            ? settings.value(k).toString()
                                            : settings.value(k).toStringList().join(", "))});
    try {
        const auto r = fo4mx::writeSupportBundle(in, toPath(where));
        QMessageBox::information(this, "Support bundle",
                                 QString("Saved %1 (%2 files). Personal details are taken out; no mod files are "
                                         "included.%3")
                                     .arg(where)
                                     .arg(r.included.size())
                                     .arg(in.crashLog.empty() ? QString("\n\nNo crash log was found.") : QString()));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Support bundle", QString("Could not write it: ") + e.what());
    }
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
