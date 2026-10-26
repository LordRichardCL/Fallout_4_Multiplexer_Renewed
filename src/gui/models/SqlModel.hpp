#pragma once

#include <QSqlQueryModel>
#include <QSet>
#include <QSqlDatabase>
#include <QVariant>

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

private:
    QSet<int> statusColumns_, numberColumns_, monoColumns_, bytesColumns_;
};

// Applies the standard table look (alternating rows, stretch last column...).
void styleTable(QTableView* view);
// Sizes columns to their content, capped so long text does not push others off screen.
void fitColumns(QTableView* view, int maxWidth = 380);

} // namespace gui
