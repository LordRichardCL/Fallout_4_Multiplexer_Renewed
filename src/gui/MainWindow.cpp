#include "MainWindow.hpp"

#include "AppController.hpp"
#include "Format.hpp"
#include "Theme.hpp"
#include "pages/AssetsPage.hpp"
#include "pages/ChangesPage.hpp"
#include "pages/CompilationPage.hpp"
#include "pages/ConflictsPage.hpp"
#include "pages/DashboardPage.hpp"
#include "pages/DependenciesPage.hpp"
#include "pages/PluginsPage.hpp"
#include "pages/AutoMergerPage.hpp"
#include "pages/DiagnosticsPage.hpp"
#include "pages/ReportsPage.hpp"
#include "pages/SettingsPage.hpp"
#include "pages/ValidationPage.hpp"
#include "widgets/Widgets.hpp"

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
#include <QPainter>
#include <QShortcut>
#include <QTimer>

#include <algorithm>
#include <QFrame>
#include <QResizeEvent>
#include "widgets/Glossary.hpp"
#include "widgets/QuickSearch.hpp"
#include <QSqlQuery>
#include <QStyledItemDelegate>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

namespace gui {

namespace {

enum NavRole { kStackIndex = Qt::UserRole, kIcon, kBadge, kBadgeColor };

// Navigation rows: group headings (no stack index) and pages with an icon and an optional count badge.
class NavDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& i) const override
    {
        return {200, i.data(kStackIndex).isValid() ? 38 : 30};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = o.rect;
        const QString text = i.data(Qt::DisplayRole).toString();
        if (!i.data(kStackIndex).isValid()) {
            QFont f = o.font;
            f.setPointSizeF(f.pointSizeF() * 0.78);
            f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
            p->setFont(f);
            p->setPen(QColor(0x6f, 0x85, 0x7a));
            p->drawText(r.adjusted(18, 8, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text.toUpper());
            p->restore();
            return;
        }
        const bool selected = o.state & QStyle::State_Selected;
        const bool hover = o.state & QStyle::State_MouseOver;
        const QRectF pill = QRectF(r).adjusted(8, 2, -8, -2);
        if (selected || hover) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? QColor(0x1a, 0x2a, 0x21) : QColor(0x18, 0x20, 0x1c));
            p->drawRoundedRect(pill, 7, 7);
            if (selected) {
                p->setBrush(theme::kAccent);
                p->drawRoundedRect(QRectF(pill.left(), pill.top() + 6, 3, pill.height() - 12), 1.5, 1.5);
            }
        }
        const QColor fg = selected ? theme::kAccent : hover ? theme::kText : QColor(0xa9, 0xbc, 0xb2);
        p->drawPixmap(QPointF(pill.left() + 12, pill.center().y() - 9), linePixmap(i.data(kIcon).toString(), fg, 18));
        p->setPen(fg);
        p->setFont(o.font);
        p->drawText(QRectF(pill.left() + 40, pill.top(), pill.width() - 80, pill.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    text);
        const int badge = i.data(kBadge).toInt();
        if (badge > 0) {
            const QString n = badge > 999 ? QString("999+") : QString::number(badge);
            QFont f = o.font;
            f.setPointSizeF(f.pointSizeF() * 0.8);
            f.setBold(true);
            p->setFont(f);
            const int w = QFontMetrics(f).horizontalAdvance(n) + 14;
            const QRectF b(pill.right() - 10 - w, pill.center().y() - 9, w, 18);
            const bool red = i.data(kBadgeColor).toString() == "red";
            p->setPen(Qt::NoPen);
            p->setBrush(red ? QColor(0x2a, 0x16, 0x16) : QColor(0x2a, 0x26, 0x14));
            p->drawRoundedRect(b, 9, 9);
            p->setPen(red ? theme::kRed : theme::kYellow);
            p->drawText(b, Qt::AlignCenter, n);
        }
        p->restore();
    }
};

} // namespace

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
    navPanel->setFixedWidth(236);
    auto* nl = new QVBoxLayout(navPanel);
    nl->setContentsMargins(0, 0, 0, 0);
    nl->setSpacing(0);
    // Brand: the app icon beside the name.
    auto* brand = new QWidget;
    auto* bl = new QHBoxLayout(brand);
    bl->setContentsMargins(18, 18, 18, 12);
    bl->setSpacing(10);
    auto* logo = new QLabel;
    logo->setPixmap(theme::appIcon().pixmap(34, 34));
    bl->addWidget(logo);
    auto* names = new QVBoxLayout;
    names->setSpacing(0);
    auto* title = new QLabel("MULTIPLEXER");
    title->setObjectName("NavBrand");
    auto* subtitle = new QLabel("Fallout 4 · Renewed");
    subtitle->setObjectName("NavBrandSub");
    names->addWidget(title);
    names->addWidget(subtitle);
    bl->addLayout(names, 1);
    nl->addWidget(brand);
    nav_ = new QListWidget;
    nav_->setObjectName("NavList");
    nav_->setFocusPolicy(Qt::NoFocus);
    nav_->setItemDelegate(new NavDelegate(nav_));
    nav_->setMouseTracking(true);
    nav_->viewport()->setAttribute(Qt::WA_Hover);
    nl->addWidget(nav_, 1);
    auto* goTo = new QPushButton("Go to…  (Ctrl+K)");
    goTo->setObjectName("LinkButton");
    goTo->setCursor(Qt::PointingHandCursor);
    goTo->setIcon(lineIcon("scan", theme::kMuted, 14));
    goTo->setToolTip("Jump to any page, plugin or record by typing part of its name");
    goTo->setStyleSheet("margin-left: 12px;");
    connect(goTo, &QPushButton::clicked, this, [this] { quickSearch(); });
    nl->addWidget(goTo, 0, Qt::AlignLeft);
    auto* ctrlK = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    connect(ctrlK, &QShortcut::activated, this, [this] { quickSearch(); });
    auto* glossary = new QPushButton("Glossary  (F1)");
    glossary->setObjectName("LinkButton");
    glossary->setStyleSheet("margin-left: 12px; margin-bottom: 4px;");
    glossary->setCursor(Qt::PointingHandCursor);
    glossary->setIcon(lineIcon("info", theme::kMuted, 14));
    glossary->setToolTip("What the words in this app mean: plugin, master, override, ESL, ...");
    connect(glossary, &QPushButton::clicked, this, [this] { showGlossary(this); });
    nl->addWidget(glossary, 0, Qt::AlignLeft);
    auto* f1 = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(f1, &QShortcut::activated, this, [this] { showGlossary(this); });
    auto* version = new QLabel("v" FO4MX_VERSION " · by Romewolf");
    version->setObjectName("NavFooter");
    nl->addWidget(version);
    h->addWidget(navPanel);

    stack_ = new QStackedWidget;
    h->addWidget(stack_, 1);
    setCentralWidget(central);

    // Page names are the internal ones (links between pages, shortcuts, screenshots); labels are shown.
    addPage("Dashboard", "Dashboard", "dashboard", new DashboardPage(app_));
    addGroup("Your mods");
    addPage("Changes", "Changes", "changes", new ChangesPage(app_));
    addPage("Plugins", "Plugins", "plugins", new PluginsPage(app_));
    addPage("Conflicts", "Conflicts", "conflicts", new ConflictsPage(app_));
    addPage("Assets", "Assets", "assets", new AssetsPage(app_));
    addPage("Dependencies", "Dependencies", "dependencies", new DependenciesPage(app_));
    addGroup("Build");
    addPage("Compilation", "Compilation", "compile", new CompilationPage(app_));
    addPage("Validation", "Validation", "validation", new ValidationPage(app_));
    addPage("Reports", "Reports", "reports", new ReportsPage(app_));
    addGroup("Tools");
    addPage("Advanced Users Auto-Merger", "Auto-Merger", "automerge", new AutoMergerPage(app_));
    addPage("Diagnostics", "Diagnostics", "diagnostics", new DiagnosticsPage(app_));
    addPage("Settings", "Settings", "settings", new SettingsPage(app_));

    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        auto* item = nav_->item(row);
        if (!item || !item->data(kStackIndex).isValid())
            return;
        const int index = item->data(kStackIndex).toInt();
        stack_->setCurrentIndex(index);
        if (index >= 0 && index < pages_.size())
            pages_[index]->refresh();
    });
    // Ctrl+1..9: the pages in order.
    for (int i = 0; i < pages_.size() && i < 9; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        connect(sc, &QShortcut::activated, this, [this, i] {
            for (auto it = navRow_.begin(); it != navRow_.end(); ++it)
                if (pageIndex_.value(it.key()) == i)
                    nav_->setCurrentRow(it.value());
        });
    }

    // Status bar: stage, progress, cancel.
    stageLabel_ = new QLabel("Ready");
    progress_ = new QProgressBar;
    progress_->setFixedWidth(260);
    progress_->setTextVisible(false);
    progress_->hide();
    cancel_ = new QPushButton("Stop");
    cancel_->setObjectName("Danger");
    cancel_->setIcon(lineIcon("block", QColor(0xff, 0xd6, 0xd6), 14));
    cancel_->setToolTip("Stop the task that is running (nothing is written half-way: the results stay as before)");
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
        stageClock_.start();
    });
    connect(app_, &AppController::progressChanged, this, [this](qint64 done, qint64 total, const QString& detail) {
        QString left;
        if (total > 0) {
            progress_->setRange(0, 1000);
            progress_->setValue(int(done * 1000 / total));
            // Time left in this stage, once enough of it has run to tell.
            const double f = double(done) / double(total);
            const qint64 ms = stageClock_.isValid() ? stageClock_.elapsed() : 0;
            if (f > 0.05 && f < 1.0 && ms > 3000) {
                const qint64 rest = qint64(ms * (1.0 - f) / f / 1000.0);
                left = rest < 60 ? QString("about %1 s left").arg(std::max<qint64>(rest, 1))
                                 : QString("about %1 min left").arg((rest + 30) / 60);
            }
        }
        QString text = stageLabel_->text().section("…", 0, 0) + "…";
        if (!left.isEmpty())
            text += "  (" + left + ")";
        if (!detail.isEmpty())
            text += "  " + detail;
        stageLabel_->setText(text);
    });
    connect(app_, &AppController::taskFinished, this, [this](const QString& task, bool ok, const QString& msg) {
        stageLabel_->setText(QString("%1 %2 — %3").arg(task, ok ? "complete" : "failed", msg));
        if (ok)
            showSummary(task, msg);
        if (!ok && msg != "Cancelled")
            QMessageBox::warning(this, task + " failed", msg);
    });
    connect(app_, &AppController::dataChanged, this, [this] {
        refreshAll();
        refreshBadges();
    });
    connect(app_, &AppController::logMessage, this, [this](int level, const QString& text) {
        QString prefix = level >= 4 ? "ERROR " : level == 3 ? "WARN  " : "";
        log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + prefix + text);
    });

    for (auto* p : pages_)
        connect(p, &Page::navigate, this, &MainWindow::showPage);

    nav_->setCurrentRow(0);
    refreshBadges();
}

