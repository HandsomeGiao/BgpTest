#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <atomic>
#include <optional>
#include <queue>
#include <vector>

namespace bgptester
{

class RouterNode;

class SimulationEngine final : public QObject
{
    Q_OBJECT

public:
    static constexpr qsizetype DefaultProcessingQuantum = 16384;
    static constexpr quint64 DefaultConvergenceEventBudget = 10'000'000;

    explicit SimulationEngine(QObject* parent = nullptr, qsizetype processingQuantum = DefaultProcessingQuantum,
                              quint64 convergenceEventBudget = DefaultConvergenceEventBudget);
    ~SimulationEngine() override = default;

    bool isRunning() const
    {
        return running_;
    }
    bool isConverged() const
    {
        return converged_;
    }
    RibSnapshot ribSnapshot(const QString& routerId) const;
    QVector<PeerSnapshot> peerSnapshots(const QString& routerId) const;
    QVector<RouterSnapshot> routerSnapshots() const;
    QStringList pathSnapshot(const QString& routerId, const QString& prefix) const;
    SimulationStats statsSnapshot();
    QString lastError() const
    {
        return lastError_;
    }
    void prepareStartup() noexcept;
    void requestStartupCancellation() noexcept;

public slots:
    void startSimulation(bgptester::Topology topology);
    // Deterministically consumes the complete current protocol wave. This is
    // used by headless control commands to establish a stable mutation
    // boundary without relying on CPU speed or a wall-clock timeout.
    [[nodiscard]] bool runUntilConverged();
    void stopSimulation();
    void setLinkState(const QString& a, const QString& b, bool enabled);
    void setRouterState(const QString& routerId, bool enabled);
    void originatePrefix(const QString& routerId, const QString& prefix);
    void withdrawPrefix(const QString& routerId, const QString& prefix);
    void requestRouterSnapshot(const QString& routerId);
    void requestAllSnapshots();
    void requestPath(const QString& routerId, const QString& prefix);

signals:
    void runningChanged(bool running);
    void convergenceChanged(bool converged);
    void eventsGenerated(QVector<bgptester::SimulationEvent> events);
    void routerSnapshotsChanged(QVector<bgptester::RouterSnapshot> snapshots);
    void ribSnapshotReady(bgptester::RibSnapshot snapshot);
    void peersSnapshotReady(QString routerId, QVector<bgptester::PeerSnapshot> snapshots);
    void pathReady(QString routerId, QString prefix, QStringList path);
    void routerStateChanged(QString routerId, bool enabled);
    void linkStateChanged(QString a, QString b, bool enabled);
    void statsChanged(bgptester::SimulationStats stats);
    void startupProgress(QString stage, qint64 completed, qint64 total);
    void startupCancelled();
    void routingStateChanged();
    void errorOccurred(QString message);

private slots:
    void processDueEvents();

private:
    struct PendingUpdate
    {
        std::optional<RouteEntry> route;
        PathAttributes withdrawalAttributes;
        quint64 generation = 0;
    };

    struct PeerRuntime
    {
        NeighborConfig config;
        PeerState state = PeerState::Idle;
        quint64 sessionEpoch = 0;
        quint64 mraiFlushGeneration = 0;
        quint64 withdrawalFlushGeneration = 0;
        qint64 nextMraiAt = 0;
        bool flushScheduled = false;
        bool withdrawalFlushScheduled = false;
        qsizetype pendingAdvertisementCount = 0;
        qsizetype pendingWithdrawalCount = 0;
        QMap<QString, PendingUpdate> pending;
    };

    struct RouterRuntime
    {
        RouterConfig config;
        RouterNode* node = nullptr;
        bool active = false;
        QMap<QString, PeerRuntime> peers;
        QHash<QString, RouteEntry> localRoutes;
        QHash<QString, QHash<QString, RouteEntry>> adjRibIn;
        QHash<QString, RouteEntry> locRib;
        QHash<QString, QHash<QString, QPair<quint64, quint64>>> adjRibOut;
        QHash<QString, QHash<QString, quint64>> outboundGenerations;
    };

    enum class ScheduledKind
    {
        DeliverMessages,
        FlushMrai,
        FlushWithdrawals
    };

    struct ScheduledEvent
    {
        qint64 dueAt = 0;
        quint64 order = 0;
        ScheduledKind kind = ScheduledKind::DeliverMessages;
        QString from;
        QString to;
        quint64 sessionEpoch = 0;
        quint64 flushGeneration = 0;
        QVector<BgpMessage> messages;
    };

    struct LaterEvent
    {
        bool operator()(const ScheduledEvent& lhs, const ScheduledEvent& rhs) const
        {
            if (lhs.dueAt != rhs.dueAt)
            {
                return lhs.dueAt > rhs.dueAt;
            }
            return lhs.order > rhs.order;
        }
    };

