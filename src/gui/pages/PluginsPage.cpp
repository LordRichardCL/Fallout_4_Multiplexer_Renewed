#include "pages/PluginsPage.hpp"

#include "Format.hpp"
#include "Theme.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QStandardPaths>
#include <QLineEdit>
#include <QSet>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableView>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace gui {

PluginsPage::PluginsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Search plugins…");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(260);
    auto* exportBtn = makeButton("Export…");
    exportBtn->setToolTip("Save the blocking record types and every plugin's type, flags, record types, masters and "
                          "eligibility to a text file");
    l->addWidget(makePageHeader("Plugin Explorer",
                                "Every installed plugin, its type, records, masters and whether it can be compiled.",
                                {search_, exportBtn}));
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(
            this, "Export plugin report",
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/PluginReport.txt", "Text (*.txt)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(exportText().toUtf8());
    });
    chips_ = new FilterChips({"All", "ESM", "ESP", "ESL", "Generated", "Conflicting", "Disabled", "Eligible", "Ineligible",
                             "Blocking Types"});
    chips_->setToolTip("Blocking Types: record types the compiler does not support yet, and how many plugins "
                       "supporting each one would make compilable");
    l->addWidget(chips_);

    auto* split = new QSplitter(Qt::Horizontal);
    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    table_->setModel(model_);
    split->addWidget(table_);

    auto* side = new QWidget;
    auto* sl = new QVBoxLayout(side);
    sl->setContentsMargins(8, 0, 0, 0);
    details_ = new QTextBrowser;
    details_->setOpenExternalLinks(false);
    sl->addWidget(details_, 1);
    exclude_ = makeButton("Exclude from compilation");
    exclude_->setEnabled(false);
    sl->addWidget(exclude_);
    split->addWidget(side);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    l->addWidget(split, 1);

    connect(chips_, &FilterChips::changed, this, [this] { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
    connect(table_, &QTableView::clicked, this, [this](const QModelIndex& i) { showDetails(i.row()); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showDetails(i.row()); });
    connect(exclude_, &QPushButton::clicked, this, [this] {
        if (selectedPlugin_.isEmpty())
            return;
        bool excluded = app_->excludedPlugins().contains(selectedPlugin_, Qt::CaseInsensitive);
        app_->setExcluded(selectedPlugin_, !excluded);
    });
}

void PluginsPage::refresh()
{
    reload();
}

void PluginsPage::reload()
{
    QString chip = chips_->current();
    generatedMode_ = chip == "Generated";
    blockersMode_ = chip == "Blocking Types";
    QVariantList binds;
    QString sql;
    QString like = "%" + search_->text().trimmed() + "%";
    if (blockersMode_) {
        sql = "SELECT signature AS Type, unlocks AS 'Would Unlock', sole_blocker AS 'Only Blocker In', "
              "plugins AS 'Found In', records AS Records, name AS Name, category AS Category, support AS Support "
              "FROM EligibilityBlockers WHERE signature LIKE ? OR name LIKE ? OR category LIKE ? "
              "ORDER BY unlocks DESC, plugins DESC, signature";
        binds << like << like << like;
        model_->setNumberColumns({1, 2, 3, 4});
        model_->setBytesColumns({});
        model_->setStatusColumns({});
    } else if (generatedMode_) {
        sql = "SELECT output_index + 1 AS '#', output_name AS 'Plugin Name', CASE light WHEN 1 THEN 'ESL Flagged ESP' "
              "ELSE 'ESP' END AS Type, record_count AS Records, new_records AS 'New', overrides AS Overrides, "
              "size_bytes AS Size, masters AS Masters FROM CompilationGroups WHERE record_count > 0 AND output_name LIKE ? "
              "ORDER BY output_index";
        binds << like;
        model_->setNumberColumns({3, 4, 5});
        model_->setBytesColumns({6});
        model_->setStatusColumns({});
    } else {
        QString where = "name LIKE ?";
        binds << like;
        if (chip == "ESM")
            where += " AND type = 'ESM'";
        else if (chip == "ESP")
            where += " AND type = 'ESP'";
        else if (chip == "ESL")
            where += " AND type IN ('ESL', 'ESL Flagged ESP', 'ESL Flagged ESM')";
        else if (chip == "Conflicting")
            where += " AND conflict_count > 0";
        else if (chip == "Disabled")
            where += " AND active = 0";
        else if (chip == "Eligible")
            where += " AND eligibility = 'Eligible'";
        else if (chip == "Ineligible")
            where += " AND eligibility IN ('Ineligible', 'Error')";
        sql = "SELECT load_order_hex AS '#', name AS 'Plugin Name', type AS Type, record_count AS 'Record Count', "
              "conflict_count AS 'Conflict Count', master_count AS 'Master Count', status AS Status, "
              "eligibility AS Eligibility, source_mod AS Mod, id FROM Plugins WHERE " +
              where + " ORDER BY active DESC, CASE WHEN load_order < 0 THEN 99999 ELSE load_order END, name";
        model_->setNumberColumns({3, 4, 5});
        model_->setBytesColumns({});
        model_->setStatusColumns({6, 7});
    }
    model_->setMonoColumns({0});
    model_->load(app_->readDb(), sql, binds);
    if (!generatedMode_ && !blockersMode_)
        table_->hideColumn(9);
    else
        table_->showColumn(9);
    fitColumns(table_);
    if (blockersMode_) {
        QString held = meta("blocked_by_types"), all = meta("unlocked_by_all_types");
        details_->setHtml(QString("<h3 style='color:#3ddc84'>What blocks compilation</h3>"
                                  "<p>%1 plugins are held back only by record types the compiler does not support yet. "
                                  "Supporting every type listed would make %2 more plugins compilable.</p>"
                                  "<p><b>Would Unlock</b>: plugins that become compilable if only this type is added, "
                                  "including plugins kept in the load order because something depends on them.<br>"
                                  "<b>Only Blocker In</b>: plugins where this is the only unsupported type.<br>"
                                  "<b>Found In</b>: plugins that contain the type.</p>"
                                  "<p style='color:#8aa196'>Select a type to see the plugins it blocks.</p>")
                              .arg(held.isEmpty() ? "0" : held, all.isEmpty() ? "0" : all));
        exclude_->setEnabled(false);
        return;
    }
    details_->setHtml("<p style='color:#8aa196'>Select a plugin to see its details.</p>");
    exclude_->setEnabled(false);
}

QString PluginsPage::exportText()
{
    QSqlDatabase db = app_->readDb();
    QStringList out;
    out << "Fallout 4 Multiplexer Renewed - plugin report"
        << QString("Plugins held back only by record types: %1; supporting every blocking type would unlock %2")
               .arg(meta("blocked_by_types"), meta("unlocked_by_all_types"))
        << "";

    // Blocking types, each with the plugins it blocks and what else those plugins need.
    QSet<QString> unsupported;
    out << "[Blocking record types]"
        << "Type\tName\tWould Unlock\tOnly Blocker In\tFound In\tRecords\tCategory\tSupport";
    QSqlQuery b(db);
    b.exec("SELECT signature, name, unlocks, sole_blocker, plugins, records, category, support FROM EligibilityBlockers "
           "ORDER BY unlocks DESC, plugins DESC, signature");
    QList<QStringList> blockers;
    while (b.next()) {
        QStringList row;
        for (int c = 0; c < 8; ++c)
            row << b.value(c).toString();
        unsupported.insert(row[0]);
        blockers << row;
        out << row.join('\t');
    }
    out << "";
    QList<QPair<QString, QString>> plugins; // name, record types
    QSqlQuery pl(db);
    pl.exec("SELECT name, record_types FROM Plugins WHERE active = 1 AND eligibility IN ('Eligible', 'Ineligible') "
            "ORDER BY load_order");
    while (pl.next())
        plugins.append({pl.value(0).toString(), pl.value(1).toString()});
    for (const auto& row : blockers) {
        out << "[" + row[0] + " - " + row[1] + " blocks]" << "Plugin\tAll unsupported types it contains";
        for (const auto& [name, types] : plugins) {
            QStringList needs;
            bool has = false;
            for (const auto& t : types.split(", ", Qt::SkipEmptyParts)) {
                QString sig = t.section(' ', 0, 0);
                if (unsupported.contains(sig))
                    needs << t;
                has |= sig == row[0];
            }
            if (has)
                out << name + "\t" + needs.join(", ");
        }
        out << "";
    }

    // Every plugin.
    out << "[Plugins]"
        << "Load Order\tPlugin\tType\tHeader Flags\tActive\tEligibility\tReason\tRecords\tNew\tOverrides\t"
           "Record Types\tMod\tMasters\tAuthor";
    QSqlQuery all(db);
    all.exec("SELECT p.load_order_hex, p.name, p.type, printf('0x%08X', p.flags), p.active, p.eligibility, "
             "p.eligibility_reason, p.record_count, p.new_records, p.override_count, p.record_types, p.source_mod, "
             "(SELECT group_concat(master_name, ', ') FROM Masters m WHERE m.plugin_id = p.id), p.author "
             "FROM Plugins p ORDER BY p.active DESC, CASE WHEN p.load_order < 0 THEN 99999 ELSE p.load_order END, "
             "p.name");
    while (all.next()) {
        QStringList row;
        for (int c = 0; c < 14; ++c) {
            QString v = all.value(c).toString();
            if (c == 4)
                v = all.value(c).toInt() ? "yes" : "no";
            row << v.replace('\t', ' ').replace('\n', ' ');
        }
        out << row.join('\t');
    }
    return out.join('\n') + '\n';
}

void PluginsPage::showBlocker(int row)
{
    QString sig = model_->value(row, 0).toString();
    QSet<QString> unsupported;
    {
        QSqlQuery b(app_->readDb());
        b.exec("SELECT signature FROM EligibilityBlockers");
        while (b.next())
            unsupported.insert(b.value(0).toString());
    }
    QSqlQuery q(app_->readDb());
    q.prepare("SELECT name, record_types, eligibility FROM Plugins WHERE active = 1 AND eligibility IN ('Eligible', "
              "'Ineligible') AND (', ' || record_types) LIKE ? ORDER BY load_order");
    q.addBindValue("%, " + sig + " %");
    q.exec();
    QStringList rows;
    while (q.next()) {
        QStringList blockers;
        for (const auto& t : q.value(1).toString().split(", ")) {
            QString type = t.section(' ', 0, 0);
            if (unsupported.contains(type))
                blockers << (type == sig ? "<b>" + type + "</b>" : type);
        }
        rows << QString("%1 <span style='color:#8aa196'>— needs %2</span>")
                    .arg(q.value(0).toString().toHtmlEscaped(), blockers.join(", "));
    }
    details_->setHtml(QString("<h3 style='color:#3ddc84'>%1 — %2</h3><p style='color:#8aa196'>%5 · %6</p>"
                              "<p>%3 active plugins contain this type. Each line lists every "
                              "unsupported type that plugin needs.</p><p>%4</p>")
                          .arg(sig.toHtmlEscaped(), model_->value(row, 5).toString().toHtmlEscaped())
                          .arg(rows.size())
                          .arg(rows.join("<br>"))
                          .arg(model_->value(row, 6).toString().toHtmlEscaped(),
                               model_->value(row, 7).toString().toHtmlEscaped()));
    exclude_->setEnabled(false);
}

void PluginsPage::showDetails(int row)
{
    if (row < 0)
        return;
    if (blockersMode_) {
        showBlocker(row);
        return;
    }
    if (generatedMode_) {
        QString name = model_->value(row, 1).toString();
        QString html = QString("<h2 style='color:#3ddc84'>%1</h2><p>Generated by Fallout 4 Multiplexer Renewed.</p>"
                               "<p><b>Records:</b> %2<br><b>New FormIDs:</b> %3<br><b>Overrides:</b> %4</p>"
                               "<p><b>Masters:</b><br>%5</p>")
                           .arg(name.toHtmlEscaped(), model_->data(model_->index(row, 3)).toString(),
                                model_->data(model_->index(row, 4)).toString(), model_->data(model_->index(row, 5)).toString(),
                                model_->value(row, 7).toString().toHtmlEscaped().replace(", ", "<br>"));
        details_->setHtml(html);
        exclude_->setEnabled(false);
        return;
    }
    qint64 id = model_->value(row, 9).toLongLong();
    QSqlDatabase db = app_->readDb();
    QSqlQuery q(db);
    q.prepare("SELECT name, type, author, description, source_mod, path, record_count, new_records, override_count, "
              "injected_count, itm_count, deleted_count, conflict_count, hard_conflict_count, eligibility, "
              "eligibility_reason, archives, record_types, load_order_hex, active, file_size, localized, base_game "
              "FROM Plugins WHERE id = ?");
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return;
    selectedPlugin_ = q.value(0).toString();
    auto v = [&](int i) { return q.value(i).toString().toHtmlEscaped(); };
    QString elig = q.value(14).toString();
    QColor ec = theme::statusText(elig);
    QString html;
    html += QString("<h2 style='color:#3ddc84;margin-bottom:2px'>%1</h2>").arg(v(0));
    html += QString("<p style='color:#8aa196;margin-top:0'>%1 · load order %2 · %3%4</p>")
                .arg(v(1), v(18).isEmpty() ? "inactive" : v(18), formatBytes(q.value(20).toLongLong()),
                     q.value(21).toInt() ? " · localized" : "");
    if (!q.value(3).toString().isEmpty())
        html += "<p>" + v(3) + "</p>";
    html += QString("<p><b>Author:</b> %1<br><b>Mod:</b> %2<br><b>Path:</b> <span style='font-family:monospace'>%3</span></p>")
                .arg(v(2), v(4), v(5));
    html += QString("<p><b>Records:</b> %1 (new %2, overrides %3, injected %4, ITM %5, deleted %6)<br>"
                    "<b>Conflicts:</b> %7 (%8 hard)</p>")
                .arg(formatCount(q.value(6).toLongLong()), formatCount(q.value(7).toLongLong()),
                     formatCount(q.value(8).toLongLong()), formatCount(q.value(9).toLongLong()),
                     formatCount(q.value(10).toLongLong()), formatCount(q.value(11).toLongLong()),
                     formatCount(q.value(12).toLongLong()), formatCount(q.value(13).toLongLong()));
    html += QString("<p><b>Compilation:</b> <span style='color:%1'>%2</span><br>%3</p>")
                .arg(ec.isValid() ? ec.name() : "#d7e4dc", elig.toHtmlEscaped(),
                     v(15).replace("; ", "<br>"));
    if (!q.value(17).toString().isEmpty())
        html += "<p><b>Record types:</b><br><span style='font-family:monospace'>" + v(17) + "</span></p>";
    if (!q.value(16).toString().isEmpty())
        html += "<p><b>Archives:</b> " + v(16) + "</p>";

    QSqlQuery m(db);
    m.prepare("SELECT master_name, missing FROM Masters WHERE plugin_id = ? ORDER BY position");
    m.addBindValue(id);
    m.exec();
    QStringList masters;
    while (m.next())
        masters << (m.value(1).toInt() ? "<span style='color:#ff5c5c'>" + m.value(0).toString().toHtmlEscaped() + " (missing)</span>"
                                        : m.value(0).toString().toHtmlEscaped());
    html += "<p><b>Masters (" + QString::number(masters.size()) + "):</b><br>" + (masters.isEmpty() ? "none" : masters.join("<br>")) + "</p>";

    QSqlQuery d(db);
    d.prepare("SELECT p.name FROM Masters m JOIN Plugins p ON p.id = m.plugin_id WHERE m.master_plugin_id = ? "
              "AND p.active = 1 ORDER BY p.load_order");
    d.addBindValue(id);
    d.exec();
    QStringList dependents;
    while (d.next())
        dependents << d.value(0).toString().toHtmlEscaped();
    html += "<p><b>Required by (" + QString::number(dependents.size()) + "):</b><br>" +
            (dependents.isEmpty() ? "none" : dependents.mid(0, 50).join("<br>")) + "</p>";
    details_->setHtml(html);

    bool excluded = app_->excludedPlugins().contains(selectedPlugin_, Qt::CaseInsensitive);
    exclude_->setText(excluded ? "Allow compilation" : "Exclude from compilation");
    exclude_->setEnabled(!app_->busy() && q.value(19).toInt() && !q.value(22).toInt());
}

} // namespace gui