void MainWindow::addGroup(const QString& title)
{
    auto* item = new QListWidgetItem(title);
    item->setFlags(Qt::NoItemFlags);
    nav_->addItem(item);
}

void MainWindow::addPage(const QString& name, const QString& label, const QString& icon, Page* page)
{
    const int index = stack_->addWidget(page);
    pageIndex_[name] = index;
    pages_.push_back(page);
    auto* item = new QListWidgetItem(label);
    item->setData(kStackIndex, index);
    item->setData(kIcon, icon);
    item->setToolTip(name);
    nav_->addItem(item);
    navRow_[name] = nav_->count() - 1;
    pageLabels_.emplace_back(label, name);
}

void MainWindow::quickSearch()
{
    showQuickSearch(this, app_, pageLabels_, [this](const QuickSearchTarget& t) {
        if (t.kind == "page") {
            showPage(t.value);
        } else if (t.kind == "plugin") {
            showPage("Plugins");
            if (auto* p = findChild<PluginsPage*>())
                p->focusPlugin(t.value);
        } else if (t.kind == "conflict") {
            showPage("Conflicts");
            if (auto* p = findChild<ConflictsPage*>())
                p->focusRecord(t.value);
        } else if (t.kind == "record") {
            showPage("Dependencies");
            if (auto* p = findChild<DependenciesPage*>())
                p->focusRecord(t.value);
        }
    });
}

