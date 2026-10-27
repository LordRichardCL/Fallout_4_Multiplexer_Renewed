#include "widgets/GraphView.hpp"

#include "Theme.hpp"

#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QMouseEvent>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <map>

namespace gui {

namespace {
constexpr double kNodeW = 230, kNodeH = 38, kColGap = 110, kRowGap = 16;
}

GraphView::GraphView(QWidget* parent) : QGraphicsView(parent), scene_(new QGraphicsScene(this))
{
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setBackgroundBrush(QColor(0x11, 0x16, 0x13));
    setFrameShape(QFrame::NoFrame);
}

void GraphView::setGraph(std::vector<Node> nodes, std::vector<Edge> edges, const QString& focus)
{
    autoFit_ = true;
    scene_->clear();
    items_.clear();
    edgeItems_.clear();
    nodes_ = std::move(nodes);
    edges_ = std::move(edges);

    // Layer = longest path from a root over the graph with every cycle collapsed into one node
    // (records that reference each other share a column instead of pushing each other right).
    const int n = static_cast<int>(nodes_.size());
    QHash<QString, int> indexOf;
    for (int i = 0; i < n; ++i)
        indexOf[nodes_[static_cast<size_t>(i)].id] = i;
    std::vector<std::vector<int>> out(static_cast<size_t>(n));
    for (const auto& e : edges_) {
        auto a = indexOf.find(e.from), b = indexOf.find(e.to);
        if (a != indexOf.end() && b != indexOf.end() && *a != *b)
            out[static_cast<size_t>(*a)].push_back(*b);
    }
    // Tarjan's strongly connected components (iterative).
    std::vector<int> comp(static_cast<size_t>(n), -1), low(static_cast<size_t>(n)), num(static_cast<size_t>(n), -1);
    std::vector<int> stack;
    std::vector<char> onStack(static_cast<size_t>(n), 0);
    int counter = 0, comps = 0;
    for (int root = 0; root < n; ++root) {
        if (num[static_cast<size_t>(root)] >= 0)
            continue;
        std::vector<std::pair<int, size_t>> work{{root, 0}};
        while (!work.empty()) {
            auto& [v, next] = work.back();
            auto vs = static_cast<size_t>(v);
            if (next == 0) {
                num[vs] = low[vs] = counter++;
                stack.push_back(v);
                onStack[vs] = 1;
            }
            if (next < out[vs].size()) {
                int w = out[vs][next++];
                auto ws = static_cast<size_t>(w);
                if (num[ws] < 0)
                    work.push_back({w, 0});
                else if (onStack[ws])
                    low[vs] = std::min(low[vs], num[ws]);
                continue;
            }
            if (low[vs] == num[vs]) {
                int w;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[static_cast<size_t>(w)] = 0;
                    comp[static_cast<size_t>(w)] = comps;
                } while (w != v);
                ++comps;
            }
            int finished = v;
            work.pop_back();
            if (!work.empty()) {
                auto ps = static_cast<size_t>(work.back().first);
                low[ps] = std::min(low[ps], low[static_cast<size_t>(finished)]);
            }
        }
    }
    // Tarjan numbers components in reverse topological order: walk them from last to first.
    std::vector<std::vector<int>> members(static_cast<size_t>(comps));
    for (int v = 0; v < n; ++v)
        members[static_cast<size_t>(comp[static_cast<size_t>(v)])].push_back(v);
    std::vector<int> compLayer(static_cast<size_t>(comps), 0);
    for (int c = comps - 1; c >= 0; --c)
        for (int v : members[static_cast<size_t>(c)])
            for (int w : out[static_cast<size_t>(v)]) {
                int cw = comp[static_cast<size_t>(w)];
                if (cw != c)
                    compLayer[static_cast<size_t>(cw)] =
                        std::max(compLayer[static_cast<size_t>(cw)], compLayer[static_cast<size_t>(c)] + 1);
            }
    QHash<QString, int> layer;
    for (int v = 0; v < n; ++v)
        layer[nodes_[static_cast<size_t>(v)].id] = compLayer[static_cast<size_t>(comp[static_cast<size_t>(v)])];
    std::map<int, std::vector<const Node*>> columns;
    for (const auto& n : nodes_)
        columns[layer[n.id]].push_back(&n);

    QFont font = this->font();
    QFont small = font;
    small.setPointSizeF(font.pointSizeF() * 0.8);
    for (auto& [col, list] : columns) {
        std::sort(list.begin(), list.end(), [](const Node* a, const Node* b) { return a->label < b->label; });
        double totalH = list.size() * (kNodeH + kRowGap);
        for (size_t i = 0; i < list.size(); ++i) {
            const Node* n = list[i];
            double x = col * (kNodeW + kColGap);
            double y = i * (kNodeH + kRowGap) - totalH / 2;
            auto* rect = scene_->addRect(QRectF(0, 0, kNodeW, kNodeH), QPen(n->color.isValid() ? n->color : theme::kLine, 1.5),
                                         QColor(0x18, 0x20, 0x1c));
            rect->setPos(x, y);
            rect->setData(0, n->id);
            rect->setToolTip(n->detail.isEmpty() ? n->label : n->label + "\n" + n->detail);
            rect->setZValue(1);
            auto* text = new QGraphicsSimpleTextItem(rect);
            QString label = QFontMetrics(font).elidedText(n->label, Qt::ElideMiddle, int(kNodeW - 16));
            text->setText(label);
            text->setFont(font);
            text->setBrush(theme::kText);
            text->setPos(8, n->detail.isEmpty() ? 10 : 3);
            if (!n->detail.isEmpty()) {
                auto* d = new QGraphicsSimpleTextItem(rect);
                d->setText(QFontMetrics(small).elidedText(n->detail, Qt::ElideRight, int(kNodeW - 16)));
                d->setFont(small);
                d->setBrush(theme::kMuted);
                d->setPos(8, 21);
            }
            items_[n->id] = rect;
        }
    }

