#pragma once

#include "pages/Page.hpp"

class QLineEdit;
class QTableView;
class QTextBrowser;
class QPushButton;

namespace gui {

class FilterChips;
class SqlModel;

class PluginsPage : public Page {
    Q_OBJECT
public:
    explicit PluginsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void reload();
    void showDetails(int row);

    FilterChips* chips_;
    QLineEdit* search_;
    QTableView* table_;
    SqlModel* model_;
    QTextBrowser* details_;
    QPushButton* exclude_;
    QString selectedPlugin_;
    bool generatedMode_ = false;
};

} // namespace gui
