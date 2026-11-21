#include "widgets/Widgets.hpp"
#include <QHelpEvent>
#include <QToolTip>
#include <QKeyEvent>

#include "Theme.hpp"

#include <QButtonGroup>
#include <QMouseEvent>
#include <QStyleOption>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace gui {

StatCard::StatCard(const QString& label, QWidget* parent) : QFrame(parent)
{
    setObjectName("Card");
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(16, 12, 16, 12);
    l->setSpacing(2);
    label_ = new QLabel(label.toUpper(), this);
    label_->setObjectName("CardLabel");
    value_ = new QLabel(QStringLiteral("—"), this);
    value_->setObjectName("CardValue");
    hint_ = new QLabel(" ", this);
    hint_->setObjectName("CardHint");
    l->addWidget(label_);
    l->addWidget(value_);
    l->addWidget(hint_);
    setMinimumWidth(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void StatCard::setValue(const QString& value, const QString& hint)
{
    value_->setText(value);
    hint_->setText(hint.isEmpty() ? QStringLiteral(" ") : hint);
}

void StatCard::setAccent(const QColor& c)
{
    value_->setStyleSheet(QString("color: %1;").arg(c.name()));
}

StageIndicator::StageIndicator(QStringList stages, QWidget* parent)
    : QWidget(parent), stages_(std::move(stages)), states_(stages_.size(), State::Pending)
{
    setMinimumHeight(96);
}

void StageIndicator::reset()
{
    std::fill(states_.begin(), states_.end(), State::Pending);
    update();
}

void StageIndicator::activate(const QString& stage)
{
    int idx = -1;
    for (int i = 0; i < stages_.size(); ++i)
        if (stages_[i].compare(stage, Qt::CaseInsensitive) == 0)
            idx = i;
    if (idx < 0)
        return;
    for (int i = 0; i < idx; ++i)
        if (states_[i] != State::Failed)
            states_[i] = State::Done;
    states_[idx] = State::Running;
    update();
}

void StageIndicator::finish(bool ok)
{
    for (auto& s : states_)
        if (s == State::Running)
            s = ok ? State::Done : State::Failed;
    update();
}

void StageIndicator::setCaption(const QString& left, const QString& right)
{
    left_ = left;
    right_ = right;
    update();
}

QSize StageIndicator::sizeHint() const
{
    return {900, 110};
}

void StageIndicator::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int n = static_cast<int>(stages_.size());
    if (n == 0)
        return;
    const double captionWidth = left_.isEmpty() ? 0 : 150;
    const double x0 = 20 + captionWidth, x1 = width() - 20 - (right_.isEmpty() ? 0 : captionWidth);
    const double y = height() * 0.42;
    const double step = n > 1 ? (x1 - x0) / (n - 1) : 0;
    const double r = 11;

    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);

    if (!left_.isEmpty()) {
        p.setPen(theme::kMuted);
        p.setFont(small);
        p.drawText(QRectF(10, 0, captionWidth - 10, height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, left_);
    }
    if (!right_.isEmpty()) {
        p.setPen(theme::kAccent);
        p.setFont(small);
        p.drawText(QRectF(width() - captionWidth, 0, captionWidth - 10, height()),
                   Qt::AlignVCenter | Qt::AlignRight | Qt::TextWordWrap, right_);
    }

    for (int i = 0; i + 1 < n; ++i) {
        bool done = states_[i] == State::Done;
        p.setPen(QPen(done ? theme::kAccent : theme::kLine, 3));
        p.drawLine(QPointF(x0 + i * step + r, y), QPointF(x0 + (i + 1) * step - r, y));
    }
    for (int i = 0; i < n; ++i) {
        QPointF c(x0 + i * step, y);
        QColor fill = theme::kPanel, ring = theme::kLine;
        switch (states_[i]) {
        case State::Done: fill = theme::kAccentDim, ring = theme::kAccent; break;
        case State::Running: fill = theme::kAccent, ring = theme::kAccent; break;
        case State::Failed: fill = QColor(0x52, 0x1f, 0x22), ring = theme::kRed; break;
        case State::Pending: break;
        }
        p.setPen(QPen(ring, 2));
        p.setBrush(fill);
        p.drawEllipse(c, r, r);
        if (states_[i] == State::Done) {
            p.setPen(QPen(theme::kAccent, 2.2, Qt::SolidLine, Qt::RoundCap));
            QPainterPath tick(QPointF(c.x() - 4.5, c.y()));
            tick.lineTo(c.x() - 1, c.y() + 3.5);
            tick.lineTo(c.x() + 5, c.y() - 3.5);
            p.setBrush(Qt::NoBrush);
            p.drawPath(tick);
        }
        p.setFont(small);
        p.setPen(states_[i] == State::Pending ? theme::kMuted : theme::kText);
        QRectF label(c.x() - step / 2 - 10, y + r + 6, step + 20, 20);
        p.drawText(label, Qt::AlignHCenter | Qt::AlignTop, stages_[i]);
    }
}

FilterChips::FilterChips(const QStringList& labels, QWidget* parent) : QWidget(parent), labels_(labels)
{
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    for (int i = 0; i < labels.size(); ++i) {
        auto* b = new QPushButton(labels[i], this);
        b->setObjectName("Chip");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        if (i == 0)
            b->setChecked(true);
        group_->addButton(b, i);
        l->addWidget(b);
    }
    l->addStretch();
    connect(group_, &QButtonGroup::idClicked, this, [this](int id) { emit changed(labels_[id]); });
}

QString FilterChips::current() const
{
    int id = group_->checkedId();
    return id >= 0 ? labels_[id] : QString();
}

void FilterChips::setCurrent(const QString& label)
{
    int idx = static_cast<int>(labels_.indexOf(label));
    if (idx >= 0)
        group_->button(idx)->setChecked(true);
}

StackedBar::StackedBar(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(46);
}

void StackedBar::setSegments(std::vector<Segment> segments)
{
    segments_ = std::move(segments);
    update();
}

void StackedBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    double total = 0;
    for (const auto& s : segments_)
        total += s.value;
    QRectF bar(0, 2, width(), 14);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kPanelRaised);
    p.drawRoundedRect(bar, 7, 7);
    if (total > 0) {
        QPainterPath clip;
        clip.addRoundedRect(bar, 7, 7);
        p.setClipPath(clip);
        double x = 0;
        for (const auto& s : segments_) {
            double w = bar.width() * s.value / total;
            p.setBrush(s.color);
            p.drawRect(QRectF(x, bar.top(), w, bar.height()));
            x += w;
        }
        p.setClipping(false);
    }
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.85);
    p.setFont(small);
    double x = 0;
    for (const auto& s : segments_) {
        p.setBrush(s.color);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(x + 5, 30), 4, 4);
        p.setPen(theme::kMuted);
        QString text = QString("%1  %2").arg(s.label).arg(QLocale().toString(qint64(s.value)));
        p.drawText(QPointF(x + 14, 34), text);
        x += QFontMetrics(small).horizontalAdvance(text) + 34;
    }
}

