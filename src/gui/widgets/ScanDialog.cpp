#include "widgets/ScanDialog.hpp"

#include "AppController.hpp"
#include "Format.hpp"
#include "widgets/Widgets.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace gui {

ScanDialog::ScanDialog(AppController* app, QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Scan your mods");
    setMinimumWidth(560);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(22, 20, 22, 18);
    l->setSpacing(10);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto sum = app->scanSummary();
    QApplication::restoreOverrideCursor();

    auto* title = new QLabel("What should the scan read?");
    title->setObjectName("SectionTitle");
    l->addWidget(title);
    auto* what = new QLabel;
    what->setWordWrap(true);
    what->setTextFormat(Qt::RichText);
    if (sum.ok)
        what->setText(QString("The build always reads what your game loads: <b>%1 active plugins</b> and their "
                              "archives (%2 archives found), from %3. Loose files count as the game sees them.<br>"
                              "<span style='color:#8a9a92'>%4 inactive plugins in Data are not read: they are listed "
                              "greyed out and never compiled.</span>")
                          .arg(formatCount(sum.activePlugins), formatCount(sum.archives), sum.manager.toHtmlEscaped(),
                               formatCount(sum.inactivePlugins)));
    else
        what->setText("The plugin list could not be read yet (" + sum.error.toHtmlEscaped() +
                      "). The scan reads the plugins your game loads and says what is wrong.");
    l->addWidget(what);

    game_ = new QRadioButton("What the game loads (recommended)");
    game_->setToolTip("Only the active plugins are read. This is all the build ever uses.");
    preview_ = new QRadioButton("Also preview inactive plugins");
    preview_->setToolTip("Read the inactive plugins too, apart from the load order: the Plugins page then says, for "
                         "each one, whether it would likely compile and which active mods it would conflict with if "
                         "you enabled it. The build is exactly the same either way.");
    auto* previewNote = new QLabel("Shown on the Plugins page (Show inactive plugins); never compiled, never part of "
                                   "the build. Slower with many inactive plugins.");
    previewNote->setObjectName("PageSubtitle");
    previewNote->setWordWrap(true);
    previewNote->setContentsMargins(26, 0, 0, 0);
    (app->previewInactive() ? preview_ : game_)->setChecked(true);
    l->addWidget(game_);
    l->addWidget(preview_);
    l->addWidget(previewNote);

    auto* keep = new QLabel("Want an active plugin left alone? It still has to be read (the game loads it), but you "
                            "can keep it out of the outputs.");
    keep->setObjectName("PageSubtitle");
    keep->setWordWrap(true);
    l->addWidget(keep);

    ask_ = new QCheckBox("Ask me before each scan");
    ask_->setChecked(true);
    ask_->setToolTip("Turn this off to scan straight away with the choice above. Settings → Scan changes it later.");
    l->addWidget(ask_);

    auto* buttons = new QHBoxLayout;
    auto* choose = makeButton("Choose Plugins to Compile…", nullptr, "plugins");
    auto* cancel = makeButton("Cancel");
    auto* scan = makeButton("Scan", "Primary", "scan");
    scan->setDefault(true);
    buttons->addWidget(choose);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(scan);
    l->addLayout(buttons);
    connect(choose, &QPushButton::clicked, this, [this] {
        emit choosePlugins();
        reject();
    });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(scan, &QPushButton::clicked, this, &QDialog::accept);
}

bool ScanDialog::previewInactive() const
{
    return preview_->isChecked();
}

bool ScanDialog::askAgain() const
{
    return ask_->isChecked();
}

bool confirmScan(AppController* app, QWidget* parent, const std::function<void()>& choosePlugins)
{
    if (!app->askBeforeScan())
        return true;
    ScanDialog d(app, parent);
    bool choose = false;
    QObject::connect(&d, &ScanDialog::choosePlugins, &d, [&] { choose = true; });
    const bool scan = d.exec() == QDialog::Accepted;
    if (choose && choosePlugins)
        choosePlugins();
    if (!scan)
        return false;
    app->setPreviewInactive(d.previewInactive());
    app->setAskBeforeScan(d.askAgain());
    return true;
}

} // namespace gui
