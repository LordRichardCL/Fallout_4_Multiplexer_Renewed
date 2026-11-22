#pragma once

#include <QDialog>

#include <functional>

class QCheckBox;
class QLabel;
class QRadioButton;

namespace gui {

class AppController;

// Asked before a scan: what will be read (the plugins the game loads, with real counts) and whether to also
// preview the inactive ones. Neither choice changes what the build reads.
class ScanDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScanDialog(AppController* app, QWidget* parent = nullptr);
    bool previewInactive() const;
    bool askAgain() const;

signals:
    // "Choose plugins to compile…": the caller opens that choice (the dialog closes without scanning).
    void choosePlugins();

private:
    QRadioButton* game_;
    QRadioButton* preview_;
    QCheckBox* ask_;
};

// Asks (unless the user said not to), saves the choice and says whether to scan now. `choosePlugins` runs
// when the user asks to choose the plugins to compile instead.
bool confirmScan(AppController* app, QWidget* parent, const std::function<void()>& choosePlugins = {});

} // namespace gui
