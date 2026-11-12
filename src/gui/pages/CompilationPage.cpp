#include "pages/CompilationPage.hpp"

#include "Format.hpp"
#include "models/GroupedSqlModel.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include "engine/Exporter.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QSqlQuery>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTreeView>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

using fo4mx::AllocationStrategy;

CompilationPage::CompilationPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    compile_ = makeButton("Compile", "Primary");
    export_ = makeButton("Export Plugins");
    apply_ = makeButton("Apply to Game…");
    apply_->setToolTip("Mod Organizer 2: switch the profile's load order to the output plugins. All installs: add "
                       "compiled plugins' archives to Fallout4Custom.ini so their assets keep loading.");
    openFolder_ = makeButton("Open Output Folder");
    l->addWidget(makePageHeader("Multiplexer Compilation",
                                "Scan → Analyse → Resolve → Merge → Rewrite → Compile → Validate → Export. Eligible "
                                "plugins are compiled into a small set of fully materialised output plugins.",
                                {compile_, export_, apply_, openFolder_}));

    stages_ = new StageIndicator({"Scan", "Analyse", "Resolve", "Merge", "Rewrite", "Compile", "Validate", "Export"});
    auto* stageFrame = new QFrame;
    stageFrame->setObjectName("Card");
    auto* sfl = new QVBoxLayout(stageFrame);
    sfl->addWidget(stages_);
    l->addWidget(stageFrame);

    auto* split = new QSplitter(Qt::Horizontal);

    // Options
    auto* optionsScroll = new QScrollArea;
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    auto* options = new QWidget;
    optionsScroll->setWidget(options);
    auto* ol = new QVBoxLayout(options);
    ol->setContentsMargins(0, 0, 8, 0);

    auto* outBox = new QGroupBox("Output");
    auto* form = new QFormLayout(outBox);
    outputs_ = new QSpinBox;
    outputs_->setRange(fo4mx::CompileOptions::kMinOutputs, fo4mx::CompileOptions::kMaxOutputs);
    prefix_ = new QLineEdit;
    outputDir_ = new QLineEdit;
    outputDir_->setPlaceholderText("Default: <game folder>\\Multiplexer Output (each export in a folder of its own)");
    auto* browse = makeButton("…");
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(outputDir_, 1);
    dirRow->addWidget(browse);
    autoOutputs_ = new QCheckBox("Automatic");
    autoOutputs_->setToolTip("Use the fewest output plugins whose master lists all fit (Fallout 4 allows at most 254 "
                             "masters per plugin), from 1 up to the maximum. Fewer outputs take fewer load order "
                             "slots; each one is larger. The number used is shown after Compile.");
    auto* outputsRow = new QHBoxLayout;
    outputsRow->addWidget(outputs_);
    outputsRow->addWidget(autoOutputs_);
    outputsRow->addStretch();
    connect(autoOutputs_, &QCheckBox::toggled, outputs_, [this](bool on) { outputs_->setEnabled(!on); });
    form->addRow("Output plugins", outputsRow);
    form->addRow("Name prefix", prefix_);
    form->addRow("Output folder", dirRow);
    outputs_->setToolTip(QString(
        "The most output plugins the compiler may create (%1 to %2). It fills them in order and starts a new one only "
        "when the current one holds about its share of the records, so a small load order uses fewer.\n\n"
        "Each output that is not ESL-flagged takes one of Fallout 4's 254 full plugin slots, shared with the plugins "
        "that stay enabled; validation stops the export if the total would pass 254. More outputs also leave fewer "
        "masters for the worldspace output (254 minus this number).")
                             .arg(fo4mx::CompileOptions::kMinOutputs)
                             .arg(fo4mx::CompileOptions::kMaxOutputs));
    prefix_->setToolTip("Output plugins are named <prefix>01.esp, <prefix>02.esp, …");
    outputDir_->setToolTip(
        "The folder that holds the exports. By default \"Multiplexer Output\" next to the game. Every export gets "
        "a folder of its own in it, \"Multiplexer Output DDMMYY HHMMSS\": its Data folder holds everything the game "
        "loads, and \"Multiplexer Output.zip\" beside it is the package for your mod manager. An export never "
        "changes an earlier one; \"Multiplexer Latest.txt\" names the latest.\n\n"
        "Advanced: a folder inside Mod Organizer 2's mods folder makes every export a mod of its own there; then "
        "compiled plugins that have archives are replaced by archive loaders instead of loose copies and INI entries.");
    outputsNote_ = new QLabel;
    outputsNote_->setObjectName("PageSubtitle");
    outputsNote_->setWordWrap(true);
    form->addRow(outputsNote_);
    auto updateOutputsNote = [this] {
        const QString count = autoOutputs_->isChecked()
                                  ? QString("the fewest output plugins that fit")
                                  : QString("up to %1 output plugins").arg(outputs_->value());
        outputsNote_->setText(
            "Compiles eligible plugins into " + count + ", plus Multiplexer_Patch.esp when the patch is on. "
            "The output folder also receives the files the game finds by plugin name and FormID (voice files, FaceGen, "
            "start-game quest lists in seq\\), loose copies that keep archive winners, and the Auto-Merger's relinked "
            "scripts and configs. The Multiplexer_*.tsv files in Data are the build's records for the next build; the "
            "lists, INI line and reports next to Data are notes for you. The game reads none of them.");
    };
    connect(outputs_, &QSpinBox::valueChanged, this, updateOutputsNote);
    connect(autoOutputs_, &QCheckBox::toggled, this, updateOutputsNote);
    updateOutputsNote();
    installNote_ = new QLabel;
    installNote_->setObjectName("PageSubtitle");
    installNote_->setWordWrap(true);
    installNote_->setTextFormat(Qt::RichText);
    form->addRow(installNote_);
    ol->addWidget(outBox);

    auto* allocBox = new QGroupBox("Record allocation");
    auto* al = new QVBoxLayout(allocBox);
    strategy_ = new QComboBox;
    strategy_->addItem("Balanced size (recommended)", int(AllocationStrategy::Balanced));
    strategy_->addItem("Grouped by record type", int(AllocationStrategy::ByRecordType));
    strategy_->addItem("Balanced, with my rules", int(AllocationStrategy::UserDefined));
    strategy_->setItemData(0, "Fills the outputs one after another to about the same size, in dependency order.",
                           Qt::ToolTipRole);
    strategy_->setItemData(1, "Gives each group of record types (items, NPCs, quests, world, …) its own outputs.",
                           Qt::ToolTipRole);
    strategy_->setItemData(2, "Balanced size, then your rules send record types or mods to the output you name.",
                           Qt::ToolTipRole);
    al->addWidget(strategy_);
    strategyNote_ = new QLabel;
    strategyNote_->setObjectName("PageSubtitle");
    strategyNote_->setWordWrap(true);
    al->addWidget(strategyNote_);
    rules_ = new QPlainTextEdit;
    rules_->setPlaceholderText("One rule per line: PATTERN=OUTPUT\nWEAP=3\nMyWeaponPack.esp=5");
    rules_->setMaximumHeight(110);
    al->addWidget(rules_);
    auto* note = new QLabel("Whatever the strategy: a record always goes into the same or a later output than the "
                            "new records it references; cells, worldspaces and quests with dialogue go together into "
                            "the last outputs; and records keep the output they had in the previous build when its "
                            "FormID map is in the output folder. The strategy changes how records are spread, not "
                            "what the game sees.");
    note->setObjectName("PageSubtitle");
    note->setWordWrap(true);
    al->addWidget(note);
    ol->addWidget(allocBox);

    auto* mergeBox = new QGroupBox("Resolution");
    auto* ml = new QVBoxLayout(mergeBox);
    merge_ = new QCheckBox("Merge compatible edits (keyword lists, leveled lists, form lists, different fields)");
    light_ = new QCheckBox("ESL-flag outputs that fit the light plugin range (≤ 2048 new records)");
    merge_->setToolTip("Where two or more mods change the same record in different ways (a leveled list, keywords, "
                       "different fields), combine their changes instead of keeping only the last mod's version.");
    light_->setToolTip("An output with few enough new records (2,048 or fewer) is ESL-flagged, so it takes no full "
                       "plugin slot. Outputs with new cells or worldspaces are never ESL-flagged.");
    translatePrevis_ = new QCheckBox("Translate previs files into the output's numbering");
    translatePrevis_->setToolTip(
        "A cell written to an output wins in game, so its previs file (.uvd) is read in the output's numbering. "
        "On: a translated copy is written loose. Off: the original file is used. Either way the same plugins "
        "compile: this setting only decides whether translated copies are written. The checks still run, and a "
        "previs file that cannot be translated keeps its cell's plugins out. Turn off only to compare the two in "
        "game.");
    for (auto* c : {merge_, light_, translatePrevis_}) {
        c->setStyleSheet("QCheckBox { spacing: 8px; }");
        ml->addWidget(c);
    }
    ol->addWidget(mergeBox);

    auto* patchBox = new QGroupBox("Conflict resolution patch");
    auto* pl = new QVBoxLayout(patchBox);
    patch_ = new QCheckBox("Build Multiplexer_Patch.esp for plugins that stay in the load order");
    patchOnly_ = new QCheckBox("Patch only: compile no plugins");
    for (auto* c : {patch_, patchOnly_}) {
        c->setStyleSheet("QCheckBox { spacing: 8px; }");
        pl->addWidget(c);
    }
    auto* patchNote = new QLabel("Like a Bashed Patch: where last-plugin-wins would drop edits (leveled lists, form "
                                 "lists, keywords, different fields), the patch carries the merged record. It only "
                                 "overrides existing records, so it is always ESL-flagged and adds no FormIDs. "
                                 "It must load last.");
    patchNote->setObjectName("PageSubtitle");
    patchNote->setWordWrap(true);
    pl->addWidget(patchNote);
    ol->addWidget(patchBox);
    ol->addStretch();
    split->addWidget(optionsScroll);

    // Results
    auto* results = new QWidget;
    auto* rl = new QVBoxLayout(results);
    rl->setContentsMargins(8, 0, 0, 0);
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setObjectName("PageSubtitle");
    rl->addWidget(summary_);
    tabs_ = new QTabWidget;
    outputsView_ = new QTableView;
    outputsModel_ = new SqlModel(this);
    styleTable(outputsView_);
    outputsView_->setModel(outputsModel_);
    // The detailed tabs are grouped by plugin: collapsed groups, each expanded and sorted on its own.
    recordsView_ = new QTreeView;
    mappingsView_ = new QTreeView;
    assetsView_ = new QTreeView;
    recordsModel_ = new GroupedSqlModel(this);
    mappingsModel_ = new GroupedSqlModel(this);
    assetsModel_ = new GroupedSqlModel(this);
    styleGroupedTree(recordsView_, recordsModel_);
    styleGroupedTree(mappingsView_, mappingsModel_);
    styleGroupedTree(assetsView_, assetsModel_);
    recordsView_->setToolTip("Grouped by output plugin. Expand a plugin to see its records; click a column header to "
                             "sort the records inside each group.");
    mappingsView_->setToolTip("Grouped by the plugin the records came from. Expand a plugin to see where each of its "
                              "records went; click a column header to sort inside each group.");
    assetsView_->setToolTip("Grouped by output plugin. Expand a plugin to see the files its records use; click a "
                            "column header to sort inside each group.");
    tabs_->addTab(outputsView_, "Output Plugins");
    tabs_->addTab(recordsView_, "Generated Records");
    tabs_->addTab(mappingsView_, "FormID Mappings");
    tabs_->addTab(assetsView_, "Assets Used");
    auto* resultSplit = new QSplitter(Qt::Vertical);
    resultSplit->addWidget(tabs_);
    trace_ = new QTextBrowser;
    trace_->setHtml("<p style='color:#8aa196'>Select a generated record to trace it back to its source plugins and the "
                    "original files its meshes, materials and textures load from. Compiled plugins contain records "
                    "only; assets stay in their original mods and archives.</p>");
    resultSplit->addWidget(trace_);
    resultSplit->setStretchFactor(0, 2);
    resultSplit->setStretchFactor(1, 3);
    rl->addWidget(resultSplit, 1);
    connect(recordsView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showTrace(i); });
    split->addWidget(results);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    l->addWidget(split, 1);

    connect(browse, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Output folder", outputDir_->text());
        if (!d.isEmpty())
            outputDir_->setText(d);
    });
    connect(compile_, &QPushButton::clicked, this, [this] {
        storeOptions();
        app_->runCompile();
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        storeOptions();
        app_->runExport();
    });
    connect(apply_, &QPushButton::clicked, this, [this] {
        // First what will happen (nothing changes yet), then the user's confirmation.
        AppController::ApplyPreview preview;
        QString msg;
        if (!app_->previewApply(&preview, &msg)) {
            QMessageBox::warning(this, "Apply to Game", msg);
            return;
        }
        QDialog dlg(this);
        dlg.setWindowTitle("Apply to Game");
        dlg.resize(780, 600);
        auto* v = new QVBoxLayout(&dlg);
        auto* intro = new QLabel(preview.ready
                                     ? "Review what will change. Every file is backed up first, and if any step "
                                       "fails everything already done is undone."
                                     : "<b>Your load order cannot be switched over yet</b> (see below)." +
                                           QString(preview.vortex || preview.mo2
                                                       ? " You can still put the package where your mod manager "
                                                         "installs it from."
                                                       : ""));
        intro->setWordWrap(true);
        v->addWidget(intro);

        // Vortex / Mod Organizer 2: the package first.
        QCheckBox* copyZip = nullptr;
        QCheckBox* enableMod = nullptr;
        if (preview.vortex || preview.mo2) {
            const QString manager = preview.vortex ? "Vortex" : "Mod Organizer 2";
            auto* box = new QGroupBox("1. The package in " + manager);
            auto* bl = new QVBoxLayout(box);
            if (preview.mo2 && !preview.outputMod.isEmpty()) {
                // The output folder is itself a mod in MO2's mods folder: no zip needed.
                if (preview.outputModEnabled) {
                    bl->addWidget(new QLabel("The output folder is the mod \"" + preview.outputMod +
                                             "\", enabled in this profile."));
                } else {
                    enableMod = new QCheckBox("Enable the mod \"" + preview.outputMod +
                                              "\" (your output folder) in this profile");
                    enableMod->setChecked(true);
                    bl->addWidget(enableMod);
                    auto* note = new QLabel("A new entry goes to the top of the left pane (highest priority), so it "
                                            "wins every file conflict. Every other mod keeps its place and state.");
                    note->setWordWrap(true);
                    note->setStyleSheet("color: palette(placeholder-text);");
                    bl->addWidget(note);
                }
            } else if (!preview.zipAvailable) {
                bl->addWidget(new QLabel("This export did not write Multiplexer Output.zip. Export again."));
            } else if (preview.downloadsFolder.isEmpty() || !preview.downloadsProblem.isEmpty()) {
                auto* how = new QLabel(
                    (preview.downloadsFolder.isEmpty()
                         ? (preview.vortex ? QString("Your Vortex downloads folder is not set (Settings → Installation). ")
                                           : QString("Mod Organizer 2's downloads folder is not known. "))
                         : manager + "'s downloads folder cannot be used: " + preview.downloadsProblem + ". ") +
                    (preview.vortex ? "Drag the zip onto Vortex's Mods page instead:\n"
                                    : "Use Install a new mod from an archive in Mod Organizer 2 instead:\n") +
                    preview.zipPath);
                how->setWordWrap(true);
                how->setTextInteractionFlags(Qt::TextSelectableByMouse);
                bl->addWidget(how);
            } else {
                copyZip = new QCheckBox("Copy Multiplexer Output.zip to " + manager + "'s downloads folder");
                copyZip->setChecked(true);
                bl->addWidget(copyZip);
                auto* where = new QLabel(preview.downloadsFolder +
                                         (preview.zipInDownloads ? "\nAn archive of that name is already there: you will "
                                                                   "be asked whether to replace it or keep both."
                                                                 : QString()) +
                                         (preview.downloadsNote.isEmpty() ? QString() : "\nNote: " + preview.downloadsNote));
                where->setWordWrap(true);
                where->setStyleSheet("color: palette(placeholder-text);");
                bl->addWidget(where);
            }
            auto* steps = new QLabel(
                preview.vortex
                    ? "In Vortex: install and enable the package from the Downloads page (restart Vortex if it is not "
                      "listed), let it win every file conflict and Deploy. Then close Vortex and press Apply to Game "
                      "again to switch your load order over."
                    : "In Mod Organizer 2: install the package from the Downloads tab (or Install a new mod from an "
                      "archive), enable it and keep it at the bottom of the left pane. Then close Mod Organizer 2 and "
                      "press Apply to Game again to switch your load order over. Your mods stay enabled.");
            steps->setWordWrap(true);
            bl->addWidget(steps);
            if (preview.managerRunning) {
                auto* running = new QLabel("<b>" + manager + " is running.</b> Close it before switching your load "
                                           "order over.");
                running->setWordWrap(true);
                bl->addWidget(running);
            }
            v->addWidget(box);
            v->addWidget(new QLabel("<b>2. Switching your load order over</b>"));
        }

        auto* text = new QPlainTextEdit(preview.summary);
        text->setReadOnly(true);
        v->addWidget(text, 1);
        QCheckBox* ghost = nullptr;
        if (preview.offerGhosting) {
            ghost = new QCheckBox(QString("Also ghost the %1 compiled plugin(s) that can be (rename Example.esp to "
                                          "Example.esp.ghost)")
                                      .arg(preview.ghostable));
            v->addWidget(ghost);
            auto* help = new QLabel(
                "Optional. Disabling the plugins in plugins.txt is enough; ghosting also hides them from the game "
                "and from tools that might enable them again. Undo it by renaming the file back. Multiplexer still "
                "reads ghosted plugins for the next build." +
                QString(preview.vortex ? " Plugins Vortex deployed are never ghosted (Vortex would restore them)." : ""));
            help->setWordWrap(true);
            help->setStyleSheet("color: palette(placeholder-text);");
            v->addWidget(help);
            if (preview.ghostRefused > 0) {
                v->addWidget(new QLabel(QString("%1 plugin(s) will not be ghosted:").arg(preview.ghostRefused)));
                auto* details = new QPlainTextEdit(preview.ghostDetails);
                details->setReadOnly(true);
                details->setMaximumHeight(110);
                v->addWidget(details);
            }
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto* go = buttons->addButton("Apply", QDialogButtonBox::AcceptRole);
        auto updateGo = [&] { go->setEnabled(preview.ready || (copyZip && copyZip->isChecked())); };
        if (enableMod)
            connect(enableMod, &QCheckBox::toggled, &dlg, updateGo);
        updateGo();
        if (copyZip)
            connect(copyZip, &QCheckBox::toggled, &dlg, updateGo);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted)
            return;

        AppController::ApplyChoice choice;
        choice.ghost = ghost && ghost->isChecked();
        choice.copyZip = copyZip && copyZip->isChecked();
        choice.enableOutputMod = enableMod && enableMod->isChecked();
        if (choice.copyZip && preview.zipInDownloads) {
            QMessageBox ask(QMessageBox::Question, "Multiplexer Output.zip is already there",
                            "An archive named Multiplexer Output.zip is already in the downloads folder.\n\n"
                            "Replace it (the old one is backed up first), or keep both (the new one gets a "
                            "numbered name)?",
                            QMessageBox::NoButton, this);
            auto* replace = ask.addButton("Replace", QMessageBox::AcceptRole);
            auto* both = ask.addButton("Keep Both", QMessageBox::AcceptRole);
            ask.addButton(QMessageBox::Cancel);
            ask.exec();
            if (ask.clickedButton() == replace)
                choice.replaceZip = true;
            else if (ask.clickedButton() != both)
                return;
        }
        bool ok = app_->applyToProfile(choice, &msg);
        if (ok)
            QMessageBox::information(this, "Apply to Game", msg);
        else
            QMessageBox::warning(this, "Apply to Game", msg);
    });
    connect(openFolder_, &QPushButton::clicked, this, [this] {
        auto* p = app_->pipeline();
        if (p && p->exportResult())
            QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(p->exportResult()->packageRoot.empty()
                                                                        ? p->exportResult()->directory
                                                                        : p->exportResult()->packageRoot)));
    });
    connect(app_, &AppController::taskStarted, this, [this](const QString& task) {
        if (task != "Check")
            stages_->reset();
    });
    connect(app_, &AppController::stageStarted, stages_, &StageIndicator::activate);
    connect(app_, &AppController::taskFinished, this, [this](const QString& task, bool ok, const QString&) {
        if (task != "Check")
            stages_->finish(ok);
        updateButtons();
    });
    connect(app_, &AppController::busyChanged, this, [this] { updateButtons(); });
    connect(app_, &AppController::settingsChanged, this, [this] { loadOptions(); });
    connect(patchOnly_, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            patch_->setChecked(true);
        patch_->setEnabled(!on);
    });
    connect(strategy_, &QComboBox::currentIndexChanged, this, [this] {
        rules_->setEnabled(strategy_->currentData().toInt() == int(AllocationStrategy::UserDefined));
        updateStrategyNote();
    });
    loadOptions();
}