void MainWindow::showPage(const QString& name)
{
    if (navRow_.contains(name))
        nav_->setCurrentRow(navRow_[name]);
}

void MainWindow::refreshBadges()
{
    QSqlDatabase db = app_->readDb();
    auto count = [&](const QString& sql) -> int {
        if (!db.isOpen())
            return 0;
        QSqlQuery q(db);
        return q.exec(sql) && q.next() ? q.value(0).toInt() : 0;
    };
    auto set = [&](const QString& page, int n, const char* color) {
        if (auto* item = nav_->item(navRow_.value(page, -1))) {
            item->setData(kBadge, n);
            item->setData(kBadgeColor, QString(color));
        }
    };
    set("Changes", count("SELECT COUNT(*) FROM Changes WHERE affects_build = 1"), "amber");
    const auto* plan = app_->pipeline() ? app_->pipeline()->patchPlan() : nullptr;
    set("Conflicts", plan ? int(plan->pending) : 0, "amber");
    set("Validation", count("SELECT COUNT(*) FROM ValidationResults WHERE severity IN ('Error', 'Critical')"), "red");
    set("Advanced Users Auto-Merger", count("SELECT COUNT(*) FROM AutoMergePlugins WHERE status LIKE 'Kept out%'"), "red");
    nav_->viewport()->update();
}

