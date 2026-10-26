#include "pages/ConflictsPage.hpp"

#include "Theme.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace gui {

ConflictsPage::ConflictsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Editor ID, FormID or plugin…");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(260);
    signature_ = new QComboBox;
    signature_->setMinimumWidth(110);
    l->addWidget(makePageHeader("Conflict Centre",
                                "Which plugin wins each record, why it wins, and what every other plugin tried to change.",
                                {signature_, search_}));
    chips_ = new FilterChips({"All", "Hard Conflicts", "Compatible Merges", "Simple Overrides", "Critical"});
    l->addWidget(chips_);

    auto* split = new QSplitter(Qt::Vertical);
    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    table_->setModel(model_);
    split->addWidget(table_);

    auto* detail = new QWidget;
    auto* dl = new QVBoxLayout(detail);
    dl->setContentsMargins(0, 8, 0, 0);
    title_ = new QLabel;
    title_->setObjectName("SectionTitle");
    chain_ = new QLabel;
    chain_->setWordWrap(true);
    chain_->setTextFormat(Qt::RichText);
    summary_ = new QLabel;
    summary_->setObjectName("PageSubtitle");
    summary_->setWordWrap(true);
    dl->addWidget(title_);
    dl->addWidget(chain_);
    dl->addWidget(summary_);

    fields_ = new QTableWidget;
    fields_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fields_->setSelectionMode(QAbstractItemView::NoSelection);
    fields_->verticalHeader()->setDefaultSectionSize(26);
    fields_->verticalHeader()->hide();
    fields_->horizontalHeader()->setStretchLastSection(true);
    fields_->setWordWrap(false);
    dl->addWidget(fields_, 1);

    auto* bottom = new QHBoxLayout;
    auto* legend = new QLabel("<span style='color:#3ddc84'>■</span> Winning value &nbsp; "
                              "<span style='color:#ff5c5c'>■</span> Replaced value &nbsp; "
                              "<span style='color:#f5c542'>■</span> Merged value &nbsp; "
                              "<span style='color:#5cb8ff'>■</span> Compiled result");
    bottom->addWidget(legend, 1);
    bottom->addWidget(new QLabel("Winner when compiling:"));
    resolution_ = new QComboBox;
    resolution_->setMinimumWidth(240);
    bottom->addWidget(resolution_);
    dl->addLayout(bottom);
    resolutionNote_ = new QLabel;
    resolutionNote_->setObjectName("PageSubtitle");
    dl->addWidget(resolutionNote_);
    split->addWidget(detail);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    l->addWidget(split, 1);

    connect(chips_, &FilterChips::changed, this, [this] { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
    connect(signature_, &QComboBox::currentTextChanged, this, [this] { reload(); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showConflict(i.row()); });
    connect(resolution_, &QComboBox::activated, this, [this] { applyResolution(); });
}

void ConflictsPage::refresh()
{
    QString current = signature_->currentText();
    signature_->blockSignals(true);
    signature_->clear();
    signature_->addItem("All types");
    QSqlDatabase db = app_->readDb();
    if (db.isOpen()) {
        QSqlQuery q(db);
        q.exec("SELECT DISTINCT signature FROM Conflicts ORDER BY signature");
        while (q.next())
            signature_->addItem(q.value(0).toString());
    }
    int idx = signature_->findText(current);
    signature_->setCurrentIndex(idx >= 0 ? idx : 0);
    signature_->blockSignals(false);
    reload();
}

void ConflictsPage::reload()
{
    QString where = "1=1";
    QVariantList binds;
    QString chip = chips_->current();
    if (chip == "Hard Conflicts")
        where += " AND conflict_type = 3";
    else if (chip == "Compatible Merges")
        where += " AND conflict_type = 2";
    else if (chip == "Simple Overrides")
        where += " AND conflict_type = 1";
    else if (chip == "Critical")
        where += " AND severity = 'Critical'";
    if (signature_->currentIndex() > 0) {
        where += " AND signature = ?";
        binds << signature_->currentText();
    }
    QString s = search_->text().trimmed();
    if (!s.isEmpty()) {
        where += " AND (editor_id LIKE ? OR form_id LIKE ? OR chain LIKE ?)";
        binds << "%" + s + "%" << s + "%" << "%" + s + "%";
    }
    QString sql = "SELECT form_id AS FormID, signature AS Type, editor_id AS 'Editor ID', type_name AS Conflict, "
                  "severity AS Severity, p.name AS 'Winning Plugin', chain_length AS Plugins, summary AS Summary, c.id, "
                  "origin_plugin_id, object_id FROM Conflicts c LEFT JOIN Plugins p ON p.id = c.winner_plugin_id WHERE " +
                  where + " ORDER BY conflict_type DESC, CASE severity WHEN 'Critical' THEN 0 ELSE 1 END, form_id";
    model_->setMonoColumns({0});
    model_->setStatusColumns({4});
    model_->setNumberColumns({6});
    model_->load(app_->readDb(), sql, binds);
    for (int c : {8, 9, 10})
        table_->hideColumn(c);
    fitColumns(table_);
    conflictId_ = -1;
    title_->setText(model_->rowCount() ? "Select a conflict" : "No conflicts to show");
    chain_->clear();
    summary_->clear();
    fields_->clear();
    fields_->setRowCount(0);
    fields_->setColumnCount(0);
    resolution_->clear();
    resolution_->setEnabled(false);
    resolutionNote_->clear();
}

void ConflictsPage::showConflict(int row)
{
    if (row < 0)
        return;
    conflictId_ = model_->value(row, 8).toLongLong();
    QSqlDatabase db = app_->readDb();

    QMap<qint64, QString> pluginNames;
    {
        QSqlQuery q(db);
        q.exec("SELECT id, name FROM Plugins");
        while (q.next())
            pluginNames[q.value(0).toLongLong()] = q.value(1).toString();
    }
    originPlugin_ = pluginNames.value(model_->value(row, 9).toLongLong());
    objectId_ = model_->value(row, 10).toUInt();

    QString edid = model_->value(row, 2).toString();
    title_->setText(QString("%1  [%2:%3]").arg(edid.isEmpty() ? "(no editor id)" : edid, model_->value(row, 1).toString(),
                                              model_->value(row, 0).toString()));
    summary_->setText(model_->value(row, 7).toString());

    // Columns: one per plugin in the chain, then the compiled result.
    QSqlQuery q(db);
    q.prepare("SELECT field_index, field, hard, position, plugin_id, value, status FROM ConflictFields "
              "WHERE conflict_id = ? ORDER BY field_index, position");
    q.addBindValue(conflictId_);
    q.exec();
    struct Cell {
        QString value, status;
    };
    QMap<int, QString> fieldNames;
    QMap<int, bool> hard;
    QMap<int, QMap<int, Cell>> cells;
    QMap<int, qint64> chainPlugins;
    while (q.next()) {
        int fi = q.value(0).toInt();
        int pos = q.value(3).toInt();
        fieldNames[fi] = q.value(1).toString();
        hard[fi] = q.value(2).toBool();
        cells[fi][pos] = {q.value(5).toString(), q.value(6).toString()};
        if (pos >= 0)
            chainPlugins[pos] = q.value(4).toLongLong();
    }
    QStringList chainNames;
    {
        QSqlQuery c(db);
        c.prepare("SELECT chain, winner_plugin_id FROM Conflicts WHERE id = ?");
        c.addBindValue(conflictId_);
        if (c.exec() && c.next())
            chainNames = c.value(0).toString().split(" > ");
    }
    QStringList pretty;
    for (int i = 0; i < chainNames.size(); ++i) {
        bool last = i == chainNames.size() - 1;
        pretty << (last ? "<b style='color:#3ddc84'>" + chainNames[i].toHtmlEscaped() + "</b> (winner)"
                        : chainNames[i].toHtmlEscaped());
    }
    chain_->setText("<span style='color:#8aa196'>Override chain:</span> " + pretty.join(" &nbsp;›&nbsp; "));

    int columns = static_cast<int>(chainNames.size());
    fields_->clear();
    fields_->setColumnCount(columns + 2);
    QStringList headers{"Field"};
    for (const auto& n : chainNames)
        headers << n;
    headers << "Compiled Result";
    fields_->setHorizontalHeaderLabels(headers);
    fields_->setRowCount(static_cast<int>(fieldNames.size()));
    int r = 0;
    for (auto it = fieldNames.begin(); it != fieldNames.end(); ++it, ++r) {
        auto* name = new QTableWidgetItem((hard[it.key()] ? "⚠ " : "") + it.value());
        if (hard[it.key()]) {
            name->setForeground(theme::kRed);
            name->setToolTip("Hard conflict: several plugins changed this field differently");
        }
        fields_->setItem(r, 0, name);
        for (int pos = 0; pos < columns; ++pos) {
            Cell c = cells[it.key()].value(pos);
            auto* item = new QTableWidgetItem(c.value);
            item->setToolTip(c.value + "\n\n" + c.status);
            QColor tint = theme::statusTint(c.status);
            if (tint.isValid())
                item->setBackground(tint);
            if (c.status == "Base" || c.status == "Same" || c.status == "Absent")
                item->setForeground(theme::kMuted);
            fields_->setItem(r, pos + 1, item);
        }
        Cell result = cells[it.key()].value(-1);
        auto* item = new QTableWidgetItem(result.value);
        item->setToolTip(result.value);
        item->setBackground(theme::statusTint("Result"));
        fields_->setItem(r, columns + 1, item);
    }
    fields_->horizontalHeader()->resizeSection(0, 150);
    for (int c = 1; c <= columns + 1; ++c)
        fields_->horizontalHeader()->resizeSection(c, 220);

    resolution_->clear();
    resolution_->addItem("Automatic (merge compatible edits)");
    for (const auto& n : chainNames)
        resolution_->addItem("Force: " + n, n);
    QSqlQuery res(db);
    res.prepare("SELECT winner_plugin FROM UserResolutions WHERE origin_plugin = ? AND object_id = ?");
    res.addBindValue(originPlugin_);
    res.addBindValue(objectId_);
    if (res.exec() && res.next()) {
        int idx = resolution_->findData(res.value(0).toString());
        resolution_->setCurrentIndex(std::max(0, idx));
        resolutionNote_->setText("A user resolution is stored for this record.");
    } else {
        resolutionNote_->setText("By default compatible edits are merged and hard conflicts go to the last plugin "
                                 "that changed the field.");
    }
    resolution_->setEnabled(!originPlugin_.isEmpty());
}

void ConflictsPage::applyResolution()
{
    if (originPlugin_.isEmpty())
        return;
    if (resolution_->currentIndex() <= 0) {
        app_->clearResolution(originPlugin_, objectId_);
        resolutionNote_->setText("Resolution cleared. Recompile to apply.");
    } else {
        app_->saveResolution(originPlugin_, objectId_, resolution_->currentData().toString());
        resolutionNote_->setText("Saved: " + resolution_->currentData().toString() +
                                 " wins this record in the next compilation.");
    }
}

} // namespace gui