void CompilationPage::updateStrategyNote()
{
    switch (static_cast<AllocationStrategy>(strategy_->currentData().toInt())) {
    case AllocationStrategy::Balanced:
        strategyNote_->setText(
            "Outputs are filled one after another, each with about the same amount of data (at least 2 MB), keeping "
            "records that reference each other together. Fewest cross-output masters and the most even sizes; best for "
            "almost every load order. A record's output says nothing about what kind of record it is.");
        break;
    case AllocationStrategy::ByRecordType:
        strategyNote_->setText(
            "Each group of record types (the catalogue's allocation groups: items, actors, quests, world, …) gets its "
            "own outputs, sized by how much of it there is. Easier to browse in xEdit; but outputs reference each other "
            "more, so they need more masters, and sizes are uneven. Useful when you want to inspect the result.");
        break;
    case AllocationStrategy::UserDefined:
        strategyNote_->setText(
            "Balanced size, then each rule sends matching records to the output you name: a record type (WEAP=3) or a "
            "plugin, either the record's defining plugin or a compiled plugin that changes it (MyWeaponPack.esp=5). "
            "Dependencies still win, so a record can end up later than its rule says. For keeping a mod's records "
            "together in a known output.");
        break;
    }
}

void CompilationPage::updateInstallNote()
{
    using fo4mx::InstallKind;
    const auto kind = app_->config().install.kind;
    QString how;
    if (kind == InstallKind::Vortex)
        how = "<b>Vortex:</b> after Export, press <b>Apply to Game</b>: with your Vortex downloads folder set in "
              "Settings it copies <b>Multiplexer Output.zip</b> there (or drag the zip onto Vortex's Mods page). In "
              "Vortex install and enable it, let it win every file conflict and Deploy. Then <b>close Vortex</b> and "
              "press <b>Apply to Game</b> again: it disables the compiled plugins in plugins.txt, enables the outputs "
              "and keeps the compiled mods' archives loading. Vortex reads plugins.txt back when it starts.";
    else if (kind == InstallKind::ModOrganizer2)
        how = "<b>Mod Organizer 2:</b> after Export, install <b>Multiplexer Output.zip</b> (Install a new mod from an "
              "archive), enable it and keep it at the bottom of the left pane so it wins over the original mods. Then "
              "press <b>Apply to Game</b>: it switches the profile's plugins to the outputs and keeps the compiled "
              "mods' archives loading.";
    else
        how = "<b>Manual install:</b> after Export, extract <b>Multiplexer Output.zip</b> into the game folder (its Data "
              "folder merges with the game's), disable the plugins listed in Multiplexer_DisablePlugins.txt, enable the "
              "output plugins, and press <b>Apply to Game</b> so the compiled mods' archives keep loading.";
    installNote_->setText(how + " Keep the original mods installed: only their plugins are disabled; their files "
                                "are still used.");
}

