#include "pages/ValidationPage.hpp"

#include "Format.hpp"
#include "Theme.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include <QHBoxLayout>
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
    l->addWidget(makePageHeader("Validation Centre",
                                "Missing references, broken FormIDs, duplicate IDs, missing masters, circular "
                                "dependencies and orphaned records. Critical failures block export.",
                                {run}));
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
    chips_ = new FilterChips({"All", "Errors", "Warnings", "Info"});
    search_ = new QLineEdit;
    search_->setPlaceholderText("Filter messages…");
    search_->setClearButtonEnabled(true);
    filterRow->addWidget(chips_, 1);
    filterRow->addWidget(search_);
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
