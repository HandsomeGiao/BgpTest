#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QElapsedTimer>
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

    [[nodiscard]] bool isRunning() const
    {
        return running_;
    }
    [[nodiscard]] bool isConverged() const
    {
        return converged_;
    }
    [[nodiscard]] RibSnapshot ribSnapshot(const QString& routerId) const;
    [[nodiscard]] QVector<PeerSnapshot> peerSnapshots(const QString& routerId) const;
    [[nodiscard]] QVector<RouterSnapshot> routerSnapshots() const;
    [[nodiscard]] BestPathUpdate bestPath(const QString& routerId, const QString& prefix) const;

public slots:
    void startSimulation(bgptester::Topology topology);
    void stopSimulation();
    void setLinkState(const QString& a, const QString& b, bool enabled);
    void setRouterState(const QString& routerId, bool enabled);
    void originatePrefix(const QString& routerId, const QString& prefix);
    void withdrawPrefix(const QString& routerId, const QString& prefix);
    void requestRouterSnapshot(const QString& routerId);
    void requestAllSnapshots();

signals:
    void runningChanged(bool running);
    void convergenceChanged(bool converged);
    void eventGenerated(bgptester::SimulationEvent event);
    void routerSnapshotsChanged(QVector<bgptester::RouterSnapshot> snapshots);
    void ribSnapshotReady(bgptester::RibSnapshot snapshot);
    void peersSnapshotReady(QString routerId, QVector<bgptester::PeerSnapshot> snapshots);
    void bestPathChanged(bgptester::BestPathUpdate update);
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
        QString prefix;
        std::optional<RouteEntry> route;
        quint64 generation = 0;
    };

    struct PeerRuntime
    {
        NeighborConfig config;
        PeerState state = PeerState::Idle;
        qint64 nextMraiAt = 0;
        bool flushScheduled = false;
        QMap<QString, PendingUpdate> pending;
    };

    struct RouterRuntime
    {
        RouterConfig config;
        RouterNode* node = nullptr;
        bool active = false;
        QMap<QString, PeerRuntime> peers;
        QMap<QString, RouteEntry> localRoutes;
        QMap<QString, QMap<QString, RouteEntry>> adjRibIn;
        QMap<QString, RouteEntry> locRib;
        QMap<QString, QMap<QString, RouteEntry>> adjRibOut;
        QMap<QString, QMap<QString, quint64>> outboundGenerations;
    };

    enum class ScheduledKind
    {
        DeliverMessages,
        FlushMrai
    };

    struct ScheduledEvent
    {
        qint64 dueAt = 0;
        quint64 order = 0;
        ScheduledKind kind = ScheduledKind::DeliverMessages;
        QString from;
        QString to;
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

    [[nodiscard]] qint64 now() const;
    void clearRuntime();
    [[nodiscard]] bool buildRuntime(QString* error);
    void armNextEvent();
    void markActivity();
    void publishStats();

    void scheduleMessages(const QString& from, const QString& to, QVector<BgpMessage> messages, int extraDelayMs = 0);
    void scheduleMraiFlush(const QString& from, const QString& to, qint64 dueAt);
    void deliverMessages(const ScheduledEvent& event);
    void flushMrai(const QString& from, const QString& to);
    [[nodiscard]] bool messageDeliverable(const QString& from, const QString& to) const;
    [[nodiscard]] int linkDelay(const QString& from, const QString& to) const;
    [[nodiscard]] bool generationIsCurrent(const BgpMessage& message) const;
    void commitOutbound(const BgpMessage& message);

    void sendOpen(const QString& from, const QString& to);
    void handleOpen(const BgpMessage& message);
    void handleUpdateBatch(const QString& receiver, const QString& sender, const QVector<BgpMessage>& messages);
    void handleNotification(const BgpMessage& message);
    void neighborDown(const QString& routerId, const QString& peerId);

    void runDecision(const QString& routerId, const QSet<QString>& changedPrefixes);
    [[nodiscard]] std::optional<RouteEntry> selectBest(const RouterRuntime& router, const QString& prefix) const;
    void disseminate(const QString& routerId, const QString& prefix, const std::optional<RouteEntry>& route);
    void queueAdvertisement(const QString& routerId, const QString& peerId, const QString& prefix, const std::optional<RouteEntry>& route);
    [[nodiscard]] BgpMessage makeUpdateMessage(const QString& from, const QString& to, const QString& prefix,
                                               const std::optional<RouteEntry>& route, quint64 generation) const;

    void recordMessage(const BgpMessage& message);
    void recordTopologyEvent(const QString& name, QMap<QString, QString> details = {});
    [[nodiscard]] bool hasRouteReflectorClients(const RouterRuntime& router) const;

    Topology topology_;
    QMap<QString, RouterRuntime> routers_;
    QMap<QString, LinkConfig> links_;
    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, LaterEvent> events_;
    QElapsedTimer clock_;
    QTimer* eventTimer_ = nullptr;
    QTimer* statusTimer_ = nullptr;
    qint64 lastActivityAt_ = 0;
    quint64 nextSequence_ = 0;
    quint64 nextOrder_ = 0;
    quint64 nextGeneration_ = 0;
    quint64 deliveredMessages_ = 0;
    bool running_ = false;
    bool converged_ = false;
};

} // namespace bgptester
