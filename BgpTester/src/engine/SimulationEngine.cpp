#include "engine/SimulationEngine.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QThread>

#include <algorithm>
#include <limits>
#include <utility>

namespace bgptester
{
namespace
{

QPair<quint64, quint64> advertisedRouteFingerprint(const RouteEntry& route)
{
    quint64 first = 0x9e3779b97f4a7c15ULL;
    quint64 second = 0xc2b2ae3d27d4eb4fULL;
    const auto mix = [](quint64 state, quint64 value)
    {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return state ^ ((value ^ (value >> 31U)) + (state << 6U) + (state >> 2U));
    };
    const auto add = [&](const auto& value)
    {
        first = mix(first, static_cast<quint64>(qHash(value, static_cast<size_t>(first))));
        second = mix(second, static_cast<quint64>(qHash(value, static_cast<size_t>(second))));
    };

    add(route.attributes.origin);
    add(route.attributes.nextHop);
    add(route.attributes.localPref);
    add(route.attributes.med);
    add(static_cast<int>(route.source));
    add(route.attributes.originatorId);
    for (const auto asn : route.attributes.asPath)
    {
        add(asn);
    }
    for (const auto& cluster : route.attributes.clusterList)
    {
        add(cluster);
    }
    for (auto it = route.attributes.communities.cbegin(); it != route.attributes.communities.cend(); ++it)
    {
        add(it.key());
        add(it.value());
    }
    if (route.attributes.tfpVersionInfo)
    {
        add(QStringLiteral("tfp-version-info"));
        add(QStringLiteral("dependency-vector"));
        for (auto it = route.attributes.tfpVersionInfo->dependencyVector.cbegin();
             it != route.attributes.tfpVersionInfo->dependencyVector.cend(); ++it)
        {
            add(it.key().asn);
            add(it.key().entityId);
            add(it.value());
        }
        add(QStringLiteral("trigger-vector"));
        for (auto it = route.attributes.tfpVersionInfo->triggerVector.cbegin(); it != route.attributes.tfpVersionInfo->triggerVector.cend();
             ++it)
        {
            add(it.key().asn);
            add(it.key().entityId);
            add(it.value());
        }
    }
    return {first, second};
}

bool advertisementTemplateEqual(const RouteEntry& lhs, const RouteEntry& rhs)
{
    return lhs.attributes == rhs.attributes && lhs.learnedFrom == rhs.learnedFrom && lhs.sourceSession == rhs.sourceSession &&
           lhs.localOrigin == rhs.localOrigin && lhs.source == rhs.source;
}

bool validIpv4Prefix(const QString& prefix)
{
    const auto slash = prefix.lastIndexOf(u'/');
    if (slash <= 0 || slash >= prefix.size() - 1)
    {
        return false;
    }
    bool lengthOk = false;
    const auto length = prefix.sliced(slash + 1).toInt(&lengthOk);
    QHostAddress address;
    return lengthOk && length >= 0 && length <= 32 && address.setAddress(prefix.first(slash)) &&
           address.protocol() == QAbstractSocket::IPv4Protocol;
}

QString tfpVectorText(const TfpVersionVector& vector)
{
    QStringList entries;
    entries.reserve(vector.size());
    for (auto it = vector.cbegin(); it != vector.cend(); ++it)
    {
        entries.append(QStringLiteral("(%1,%2)=%3").arg(it.key().asn).arg(it.key().entityId).arg(it.value()));
    }
    return entries.join(u',');
}

} // namespace

SimulationEngine::SimulationEngine(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<Topology>();
    qRegisterMetaType<SimulationEvent>();
    qRegisterMetaType<QVector<SimulationEvent>>();
    qRegisterMetaType<SimulationStats>();
    qRegisterMetaType<RibSnapshot>();
    qRegisterMetaType<QVector<RouterSnapshot>>();
    qRegisterMetaType<QVector<PeerSnapshot>>();

    eventTimer_ = new QTimer(this);
    eventTimer_->setSingleShot(true);
    eventTimer_->setTimerType(Qt::PreciseTimer);
    connect(eventTimer_, &QTimer::timeout, this, &SimulationEngine::processDueEvents);

    statusTimer_ = new QTimer(this);
    statusTimer_->setInterval(50);
    statusTimer_->setTimerType(Qt::CoarseTimer);
    connect(statusTimer_, &QTimer::timeout, this, &SimulationEngine::updateStatus);
}

qint64 SimulationEngine::now() const
{
    return clock_.isValid() ? clock_.elapsed() : 0;
}

void SimulationEngine::prepareStartup() noexcept
{
    startupCancelRequested_.store(false, std::memory_order_release);
}

void SimulationEngine::requestStartupCancellation() noexcept
{
    startupCancelRequested_.store(true, std::memory_order_release);
}

void SimulationEngine::startSimulation(Topology topology)
{
    if (running_)
    {
        stopSimulation();
    }
    clearRuntime();
    simulation_ = topology.simulation;
    emit startupProgress(QStringLiteral("正在校验拓扑"), 0, topology.routers.size() + topology.links.size());
    const auto problems = topology.validate();
    if (!problems.isEmpty())
    {
        recordTopologyEvent(QStringLiteral("simulation_start_failed"), {{QStringLiteral("error"), problems.join(u'\n')}});
        clearRuntime();
        emit errorOccurred(QStringLiteral("无法启动仿真：\n%1").arg(problems.join(u'\n')));
        return;
    }
    if (startupCancelRequested_.load(std::memory_order_acquire))
    {
        recordTopologyEvent(QStringLiteral("simulation_start_cancelled"));
        clearRuntime();
        emit startupCancelled();
        return;
    }
    recordTopologyEvent(QStringLiteral("simulation_initializing"),
                        {{QStringLiteral("name"), simulation_.name},
                         {QStringLiteral("routers"), QString::number(topology.routers.size())},
                         {QStringLiteral("links"), QString::number(topology.links.size())}});
    QString pluginError;
    if (!buildRuntime(topology, &pluginError))
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            recordTopologyEvent(QStringLiteral("simulation_start_cancelled"));
            clearRuntime();
            emit startupCancelled();
            return;
        }
        recordTopologyEvent(QStringLiteral("simulation_start_failed"), {{QStringLiteral("error"), pluginError}});
        clearRuntime();
        emit errorOccurred(QStringLiteral("无法启动仿真：\n%1").arg(pluginError));
        return;
    }
    clock_.start();
    lastActivityAt_ = 0;
    convergenceStartedAt_ = 0;
    convergenceSequence_ = 0;
    convergenceMessageCount_ = 0;
    convergenceTriggerEvent_ = QStringLiteral("simulation_started");
    convergenceTriggerContext_ = simulation_.name;
    converged_ = false;
    for (auto& router : routers_)
    {
        router.node->simulationStarted();
        router.node->routerStateChanged(true);
    }
    pluginLifecycleActive_ = true;
    if (startupCancelRequested_.load(std::memory_order_acquire))
    {
        recordTopologyEvent(QStringLiteral("simulation_start_cancelled"));
        clearRuntime();
        emit startupCancelled();
        return;
    }

    emit startupProgress(QStringLiteral("正在建立 BGP 会话"), 0, links_.size());
    if (!scheduleInitialOpenMessages())
    {
        recordTopologyEvent(QStringLiteral("simulation_start_cancelled"));
        clearRuntime();
        emit startupCancelled();
        return;
    }
    if (startupCancelRequested_.load(std::memory_order_acquire))
    {
        recordTopologyEvent(QStringLiteral("simulation_start_cancelled"));
        clearRuntime();
        emit startupCancelled();
        return;
    }

    running_ = true;
    emit runningChanged(true);
    emit convergenceChanged(false);
    recordTopologyEvent(QStringLiteral("simulation_started"), {{QStringLiteral("name"), simulation_.name},
                                                               {QStringLiteral("routers"), QString::number(routers_.size())},
                                                               {QStringLiteral("links"), QString::number(links_.size())}});

    statusTimer_->start();
    requestAllSnapshots();
    armNextEvent();
    publishStats();
}

