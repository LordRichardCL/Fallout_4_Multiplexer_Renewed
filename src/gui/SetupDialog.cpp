#include "SetupDialog.hpp"

#include "AppController.hpp"
#include "Format.hpp"
#include "Theme.hpp"
#include "widgets/Widgets.hpp"

#include "util/Platform.hpp"
#include "vfs/GameInstall.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRadioButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace gui {

using fo4mx::InstallKind;

namespace {

const char* kShowAtStartupKey = "setup/showAtStartup";

// The checks the start-up screen runs, shared with shouldShowAtStartup().
struct SetupCheck {
    bool ok = false;
    QString error;
    fo4mx::ResolvedInstall install;
};

SetupCheck checkInstall(const fo4mx::InstallConfig& config)
{
    SetupCheck c;
    try {
        c.install = fo4mx::resolveInstall(config);
        bool hasGame = std::filesystem::exists(c.install.dataPath / "Fallout4.esm");
        bool hasPlugins = std::any_of(c.install.plugins.begin(), c.install.plugins.end(),
                                      [](const fo4mx::PluginListEntry& p) { return !p.implicit; });
        c.ok = hasGame && hasPlugins;
        if (!hasGame)
            c.error = "Fallout4.esm was not found in " + toQString(c.install.dataPath);
        else if (!hasPlugins)
            c.error = "The load order file lists no plugins";
    } catch (const std::exception& e) {
        c.error = QString::fromUtf8(e.what());
    }
    return c;
}

} // namespace