BuildStatusBanner::BuildStatusBanner(QWidget* parent) : QFrame(parent)
{
    setObjectName("Card");
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(18, 14, 18, 14);
    h->setSpacing(16);
    icon_ = new QLabel;
    icon_->setFixedWidth(46);
    icon_->setAlignment(Qt::AlignCenter);
    h->addWidget(icon_);
    auto* texts = new QVBoxLayout;
    texts->setSpacing(2);
    auto* caption = new QLabel("CURRENT BUILD STATUS");
    caption->setObjectName("CardLabel");
    title_ = new QLabel;
    detail_ = new QLabel;
    detail_->setObjectName("PageSubtitle");
    detail_->setWordWrap(true);
    counts_ = new QLabel;
    counts_->setWordWrap(true);
    texts->addWidget(caption);
    texts->addWidget(title_);
    texts->addWidget(detail_);
    texts->addWidget(counts_);
    h->addLayout(texts, 1);
    actions_ = new QHBoxLayout;
    actions_->setSpacing(8);
    h->addLayout(actions_);
}

void BuildStatusBanner::setStatus(const QString& status, const QString& detail, const QStringList& counts)
{
    QString symbol = "•";
    QColor color = theme::kMuted;
    QString text = status.isEmpty() ? QString("Not Built") : status;
    if (status == "Up To Date")
        symbol = "✓", color = theme::kGreen;
    else if (status == "Changes Detected")
        symbol = "⚠", color = theme::kYellow;
    else if (status == "Requires Rebuild")
        symbol = "✖", color = theme::kRed;
    icon_->setText(symbol);
    // Sizes go through the style sheet: the application sheet would override setFont().
    icon_->setStyleSheet(QString("color:%1; font-size:26pt; font-weight:700;").arg(color.name()));
    title_->setText(text);
    title_->setStyleSheet(QString("color:%1; font-size:15pt; font-weight:600;").arg(color.name()));
    detail_->setText(detail);
    counts_->setText(counts.join("   ·   "));
    counts_->setVisible(!counts.isEmpty());
    setStyleSheet(QString("#Card { border-left: 4px solid %1; }").arg(color.name()));
}

