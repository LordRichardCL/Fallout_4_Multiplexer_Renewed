#include "pages/ReportsPage.hpp"

#include "Format.hpp"
#include "widgets/Widgets.hpp"

#include "report/ReportGenerator.hpp"

#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMessageBox>
#include <QPageSize>
#include <QPdfWriter>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace gui {

using fo4mx::ReportKind;

namespace {

fo4mx::ReportContext contextFor(AppController* app)
{
    fo4mx::ReportContext ctx;
    if (auto* p = app->pipeline(); p && p->scanned()) {
        ctx.session = &p->session();
        ctx.conflicts = p->conflicts();
        ctx.dependencies = p->dependencies();
        ctx.compiled = p->compiledOutput();
        ctx.validation = p->reportValidation();
        ctx.autoMerge = p->autoMerge();
        ctx.comparison = p->comparison();
    }
    ctx.maxRows = 2000;
    return ctx;
}

} // namespace

ReportsPage::ReportsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    html_ = makeButton("HTML", nullptr, "export");
    json_ = makeButton("JSON", nullptr, "export");
    txt_ = makeButton("TXT", nullptr, "export");
    pdf_ = makeButton("PDF", nullptr, "export");
    for (auto [b, f] : {std::pair{html_, "HTML"}, {json_, "JSON"}, {txt_, "TXT"}, {pdf_, "PDF"}}) {
        b->setToolTip(QString("Export the report shown as %1").arg(f));
        b->setAccessibleName(QString("Export %1").arg(f));
    }
    all_ = makeButton("Export All…", "Primary", "export");
    all_->setToolTip("Write every report in every format to a folder");
    l->addWidget(makePageHeader("Reports", "Every report, ready to read or export.", {html_, json_, txt_, pdf_, all_},
                                "reports",
                                "Conflict, asset, dependency, compilation, validation and Auto-Merger reports. Pick one "
                                "on the left to preview it; the buttons export the report shown as HTML, JSON, TXT or "
                                "PDF. Reports need a scan and analysis from this session; the compilation and "
                                "validation reports also need a compile."));
    auto* split = new QSplitter(Qt::Horizontal);
    list_ = new QListWidget;
    list_->setMaximumWidth(280);
    const std::pair<const char*, const char*> items[] = {
        {"Conflict Report", "Override chains, winners, hard conflicts"},
        {"Asset Report", "Asset conflicts, archive load order"},
        {"Dependency Report", "Masters, eligibility, missing requirements"},
        {"Compilation Report", "Output plugins and FormID mappings"},
        {"Validation Report", "Every check that failed or warned"},
        {"Auto-Merger Report", "Advanced: relinked scripts and configs"}};
    for (auto [t, d] : items) {
        auto* it = new QListWidgetItem(QString("%1\n%2").arg(t, d));
        it->setSizeHint(QSize(240, 54));
        list_->addItem(it);
    }
    split->addWidget(list_);
    preview_ = new QTextBrowser;
    split->addWidget(preview_);
    split->setStretchFactor(1, 1);
    l->addWidget(split, 1);

    connect(list_, &QListWidget::currentRowChanged, this, [this] { showReport(); });
    connect(html_, &QPushButton::clicked, this, [this] { exportReport("html"); });
    connect(json_, &QPushButton::clicked, this, [this] { exportReport("json"); });
    connect(txt_, &QPushButton::clicked, this, [this] { exportReport("txt"); });
    connect(pdf_, &QPushButton::clicked, this, [this] { exportReport("pdf"); });
    connect(all_, &QPushButton::clicked, this, [this] { exportAll(); });
}

void ReportsPage::refresh()
{
    if (list_->currentRow() < 0)
        list_->setCurrentRow(0);
    else
        showReport();
}

void ReportsPage::showReport()
{
    int row = list_->currentRow();
    auto ctx = contextFor(app_);
    currentHtml_.clear();
    if (row >= 0)
        currentHtml_ = qs(fo4mx::ReportGenerator::html(static_cast<ReportKind>(row), ctx));
    ctx.style = fo4mx::ReportStyle::Preview;
    QString preview = row >= 0 ? qs(fo4mx::ReportGenerator::html(static_cast<ReportKind>(row), ctx)) : QString();
    bool have = !currentHtml_.isEmpty();
    for (auto* b : {html_, json_, txt_, pdf_})
        b->setEnabled(have);
    all_->setEnabled(ctx.session != nullptr);
    if (!have) {
        preview_->setHtml("<p style='color:#8aa196'>This report needs results from the current session. Run Scan and "
                          "Analyse (and Compile for the compilation and validation reports).</p>");
        return;
    }
    preview_->setHtml(preview);
}

void ReportsPage::exportReport(const QString& format)
{
    int row = list_->currentRow();
    if (row < 0 || currentHtml_.isEmpty())
        return;
    auto kind = static_cast<ReportKind>(row);
    QString base = QString("%1Report.%2").arg(fo4mx::reportKindName(kind), format);
    QString path = QFileDialog::getSaveFileName(this, "Export report", base,
                                                format == "pdf"    ? "PDF (*.pdf)"
                                                : format == "json" ? "JSON (*.json)"
                                                : format == "txt"  ? "Text (*.txt)"
                                                                   : "HTML (*.html)");
    if (path.isEmpty())
        return;
    if (format == "pdf") {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(110);
        writer.setTitle(QString("%1 Report").arg(fo4mx::reportKindName(kind)));
        auto ctx = contextFor(app_);
        ctx.style = fo4mx::ReportStyle::Print;
        QTextDocument doc;
        doc.setHtml(qs(fo4mx::ReportGenerator::html(kind, ctx)));
        doc.print(&writer);
    } else {
        QString content = format == "json"  ? qs(fo4mx::ReportGenerator::json(kind, contextFor(app_)))
                          : format == "txt" ? qs(fo4mx::ReportGenerator::text(kind, contextFor(app_)))
                                            : currentHtml_;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, "Export", "Cannot write " + path);
            return;
        }
        f.write(content.toUtf8());
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void ReportsPage::exportAll()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Export all reports to");
    if (dir.isEmpty() || !app_->pipeline())
        return;
    auto files = app_->pipeline()->writeReports(toPath(dir));
    QMessageBox::information(this, "Reports", QString("Wrote %1 files to %2").arg(files.size()).arg(dir));
}

} // namespace gui
