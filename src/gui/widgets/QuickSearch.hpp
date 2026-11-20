#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <utility>
#include <vector>

class QWidget;

namespace gui {

class AppController;

// Ctrl+K: type part of a page, plugin or record name and jump to it. `open` is told what was picked:
// ("page", page name), ("plugin", plugin name), ("conflict", Editor ID) or ("record", Editor ID).
struct QuickSearchTarget {
    QString kind, value;
};
void showQuickSearch(QWidget* parent, AppController* app, const std::vector<std::pair<QString, QString>>& pages,
                     const std::function<void(const QuickSearchTarget&)>& open);

} // namespace gui
