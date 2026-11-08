#include "pages/SettingsPage.hpp"

#include "Format.hpp"
#include "SetupDialog.hpp"
#include "widgets/Widgets.hpp"

#include "engine/VortexSupport.hpp"
#include "vfs/GameInstall.hpp"
#include "util/Platform.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace gui {

using fo4mx::InstallKind;

SettingsPage::SettingsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* l = new QVBoxLayout(body);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(12);

    auto* setupBtn = makeButton("Open Setup Screen");
    auto* saveBtn = makeButton("Save Settings", "Primary");
    l->addWidget(makePageHeader("Settings", "Where your game and mods are, and how analysis behaves.", {setupBtn, saveBtn}));

    auto* installBox = new QGroupBox("Installation");
    auto* il = new QVBoxLayout(installBox);
    auto* kinds = new QHBoxLayout;
    mo2_ = new QRadioButton("Mod Organizer 2");
    vortex_ = new QRadioButton("Vortex");
    standalone_ = new QRadioButton("Standalone");
    auto* group = new QButtonGroup(this);
    for (auto* r : {mo2_, vortex_, standalone_}) {
        group->addButton(r);
        kinds->addWidget(r);
    }
    kinds->addStretch();
    il->addLayout(kinds);

    mo2Rows_ = new QWidget;
    auto* mo2Form = new QFormLayout(mo2Rows_);
    mo2Form->setContentsMargins(0, 0, 0, 0);
    auto* mo2Row = pathRow(mo2Path_, true, "Folder containing ModOrganizer.ini");
    auto* findMo2 = makeButton("Find Instances");
    static_cast<QHBoxLayout*>(mo2Row->layout())->addWidget(findMo2);
    mo2Form->addRow("MO2 instance folder", mo2Row);
    connect(findMo2, &QPushButton::clicked, this, [this] {
        const auto found = fo4mx::findMo2Instances();
        if (found.empty()) {
            QMessageBox::information(this, "Mod Organizer 2 instances",
                                     "No global Mod Organizer 2 instance for Fallout 4 was found. A portable instance is "
                                     "the folder where you installed Mod Organizer 2: browse to the folder that holds "
                                     "ModOrganizer.ini.");
            return;
        }
        QStringList items;
        for (const auto& f : found)
            items << toQString(f);
        bool ok = false;
        // Never chosen for the user, even when there is only one.
        const QString pick = QInputDialog::getItem(this, "Mod Organizer 2 instances",
                                                   QString("%1 instance(s) for Fallout 4 found. Which one do you use?")
                                                       .arg(items.size()),
                                                   items, 0, false, &ok);
        if (ok && !pick.isEmpty()) {
            mo2Path_->setText(pick);
            reloadProfiles();
        }
    });
    profile_ = new QComboBox;
    profile_->setEditable(true);
    mo2Form->addRow("Profile", profile_);
    il->addWidget(mo2Rows_);

    // Vortex: where Apply to Game copies the package for Vortex to install.
    vortexRows_ = new QWidget;
    auto* vortexForm = new QVBoxLayout(vortexRows_);
    vortexForm->setContentsMargins(0, 0, 0, 0);
    auto* vf = new QFormLayout;
    vf->setContentsMargins(0, 0, 0, 0);
    auto* dlRow = pathRow(vortexDownloads_, true, "Vortex downloads folder for Fallout 4 (…\\downloads\\fallout4)");
    auto* suggest = makeButton("Use Vortex's Default");
    auto* checkDl = makeButton("Check");
    auto* clearDl = makeButton("Clear");
    for (auto* b : {suggest, checkDl, clearDl})
        static_cast<QHBoxLayout*>(dlRow->layout())->addWidget(b);
    vf->addRow("Vortex downloads folder", dlRow);
    vortexForm->addLayout(vf);
    auto* dlHelp = new QLabel(
        "Optional. When set, Apply to Game can copy Multiplexer Output.zip here, so it appears on Vortex's "
        "Downloads page ready to install (restart Vortex if it does not show). In Vortex: Settings → Downloads shows "
        "the folder; use its \"fallout4\" subfolder. Without it, drag the zip onto Vortex's Mods page instead.");
    dlHelp->setWordWrap(true);
    dlHelp->setStyleSheet("color: palette(placeholder-text);");
    vortexForm->addWidget(dlHelp);
    il->addWidget(vortexRows_);
    connect(suggest, &QPushButton::clicked, this, [this] {
        const auto folder = fo4mx::vortex::suggestedDownloadsFolder();
        if (folder.empty())
            QMessageBox::information(this, "Vortex downloads folder",
                                     "Vortex's default downloads folder for Fallout 4 was not found on this PC. Look "
                                     "it up in Vortex (Settings → Downloads) and browse to its \"fallout4\" folder.");
        else
            vortexDownloads_->setText(toQString(folder));
    });
    connect(clearDl, &QPushButton::clicked, this, [this] { vortexDownloads_->clear(); });
    connect(checkDl, &QPushButton::clicked, this, [this] {
        const auto c = fo4mx::vortex::checkDownloadsFolder(toPath(vortexDownloads_->text().trimmed()));
        QStringList lines;
        for (const auto& p : c.problems)
            lines << "\u2022 " + qs(p);
        for (const auto& n : c.notes)
            lines << "\u2022 " + qs(n);
        if (c.ok())
            QMessageBox::information(this, "Vortex downloads folder",
                                     lines.isEmpty() ? QString("The folder can be used.")
                                                     : "The folder can be used. Note:\n" + lines.join("\n"));
        else
            QMessageBox::warning(this, "Vortex downloads folder", "This folder cannot be used:\n" + lines.join("\n"));
    });

    auto* form = new QFormLayout;
    auto* gameRow = pathRow(gamePath_, true, "Fallout 4 folder (contains Fallout4.exe)");
    auto* detect = makeButton("Detect");
    static_cast<QHBoxLayout*>(gameRow->layout())->addWidget(detect);
    form->addRow("Game folder", gameRow);
    form->addRow("plugins.txt override", pathRow(pluginsFile_, false, "Optional: use this plugins.txt instead"));
    form->addRow("INI folder override", pathRow(iniDir_, true, "Optional: folder with Fallout4.ini / Fallout4Custom.ini"));
    language_ = new QComboBox;
    for (const char* lang : {"en", "fr", "de", "it", "es", "pl", "ru", "ptbr", "ja", "cn"})
        language_->addItem(lang);
    language_->setEditable(true);
    form->addRow("String table language", language_);
    runtime_ = new QComboBox;
    runtime_->addItem("Fallout 4 1.10.163 (Old Gen) — recommended", int(fo4mx::RuntimeProfile::OldGen_1_10_163));
    runtime_->addItem("Fallout 4 Next Gen (experimental)", int(fo4mx::RuntimeProfile::NextGen));
    form->addRow("Runtime profile", runtime_);
    il->addLayout(form);
    l->addWidget(installBox);

    auto* analysisBox = new QGroupBox("Analysis and compilation");
    auto* al = new QVBoxLayout(analysisBox);
    al->addWidget(new QLabel("Never compile these plugins (one per line):"));
    excluded_ = new QPlainTextEdit;
    excluded_->setMaximumHeight(130);
    al->addWidget(excluded_);
    creationClub_ = new QCheckBox("Allow compiling Creation Club plugins");
    allTypes_ = new QCheckBox("Show field differences for record types that are not merged (slower, display only)");
    al->addWidget(creationClub_);
    al->addWidget(allTypes_);
    l->addWidget(analysisBox);

    auto* diagBox = new QGroupBox("Diagnostics");
    auto* dl = new QVBoxLayout(diagBox);
    recorder_ = new QCheckBox("Prepare the Diagnostic Recorder (optional F4SE plugin) with each export");
    dl->addWidget(recorder_);
    auto* diagNote = new QLabel(
        "MultiplexerDiagnosticRecorder.dll records a small, fixed set of events while you play (versions, F4SE's "
        "start-up, game data, new game, load and save messages, the active plugins, which F4SE plugins loaded, and "
        "whether the game exited normally) so the Diagnostics page can point to likely causes. It changes no "
        "plugin, save or game data and installs no hooks. Export puts it, with a zip and install notes, in "
        "\"Multiplexer Output\\Diagnostic Recorder\" (beside the package, never inside it); install that zip as a mod "
        "to use it. Its logs: Documents\\My Games\\Fallout4\\F4SE\\MultiplexerDiagnosticRecorder.");
    diagNote->setWordWrap(true);
    diagNote->setObjectName("PageSubtitle");
    dl->addWidget(diagNote);
    l->addWidget(diagBox);

    auto* loaderBox = new QGroupBox("Archives");
    auto* ll = new QVBoxLayout(loaderBox);
    archiveLoader_ = new QCheckBox("Load compiled plugins' archives with the F4SE archive loader (requires F4SE)");
    ll->addWidget(archiveLoader_);
    auto* loaderNote = new QLabel(
        "Fallout 4 reads only the first 1,023 characters of the archive list in Fallout4Custom.ini, so without this "
        "plugins whose archives do not fit stay in the load order. With it, export ships MultiplexerArchiveLoader.dll "
        "and the full list in the package (Data\\F4SE\\Plugins); the loader gives the game the whole list when it "
        "starts. The game must then always be started through F4SE. Fallout 4 1.10.163 only. Its log: "
        "Documents\\My Games\\Fallout4\\F4SE\\MultiplexerArchiveLoader.log.");
    loaderNote->setWordWrap(true);
    loaderNote->setObjectName("PageSubtitle");
    ll->addWidget(loaderNote);
    l->addWidget(loaderBox);

    auto* advBox = new QGroupBox("Advanced");
    auto* af = new QFormLayout(advBox);
    threads_ = new QSpinBox;
    threads_->setRange(0, 64);
    threads_->setSpecialValueText("Automatic");
    af->addRow("Worker threads", threads_);
    af->addRow("Results database", pathRow(database_, false, "SQLite file"));
    af->addRow("Report folder", pathRow(reports_, true, "Default: next to the output plugins"));
    auto* logs = new QLabel(QString("Logs: %1").arg(toQString(fo4mx::platform::applicationDataDir() / "logs")));
    logs->setObjectName("PageSubtitle");
    logs->setTextInteractionFlags(Qt::TextSelectableByMouse);
    af->addRow(logs);
    l->addWidget(advBox);

    auto* resetBox = new QGroupBox("Start fresh");
    auto* rl = new QVBoxLayout(resetBox);
    auto* resetNote = new QLabel(
        "Forget everything this app has stored: settings, folders, exclusions, conflict resolutions, the results "
        "database, the build cache and the logs. Use it after installing a new version, or when results look stale. "
        "Generated plugins and reports in your output folder are not deleted.");
    resetNote->setObjectName("PageSubtitle");
    resetNote->setWordWrap(true);
    rl->addWidget(resetNote);
    auto* reset = makeButton("Reset to Fresh State…", "Danger");
    auto* resetRow = new QHBoxLayout;
    resetRow->addWidget(reset);
    resetRow->addStretch();
    rl->addLayout(resetRow);
    l->addWidget(resetBox);

    status_ = new QLabel;
    status_->setObjectName("PageSubtitle");
    l->addWidget(status_);
    l->addStretch();

    connect(saveBtn, &QPushButton::clicked, this, [this] { save(); });
    connect(setupBtn, &QPushButton::clicked, this, [this] {
        SetupDialog setup(app_, this);
        if (setup.exec() == QDialog::Accepted)
            status_->setText("Setup saved. Run Scan to apply installation changes.");
    });
    connect(app_, &AppController::settingsChanged, this, [this] { load(); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (app_->busy()) {
            status_->setText("Wait for the current task to finish first.");
            return;
        }
        auto answer = QMessageBox::warning(
            this, "Reset to fresh state",
            "This deletes all settings, the results database, the build cache and the logs, as if the app had just "
            "been installed.\n\nYour mods, your load order and the generated plugins in the output folder are not "
            "touched.\n\nContinue?",
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        QStringList removed = app_->resetToFreshState();
        QMessageBox::information(this, "Reset to fresh state",
                                 "Removed:\n" + removed.join("\n") + "\n\nThe Setup screen opens next.");
        SetupDialog setup(app_, this);
        setup.exec();
        load();
        status_->setText("Reset complete. Run Scan to start again.");
    });
    connect(group, &QButtonGroup::buttonToggled, this, [this] { updateVisibility(); });
    connect(mo2Path_, &QLineEdit::editingFinished, this, [this] { reloadProfiles(); });
    connect(detect, &QPushButton::clicked, this, [this] {
        if (auto p = fo4mx::platform::detectGamePath())
            gamePath_->setText(toQString(*p));
        else
            status_->setText("Fallout 4 was not found in the registry; please browse to it.");
    });
    load();
}

