#include "pages/CompilationPage.hpp"

#include "Format.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include "engine/Exporter.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
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
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
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
    apply_ = makeButton("Apply to MO2 Profile…");
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
    outputDir_->setPlaceholderText("Default: MO2 mod folder \"Fallout 4 Multiplexer Output\", or Data");
    auto* browse = makeButton("…");
    auto* dirRow = new QHBoxLayout;
    dirRow->addWidget(outputDir_, 1);
    dirRow->addWidget(browse);
    form->addRow("Output plugins", outputs_);
    form->addRow("Name prefix", prefix_);
    form->addRow("Output folder", dirRow);
    ol->addWidget(outBox);

    auto* allocBox = new QGroupBox("Record allocation");
    auto* al = new QVBoxLayout(allocBox);
    strategy_ = new QComboBox;
    strategy_->addItem("Strategy A — By record type", int(AllocationStrategy::ByRecordType));
    strategy_->addItem("Strategy B — Balanced size", int(AllocationStrategy::Balanced));
    strategy_->addItem("Strategy C — User defined", int(AllocationStrategy::UserDefined));
    al->addWidget(strategy_);
    rules_ = new QPlainTextEdit;
    rules_->setPlaceholderText("One rule per line: PATTERN=OUTPUT\nWEAP=3\nMyWeaponPack.esp=5");
    rules_->setMaximumHeight(110);
    al->addWidget(rules_);
    auto* note = new QLabel("Records are always placed after everything they reference, so dependencies can move "
                            "records to a later output.");
    note->setObjectName("PageSubtitle");
    note->setWordWrap(true);
    al->addWidget(note);
    ol->addWidget(allocBox);

    auto* mergeBox = new QGroupBox("Resolution");
    auto* ml = new QVBoxLayout(mergeBox);
    merge_ = new QCheckBox("Merge compatible edits (keyword lists, leveled lists, form lists, different fields)");
    light_ = new QCheckBox("ESL-flag outputs that fit the light plugin range (≤ 2048 new records)");
    heuristic_ = new QCheckBox("Also rewrite probable FormIDs in undescribed data (advanced)");
    for (auto* c : {merge_, light_, heuristic_}) {
        c->setStyleSheet("QCheckBox { spacing: 8px; }");
        ml->addWidget(c);
    }
    ol->addWidget(mergeBox);
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
    recordsView_ = new QTableView;
    mappingsView_ = new QTableView;
    outputsModel_ = new SqlModel(this);
    recordsModel_ = new SqlModel(this);
    mappingsModel_ = new SqlModel(this);
    for (auto [v, m] : {std::pair{outputsView_, outputsModel_}, std::pair{recordsView_, recordsModel_},
                        std::pair{mappingsView_, mappingsModel_}}) {
        styleTable(v);
        v->setModel(m);
    }
    tabs_->addTab(outputsView_, "Output Plugins");
    tabs_->addTab(recordsView_, "Generated Records");
    tabs_->addTab(mappingsView_, "FormID Mappings");
    rl->addWidget(tabs_, 1);
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
        auto answer = QMessageBox::question(
            this, "Apply to profile",
            "This rewrites the active profile's plugins.txt (and loadorder.txt) so that compiled plugins are disabled "
            "and the output plugins are enabled. Backups of both files are created first.\n\nContinue?");
        if (answer != QMessageBox::Yes)
            return;
        QString msg;
        bool ok = app_->applyToProfile(&msg);
        if (ok)
            QMessageBox::information(this, "Apply to profile", msg);
        else
            QMessageBox::warning(this, "Apply to profile", msg);
    });
    connect(openFolder_, &QPushButton::clicked, this, [this] {
        auto* p = app_->pipeline();
        if (p && p->exportResult())
            QDesktopServices::openUrl(QUrl::fromLocalFile(toQString(p->exportResult()->directory)));
    });
    connect(app_, &AppController::taskStarted, this, [this](const QString&) { stages_->reset(); });
    connect(app_, &AppController::stageStarted, stages_, &StageIndicator::activate);
    connect(app_, &AppController::taskFinished, this, [this](const QString&, bool ok, const QString&) {
        stages_->finish(ok);
        updateButtons();
    });
    connect(app_, &AppController::busyChanged, this, [this] { updateButtons(); });
    connect(app_, &AppController::settingsChanged, this, [this] { loadOptions(); });
    connect(strategy_, &QComboBox::currentIndexChanged, this,
            [this] { rules_->setEnabled(strategy_->currentData().toInt() == int(AllocationStrategy::UserDefined)); });
    loadOptions();
}

