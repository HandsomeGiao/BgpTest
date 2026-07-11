#include "engine/SimulationEngine.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QThread>

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace bgptester {
namespace {

bool primaryBetter(const RouteEntry &lhs, const RouteEntry &rhs) {
  if (lhs.localOrigin != rhs.localOrigin) {
    return lhs.localOrigin;
  }
  if (lhs.attributes.localPref != rhs.attributes.localPref) {
    return lhs.attributes.localPref > rhs.attributes.localPref;
  }
  if (lhs.attributes.asPath.size() != rhs.attributes.asPath.size()) {
    return lhs.attributes.asPath.size() < rhs.attributes.asPath.size();
  }
  if (lhs.attributes.med != rhs.attributes.med) {
    return lhs.attributes.med < rhs.attributes.med;
  }
  if (lhs.sourceSession != rhs.sourceSession) {
    return lhs.sourceSession == SessionType::Ebgp;
  }
  return false;
}

bool samePrimaryPreference(const RouteEntry &lhs, const RouteEntry &rhs) {
  return lhs.localOrigin == rhs.localOrigin &&
         lhs.attributes.localPref == rhs.attributes.localPref &&
         lhs.attributes.asPath.size() == rhs.attributes.asPath.size() &&
         lhs.attributes.med == rhs.attributes.med &&
         lhs.sourceSession == rhs.sourceSession;
}

bool deterministicBetter(const RouteEntry &lhs, const RouteEntry &rhs) {
  return std::tie(lhs.attributes.nextHop, lhs.learnedFrom) <
         std::tie(rhs.attributes.nextHop, rhs.learnedFrom);
}

bool advertisedRouteEqual(const RouteEntry &lhs, const RouteEntry &rhs) {
  return lhs.prefix == rhs.prefix && lhs.attributes == rhs.attributes;
}

bool validIpv4Prefix(const QString &prefix) {
  const auto slash = prefix.lastIndexOf(u'/');
  if (slash <= 0 || slash >= prefix.size() - 1) {
    return false;
  }
  bool lengthOk = false;
  const auto length = prefix.sliced(slash + 1).toInt(&lengthOk);
  QHostAddress address;
  return lengthOk && length >= 0 && length <= 32 &&
         address.setAddress(prefix.first(slash)) &&
         address.protocol() == QAbstractSocket::IPv4Protocol;
}

} // namespace

SimulationEngine::SimulationEngine(QObject *parent) : QObject(parent) {
  qRegisterMetaType<Topology>();
  qRegisterMetaType<SimulationEvent>();
  qRegisterMetaType<SimulationStats>();
  qRegisterMetaType<RibSnapshot>();
  qRegisterMetaType<BestPathUpdate>();
  qRegisterMetaType<QVector<RouterSnapshot>>();
  qRegisterMetaType<QVector<PeerSnapshot>>();

  eventTimer_ = new QTimer(this);
  eventTimer_->setSingleShot(true);
  eventTimer_->setTimerType(Qt::PreciseTimer);
  connect(eventTimer_, &QTimer::timeout, this,
          &SimulationEngine::processDueEvents);

  statusTimer_ = new QTimer(this);
  statusTimer_->setInterval(50);
  statusTimer_->setTimerType(Qt::CoarseTimer);
  connect(statusTimer_, &QTimer::timeout, this,
          &SimulationEngine::updateStatus);
}

qint64 SimulationEngine::now() const {
  return clock_.isValid() ? clock_.elapsed() : 0;
}

void SimulationEngine::startSimulation(Topology topology) {
  if (running_) {
    stopSimulation();
  }
  const auto problems = topology.validate();
  if (!problems.isEmpty()) {
    emit errorOccurred(QStringLiteral("无法启动仿真：\n%1")
                           .arg(problems.join(u'\n')));
    return;
  }

  topology_ = std::move(topology);
  clearRuntime();
  clock_.start();
  lastActivityAt_ = 0;
  running_ = true;
  converged_ = false;
  buildRuntime();

  emit runningChanged(true);
  emit convergenceChanged(false);
  recordTopologyEvent(QStringLiteral("simulation_started"),
                      {{QStringLiteral("name"), topology_.simulation.name},
                       {QStringLiteral("routers"),
                        QString::number(routers_.size())},
                       {QStringLiteral("links"),
                        QString::number(links_.size())}});

  for (auto routerIt = routers_.cbegin(); routerIt != routers_.cend();
       ++routerIt) {
    for (auto routeIt = routerIt->locRib.cbegin();
         routeIt != routerIt->locRib.cend(); ++routeIt) {
      emit bestPathChanged(BestPathUpdate{.router = routerIt.key(),
                                         .prefix = routeIt.key(),
                                         .valid = true,
                                         .route = routeIt.value()});
    }
  }

  for (auto it = links_.cbegin(); it != links_.cend(); ++it) {
    if (!it->enabled) {
      continue;
    }
    sendOpen(it->a, it->b);
    sendOpen(it->b, it->a);
  }

  statusTimer_->start();
  requestAllSnapshots();
  armNextEvent();
  publishStats();
}

void SimulationEngine::stopSimulation() {
  if (!running_) {
    return;
  }
  recordTopologyEvent(QStringLiteral("simulation_stopped"));
  running_ = false;
  converged_ = false;
  eventTimer_->stop();
  statusTimer_->stop();
  events_ = {};
  for (auto &router : routers_) {
    router.active = false;
    for (auto &peer : router.peers) {
      peer.state = PeerState::Idle;
      peer.pending.clear();
      peer.flushScheduled = false;
    }
  }
  requestAllSnapshots();
  publishStats();
  emit convergenceChanged(false);
  emit runningChanged(false);
}

void SimulationEngine::clearRuntime() {
  eventTimer_->stop();
  statusTimer_->stop();
  events_ = {};
  routers_.clear();
  links_.clear();
  nextSequence_ = 0;
  nextOrder_ = 0;
  nextGeneration_ = 0;
  deliveredMessages_ = 0;
}

void SimulationEngine::buildRuntime() {
  for (auto it = topology_.routers.cbegin(); it != topology_.routers.cend();
       ++it) {
    RouterRuntime runtime;
    runtime.config = it.value();
    runtime.active = true;
    for (const auto &neighbor : topology_.neighborsFor(it.key())) {
      PeerRuntime peer;
      peer.config = neighbor;
      peer.nextMraiAt = neighbor.mraiMs;
      runtime.peers.insert(neighbor.id, peer);
    }
    for (const auto &prefix : runtime.config.originatedPrefixes) {
      RouteEntry route;
      route.prefix = prefix;
      route.attributes.nextHop = runtime.config.routerId;
      route.learnedFrom = runtime.config.id;
      route.localOrigin = true;
      runtime.localRoutes.insert(prefix, route);
      runtime.locRib.insert(prefix, route);
    }
    routers_.insert(it.key(), runtime);
  }
  for (const auto &link : topology_.links) {
    links_.insert(Topology::edgeKey(link.a, link.b), link);
  }
}

void SimulationEngine::armNextEvent() {
  if (!running_ || events_.empty()) {
    eventTimer_->stop();
    return;
  }
  const auto delay = std::max<qint64>(0, events_.top().dueAt - now());
  eventTimer_->start(static_cast<int>(std::min<qint64>(
      delay, std::numeric_limits<int>::max())));
}

void SimulationEngine::markActivity() {
  lastActivityAt_ = now();
  if (converged_) {
    converged_ = false;
    emit convergenceChanged(false);
  }
}

void SimulationEngine::publishStats() {
  emit statsChanged(SimulationStats{
      .running = running_,
      .converged = converged_,
      .pendingEvents = static_cast<qsizetype>(events_.size()),
      .deliveredMessages = deliveredMessages_,
      .elapsedMs = now(),
  });
}

void SimulationEngine::scheduleMessages(const QString &from,
                                        const QString &to,
                                        QVector<BgpMessage> messages,
                                        int extraDelayMs) {
  if (!running_ || messages.isEmpty() || !messageDeliverable(from, to)) {
    return;
  }
  for (auto &message : messages) {
    message.from = from;
    message.to = to;
    message.sequence = ++nextSequence_;
  }
  events_.push(ScheduledEvent{
      .dueAt = now() + linkDelay(from, to) + std::max(0, extraDelayMs),
      .order = ++nextOrder_,
      .kind = ScheduledKind::DeliverMessages,
      .from = from,
      .to = to,
      .messages = std::move(messages),
  });
  markActivity();
  armNextEvent();
}

void SimulationEngine::scheduleMraiFlush(const QString &from,
                                         const QString &to, qint64 dueAt) {
  events_.push(ScheduledEvent{
      .dueAt = std::max(now(), dueAt),
      .order = ++nextOrder_,
      .kind = ScheduledKind::FlushMrai,
      .from = from,
      .to = to,
      .messages = {},
  });
  markActivity();
  armNextEvent();
}

void SimulationEngine::processDueEvents() {
  if (!running_) {
    return;
  }
  int processed = 0;
  constexpr int maxPerTurn = 10000;
  while (!events_.empty() && events_.top().dueAt <= now() &&
         processed < maxPerTurn) {
    auto event = events_.top();
    events_.pop();
    if (event.kind == ScheduledKind::DeliverMessages) {
      deliverMessages(event);
    } else {
      flushMrai(event.from, event.to);
    }
    ++processed;
  }
  armNextEvent();
  updateStatus();
}

void SimulationEngine::deliverMessages(const ScheduledEvent &event) {
  if (!messageDeliverable(event.from, event.to)) {
    return;
  }

  QVector<BgpMessage> updates;
  for (const auto &message : event.messages) {
    if (message.guarded && !generationIsCurrent(message)) {
      continue;
    }
    if (message.guarded) {
      commitOutbound(message);
    }
    recordMessage(message);
    ++deliveredMessages_;
    markActivity();
    switch (message.type) {
    case MessageType::Open:
      handleOpen(message);
      break;
    case MessageType::Update:
      updates.append(message);
      break;
    case MessageType::Notification:
      handleNotification(message);
      break;
    }
  }
  if (!updates.isEmpty()) {
    handleUpdateBatch(event.to, event.from, updates);
  }
}

void SimulationEngine::flushMrai(const QString &from, const QString &to) {
  auto routerIt = routers_.find(from);
  if (routerIt == routers_.end()) {
    return;
  }
  auto peerIt = routerIt->peers.find(to);
  if (peerIt == routerIt->peers.end()) {
    return;
  }
  peerIt->flushScheduled = false;
  if (!routerIt->active || peerIt->state != PeerState::Established ||
      !messageDeliverable(from, to)) {
    peerIt->pending.clear();
    return;
  }

  QVector<BgpMessage> messages;
  for (const auto &pending : std::as_const(peerIt->pending)) {
    const auto current =
        routerIt->outboundGenerations.value(to).value(pending.prefix);
    if (current != pending.generation || !pending.route.has_value()) {
      continue;
    }
    messages.append(makeUpdateMessage(from, to, pending.prefix,
                                      pending.route, pending.generation));
  }
  peerIt->pending.clear();
  if (messages.isEmpty()) {
    peerIt->nextMraiAt = now();
    return;
  }
  peerIt->nextMraiAt = now() + peerIt->config.mraiMs;
  scheduleMessages(from, to, std::move(messages));
}

bool SimulationEngine::messageDeliverable(const QString &from,
                                          const QString &to) const {
  const auto fromIt = routers_.constFind(from);
  const auto toIt = routers_.constFind(to);
  if (fromIt == routers_.cend() || toIt == routers_.cend() ||
      !fromIt->active || !toIt->active) {
    return false;
  }
  const auto linkIt = links_.constFind(Topology::edgeKey(from, to));
  if (linkIt == links_.cend() || !linkIt->enabled) {
    return false;
  }
  const auto peerIt = fromIt->peers.constFind(to);
  return peerIt != fromIt->peers.cend() && peerIt->config.enabled;
}

int SimulationEngine::linkDelay(const QString &from, const QString &to) const {
  const auto it = links_.constFind(Topology::edgeKey(from, to));
  return it == links_.cend() ? 0 : std::max(0, it->delayMs);
}

bool SimulationEngine::generationIsCurrent(const BgpMessage &message) const {
  const auto routerIt = routers_.constFind(message.from);
  if (routerIt == routers_.cend()) {
    return false;
  }
  const auto peerGenerations =
      routerIt->outboundGenerations.value(message.to);
  for (auto it = message.generations.cbegin();
       it != message.generations.cend(); ++it) {
    if (peerGenerations.value(it.key()) != it.value()) {
      return false;
    }
  }
  return true;
}

void SimulationEngine::commitOutbound(const BgpMessage &message) {
  auto routerIt = routers_.find(message.from);
  if (routerIt == routers_.end()) {
    return;
  }
  auto &out = routerIt->adjRibOut[message.to];
  for (const auto &prefix : message.withdrawn) {
    out.remove(prefix);
  }
  for (const auto &prefix : message.nlri) {
    RouteEntry route = message.advertisedRoute.value_or(RouteEntry{});
    route.prefix = prefix;
    route.attributes = message.attributes;
    out.insert(prefix, route);
  }
}

void SimulationEngine::sendOpen(const QString &from, const QString &to) {
  auto fromIt = routers_.find(from);
  if (fromIt == routers_.end() || !messageDeliverable(from, to)) {
    return;
  }
  auto peerIt = fromIt->peers.find(to);
  if (peerIt == fromIt->peers.end()) {
    return;
  }
  if (peerIt->state != PeerState::Established) {
    peerIt->state = PeerState::OpenSent;
  }
  BgpMessage message;
  message.type = MessageType::Open;
  message.openAsn = fromIt->config.asn;
  message.openRouterId = fromIt->config.routerId;
  scheduleMessages(from, to, {message});
}

void SimulationEngine::handleOpen(const BgpMessage &message) {
  auto receiverIt = routers_.find(message.to);
  if (receiverIt == routers_.end()) {
    return;
  }
  auto peerIt = receiverIt->peers.find(message.from);
  if (peerIt == receiverIt->peers.end()) {
    return;
  }
  if (message.openAsn != peerIt->config.remoteAsn) {
    BgpMessage notification;
    notification.type = MessageType::Notification;
    notification.errorCode = 2;
    notification.errorSubcode = 2;
    notification.errorData = QStringLiteral("Bad Peer AS");
    scheduleMessages(message.to, message.from, {notification});
    peerIt->state = PeerState::Idle;
    return;
  }

  const auto wasEstablished = peerIt->state == PeerState::Established;
  if (peerIt->state == PeerState::Idle) {
    sendOpen(message.to, message.from);
  }
  peerIt->state = PeerState::Established;
  if (!wasEstablished) {
    const auto routes = receiverIt->locRib;
    for (auto routeIt = routes.cbegin(); routeIt != routes.cend(); ++routeIt) {
      disseminate(message.to, routeIt.key(), routeIt.value());
    }
  }
  emit peersSnapshotReady(message.to, peerSnapshots(message.to));
}

void SimulationEngine::handleUpdateBatch(
    const QString &receiver, const QString &sender,
    const QVector<BgpMessage> &messages) {
  auto routerIt = routers_.find(receiver);
  if (routerIt == routers_.end()) {
    return;
  }
  const auto peerIt = routerIt->peers.constFind(sender);
  if (peerIt == routerIt->peers.cend() ||
      peerIt->state != PeerState::Established) {
    return;
  }

  QSet<QString> changed;
  auto &peerRoutes = routerIt->adjRibIn[sender];
  for (const auto &message : messages) {
    for (const auto &prefix : message.withdrawn) {
      if (peerRoutes.remove(prefix) > 0) {
        changed.insert(prefix);
      }
    }
    for (const auto &prefix : message.nlri) {
      bool rejected = message.attributes.asPath.contains(routerIt->config.asn);
      rejected = rejected ||
                 (!message.attributes.originatorId.isEmpty() &&
                  message.attributes.originatorId == routerIt->config.routerId);
      const auto clusterId = routerIt->config.clusterId.isEmpty()
                                 ? routerIt->config.routerId
                                 : routerIt->config.clusterId;
      rejected = rejected || message.attributes.clusterList.contains(clusterId);
      if (rejected) {
        if (peerRoutes.remove(prefix) > 0) {
          changed.insert(prefix);
        }
        continue;
      }
      RouteEntry route;
      route.prefix = prefix;
      route.attributes = message.attributes;
      route.learnedFrom = sender;
      route.sourceSession = peerIt->config.sessionType;
      route.localOrigin = false;
      const auto old = peerRoutes.constFind(prefix);
      if (old == peerRoutes.cend() || old.value() != route) {
        peerRoutes.insert(prefix, route);
        changed.insert(prefix);
      }
    }
  }
  runDecision(receiver, changed);
}

void SimulationEngine::handleNotification(const BgpMessage &message) {
  neighborDown(message.to, message.from);
  emit peersSnapshotReady(message.to, peerSnapshots(message.to));
}

void SimulationEngine::neighborDown(const QString &routerId,
                                    const QString &peerId) {
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end()) {
    return;
  }
  auto peerIt = routerIt->peers.find(peerId);
  if (peerIt == routerIt->peers.end()) {
    return;
  }
  peerIt->state = PeerState::Idle;
  peerIt->pending.clear();
  peerIt->flushScheduled = false;
  auto &generations = routerIt->outboundGenerations[peerId];
  for (auto generationIt = generations.begin();
       generationIt != generations.end(); ++generationIt) {
    generationIt.value() = ++nextGeneration_;
  }
  routerIt->adjRibOut.remove(peerId);
  QSet<QString> affected;
  const auto inbound = routerIt->adjRibIn.take(peerId);
  for (auto it = inbound.cbegin(); it != inbound.cend(); ++it) {
    affected.insert(it.key());
  }
  if (routerIt->active) {
    runDecision(routerId, affected);
  }
}

