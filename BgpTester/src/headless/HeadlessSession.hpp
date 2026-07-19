#pragma once

#include "engine/BgpTypes.hpp"
#include "model/Topology.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QThread>

#include <atomic>

namespace bgptester
{

class EventStore;
class SimulationEngine;

struct HeadlessCommandResult
{
    bool ok = false;
    QJsonObject data;
    QString error;
    bool exitRequested = false;
};

// Stateful, QWidget-free facade over topology editing, simulation control,
// snapshots and history. A single instance is intentionally used for an
// entire CLI script so runtime actions operate on the same simulation.
class HeadlessSession final : public QObject
{
    Q_OBJECT

public:
    explicit HeadlessSession(QObject* parent = nullptr);
    ~HeadlessSession() override;

    HeadlessSession(const HeadlessSession&) = delete;
    HeadlessSession& operator=(const HeadlessSession&) = delete;

    HeadlessCommandResult execute(const QJsonObject& command);
    QJsonObject statusJson() const;
    QJsonArray commandHelp() const;
    bool shutdown(QString* error = nullptr);
    void setInterruptionFlag(const std::atomic_bool* flag)
    {
        interruptionFlag_ = flag;
    }

    bool isRunning() const;
    bool isDirty() const
    {
        return dirty_;
    }
    QString topologyPath() const
    {
        return topologyPath_;
    }
    QString logFilePath() const
    {
        return logFilePath_;
    }
    QString databasePath() const
    {
        return databasePath_;
    }

private:
    using Handler = HeadlessCommandResult (HeadlessSession::*)(const QJsonObject&);

    HeadlessCommandResult dispatch(const QString& name, const QJsonObject& command);
    HeadlessCommandResult helpCommand(const QJsonObject& command);
    HeadlessCommandResult statusCommand(const QJsonObject& command);
    HeadlessCommandResult newCommand(const QJsonObject& command);
    HeadlessCommandResult loadCommand(const QJsonObject& command);
    HeadlessCommandResult saveCommand(const QJsonObject& command);
    HeadlessCommandResult topologyCommand(const QJsonObject& command);
    HeadlessCommandResult validateCommand(const QJsonObject& command);
    HeadlessCommandResult pluginsCommand(const QJsonObject& command);
    HeadlessCommandResult setSimulationCommand(const QJsonObject& command);
    HeadlessCommandResult addRouterCommand(const QJsonObject& command);
    HeadlessCommandResult updateRouterCommand(const QJsonObject& command);
    HeadlessCommandResult moveRouterCommand(const QJsonObject& command);
    HeadlessCommandResult deleteRouterCommand(const QJsonObject& command);
    HeadlessCommandResult addLinkCommand(const QJsonObject& command);
    HeadlessCommandResult updateLinkCommand(const QJsonObject& command);
    HeadlessCommandResult deleteLinkCommand(const QJsonObject& command);
    HeadlessCommandResult batchUpdateCommand(const QJsonObject& command);
    HeadlessCommandResult startCommand(const QJsonObject& command);
    HeadlessCommandResult stopCommand(const QJsonObject& command);
    HeadlessCommandResult waitCommand(const QJsonObject& command);
    HeadlessCommandResult waitConvergedCommand(const QJsonObject& command);
    HeadlessCommandResult setRouterStateCommand(const QJsonObject& command);
    HeadlessCommandResult toggleRouterCommand(const QJsonObject& command);
    HeadlessCommandResult setLinkStateCommand(const QJsonObject& command);
    HeadlessCommandResult toggleLinkCommand(const QJsonObject& command);
    HeadlessCommandResult advertisePrefixCommand(const QJsonObject& command);
    HeadlessCommandResult withdrawPrefixCommand(const QJsonObject& command);
    HeadlessCommandResult routersCommand(const QJsonObject& command);
    HeadlessCommandResult ribCommand(const QJsonObject& command);
    HeadlessCommandResult peersCommand(const QJsonObject& command);
    HeadlessCommandResult pathCommand(const QJsonObject& command);
    HeadlessCommandResult snapshotCommand(const QJsonObject& command);
    HeadlessCommandResult queryEventsCommand(const QJsonObject& command);
    HeadlessCommandResult queryConvergenceCommand(const QJsonObject& command);
    HeadlessCommandResult flushLogsCommand(const QJsonObject& command);
    HeadlessCommandResult exitCommand(const QJsonObject& command);

    HeadlessCommandResult rejectWhileRunning() const;
    HeadlessCommandResult requireRuntime() const;
    bool beginEventRun(QString* error);
    bool flushEventRun();
    bool endEventRun();
    void refreshRuntimeStatus();
    void refreshEventStoreStatus();
    void updateRuntimeLinks();
    void invalidateRuntime();

    static QJsonObject statsToJson(const SimulationStats& stats);
    static QJsonObject routeToJson(const RouteEntry& route);
    static QJsonObject routerSnapshotToJson(const RouterSnapshot& snapshot);
    static QJsonObject peerSnapshotToJson(const PeerSnapshot& snapshot);
    static QJsonObject ribSnapshotToJson(const RibSnapshot& snapshot, const QString& prefixFilter);

    Topology topology_;
    QString topologyPath_;
    bool dirty_ = false;
    bool runtimeAvailable_ = false;
    bool eventRunOpen_ = false;
    bool shuttingDown_ = false;
    bool simulationRunning_ = false;
    bool simulationConverged_ = false;

    SimulationEngine* engine_ = nullptr;
    QThread engineThread_;
    EventStore* eventStore_ = nullptr;
    QThread eventStoreThread_;
    SimulationStats latestStats_;
    QString lastEngineError_;
    QString lastStoreError_;
    QString logFilePath_;
    QString databasePath_;
    QString runDirectory_;
    quint64 eventRunSerial_ = 0;
    quint64 committedEventId_ = 0;
    QMap<QString, bool> runtimeLinks_;
    QMap<QString, QSet<QString>> runtimeOriginatedPrefixes_;
    const std::atomic_bool* interruptionFlag_ = nullptr;
};

} // namespace bgptester
