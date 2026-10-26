#include "pages/DashboardPage.hpp"

#include "Format.hpp"
#include "Theme.hpp"
#include "widgets/Widgets.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSqlQuery>
#include <QVBoxLayout>

namespace gui {

DashboardPage::DashboardPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget;
    scroll->setWidget(body);
    auto* l = new QVBoxLayout(body);
    l->setContentsMargins(28, 24, 28, 24);
    l->setSpacing(14);

    auto* scan = makeButton("Scan", "Primary");
    auto* analyse = makeButton("Analyse");
    auto* compile = makeButton("Compile");
    auto* validate = makeButton("Validate");
    auto* reports = makeButton("Reports");
    connect(scan, &QPushButton::clicked, app_, &AppController::runScan);
    connect(analyse, &QPushButton::clicked, app_, &AppController::runAnalyse);
    connect(compile, &QPushButton::clicked, this, [this] { emit navigate("Compilation"); });
    connect(validate, &QPushButton::clicked, this, [this] {
        app_->runCompile();
        emit navigate("Validation");
    });
    connect(reports, &QPushButton::clicked, this, [this] { emit navigate("Reports"); });
    for (auto* b : {scan, analyse, compile, validate, reports})
        connect(app_, &AppController::busyChanged, b, [b](bool busy) { b->setEnabled(!busy); });

    auto* header = makePageHeader("Dashboard", "", {scan, analyse, compile, validate, reports});
    subtitle_ = header->findChild<QLabel*>("PageSubtitle");
    l->addWidget(header);

    empty_ = new QLabel("No scan results yet. Configure your installation in Settings, then press Scan.");
    empty_->setObjectName("PageSubtitle");
    l->addWidget(empty_);

    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    const QStringList names = {"Mods Installed", "Total Plugins", "ESM Count",       "ESP Count",
                               "ESL Count",      "Archive Count", "Loose File Count", "Records",
                               "Conflicts",      "Asset Conflicts", "Eligible Plugins", "Output Plugins"};
    for (int i = 0; i < names.size(); ++i) {
        auto* c = new StatCard(names[i]);
        cards_[names[i]] = c;
        grid->addWidget(c, i / 4, i % 4);
    }
    cards_["Conflicts"]->setAccent(theme::kYellow);
    cards_["Output Plugins"]->setAccent(theme::kBlue);
    l->addLayout(grid);

    l->addWidget(makeSectionTitle("Compilation Pipeline"));
    stages_ = new StageIndicator({"Scan", "Analyse", "Resolve", "Merge", "Rewrite", "Compile", "Validate", "Export"});
    auto* stageFrame = new QFrame;
    stageFrame->setObjectName("Card");
    auto* sl = new QVBoxLayout(stageFrame);
    sl->addWidget(stages_);
    l->addWidget(stageFrame);

    auto* bars = new QHBoxLayout;
    bars->setSpacing(12);
    auto makeBarCard = [&](const QString& title, StackedBar*& bar) {
        auto* f = new QFrame;
        f->setObjectName("Card");
        auto* fl = new QVBoxLayout(f);
        fl->setContentsMargins(16, 12, 16, 12);
        auto* t = new QLabel(title.toUpper());
        t->setObjectName("CardLabel");
        fl->addWidget(t);
        bar = new StackedBar;
        fl->addWidget(bar);
        bars->addWidget(f, 1);
    };
    makeBarCard("Conflict types", conflictBar_);
    makeBarCard("Compilation eligibility", eligibilityBar_);
    l->addLayout(bars);

    status_ = new QLabel;
    status_->setObjectName("PageSubtitle");
    status_->setWordWrap(true);
    l->addWidget(status_);
    l->addStretch();

    connect(app_, &AppController::taskStarted, this, [this](const QString&) { stages_->reset(); });
    connect(app_, &AppController::stageStarted, stages_, &StageIndicator::activate);
    connect(app_, &AppController::taskFinished, this, [this](const QString& task, bool ok, const QString& msg) {
        stages_->finish(ok);
        status_->setText(QString("%1 %2: %3").arg(task, ok ? "finished" : "failed", msg));
    });
}

