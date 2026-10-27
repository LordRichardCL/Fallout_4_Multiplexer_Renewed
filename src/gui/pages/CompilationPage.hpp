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
class QTabWidget;
class QTextBrowser;

namespace gui {

class SqlModel;
class StageIndicator;

// Multiplexer Compilation System: options, pipeline, generated output.
class CompilationPage : public Page {
    Q_OBJECT
public:
    explicit CompilationPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void loadOptions();
    void storeOptions();
    void updateButtons();
    void showTrace(int row);

    QSpinBox* outputs_;
    QLineEdit* prefix_;
    QComboBox* strategy_;
    QPlainTextEdit* rules_;
    QCheckBox* merge_;
    QCheckBox* light_;
    QCheckBox* patch_;
    QCheckBox* patchOnly_;
    QCheckBox* heuristic_;
    QLineEdit* outputDir_;
    QPushButton* compile_;
    QPushButton* export_;
    QPushButton* apply_;
    QPushButton* openFolder_;
    StageIndicator* stages_;
    QLabel* summary_;
    QTabWidget* tabs_;
    SqlModel* outputsModel_;
    SqlModel* recordsModel_;
    SqlModel* mappingsModel_;
    QTableView* outputsView_;
    QTableView* recordsView_;
    QTableView* mappingsView_;
    QTableView* assetsView_;
    SqlModel* assetsModel_;
    QTextBrowser* trace_;
};

} // namespace gui