void SimulationEngine::stopSimulation()
{
    if (!running_)
    {
        return;
    }
    recordTopologyEvent(QStringLiteral("simulation_stopped"));
    running_ = false;
    converged_ = false;
    eventTimer_->stop();
    statusTimer_->stop();
    events_ = {};
    for (auto& router : routers_)
    {
        router.active = false;
        router.node->routerStateChanged(false);
        for (auto& peer : router.peers)
        {
            peer.state = PeerState::Idle;
            router.node->peerStateChanged(peer.config, peer.state);
            clearPendingUpdates(peer);
            peer.flushScheduled = false;
            peer.withdrawalFlushScheduled = false;
        }
        router.node->simulationStopped();
    }
    pluginLifecycleActive_ = false;
    requestAllSnapshots();
    publishStats();
    emit convergenceChanged(false);
    emit runningChanged(false);
}

void SimulationEngine::clearRuntime()
{
    eventTimer_->stop();
    statusTimer_->stop();
    activeEventBatch_ = nullptr;
    events_ = {};
    if (pluginLifecycleActive_)
    {
        for (auto& router : routers_)
        {
            router.node->simulationStopped();
        }
        pluginLifecycleActive_ = false;
    }
    for (auto& router : routers_)
    {
        delete router.node;
        router.node = nullptr;
    }
    routers_.clear();
    links_.clear();
    simulation_ = {};
    nextSequence_ = 0;
    nextOrder_ = 0;
    nextGeneration_ = 0;
    deliveredMessages_ = 0;
    convergenceMessageCount_ = 0;
    routingStateDirty_ = false;
    convergenceTriggerEvent_.clear();
    convergenceTriggerContext_.clear();
}

bool SimulationEngine::buildRuntime(const Topology& topology, QString* error)
{
    if (error)
    {
        error->clear();
    }
    emit startupProgress(QStringLiteral("正在构建邻接索引"), 0, topology.links.size());
    const auto neighborIndex = topology.buildNeighborIndex();
    if (startupCancelRequested_.load(std::memory_order_acquire))
    {
        if (error)
        {
            *error = QStringLiteral("仿真启动已取消");
        }
        return false;
    }
    emit startupProgress(QStringLiteral("正在创建路由器运行时"), 0, topology.routers.size());
    qint64 completedRouters = 0;
    for (auto it = topology.routers.cbegin(); it != topology.routers.cend(); ++it)
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            if (error)
            {
                *error = QStringLiteral("仿真启动已取消");
            }
            return false;
        }
        RouterRuntime runtime;
        runtime.config = it.value();
        runtime.active = true;
        runtime.localRoutes.reserve(runtime.config.originatedPrefixes.size());
        runtime.locRib.reserve(runtime.config.originatedPrefixes.size());
        const auto neighbors = neighborIndex.value(it.key());
        for (const auto& neighbor : neighbors)
        {
            PeerRuntime peer;
            peer.config = neighbor;
            peer.nextMraiAt = neighbor.mraiMs;
            runtime.peers.insert(neighbor.id, peer);
        }
        QString creationError;
        const RouterNodeContext context{
            .config = runtime.config,
            .topologyRouters = topology.routers,
            .neighbors = neighbors,
        };
        runtime.node = RouterPluginRegistry::instance().createRouterNode(context, this, &creationError);
        if (!runtime.node)
        {
            if (error)
            {
                *error = creationError;
            }
            return false;
        }
        const auto configurationProblems = runtime.node->validateConfiguration();
        if (!configurationProblems.isEmpty())
        {
            if (error)
            {
                *error = QStringLiteral("路由器 %1 的插件配置无效：\n%2").arg(runtime.config.id, configurationProblems.join(u'\n'));
            }
            delete runtime.node;
            runtime.node = nullptr;
            return false;
        }
        for (const auto& prefix : runtime.config.originatedPrefixes)
        {
            auto route = runtime.node->createOriginatedRoute(prefix);
            runtime.localRoutes.insert(prefix, route);
            runtime.locRib.insert(prefix, route);
        }
        routers_.insert(it.key(), std::move(runtime));
        ++completedRouters;
        if ((completedRouters & 0xff) == 0 || completedRouters == topology.routers.size())
        {
            emit startupProgress(QStringLiteral("正在创建路由器运行时"), completedRouters, topology.routers.size());
        }
    }
    emit startupProgress(QStringLiteral("正在建立链路运行时"), 0, topology.links.size());
    qint64 completedLinks = 0;
    for (const auto& link : topology.links)
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            if (error)
            {
                *error = QStringLiteral("仿真启动已取消");
            }
            return false;
        }
        links_.insert(Topology::edgeKey(link.a, link.b), link);
        ++completedLinks;
        if ((completedLinks & 0x3ff) == 0 || completedLinks == topology.links.size())
        {
            emit startupProgress(QStringLiteral("正在建立链路运行时"), completedLinks, topology.links.size());
        }
    }
    return true;
}

bool SimulationEngine::scheduleInitialOpenMessages()
{
    std::vector<ScheduledEvent> initialEvents;
    initialEvents.reserve(static_cast<size_t>(links_.size()) * 2);
    const auto scheduledAt = now();
    const auto appendOpen = [this, scheduledAt, &initialEvents](const QString& from, const QString& to, int delayMs)
    {
        auto router = routers_.find(from);
        if (router == routers_.end() || !router->active)
        {
            return;
        }
        auto peer = router->peers.find(to);
        if (peer == router->peers.end() || !peer->config.enabled)
        {
            return;
        }
        peer->state = PeerState::OpenSent;
        router->node->peerStateChanged(peer->config, peer->state);

        BgpMessage message;
        message.type = MessageType::Open;
        message.from = from;
        message.to = to;
        message.sequence = ++nextSequence_;
        message.openAsn = router->config.asn;
        message.openRouterId = router->config.routerId;
        initialEvents.push_back(ScheduledEvent{
            .dueAt = scheduledAt + std::max(0, delayMs),
            .order = ++nextOrder_,
            .kind = ScheduledKind::DeliverMessages,
            .from = from,
            .to = to,
            .sessionEpoch = peer->sessionEpoch,
            .messages = {std::move(message)},
        });
    };

    qint64 completedLinks = 0;
    for (auto it = links_.cbegin(); it != links_.cend(); ++it)
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            return false;
        }
        if (it->enabled)
        {
            appendOpen(it->a, it->b, it->delayMs);
            appendOpen(it->b, it->a, it->delayMs);
        }
        ++completedLinks;
        if ((completedLinks & 0x3ff) == 0 || completedLinks == links_.size())
        {
            emit startupProgress(QStringLiteral("正在建立 BGP 会话"), completedLinks, links_.size());
        }
    }
    events_ = decltype(events_)(LaterEvent{}, std::move(initialEvents));
    if (!events_.empty())
    {
        markActivity();
    }
    return true;
}

