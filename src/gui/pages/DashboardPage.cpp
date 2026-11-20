#include "pages/DashboardPage.hpp"

#include "Format.hpp"
#include "Theme.hpp"
#include "widgets/Widgets.hpp"

#include <QDateTime>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSqlQuery>
#include <QStyle>
#include <QVBoxLayout>

namespace gui {

namespace {

QString when(qint64 secs)
{
    if (secs <= 0)
        return "never";
    const auto t = QDateTime::fromSecsSinceEpoch(secs);
    const auto now = QDateTime::currentDateTime();
    if (t.date() == now.date())
        return "today at " + t.toString("HH:mm");
    if (t.date() == now.date().addDays(-1))
        return "yesterday at " + t.toString("HH:mm");
    return t.toString("d MMM yyyy HH:mm");
}

QFrame* card()
{
    auto* f = new QFrame;
    f->setObjectName("Card");
    return f;
}

} // namespace

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
    l->setContentsMargins(28, 22, 28, 22);
    l->setSpacing(14);

    // Header: the page's quick actions stay available, small, on the right.
    auto* scan = makeButton("Scan");
    auto* analyse = makeButton("Analyse");
    auto* compile = makeButton("Compile");
    auto* validate = makeButton("Validate");
    auto* reports = makeButton("Reports");
    scan->setIcon(lineIcon("scan", theme::kText, 15));
    analyse->setIcon(lineIcon("analyse", theme::kText, 15));
    compile->setIcon(lineIcon("compile", theme::kText, 15));
    validate->setIcon(lineIcon("validation", theme::kText, 15));
    reports->setIcon(lineIcon("reports", theme::kText, 15));
    scan->setToolTip("Read the load order, plugins, archives and loose files");
    analyse->setToolTip("Find conflicts, dependencies and what can be compiled");
    compile->setToolTip("Open the Compilation page");
    validate->setToolTip("Compile and show the validation results");
    reports->setToolTip("Open the reports");
    connect(scan, &QPushButton::clicked, app_, &AppController::runScan);
    connect(analyse, &QPushButton::clicked, app_, &AppController::runAnalyse);
    connect(compile, &QPushButton::clicked, this, [this] { emit navigate("Compilation"); });
    connect(validate, &QPushButton::clicked, this, [this] {
        app_->runCompile();
        emit navigate("Validation");
    });
    connect(reports, &QPushButton::clicked, this, [this] { emit navigate("Reports"); });
    for (auto* b : {scan, analyse, compile, validate})
        connect(app_, &AppController::busyChanged, b, [b](bool busy) { b->setEnabled(!busy); });
    auto* header = makePageHeader("Dashboard", "", {scan, analyse, compile, validate, reports});
    subtitle_ = header->findChild<QLabel*>("PageSubtitle");
    l->addWidget(header);

    // Status card: where the build stands, and the one thing to do next.
    auto* hero = new QFrame;
    hero->setObjectName("Hero");
    auto* hh = new QHBoxLayout(hero);
    hh->setContentsMargins(22, 18, 22, 18);
    hh->setSpacing(18);
    heroIcon_ = new QLabel;
    heroIcon_->setFixedSize(54, 54);
    heroIcon_->setAlignment(Qt::AlignCenter);
    hh->addWidget(heroIcon_);
    auto* ht = new QVBoxLayout;
    ht->setSpacing(3);
    heroTitle_ = new QLabel;
    heroTitle_->setObjectName("HeroTitle");
    heroDetail_ = new QLabel;
    heroDetail_->setObjectName("PageSubtitle");
    heroDetail_->setWordWrap(true);
    ht->addWidget(heroTitle_);
    ht->addWidget(heroDetail_);
    hh->addLayout(ht, 1);
    review_ = makeButton("Review Changes");
    checkNow_ = makeButton("Check again");
    checkNow_->setIcon(lineIcon("refresh", theme::kText, 15));
    rebuild_ = makeButton("Rebuild", "Big");
    rebuild_->setIcon(lineIcon("compile", QColor(0xea, 0xff, 0xf2), 17));
    scanBig_ = makeButton("Scan my mods", "Big");
    scanBig_->setIcon(lineIcon("scan", QColor(0xea, 0xff, 0xf2), 17));
    for (auto* b : {review_, checkNow_, rebuild_, scanBig_})
        hh->addWidget(b, 0, Qt::AlignVCenter);
    review_->setToolTip("See what changed in your mods since the last build");
    checkNow_->setToolTip("Check whether the installed mods still match the last build");
    rebuild_->setToolTip("Scan, analyse, compile and export again");
    connect(review_, &QPushButton::clicked, this, [this] { emit navigate("Changes"); });
    connect(checkNow_, &QPushButton::clicked, app_, &AppController::runCheck);
    connect(rebuild_, &QPushButton::clicked, app_, &AppController::runRebuild);
    connect(scanBig_, &QPushButton::clicked, app_, &AppController::runScan);
    for (auto* b : {checkNow_, rebuild_, scanBig_})
        connect(app_, &AppController::busyChanged, b, [b](bool busy) { b->setEnabled(!busy); });
    l->addWidget(hero);

