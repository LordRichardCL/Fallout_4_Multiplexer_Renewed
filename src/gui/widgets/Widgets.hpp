#pragma once

#include <QColor>
#include <QFrame>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

#include <vector>

class QLabel;
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

// Page title + subtitle + optional right-aligned widgets.
QWidget* makePageHeader(const QString& title, const QString& subtitle, const std::vector<QWidget*>& actions = {});
QLabel* makeSectionTitle(const QString& text);
QPushButton* makeButton(const QString& text, const char* objectName = nullptr);

} // namespace gui