void SimulationEngine::runDecision(const QString &routerId,
                                   const QSet<QString> &changedPrefixes) {
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end() || !routerIt->active ||
      changedPrefixes.isEmpty()) {
    return;
  }

  bool anyChange = false;
  for (const auto &prefix : changedPrefixes) {
    const auto oldIt = routerIt->locRib.constFind(prefix);
    const std::optional<RouteEntry> old =
        oldIt == routerIt->locRib.cend()
            ? std::nullopt
            : std::optional<RouteEntry>(oldIt.value());
    const auto selected = selectBest(*routerIt, prefix);
    if (old == selected) {
      continue;
    }
    anyChange = true;
    if (selected) {
      routerIt->locRib.insert(prefix, *selected);
    } else {
      routerIt->locRib.remove(prefix);
    }
    emit bestPathChanged(BestPathUpdate{.router = routerId,
                                       .prefix = prefix,
                                       .valid = selected.has_value(),
                                       .route = selected});
    disseminate(routerId, prefix, selected);
  }
  if (anyChange) {
    emit ribSnapshotReady(ribSnapshot(routerId));
    emit routerSnapshotsChanged(routerSnapshots());
  }
}

std::optional<RouteEntry>
SimulationEngine::selectBest(const RouterRuntime &router,
                             const QString &prefix) const {
  QVector<RouteEntry> candidates;
  if (const auto local = router.localRoutes.constFind(prefix);
      local != router.localRoutes.cend()) {
    candidates.append(local.value());
  }
  for (const auto &peerRoutes : router.adjRibIn) {
    if (const auto route = peerRoutes.constFind(prefix);
        route != peerRoutes.cend()) {
      candidates.append(route.value());
    }
  }
  if (candidates.isEmpty()) {
    return std::nullopt;
  }

  RouteEntry primaryBest = candidates.front();
  for (const auto &candidate : candidates) {
    if (primaryBetter(candidate, primaryBest)) {
      primaryBest = candidate;
    }
  }

  if (const auto old = router.locRib.constFind(prefix);
      old != router.locRib.cend() &&
      samePrimaryPreference(old.value(), primaryBest)) {
    const auto stillPresent =
        std::find(candidates.cbegin(), candidates.cend(), old.value());
    if (stillPresent != candidates.cend()) {
      return old.value();
    }
  }

  RouteEntry result = primaryBest;
  for (const auto &candidate : candidates) {
    if (samePrimaryPreference(candidate, primaryBest) &&
        deterministicBetter(candidate, result)) {
      result = candidate;
    }
  }
  return result;
}

