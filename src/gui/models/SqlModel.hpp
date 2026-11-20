#pragma once

#include <QSqlQueryModel>
#include <QSet>
#include <QSqlDatabase>
#include <QVariant>

#include <functional>

class QTableView;

namespace gui {

// Read-only SQL-backed table model with status colouring and number formatting.
class SqlModel : public QSqlQueryModel {
    Q_OBJECT
public:
    explicit SqlModel(QObject* parent = nullptr);

    // Runs `sql` with positional bindings; returns false and logs on error.
    bool load(QSqlDatabase db, const QString& sql, const QVariantList& binds = {});
    void setStatusColumns(QSet<int> columns) { statusColumns_ = std::move(columns); }
    void setNumberColumns(QSet<int> columns) { numberColumns_ = std::move(columns); }
    void setMonoColumns(QSet<int> columns) { monoColumns_ = std::move(columns); }
    void setBytesColumns(QSet<int> columns) { bytesColumns_ = std::move(columns); }
    QVariant value(int row, int column) const;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    // Clicking a column header sorts by that column: the query runs again ordered by it (rows keep their
    // columns, so value(row, column) stays right). Column -1: the query's own order.
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    bool run();
    QSet<int> statusColumns_, numberColumns_, monoColumns_, bytesColumns_;
    QSqlDatabase db_;
    QString sql_;
    QVariantList binds_;
    int sortColumn_ = -1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
};

// Applies the standard table look (alternating rows, stretch last column...).
// `sortable`: clicking a header sorts (pages that sort in their own query pass false).
void styleTable(QTableView* view, bool sortable = true);
// A right-click menu on the header to show or hide columns; `hidden` start hidden, `never` are never
// offered (key columns the page reads).
// `changed` is told each change (column, now hidden).
void enableColumnChooser(QTableView* view, const QSet<int>& hidden, const QSet<int>& never = {},
                         std::function<void(int, bool)> changed = {});
// Sizes columns to their content, capped so long text does not push others off screen.
void fitColumns(QTableView* view, int maxWidth = 380);

} // namespace gui