void CompilationPage::loadOptions()
{
    const auto& c = app_->config().compile;
    outputs_->setValue(std::clamp(c.outputCount, fo4mx::CompileOptions::kMinOutputs, fo4mx::CompileOptions::kMaxOutputs));
    prefix_->setText(qs(c.outputPrefix));
    strategy_->setCurrentIndex(strategy_->findData(int(c.strategy)));
    QStringList rules;
    for (const auto& r : c.rules)
        rules << qs(r.pattern) + "=" + QString::number(r.output);
    rules_->setPlainText(rules.join("\n"));
    rules_->setEnabled(c.strategy == AllocationStrategy::UserDefined);
    merge_->setChecked(c.mergeCompatible);
    light_->setChecked(c.lightOutputsWhenPossible);
    heuristic_->setChecked(c.rewriteHeuristicReferences);
    outputDir_->setText(toQString(app_->config().exportOptions.outputDirectory));
}

void CompilationPage::storeOptions()
{
    auto& c = app_->config().compile;
    c.outputCount = outputs_->value();
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
    c.rewriteHeuristicReferences = heuristic_->isChecked();
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
    bool mo2 = p && p->scanned() && p->session().install.config.kind == fo4mx::InstallKind::ModOrganizer2;
    apply_->setEnabled(!busy && mo2 && p->exportResult().has_value());
    openFolder_->setEnabled(p && p->exportResult().has_value());
}

void CompilationPage::refresh()
{
    QSqlDatabase db = app_->readDb();
    outputsModel_->setNumberColumns({1, 2, 3});
    outputsModel_->setBytesColumns({4});
    outputsModel_->load(db,
                        "SELECT output_name AS Plugin, record_count AS Records, new_records AS 'New FormIDs', overrides AS "
                        "Overrides, size_bytes AS Size, CASE light WHEN 1 THEN 'Yes' ELSE 'No' END AS Light, masters AS "
                        "Masters FROM CompilationGroups WHERE record_count > 0 ORDER BY output_index");
    recordsModel_->setMonoColumns({1});
    recordsModel_->setBytesColumns({7});
    recordsModel_->load(db,
                        "SELECT output_plugin AS Output, form_id AS FormID, signature AS Type, editor_id AS 'Editor ID', "
                        "CASE is_new WHEN 1 THEN 'New' ELSE 'Override' END AS Kind, CASE merged WHEN 1 THEN 'Merged' ELSE "
                        "'' END AS Merge, p.name AS 'Source Winner', size_bytes AS Size, contributors AS Contributors "
                        "FROM GeneratedRecords g LEFT JOIN Plugins p ON p.id = g.winner_plugin_id ORDER BY output_index, "
                        "signature, form_id LIMIT 500000");
    recordsModel_->setStatusColumns({5});
    mappingsModel_->setMonoColumns({1, 3});
    mappingsModel_->load(db,
                         "SELECT source_plugin AS 'Source Plugin', printf('%06X', source_object_id) AS 'Source Object', "
                         "output_plugin AS Output, printf('%06X', output_object_id) AS 'New Object', signature AS Type, "
                         "editor_id AS 'Editor ID' FROM FormMappings ORDER BY source_plugin, source_object_id LIMIT 500000");
    for (auto* v : {outputsView_, recordsView_, mappingsView_})
        fitColumns(v);

    QString compiled = meta("compiled_plugins");
    if (compiled.isEmpty()) {
        summary_->setText("Not compiled yet. Options on the left are saved with your settings.");
    } else {
        summary_->setText(QString("%1 plugins compiled into %2 output plugins · %3 records written · validation: %4 "
                                  "checks, %5 warnings, %6 errors")
                              .arg(compiled, meta("output_plugins"), formatCount(meta("compiled_records").toLongLong()),
                                   formatCount(meta("validation_pre-export_checks").toLongLong()),
                                   meta("validation_pre-export_warnings"), meta("validation_pre-export_errors")));
    }
    updateButtons();
}

} // namespace gui