bool SimulationEngine::exportAllowed(const RouterRuntime &router,
                                     const RouteEntry &route,
                                     const PeerRuntime &peer) const {
  if (route.learnedFrom == peer.config.id) {
    return false;
  }
  if (peer.config.sessionType == SessionType::Ebgp) {
    return true;
  }
  if (route.localOrigin || route.sourceSession == SessionType::Ebgp) {
    return true;
  }
  if (!hasRouteReflectorClients(router)) {
    return false;
  }
  const auto learnedPeer = router.peers.constFind(route.learnedFrom);
  const auto learnedFromClient =
      learnedPeer != router.peers.cend() && learnedPeer->config.rrClient;
  return learnedFromClient || peer.config.rrClient;
}

RouteEntry SimulationEngine::transformForPeer(const RouterRuntime &router,
                                              const RouteEntry &route,
                                              const PeerRuntime &peer) const {
  RouteEntry result = route;
  result.learnedFrom = router.config.id;
  result.localOrigin = false;
  result.sourceSession = peer.config.sessionType;
  if (peer.config.sessionType == SessionType::Ebgp) {
    result.attributes.asPath.prepend(router.config.asn);
    result.attributes.nextHop = router.config.routerId;
    result.attributes.originatorId.clear();
    result.attributes.clusterList.clear();
    return result;
  }

  if (result.attributes.nextHop.isEmpty()) {
    result.attributes.nextHop = router.config.routerId;
  }
  if (route.sourceSession == SessionType::Ibgp && !route.localOrigin &&
      hasRouteReflectorClients(router)) {
    if (result.attributes.originatorId.isEmpty()) {
      const auto learned = routers_.constFind(route.learnedFrom);
      result.attributes.originatorId =
          learned == routers_.cend() ? route.attributes.nextHop
                                     : learned->config.routerId;
    }
    const auto clusterId = router.config.clusterId.isEmpty()
                               ? router.config.routerId
                               : router.config.clusterId;
    if (!result.attributes.clusterList.contains(clusterId)) {
      result.attributes.clusterList.append(clusterId);
    }
  }
  return result;
}

