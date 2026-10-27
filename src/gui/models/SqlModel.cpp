#include "models/SqlModel.hpp"

#include "Format.hpp"
#include "Theme.hpp"

#include <QFontDatabase>
#include <QHeaderView>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableView>

#include <spdlog/spdlog.h>

namespace gui {

SqlModel::SqlModel(QObject* parent) : QSqlQueryModel(parent) {}

bool SqlModel::load(QSqlDatabase db, const QString& sql, const QVariantList& binds)
{
    if (!db.isOpen()) {
        clear();
        return false;
    }
    QSqlQuery q(db);
    q.setForwardOnly(false);
    if (!q.prepare(sql)) {
        spdlog::warn("SQL prepare failed: {} ({})", q.lastError().text().toStdString(), sql.left(120).toStdString());
        clear();
        return false;
    }
    for (int i = 0; i < binds.size(); ++i)
        q.bindValue(i, binds[i]);
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

void styleTable(QTableView* view)
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
    view->setSortingEnabled(false);
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