void DashboardPage::refresh()
{
    QSqlDatabase db = app_->readDb();
    bool have = db.isOpen() && scalar("SELECT COUNT(*) FROM Plugins").toInt() > 0;
    empty_->setVisible(!have);

    QString kind = meta("install_kind");
    QString game = meta("game_path");
    QString version = meta("game_version");
    QString runtime = meta("runtime_profile");
    subtitle_->setText(have ? QString("%1 · %2 · Fallout 4 %3 · %4").arg(kind, game, version, runtime)
                            : QString("Load order intelligence for Fallout 4 1.10.163"));

    auto setCard = [&](const QString& name, const QVariant& v, const QString& hint = {}) {
        cards_[name]->setValue(v.isValid() && !v.toString().isEmpty() ? formatCount(v.toLongLong()) : QString("—"), hint);
    };

    qint64 esm = 0, esp = 0, esl = 0, total = 0;
    QMap<QString, qint64> eligibility;
    if (have) {
        QSqlQuery q(db);
        q.exec("SELECT type, COUNT(*) FROM Plugins WHERE active = 1 GROUP BY type");
        while (q.next()) {
            QString t = q.value(0).toString();
            qint64 n = q.value(1).toLongLong();
            total += n;
            if (t == "ESM")
                esm += n;
            else if (t == "ESP")
                esp += n;
            else
                esl += n;
        }
        q.exec("SELECT eligibility, COUNT(*) FROM Plugins WHERE active = 1 GROUP BY eligibility");
        while (q.next())
            eligibility[q.value(0).toString()] = q.value(1).toLongLong();
    }
    setCard("Mods Installed", have ? meta("mods_enabled") : QVariant());
    setCard("Total Plugins", have ? QVariant(total) : QVariant(),
            have ? QString("%1 installed").arg(formatCount(scalar("SELECT COUNT(*) FROM Plugins").toLongLong())) : "");
    setCard("ESM Count", have ? QVariant(esm) : QVariant());
    setCard("ESP Count", have ? QVariant(esp) : QVariant());
    setCard("ESL Count", have ? QVariant(esl) : QVariant(), have ? "including ESL-flagged plugins" : "");
    setCard("Archive Count", have ? meta("archive_count") : QVariant());
    setCard("Loose File Count", have ? meta("loose_file_count") : QVariant());
    setCard("Records", have ? meta("total_records") : QVariant(),
            have ? QString("%1 unique forms").arg(formatCount(meta("unique_forms").toLongLong())) : "");
    QString conflicts = meta("conflicts_total");
    setCard("Conflicts", conflicts.isEmpty() ? QVariant() : QVariant(conflicts),
            conflicts.isEmpty() ? "run Analyse" : QString("%1 hard").arg(formatCount(meta("conflicts_type3").toLongLong())));
    setCard("Asset Conflicts", have ? meta("asset_conflicts") : QVariant());
    setCard("Eligible Plugins", have ? QVariant(eligibility.value("Eligible")) : QVariant(),
            have ? QString("%1 records").arg(formatCount(meta("eligible_records").toLongLong())) : "");
    QString outputs = meta("output_plugins");
    setCard("Output Plugins", outputs.isEmpty() ? QVariant() : QVariant(outputs),
            outputs.isEmpty() ? QString("estimated after Compile (max %1)").arg(app_->config().compile.outputCount)
                              : QString("from %1 compiled plugins").arg(meta("compiled_plugins")));

    conflictBar_->setSegments({{"Simple override", meta("conflicts_type1").toDouble(), theme::kBlue},
                               {"Compatible merge", meta("conflicts_type2").toDouble(), theme::kYellow},
                               {"Hard conflict", meta("conflicts_type3").toDouble(), theme::kRed}});
    eligibilityBar_->setSegments({{"Eligible", double(eligibility.value("Eligible")), theme::kGreen},
                                  {"Retained", double(eligibility.value("Retained")), theme::kBlue},
                                  {"Ineligible", double(eligibility.value("Ineligible")), theme::kYellow},
                                  {"Error", double(eligibility.value("Error")), theme::kRed}});

    if (have)
        stages_->setCaption(QString("Input\n%1 plugins\n%2 records").arg(formatCount(total), formatCount(meta("total_records").toLongLong())),
                            outputs.isEmpty() ? QString() : QString("Output\n%1 compiled\nplugins").arg(outputs));
    else
        stages_->setCaption({}, {});
}

} // namespace gui