void MainWindow::showSummary(const QString& task, const QString& message)
{
    QSqlDatabase db = app_->readDb();
    auto count = [&](const QString& sql) -> int {
        if (!db.isOpen())
            return 0;
        QSqlQuery q(db);
        return q.exec(sql) && q.next() ? q.value(0).toInt() : 0;
    };
    auto meta = [&](const QString& key) -> QString {
        if (!db.isOpen())
            return {};
        QSqlQuery q(db);
        q.prepare("SELECT value FROM Meta WHERE key = ?");
        q.addBindValue(key);
        return q.exec() && q.next() ? q.value(0).toString() : QString();
    };
    QString title, text;
    std::vector<std::pair<QString, QString>> actions; // (button, page)
    const int active = count("SELECT COUNT(*) FROM Plugins WHERE active = 1");
    const int will = count("SELECT COUNT(*) FROM Plugins WHERE eligibility = 'Eligible'");
    const auto* plan = app_->pipeline() ? app_->pipeline()->patchPlan() : nullptr;
    const int choices = plan ? int(plan->pending) : 0;
    const int errors = count("SELECT COUNT(*) FROM ValidationResults WHERE severity IN ('Error', 'Critical')");
    const int warnings = count("SELECT COUNT(*) FROM ValidationResults WHERE severity = 'Warning'");
    if (task == "Scan") {
        title = "Scan complete";
        text = QString("%1 plugins in your load order. Next: Analyse, to see what can be compiled.").arg(formatCount(active));
        actions = {{"Plugins", "Plugins"}};
    } else if (task == "Analyse") {
        title = "Analysis complete";
        text = QString("%1 of %2 plugins will compile; %3 stay in the load order.")
                   .arg(formatCount(will), formatCount(active), formatCount(std::max(0, active - will)));
        if (choices)
            text += QString(" %1 patch choice(s) wait for you.").arg(choices);
        actions = {{"Plugins", "Plugins"}};
        if (choices)
            actions.push_back({"Review choices", "Conflicts"});
        actions.push_back({"Compile", "Compilation"});
    } else if (task == "Compile" || task == "Rebuild") {
        title = task == "Rebuild" ? "Rebuild complete" : "Compile complete";
        const int kept = count("SELECT COUNT(*) FROM Plugins WHERE eligibility_reason LIKE 'Kept out automatically%'");
        text = QString("%1 plugins compiled into %2 output plugins.")
                   .arg(meta("compiled_plugins").isEmpty() ? QString("—") : meta("compiled_plugins"),
                        meta("output_plugins").isEmpty() ? QString("—") : meta("output_plugins"));
        if (kept)
            text += QString(" %1 kept out automatically (see their reasons on the Plugins page).").arg(kept);
        if (errors || warnings)
            text += QString(" Validation: %1 error(s), %2 warning(s).").arg(errors).arg(warnings);
        if (errors || warnings)
            actions.push_back({"Validation", "Validation"});
        actions.push_back({"Export and install", "Compilation"});
    } else if (task == "Export") {
        title = "Export complete";
        text = message + ". Next: install it (Compilation page, How to install), then start a new game.";
        actions = {{"How to install", "Compilation"}};
    } else {
        return;
    }
    if (!summary_) {
        summary_ = new QFrame(centralWidget());
        summary_->setObjectName("Toast");
        summary_->setStyleSheet("QFrame#Toast { background: #16241d; border: 1px solid #2f6b4c; border-radius: 10px; }");
        summaryTimer_ = new QTimer(this);
        summaryTimer_->setSingleShot(true);
        connect(summaryTimer_, &QTimer::timeout, summary_, &QWidget::hide);
    }
    // Rebuilt each time: its buttons depend on the task.
    if (auto* old = summary_->layout()) {
        while (auto* item = old->takeAt(0)) {
            if (auto* w = item->widget())
                w->deleteLater();
            else if (auto* sub = item->layout()) {
                while (auto* si = sub->takeAt(0)) {
                    if (si->widget())
                        si->widget()->deleteLater();
                    delete si;
                }
            }
            delete item;
        }
        delete old;
    }
    auto* l = new QVBoxLayout(summary_);
    l->setContentsMargins(16, 12, 12, 12);
    l->setSpacing(6);
    auto* head = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(linePixmap(errors ? "warning" : "check", errors ? theme::kYellow : theme::kGreen, 18));
    auto* t = new QLabel(title);
    t->setObjectName("OptionTitle");
    auto* close = new QPushButton("×");
    close->setObjectName("LinkButton");
    close->setFixedWidth(24);
    close->setToolTip("Close");
    connect(close, &QPushButton::clicked, summary_, &QWidget::hide);
    head->addWidget(icon);
    head->addWidget(t, 1);
    head->addWidget(close);
    l->addLayout(head);
    auto* body = new QLabel(text);
    body->setWordWrap(true);
    body->setObjectName("OptionDescription");
    l->addWidget(body);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    for (const auto& [label, page] : actions) {
        auto* b = makeButton(label);
        connect(b, &QPushButton::clicked, this, [this, page = page] {
            showPage(page);
            summary_->hide();
        });
        row->addWidget(b);
    }
    l->addLayout(row);
    // Sized from its contents (word wrap needs a width; the window may not have been shown yet).
    const int width = 420, inner = width - 16 - 12;
    body->setFixedWidth(inner);
    int height = 12 + 12;                                                       // margins
    height += std::max({18, t->sizeHint().height(), close->sizeHint().height()}); // title row
    height += 6 + body->heightForWidth(inner);
    if (!actions.empty()) {
        QPushButton probe("X");
        height += 6 + probe.sizeHint().height();
    }
    summary_->setFixedSize(width, height);
    placeSummary();
    summary_->show();
    summary_->raise();
    summaryTimer_->start(30000);
}

void MainWindow::placeSummary()
{
    if (!summary_ || !centralWidget())
        return;
    const auto area = centralWidget()->rect();
    summary_->move(area.right() - summary_->width() - 20, area.bottom() - summary_->height() - 16);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    placeSummary();
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
    if (app_->hasUnsavedSettings()) {
        // Changed settings are kept while the app runs; ask before they are lost.
        auto answer = QMessageBox::question(
            this, "Unsaved settings",
            "Some settings have changed and are not saved. Save them before quitting?",
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            e->ignore();
            return;
        }
        if (answer == QMessageBox::Save)
            app_->saveSettings();
    }
    e->accept();
}

} // namespace gui