    qint64 now() const;
    QDateTime simulationTimestamp() const;
    void clearRuntime();
    bool rejectReentrantControl(const QString& operation);
    bool buildRuntime(const Topology& topology, QString* error);
    bool scheduleInitialOpenMessages();
    void armNextEvent();
    qsizetype processEventQuantum(qsizetype maximumEvents);
    void failConvergenceBudget();
    void finishConvergenceIfIdle();
    void markActivity(const QString& convergenceTriggerEvent = {}, const QString& convergenceTriggerContext = {});
    void notifyConvergenceStateChanged(bool converged);
    void publishStats();
    void invalidateSession(PeerRuntime& peer);
    void clearPendingUpdates(PeerRuntime& peer);
    const PendingUpdate& setPendingUpdate(PeerRuntime& peer, const QString& prefix, PendingUpdate update);
    void removePendingUpdate(PeerRuntime& peer, const QString& prefix);
    void cancelEmptyFlushes(PeerRuntime& peer);
    std::optional<quint64> sessionEpoch(const QString& from, const QString& to) const;
    bool guardedMessageHasCurrentRoutes(const BgpMessage& message) const;
    bool scheduledEventValid(const ScheduledEvent& event) const;
    quint64 pruneInvalidScheduledEvents(quint64 maximumEvents);

    void scheduleMessages(const QString& from, const QString& to, QVector<BgpMessage> messages, int extraDelayMs = 0);
    void scheduleMraiFlush(const QString& from, const QString& to, qint64 dueAt);
    void scheduleWithdrawalFlush(const QString& from, const QString& to);
    void deliverMessages(ScheduledEvent& event);
    void flushMrai(const QString& from, const QString& to);
    void flushWithdrawals(const QString& from, const QString& to);
    bool messageDeliverable(const QString& from, const QString& to) const;
    int linkDelay(const QString& from, const QString& to) const;
    bool retainCurrentRoutes(BgpMessage& message) const;
    void commitOutbound(const BgpMessage& message);

    void sendOpen(const QString& from, const QString& to);
    void handleOpen(const BgpMessage& message);
    void handleUpdateBatch(const QString& receiver, const QString& sender, const QVector<BgpMessage>& messages);
    void handleNotification(const BgpMessage& message);
    void neighborDown(const QString& routerId, const QString& peerId);

    void runDecision(const QString& routerId, const QSet<QString>& changedPrefixes);
    std::optional<RouteEntry> selectBest(const RouterRuntime& router, const QString& prefix,
                                         const std::optional<RouteEntry>& currentBest,
                                         QVector<RouteEntry>& candidateScratch) const;
    void disseminate(const QString& routerId, const QString& prefix, const std::optional<RouteEntry>& route, bool force = false);
    void advertiseTableToPeer(const QString& routerId, const QString& peerId);
    void queueAdvertisement(const QString& routerId, const QString& peerId, const QString& prefix, const std::optional<RouteEntry>& route,
                            PathAttributes withdrawalAttributes = {});
    BgpMessage makeUpdateMessage(const QString& from, const QString& to, const QString& prefix, const std::optional<RouteEntry>& route,
                                 const PathAttributes& withdrawalAttributes, quint64 generation) const;

    SimulationEvent messageEvent(const BgpMessage& message) const;
    void publishEvents(QVector<SimulationEvent> events);
    void recordTopologyEvent(const QString& name, QMap<QString, QString> details = {});
    bool hasRouteReflectorClients(const RouterRuntime& router) const;

    SimulationSettings simulation_;
    QMap<QString, RouterRuntime> routers_;
    QMap<QString, LinkConfig> links_;
    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, LaterEvent> events_;
    QTimer* eventTimer_ = nullptr;
    qint64 simulationTimeMs_ = 0;
    qint64 lastActivityAt_ = 0;
    qint64 convergenceStartedAt_ = 0;
    quint64 convergenceSequence_ = 0;
    quint64 convergenceMessageCount_ = 0;
    QString convergenceTriggerEvent_;
    QString convergenceTriggerContext_;
    quint64 nextSequence_ = 0;
    quint64 nextOrder_ = 0;
    quint64 nextGeneration_ = 0;
    // Generations are unique within one active simulation. Only generations
    // carrying an uncommitted Trigger allocate an entry in this sparse index.
    QHash<quint64, TfpVersionVector> outstandingTriggers_;
    quint64 deliveredMessages_ = 0;
    qsizetype processingQuantum_ = DefaultProcessingQuantum;
    quint64 convergenceEventBudget_ = DefaultConvergenceEventBudget;
    quint64 processedEventsInConvergence_ = 0;
    bool routingStateDirty_ = false;
    QVector<SimulationEvent>* activeEventBatch_ = nullptr;
    bool drainingToConvergence_ = false;
    bool processingEvents_ = false;
    bool controlOperationActive_ = false;
    bool eventPumpDeferred_ = false;
    bool convergenceFailed_ = false;
    std::atomic_bool startupCancelRequested_{false};
    bool pluginLifecycleActive_ = false;
    bool running_ = false;
    bool converged_ = false;
    QString lastError_;
};

} // namespace bgptester
