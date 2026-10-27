#pragma once

#include "pages/Page.hpp"

class QLineEdit;
class QTableView;
class QTextBrowser;

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
    // The rows currently shown, as tab-separated text with a header line.
    QString shownRowsAsText();

    StatCard* checks_;
    StatCard* passed_;
    StatCard* warnings_;
    StatCard* errors_;
    StackedBar* bar_;
    FilterChips* chips_;
    QLineEdit* search_;
    QTableView* table_;
    SqlModel* model_;
    QTextBrowser* details_;
};

} // namespace gui
