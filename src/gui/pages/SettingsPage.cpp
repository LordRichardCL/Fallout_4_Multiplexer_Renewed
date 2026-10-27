#include "pages/SettingsPage.hpp"

#include "Format.hpp"
#include "SetupDialog.hpp"
#include "widgets/Widgets.hpp"

#include "util/Platform.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
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
    mo2Form->addRow("MO2 instance folder", pathRow(mo2Path_, true, "Folder containing ModOrganizer.ini"));
    profile_ = new QComboBox;
    profile_->setEditable(true);
    mo2Form->addRow("Profile", profile_);
    il->addWidget(mo2Rows_);

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
    allTypes_ = new QCheckBox("Field-level conflict analysis for every record type (slower, more detail)");
    al->addWidget(creationClub_);
    al->addWidget(allTypes_);
    l->addWidget(analysisBox);

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
    language_->setCurrentText(qs(in.language));
    runtime_->setCurrentIndex(runtime_->findData(int(in.runtime)));
    excluded_->setPlainText(app_->excludedPlugins().join("\n"));
    creationClub_->setChecked(c.analysis.compileCreationClub);
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
    in.language = ss(language_->currentText().trimmed());
    in.runtime = static_cast<fo4mx::RuntimeProfile>(runtime_->currentData().toInt());
    c.analysis.excludedPlugins.clear();
    for (const auto& line : excluded_->toPlainText().split('\n', Qt::SkipEmptyParts))
        if (!line.trimmed().isEmpty())
            c.analysis.excludedPlugins.push_back(ss(line.trimmed()));
    c.analysis.compileCreationClub = creationClub_->isChecked();
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
