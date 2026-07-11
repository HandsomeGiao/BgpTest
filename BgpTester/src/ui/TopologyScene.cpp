#include "ui/TopologyScene.hpp"

#include <QApplication>
#include <QGraphicsItemGroup>
#include <QGraphicsSceneMouseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace bgptester {
namespace {

QColor asColor(quint32 asn) {
  const auto hue = static_cast<int>((asn * 137U) % 360U);
  return QColor::fromHsv(hue, 90, 225, 35);
}

QPointF pointOnLine(const QLineF &line, qreal distanceFromStart) {
  if (line.length() < 0.001) {
    return line.p1();
  }
  return line.pointAt(distanceFromStart / line.length());
}

void drawArrow(QPainter *painter, const QLineF &line, bool atEnd,
               const QColor &color) {
  if (line.length() < 30.0) {
    return;
  }
  const auto angle = std::atan2(-line.dy(), line.dx());
  const auto center = pointOnLine(line, atEnd ? line.length() - 57.0 : 57.0);
  const auto direction = atEnd ? angle : angle + std::numbers::pi;
  constexpr qreal size = 9.0;
  const QPointF p1 = center + QPointF(std::cos(direction + 2.55) * size,
                                      -std::sin(direction + 2.55) * size);
  const QPointF p2 = center + QPointF(std::cos(direction - 2.55) * size,
                                      -std::sin(direction - 2.55) * size);
  QPainterPath path(center);
  path.lineTo(p1);
  path.lineTo(p2);
  path.closeSubpath();
  painter->setPen(Qt::NoPen);
  painter->setBrush(color);
  painter->drawPath(path);
}

} // namespace

class RouterGraphicsItem final : public QGraphicsObject {
public:
  enum { Type = QGraphicsItem::UserType + 1 };

  RouterGraphicsItem(RouterConfig config, bool routeReflector,
                     TopologyScene *topologyScene)
      : config_(std::move(config)), routeReflector_(routeReflector),
        topologyScene_(topologyScene) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setZValue(2.0);
    setToolTip(QStringLiteral("%1\n插件：%2")
                   .arg(config_.id, config_.pluginId));
  }

  int type() const override { return Type; }
  QRectF boundingRect() const override { return {-55.0, -35.0, 110.0, 70.0}; }

  void setRuntimeState(std::optional<bool> active) {
    active_ = active;
    update();
  }

  void setLinkStart(bool value) {
    linkStart_ = value;
    update();
  }

  const QString &routerId() const { return config_.id; }

protected:
  QVariant itemChange(GraphicsItemChange change,
                      const QVariant &value) override {
    if (change == ItemPositionHasChanged && topologyScene_) {
      topologyScene_->routerMoved(config_.id, value.toPointF());
    }
    return QGraphicsObject::itemChange(change, value);
  }

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
    if (topologyScene_ && topologyScene_->isEditable()) {
      topologyScene_->requestRouterEdit(config_.id);
      event->accept();
      return;
    }
    QGraphicsObject::mouseDoubleClickEvent(event);
  }

  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
             QWidget *) override {
    painter->setRenderHint(QPainter::Antialiasing);
    QColor fill(QStringLiteral("#f7fafc"));
    QColor border(QStringLiteral("#415a77"));
    if (active_.has_value() && !*active_) {
      fill = QColor(QStringLiteral("#f8d7da"));
      border = QColor(QStringLiteral("#b02a37"));
    } else if (active_.has_value()) {
      fill = QColor(QStringLiteral("#d8f3dc"));
      border = QColor(QStringLiteral("#2d6a4f"));
    }
    if (option->state & QStyle::State_Selected) {
      border = QColor(QStringLiteral("#1976d2"));
    }
    if (linkStart_) {
      border = QColor(QStringLiteral("#ff9800"));
    }
    painter->setPen(QPen(border, (option->state & QStyle::State_Selected) ||
                                     linkStart_
                                 ? 3.0
                                 : 2.0));
    painter->setBrush(fill);
    painter->drawRoundedRect(boundingRect().adjusted(2, 2, -2, -2), 12, 12);
    painter->setPen(QColor(QStringLiteral("#172b4d")));
    auto bold = painter->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() + 1.0);
    painter->setFont(bold);
    painter->drawText(QRectF(-50, -27, 100, 24), Qt::AlignCenter, config_.id);
    auto normal = painter->font();
    normal.setBold(false);
    normal.setPointSizeF(normal.pointSizeF() - 1.0);
    painter->setFont(normal);
    painter->drawText(QRectF(-50, -6, 100, 18), Qt::AlignCenter,
                      QStringLiteral("AS%1").arg(config_.asn));
    painter->drawText(QRectF(-50, 11, 100, 17), Qt::AlignCenter,
                      config_.routerId);
    if (routeReflector_) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#7b2cbf")));
      painter->drawEllipse(QPointF(43, -24), 9, 9);
      painter->setPen(Qt::white);
      auto tiny = painter->font();
      tiny.setPointSize(6);
      tiny.setBold(true);
      painter->setFont(tiny);
      painter->drawText(QRectF(34, -33, 18, 18), Qt::AlignCenter,
                        QStringLiteral("RR"));
    }
    if (config_.pluginId != StandardRouterPluginId) {
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(QStringLiteral("#1976d2")));
      painter->drawEllipse(QPointF(-43, -24), 9, 9);
      painter->setPen(Qt::white);
      auto tiny = painter->font();
      tiny.setPointSize(7);
      tiny.setBold(true);
      painter->setFont(tiny);
      painter->drawText(QRectF(-52, -33, 18, 18), Qt::AlignCenter,
                        QStringLiteral("P"));
    }
  }

