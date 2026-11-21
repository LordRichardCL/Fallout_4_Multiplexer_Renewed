#pragma once

#include <QCheckBox>
#include <QColor>
#include <QIcon>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

#include <vector>

class QHBoxLayout;
class QButtonGroup;
class QVBoxLayout;

namespace gui {

// Large number + caption card used on the dashboard and summary rows.
class StatCard : public QFrame {
    Q_OBJECT
public:
    StatCard(const QString& label, QWidget* parent = nullptr);
    void setValue(const QString& value, const QString& hint = {});
    void setAccent(const QColor& c);

private:
    QLabel* value_;
    QLabel* label_;
    QLabel* hint_;
};

// Horizontal pipeline visualisation: Scan -> Analyse -> ... -> Export.
class StageIndicator : public QWidget {
    Q_OBJECT
public:
    enum class State { Pending, Running, Done, Failed };
    explicit StageIndicator(QStringList stages, QWidget* parent = nullptr);
    void reset();
    // Marks `stage` running and every earlier stage done. Unknown names are ignored.
    void activate(const QString& stage);
    void finish(bool ok);
    void setCaption(const QString& left, const QString& right);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QStringList stages_;
    std::vector<State> states_;
    QString left_, right_;
};

// Single-choice row of rounded "chip" buttons.
class FilterChips : public QWidget {
    Q_OBJECT
public:
    explicit FilterChips(const QStringList& labels, QWidget* parent = nullptr);
    QString current() const;
    void setCurrent(const QString& label);

signals:
    void changed(const QString& label);

private:
    QButtonGroup* group_;
    QStringList labels_;
};

// Proportional stacked bar (conflict type mix, validation results).
class StackedBar : public QWidget {
    Q_OBJECT
public:
    struct Segment {
        QString label;
        double value;
        QColor color;
    };
    explicit StackedBar(QWidget* parent = nullptr);
    void setSegments(std::vector<Segment> segments);
    QSize sizeHint() const override { return {400, 46}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::vector<Segment> segments_;
};

// "✓ Up To Date / ⚠ Changes Detected / ✖ Requires Rebuild" banner.
class BuildStatusBanner : public QFrame {
    Q_OBJECT
public:
    explicit BuildStatusBanner(QWidget* parent = nullptr);
    // status: Build status name as stored in Meta (empty = unknown)
    void setStatus(const QString& status, const QString& detail, const QStringList& counts);
    QHBoxLayout* actions() const { return actions_; }

private:
    QLabel* icon_;
    QLabel* title_;
    QLabel* detail_;
    QLabel* counts_;
    QHBoxLayout* actions_;
};

// ---- Friendlier layout (19 Nov 2026): icons, switches, help badges, collapsible sections ----------------

// Line icons drawn with QPainter in the app's style (the build has no Qt SVG). Names: dashboard, changes,
// plugins, conflicts, assets, dependencies, compile, validation, reports, automerge, diagnostics, settings,
// scan, analyse, review, install, play, check, refresh, block, info, folder, export, warning, records.
QPixmap linePixmap(const QString& name, const QColor& color, int size, qreal dpr = 2.0);
QIcon lineIcon(const QString& name, const QColor& color, int size = 18);

// A check box drawn as a switch. Its text is kept (for accessibility and lookups) but not drawn: the row
// around it shows the title and description.
class ToggleSwitch : public QCheckBox {
    Q_OBJECT
public:
    explicit ToggleSwitch(const QString& text, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {40, 22}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;
    bool hitButton(const QPoint& pos) const override { return rect().contains(pos); }
};

// A small "?" that shows its explanation as a tooltip.
class HelpBadge : public QLabel {
    Q_OBJECT
public:
    explicit HelpBadge(const QString& help, QWidget* parent = nullptr);
};

// One option: a title (with a help badge when `help` is given), a one-line description under it, and the
// controls on the right. `description` may be changed later through the returned label (objectName
// "OptionDescription").
QWidget* makeOptionRow(const QString& title, const QString& description, const QString& help,
                       const std::vector<QWidget*>& controls);

// A card whose header (icon, title, one-line summary, chevron) shows or hides its body.
class CollapsibleSection : public QFrame {
    Q_OBJECT
public:
    CollapsibleSection(const QString& icon, const QString& title, bool expanded = false, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return bodyLayout_; }
    void setSummary(const QString& text);
    void setExpanded(bool on);
    bool expanded() const { return expanded_; }

private:
    QWidget* bodyWidget_;
    QVBoxLayout* bodyLayout_;
    QLabel* summary_;
    QLabel* chevron_;
    bool expanded_ = false;
};

// A card with a titled header (icon + title) and a body layout, for options that are always shown.
QFrame* makeSectionCard(const QString& icon, const QString& title, QVBoxLayout** body);

// The steps from scanning to playing, each done, waiting on the user, or still ahead.
class JourneyView : public QWidget {
    Q_OBJECT
public:
    enum class State { Done, Attention, Pending };
    struct Step {
        QString icon, title, detail;
        State state = State::Pending;
        QString action; // tooltip: what clicking the step does; empty = not clickable
    };
    explicit JourneyView(QWidget* parent = nullptr);
    void setSteps(std::vector<Step> steps);
    QSize sizeHint() const override { return {800, 104}; }
    int stepAt(QPoint pos) const;

signals:
    // A step was clicked (or chosen with the keyboard: Tab to the view, arrows, Enter).
    void stepActivated(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent*) override;
    bool event(QEvent* e) override;

private:
    std::vector<Step> steps_;
    int hover_ = -1;
    int focusStep_ = 0;
};

// A number with an icon tile and one line of text under it.
class IconTile : public QFrame {
    Q_OBJECT
public:
    IconTile(const QString& icon, const QColor& accent, QWidget* parent = nullptr);
    void setValue(const QString& value, const QString& text);

private:
    QLabel* value_;
    QLabel* text_;
};

// A row of the "Needs your attention" list: coloured icon, a bold line, a plain line, and an action.
QWidget* makeAttentionRow(const QString& icon, const QColor& accent, const QString& title, const QString& text,
                          QPushButton* action);

// Single-choice segmented buttons (presets).
class SegmentedControl : public QFrame {
    Q_OBJECT
public:
    explicit SegmentedControl(const QStringList& labels, QWidget* parent = nullptr);
    QString current() const;
    void setCurrent(const QString& label); // does not emit
    QPushButton* button(const QString& label) const;

signals:
    void picked(const QString& label);

private:
    QButtonGroup* group_;
    QStringList labels_;
};

// Page title + subtitle + optional right-aligned widgets. With `icon`, the page's icon sits in a tile on the
// left; with `help`, a "?" next to the title holds the longer explanation.
QWidget* makePageHeader(const QString& title, const QString& subtitle, const std::vector<QWidget*>& actions = {},
                        const QString& icon = {}, const QString& help = {});
QLabel* makeSectionTitle(const QString& text);
// A button; with `icon`, a line icon in the button's text colour (light on Primary buttons).
QPushButton* makeButton(const QString& text, const char* objectName = nullptr, const QString& icon = {});
// A section title with a "?" holding its explanation.
QWidget* makeSectionTitle(const QString& text, const QString& help);

} // namespace gui
