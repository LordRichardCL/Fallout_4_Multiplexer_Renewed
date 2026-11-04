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

// Multiplexer Compilation System: options, pipeline, generated output.
class CompilationPage : public Page {
    Q_OBJECT
public:
    explicit CompilationPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void loadOptions();
    void updateStrategyNote();
    void updateInstallNote();
    void storeOptions();
    void updateButtons();
    void showTrace(const QModelIndex& index);

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
};

} // namespace gui
