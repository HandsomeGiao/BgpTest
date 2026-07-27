#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace bgptester
{

struct EventHistoryPage
{
    std::vector<SimulationEvent> events;
    std::int64_t totalCount = 0;
    std::int64_t filteredCount = 0;
    std::int64_t messageTotalCount = 0;
    std::int64_t filteredMessageCount = 0;
    std::uint64_t maxEventId = 0;
};

struct ConvergenceHistoryPage
{
    std::vector<SimulationEvent> events;
    std::int64_t totalCount = 0;
    std::uint64_t maxEventId = 0;
};

// Synchronous portable event store. SimulationEngine already advances on a
// deterministic virtual clock, so the CLI needs no framework event loop
// around SQLite writes.
class EventStore final
{
public:
    EventStore() = default;
    ~EventStore();

    EventStore(const EventStore&) = delete;
    EventStore& operator=(const EventStore&) = delete;

    bool beginRun(const SimulationSettings& settings, std::string* error = nullptr);
    void endRun();
    bool flush(std::string* error = nullptr);
    void enqueueEvents(std::vector<SimulationEvent> events);
    void appendEvent(SimulationEvent event);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const std::string& runDirectory() const noexcept { return runDirectory_; }
    [[nodiscard]] const std::string& logFilePath() const noexcept { return logFilePath_; }
    [[nodiscard]] const std::string& databasePath() const noexcept { return databasePath_; }
    [[nodiscard]] std::uint64_t runSerial() const noexcept { return runSerial_; }
    [[nodiscard]] std::uint64_t committedEventId() const noexcept { return committedEventId_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }
    [[nodiscard]] int encodingWorkerCount() const noexcept { return encodingWorkerCount_; }

    static EventHistoryPage queryDatabase(const std::string& path, int limit, const std::string& filter = {},
                                          std::string* error = nullptr,
                                          const std::function<bool(std::size_t, std::size_t)>& progress = {},
                                          const std::function<bool()>& cancelled = {});
    static EventHistoryPage countDatabase(const std::string& path, const std::string& filter,
                                          std::uint64_t maxEventId, std::string* error = nullptr,
                                          const std::function<bool()>& cancelled = {});
    static ConvergenceHistoryPage queryConvergenceDatabase(const std::string& path, int limit,
                                                            std::string* error = nullptr,
                                                            const std::function<bool()>& cancelled = {});
    static std::vector<SimulationEvent> readDatabase(
        const std::string& path, int limit, std::string* error = nullptr,
        const std::function<bool(std::size_t, std::size_t)>& progress = {});
    static Json eventToJson(const SimulationEvent& event);
    static std::optional<SimulationEvent> eventFromJson(const Json& object, std::string* error = nullptr);

private:
    bool initializeSchema(std::string* error);
    bool prepareInsert(std::string* error);
    bool insertEvent(const SimulationEvent& event, const std::string& rawJson, std::string* error);
    bool execute(const char* sql, std::string* error);
    bool beginTransaction(std::string* error);
    bool commitTransaction(bool restart, std::string* error);
    void setLastError(std::string message, std::string* error = nullptr);

    mutable std::mutex mutex_;
    sqlite3* database_ = nullptr;
    sqlite3_stmt* insertStatement_ = nullptr;
    std::ofstream logFile_;
    std::string runDirectory_;
    std::string logFilePath_;
    std::string databasePath_;
    std::uint64_t nextId_ = 1;
    std::uint64_t runSerial_ = 0;
    std::uint64_t lastInsertedEventId_ = 0;
    std::uint64_t committedEventId_ = 0;
    std::string lastError_;
    int encodingWorkerCount_ = 1;
    int pendingTransactionRows_ = 0;
    bool transactionOpen_ = false;

    static constexpr int transactionBatchSize_ = 16384;
};

} // namespace bgptester
