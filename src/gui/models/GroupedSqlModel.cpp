#include "models/GroupedSqlModel.hpp"

#include "Format.hpp"
#include "Theme.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QSqlError>
#include <QSqlQuery>
#include <QTreeView>

#include <spdlog/spdlog.h>

namespace gui {

namespace {
constexpr int kBatch = 2000;
}

GroupedSqlModel::GroupedSqlModel(QObject* parent) : QAbstractItemModel(parent) {}

void GroupedSqlModel::load(QSqlDatabase db, Spec spec)
{
    beginResetModel();
    db_ = db;
    spec_ = std::move(spec);
    groups_.clear();
    if (db_.isOpen()) {
        QSqlQuery q(db_);
        if (q.prepare(spec_.groupSql)) {
            for (int i = 0; i < spec_.binds.size(); ++i)
                q.bindValue(i, spec_.binds[i]);
            if (q.exec())
                while (q.next())
                    groups_.push_back({q.value(0).toString(), q.value(1).toLongLong(), {}});
            else
                spdlog::warn("SQL failed: {}", q.lastError().text().toStdString());
        } else {
            spdlog::warn("SQL prepare failed: {}", q.lastError().text().toStdString());
        }
    }
    endResetModel();
}

QString GroupedSqlModel::orderClause() const
{
    if (sortColumn_ < 0 || sortColumn_ >= spec_.sortExpr.size() || spec_.sortExpr[sortColumn_].isEmpty())
        return spec_.defaultOrder;
    return "ORDER BY " + spec_.sortExpr[sortColumn_] + (sortOrder_ == Qt::AscendingOrder ? " ASC" : " DESC");
}

void GroupedSqlModel::fetch(int g, int batch)
{
    auto& group = groups_[g];
    const qint64 have = group.rows.size();
    const qint64 n = std::min<qint64>(batch, group.count - have);
    if (n <= 0)
        return;
    QSqlQuery q(db_);
    QString sql = spec_.rowSql;
    sql.replace("%ORDER%", orderClause());
    if (!q.prepare(sql)) {
        spdlog::warn("SQL prepare failed: {}", q.lastError().text().toStdString());
        return;
    }
    int b = 0;
    for (const auto& v : spec_.binds)
        q.bindValue(b++, v);
    q.bindValue(b++, group.key);
    q.bindValue(b++, n);
    q.bindValue(b++, have);
    if (!q.exec()) {
        spdlog::warn("SQL failed: {}", q.lastError().text().toStdString());
        return;
    }
    QVector<QVector<QVariant>> rows;
    const int cols = spec_.headers.size();
    while (q.next()) {
        QVector<QVariant> r(cols);
        for (int c = 0; c < cols; ++c)
            r[c] = q.value(c);
        rows.push_back(std::move(r));
    }
    if (rows.isEmpty()) {
        group.count = have; // fewer rows than counted: stop asking
        return;
    }
    const QModelIndex parent = index(g, 0);
    beginInsertRows(parent, static_cast<int>(have), static_cast<int>(have + rows.size() - 1));
    for (auto& r : rows)
        group.rows.push_back(std::move(r));
    endInsertRows();
}

QString GroupedSqlModel::groupKey(const QModelIndex& i) const
{
    if (!i.isValid())
        return {};
    const int g = i.internalId() == 0 ? i.row() : static_cast<int>(i.internalId() - 1);
    return g >= 0 && g < groups_.size() ? groups_[g].key : QString();
}

QVariant GroupedSqlModel::rowValue(const QModelIndex& i, int column) const
{
    if (!i.isValid() || i.internalId() == 0)
        return {};
    const auto& g = groups_[static_cast<int>(i.internalId() - 1)];
    if (i.row() >= g.rows.size() || column >= g.rows[i.row()].size())
        return {};
    return g.rows[i.row()][column];
}

QModelIndex GroupedSqlModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column < 0 || column >= columnCount())
        return {};
    if (!parent.isValid())
        return row >= 0 && row < groups_.size() ? createIndex(row, column, quintptr(0)) : QModelIndex();
    if (parent.internalId() != 0)
        return {};
    const int g = parent.row();
    if (row < 0 || row >= groups_[g].rows.size())
        return {};
    return createIndex(row, column, quintptr(g + 1));
}

