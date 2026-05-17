#include "toposim/BgpRouter.hpp"

#include <algorithm>
#include <tuple>

#include "toposim/TopoManager.hpp"

namespace toposim {
namespace {

int sessionPreference(SessionType type) {
  return type == SessionType::Ebgp ? 0 : 1;
}

std::string clusterAppend(const std::string &existing,
                          const std::string &cluster_id) {
  if (cluster_id.empty()) {
    return existing;
  }
  if (existing.empty()) {
    return cluster_id;
  }
  return existing + "," + cluster_id;
}

} // namespace

BgpRouter::BgpRouter(RouterConfig config) : config_(std::move(config)) {
  if (config_.cluster_id.empty()) {
    config_.cluster_id = config_.router_id;
  }
  for (const auto &neighbor : config_.neighbors) {
    addOrUpdateNeighbor(neighbor);
  }
}

const std::string &BgpRouter::id() const { return config_.id; }

const std::string &BgpRouter::routerId() const { return config_.router_id; }

std::uint32_t BgpRouter::asn() const { return config_.asn; }

bool BgpRouter::isRouteReflector() const {
  std::lock_guard lock(mutex_);
  return std::any_of(neighbors_.begin(), neighbors_.end(),
                     [](const auto &entry) { return entry.second.rr_client; });
}

bool BgpRouter::isActive() const {
  std::lock_guard lock(mutex_);
  return active_;
}

void BgpRouter::attachManager(TopoManager *manager) { manager_ = manager; }

void BgpRouter::addOrUpdateNeighbor(const NeighborConfig &neighbor) {
  std::lock_guard lock(mutex_);
  auto normalized = neighbor;
  if (normalized.remote_asn == 0) {
    normalized.remote_asn = config_.asn;
  }
  neighbors_[normalized.id] = normalized;
  peer_states_.try_emplace(normalized.id, PeerState::Idle);
}

std::optional<NeighborConfig>
BgpRouter::neighbor(const std::string &peer_id) const {
  std::lock_guard lock(mutex_);
  const auto it = neighbors_.find(peer_id);
  if (it == neighbors_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<NeighborConfig> BgpRouter::neighbors() const {
  std::lock_guard lock(mutex_);
  std::vector<NeighborConfig> result;
  result.reserve(neighbors_.size());
  for (const auto &[_, neighbor] : neighbors_) {
    result.push_back(neighbor);
  }
  return result;
}

void BgpRouter::start(bool send_open_messages) {
  std::vector<NeighborConfig> enabled_neighbors;
  {
    std::lock_guard lock(mutex_);
    active_ = true;
    local_routes_.clear();
    adj_rib_in_.clear();
    loc_rib_.clear();
    adj_rib_out_.clear();

    for (const auto &prefix : config_.originated_prefixes) {
      RouteEntry route;
      route.prefix = prefix;
      route.learned_from = config_.id;
      route.local_origin = true;
      route.attributes.next_hop = config_.router_id;
      route.attributes.local_pref = 100;
      local_routes_[prefix] = route;
      loc_rib_[prefix] = route;
    }

    for (auto &[peer_id, state] : peer_states_) {
      state = PeerState::Idle;
    }
    for (const auto &[_, neighbor] : neighbors_) {
      if (neighbor.enabled) {
        enabled_neighbors.push_back(neighbor);
      }
    }
  }

  if (send_open_messages) {
    for (const auto &neighbor : enabled_neighbors) {
      sendOpenToNeighbor(neighbor);
    }
  }
}

void BgpRouter::stop() {
  std::lock_guard lock(mutex_);
  active_ = false;
  adj_rib_in_.clear();
  loc_rib_.clear();
  adj_rib_out_.clear();
  mrai_next_update_.clear();
  update_generations_.clear();
  for (auto &[_, state] : peer_states_) {
    state = PeerState::Idle;
  }
}

void BgpRouter::receiveMessage(const BgpMessage &message) {
  {
    std::lock_guard lock(mutex_);
    if (!active_ || message.to != config_.id) {
      return;
    }
  }

  onMessageReceived(message);

  switch (message.type) {
  case BgpMessageType::Open:
    onOpenMessage(message);
    break;
  case BgpMessageType::Update:
    onUpdateMessage(message);
    break;
  case BgpMessageType::Notification:
    onNotificationMessage(message);
    break;
  }
}

void BgpRouter::neighborDown(const std::string &peer_id) {
  std::set<std::string> changed_prefixes;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    peer_states_[peer_id] = PeerState::Idle;
    if (auto it = adj_rib_in_.find(peer_id); it != adj_rib_in_.end()) {
      for (const auto &[prefix, _] : it->second) {
        changed_prefixes.insert(prefix);
      }
      adj_rib_in_.erase(it);
    }
    adj_rib_out_.erase(peer_id);
    mrai_next_update_.erase(peer_id);
    update_generations_.erase(peer_id);
  }

  if (!changed_prefixes.empty()) {
    runDecisionProcessFor(changed_prefixes);
  }
}

void BgpRouter::neighborUp(const std::string &peer_id) {
  auto neighbor_config = neighbor(peer_id);
  if (!neighbor_config || !neighbor_config->enabled) {
    return;
  }
  sendOpenToNeighbor(*neighbor_config);
}

void BgpRouter::originatePrefix(const std::string &prefix) {
  {
    std::lock_guard lock(mutex_);
    if (std::find(config_.originated_prefixes.begin(),
                  config_.originated_prefixes.end(),
                  prefix) == config_.originated_prefixes.end()) {
      config_.originated_prefixes.push_back(prefix);
    }
    RouteEntry route;
    route.prefix = prefix;
    route.learned_from = config_.id;
    route.local_origin = true;
    route.attributes.next_hop = config_.router_id;
    local_routes_[prefix] = route;
  }
  runDecisionProcessFor({prefix});
}

void BgpRouter::withdrawLocalPrefix(const std::string &prefix) {
  {
    std::lock_guard lock(mutex_);
    local_routes_.erase(prefix);
    config_.originated_prefixes.erase(
        std::remove(config_.originated_prefixes.begin(),
                    config_.originated_prefixes.end(), prefix),
        config_.originated_prefixes.end());
  }
  runDecisionProcessFor({prefix});
}

RibSnapshot BgpRouter::ribSnapshot() const {
  std::lock_guard lock(mutex_);
  RibSnapshot result;
  result.router = config_.id;
  for (const auto &[_, route] : loc_rib_) {
    result.loc_rib.push_back(route);
  }
  result.adj_rib_in = adj_rib_in_;
  result.adj_rib_out = adj_rib_out_;
  return result;
}

std::vector<PeerSnapshot> BgpRouter::peerSnapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<PeerSnapshot> result;
  result.reserve(neighbors_.size());
  for (const auto &[peer_id, neighbor] : neighbors_) {
    result.push_back({
        .id = peer_id,
        .remote_asn = neighbor.remote_asn,
        .session_type = neighbor.session_type,
        .rr_client = neighbor.rr_client,
        .enabled = neighbor.enabled,
        .mrai_ms = neighbor.mrai_ms,
        .state = peer_states_.at(peer_id),
    });
  }
  return result;
}

void BgpRouter::onMessageReceived(const BgpMessage &message) {
  (void)message;
}

void BgpRouter::onOpenMessage(const BgpMessage &message) {
  if (!message.open) {
    return;
  }

  std::optional<NeighborConfig> neighbor_config;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    const auto it = neighbors_.find(message.from);
    if (it == neighbors_.end() || !it->second.enabled) {
      return;
    }
    if (message.open->version != 4 ||
        message.open->asn != it->second.remote_asn ||
        message.open->router_id.empty()) {
      return;
    }
    peer_states_[message.from] = PeerState::Established;
    neighbor_config = it->second;
  }
  advertiseCurrentRoutesToNeighbor(*neighbor_config);
}

