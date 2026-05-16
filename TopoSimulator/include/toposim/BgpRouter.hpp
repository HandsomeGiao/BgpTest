#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "toposim/BgpTypes.hpp"

namespace toposim {

class TopoManager;

class BgpRouter {
public:
  explicit BgpRouter(RouterConfig config);
  virtual ~BgpRouter() = default;

  BgpRouter(const BgpRouter &) = delete;
  BgpRouter &operator=(const BgpRouter &) = delete;

  [[nodiscard]] const std::string &id() const;
  [[nodiscard]] const std::string &routerId() const;
  [[nodiscard]] std::uint32_t asn() const;
  [[nodiscard]] bool isRouteReflector() const;
  [[nodiscard]] bool isActive() const;

  void attachManager(TopoManager *manager);
  void addOrUpdateNeighbor(const NeighborConfig &neighbor);
  [[nodiscard]] std::optional<NeighborConfig>
  neighbor(const std::string &peer_id) const;
  [[nodiscard]] std::vector<NeighborConfig> neighbors() const;

  void start();
  void stop();
  void receiveMessage(const BgpMessage &message);
  void neighborDown(const std::string &peer_id);
  void neighborUp(const std::string &peer_id);
  void originatePrefix(const std::string &prefix);
  void withdrawLocalPrefix(const std::string &prefix);

  [[nodiscard]] RibSnapshot ribSnapshot() const;
  [[nodiscard]] std::vector<PeerSnapshot> peerSnapshot() const;

protected:
  virtual void onMessageReceived(const BgpMessage &message);
  virtual void onOpenMessage(const BgpMessage &message);
  virtual void onKeepaliveMessage(const BgpMessage &message);
  virtual void onUpdateMessage(const BgpMessage &message);
  virtual void onNotificationMessage(const BgpMessage &message);

  virtual bool importRouteAllowed(const RouteEntry &route,
                                  const NeighborConfig &from_peer) const;
  virtual bool exportRouteAllowed(const RouteEntry &route,
                                  const NeighborConfig &to_peer) const;
  virtual RouteEntry transformRouteForPeer(const RouteEntry &route,
                                           const NeighborConfig &to_peer) const;
  virtual std::optional<RouteEntry>
  selectBestRoute(const std::string &prefix,
                  const std::vector<RouteEntry> &candidates) const;

  void sendMessage(const std::string &peer_id, BgpMessage message,
                   std::chrono::milliseconds extra_delay =
                       std::chrono::milliseconds{0},
                   std::function<bool()> delivery_guard = {}) const;

private:
  struct UpdateSchedule {
    std::chrono::milliseconds delay{0};
    std::map<std::string, std::uint64_t> generations;
  };

  void sendOpenToNeighbor(const NeighborConfig &neighbor);
  void sendKeepaliveToNeighbor(const NeighborConfig &neighbor);
  void sendUpdateToNeighbor(const NeighborConfig &neighbor,
                            const std::vector<std::string> &nlri,
                            const std::vector<std::string> &withdrawn,
                            const std::optional<RouteEntry> &route);
  UpdateSchedule scheduleUpdate(const NeighborConfig &neighbor,
                                const std::vector<std::string> &nlri,
                                const std::vector<std::string> &withdrawn);
  [[nodiscard]] bool
  updateStillCurrent(const std::string &peer_id,
                     const std::map<std::string, std::uint64_t> &generations)
      const;
  void runDecisionProcessFor(const std::set<std::string> &changed_prefixes);
  void disseminateChangedRoutes(
      const std::map<std::string, std::optional<RouteEntry>> &changes);
  [[nodiscard]] std::vector<RouteEntry>
  candidatesForPrefix(const std::string &prefix) const;
  [[nodiscard]] bool
  shouldReflectIbgpRoute(const RouteEntry &route,
                         const NeighborConfig &to_peer) const;
  [[nodiscard]] bool containsOwnAs(const PathAttributes &attrs) const;

  RouterConfig config_;
  TopoManager *manager_ = nullptr;

  mutable std::mutex mutex_;
  bool active_ = false;
  std::unordered_map<std::string, NeighborConfig> neighbors_;
  std::unordered_map<std::string, PeerState> peer_states_;
  std::map<std::string, RouteEntry> local_routes_;
  std::map<std::string, std::map<std::string, RouteEntry>> adj_rib_in_;
  std::map<std::string, RouteEntry> loc_rib_;
  std::map<std::string, std::map<std::string, RouteEntry>> adj_rib_out_;
  std::map<std::string, std::map<std::string, std::chrono::steady_clock::time_point>>
      mrai_next_advertisement_;
  std::map<std::string, std::map<std::string, std::uint64_t>>
      update_generations_;
  std::uint64_t update_generation_counter_ = 0;
};

} // namespace toposim
