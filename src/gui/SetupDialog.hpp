#pragma once

#include <QDialog>

namespace fo4mx {
struct InstallConfig;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QTimer;

namespace gui {

class AppController;

// Start-up screen: asks for the game and mod manager folders and the load order file,
// checks them and shows the load order that will be used before the main window opens.
class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(AppController* app, QWidget* parent = nullptr);

    // True when the screen should open at start-up: the user asked for it,
    // or the saved installation cannot be read.
    static bool shouldShowAtStartup(AppController* app);

    // Re-checks the entered folders immediately (normally debounced).
    void validateNow();

private:
    fo4mx::InstallConfig current() const;
    void load();
    void save();
    void updateKind();
    void reloadProfiles();
    void detectGame();
    void detectMo2();
    void scheduleValidation();
    void setCheck(QLabel* label, bool ok, const QString& text);
    QWidget* pathRow(QLineEdit*& edit, bool directory, const QString& placeholder, const QString& filter = {});

    AppController* app_;
    QRadioButton* mo2_;
    QRadioButton* vortex_;
    QRadioButton* standalone_;
    QWidget* mo2Rows_;
    QLineEdit* mo2Path_;
    QComboBox* profile_;
    QLineEdit* gamePath_;
    QLineEdit* pluginsFile_;
    QLineEdit* outputDir_;
    QLabel* pluginsDefault_;
    QLabel* checkGame_;
    QLabel* checkManager_;
    QLabel* checkPlugins_;
    QLabel* summary_;
    QListWidget* loadOrder_;
    QCheckBox* showAtStartup_;
    QPushButton* continue_;
    QTimer* debounce_;
    bool valid_ = false;
};

} // namespace gui
