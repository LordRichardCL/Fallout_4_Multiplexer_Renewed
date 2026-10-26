#pragma once

#include <QGraphicsView>
#include <QHash>
#include <QString>

#include <vector>

class QGraphicsScene;
class QGraphicsRectItem;
class QGraphicsPathItem;

namespace gui {

// Zoomable, pannable dependency graph with relationship highlighting.
class GraphView : public QGraphicsView {
    Q_OBJECT
public:
    struct Node {
        QString id;
        QString label;
        QString detail;
        QColor color;
    };
    struct Edge {
        QString from;
        QString to;
        QString label;
    };

    explicit GraphView(QWidget* parent = nullptr);
    // Lays nodes out in dependency layers (dependencies to the right).
    void setGraph(std::vector<Node> nodes, std::vector<Edge> edges, const QString& focus = {});
    bool findAndSelect(const QString& text);
    void selectNode(const QString& id);
    void fit();

signals:
    void nodeActivated(const QString& id);

protected:
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void highlight(const QString& id);
    QString nodeAt(const QPoint& pos) const;

    QGraphicsScene* scene_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    QHash<QString, QGraphicsRectItem*> items_;
    std::vector<QGraphicsPathItem*> edgeItems_;
};

} // namespace gui
