#include "pages/ConflictsPage.hpp"

#include "Format.hpp"
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

#include <map>

namespace gui {

namespace {

// One cell of the field comparison grid. Kept at namespace scope and in std::map:
// a function-local type inside nested QMaps made MSVC 19.44 hang while compiling.
struct FieldCell {
    QString value;
    QString status;
};

// Columns of the conflict list. Hidden ones carry keys for the detail panel.
enum Column {
    ColFormId,
    ColType,
    ColEditorId,
    ColConflict,
    ColSeverity,
    ColWinner,
    ColResolvedIn,
    ColOrigin,
    ColPlugins,
    ColChain,
    ColSummary,
    ColId,
    ColOriginId,
    ColObjectId,
};

// ORDER BY expression for each sortable column.
QString sortExpression(int column)
{
    switch (column) {
    case ColFormId: return "c.form_id";
    case ColType: return "c.signature";
    case ColEditorId: return "c.editor_id COLLATE NOCASE";
    case ColConflict: return "c.conflict_type";
    case ColSeverity: return "CASE c.severity WHEN 'Critical' THEN 0 WHEN 'High' THEN 1 WHEN 'Medium' THEN 2 ELSE 3 END";
    case ColWinner: return "p.name COLLATE NOCASE";
    case ColOrigin: return "o.name COLLATE NOCASE";
    case ColPlugins: return "c.chain_length";
    case ColChain: return "c.chain COLLATE NOCASE";
    case ColResolvedIn: return "resolved_in COLLATE NOCASE";
    case ColSummary: return "c.summary COLLATE NOCASE";
    default: return {};
    }
}

} // namespace

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
    plugin_ = new QComboBox;
    plugin_->setMinimumWidth(240);
    plugin_->setToolTip("Only show conflicts involving this plugin");
    role_ = new QComboBox;
    role_->addItem("is involved", "involved");
    role_->addItem("wins", "wins");
    role_->addItem("loses", "loses");
    role_->setToolTip("Involved: the plugin edits the record. Wins: it is the last plugin to edit it. "
                      "Loses: it edits the record but another plugin wins.");
    auto* clear = makeButton("Clear Filters");
    count_ = new QLabel;
    count_->setObjectName("PageSubtitle");
    auto* filters = new QHBoxLayout;
    filters->addWidget(chips_);
    filters->addStretch();
    filters->addWidget(new QLabel("Plugin"));
    filters->addWidget(plugin_);
    filters->addWidget(role_);
    filters->addWidget(clear);
    l->addLayout(filters);

    auto* split = new QSplitter(Qt::Vertical);
    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    table_->setModel(model_);
    // Sorting runs in SQL: the view only shows the indicator and reports clicks.
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    table_->setToolTip("Click a column header to sort. Double-click a row to filter by its winning plugin.");
    auto* listPane = new QWidget;
    auto* lp = new QVBoxLayout(listPane);
    lp->setContentsMargins(0, 0, 0, 0);
    lp->setSpacing(4);
    lp->addWidget(table_, 1);
    lp->addWidget(count_);
    split->addWidget(listPane);

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
    connect(plugin_, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    connect(role_, &QComboBox::currentIndexChanged, this, [this] { reload(); });
    connect(clear, &QPushButton::clicked, this, [this] { clearFilters(); });
    connect(table_->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, [this](int column, Qt::SortOrder order) {
        sortColumn_ = sortExpression(column).isEmpty() ? -1 : column;
        sortOrder_ = order;
        reload();
    });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
        int i = plugin_->findData(model_->value(index.row(), ColWinner).toString());
        if (i < 0)
            return;
        plugin_->setCurrentIndex(i);
        role_->setCurrentIndex(role_->findData("wins"));
    });
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
    reloadPluginFilter();
    reload();
}

