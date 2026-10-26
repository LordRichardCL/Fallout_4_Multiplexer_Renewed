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
    void addPage(const QString& name, Page* page);
    void refreshAll();

    AppController* app_;
    QListWidget* nav_;
    QStackedWidget* stack_;
    QMap<QString, int> pageIndex_;
    QList<Page*> pages_;
    QProgressBar* progress_;
    QLabel* stageLabel_;
    QPushButton* cancel_;
    QPlainTextEdit* log_;
    QDockWidget* logDock_;
};

} // namespace gui
