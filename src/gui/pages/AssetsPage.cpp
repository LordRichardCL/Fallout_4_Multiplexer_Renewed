#include "pages/AssetsPage.hpp"

#include "Format.hpp"
#include "models/SqlModel.hpp"
#include "widgets/Widgets.hpp"

#include "assets/AssetPreview.hpp"
#include "ba2/Ba2Archive.hpp"
#include "util/Text.hpp"

#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableView>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <fstream>

namespace gui {

AssetsPage::AssetsPage(AppController* app, QWidget* parent) : Page(app, parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(28, 24, 28, 18);
    l->setSpacing(10);
    search_ = new QLineEdit;
    search_->setPlaceholderText("Search asset path, e.g. textures\\weapons\\10mm");
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(320);
    l->addWidget(makePageHeader("Asset Intelligence",
                                "Loose files and BA2 archives merged in engine load order. The last source wins.",
                                {search_}));
    chips_ = new FilterChips({"Conflicting", "All", "Texture", "Mesh", "Script", "Material", "Audio", "Animation",
                              "Interface", "FaceGen", "Precombined"});
    l->addWidget(chips_);

    auto* split = new QSplitter(Qt::Horizontal);
    table_ = new QTableView;
    styleTable(table_);
    model_ = new SqlModel(this);
    table_->setModel(model_);
    split->addWidget(table_);

    auto* right = new QSplitter(Qt::Vertical);
    sources_ = new QTableView;
    styleTable(sources_);
    sourcesModel_ = new SqlModel(this);
    sources_->setModel(sourcesModel_);
    right->addWidget(sources_);
    auto* previewBox = new QWidget;
    auto* pl = new QVBoxLayout(previewBox);
    pl->setContentsMargins(0, 6, 0, 0);
    image_ = new QLabel;
    image_->setAlignment(Qt::AlignCenter);
    image_->setMinimumHeight(220);
    image_->setStyleSheet("background:#111613;border:1px solid #26302b;border-radius:8px;");
    pl->addWidget(image_, 2);
    info_ = new QTextBrowser;
    pl->addWidget(info_, 1);
    right->addWidget(previewBox);
    right->setStretchFactor(0, 1);
    right->setStretchFactor(1, 3);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    l->addWidget(split, 1);

    connect(chips_, &FilterChips::changed, this, [this] { reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { reload(); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showAsset(i.row()); });
    connect(sources_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& i) { showSource(i.row()); });
}

AssetsPage::~AssetsPage() = default;

void AssetsPage::refresh()
{
    reload();
}

void AssetsPage::reload()
{
    QString chip = chips_->current();
    QString where = "1=1";
    QVariantList binds;
    if (chip == "Conflicting")
        where += " AND source_count > 1";
    else if (chip != "All") {
        where += " AND asset_type = ?";
        binds << chip;
    }
    QString s = search_->text().trimmed();
    if (!s.isEmpty()) {
        where += " AND path LIKE ?";
        binds << "%" + QString::fromStdString(fo4mx::normalizeAssetPath(s.toStdString())) + "%";
    }
    model_->setNumberColumns({2});
    model_->setBytesColumns({6});
    model_->setMonoColumns({0});
    model_->load(app_->readDb(),
                 "SELECT path AS 'Asset Path', asset_type AS Type, source_count AS Sources, winner_mod AS 'Winning Mod', "
                 "winner_kind AS Source, winner_archive AS Archive, winner_size AS Size, id, reason, winner_physical "
                 "FROM Assets WHERE " + where + " ORDER BY path LIMIT 200000",
                 binds);
    for (int c : {7, 8, 9})
        table_->hideColumn(c);
    fitColumns(table_, 460);
    sourcesModel_->clear();
    image_->clear();
    if (model_->rowCount() == 0)
        info_->setHtml(chip == "Conflicting"
                           ? "<p style='color:#8aa196'>No asset conflicts: every asset path is provided by exactly one "
                             "source. Choose <b>All</b> to browse every asset.</p>"
                           : "<p style='color:#8aa196'>No assets match this filter.</p>");
    else
        info_->setHtml("<p style='color:#8aa196'>Select an asset to see every version and a preview.</p>");
}

void AssetsPage::showAsset(int row)
{
    if (row < 0)
        return;
    qint64 id = model_->value(row, 7).toLongLong();
    currentPath_ = model_->value(row, 0).toString();
    int count = model_->value(row, 2).toInt();
    if (count > 1) {
        sourcesModel_->setBytesColumns({4});
        sourcesModel_->setStatusColumns({0});
        sourcesModel_->load(app_->readDb(),
                            "SELECT CASE is_winner WHEN 1 THEN 'Winning' ELSE 'Replaced' END AS Status, mod_name AS Mod, "
                            "kind AS Source, archive AS Archive, size AS Size, physical AS 'Physical Path' "
                            "FROM AssetSources WHERE asset_id = ? ORDER BY position DESC",
                            {id});
        fitColumns(sources_);
    } else {
        sourcesModel_->setBytesColumns({4});
        sourcesModel_->setStatusColumns({0});
        sourcesModel_->load(app_->readDb(),
                            "SELECT 'Winning' AS Status, winner_mod AS Mod, winner_kind AS Source, winner_archive AS "
                            "Archive, winner_size AS Size, winner_physical AS 'Physical Path' FROM Assets WHERE id = ?",
                            {id});
    }
    QString reason = model_->value(row, 8).toString();
    preview(currentPath_, model_->value(row, 4).toString(), model_->value(row, 5).toString(), model_->value(row, 9).toString(),
            model_->value(row, 3).toString() + " — " + reason);
}

void AssetsPage::showSource(int row)
{
    if (row < 0 || currentPath_.isEmpty())
        return;
    preview(currentPath_, sourcesModel_->value(row, 2).toString(), sourcesModel_->value(row, 3).toString(),
            sourcesModel_->value(row, 5).toString(), sourcesModel_->value(row, 1).toString());
}

std::vector<uint8_t> AssetsPage::readBytes(const QString& path, const QString& kind, const QString& physical, size_t maxBytes)
{
    auto p = toPath(physical);
    if (kind == "Archive") {
        if (!archive_ || archive_->path() != p)
            archive_ = fo4mx::Ba2Archive::open(p);
        std::string key = path.toStdString();
        const auto& entries = archive_->entries();
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].path == key)
                return maxBytes ? archive_->extractHead(i, maxBytes) : archive_->extract(i);
        throw std::runtime_error("asset not found in archive");
    }
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + physical.toStdString());
    std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (maxBytes && out.size() > maxBytes)
        out.resize(maxBytes);
    return out;
}

