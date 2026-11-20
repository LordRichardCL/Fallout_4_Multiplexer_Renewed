#include "pages/CompilationPage.hpp"

#include "Format.hpp"
#include "models/GroupedSqlModel.hpp"
#include "models/SqlModel.hpp"
#include "Theme.hpp"
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
#include <QProgressBar>
#include <QDateTime>
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
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* l = new QVBoxLayout;
    l->setContentsMargins(28, 22, 28, 0);
    l->setSpacing(12);
    outer->addLayout(l, 1);
    compile_ = makeButton("Compile", "Big");
    compile_->setIcon(lineIcon("compile", QColor(0xea, 0xff, 0xf2), 17));
    export_ = makeButton("Export");
    export_->setIcon(lineIcon("export", theme::kText, 15));
    export_->setAccessibleName("Export Plugins");
    apply_ = makeButton("Apply to Game…");
    apply_->setIcon(lineIcon("install", theme::kText, 15));
    apply_->setToolTip("Mod Organizer 2: switch the profile's load order to the output plugins. All installs: add "
                       "compiled plugins' archives to Fallout4Custom.ini so their assets keep loading.");
    openFolder_ = makeButton("Open Folder");
    openFolder_->setIcon(lineIcon("folder", theme::kText, 15));
    auto* saveSettings = makeSaveSettingsButton();

    // Header: title, one line, and the presets.
    presets_ = new SegmentedControl({"Recommended", "Safest", "Custom"});
    presets_->button("Recommended")
        ->setToolTip("Combine compatible changes, apply your patch choices, merge cell settings, translate previs files "
                     "and build Multiplexer_Patch.esp. Light outputs and writing broken references as null stay off.");
    presets_->button("Safest")
        ->setToolTip("Combine compatible changes only: no patch choices, no cell merge, no previs translation, no "
                     "patch, no light outputs, broken references never written as null.");
    presets_->button("Custom")->setToolTip("Your own choice of switches below.");
    l->addWidget(makePageHeader("Compilation", "Turns your mods into a few output plugins, with the same game data.",
                                {presets_}));

    auto* cols = new QHBoxLayout;
    cols->setSpacing(16);

    // ---- Options (left) ----
    auto* optionsScroll = new QScrollArea;
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    auto* options = new QWidget;
    optionsScroll->setWidget(options);
    auto* ol = new QVBoxLayout(options);
    ol->setContentsMargins(0, 0, 8, 12);
    ol->setSpacing(12);

    outputs_ = new QSpinBox;
    outputs_->setRange(fo4mx::CompileOptions::kMinOutputs, fo4mx::CompileOptions::kMaxOutputs);
    outputs_->setFixedWidth(70);
    prefix_ = new QLineEdit;
    prefix_->setFixedWidth(190);
    outputDir_ = new QLineEdit;
    outputDir_->setMinimumWidth(240);
    outputDir_->setPlaceholderText("Default: <game folder>\\Multiplexer Output");
    auto* browse = makeButton("Browse");
    autoOutputs_ = new ToggleSwitch("Automatic");
    autoOutputs_->setToolTip("Use the fewest output plugins whose master lists all fit (Fallout 4 allows at most 254 "
                             "masters per plugin), from 1 up to the maximum. Fewer outputs take fewer load order "
                             "slots; each one is larger. The number used is shown after Compile.");
    auto* autoLabel = new QLabel("Auto");
    autoLabel->setObjectName("OptionDescription");
    connect(autoOutputs_, &QCheckBox::toggled, outputs_, [this](bool on) { outputs_->setEnabled(!on); });
    prefix_->setPlaceholderText(QString("Default: %1").arg(fo4mx::CompileOptions::kDefaultOutputPrefix));
    auto* prefixDefault = makeButton("Default");
    prefixDefault->setToolTip(QString("Use the default name prefix, %1 (outputs %101.esp, %102.esp, …)")
                                  .arg(fo4mx::CompileOptions::kDefaultOutputPrefix));
    connect(prefixDefault, &QPushButton::clicked, this,
            [this] { prefix_->setText(fo4mx::CompileOptions::kDefaultOutputPrefix); });
    const QString outputsHelp = QString(
        "The most output plugins the compiler may create (%1 to %2). It fills them in order and starts a new one only "
        "when the current one holds about its share of the records, so a small load order uses fewer.\n\n"
        "Each output that is not ESL-flagged takes one of Fallout 4's 254 full plugin slots, shared with the plugins "
        "that stay enabled; validation stops the export if the total would pass 254. More outputs also leave fewer "
        "masters for the worldspace output (254 minus this number).\n\nAuto: " + autoOutputs_->toolTip())
                                    .arg(fo4mx::CompileOptions::kMinOutputs)
                                    .arg(fo4mx::CompileOptions::kMaxOutputs);
    outputs_->setToolTip(outputsHelp);
    prefix_->setToolTip(QString("Output plugins are named <prefix>01.esp, <prefix>02.esp, … The default is %1; "
                                "any other prefix works the same. After changing it, the earlier build's outputs keep "
                                "their old names: remove them from the game once the new build is switched on.")
                            .arg(fo4mx::CompileOptions::kDefaultOutputPrefix));
    outputDir_->setToolTip(
        "The folder that holds the exports. By default \"Multiplexer Output\" next to the game. Every export gets "
        "a folder of its own in it, \"Multiplexer Output DDMMYY HHMMSS\": its Data folder holds everything the game "
        "loads, and \"Multiplexer Output.zip\" beside it is the package for your mod manager. An export never "
        "changes an earlier one; \"Multiplexer Latest.txt\" names the latest.\n\n"
        "Advanced: a folder inside Mod Organizer 2's mods folder makes every export a mod of its own there; then "
        "compiled plugins that have archives are replaced by archive loaders instead of loose copies and INI entries.");
    {
        QVBoxLayout* body = nullptr;
        ol->addWidget(makeSectionCard("compile", "Output", &body));
        body->addWidget(makeOptionRow("Number of output plugins", "How many plugins your mods are compiled into",
                                      outputsHelp, {outputs_, autoOutputs_, autoLabel}));
        auto* nameRow = makeOptionRow("Name", "", prefix_->toolTip(), {prefix_, prefixDefault});
        prefixDescription_ = nameRow->findChild<QLabel*>("OptionDescription");
        body->addWidget(nameRow);
        body->addWidget(makeOptionRow("Output folder", "A Mod Organizer 2 mod or any folder", outputDir_->toolTip(),
                                      {outputDir_, browse}));
    }

    merge_ = new ToggleSwitch("Merge compatible edits (keyword lists, leveled lists, form lists, different fields)");
    light_ = new ToggleSwitch("ESL-flag outputs that fit the light plugin range (≤ 2048 new records)");
    merge_->setToolTip("Where two or more mods change the same record in different ways (a leveled list, keywords, "
                       "different fields), combine their changes instead of keeping only the last mod's version.");
    light_->setToolTip("An output with few enough new records (2,048 or fewer) is ESL-flagged, so it takes no full "
                       "plugin slot. Outputs with new cells or worldspaces are never ESL-flagged.");
    translatePrevis_ = new ToggleSwitch("Translate previs files into the output's numbering");
    translatePrevis_->setToolTip(
        "A cell written to an output wins in game, so its previs file (.uvd) is read in the output's numbering. "
        "On: a translated copy is written loose. Off: the original file is used. Either way the same plugins "
        "compile: this setting only decides whether translated copies are written. The checks still run, and a "
        "previs file that cannot be translated keeps its cell's plugins out. Turn off only to compare the two in "
        "game.");
    mergeCellConfig_ = new ToggleSwitch("Merge cell configuration (lighting, location, water, regions...)");
    mergeCellConfig_->setToolTip(
        "A cell several mods change keeps the winning mod's precombine and previs data, and takes the last real "
        "change of each configuration field from the others: lighting, lighting template, location, encounter "
        "zone, music, acoustic space, image space, god rays, water, regions and the cell's flags. Like "
        "FO4CheckPrevisbines' \"Fix only Cell Config Conflicts\". Off: the winning mod's version is written as "
        "it is. Worldspaces take the same from their versions (climate, water, music, map, location...), and keep "
        "the large references the game uses today. A cell or worldspace whose winning mod stays in the load order "
        "is written, merged, to Multiplexer_Patch.esp. Every field taken is listed in the report (Cell "
        "Configuration).");
    applyPlan_ = new ToggleSwitch("Apply the compatibility patch plan");
    applyPlan_->setToolTip(
        "Records the compatibility patch plan marks Ready (Conflict Report, \"Compatibility patch plan\") are "
        "written as their winning version plus the changes the plan carries: Green ones by default, Yellow ones "
        "you chose. Only plugins that stay hold the record: it goes to Multiplexer_Patch.esp. A compiled plugin "
        "holds it: it goes to an output. Each record is checked to hold every planned change before it is "
        "written; one that does not is written as it would be without the plan. Off: the plan is only reported.");
    brokenAsNull_ = new ToggleSwitch("Write broken references as null (lossy)");
    brokenAsNull_->setToolTip(
        "A reference that cannot be written faithfully (its master is missing, or it names a record its compiled "
        "plugin does not contain) is never guessed. Off (default): the plugin that stores it stays in the load "
        "order, so the game reads its own data unchanged (Kept Out Automatically: Broken reference). On: such "
        "references are written as null (00000000) and the plugin compiles; each is listed under Broken in "
        "Source. The game's own reading of these references is not verified, so this can lose data.");
    masterPlaceholders_ = new ToggleSwitch("Placeholders for unused masters");
    masterPlaceholders_->setToolTip(
        "A plugin that stays in the load order keeps its masters there too. Some list a master without using any "
        "of its records (common in patches). On: such a master compiles, and an empty light plugin with its name "
        "(like an archive loader) keeps the plugin that lists it loading. A master counts as unused only when "
        "none of the plugin's records overrides it, sits in its groups, or names it in a FormID; data the "
        "definitions do not describe counts as using it. Mod Organizer 2 with the output folder in the mods "
        "folder only, as for archive loaders. The Dependency Report lists every unused master either way.");
    planPending_ = new QLabel;
    planPending_->setObjectName("Pill");
    planPending_->setStyleSheet("background:#2a2614; color:#f5c542;");
    planPending_->setToolTip("Choices waiting in the Conflicts page (Compatibility patch plan)");
    {
        QVBoxLayout* body = nullptr;
        ol->addWidget(makeSectionCard("conflicts", "When mods change the same thing", &body));
        body->addWidget(makeOptionRow("Combine compatible changes", "Keywords, leveled lists and different fields from every mod",
                                      merge_->toolTip(), {merge_}));
        body->addWidget(makeOptionRow("Apply my patch choices", "Keep changes the winning mod would lose",
                                      applyPlan_->toolTip(), {planPending_, applyPlan_}));
        body->addWidget(makeOptionRow("Merge cell settings", "Lighting, water, location and regions · never precombines",
                                      mergeCellConfig_->toolTip(), {mergeCellConfig_}));
        body->addWidget(makeOptionRow("Translate previs files", "Writes copies that match the outputs' numbering",
                                      translatePrevis_->toolTip(), {translatePrevis_}));
    }

    patch_ = new ToggleSwitch("Build Multiplexer_Patch.esp for plugins that stay in the load order");
    patchOnly_ = new ToggleSwitch("Patch only: compile no plugins");
    const QString patchHelp = "Like a Bashed Patch: where last-plugin-wins would drop edits (leveled lists, form "
                              "lists, keywords, different fields), the patch carries the merged record. It only "
                              "overrides existing records, so it is always ESL-flagged and adds no FormIDs. It must "
                              "load last.";
    patch_->setToolTip(patchHelp);
    patchSection_ = new CollapsibleSection("validation", "Patch");
    patchSection_->body()->addWidget(makeOptionRow("Build Multiplexer_Patch.esp",
                                                   "Merged records for the plugins that stay in your load order",
                                                   patchHelp, {patch_}));
    patchSection_->body()->addWidget(makeOptionRow("Patch only", "Build only the patch: compile no plugins",
                                                   "Compile no plugins: only Multiplexer_Patch.esp is built, for the "
                                                   "load order as it is.",
                                                   {patchOnly_}));
    ol->addWidget(patchSection_);

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
    strategy_->setMinimumWidth(240);
    strategyNote_ = new QLabel;
    strategyNote_->setObjectName("OptionDescription");
    strategyNote_->setWordWrap(true);
    rules_ = new QPlainTextEdit;
    rules_->setPlaceholderText("One rule per line: PATTERN=OUTPUT\nWEAP=3\nMyWeaponPack.esp=5");
    rules_->setMaximumHeight(90);
    const QString allocationHelp =
        "Whatever the strategy: a record always goes into the same or a later output than the new records it "
        "references; cells, worldspaces and quests with dialogue go together into the last outputs; and records keep "
        "the output they had in the previous build when its FormID map is in the output folder. The strategy changes "
        "how records are spread, not what the game sees.";
    advancedSection_ = new CollapsibleSection("records", "Advanced");
    advancedSection_->body()->addWidget(
        makeOptionRow("Record allocation", "How records are spread over the outputs", allocationHelp, {strategy_}));
    advancedSection_->body()->addWidget(strategyNote_);
    advancedSection_->body()->addWidget(rules_);
    advancedSection_->body()->addWidget(
        makeOptionRow("Light outputs", "ESL-flag outputs with 2,048 new records or fewer", light_->toolTip(), {light_}));
    auto* lossy = new QLabel("lossy");
    lossy->setObjectName("Pill");
    lossy->setStyleSheet("background:#2a1616; color:#ff5c5c;");
    advancedSection_->body()->addWidget(makeOptionRow("Write broken references as null",
                                                      "Lets plugins with broken references compile",
                                                      brokenAsNull_->toolTip(), {lossy, brokenAsNull_}));
    advancedSection_->body()->addWidget(makeOptionRow("Placeholders for unused masters",
                                                      "Compile masters that plugins staying in the load order list "
                                                      "but do not use (Mod Organizer 2)",
                                                      masterPlaceholders_->toolTip(), {masterPlaceholders_}));
    ol->addWidget(advancedSection_);

    auto* installSection = new CollapsibleSection("install", "How to install");
    installNote_ = new QLabel;
    installNote_->setObjectName("OptionDescription");
    installNote_->setWordWrap(true);
    installNote_->setTextFormat(Qt::RichText);
    outputsNote_ = new QLabel;
    outputsNote_->setObjectName("OptionDescription");
    outputsNote_->setWordWrap(true);
    installSection->body()->setSpacing(8);
    installSection->body()->addWidget(installNote_);
    installSection->body()->addWidget(outputsNote_);
    installSection->setSummary("after Export, for your mod manager");
    ol->addWidget(installSection);
    auto updateOutputsNote = [this] {
        const QString count = autoOutputs_->isChecked() ? QString("the fewest output plugins that fit")
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
    ol->addStretch();
    cols->addWidget(optionsScroll, 5);

    // ---- Results (right) ----
    auto* results = new QWidget;
    auto* rl = new QVBoxLayout(results);
    rl->setContentsMargins(0, 0, 0, 12);
    rl->setSpacing(10);
    stages_ = new StageIndicator({"Scan", "Analyse", "Resolve", "Merge", "Rewrite", "Compile", "Validate", "Export"});
    stageCard_ = new QFrame;
    stageCard_->setObjectName("Card");
    auto* sfl = new QVBoxLayout(stageCard_);
    sfl->setContentsMargins(10, 4, 10, 4);
    sfl->addWidget(stages_);
    stageCard_->hide();
    rl->addWidget(stageCard_);

    auto* resultCard = new QFrame;
    resultCard->setObjectName("Card");
    auto* rc = new QVBoxLayout(resultCard);
    rc->setContentsMargins(18, 14, 18, 12);
    rc->setSpacing(8);
    auto* rh = new QHBoxLayout;
    auto* lb = new QLabel("Last build");
    lb->setObjectName("SectionCardTitle");
    buildPill_ = new QLabel;
    buildPill_->setObjectName("Pill");
    lastBuild_ = new QLabel;
    lastBuild_->setObjectName("OptionDescription");
    rh->addWidget(lb);
    rh->addWidget(buildPill_);
    rh->addStretch();
    rh->addWidget(lastBuild_);
    rc->addLayout(rh);
    auto* flow = new QHBoxLayout;
    flow->setSpacing(14);
    auto number = [&](QLabel*& value, const QString& caption) {
        auto* v = new QVBoxLayout;
        v->setSpacing(0);
        value = new QLabel("—");
        value->setObjectName("TileValue");
        auto* c = new QLabel(caption);
        c->setObjectName("OptionDescription");
        v->addWidget(value);
        v->addWidget(c);
        flow->addLayout(v);
    };
    auto arrow = [&] {
        auto* a = new QLabel("→");
        a->setStyleSheet("color:#3ddc84; font-size:15pt;");
        flow->addWidget(a);
    };
    number(compiledN_, "plugins compiled");
    arrow();
    number(outputsN_, "output plugins");
    arrow();
    number(recordsN_, "records written");
    flow->addStretch();
    rc->addLayout(flow);
    outputsList_ = new QWidget;
    auto* olist = new QVBoxLayout(outputsList_);
    olist->setContentsMargins(0, 0, 0, 0);
    olist->setSpacing(0);
    rc->addWidget(outputsList_);
    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setObjectName("OptionDescription");
    rc->addWidget(summary_);
    rl->addWidget(resultCard);

    auto* checks = new QHBoxLayout;
    checks->setSpacing(10);
    auto check = [&](QLabel*& value, const QString& caption, const QString& color, const QString& page) {
        auto* f = new QFrame;
        f->setObjectName("Card");
        auto* v = new QVBoxLayout(f);
        v->setContentsMargins(14, 8, 14, 8);
        v->setSpacing(0);
        auto* top = new QHBoxLayout;
        value = new QLabel("—");
        value->setObjectName("TileValue");
        value->setStyleSheet("color:" + color + ";");
        top->addWidget(value);
        top->addStretch();
        auto* open = makeButton("Open", "LinkButton");
        connect(open, &QPushButton::clicked, this, [this, page] { emit navigate(page); });
        top->addWidget(open);
        v->addLayout(top);
        auto* c = new QLabel(caption);
        c->setObjectName("OptionDescription");
        v->addWidget(c);
        checks->addWidget(f, 1);
    };
    check(errorsN_, "errors", "#3ddc84", "Validation");
    check(warningsN_, "warnings", "#f5c542", "Validation");
    check(keptN_, "plugins kept out", "#e4efe8", "Advanced Users Auto-Merger");
    rl->addLayout(checks);

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
    resultSplit->setStretchFactor(0, 3);
    resultSplit->setStretchFactor(1, 2);
    rl->addWidget(resultSplit, 1);
    connect(recordsView_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showTrace(i); });
    cols->addWidget(results, 4);
    l->addLayout(cols, 1);

    // ---- Action bar (bottom) ----
    auto* bar = new QFrame;
    bar->setObjectName("ActionBar");
    auto* bh = new QHBoxLayout(bar);
    bh->setContentsMargins(28, 10, 28, 10);
    bh->setSpacing(10);
    pipe_ = new QLabel;
    pipe_->setTextFormat(Qt::RichText);
    pipe_->setObjectName("OptionDescription");
    bh->addWidget(pipe_);
    bh->addStretch();
    for (auto* b : {saveSettings, openFolder_, export_, apply_, compile_})
        bh->addWidget(b);
    outer->addWidget(bar);

    connect(browse, &QPushButton::clicked, this, [this] {
        QString d = QFileDialog::getExistingDirectory(this, "Output folder", outputDir_->text());
        if (!d.isEmpty())
            outputDir_->setText(d);
    });
    connect(compile_, &QPushButton::clicked, app_, &AppController::runCompile);
    connect(export_, &QPushButton::clicked, app_, &AppController::runExport);
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
        QCheckBox* copyRecorder = nullptr;
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
            // The Diagnostic Recorder (when this export prepared it): into the same downloads folder, to be
            // installed as its own mod.
            const bool downloadsUsable = !preview.downloadsFolder.isEmpty() && preview.downloadsProblem.isEmpty();
            if (preview.recorderAvailable && downloadsUsable) {
                copyRecorder = new QCheckBox("Also copy the Diagnostic Recorder (Multiplexer Diagnostic Recorder.zip) to " +
                                             manager + "'s downloads folder");
                copyRecorder->setChecked(true);
                bl->addWidget(copyRecorder);
                auto* rnote = new QLabel(QString("Install it as its own mod, like the package. It records what the game "
                                                 "loads for the reports and changes nothing in the game.") +
                                         (preview.recorderInDownloads ? " The copy already there is replaced (backed up "
                                                                        "first)."
                                                                      : ""));
                rnote->setWordWrap(true);
                rnote->setStyleSheet("color: palette(placeholder-text);");
                bl->addWidget(rnote);
            } else if (preview.recorderAvailable) {
                auto* rnote = new QLabel("The Diagnostic Recorder is ready to install: " + preview.recorderZipPath);
                rnote->setWordWrap(true);
                rnote->setTextInteractionFlags(Qt::TextSelectableByMouse);
                bl->addWidget(rnote);
            }
            auto* steps = new QLabel(
                preview.vortex
                    ? QString("In Vortex: install and enable the package from the Downloads page (restart Vortex if it "
                              "is not listed)%1, let it win every file conflict and Deploy. Then close Vortex and press "
                              "Apply to Game again to switch your load order over.")
                          .arg(preview.recorderAvailable ? " and the Diagnostic Recorder" : "")
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
        auto updateGo = [&] {
            go->setEnabled(preview.ready || (copyZip && copyZip->isChecked()) ||
                           (copyRecorder && copyRecorder->isChecked()));
        };
        if (enableMod)
            connect(enableMod, &QCheckBox::toggled, &dlg, updateGo);
        updateGo();
        if (copyZip)
            connect(copyZip, &QCheckBox::toggled, &dlg, updateGo);
        if (copyRecorder)
            connect(copyRecorder, &QCheckBox::toggled, &dlg, updateGo);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted)
            return;

        AppController::ApplyChoice choice;
        choice.ghost = ghost && ghost->isChecked();
        choice.copyZip = copyZip && copyZip->isChecked();
        choice.enableOutputMod = enableMod && enableMod->isChecked();
        choice.copyRecorder = copyRecorder && copyRecorder->isChecked();
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
        if (const auto src = p ? p->applySource() : fo4mx::Pipeline::ApplySource{})
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                toQString(src.exported->packageRoot.empty() ? src.exported->directory : src.exported->packageRoot)));
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
    // Every option is written to the settings state as it changes (saved with Save Settings).
    connect(outputs_, &QSpinBox::valueChanged, this, [this] { storeOptions(); });
    connect(prefix_, &QLineEdit::textChanged, this, [this] { storeOptions(); });
    connect(outputDir_, &QLineEdit::textChanged, this, [this] { storeOptions(); });
    connect(strategy_, &QComboBox::currentIndexChanged, this, [this] { storeOptions(); });
    connect(rules_, &QPlainTextEdit::textChanged, this, [this] { storeOptions(); });
    for (auto* c : {autoOutputs_, merge_, light_, translatePrevis_, mergeCellConfig_, applyPlan_, brokenAsNull_, patch_,
                    patchOnly_, masterPlaceholders_})
        connect(c, &QCheckBox::toggled, this, [this] {
            storeOptions();
            updatePresetState();
            updateSummaries();
        });
    connect(strategy_, &QComboBox::currentIndexChanged, this, [this] { updateSummaries(); });
    connect(prefix_, &QLineEdit::textChanged, this, [this] { updateSummaries(); });
    connect(presets_, &SegmentedControl::picked, this, [this](const QString& name) { applyPreset(name); });
    connect(app_, &AppController::taskStarted, this, [this](const QString& task) {
        if (task != "Check")
            stageCard_->show();
    });
    connect(app_, &AppController::busyChanged, this, [this](bool busy) {
        if (!busy)
            stageCard_->hide();
    });
    loadOptions();
}

