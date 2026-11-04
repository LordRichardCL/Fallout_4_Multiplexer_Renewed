#pragma once

#include "pages/Page.hpp"

class QLineEdit;
class QTableView;
class QTextBrowser;
class QPushButton;

class QFrame;
class QLabel;

namespace gui {

class FilterChips;
class SqlModel;

class PluginsPage : public Page {
    Q_OBJECT
public:
    explicit PluginsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;
    // Blocking types and every plugin's header, record types and eligibility, as tab-separated text.
    QString exportText();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct SummaryCard {
        QFrame* frame = nullptr;
        QLabel* value = nullptr;
        QLabel* hint = nullptr;
        QLabel* esl = nullptr;
        QLabel* eslFlaggedEsp = nullptr;
        QLabel* esp = nullptr;
        QLabel* esm = nullptr;
    };
    SummaryCard makeSummaryCard(const QString& title, const QString& chip, const QColor& accent, bool breakdown);
    void updateSummary();
    void reload();
    void showDetails(int row);
    void showBlocker(int row);

    FilterChips* chips_;
    QWidget* summary_;
    SummaryCard total_, eligible_, ineligible_;
    QLineEdit* search_;
    QTableView* table_;
    SqlModel* model_;
    QTextBrowser* details_;
    QPushButton* exclude_;
    QString selectedPlugin_;
    bool generatedMode_ = false;
    bool blockersMode_ = false;
};

} // namespace gui