void AssetsPage::preview(const QString& path, const QString& kind, const QString& archive, const QString& physical,
                         const QString& mod)
{
    image_->clear();
    QString html = QString("<h3 style='color:#3ddc84;margin-bottom:2px'>%1</h3>").arg(path.toHtmlEscaped());
    html += QString("<p><b>Winning source:</b> %1<br><b>Location:</b> %2%3<br><b>Physical path:</b> "
                    "<span style='font-family:monospace'>%4</span></p>")
                .arg(mod.toHtmlEscaped(), kind, archive.isEmpty() ? "" : " in " + archive.toHtmlEscaped(),
                     physical.toHtmlEscaped());
    std::string ext = fo4mx::toLower(path.section('.', -1).toStdString());
    try {
        if (ext == "dds") {
            auto bytes = readBytes(path, kind, physical, 0);
            auto info = fo4mx::preview::readDdsInfo(bytes);
            if (info) {
                html += QString("<p><b>Resolution:</b> %1 × %2<br><b>Format:</b> %3<br><b>Mip levels:</b> %4<br>"
                                "<b>Size:</b> %5%6</p>")
                            .arg(info->width)
                            .arg(info->height)
                            .arg(qs(info->format))
                            .arg(info->mipCount)
                            .arg(formatBytes(qint64(bytes.size())), info->cubemap ? "<br><b>Cubemap</b>" : "");
                if (auto img = fo4mx::preview::decodeDds(bytes, 1024)) {
                    QImage qi(img->rgba.data(), int(img->width), int(img->height), int(img->width * 4),
                              QImage::Format_RGBA8888);
                    QPixmap pm = QPixmap::fromImage(qi.copy());
                    image_->setPixmap(pm.scaled(image_->size().boundedTo(QSize(512, 512)), Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));
                } else {
                    image_->setText(QString("Preview not available for %1").arg(qs(info->format)));
                }
            }
        } else if (ext == "nif") {
            auto bytes = readBytes(path, kind, physical, 2 * 1024 * 1024);
            if (auto m = fo4mx::preview::readNifInfo(bytes)) {
                html += QString("<p><b>Format:</b> %1<br><b>Version:</b> 0x%2 · user %3 · BS %4<br><b>Blocks:</b> %5</p>")
                            .arg(qs(m->headerLine).toHtmlEscaped())
                            .arg(m->version, 8, 16, QChar('0'))
                            .arg(m->userVersion)
                            .arg(m->bsVersion)
                            .arg(m->blockCount);
                QStringList types;
                for (const auto& [t, n] : m->blockTypes)
                    types << QString("%1 × %2").arg(qs(t)).arg(n);
                html += "<p><b>Block types:</b><br>" + types.join("<br>") + "</p>";
                QStringList mats;
                for (const auto& s : m->strings)
                    if (fo4mx::iendsWith(s, ".bgsm") || fo4mx::iendsWith(s, ".bgem") || fo4mx::iendsWith(s, ".dds"))
                        mats << qs(s).toHtmlEscaped();
                if (!mats.isEmpty())
                    html += "<p><b>Materials / textures:</b><br>" + mats.join("<br>") + "</p>";
                image_->setText(QString("Mesh · %1 blocks").arg(m->blockCount));
            }
        } else if (ext == "bgsm" || ext == "bgem") {
            auto bytes = readBytes(path, kind, physical, 0);
            if (auto m = fo4mx::preview::readMaterialInfo(bytes)) {
                QStringList tex;
                for (const auto& t : m->textures)
                    tex << qs(t).toHtmlEscaped();
                html += QString("<p><b>Material:</b> %1 v%2<br><b>Textures:</b><br>%3</p>")
                            .arg(qs(m->kind))
                            .arg(m->version)
                            .arg(tex.join("<br>"));
                image_->setText(QString("%1 material").arg(qs(m->kind)));
            }
        } else {
            image_->setText("No preview for this file type");
        }
    } catch (const std::exception& e) {
        html += QString("<p style='color:#ff5c5c'>Cannot read asset: %1</p>").arg(QString::fromUtf8(e.what()).toHtmlEscaped());
    }
    info_->setHtml(html);
}

} // namespace gui