SetupDialog::SetupDialog(AppController* app, QWidget* parent) : QDialog(parent), app_(app)
{
    setWindowTitle("Fallout 4 Multiplexer Renewed — Setup");
    setWindowIcon(theme::appIcon());
    setMinimumSize(1040, 700);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---- Splash header -------------------------------------------------------
    auto* hero = new QFrame;
    hero->setObjectName("SetupHero");
    auto* hl = new QHBoxLayout(hero);
    hl->setContentsMargins(28, 22, 28, 22);
    hl->setSpacing(18);
    auto* logo = new QLabel;
    logo->setPixmap(theme::appIcon().pixmap(64, 64));
    hl->addWidget(logo);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel("Fallout 4 Multiplexer Renewed");
    title->setObjectName("SetupTitle");
    auto* tagline = new QLabel("Load order compiler · version " + QCoreApplication::applicationVersion());
    tagline->setObjectName("SetupTagline");
    auto* intro = new QLabel("Tell the Multiplexer where your game and mods are and which load order to use. "
                             "Nothing is written to your game or mods until you export.");
    intro->setObjectName("PageSubtitle");
    intro->setWordWrap(true);
    titles->addWidget(title);
    titles->addWidget(tagline);
    titles->addSpacing(6);
    titles->addWidget(intro);
    hl->addLayout(titles, 1);
    outer->addWidget(hero);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(28, 20, 28, 12);
    body->setSpacing(20);
    outer->addLayout(body, 1);

    // ---- Left: folders -----------------------------------------------------------
    auto* left = new QVBoxLayout;
    left->setSpacing(12);
    body->addLayout(left, 3);

    auto* step1 = new QGroupBox("1  How are your mods installed?");
    auto* s1 = new QVBoxLayout(step1);
    auto* kinds = new QHBoxLayout;
    mo2_ = new QRadioButton("Mod Organizer 2");
    vortex_ = new QRadioButton("Vortex");
    standalone_ = new QRadioButton("Manually / no mod manager");
    auto* group = new QButtonGroup(this);
    for (auto* r : {mo2_, vortex_, standalone_}) {
        group->addButton(r);
        kinds->addWidget(r);
    }
    kinds->addStretch();
    s1->addLayout(kinds);
    left->addWidget(step1);

    auto* step2 = new QGroupBox("2  Folders");
    auto* s2 = new QFormLayout(step2);
    s2->setLabelAlignment(Qt::AlignLeft);
    mo2Rows_ = new QWidget;
    auto* mo2Form = new QFormLayout(mo2Rows_);
    mo2Form->setContentsMargins(0, 0, 0, 0);
    auto* mo2Row = pathRow(mo2Path_, true, "Folder containing ModOrganizer.ini");
    auto* findMo2 = makeButton("Find");
    static_cast<QHBoxLayout*>(mo2Row->layout())->addWidget(findMo2);
    mo2Form->addRow("MO2 instance", mo2Row);
    profile_ = new QComboBox;
    profile_->setEditable(true);
    mo2Form->addRow("MO2 profile", profile_);
    s2->addRow(mo2Rows_);
    auto* gameRow = pathRow(gamePath_, true, "Folder containing Fallout4.exe");
    auto* findGame = makeButton("Find");
    static_cast<QHBoxLayout*>(gameRow->layout())->addWidget(findGame);
    s2->addRow("Fallout 4 folder", gameRow);
    s2->addRow("Output folder", pathRow(outputDir_, true, "Where Dummy_Plugin_XX.esp go (default: chosen automatically)"));
    left->addWidget(step2);

    auto* step3 = new QGroupBox("3  Load order");
    auto* s3 = new QVBoxLayout(step3);
    auto* lf = new QFormLayout;
    lf->addRow("plugins.txt", pathRow(pluginsFile_, false, "Leave empty to use the one your mod manager uses",
                                      "Load order (plugins.txt loadorder.txt *.txt)"));
    s3->addLayout(lf);
    pluginsDefault_ = new QLabel;
    pluginsDefault_->setObjectName("PageSubtitle");
    pluginsDefault_->setWordWrap(true);
    pluginsDefault_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    s3->addWidget(pluginsDefault_);
    left->addWidget(step3);

    auto* checks = new QGroupBox("Checks");
    auto* cl = new QVBoxLayout(checks);
    checkGame_ = new QLabel;
    checkManager_ = new QLabel;
    checkPlugins_ = new QLabel;
    for (auto* c : {checkGame_, checkManager_, checkPlugins_}) {
        c->setWordWrap(true);
        c->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cl->addWidget(c);
    }
    left->addWidget(checks);
    left->addStretch();

    // ---- Right: load order preview ----------------------------------------------
    auto* right = new QVBoxLayout;
    right->setSpacing(8);
    body->addLayout(right, 2);
    auto* previewTitle = new QLabel("Load order that will be used");
    previewTitle->setObjectName("SectionTitle");
    right->addWidget(previewTitle);
    summary_ = new QLabel;
    summary_->setObjectName("PageSubtitle");
    summary_->setWordWrap(true);
    right->addWidget(summary_);
    loadOrder_ = new QListWidget;
    loadOrder_->setSelectionMode(QAbstractItemView::NoSelection);
    loadOrder_->setUniformItemSizes(true);
    loadOrder_->setObjectName("LoadOrderPreview");
    right->addWidget(loadOrder_, 1);

    // ---- Footer -------------------------------------------------------------------
    auto* footer = new QFrame;
    footer->setObjectName("SetupFooter");
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(28, 12, 28, 14);
    showAtStartup_ = new QCheckBox("Show this screen at start-up");
    fl->addWidget(showAtStartup_);
    fl->addStretch();
    // At start-up this closes the application; opened from Settings it only closes the screen.
    auto* quit = makeButton(parent ? "Cancel" : "Quit");
    auto* skip = makeButton("Skip for Now");
    skip->setVisible(!parent);
    continue_ = makeButton("Save and Continue", "Primary");
    continue_->setDefault(true);
    fl->addWidget(quit);
    fl->addWidget(skip);
    fl->addWidget(continue_);
    outer->addWidget(footer);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(350);
    connect(debounce_, &QTimer::timeout, this, [this] { validateNow(); });

    connect(group, &QButtonGroup::buttonToggled, this, [this] { updateKind(); });
    for (auto* e : {mo2Path_, gamePath_, pluginsFile_})
        connect(e, &QLineEdit::textChanged, this, [this] { scheduleValidation(); });
    connect(mo2Path_, &QLineEdit::editingFinished, this, [this] { reloadProfiles(); });
    connect(profile_, &QComboBox::currentTextChanged, this, [this] { scheduleValidation(); });
    connect(findGame, &QPushButton::clicked, this, [this] { detectGame(); });
    connect(findMo2, &QPushButton::clicked, this, [this] { detectMo2(); });
    connect(quit, &QPushButton::clicked, this, &QDialog::reject);
    connect(skip, &QPushButton::clicked, this, [this] {
        QSettings().setValue(kShowAtStartupKey, showAtStartup_->isChecked());
        done(QDialog::Accepted);
    });
    connect(continue_, &QPushButton::clicked, this, [this] {
        save();
        accept();
    });

    load();
    validateNow();
}

bool SetupDialog::shouldShowAtStartup(AppController* app)
{
    if (QSettings().value(kShowAtStartupKey, true).toBool())
        return true;
    return !checkInstall(app->config().install).ok;
}

