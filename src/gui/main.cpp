// Fallout 4 Multiplexer Renewed - desktop application.

#include "AppController.hpp"
#include "Format.hpp"
#include "MainWindow.hpp"
#include "Theme.hpp"

#include "util/Log.hpp"
#include "util/Platform.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QListWidget>
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

    QDir outDir(args.value("screenshots"));
    outDir.mkpath(".");
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
                               "Compilation", "Validation", "Reports", "Settings"};
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
    for (const char* opt : {"screenshots", "game", "mo2", "plugins", "ini-dir", "db", "out"})
        args.addOption(QCommandLineOption(opt, QString("Developer screenshot mode: %1").arg(opt), "value"));
    args.addOption(QCommandLineOption("check-only", "Developer screenshot mode: only check for changes"));
    args.process(app);

    fo4mx::log::init(fo4mx::platform::applicationDataDir() / "logs" / "multiplexer.log");
    gui::theme::apply(app);

    gui::AppController controller;
    gui::MainWindow window(&controller);
    window.show();
    if (args.isSet("screenshots"))
        return runScreenshots(controller, window, args);
    QTimer::singleShot(400, &controller, &gui::AppController::checkOnStartup);
    return app.exec();
}