namespace {
struct Preset {
    bool merge, plan, cells, previs, patch, patchOnly, light, brokenNull;
};
const Preset kRecommended{true, true, true, true, true, false, false, false};
const Preset kSafest{true, false, false, false, false, false, false, false};
} // namespace

void CompilationPage::applyPreset(const QString& name)
{
    if (name != "Recommended" && name != "Safest") {
        updatePresetState();
        return;
    }
    const Preset& p = name == "Recommended" ? kRecommended : kSafest;
    patchOnly_->setChecked(p.patchOnly);
    merge_->setChecked(p.merge);
    applyPlan_->setChecked(p.plan);
    mergeCellConfig_->setChecked(p.cells);
    translatePrevis_->setChecked(p.previs);
    patch_->setChecked(p.patch);
    light_->setChecked(p.light);
    brokenAsNull_->setChecked(p.brokenNull);
    updatePresetState();
}

void CompilationPage::updatePresetState()
{
    if (!presets_)
        return;
    const Preset now{merge_->isChecked(),  applyPlan_->isChecked(), mergeCellConfig_->isChecked(),
                     translatePrevis_->isChecked(), patch_->isChecked(), patchOnly_->isChecked(),
                     light_->isChecked(), brokenAsNull_->isChecked()};
    auto same = [&](const Preset& p) {
        return p.merge == now.merge && p.plan == now.plan && p.cells == now.cells && p.previs == now.previs &&
               p.patch == now.patch && p.patchOnly == now.patchOnly && p.light == now.light &&
               p.brokenNull == now.brokenNull;
    };
    presets_->setCurrent(same(kRecommended) ? "Recommended" : same(kSafest) ? "Safest" : "Custom");
}

