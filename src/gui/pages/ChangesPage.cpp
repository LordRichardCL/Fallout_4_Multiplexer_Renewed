#include "pages/ChangesPage.hpp"

#include "Format.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QTableView>
#include <QVBoxLayout>

namespace gui {

void fillBuildBanner(Page*, BuildStatusBanner* banner, const std::function<QString(const QString&)>& meta)
{
    QString status = meta("build_status");
    auto when = [&](const QString& key) {
        qint64 t = meta(key).toLongLong();
        return t > 0 ? QDateTime::fromSecsSinceEpoch(t).toString("yyyy-MM-dd HH:mm") : QString("never");
    };
    QString detail;
    if (status.isEmpty() || status == "Not Built")
        detail = "No compiled output has been exported yet. Scan, analyse and compile to create the first build.";
    else if (status == "Up To Date")
        detail = QString("The compiled output matches the installed mods. Last build %1, checked %2.")
                     .arg(when("build_reference"), when("build_checked"));
    else if (status == "Changes Detected")
        detail = QString("Assets changed since the build on %1, but the compiled records are still valid.")
                     .arg(when("build_reference"));
    else
        detail = QString("The compiled output no longer represents the installed mods (built %1). %2")
                     .arg(when("build_reference"), meta("changes_reasons").split('\n').mid(0, 3).join("; "));
    QStringList counts;
    if (status == "Changes Detected" || status == "Requires Rebuild") {
        counts << QString("New mods: %1").arg(meta("changes_new_mods"))
               << QString("Removed mods: %1").arg(meta("changes_removed_mods"))
               << QString("Updated mods: %1").arg(meta("changes_updated_mods"))
               << QString("Plugins added / removed / updated: %1 / %2 / %3")
                      .arg(meta("changes_added_plugins"), meta("changes_removed_plugins"), meta("changes_updated_plugins"))
               << QString("Archives: %1").arg(meta("changes_archives"))
               << QString("Load order moves: %1").arg(meta("changes_load_order"));
    }
    banner->setStatus(status, detail, counts);
}

ChangesPage::ChangesPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    auto* check = makeButton("Check Now");
    auto* rebuild = makeButton("Rebuild", "Primary");
    connect(check, &QPushButton::clicked, app_, &AppController::runCheck);
    connect(rebuild, &QPushButton::clicked, app_, &AppController::runRebuild);
    for (auto* b : {check, rebuild})
        connect(app_, &AppController::busyChanged, b, [b](bool busy) { b->setEnabled(!busy); });
    l->addWidget(makePageHeader("Mod Space Changes",
                                "Everything installed, removed, updated or reordered since the last build, and what it "
                                "means for the compiled output. Changes marked as affecting the build require recompilation.",
                                {check, rebuild}));
    banner_ = new BuildStatusBanner;
    l->addWidget(banner_);
    types_ = new QLabel;
    types_->setWordWrap(true);
    types_->setObjectName("PageSubtitle");
    l->addWidget(types_);
    chips_ = new FilterChips({"All", "Affects Build", "Mods", "Plugins", "Archives", "Load Order", "Outputs", "Settings"});
    l->addWidget(chips_);
    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    model_->setStatusColumns({4});
    table_->setModel(model_);
    l->addWidget(table_, 1);
    connect(chips_, &FilterChips::changed, this, [this] { reload(); });
}

void ChangesPage::refresh()
{
    fillBuildBanner(this, banner_, [this](const QString& k) { return meta(k); });
    QString affected = meta("changes_affected_types");
    QString unaffected = meta("changes_unaffected_types");
    types_->setText(affected.isEmpty() ? QString()
                                       : QString("<b>Affected record types:</b> %1<br><b>Unaffected compiled types:</b> %2")
                                             .arg(affected.toHtmlEscaped(), unaffected.isEmpty() ? "none" : unaffected.toHtmlEscaped()));
    types_->setVisible(!affected.isEmpty());
    reload();
}

void ChangesPage::reload()
{
    QString chip = chips_->current();
    QString where = "1=1";
    if (chip == "Affects Build")
        where = "affects_build = 1";
    else if (chip == "Mods")
        where = "category = 'Mod'";
    else if (chip == "Plugins")
        where = "category = 'Plugin'";
    else if (chip == "Archives")
        where = "category = 'Archive'";
    else if (chip == "Load Order")
        where = "category = 'Load Order'";
    else if (chip == "Outputs")
        where = "category = 'Output'";
    else if (chip == "Settings")
        where = "category = 'Settings'";
    model_->load(app_->readDb(),
                 "SELECT category AS Category, kind AS Change, name AS Name, detail AS Details, "
                 "CASE affects_build WHEN 1 THEN 'Requires Rebuild' ELSE 'Asset layer only' END AS Impact "
                 "FROM Changes WHERE " + where + " ORDER BY affects_build DESC, category, name");
    fitColumns(table_, 520);
}

} // namespace gui