    // The steps from scanning to playing.
    auto* jc = card();
    auto* jl = new QVBoxLayout(jc);
    jl->setContentsMargins(16, 16, 16, 10);
    journey_ = new JourneyView;
    jl->addWidget(journey_);
    l->addWidget(jc);

    // Four headline numbers.
    auto* tiles = new QHBoxLayout;
    tiles->setSpacing(14);
    const struct {
        const char* key;
        const char* icon;
        QColor accent;
    } tileDefs[] = {{"plugins", "plugins", theme::kGreen},
                    {"outputs", "compile", theme::kBlue},
                    {"conflicts", "conflicts", theme::kYellow},
                    {"validation", "validation", theme::kGreen}};
    for (const auto& t : tileDefs) {
        auto* tile = new IconTile(t.icon, t.accent);
        tiles_[t.key] = tile;
        tiles->addWidget(tile, 1);
    }
    l->addLayout(tiles);

    // What needs the user, and the load order at a glance.
    auto* lower = new QHBoxLayout;
    lower->setSpacing(14);
    auto* attentionCard = card();
    auto* al = new QVBoxLayout(attentionCard);
    al->setContentsMargins(16, 14, 16, 12);
    al->setSpacing(4);
    auto* ah = new QHBoxLayout;
    auto* at = new QLabel("Needs your attention");
    at->setObjectName("SectionCardTitle");
    attentionCount_ = new QLabel;
    attentionCount_->setObjectName("Pill");
    attentionCount_->setStyleSheet("background:#2a2614; color:#f5c542;");
    ah->addWidget(at);
    ah->addWidget(attentionCount_);
    ah->addStretch();
    al->addLayout(ah);
    attention_ = new QVBoxLayout;
    attention_->setSpacing(0);
    al->addLayout(attention_);
    al->addStretch();
    lower->addWidget(attentionCard, 3);

    auto* orderCard = card();
    auto* ol = new QVBoxLayout(orderCard);
    ol->setContentsMargins(16, 14, 16, 12);
    ol->setSpacing(4);
    auto* ot = new QLabel("Your load order");
    ot->setObjectName("SectionCardTitle");
    ol->addWidget(ot);
    auto* el = new QLabel("HOW PLUGINS ARE HANDLED");
    el->setObjectName("CardLabel");
    ol->addWidget(el);
    eligibilityBar_ = new StackedBar;
    ol->addWidget(eligibilityBar_);
    auto* cl = new QLabel("CONFLICTS");
    cl->setObjectName("CardLabel");
    ol->addWidget(cl);
    conflictBar_ = new StackedBar;
    ol->addWidget(conflictBar_);
    ol->addStretch();
    totals_ = new QLabel;
    totals_->setObjectName("OptionDescription");
    totals_->setWordWrap(true);
    ol->addWidget(totals_);
    lower->addWidget(orderCard, 2);
    l->addLayout(lower);

    // Every number the earlier dashboard showed.
    allNumbers_ = new CollapsibleSection("records", "All numbers");
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
    allNumbers_->body()->addLayout(grid);
    l->addWidget(allNumbers_);
    l->addStretch();
}