// ---- Friendlier layout ------------------------------------------------------------------------------------

namespace {

// Draws the icon `name` in a 24 x 24 grid (the coordinates the paths below use).
void drawIcon(QPainter& p, const QString& name, const QColor& color)
{
    QPen pen(color, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    auto line = [&](double x1, double y1, double x2, double y2) {
        path.moveTo(x1, y1);
        path.lineTo(x2, y2);
    };
    auto poly = [&](std::initializer_list<QPointF> pts, bool close = false) {
        auto it = pts.begin();
        path.moveTo(*it);
        for (++it; it != pts.end(); ++it)
            path.lineTo(*it);
        if (close)
            path.closeSubpath();
    };
    auto circle = [&](double x, double y, double r) { path.addEllipse(QPointF(x, y), r, r); };
    auto hexagon = [&] {
        poly({{12, 3}, {20, 7.5}, {20, 16.5}, {12, 21}, {4, 16.5}, {4, 7.5}}, true);
    };
    if (name == "dashboard") {
        path.addRoundedRect(3, 3, 7, 9, 1.5, 1.5);
        path.addRoundedRect(14, 3, 7, 5, 1.5, 1.5);
        path.addRoundedRect(14, 12, 7, 9, 1.5, 1.5);
        path.addRoundedRect(3, 16, 7, 5, 1.5, 1.5);
    } else if (name == "changes") {
        circle(12, 12, 3);
        line(12, 3, 12, 9);
        line(12, 15, 12, 21);
        line(3, 12, 9, 12);
        line(15, 12, 21, 12);
    } else if (name == "plugins" || name == "records") {
        line(4, 6, 20, 6);
        line(4, 12, 20, 12);
        line(4, 18, 14, 18);
    } else if (name == "conflicts") {
        poly({{8, 4}, {4, 8}, {8, 12}});
        poly({{4, 8}, {20, 8}});
        poly({{16, 12}, {20, 16}, {16, 20}});
        poly({{20, 16}, {4, 16}});
    } else if (name == "assets") {
        path.addRoundedRect(3, 4, 18, 16, 2, 2);
        circle(9, 10, 2);
        poly({{21, 16}, {16, 11}, {7, 20}});
    } else if (name == "dependencies") {
        circle(6, 6, 2.5);
        circle(18, 6, 2.5);
        circle(12, 18, 2.5);
        line(7.3, 8.2, 10.8, 15.8);
        line(16.7, 8.2, 13.2, 15.8);
    } else if (name == "compile") {
        hexagon();
        poly({{4, 7.5}, {12, 12}, {20, 7.5}});
        line(12, 12, 12, 21);
    } else if (name == "validation") {
        poly({{12, 3}, {19, 6}, {19, 12}});
        path.cubicTo(19, 16.5, 16, 19.5, 12, 21);
        path.cubicTo(8, 19.5, 5, 16.5, 5, 12);
        poly({{5, 12}, {5, 6}, {12, 3}});
        poly({{9, 12}, {11, 14}, {15, 10}});
    } else if (name == "reports") {
        poly({{6, 3}, {15, 3}, {19, 7}, {19, 21}, {6, 21}}, true);
        poly({{14, 3}, {14, 8}, {19, 8}});
        line(9, 13, 16, 13);
        line(9, 17, 14, 17);
    } else if (name == "automerge") {
        circle(12, 12, 9);
        line(12, 7, 12, 17);
        line(7, 12, 17, 12);
    } else if (name == "diagnostics") {
        poly({{3, 12}, {7, 12}, {10, 4}, {14, 20}, {17, 12}, {21, 12}});
    } else if (name == "settings") {
        circle(12, 12, 3);
        circle(12, 12, 7.5);
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4;
            line(12 + 7.5 * std::cos(a), 12 + 7.5 * std::sin(a), 12 + 10 * std::cos(a), 12 + 10 * std::sin(a));
        }
    } else if (name == "scan") {
        circle(11, 11, 6);
        line(15.5, 15.5, 20, 20);
    } else if (name == "analyse") {
        line(4, 19, 4, 11);
        line(10, 19, 10, 5);
        line(16, 19, 16, 12);
        line(2, 19, 22, 19);
    } else if (name == "review" || name == "info") {
        circle(12, 12, 9);
        if (name == "review") {
            line(12, 8, 12, 13);
            line(12, 16.5, 12, 16.6);
        } else {
            line(12, 11, 12, 16);
            line(12, 7.5, 12, 7.6);
        }
    } else if (name == "warning") {
        poly({{12, 3.5}, {21, 19.5}, {3, 19.5}}, true);
        line(12, 10, 12, 14);
        line(12, 16.8, 12, 16.9);
    } else if (name == "install") {
        line(12, 4, 12, 15);
        poly({{7, 10}, {12, 15}, {17, 10}});
        line(5, 20, 19, 20);
    } else if (name == "export") {
        line(12, 15, 12, 4);
        poly({{7, 9}, {12, 4}, {17, 9}});
        poly({{5, 13}, {5, 20}, {19, 20}, {19, 13}});
    } else if (name == "play") {
        poly({{8, 5}, {19, 12}, {8, 19}}, true);
    } else if (name == "check") {
        poly({{5, 12.5}, {9.5, 17}, {19, 7.5}});
    } else if (name == "refresh") {
        path.arcMoveTo(4, 4, 16, 16, 30);
        path.arcTo(4, 4, 16, 16, 30, 300);
        poly({{20, 4}, {19.9, 9}, {15, 9}});
    } else if (name == "block") {
        circle(12, 12, 9);
        line(5.8, 5.8, 18.2, 18.2);
    } else if (name == "folder") {
        poly({{3, 6}, {9, 6}, {11, 8}, {21, 8}, {21, 19}, {3, 19}}, true);
    } else {
        circle(12, 12, 8);
    }
    p.drawPath(path);
}

} // namespace

