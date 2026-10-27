#pragma once

#include "pages/Page.hpp"

class QLabel;
class QTableView;

namespace gui {

class BuildStatusBanner;
class FilterChips;
class SqlModel;

// Mod space changes since the last build, and whether a rebuild is needed.
class ChangesPage : public Page {
    Q_OBJECT
public:
    explicit ChangesPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void reload();

    BuildStatusBanner* banner_;
    FilterChips* chips_;
    QTableView* table_;
    SqlModel* model_;
    QLabel* types_;
};

// Shared by the dashboard: fills a banner from the Meta table.
void fillBuildBanner(Page* page, BuildStatusBanner* banner, const std::function<QString(const QString&)>& meta);

} // namespace gui