void DashboardPage::fillHero(bool have)
{
    const QString status = meta("build_status");
    QString title, detail, icon = "check";
    QColor color = theme::kGreen;
    if (!have) {
        title = "Let's get started";
        detail = "Set up your installation in Settings, then scan your mods.";
        icon = "scan";
        color = theme::kMuted;
    } else if (status.isEmpty() || status == "Not Built") {
        title = "Ready to compile";
        detail = "Your mods are scanned. Compile and export to create the first build.";
        icon = "compile";
        color = theme::kBlue;
    } else if (status == "Up To Date") {
        title = "Your game is up to date";
        detail = QString("Last build %1 · checked %2 · nothing has changed since")
                     .arg(when(meta("build_reference").toLongLong()), when(meta("build_checked").toLongLong()));
    } else if (status == "Changes Detected") {
        title = "Some files changed";
        detail = QString("Assets changed since the build %1. The compiled records are still valid.")
                     .arg(when(meta("build_reference").toLongLong()));
        icon = "warning";
        color = theme::kYellow;
    } else {
        title = "Your mods changed: rebuild needed";
        const QString why = meta("changes_reasons").split('\n').mid(0, 2).join("; ");
        detail = QString("The build from %1 no longer matches your mods.%2")
                     .arg(when(meta("build_reference").toLongLong()), why.isEmpty() ? QString() : " " + why);
        icon = "warning";
        color = theme::kRed;
    }
    heroIcon_->setPixmap(linePixmap(icon, color, 26));
    heroIcon_->setStyleSheet(QString("border: 2px solid %1; border-radius: 27px; background: #102018;").arg(color.name()));
    heroTitle_->setText(title);
    heroTitle_->setStyleSheet(QString("color:%1;").arg(color.name()));
    heroDetail_->setText(detail);
    const bool built = have && !status.isEmpty() && status != "Not Built";
    scanBig_->setVisible(!have);
    rebuild_->setVisible(have);
    rebuild_->setText(built ? "Rebuild" : "Compile");
    if (!built && have) {
        disconnect(rebuild_, nullptr, nullptr, nullptr);
        connect(rebuild_, &QPushButton::clicked, this, [this] { emit navigate("Compilation"); });
    } else if (built) {
        disconnect(rebuild_, nullptr, nullptr, nullptr);
        connect(rebuild_, &QPushButton::clicked, app_, &AppController::runRebuild);
    }
    checkNow_->setVisible(built);
    review_->setVisible(built && status != "Up To Date");
    // The highlighted button is the one the situation calls for: Rebuild only when the mods changed (or
    // Compile before the first build); when the game is up to date, nothing needs doing.
    const bool urgent = !built || (status != "Up To Date" && status != "Changes Detected");
    rebuild_->setObjectName(urgent ? "Primary" : "");
    rebuild_->setText(!built ? "Compile" : urgent ? "Rebuild" : "Rebuild anyway");
    rebuild_->setIcon(lineIcon("compile", urgent ? QColor(0xea, 0xff, 0xf2) : theme::kText, urgent ? 17 : 15));
    rebuild_->style()->unpolish(rebuild_);
    rebuild_->style()->polish(rebuild_);
}

