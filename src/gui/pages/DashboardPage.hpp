#pragma once

#include "pages/Page.hpp"

#include <QMap>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace gui {

class CollapsibleSection;
class IconTile;
class JourneyView;
class StatCard;
class StackedBar;

// The first page: where the build stands, the steps from scanning to playing, four headline numbers, what
// needs the user, and the load order at a glance. Every number the earlier dashboard showed is under
// "All numbers".
class DashboardPage : public Page {
    Q_OBJECT
public:
    explicit DashboardPage(AppController* app, QWidget* parent = nullptr);
    void refresh() override;

private:
    void fillHero(bool have);
    void fillAttention(bool have);

    QLabel* subtitle_;
    // Status card
    QLabel* heroIcon_;
    QLabel* heroTitle_;
    QLabel* heroDetail_;
    QPushButton* review_;
    QPushButton* checkNow_;
    QPushButton* rebuild_;
    QPushButton* scanBig_;
    JourneyView* journey_;
    QMap<QString, IconTile*> tiles_;
    QVBoxLayout* attention_;
    QLabel* attentionCount_;
    StackedBar* eligibilityBar_;
    StackedBar* conflictBar_;
    QLabel* totals_;
    CollapsibleSection* allNumbers_;
    QMap<QString, StatCard*> cards_;
};

} // namespace gui