    for (const auto& e : edges_) {
        auto* a = items_.value(e.from);
        auto* b = items_.value(e.to);
        if (!a || !b || a == b)
            continue;
        QPointF p1 = a->pos() + QPointF(kNodeW, kNodeH / 2);
        QPointF p2 = b->pos() + QPointF(0, kNodeH / 2);
        if (p2.x() <= p1.x()) { // back edge (cycle or same layer)
            p1 = a->pos() + QPointF(kNodeW / 2, kNodeH);
            p2 = b->pos() + QPointF(kNodeW / 2, kNodeH);
        }
        QPainterPath path(p1);
        double dx = std::max(40.0, (p2.x() - p1.x()) / 2);
        path.cubicTo(p1 + QPointF(dx, 0), p2 - QPointF(dx, 0), p2);
        // Arrow head
        QPointF tip = p2;
        path.moveTo(tip);
        path.lineTo(tip + QPointF(-8, -4));
        path.moveTo(tip);
        path.lineTo(tip + QPointF(-8, 4));
        auto* item = scene_->addPath(path, QPen(QColor(0x3a, 0x48, 0x41), 1.3));
        item->setData(0, e.from);
        item->setData(1, e.to);
        item->setToolTip(e.label);
        item->setZValue(0);
        edgeItems_.push_back(item);
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-60, -60, 60, 60));
    if (!focus.isEmpty() && items_.contains(focus))
        selectNode(focus);
    fit();
}

void GraphView::resizeEvent(QResizeEvent* e)
{
    QGraphicsView::resizeEvent(e);
    if (autoFit_)
        fit();
}

void GraphView::fit()
{
    if (!scene_->items().isEmpty()) {
        fitInView(scene_->itemsBoundingRect().adjusted(-40, -40, 40, 40), Qt::KeepAspectRatio);
        if (transform().m11() > 1.2)
            setTransform(QTransform::fromScale(1.2, 1.2));
    }
}

void GraphView::highlight(const QString& id)
{
    QSet<QString> related;
    related.insert(id);
    for (auto* e : edgeItems_) {
        QString from = e->data(0).toString(), to = e->data(1).toString();
        bool outgoing = from == id, incoming = to == id;
        QColor c = outgoing ? theme::kAccent : incoming ? theme::kYellow : QColor(0x2a, 0x33, 0x2f);
        e->setPen(QPen(c, outgoing || incoming ? 2.2 : 1.0));
        e->setZValue(outgoing || incoming ? 0.5 : 0);
        if (outgoing)
            related.insert(to);
        if (incoming)
            related.insert(from);
    }
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        bool sel = it.key() == id;
        bool rel = related.contains(it.key());
        it.value()->setOpacity(id.isEmpty() || rel ? 1.0 : 0.35);
        it.value()->setBrush(sel ? QColor(0x1f, 0x4a, 0x33) : QColor(0x18, 0x20, 0x1c));
    }
}

void GraphView::selectNode(const QString& id)
{
    highlight(id);
    if (auto* item = items_.value(id))
        centerOn(item);
}

bool GraphView::findAndSelect(const QString& text)
{
    for (const auto& n : nodes_)
        if (n.label.contains(text, Qt::CaseInsensitive) || n.id.compare(text, Qt::CaseInsensitive) == 0) {
            selectNode(n.id);
            return true;
        }
    return false;
}

QString GraphView::nodeAt(const QPoint& pos) const
{
    for (auto* item : items(pos)) {
        QGraphicsItem* it = item;
        while (it && !it->data(0).isValid())
            it = it->parentItem();
        if (it && dynamic_cast<QGraphicsRectItem*>(it))
            return it->data(0).toString();
    }
    return {};
}

void GraphView::wheelEvent(QWheelEvent* e)
{
    autoFit_ = false;
    double factor = e->angleDelta().y() > 0 ? 1.15 : 1 / 1.15;
    double current = transform().m11();
    if ((current > 4 && factor > 1) || (current < 0.05 && factor < 1))
        return;
    scale(factor, factor);
}

void GraphView::mousePressEvent(QMouseEvent* e)
{
    autoFit_ = false; // the user is navigating: keep their view
    QString id = nodeAt(e->pos());
    if (!id.isEmpty()) {
        highlight(id);
        emit nodeSelected(id);
    }
    QGraphicsView::mousePressEvent(e);
}

void GraphView::mouseDoubleClickEvent(QMouseEvent* e)
{
    QString id = nodeAt(e->pos());
    if (!id.isEmpty())
        emit nodeActivated(id);
    QGraphicsView::mouseDoubleClickEvent(e);
}

} // namespace gui