void SimulationEngine::armNextEvent()
{
    pruneInvalidScheduledEvents();
    if (!running_ || events_.empty())
    {
        eventTimer_->stop();
        return;
    }
    const auto delay = std::max<qint64>(0, events_.top().dueAt - now());
    eventTimer_->start(static_cast<int>(std::min<qint64>(delay, std::numeric_limits<int>::max())));
}

void SimulationEngine::invalidateSession(PeerRuntime& peer)
{
    ++peer.sessionEpoch;
    clearPendingUpdates(peer);
    peer.flushScheduled = false;
    peer.withdrawalFlushScheduled = false;
}

void SimulationEngine::clearPendingUpdates(PeerRuntime& peer)
{
    peer.pending.clear();
    peer.pendingAdvertisementCount = 0;
    peer.pendingWithdrawalCount = 0;
}

void SimulationEngine::setPendingUpdate(PeerRuntime& peer, const QString& prefix, PendingUpdate update)
{
    if (const auto existing = peer.pending.constFind(prefix); existing != peer.pending.cend())
    {
        if (existing->route)
        {
            --peer.pendingAdvertisementCount;
        }
        else
        {
            --peer.pendingWithdrawalCount;
        }
    }
    if (update.route)
    {
        ++peer.pendingAdvertisementCount;
    }
    else
    {
        ++peer.pendingWithdrawalCount;
    }
    peer.pending.insert(prefix, std::move(update));
}

void SimulationEngine::removePendingUpdate(PeerRuntime& peer, const QString& prefix)
{
    const auto existing = peer.pending.find(prefix);
    if (existing == peer.pending.end())
    {
        return;
    }
    if (existing->route)
    {
        --peer.pendingAdvertisementCount;
    }
    else
    {
        --peer.pendingWithdrawalCount;
    }
    peer.pending.erase(existing);
}

void SimulationEngine::cancelEmptyFlushes(PeerRuntime& peer)
{
    if (peer.flushScheduled && peer.pendingAdvertisementCount == 0)
    {
        peer.flushScheduled = false;
        ++peer.mraiFlushGeneration;
    }
    if (peer.withdrawalFlushScheduled && peer.pendingWithdrawalCount == 0)
    {
        peer.withdrawalFlushScheduled = false;
        ++peer.withdrawalFlushGeneration;
    }
}

std::optional<quint64> SimulationEngine::sessionEpoch(const QString& from, const QString& to) const
{
    const auto routerIt = routers_.constFind(from);
    if (routerIt == routers_.cend())
    {
        return std::nullopt;
    }
    const auto peerIt = routerIt->peers.constFind(to);
    if (peerIt == routerIt->peers.cend())
    {
        return std::nullopt;
    }
    return peerIt->sessionEpoch;
}

bool SimulationEngine::guardedMessageHasCurrentRoutes(const BgpMessage& message) const
{
    if (!message.guarded)
    {
        return true;
    }
    const auto routerIt = routers_.constFind(message.from);
    if (routerIt == routers_.cend())
    {
        return false;
    }
    const auto peerGenerations = routerIt->outboundGenerations.constFind(message.to);
    if (peerGenerations == routerIt->outboundGenerations.cend())
    {
        return false;
    }
    const auto isCurrent = [&](const QString& prefix)
    {
        const auto expected = message.generations.constFind(prefix);
        return expected != message.generations.cend() && peerGenerations->value(prefix) == expected.value();
    };
    return std::any_of(message.nlri.cbegin(), message.nlri.cend(), isCurrent) ||
           std::any_of(message.withdrawn.cbegin(), message.withdrawn.cend(), isCurrent);
}

bool SimulationEngine::scheduledEventValid(const ScheduledEvent& event) const
{
    const auto routerIt = routers_.constFind(event.from);
    if (routerIt == routers_.cend() || !messageDeliverable(event.from, event.to))
    {
        return false;
    }
    const auto peerIt = routerIt->peers.constFind(event.to);
    if (peerIt == routerIt->peers.cend() || peerIt->sessionEpoch != event.sessionEpoch)
    {
        return false;
    }
    switch (event.kind)
    {
        case ScheduledKind::DeliverMessages:
            return std::any_of(event.messages.cbegin(), event.messages.cend(),
                               [this](const auto& message) { return guardedMessageHasCurrentRoutes(message); });
        case ScheduledKind::FlushMrai:
            return peerIt->state == PeerState::Established && peerIt->flushScheduled && peerIt->pendingAdvertisementCount > 0 &&
                   peerIt->mraiFlushGeneration == event.flushGeneration;
        case ScheduledKind::FlushWithdrawals:
            return peerIt->state == PeerState::Established && peerIt->withdrawalFlushScheduled && peerIt->pendingWithdrawalCount > 0 &&
                   peerIt->withdrawalFlushGeneration == event.flushGeneration;
    }
    return false;
}

void SimulationEngine::pruneInvalidScheduledEvents()
{
    // Invalidated sessions can leave hundreds of thousands of entries in a
    // large heap.  Rebuilding the complete heap makes a single link change
    // O(queue_size log queue_size); stale entries are cheaper to discard only
    // when they reach the head.
    while (!events_.empty() && !scheduledEventValid(events_.top()))
    {
        events_.pop();
    }
}

void SimulationEngine::markActivity(const QString& convergenceTriggerEvent, const QString& convergenceTriggerContext)
{
    const auto activityAt = now();
    lastActivityAt_ = activityAt;
    if (converged_)
    {
        converged_ = false;
        convergenceStartedAt_ = activityAt;
        convergenceMessageCount_ = 0;
        convergenceTriggerEvent_ = convergenceTriggerEvent.isEmpty() ? QStringLiteral("routing_activity") : convergenceTriggerEvent;
        convergenceTriggerContext_ = convergenceTriggerContext;
        emit convergenceChanged(false);
    }
    else if (convergenceTriggerEvent_.isEmpty() && !convergenceTriggerEvent.isEmpty())
    {
        convergenceTriggerEvent_ = convergenceTriggerEvent;
        convergenceTriggerContext_ = convergenceTriggerContext;
    }
}

void SimulationEngine::publishStats()
{
    pruneInvalidScheduledEvents();
    const auto elapsedMs = now();
    emit statsChanged(SimulationStats{
        .running = running_,
        .converged = converged_,
        .pendingEvents = static_cast<qsizetype>(events_.size()),
        .deliveredMessages = deliveredMessages_,
        .elapsedMs = elapsedMs,
        .convergenceElapsedMs = running_ && !converged_ ? std::max<qint64>(0, elapsedMs - convergenceStartedAt_) : 0,
        .convergenceTriggerEvent = convergenceTriggerEvent_,
        .convergenceTriggerContext = convergenceTriggerContext_,
    });
}