QPixmap linePixmap(const QString& name, const QColor& color, int size, qreal dpr)
{
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    drawIcon(p, name, color);
    return pm;
}

QIcon lineIcon(const QString& name, const QColor& color, int size)
{
    QIcon icon;
    icon.addPixmap(linePixmap(name, color, size, 1.0));
    icon.addPixmap(linePixmap(name, color, size, 2.0));
    return icon;
}

ToggleSwitch::ToggleSwitch(const QString& text, QWidget* parent) : QCheckBox(text, parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(text);
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track(1, (height() - 20) / 2.0, 38, 20);
    const bool on = isChecked();
    const bool enabled = isEnabled();
    QColor fill = on ? theme::kAccentDim : QColor(0x2a, 0x35, 0x30);
    QColor knob = on ? QColor(0xea, 0xff, 0xf2) : theme::kMuted;
    if (!enabled) {
        fill = QColor(0x1c, 0x24, 0x20);
        knob = QColor(0x4f, 0x63, 0x59);
    }
    p.setPen(on && enabled ? QPen(theme::kAccent, 1) : QPen(Qt::NoPen));
    p.setBrush(fill);
    p.drawRoundedRect(track, 10, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(knob);
    const double x = on ? track.right() - 17 : track.left() + 3;
    p.drawEllipse(QRectF(x, track.top() + 3, 14, 14));
    if (hasFocus()) {
        p.setPen(QPen(theme::kAccent, 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track.adjusted(-1, -1, 1, 1), 11, 11);
    }
}

HelpBadge::HelpBadge(const QString& help, QWidget* parent) : QLabel("?", parent)
{
    setObjectName("HelpBadge");
    setAlignment(Qt::AlignCenter);
    setFixedSize(17, 17);
    setToolTip(help);
    setCursor(Qt::WhatsThisCursor);
}

QWidget* makeOptionRow(const QString& title, const QString& description, const QString& help,
                       const std::vector<QWidget*>& controls)
{
    auto* w = new QWidget;
    w->setObjectName("OptionRow");
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(2, 9, 2, 9);
    h->setSpacing(12);
    auto* texts = new QVBoxLayout;
    texts->setSpacing(1);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(7);
    auto* t = new QLabel(title);
    t->setObjectName("OptionTitle");
    titleRow->addWidget(t);
    if (!help.isEmpty())
        titleRow->addWidget(new HelpBadge(help));
    titleRow->addStretch();
    texts->addLayout(titleRow);
    auto* d = new QLabel(description);
    d->setObjectName("OptionDescription");
    d->setWordWrap(true);
    d->setVisible(!description.isEmpty());
    texts->addWidget(d);
    h->addLayout(texts, 1);
    for (auto* c : controls)
        h->addWidget(c, 0, Qt::AlignVCenter);
    return w;
}

namespace {
QWidget* sectionHeader(const QString& icon, const QString& title, QLabel** summary, QLabel** chevron)
{
    auto* head = new QWidget;
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);
    auto* ic = new QLabel;
    ic->setPixmap(linePixmap(icon, theme::kAccent, 17));
    h->addWidget(ic);
    auto* t = new QLabel(title);
    t->setObjectName("SectionCardTitle");
    h->addWidget(t);
    if (summary) {
        *summary = new QLabel;
        (*summary)->setObjectName("OptionDescription");
        h->addWidget(*summary, 1);
    } else {
        h->addStretch(1);
    }
    if (chevron) {
        *chevron = new QLabel("▸");
        (*chevron)->setObjectName("OptionDescription");
        h->addWidget(*chevron);
    }
    return head;
}
} // namespace

CollapsibleSection::CollapsibleSection(const QString& icon, const QString& title, bool expanded, QWidget* parent)
    : QFrame(parent)
{
    setObjectName("Card");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(6);
    auto* head = sectionHeader(icon, title, &summary_, &chevron_);
    head->setCursor(Qt::PointingHandCursor);
    head->installEventFilter(this);
    v->addWidget(head);
    bodyWidget_ = new QWidget;
    bodyLayout_ = new QVBoxLayout(bodyWidget_);
    bodyLayout_->setContentsMargins(0, 4, 0, 0);
    bodyLayout_->setSpacing(0);
    v->addWidget(bodyWidget_);
    // A click anywhere on the header toggles the section.
    auto* toggle = new QPushButton(head);
    toggle->setObjectName("SectionToggle");
    toggle->setFlat(true);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setAccessibleName(title);
    toggle->setStyleSheet("QPushButton#SectionToggle { background: transparent; border: none; }");
    toggle->lower();
    auto resize = [toggle, head] { toggle->setGeometry(head->rect()); };
    connect(toggle, &QPushButton::clicked, this, [this] { setExpanded(!expanded_); });
    struct Fitter : QObject {
        std::function<void()> fit;
        bool eventFilter(QObject*, QEvent* e) override
        {
            if (e->type() == QEvent::Resize || e->type() == QEvent::Show)
                fit();
            return false;
        }
    };
    auto* fitter = new Fitter;
    fitter->setParent(head);
    fitter->fit = resize;
    head->installEventFilter(fitter);
    toggle->raise();
    setExpanded(expanded);
}

void CollapsibleSection::setSummary(const QString& text)
{
    summary_->setText(text.isEmpty() ? QString() : QString("· ") + text);
}

void CollapsibleSection::setExpanded(bool on)
{
    expanded_ = on;
    bodyWidget_->setVisible(on);
    chevron_->setText(on ? "▾" : "▸");
    summary_->setVisible(!on);
}

QFrame* makeSectionCard(const QString& icon, const QString& title, QVBoxLayout** body)
{
    auto* f = new QFrame;
    f->setObjectName("Card");
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(16, 12, 16, 8);
    v->setSpacing(4);
    v->addWidget(sectionHeader(icon, title, nullptr, nullptr));
    auto* b = new QVBoxLayout;
    b->setSpacing(0);
    v->addLayout(b);
    *body = b;
    return f;
}

JourneyView::JourneyView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(104);
    setMouseTracking(true);
    setFocusPolicy(Qt::TabFocus);
}

