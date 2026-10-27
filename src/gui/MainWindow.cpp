#include "MainWindow.hpp"

#include "AppController.hpp"
#include "Theme.hpp"
#include "pages/AssetsPage.hpp"
#include "pages/ChangesPage.hpp"
#include "pages/CompilationPage.hpp"
#include "pages/ConflictsPage.hpp"
#include "pages/DashboardPage.hpp"
#include "pages/DependenciesPage.hpp"
#include "pages/PluginsPage.hpp"
#include "pages/ReportsPage.hpp"
#include "pages/SettingsPage.hpp"
#include "pages/ValidationPage.hpp"

#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

namespace gui {

MainWindow::MainWindow(AppController* app, QWidget* parent) : QMainWindow(parent), app_(app)
{
    setWindowTitle("Fallout 4 Multiplexer Renewed");
    resize(1500, 920);
    setMinimumSize(1100, 700);

    auto* central = new QWidget;
    auto* h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    auto* navPanel = new QWidget;
    navPanel->setObjectName("NavPanel");
    navPanel->setFixedWidth(220);
    auto* nl = new QVBoxLayout(navPanel);
    nl->setContentsMargins(0, 0, 0, 0);
    nl->setSpacing(0);
    auto* title = new QLabel("MULTIPLEXER");
    title->setObjectName("NavTitle");
    auto* subtitle = new QLabel("Fallout 4 · Renewed");
    subtitle->setObjectName("NavSubtitle");
    nl->addWidget(title);
    nl->addWidget(subtitle);
    nav_ = new QListWidget;
    nav_->setObjectName("NavList");
    nav_->setFocusPolicy(Qt::NoFocus);
    nl->addWidget(nav_, 1);
    auto* version = new QLabel("v" FO4MX_VERSION " · by Romewolf");
    version->setObjectName("NavSubtitle");
    nl->addWidget(version);
    h->addWidget(navPanel);

    stack_ = new QStackedWidget;
    h->addWidget(stack_, 1);
    setCentralWidget(central);

    addPage("Dashboard", new DashboardPage(app_));
    addPage("Changes", new ChangesPage(app_));
    addPage("Plugins", new PluginsPage(app_));
    addPage("Conflicts", new ConflictsPage(app_));
    addPage("Assets", new AssetsPage(app_));
    addPage("Dependencies", new DependenciesPage(app_));
    addPage("Compilation", new CompilationPage(app_));
    addPage("Validation", new ValidationPage(app_));
    addPage("Reports", new ReportsPage(app_));
    addPage("Settings", new SettingsPage(app_));

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        stack_->setCurrentIndex(row);
        if (row >= 0 && row < pages_.size())
            pages_[row]->refresh();
    });
    for (int i = 0; i < pages_.size() && i < 9; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        connect(sc, &QShortcut::activated, this, [this, i] { nav_->setCurrentRow(i); });
    }

    // Status bar: stage, progress, cancel.
    stageLabel_ = new QLabel("Ready");
    progress_ = new QProgressBar;
    progress_->setFixedWidth(260);
    progress_->setTextVisible(false);
    progress_->hide();
    cancel_ = new QPushButton("Cancel");
    cancel_->setObjectName("Danger");
    cancel_->hide();
    auto* logToggle = new QPushButton("Log");
    logToggle->setCheckable(true);
    statusBar()->addWidget(stageLabel_, 1);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(cancel_);
    statusBar()->addPermanentWidget(logToggle);

    logDock_ = new QDockWidget("Log", this);
    logDock_->setObjectName("LogDock");
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    log_->setStyleSheet("font-family: Consolas, 'Cascadia Mono', monospace; font-size: 9pt;");
    logDock_->setWidget(log_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    logDock_->hide();
    connect(logToggle, &QPushButton::toggled, logDock_, &QDockWidget::setVisible);
    connect(logDock_, &QDockWidget::visibilityChanged, logToggle, &QPushButton::setChecked);

    connect(cancel_, &QPushButton::clicked, app_, &AppController::cancel);
    connect(app_, &AppController::busyChanged, this, [this](bool busy) {
        progress_->setVisible(busy);
        cancel_->setVisible(busy);
        if (busy) {
            progress_->setRange(0, 0);
        }
    });
    connect(app_, &AppController::stageStarted, this, [this](const QString& s) {
        stageLabel_->setText(s + "…");
        progress_->setRange(0, 0);
    });
    connect(app_, &AppController::progressChanged, this, [this](qint64 done, qint64 total, const QString& detail) {
        if (total > 0) {
            progress_->setRange(0, 1000);
            progress_->setValue(int(done * 1000 / total));
        }
        if (!detail.isEmpty())
            stageLabel_->setText(stageLabel_->text().section("…", 0, 0) + "…  " + detail);
    });
    connect(app_, &AppController::taskFinished, this, [this](const QString& task, bool ok, const QString& msg) {
        stageLabel_->setText(QString("%1 %2 — %3").arg(task, ok ? "complete" : "failed", msg));
        if (!ok && msg != "Cancelled")
            QMessageBox::warning(this, task + " failed", msg);
    });
    connect(app_, &AppController::dataChanged, this, [this] { refreshAll(); });
    connect(app_, &AppController::logMessage, this, [this](int level, const QString& text) {
        QString prefix = level >= 4 ? "ERROR " : level == 3 ? "WARN  " : "";
        log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + prefix + text);
    });

    for (auto* p : pages_)
        connect(p, &Page::navigate, this, &MainWindow::showPage);

    nav_->setCurrentRow(0);
}

void MainWindow::addPage(const QString& name, Page* page)
{
    pageIndex_[name] = stack_->addWidget(page);
    pages_.push_back(page);
    auto* item = new QListWidgetItem(name);
    item->setSizeHint(QSize(200, 40));
    nav_->addItem(item);
}

void MainWindow::showPage(const QString& name)
{
    if (pageIndex_.contains(name))
        nav_->setCurrentRow(pageIndex_[name]);
}

void MainWindow::refreshAll()
{
    // Only the visible page reloads immediately; others reload when shown.
    int row = nav_->currentRow();
    if (row >= 0 && row < pages_.size())
        pages_[row]->refresh();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (app_->busy()) {
        auto answer = QMessageBox::question(this, "Quit", "A task is still running. Cancel it and quit?");
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        app_->cancel();
    }
    e->accept();
}

} // namespace gui
