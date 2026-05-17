#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "toposim/BgpTypes.hpp"

namespace toposim {

using BmpEventValue = std::variant<std::string, std::uint64_t, bool>;
using BmpEventDetail = std::map<std::string, BmpEventValue>;

struct BmpLogRecord {
  std::uint64_t id = 0;
  std::string timestamp;
  std::string event;
  std::string router;
  std::string from;
  std::string to;
  std::optional<std::uint32_t> from_as;
  std::optional<std::uint32_t> to_as;
  std::string msg_type;
  std::string action;
  std::uint64_t sequence = 0;
  std::string prefixes;
  std::string withdrawn;
  std::string next_hop;
  std::string as_path;
  std::optional<std::uint32_t> local_pref;
  std::optional<std::uint32_t> med;
  std::string raw_json;
};

struct BmpLogQuery {
  std::vector<std::string> routers;
  std::vector<std::string> from_routers;
  std::vector<std::string> to_routers;
  std::vector<std::string> actions;
  std::vector<std::uint32_t> from_asns;
  std::vector<std::uint32_t> to_asns;
  std::size_t limit = 500;
};

class BmpLogManager {
public:
  static BmpLogManager &instance();

  BmpLogManager(const BmpLogManager &) = delete;
  BmpLogManager &operator=(const BmpLogManager &) = delete;

  void initialize(std::filesystem::path log_file,
                  std::filesystem::path database_file,
                  std::size_t live_capacity = 20000);
  void shutdown();
  void flush();

  void recordReceive(const std::string &router_id, const BgpMessage &message,
                     std::optional<std::uint32_t> from_as = std::nullopt,
                     std::optional<std::uint32_t> to_as = std::nullopt);
  void recordTopologyEvent(const std::string &event_name,
                           const BmpEventDetail &detail);

  [[nodiscard]] std::vector<BmpLogRecord> liveSnapshot() const;
  [[nodiscard]] std::vector<BmpLogRecord>
  queryHistory(const BmpLogQuery &query) const;

  [[nodiscard]] const std::filesystem::path &logFile() const;
  [[nodiscard]] const std::filesystem::path &databaseFile() const;
  [[nodiscard]] bool initialized() const;
  [[nodiscard]] std::uint64_t totalEvents() const;

private:
  BmpLogManager() = default;
  ~BmpLogManager();

  void enqueue(BmpLogRecord record);
  void writerLoop();
  void markBatchDrained(std::size_t batch_size);
  void rememberWriterError(std::string message);
  void openDatabase();
  void createSchema();
  void writeRecord(const BmpLogRecord &record);
  void insertRecord(const BmpLogRecord &record);
  void closeDatabase();

  mutable std::mutex mutex_;
  mutable std::mutex io_mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<BmpLogRecord> queue_;
  std::deque<BmpLogRecord> live_records_;
  std::size_t live_capacity_ = 20000;
  std::size_t inflight_ = 0;
  std::string writer_error_;
  std::thread writer_thread_;
  std::filesystem::path log_file_;
  std::filesystem::path database_file_;
  std::ofstream out_;
  void *db_ = nullptr;
  std::atomic<std::uint64_t> next_id_{1};
  std::atomic<std::uint64_t> total_events_{0};
  bool initialized_ = false;
  bool stopping_ = false;
};

} // namespace toposim
