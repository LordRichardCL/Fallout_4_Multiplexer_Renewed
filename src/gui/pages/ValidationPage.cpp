#include "pages/ValidationPage.hpp"

#include "Format.hpp"
#include "Theme.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <QHeaderView>
#include <QLineEdit>
#include <QTableView>
#include <QVBoxLayout>

namespace gui {

ValidationPage::ValidationPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    auto* run = makeButton("Run Validation", "Primary");
    connect(run, &QPushButton::clicked, app_, &AppController::runCompile);
    connect(app_, &AppController::busyChanged, run, [run](bool b) { run->setEnabled(!b); });
    auto* defs = makeButton("Check Record Definitions");
    defs->setToolTip("Decode every record of the supported types in your load order (base game included) and report "
                     "field layouts that look wrong. Results appear under Definitions.");
    connect(defs, &QPushButton::clicked, app_, &AppController::runDefinitionCheck);
    connect(app_, &AppController::busyChanged, defs, [defs](bool b) { defs->setEnabled(!b); });
    l->addWidget(makePageHeader("Validation Centre",
                                "Missing references, broken FormIDs, duplicate IDs, missing masters, circular "
                                "dependencies and orphaned records. Critical failures block export.",
                                {defs, run}));
    auto* cards = new QHBoxLayout;
    checks_ = new StatCard("Checks");
    passed_ = new StatCard("Passed");
    warnings_ = new StatCard("Warnings");
    errors_ = new StatCard("Errors");
    warnings_->setAccent(theme::kYellow);
    errors_->setAccent(theme::kRed);
    for (auto* c : {checks_, passed_, warnings_, errors_})
        cards->addWidget(c);
    l->addLayout(cards);
    bar_ = new StackedBar;
    l->addWidget(bar_);

    auto* filterRow = new QHBoxLayout;
    chips_ = new FilterChips({"All", "Errors", "Warnings", "Info", "Definitions"});
    search_ = new QLineEdit;
    search_->setPlaceholderText("Filter messages…");
    search_->setClearButtonEnabled(true);
    auto* copy = makeButton("Copy");
    copy->setToolTip("Copy the rows shown (with the current filter) to the clipboard");
    auto* save = makeButton("Save…");
    save->setToolTip("Save the rows shown (with the current filter) to a text file");
    filterRow->addWidget(chips_, 1);
    filterRow->addWidget(search_);
    filterRow->addWidget(copy);
    filterRow->addWidget(save);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(shownRowsAsText()); });
    connect(save, &QPushButton::clicked, this, [this] {
        QString name = chips_->current() == "Definitions" ? "DefinitionCheck.txt" : "Validation.txt";
        QString path = QFileDialog::getSaveFileName(
            this, "Save validation results",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + name, "Text (*.txt)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(shownRowsAsText().toUtf8());
    });
    l->addLayout(filterRow);

    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    model_->setStatusColumns({0});
    model_->setMonoColumns({4});
    table_->setModel(model_);
    l->addWidget(table_, 1);

    connect(chips_, &FilterChips::changed, this, [this] { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
}

void ValidationPage::refresh()
{
    qint64 checks = 0, passed = 0, warnings = 0, errors = 0;
    for (const char* stage : {"pre-export", "post-export"}) {
        QString s = stage;
        checks += meta("validation_" + s + "_checks").toLongLong();
        passed += meta("validation_" + s + "_passed").toLongLong();
        warnings += meta("validation_" + s + "_warnings").toLongLong();
        errors += meta("validation_" + s + "_errors").toLongLong();
    }
    bool have = checks > 0;
    checks_->setValue(have ? formatCount(checks) : "—");
    passed_->setValue(have ? formatCount(passed) : "—");
    warnings_->setValue(have ? formatCount(warnings) : "—");
    errors_->setValue(have ? formatCount(errors) : "—", have ? (errors ? "export blocked" : "no critical failures") : "");
    bar_->setSegments({{"Passed", double(passed), theme::kGreen},
                       {"Warnings", double(warnings), theme::kYellow},
                       {"Errors", double(errors), theme::kRed}});
    reload();
}

QString ValidationPage::shownRowsAsText()
{
    while (model_->canFetchMore())
        model_->fetchMore();
    QStringList lines;
    QStringList header;
    for (int c = 0; c < model_->columnCount(); ++c)
        header << model_->headerData(c, Qt::Horizontal).toString();
    lines << header.join('\t');
    for (int r = 0; r < model_->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < model_->columnCount(); ++c)
            cells << model_->value(r, c).toString();
        lines << cells.join('\t');
    }
    return lines.join('\n') + '\n';
}

void ValidationPage::reload()
{
    QString where = "1=1";
    QVariantList binds;
    QString chip = chips_->current();
    if (chip == "Errors")
        where += " AND severity = 'Error'";
    else if (chip == "Warnings")
        where += " AND severity = 'Warning'";
    else if (chip == "Info")
        where += " AND severity = 'Info'";
    else if (chip == "Definitions")
        where += " AND stage = 'definitions'";
    if (!search_->text().trimmed().isEmpty()) {
        where += " AND (message LIKE ? OR category LIKE ? OR plugin LIKE ?)";
        QString like = "%" + search_->text().trimmed() + "%";
        binds << like << like << like;
    }
    model_->load(app_->readDb(),
                 "SELECT severity AS Severity, category AS Check_, message AS Message, plugin AS Plugin, form_id AS "
                 "FormID, stage AS Stage FROM ValidationResults WHERE " + where +
                     " ORDER BY CASE severity WHEN 'Error' THEN 0 WHEN 'Warning' THEN 1 ELSE 2 END, id LIMIT 200000",
                 binds);
    model_->setHeaderData(1, Qt::Horizontal, "Check");
    fitColumns(table_, 640);
}

} // namespace gui
