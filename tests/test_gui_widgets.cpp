// GUI: the reading aids added to the pages (sortable tables, the column chooser, plain status words, quick
// search, the glossary, plugin selection and the summary card after a task). Drives the real widgets
// offscreen against a small results database in a temporary folder; nothing of the user's is touched.

#include "AppController.hpp"
#include "Format.hpp"
#include "MainWindow.hpp"
#include "pages/ConflictsPage.hpp"
#include "db/Database.hpp"
#include "db/Schema.hpp"
#include "models/SqlModel.hpp"
#include "pages/CompilationPage.hpp"
#include "pages/PluginsPage.hpp"
#include "pages/AssetsPage.hpp"
#include "pages/ValidationPage.hpp"
#include "widgets/Glossary.hpp"
#include "widgets/PluginSelection.hpp"
#include "widgets/QuickSearch.hpp"
#include "widgets/Widgets.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTimer>

#include <functional>
#include <memory>

namespace {

// Runs `step` on the next modal dialog or popup menu once it is open (the code under test blocks in exec()).
// A second timer closes anything left open, so a failed step cannot hang the test.
void whenOpen(const std::function<void(QWidget*)>& step)
{
    QTimer::singleShot(0, [step] {
        QWidget* w = QApplication::activePopupWidget();
        if (!w)
            w = QApplication::activeModalWidget();
        if (w)
            step(w);
    });
    QTimer::singleShot(5000, [] {
        if (auto* p = QApplication::activePopupWidget())
            p->close();
        if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
            d->reject();
    });
}

template <class T>
T* childWithText(QWidget* parent, const QString& text)
{
    for (auto* w : parent->findChildren<T*>())
        if (w->text() == text)
            return w;
    return nullptr;
}

} // namespace

class GuiWidgetsTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    std::unique_ptr<gui::AppController> app_;
    std::unique_ptr<gui::MainWindow> window_;

    QString dbPath() const { return dir_.filePath("results.sqlite"); }

    // Four plugins: two compile, one stays (it holds back a master), one is switched off by the user.
    void writeResults()
    {
        QFile::remove(dbPath());
        fo4mx::db::Database db(gui::toPath(dbPath()));
        db.exec(fo4mx::db::kSchema);
        db.exec("INSERT INTO Plugins (id, name, type, load_order, active, base_game, creation_club, record_count, "
                "eligibility, eligibility_reason, record_types) VALUES "
                "(1, 'Fallout4.esm', 'ESM', 0, 1, 1, 0, 900, 'Blocked', 'Base game', 'WEAP'),"
                "(2, 'Weapons.esp', 'ESP', 1, 1, 0, 0, 30, 'Eligible', '', 'WEAP,KYWD'),"
                "(3, 'Armour.esp', 'ESP', 2, 1, 0, 0, 20, 'Eligible', '', 'ARMO'),"
                "(4, 'Quest.esp', 'ESP', 3, 1, 0, 0, 50, 'Blocked', 'Uses scripts that name plugins', 'QUST'),"
                "(5, 'Mine.esp', 'ESP', 4, 1, 0, 0, 10, 'Blocked', 'Excluded by user', 'MISC'),"
                "(6, 'Off.esp', 'ESP', -1, 0, 0, 0, 5, 'Inactive', '', 'MISC')");
    }
    void start()
    {
        window_.reset();
        app_ = std::make_unique<gui::AppController>();
        window_ = std::make_unique<gui::MainWindow>(app_.get());
    }
    QWidget* currentPage() { return window_->findChild<QStackedWidget*>()->currentWidget(); }