void BgpRouter::onUpdateMessage(const BgpMessage &message) {
  if (!message.update) {
    return;
  }

  std::set<std::string> changed_prefixes;
  std::optional<NeighborConfig> from_peer;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    const auto neighbor_it = neighbors_.find(message.from);
    const auto state_it = peer_states_.find(message.from);
    if (neighbor_it == neighbors_.end() || !neighbor_it->second.enabled ||
        state_it == peer_states_.end() ||
        state_it->second != PeerState::Established) {
      return;
    }
    from_peer = neighbor_it->second;

    for (const auto &prefix : message.update->withdrawn_routes) {
      if (auto peer_rib = adj_rib_in_.find(message.from);
          peer_rib != adj_rib_in_.end()) {
        peer_rib->second.erase(prefix);
      }
      changed_prefixes.insert(prefix);
    }
  }

  std::vector<RouteEntry> imported_routes;
  if (!message.update->nlri.empty() && from_peer) {
    RouteEntry route;
    route.attributes = message.update->path_attributes;
    route.learned_from = message.from;
    route.source_session = from_peer->session_type;
    route.local_origin = false;

    if (!containsOwnAs(route.attributes)) {
      for (const auto &prefix : message.update->nlri) {
        route.prefix = prefix;
        if (importRouteAllowed(route, *from_peer)) {
          imported_routes.push_back(route);
        }
      }
    }
  }

  if (!imported_routes.empty()) {
    std::lock_guard lock(mutex_);
    const auto neighbor_it = neighbors_.find(message.from);
    const auto state_it = peer_states_.find(message.from);
    if (active_ && neighbor_it != neighbors_.end() &&
        neighbor_it->second.enabled && state_it != peer_states_.end() &&
        state_it->second == PeerState::Established) {
      for (const auto &route : imported_routes) {
        adj_rib_in_[message.from][route.prefix] = route;
        changed_prefixes.insert(route.prefix);
      }
    }
  }

  if (!changed_prefixes.empty()) {
    runDecisionProcessFor(changed_prefixes);
  }
}