private:
  RouterConfig config_;
  bool routeReflector_ = false;
  TopologyScene *topologyScene_ = nullptr;
  std::optional<bool> active_;
  bool linkStart_ = false;
};

class LinkGraphicsItem final : public QGraphicsObject {
public:
  enum { Type = QGraphicsItem::UserType + 2 };

  LinkGraphicsItem(LinkConfig config, RouterGraphicsItem *a,
                   RouterGraphicsItem *b, TopologyScene *topologyScene)
      : config_(std::move(config)), a_(a), b_(b),
        topologyScene_(topologyScene) {
    setFlags(ItemIsSelectable);
    setZValue(0.0);
    updateGeometry();
  }

  int type() const override { return Type; }
  QRectF boundingRect() const override {
    return QRectF(line_.p1(), line_.p2()).normalized().adjusted(-16, -16, 16,
                                                                16);
  }
  QPainterPath shape() const override {
    QPainterPath path;
    path.moveTo(line_.p1());
    path.lineTo(line_.p2());
    QPainterPathStroker stroker;
    stroker.setWidth(16.0);
    return stroker.createStroke(path);
  }

  const QString &aId() const { return config_.a; }
  const QString &bId() const { return config_.b; }

  void updateGeometry() {
    prepareGeometryChange();
    line_ = QLineF(a_ ? a_->pos() : QPointF{}, b_ ? b_->pos() : QPointF{});
    update();
  }
  void setRuntimeState(std::optional<bool> enabled) {
    runtimeEnabled_ = enabled;
    update();
  }
  void setHighlighted(bool highlighted) {
    highlighted_ = highlighted;
    update();
  }

protected:
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
    if (topologyScene_ && topologyScene_->isEditable()) {
      topologyScene_->requestLinkEdit(config_.a, config_.b);
      event->accept();
      return;
    }
    QGraphicsObject::mouseDoubleClickEvent(event);
  }

  void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
             QWidget *) override {
    painter->setRenderHint(QPainter::Antialiasing);
    const bool enabled = runtimeEnabled_.value_or(config_.enabled);
    QColor color = enabled ? QColor(QStringLiteral("#607d8b"))
                           : QColor(QStringLiteral("#c62828"));
    qreal width = 2.0;
    if (highlighted_) {
      color = QColor(QStringLiteral("#00a8e8"));
      width = 5.0;
    } else if (option->state & QStyle::State_Selected) {
      color = QColor(QStringLiteral("#1976d2"));
      width = 3.5;
    }
    painter->setPen(QPen(color, width, enabled ? Qt::SolidLine : Qt::DashLine,
                         Qt::RoundCap));
    painter->drawLine(line_);
    if (config_.rrClientFromA) {
      drawArrow(painter, line_, true, color);
    }
    if (config_.rrClientFromB) {
      drawArrow(painter, line_, false, color);
    }

    QStringList annotations;
    if (config_.delayMs > 0) {
      annotations.append(QStringLiteral("%1 ms").arg(config_.delayMs));
    }
    if (config_.mraiMsFromA > 0 || config_.mraiMsFromB > 0) {
      annotations.append(QStringLiteral("MRAI %1/%2")
                             .arg(config_.mraiMsFromA)
                             .arg(config_.mraiMsFromB));
    }
    if (!annotations.isEmpty()) {
      const auto center = line_.center();
      const auto text = annotations.join(QStringLiteral(" · "));
      const auto metrics = painter->fontMetrics();
      QRectF rect = metrics.boundingRect(text).adjusted(-4, -2, 4, 2);
      rect.moveCenter(center + QPointF(0, -11));
      painter->setPen(Qt::NoPen);
      painter->setBrush(QColor(255, 255, 255, 220));
      painter->drawRoundedRect(rect, 3, 3);
      painter->setPen(QColor(QStringLiteral("#455a64")));
      painter->drawText(rect, Qt::AlignCenter, text);
    }
  }