void CompilationPage::updateSummaries()
{
    if (!patchSection_)
        return;
    const QString prefix = prefix_->text().trimmed().isEmpty() ? QString(fo4mx::CompileOptions::kDefaultOutputPrefix)
                                                               : prefix_->text().trimmed();
    if (prefixDescription_) {
        prefixDescription_->setText("Outputs are named " + prefix + "01.esp, " + prefix + "02.esp…");
        prefixDescription_->show();
    }
    patchSection_->setSummary(patchOnly_->isChecked() ? "patch only: no plugins compiled"
                              : patch_->isChecked()   ? "Multiplexer_Patch.esp on"
                                                      : "no patch");
    const int strategy = strategy_->currentData().toInt();
    advancedSection_->setSummary(
        QString("%1 · light outputs %2 · broken references %3 · master placeholders %4")
            .arg(strategy == int(AllocationStrategy::ByRecordType)   ? "grouped by record type"
                 : strategy == int(AllocationStrategy::UserDefined) ? "balanced with your rules"
                                                                    : "balanced allocation",
                 light_->isChecked() ? "on" : "off", brokenAsNull_->isChecked() ? "written as null" : "kept",
                 masterPlaceholders_->isChecked() ? "on" : "off"));
}

namespace {

std::vector<fo4mx::AllocationRule> parseRules(const QString& text)
{
    std::vector<fo4mx::AllocationRule> rules;
    for (const auto& line : text.split('\n', Qt::SkipEmptyParts)) {
        auto parts = line.split('=');
        if (parts.size() == 2)
            rules.push_back({ss(parts[0].trimmed()), parts[1].trimmed().toInt()});
    }
    return rules;
}

std::string prefixFrom(const QString& text)
{
    return ss(text.trimmed().isEmpty() ? QString(fo4mx::CompileOptions::kDefaultOutputPrefix) : text.trimmed());
}

} // namespace

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
    loading_ = true;
    const auto& c = app_->config().compile;
    outputs_->setValue(std::clamp(c.outputCount, fo4mx::CompileOptions::kMinOutputs, fo4mx::CompileOptions::kMaxOutputs));
    autoOutputs_->setChecked(c.autoOutputs);
    outputs_->setEnabled(!c.autoOutputs);
    // Text that already says the same is left as typed (a rule being written, spaces).
    if (prefixFrom(prefix_->text()) != c.outputPrefix)
        prefix_->setText(qs(c.outputPrefix));
    strategy_->setCurrentIndex(strategy_->findData(int(c.strategy)));
    const auto shown = parseRules(rules_->toPlainText());
    const bool sameRules = shown.size() == c.rules.size() &&
                           std::equal(shown.begin(), shown.end(), c.rules.begin(), [](const auto& a, const auto& b) {
                               return a.pattern == b.pattern && a.output == b.output;
                           });
    if (!sameRules) {
        QStringList rules;
        for (const auto& r : c.rules)
            rules << qs(r.pattern) + "=" + QString::number(r.output);
        rules_->setPlainText(rules.join("\n"));
    }
    rules_->setEnabled(c.strategy == AllocationStrategy::UserDefined);
    updateStrategyNote();
    updateInstallNote();
    merge_->setChecked(c.mergeCompatible);
    light_->setChecked(c.lightOutputsWhenPossible);
    translatePrevis_->setChecked(c.translatePrevis);
    mergeCellConfig_->setChecked(c.mergeCellConfig);
    applyPlan_->setChecked(c.applyPatchPlan);
    brokenAsNull_->setChecked(c.brokenReferencesAsNull);
    masterPlaceholders_->setChecked(c.unusedMasterPlaceholders);
    patchOnly_->setChecked(c.patchOnly);
    patch_->setChecked(c.buildPatch || c.patchOnly);
    patch_->setEnabled(!c.patchOnly);
    if (toPath(outputDir_->text().trimmed()) != app_->config().exportOptions.outputDirectory)
        outputDir_->setText(toQString(app_->config().exportOptions.outputDirectory));
    loading_ = false;
    updatePresetState();
    updateSummaries();
}