void SimulationEngine::scheduleMessages(const QString& from, const QString& to, QVector<BgpMessage> messages, int extraDelayMs)
{
    const auto currentEpoch = sessionEpoch(from, to);
    if (!running_ || messages.isEmpty() || !currentEpoch || !messageDeliverable(from, to))
    {
        return;
    }
    for (auto& message : messages)
    {
        message.from = from;
        message.to = to;
        message.sequence = ++nextSequence_;
    }
    constexpr qsizetype maxMessagesPerScheduledEvent = 16;
    const auto dueAt = now() + linkDelay(from, to) + std::max(0, extraDelayMs);
    qsizetype offset = 0;
    while (offset < messages.size())
    {
        const auto count = std::min(maxMessagesPerScheduledEvent, messages.size() - offset);
        QVector<BgpMessage> chunk;
        chunk.reserve(count);
        for (qsizetype index = 0; index < count; ++index)
        {
            chunk.append(std::move(messages[offset + index]));
        }
        offset += count;
        events_.push(ScheduledEvent{
            .dueAt = dueAt,
            .order = ++nextOrder_,
            .kind = ScheduledKind::DeliverMessages,
            .from = from,
            .to = to,
            .sessionEpoch = *currentEpoch,
            .messages = std::move(chunk),
        });
    }
    markActivity();
    armNextEvent();
}