int JourneyView::stepAt(QPoint pos) const
{
    if (steps_.empty() || width() <= 0)
        return -1;
    const int i = int(pos.x() / (width() / double(steps_.size())));
    return i >= 0 && i < int(steps_.size()) && !steps_[size_t(i)].action.isEmpty() ? i : -1;
}

void JourneyView::mouseMoveEvent(QMouseEvent* e)
{
    const int i = stepAt(e->position().toPoint());
    if (i != hover_) {
        hover_ = i;
        setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void JourneyView::leaveEvent(QEvent*)
{
    hover_ = -1;
    update();
}

void JourneyView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        if (const int i = stepAt(e->position().toPoint()); i >= 0)
            emit stepActivated(i);
}

void JourneyView::keyPressEvent(QKeyEvent* e)
{
    const int n = int(steps_.size());
    if (n == 0)
        return QWidget::keyPressEvent(e);
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) {
        focusStep_ = (focusStep_ + (e->key() == Qt::Key_Right ? 1 : n - 1)) % n;
        update();
    } else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) &&
               !steps_[size_t(focusStep_)].action.isEmpty()) {
        emit stepActivated(focusStep_);
    } else {
        QWidget::keyPressEvent(e);
    }
}

bool JourneyView::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int i = stepAt(he->pos());
        if (i >= 0)
            QToolTip::showText(he->globalPos(), steps_[size_t(i)].action, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

void JourneyView::setSteps(std::vector<Step> steps)
{
    steps_ = std::move(steps);
    updateGeometry();
    update();
}

void JourneyView::paintEvent(QPaintEvent*)
{
    if (steps_.empty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int n = int(steps_.size());
    const double colW = width() / double(n);
    const double cy = 28, r = 21;
    auto colorOf = [](State s) {
        return s == State::Done ? theme::kAccent : s == State::Attention ? theme::kYellow : QColor(0x4f, 0x63, 0x59);
    };
    // Connectors: green up to the first step not done.
    for (int i = 0; i + 1 < n; ++i) {
        const double x1 = colW * (i + 0.5) + r, x2 = colW * (i + 1.5) - r;
        const bool done = steps_[i].state == State::Done && steps_[i + 1].state == State::Done;
        p.setPen(QPen(done ? theme::kAccent : QColor(0x2f, 0x3b, 0x35), 2));
        p.drawLine(QPointF(x1, cy), QPointF(x2, cy));
    }
    QFont titleFont = font();
    titleFont.setBold(true);
    QFont small = font();
    small.setPointSizeF(font().pointSizeF() * 0.86);
    for (int i = 0; i < n; ++i) {
        const auto& s = steps_[i];
        const double cx = colW * (i + 0.5);
        const QColor c = colorOf(s.state);
        if (i == hover_ || (hasFocus() && i == focusStep_)) {
            p.setPen(QPen(QColor(0x8a, 0xa1, 0x96), 1, hasFocus() && i == focusStep_ ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(QColor(0x3d, 0xdc, 0x84, 18));
            p.drawEllipse(QPointF(cx, cy), r + 8, r + 8);
        }
        if (s.state == State::Attention) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xf5, 0xc5, 0x42, 30));
            p.drawEllipse(QPointF(cx, cy), r + 5, r + 5);
        }
        p.setPen(QPen(c, 2));
        p.setBrush(s.state == State::Attention ? QColor(0x2a, 0x26, 0x14)
                   : s.state == State::Done    ? QColor(0x10, 0x20, 0x18)
                                               : QColor(0x14, 0x1a, 0x17));
        p.drawEllipse(QPointF(cx, cy), r, r);
        p.drawPixmap(QPointF(cx - 10, cy - 10), linePixmap(s.icon, s.state == State::Pending ? theme::kMuted : c, 20));
        p.setFont(titleFont);
        p.setPen(QColor(0xe4, 0xef, 0xe8));
        p.drawText(QRectF(cx - colW / 2, cy + r + 6, colW, 20), Qt::AlignHCenter | Qt::AlignTop, s.title);
        p.setFont(small);
        p.setPen(s.state == State::Attention ? theme::kYellow : theme::kMuted);
        p.drawText(QRectF(cx - colW / 2 + 4, cy + r + 26, colW - 8, 18), Qt::AlignHCenter | Qt::AlignTop,
                   QFontMetrics(small).elidedText(s.detail, Qt::ElideRight, int(colW - 8)));
    }
}

IconTile::IconTile(const QString& icon, const QColor& accent, QWidget* parent) : QFrame(parent)
{
    setObjectName("Card");
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(16, 14, 16, 14);
    h->setSpacing(14);
    auto* ic = new QLabel;
    ic->setFixedSize(42, 42);
    ic->setAlignment(Qt::AlignCenter);
    ic->setPixmap(linePixmap(icon, accent, 21));
    QColor bg = accent;
    bg.setAlpha(34);
    ic->setStyleSheet(QString("background: rgba(%1,%2,%3,%4); border-radius: 10px;")
                          .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(bg.alpha()));
    h->addWidget(ic);
    auto* v = new QVBoxLayout;
    v->setSpacing(0);
    value_ = new QLabel("—");
    value_->setObjectName("TileValue");
    text_ = new QLabel(" ");
    text_->setObjectName("OptionDescription");
    text_->setWordWrap(true);
    v->addWidget(value_);
    v->addWidget(text_);
    v->addStretch();
    h->addLayout(v, 1);
    // Room for a value and two lines of text, the same for every tile, so a hint that wraps (large counts)
    // neither overlaps the value nor leaves the tiles uneven.
    setFixedHeight(92);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void IconTile::setValue(const QString& value, const QString& text)
{
    value_->setText(value);
    text_->setText(text.isEmpty() ? QStringLiteral(" ") : text);
}

QWidget* makeAttentionRow(const QString& icon, const QColor& accent, const QString& title, const QString& text,
                          QPushButton* action)
{
    auto* w = new QWidget;
    w->setObjectName("AttentionRow");
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(8, 9, 8, 9);
    h->setSpacing(12);
    auto* ic = new QLabel;
    ic->setFixedSize(30, 30);
    ic->setAlignment(Qt::AlignCenter);
    ic->setPixmap(linePixmap(icon, accent, 16));
    ic->setStyleSheet(QString("background: rgba(%1,%2,%3,34); border-radius: 8px;")
                          .arg(accent.red()).arg(accent.green()).arg(accent.blue()));
    h->addWidget(ic);
    auto* v = new QVBoxLayout;
    v->setSpacing(1);
    auto* t = new QLabel(title);
    t->setObjectName("OptionTitle");
    auto* d = new QLabel(text);
    d->setObjectName("OptionDescription");
    d->setWordWrap(true);
    v->addWidget(t);
    v->addWidget(d);
    h->addLayout(v, 1);
    if (action)
        h->addWidget(action, 0, Qt::AlignVCenter);
    return w;
}

SegmentedControl::SegmentedControl(const QStringList& labels, QWidget* parent) : QFrame(parent), labels_(labels)
{
    setObjectName("Segmented");
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(3, 3, 3, 3);
    h->setSpacing(2);
    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    for (const auto& l : labels) {
        auto* b = new QPushButton(l);
        b->setObjectName("Segment");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        group_->addButton(b);
        h->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, l] { emit picked(l); });
    }
}