private slots:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        QCoreApplication::setOrganizationName("fo4mx-gui-tests");
        QCoreApplication::setApplicationName("widgets");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().clear();
        QSettings().setValue("advanced/database", dbPath());
        writeResults();
    }

    void init() { start(); }

    void cleanup()
    {
        window_.reset();
        app_.reset();
    }

    // The status words the tables show (an SQL expression) and the ones the detail panes show (C++) agree.
    void plain_status_words_match_their_sql()
    {
        QSqlDatabase db = app_->readDb();
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("SELECT name, eligibility, eligibility_reason, base_game, " + gui::plainStatusSql() +
                       " FROM Plugins ORDER BY id"));
        QStringList words;
        while (q.next()) {
            QCOMPARE(q.value(4).toString(),
                     gui::plainStatus(q.value(1).toString(), q.value(2).toString(), q.value(3).toBool()));
            words << q.value(4).toString();
        }
        QCOMPARE(words, (QStringList{"Base game", "Will compile", "Will compile", "Stays in load order", "Kept by you",
                                     "Not enabled"}));
        QCOMPARE(gui::plainStatus("Blocked", "Not chosen for compilation (only chosen plugins)", false),
                 QString("Kept by you"));
        // Master Redirection: stays as a copy pointing at the outputs.
        const QString reason = "Uses scripts that name plugins; Redirected (Master Redirection): it stays";
        QCOMPARE(gui::plainStatus("Blocked", reason, false), QString("Stays, redirected"));
        QVERIFY(q.exec("SELECT " + gui::plainStatusSql() +
                       " FROM (SELECT 'Blocked' AS eligibility, '" + reason + "' AS eligibility_reason, 0 AS base_game)"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("Stays, redirected"));
    }

    // Clicking a header sorts by running the query again; LIKE patterns (% signs) and bindings survive it, and
    // column -1 goes back to the query's own order.
    void sorting_runs_the_query_again()
    {
        gui::SqlModel model;
        QVERIFY(model.load(app_->readDb(),
                           "SELECT name, record_count FROM Plugins WHERE name LIKE '%o%' AND active = ? ORDER BY id",
                           {1}));
        auto names = [&] {
            QStringList out;
            for (int r = 0; r < model.rowCount(); ++r)
                out << model.value(r, 0).toString();
            return out;
        };
        QCOMPARE(names(), (QStringList{"Fallout4.esm", "Weapons.esp", "Armour.esp"}));
        model.sort(1, Qt::DescendingOrder);
        QCOMPARE(names(), (QStringList{"Fallout4.esm", "Weapons.esp", "Armour.esp"}));
        model.sort(1, Qt::AscendingOrder);
        QCOMPARE(names(), (QStringList{"Armour.esp", "Weapons.esp", "Fallout4.esm"}));
        QCOMPARE(model.value(0, 1).toInt(), 20); // rows keep their columns
        model.sort(0, Qt::AscendingOrder);
        QCOMPARE(names(), (QStringList{"Armour.esp", "Fallout4.esm", "Weapons.esp"}));
        model.sort(-1);
        QCOMPARE(names(), (QStringList{"Fallout4.esm", "Weapons.esp", "Armour.esp"}));
    }

    // The header's right-click menu lists the columns except the key ones, shows each hidden or not, and
    // switching one on shows it and reports the change.
    void column_chooser_shows_and_hides_columns()
    {
        gui::SqlModel model;
        QVERIFY(model.load(app_->readDb(), "SELECT id, name, record_count FROM Plugins"));
        QTableView view;
        view.setModel(&model);
        QList<std::pair<int, bool>> changes;
        gui::enableColumnChooser(&view, {2}, {0}, [&](int c, bool hidden) { changes.push_back({c, hidden}); });
        QVERIFY(view.isColumnHidden(2));
        QVERIFY(!view.isColumnHidden(1));
        QStringList offered;
        bool nameShown = false, countShown = true;
        whenOpen([&](QWidget* w) {
            auto* menu = qobject_cast<QMenu*>(w);
            if (!menu)
                return;
            for (auto* a : menu->actions()) {
                offered << a->text();
                if (a->text() == "name")
                    nameShown = a->isChecked();
                if (a->text() == "record_count") {
                    countShown = a->isChecked();
                    a->setChecked(true);
                }
            }
            menu->close();
        });
        emit view.horizontalHeader()->customContextMenuRequested(QPoint(2, 2));
        QCOMPARE(offered, (QStringList{"name", "record_count"}));
        QVERIFY(nameShown);
        QVERIFY(!countShown);
        QVERIFY(!view.isColumnHidden(2));
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0].first, 2);
        QVERIFY(!changes[0].second);
    }

    // Quick search: Enter opens the first result for the text typed, even when typed faster than the results
    // refresh; plugins are found by part of their name.
    void quick_search_opens_what_was_typed()
    {
        const std::vector<std::pair<QString, QString>> pages = {{"Dashboard", "Dashboard"}, {"Settings", "Settings"}};
        auto run = [&](const QString& text) {
            gui::QuickSearchTarget picked;
            whenOpen([&](QWidget* w) {
                auto* field = w->findChild<QLineEdit*>();
                if (!field)
                    return;
                QTest::keyClicks(field, text);
                QTest::keyClick(field, Qt::Key_Return);
            });
            gui::showQuickSearch(window_.get(), app_.get(), pages, [&](const gui::QuickSearchTarget& t) { picked = t; });
            return std::pair{picked.kind, picked.value};
        };
        QCOMPARE(run("sett"), (std::pair<QString, QString>{"page", "Settings"}));
        QCOMPARE(run("weap"), (std::pair<QString, QString>{"plugin", "Weapons.esp"}));
    }

    // A plugin picked in quick search (or a plugin name clicked in a reason) is shown and selected on the
    // Plugins page.
    void focusing_a_plugin_selects_it()
    {
        auto* page = window_->findChild<gui::PluginsPage*>();
        QVERIFY(page);
        window_->showPage("Plugins");
        page->focusPlugin("armour.esp");
        bool found = false;
        for (auto* view : page->findChildren<QTableView*>()) {
            auto* model = qobject_cast<gui::SqlModel*>(view->model());
            if (!model || !view->selectionModel()->hasSelection())
                continue;
            const int row = view->selectionModel()->selectedRows().value(0).row();
            found = model->value(row, 1).toString() == "Armour.esp";
        }
        QVERIFY(found);
    }

    // The glossary's filter narrows the terms, and says so when none matches.
    void glossary_filter_narrows_the_terms()
    {
        QString light, none;
        whenOpen([&](QWidget* w) {
            auto* filter = w->findChild<QLineEdit*>();
            auto* text = w->findChild<QTextBrowser*>();
            if (!filter || !text)
                return;
            filter->setText("light plugin");
            light = text->toPlainText();
            filter->setText("zzzz");
            none = text->toPlainText();
            static_cast<QDialog*>(w)->reject();
        });
        gui::showGlossary(window_.get());
        QVERIFY(light.contains("Light plugin (ESL flag)"));
        QVERIFY(!light.contains("Winner"));
        QVERIFY(none.contains("No term matches"));
    }

    // Plugins to compile: the list holds the plugins that could be compiled (not the base game or inactive
    // ones), and switching one off excludes it.
    void plugin_selection_lists_and_switches_plugins()
    {
        auto* sel = window_->findChild<gui::PluginSelection*>();
        QVERIFY(sel);
        sel->refresh();
        auto* table = sel->findChild<QTableWidget*>();
        QVERIFY(table);
        QStringList names;
        int weapons = -1;
        for (int r = 0; r < table->rowCount(); ++r) {
            names << table->item(r, 1)->text();
            if (names.back() == "Weapons.esp")
                weapons = r;
        }
        QCOMPARE(names, (QStringList{"Weapons.esp", "Armour.esp", "Quest.esp", "Mine.esp"}));
        QCOMPARE(table->item(weapons, 0)->checkState(), Qt::Checked);
        QCOMPARE(table->item(3, 2)->text(), QString("Switched off"));
        table->item(weapons, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(app_->excludedPlugins().contains("Weapons.esp"));
        app_->setExcluded("Weapons.esp", false);
        QVERIFY(!app_->excludedPlugins().contains("Weapons.esp"));
    }

    // "Compile more plugins" says what each switch would change on this load order, from the last analysis.
    void compile_more_plugins_says_what_it_would_free()
    {
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("UPDATE Plugins SET holds_back = 2, redirect_note = 'Would free them: turn on Master Redirection' "
                    "WHERE name = 'Quest.esp'");
        }
        auto* page = window_->findChild<gui::CompilationPage*>();
        QVERIFY(page);
        page->refresh();
        auto said = [&](const QString& start) {
            for (auto* l : page->findChildren<QLabel*>())
                if (!l->isHidden() && l->text().startsWith(start))
                    return true;
            return false;
        };
        QVERIFY(said("Your load order: 1 plugin could be redirected, freeing 2 plugins they hold back."));
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("UPDATE Plugins SET holds_back = NULL, redirect_note = NULL WHERE name = 'Quest.esp'");
        }
        page->refresh();
        QVERIFY(!said("Your load order: 1 plugin could be redirected"));
    }

    // After Analyse, a card says what will compile in plain words and its buttons open the pages.
    void summary_card_after_analysis()
    {
        emit app_->taskFinished("Analyse", true, "done");
        QFrame* toast = nullptr;
        for (auto* f : window_->findChildren<QFrame*>())
            if (f->objectName() == "Toast")
                toast = f;
        QVERIFY(toast);
        QVERIFY(!toast->isHidden());
        bool said = false;
        for (auto* l : toast->findChildren<QLabel*>())
            said = said || l->text().startsWith("2 of 5 plugins will compile; 3 stay in the load order.");
        QVERIFY(said);
        QVERIFY(childWithText<QPushButton>(toast, "Compile"));
        auto* plugins = childWithText<QPushButton>(toast, "Plugins");
        QVERIFY(plugins);
        QTest::mouseClick(plugins, Qt::LeftButton);
        QVERIFY(qobject_cast<gui::PluginsPage*>(currentPage()));
        QVERIFY(toast->isHidden());
        // A failed task shows no card.
        emit app_->taskFinished("Scan", false, "Cancelled");
        QVERIFY(toast->isHidden());
    }

    // The Conflict Centre says which records wait for the user's choice and which have one saved (a plan
    // choice or a chosen winner), and its chips list them.
    void conflict_centre_shows_what_waits_for_the_user()
    {
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("INSERT INTO Conflicts (id, origin_plugin_id, object_id, form_id, signature, editor_id, conflict_type, "
                    "type_name, severity, winner_plugin_id, chain_length, chain, summary, preserved) VALUES "
                    "(1, 1, 16, '00000010', 'WEAP', 'Pistol', 3, 'Hard Conflict', 'High', 3, 3, 'a, b, c', '', 0),"
                    "(2, 1, 17, '00000011', 'WEAP', 'Rifle', 2, 'Compatible Merge', 'Low', 2, 2, 'a, b', '', 0),"
                    "(3, 1, 18, '00000012', 'WEAP', 'Knife', 1, 'Simple Override', 'Low', 2, 2, 'a, b', '', 0)");
            db.exec("INSERT INTO ConflictReview VALUES (1, 16, 'waiting')");
            db.exec("INSERT INTO UserResolutions VALUES ('fallout4.esm', 17, 'Weapons.esp')");
        }
        start();
        auto* page = window_->findChild<gui::ConflictsPage*>();
        QVERIFY(page);
        page->refresh();
        QAbstractItemModel* model = page->findChild<gui::SqlModel*>();
        QVERIFY(model);
        int review = -1;
        for (int c = 0; c < model->columnCount(); ++c)
            if (model->headerData(c, Qt::Horizontal).toString() == "Review")
                review = c;
        QVERIFY(review >= 0);
        QMap<QString, QString> states;
        for (int r = 0; r < model->rowCount(); ++r)
            states[model->index(r, 2).data().toString()] = model->index(r, review).data().toString();
        QCOMPARE(states.value("Pistol"), QString("Waiting for you"));
        QCOMPARE(states.value("Rifle"), QString("Your choice")); // the winner chosen, plugin name in any case
        QCOMPARE(states.value("Knife"), QString());
        bool told = false;
        for (auto* l : page->findChildren<QLabel*>())
            told = told || l->text().contains("1 record waits for your choice");
        QVERIFY(told);
        QVERIFY(childWithText<QPushButton>(page, "Accept Automatic for All")->isEnabled());

        // The chips carry their counts; Review Them lists the waiting records and opens the first.
        QVERIFY(childWithText<QPushButton>(page, "Waiting for you  1"));
        QVERIFY(childWithText<QPushButton>(page, "Hard Conflicts  1"));
        QTest::mouseClick(childWithText<QPushButton>(page, "Review Them"), Qt::LeftButton);
        QCOMPARE(model->rowCount(), 1);
        bool verdict = false;
        for (auto* l : page->findChildren<QLabel*>())
            verdict = verdict || l->text().contains("Waiting for your choice.");
        QVERIFY(verdict);

        auto* chips = page->findChild<gui::FilterChips*>();
        chips->setCurrent("Waiting for you");
        page->refresh();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 2).data().toString(), QString("Pistol"));
        chips->setCurrent("Your choices");
        page->refresh();
        QCOMPARE(model->rowCount(), 1);
        QCOMPARE(model->index(0, 2).data().toString(), QString("Rifle"));

        // Nothing waits: the bulk button is off. No pipeline: accepting cannot run and says so.
        QCOMPARE(app_->acceptAutomatic(), -1);
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("DELETE FROM Conflicts; DELETE FROM ConflictReview; DELETE FROM UserResolutions");
        }
        page->refresh();
        QVERIFY(!childWithText<QPushButton>(page, "Accept Automatic for All")->isEnabled());
    }

    // The changes check that runs when a watched file changes is quiet: no summary card, no dialog, a line in
    // the status bar.
    void auto_check_is_quiet()
    {
        emit app_->taskFinished(gui::AppController::kAutoCheck, true, "Requires Rebuild — 1 change(s)");
        for (auto* f : window_->findChildren<QFrame*>())
            if (f->objectName() == "Toast")
                QVERIFY(f->isHidden());
        emit app_->taskFinished(gui::AppController::kAutoCheck, false, "no install");
        QVERIFY(!QApplication::activeModalWidget());
    }

    // Editing plugins.txt while the app is open starts the changes check a few seconds later, once a build
    // exists; nothing happens before there is one.
    void editing_the_load_order_starts_a_check()
    {
        QDir root(dir_.filePath("install"));
        root.mkpath("Game/Data");
        QFile esm(root.filePath("Game/Data/Fallout4.esm"));
        QVERIFY(esm.open(QIODevice::WriteOnly));
        esm.close();
        const QString plugins = root.filePath("plugins.txt");
        auto write = [&](const QByteArray& text) {
            QFile f(plugins);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(text);
        };
        write("*Weapons.esp\n");
        auto& cfg = app_->config();
        cfg.install.kind = fo4mx::InstallKind::Standalone;
        cfg.install.gamePath = gui::toPath(root.filePath("Game"));
        cfg.install.pluginsFile = gui::toPath(plugins);
        app_->watchInstall();
        QStringList started;
        connect(app_.get(), &gui::AppController::taskStarted, this, [&](const QString& t) { started << t; });

        write("*Weapons.esp\n*Armour.esp\n"); // no build yet: no check
        QTest::qWait(5500);
        QVERIFY(!started.contains(gui::AppController::kAutoCheck));

        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("INSERT INTO Snapshots (kind, created, data) VALUES ('build', 1, '{}')");
        }
        write("*Weapons.esp\n");
        QTRY_VERIFY_WITH_TIMEOUT(started.contains(gui::AppController::kAutoCheck), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!app_->busy(), 20000);
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("DELETE FROM Snapshots");
        }
    }

    // A Precombined Cell Check row links to the cell's files in Assets: <plugin>\<00 + object ID>.
    void precombined_cell_check_opens_its_files_in_assets()
    {
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("INSERT INTO ValidationResults (stage, severity, category, message, plugin, form_id) VALUES "
                    "('pre-export', 'Info', 'Precombined Cell Check', 'New cell ModNCell: interior.', 'ModN.esp', "
                    "'05000801')");
        }
        window_->showPage("Validation");
        auto* page = qobject_cast<gui::ValidationPage*>(currentPage());
        QVERIFY(page);
        page->refresh();
        auto* table = page->findChild<QTableView*>();
        QVERIFY(table);
        int row = -1;
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, 1).data().toString() == "Precombined Cell Check")
                row = r;
        QVERIFY(row >= 0);
        table->setCurrentIndex(table->model()->index(row, 0));
        auto* details = page->findChild<QTextBrowser*>();
        QVERIFY(details);
        QVERIFY(details->toHtml().contains("Show the cell's precombined meshes and previs file in Assets"));
        emit details->anchorClicked(QUrl("assets:" + QString::fromUtf8(QUrl::toPercentEncoding("ModN.esp\\00000801"))));
        auto* assets = qobject_cast<gui::AssetsPage*>(currentPage());
        QVERIFY(assets);
        bool searched = false;
        for (auto* e : assets->findChildren<QLineEdit*>())
            searched |= e->text() == "ModN.esp\\00000801";
        QVERIFY(searched);
        {
            fo4mx::db::Database db(gui::toPath(dbPath()));
            db.exec("DELETE FROM ValidationResults");
        }
    }

    // The Dashboard's steps are its actions: Review opens the Conflict Centre, Play & check Diagnostics.
    void dashboard_steps_open_their_pages()
    {
        window_->showPage("Dashboard");
        auto* journey = currentPage()->findChild<gui::JourneyView*>();
        QVERIFY(journey);
        emit journey->stepActivated(2); // Scan, Analyse, Review, Compile, Install, Play & check
        QCOMPARE(currentPage()->objectName().isEmpty() ? QString(currentPage()->metaObject()->className())
                                                       : currentPage()->objectName(),
                 QString("gui::ConflictsPage"));
        window_->showPage("Dashboard");
        emit journey->stepActivated(5);
        QCOMPARE(QString(currentPage()->metaObject()->className()), QString("gui::DiagnosticsPage"));
        // No duplicate Scan/Analyse buttons in the header any more.
        window_->showPage("Dashboard");
        QVERIFY(!childWithText<QPushButton>(currentPage(), "Validate"));
    }

    // The status bar says a step is still working after its count is done, with a clock that keeps going.
    void status_bar_shows_a_step_still_working()
    {
        emit app_->taskStarted("Analyse");
        emit app_->stageStarted("Analysing dependencies");
        emit app_->progressChanged(100, 100, "record references");
        QLabel* status = nullptr;
        for (auto* l : window_->findChildren<QLabel*>())
            if (l->text().startsWith("Step 1"))
                status = l;
        QVERIFY(status);
        QVERIFY(status->text().contains("100 / 100"));
        QVERIFY(status->text().contains("finishing this step"));
        QVERIFY(status->text().contains("s in this step"));
        QTest::qWait(1200); // the clock ticks without new progress
        QVERIFY(status->text().contains("in this step"));
        emit app_->taskFinished("Analyse", false, "Cancelled");
    }
};

QTEST_MAIN(GuiWidgetsTest)
#include "test_gui_widgets.moc"
