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

  void start(bool send_open_messages = true);
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
  struct PendingUpdate {
    std::vector<std::string> nlri;
    std::vector<std::string> withdrawn;
    std::optional<RouteEntry> route;
    std::map<std::string, std::uint64_t> generations;
  };

  struct MraiQueue {
    std::vector<PendingUpdate> updates;
    bool flush_scheduled = false;
  };

  [[nodiscard]] PendingUpdate makePendingUpdateLocked(
      const NeighborConfig &neighbor, const std::vector<std::string> &nlri,
      const std::vector<std::string> &withdrawn,
      const std::optional<RouteEntry> &route);
  void sendOpenToNeighbor(const NeighborConfig &neighbor);
  void sendUpdateToNeighbor(const NeighborConfig &neighbor,
                            const std::vector<std::string> &nlri,
                            const std::vector<std::string> &withdrawn,
                            const std::optional<RouteEntry> &route);
  void sendUpdateNowToNeighbor(const NeighborConfig &neighbor,
                               const PendingUpdate &update);
  void sendUpdatesNowToNeighbor(const NeighborConfig &neighbor,
                                const std::vector<PendingUpdate> &updates);
  void scheduleMraiFlush(const NeighborConfig &neighbor,
                         std::chrono::milliseconds delay);
  void flushMraiUpdates(const std::string &peer_id);
  [[nodiscard]] bool updateStillCurrentLocked(
      const std::string &peer_id,
      const std::map<std::string, std::uint64_t> &generations) const;
  [[nodiscard]] bool commitUpdateDelivery(
      const std::string &peer_id, const std::vector<std::string> &nlri,
      const std::vector<std::string> &withdrawn,
      const std::optional<RouteEntry> &route,
      const std::map<std::string, std::uint64_t> &generations);
  void cancelPendingUpdate(const std::string &peer_id,
                           const std::string &prefix);
  void runDecisionProcessFor(const std::set<std::string> &changed_prefixes);
  void advertiseCurrentRoutesToNeighbor(const NeighborConfig &neighbor);
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
  std::map<std::string, std::chrono::steady_clock::time_point>
      mrai_next_update_;
  std::map<std::string, MraiQueue> mrai_queues_;
  std::map<std::string, std::map<std::string, std::uint64_t>>
      update_generations_;
  std::uint64_t update_generation_counter_ = 0;
};

} // namespace toposim
