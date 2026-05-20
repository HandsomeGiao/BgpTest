#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "toposim/BgpRouter.hpp"
#include "toposim/BgpTypes.hpp"
#include "toposim/BmpLogManager.hpp"
#include "toposim/ThreadPool.hpp"

namespace toposim {

class TopoManager {
public:
  explicit TopoManager(TopologyConfig config);
  ~TopoManager();

  TopoManager(const TopoManager &) = delete;
  TopoManager &operator=(const TopoManager &) = delete;

  void start();
  void stop();
  void sendMessage(const std::string &from, const std::string &to,
                   BgpMessage message,
                   std::chrono::milliseconds extra_delay =
                       std::chrono::milliseconds{0},
                   std::function<bool()> delivery_guard = {});
  void sendMessages(const std::string &from, const std::string &to,
                    std::vector<BgpMessage> messages,
                    std::chrono::milliseconds extra_delay =
                        std::chrono::milliseconds{0},
                    std::vector<std::function<bool()>> delivery_guards = {});
  void scheduleTask(std::chrono::milliseconds delay,
                    std::function<void()> task);
  bool setLinkState(const std::string &a, const std::string &b, bool enabled);
  bool setRouterState(const std::string &router_id, bool enabled);
  void originatePrefix(const std::string &router_id, const std::string &prefix);
  void withdrawPrefix(const std::string &router_id, const std::string &prefix);
  bool waitForConvergence(std::chrono::milliseconds timeout);
  [[nodiscard]] bool isConverged() const;

  struct BestPathSnapshot {
    std::string router;
    std::string prefix;
    bool valid = false;
    std::optional<RouteEntry> route;
  };
  using BestPathObserver =
      std::function<void(const std::string &, const std::string &)>;
  void setBestPathObserver(BestPathObserver observer);
  void notifyBestPathChanges(const std::string &router_id,
                             const std::vector<std::string> &prefixes);
  void publishCurrentBestPaths() const;
  [[nodiscard]] BestPathSnapshot
  bestPathSnapshot(const std::string &router_id,
                    const std::string &prefix) const;

  [[nodiscard]] std::chrono::steady_clock::time_point
  lastMessageProcessedAt() const;
  [[nodiscard]] std::chrono::steady_clock::time_point
  lastConvergenceActivityAt() const;

  [[nodiscard]] std::vector<RouterSnapshot> routersSnapshot() const;
  [[nodiscard]] RibSnapshot ribSnapshot(const std::string &router_id) const;
  [[nodiscard]] std::vector<PeerSnapshot>
  peersSnapshot(const std::string &router_id) const;
  [[nodiscard]] std::filesystem::path logFile() const;
  [[nodiscard]] std::filesystem::path databaseFile() const;

private:
  struct LinkRuntime {
    LinkConfig config;
  };

  static std::string edgeKey(const std::string &a, const std::string &b);
  static std::string directedKey(const std::string &from,
                                 const std::string &to);

  void validateConfig() const;
  void buildRouters();
  void normalizeNeighborsFromLinks();
  [[nodiscard]] bool messageStillDeliverable(const std::string &from,
                                             const std::string &to) const;
  [[nodiscard]] std::optional<LinkRuntime> linkFor(const std::string &a,
                                                   const std::string &b) const;
  [[nodiscard]] std::chrono::milliseconds convergenceQuietPeriod() const;
  [[nodiscard]] std::filesystem::path makeRunDirectory() const;
  void markConvergenceActivity();

  TopologyConfig config_;
  mutable std::mutex mutex_;
  mutable std::mutex observer_mutex_;
  std::unordered_map<std::string, std::shared_ptr<BgpRouter>> routers_;
  std::unordered_map<std::string, LinkRuntime> links_;
  std::unordered_map<std::string, std::shared_ptr<std::mutex>> delivery_locks_;
  std::unique_ptr<ThreadPool> pool_;
  std::filesystem::path run_dir_;
  std::filesystem::path bmp_log_file_;
  std::filesystem::path bmp_database_file_;
  std::chrono::steady_clock::time_point last_message_processed_at_{};
  std::chrono::steady_clock::time_point last_convergence_activity_at_{};
  std::atomic<std::uint64_t> sequence_{0};
  BestPathObserver best_path_observer_;
  bool running_ = false;
};

} // namespace toposim