QModelIndex GroupedSqlModel::parent(const QModelIndex& i) const
{
    if (!i.isValid() || i.internalId() == 0)
        return {};
    return createIndex(static_cast<int>(i.internalId() - 1), 0, quintptr(0));
}

int GroupedSqlModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return groups_.size();
    if (parent.internalId() != 0 || parent.column() != 0)
        return 0;
    return groups_[parent.row()].rows.size();
}

int GroupedSqlModel::columnCount(const QModelIndex&) const
{
    return spec_.headers.size();
}

bool GroupedSqlModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return !groups_.isEmpty();
    return parent.internalId() == 0 && parent.column() == 0 && groups_[parent.row()].count > 0;
}

bool GroupedSqlModel::canFetchMore(const QModelIndex& parent) const
{
    return parent.isValid() && parent.internalId() == 0 &&
           groups_[parent.row()].rows.size() < groups_[parent.row()].count;
}

void GroupedSqlModel::fetchMore(const QModelIndex& parent)
{
    if (canFetchMore(parent))
        fetch(parent.row(), kBatch);
}

QVariant GroupedSqlModel::data(const QModelIndex& i, int role) const
{
    if (!i.isValid())
        return {};
    if (i.internalId() == 0) {
        const auto& g = groups_[i.row()];
        if (role == Qt::DisplayRole && i.column() == 0)
            return QString("%1  ·  %2 %3").arg(g.key, formatCount(g.count), spec_.rowNoun);
        if (role == Qt::FontRole) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    }
    const int c = i.column();
    const QVariant v = rowValue(i, c);
    switch (role) {
    case Qt::DisplayRole:
        if (numberColumns_.contains(c) && !v.isNull())
            return formatCount(v.toLongLong());
        if (bytesColumns_.contains(c) && !v.isNull())
            return formatBytes(v.toLongLong());
        return v;
    case Qt::TextAlignmentRole:
        if (numberColumns_.contains(c) || bytesColumns_.contains(c))
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    case Qt::ForegroundRole:
        if (statusColumns_.contains(c)) {
            QColor col = theme::statusText(v.toString());
            if (col.isValid())
                return col;
        }
        return {};
    case Qt::FontRole:
        if (monoColumns_.contains(c))
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        return {};
    case Qt::ToolTipRole: {
        const QString text = v.toString();
        if (text.size() > 40)
            return "<p style='white-space:pre-wrap'>" + text.toHtmlEscaped() + "</p>";
        return {};
    }
    default:
        return {};
    }
}

QVariant GroupedSqlModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < spec_.headers.size())
        return spec_.headers[section];
    return {};
}

void GroupedSqlModel::sort(int column, Qt::SortOrder order)
{
    if (column == sortColumn_ && order == sortOrder_)
        return;
    sortColumn_ = column;
    sortOrder_ = order;
    // Re-read the loaded part of every expanded group in the new order; groups stay where they are.
    for (int g = 0; g < groups_.size(); ++g) {
        const int loaded = groups_[g].rows.size();
        if (loaded == 0)
            continue;
        beginRemoveRows(index(g, 0), 0, loaded - 1);
        groups_[g].rows.clear();
        endRemoveRows();
        fetch(g, std::max(loaded, kBatch));
    }
}

void styleGroupedTree(QTreeView* view, GroupedSqlModel* model)
{
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setUniformRowHeights(true);
    view->setWordWrap(false);
    view->setRootIsDecorated(true);
    view->setExpandsOnDoubleClick(true);
    view->header()->setStretchLastSection(true);
    view->header()->setHighlightSections(false);
    view->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    view->header()->setSortIndicatorShown(true);
    view->header()->setSortIndicator(-1, Qt::AscendingOrder);
    view->setSortingEnabled(true);
    // A group's label (plugin and row count) spans every column, so it is not cut by the first
    // column's width and is not repeated on its rows.
    QObject::connect(model, &QAbstractItemModel::modelReset, view, [view, model] {
        for (int r = 0; r < model->rowCount(); ++r)
            view->setFirstColumnSpanned(r, QModelIndex(), true);
        view->collapseAll();
    });
}

} // namespace gui
