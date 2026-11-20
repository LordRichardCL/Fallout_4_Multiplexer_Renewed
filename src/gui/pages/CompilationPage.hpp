#pragma once

#include "pages/Page.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableView;
class QTreeView;
class QModelIndex;
class QTabWidget;
class QTextBrowser;

namespace gui {

class SqlModel;
class GroupedSqlModel;
class StageIndicator;
class CollapsibleSection;
class SegmentedControl;

// Multiplexer Compilation System: options, pipeline, generated output.
class CompilationPage : public Page {
    Q_OBJECT
public:
    explicit CompilationPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    // The options shown come from the shared settings state (AppController::config()), and each change
    // is written back to it at once, so nothing is lost when another page is shown.
    void loadOptions();
    void updateStrategyNote();
    void updateInstallNote();
    void storeOptions();
    void updateButtons();
    void showTrace(const QModelIndex& index);
    // Presets set the switches of "When mods change the same thing" and "Patch" (never the output count,
    // folder, name or allocation); the selector shows Custom when the switches match neither.
    void applyPreset(const QString& name);
    void updatePresetState();
    void updateSummaries();
    void fillResults();
    // After a compile stopped because plugins the last build compiled would return: ask which may.
    void reviewReturning();

    QSpinBox* outputs_;
    QCheckBox* autoOutputs_ = nullptr;
    QLineEdit* prefix_;
    QComboBox* strategy_;
    QLabel* strategyNote_ = nullptr;
    QLabel* outputsNote_ = nullptr;
    QLabel* installNote_ = nullptr;
    QPlainTextEdit* rules_;
    QCheckBox* merge_;
    QCheckBox* light_;
    QSpinBox* lightHeadroom_ = nullptr;
    QCheckBox* translatePrevis_;
    QCheckBox* mergeCellConfig_;
    QCheckBox* applyPlan_;
    QCheckBox* brokenAsNull_;
    QCheckBox* masterPlaceholders_;
    QCheckBox* newQuestsNpcs_;
    class PluginSelection* selection_ = nullptr;
    QCheckBox* patch_;
    QCheckBox* patchOnly_;
    QLineEdit* outputDir_;
    QPushButton* compile_;
    QPushButton* export_;
    QPushButton* apply_;
    QPushButton* openFolder_;
    StageIndicator* stages_;
    QLabel* summary_;
    QTabWidget* tabs_;
    SqlModel* outputsModel_;
    GroupedSqlModel* recordsModel_;
    GroupedSqlModel* mappingsModel_;
    QTableView* outputsView_;
    QTreeView* recordsView_;
    QTreeView* mappingsView_;
    QTreeView* assetsView_;
    GroupedSqlModel* assetsModel_;
    QTextBrowser* trace_;
    SegmentedControl* presets_ = nullptr;
    CollapsibleSection* patchSection_ = nullptr;
    CollapsibleSection* advancedSection_ = nullptr;
    QLabel* prefixDescription_ = nullptr;
    QLabel* planPending_ = nullptr;
    QWidget* stageCard_ = nullptr;
    QLabel* lastBuild_ = nullptr;
    QLabel* buildPill_ = nullptr;
    QLabel* compiledN_ = nullptr;
    QLabel* outputsN_ = nullptr;
    QLabel* recordsN_ = nullptr;
    QWidget* outputsList_ = nullptr;
    QLabel* errorsN_ = nullptr;
    QLabel* warningsN_ = nullptr;
    QLabel* keptN_ = nullptr;
    QLabel* pipe_ = nullptr;
    bool loading_ = false; // loadOptions() is setting the controls: no write-back
};

} // namespace gui