void SimulationEngine::disseminate(const QString &routerId,
                                   const QString &prefix,
                                   const std::optional<RouteEntry> &route) {
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end() || !routerIt->active) {
    return;
  }

  const auto peerIds = routerIt->peers.keys();
  for (const auto &peerId : peerIds) {
    auto peerIt = routerIt->peers.find(peerId);
    if (peerIt == routerIt->peers.end() ||
        peerIt->state != PeerState::Established || !peerIt->config.enabled) {
      continue;
    }
    std::optional<RouteEntry> outbound;
    if (route && exportAllowed(*routerIt, *route, *peerIt)) {
      outbound = transformForPeer(*routerIt, *route, *peerIt);
      if (const auto outboundRoutes = routerIt->adjRibOut.constFind(peerId);
          outboundRoutes != routerIt->adjRibOut.cend()) {
        const auto existing = outboundRoutes->constFind(prefix);
        if (existing != outboundRoutes->cend() &&
            advertisedRouteEqual(existing.value(), *outbound)) {
          continue;
        }
      }
    }
    queueAdvertisement(routerId, peerId, prefix, outbound);
  }
}

void SimulationEngine::queueAdvertisement(
    const QString &routerId, const QString &peerId, const QString &prefix,
    const std::optional<RouteEntry> &route) {
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end()) {
    return;
  }
  auto peerIt = routerIt->peers.find(peerId);
  if (peerIt == routerIt->peers.end()) {
    return;
  }
  const auto generation = ++nextGeneration_;
  routerIt->outboundGenerations[peerId][prefix] = generation;

  if (!route) {
    const auto hadPending = peerIt->pending.remove(prefix) > 0;
    const auto hadDelivered =
        routerIt->adjRibOut.value(peerId).contains(prefix);
    if (!hadDelivered) {
      Q_UNUSED(hadPending);
      return;
    }
    scheduleMessages(routerId, peerId,
                     {makeUpdateMessage(routerId, peerId, prefix,
                                        std::nullopt, generation)});
    return;
  }

  if (peerIt->config.mraiMs <= 0) {
    scheduleMessages(routerId, peerId,
                     {makeUpdateMessage(routerId, peerId, prefix, route,
                                        generation)});
    return;
  }

  peerIt->pending.insert(prefix, PendingUpdate{.prefix = prefix,
                                               .route = route,
                                               .generation = generation});
  if (!peerIt->flushScheduled) {
    peerIt->flushScheduled = true;
    scheduleMraiFlush(routerId, peerId,
                      std::max(now(), peerIt->nextMraiAt));
  }
}

