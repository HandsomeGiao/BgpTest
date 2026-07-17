#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

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
    explicit SimulationEngine(QObject* parent = nullptr);
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

public slots:
    void startSimulation(bgptester::Topology topology);
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
    void ribChanged(QString routerId);
    void pathReady(QString routerId, QString prefix, QStringList path);
    void routerStateChanged(QString routerId, bool enabled);
    void linkStateChanged(QString a, QString b, bool enabled);
    void statsChanged(bgptester::SimulationStats stats);
    void errorOccurred(QString message);

private slots:
    void processDueEvents();
    void updateStatus();

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
    void clearRuntime();
    bool buildRuntime(const Topology& topology, QString* error);
    void armNextEvent();
    void markActivity(const QString& convergenceTriggerEvent = {}, const QString& convergenceTriggerContext = {});
    void publishStats();
    void invalidateSession(PeerRuntime& peer);
    void clearPendingUpdates(PeerRuntime& peer);
    void setPendingUpdate(PeerRuntime& peer, const QString& prefix, PendingUpdate update);
    void removePendingUpdate(PeerRuntime& peer, const QString& prefix);
    void cancelEmptyFlushes(PeerRuntime& peer);
    std::optional<quint64> sessionEpoch(const QString& from, const QString& to) const;
    bool guardedMessageHasCurrentRoutes(const BgpMessage& message) const;
    bool scheduledEventValid(const ScheduledEvent& event) const;
    void pruneInvalidScheduledEvents();

    void scheduleMessages(const QString& from, const QString& to, QVector<BgpMessage> messages, int extraDelayMs = 0);
    void scheduleMraiFlush(const QString& from, const QString& to, qint64 dueAt);
    void scheduleWithdrawalFlush(const QString& from, const QString& to);
    void deliverMessages(const ScheduledEvent& event);
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
    std::optional<RouteEntry> selectBest(const RouterRuntime& router, const QString& prefix) const;
    void disseminate(const QString& routerId, const QString& prefix, const std::optional<RouteEntry>& route);
    void advertiseTableToPeer(const QString& routerId, const QString& peerId);
    void queueAdvertisement(const QString& routerId, const QString& peerId, const QString& prefix, const std::optional<RouteEntry>& route,
                            PathAttributes withdrawalAttributes = {});
    BgpMessage makeUpdateMessage(const QString& from, const QString& to, const QString& prefix, const std::optional<RouteEntry>& route,
                                 const PathAttributes& withdrawalAttributes, quint64 generation) const;

    SimulationEvent messageEvent(const BgpMessage& message) const;
    void recordTopologyEvent(const QString& name, QMap<QString, QString> details = {});
    bool hasRouteReflectorClients(const RouterRuntime& router) const;

    SimulationSettings simulation_;
    QMap<QString, RouterRuntime> routers_;
    QMap<QString, LinkConfig> links_;
    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, LaterEvent> events_;
    QElapsedTimer clock_;
    QTimer* eventTimer_ = nullptr;
    QTimer* statusTimer_ = nullptr;
    qint64 lastActivityAt_ = 0;
    qint64 convergenceStartedAt_ = 0;
    quint64 convergenceSequence_ = 0;
    QString convergenceTriggerEvent_;
    QString convergenceTriggerContext_;
    quint64 nextSequence_ = 0;
    quint64 nextOrder_ = 0;
    quint64 nextGeneration_ = 0;
    quint64 deliveredMessages_ = 0;
    bool scheduledEventsNeedPruning_ = false;
    bool routerSnapshotsDirty_ = false;
    bool running_ = false;
    bool converged_ = false;
};

} // namespace bgptester