QString SegmentedControl::current() const
{
    auto* b = group_->checkedButton();
    return b ? b->text() : QString();
}

void SegmentedControl::setCurrent(const QString& label)
{
    for (auto* b : group_->buttons())
        if (b->text() == label)
            b->setChecked(true);
}

QPushButton* SegmentedControl::button(const QString& label) const
{
    for (auto* b : group_->buttons())
        if (b->text() == label)
            return static_cast<QPushButton*>(b);
    return nullptr;
}

QWidget* makePageHeader(const QString& title, const QString& subtitle, const std::vector<QWidget*>& actions,
                        const QString& icon, const QString& help)
{
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 6);
    l->setSpacing(12);
    if (!icon.isEmpty()) {
        auto* tile = new QLabel;
        tile->setObjectName("HeaderIcon");
        tile->setFixedSize(42, 42);
        tile->setAlignment(Qt::AlignCenter);
        tile->setPixmap(linePixmap(icon, theme::kAccent, 22, tile->devicePixelRatioF() > 1 ? tile->devicePixelRatioF() : 2.0));
        l->addWidget(tile, 0, Qt::AlignVCenter);
    }
    auto* texts = new QVBoxLayout;
    texts->setSpacing(2);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto* t = new QLabel(title);
    t->setObjectName("PageTitle");
    titleRow->addWidget(t);
    if (!help.isEmpty())
        titleRow->addWidget(new HelpBadge(help), 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    auto* s = new QLabel(subtitle);
    s->setObjectName("PageSubtitle");
    s->setWordWrap(true);
    s->setVisible(!subtitle.isEmpty());
    texts->addLayout(titleRow);
    texts->addWidget(s);
    l->addLayout(texts, 1);
    for (auto* a : actions)
        l->addWidget(a, 0, icon.isEmpty() ? Qt::AlignBottom : Qt::AlignVCenter);
    return w;
}

QLabel* makeSectionTitle(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("SectionTitle");
    return l;
}

QWidget* makeSectionTitle(const QString& text, const QString& help)
{
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(8);
    l->addWidget(makeSectionTitle(text));
    if (!help.isEmpty())
        l->addWidget(new HelpBadge(help), 0, Qt::AlignVCenter);
    l->addStretch(1);
    return w;
}

QPushButton* makeButton(const QString& text, const char* objectName, const QString& icon)
{
    auto* b = new QPushButton(text);
    if (objectName)
        b->setObjectName(objectName);
    b->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty()) {
        const bool primary = objectName && QByteArray(objectName) == "Primary";
        b->setIcon(lineIcon(icon, primary ? QColor(0xea, 0xff, 0xf2) : theme::kText, 15));
    }
    return b;
}

} // namespace gui