void DashboardPage::fillAttention(bool have)
{
    while (auto* item = attention_->takeAt(0)) {
        if (auto* w = item->widget())
            w->deleteLater();
        delete item;
    }
    int count = 0;
    auto add = [&](const QString& icon, const QColor& c, const QString& title, const QString& text,
                   const QString& button, const QString& page) {
        auto* b = makeButton(button);
        connect(b, &QPushButton::clicked, this, [this, page] { emit navigate(page); });
        attention_->addWidget(makeAttentionRow(icon, c, title, text, b));
        ++count;
    };
    if (!have) {
        attention_->addWidget(makeAttentionRow("info", theme::kBlue, "Nothing scanned yet",
                                               "Choose your game folder and mod manager in Settings, then press Scan.",
                                               nullptr));
        attentionCount_->hide();
        return;
    }
    const QString status = meta("build_status");
    if (status == "Requires Rebuild" || status == "Changes Detected")
        add("changes", theme::kYellow, "Your mods changed since the last build",
            status == "Requires Rebuild" ? "Rebuild so the game gets the same data again." : "Only assets changed.",
            "Review", "Changes");
    const auto* plan = app_->pipeline() ? app_->pipeline()->patchPlan() : nullptr;
    if (plan && plan->pending)
        add("conflicts", theme::kYellow, QString("%1 patch choice(s) are waiting").arg(formatCount(qint64(plan->pending))),
            "Two mods changed the same thing differently. Pick which to keep.", "Review", "Conflicts");
    const int errors = meta("validation_pre-export_errors").toInt();
    if (errors > 0)
        add("block", theme::kRed, QString("%1 validation error(s)").arg(errors), "Export waits until they are fixed.",
            "Open", "Validation");
    const int keptOut = scalar("SELECT COUNT(*) FROM AutoMergePlugins WHERE status LIKE 'Kept out%'").toInt();
    if (keptOut > 0)
        add("block", theme::kRed, QString("%1 plugin(s) kept out of compiling").arg(formatCount(qint64(keptOut))),
            "Another file needs their name. They stay in your load order.", "See why", "Advanced Users Auto-Merger");
    const int missing = scalar("SELECT COUNT(*) FROM Plugins WHERE active = 1 AND status = 'Missing Masters'").toInt();
    if (missing > 0)
        add("warning", theme::kRed, QString("%1 plugin(s) are missing masters").arg(missing),
            "The game cannot load them as they are.", "Open", "Plugins");
    const int largeRefs = scalar("SELECT COUNT(*) FROM ValidationResults WHERE category = 'Large References' AND "
                                 "severity = 'Warning'")
                              .toInt();
    if (largeRefs > 0)
        add("info", theme::kBlue, QString("%1 worldspace(s) left as they are").arg(largeRefs),
            "Large references could not be kept exactly. Check distant objects in game.", "Open", "Validation");
    if (!count)
        attention_->addWidget(makeAttentionRow("check", theme::kGreen, "Nothing needs you right now",
                                               "Everything the last run found is handled.", nullptr));
    // After playing: the game's own logs, read by Diagnostics.
    auto* d = makeButton("Diagnostics");
    connect(d, &QPushButton::clicked, this, [this] { emit navigate("Diagnostics"); });
    attention_->addWidget(makeAttentionRow("play", theme::kMuted, "After you play",
                                           "Read the Diagnostic Recorder's log to see how the session went.", d));
    attentionCount_->setText(QString::number(count));
    attentionCount_->setVisible(count > 0);
}