void CompilationPage::loadOptions()
{
    const auto& c = app_->config().compile;
    outputs_->setValue(std::clamp(c.outputCount, fo4mx::CompileOptions::kMinOutputs, fo4mx::CompileOptions::kMaxOutputs));
    autoOutputs_->setChecked(c.autoOutputs);
    outputs_->setEnabled(!c.autoOutputs);
    prefix_->setText(qs(c.outputPrefix));
    strategy_->setCurrentIndex(strategy_->findData(int(c.strategy)));
    QStringList rules;
    for (const auto& r : c.rules)
        rules << qs(r.pattern) + "=" + QString::number(r.output);
    rules_->setPlainText(rules.join("\n"));
    rules_->setEnabled(c.strategy == AllocationStrategy::UserDefined);
    updateStrategyNote();
    updateInstallNote();
    merge_->setChecked(c.mergeCompatible);
    light_->setChecked(c.lightOutputsWhenPossible);
    translatePrevis_->setChecked(c.translatePrevis);
    patchOnly_->setChecked(c.patchOnly);
    patch_->setChecked(c.buildPatch || c.patchOnly);
    patch_->setEnabled(!c.patchOnly);
    outputDir_->setText(toQString(app_->config().exportOptions.outputDirectory));
}

void CompilationPage::storeOptions()
{
    auto& c = app_->config().compile;
    c.outputCount = outputs_->value();
    c.autoOutputs = autoOutputs_->isChecked();
    c.outputPrefix = ss(prefix_->text().trimmed().isEmpty() ? "Dummy_Plugin_" : prefix_->text().trimmed());
    c.strategy = static_cast<AllocationStrategy>(strategy_->currentData().toInt());
    c.rules.clear();
    for (const auto& line : rules_->toPlainText().split('\n', Qt::SkipEmptyParts)) {
        auto parts = line.split('=');
        if (parts.size() == 2)
            c.rules.push_back({ss(parts[0].trimmed()), parts[1].trimmed().toInt()});
    }
    c.mergeCompatible = merge_->isChecked();
    c.lightOutputsWhenPossible = light_->isChecked();
    c.translatePrevis = translatePrevis_->isChecked();
    c.patchOnly = patchOnly_->isChecked();
    c.buildPatch = c.patchOnly || patch_->isChecked();
    app_->config().exportOptions.outputDirectory = toPath(outputDir_->text().trimmed());
    app_->saveSettings();
}

