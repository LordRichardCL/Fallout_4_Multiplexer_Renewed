#include "pages/AutoMergerPage.hpp"

#include "Format.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include "report/ReportGenerator.hpp"

#include <QCheckBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

AutoMergerPage::AutoMergerPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    analyse_ = makeButton("Analyse");
    html_ = makeButton("Export HTML");
    json_ = makeButton("Export JSON");
    txt_ = makeButton("Export TXT");
    l->addWidget(makePageHeader("Advanced Users Auto-Merger",
                                "Compiles plugins that scripts and configs refer to by name, and relinks those files.",
                                {analyse_, html_, json_, txt_}));

    auto* intro = new QLabel(
        "<b>For advanced users.</b> Plugins whose file names appear in scripts (.pex) or config files (MCM, "
        "RobCo Patcher, ...) are normally kept out of compilation. With the Auto-Merger on, every such file is "
        "checked; a plugin whose files can all be relinked is compiled, and relinked copies of those files are "
        "written into the output folder, where they must override the originals. Scripts: Game.GetFormFromFile "
        "calls are pointed at the output plugin and the record's new FormID. Configs: \"Plugin.esp|FormID\" pairs "
        "are rewritten. Anything it cannot relink safely (F4SE DLLs, names or FormIDs computed at run time, "
        "Transfer Settlements blueprints, configs naming a plugin without a FormID) keeps its plugin out. "
        "Start a new game, and keep the output mod enabled above the mods it relinks.");
    intro->setWordWrap(true);
    intro->setStyleSheet("color:#8aa196");
    l->addWidget(intro);

    enabled_ = new QCheckBox("Enable the Advanced Users Auto-Merger (applies at the next Analyse and Compile)");
    enabled_->setChecked(app_->config().compile.autoMerge);
    l->addWidget(enabled_);
    summary_ = new QLabel;
    l->addWidget(summary_);
    chips_ = new FilterChips({"All", "Kept Out", "Relinkable", "Compiled and Relinked"});
    l->addWidget(chips_);

    auto* split = new QSplitter(Qt::Vertical);
    plugins_ = new QTableView;
    styleTable(plugins_);
    pluginsModel_ = new SqlModel(this);
    plugins_->setModel(pluginsModel_);
    files_ = new QTableView;
    styleTable(files_);
    filesModel_ = new SqlModel(this);
    filesModel_->setNumberColumns({4, 5, 6});
    files_->setModel(filesModel_);
    details_ = new QTextBrowser;
    details_->setMaximumHeight(140);
    split->addWidget(plugins_);
    split->addWidget(files_);
    split->addWidget(details_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    l->addWidget(split, 1);

    connect(enabled_, &QCheckBox::toggled, this, [this](bool on) {
        app_->config().compile.autoMerge = on;
        if (app_->pipeline())
            app_->pipeline()->config().compile.autoMerge = on;
        app_->saveSettings();
        summary_->setText(on ? "Enabled. Run Analyse to see which plugins can be relinked."
                             : "Off. Plugins named in scripts or configs stay in the load order.");
    });
    connect(analyse_, &QPushButton::clicked, app_, &AppController::runAnalyse);
    connect(chips_, &FilterChips::changed, this, [this] { refresh(); });
    connect(html_, &QPushButton::clicked, this, [this] { exportReport("html"); });
    connect(json_, &QPushButton::clicked, this, [this] { exportReport("json"); });
    connect(txt_, &QPushButton::clicked, this, [this] { exportReport("txt"); });
    connect(plugins_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& i) {
        if (i.row() < 0)
            return;
        QString plugin = pluginsModel_->value(i.row(), 0).toString();
        filesModel_->load(app_->readDb(),
                          "SELECT path AS File, mod AS Mod, kind AS Kind, plugins AS Plugins, calls AS GetFormFromFile, "
                          "pairs AS Pairs, mentions AS 'Other Mentions', CASE WHEN problem <> '' THEN problem ELSE result END "
                          "AS Result FROM AutoMergeFiles WHERE (', ' || plugins || ', ') LIKE ? ORDER BY path",
                          {"%, " + plugin + ", %"});
        fitColumns(files_, 520);
        details_->setHtml(QString("<p><b>%1</b> · %2</p><p>%3</p>")
                              .arg(plugin.toHtmlEscaped(), pluginsModel_->value(i.row(), 2).toString().toHtmlEscaped(),
                                   pluginsModel_->value(i.row(), 3).toString().toHtmlEscaped()));
    });
    connect(files_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& i) {
        if (i.row() < 0)
            return;
        auto v = [&](int c) { return filesModel_->value(i.row(), c).toString().toHtmlEscaped(); };
        details_->setHtml(QString("<p><b>%1</b> (%2, from %3)</p><p>Names: %4</p><p>%5</p>")
                              .arg(v(0), v(2), v(1), v(3), v(7)));
    });
}

