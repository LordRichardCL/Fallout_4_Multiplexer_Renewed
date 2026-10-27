#pragma once

#include "pages/Page.hpp"

#include <QMap>

class QLabel;

namespace gui {

class BuildStatusBanner;
class StatCard;
class StageIndicator;
class StackedBar;

class DashboardPage : public Page {
    Q_OBJECT
public:
    explicit DashboardPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;
    StageIndicator* stages() const { return stages_; }

private:
    QMap<QString, StatCard*> cards_;
    StageIndicator* stages_;
    StackedBar* conflictBar_;
    StackedBar* eligibilityBar_;
    QLabel* subtitle_;
    BuildStatusBanner* banner_;
    QLabel* status_;
    QLabel* empty_;
};

} // namespace gui
