#include "engine/SimulationEngine.hpp"

#include "model/StrictIpv4.hpp"
#include "plugin/RouterPluginRegistry.hpp"

#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <utility>

namespace bgptester
{
using Json = nlohmann::json;
namespace
{

qint64 saturatingAddMilliseconds(qint64 base, qint64 delay)
{
    const auto nonNegativeDelay = std::max<qint64>(0, delay);
    return base > std::numeric_limits<qint64>::max() - nonNegativeDelay
               ? std::numeric_limits<qint64>::max()
               : base + nonNegativeDelay;
}

class StableFingerprintBuilder final
{
public:
    void addUnsigned(quint64 value)
    {
        addByte(0x4e);
        for (int shift = 0; shift < 64; shift += 8)
        {
            addByte(static_cast<quint8>((value >> shift) & 0xffU));
        }
    }

    void addString(QStringView value)
    {
        addByte(0x53);
        addUnsigned(static_cast<quint64>(value.size()));
        for (const auto codeUnit : value)
        {
            addByte(static_cast<quint8>(codeUnit.unicode() & 0xffU));
            addByte(static_cast<quint8>((codeUnit.unicode() >> 8U) & 0xffU));
        }
    }

    QPair<quint64, quint64> result() const
    {
        return {first_, second_};
    }

private:
    void addByte(quint8 value)
    {
        first_ = (first_ ^ value) * 0x100000001b3ULL;
        second_ = (second_ ^ static_cast<quint8>(value + 0x9dU)) * 0x9e3779b185ebca87ULL;
    }

