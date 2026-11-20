#pragma once

#include <QMainWindow>
#include <QMap>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QDockWidget;

namespace gui {

class AppController;
class Page;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppController* app, QWidget* parent = nullptr);

    void showPage(const QString& name);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void addGroup(const QString& title);
    void addPage(const QString& name, const QString& label, const QString& icon, Page* page);
    void refreshAll();
    // Counts on the navigation (changes affecting the build, patch choices waiting, plugins kept out...).
    void refreshBadges();

    AppController* app_;
    QListWidget* nav_;
    QStackedWidget* stack_;
    QMap<QString, int> pageIndex_; // page name -> stack index
    QMap<QString, int> navRow_;    // page name -> navigation row (group headers have rows too)
    QList<Page*> pages_;
    std::vector<std::pair<QString, QString>> pageLabels_; // (label, page name), for quick search
    // Ctrl+K: jump to a page, plugin or record.
    void quickSearch();
    QProgressBar* progress_;
    QLabel* stageLabel_;
    QPushButton* cancel_;
    QPlainTextEdit* log_;
    QDockWidget* logDock_;
};

} // namespace gui