void CompilationPage::storeOptions()
{
    if (loading_)
        return;
    auto& c = app_->config().compile;
    c.outputCount = outputs_->value();
    c.autoOutputs = autoOutputs_->isChecked();
    c.outputPrefix = prefixFrom(prefix_->text());
    c.strategy = static_cast<AllocationStrategy>(strategy_->currentData().toInt());
    c.rules = parseRules(rules_->toPlainText());
    c.mergeCompatible = merge_->isChecked();
    c.lightOutputsWhenPossible = light_->isChecked();
    c.translatePrevis = translatePrevis_->isChecked();
    c.mergeCellConfig = mergeCellConfig_->isChecked();
    c.applyPatchPlan = applyPlan_->isChecked();
    c.brokenReferencesAsNull = brokenAsNull_->isChecked();
    c.unusedMasterPlaceholders = masterPlaceholders_->isChecked();
    c.patchOnly = patchOnly_->isChecked();
    c.buildPatch = c.patchOnly || patch_->isChecked();
    app_->config().exportOptions.outputDirectory = toPath(outputDir_->text().trimmed());
    app_->settingsEdited();
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
    // Apply works on this session's export, or on the last build reopened after a scan (verified).
    const bool applicable = p && p->applySource();
    apply_->setEnabled(!busy && applicable);
    apply_->setToolTip(applicable ? QString()
                                  : QString("Export first, or after a restart Scan: the last export is reopened when it "
                                            "still matches the load order."));
    openFolder_->setEnabled(applicable);
    // Progress through the three steps of this page.
    auto step = [](bool done, int n, const QString& text) {
        return done ? QString("<span style='color:#3ddc84'>✓</span> %1").arg(text)
                    : QString("<span style='color:#8aa196'>%1</span> %2").arg(n).arg(text);
    };
    pipe_->setText(step(compiled || applicable, 1, "Compiled") + "&nbsp;&nbsp;—&nbsp;&nbsp;" +
                   step(applicable, 2, "Exported") + "&nbsp;&nbsp;—&nbsp;&nbsp;" + step(false, 3, "Apply to game"));
}