void SimulationEngine::scheduleMraiFlush(const QString& from, const QString& to, qint64 dueAt)
{
    auto routerIt = routers_.find(from);
    if (!running_ || routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(to);
    if (peerIt == routerIt->peers.end())
    {
        return;
    }
    const auto flushGeneration = ++peerIt->mraiFlushGeneration;
    events_.push(ScheduledEvent{
        .dueAt = std::max(now(), dueAt),
        .order = ++nextOrder_,
        .kind = ScheduledKind::FlushMrai,
        .from = from,
        .to = to,
        .sessionEpoch = peerIt->sessionEpoch,
        .flushGeneration = flushGeneration,
        .messages = {},
    });
    markActivity();
    armNextEvent();
}

void SimulationEngine::scheduleWithdrawalFlush(const QString& from, const QString& to)
{
    auto routerIt = routers_.find(from);
    if (!running_ || routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(to);
    if (peerIt == routerIt->peers.end())
    {
        return;
    }
    const auto flushGeneration = ++peerIt->withdrawalFlushGeneration;
    events_.push(ScheduledEvent{
        .dueAt = now(),
        .order = ++nextOrder_,
        .kind = ScheduledKind::FlushWithdrawals,
        .from = from,
        .to = to,
        .sessionEpoch = peerIt->sessionEpoch,
        .flushGeneration = flushGeneration,
        .messages = {},
    });
    markActivity();
    armNextEvent();
}

void SimulationEngine::processDueEvents()
{
    if (!running_)
    {
        return;
    }
    int processed = 0;
    constexpr int maxPerTurn = 4096;
    constexpr qint64 maxTurnMs = 12;
    QElapsedTimer turnTimer;
    turnTimer.start();
    QVector<SimulationEvent> turnEvents;
    turnEvents.reserve(maxPerTurn);
    activeEventBatch_ = &turnEvents;
    pruneInvalidScheduledEvents();
    while (!events_.empty() && events_.top().dueAt <= now() && processed < maxPerTurn && turnTimer.elapsed() < maxTurnMs)
    {
        auto event = events_.top();
        events_.pop();
        if (!scheduledEventValid(event))
        {
            ++processed;
            continue;
        }
        switch (event.kind)
        {
            case ScheduledKind::DeliverMessages:
                deliverMessages(event);
                break;
            case ScheduledKind::FlushMrai:
                flushMrai(event.from, event.to);
                break;
            case ScheduledKind::FlushWithdrawals:
                flushWithdrawals(event.from, event.to);
                break;
        }
        ++processed;
    }
    activeEventBatch_ = nullptr;
    if (!turnEvents.isEmpty())
    {
        emit eventsGenerated(std::move(turnEvents));
    }
    armNextEvent();
}

void SimulationEngine::deliverMessages(const ScheduledEvent& event)
{
    if (!messageDeliverable(event.from, event.to))
    {
        return;
    }

    QVector<BgpMessage> updates;
    QVector<SimulationEvent> recordedEvents;
    recordedEvents.reserve(event.messages.size());
    for (auto message : event.messages)
    {
        if (message.guarded && !retainCurrentRoutes(message))
        {
            continue;
        }
        if (message.guarded)
        {
            commitOutbound(message);
        }
        recordedEvents.append(messageEvent(message));
        ++deliveredMessages_;
        markActivity();
        ++convergenceMessageCount_;
        switch (message.type)
        {
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
    if (!updates.isEmpty())
    {
        handleUpdateBatch(event.to, event.from, updates);
    }
    if (!recordedEvents.isEmpty())
    {
        publishEvents(std::move(recordedEvents));
    }
}

void SimulationEngine::flushMrai(const QString& from, const QString& to)
{
    auto routerIt = routers_.find(from);
    if (routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(to);
    if (peerIt == routerIt->peers.end())
    {
        return;
    }
    peerIt->flushScheduled = false;
    if (!routerIt->active || peerIt->state != PeerState::Established || !messageDeliverable(from, to))
    {
        clearPendingUpdates(*peerIt);
        return;
    }

    QVector<BgpMessage> messages;
    const auto peerGenerations = routerIt->outboundGenerations.constFind(to);
    for (auto pendingIt = peerIt->pending.begin(); pendingIt != peerIt->pending.end();)
    {
        const auto& prefix = pendingIt.key();
        const auto& pending = pendingIt.value();
        if (!pending.route)
        {
            ++pendingIt;
            continue;
        }
        const auto current = peerGenerations == routerIt->outboundGenerations.cend() ? 0 : peerGenerations->value(prefix);
        if (current == pending.generation)
        {
            if (!messages.isEmpty() && messages.last().advertisedRoute &&
                advertisementTemplateEqual(*messages.last().advertisedRoute, *pending.route))
            {
                messages.last().nlri.append(prefix);
                messages.last().generations.insert(prefix, pending.generation);
            }
            else
            {
                messages.append(makeUpdateMessage(from, to, prefix, pending.route, {}, pending.generation));
            }
        }
        --peerIt->pendingAdvertisementCount;
        pendingIt = peerIt->pending.erase(pendingIt);
    }
    if (messages.isEmpty())
    {
        peerIt->nextMraiAt = now();
        return;
    }
    peerIt->nextMraiAt = now() + peerIt->config.mraiMs;
    scheduleMessages(from, to, std::move(messages));
}

void SimulationEngine::flushWithdrawals(const QString& from, const QString& to)
{
    auto routerIt = routers_.find(from);
    if (routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(to);
    if (peerIt == routerIt->peers.end())
    {
        return;
    }
    peerIt->withdrawalFlushScheduled = false;
    if (!routerIt->active || peerIt->state != PeerState::Established || !messageDeliverable(from, to))
    {
        clearPendingUpdates(*peerIt);
        return;
    }

    QVector<BgpMessage> messages;
    const auto peerGenerations = routerIt->outboundGenerations.constFind(to);
    for (auto pendingIt = peerIt->pending.begin(); pendingIt != peerIt->pending.end();)
    {
        const auto& prefix = pendingIt.key();
        const auto& pending = pendingIt.value();
        if (pending.route)
        {
            ++pendingIt;
            continue;
        }
        const auto current = peerGenerations == routerIt->outboundGenerations.cend() ? 0 : peerGenerations->value(prefix);
        if (current == pending.generation)
        {
            if (!messages.isEmpty() && messages.last().withdrawalAttributes == pending.withdrawalAttributes)
            {
                messages.last().withdrawn.append(prefix);
                messages.last().generations.insert(prefix, pending.generation);
            }
            else
            {
                messages.append(makeUpdateMessage(from, to, prefix, std::nullopt, pending.withdrawalAttributes, pending.generation));
            }
        }
        --peerIt->pendingWithdrawalCount;
        pendingIt = peerIt->pending.erase(pendingIt);
    }
    scheduleMessages(from, to, std::move(messages));
}

bool SimulationEngine::messageDeliverable(const QString& from, const QString& to) const
{
    const auto fromIt = routers_.constFind(from);
    const auto toIt = routers_.constFind(to);
    if (fromIt == routers_.cend() || toIt == routers_.cend() || !fromIt->active || !toIt->active)
    {
        return false;
    }
    const auto linkIt = links_.constFind(Topology::edgeKey(from, to));
    if (linkIt == links_.cend() || !linkIt->enabled)
    {
        return false;
    }
    const auto peerIt = fromIt->peers.constFind(to);
    return peerIt != fromIt->peers.cend() && peerIt->config.enabled;
}

int SimulationEngine::linkDelay(const QString& from, const QString& to) const
{
    const auto it = links_.constFind(Topology::edgeKey(from, to));
    return it == links_.cend() ? 0 : std::max(0, it->delayMs);
}

bool SimulationEngine::retainCurrentRoutes(BgpMessage& message) const
{
    const auto routerIt = routers_.constFind(message.from);
    if (routerIt == routers_.cend())
    {
        return false;
    }
    const auto peerGenerations = routerIt->outboundGenerations.constFind(message.to);
    if (peerGenerations == routerIt->outboundGenerations.cend())
    {
        return false;
    }

    QStringList currentNlri;
    QStringList currentWithdrawals;
    QMap<QString, quint64> currentGenerations;
    currentNlri.reserve(message.nlri.size());
    currentWithdrawals.reserve(message.withdrawn.size());
    const auto retain = [&](const QString& prefix, QStringList& routes)
    {
        const auto expected = message.generations.constFind(prefix);
        if (expected != message.generations.cend() && peerGenerations->value(prefix) == expected.value())
        {
            routes.append(prefix);
            currentGenerations.insert(prefix, expected.value());
        }
    };
    for (const auto& prefix : message.nlri)
    {
        retain(prefix, currentNlri);
    }
    for (const auto& prefix : message.withdrawn)
    {
        retain(prefix, currentWithdrawals);
    }
    message.nlri = std::move(currentNlri);
    message.withdrawn = std::move(currentWithdrawals);
    message.generations = std::move(currentGenerations);
    return !message.nlri.isEmpty() || !message.withdrawn.isEmpty();
}

void SimulationEngine::commitOutbound(const BgpMessage& message)
{
    auto routerIt = routers_.find(message.from);
    if (routerIt == routers_.end())
    {
        return;
    }
    auto& out = routerIt->adjRibOut[message.to];
    out.reserve(out.size() + message.nlri.size());
    for (const auto& prefix : message.withdrawn)
    {
        out.remove(prefix);
    }
    if (!message.nlri.isEmpty())
    {
        const auto fingerprint = advertisedRouteFingerprint(message.advertisedRoute.value_or(RouteEntry{}));
        for (const auto& prefix : message.nlri)
        {
            out.insert(prefix, fingerprint);
        }
    }
    auto generationsIt = routerIt->outboundGenerations.find(message.to);
    if (generationsIt != routerIt->outboundGenerations.end())
    {
        for (auto it = message.generations.cbegin(); it != message.generations.cend(); ++it)
        {
            const auto current = generationsIt->constFind(it.key());
            if (current != generationsIt->cend() && current.value() == it.value())
            {
                generationsIt->remove(it.key());
            }
        }
        if (generationsIt->isEmpty())
        {
            routerIt->outboundGenerations.erase(generationsIt);
        }
    }
}

void SimulationEngine::sendOpen(const QString& from, const QString& to)
{
    auto fromIt = routers_.find(from);
    if (fromIt == routers_.end() || !messageDeliverable(from, to))
    {
        return;
    }
    auto peerIt = fromIt->peers.find(to);
    if (peerIt == fromIt->peers.end())
    {
        return;
    }
    if (peerIt->state != PeerState::Established)
    {
        peerIt->state = PeerState::OpenSent;
        fromIt->node->peerStateChanged(peerIt->config, peerIt->state);
    }
    BgpMessage message;
    message.type = MessageType::Open;
    message.openAsn = fromIt->config.asn;
    message.openRouterId = fromIt->config.routerId;
    scheduleMessages(from, to, {message});
}

void SimulationEngine::handleOpen(const BgpMessage& message)
{
    auto receiverIt = routers_.find(message.to);
    if (receiverIt == routers_.end())
    {
        return;
    }
    auto peerIt = receiverIt->peers.find(message.from);
    if (peerIt == receiverIt->peers.end())
    {
        return;
    }
    if (message.openAsn != peerIt->config.remoteAsn)
    {
        neighborDown(message.to, message.from);
        BgpMessage notification;
        notification.type = MessageType::Notification;
        notification.errorCode = 2;
        notification.errorSubcode = 2;
        notification.errorData = QStringLiteral("Bad Peer AS");
        scheduleMessages(message.to, message.from, {notification});
        return;
    }

    const auto wasEstablished = peerIt->state == PeerState::Established;
    if (peerIt->state == PeerState::Idle)
    {
        sendOpen(message.to, message.from);
    }
    peerIt->state = PeerState::Established;
    receiverIt->node->peerStateChanged(peerIt->config, peerIt->state);
    if (!wasEstablished)
    {
        advertiseTableToPeer(message.to, message.from);
    }
    routingStateDirty_ = true;
}

void SimulationEngine::handleUpdateBatch(const QString& receiver, const QString& sender, const QVector<BgpMessage>& messages)
{
    auto routerIt = routers_.find(receiver);
    if (routerIt == routers_.end())
    {
        return;
    }
    const auto peerIt = routerIt->peers.constFind(sender);
    if (peerIt == routerIt->peers.cend() || peerIt->state != PeerState::Established)
    {
        return;
    }

    QSet<QString> changed;
    auto& peerRoutes = routerIt->adjRibIn[sender];
    qsizetype advertisedCount = 0;
    for (const auto& message : messages)
    {
        advertisedCount += message.nlri.size();
    }
    peerRoutes.reserve(peerRoutes.size() + advertisedCount);
    for (const auto& message : messages)
    {
        for (const auto& prefix : message.withdrawn)
        {
            routerIt->node->importWithdrawal(prefix, message.withdrawalAttributes, peerIt->config);
            peerRoutes.remove(prefix);
            // A version trigger can stale a candidate learned from another
            // peer even if this sender had no Adj-RIB-In entry to remove.
            changed.insert(prefix);
        }
        const RouteEntry advertisedRoute = message.advertisedRoute.value_or(RouteEntry{});
        for (const auto& prefix : message.nlri)
        {
            auto imported = routerIt->node->importAdvertisedRoute(prefix, advertisedRoute, peerIt->config);
            if (!imported)
            {
                if (peerRoutes.remove(prefix) > 0)
                {
                    changed.insert(prefix);
                }
                continue;
            }
            const auto old = peerRoutes.constFind(prefix);
            if (old == peerRoutes.cend() || old.value() != *imported)
            {
                peerRoutes.insert(prefix, *imported);
                changed.insert(prefix);
            }
        }
    }
    runDecision(receiver, changed);
}

void SimulationEngine::handleNotification(const BgpMessage& message)
{
    neighborDown(message.to, message.from);
}

void SimulationEngine::neighborDown(const QString& routerId, const QString& peerId)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(peerId);
    if (peerIt == routerIt->peers.end())
    {
        return;
    }
    peerIt->state = PeerState::Idle;
    routerIt->node->peerStateChanged(peerIt->config, peerIt->state);
    invalidateSession(*peerIt);
    routerIt->outboundGenerations.remove(peerId);
    routerIt->adjRibOut.remove(peerId);
    QSet<QString> affected;
    const auto inbound = routerIt->adjRibIn.take(peerId);
    for (auto it = inbound.cbegin(); it != inbound.cend(); ++it)
    {
        affected.insert(it.key());
    }
    if (routerIt->active)
    {
        runDecision(routerId, affected);
    }
    routingStateDirty_ = true;
}

void SimulationEngine::runDecision(const QString& routerId, const QSet<QString>& changedPrefixes)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end() || !routerIt->active || changedPrefixes.isEmpty())
    {
        return;
    }

    routerIt->locRib.reserve(routerIt->locRib.size() + changedPrefixes.size());
    bool anyChange = false;
    for (const auto& prefix : changedPrefixes)
    {
        const auto oldIt = routerIt->locRib.constFind(prefix);
        const std::optional<RouteEntry> old = oldIt == routerIt->locRib.cend() ? std::nullopt : std::optional<RouteEntry>(oldIt.value());
        const auto selected = selectBest(*routerIt, prefix);
        if (old == selected)
        {
            continue;
        }
        anyChange = true;
        if (selected)
        {
            routerIt->locRib.insert(prefix, *selected);
        }
        else
        {
            routerIt->locRib.remove(prefix);
        }
        disseminate(routerId, prefix, selected);
    }
    if (anyChange)
    {
        routingStateDirty_ = true;
    }
}

void SimulationEngine::advertiseTableToPeer(const QString& routerId, const QString& peerId)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end() || !routerIt->active)
    {
        return;
    }
    const auto peerIt = routerIt->peers.constFind(peerId);
    if (peerIt == routerIt->peers.cend() || peerIt->state != PeerState::Established || !peerIt->config.enabled)
    {
        return;
    }
    routerIt->outboundGenerations[peerId].reserve(routerIt->locRib.size());
    for (auto routeIt = routerIt->locRib.cbegin(); routeIt != routerIt->locRib.cend(); ++routeIt)
    {
        auto outbound = routerIt->node->exportRouteForPrefix(routeIt.key(), routeIt.value(), peerIt->config);
        queueAdvertisement(routerId, peerId, routeIt.key(), outbound);
    }
}

std::optional<RouteEntry> SimulationEngine::selectBest(const RouterRuntime& router, const QString& prefix) const
{
    QVector<RouteEntry> candidates;
    candidates.reserve(router.adjRibIn.size() + 1);
    if (const auto local = router.localRoutes.constFind(prefix); local != router.localRoutes.cend())
    {
        candidates.append(local.value());
    }
    for (const auto& peerRoutes : router.adjRibIn)
    {
        if (const auto route = peerRoutes.constFind(prefix); route != peerRoutes.cend())
        {
            candidates.append(route.value());
        }
    }
    const auto old = router.locRib.constFind(prefix);
    const std::optional<RouteEntry> currentBest = old == router.locRib.cend() ? std::nullopt : std::optional<RouteEntry>(old.value());
    auto selected = router.node->selectBestRoute(prefix, candidates, currentBest);
    return selected;
}

void SimulationEngine::disseminate(const QString& routerId, const QString& prefix, const std::optional<RouteEntry>& route)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end() || !routerIt->active)
    {
        return;
    }

    for (auto peerIt = routerIt->peers.begin(); peerIt != routerIt->peers.end(); ++peerIt)
    {
        const auto peerId = peerIt.key();
        if (peerIt->state != PeerState::Established || !peerIt->config.enabled)
        {
            continue;
        }
        std::optional<RouteEntry> outbound;
        if (route)
        {
            outbound = routerIt->node->exportRouteForPrefix(prefix, *route, peerIt->config);
        }
        if (outbound)
        {
            const auto pendingGenerations = routerIt->outboundGenerations.constFind(peerId);
            const auto outboundChangePending =
                pendingGenerations != routerIt->outboundGenerations.cend() && pendingGenerations->contains(prefix);
            if (const auto outboundRoutes = routerIt->adjRibOut.constFind(peerId); outboundRoutes != routerIt->adjRibOut.cend())
            {
                const auto existing = outboundRoutes->constFind(prefix);
                if (!outboundChangePending && existing != outboundRoutes->cend() &&
                    existing.value() == advertisedRouteFingerprint(*outbound))
                {
                    continue;
                }
            }
        }
        PathAttributes withdrawalAttributes;
        if (!route)
        {
            withdrawalAttributes = routerIt->node->exportWithdrawal(prefix, peerIt->config);
        }
        queueAdvertisement(routerId, peerId, prefix, outbound, std::move(withdrawalAttributes));
    }
}

