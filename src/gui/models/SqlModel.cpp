#include "models/SqlModel.hpp"

#include "Format.hpp"
#include "Theme.hpp"

#include <QFontDatabase>
#include <QHeaderView>
#include <QMenu>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableView>

#include <spdlog/spdlog.h>

namespace gui {

SqlModel::SqlModel(QObject* parent) : QSqlQueryModel(parent) {}

bool SqlModel::load(QSqlDatabase db, const QString& sql, const QVariantList& binds)
{
    db_ = db; // the column clicked last keeps sorting the new rows
    sql_ = sql;
    binds_ = binds;
    return run();
}

void SqlModel::sort(int column, Qt::SortOrder order)
{
    if (column == sortColumn_ && order == sortOrder_)
        return;
    sortColumn_ = column;
    sortOrder_ = order;
    if (!sql_.isEmpty())
        run();
}

bool SqlModel::run()
{
    if (!db_.isOpen()) {
        clear();
        return false;
    }
    // Concatenated, not QString::arg: the query may contain % signs (LIKE patterns).
    const QString sql = sortColumn_ < 0 ? sql_
                                        : "SELECT * FROM (" + sql_ + ") ORDER BY " + QString::number(sortColumn_ + 1) +
                                              (sortOrder_ == Qt::AscendingOrder ? " ASC" : " DESC");
    QSqlQuery q(db_);
    q.setForwardOnly(false);
    if (sortColumn_ >= 0 && !q.prepare(sql)) { // a column the new query does not have: its own order
        sortColumn_ = -1;
        return run();
    }
    if (sortColumn_ < 0 && !q.prepare(sql)) {
        spdlog::warn("SQL prepare failed: {} ({})", q.lastError().text().toStdString(), sql.left(120).toStdString());
        clear();
        return false;
    }
    for (int i = 0; i < binds_.size(); ++i)
        q.bindValue(i, binds_[i]);
    if (!q.exec()) {
        spdlog::warn("SQL failed: {}", q.lastError().text().toStdString());
        clear();
        return false;
    }
    setQuery(std::move(q));
    while (canFetchMore() && rowCount() < 2000)
        fetchMore();
    return true;
}

QVariant SqlModel::value(int row, int column) const
{
    return QSqlQueryModel::data(index(row, column), Qt::DisplayRole);
}

QVariant SqlModel::data(const QModelIndex& idx, int role) const
{
    const int c = idx.column();
    if (role == Qt::DisplayRole) {
        QVariant v = QSqlQueryModel::data(idx, role);
        if (numberColumns_.contains(c) && !v.isNull())
            return formatCount(v.toLongLong());
        if (bytesColumns_.contains(c) && !v.isNull())
            return formatBytes(v.toLongLong());
        return v;
    }
    if (role == Qt::TextAlignmentRole && (numberColumns_.contains(c) || bytesColumns_.contains(c)))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ForegroundRole && statusColumns_.contains(c)) {
        QColor col = theme::statusText(QSqlQueryModel::data(idx, Qt::DisplayRole).toString());
        if (col.isValid())
            return col;
    }
    if (role == Qt::FontRole && monoColumns_.contains(c))
        return QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (role == Qt::ToolTipRole) {
        // Long cells are shortened by the column width: the tooltip shows them in full, wrapped.
        QString text = QSqlQueryModel::data(idx, Qt::DisplayRole).toString();
        if (text.size() > 40)
            return "<p style='white-space:pre-wrap'>" + text.toHtmlEscaped() + "</p>";
        return {};
    }
    return QSqlQueryModel::data(idx, role);
}

void styleTable(QTableView* view, bool sortable)
{
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setShowGrid(false);
    view->setWordWrap(false);
    view->verticalHeader()->hide();
    view->verticalHeader()->setDefaultSectionSize(26);
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // No sort indicator until a header is clicked: the page's own order first.
    view->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    view->setSortingEnabled(sortable);
}

void enableColumnChooser(QTableView* view, const QSet<int>& hidden, const QSet<int>& never,
                         std::function<void(int, bool)> changed)
{
    auto* h = view->horizontalHeader();
    for (int c : hidden)
        view->setColumnHidden(c, true);
    h->setContextMenuPolicy(Qt::CustomContextMenu);
    h->setToolTip("Right-click to show or hide columns");
    QObject::connect(h, &QHeaderView::customContextMenuRequested, view, [view, h, never, changed](const QPoint& at) {
        QMenu menu(view);
        auto* model = view->model();
        if (!model)
            return;
        for (int c = 0; c < model->columnCount(); ++c) {
            if (never.contains(c))
                continue;
            auto* a = menu.addAction(model->headerData(c, Qt::Horizontal).toString());
            a->setCheckable(true);
            a->setChecked(!view->isColumnHidden(c));
            QObject::connect(a, &QAction::toggled, view, [view, c, changed](bool on) {
                view->setColumnHidden(c, !on);
                if (changed)
                    changed(c, !on);
            });
        }
        menu.exec(h->mapToGlobal(at));
    });
}

void fitColumns(QTableView* view, int maxWidth)
{
    view->resizeColumnsToContents();
    auto* h = view->horizontalHeader();
    for (int c = 0; c < h->count(); ++c)
        if (h->sectionSize(c) > maxWidth)
            h->resizeSection(c, maxWidth);
        else
            h->resizeSection(c, h->sectionSize(c) + 12);
}

} // namespace gui
