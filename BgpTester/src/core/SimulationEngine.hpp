#pragma once

#include "core/RouterPolicy.hpp"
#include "core/Topology.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bgptester
{

// A small synchronous observer primitive. Slots are copied before dispatch so
// connecting or disconnecting from inside a callback is well-defined.
template <typename... Args>
class Signal final
{
public:
    using Slot = std::function<void(Args...)>;
    using Connection = std::uint64_t;

    Connection connect(Slot slot)
    {
        if (!slot)
        {
            return 0;
        }
        const auto id = nextConnection_++;
        slots_.emplace_back(id, std::move(slot));
        return id;
    }

    bool disconnect(Connection connection)
    {
        const auto oldSize = slots_.size();
        std::erase_if(slots_, [connection](const auto& entry) { return entry.first == connection; });
        return slots_.size() != oldSize;
    }

    void clear()
    {
        slots_.clear();
    }

    void operator()(Args... args) const
    {
        const auto snapshot = slots_;
        for (const auto& [id, slot] : snapshot)
        {
            (void)id;
            slot(args...);
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return slots_.empty();
    }

private:
    std::vector<std::pair<Connection, Slot>> slots_;
    Connection nextConnection_ = 1;
};

class SimulationEngine final
{
public:
    static constexpr std::size_t DefaultProcessingQuantum = 16384;
    static constexpr std::uint64_t DefaultConvergenceEventBudget = 10'000'000;

    explicit SimulationEngine(std::size_t processingQuantum = DefaultProcessingQuantum,
                              std::uint64_t convergenceEventBudget = DefaultConvergenceEventBudget);
    ~SimulationEngine();

    SimulationEngine(const SimulationEngine&) = delete;
    SimulationEngine& operator=(const SimulationEngine&) = delete;

    [[nodiscard]] bool isRunning() const noexcept;
    [[nodiscard]] bool isConverged() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;

    [[nodiscard]] RibSnapshot ribSnapshot(const std::string& routerId) const;
    [[nodiscard]] std::vector<PeerSnapshot> peerSnapshots(const std::string& routerId) const;
    [[nodiscard]] std::vector<RouterSnapshot> routerSnapshots() const;
    [[nodiscard]] std::vector<std::string> pathSnapshot(const std::string& routerId, const std::string& prefix) const;
    [[nodiscard]] SimulationStats statsSnapshot() const;

    void prepareStartup() noexcept;
    void requestStartupCancellation() noexcept;
    void startSimulation(Topology topology);
    [[nodiscard]] bool runUntilConverged();
    void stopSimulation();
    void setLinkState(const std::string& a, const std::string& b, bool enabled);
    void setRouterState(const std::string& routerId, bool enabled);
    void originatePrefix(const std::string& routerId, const std::string& prefix);
    void withdrawPrefix(const std::string& routerId, const std::string& prefix);
    void requestRouterSnapshot(const std::string& routerId);
    void requestAllSnapshots();
    void requestPath(const std::string& routerId, const std::string& prefix);

    Signal<bool> runningChanged;
    Signal<bool> convergenceChanged;
    Signal<const std::vector<SimulationEvent>&> eventsGenerated;
    Signal<const std::vector<RouterSnapshot>&> routerSnapshotsChanged;
    Signal<const RibSnapshot&> ribSnapshotReady;
    Signal<const std::string&, const std::vector<PeerSnapshot>&> peersSnapshotReady;
    Signal<const std::string&, const std::string&, const std::vector<std::string>&> pathReady;
    Signal<const std::string&, bool> routerStateChanged;
    Signal<const std::string&, const std::string&, bool> linkStateChanged;
    Signal<const SimulationStats&> statsChanged;
    Signal<const std::string&, std::int64_t, std::int64_t> startupProgress;
    Signal<> startupCancelled;
    Signal<> routingStateChanged;
    Signal<const std::string&> errorOccurred;

private:
    struct PendingUpdate
    {
        std::optional<RouteEntry> route;
        PathAttributes withdrawalAttributes;
        std::uint64_t generation = 0;
    };

    struct PeerRuntime
    {
        NeighborConfig config;
        PeerState state = PeerState::Idle;
        std::uint64_t sessionEpoch = 0;
        std::uint64_t mraiFlushGeneration = 0;
        std::uint64_t withdrawalFlushGeneration = 0;
        std::int64_t nextMraiAt = 0;
        bool flushScheduled = false;
        bool withdrawalFlushScheduled = false;
        std::size_t pendingAdvertisementCount = 0;
        std::size_t pendingWithdrawalCount = 0;
        std::map<std::string, PendingUpdate, std::less<>> pending;
    };

    using Fingerprint = std::pair<std::uint64_t, std::uint64_t>;
    using GenerationMap = std::unordered_map<std::string, std::uint64_t>;
    using FingerprintMap = std::unordered_map<std::string, Fingerprint>;

    struct RouterRuntime
    {
        RouterConfig config;
        std::unique_ptr<RouterPolicy> policy;
        bool active = false;
        std::map<std::string, PeerRuntime, std::less<>> peers;
        RouteTable localRoutes;
        std::unordered_map<std::string, RouteTable> adjRibIn;
        RouteTable locRib;
        std::unordered_map<std::string, FingerprintMap> adjRibOut;
        std::unordered_map<std::string, GenerationMap> outboundGenerations;
    };

    enum class ScheduledKind
    {
        DeliverMessages,
        FlushMrai,
        FlushWithdrawals
    };

    struct ScheduledEvent
    {
        std::int64_t dueAt = 0;
        std::uint64_t order = 0;
        ScheduledKind kind = ScheduledKind::DeliverMessages;
        std::string from;
        std::string to;
        std::uint64_t sessionEpoch = 0;
        std::uint64_t flushGeneration = 0;
        std::vector<BgpMessage> messages;
    };

    struct LaterEvent
    {
        bool operator()(const ScheduledEvent& lhs, const ScheduledEvent& rhs) const noexcept
        {
            return lhs.dueAt != rhs.dueAt ? lhs.dueAt > rhs.dueAt : lhs.order > rhs.order;
        }
    };

    [[nodiscard]] std::int64_t now() const noexcept;
    [[nodiscard]] std::int64_t simulationTimestamp() const noexcept;
    void clearRuntime();
    [[nodiscard]] bool rejectReentrantControl(std::string_view operation);
    [[nodiscard]] bool buildRuntime(const Topology& topology, std::string* error);
    [[nodiscard]] bool scheduleInitialOpenMessages();
    [[nodiscard]] std::size_t processEventQuantum(std::size_t maximumEvents);
    void failConvergenceBudget();
    void finishConvergenceIfIdle();
    void markActivity(std::string convergenceTriggerEvent = {}, std::string convergenceTriggerContext = {});
    void notifyConvergenceStateChanged(bool converged);
    void publishStats();
    void invalidateSession(PeerRuntime& peer);
    void clearPendingUpdates(PeerRuntime& peer);
    const PendingUpdate& setPendingUpdate(PeerRuntime& peer, const std::string& prefix, PendingUpdate update);
    void removePendingUpdate(PeerRuntime& peer, const std::string& prefix);
    void cancelEmptyFlushes(PeerRuntime& peer);
    [[nodiscard]] std::optional<std::uint64_t> sessionEpoch(const std::string& from, const std::string& to) const;
    [[nodiscard]] bool guardedMessageHasCurrentRoutes(const BgpMessage& message) const;
    [[nodiscard]] bool scheduledEventValid(const ScheduledEvent& event) const;
    std::uint64_t pruneInvalidScheduledEvents(std::uint64_t maximumEvents);

    void scheduleMessages(const std::string& from, const std::string& to, std::vector<BgpMessage> messages,
                          int extraDelayMs = 0);
    void scheduleMraiFlush(const std::string& from, const std::string& to, std::int64_t dueAt);
    void scheduleWithdrawalFlush(const std::string& from, const std::string& to);
    void deliverMessages(ScheduledEvent& event);
    void flushMrai(const std::string& from, const std::string& to);
    void flushWithdrawals(const std::string& from, const std::string& to);
    [[nodiscard]] bool messageDeliverable(const std::string& from, const std::string& to) const;
    [[nodiscard]] int linkDelay(const std::string& from, const std::string& to) const;
    [[nodiscard]] bool retainCurrentRoutes(BgpMessage& message) const;
    void commitOutbound(const BgpMessage& message);

    void sendOpen(const std::string& from, const std::string& to);
    void handleOpen(const BgpMessage& message);
    void handleUpdateBatch(const std::string& receiver, const std::string& sender, const std::vector<BgpMessage>& messages);
    void handleNotification(const BgpMessage& message);
    void neighborDown(const std::string& routerId, const std::string& peerId);

    void runDecision(const std::string& routerId, const std::unordered_set<std::string>& changedPrefixes);
    [[nodiscard]] std::optional<RouteEntry> selectBest(const RouterRuntime& router, const std::string& prefix,
                                                       const std::optional<RouteEntry>& currentBest,
                                                       std::vector<RouteEntry>& candidateScratch) const;
    void disseminate(const std::string& routerId, const std::string& prefix, const std::optional<RouteEntry>& route,
                     bool force = false);
    void advertiseTableToPeer(const std::string& routerId, const std::string& peerId);
    void queueAdvertisement(const std::string& routerId, const std::string& peerId, const std::string& prefix,
                            const std::optional<RouteEntry>& route, PathAttributes withdrawalAttributes = {});
    [[nodiscard]] BgpMessage makeUpdateMessage(const std::string& from, const std::string& to, const std::string& prefix,
                                                const std::optional<RouteEntry>& route,
                                                const PathAttributes& withdrawalAttributes,
                                                std::uint64_t generation) const;

    [[nodiscard]] SimulationEvent messageEvent(const BgpMessage& message) const;
    void publishEvents(std::vector<SimulationEvent> events);
    void recordTopologyEvent(const std::string& name, std::map<std::string, std::string> details = {});
    [[nodiscard]] bool hasRouteReflectorClients(const RouterRuntime& router) const;

    SimulationSettings simulation_;
    std::shared_ptr<const RouterMap> topologyRouters_;
    std::map<std::string, RouterRuntime, std::less<>> routers_;
    std::map<std::string, LinkConfig, std::less<>> links_;
    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, LaterEvent> events_;
    std::int64_t simulationTimeMs_ = 0;
    std::int64_t lastActivityAt_ = 0;
    std::int64_t convergenceStartedAt_ = 0;
    std::uint64_t convergenceSequence_ = 0;
    std::uint64_t convergenceMessageCount_ = 0;
    std::string convergenceTriggerEvent_;
    std::string convergenceTriggerContext_;
    std::uint64_t nextSequence_ = 0;
    std::uint64_t nextOrder_ = 0;
    std::uint64_t nextGeneration_ = 0;
    std::unordered_map<std::uint64_t, TfpVersionVector> outstandingTriggers_;
    std::uint64_t deliveredMessages_ = 0;
    std::size_t processingQuantum_ = DefaultProcessingQuantum;
    std::uint64_t convergenceEventBudget_ = DefaultConvergenceEventBudget;
    std::uint64_t processedEventsInConvergence_ = 0;
    bool routingStateDirty_ = false;
    std::vector<SimulationEvent>* activeEventBatch_ = nullptr;
    bool drainingToConvergence_ = false;
    bool processingEvents_ = false;
    bool controlOperationActive_ = false;
    bool convergenceFailed_ = false;
    std::atomic_bool startupCancelRequested_{false};
    bool policyLifecycleActive_ = false;
    bool running_ = false;
    bool converged_ = false;
    std::string lastError_;
};

} // namespace bgptester