BgpMessage SimulationEngine::makeUpdateMessage(
    const QString &from, const QString &to, const QString &prefix,
    const std::optional<RouteEntry> &route, quint64 generation) const {
  BgpMessage message;
  message.type = MessageType::Update;
  message.from = from;
  message.to = to;
  message.guarded = true;
  message.generations.insert(prefix, generation);
  if (route) {
    message.nlri.append(prefix);
    message.attributes = route->attributes;
    message.advertisedRoute = route;
  } else {
    message.withdrawn.append(prefix);
  }
  return message;
}

void SimulationEngine::setLinkState(const QString &a, const QString &b,
                                    bool enabled) {
  if (!running_) {
    emit errorOccurred(QStringLiteral("仿真尚未运行"));
    return;
  }
  auto linkIt = links_.find(Topology::edgeKey(a, b));
  if (linkIt == links_.end()) {
    emit errorOccurred(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
    return;
  }
  if (linkIt->enabled == enabled) {
    return;
  }
  linkIt->enabled = enabled;
  if (auto *configLink = topology_.findLink(a, b)) {
    configLink->enabled = enabled;
  }
  if (auto router = routers_.find(a); router != routers_.end()) {
    if (auto peer = router->peers.find(b); peer != router->peers.end()) {
      peer->config.enabled = enabled;
    }
  }
  if (auto router = routers_.find(b); router != routers_.end()) {
    if (auto peer = router->peers.find(a); peer != router->peers.end()) {
      peer->config.enabled = enabled;
    }
  }
  markActivity();
  if (enabled) {
    sendOpen(a, b);
    sendOpen(b, a);
  } else {
    neighborDown(a, b);
    neighborDown(b, a);
  }
  recordTopologyEvent(enabled ? QStringLiteral("link_up")
                              : QStringLiteral("link_down"),
                      {{QStringLiteral("a"), a}, {QStringLiteral("b"), b}});
  emit linkStateChanged(a, b, enabled);
  requestAllSnapshots();
}

void SimulationEngine::setRouterState(const QString &routerId, bool enabled) {
  if (!running_) {
    emit errorOccurred(QStringLiteral("仿真尚未运行"));
    return;
  }
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end()) {
    emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
    return;
  }
  if (routerIt->active == enabled) {
    return;
  }
  markActivity();
  if (!enabled) {
    routerIt->active = false;
    const auto oldPrefixes = routerIt->locRib.keys();
    const auto peers = routerIt->peers.keys();
    for (const auto &peerId : peers) {
      neighborDown(peerId, routerId);
    }
    routerIt->localRoutes.clear();
    routerIt->locRib.clear();
    routerIt->adjRibIn.clear();
    routerIt->adjRibOut.clear();
    for (auto &peer : routerIt->peers) {
      peer.state = PeerState::Idle;
      peer.pending.clear();
      peer.flushScheduled = false;
    }
    for (const auto &prefix : oldPrefixes) {
      emit bestPathChanged(BestPathUpdate{.router = routerId,
                                         .prefix = prefix,
                                         .valid = false,
                                         .route = std::nullopt});
    }
  } else {
    routerIt->active = true;
    QSet<QString> localPrefixes;
    for (const auto &prefix : routerIt->config.originatedPrefixes) {
      RouteEntry route;
      route.prefix = prefix;
      route.attributes.nextHop = routerIt->config.routerId;
      route.learnedFrom = routerId;
      route.localOrigin = true;
      routerIt->localRoutes.insert(prefix, route);
      localPrefixes.insert(prefix);
    }
    runDecision(routerId, localPrefixes);
    for (const auto &peerId : routerIt->peers.keys()) {
      if (messageDeliverable(routerId, peerId)) {
        sendOpen(routerId, peerId);
        sendOpen(peerId, routerId);
      }
    }
  }
  recordTopologyEvent(enabled ? QStringLiteral("router_up")
                              : QStringLiteral("router_down"),
                      {{QStringLiteral("router"), routerId}});
  emit routerStateChanged(routerId, enabled);
  requestAllSnapshots();
}