void BgpRouter::onNotificationMessage(const BgpMessage &message) {
  neighborDown(message.from);
}

bool BgpRouter::importRouteAllowed(const RouteEntry &route,
                                   const NeighborConfig &) const {
  if (route.attributes.originator_id == config_.router_id) {
    return false;
  }
  if (route.attributes.cluster_list) {
    const auto &clusters = *route.attributes.cluster_list;
    std::size_t start = 0;
    while (start <= clusters.size()) {
      const auto end = clusters.find(',', start);
      const auto token =
          clusters.substr(start, end == std::string::npos ? end : end - start);
      if (token == config_.cluster_id) {
        return false;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }
  return true;
}

bool BgpRouter::exportRouteAllowed(const RouteEntry &route,
                                   const NeighborConfig &to_peer) const {
  if (!to_peer.enabled) {
    return false;
  }
  if (route.learned_from == to_peer.id) {
    return false;
  }
  if (route.local_origin || route.source_session == SessionType::Ebgp) {
    return true;
  }
  if (route.source_session == SessionType::Ibgp &&
      to_peer.session_type == SessionType::Ebgp) {
    return true;
  }
  if (route.source_session == SessionType::Ibgp &&
      to_peer.session_type == SessionType::Ibgp) {
    return shouldReflectIbgpRoute(route, to_peer);
  }
  return true;
}

RouteEntry
BgpRouter::transformRouteForPeer(const RouteEntry &route,
                                 const NeighborConfig &to_peer) const {
  RouteEntry transformed = route;
  transformed.learned_from = config_.id;
  transformed.local_origin = false;
  transformed.source_session = to_peer.session_type;

  if (to_peer.session_type == SessionType::Ebgp) {
    transformed.attributes.as_path.insert(
        transformed.attributes.as_path.begin(), config_.asn);
    transformed.attributes.next_hop = config_.router_id;
  } else if (transformed.attributes.next_hop.empty()) {
    transformed.attributes.next_hop = config_.router_id;
  }

  if (isRouteReflector() && !route.local_origin &&
      route.source_session == SessionType::Ibgp &&
      to_peer.session_type == SessionType::Ibgp) {
    if (!transformed.attributes.originator_id) {
      transformed.attributes.originator_id = route.attributes.next_hop.empty()
                                                 ? route.learned_from
                                                 : route.attributes.next_hop;
    }
    transformed.attributes.cluster_list = clusterAppend(
        transformed.attributes.cluster_list.value_or(""), config_.cluster_id);
  }

  return transformed;
}

std::optional<RouteEntry>
BgpRouter::selectBestRoute(const std::string &,
                           const std::vector<RouteEntry> &candidates) const {
  if (candidates.empty()) {
    return std::nullopt;
  }

  return *std::min_element(
      candidates.begin(), candidates.end(),
      [](const auto &lhs, const auto &rhs) {
        if (lhs.attributes.local_pref != rhs.attributes.local_pref) {
          return lhs.attributes.local_pref > rhs.attributes.local_pref;
        }
        return std::tuple{
                   lhs.attributes.as_path.size(),
                   lhs.attributes.med,
                   sessionPreference(lhs.source_session),
                   lhs.attributes.next_hop,
                   lhs.learned_from,
               } < std::tuple{
                       rhs.attributes.as_path.size(),
                       rhs.attributes.med,
                       sessionPreference(rhs.source_session),
                       rhs.attributes.next_hop,
                       rhs.learned_from,
                   };
      });
}

void BgpRouter::sendMessage(const std::string &peer_id,
                            BgpMessage message,
                            std::chrono::milliseconds extra_delay,
                            std::function<bool()> delivery_guard) const {
  if (!manager_) {
    return;
  }
  message.from = config_.id;
  message.to = peer_id;
  manager_->sendMessage(config_.id, peer_id, std::move(message), extra_delay,
                        std::move(delivery_guard));
}

void BgpRouter::sendOpenToNeighbor(const NeighborConfig &neighbor) {
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    peer_states_[neighbor.id] = PeerState::OpenSent;
  }

  BgpMessage message;
  message.type = BgpMessageType::Open;
  message.open = BgpOpenPayload{
      .version = 4,
      .asn = config_.asn,
      .hold_time_seconds = neighbor.hold_time_seconds,
      .router_id = config_.router_id,
  };
  sendMessage(neighbor.id, std::move(message));
}

void BgpRouter::sendUpdateToNeighbor(const NeighborConfig &neighbor,
                                     const std::vector<std::string> &nlri,
                                     const std::vector<std::string> &withdrawn,
                                     const std::optional<RouteEntry> &route) {
  BgpMessage message;
  message.type = BgpMessageType::Update;
  BgpUpdatePayload update;
  update.nlri = nlri;
  update.withdrawn_routes = withdrawn;
  if (route) {
    update.path_attributes = route->attributes;
  }
  message.update = std::move(update);
  auto schedule = scheduleUpdate(neighbor, nlri, withdrawn);
  auto generations = std::move(schedule.generations);
  auto delivery_guard = [this, peer_id = neighbor.id, nlri, withdrawn, route,
                         generations = std::move(generations)]() {
    return commitUpdateDelivery(peer_id, nlri, withdrawn, route, generations);
  };
  sendMessage(neighbor.id, std::move(message), schedule.delay,
              std::move(delivery_guard));
}

BgpRouter::UpdateSchedule
BgpRouter::scheduleUpdate(const NeighborConfig &neighbor,
                          const std::vector<std::string> &nlri,
                          const std::vector<std::string> &withdrawn) {
  UpdateSchedule schedule;
  std::lock_guard lock(mutex_);

  for (const auto &prefix : withdrawn) {
    const auto generation = ++update_generation_counter_;
    update_generations_[neighbor.id][prefix] = generation;
    schedule.generations[prefix] = generation;
  }

  for (const auto &prefix : nlri) {
    const auto generation = ++update_generation_counter_;
    update_generations_[neighbor.id][prefix] = generation;
    schedule.generations[prefix] = generation;
  }

  std::set<std::string> affected_prefixes;
  affected_prefixes.insert(nlri.begin(), nlri.end());
  affected_prefixes.insert(withdrawn.begin(), withdrawn.end());
  if (neighbor.mrai_ms == 0 || affected_prefixes.empty()) {
    return schedule;
  }

  const auto interval = std::chrono::milliseconds(neighbor.mrai_ms);
  const auto now = std::chrono::steady_clock::now();
  auto send_at = now;
  for (const auto &prefix : affected_prefixes) {
    const auto peer_it = mrai_next_update_.find(neighbor.id);
    if (peer_it == mrai_next_update_.end()) {
      continue;
    }
    const auto prefix_it = peer_it->second.find(prefix);
    if (prefix_it != peer_it->second.end() && prefix_it->second > send_at) {
      send_at = prefix_it->second;
    }
  }
  schedule.delay =
      std::chrono::duration_cast<std::chrono::milliseconds>(send_at - now);

  const auto next_available = send_at + interval;
  for (const auto &prefix : affected_prefixes) {
    mrai_next_update_[neighbor.id][prefix] = next_available;
  }

  return schedule;
}

bool BgpRouter::commitUpdateDelivery(
    const std::string &peer_id, const std::vector<std::string> &nlri,
    const std::vector<std::string> &withdrawn,
    const std::optional<RouteEntry> &route,
    const std::map<std::string, std::uint64_t> &generations) {
  std::lock_guard lock(mutex_);
  if (!active_) {
    return false;
  }
  const auto neighbor_it = neighbors_.find(peer_id);
  const auto state_it = peer_states_.find(peer_id);
  if (neighbor_it == neighbors_.end() || !neighbor_it->second.enabled ||
      state_it == peer_states_.end() ||
      state_it->second != PeerState::Established) {
    return false;
  }
  const auto peer_it = update_generations_.find(peer_id);
  if (!generations.empty() && peer_it == update_generations_.end()) {
    return false;
  }
  for (const auto &[prefix, generation] : generations) {
    const auto prefix_it = peer_it->second.find(prefix);
    if (prefix_it == peer_it->second.end() ||
        prefix_it->second != generation) {
      return false;
    }
  }

  for (const auto &prefix : withdrawn) {
    if (auto peer_out = adj_rib_out_.find(peer_id);
        peer_out != adj_rib_out_.end()) {
      peer_out->second.erase(prefix);
    }
  }
  if (!nlri.empty()) {
    if (!route) {
      return false;
    }
    for (const auto &prefix : nlri) {
      auto committed = *route;
      committed.prefix = prefix;
      adj_rib_out_[peer_id][prefix] = std::move(committed);
    }
  }
  return true;
}

void BgpRouter::cancelPendingUpdate(const std::string &peer_id,
                                    const std::string &prefix) {
  std::lock_guard lock(mutex_);
  update_generations_[peer_id][prefix] = ++update_generation_counter_;
}

void BgpRouter::runDecisionProcessFor(
    const std::set<std::string> &changed_prefixes) {
  std::set<std::string> prefixes = changed_prefixes;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    if (prefixes.empty()) {
      for (const auto &[prefix, _] : loc_rib_) {
        prefixes.insert(prefix);
      }
      for (const auto &[prefix, _] : local_routes_) {
        prefixes.insert(prefix);
      }
    }
  }

  std::map<std::string, std::optional<RouteEntry>> changes;
  for (const auto &prefix : prefixes) {
    while (true) {
      std::optional<RouteEntry> old_route;
      std::vector<RouteEntry> candidates;
      {
        std::lock_guard lock(mutex_);
        if (!active_) {
          return;
        }
        const auto old_it = loc_rib_.find(prefix);
        if (old_it != loc_rib_.end()) {
          old_route = old_it->second;
        }
        candidates = candidatesForPrefix(prefix);
      }

      auto selected = selectBestRoute(prefix, candidates);

      {
        std::lock_guard lock(mutex_);
        if (!active_) {
          return;
        }
        if (candidatesForPrefix(prefix) != candidates) {
          continue;
        }

        const auto current_it = loc_rib_.find(prefix);
        const auto current_route =
            current_it == loc_rib_.end()
                ? std::optional<RouteEntry>{}
                : std::optional<RouteEntry>{current_it->second};
        if (current_route != old_route) {
          continue;
        }

        const bool changed =
            (!current_route && selected) || (current_route && !selected) ||
            (current_route && selected && *current_route != *selected);

        if (changed) {
          if (selected) {
            loc_rib_[prefix] = *selected;
          } else {
            loc_rib_.erase(prefix);
          }
          changes[prefix] = selected;
        }
        break;
      }
    }
  }

  if (!changes.empty()) {
    disseminateChangedRoutes(changes);
  }
}