void CompilationPage::updateButtons()
{
    bool busy = app_->busy();
    auto* p = app_->pipeline();
    bool compiled = p && p->compiledOutput();
    bool valid = compiled && p->validation() && !p->validation()->critical();
    compile_->setEnabled(!busy);
    export_->setEnabled(!busy && valid);
    export_->setToolTip(valid ? "" : "Compile first; export is blocked while critical validation errors exist.");
    apply_->setEnabled(!busy && p && p->exportResult().has_value());
    openFolder_->setEnabled(p && p->exportResult().has_value());
}

void CompilationPage::refresh()
{
    QSqlDatabase db = app_->readDb();
    const QString patchName = meta("patch_name");
    outputsModel_->setNumberColumns({1, 2, 3});
    outputsModel_->setBytesColumns({4});
    outputsModel_->load(db,
                        "SELECT output_name AS Plugin, record_count AS Records, new_records AS 'New FormIDs', overrides AS "
                        "Overrides, size_bytes AS Size, CASE light WHEN 1 THEN 'Yes' ELSE 'No' END AS Light, masters AS "
                        "Masters FROM CompilationGroups WHERE record_count > 0 ORDER BY output_index");
    {
        GroupedSqlModel::Spec r;
        r.groupSql = "SELECT output_plugin, COUNT(*) FROM GeneratedRecords GROUP BY output_index, output_plugin "
                     "ORDER BY output_index";
        r.rowSql = "SELECT form_id, signature, editor_id, CASE WHEN is_new = 1 THEN 'New' WHEN instr(',' || ? || ',', "
                   "',' || output_plugin || ',') > 0 THEN 'Patch' ELSE 'Override' END, CASE merged WHEN 1 THEN "
                   "'Merged' ELSE '' END, p.name, size_bytes, contributors FROM GeneratedRecords g LEFT JOIN Plugins "
                   "p ON p.id = g.winner_plugin_id WHERE g.output_plugin = ? %ORDER% LIMIT ? OFFSET ?";
        r.headers = {"FormID", "Type", "Editor ID", "Kind", "Merge", "Source Winner", "Size", "Contributors"};
        r.sortExpr = {"form_id", "signature, form_id", "editor_id", "is_new, form_id", "merged, form_id",
                      "p.name, form_id", "size_bytes", "contributors"};
        r.defaultOrder = "ORDER BY signature, form_id";
        r.rowNoun = "records";
        // The patch plugins' names, written into the row query (the group query has no placeholder).
        r.rowSql.replace("instr(',' || ? || ',',", "instr(',' || '" + QString(patchName).replace("'", "''") + "' || ',',");
        recordsModel_->setMonoColumns({0});
        recordsModel_->setBytesColumns({6});
        recordsModel_->setStatusColumns({4});
        recordsModel_->load(db, r);
    }
    {
        GroupedSqlModel::Spec m;
        m.groupSql = "SELECT source_plugin, COUNT(*) FROM FormMappings GROUP BY source_plugin ORDER BY source_plugin "
                     "COLLATE NOCASE";
        m.rowSql = "SELECT printf('%06X', source_object_id), output_plugin, printf('%06X', output_object_id), signature, "
                   "editor_id FROM FormMappings WHERE source_plugin = ? %ORDER% LIMIT ? OFFSET ?";
        m.headers = {"Source Object", "Output", "New Object", "Type", "Editor ID"};
        m.sortExpr = {"source_object_id", "output_index, output_object_id", "output_object_id",
                      "signature, source_object_id", "editor_id"};
        m.defaultOrder = "ORDER BY source_object_id";
        m.rowNoun = "records moved";
        mappingsModel_->setMonoColumns({0, 2});
        mappingsModel_->load(db, m);
    }
    {
        GroupedSqlModel::Spec a;
        a.groupSql = "SELECT output_plugin, COUNT(*) FROM RecordAssets GROUP BY output_plugin ORDER BY output_plugin";
        a.rowSql = "SELECT form_id, editor_id, asset_path, CASE WHEN found = 0 THEN 'Missing' WHEN needs_registration = 1 "
                   "THEN 'Via Archive INI' ELSE 'OK' END, role, origin_mod, archive, archive_owner, physical FROM "
                   "RecordAssets WHERE output_plugin = ? %ORDER% LIMIT ? OFFSET ?";
        a.headers = {"FormID", "Editor ID", "Asset", "Status", "Role", "Origin Mod", "Archive", "Archive Owner",
                     "Physical Path"};
        a.sortExpr = {"form_id, role", "editor_id", "asset_path", "found, needs_registration, asset_path", "role, form_id",
                      "origin_mod", "archive", "archive_owner", "physical"};
        a.defaultOrder = "ORDER BY form_id, role";
        a.rowNoun = "asset uses";
        assetsModel_->setStatusColumns({3});
        assetsModel_->load(db, a);
    }
    fitColumns(outputsView_);
    for (auto* v : {recordsView_, mappingsView_, assetsView_})
        for (int c = 0; c + 1 < v->model()->columnCount(); ++c)
            v->setColumnWidth(c, c == 0 ? 220 : 150);

    QString compiled = meta("compiled_plugins");
    QString patchText;
    if (!patchName.isEmpty())
        patchText = QString(" · %1: %2 merged records").arg(QString(patchName).replace(",", ", "),
                                                               formatCount(meta("patched_records").toLongLong()));
    if (compiled.isEmpty()) {
        summary_->setText("Not compiled yet. Options on the left are saved with your settings.");
    } else {
        QString incremental;
        if (!meta("merges_reused").isEmpty())
            incremental = QString(" · incremental: %1 override chains reused, %2 rebuilt").arg(meta("merges_reused"),
                                                                                              meta("merges_computed"));
        if (!meta("export_written").isEmpty())
            incremental += QString(" · last export: %1 plugins written, %2 unchanged, %3 removed")
                               .arg(meta("export_written"), meta("export_unchanged"), meta("export_removed"));
        summary_->setText(QString("%1 plugins compiled into %2 output plugins · %3 records written · validation: %4 "
                                  "checks, %5 warnings, %6 errors%7")
                              .arg(compiled, meta("output_plugins"), formatCount(meta("compiled_records").toLongLong()),
                                   formatCount(meta("validation_pre-export_checks").toLongLong()),
                                   meta("validation_pre-export_warnings"), meta("validation_pre-export_errors"),
                                   patchText + incremental));
    }
    updateButtons();
}