void SimulationEngine::originatePrefix(const QString &routerId,
                                       const QString &prefixValue) {
  const auto prefix = prefixValue.trimmed();
  if (!running_) {
    emit errorOccurred(QStringLiteral("仿真尚未运行"));
    return;
  }
  if (!validIpv4Prefix(prefix)) {
    emit errorOccurred(QStringLiteral("前缀无效：%1").arg(prefix));
    return;
  }
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end()) {
    emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
    return;
  }
  if (!routerIt->config.originatedPrefixes.contains(prefix)) {
    routerIt->config.originatedPrefixes.append(prefix);
    topology_.routers[routerId].originatedPrefixes.append(prefix);
  }
  if (!routerIt->active || routerIt->localRoutes.contains(prefix)) {
    return;
  }
  RouteEntry route;
  route.prefix = prefix;
  route.attributes.nextHop = routerIt->config.routerId;
  route.learnedFrom = routerId;
  route.localOrigin = true;
  routerIt->localRoutes.insert(prefix, route);
  markActivity();
  runDecision(routerId, {prefix});
  recordTopologyEvent(QStringLiteral("prefix_advertised"),
                      {{QStringLiteral("router"), routerId},
                       {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::withdrawPrefix(const QString &routerId,
                                      const QString &prefixValue) {
  const auto prefix = prefixValue.trimmed();
  if (!running_) {
    emit errorOccurred(QStringLiteral("仿真尚未运行"));
    return;
  }
  auto routerIt = routers_.find(routerId);
  if (routerIt == routers_.end()) {
    emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
    return;
  }
  routerIt->config.originatedPrefixes.removeAll(prefix);
  topology_.routers[routerId].originatedPrefixes.removeAll(prefix);
  if (routerIt->localRoutes.remove(prefix) == 0) {
    return;
  }
  markActivity();
  runDecision(routerId, {prefix});
  recordTopologyEvent(QStringLiteral("prefix_withdrawn"),
                      {{QStringLiteral("router"), routerId},
                       {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::updateStatus() {
  if (!running_) {
    return;
  }
  const auto quietMs =
      std::max(0, topology_.simulation.convergenceQuietMs);
  const auto isNowConverged = events_.empty() && now() - lastActivityAt_ >= quietMs;
  if (isNowConverged != converged_) {
    converged_ = isNowConverged;
    emit convergenceChanged(converged_);
    if (converged_) {
      recordTopologyEvent(QStringLiteral("converged"),
                          {{QStringLiteral("elapsed_ms"),
                            QString::number(now())}});
    }
  }
  publishStats();
}

RibSnapshot SimulationEngine::ribSnapshot(const QString &routerId) const {
  RibSnapshot snapshot;
  snapshot.router = routerId;
  const auto routerIt = routers_.constFind(routerId);
  if (routerIt == routers_.cend()) {
    return snapshot;
  }
  snapshot.localRoutes = routerIt->localRoutes;
  snapshot.locRib = routerIt->locRib;
  snapshot.adjRibIn = routerIt->adjRibIn;
  snapshot.adjRibOut = routerIt->adjRibOut;
  return snapshot;
}

QVector<PeerSnapshot>
SimulationEngine::peerSnapshots(const QString &routerId) const {
  QVector<PeerSnapshot> snapshots;
  const auto routerIt = routers_.constFind(routerId);
  if (routerIt == routers_.cend()) {
    return snapshots;
  }
  for (auto it = routerIt->peers.cbegin(); it != routerIt->peers.cend(); ++it) {
    snapshots.append(PeerSnapshot{
        .id = it.key(),
        .remoteAsn = it->config.remoteAsn,
        .sessionType = it->config.sessionType,
        .rrClient = it->config.rrClient,
        .enabled = it->config.enabled,
        .mraiMs = it->config.mraiMs,
        .state = it->state,
    });
  }
  return snapshots;
}

QVector<RouterSnapshot> SimulationEngine::routerSnapshots() const {
  QVector<RouterSnapshot> snapshots;
  for (auto it = routers_.cbegin(); it != routers_.cend(); ++it) {
    snapshots.append(RouterSnapshot{
        .id = it.key(),
        .routerId = it->config.routerId,
        .asn = it->config.asn,
        .active = it->active,
        .routeReflector = hasRouteReflectorClients(it.value()),
        .bestRouteCount = static_cast<int>(it->locRib.size()),
    });
  }
  return snapshots;
}

BestPathUpdate SimulationEngine::bestPath(const QString &routerId,
                                          const QString &prefix) const {
  BestPathUpdate update{.router = routerId,
                        .prefix = prefix,
                        .valid = false,
                        .route = std::nullopt};
  const auto routerIt = routers_.constFind(routerId);
  if (routerIt == routers_.cend()) {
    return update;
  }
  const auto routeIt = routerIt->locRib.constFind(prefix);
  if (routeIt != routerIt->locRib.cend()) {
    update.valid = true;
    update.route = routeIt.value();
  }
  return update;
}

void SimulationEngine::requestRouterSnapshot(const QString &routerId) {
  emit ribSnapshotReady(ribSnapshot(routerId));
  emit peersSnapshotReady(routerId, peerSnapshots(routerId));
}

void SimulationEngine::requestAllSnapshots() {
  emit routerSnapshotsChanged(routerSnapshots());
}

void SimulationEngine::recordMessage(const BgpMessage &message) {
  SimulationEvent event;
  event.timestamp = QDateTime::currentDateTime();
  event.event = QStringLiteral("message_received");
  event.router = message.to;
  event.from = message.from;
  event.to = message.to;
  event.messageType = toString(message.type);
  event.sequence = message.sequence;
  event.prefixes = message.nlri;
  event.withdrawn = message.withdrawn;
  const auto fromIt = routers_.constFind(message.from);
  const auto toIt = routers_.constFind(message.to);
  if (fromIt != routers_.cend()) {
    event.fromAs = fromIt->config.asn;
  }
  if (toIt != routers_.cend()) {
    event.toAs = toIt->config.asn;
  }
  if (message.type == MessageType::Update) {
    event.action = message.nlri.isEmpty() && !message.withdrawn.isEmpty()
                       ? QStringLiteral("WITHDRAW")
                       : QStringLiteral("UPDATE");
    event.nextHop = message.attributes.nextHop;
    event.asPath = message.attributes.asPath;
    event.localPref = message.attributes.localPref;
    event.med = message.attributes.med;
  } else {
    event.action = toString(message.type);
  }
  emit eventGenerated(event);
}

void SimulationEngine::recordTopologyEvent(
    const QString &name, QMap<QString, QString> details) {
  SimulationEvent event;
  event.timestamp = QDateTime::currentDateTime();
  event.event = name;
  event.action = QStringLiteral("TOPOLOGY");
  event.details = std::move(details);
  event.router = event.details.value(QStringLiteral("router"));
  event.from = event.details.value(QStringLiteral("a"));
  event.to = event.details.value(QStringLiteral("b"));
  emit eventGenerated(event);
}

bool SimulationEngine::hasRouteReflectorClients(
    const RouterRuntime &router) const {
  return std::any_of(router.peers.cbegin(), router.peers.cend(),
                     [](const auto &peer) { return peer.config.rrClient; });
}

} // namespace bgptester
