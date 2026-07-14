#pragma once

#include "model/Topology.hpp"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QMap>
#include <QSet>

namespace bgptester
{

class RouterGraphicsItem;
class LinkGraphicsItem;

class TopologyScene final : public QGraphicsScene
{
    Q_OBJECT

public:
    enum class Mode
    {
        Select,
        AddRouter,
        AddLink
    };
    Q_ENUM(Mode)

    explicit TopologyScene(QObject* parent = nullptr);
    ~TopologyScene() override;

    void setTopology(Topology* topology);
    void rebuild();
    void setMode(Mode mode);
    Mode mode() const
    {
        return mode_;
    }
    void setEditable(bool editable);
    bool isEditable() const
    {
        return editable_;
    }
    QStringList selectedRouterIds() const;
    bool deleteSelection();

    void setRouterRuntimeState(const QString& routerId, bool active);
    void setLinkRuntimeState(const QString& a, const QString& b, bool enabled);
    void highlightPath(const QStringList& hops);
    void clearRuntimeState();

    void routerMoved(const QString& routerId, const QPointF& position);
    void requestRouterEdit(const QString& routerId);
    void requestLinkEdit(const QString& a, const QString& b);

signals:
    void createRouterRequested(QPointF position);
    void createLinkRequested(QString a, QString b);
    void editRouterRequested(QString routerId);
    void editLinkRequested(QString a, QString b);
    void topologyModified();
    void selectionContextChanged(QString routerId, QString linkA, QString linkB);
    void modeChanged(bgptester::TopologyScene::Mode mode);

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void finishRouterMove();
    void updateConnectedLinks(const QString& routerId);
    void rebuildAsGroups();
    void updateSceneRectFromRouters();
    void updateSelectionContext();
    RouterGraphicsItem* routerItemAt(const QPointF& position) const;

    Topology* topology_ = nullptr;
    QMap<QString, RouterGraphicsItem*> routerItems_;
    QMap<QString, LinkGraphicsItem*> linkItems_;
    QList<QGraphicsItem*> asGroupItems_;
    QMap<QString, bool> runtimeRouterState_;
    QMap<QString, bool> runtimeLinkState_;
    QSet<QString> highlightedEdges_;
    QString pendingLinkStart_;
    Mode mode_ = Mode::Select;
    bool editable_ = true;
    bool rebuilding_ = false;
    bool routerMovePending_ = false;
};

class TopologyView final : public QGraphicsView
{
    Q_OBJECT

public:
    explicit TopologyView(TopologyScene* scene, QWidget* parent = nullptr);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    bool panning_ = false;
    QPoint lastPanPoint_;
};

} // namespace bgptester