void DashboardPage::refresh()
{
    QSqlDatabase db = app_->readDb();
    const bool have = db.isOpen() && scalar("SELECT COUNT(*) FROM Plugins").toInt() > 0;

    const QString kind = meta("install_kind");
    const QString game = meta("game_path");
    subtitle_->setText(have ? QString("%1 · Fallout 4 %2 · %3").arg(kind, meta("game_version"), meta("runtime_profile"))
                            : QString("Load order compiler for Fallout 4 1.10.163"));
    subtitle_->setToolTip(game);

    qint64 esm = 0, esp = 0, esl = 0, total = 0;
    QMap<QString, qint64> eligibility;
    if (have) {
        QSqlQuery q(db);
        q.exec("SELECT type, COUNT(*) FROM Plugins WHERE active = 1 GROUP BY type");
        while (q.next()) {
            const QString t = q.value(0).toString();
            const qint64 n = q.value(1).toLongLong();
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
    fillHero(have);

    // Steps.
    const QString conflicts = meta("conflicts_total");
    const QString compiled = meta("compiled_plugins");
    const QString outputs = meta("output_plugins");
    const QString status = meta("build_status");
    const auto* plan = app_->pipeline() ? app_->pipeline()->patchPlan() : nullptr;
    using S = JourneyView::State;
    std::vector<JourneyView::Step> steps;
    steps.push_back({"scan", "Scan", have ? QString("%1 plugins found").arg(formatCount(total)) : "find your mods",
                     have ? S::Done : S::Attention});
    steps.push_back({"analyse", "Analyse",
                     conflicts.isEmpty() ? "find conflicts" : QString("%1 conflicts sorted").arg(formatCount(conflicts.toLongLong())),
                     conflicts.isEmpty() ? (have ? S::Attention : S::Pending) : S::Done});
    if (plan && plan->pending)
        steps.push_back({"review", "Review", QString("%1 choices waiting").arg(formatCount(qint64(plan->pending))), S::Attention});
    else
        steps.push_back({"review", "Review", conflicts.isEmpty() ? "your choices" : "nothing waiting",
                         conflicts.isEmpty() ? S::Pending : S::Done});
    const bool rebuild = status == "Requires Rebuild";
    steps.push_back({"compile", "Compile",
                     rebuild             ? "rebuild needed"
                     : compiled.isEmpty() ? "make the outputs"
                                          : QString("%1 output plugins").arg(outputs),
                     rebuild ? S::Attention : compiled.isEmpty() ? (conflicts.isEmpty() ? S::Pending : S::Attention) : S::Done});
    const bool exported = !meta("export_written").isEmpty() || (!status.isEmpty() && status != "Not Built");
    steps.push_back({"install", "Install", exported ? "exported" : "export and apply", exported ? S::Done : S::Pending});
    steps.push_back({"play", "Play & check", "read the game's logs", S::Pending});
    journey_->setSteps(std::move(steps));

    // Headline numbers.
    auto n = [](const QString& v) { return v.isEmpty() ? QString("—") : formatCount(v.toLongLong()); };
    tiles_["plugins"]->setValue(have ? formatCount(total) : "—",
                                have ? (compiled.isEmpty() ? QString("plugins") : QString("plugins · %1 compiled").arg(n(compiled)))
                                     : "plugins");
    const qint64 bytes = scalar("SELECT SUM(size_bytes) FROM CompilationGroups").toLongLong();
    tiles_["outputs"]->setValue(n(outputs), outputs.isEmpty() ? QString("output plugins · after Compile")
                                                              : QString("output plugins · %1").arg(formatBytes(bytes)));
    tiles_["conflicts"]->setValue(n(conflicts),
                                  conflicts.isEmpty() ? QString("conflicts · after Analyse")
                                  : plan && plan->pending
                                      ? QString("conflicts · %1 need you").arg(formatCount(qint64(plan->pending)))
                                      : QString("conflicts · %1 hard").arg(n(meta("conflicts_type3"))));
    const QString errors = meta("validation_pre-export_errors");
    tiles_["validation"]->setValue(errors.isEmpty() ? QString("—") : QString("%1 errors").arg(n(errors)),
                                   errors.isEmpty() ? QString("validation · after Compile")
                                                    : QString("validation · %1 warnings").arg(n(meta("validation_pre-export_warnings"))));

    fillAttention(have);

    // Load order at a glance.
    eligibilityBar_->setSegments({{"Compiled", double(eligibility.value("Eligible")), theme::kGreen},
                                  {"Stay", double(eligibility.value("Retained")), theme::kBlue},
                                  {"Can't compile", double(eligibility.value("Ineligible")), theme::kYellow},
                                  {"Error", double(eligibility.value("Error")), theme::kRed}});
    conflictBar_->setSegments({{"Simple", meta("conflicts_type1").toDouble(), theme::kBlue},
                               {"Merged", meta("conflicts_type2").toDouble(), theme::kGreen},
                               {"Not merged", meta("conflicts_type4").toDouble(), theme::kYellow},
                               {"Hard", meta("conflicts_type3").toDouble(), theme::kRed}});
    totals_->setText(have ? QString("ESM %1 · ESP %2 · ESL %3 · %4 archives · %5 loose files")
                                .arg(formatCount(esm), formatCount(esp), formatCount(esl),
                                     n(meta("archive_count")), n(meta("loose_file_count")))
                          : QString());

    // All numbers.
    auto setCard = [&](const QString& name, const QVariant& v, const QString& hint = {}) {
        cards_[name]->setValue(v.isValid() && !v.toString().isEmpty() ? formatCount(v.toLongLong()) : QString("—"), hint);
    };
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
    setCard("Conflicts", conflicts.isEmpty() ? QVariant() : QVariant(conflicts),
            conflicts.isEmpty() ? "run Analyse" : QString("%1 hard").arg(formatCount(meta("conflicts_type3").toLongLong())));
    setCard("Asset Conflicts", have ? meta("asset_conflicts") : QVariant());
    setCard("Eligible Plugins", have ? QVariant(eligibility.value("Eligible")) : QVariant(),
            have ? QString("%1 records").arg(formatCount(meta("eligible_records").toLongLong())) : "");
    const QString reuse = meta("merges_reused").isEmpty()
                              ? QString()
                              : QString(" · %1 chains reused, %2 rebuilt").arg(meta("merges_reused"), meta("merges_computed"));
    setCard("Output Plugins", outputs.isEmpty() ? QVariant() : QVariant(outputs),
            outputs.isEmpty() ? QString("estimated after Compile (max %1)").arg(app_->config().compile.outputCount)
                              : QString("from %1 compiled plugins%2").arg(compiled, reuse));
    allNumbers_->setSummary(have ? QString("%1 plugins · %2 records · %3 archives")
                                       .arg(formatCount(total), n(meta("total_records")), n(meta("archive_count")))
                                 : QString());
}

} // namespace gui