QWidget* SetupDialog::pathRow(QLineEdit*& edit, bool directory, const QString& placeholder, const QString& filter)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit;
    edit->setPlaceholderText(placeholder);
    edit->setMinimumWidth(320);
    auto* b = makeButton("Browse…");
    h->addWidget(edit, 1);
    h->addWidget(b);
    QLineEdit* target = edit;
    connect(b, &QPushButton::clicked, this, [this, target, directory, filter] {
        QString start = target->text().isEmpty() ? QString() : target->text();
        QString v = directory ? QFileDialog::getExistingDirectory(this, "Select folder", start)
                              : QFileDialog::getOpenFileName(this, "Select load order file", start, filter);
        if (v.isEmpty())
            return;
        target->setText(QDir::toNativeSeparators(v));
        if (target == mo2Path_)
            reloadProfiles();
    });
    return w;
}

fo4mx::InstallConfig SetupDialog::current() const
{
    fo4mx::InstallConfig in = app_->config().install;
    in.kind = mo2_->isChecked()      ? InstallKind::ModOrganizer2
              : vortex_->isChecked() ? InstallKind::Vortex
                                     : InstallKind::Standalone;
    in.mo2Path = toPath(mo2Path_->text().trimmed());
    in.mo2Profile = profile_->currentIndex() == 0 && profile_->currentText().startsWith("(")
                        ? std::string()
                        : ss(profile_->currentText().trimmed());
    in.gamePath = toPath(gamePath_->text().trimmed());
    in.pluginsFile = toPath(pluginsFile_->text().trimmed());
    return in;
}

void SetupDialog::load()
{
    const auto& c = app_->config();
    const auto& in = c.install;
    mo2_->setChecked(in.kind == InstallKind::ModOrganizer2);
    vortex_->setChecked(in.kind == InstallKind::Vortex);
    standalone_->setChecked(in.kind == InstallKind::Standalone);
    mo2Path_->setText(toQString(in.mo2Path));
    reloadProfiles();
    if (!in.mo2Profile.empty()) {
        if (profile_->findText(qs(in.mo2Profile)) < 0)
            profile_->addItem(qs(in.mo2Profile));
        profile_->setCurrentText(qs(in.mo2Profile));
    }
    gamePath_->setText(toQString(in.gamePath));
    pluginsFile_->setText(toQString(in.pluginsFile));
    outputDir_->setText(toQString(c.exportOptions.outputDirectory));
    showAtStartup_->setChecked(QSettings().value(kShowAtStartupKey, true).toBool());

    // First run: fill in what can be found automatically.
    if (in.gamePath.empty() && in.mo2Path.empty()) {
        if (auto p = fo4mx::platform::detectGamePath())
            gamePath_->setText(toQString(*p));
    }
    updateKind();
}

void SetupDialog::save()
{
    auto& c = app_->config();
    c.install = current();
    c.exportOptions.outputDirectory = toPath(outputDir_->text().trimmed());
    QSettings().setValue(kShowAtStartupKey, showAtStartup_->isChecked());
    app_->saveSettings();
}

void SetupDialog::updateKind()
{
    bool mo2 = mo2_->isChecked();
    mo2Rows_->setVisible(mo2);
    gamePath_->setPlaceholderText(mo2 ? "Optional: read from MO2 when empty" : "Folder containing Fallout4.exe");
    scheduleValidation();
}

