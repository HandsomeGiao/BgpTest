#include "engine/SimulationEngine.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QScopedValueRollback>
#include <QThread>

#include <algorithm>
#include <limits>
#include <utility>

namespace bgptester
{
namespace
{

QPair<quint64, quint64> advertisedRouteFingerprint(const RouteEntry& route, const TfpVersionInfo* prefixVersionInfo = nullptr)
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
        first = mix(first, static_cast<quint64>(::qHash(value, static_cast<size_t>(first))));
        second = mix(second, static_cast<quint64>(::qHash(value, static_cast<size_t>(second))));
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
    if (!prefixVersionInfo && route.attributes.tfpVersionInfo)
    {
        prefixVersionInfo = &*route.attributes.tfpVersionInfo;
    }
    if (prefixVersionInfo && !prefixVersionInfo->dependencyVector.isEmpty())
    {
        add(QStringLiteral("tfp-version-info"));
        add(QStringLiteral("dependency-vector"));
        for (auto it = prefixVersionInfo->dependencyVector.cbegin(); it != prefixVersionInfo->dependencyVector.cend(); ++it)
        {
            add(it.key().asn);
            add(it.key().entityId);
            add(it.value());
        }
    }
    return {first, second};
}

bool pathAttributesTemplateEqual(const PathAttributes& lhs, const PathAttributes& rhs)
{
    return lhs.origin == rhs.origin && lhs.asPath == rhs.asPath && lhs.nextHop == rhs.nextHop &&
           lhs.localPref == rhs.localPref && lhs.med == rhs.med && lhs.originatorId == rhs.originatorId &&
           lhs.clusterList == rhs.clusterList && lhs.communities == rhs.communities;
}

bool advertisementTemplateEqual(const RouteEntry& lhs, const RouteEntry& rhs)
{
    return pathAttributesTemplateEqual(lhs.attributes, rhs.attributes) && lhs.learnedFrom == rhs.learnedFrom &&
           lhs.sourceSession == rhs.sourceSession && lhs.localOrigin == rhs.localOrigin && lhs.source == rhs.source;
}

void mergeVersions(TfpVersionVector& destination, const TfpVersionVector& source)
{
    for (auto it = source.cbegin(); it != source.cend(); ++it)
    {
        auto current = destination.find(it.key());
        if (current == destination.end() || current.value() < it.value())
        {
            destination.insert(it.key(), it.value());
        }
    }
}

void promoteCommonTfpVersionInfoToSidecar(BgpMessage& message)
{
    if (!message.tfpVersionInfoByPrefix.isEmpty())
    {
        return;
    }

    auto* commonVersionInfo = message.advertisedRoute ? &message.advertisedRoute->attributes.tfpVersionInfo
                                                      : &message.withdrawalAttributes.tfpVersionInfo;
    if (!*commonVersionInfo)
    {
        return;
    }

    const auto& prefixes = message.advertisedRoute ? message.nlri : message.withdrawn;
    for (const auto& prefix : prefixes)
    {
        message.tfpVersionInfoByPrefix.insert(prefix, **commonVersionInfo);
    }
    commonVersionInfo->reset();
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

qint64 SimulationEngine::schedulingTime() const
{
    if (activeSchedulingAt_)
    {
        return *activeSchedulingAt_;
    }
    return std::max({now(), lastProcessedSimulationAt_, lastSimulationActivityAt_});
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
    lastError_.clear();
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
        lastError_ = QStringLiteral("无法启动仿真：\n%1").arg(problems.join(u'\n'));
        emit errorOccurred(lastError_);
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
        lastError_ = QStringLiteral("无法启动仿真：\n%1").arg(pluginError);
        emit errorOccurred(lastError_);
        return;
    }
    clock_.start();
    lastActivityAt_ = 0;
    convergenceStartedAt_ = 0;
    lastSimulationActivityAt_ = 0;
    lastProcessedSimulationAt_ = 0;
    convergenceStartedSimulationAt_ = 0;
    convergenceSequence_ = 0;
    convergenceMessageCount_ = 0;
    convergenceTriggerEvent_ = QStringLiteral("simulation_started");
    convergenceTriggerContext_ = simulation_.name;
    converged_ = false;
    for (auto& router : routers_)
    {
        router.node->simulationStarted();
        router.node->routerStateChanged(true);
        router.node->convergenceStateChanged(false);
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
    lastError_.clear();
    if (!running_)
    {
        return;
    }
    recordTopologyEvent(QStringLiteral("simulation_stopped"));
    running_ = false;
    const auto wasConverged = converged_;
    converged_ = false;
    if (wasConverged)
    {
        notifyConvergenceStateChanged(false);
    }
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
        router.outboundGenerations.clear();
        router.node->simulationStopped();
    }
    outstandingTriggers_.clear();
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
    activeSchedulingAt_.reset();
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
    outstandingTriggers_.clear();
    deliveredMessages_ = 0;
    convergenceMessageCount_ = 0;
    lastSimulationActivityAt_ = 0;
    lastProcessedSimulationAt_ = 0;
    convergenceStartedSimulationAt_ = 0;
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
    const auto scheduledAt = schedulingTime();
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

const SimulationEngine::PendingUpdate& SimulationEngine::setPendingUpdate(PeerRuntime& peer, const QString& prefix,
                                                                           PendingUpdate update)
{
    if (const auto existing = peer.pending.constFind(prefix); existing != peer.pending.cend())
    {
        // The sparse generation table owns uncommitted Trigger causality.
        // queueAdvertisement() migrates it before replacing this pending item,
        // so merging the same vector here would duplicate the hot-path work.
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
    return peer.pending.insert(prefix, std::move(update)).value();
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
    const auto simulationActivityAt = schedulingTime();
    lastSimulationActivityAt_ = std::max(lastSimulationActivityAt_, simulationActivityAt);
    if (converged_)
    {
        converged_ = false;
        convergenceStartedAt_ = activityAt;
        convergenceStartedSimulationAt_ = simulationActivityAt;
        convergenceMessageCount_ = 0;
        convergenceTriggerEvent_ = convergenceTriggerEvent.isEmpty() ? QStringLiteral("routing_activity") : convergenceTriggerEvent;
        convergenceTriggerContext_ = convergenceTriggerContext;
        notifyConvergenceStateChanged(false);
        emit convergenceChanged(false);
    }
    else if (convergenceTriggerEvent_.isEmpty() && !convergenceTriggerEvent.isEmpty())
    {
        convergenceTriggerEvent_ = convergenceTriggerEvent;
        convergenceTriggerContext_ = convergenceTriggerContext;
    }
}

void SimulationEngine::notifyConvergenceStateChanged(bool converged)
{
    for (auto& router : routers_)
    {
        if (router.node)
        {
            router.node->convergenceStateChanged(converged);
        }
    }
}

void SimulationEngine::publishStats()
{
    emit statsChanged(statsSnapshot());
}

SimulationStats SimulationEngine::statsSnapshot()
{
    pruneInvalidScheduledEvents();
    const auto elapsedMs = now();
    return SimulationStats{
        .running = running_,
        .converged = converged_,
        .pendingEvents = static_cast<qsizetype>(events_.size()),
        .deliveredMessages = deliveredMessages_,
        .elapsedMs = elapsedMs,
        .convergenceElapsedMs = running_ && !converged_ ? std::max<qint64>(0, elapsedMs - convergenceStartedAt_) : 0,
        .convergenceTriggerEvent = convergenceTriggerEvent_,
        .convergenceTriggerContext = convergenceTriggerContext_,
    };
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
    const auto dueAt = schedulingTime() + linkDelay(from, to) + std::max(0, extraDelayMs);
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
        .dueAt = std::max(schedulingTime(), dueAt),
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
        .dueAt = schedulingTime(),
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
    constexpr int maxPerTurn = 16384;
    constexpr qint64 maxTurnMs = 48;
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
        const QScopedValueRollback<std::optional<qint64>> schedulingScope(activeSchedulingAt_, event.dueAt);
        lastProcessedSimulationAt_ = std::max(lastProcessedSimulationAt_, event.dueAt);
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

void SimulationEngine::deliverMessages(ScheduledEvent& event)
{
    if (!messageDeliverable(event.from, event.to))
    {
        return;
    }

    QVector<BgpMessage> updates;
    QVector<SimulationEvent> recordedEvents;
    recordedEvents.reserve(event.messages.size());
    for (auto& message : event.messages)
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
                updates.append(std::move(message));
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
                // A one-prefix UPDATE keeps TFP state in the common route
                // attributes. Identical prefix metadata remains common across
                // an aggregate; promote only when a real per-prefix override
                // appears.
                const auto keepCommonVersionInfo = messages.last().tfpVersionInfoByPrefix.isEmpty() &&
                                                   messages.last().advertisedRoute->attributes.tfpVersionInfo ==
                                                       pending.route->attributes.tfpVersionInfo;
                if (!keepCommonVersionInfo)
                {
                    promoteCommonTfpVersionInfoToSidecar(messages.last());
                }
                messages.last().nlri.append(prefix);
                messages.last().generations.insert(prefix, pending.generation);
                if (!keepCommonVersionInfo && pending.route->attributes.tfpVersionInfo)
                {
                    messages.last().tfpVersionInfoByPrefix.insert(prefix, *pending.route->attributes.tfpVersionInfo);
                }
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
        peerIt->nextMraiAt = schedulingTime();
        return;
    }
    peerIt->nextMraiAt = schedulingTime() + peerIt->config.mraiMs;
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
            if (!messages.isEmpty() &&
                pathAttributesTemplateEqual(messages.last().withdrawalAttributes, pending.withdrawalAttributes))
            {
                const auto keepCommonVersionInfo = messages.last().tfpVersionInfoByPrefix.isEmpty() &&
                                                   messages.last().withdrawalAttributes.tfpVersionInfo ==
                                                       pending.withdrawalAttributes.tfpVersionInfo;
                if (!keepCommonVersionInfo)
                {
                    promoteCommonTfpVersionInfoToSidecar(messages.last());
                }
                messages.last().withdrawn.append(prefix);
                messages.last().generations.insert(prefix, pending.generation);
                if (!keepCommonVersionInfo && pending.withdrawalAttributes.tfpVersionInfo)
                {
                    messages.last().tfpVersionInfoByPrefix.insert(prefix, *pending.withdrawalAttributes.tfpVersionInfo);
                }
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

    const auto isCurrent = [&](const QString& prefix)
    {
        const auto expected = message.generations.constFind(prefix);
        return expected != message.generations.cend() && peerGenerations->value(prefix) == expected.value();
    };
    if (std::all_of(message.nlri.cbegin(), message.nlri.cend(), isCurrent) &&
        std::all_of(message.withdrawn.cbegin(), message.withdrawn.cend(), isCurrent))
    {
        return true;
    }

    QStringList currentNlri;
    QStringList currentWithdrawals;
    QMap<QString, quint64> currentGenerations;
    QMap<QString, TfpVersionInfo> currentVersionInfo;
    currentNlri.reserve(message.nlri.size());
    currentWithdrawals.reserve(message.withdrawn.size());
    const auto retain = [&](const QString& prefix, QStringList& routes)
    {
        if (isCurrent(prefix))
        {
            const auto expected = message.generations.constFind(prefix);
            routes.append(prefix);
            currentGenerations.insert(prefix, expected.value());
            if (const auto versionInfo = message.tfpVersionInfoByPrefix.constFind(prefix);
                versionInfo != message.tfpVersionInfoByPrefix.cend())
            {
                currentVersionInfo.insert(prefix, versionInfo.value());
            }
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
    message.tfpVersionInfoByPrefix = std::move(currentVersionInfo);
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
        const auto advertisedRoute = message.advertisedRoute.value_or(RouteEntry{});
        const auto templateFingerprint = advertisedRouteFingerprint(advertisedRoute);
        const TfpVersionVector* previousDependencyVector = nullptr;
        QPair<quint64, quint64> previousPrefixFingerprint;
        for (const auto& prefix : message.nlri)
        {
            const auto versionInfo = message.tfpVersionInfoByPrefix.constFind(prefix);
            auto fingerprint = templateFingerprint;
            if (versionInfo != message.tfpVersionInfoByPrefix.cend() && !versionInfo->dependencyVector.isEmpty())
            {
                if (previousDependencyVector && *previousDependencyVector == versionInfo->dependencyVector)
                {
                    fingerprint = previousPrefixFingerprint;
                }
                else
                {
                    fingerprint = advertisedRouteFingerprint(advertisedRoute, &versionInfo.value());
                    previousDependencyVector = &versionInfo->dependencyVector;
                    previousPrefixFingerprint = fingerprint;
                }
            }
            out.insert(prefix, fingerprint);
        }
    }
    auto generationsIt = routerIt->outboundGenerations.find(message.to);
    if (generationsIt != routerIt->outboundGenerations.end())
    {
        for (auto it = message.generations.cbegin(); it != message.generations.cend(); ++it)
        {
            const auto current = generationsIt->find(it.key());
            if (current != generationsIt->end() && current.value() == it.value())
            {
                outstandingTriggers_.remove(it.value());
                generationsIt->erase(current);
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
            if (const auto versionInfo = message.tfpVersionInfoByPrefix.constFind(prefix);
                versionInfo != message.tfpVersionInfoByPrefix.cend())
            {
                auto withdrawalAttributes = message.withdrawalAttributes;
                withdrawalAttributes.tfpVersionInfo = versionInfo.value();
                routerIt->node->importWithdrawal(prefix, withdrawalAttributes, peerIt->config);
            }
            else
            {
                routerIt->node->importWithdrawal(prefix, message.withdrawalAttributes, peerIt->config);
            }
            peerRoutes.remove(prefix);
            // A version trigger can stale a candidate learned from another
            // peer even if this sender had no Adj-RIB-In entry to remove.
            changed.insert(prefix);
        }
        const RouteEntry advertisedRouteTemplate = message.advertisedRoute.value_or(RouteEntry{});
        for (const auto& prefix : message.nlri)
        {
            std::optional<RouteEntry> imported;
            if (const auto versionInfo = message.tfpVersionInfoByPrefix.constFind(prefix);
                versionInfo != message.tfpVersionInfoByPrefix.cend())
            {
                auto advertisedRoute = advertisedRouteTemplate;
                advertisedRoute.attributes.tfpVersionInfo = versionInfo.value();
                imported = routerIt->node->importAdvertisedRoute(prefix, advertisedRoute, peerIt->config);
            }
            else
            {
                imported = routerIt->node->importAdvertisedRoute(prefix, advertisedRouteTemplate, peerIt->config);
            }
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
            else if (routerIt->node->requiresDissemination(prefix))
            {
                // Stateful plugins may consume a transient trigger during
                // import, leaving the stored RouteEntry byte-for-byte stable.
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
    if (const auto generations = routerIt->outboundGenerations.constFind(peerId);
        generations != routerIt->outboundGenerations.cend())
    {
        for (auto generation = generations->cbegin(); generation != generations->cend(); ++generation)
        {
            outstandingTriggers_.remove(generation.value());
        }
    }
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
    QVector<RouteEntry> candidateScratch;
    candidateScratch.reserve(routerIt->adjRibIn.size() + 1);
    bool anyChange = false;
    for (const auto& prefix : changedPrefixes)
    {
        const auto oldIt = routerIt->locRib.constFind(prefix);
        const std::optional<RouteEntry> old = oldIt == routerIt->locRib.cend() ? std::nullopt : std::optional<RouteEntry>(oldIt.value());
        const auto selected = selectBest(*routerIt, prefix, old, candidateScratch);
        const auto routeChanged = old != selected;
        const auto forceDissemination = routerIt->node->requiresDissemination(prefix);
        if (routeChanged)
        {
            anyChange = true;
            if (selected)
            {
                routerIt->locRib.insert(prefix, *selected);
            }
            else
            {
                routerIt->locRib.remove(prefix);
            }
        }
        if (routeChanged || forceDissemination)
        {
            disseminate(routerId, prefix, selected, forceDissemination);
        }
        routerIt->node->decisionCompleted(prefix);
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

std::optional<RouteEntry> SimulationEngine::selectBest(const RouterRuntime& router, const QString& prefix,
                                                        const std::optional<RouteEntry>& currentBest,
                                                        QVector<RouteEntry>& candidates) const
{
    candidates.clear();
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
    auto selected = router.node->selectBestRoute(prefix, candidates, currentBest);
    return selected;
}

void SimulationEngine::disseminate(const QString& routerId, const QString& prefix, const std::optional<RouteEntry>& route, bool force)
{
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end() || !routerIt->active)
    {
        return;
    }

    for (auto peerIt = routerIt->peers.begin(); peerIt != routerIt->peers.end(); ++peerIt)
    {
        const auto& peerId = peerIt.key();
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
                if (!force && !outboundChangePending && existing != outboundRoutes->cend() &&
                    existing.value() == advertisedRouteFingerprint(*outbound))
                {
                    continue;
                }
            }
        }
        PathAttributes withdrawalAttributes;
        if (!route)
        {
            const auto outboundRoutes = routerIt->adjRibOut.constFind(peerId);
            const auto hadDelivered = outboundRoutes != routerIt->adjRibOut.cend() && outboundRoutes->contains(prefix);
            const auto hasPending = peerIt->pending.contains(prefix);
            const auto inFlightGenerations = routerIt->outboundGenerations.constFind(peerId);
            const auto hasInFlight = inFlightGenerations != routerIt->outboundGenerations.cend() &&
                                     inFlightGenerations->contains(prefix);
            auto hasOutstandingTrigger = false;
            if (hasInFlight && !outstandingTriggers_.isEmpty())
            {
                const auto generation = inFlightGenerations->value(prefix);
                if (const auto outstanding = outstandingTriggers_.constFind(generation);
                    outstanding != outstandingTriggers_.cend())
                {
                    hasOutstandingTrigger = !outstanding->isEmpty();
                }
            }
            if (!hadDelivered && !hasPending && !hasInFlight && !hasOutstandingTrigger)
            {
                // There is nothing to withdraw or cancel for this peer. Apart
                // from avoiding redundant queue work, this prevents stateful
                // plugins from materializing per-peer withdrawal metadata that
                // would be discarded immediately.
                continue;
            }
            if (hadDelivered || hasOutstandingTrigger)
            {
                withdrawalAttributes = routerIt->node->exportWithdrawal(prefix, peerIt->config);
            }
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

    TfpVersionVector priorOutstandingTriggers;
    if (!outstandingTriggers_.isEmpty())
    {
        const auto peerGenerations = routerIt->outboundGenerations.constFind(peerId);
        const auto currentGeneration = peerGenerations == routerIt->outboundGenerations.cend()
                                           ? 0
                                           : peerGenerations->value(prefix);
        if (const auto outstanding = outstandingTriggers_.find(currentGeneration);
            outstanding != outstandingTriggers_.end())
        {
            priorOutstandingTriggers = std::move(outstanding.value());
            outstandingTriggers_.erase(outstanding);
        }
    }
    const auto mergeOutstandingTriggers = [&](PendingUpdate& update)
    {
        if (priorOutstandingTriggers.isEmpty())
        {
            return;
        }
        auto* versionInfo = update.route ? &update.route->attributes.tfpVersionInfo
                                         : &update.withdrawalAttributes.tfpVersionInfo;
        if (!*versionInfo)
        {
            versionInfo->emplace();
        }
        if ((*versionInfo)->triggerVector.isEmpty())
        {
            (*versionInfo)->triggerVector = std::move(priorOutstandingTriggers);
        }
        else
        {
            mergeVersions((*versionInfo)->triggerVector, priorOutstandingTriggers);
        }
    };
    const auto rememberOutstandingTriggers = [&](quint64 generation, const PendingUpdate& queued)
    {
        const TfpVersionInfo* versionInfo = nullptr;
        if (queued.route && queued.route->attributes.tfpVersionInfo)
        {
            versionInfo = &*queued.route->attributes.tfpVersionInfo;
        }
        else if (!queued.route && queued.withdrawalAttributes.tfpVersionInfo)
        {
            versionInfo = &*queued.withdrawalAttributes.tfpVersionInfo;
        }
        if (!versionInfo || versionInfo->triggerVector.isEmpty())
        {
            return;
        }
        outstandingTriggers_.insert(generation, versionInfo->triggerVector);
    };

    if (!route)
    {
        const auto outbound = routerIt->adjRibOut.constFind(peerId);
        const auto hadDelivered = outbound != routerIt->adjRibOut.cend() && outbound->contains(prefix);
        if (!hadDelivered && priorOutstandingTriggers.isEmpty())
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
        PendingUpdate update{
            .route = std::nullopt, .withdrawalAttributes = std::move(withdrawalAttributes), .generation = generation};
        mergeOutstandingTriggers(update);
        const auto& queued = setPendingUpdate(*peerIt, prefix, std::move(update));
        rememberOutstandingTriggers(generation, queued);
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
    PendingUpdate update{.route = route, .withdrawalAttributes = {}, .generation = generation};
    mergeOutstandingTriggers(update);
    const auto& queued = setPendingUpdate(*peerIt, prefix, std::move(update));
    rememberOutstandingTriggers(generation, queued);
    cancelEmptyFlushes(*peerIt);
    if (!peerIt->flushScheduled)
    {
        peerIt->flushScheduled = true;
        const auto currentSchedulingTime = schedulingTime();
        const auto dueAt = peerIt->config.mraiMs <= 0 ? currentSchedulingTime
                                                      : std::max(currentSchedulingTime, peerIt->nextMraiAt);
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
        message.advertisedRoute = *route;
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
    lastError_.clear();
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        emit errorOccurred(lastError_);
        return;
    }
    auto linkIt = links_.find(Topology::edgeKey(a, b));
    if (linkIt == links_.end())
    {
        lastError_ = QStringLiteral("链路不存在：%1 - %2").arg(a, b);
        emit errorOccurred(lastError_);
        return;
    }
    if (linkIt->enabled == enabled)
    {
        return;
    }
    const QScopedValueRollback<std::optional<qint64>> schedulingScope(activeSchedulingAt_, schedulingTime());
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
    lastError_.clear();
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        emit errorOccurred(lastError_);
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        lastError_ = QStringLiteral("路由器不存在：%1").arg(routerId);
        emit errorOccurred(lastError_);
        return;
    }
    if (routerIt->active == enabled)
    {
        return;
    }
    const QScopedValueRollback<std::optional<qint64>> schedulingScope(activeSchedulingAt_, schedulingTime());
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
    lastError_.clear();
    const auto prefix = prefixValue.trimmed();
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        emit errorOccurred(lastError_);
        return;
    }
    if (!validIpv4Prefix(prefix))
    {
        lastError_ = QStringLiteral("前缀无效：%1").arg(prefix);
        emit errorOccurred(lastError_);
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        lastError_ = QStringLiteral("路由器不存在：%1").arg(routerId);
        emit errorOccurred(lastError_);
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
    const QScopedValueRollback<std::optional<qint64>> schedulingScope(activeSchedulingAt_, schedulingTime());
    auto route = routerIt->node->createOriginatedRoute(prefix);
    routerIt->localRoutes.insert(prefix, route);
    markActivity(QStringLiteral("prefix_advertised"), QStringLiteral("%1 · %2").arg(routerId, prefix));
    runDecision(routerId, {prefix});
    recordTopologyEvent(QStringLiteral("prefix_advertised"), {{QStringLiteral("router"), routerId}, {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::withdrawPrefix(const QString& routerId, const QString& prefixValue)
{
    lastError_.clear();
    const auto prefix = prefixValue.trimmed();
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        emit errorOccurred(lastError_);
        return;
    }
    auto routerIt = routers_.find(routerId);
    if (routerIt == routers_.end())
    {
        lastError_ = QStringLiteral("路由器不存在：%1").arg(routerId);
        emit errorOccurred(lastError_);
        return;
    }
    routerIt->config.originatedPrefixes.removeAll(prefix);
    if (!routerIt->localRoutes.contains(prefix))
    {
        return;
    }
    const QScopedValueRollback<std::optional<qint64>> schedulingScope(activeSchedulingAt_, schedulingTime());
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
        notifyConvergenceStateChanged(converged_);
        emit convergenceChanged(converged_);
        if (converged_)
        {
            const auto completedAt = now();
            const auto wallDurationMs = std::max<qint64>(0, completedAt - convergenceStartedAt_);
            const auto activeWallDurationMs = std::max<qint64>(0, lastActivityAt_ - convergenceStartedAt_);
            const auto simulationCompletedAt = std::max(lastProcessedSimulationAt_, lastSimulationActivityAt_);
            const auto simulatedActiveDurationMs =
                std::max<qint64>(0, simulationCompletedAt - convergenceStartedSimulationAt_);
            const auto simulatedDurationMs = simulatedActiveDurationMs + quietMs;
            recordTopologyEvent(QStringLiteral("converged"),
                                {{QStringLiteral("elapsed_ms"), QString::number(completedAt)},
                                 {QStringLiteral("convergence_sequence"), QString::number(++convergenceSequence_)},
                                 {QStringLiteral("started_at_ms"), QString::number(convergenceStartedAt_)},
                                 {QStringLiteral("completed_at_ms"), QString::number(completedAt)},
                                 // duration_ms is the protocol metric shown to
                                 // users: causal simulated time through the
                                 // final routing activity. Machine throughput,
                                 // logging and the confirmation window remain
                                 // available as separate diagnostics.
                                 {QStringLiteral("duration_ms"), QString::number(simulatedActiveDurationMs)},
                                 {QStringLiteral("wall_duration_ms"), QString::number(wallDurationMs)},
                                 {QStringLiteral("active_wall_duration_ms"), QString::number(activeWallDurationMs)},
                                 {QStringLiteral("simulated_active_duration_ms"), QString::number(simulatedActiveDurationMs)},
                                 {QStringLiteral("simulated_duration_ms"), QString::number(simulatedDurationMs)},
                                 {QStringLiteral("quiet_confirmation_ms"), QString::number(quietMs)},
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

QStringList SimulationEngine::pathSnapshot(const QString& routerId, const QString& prefix) const
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
    return path;
}

void SimulationEngine::requestPath(const QString& routerId, const QString& prefix)
{
    emit pathReady(routerId, prefix, pathSnapshot(routerId, prefix));
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

        qsizetype versionedPrefixCount = 0;
        qsizetype dependencyEntryCount = 0;
        qsizetype triggerEntryCount = 0;
        for (auto it = message.tfpVersionInfoByPrefix.cbegin(); it != message.tfpVersionInfoByPrefix.cend(); ++it)
        {
            ++versionedPrefixCount;
            dependencyEntryCount += it->dependencyVector.size();
            triggerEntryCount += it->triggerVector.size();
        }
        const auto prefixCount = message.nlri.size() + message.withdrawn.size();
        if (message.tfpVersionInfoByPrefix.isEmpty() && attributes.tfpVersionInfo)
        {
            versionedPrefixCount = prefixCount;
            dependencyEntryCount = prefixCount * attributes.tfpVersionInfo->dependencyVector.size();
            triggerEntryCount = prefixCount * attributes.tfpVersionInfo->triggerVector.size();
        }
        // Dependency-only bootstrap traffic is intentionally indistinguishable
        // in size from standard BGP logging. Trigger-bearing fault traffic keeps
        // compact counters for protocol diagnosis.
        const auto detailedTfpLogging = routers_.size() <= 128;
        const auto sampledTfpSummary = detailedTfpLogging || prefixCount > 1 || (message.sequence % 256U) == 0;
        if (triggerEntryCount > 0 && sampledTfpSummary)
        {
            event.details.insert(QStringLiteral("tfp_versioned_prefix_count"), QString::number(versionedPrefixCount));
            event.details.insert(QStringLiteral("tfp_dependency_entry_count"), QString::number(dependencyEntryCount));
            event.details.insert(QStringLiteral("tfp_trigger_entry_count"), QString::number(triggerEntryCount));
        }
        if (prefixCount == 1)
        {
            const auto& prefix = message.nlri.isEmpty() ? message.withdrawn.first() : message.nlri.first();
            const TfpVersionInfo* versionInfo = nullptr;
            if (const auto prefixVersionInfo = message.tfpVersionInfoByPrefix.constFind(prefix);
                prefixVersionInfo != message.tfpVersionInfoByPrefix.cend())
            {
                versionInfo = &prefixVersionInfo.value();
            }
            else if (attributes.tfpVersionInfo)
            {
                versionInfo = &*attributes.tfpVersionInfo;
            }
            // Keep a readable sample for small causal deltas (and compatibility
            // with focused diagnostics), but do not expand long vectors on every
            // fault message in a large topology.
            if (detailedTfpLogging && versionInfo && !versionInfo->triggerVector.isEmpty() &&
                versionInfo->triggerVector.size() <= 4)
            {
                event.details.insert(QStringLiteral("tfp_trigger_vector"), tfpVectorText(versionInfo->triggerVector));
            }
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