void SimulationEngine::queueAdvertisement(const QString& routerId, const QString& peerId, const QString& prefix,
                                           const std::optional<RouteEntry>& route, PathAttributes withdrawalAttributes)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        return;
    }
    auto peerIt = routerIt->peers.find(peerId);
    if (peerIt == routerIt->peers.end() || peerIt->state != PeerState::Established ||
        !messageDeliverable(routerId, peerId))
    {
        return;
    }
    if (!route)
    {
        const auto outbound = routerIt->adjRibOut.constFind(peerId);
        const auto hadDelivered = outbound != routerIt->adjRibOut.cend() && outbound->contains(prefix);
        if (!hadDelivered)
        {
            removePendingUpdate(*peerIt, prefix);
            auto generations = routerIt->outboundGenerations.find(peerId);
            if (generations != routerIt->outboundGenerations.end())
            {
                generations->remove(prefix);
                if (generations->isEmpty())
                {
                    routerIt->outboundGenerations.erase(generations);
                }
            }
            cancelEmptyFlushes(*peerIt);
            return;
        }
        const auto generation = ++nextGeneration_;
        auto& generations = routerIt->outboundGenerations[peerId];
        generations[prefix] = generation;
        setPendingUpdate(
            *peerIt, prefix,
            PendingUpdate{.route = std::nullopt, .withdrawalAttributes = std::move(withdrawalAttributes), .generation = generation});
        cancelEmptyFlushes(*peerIt);
        if (!peerIt->withdrawalFlushScheduled)
        {
            peerIt->withdrawalFlushScheduled = true;
            scheduleWithdrawalFlush(routerId, peerId);
        }
        return;
    }

    const auto generation = ++nextGeneration_;
    auto& generations = routerIt->outboundGenerations[peerId];
    generations[prefix] = generation;
    setPendingUpdate(*peerIt, prefix, PendingUpdate{.route = route, .withdrawalAttributes = {}, .generation = generation});
    cancelEmptyFlushes(*peerIt);
    if (!peerIt->flushScheduled)
    {
        peerIt->flushScheduled = true;
        const auto dueAt = peerIt->config.mraiMs <= 0 ? now() : std::max(now(), peerIt->nextMraiAt);
        scheduleMraiFlush(routerId, peerId, dueAt);
    }
}