void SetupDialog::reloadProfiles()
{
    QString current = profile_->currentText();
    QSignalBlocker block(profile_);
    profile_->clear();
    profile_->addItem("(selected in MO2)");
    QDir dir(mo2Path_->text().trimmed() + "/profiles");
    for (const auto& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        profile_->addItem(name);
    int idx = profile_->findText(current);
    profile_->setCurrentIndex(idx >= 0 ? idx : 0);
    scheduleValidation();
}

void SetupDialog::detectGame()
{
    if (auto p = fo4mx::platform::detectGamePath())
        gamePath_->setText(toQString(*p));
    else
        setCheck(checkGame_, false, "Fallout 4 was not found in the registry. Use Browse… to select its folder.");
}

void SetupDialog::detectMo2()
{
    // Global MO2 instances live in %LOCALAPPDATA%/ModOrganizer/<instance>.
    QDir root(toQString(fo4mx::platform::localAppDataDir() / "ModOrganizer"));
    for (const auto& name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile ini(root.filePath(name + "/ModOrganizer.ini"));
        if (!ini.open(QIODevice::ReadOnly))
            continue;
        if (QString::fromUtf8(ini.readAll()).contains("Fallout 4", Qt::CaseInsensitive)) {
            mo2Path_->setText(QDir::toNativeSeparators(root.filePath(name)));
            reloadProfiles();
            return;
        }
    }
    setCheck(checkManager_, false,
             "No Fallout 4 MO2 instance was found in %LOCALAPPDATA%\\ModOrganizer. "
             "For a portable install, select the folder that contains ModOrganizer.exe.");
}

void SetupDialog::scheduleValidation()
{
    debounce_->start();
}

void SetupDialog::setCheck(QLabel* label, bool ok, const QString& text)
{
    QColor c = ok ? theme::kGreen : theme::kRed;
    label->setText(QString("<span style='color:%1'>%2</span>&nbsp; %3")
                       .arg(c.name(), ok ? "✓" : "✖", text.toHtmlEscaped()));
}

void SetupDialog::validateNow()
{
    debounce_->stop();
    auto in = current();
    auto check = checkInstall(in);
    const auto& r = check.install;
    valid_ = check.ok;

    // Game
    if (!r.dataPath.empty() && std::filesystem::exists(r.dataPath / "Fallout4.esm")) {
        QString version = qs(r.gameVersion);
        setCheck(checkGame_, true,
                 "Fallout 4 found in " + toQString(r.gamePath) + (version.isEmpty() || version == "unknown" ? QString() : " (" + version + ")"));
    } else {
        setCheck(checkGame_, false, check.error.isEmpty() ? "Fallout 4 not found" : check.error);
    }

    // Mod manager
    switch (in.kind) {
    case InstallKind::ModOrganizer2:
        if (!r.profilePath.empty())
            setCheck(checkManager_, true,
                     QString("MO2 profile %1: %2 of %3 mods enabled")
                         .arg(toQString(r.profilePath.filename()))
                         .arg(r.enabledModCount())
                         .arg(r.mods.size()));
        else
            setCheck(checkManager_, false, check.error.isEmpty() ? "MO2 instance not found" : check.error);
        break;
    case InstallKind::Vortex:
        if (!r.vortexOwners.empty())
            setCheck(checkManager_, true,
                     QString("Vortex deployment found: %1 files attributed to mods").arg(formatCount(qint64(r.vortexOwners.size()))));
        else
            setCheck(checkManager_, false,
                     "vortex.deployment.json was not found in Data. Deploy your mods in Vortex first; "
                     "without it files cannot be attributed to mods.");
        break;
    case InstallKind::Standalone:
        setCheck(checkManager_, true, "No mod manager: mods are read directly from the Data folder");
        break;
    }

    // Load order
    loadOrder_->clear();
    int active = 0, inactive = 0, implicit = 0;
    for (const auto& p : r.plugins) {
        if (p.implicit)
            ++implicit;
        else
            (p.active ? active : inactive)++;
    }
    if (!in.pluginsFile.empty()) {
        pluginsDefault_->setText(std::filesystem::exists(in.pluginsFile) ? "Using your file " + toQString(in.pluginsFile)
                                                                         : "File not found: " + toQString(in.pluginsFile));
    } else if (!r.pluginsFile.empty()) {
        pluginsDefault_->setText("Using " + toQString(r.pluginsFile));
    } else {
        pluginsDefault_->setText("The load order file is chosen once the folders above are valid.");
    }
    if (active + inactive > 0)
        setCheck(checkPlugins_, true,
                 QString("%1 plugins listed, %2 active").arg(active + inactive).arg(active));
    else
        setCheck(checkPlugins_, false,
                 r.pluginsFile.empty() ? "No load order yet"
                                       : "No plugins found in " + toQString(r.pluginsFile) +
                                             ". Enable your mods in your mod manager, or select another plugins.txt.");
    for (const auto& w : r.warnings)
        checkPlugins_->setText(checkPlugins_->text() + "<br><span style='color:" + theme::kYellow.name() + "'>⚠</span>&nbsp; " +
                               qs(w).toHtmlEscaped());

    int position = 0;
    for (const auto& p : r.plugins) {
        bool on = p.active || p.implicit;
        QString text = QString("%1  %2").arg(on ? QString::number(++position).rightJustified(4) : QString("   –"), qs(p.name));
        if (p.implicit)
            text += "   (base game)";
        else if (!p.active)
            text += "   (inactive)";
        auto* item = new QListWidgetItem(text);
        item->setForeground(p.implicit ? theme::kMuted : p.active ? theme::kText : QColor(0x5a, 0x6a, 0x62));
        loadOrder_->addItem(item);
    }
    summary_->setText(r.plugins.empty()
                          ? "Nothing to show yet."
                          : QString("%1 base game and Creation Club plugins, %2 active and %3 inactive plugins. "
                                    "Inactive plugins are ignored.")
                                .arg(implicit)
                                .arg(active)
                                .arg(inactive));
    continue_->setEnabled(valid_);
}

} // namespace gui