private:
  LinkConfig config_;
  RouterGraphicsItem *a_ = nullptr;
  RouterGraphicsItem *b_ = nullptr;
  TopologyScene *topologyScene_ = nullptr;
  QLineF line_;
  std::optional<bool> runtimeEnabled_;
  bool highlighted_ = false;
};

TopologyScene::TopologyScene(QObject *parent) : QGraphicsScene(parent) {
  setSceneRect(-1000, -800, 3000, 2200);
  connect(this, &QGraphicsScene::selectionChanged, this,
          &TopologyScene::updateSelectionContext);
}

void TopologyScene::setTopology(Topology *topology) {
  topology_ = topology;
  rebuild();
}

void TopologyScene::rebuild() {
  rebuilding_ = true;
  clear();
  routerItems_.clear();
  linkItems_.clear();
  asGroupItems_.clear();
  pendingLinkStart_.clear();
  if (!topology_) {
    rebuilding_ = false;
    return;
  }

  for (auto it = topology_->routers.cbegin(); it != topology_->routers.cend();
       ++it) {
    bool isReflector = false;
    for (const auto &neighbor : topology_->neighborsFor(it.key())) {
      isReflector = isReflector || neighbor.rrClient;
    }
    auto *item = new RouterGraphicsItem(it.value(), isReflector, this);
    addItem(item);
    item->setPos(it->position);
    item->setFlag(QGraphicsItem::ItemIsMovable, editable_);
    if (runtimeRouterState_.contains(it.key())) {
      item->setRuntimeState(runtimeRouterState_.value(it.key()));
    }
    routerItems_.insert(it.key(), item);
  }
  for (const auto &link : topology_->links) {
    auto *a = routerItems_.value(link.a);
    auto *b = routerItems_.value(link.b);
    if (!a || !b) {
      continue;
    }
    auto *item = new LinkGraphicsItem(link, a, b, this);
    addItem(item);
    const auto key = Topology::edgeKey(link.a, link.b);
    if (runtimeLinkState_.contains(key)) {
      item->setRuntimeState(runtimeLinkState_.value(key));
    }
    item->setHighlighted(highlightedEdges_.contains(key));
    linkItems_.insert(key, item);
  }
  rebuildAsGroups();
  rebuilding_ = false;
}

void TopologyScene::setMode(Mode mode) {
  if (!editable_ && mode != Mode::Select) {
    mode = Mode::Select;
  }
  if (mode_ == mode) {
    return;
  }
  if (auto *old = routerItems_.value(pendingLinkStart_)) {
    old->setLinkStart(false);
  }
  pendingLinkStart_.clear();
  mode_ = mode;
  emit modeChanged(mode_);
}

void TopologyScene::setEditable(bool editable) {
  editable_ = editable;
  if (!editable_) {
    setMode(Mode::Select);
  }
  for (auto *item : std::as_const(routerItems_)) {
    item->setFlag(QGraphicsItem::ItemIsMovable, editable_);
  }
}

