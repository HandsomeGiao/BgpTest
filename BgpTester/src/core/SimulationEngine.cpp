#include "core/SimulationEngine.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <limits>
#include <sstream>
#include <utility>

namespace bgptester
{
namespace
{

std::int64_t saturatingAddMilliseconds(std::int64_t base, std::int64_t delay)
{
    const auto nonNegativeDelay = std::max<std::int64_t>(0, delay);
    return base > std::numeric_limits<std::int64_t>::max() - nonNegativeDelay
               ? std::numeric_limits<std::int64_t>::max()
               : base + nonNegativeDelay;
}

std::string joinLines(const std::vector<std::string>& values)
{
    std::string result;
    for (const auto& value : values)
    {
        if (!result.empty())
        {
            result.push_back('\n');
        }
        result += value;
    }
    return result;
}

std::string trimAscii(std::string value)
{
    const auto whitespace = [](unsigned char value)
    {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), [&](char value) { return whitespace(value); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [&](char value) { return whitespace(value); }).base();
    return first < last ? std::string(first, last) : std::string{};
}

class BoolRollback final
{
public:
    BoolRollback(bool& value, bool replacement) : value_(value), old_(std::exchange(value, replacement))
    {
    }
    ~BoolRollback()
    {
        value_ = old_;
    }

private:
    bool& value_;
    bool old_;
};

class StableFingerprintBuilder final
{
public:
    void addUnsigned(std::uint64_t value)
    {
        addByte(0x4e);
        for (int shift = 0; shift < 64; shift += 8)
        {
            addByte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    void addString(std::string_view value)
    {
        addByte(0x53);
        addUnsigned(value.size());
        for (const auto byte : value)
        {
            addByte(static_cast<std::uint8_t>(byte));
        }
    }

    std::pair<std::uint64_t, std::uint64_t> result() const noexcept
    {
        return {first_, second_};
    }

private:
    void addByte(std::uint8_t value)
    {
        first_ = (first_ ^ value) * 0x100000001b3ULL;
        second_ = (second_ ^ static_cast<std::uint8_t>(value + 0x9dU)) * 0x9e3779b185ebca87ULL;
    }
    std::uint64_t first_ = 0xcbf29ce484222325ULL;
    std::uint64_t second_ = 0x84222325cbf29ce4ULL;
};

std::pair<std::uint64_t, std::uint64_t> advertisedRouteFingerprint(const RouteEntry& route,
                                                                   const TfpVersionInfo* prefixVersionInfo = nullptr)
{
    StableFingerprintBuilder result;
    result.addString(route.attributes.origin);
    result.addString(route.attributes.nextHop);
    result.addUnsigned(route.attributes.localPref);
    result.addUnsigned(route.attributes.med);
    result.addUnsigned(static_cast<std::uint64_t>(route.source));
    result.addString(route.attributes.originatorId);
    result.addUnsigned(route.attributes.asPath.size());
    for (const auto asn : route.attributes.asPath)
    {
        result.addUnsigned(asn);
    }
    result.addUnsigned(route.attributes.clusterList.size());
    for (const auto& cluster : route.attributes.clusterList)
    {
        result.addString(cluster);
    }
    result.addUnsigned(route.attributes.communities.size());
    for (const auto& [key, value] : route.attributes.communities)
    {
        result.addString(key);
        result.addString(value);
    }
    if (!prefixVersionInfo && route.attributes.tfpVersionInfo)
    {
        prefixVersionInfo = &*route.attributes.tfpVersionInfo;
    }
    if (prefixVersionInfo && !prefixVersionInfo->dependencyVector.empty())
    {
        result.addString("tfp-version-info");
        for (const auto& [entity, version] : prefixVersionInfo->dependencyVector)
        {
            result.addUnsigned(entity.asn);
            result.addString(entity.entityId);
            result.addUnsigned(version);
        }
    }
    return result.result();
}

void mergeVersions(TfpVersionVector& destination, const TfpVersionVector& source)
{
    for (const auto& [entity, version] : source)
    {
        const auto current = destination.find(entity);
        if (current == destination.end() || current->second < version)
        {
            destination.insert_or_assign(entity, version);
        }
    }
}

std::string tfpVectorText(const TfpVersionVector& vector)
{
    std::string result;
    for (const auto& [entity, version] : vector)
    {
        if (!result.empty())
        {
            result.push_back(',');
        }
        result += "(" + std::to_string(entity.asn) + "," + entity.entityId + ")=" + std::to_string(version);
    }
    return result;
}

} // namespace

SimulationEngine::SimulationEngine(std::size_t processingQuantum, std::uint64_t convergenceEventBudget)
    : processingQuantum_(std::max<std::size_t>(1, processingQuantum)),
      convergenceEventBudget_(std::max<std::uint64_t>(1, convergenceEventBudget))
{
}

SimulationEngine::~SimulationEngine()
{
    clearRuntime();
}

bool SimulationEngine::isRunning() const noexcept
{
    return running_;
}

bool SimulationEngine::isConverged() const noexcept
{
    return converged_;
}

const std::string& SimulationEngine::lastError() const noexcept
{
    return lastError_;
}

std::int64_t SimulationEngine::now() const noexcept
{
    return simulationTimeMs_;
}

std::int64_t SimulationEngine::simulationTimestamp() const noexcept
{
    return saturatingAddMilliseconds(SimulationEpochMilliseconds, simulationTimeMs_);
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
    if (rejectReentrantControl("startSimulation"))
    {
        return;
    }
    lastError_.clear();
    if (running_)
    {
        stopSimulation();
    }
    BoolRollback controlGuard(controlOperationActive_, true);
    clearRuntime();
    simulation_ = topology.simulation;
    startupProgress("正在校验拓扑", 0, static_cast<std::int64_t>(topology.routers.size() + topology.links.size()));
    const auto problems = topology.validate();
    if (!problems.empty())
    {
        lastError_ = "无法启动仿真：\n" + joinLines(problems);
        recordTopologyEvent("simulation_start_failed", {{"error", lastError_}});
        errorOccurred(lastError_);
        clearRuntime();
        return;
    }
    if (startupCancelRequested_.load(std::memory_order_acquire))
    {
        recordTopologyEvent("simulation_start_cancelled");
        startupCancelled();
        clearRuntime();
        return;
    }
    recordTopologyEvent("simulation_initializing", {{"name", simulation_.name},
                                                     {"routers", std::to_string(topology.routers.size())},
                                                     {"links", std::to_string(topology.links.size())}});
    std::string policyError;
    if (!buildRuntime(topology, &policyError))
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            recordTopologyEvent("simulation_start_cancelled");
            startupCancelled();
        }
        else
        {
            lastError_ = "无法启动仿真：\n" + policyError;
            recordTopologyEvent("simulation_start_failed", {{"error", policyError}});
            errorOccurred(lastError_);
        }
        clearRuntime();
        return;
    }
    simulationTimeMs_ = 0;
    lastActivityAt_ = 0;
    convergenceStartedAt_ = 0;
    convergenceSequence_ = 0;
    convergenceMessageCount_ = 0;
    convergenceTriggerEvent_ = "simulation_started";
    convergenceTriggerContext_ = simulation_.name;
    converged_ = false;
    for (auto& [id, router] : routers_)
    {
        (void)id;
        router.policy->simulationStarted();
        router.policy->routerStateChanged(true);
        router.policy->convergenceStateChanged(false);
    }
    policyLifecycleActive_ = true;
    startupProgress("正在建立 BGP 会话", 0, static_cast<std::int64_t>(links_.size()));
    if (startupCancelRequested_.load(std::memory_order_acquire) || !scheduleInitialOpenMessages())
    {
        recordTopologyEvent("simulation_start_cancelled");
        startupCancelled();
        clearRuntime();
        return;
    }
    running_ = true;
    runningChanged(true);
    convergenceChanged(false);
    recordTopologyEvent("simulation_started", {{"name", simulation_.name},
                                                {"routers", std::to_string(routers_.size())},
                                                {"links", std::to_string(links_.size())}});
    requestAllSnapshots();
    publishStats();
}

void SimulationEngine::stopSimulation()
{
    if (rejectReentrantControl("stopSimulation"))
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
    const auto drained = runUntilConverged();
    BoolRollback controlGuard(controlOperationActive_, true);
    if (!drained)
    {
        recordTopologyEvent("simulation_aborted", {{"reason", lastError_},
                                                    {"processed_events", std::to_string(processedEventsInConvergence_)},
                                                    {"event_budget", std::to_string(convergenceEventBudget_)}});
    }
    recordTopologyEvent("simulation_stopped");
    running_ = false;
    const auto wasConverged = std::exchange(converged_, false);
    if (wasConverged)
    {
        notifyConvergenceStateChanged(false);
    }
    events_ = {};
    for (auto& [id, router] : routers_)
    {
        (void)id;
        router.active = false;
        router.policy->routerStateChanged(false);
        for (auto& [peerId, peer] : router.peers)
        {
            (void)peerId;
            peer.state = PeerState::Idle;
            router.policy->peerStateChanged(peer.config, peer.state);
            clearPendingUpdates(peer);
            peer.flushScheduled = false;
            peer.withdrawalFlushScheduled = false;
        }
        router.outboundGenerations.clear();
        router.policy->simulationStopped();
    }
    outstandingTriggers_.clear();
    policyLifecycleActive_ = false;
    requestAllSnapshots();
    publishStats();
    convergenceChanged(false);
    runningChanged(false);
}

void SimulationEngine::clearRuntime()
{
    activeEventBatch_ = nullptr;
    drainingToConvergence_ = false;
    convergenceFailed_ = false;
    events_ = {};
    if (policyLifecycleActive_)
    {
        for (auto& [id, router] : routers_)
        {
            (void)id;
            router.policy->simulationStopped();
        }
    }
    policyLifecycleActive_ = false;
    routers_.clear();
    links_.clear();
    topologyRouters_.reset();
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

bool SimulationEngine::rejectReentrantControl(std::string_view operation)
{
    if (!processingEvents_ && !controlOperationActive_)
    {
        return false;
    }
    lastError_ = "仿真事件或控制回调中不能重入控制操作：" + std::string(operation);
    errorOccurred(lastError_);
    return true;
}

bool SimulationEngine::buildRuntime(const Topology& topology, std::string* error)
{
    if (error)
    {
        error->clear();
    }
    startupProgress("正在构建邻接索引", 0, static_cast<std::int64_t>(topology.links.size()));
    const auto neighborIndex = topology.buildNeighborIndex();
    topologyRouters_ = std::make_shared<const RouterMap>(topology.routers);
    startupProgress("正在创建路由器运行时", 0, static_cast<std::int64_t>(topology.routers.size()));
    std::int64_t completed = 0;
    for (const auto& [id, config] : topology.routers)
    {
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            if (error)
            {
                *error = "仿真启动已取消";
            }
            return false;
        }
        RouterRuntime runtime;
        runtime.config = config;
        runtime.active = true;
        const auto neighborsIt = neighborIndex.find(id);
        const NeighborMap neighbors = neighborsIt == neighborIndex.end() ? NeighborMap{} : neighborsIt->second;
        for (const auto& [peerId, neighbor] : neighbors)
        {
            runtime.peers.emplace(peerId, PeerRuntime{.config = neighbor, .nextMraiAt = neighbor.mraiMs});
        }
        RouterPolicyContext context{runtime.config, topologyRouters_, neighbors};
        std::string creationError;
        runtime.policy = RouterPolicyRegistry::instance().createRouterPolicy(std::move(context), &creationError);
        if (!runtime.policy)
        {
            if (error)
            {
                *error = creationError;
            }
            return false;
        }
        const auto configurationProblems = runtime.policy->validateConfiguration();
        if (!configurationProblems.empty())
        {
            if (error)
            {
                *error = "路由器 " + runtime.config.id + " 的插件配置无效：\n" + joinLines(configurationProblems);
            }
            return false;
        }
        for (const auto& prefix : runtime.config.originatedPrefixes)
        {
            auto route = runtime.policy->createOriginatedRoute(prefix);
            runtime.localRoutes.insert_or_assign(prefix, route);
            runtime.locRib.insert_or_assign(prefix, std::move(route));
        }
        routers_.emplace(id, std::move(runtime));
        if ((++completed & 0xff) == 0 || completed == static_cast<std::int64_t>(topology.routers.size()))
        {
            startupProgress("正在创建路由器运行时", completed, static_cast<std::int64_t>(topology.routers.size()));
        }
    }
    startupProgress("正在建立链路运行时", 0, static_cast<std::int64_t>(topology.links.size()));
    completed = 0;
    for (const auto& link : topology.links)
    {
        links_.insert_or_assign(Topology::edgeKey(link.a, link.b), link);
        if ((++completed & 0x3ff) == 0 || completed == static_cast<std::int64_t>(topology.links.size()))
        {
            startupProgress("正在建立链路运行时", completed, static_cast<std::int64_t>(topology.links.size()));
        }
    }
    return true;
}

bool SimulationEngine::scheduleInitialOpenMessages()
{
    std::vector<ScheduledEvent> initialEvents;
    initialEvents.reserve(links_.size() * 2);
    const auto appendOpen = [&](const std::string& from, const std::string& to, int delay)
    {
        const auto router = routers_.find(from);
        if (router == routers_.end() || !router->second.active)
        {
            return;
        }
        const auto peer = router->second.peers.find(to);
        if (peer == router->second.peers.end() || !peer->second.config.enabled)
        {
            return;
        }
        peer->second.state = PeerState::OpenSent;
        router->second.policy->peerStateChanged(peer->second.config, peer->second.state);
        BgpMessage message;
        message.type = MessageType::Open;
        message.from = from;
        message.to = to;
        message.sequence = ++nextSequence_;
        message.openAsn = router->second.config.asn;
        message.openRouterId = router->second.config.routerId;
        initialEvents.push_back(ScheduledEvent{.dueAt = saturatingAddMilliseconds(now(), delay),
                                               .order = ++nextOrder_,
                                               .kind = ScheduledKind::DeliverMessages,
                                               .from = from,
                                               .to = to,
                                               .sessionEpoch = peer->second.sessionEpoch,
                                               .messages = {std::move(message)}});
    };
    std::int64_t completed = 0;
    for (const auto& [key, link] : links_)
    {
        (void)key;
        if (startupCancelRequested_.load(std::memory_order_acquire))
        {
            return false;
        }
        if (link.enabled)
        {
            appendOpen(link.a, link.b, link.delayMs);
            appendOpen(link.b, link.a, link.delayMs);
        }
        if ((++completed & 0x3ff) == 0 || completed == static_cast<std::int64_t>(links_.size()))
        {
            startupProgress("正在建立 BGP 会话", completed, static_cast<std::int64_t>(links_.size()));
        }
    }
    events_ = decltype(events_)(LaterEvent{}, std::move(initialEvents));
    if (!events_.empty())
    {
        markActivity();
    }
    return true;
}

bool SimulationEngine::runUntilConverged()
{
    if (rejectReentrantControl("runUntilConverged"))
    {
        return false;
    }
    if (!running_)
    {
        lastError_ = "仿真尚未运行";
        return false;
    }
    if (converged_)
    {
        return true;
    }
    if (convergenceFailed_ || drainingToConvergence_)
    {
        if (lastError_.empty())
        {
            lastError_ = "仿真无法进入收敛边界";
        }
        return false;
    }
    BoolRollback controlGuard(controlOperationActive_, true);
    drainingToConvergence_ = true;
    while (running_ && !converged_ && !convergenceFailed_)
    {
        if (processEventQuantum(processingQuantum_) == 0 && !converged_ && !convergenceFailed_)
        {
            lastError_ = "确定性事件队列没有取得进展";
            convergenceFailed_ = true;
            errorOccurred(lastError_);
        }
    }
    drainingToConvergence_ = false;
    return converged_;
}

void SimulationEngine::failConvergenceBudget()
{
    if (convergenceFailed_)
    {
        return;
    }
    convergenceFailed_ = true;
    lastError_ = "仿真在一个收敛周期内达到确定性事件预算 " + std::to_string(convergenceEventBudget_) +
                 "；可能存在协议振荡或插件零延迟循环";
    recordTopologyEvent("convergence_failed", {{"reason", "event_budget_exhausted"},
                                                {"processed_events", std::to_string(processedEventsInConvergence_)},
                                                {"event_budget", std::to_string(convergenceEventBudget_)}});
    errorOccurred(lastError_);
}

void SimulationEngine::finishConvergenceIfIdle()
{
    if (!running_ || converged_ || !events_.empty())
    {
        return;
    }
    const auto quietMs = static_cast<std::int64_t>(std::max(0, simulation_.convergenceQuietMs));
    const auto activeCompletedAt = std::max(simulationTimeMs_, lastActivityAt_);
    const auto confirmedAt = saturatingAddMilliseconds(activeCompletedAt, quietMs);
    simulationTimeMs_ = confirmedAt;
    const auto activeDuration = std::max<std::int64_t>(0, activeCompletedAt - convergenceStartedAt_);
    const auto simulatedDuration = std::max<std::int64_t>(0, confirmedAt - convergenceStartedAt_);
    converged_ = true;
    recordTopologyEvent("converged", {{"elapsed_ms", std::to_string(confirmedAt)},
                                      {"convergence_sequence", std::to_string(++convergenceSequence_)},
                                      {"started_at_ms", std::to_string(convergenceStartedAt_)},
                                      {"completed_at_ms", std::to_string(activeCompletedAt)},
                                      {"confirmed_at_ms", std::to_string(confirmedAt)},
                                      {"duration_ms", std::to_string(activeDuration)},
                                      {"simulated_duration_ms", std::to_string(simulatedDuration)},
                                      {"quiet_confirmation_ms", std::to_string(quietMs)},
                                      {"processed_events", std::to_string(processedEventsInConvergence_)},
                                      {"bgp_message_count", std::to_string(convergenceMessageCount_)},
                                      {"trigger_event", convergenceTriggerEvent_},
                                      {"trigger_context", convergenceTriggerContext_}});
    notifyConvergenceStateChanged(true);
    convergenceChanged(true);
}

void SimulationEngine::markActivity(std::string convergenceTriggerEvent, std::string convergenceTriggerContext)
{
    const auto activityAt = now();
    lastActivityAt_ = std::max(lastActivityAt_, activityAt);
    if (converged_)
    {
        converged_ = false;
        convergenceStartedAt_ = activityAt;
        processedEventsInConvergence_ = 0;
        convergenceMessageCount_ = 0;
        notifyConvergenceStateChanged(false);
        convergenceChanged(false);
    }
    if (!convergenceTriggerEvent.empty())
    {
        convergenceTriggerEvent_ = std::move(convergenceTriggerEvent);
        convergenceTriggerContext_ = std::move(convergenceTriggerContext);
    }
}

void SimulationEngine::notifyConvergenceStateChanged(bool converged)
{
    for (auto& [id, router] : routers_)
    {
        (void)id;
        router.policy->convergenceStateChanged(converged);
    }
}

void SimulationEngine::publishStats()
{
    const auto stats = statsSnapshot();
    statsChanged(stats);
}

SimulationStats SimulationEngine::statsSnapshot() const
{
    return SimulationStats{.running = running_,
                           .converged = converged_,
                           .pendingEvents = events_.size(),
                           .deliveredMessages = deliveredMessages_,
                           .elapsedMs = now(),
                           .convergenceElapsedMs = std::max<std::int64_t>(0, now() - convergenceStartedAt_),
                           .convergenceTriggerEvent = convergenceTriggerEvent_,
                           .convergenceTriggerContext = convergenceTriggerContext_};
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

const SimulationEngine::PendingUpdate& SimulationEngine::setPendingUpdate(PeerRuntime& peer, const std::string& prefix,
                                                                           PendingUpdate update)
{
    const auto existing = peer.pending.find(prefix);
    if (existing != peer.pending.end())
    {
        if (existing->second.route)
        {
            --peer.pendingAdvertisementCount;
        }
        else
        {
            --peer.pendingWithdrawalCount;
        }
        existing->second = std::move(update);
        if (existing->second.route)
        {
            ++peer.pendingAdvertisementCount;
        }
        else
        {
            ++peer.pendingWithdrawalCount;
        }
        return existing->second;
    }
    if (update.route)
    {
        ++peer.pendingAdvertisementCount;
    }
    else
    {
        ++peer.pendingWithdrawalCount;
    }
    return peer.pending.emplace(prefix, std::move(update)).first->second;
}

void SimulationEngine::removePendingUpdate(PeerRuntime& peer, const std::string& prefix)
{
    const auto existing = peer.pending.find(prefix);
    if (existing == peer.pending.end())
    {
        return;
    }
    if (existing->second.route)
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
    const auto pendingMrai = peer.pendingAdvertisementCount +
                             (simulation_.withdrawalIgnoresMrai ? std::size_t{0} : peer.pendingWithdrawalCount);
    if (peer.flushScheduled && pendingMrai == 0)
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

std::optional<std::uint64_t> SimulationEngine::sessionEpoch(const std::string& from, const std::string& to) const
{
    const auto router = routers_.find(from);
    if (router == routers_.end())
    {
        return std::nullopt;
    }
    const auto peer = router->second.peers.find(to);
    return peer == router->second.peers.end() ? std::nullopt : std::optional<std::uint64_t>(peer->second.sessionEpoch);
}

bool SimulationEngine::guardedMessageHasCurrentRoutes(const BgpMessage& message) const
{
    if (!message.guarded)
    {
        return true;
    }
    const auto router = routers_.find(message.from);
    if (router == routers_.end())
    {
        return false;
    }
    const auto generations = router->second.outboundGenerations.find(message.to);
    if (generations == router->second.outboundGenerations.end())
    {
        return false;
    }
    const auto current = [&](const std::string& prefix)
    {
        const auto expected = message.generations.find(prefix);
        const auto actual = generations->second.find(prefix);
        return expected != message.generations.end() && actual != generations->second.end() && actual->second == expected->second;
    };
    return std::any_of(message.nlri.cbegin(), message.nlri.cend(), current) ||
           std::any_of(message.withdrawn.cbegin(), message.withdrawn.cend(), current);
}

bool SimulationEngine::scheduledEventValid(const ScheduledEvent& event) const
{
    const auto router = routers_.find(event.from);
    if (router == routers_.end() || !messageDeliverable(event.from, event.to))
    {
        return false;
    }
    const auto peer = router->second.peers.find(event.to);
    if (peer == router->second.peers.end() || peer->second.sessionEpoch != event.sessionEpoch)
    {
        return false;
    }
    switch (event.kind)
    {
        case ScheduledKind::DeliverMessages:
            return std::any_of(event.messages.cbegin(), event.messages.cend(),
                               [this](const auto& message) { return guardedMessageHasCurrentRoutes(message); });
        case ScheduledKind::FlushMrai:
            return peer->second.flushScheduled && peer->second.mraiFlushGeneration == event.flushGeneration;
        case ScheduledKind::FlushWithdrawals:
            return peer->second.withdrawalFlushScheduled && peer->second.withdrawalFlushGeneration == event.flushGeneration;
    }
    return false;
}

std::uint64_t SimulationEngine::pruneInvalidScheduledEvents(std::uint64_t maximumEvents)
{
    std::uint64_t processed = 0;
    const auto remaining = convergenceEventBudget_ > processedEventsInConvergence_
                               ? convergenceEventBudget_ - processedEventsInConvergence_
                               : 0;
    const auto limit = std::min(maximumEvents, remaining);
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

void SimulationEngine::scheduleMessages(const std::string& from, const std::string& to, std::vector<BgpMessage> messages,
                                        int extraDelayMs)
{
    const auto epoch = sessionEpoch(from, to);
    if (!running_ || messages.empty() || !epoch || !messageDeliverable(from, to))
    {
        return;
    }
    for (auto& message : messages)
    {
        message.from = from;
        message.to = to;
        message.sequence = ++nextSequence_;
    }
    constexpr std::size_t MaximumMessagesPerEvent = 256;
    const auto dueAt = saturatingAddMilliseconds(saturatingAddMilliseconds(now(), linkDelay(from, to)), extraDelayMs);
    for (std::size_t offset = 0; offset < messages.size(); offset += MaximumMessagesPerEvent)
    {
        const auto count = std::min(MaximumMessagesPerEvent, messages.size() - offset);
        std::vector<BgpMessage> chunk;
        chunk.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            chunk.push_back(std::move(messages[offset + index]));
        }
        events_.push(ScheduledEvent{.dueAt = dueAt,
                                    .order = ++nextOrder_,
                                    .kind = ScheduledKind::DeliverMessages,
                                    .from = from,
                                    .to = to,
                                    .sessionEpoch = *epoch,
                                    .messages = std::move(chunk)});
    }
    markActivity();
}

void SimulationEngine::scheduleMraiFlush(const std::string& from, const std::string& to, std::int64_t dueAt)
{
    const auto router = routers_.find(from);
    if (!running_ || router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(to);
    if (peer == router->second.peers.end())
    {
        return;
    }
    peer->second.flushScheduled = true;
    const auto generation = ++peer->second.mraiFlushGeneration;
    events_.push(ScheduledEvent{.dueAt = std::max(now(), dueAt),
                                .order = ++nextOrder_,
                                .kind = ScheduledKind::FlushMrai,
                                .from = from,
                                .to = to,
                                .sessionEpoch = peer->second.sessionEpoch,
                                .flushGeneration = generation});
    markActivity();
}

void SimulationEngine::scheduleWithdrawalFlush(const std::string& from, const std::string& to)
{
    const auto router = routers_.find(from);
    if (!running_ || router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(to);
    if (peer == router->second.peers.end())
    {
        return;
    }
    peer->second.withdrawalFlushScheduled = true;
    const auto generation = ++peer->second.withdrawalFlushGeneration;
    events_.push(ScheduledEvent{.dueAt = now(),
                                .order = ++nextOrder_,
                                .kind = ScheduledKind::FlushWithdrawals,
                                .from = from,
                                .to = to,
                                .sessionEpoch = peer->second.sessionEpoch,
                                .flushGeneration = generation});
    markActivity();
}

std::size_t SimulationEngine::processEventQuantum(std::size_t maximumEvents)
{
    if (!running_ || processingEvents_ || convergenceFailed_)
    {
        return 0;
    }
    BoolRollback processingGuard(processingEvents_, true);
    std::size_t processed = 0;
    std::vector<SimulationEvent> batch;
    batch.reserve(maximumEvents);
    activeEventBatch_ = &batch;
    processed += static_cast<std::size_t>(pruneInvalidScheduledEvents(maximumEvents));
    while (!events_.empty() && processed < maximumEvents && !convergenceFailed_)
    {
        auto event = events_.top();
        events_.pop();
        ++processed;
        ++processedEventsInConvergence_;
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
        processed += static_cast<std::size_t>(pruneInvalidScheduledEvents(maximumEvents - processed));
    }
    finishConvergenceIfIdle();
    if (!converged_ && !events_.empty() && processedEventsInConvergence_ >= convergenceEventBudget_)
    {
        failConvergenceBudget();
    }
    activeEventBatch_ = nullptr;
    if (!batch.empty())
    {
        eventsGenerated(batch);
    }
    if (routingStateDirty_)
    {
        routingStateDirty_ = false;
        routingStateChanged();
    }
    publishStats();
    return processed;
}

void SimulationEngine::deliverMessages(ScheduledEvent& event)
{
    if (!messageDeliverable(event.from, event.to))
    {
        return;
    }
    std::vector<BgpMessage> updates;
    std::vector<SimulationEvent> recorded;
    recorded.reserve(event.messages.size());
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
        recorded.push_back(messageEvent(message));
        ++deliveredMessages_;
        ++convergenceMessageCount_;
        markActivity();
        switch (message.type)
        {
            case MessageType::Open:
                handleOpen(message);
                break;
            case MessageType::Update:
                updates.push_back(std::move(message));
                break;
            case MessageType::Notification:
                handleNotification(message);
                break;
        }
    }
    if (!updates.empty())
    {
        handleUpdateBatch(event.to, event.from, updates);
    }
    publishEvents(std::move(recorded));
}

void SimulationEngine::flushMrai(const std::string& from, const std::string& to)
{
    const auto router = routers_.find(from);
    if (router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(to);
    if (peer == router->second.peers.end())
    {
        return;
    }
    peer->second.flushScheduled = false;
    if (!router->second.active || peer->second.state != PeerState::Established || !messageDeliverable(from, to))
    {
        return;
    }
    std::vector<BgpMessage> messages;
    for (auto it = peer->second.pending.begin(); it != peer->second.pending.end();)
    {
        if (!it->second.route && simulation_.withdrawalIgnoresMrai)
        {
            ++it;
            continue;
        }
        const auto current = router->second.outboundGenerations[to].find(it->first);
        if (current == router->second.outboundGenerations[to].end() || current->second != it->second.generation)
        {
            const auto prefix = it->first;
            ++it;
            removePendingUpdate(peer->second, prefix);
            continue;
        }
        messages.push_back(makeUpdateMessage(from, to, it->first, it->second.route, it->second.withdrawalAttributes,
                                             it->second.generation));
        const auto prefix = it->first;
        ++it;
        removePendingUpdate(peer->second, prefix);
    }
    cancelEmptyFlushes(peer->second);
    if (messages.empty())
    {
        return;
    }
    peer->second.nextMraiAt = saturatingAddMilliseconds(now(), peer->second.config.mraiMs);
    scheduleMessages(from, to, std::move(messages));
}

void SimulationEngine::flushWithdrawals(const std::string& from, const std::string& to)
{
    const auto router = routers_.find(from);
    if (router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(to);
    if (peer == router->second.peers.end())
    {
        return;
    }
    peer->second.withdrawalFlushScheduled = false;
    if (!router->second.active || peer->second.state != PeerState::Established || !messageDeliverable(from, to))
    {
        return;
    }
    std::vector<BgpMessage> messages;
    for (auto it = peer->second.pending.begin(); it != peer->second.pending.end();)
    {
        if (it->second.route)
        {
            ++it;
            continue;
        }
        const auto current = router->second.outboundGenerations[to].find(it->first);
        if (current != router->second.outboundGenerations[to].end() && current->second == it->second.generation)
        {
            messages.push_back(makeUpdateMessage(from, to, it->first, std::nullopt, it->second.withdrawalAttributes,
                                                 it->second.generation));
        }
        const auto prefix = it->first;
        ++it;
        removePendingUpdate(peer->second, prefix);
    }
    cancelEmptyFlushes(peer->second);
    scheduleMessages(from, to, std::move(messages));
}

bool SimulationEngine::messageDeliverable(const std::string& from, const std::string& to) const
{
    const auto source = routers_.find(from);
    const auto destination = routers_.find(to);
    if (source == routers_.end() || destination == routers_.end() || !source->second.active || !destination->second.active)
    {
        return false;
    }
    const auto link = links_.find(Topology::edgeKey(from, to));
    if (link == links_.end() || !link->second.enabled)
    {
        return false;
    }
    const auto peer = source->second.peers.find(to);
    return peer != source->second.peers.end() && peer->second.config.enabled;
}

int SimulationEngine::linkDelay(const std::string& from, const std::string& to) const
{
    const auto link = links_.find(Topology::edgeKey(from, to));
    return link == links_.end() ? 0 : std::max(0, link->second.delayMs);
}

bool SimulationEngine::retainCurrentRoutes(BgpMessage& message) const
{
    const auto router = routers_.find(message.from);
    if (router == routers_.end())
    {
        return false;
    }
    const auto generations = router->second.outboundGenerations.find(message.to);
    if (generations == router->second.outboundGenerations.end())
    {
        return false;
    }
    const auto current = [&](const std::string& prefix)
    {
        const auto expected = message.generations.find(prefix);
        const auto actual = generations->second.find(prefix);
        return expected != message.generations.end() && actual != generations->second.end() && actual->second == expected->second;
    };
    std::vector<std::string> nlri;
    std::vector<std::string> withdrawn;
    decltype(message.generations) keptGenerations;
    decltype(message.tfpVersionInfoByPrefix) keptVersions;
    const auto retain = [&](const std::string& prefix, std::vector<std::string>& destination)
    {
        if (!current(prefix))
        {
            return;
        }
        destination.push_back(prefix);
        keptGenerations.emplace(prefix, message.generations.at(prefix));
        if (const auto version = message.tfpVersionInfoByPrefix.find(prefix); version != message.tfpVersionInfoByPrefix.end())
        {
            keptVersions.emplace(prefix, version->second);
        }
    };
    for (const auto& prefix : message.nlri)
    {
        retain(prefix, nlri);
    }
    for (const auto& prefix : message.withdrawn)
    {
        retain(prefix, withdrawn);
    }
    message.nlri = std::move(nlri);
    message.withdrawn = std::move(withdrawn);
    message.generations = std::move(keptGenerations);
    message.tfpVersionInfoByPrefix = std::move(keptVersions);
    return !message.nlri.empty() || !message.withdrawn.empty();
}

void SimulationEngine::commitOutbound(const BgpMessage& message)
{
    const auto router = routers_.find(message.from);
    if (router == routers_.end())
    {
        return;
    }
    auto& outbound = router->second.adjRibOut[message.to];
    for (const auto& prefix : message.withdrawn)
    {
        outbound.erase(prefix);
    }
    if (message.advertisedRoute)
    {
        for (const auto& prefix : message.nlri)
        {
            const TfpVersionInfo* versionInfo = nullptr;
            if (const auto version = message.tfpVersionInfoByPrefix.find(prefix); version != message.tfpVersionInfoByPrefix.end())
            {
                versionInfo = &version->second;
            }
            outbound.insert_or_assign(prefix, advertisedRouteFingerprint(*message.advertisedRoute, versionInfo));
        }
    }
    const auto generations = router->second.outboundGenerations.find(message.to);
    if (generations == router->second.outboundGenerations.end())
    {
        return;
    }
    for (const auto& [prefix, generation] : message.generations)
    {
        const auto current = generations->second.find(prefix);
        if (current != generations->second.end() && current->second == generation)
        {
            outstandingTriggers_.erase(generation);
        }
    }
}

void SimulationEngine::sendOpen(const std::string& from, const std::string& to)
{
    const auto router = routers_.find(from);
    if (router == routers_.end() || !messageDeliverable(from, to))
    {
        return;
    }
    const auto peer = router->second.peers.find(to);
    if (peer == router->second.peers.end())
    {
        return;
    }
    if (peer->second.state != PeerState::Established)
    {
        peer->second.state = PeerState::OpenSent;
        router->second.policy->peerStateChanged(peer->second.config, peer->second.state);
    }
    BgpMessage message;
    message.type = MessageType::Open;
    message.openAsn = router->second.config.asn;
    message.openRouterId = router->second.config.routerId;
    scheduleMessages(from, to, {std::move(message)});
}

void SimulationEngine::handleOpen(const BgpMessage& message)
{
    const auto receiver = routers_.find(message.to);
    if (receiver == routers_.end())
    {
        return;
    }
    const auto peer = receiver->second.peers.find(message.from);
    if (peer == receiver->second.peers.end())
    {
        return;
    }
    if (message.openAsn != peer->second.config.remoteAsn)
    {
        BgpMessage notification;
        notification.type = MessageType::Notification;
        notification.errorCode = 2;
        notification.errorSubcode = 2;
        notification.errorData = "Bad Peer AS";
        scheduleMessages(message.to, message.from, {std::move(notification)});
        neighborDown(message.to, message.from);
        return;
    }
    const auto wasEstablished = peer->second.state == PeerState::Established;
    if (peer->second.state == PeerState::Idle)
    {
        sendOpen(message.to, message.from);
    }
    peer->second.state = PeerState::Established;
    receiver->second.policy->peerStateChanged(peer->second.config, peer->second.state);
    if (!wasEstablished)
    {
        advertiseTableToPeer(message.to, message.from);
    }
}

void SimulationEngine::handleUpdateBatch(const std::string& receiverId, const std::string& sender,
                                         const std::vector<BgpMessage>& messages)
{
    const auto router = routers_.find(receiverId);
    if (router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(sender);
    if (peer == router->second.peers.end() || peer->second.state != PeerState::Established)
    {
        return;
    }
    auto& peerRoutes = router->second.adjRibIn[sender];
    std::unordered_set<std::string> changed;
    for (const auto& message : messages)
    {
        for (const auto& prefix : message.withdrawn)
        {
            auto attributes = message.withdrawalAttributes;
            if (const auto version = message.tfpVersionInfoByPrefix.find(prefix); version != message.tfpVersionInfoByPrefix.end())
            {
                attributes.tfpVersionInfo = version->second;
            }
            router->second.policy->importWithdrawal(prefix, attributes, peer->second.config);
            if (peerRoutes.erase(prefix) > 0)
            {
                changed.insert(prefix);
            }
            else if (attributes.tfpVersionInfo && !attributes.tfpVersionInfo->triggerVector.empty())
            {
                changed.insert(prefix);
            }
        }
        for (const auto& prefix : message.nlri)
        {
            if (!message.advertisedRoute)
            {
                continue;
            }
            auto advertised = *message.advertisedRoute;
            if (const auto version = message.tfpVersionInfoByPrefix.find(prefix); version != message.tfpVersionInfoByPrefix.end())
            {
                advertised.attributes.tfpVersionInfo = version->second;
            }
            auto imported = router->second.policy->importAdvertisedRoute(prefix, advertised, peer->second.config);
            if (!imported)
            {
                if (peerRoutes.erase(prefix) > 0)
                {
                    changed.insert(prefix);
                }
                continue;
            }
            const auto old = peerRoutes.find(prefix);
            if (old == peerRoutes.end() || old->second != *imported)
            {
                peerRoutes.insert_or_assign(prefix, std::move(*imported));
                changed.insert(prefix);
            }
            else if (advertised.attributes.tfpVersionInfo && !advertised.attributes.tfpVersionInfo->triggerVector.empty())
            {
                changed.insert(prefix);
            }
        }
    }
    runDecision(receiverId, changed);
}

void SimulationEngine::handleNotification(const BgpMessage& message)
{
    neighborDown(message.to, message.from);
}

void SimulationEngine::neighborDown(const std::string& routerId, const std::string& peerId)
{
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(peerId);
    if (peer == router->second.peers.end())
    {
        return;
    }
    invalidateSession(peer->second);
    peer->second.state = PeerState::Idle;
    router->second.policy->peerStateChanged(peer->second.config, peer->second.state);
    if (const auto generations = router->second.outboundGenerations.find(peerId);
        generations != router->second.outboundGenerations.end())
    {
        for (const auto& [prefix, generation] : generations->second)
        {
            (void)prefix;
            outstandingTriggers_.erase(generation);
        }
    }
    router->second.outboundGenerations.erase(peerId);
    router->second.adjRibOut.erase(peerId);
    std::unordered_set<std::string> affected;
    if (const auto inbound = router->second.adjRibIn.find(peerId); inbound != router->second.adjRibIn.end())
    {
        for (const auto& [prefix, route] : inbound->second)
        {
            (void)route;
            affected.insert(prefix);
        }
        router->second.adjRibIn.erase(inbound);
    }
    if (router->second.active)
    {
        runDecision(routerId, affected);
    }
}

void SimulationEngine::runDecision(const std::string& routerId, const std::unordered_set<std::string>& changedPrefixes)
{
    const auto router = routers_.find(routerId);
    if (router == routers_.end() || !router->second.active || changedPrefixes.empty())
    {
        return;
    }
    std::vector<std::string> ordered(changedPrefixes.cbegin(), changedPrefixes.cend());
    std::sort(ordered.begin(), ordered.end());
    std::vector<RouteEntry> candidates;
    candidates.reserve(router->second.adjRibIn.size() + 1);
    bool anyChange = false;
    for (const auto& prefix : ordered)
    {
        const auto old = router->second.locRib.find(prefix);
        const std::optional<RouteEntry> currentBest = old == router->second.locRib.end()
                                                          ? std::nullopt
                                                          : std::optional<RouteEntry>(old->second);
        auto selected = selectBest(router->second, prefix, currentBest, candidates);
        const auto routeChanged = selected != currentBest;
        const auto force = router->second.policy->requiresDissemination(prefix);
        if (routeChanged)
        {
            anyChange = true;
            if (selected)
            {
                router->second.locRib.insert_or_assign(prefix, *selected);
            }
            else
            {
                router->second.locRib.erase(prefix);
            }
        }
        if (routeChanged || force)
        {
            disseminate(routerId, prefix, selected, force);
        }
        router->second.policy->decisionCompleted(prefix);
    }
    if (anyChange)
    {
        routingStateDirty_ = true;
    }
}

void SimulationEngine::advertiseTableToPeer(const std::string& routerId, const std::string& peerId)
{
    const auto router = routers_.find(routerId);
    if (router == routers_.end() || !router->second.active)
    {
        return;
    }
    const auto peer = router->second.peers.find(peerId);
    if (peer == router->second.peers.end() || peer->second.state != PeerState::Established || !peer->second.config.enabled)
    {
        return;
    }
    std::vector<std::string> prefixes;
    prefixes.reserve(router->second.locRib.size());
    for (const auto& [prefix, route] : router->second.locRib)
    {
        (void)route;
        prefixes.push_back(prefix);
    }
    std::sort(prefixes.begin(), prefixes.end());
    for (const auto& prefix : prefixes)
    {
        const auto route = router->second.locRib.find(prefix);
        auto exported = router->second.policy->exportRouteForPrefix(prefix, route->second, peer->second.config);
        queueAdvertisement(routerId, peerId, prefix, exported);
    }
}

std::optional<RouteEntry> SimulationEngine::selectBest(const RouterRuntime& router, const std::string& prefix,
                                                        const std::optional<RouteEntry>& currentBest,
                                                        std::vector<RouteEntry>& candidates) const
{
    candidates.clear();
    if (const auto local = router.localRoutes.find(prefix); local != router.localRoutes.end())
    {
        candidates.push_back(local->second);
    }
    std::vector<std::string> peerIds;
    peerIds.reserve(router.adjRibIn.size());
    for (const auto& [peerId, routes] : router.adjRibIn)
    {
        (void)routes;
        peerIds.push_back(peerId);
    }
    std::sort(peerIds.begin(), peerIds.end());
    for (const auto& peerId : peerIds)
    {
        const auto& routes = router.adjRibIn.at(peerId);
        if (const auto route = routes.find(prefix); route != routes.end())
        {
            candidates.push_back(route->second);
        }
    }
    return router.policy->selectBestRoute(prefix, candidates, currentBest);
}

void SimulationEngine::disseminate(const std::string& routerId, const std::string& prefix,
                                   const std::optional<RouteEntry>& route, bool force)
{
    const auto router = routers_.find(routerId);
    if (router == routers_.end() || !router->second.active)
    {
        return;
    }
    for (auto& [peerId, peer] : router->second.peers)
    {
        if (peer.state != PeerState::Established || !peer.config.enabled)
        {
            continue;
        }
        std::optional<RouteEntry> outbound;
        if (route)
        {
            outbound = router->second.policy->exportRouteForPrefix(prefix, *route, peer.config);
        }
        const auto advertisedPeer = router->second.adjRibOut.find(peerId);
        const auto advertised = advertisedPeer == router->second.adjRibOut.end()
                                    ? FingerprintMap::const_iterator{}
                                    : advertisedPeer->second.find(prefix);
        const auto hasAdvertised = advertisedPeer != router->second.adjRibOut.end() && advertised != advertisedPeer->second.end();
        if (outbound)
        {
            const auto fingerprint = advertisedRouteFingerprint(*outbound);
            if (!force && hasAdvertised && advertised->second == fingerprint && !peer.pending.contains(prefix))
            {
                continue;
            }
            queueAdvertisement(routerId, peerId, prefix, outbound);
        }
        else
        {
            const auto hasPending = peer.pending.contains(prefix);
            if (!force && !hasAdvertised && !hasPending)
            {
                continue;
            }
            auto withdrawalAttributes = router->second.policy->exportWithdrawal(prefix, peer.config);
            queueAdvertisement(routerId, peerId, prefix, std::nullopt, std::move(withdrawalAttributes));
        }
    }
}

void SimulationEngine::queueAdvertisement(const std::string& routerId, const std::string& peerId,
                                          const std::string& prefix, const std::optional<RouteEntry>& route,
                                          PathAttributes withdrawalAttributes)
{
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        return;
    }
    const auto peer = router->second.peers.find(peerId);
    if (peer == router->second.peers.end() || peer->second.state != PeerState::Established ||
        !messageDeliverable(routerId, peerId))
    {
        return;
    }

    TfpVersionVector inheritedTriggers;
    if (const auto existing = peer->second.pending.find(prefix); existing != peer->second.pending.end())
    {
        const auto& info = existing->second.route ? existing->second.route->attributes.tfpVersionInfo
                                                   : existing->second.withdrawalAttributes.tfpVersionInfo;
        if (info)
        {
            mergeVersions(inheritedTriggers, info->triggerVector);
        }
        outstandingTriggers_.erase(existing->second.generation);
    }
    if (const auto currentGeneration = router->second.outboundGenerations[peerId].find(prefix);
        currentGeneration != router->second.outboundGenerations[peerId].end())
    {
        if (const auto outstanding = outstandingTriggers_.find(currentGeneration->second); outstanding != outstandingTriggers_.end())
        {
            mergeVersions(inheritedTriggers, outstanding->second);
        }
    }

    PendingUpdate update{route, std::move(withdrawalAttributes), ++nextGeneration_};
    auto& info = update.route ? update.route->attributes.tfpVersionInfo : update.withdrawalAttributes.tfpVersionInfo;
    if (!inheritedTriggers.empty())
    {
        if (!info)
        {
            info.emplace();
        }
        mergeVersions(info->triggerVector, inheritedTriggers);
    }
    router->second.outboundGenerations[peerId].insert_or_assign(prefix, update.generation);
    if (info && !info->triggerVector.empty())
    {
        outstandingTriggers_.insert_or_assign(update.generation, info->triggerVector);
    }
    setPendingUpdate(peer->second, prefix, std::move(update));

    if (!route && simulation_.withdrawalIgnoresMrai)
    {
        if (!peer->second.withdrawalFlushScheduled)
        {
            scheduleWithdrawalFlush(routerId, peerId);
        }
        return;
    }
    if (!peer->second.flushScheduled)
    {
        scheduleMraiFlush(routerId, peerId, std::max(now(), peer->second.nextMraiAt));
    }
}

BgpMessage SimulationEngine::makeUpdateMessage(const std::string& from, const std::string& to,
                                               const std::string& prefix, const std::optional<RouteEntry>& route,
                                               const PathAttributes& withdrawalAttributes,
                                               std::uint64_t generation) const
{
    BgpMessage message;
    message.type = MessageType::Update;
    message.from = from;
    message.to = to;
    message.guarded = true;
    message.generations.emplace(prefix, generation);
    if (route)
    {
        message.nlri.push_back(prefix);
        message.advertisedRoute = route;
    }
    else
    {
        message.withdrawn.push_back(prefix);
        message.withdrawalAttributes = withdrawalAttributes;
    }
    return message;
}

void SimulationEngine::setLinkState(const std::string& a, const std::string& b, bool enabled)
{
    if (rejectReentrantControl("setLinkState"))
    {
        return;
    }
    if (convergenceFailed_)
    {
        errorOccurred(lastError_);
        return;
    }
    BoolRollback guard(controlOperationActive_, true);
    if (!running_)
    {
        lastError_ = "仿真尚未运行";
        errorOccurred(lastError_);
        return;
    }
    const auto link = links_.find(Topology::edgeKey(a, b));
    if (link == links_.end())
    {
        lastError_ = "链路不存在：" + a + " - " + b;
        errorOccurred(lastError_);
        return;
    }
    if (link->second.enabled == enabled)
    {
        return;
    }
    link->second.enabled = enabled;
    if (const auto router = routers_.find(a); router != routers_.end())
    {
        if (const auto peer = router->second.peers.find(b); peer != router->second.peers.end())
        {
            peer->second.config.enabled = enabled;
        }
    }
    if (const auto router = routers_.find(b); router != routers_.end())
    {
        if (const auto peer = router->second.peers.find(a); peer != router->second.peers.end())
        {
            peer->second.config.enabled = enabled;
        }
    }
    const auto event = enabled ? "link_up" : "link_down";
    markActivity(event, a + " ↔ " + b);
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
    recordTopologyEvent(event, {{"a", a}, {"b", b}});
    linkStateChanged(a, b, enabled);
}

void SimulationEngine::setRouterState(const std::string& routerId, bool enabled)
{
    if (rejectReentrantControl("setRouterState"))
    {
        return;
    }
    if (convergenceFailed_)
    {
        errorOccurred(lastError_);
        return;
    }
    BoolRollback guard(controlOperationActive_, true);
    if (!running_)
    {
        lastError_ = "仿真尚未运行";
        errorOccurred(lastError_);
        return;
    }
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        lastError_ = "路由器不存在：" + routerId;
        errorOccurred(lastError_);
        return;
    }
    if (router->second.active == enabled)
    {
        return;
    }
    const auto event = enabled ? "router_up" : "router_down";
    markActivity(event, routerId);
    if (!enabled)
    {
        router->second.active = false;
        router->second.policy->routerStateChanged(false);
        std::vector<std::string> peers;
        peers.reserve(router->second.peers.size());
        for (const auto& [peerId, peer] : router->second.peers)
        {
            (void)peer;
            peers.push_back(peerId);
        }
        for (const auto& peerId : peers)
        {
            neighborDown(routerId, peerId);
            neighborDown(peerId, routerId);
        }
        router->second.localRoutes.clear();
        router->second.locRib.clear();
        router->second.adjRibIn.clear();
        router->second.adjRibOut.clear();
        router->second.outboundGenerations.clear();
        routingStateDirty_ = true;
    }
    else
    {
        router->second.active = true;
        router->second.policy->routerStateChanged(true);
        for (const auto& prefix : router->second.config.originatedPrefixes)
        {
            auto route = router->second.policy->createOriginatedRoute(prefix);
            router->second.localRoutes.insert_or_assign(prefix, route);
            router->second.locRib.insert_or_assign(prefix, std::move(route));
        }
        for (auto& [peerId, peer] : router->second.peers)
        {
            invalidateSession(peer);
            peer.state = PeerState::Idle;
            router->second.policy->peerStateChanged(peer.config, peer.state);
            if (messageDeliverable(routerId, peerId))
            {
                sendOpen(routerId, peerId);
                sendOpen(peerId, routerId);
            }
        }
        routingStateDirty_ = true;
    }
    recordTopologyEvent(event, {{"router", routerId}});
    routerStateChanged(routerId, enabled);
}

void SimulationEngine::originatePrefix(const std::string& routerId, const std::string& prefixValue)
{
    if (rejectReentrantControl("originatePrefix"))
    {
        return;
    }
    if (convergenceFailed_)
    {
        errorOccurred(lastError_);
        return;
    }
    BoolRollback guard(controlOperationActive_, true);
    const auto prefix = trimAscii(prefixValue);
    if (!running_)
    {
        lastError_ = "仿真尚未运行";
        errorOccurred(lastError_);
        return;
    }
    if (!isCanonicalIpv4Prefix(prefix))
    {
        lastError_ = "前缀无效：" + prefix;
        errorOccurred(lastError_);
        return;
    }
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        lastError_ = "路由器不存在：" + routerId;
        errorOccurred(lastError_);
        return;
    }
    if (std::find(router->second.config.originatedPrefixes.cbegin(), router->second.config.originatedPrefixes.cend(), prefix) ==
        router->second.config.originatedPrefixes.cend())
    {
        router->second.config.originatedPrefixes.push_back(prefix);
    }
    if (!router->second.active || router->second.localRoutes.contains(prefix))
    {
        return;
    }
    auto route = router->second.policy->createOriginatedRoute(prefix);
    router->second.localRoutes.insert_or_assign(prefix, std::move(route));
    markActivity("prefix_advertised", routerId + " · " + prefix);
    runDecision(routerId, {prefix});
    recordTopologyEvent("prefix_advertised", {{"router", routerId}, {"prefix", prefix}});
}

void SimulationEngine::withdrawPrefix(const std::string& routerId, const std::string& prefixValue)
{
    if (rejectReentrantControl("withdrawPrefix"))
    {
        return;
    }
    if (convergenceFailed_)
    {
        errorOccurred(lastError_);
        return;
    }
    BoolRollback guard(controlOperationActive_, true);
    const auto prefix = trimAscii(prefixValue);
    if (!running_)
    {
        lastError_ = "仿真尚未运行";
        errorOccurred(lastError_);
        return;
    }
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        lastError_ = "路由器不存在：" + routerId;
        errorOccurred(lastError_);
        return;
    }
    std::erase(router->second.config.originatedPrefixes, prefix);
    if (!router->second.localRoutes.contains(prefix))
    {
        return;
    }
    router->second.policy->localRouteWithdrawn(prefix);
    router->second.localRoutes.erase(prefix);
    markActivity("prefix_withdrawn", routerId + " · " + prefix);
    runDecision(routerId, {prefix});
    recordTopologyEvent("prefix_withdrawn", {{"router", routerId}, {"prefix", prefix}});
}

RibSnapshot SimulationEngine::ribSnapshot(const std::string& routerId) const
{
    RibSnapshot snapshot;
    snapshot.router = routerId;
    const auto router = routers_.find(routerId);
    if (router != routers_.end())
    {
        snapshot.localRoutes = router->second.localRoutes;
        snapshot.locRib = router->second.locRib;
        snapshot.adjRibIn = router->second.adjRibIn;
    }
    return snapshot;
}

std::vector<PeerSnapshot> SimulationEngine::peerSnapshots(const std::string& routerId) const
{
    std::vector<PeerSnapshot> snapshots;
    const auto router = routers_.find(routerId);
    if (router == routers_.end())
    {
        return snapshots;
    }
    snapshots.reserve(router->second.peers.size());
    for (const auto& [id, peer] : router->second.peers)
    {
        snapshots.push_back(PeerSnapshot{.id = id,
                                         .remoteAsn = peer.config.remoteAsn,
                                         .sessionType = peer.config.sessionType,
                                         .rrClient = peer.config.rrClient,
                                         .enabled = peer.config.enabled,
                                         .mraiMs = peer.config.mraiMs,
                                         .state = peer.state,
                                         .relationship = peer.config.relationship});
    }
    return snapshots;
}

std::vector<RouterSnapshot> SimulationEngine::routerSnapshots() const
{
    std::vector<RouterSnapshot> snapshots;
    snapshots.reserve(routers_.size());
    for (const auto& [id, router] : routers_)
    {
        snapshots.push_back(RouterSnapshot{.id = id,
                                           .routerId = router.config.routerId,
                                           .asn = router.config.asn,
                                           .active = router.active,
                                           .routeReflector = hasRouteReflectorClients(router),
                                           .bestRouteCount = static_cast<int>(router.locRib.size())});
    }
    return snapshots;
}

void SimulationEngine::requestRouterSnapshot(const std::string& routerId)
{
    const auto rib = ribSnapshot(routerId);
    ribSnapshotReady(rib);
    const auto peers = peerSnapshots(routerId);
    peersSnapshotReady(routerId, peers);
}

void SimulationEngine::requestAllSnapshots()
{
    const auto snapshots = routerSnapshots();
    routerSnapshotsChanged(snapshots);
}

std::vector<std::string> SimulationEngine::pathSnapshot(const std::string& routerId, const std::string& prefix) const
{
    std::vector<std::string> path;
    std::unordered_set<std::string> seen;
    auto current = routerId;
    while (!current.empty() && !seen.contains(current))
    {
        const auto router = routers_.find(current);
        if (router == routers_.end())
        {
            break;
        }
        seen.insert(current);
        path.push_back(current);
        const auto route = router->second.locRib.find(prefix);
        if (route == router->second.locRib.end() || route->second.localOrigin || route->second.learnedFrom.empty() ||
            route->second.learnedFrom == current)
        {
            break;
        }
        current = route->second.learnedFrom;
    }
    return path;
}

void SimulationEngine::requestPath(const std::string& routerId, const std::string& prefix)
{
    const auto path = pathSnapshot(routerId, prefix);
    pathReady(routerId, prefix, path);
}

SimulationEvent SimulationEngine::messageEvent(const BgpMessage& message) const
{
    SimulationEvent event;
    event.timestamp = simulationTimestamp();
    event.event = "message_received";
    event.router = message.to;
    event.from = message.from;
    event.to = message.to;
    event.messageType = toString(message.type);
    event.sequence = message.sequence;
    event.prefixes = message.nlri;
    event.withdrawn = message.withdrawn;
    if (const auto from = routers_.find(message.from); from != routers_.end())
    {
        event.fromAs = from->second.config.asn;
    }
    if (const auto to = routers_.find(message.to); to != routers_.end())
    {
        event.toAs = to->second.config.asn;
    }
    switch (message.type)
    {
        case MessageType::Open:
            event.action = "OPEN";
            event.details.emplace("open_asn", std::to_string(message.openAsn));
            event.details.emplace("open_router_id", message.openRouterId);
            break;
        case MessageType::Update:
        {
            event.action = !message.nlri.empty() ? "ANNOUNCE" : "WITHDRAW";
            const auto* attributes = message.advertisedRoute ? &message.advertisedRoute->attributes : &message.withdrawalAttributes;
            event.nextHop = attributes->nextHop;
            event.asPath = attributes->asPath;
            event.localPref = attributes->localPref;
            event.med = attributes->med;
            std::size_t versioned = 0;
            std::size_t dependencies = 0;
            std::size_t triggers = 0;
            for (const auto& prefix : !message.nlri.empty() ? message.nlri : message.withdrawn)
            {
                const TfpVersionInfo* info = nullptr;
                if (const auto perPrefix = message.tfpVersionInfoByPrefix.find(prefix);
                    perPrefix != message.tfpVersionInfoByPrefix.end())
                {
                    info = &perPrefix->second;
                }
                else if (attributes->tfpVersionInfo)
                {
                    info = &*attributes->tfpVersionInfo;
                }
                if (info)
                {
                    ++versioned;
                    dependencies += info->dependencyVector.size();
                    triggers += info->triggerVector.size();
                    if (!info->triggerVector.empty())
                    {
                        event.details.insert_or_assign("tfp_trigger_vector", tfpVectorText(info->triggerVector));
                    }
                }
            }
            if (versioned > 0)
            {
                event.details.emplace("tfp_versioned_prefix_count", std::to_string(versioned));
                event.details.emplace("tfp_dependency_entry_count", std::to_string(dependencies));
                event.details.emplace("tfp_trigger_entry_count", std::to_string(triggers));
            }
            break;
        }
        case MessageType::Notification:
            event.action = "NOTIFICATION";
            event.details.emplace("error_code", std::to_string(message.errorCode));
            event.details.emplace("error_subcode", std::to_string(message.errorSubcode));
            event.details.emplace("error_data", message.errorData);
            break;
    }
    event.details.insert_or_assign("simulation_time_ms", std::to_string(now()));
    return event;
}

void SimulationEngine::publishEvents(std::vector<SimulationEvent> events)
{
    if (events.empty())
    {
        return;
    }
    if (activeEventBatch_)
    {
        activeEventBatch_->reserve(activeEventBatch_->size() + events.size());
        for (auto& event : events)
        {
            activeEventBatch_->push_back(std::move(event));
        }
        return;
    }
    eventsGenerated(events);
}

void SimulationEngine::recordTopologyEvent(const std::string& name, std::map<std::string, std::string> details)
{
    SimulationEvent event;
    event.timestamp = simulationTimestamp();
    event.event = name;
    event.action = "TOPOLOGY";
    details.insert_or_assign("simulation_time_ms", std::to_string(now()));
    event.details = std::move(details);
    if (const auto value = event.details.find("router"); value != event.details.end())
    {
        event.router = value->second;
    }
    if (const auto value = event.details.find("a"); value != event.details.end())
    {
        event.from = value->second;
    }
    if (const auto value = event.details.find("b"); value != event.details.end())
    {
        event.to = value->second;
    }
    publishEvents({std::move(event)});
}

bool SimulationEngine::hasRouteReflectorClients(const RouterRuntime& router) const
{
    return std::any_of(router.peers.cbegin(), router.peers.cend(),
                       [](const auto& entry) { return entry.second.config.rrClient; });
}

} // namespace bgptester
