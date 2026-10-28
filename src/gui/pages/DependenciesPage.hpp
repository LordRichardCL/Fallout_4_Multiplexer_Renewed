#pragma once

#include "pages/Page.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableView;
class QTextBrowser;
class QTabWidget;

namespace gui {

class GraphView;
class SqlModel;

// Dependency graph viewer: plugin masters and record references.
class DependenciesPage : public Page {
    Q_OBJECT
public:
    explicit DependenciesPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void buildPluginGraph();
    void buildRecordGraph(const QString& query);
    void showPluginDetails(const QString& name);
    void showRecordDetails(const QString& id);

    QTabWidget* tabs_;
    GraphView* pluginGraph_;
    QLineEdit* pluginSearch_;
    QCheckBox* neighbourhoodOnly_;
    GraphView* recordGraph_;
    QLineEdit* recordSearch_;
    QSpinBox* depth_;
    QLabel* recordInfo_;
    QTextBrowser* pluginDetails_;
    QTextBrowser* recordDetails_;
    QTextBrowser* issueDetails_;
    QTableView* issues_;
    SqlModel* issuesModel_;
    QTableView* cleaning_;
    SqlModel* cleaningModel_;
    QComboBox* cleaningCategory_;
    QTextBrowser* cleaningDetails_;
    void loadCleaning();
    QString focusPlugin_;
};

} // namespace gui
