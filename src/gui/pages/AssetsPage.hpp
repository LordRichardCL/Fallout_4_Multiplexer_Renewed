#pragma once

#include "pages/Page.hpp"

#include <memory>

class QLabel;
class QLineEdit;
class QTableView;
class QTextBrowser;

namespace fo4mx {
class Ba2Archive;
}

namespace gui {

class FilterChips;
class SqlModel;

// Asset conflict viewer and preview (textures, meshes, materials).
class AssetsPage : public Page {
    Q_OBJECT
public:
    explicit AssetsPage(AppController* app, QWidget* parent = nullptr);
    ~AssetsPage() override;
    void refresh() override;

private:
    void reload();
    void showAsset(int row);
    void showSource(int row);
    void preview(const QString& path, const QString& kind, const QString& archive, const QString& physical,
                 const QString& mod);
    std::vector<uint8_t> readBytes(const QString& path, const QString& kind, const QString& physical, size_t maxBytes);

    FilterChips* chips_;
    QLineEdit* search_;
    QTableView* table_;
    SqlModel* model_;
    QTableView* sources_;
    SqlModel* sourcesModel_;
    QTextBrowser* info_;
    QLabel* image_;
    QString currentPath_;
    std::unique_ptr<fo4mx::Ba2Archive> archive_; // last opened archive (cache)
};

} // namespace gui
