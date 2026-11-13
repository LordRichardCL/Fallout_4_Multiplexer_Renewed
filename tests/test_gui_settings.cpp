// GUI: the settings state is shared by the pages, kept across navigation, and saved from any page with
// options. Drives the real main window offscreen; settings go to a temporary folder, never the user's.

#include "AppController.hpp"
#include "MainWindow.hpp"
#include "pages/AutoMergerPage.hpp"
#include "pages/CompilationPage.hpp"
#include "pages/SettingsPage.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

template <class T>
T* childWithText(QWidget* parent, const QString& prefix)
{
    for (auto* w : parent->findChildren<T*>())
        if (w->text().startsWith(prefix))
            return w;
    return nullptr;
}

} // namespace

class GuiSettingsTest : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;
    std::unique_ptr<gui::AppController> app_;
    std::unique_ptr<gui::MainWindow> window_;

    void start()
    {
        window_.reset();
        app_ = std::make_unique<gui::AppController>();
        window_ = std::make_unique<gui::MainWindow>(app_.get());
    }
    gui::CompilationPage* compilation() { return window_->findChild<gui::CompilationPage*>(); }
    gui::SettingsPage* settings() { return window_->findChild<gui::SettingsPage*>(); }
    gui::AutoMergerPage* autoMerger() { return window_->findChild<gui::AutoMergerPage*>(); }
    QCheckBox* previsBox() { return childWithText<QCheckBox>(compilation(), "Translate previs files"); }
    QSpinBox* threadsBox()
    {
        for (auto* s : settings()->findChildren<QSpinBox*>())
            if (s->specialValueText() == "Automatic")
                return s;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        QCoreApplication::setOrganizationName("fo4mx-gui-tests");
        QCoreApplication::setApplicationName("settings");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir_.path());
        QSettings().clear();
        // A results database inside the temporary folder.
        QSettings().setValue("advanced/database", dir_.filePath("results.sqlite"));
    }

    void init() { start(); }

    // The reported bug: an option ticked on the Compilation page was lost when another page saved
    // (and reloaded the controls) before the user saved it.
    void compilation_option_survives_navigation_and_other_saves()
    {
        auto* box = previsBox();
        QVERIFY(box);
        QVERIFY(box->isChecked());
        box->setChecked(false);
        QVERIFY(!app_->config().compile.translatePrevis); // in the shared state at once
        QVERIFY(app_->hasUnsavedSettings());
        window_->showPage("Settings");
        app_->setExcluded("Other.esp", true); // another page saves its own setting
        window_->showPage("Advanced Users Auto-Merger");
        window_->showPage("Compilation");
        QVERIFY(!previsBox()->isChecked());
        QVERIFY(!app_->config().compile.translatePrevis);
        QVERIFY(app_->hasUnsavedSettings());
        // The exclusion was saved on its own; the unsaved option was not.
        QCOMPARE(QSettings().value("analysis/excluded").toStringList(), QStringList{"Other.esp"});
        QVERIFY(QSettings().value("compile/translatePrevis", true).toBool());
        app_->setExcluded("Other.esp", false);
    }

    // Save Settings on the Compilation page saves; the value survives a restart.
    void compilation_save_button_persists_across_restart()
    {
        previsBox()->setChecked(false);
        auto* save = childWithText<QPushButton>(compilation(), "Save Settings");
        QVERIFY(save);
        QTest::mouseClick(save, Qt::LeftButton);
        QVERIFY(!app_->hasUnsavedSettings());
        QVERIFY(!QSettings().value("compile/translatePrevis", true).toBool());
        start(); // restart
        QVERIFY(!app_->config().compile.translatePrevis);
        QVERIFY(!previsBox()->isChecked());
        previsBox()->setChecked(true);
        app_->saveSettings();
    }

    // The Settings page had the same bug: its edits were reloaded away by any other page's save.
    void settings_page_edit_survives_other_pages()
    {
        auto* threads = threadsBox();
        QVERIFY(threads);
        threads->setValue(7);
        QCOMPARE(app_->config().scan.threads, 7u);
        window_->showPage("Compilation");
        auto* merge = childWithText<QCheckBox>(autoMerger(), "Enable the Advanced Users Auto-Merger");
        QVERIFY(merge);
        merge->setChecked(!merge->isChecked()); // another page's option
        app_->setExcluded("Other.esp", true);
        window_->showPage("Settings");
        QCOMPARE(threadsBox()->value(), 7);
        // Saving from the Auto-Merger page saves the Settings page's change too: one settings state.
        auto* save = childWithText<QPushButton>(autoMerger(), "Save Settings");
        QVERIFY(save);
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(QSettings().value("advanced/threads").toUInt(), 7u);
        start();
        QCOMPARE(threadsBox()->value(), 7);
        threadsBox()->setValue(0);
        app_->setExcluded("Other.esp", false);
        app_->config().compile.autoMerge = false;
        app_->saveSettings();
    }

    // A typed results database path is kept in the state but used once saved.
    void database_path_is_used_once_saved()
    {
        const QString before = app_->databasePath();
        QLineEdit* db = nullptr;
        for (auto* e : settings()->findChildren<QLineEdit*>())
            if (e->placeholderText() == "SQLite file")
                db = e;
        QVERIFY(db);
        db->setText(dir_.filePath("other.sqlite"));
        QCOMPARE(app_->databasePath(), before);
        app_->saveSettings();
        QCOMPARE(app_->databasePath(), dir_.filePath("other.sqlite"));
        db->setText(before);
        app_->saveSettings();
    }

    // Every page with options has a Save Settings button.
    void pages_with_options_have_save_settings()
    {
        QVERIFY(childWithText<QPushButton>(compilation(), "Save Settings"));
        QVERIFY(childWithText<QPushButton>(autoMerger(), "Save Settings"));
        QVERIFY(childWithText<QPushButton>(settings(), "Save Settings"));
    }

    // Showing a page does not mark anything unsaved.
    void showing_pages_changes_nothing()
    {
        for (const char* page : {"Compilation", "Advanced Users Auto-Merger", "Settings", "Dashboard"})
            window_->showPage(page);
        QVERIFY(!app_->hasUnsavedSettings());
    }

    void cleanup()
    {
        window_.reset();
        app_.reset();
    }
};

QTEST_MAIN(GuiSettingsTest)
#include "test_gui_settings.moc"