    quint64 first_ = 0xcbf29ce484222325ULL;
    quint64 second_ = 0x84222325cbf29ce4ULL;
};

QPair<quint64, quint64> advertisedRouteFingerprint(const RouteEntry& route, const TfpVersionInfo* prefixVersionInfo = nullptr)
{
    StableFingerprintBuilder fingerprint;
    fingerprint.addString(route.attributes.origin);
    fingerprint.addString(route.attributes.nextHop);
    fingerprint.addUnsigned(route.attributes.localPref);
    fingerprint.addUnsigned(route.attributes.med);
    fingerprint.addUnsigned(static_cast<quint64>(route.source));
    fingerprint.addString(route.attributes.originatorId);
    fingerprint.addUnsigned(static_cast<quint64>(route.attributes.asPath.size()));
    for (const auto asn : route.attributes.asPath)
    {
        fingerprint.addUnsigned(asn);
    }
    fingerprint.addUnsigned(static_cast<quint64>(route.attributes.clusterList.size()));
    for (const auto& cluster : route.attributes.clusterList)
    {
        fingerprint.addString(cluster);
    }
    fingerprint.addUnsigned(static_cast<quint64>(route.attributes.communities.size()));
    for (auto it = route.attributes.communities.cbegin(); it != route.attributes.communities.cend(); ++it)
    {
        fingerprint.addString(it.key());
        fingerprint.addString(it.value());
    }
    if (route.attributes.versionedPath)
        fingerprint.addString(QString::fromStdString(versionedPathJson(*route.attributes.versionedPath).dump()));
    if (!prefixVersionInfo && route.attributes.tfpVersionInfo)
    {
        prefixVersionInfo = &*route.attributes.tfpVersionInfo;
    }
    if (prefixVersionInfo && !prefixVersionInfo->dependencyVector.isEmpty())
    {
        fingerprint.addString(QStringLiteral("tfp-version-info"));
        fingerprint.addUnsigned(static_cast<quint64>(prefixVersionInfo->dependencyVector.size()));
        for (auto it = prefixVersionInfo->dependencyVector.cbegin(); it != prefixVersionInfo->dependencyVector.cend(); ++it)
        {
            fingerprint.addUnsigned(it.key().asn);
            fingerprint.addString(it.key().entityId);
            fingerprint.addUnsigned(it.value());
        }
    }
    return fingerprint.result();
}

bool pathAttributesTemplateEqual(const PathAttributes& lhs, const PathAttributes& rhs)
{
    return lhs.origin == rhs.origin && lhs.asPath == rhs.asPath && lhs.nextHop == rhs.nextHop &&
           lhs.localPref == rhs.localPref && lhs.med == rhs.med && lhs.originatorId == rhs.originatorId &&
           lhs.clusterList == rhs.clusterList && lhs.communities == rhs.communities && lhs.versionedPath == rhs.versionedPath;
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

SimulationEngine::SimulationEngine(QObject* parent, qsizetype processingQuantum, quint64 convergenceEventBudget, bool detailedProtocolTracing)
    : QObject(parent), processingQuantum_(std::max<qsizetype>(1, processingQuantum)),
      convergenceEventBudget_(std::max<quint64>(1, convergenceEventBudget)), detailedProtocolTracing_(detailedProtocolTracing)
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
}

qint64 SimulationEngine::now() const
{
    return simulationTimeMs_;
}

QDateTime SimulationEngine::simulationTimestamp() const
{
    return QDateTime::fromMSecsSinceEpoch(
        saturatingAddMilliseconds(SimulationEpochMilliseconds, simulationTimeMs_), QTimeZone::UTC);
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
    if (rejectReentrantControl(QStringLiteral("startSimulation")))
    {
        return;
    }
    lastError_.clear();
    if (running_)
    {
        stopSimulation();
    }
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
    clearRuntime();
    lastError_.clear();
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
    simulationTimeMs_ = 0;
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

    requestAllSnapshots();
    armNextEvent();
    publishStats();
}

void SimulationEngine::stopSimulation()
{
    if (rejectReentrantControl(QStringLiteral("stopSimulation")))
    {
        return;
    }
    if (!running_)
    {
        lastError_.clear();
        return;
    }
    if (!convergenceFailed_)
    {
        lastError_.clear();
    }
    // A stop is a graceful deterministic boundary, not a wall-clock sample of
    // partially processed state. This also makes `start; stop` scripts produce
    // the same transcript on fast and slow machines.
    const auto drained = runUntilConverged();
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
    if (!drained)
    {
        recordTopologyEvent(QStringLiteral("simulation_aborted"),
                            {{QStringLiteral("reason"), lastError_},
                             {QStringLiteral("processed_events"), QString::number(processedEventsInConvergence_)},
                             {QStringLiteral("event_budget"), QString::number(convergenceEventBudget_)}});
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
    activeEventBatch_ = nullptr;
    drainingToConvergence_ = false;
    eventPumpDeferred_ = false;
    convergenceFailed_ = false;
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
    processedEventsInConvergence_ = 0;
    convergenceMessageCount_ = 0;
    simulationTimeMs_ = 0;
    lastActivityAt_ = 0;
    convergenceStartedAt_ = 0;
    routingStateDirty_ = false;
    convergenceTriggerEvent_.clear();
    convergenceTriggerContext_.clear();
}

bool SimulationEngine::rejectReentrantControl(const QString& operation)
{
    if (!processingEvents_ && !controlOperationActive_)
    {
        return false;
    }
    lastError_ = QStringLiteral("仿真事件或控制回调中不能重入控制操作：%1").arg(operation);
    const auto message = lastError_;
    QMetaObject::invokeMethod(this, [this, message] { emit errorOccurred(message); }, Qt::QueuedConnection);
    return true;
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
        message.versionedPathCapability = router->node->usesVersionedPaths();
        initialEvents.push_back(ScheduledEvent{
            .dueAt = saturatingAddMilliseconds(scheduledAt, delayMs),
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
    if (drainingToConvergence_ || convergenceFailed_ || !running_ || (events_.empty() && converged_))
    {
        eventTimer_->stop();
        return;
    }
    // Wall time only decides when the engine thread gets another turn. The
    // scheduled due time is consumed by processDueEvents() as virtual time.
    eventTimer_->start(0);
}

bool SimulationEngine::runUntilConverged()
{
    if (rejectReentrantControl(QStringLiteral("runUntilConverged")))
    {
        return false;
    }
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        return false;
    }
    if (converged_)
    {
        return true;
    }
    if (convergenceFailed_ || drainingToConvergence_)
    {
        if (lastError_.isEmpty())
        {
            lastError_ = QStringLiteral("仿真无法进入收敛边界");
        }
        return false;
    }

    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
    drainingToConvergence_ = true;
    eventTimer_->stop();
    while (running_ && !converged_ && !convergenceFailed_)
    {
        if (processEventQuantum(processingQuantum_) == 0 && !converged_ && !convergenceFailed_)
        {
            lastError_ = QStringLiteral("确定性事件队列没有取得进展");
            convergenceFailed_ = true;
            emit errorOccurred(lastError_);
        }
    }
    drainingToConvergence_ = false;
    armNextEvent();
    return converged_;
}

void SimulationEngine::failConvergenceBudget()
{
    if (convergenceFailed_)
    {
        return;
    }
    convergenceFailed_ = true;
    lastError_ = QStringLiteral("仿真在一个收敛周期内达到确定性事件预算 %1；可能存在协议振荡或插件零延迟循环")
                     .arg(convergenceEventBudget_);
    recordTopologyEvent(QStringLiteral("convergence_failed"),
                        {{QStringLiteral("reason"), QStringLiteral("event_budget_exhausted")},
                         {QStringLiteral("processed_events"), QString::number(processedEventsInConvergence_)},
                         {QStringLiteral("event_budget"), QString::number(convergenceEventBudget_)}});
    emit errorOccurred(lastError_);
}

void SimulationEngine::finishConvergenceIfIdle()
{
    if (!running_ || converged_ || !events_.empty())
    {
        return;
    }

    const auto quietMs = static_cast<qint64>(std::max(0, simulation_.convergenceQuietMs));
    const auto activeCompletedAt = std::max(simulationTimeMs_, lastActivityAt_);
    const auto confirmedAt = saturatingAddMilliseconds(activeCompletedAt, quietMs);
    simulationTimeMs_ = confirmedAt;
    const auto activeDurationMs = std::max<qint64>(0, activeCompletedAt - convergenceStartedAt_);
    const auto simulatedDurationMs = std::max<qint64>(0, confirmedAt - convergenceStartedAt_);

    converged_ = true;
    recordTopologyEvent(QStringLiteral("converged"),
                        {{QStringLiteral("elapsed_ms"), QString::number(confirmedAt)},
                         {QStringLiteral("convergence_sequence"), QString::number(++convergenceSequence_)},
                         {QStringLiteral("started_at_ms"), QString::number(convergenceStartedAt_)},
                         {QStringLiteral("completed_at_ms"), QString::number(activeCompletedAt)},
                         {QStringLiteral("confirmed_at_ms"), QString::number(confirmedAt)},
                         {QStringLiteral("duration_ms"), QString::number(activeDurationMs)},
                         {QStringLiteral("simulated_active_duration_ms"), QString::number(activeDurationMs)},
                         {QStringLiteral("simulated_duration_ms"), QString::number(simulatedDurationMs)},
                         {QStringLiteral("quiet_confirmation_ms"), QString::number(quietMs)},
                         {QStringLiteral("processed_events"), QString::number(processedEventsInConvergence_)},
                         {QStringLiteral("bgp_message_count"), QString::number(convergenceMessageCount_)},
                         {QStringLiteral("trigger_event"), convergenceTriggerEvent_},
                         {QStringLiteral("trigger_context"), convergenceTriggerContext_}});
    // Commit the complete convergence record before publishing callbacks.
    // Direct signal handlers therefore cannot overwrite this cycle's fields
    // by starting a new control operation reentrantly.
    notifyConvergenceStateChanged(true);
    emit convergenceChanged(true);
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
        for (const auto& record : existing->stateRecords)
            if (std::find(update.stateRecords.begin(), update.stateRecords.end(), record) == update.stateRecords.end())
                update.stateRecords.push_back(record);
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
    const auto pendingMraiCount = peer.pendingAdvertisementCount +
                                  (simulation_.withdrawalIgnoresMrai ? 0 : peer.pendingWithdrawalCount);
    if (peer.flushScheduled && pendingMraiCount == 0)
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
            return !event.messages.isEmpty();
        case ScheduledKind::FlushMrai:
            return peerIt->state == PeerState::Established && peerIt->flushScheduled &&
                   (peerIt->pendingAdvertisementCount > 0 ||
                    (!simulation_.withdrawalIgnoresMrai && peerIt->pendingWithdrawalCount > 0)) &&
                   peerIt->mraiFlushGeneration == event.flushGeneration;
        case ScheduledKind::FlushWithdrawals:
            return peerIt->state == PeerState::Established && peerIt->withdrawalFlushScheduled && peerIt->pendingWithdrawalCount > 0 &&
                   peerIt->withdrawalFlushGeneration == event.flushGeneration;
    }
    return false;
}

quint64 SimulationEngine::pruneInvalidScheduledEvents(quint64 maximumEvents)
{
    // Invalidated sessions can leave hundreds of thousands of entries in a
    // large heap.  Rebuilding the complete heap makes a single link change
    // O(queue_size log queue_size); stale entries are cheaper to discard only
    // when they reach the head.
    const auto remainingBudget = convergenceEventBudget_ -
                                 std::min(convergenceEventBudget_, processedEventsInConvergence_);
    const auto limit = std::min(maximumEvents, remainingBudget);
    quint64 processed = 0;
    while (!events_.empty() && !scheduledEventValid(events_.top()) && processed < limit)
    {
        events_.pop();
        ++processed;
        ++processedEventsInConvergence_;
    }
    if (running_ && !converged_ && !events_.empty() && processedEventsInConvergence_ >= convergenceEventBudget_)
    {
        failConvergenceBudget();
    }
    return processed;
}

void SimulationEngine::markActivity(const QString& convergenceTriggerEvent, const QString& convergenceTriggerContext)
{
    const auto activityAt = now();
    lastActivityAt_ = activityAt;
    if (converged_)
    {
        converged_ = false;
        convergenceFailed_ = false;
        processedEventsInConvergence_ = 0;
        convergenceStartedAt_ = activityAt;
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
    if (running_ && !activeEventBatch_ && !eventTimer_->isActive())
    {
        eventTimer_->start(0);
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
    QVector<BgpMessage> sent;
    for (auto& message : messages)
    {
        message.from = from;
        message.to = to;
        if (message.guarded && !retainCurrentRoutes(message)) continue;
        if (message.guarded) commitOutbound(message);
        message.guarded = false;
        message.sequence = ++nextSequence_;
        if (detailedProtocolTracing_)
        {
            auto event = messageEvent(message);
            event.event = QStringLiteral("message_sent");
            event.router = from;
            publishEvents({std::move(event)});
        }
        sent.append(std::move(message));
    }
    messages = std::move(sent);
    if (messages.isEmpty()) return;
    constexpr qsizetype maxMessagesPerScheduledEvent = 16;
    const auto dueAt = saturatingAddMilliseconds(saturatingAddMilliseconds(now(), linkDelay(from, to)), extraDelayMs);
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

bool SimulationEngine::advanceTime(qint64 milliseconds)
{
    if (rejectReentrantControl(QStringLiteral("advanceTime")) || !running_ || convergenceFailed_ || milliseconds < 0) return false;
    QScopedValueRollback control(controlOperationActive_, true);
    QScopedValueRollback draining(drainingToConvergence_, true);
    eventTimer_->stop();
    const auto deadline = saturatingAddMilliseconds(now(), milliseconds);
    while (!events_.empty() && events_.top().dueAt <= deadline && !convergenceFailed_)
        if (processEventQuantum(processingQuantum_, deadline) == 0) break;
    if (convergenceFailed_) return false;
    if (events_.empty() && saturatingAddMilliseconds(std::max(now(), lastActivityAt_), simulation_.convergenceQuietMs) <= deadline)
        finishConvergenceIfIdle();
    simulationTimeMs_ = deadline;
    publishStats();
    return true;
}

void SimulationEngine::processDueEvents()
{
    processEventQuantum(processingQuantum_);
}

qsizetype SimulationEngine::processEventQuantum(qsizetype maximumEvents, std::optional<qint64> deadline)
{
    if (!running_ || processingEvents_ || convergenceFailed_)
    {
        return 0;
    }
    if (controlOperationActive_ && !drainingToConvergence_)
    {
        eventPumpDeferred_ = true;
        return 0;
    }
    QScopedValueRollback processingGuard(processingEvents_, true);
    qsizetype processed = 0;
    const auto quantum = std::max<qsizetype>(0, maximumEvents);
    QVector<SimulationEvent> turnEvents;
    turnEvents.reserve(quantum);
    activeEventBatch_ = &turnEvents;
    processed += static_cast<qsizetype>(pruneInvalidScheduledEvents(static_cast<quint64>(quantum)));
    while (!events_.empty() && processed < quantum && !convergenceFailed_ &&
           (!deadline || events_.top().dueAt <= *deadline))
    {
        auto event = events_.top();
        events_.pop();
        ++processed;
        ++processedEventsInConvergence_;
        Q_ASSERT(scheduledEventValid(event));
        simulationTimeMs_ = std::max(simulationTimeMs_, event.dueAt);
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
        const auto remainingQuantum = static_cast<quint64>(quantum - processed);
        processed += static_cast<qsizetype>(pruneInvalidScheduledEvents(remainingQuantum));
    }
    if (!deadline) finishConvergenceIfIdle();
    if (!converged_ && !events_.empty() && processedEventsInConvergence_ >= convergenceEventBudget_)
    {
        failConvergenceBudget();
    }
    activeEventBatch_ = nullptr;
    if (!turnEvents.isEmpty())
    {
        emit eventsGenerated(std::move(turnEvents));
    }
    if (routingStateDirty_)
    {
        routingStateDirty_ = false;
        emit routingStateChanged();
    }
    publishStats();
    armNextEvent();
    return processed;
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
    QVector<BgpMessage> withdrawalMessages;
    const auto peerGenerations = routerIt->outboundGenerations.constFind(to);
    for (auto pendingIt = peerIt->pending.begin(); pendingIt != peerIt->pending.end();)
    {
        const auto& prefix = pendingIt.key();
        const auto& pending = pendingIt.value();
        if (!pending.route)
        {
            if (!simulation_.withdrawalIgnoresMrai)
            {
                const auto current = peerGenerations == routerIt->outboundGenerations.cend() ? 0 : peerGenerations->value(prefix);
                if (current == pending.generation)
                {
                    if (!routerIt->node->usesVersionedPaths() && !withdrawalMessages.isEmpty() &&
                        pathAttributesTemplateEqual(withdrawalMessages.last().withdrawalAttributes, pending.withdrawalAttributes))
                    {
                        const auto keepCommonVersionInfo = withdrawalMessages.last().tfpVersionInfoByPrefix.isEmpty() &&
                                                           withdrawalMessages.last().withdrawalAttributes.tfpVersionInfo ==
                                                               pending.withdrawalAttributes.tfpVersionInfo;
                        if (!keepCommonVersionInfo)
                        {
                            promoteCommonTfpVersionInfoToSidecar(withdrawalMessages.last());
                        }
                        withdrawalMessages.last().withdrawn.append(prefix);
                        withdrawalMessages.last().generations.insert(prefix, pending.generation);
                        if (!keepCommonVersionInfo && pending.withdrawalAttributes.tfpVersionInfo)
                        {
                            withdrawalMessages.last().tfpVersionInfoByPrefix.insert(
                                prefix, *pending.withdrawalAttributes.tfpVersionInfo);
                        }
                    }
                    else
                    {
                        withdrawalMessages.append(
                            makeUpdateMessage(from, to, prefix, std::nullopt, pending.withdrawalAttributes, pending.generation, pending.stateRecords));
                    }
                }
                --peerIt->pendingWithdrawalCount;
                pendingIt = peerIt->pending.erase(pendingIt);
                continue;
            }
            ++pendingIt;
            continue;
        }
        const auto current = peerGenerations == routerIt->outboundGenerations.cend() ? 0 : peerGenerations->value(prefix);
        if (current == pending.generation)
        {
            if (!routerIt->node->usesVersionedPaths() && !messages.isEmpty() && messages.last().advertisedRoute &&
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
                messages.append(makeUpdateMessage(from, to, prefix, pending.route, {}, pending.generation, pending.stateRecords));
            }
        }
        --peerIt->pendingAdvertisementCount;
        pendingIt = peerIt->pending.erase(pendingIt);
    }
    messages.reserve(messages.size() + withdrawalMessages.size());
    for (auto& message : withdrawalMessages)
    {
        messages.append(std::move(message));
    }
    if (messages.isEmpty())
    {
        peerIt->nextMraiAt = now();
        return;
    }
    peerIt->nextMraiAt = saturatingAddMilliseconds(now(), peerIt->config.mraiMs);
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
            if (!routerIt->node->usesVersionedPaths() && !messages.isEmpty() &&
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
                messages.append(makeUpdateMessage(from, to, prefix, std::nullopt, pending.withdrawalAttributes, pending.generation, pending.stateRecords));
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
        routerIt->advertisedVersions[message.to].remove(prefix);
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
            if (advertisedRoute.attributes.versionedPath)
                routerIt->advertisedVersions[message.to].insert(prefix, *advertisedRoute.attributes.versionedPath);
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
    message.versionedPathCapability = fromIt->node->usesVersionedPaths();
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

    if (receiverIt->node->usesVersionedPaths() && !message.versionedPathCapability)
    {
        neighborDown(message.to, message.from);
        recordTopologyEvent(QStringLiteral("versioned_capability_rejected"), {{"router", message.to}, {"peer", message.from}});
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
        sendStateRecords(message.to);
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
        changed.unite(routerIt->node->observeStateRecords(message.stateRecords, peerIt->config));
        for (const auto& prefix : message.withdrawn)
        {
            if (routerIt->node->usesVersionedPaths())
            {
                const auto binding = message.versionedWithdrawals.constFind(prefix);
                const auto current = peerRoutes.constFind(prefix);
                bool valid = binding != message.versionedWithdrawals.cend();
                if (valid && current != peerRoutes.cend())
                {
                    const auto& info = current->attributes.versionedPath;
                    const auto head = info && !info->path.empty() ? std::optional(info->path.front()) : std::nullopt;
                    valid = info && info->advertiser == binding->advertiser && head == binding->pathHead;
                }
                if (!valid)
                {
                    recordTopologyEvent(QStringLiteral("versioned_withdrawal_rejected"),
                                        {{"router", receiver}, {"peer", sender}, {"prefix", prefix}});
                    continue;
                }
            }
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
            if (routerIt->node->usesVersionedPaths()) changed.insert(prefix);
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
    routerIt->advertisedVersions.remove(peerId);
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
    auto orderedPrefixes = changedPrefixes.values();
    orderedPrefixes.sort(Qt::CaseSensitive);
    for (const auto& prefix : orderedPrefixes)
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
        if (routeChanged && detailedProtocolTracing_)
        {
            Json path = Json::array();
            if (selected) for (auto asn : selected->attributes.asPath) path.push_back(asn);
            recordTopologyEvent(QStringLiteral("route_selected"),
                {{"router", routerId}, {"prefix", prefix}, {"learned_from", selected ? selected->learnedFrom : QString()},
                 {"as_path", QString::fromStdString(path.dump())}});
        }
        if (routeChanged || forceDissemination)
        {
            disseminate(routerId, prefix, selected, forceDissemination);
        }
        routerIt->node->decisionCompleted(prefix);
    }
    sendStateRecords(routerId);
    if (anyChange)
    {
        routingStateDirty_ = true;
    }
}

void SimulationEngine::sendStateRecords(const QString& routerId)
{
    auto& router = routers_[routerId];
    if (!router.active || !router.node->usesVersionedPaths()) return;
    for (auto peer = router.peers.cbegin(); peer != router.peers.cend(); ++peer)
    {
        if (peer->state != PeerState::Established || !messageDeliverable(routerId, peer.key())) continue;
        auto records = router.node->takeStateRecords(peer->config);
        constexpr std::size_t batchSize = 64;
        for (std::size_t first = 0; first < records.size(); first += batchSize)
        {
            BgpMessage message;
            message.type = MessageType::Update;
            const auto last = std::min(first + batchSize, records.size());
            message.stateRecords.assign(records.begin() + static_cast<std::ptrdiff_t>(first),
                                        records.begin() + static_cast<std::ptrdiff_t>(last));
            scheduleMessages(routerId, peer.key(), {std::move(message)});
        }
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
    auto prefixes = routerIt->locRib.keys();
    prefixes.sort(Qt::CaseSensitive);
    for (const auto& prefix : prefixes)
    {
        const auto routeIt = routerIt->locRib.constFind(prefix);
        auto outbound = peerIt->exportEnabled ? routerIt->node->exportRouteForPrefix(prefix, routeIt.value(), peerIt->config) : std::nullopt;
        queueAdvertisement(routerId, peerId, prefix, outbound);
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
    auto peerIds = router.adjRibIn.keys();
    peerIds.sort(Qt::CaseSensitive);
    for (const auto& peerId : peerIds)
    {
        const auto& peerRoutes = router.adjRibIn.value(peerId);
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
        if (route && peerIt->exportEnabled)
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
            .route = std::nullopt, .withdrawalAttributes = std::move(withdrawalAttributes), .generation = generation, .stateRecords = routerIt->node->withdrawalRecords(prefix)};
        mergeOutstandingTriggers(update);
        const auto& queued = setPendingUpdate(*peerIt, prefix, std::move(update));
        rememberOutstandingTriggers(generation, queued);
        cancelEmptyFlushes(*peerIt);
        if (simulation_.withdrawalIgnoresMrai && !peerIt->withdrawalFlushScheduled)
        {
            peerIt->withdrawalFlushScheduled = true;
            scheduleWithdrawalFlush(routerId, peerId);
        }
        else if (!simulation_.withdrawalIgnoresMrai && !peerIt->flushScheduled)
        {
            peerIt->flushScheduled = true;
            const auto currentSchedulingTime = now();
            const auto dueAt = peerIt->config.mraiMs <= 0 ? currentSchedulingTime
                                                          : std::max(currentSchedulingTime, peerIt->nextMraiAt);
            scheduleMraiFlush(routerId, peerId, dueAt);
        }
        return;
    }

    const auto generation = ++nextGeneration_;
    auto& generations = routerIt->outboundGenerations[peerId];
    generations[prefix] = generation;
    PendingUpdate update{.route = route, .withdrawalAttributes = {}, .generation = generation, .stateRecords = routerIt->node->withdrawalRecords(prefix)};
    mergeOutstandingTriggers(update);
    const auto& queued = setPendingUpdate(*peerIt, prefix, std::move(update));
    rememberOutstandingTriggers(generation, queued);
    cancelEmptyFlushes(*peerIt);
    if (!peerIt->flushScheduled)
    {
        peerIt->flushScheduled = true;
        const auto currentSchedulingTime = now();
        const auto dueAt = peerIt->config.mraiMs <= 0 ? currentSchedulingTime
                                                      : std::max(currentSchedulingTime, peerIt->nextMraiAt);
        scheduleMraiFlush(routerId, peerId, dueAt);
    }
}

BgpMessage SimulationEngine::makeUpdateMessage(const QString& from, const QString& to, const QString& prefix,
                                               const std::optional<RouteEntry>& route, const PathAttributes& withdrawalAttributes,
                                               quint64 generation, const std::vector<StateRecord>& records) const
{
    BgpMessage message;
    message.type = MessageType::Update;
    message.from = from;
    message.to = to;
    message.guarded = true;
    message.generations.insert(prefix, generation);
    message.stateRecords = records;
    if (route)
    {
        message.nlri.append(prefix);
        message.advertisedRoute = *route;
    }
    else
    {
        message.withdrawalAttributes = withdrawalAttributes;
        const auto& router = routers_[from];
        if (router.node->usesVersionedPaths())
        {
            const auto peer = router.advertisedVersions.constFind(to);
            if (peer == router.advertisedVersions.cend() || !peer->contains(prefix)) return message;
            const auto& old = peer->value(prefix);
            message.versionedWithdrawals.insert(prefix, WithdrawalBinding{
                old.advertiser, old.path.empty() ? std::nullopt : std::optional(old.path.front())});
        }
        message.withdrawn.append(prefix);
    }
    return message;
}

void SimulationEngine::setPeerExportEnabled(const QString& routerId, const QString& peerId, bool enabled)
{
    if (rejectReentrantControl(QStringLiteral("setPeerExportEnabled")) || !running_ || convergenceFailed_) return;
    QScopedValueRollback control(controlOperationActive_, true);
    auto router = routers_.find(routerId);
    if (router == routers_.end() || !router->peers.contains(peerId))
    {
        lastError_ = QStringLiteral("不存在的出口邻居: ") + routerId + " -> " + peerId;
        emit errorOccurred(lastError_);
        return;
    }
    auto& peer = router->peers[peerId];
    if (peer.exportEnabled == enabled) return;
    peer.exportEnabled = enabled;
    markActivity(QStringLiteral("peer_export_changed"), routerId + " -> " + peerId);
    QSet<QString> prefixes;
    for (const auto& p : router->locRib.keys()) prefixes.insert(p);
    for (const auto& p : peer.pending.keys()) prefixes.insert(p);
    for (const auto& p : router->adjRibOut[peerId].keys()) prefixes.insert(p);
    auto ordered = prefixes.values();
    ordered.sort(Qt::CaseSensitive);
    for (const auto& prefix : ordered)
    {
        const auto route = router->locRib.constFind(prefix);
        disseminate(routerId, prefix, route == router->locRib.cend() ? std::nullopt : std::optional(route.value()));
    }
    recordTopologyEvent(QStringLiteral("peer_export_changed"), {{"router", routerId}, {"peer", peerId},
                                                               {"enabled", enabled ? "true" : "false"}});
}

void SimulationEngine::setLinkState(const QString& a, const QString& b, bool enabled)
{
    if (rejectReentrantControl(QStringLiteral("setLinkState")))
    {
        return;
    }
    if (convergenceFailed_)
    {
        return;
    }
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
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
    if (rejectReentrantControl(QStringLiteral("setRouterState")))
    {
        return;
    }
    if (convergenceFailed_)
    {
        return;
    }
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
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
        routerIt->advertisedVersions.clear();
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
    if (rejectReentrantControl(QStringLiteral("originatePrefix")))
    {
        return;
    }
    if (convergenceFailed_)
    {
        return;
    }
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
    lastError_.clear();
    const auto prefix = prefixValue.trimmed();
    if (!running_)
    {
        lastError_ = QStringLiteral("仿真尚未运行");
        emit errorOccurred(lastError_);
        return;
    }
    if (!isCanonicalIpv4Prefix(prefix))
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
    auto route = routerIt->node->createOriginatedRoute(prefix);
    routerIt->localRoutes.insert(prefix, route);
    markActivity(QStringLiteral("prefix_advertised"), QStringLiteral("%1 · %2").arg(routerId, prefix));
    runDecision(routerId, {prefix});
    recordTopologyEvent(QStringLiteral("prefix_advertised"), {{QStringLiteral("router"), routerId}, {QStringLiteral("prefix"), prefix}});
}

void SimulationEngine::withdrawPrefix(const QString& routerId, const QString& prefixValue)
{
    if (rejectReentrantControl(QStringLiteral("withdrawPrefix")))
    {
        return;
    }
    if (convergenceFailed_)
    {
        return;
    }
    QScopedValueRollback controlGuard(controlOperationActive_, true);
    const auto deferredPumpGuard = qScopeGuard(
        [this]
        {
            if (eventPumpDeferred_)
            {
                eventPumpDeferred_ = false;
                armNextEvent();
            }
        });
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
    routerIt->node->localRouteWithdrawn(prefix);
    routerIt->localRoutes.remove(prefix);
    markActivity(QStringLiteral("prefix_withdrawn"), QStringLiteral("%1 · %2").arg(routerId, prefix));
    runDecision(routerId, {prefix});
    recordTopologyEvent(QStringLiteral("prefix_withdrawn"), {{QStringLiteral("router"), routerId}, {QStringLiteral("prefix"), prefix}});
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
    snapshot.policyDiagnostics = routerIt->node->diagnostics();
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
    event.timestamp = simulationTimestamp();
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
        if (message.nlri.isEmpty() && message.withdrawn.isEmpty()) event.action = QStringLiteral("STATE_RECORDS");
        if (!message.versionedWithdrawals.isEmpty())
        {
            Json bindings = Json::object(), prefixes = Json::array();
            for (auto it = message.versionedWithdrawals.cbegin(); it != message.versionedWithdrawals.cend(); ++it)
                bindings[it.key().toStdString()] = withdrawalBindingJson(it.value());
            for (const auto& prefix : message.withdrawn) prefixes.push_back(prefix.toStdString());
            event.details["mp_unreach_nlri"] = QString::fromStdString(Json{{"afi",1},{"safi",1},{"prefixes",prefixes}}.dump());
            event.details["versioned_withdraw"] = QString::fromStdString(bindings.dump());
        }
        if (!message.stateRecords.empty())
        {
            Json records = Json::array();
            for (const auto& record : message.stateRecords) records.push_back(stateRecordJson(record));
            event.details["state_records"] = QString::fromStdString(records.dump());
        }
        std::size_t extensionBytes = message.stateRecords.size() * 26 + message.versionedWithdrawals.size() * 42;
        if (attributes.versionedPath)
        {
            event.details["versioned_path"] = QString::fromStdString(versionedPathJson(*attributes.versionedPath).dump());
            extensionBytes += 20 + 12 * attributes.versionedPath->path.size();
        }
        if (extensionBytes) event.details["estimated_extension_bytes"] = QString::number(extensionBytes);
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
    event.details.insert(QStringLiteral("simulation_time_ms"), QString::number(now()));
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
    event.timestamp = simulationTimestamp();
    event.event = name;
    event.action = QStringLiteral("TOPOLOGY");
    details.insert(QStringLiteral("simulation_time_ms"), QString::number(now()));
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
