#pragma once

#include "pages/Page.hpp"

class QListWidget;
class QTextBrowser;
class QPushButton;

namespace gui {

class ReportsPage : public Page {
    Q_OBJECT
public:
    explicit ReportsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void showReport();
    void exportReport(const QString& format);
    void exportAll();

    QListWidget* list_;
    QTextBrowser* preview_;
    QPushButton* html_;
    QPushButton* json_;
    QPushButton* pdf_;
    QPushButton* all_;
    QString currentHtml_;
};

} // namespace gui
