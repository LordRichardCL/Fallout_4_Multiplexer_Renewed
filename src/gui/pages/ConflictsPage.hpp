#pragma once

#include "pages/Page.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QTableView;
class QTableWidget;

namespace gui {

class FilterChips;
class SqlModel;

// Conflict Centre: override chains, winners, field-level differences.
class ConflictsPage : public Page {
    Q_OBJECT
public:
    explicit ConflictsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void reload();
    void reloadPluginFilter();
    void showConflict(int row);
    void applyResolution();
    void clearFilters();

    FilterChips* chips_;
    QComboBox* signature_;
    QLineEdit* search_;
    QComboBox* plugin_;
    QComboBox* role_;
    QLabel* count_;
    QTableView* table_;
    SqlModel* model_;
    QLabel* title_;
    QLabel* chain_;
    QLabel* summary_;
    QTableWidget* fields_;
    QComboBox* resolution_;
    QLabel* resolutionNote_;

    int sortColumn_ = -1; // -1: by conflict type and severity
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    qint64 conflictId_ = -1;
    QString originPlugin_;
    quint32 objectId_ = 0;
};

} // namespace gui