BgpMessage SimulationEngine::makeUpdateMessage(const QString& from, const QString& to, const QString& prefix,
                                               const std::optional<RouteEntry>& route, const PathAttributes& withdrawalAttributes,
                                               quint64 generation) const
{
    BgpMessage message;
    message.type = MessageType::Update;
    message.from = from;
    message.to = to;
    message.guarded = true;
    message.generations.insert(prefix, generation);
    if (route)
    {
        message.nlri.append(prefix);
        message.advertisedRoute = route;
    }
    else
    {
        message.withdrawn.append(prefix);
        message.withdrawalAttributes = withdrawalAttributes;
    }
    return message;
}

void SimulationEngine::setLinkState(const QString& a, const QString& b, bool enabled)
{
    if (!running_)
    {
        emit errorOccurred(QStringLiteral("仿真尚未运行"));
        return;
    }
    auto linkIt = links_.find(Topology::edgeKey(a, b));
    if (linkIt == links_.end())
    {
        emit errorOccurred(QStringLiteral("链路不存在：%1 - %2").arg(a, b));
        return;
    }
    if (linkIt->enabled == enabled)
    {
        return;
    }
    linkIt->enabled = enabled;
    if (auto router = routers_.find(a); router != routers_.end())
    {
        if (auto peer = router->peers.find(b); peer != router->peers.end())
        {
            peer->config.enabled = enabled;
        }
    }
    if (auto router = routers_.find(b); router != routers_.end())
    {
        if (auto peer = router->peers.find(a); peer != router->peers.end())
        {
            peer->config.enabled = enabled;
        }
    }
    const auto triggerEvent = enabled ? QStringLiteral("link_up") : QStringLiteral("link_down");
    markActivity(triggerEvent, QStringLiteral("%1 ↔ %2").arg(a, b));
    if (enabled)
    {
        sendOpen(a, b);
        sendOpen(b, a);
    }
    else
    {
        neighborDown(a, b);
        neighborDown(b, a);
    }
    recordTopologyEvent(triggerEvent, {{QStringLiteral("a"), a}, {QStringLiteral("b"), b}});
    emit linkStateChanged(a, b, enabled);
    requestAllSnapshots();
}

void SimulationEngine::setRouterState(const QString& routerId, bool enabled)
{
    if (!running_)
    {
        emit errorOccurred(QStringLiteral("仿真尚未运行"));
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
        return;
    }
    if (routerIt->active == enabled)
    {
        return;
    }
    const auto triggerEvent = enabled ? QStringLiteral("router_up") : QStringLiteral("router_down");
    markActivity(triggerEvent, routerId);
    if (!enabled)
    {
        routerIt->active = false;
        routerIt->node->routerStateChanged(false);
        const auto oldPrefixes = routerIt->locRib.keys();
        const auto peers = routerIt->peers.keys();
        for (const auto& peerId : peers)
        {
            neighborDown(peerId, routerId);
        }
        for (const auto& peerId : peers)
        {
            neighborDown(routerId, peerId);
        }
        routerIt->localRoutes.clear();
        routerIt->locRib.clear();
        routerIt->adjRibIn.clear();
        routerIt->adjRibOut.clear();
        if (!oldPrefixes.isEmpty())
        {
            routingStateDirty_ = true;
        }
    }
    else
    {
        routerIt->active = true;
        routerIt->node->routerStateChanged(true);
        QSet<QString> localPrefixes;
        for (const auto& prefix : routerIt->config.originatedPrefixes)
        {
            auto route = routerIt->node->createOriginatedRoute(prefix);
            routerIt->localRoutes.insert(prefix, route);
            localPrefixes.insert(prefix);
        }
        runDecision(routerId, localPrefixes);
        for (const auto& peerId : routerIt->peers.keys())
        {
            if (messageDeliverable(routerId, peerId))
            {
                sendOpen(routerId, peerId);
                sendOpen(peerId, routerId);
            }
        }
    }
    recordTopologyEvent(triggerEvent, {{QStringLiteral("router"), routerId}});
    emit routerStateChanged(routerId, enabled);
    requestAllSnapshots();
}