bool TopologyScene::deleteSelection() {
  if (!editable_ || !topology_) {
    return false;
  }
  QSet<QString> routersToRemove;
  QSet<QString> linksToRemove;
  for (auto *item : selectedItems()) {
    if (item->type() == RouterGraphicsItem::Type) {
      routersToRemove.insert(
          static_cast<RouterGraphicsItem *>(item)->routerId());
    } else if (item->type() == LinkGraphicsItem::Type) {
      auto *link = static_cast<LinkGraphicsItem *>(item);
      linksToRemove.insert(Topology::edgeKey(link->aId(), link->bId()));
    }
  }
  if (routersToRemove.isEmpty() && linksToRemove.isEmpty()) {
    return false;
  }
  for (const auto &id : routersToRemove) {
    topology_->routers.remove(id);
  }
  topology_->links.erase(
      std::remove_if(topology_->links.begin(), topology_->links.end(),
                     [&](const auto &link) {
                       return routersToRemove.contains(link.a) ||
                              routersToRemove.contains(link.b) ||
                              linksToRemove.contains(
                                  Topology::edgeKey(link.a, link.b));
                     }),
      topology_->links.end());
  rebuild();
  emit topologyModified();
  return true;
}

void TopologyScene::setRouterRuntimeState(const QString &routerId,
                                          bool active) {
  runtimeRouterState_.insert(routerId, active);
  if (auto *item = routerItems_.value(routerId)) {
    item->setRuntimeState(active);
  }
}

void TopologyScene::setLinkRuntimeState(const QString &a, const QString &b,
                                        bool enabled) {
  const auto key = Topology::edgeKey(a, b);
  runtimeLinkState_.insert(key, enabled);
  if (auto *item = linkItems_.value(key)) {
    item->setRuntimeState(enabled);
  }
}

void TopologyScene::highlightPath(const QStringList &hops) {
  highlightedEdges_.clear();
  for (int index = 0; index + 1 < hops.size(); ++index) {
    highlightedEdges_.insert(Topology::edgeKey(hops.at(index),
                                               hops.at(index + 1)));
  }
  for (auto it = linkItems_.begin(); it != linkItems_.end(); ++it) {
    it.value()->setHighlighted(highlightedEdges_.contains(it.key()));
  }
}

void TopologyScene::clearRuntimeState() {
  runtimeRouterState_.clear();
  runtimeLinkState_.clear();
  highlightedEdges_.clear();
  for (auto *item : std::as_const(routerItems_)) {
    item->setRuntimeState(std::nullopt);
  }
  for (auto *item : std::as_const(linkItems_)) {
    item->setRuntimeState(std::nullopt);
    item->setHighlighted(false);
  }
}

void TopologyScene::routerMoved(const QString &routerId,
                                const QPointF &position) {
  if (rebuilding_ || !topology_ || !topology_->routers.contains(routerId)) {
    return;
  }
  topology_->routers[routerId].position = position;
  updateConnectedLinks(routerId);
  rebuildAsGroups();
  emit topologyModified();
}

void TopologyScene::requestRouterEdit(const QString &routerId) {
  emit editRouterRequested(routerId);
}

void TopologyScene::requestLinkEdit(const QString &a, const QString &b) {
  emit editLinkRequested(a, b);
}

void TopologyScene::mousePressEvent(QGraphicsSceneMouseEvent *event) {
  if (editable_ && event->button() == Qt::LeftButton &&
      mode_ == Mode::AddRouter && !routerItemAt(event->scenePos())) {
    emit createRouterRequested(event->scenePos());
    event->accept();
    return;
  }
  if (editable_ && event->button() == Qt::LeftButton &&
      mode_ == Mode::AddLink) {
    if (auto *router = routerItemAt(event->scenePos())) {
      if (pendingLinkStart_.isEmpty()) {
        pendingLinkStart_ = router->routerId();
        router->setLinkStart(true);
      } else if (pendingLinkStart_ != router->routerId()) {
        const auto start = pendingLinkStart_;
        if (auto *old = routerItems_.value(start)) {
          old->setLinkStart(false);
        }
        pendingLinkStart_.clear();
        emit createLinkRequested(start, router->routerId());
      }
      event->accept();
      return;
    }
  }
  QGraphicsScene::mousePressEvent(event);
}

