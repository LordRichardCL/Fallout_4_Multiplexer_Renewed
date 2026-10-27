#include "widgets/Widgets.hpp"

#include "Theme.hpp"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

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

QWidget* makePageHeader(const QString& title, const QString& subtitle, const std::vector<QWidget*>& actions)
{
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 6);
    auto* texts = new QVBoxLayout;
    texts->setSpacing(2);
    auto* t = new QLabel(title);
    t->setObjectName("PageTitle");
    auto* s = new QLabel(subtitle);
    s->setObjectName("PageSubtitle");
    s->setWordWrap(true);
    texts->addWidget(t);
    texts->addWidget(s);
    l->addLayout(texts, 1);
    for (auto* a : actions)
        l->addWidget(a, 0, Qt::AlignBottom);
    return w;
}

QLabel* makeSectionTitle(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("SectionTitle");
    return l;
}

QPushButton* makeButton(const QString& text, const char* objectName)
{
    auto* b = new QPushButton(text);
    if (objectName)
        b->setObjectName(objectName);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

} // namespace gui
