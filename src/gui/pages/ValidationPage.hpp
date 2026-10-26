#pragma once

#include "pages/Page.hpp"

class QLineEdit;
class QTableView;

namespace gui {

class FilterChips;
class SqlModel;
class StatCard;
class StackedBar;

class ValidationPage : public Page {
    Q_OBJECT
public:
    explicit ValidationPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void reload();

    StatCard* checks_;
    StatCard* passed_;
    StatCard* warnings_;
    StatCard* errors_;
    StackedBar* bar_;
    FilterChips* chips_;
    QLineEdit* search_;
    QTableView* table_;
    SqlModel* model_;
};

} // namespace gui
