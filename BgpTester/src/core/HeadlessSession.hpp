#pragma once

#include "core/Topology.hpp"
#include "core/Types.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace bgptester
{

class EventStore;
class SimulationEngine;

struct HeadlessCommandResult
{
    bool ok = false;
    Json data = Json::object();
    std::string error;
    bool exitRequested = false;
};

// Stateful portable facade used by the JSONL command-line protocol. The
// portable simulation and event store are synchronous, so each command leaves
// both components at an explicit deterministic boundary before returning.
class HeadlessSession final
{
public:
    HeadlessSession();
    ~HeadlessSession();

    HeadlessSession(const HeadlessSession&) = delete;
    HeadlessSession& operator=(const HeadlessSession&) = delete;

    HeadlessCommandResult execute(const Json& command);
    Json statusJson() const;
    Json commandHelp() const;
    bool shutdown(std::string* error = nullptr);
    void setInterruptionFlag(const std::atomic_bool* flag) noexcept { interruptionFlag_ = flag; }

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }
    [[nodiscard]] const std::string& topologyPath() const noexcept { return topologyPath_; }
    [[nodiscard]] const std::string& logFilePath() const noexcept { return logFilePath_; }
    [[nodiscard]] const std::string& databasePath() const noexcept { return databasePath_; }

private:
    using Handler = HeadlessCommandResult (HeadlessSession::*)(const Json&);

    HeadlessCommandResult dispatch(const std::string& name, const Json& command);
    HeadlessCommandResult helpCommand(const Json& command);
    HeadlessCommandResult statusCommand(const Json& command);
    HeadlessCommandResult newCommand(const Json& command);
    HeadlessCommandResult loadCommand(const Json& command);
    HeadlessCommandResult saveCommand(const Json& command);
    HeadlessCommandResult topologyCommand(const Json& command);
    HeadlessCommandResult validateCommand(const Json& command);
    HeadlessCommandResult pluginsCommand(const Json& command);
    HeadlessCommandResult setSimulationCommand(const Json& command);
    HeadlessCommandResult addRouterCommand(const Json& command);
    HeadlessCommandResult updateRouterCommand(const Json& command);
    HeadlessCommandResult moveRouterCommand(const Json& command);
    HeadlessCommandResult deleteRouterCommand(const Json& command);
    HeadlessCommandResult addLinkCommand(const Json& command);
    HeadlessCommandResult updateLinkCommand(const Json& command);
    HeadlessCommandResult deleteLinkCommand(const Json& command);
    HeadlessCommandResult batchUpdateCommand(const Json& command);
    HeadlessCommandResult startCommand(const Json& command);
    HeadlessCommandResult stopCommand(const Json& command);
    HeadlessCommandResult waitCommand(const Json& command);
    HeadlessCommandResult waitConvergedCommand(const Json& command);
    HeadlessCommandResult setRouterStateCommand(const Json& command);
    HeadlessCommandResult toggleRouterCommand(const Json& command);
    HeadlessCommandResult setLinkStateCommand(const Json& command);
    HeadlessCommandResult toggleLinkCommand(const Json& command);
    HeadlessCommandResult advertisePrefixCommand(const Json& command);
    HeadlessCommandResult withdrawPrefixCommand(const Json& command);
    HeadlessCommandResult routersCommand(const Json& command);
    HeadlessCommandResult ribCommand(const Json& command);
    HeadlessCommandResult peersCommand(const Json& command);
    HeadlessCommandResult pathCommand(const Json& command);
    HeadlessCommandResult snapshotCommand(const Json& command);
    HeadlessCommandResult queryEventsCommand(const Json& command);
    HeadlessCommandResult queryConvergenceCommand(const Json& command);
    HeadlessCommandResult flushLogsCommand(const Json& command);
    HeadlessCommandResult exitCommand(const Json& command);

    HeadlessCommandResult rejectWhileRunning() const;
    HeadlessCommandResult requireRuntime() const;
    bool beginEventRun(std::string* error);
    bool flushEventRun();
    bool endEventRun();
    bool stabilizeRuntime(std::string* error);
    void refreshRuntimeStatus();
    void refreshEventStoreStatus();
    void updateRuntimeState();
    void invalidateRuntime();

    static Json statsToJson(const SimulationStats& stats);
    static Json routeToJson(const RouteEntry& route);
    static Json routerSnapshotToJson(const RouterSnapshot& snapshot);
    static Json peerSnapshotToJson(const PeerSnapshot& snapshot);
    static Json ribSnapshotToJson(const RibSnapshot& snapshot, const std::string& prefixFilter);

    Topology topology_;
    std::string topologyPath_;
    bool dirty_ = false;
    bool runtimeAvailable_ = false;
    bool eventRunOpen_ = false;
    bool shuttingDown_ = false;
    bool simulationRunning_ = false;
    bool simulationConverged_ = false;

    std::unique_ptr<SimulationEngine> engine_;
    std::unique_ptr<EventStore> eventStore_;
    SimulationStats latestStats_;
    std::string lastEngineError_;
    std::string lastStoreError_;
    std::string logFilePath_;
    std::string databasePath_;
    std::string runDirectory_;
    std::uint64_t eventRunSerial_ = 0;
    std::uint64_t committedEventId_ = 0;
    std::map<std::string, bool, std::less<>> runtimeLinks_;
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> runtimeOriginatedPrefixes_;
    const std::atomic_bool* interruptionFlag_ = nullptr;
};

} // namespace bgptester