void BgpRouter::advertiseCurrentRoutesToNeighbor(
    const NeighborConfig &neighbor) {
  std::map<std::string, RouteEntry> current_routes;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    const auto state_it = peer_states_.find(neighbor.id);
    if (state_it == peer_states_.end() ||
        state_it->second != PeerState::Established) {
      return;
    }
    current_routes = loc_rib_;
  }

  for (const auto &[prefix, route] : current_routes) {
    if (!exportRouteAllowed(route, neighbor)) {
      continue;
    }
    auto transformed = transformRouteForPeer(route, neighbor);
    transformed.prefix = prefix;
    sendUpdateToNeighbor(neighbor, {prefix}, {}, transformed);
  }
}

void BgpRouter::disseminateChangedRoutes(
    const std::map<std::string, std::optional<RouteEntry>> &changes) {
  std::vector<NeighborConfig> peers;
  {
    std::lock_guard lock(mutex_);
    if (!active_) {
      return;
    }
    for (const auto &[_, neighbor] : neighbors_) {
      if (neighbor.enabled &&
          peer_states_[neighbor.id] == PeerState::Established) {
        peers.push_back(neighbor);
      }
    }
  }

  for (const auto &neighbor : peers) {
    for (const auto &[prefix, maybe_route] : changes) {
      if (!maybe_route || !exportRouteAllowed(*maybe_route, neighbor)) {
        bool was_advertised = false;
        {
          std::lock_guard lock(mutex_);
          if (auto peer_out = adj_rib_out_.find(neighbor.id);
              peer_out != adj_rib_out_.end()) {
            was_advertised = peer_out->second.contains(prefix);
          }
        }
        if (was_advertised) {
          sendUpdateToNeighbor(neighbor, {}, {prefix}, std::nullopt);
        } else {
          cancelPendingUpdate(neighbor.id, prefix);
        }
        continue;
      }
      auto transformed = transformRouteForPeer(*maybe_route, neighbor);
      transformed.prefix = prefix;
      sendUpdateToNeighbor(neighbor, {prefix}, {}, transformed);
    }
  }
}

