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
#include "toposim/BmpCollector.hpp"
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
  void setLinkState(const std::string &a, const std::string &b, bool enabled);
  void setRouterState(const std::string &router_id, bool enabled);
  void originatePrefix(const std::string &router_id, const std::string &prefix);
  void withdrawPrefix(const std::string &router_id, const std::string &prefix);
  bool waitForConvergence(std::chrono::milliseconds timeout);
  [[nodiscard]] bool isConverged() const;

  [[nodiscard]] std::vector<RouterSnapshot> routersSnapshot() const;
  [[nodiscard]] RibSnapshot ribSnapshot(const std::string &router_id) const;
  [[nodiscard]] std::vector<PeerSnapshot>
  peersSnapshot(const std::string &router_id) const;
  [[nodiscard]] const std::filesystem::path &logFile() const;

private:
  struct LinkRuntime {
    LinkConfig config;
  };

  static std::string edgeKey(const std::string &a, const std::string &b);

  void validateConfig() const;
  void buildRouters();
  void normalizeNeighborsFromLinks();
  [[nodiscard]] bool messageStillDeliverable(const std::string &from,
                                             const std::string &to) const;
  [[nodiscard]] std::optional<LinkRuntime> linkFor(const std::string &a,
                                                   const std::string &b) const;
  [[nodiscard]] std::chrono::milliseconds convergenceQuietPeriod() const;
  [[nodiscard]] std::filesystem::path makeRunDirectory() const;

  TopologyConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<BgpRouter>> routers_;
  std::unordered_map<std::string, LinkRuntime> links_;
  std::unique_ptr<ThreadPool> pool_;
  std::unique_ptr<BmpCollector> bmp_;
  std::filesystem::path run_dir_;
  std::atomic<std::uint64_t> sequence_{0};
  bool running_ = false;
};

} // namespace toposim
