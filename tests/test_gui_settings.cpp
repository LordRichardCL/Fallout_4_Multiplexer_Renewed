// GUI: the settings state is shared by the pages, kept across navigation, and saved from any page with
// options. Drives the real main window offscreen; settings go to a temporary folder, never the user's.

#include "AppController.hpp"
#include "MainWindow.hpp"
#include "pages/AutoMergerPage.hpp"
#include "pages/CompilationPage.hpp"
#include "pages/SettingsPage.hpp"
#include "widgets/PluginSelection.hpp"
#include "widgets/Widgets.hpp"

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
        QVERIFY(!box->isChecked()); // off on the Safest preset (first run)
        box->setChecked(true);
        QVERIFY(app_->config().compile.translatePrevis); // in the shared state at once
        QVERIFY(app_->hasUnsavedSettings());
        window_->showPage("Settings");
        app_->setExcluded("Other.esp", true); // another page saves its own setting
        window_->showPage("Advanced Users Auto-Merger");
        window_->showPage("Compilation");
        QVERIFY(previsBox()->isChecked());
        QVERIFY(app_->config().compile.translatePrevis);
        QVERIFY(app_->hasUnsavedSettings());
        // The exclusion was saved on its own; the unsaved option was not.
        QCOMPARE(QSettings().value("analysis/excluded").toStringList(), QStringList{"Other.esp"});
        QVERIFY(!QSettings().value("compile/translatePrevis", false).toBool());
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

    // Plugins to compile: switching to "Only the ones I choose" is saved at once and survives a restart; a
    // plugin switched on is chosen (and no longer excluded).
    void plugin_selection_mode_and_choices_are_saved()
    {
        auto* mode = compilation()->findChild<gui::SegmentedControl*>();
        gui::SegmentedControl* selectionMode = nullptr;
        for (auto* c : compilation()->findChildren<gui::SegmentedControl*>())
            if (c->button(gui::PluginSelection::kChosen))
                selectionMode = c;
        QVERIFY(mode && selectionMode);
        QTest::mouseClick(selectionMode->button(gui::PluginSelection::kChosen), Qt::LeftButton);
        QVERIFY(app_->onlySelected());
        QVERIFY(QSettings().value("analysis/onlySelected").toBool());
        app_->setExcluded("Weapons.esp", true);
        app_->setChosen({"Weapons.esp"}, true);
        QVERIFY(!app_->excludedPlugins().contains("Weapons.esp"));
        start();
        QVERIFY(app_->onlySelected());
        QCOMPARE(app_->chosenPlugins(), QStringList{"Weapons.esp"});
        app_->setChosen({"Weapons.esp"}, false);
        app_->setOnlySelected(false);
        QVERIFY(!QSettings().value("analysis/onlySelected").toBool());
    }

    // Master Redirection and the light outputs' room to grow: set on the Compilation page, saved, kept across a
    // restart. The per-plugin opt-out (Plugins page) is saved at once.
    void redirection_and_light_headroom_are_saved()
    {
        auto* redirect = childWithText<QCheckBox>(compilation(), "Master Redirection");
        QSpinBox* room = nullptr;
        for (auto* sb : compilation()->findChildren<QSpinBox*>())
            if (sb->suffix() == " records free")
                room = sb;
        QVERIFY(redirect && room);
        QVERIFY(!redirect->isChecked());
        QCOMPARE(room->value(), 200);
        redirect->setChecked(true);
        room->setValue(350);
        QVERIFY(app_->config().compile.redirectMasters);
        QCOMPARE(app_->config().compile.lightHeadroom, 350u);
        app_->saveSettings();
        app_->setRedirect("Quest.esp", false);
        QCOMPARE(QSettings().value("compile/noRedirect").toStringList(), QStringList{"Quest.esp"});
        start();
        QVERIFY(app_->config().compile.redirectMasters);
        QCOMPARE(app_->config().compile.lightHeadroom, 350u);
        QCOMPARE(app_->notRedirected(), QStringList{"Quest.esp"});
        QVERIFY(childWithText<QCheckBox>(compilation(), "Master Redirection")->isChecked());
        app_->setRedirect("Quest.esp", true);
        QVERIFY(app_->notRedirected().isEmpty());
        app_->config().compile.redirectMasters = false;
        app_->config().compile.lightHeadroom = 200;
        app_->saveSettings();
    }

    // Every page with options has a Save Settings button.
    void pages_with_options_have_save_settings()
    {
        QVERIFY(childWithText<QPushButton>(compilation(), "Save Settings"));
        QVERIFY(childWithText<QPushButton>(autoMerger(), "Save Settings"));
        QVERIFY(childWithText<QPushButton>(settings(), "Save Settings"));
    }

    // Presets set only the switches; the selector follows the switches (Advanced when they match neither).
    void compilation_presets_set_switches()
    {
        auto* page = compilation();
        auto* safest = childWithText<QPushButton>(page, "Safest");
        auto* recommended = childWithText<QPushButton>(page, "Recommended");
        auto* custom = childWithText<QPushButton>(page, "Advanced");
        QVERIFY(safest && recommended && custom);
        const int outputs = app_->config().compile.outputCount;
        QTest::mouseClick(recommended, Qt::LeftButton);
        const auto& c = app_->config().compile;
        QVERIFY(c.mergeCompatible && c.applyPatchPlan && c.mergeCellConfig && c.translatePrevis && c.buildPatch);
        QVERIFY(!c.lightOutputsWhenPossible && !c.brokenReferencesAsNull && !c.patchOnly);
        QVERIFY(recommended->isChecked());
        QTest::mouseClick(safest, Qt::LeftButton);
        QVERIFY(c.mergeCompatible && !c.applyPatchPlan && !c.mergeCellConfig && !c.translatePrevis && !c.buildPatch);
        QVERIFY(safest->isChecked());
        QCOMPARE(app_->config().compile.outputCount, outputs); // never touched by a preset
        previsBox()->setChecked(true); // one switch away from Safest
        QVERIFY(custom->isChecked());
        app_->loadSettings(); // back to the saved state
    }

    // A first run (nothing saved) starts on the Safest preset.
    void first_run_starts_safest()
    {
        const QString db = QSettings().value("advanced/database").toString();
        QSettings().clear();
        QSettings().setValue("advanced/database", db);
        start();
        auto* safest = childWithText<QPushButton>(compilation(), "Safest");
        QVERIFY(safest);
        QVERIFY(safest->isChecked());
        const auto& c = app_->config().compile;
        QVERIFY(c.mergeCompatible);
        QVERIFY(!c.applyPatchPlan && !c.mergeCellConfig && !c.translatePrevis && !c.buildPatch &&
                !c.compileNewQuestsAndNpcs);
        QVERIFY(!c.lightOutputsWhenPossible && !c.unusedMasterPlaceholders && !c.redirectMasters);
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