std::vector<RouteEntry>
BgpRouter::candidatesForPrefix(const std::string &prefix) const {
  std::vector<RouteEntry> candidates;
  if (auto it = local_routes_.find(prefix); it != local_routes_.end()) {
    candidates.push_back(it->second);
  }
  for (const auto &[_, routes] : adj_rib_in_) {
    if (auto it = routes.find(prefix); it != routes.end()) {
      candidates.push_back(it->second);
    }
  }
  return candidates;
}

bool BgpRouter::shouldReflectIbgpRoute(const RouteEntry &route,
                                       const NeighborConfig &to_peer) const {
  bool has_rr_client = false;
  bool learned_from_client = false;
  {
    std::lock_guard lock(mutex_);
    has_rr_client =
        std::any_of(neighbors_.begin(), neighbors_.end(),
                    [](const auto &entry) { return entry.second.rr_client; });
    const auto learned_neighbor = neighbors_.find(route.learned_from);
    learned_from_client = learned_neighbor != neighbors_.end() &&
                          learned_neighbor->second.rr_client;
  }

  if (!has_rr_client) {
    return false;
  }

  if (learned_from_client) {
    return true;
  }
  return to_peer.rr_client;
}

bool BgpRouter::containsOwnAs(const PathAttributes &attrs) const {
  return std::find(attrs.as_path.begin(), attrs.as_path.end(), config_.asn) !=
         attrs.as_path.end();
}

} // namespace toposim
