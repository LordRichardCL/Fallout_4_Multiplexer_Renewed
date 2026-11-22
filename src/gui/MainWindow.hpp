#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QMap>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QDockWidget;
class QFrame;
class QTimer;

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
    void resizeEvent(QResizeEvent* e) override;

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
    // A card in the corner when a task finishes: what it did in plain words, and buttons to what needs you.
    void showSummary(const QString& task, const QString& message);
    void placeSummary();
    QFrame* summary_ = nullptr;
    QTimer* summaryTimer_ = nullptr;
    QProgressBar* progress_;
    QLabel* stageLabel_;
    QPushButton* cancel_;
    QElapsedTimer stageClock_; // since the current stage started, for the time left
    // Where a task is: step n of the steps the same task took last time (remembered in the settings), and
    // the time each took then, for the time left in the whole task.
    QString task_;
    int step_ = 0;
    QStringList lastSteps_;
    QList<qint64> lastMs_;
    QStringList steps_;
    QList<qint64> stepMs_;
    QString stageName_;
    // The latest progress, shown again every second (a clock that keeps going while a step reports nothing).
    qint64 lastDone_ = 0, lastTotal_ = 0;
    QString lastDetail_;
    class QTimer* tick_ = nullptr;
    QString progressText(qint64 done, qint64 total, const QString& detail) const;
    QPlainTextEdit* log_;
    QDockWidget* logDock_;
};

} // namespace gui
