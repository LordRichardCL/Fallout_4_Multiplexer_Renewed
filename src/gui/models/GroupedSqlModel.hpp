#pragma once

#include <QAbstractItemModel>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
#include <QVariant>
#include <QVector>

class QTreeView;

namespace gui {

// A read-only, two-level SQL model: one parent row per group (an output plugin, a source plugin),
// its rows underneath. Groups are listed with their row count; a group's rows are read only when
// it is expanded, in batches as the view scrolls, so groups of hundreds of thousands of rows stay
// responsive. Sorting a column sorts the rows inside every group, in SQL (every row of the group,
// not only the ones loaded), and keeps the groups and their expanded state as they are.
class GroupedSqlModel : public QAbstractItemModel {
    Q_OBJECT
public:
    struct Spec {
        // SELECT <group key>, <row count> ... ORDER BY ...; the key is also the group's label.
        QString groupSql;
        // SELECT <columns> ... WHERE <group column> = ? %ORDER% LIMIT ? OFFSET ?
        // %ORDER% becomes "ORDER BY <sortExpr[column]> ASC|DESC" (or `defaultOrder`).
        QString rowSql;
        QStringList headers;
        QStringList sortExpr;     // per column: the SQL expression to sort by
        QString defaultOrder;     // "ORDER BY ..." used before any column is sorted
        QVariantList binds;       // bound before the group key in both queries
        QString rowNoun = "rows"; // "records", "mappings", "assets"
    };

    explicit GroupedSqlModel(QObject* parent = nullptr);

    void load(QSqlDatabase db, Spec spec);
    void setStatusColumns(QSet<int> c) { statusColumns_ = std::move(c); }
    void setNumberColumns(QSet<int> c) { numberColumns_ = std::move(c); }
    void setMonoColumns(QSet<int> c) { monoColumns_ = std::move(c); }
    void setBytesColumns(QSet<int> c) { bytesColumns_ = std::move(c); }

    bool isGroup(const QModelIndex& index) const { return index.isValid() && index.internalId() == 0; }
    QString groupKey(const QModelIndex& index) const; // of a group or of a row in it
    QVariant rowValue(const QModelIndex& index, int column) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    struct Group {
        QString key;
        qint64 count = 0;
        QVector<QVector<QVariant>> rows;
    };
    QString orderClause() const;
    void fetch(int group, int batch);

    QSqlDatabase db_;
    Spec spec_;
    QVector<Group> groups_;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    QSet<int> statusColumns_, numberColumns_, monoColumns_, bytesColumns_;
};

// Tree look matching the tables (alternating rows, sortable headers), groups spanning every column.
void styleGroupedTree(QTreeView* view, GroupedSqlModel* model);

} // namespace gui