void CompilationPage::showTrace(const QModelIndex& index)
{
    if (!index.isValid() || recordsModel_->isGroup(index))
        return;
    QString output = recordsModel_->groupKey(index);
    QString formId = recordsModel_->rowValue(index, 0).toString();
    QSqlDatabase db = app_->readDb();
    QSqlQuery q(db);
    q.prepare("SELECT g.editor_id, g.signature, o.name, w.name, g.contributors, g.is_new, g.merged, g.object_id "
              "FROM GeneratedRecords g LEFT JOIN Plugins o ON o.id = g.origin_plugin_id "
              "LEFT JOIN Plugins w ON w.id = g.winner_plugin_id WHERE g.output_plugin = ? AND g.form_id = ?");
    q.addBindValue(output);
    q.addBindValue(formId);
    if (!q.exec() || !q.next())
        return;
    auto v = [&](int i) { return q.value(i).toString().toHtmlEscaped(); };
    QString html = QString("<h3 style='color:#3ddc84;margin-bottom:2px'>%1 <span style='color:#8aa196'>[%2]</span></h3>")
                       .arg(v(0).isEmpty() ? "(no editor id)" : v(0), v(1));
    html += "<table cellpadding='3'>";
    auto row2 = [&](const QString& k, const QString& val) {
        html += "<tr><td style='color:#8aa196'>" + k + "</td><td>" + val + "</td></tr>";
    };
    row2("Generated plugin", output.toHtmlEscaped() + " · " + formId);
    row2("Source plugin", v(2) + (q.value(5).toInt()
                                      ? QString(" · object %1").arg(QString::number(q.value(7).toUInt(), 16).rightJustified(6, '0').toUpper())
                                      : QString(" (override of an existing record)")));
    row2("Winning override", v(3));
    row2("Changed by", v(4).isEmpty() ? v(3) : v(4));
    row2("Resolution", q.value(6).toInt() ? "merged from several plugins" : "taken from the winning plugin");
    html += "</table>";

    QSqlQuery a(db);
    a.prepare("SELECT role, asset_path, via, found, origin_mod, archive, archive_owner, physical, needs_registration "
              "FROM RecordAssets WHERE output_plugin = ? AND form_id = ? ORDER BY CASE role WHEN 'Mesh' THEN 0 WHEN "
              "'Icon' THEN 1 WHEN 'Material' THEN 2 ELSE 3 END");
    a.addBindValue(output);
    a.addBindValue(formId);
    a.exec();
    QString assets;
    while (a.next()) {
        QString origin;
        if (!a.value(3).toInt())
            origin = "<span style='color:#ff5c5c'>missing: no installed mod provides this file</span>";
        else if (!a.value(5).toString().isEmpty())
            origin = QString("%1 → <b>%2</b>%3").arg(a.value(4).toString().toHtmlEscaped(), a.value(5).toString().toHtmlEscaped(),
                                                      a.value(8).toInt() ? " <span style='color:#f5c542'>(kept loading through "
                                                                           "Fallout4Custom.ini after Apply to Game)</span>"
                                                                         : "");
        else
            origin = QString("%1 (loose file)").arg(a.value(4).toString().toHtmlEscaped());
        assets += QString("<p style='margin:4px 0'><b>%1:</b> <span style='font-family:monospace'>%2</span>%3<br>"
                          "<span style='color:#8aa196'>Origin:</span> %4<br><span style='color:#8aa196;font-family:monospace'>%5</span></p>")
                      .arg(a.value(0).toString(), a.value(1).toString().toHtmlEscaped(),
                           a.value(2).toString().isEmpty() ? QString() : " <span style='color:#8aa196'>(via " + a.value(2).toString().toHtmlEscaped() + ")</span>",
                           origin, a.value(7).toString().toHtmlEscaped());
    }
    html += "<h4>Referenced assets</h4>" + (assets.isEmpty() ? QString("<p style='color:#8aa196'>This record references no "
                                                                         "meshes, materials or textures.</p>")
                                                              : assets);
    trace_->setHtml(html);
}

} // namespace gui
