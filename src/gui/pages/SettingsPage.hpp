#pragma once

#include "pages/Page.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QRadioButton;
class QSpinBox;

namespace gui {

class SettingsPage : public Page {
    Q_OBJECT
public:
    explicit SettingsPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    // load() shows the shared settings state; store() writes the controls back to it as they change
    // (Save Settings persists it).
    void load();
    void store();
    void updateVisibility();
    void reloadProfiles();
    QWidget* pathRow(QLineEdit*& edit, bool directory, const QString& placeholder);

    QRadioButton* mo2_;
    QRadioButton* vortex_;
    QRadioButton* standalone_;
    QLineEdit* mo2Path_;
    QComboBox* profile_;
    QLineEdit* gamePath_;
    QLineEdit* pluginsFile_;
    QLineEdit* iniDir_;
    QComboBox* language_;
    QComboBox* runtime_;
    QPlainTextEdit* excluded_;
    QCheckBox* creationClub_;
    QCheckBox* allTypes_;
    QCheckBox* recorder_;
    QCheckBox* archiveLoader_;
    QSpinBox* threads_;
    QLineEdit* database_;
    QLineEdit* reports_;
    QLabel* status_;
    QWidget* mo2Rows_;
    QWidget* vortexRows_;
    QLineEdit* vortexDownloads_;
    bool loading_ = false; // load() is setting the controls: no write-back
};

} // namespace gui