void ConflictsPage::reloadPluginFilter()
{
    QString current = plugin_->currentData().toString();
    QSignalBlocker block(plugin_);
    plugin_->clear();
    plugin_->addItem("Any plugin", QString());
    QSqlDatabase db = app_->readDb();
    if (db.isOpen()) {
        // Every plugin that takes part in a conflict, in load order, with how many it wins.
        QSqlQuery q(db);
        q.exec("SELECT p.name, COUNT(*), SUM(CASE WHEN c.winner_plugin_id = p.id THEN 1 ELSE 0 END) "
               "FROM ConflictPlugins cp JOIN Plugins p ON p.id = cp.plugin_id JOIN Conflicts c ON c.id = cp.conflict_id "
               "GROUP BY p.id ORDER BY p.load_order");
        while (q.next())
            plugin_->addItem(QString("%1  (%2 conflicts, wins %3)")
                                 .arg(q.value(0).toString(), formatCount(q.value(1).toLongLong()),
                                      formatCount(q.value(2).toLongLong())),
                             q.value(0).toString());
    }
    int idx = plugin_->findData(current);
    plugin_->setCurrentIndex(idx >= 0 ? idx : 0);
}

void ConflictsPage::clearFilters()
{
    QSignalBlocker b1(chips_), b2(signature_), b3(search_), b4(plugin_), b5(role_), b6(table_->horizontalHeader());
    chips_->setCurrent("All");
    signature_->setCurrentIndex(0);
    search_->clear();
    plugin_->setCurrentIndex(0);
    role_->setCurrentIndex(0);
    table_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    sortColumn_ = -1;
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
        where += " AND (c.editor_id LIKE ? OR c.form_id LIKE ? OR c.chain LIKE ?)";
        binds << "%" + s + "%" << s + "%" << "%" + s + "%";
    }
    QString plugin = plugin_->currentData().toString();
    if (!plugin.isEmpty()) {
        QString role = role_->currentData().toString();
        QString pid = "(SELECT id FROM Plugins WHERE name = ?)";
        if (role == "wins") {
            where += " AND c.winner_plugin_id = " + pid;
            binds << plugin;
        } else {
            where += " AND EXISTS (SELECT 1 FROM ConflictPlugins cp WHERE cp.conflict_id = c.id AND cp.plugin_id = " + pid + ")";
            binds << plugin;
            if (role == "loses") {
                where += " AND c.winner_plugin_id <> " + pid;
                binds << plugin;
            }
        }
    }

    QString order = "c.conflict_type DESC, CASE c.severity WHEN 'Critical' THEN 0 ELSE 1 END, c.form_id";
    if (sortColumn_ >= 0)
        order = sortExpression(sortColumn_) + (sortOrder_ == Qt::DescendingOrder ? " DESC" : " ASC") + ", c.form_id";
    const QString from = " FROM Conflicts c LEFT JOIN Plugins p ON p.id = c.winner_plugin_id "
                         "LEFT JOIN Plugins o ON o.id = c.origin_plugin_id WHERE " + where;
    QString sql = "SELECT c.form_id AS FormID, c.signature AS Type, c.editor_id AS 'Editor ID', c.type_name AS Conflict, "
                  "c.severity AS Severity, p.name AS 'Winning Plugin', (SELECT g.output_plugin FROM "
                  "GeneratedRecords g WHERE g.origin_plugin_id = c.origin_plugin_id AND g.object_id = c.object_id "
                  "LIMIT 1) AS resolved_in, o.name AS 'Origin', c.chain_length AS Plugins, c.chain AS 'Override Chain', "
                  "c.summary AS Summary, c.id, c.origin_plugin_id, c.object_id" +
                  from + " ORDER BY " + order;
    model_->setMonoColumns({ColFormId});
    model_->setStatusColumns({ColSeverity});
    model_->setNumberColumns({ColPlugins});
    model_->load(app_->readDb(), sql, binds);
    model_->setHeaderData(ColResolvedIn, Qt::Horizontal, "Written To");
    for (int c : {ColId, ColOriginId, ColObjectId})
        table_->hideColumn(c);
    fitColumns(table_);

    qint64 shown = 0, total = 0;
    {
        QSqlDatabase db = app_->readDb();
        QSqlQuery q(db);
        q.prepare("SELECT COUNT(*)" + from);
        for (const auto& b : binds)
            q.addBindValue(b);
        if (q.exec() && q.next())
            shown = q.value(0).toLongLong();
        if (q.exec("SELECT COUNT(*) FROM Conflicts") && q.next())
            total = q.value(0).toLongLong();
    }
    count_->setText(shown == total ? QString("%1 conflicts").arg(formatCount(total))
                                   : QString("Showing %1 of %2 conflicts").arg(formatCount(shown), formatCount(total)));
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
    conflictId_ = model_->value(row, ColId).toLongLong();
    QSqlDatabase db = app_->readDb();

    QMap<qint64, QString> pluginNames;
    {
        QSqlQuery q(db);
        q.exec("SELECT id, name FROM Plugins");
        while (q.next())
            pluginNames[q.value(0).toLongLong()] = q.value(1).toString();
    }
    originPlugin_ = pluginNames.value(model_->value(row, ColOriginId).toLongLong());
    objectId_ = model_->value(row, ColObjectId).toUInt();

    QString edid = model_->value(row, ColEditorId).toString();
    title_->setText(QString("%1  [%2:%3]").arg(edid.isEmpty() ? "(no editor id)" : edid,
                                              model_->value(row, ColType).toString(),
                                              model_->value(row, ColFormId).toString()));
    QString written = model_->value(row, ColResolvedIn).toString();
    summary_->setText(model_->value(row, ColSummary).toString() +
                      (written.isEmpty() ? QString() : "  ·  Written to " + written));

    // Columns: one per plugin in the chain, then the compiled result.
    QSqlQuery q(db);
    q.prepare("SELECT field_index, field, hard, position, plugin_id, value, status FROM ConflictFields "
              "WHERE conflict_id = ? ORDER BY field_index, position");
    q.addBindValue(conflictId_);
    q.exec();
    std::map<int, QString> fieldNames;
    std::map<int, bool> hard;
    std::map<int, std::map<int, FieldCell>> cells; // field index -> chain position (-1 = result) -> cell
    while (q.next()) {
        int fi = q.value(0).toInt();
        int pos = q.value(3).toInt();
        fieldNames[fi] = q.value(1).toString();
        hard[fi] = q.value(2).toBool();
        cells[fi][pos] = FieldCell{q.value(5).toString(), q.value(6).toString()};
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
    for (const auto& [fieldIndex, fieldName] : fieldNames) {
        const auto& row = cells[fieldIndex];
        auto cellAt = [&](int pos) {
            auto c = row.find(pos);
            return c == row.end() ? FieldCell{} : c->second;
        };
        auto* name = new QTableWidgetItem((hard[fieldIndex] ? QStringLiteral("⚠ ") : QString()) + fieldName);
        if (hard[fieldIndex]) {
            name->setForeground(theme::kRed);
            name->setToolTip("Hard conflict: several plugins changed this field differently");
        }
        fields_->setItem(r, 0, name);
        for (int pos = 0; pos < columns; ++pos) {
            FieldCell c = cellAt(pos);
            auto* item = new QTableWidgetItem(c.value);
            item->setToolTip(c.value + "\n\n" + c.status);
            QColor tint = theme::statusTint(c.status);
            if (tint.isValid())
                item->setBackground(tint);
            if (c.status == "Base" || c.status == "Same" || c.status == "Absent")
                item->setForeground(theme::kMuted);
            fields_->setItem(r, pos + 1, item);
        }
        FieldCell result = cellAt(-1);
        auto* item = new QTableWidgetItem(result.value);
        item->setToolTip(result.value);
        item->setBackground(theme::statusTint("Result"));
        fields_->setItem(r, columns + 1, item);
        ++r;
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
