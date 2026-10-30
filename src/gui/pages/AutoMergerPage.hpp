#pragma once

#include "pages/Page.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QTableView;
class QTextBrowser;

namespace gui {

class FilterChips;
class SqlModel;

// Advanced Users Auto-Merger: opt-in relinking of scripts and configs that name compiled plugins,
// with its own results and report.
class AutoMergerPage : public Page {
    Q_OBJECT
public:
    explicit AutoMergerPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void exportReport(const QString& format);

    QCheckBox* enabled_;
    QLabel* summary_;
    FilterChips* chips_;
    QTableView* plugins_;
    SqlModel* pluginsModel_;
    QTableView* files_;
    SqlModel* filesModel_;
    QTextBrowser* details_;
    QPushButton* analyse_;
    QPushButton* html_;
    QPushButton* json_;
    QPushButton* txt_;
};

} // namespace gui