void CompilationPage::refresh()
{
    loadOptions();
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
    // The last build after a restart: reopened (verified) or why not. Reopening needs a scan.
    if (auto* p = app_->pipeline(); !p || !p->scanned()) {
        summary_->setText(summary_->text().toHtmlEscaped() +
                          "<br>After a restart, <b>Scan</b> (Dashboard) first: the last export is reopened for Apply to "
                          "Game when its files and the load order still match it, without compiling again.");
    } else if (!p->compiledOutput()) {
        if (const auto* rb = p->restoredBuild())
            summary_->setText(summary_->text().toHtmlEscaped() +
                              QString("<br>Last build reopened: <b>%1</b> (%2 compiled plugins, %3 outputs). Its files and "
                                      "the load order still match it, so Apply to Game is available without compiling "
                                      "again.%4")
                                  .arg(QString::fromStdString(rb->run).toHtmlEscaped())
                                  .arg(rb->compiled.compiledPlugins.size())
                                  .arg(rb->compiled.outputs.size())
                                  .arg(rb->notes.empty() ? QString()
                                                         : QString(" %1 change(s) since then do not affect it.")
                                                               .arg(rb->notes.size())));
        else if (!p->restoreProblem().empty())
            summary_->setText(summary_->text().toHtmlEscaped() + "<br>" +
                              QString::fromStdString(p->restoreProblem()).toHtmlEscaped() +
                              (p->restoreDetails().empty()
                                   ? QString()
                                   : " (first: " + QString::fromStdString(p->restoreDetails().front()).toHtmlEscaped() +
                                         ")"));
    }
    fillResults();
    updateButtons();
}

