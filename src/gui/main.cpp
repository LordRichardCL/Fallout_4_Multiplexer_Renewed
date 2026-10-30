// Fallout 4 Multiplexer Renewed - desktop application.

#include "AppController.hpp"
#include "Format.hpp"
#include "MainWindow.hpp"
#include "SetupDialog.hpp"
#include "pages/PluginsPage.hpp"
#include "Theme.hpp"

#include "util/Log.hpp"
#include "util/Platform.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>

namespace {

// Developer mode: run every stage against an install and save a screenshot of each page.
int runScreenshots(gui::AppController& controller, gui::MainWindow& window, const QCommandLineParser& args)
{
    auto& cfg = controller.config();
    cfg.install.kind = fo4mx::InstallKind::Standalone;
    if (args.isSet("mo2")) {
        cfg.install.kind = fo4mx::InstallKind::ModOrganizer2;
        cfg.install.mo2Path = gui::toPath(args.value("mo2"));
    }
    cfg.install.gamePath = gui::toPath(args.value("game"));
    cfg.install.pluginsFile = gui::toPath(args.value("plugins"));
    cfg.install.iniDirectory = gui::toPath(args.value("ini-dir"));
    cfg.databasePath = gui::toPath(args.value("db"));
    cfg.exportOptions.outputDirectory = gui::toPath(args.value("out"));
    cfg.compile.outputCount = 10;
    cfg.compile.autoMerge = args.isSet("auto-merge");

    QDir outDir(args.value("screenshots"));
    outDir.mkpath(".");
    {
        gui::SetupDialog setup(&controller);
        setup.show();
        setup.validateNow();
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        setup.grab().save(outDir.filePath("00-setup.png"));
    }
    auto runAndWait = [&](void (gui::AppController::*fn)()) {
        QEventLoop loop;
        auto c = QObject::connect(&controller, &gui::AppController::taskFinished, &loop, [&] { loop.quit(); });
        (controller.*fn)();
        loop.exec();
        QObject::disconnect(c);
    };
    if (args.isSet("check-only")) {
        // Show the state of an earlier build against the current mod space.
        runAndWait(&gui::AppController::runCheck);
    } else {
        runAndWait(&gui::AppController::runScan);
        runAndWait(&gui::AppController::runAnalyse);
        runAndWait(&gui::AppController::runCompile);
        runAndWait(&gui::AppController::runExport);
        // A second, incremental build so the screenshots show build-system state.
        runAndWait(&gui::AppController::runRebuild);
        runAndWait(&gui::AppController::runCheck);
    }

    const QStringList pages = {"Dashboard", "Changes", "Plugins", "Conflicts", "Assets", "Dependencies",
                               "Compilation", "Validation", "Reports", "Settings", "Advanced Users Auto-Merger"};
    for (int i = 0; i < pages.size(); ++i) {
        window.showPage(pages[i]);
        // Select the first row of the main table so detail panes are populated.
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        for (auto* view : window.findChildren<QAbstractItemView*>()) {
            if (view->isVisible() && view->model() && view->model()->rowCount() > 0 && !qobject_cast<QListWidget*>(view)) {
                int row = 0;
                for (int r = 0; r < view->model()->rowCount(); ++r)
                    if (view->model()->index(r, 0).data().toString().endsWith(".dds")) {
                        row = r; // prefer a texture so the preview is shown
                        break;
                    }
                view->setCurrentIndex(view->model()->index(row, 0));
                break;
            }
        }
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        window.grab().save(outDir.filePath(QString("%1-%2.png").arg(i + 1, 2, 10, QChar('0')).arg(pages[i].toLower())));
    }

    // Record types that block compilation, with the plugins the first one holds back.
    window.showPage("Plugins");
    for (auto* b : window.findChildren<QPushButton*>())
        if (b->isVisible() && b->text() == "Blocking Types") {
            b->click();
            for (auto* view : window.findChildren<QTableView*>())
                if (view->isVisible() && view->model() && view->model()->rowCount() > 0) {
                    view->setCurrentIndex(view->model()->index(0, 0));
                    break;
                }
            QEventLoop settle;
            QTimer::singleShot(300, &settle, &QEventLoop::quit);
            settle.exec();
            window.grab().save(outDir.filePath("13-blocking-types.png"));
            break;
        }

    // Record dependency graph with the details panel.
    window.showPage("Dependencies");
    for (auto* tabs : window.findChildren<QTabWidget*>()) {
        if (!tabs->isVisible() || tabs->count() < 2 || tabs->tabText(1) != "Records")
            continue;
        tabs->setCurrentIndex(1);
        for (auto* edit : window.findChildren<QLineEdit*>())
            if (edit->isVisible() && edit->placeholderText().startsWith("Editor ID or load-order")) {
                edit->setText(args.isSet("record") ? args.value("record") : QStringLiteral("Crate"));
                QMetaObject::invokeMethod(edit, "returnPressed");
                break;
            }
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        window.grab().save(outDir.filePath("15-record-details.png"));
        break;
    }

    // Cleaning report with the first finding selected.
    window.showPage("Dependencies");
    for (auto* tabs : window.findChildren<QTabWidget*>()) {
        if (!tabs->isVisible() || tabs->count() < 4 || !tabs->tabText(3).startsWith("Cleaning"))
            continue;
        tabs->setCurrentIndex(3);
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        for (auto* view : tabs->currentWidget()->findChildren<QTableView*>())
            if (view->model() && view->model()->rowCount() > 0) {
                view->setCurrentIndex(view->model()->index(0, 0));
                break;
            }
        QEventLoop settle2;
        QTimer::singleShot(300, &settle2, &QEventLoop::quit);
        settle2.exec();
        window.grab().save(outDir.filePath("16-cleaning.png"));
        break;
    }

    // The Plugins page's text export.
    for (auto* page : window.findChildren<gui::PluginsPage*>()) {
        QFile f(outDir.filePath("plugin-report.txt"));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(page->exportText().toUtf8());
    }

    // Record definition check, filtered to its findings.
    {
        QEventLoop loop;
        auto c = QObject::connect(&controller, &gui::AppController::taskFinished, &loop, [&] { loop.quit(); });
        controller.runDefinitionCheck();
        loop.exec();
        QObject::disconnect(c);
        window.showPage("Validation");
        for (auto* b : window.findChildren<QPushButton*>())
            if (b->isVisible() && b->text() == "Definitions") {
                b->click();
                break;
            }
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        window.grab().save(outDir.filePath("14-definition-check.png"));
    }

    // Traceability: a compiled weapon traced back to its sources and original assets.
    window.showPage("Compilation");
    for (auto* tabs : window.findChildren<QTabWidget*>()) {
        if (!tabs->isVisible() || tabs->count() < 4 || tabs->tabText(1) != "Generated Records")
            continue;
        tabs->setCurrentIndex(1);
        auto* view = qobject_cast<QAbstractItemView*>(tabs->currentWidget());
        if (!view || !view->model())
            break;
        for (int r = 0; r < view->model()->rowCount(); ++r)
            if (view->model()->index(r, 2).data().toString() == "WEAP" &&
                view->model()->index(r, 4).data().toString() == "New") {
                view->setCurrentIndex(view->model()->index(r, 0));
                break;
            }
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        window.grab().save(outDir.filePath("11-traceability.png"));
        break;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Romewolf");
    QApplication::setApplicationName("Fallout4MultiplexerRenewed");
    QApplication::setApplicationDisplayName("Fallout 4 Multiplexer Renewed");
    QApplication::setApplicationVersion(FO4MX_VERSION);

    QCommandLineParser args;
    args.addHelpOption();
    args.addVersionOption();
    for (const char* opt : {"screenshots", "game", "mo2", "plugins", "ini-dir", "db", "out", "record"})
        args.addOption(QCommandLineOption(opt, QString("Developer screenshot mode: %1").arg(opt), "value"));
    args.addOption(QCommandLineOption("check-only", "Developer screenshot mode: only check for changes"));
    args.addOption(QCommandLineOption("auto-merge", "Developer screenshot mode: turn the Auto-Merger on"));
    args.process(app);

    fo4mx::log::init(fo4mx::platform::applicationDataDir() / "logs" / "multiplexer.log");
    gui::theme::apply(app);

    gui::AppController controller;
    // Results and caches from a different build are cleared so nothing stale carries over.
    if (!args.isSet("screenshots"))
        controller.clearResultsAfterUpgrade(QStringLiteral(FO4MX_VERSION " " __DATE__ " " __TIME__));
    if (!args.isSet("screenshots") && gui::SetupDialog::shouldShowAtStartup(&controller)) {
        gui::SetupDialog setup(&controller);
        if (setup.exec() != QDialog::Accepted)
            return 0;
    }
    gui::MainWindow window(&controller);
    window.show();
    if (args.isSet("screenshots"))
        return runScreenshots(controller, window, args);
    QTimer::singleShot(400, &controller, &gui::AppController::checkOnStartup);
    return app.exec();
}