void TopologyScene::updateConnectedLinks(const QString &routerId) {
  for (auto *link : std::as_const(linkItems_)) {
    if (link->aId() == routerId || link->bId() == routerId) {
      link->updateGeometry();
    }
  }
}

void TopologyScene::rebuildAsGroups() {
  for (auto *item : std::as_const(asGroupItems_)) {
    removeItem(item);
    delete item;
  }
  asGroupItems_.clear();
  if (!topology_) {
    return;
  }
  QMap<quint32, QRectF> bounds;
  for (auto it = topology_->routers.cbegin(); it != topology_->routers.cend();
       ++it) {
    const QRectF routerRect(it->position - QPointF(65, 45), QSizeF(130, 90));
    bounds[it->asn] = bounds.contains(it->asn)
                          ? bounds.value(it->asn).united(routerRect)
                          : routerRect;
  }
  for (auto it = bounds.cbegin(); it != bounds.cend(); ++it) {
    const auto rect = it.value().adjusted(-25, -35, 25, 25);
    auto *frame = addRect(rect, QPen(asColor(it.key()).darker(180), 1.5,
                                    Qt::DashLine),
                          QBrush(asColor(it.key())));
    frame->setZValue(-10);
    auto *label = addSimpleText(QStringLiteral("AS %1").arg(it.key()));
    label->setBrush(asColor(it.key()).darker(230));
    label->setPos(rect.topLeft() + QPointF(8, 5));
    label->setZValue(-9);
    asGroupItems_.append(frame);
    asGroupItems_.append(label);
  }
}

void TopologyScene::updateSelectionContext() {
  QString routerId;
  QString linkA;
  QString linkB;
  const auto selected = selectedItems();
  if (selected.size() == 1) {
    auto *item = selected.front();
    if (item->type() == RouterGraphicsItem::Type) {
      routerId = static_cast<RouterGraphicsItem *>(item)->routerId();
    } else if (item->type() == LinkGraphicsItem::Type) {
      auto *link = static_cast<LinkGraphicsItem *>(item);
      linkA = link->aId();
      linkB = link->bId();
    }
  }
  emit selectionContextChanged(routerId, linkA, linkB);
}

RouterGraphicsItem *TopologyScene::routerItemAt(const QPointF &position) const {
  auto *item = itemAt(position, QTransform{});
  while (item && item->type() != RouterGraphicsItem::Type) {
    item = item->parentItem();
  }
  return item ? static_cast<RouterGraphicsItem *>(item) : nullptr;
}

TopologyView::TopologyView(TopologyScene *scene, QWidget *parent)
    : QGraphicsView(scene, parent) {
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                 QPainter::SmoothPixmapTransform);
  setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setResizeAnchor(QGraphicsView::AnchorViewCenter);
  setBackgroundBrush(QColor(QStringLiteral("#f4f7fb")));
  setDragMode(QGraphicsView::RubberBandDrag);
}

void TopologyView::wheelEvent(QWheelEvent *event) {
  const qreal factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
  const auto nextScale = transform().m11() * factor;
  if (nextScale >= 0.2 && nextScale <= 4.0) {
    scale(factor, factor);
  }
  event->accept();
}

void TopologyView::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::MiddleButton ||
      event->button() == Qt::RightButton) {
    panning_ = true;
    lastPanPoint_ = event->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }
  QGraphicsView::mousePressEvent(event);
}

void TopologyView::mouseMoveEvent(QMouseEvent *event) {
  if (panning_) {
    const auto delta = event->position().toPoint() - lastPanPoint_;
    lastPanPoint_ = event->position().toPoint();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    event->accept();
    return;
  }
  QGraphicsView::mouseMoveEvent(event);
}

void TopologyView::mouseReleaseEvent(QMouseEvent *event) {
  if (panning_ && (event->button() == Qt::MiddleButton ||
                   event->button() == Qt::RightButton)) {
    panning_ = false;
    unsetCursor();
    event->accept();
    return;
  }
  QGraphicsView::mouseReleaseEvent(event);
}

} // namespace bgptester