void SimulationEngine::originatePrefix(const QString& routerId, const QString& prefixValue)
{
    const auto prefix = prefixValue.trimmed();
    if (!running_)
    {
        emit errorOccurred(QStringLiteral("仿真尚未运行"));
        return;
    }
    if (!validIpv4Prefix(prefix))
    {
        emit errorOccurred(QStringLiteral("前缀无效：%1").arg(prefix));
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
        return;
    }
    if (!routerIt->config.originatedPrefixes.contains(prefix))
    {
        routerIt->config.originatedPrefixes.append(prefix);
    }
    if (!routerIt->active || routerIt->localRoutes.contains(prefix))
    {
        return;
    }
    auto route = routerIt->node->createOriginatedRoute(prefix);
    routerIt->localRoutes.insert(prefix, route);
    markActivity(QStringLiteral("prefix_advertised"), QStringLiteral("%1 · %2").arg(routerId, prefix));
    runDecision(routerId, {prefix});
    recordTopologyEvent(QStringLiteral("prefix_advertised"), {{QStringLiteral("router"), routerId}, {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::withdrawPrefix(const QString& routerId, const QString& prefixValue)
{
    const auto prefix = prefixValue.trimmed();
    if (!running_)
    {
        emit errorOccurred(QStringLiteral("仿真尚未运行"));
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        emit errorOccurred(QStringLiteral("路由器不存在：%1").arg(routerId));
        return;
    }
    routerIt->config.originatedPrefixes.removeAll(prefix);
    if (!routerIt->localRoutes.contains(prefix))
    {
        return;
    }
    routerIt->node->localRouteWithdrawn(prefix);
    routerIt->localRoutes.remove(prefix);
    markActivity(QStringLiteral("prefix_withdrawn"), QStringLiteral("%1 · %2").arg(routerId, prefix));
    runDecision(routerId, {prefix});
    recordTopologyEvent(QStringLiteral("prefix_withdrawn"), {{QStringLiteral("router"), routerId}, {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::updateStatus()
{
    if (!running_)
    {
        return;
    }
    pruneInvalidScheduledEvents();
    const auto quietMs = std::max(0, simulation_.convergenceQuietMs);
    const auto isNowConverged = events_.empty() && now() - lastActivityAt_ >= quietMs;
    if (isNowConverged != converged_)
    {
        converged_ = isNowConverged;
        emit convergenceChanged(converged_);
        if (converged_)
        {
            const auto completedAt = now();
            const auto durationMs = std::max<qint64>(0, completedAt - convergenceStartedAt_);
            recordTopologyEvent(QStringLiteral("converged"),
                                {{QStringLiteral("elapsed_ms"), QString::number(completedAt)},
                                 {QStringLiteral("convergence_sequence"), QString::number(++convergenceSequence_)},
                                 {QStringLiteral("started_at_ms"), QString::number(convergenceStartedAt_)},
                                 {QStringLiteral("completed_at_ms"), QString::number(completedAt)},
                                 {QStringLiteral("duration_ms"), QString::number(durationMs)},
                                 {QStringLiteral("bgp_message_count"), QString::number(convergenceMessageCount_)},
                                 {QStringLiteral("trigger_event"), convergenceTriggerEvent_},
                                 {QStringLiteral("trigger_context"), convergenceTriggerContext_}});
        }
    }
    if (routingStateDirty_)
    {
        routingStateDirty_ = false;
        emit routingStateChanged();
    }
    publishStats();
}

RibSnapshot SimulationEngine::ribSnapshot(const QString& routerId) const
{
    RibSnapshot snapshot;
    snapshot.router = routerId;
    const auto routerIt = routers_.constFind(routerId);
    if (routerIt == routers_.cend())
    {
        return snapshot;
    }
    snapshot.localRoutes = routerIt->localRoutes;
    snapshot.locRib = routerIt->locRib;
    snapshot.adjRibIn = routerIt->adjRibIn;
    return snapshot;
}

QVector<PeerSnapshot> SimulationEngine::peerSnapshots(const QString& routerId) const
{
    QVector<PeerSnapshot> snapshots;
    const auto routerIt = routers_.constFind(routerId);
    if (routerIt == routers_.cend())
    {
        return snapshots;
    }
    for (auto it = routerIt->peers.cbegin(); it != routerIt->peers.cend(); ++it)
    {
        snapshots.append(PeerSnapshot{
            .id = it.key(),
            .remoteAsn = it->config.remoteAsn,
            .sessionType = it->config.sessionType,
            .rrClient = it->config.rrClient,
            .enabled = it->config.enabled,
            .mraiMs = it->config.mraiMs,
            .state = it->state,
            .relationship = it->config.relationship,
        });
    }
    return snapshots;
}

QVector<RouterSnapshot> SimulationEngine::routerSnapshots() const
{
    QVector<RouterSnapshot> snapshots;
    for (auto it = routers_.cbegin(); it != routers_.cend(); ++it)
    {
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

void SimulationEngine::requestRouterSnapshot(const QString& routerId)
{
    emit ribSnapshotReady(ribSnapshot(routerId));
    emit peersSnapshotReady(routerId, peerSnapshots(routerId));
}

void SimulationEngine::requestAllSnapshots()
{
    emit routerSnapshotsChanged(routerSnapshots());
}

void SimulationEngine::requestPath(const QString& routerId, const QString& prefix)
{
    QStringList path;
    QSet<QString> seen;
    auto current = routerId;
    while (!current.isEmpty() && !seen.contains(current))
    {
        const auto routerIt = routers_.constFind(current);
        if (routerIt == routers_.cend())
        {
            break;
        }
        seen.insert(current);
        path.append(current);
        const auto routeIt = routerIt->locRib.constFind(prefix);
        if (routeIt == routerIt->locRib.cend() || routeIt->localOrigin || routeIt->learnedFrom.isEmpty() || routeIt->learnedFrom == current)
        {
            break;
        }
        current = routeIt->learnedFrom;
    }
    emit pathReady(routerId, prefix, path);
}

SimulationEvent SimulationEngine::messageEvent(const BgpMessage& message) const
{
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
    if (fromIt != routers_.cend())
    {
        event.fromAs = fromIt->config.asn;
    }
    if (toIt != routers_.cend())
    {
        event.toAs = toIt->config.asn;
    }
    if (message.type == MessageType::Update)
    {
        const auto& attributes = message.advertisedRoute ? message.advertisedRoute->attributes : message.withdrawalAttributes;
        event.action = message.nlri.isEmpty() && !message.withdrawn.isEmpty() ? QStringLiteral("WITHDRAW") : QStringLiteral("UPDATE");
        event.nextHop = attributes.nextHop;
        event.asPath = attributes.asPath;
        event.localPref = attributes.localPref;
        event.med = attributes.med;
        if (attributes.tfpVersionInfo)
        {
            event.details.insert(QStringLiteral("tfp_dependency_vector"),
                                 tfpVectorText(attributes.tfpVersionInfo->dependencyVector));
            event.details.insert(QStringLiteral("tfp_trigger_vector"), tfpVectorText(attributes.tfpVersionInfo->triggerVector));
        }
    }
    else
    {
        event.action = toString(message.type);
    }
    return event;
}

void SimulationEngine::publishEvents(QVector<SimulationEvent> events)
{
    if (events.isEmpty())
    {
        return;
    }
    if (activeEventBatch_)
    {
        const auto requiredCapacity = activeEventBatch_->size() + events.size();
        if (requiredCapacity > activeEventBatch_->capacity())
        {
            const auto grownCapacity = std::max<qsizetype>(4096, activeEventBatch_->capacity() * 2);
            activeEventBatch_->reserve(std::max(requiredCapacity, grownCapacity));
        }
        for (auto& event : events)
        {
            activeEventBatch_->append(std::move(event));
        }
        return;
    }
    emit eventsGenerated(std::move(events));
}

void SimulationEngine::recordTopologyEvent(const QString& name, QMap<QString, QString> details)
{
    SimulationEvent event;
    event.timestamp = QDateTime::currentDateTime();
    event.event = name;
    event.action = QStringLiteral("TOPOLOGY");
    event.details = std::move(details);
    event.router = event.details.value(QStringLiteral("router"));
    event.from = event.details.value(QStringLiteral("a"));
    event.to = event.details.value(QStringLiteral("b"));
    publishEvents({std::move(event)});
}

bool SimulationEngine::hasRouteReflectorClients(const RouterRuntime& router) const
{
    return std::any_of(router.peers.cbegin(), router.peers.cend(), [](const auto& peer) { return peer.config.rrClient; });
}

} // namespace bgptester