QWidget* SettingsPage::pathRow(QLineEdit*& edit, bool directory, const QString& placeholder)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit;
    edit->setPlaceholderText(placeholder);
    auto* b = makeButton("Browse…");
    h->addWidget(edit, 1);
    h->addWidget(b);
    QLineEdit* target = edit;
    connect(b, &QPushButton::clicked, this, [this, target, directory] {
        QString v = directory ? QFileDialog::getExistingDirectory(this, "Select folder", target->text())
                              : QFileDialog::getSaveFileName(this, "Select file", target->text(), QString(), nullptr,
                                                             QFileDialog::DontConfirmOverwrite);
        if (!v.isEmpty()) {
            target->setText(QDir::toNativeSeparators(v));
            if (target == mo2Path_)
                reloadProfiles();
        }
    });
    return w;
}

void SettingsPage::reloadProfiles()
{
    QString current = profile_->currentText();
    profile_->clear();
    QDir dir(mo2Path_->text() + "/profiles");
    for (const auto& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        profile_->addItem(name);
    profile_->insertItem(0, "(selected in MO2)");
    int idx = profile_->findText(current);
    profile_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void SettingsPage::updateVisibility()
{
    mo2Rows_->setVisible(mo2_->isChecked());
    vortexRows_->setVisible(vortex_->isChecked());
}

void SettingsPage::refresh() {}

void SettingsPage::load()
{
    const auto& c = app_->config();
    const auto& in = c.install;
    mo2_->setChecked(in.kind == InstallKind::ModOrganizer2);
    vortex_->setChecked(in.kind == InstallKind::Vortex);
    standalone_->setChecked(in.kind == InstallKind::Standalone);
    mo2Path_->setText(toQString(in.mo2Path));
    reloadProfiles();
    if (!in.mo2Profile.empty()) {
        int idx = profile_->findText(qs(in.mo2Profile));
        if (idx < 0)
            profile_->addItem(qs(in.mo2Profile));
        profile_->setCurrentText(qs(in.mo2Profile));
    }
    gamePath_->setText(toQString(in.gamePath));
    pluginsFile_->setText(toQString(in.pluginsFile));
    iniDir_->setText(toQString(in.iniDirectory));
    vortexDownloads_->setText(toQString(in.vortexDownloads));
    language_->setCurrentText(qs(in.language));
    runtime_->setCurrentIndex(runtime_->findData(int(in.runtime)));
    excluded_->setPlainText(app_->excludedPlugins().join("\n"));
    creationClub_->setChecked(c.analysis.compileCreationClub);
    recorder_->setChecked(c.diagnosticRecorder);
    archiveLoader_->setChecked(c.f4seArchiveLoader);
    allTypes_->setChecked(c.conflicts.analyseAllRecordTypes);
    threads_->setValue(static_cast<int>(c.scan.threads));
    database_->setText(toQString(c.databasePath));
    reports_->setText(toQString(c.reportDirectory));
    updateVisibility();
}

void SettingsPage::save()
{
    auto& c = app_->config();
    auto& in = c.install;
    in.kind = mo2_->isChecked() ? InstallKind::ModOrganizer2 : vortex_->isChecked() ? InstallKind::Vortex : InstallKind::Standalone;
    in.mo2Path = toPath(mo2Path_->text().trimmed());
    in.mo2Profile = profile_->currentIndex() == 0 && profile_->currentText().startsWith("(") ? std::string()
                                                                                              : ss(profile_->currentText());
    in.gamePath = toPath(gamePath_->text().trimmed());
    in.pluginsFile = toPath(pluginsFile_->text().trimmed());
    in.iniDirectory = toPath(iniDir_->text().trimmed());
    in.vortexDownloads = toPath(vortexDownloads_->text().trimmed());
    in.language = ss(language_->currentText().trimmed());
    in.runtime = static_cast<fo4mx::RuntimeProfile>(runtime_->currentData().toInt());
    c.analysis.excludedPlugins.clear();
    for (const auto& line : excluded_->toPlainText().split('\n', Qt::SkipEmptyParts))
        if (!line.trimmed().isEmpty())
            c.analysis.excludedPlugins.push_back(ss(line.trimmed()));
    c.analysis.compileCreationClub = creationClub_->isChecked();
    c.diagnosticRecorder = recorder_->isChecked();
    c.f4seArchiveLoader = archiveLoader_->isChecked();
    if (app_->pipeline()) {
        app_->pipeline()->config().diagnosticRecorder = c.diagnosticRecorder;
        app_->pipeline()->config().f4seArchiveLoader = c.f4seArchiveLoader;
    }
    c.conflicts.analyseAllRecordTypes = allTypes_->isChecked();
    c.scan.threads = static_cast<unsigned>(threads_->value());
    if (!database_->text().trimmed().isEmpty())
        c.databasePath = toPath(database_->text().trimmed());
    c.reportDirectory = toPath(reports_->text().trimmed());
    app_->saveSettings();
    app_->reopenDatabase();
    status_->setText("Settings saved. Run Scan to apply installation changes.");
}

} // namespace gui
