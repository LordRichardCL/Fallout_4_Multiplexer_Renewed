#pragma once

#include <QFrame>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace gui {

class AppController;
class SegmentedControl;

// Which plugins are compiled (Compilation page, after Scan and Analyse, before Compile): every eligible
// plugin with a switch to leave one out, or only the plugins switched on (opt-in, for compiling only
// some mods, such as weapons). Each change is saved at once and applied to the current analysis, so the
// list shows straight away what it means: a plugin left in the load order keeps its masters there too.
class PluginSelection : public QFrame {
    Q_OBJECT
public:
    explicit PluginSelection(AppController* app, QWidget* parent = nullptr);
    void refresh();

    static constexpr const char* kAll = "All eligible";
    static constexpr const char* kChosen = "Only the ones I choose";

private:
    struct Row {
        QString name, eligibility, reason, types;
        qint64 records = 0;
    };
    void reload();       // rows from the results database
    void fillTable();    // rows shown with the current filters
    bool switchedOn(const Row& r) const;
    QStringList shownNames() const;
    void setShown(bool on);

    AppController* app_;
    SegmentedControl* mode_;
    QLabel* summary_;
    QLineEdit* search_;
    QComboBox* show_;
    QComboBox* type_;
    QPushButton* on_;
    QPushButton* off_;
    QTableWidget* table_;
    QLabel* empty_;
    std::vector<Row> rows_;
    bool filling_ = false;
};

} // namespace gui