void AutoMergerPage::refresh()
{
    enabled_->setChecked(app_->config().compile.autoMerge);
    const bool on = meta("auto_merge_enabled") == "1";
    QString chip = chips_->current();
    QString where = "1=1";
    if (chip == "Kept Out")
        where = "status = 'Kept out'";
    else if (chip == "Relinkable")
        where = "status = 'Relinkable'";
    else if (chip == "Compiled and Relinked")
        where = "status = 'Compiled and relinked'";
    pluginsModel_->load(app_->readDb(),
                        "SELECT plugin AS Plugin, files AS Files, status AS Status, reason AS Reason FROM AutoMergePlugins "
                        "WHERE " + where + " ORDER BY CASE status WHEN 'Kept out' THEN 1 ELSE 0 END, plugin");
    pluginsModel_->setNumberColumns({1});
    fitColumns(plugins_, 520);
    filesModel_->load(app_->readDb(),
                      "SELECT path AS File, mod AS Mod, kind AS Kind, plugins AS Plugins, calls AS GetFormFromFile, "
                      "pairs AS Pairs, mentions AS 'Other Mentions', CASE WHEN problem <> '' THEN problem ELSE result END "
                      "AS Result FROM AutoMergeFiles ORDER BY CASE WHEN problem <> '' THEN 1 ELSE 0 END, path");
    fitColumns(files_, 520);

    const qint64 named = scalar("SELECT COUNT(*) FROM AutoMergePlugins").toLongLong();
    const qint64 relinkable = scalar("SELECT COUNT(*) FROM AutoMergePlugins WHERE status <> 'Kept out'").toLongLong();
    const qint64 relinked = scalar("SELECT COUNT(*) FROM AutoMergeFiles WHERE result LIKE 'Relinked%'").toLongLong();
    if (!app_->config().compile.autoMerge)
        summary_->setText("Off. Plugins named in scripts or configs stay in the load order.");
    else if (!on)
        summary_->setText("Enabled. Run Analyse to see which plugins can be relinked.");
    else
        summary_->setText(QString("%1 plugins are named in scripts or configs; %2 can be relinked and compiled. "
                                  "%3 files were relinked by the last export.")
                              .arg(formatCount(named), formatCount(relinkable), formatCount(relinked)));
    const bool have = app_->pipeline() && app_->pipeline()->autoMerge();
    for (auto* b : {html_, json_, txt_})
        b->setEnabled(have);
}

void AutoMergerPage::exportReport(const QString& format)
{
    auto* p = app_->pipeline();
    if (!p || !p->autoMerge())
        return;
    fo4mx::ReportContext ctx;
    ctx.session = &p->session();
    ctx.compiled = p->compiledOutput();
    ctx.autoMerge = p->autoMerge();
    QString path = QFileDialog::getSaveFileName(this, "Export Auto-Merger report", "AutoMergerReport." + format,
                                                format == "json" ? "JSON (*.json)"
                                                : format == "txt" ? "Text (*.txt)"
                                                                  : "HTML (*.html)");
    if (path.isEmpty())
        return;
    auto kind = fo4mx::ReportKind::AutoMerger;
    QString content = format == "json"  ? qs(fo4mx::ReportGenerator::json(kind, ctx))
                      : format == "txt" ? qs(fo4mx::ReportGenerator::text(kind, ctx))
                                        : qs(fo4mx::ReportGenerator::html(kind, ctx));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "Export", "Cannot write " + path);
        return;
    }
    f.write(content.toUtf8());
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

} // namespace gui