void CompilationPage::fillResults()
{
    const QString status = meta("build_status");
    struct {
        const char* text;
        const char* style;
    } pill = {"Not built", "background:#1c2420; color:#8aa196;"};
    if (status == "Up To Date")
        pill = {"Up to date", "background:#1a2a21; color:#3ddc84;"};
    else if (status == "Changes Detected")
        pill = {"Files changed", "background:#2a2614; color:#f5c542;"};
    else if (status == "Requires Rebuild")
        pill = {"Rebuild needed", "background:#2a1616; color:#ff5c5c;"};
    buildPill_->setText(pill.text);
    buildPill_->setStyleSheet(pill.style);
    const qint64 built = meta("build_reference").toLongLong();
    lastBuild_->setText(built > 0 ? QDateTime::fromSecsSinceEpoch(built).toString("d MMM yyyy HH:mm") : QString());

    auto shortCount = [](qint64 v) {
        if (v >= 100000)
            return QString("%1k").arg(v / 1000);
        return formatCount(v);
    };
    const QString compiled = meta("compiled_plugins");
    const QString outputs = meta("output_plugins");
    const QString patchName = meta("patch_name");
    compiledN_->setText(compiled.isEmpty() ? QString("—") : formatCount(compiled.toLongLong()));
    outputsN_->setText(outputs.isEmpty() ? QString("—")
                       : patchName.isEmpty() ? outputs
                                             : QString("%1 + %2").arg(outputs).arg(patchName.split(',').size()));
    recordsN_->setText(compiled.isEmpty() ? QString("—") : shortCount(meta("compiled_records").toLongLong()));

    // One row per output: name, a bar for its size against the largest, records and size.
    auto* list = static_cast<QVBoxLayout*>(outputsList_->layout());
    while (auto* item = list->takeAt(0)) {
        if (auto* w = item->widget())
            w->deleteLater();
        delete item;
    }
    QSqlDatabase db = app_->readDb();
    if (db.isOpen()) {
        QSqlQuery q(db);
        q.exec("SELECT output_name, record_count, size_bytes, light FROM CompilationGroups WHERE record_count > 0 "
               "ORDER BY output_index");
        struct Row {
            QString name;
            qint64 records, bytes;
            bool light;
        };
        std::vector<Row> rows;
        qint64 largest = 1;
        while (q.next()) {
            rows.push_back({q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toLongLong(), q.value(3).toInt() != 0});
            largest = std::max(largest, rows.back().bytes);
        }
        for (const auto& r : rows) {
            auto* w = new QWidget;
            w->setObjectName("AttentionRow");
            auto* h = new QHBoxLayout(w);
            h->setContentsMargins(0, 6, 0, 6);
            h->setSpacing(10);
            auto* name = new QLabel(r.name + (r.light ? "  <span style='color:#5cb8ff'>ESL</span>" : QString()));
            name->setTextFormat(Qt::RichText);
            h->addWidget(name, 1);
            auto* meter = new QProgressBar;
            meter->setTextVisible(false);
            meter->setRange(0, 1000);
            meter->setValue(int(std::max<qint64>(10, r.bytes * 1000 / largest)));
            meter->setFixedSize(90, 6);
            meter->setStyleSheet("QProgressBar { border: none; background: #1f2824; border-radius: 3px; }"
                                 "QProgressBar::chunk { border-radius: 3px; background: " +
                                 QString(r.light ? "#5cb8ff" : "#3ddc84") + "; }");
            h->addWidget(meter);
            auto* size = new QLabel(QString("%1 · %2").arg(formatCount(r.records), formatBytes(r.bytes)));
            size->setObjectName("OptionDescription");
            size->setMinimumWidth(130);
            size->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(size);
            list->addWidget(w);
        }
    }
    outputsList_->setVisible(list->count() > 0);

    const QString errors = meta("validation_pre-export_errors");
    errorsN_->setText(errors.isEmpty() ? QString("—") : formatCount(errors.toLongLong()));
    errorsN_->setStyleSheet(QString("color:%1;").arg(errors.toLongLong() > 0 ? "#ff5c5c" : "#3ddc84"));
    const QString warnings = meta("validation_pre-export_warnings");
    warningsN_->setText(warnings.isEmpty() ? QString("—") : formatCount(warnings.toLongLong()));
    keptN_->setText(formatCount(scalar("SELECT COUNT(*) FROM AutoMergePlugins WHERE status LIKE 'Kept out%'").toLongLong()));

    const auto* plan = app_->pipeline() ? app_->pipeline()->patchPlan() : nullptr;
    planPending_->setText(plan ? QString("%1 waiting").arg(formatCount(qint64(plan->pending))) : QString());
    planPending_->setVisible(plan && plan->pending > 0);
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
