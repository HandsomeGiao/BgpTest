#include "toposim/TopoManager.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace toposim {
namespace {

std::uint32_t defaultWorkerCount(std::uint32_t configured) {
  if (configured != 0) {
    return configured;
  }
  const auto detected = std::thread::hardware_concurrency();
  return detected == 0 ? 4 : detected;
}

std::string runTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) %
                      1000;
  std::tm tm{};
  localtime_s(&tm, &time);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3)
      << std::setfill('0') << millis.count();
  return oss.str();
}

bool isValidIpv4Address(const std::string &value, bool allow_zero) {
  if (!allow_zero && value == "0.0.0.0") {
    return false;
  }

  std::size_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    if (pos >= value.size() ||
        !std::isdigit(static_cast<unsigned char>(value[pos]))) {
      return false;
    }

    unsigned int octet = 0;
    while (pos < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[pos]))) {
      octet = octet * 10U + static_cast<unsigned int>(value[pos] - '0');
      if (octet > 255U) {
        return false;
      }
      ++pos;
    }

    if (part < 3) {
      if (pos >= value.size() || value[pos] != '.') {
        return false;
      }
      ++pos;
    }
  }

  return pos == value.size();
}

bool isValidBgpRouterId(const std::string &value) {
  return isValidIpv4Address(value, false);
}

bool isValidIpv4Cidr(const std::string &value) {
  const auto slash = value.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 == value.size()) {
    return false;
  }

  if (!isValidIpv4Address(value.substr(0, slash), true)) {
    return false;
  }

  const auto prefix_length = value.substr(slash + 1);
  if (prefix_length.size() > 2 ||
      !std::all_of(prefix_length.begin(), prefix_length.end(),
                   [](unsigned char ch) { return std::isdigit(ch); })) {
    return false;
  }

  const auto length = std::stoul(prefix_length);
  return length <= 32;
}

std::atomic<std::uint64_t> run_counter{0};

} // namespace

TopoManager::TopoManager(TopologyConfig config) : config_(std::move(config)) {
  validateConfig();
  buildRouters();
  normalizeNeighborsFromLinks();

  for (const auto &link : config_.links) {
    links_[edgeKey(link.a, link.b)] = LinkRuntime{link};
  }

  const auto worker_count =
      defaultWorkerCount(config_.simulation.worker_threads);
  pool_ = std::make_unique<ThreadPool>(worker_count);
  run_dir_ = makeRunDirectory();
  bmp_log_file_ = run_dir_ / "bmp_collector.log";
  bmp_database_file_ = run_dir_ / "bmp_collector.sqlite";
  BmpLogManager::instance().initialize(bmp_log_file_, bmp_database_file_);
}

TopoManager::~TopoManager() { stop(); }

void TopoManager::start() {
  std::vector<std::shared_ptr<BgpRouter>> routers;
  {
    std::lock_guard lock(mutex_);
    if (running_) {
      return;
    }
    if (!pool_) {
      throw std::runtime_error(
          "TopoManager cannot be restarted after stop(); create a new "
          "TopoManager for another simulation run.");
    }
    running_ = true;
    last_message_processed_at_ = {};
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
    for (const auto &[_, router] : routers_) {
      routers.push_back(router);
    }
  }

  BmpLogManager::instance().recordTopologyEvent(
      "simulation_started",
      {
          {"name", config_.simulation.name},
          {"router_count",
           BmpEventValue{static_cast<std::uint64_t>(routers.size())}},
          {"link_count",
           BmpEventValue{static_cast<std::uint64_t>(links_.size())}},
      });

  for (const auto &router : routers) {
    router->start(false);
  }
  for (const auto &router : routers) {
    for (const auto &neighbor : router->neighbors()) {
      if (neighbor.enabled) {
        router->neighborUp(neighbor.id);
      }
    }
  }
}

void TopoManager::stop() {
  std::vector<std::shared_ptr<BgpRouter>> routers;
  std::unique_ptr<ThreadPool> pool;
  {
    std::lock_guard lock(mutex_);
    if (!running_ && !pool_) {
      return;
    }
    running_ = false;
    for (const auto &[_, router] : routers_) {
      routers.push_back(router);
    }
    pool = std::move(pool_);
  }
  for (const auto &router : routers) {
    router->stop();
  }
  if (pool) {
    pool->waitForIdle(std::chrono::seconds(5), std::chrono::milliseconds(10));
    pool->stop();
  }
  for (const auto &router : routers) {
    router->attachManager(nullptr);
  }
  BmpLogManager::instance().recordTopologyEvent("simulation_stopped",
                                                BmpEventDetail{});
  BmpLogManager::instance().flush();
}

void TopoManager::sendMessage(const std::string &from, const std::string &to,
                              BgpMessage message,
                              std::chrono::milliseconds extra_delay,
                              std::function<bool()> delivery_guard) {
  std::vector<BgpMessage> messages;
  messages.push_back(std::move(message));
  std::vector<std::function<bool()>> delivery_guards;
  delivery_guards.push_back(std::move(delivery_guard));
  sendMessages(from, to, std::move(messages), extra_delay,
               std::move(delivery_guards));
}

void TopoManager::sendMessages(
    const std::string &from, const std::string &to,
    std::vector<BgpMessage> messages, std::chrono::milliseconds extra_delay,
    std::vector<std::function<bool()>> delivery_guards) {
  if (messages.empty()) {
    return;
  }
  if (delivery_guards.size() < messages.size()) {
    delivery_guards.resize(messages.size());
  }

  std::shared_ptr<std::mutex> delivery_lock;
  {
    std::lock_guard lock(mutex_);
    if (!running_ || !pool_ || !BmpLogManager::instance().initialized()) {
      return;
    }
    auto dst_it = routers_.find(to);
    auto src_it = routers_.find(from);
    if (dst_it == routers_.end() || src_it == routers_.end()) {
      return;
    }
    const auto link = linkFor(from, to);
    if (!link || !link->config.enabled) {
      return;
    }
    auto destination = dst_it->second;
    const auto from_as = src_it->second->asn();
    const auto to_as = dst_it->second->asn();
    const auto delay_ms = link->config.delay_ms;
    for (auto &message : messages) {
      message.from = from;
      message.to = to;
      message.sequence = ++sequence_;
    }
    auto &lock_slot = delivery_locks_[directedKey(from, to)];
    if (!lock_slot) {
      lock_slot = std::make_shared<std::mutex>();
    }
    delivery_lock = lock_slot;
    last_convergence_activity_at_ = std::chrono::steady_clock::now();

    pool_->enqueue([this, destination = std::move(destination), delay_ms,
                    extra_delay,
                    delivery_guards = std::move(delivery_guards),
                    from_as, to_as, delivery_lock = std::move(delivery_lock),
                    from, to,
                    messages = std::move(messages)]() mutable {
      if (extra_delay.count() > 0) {
        std::this_thread::sleep_for(extra_delay);
      }
      std::lock_guard ordered_delivery(*delivery_lock);
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      if (!messageStillDeliverable(from, to)) {
        markConvergenceActivity();
        return;
      }
      bool delivered_any = false;
      std::vector<BgpMessage> delivered_messages;
      delivered_messages.reserve(messages.size());
      for (std::size_t i = 0; i < messages.size(); ++i) {
        auto &message = messages[i];
        auto &delivery_guard = delivery_guards[i];
        if (delivery_guard && !delivery_guard()) {
          continue;
        }
        BmpLogManager::instance().recordReceive(message.to, message, from_as,
                                                to_as);
        delivered_messages.push_back(std::move(message));
        delivered_any = true;
      }
      if (!delivered_messages.empty()) {
        destination->receiveMessages(delivered_messages);
      }
      {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (delivered_any) {
          last_message_processed_at_ = now;
        }
        last_convergence_activity_at_ = now;
      }
    });
  }
}

void TopoManager::scheduleTask(std::chrono::milliseconds delay,
                               std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (!running_ || !pool_) {
      return;
    }
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
    pool_->enqueue([this, delay, task = std::move(task)]() mutable {
      if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
      }
      task();
      markConvergenceActivity();
    });
  }
}

bool TopoManager::setLinkState(const std::string &a, const std::string &b,
                               bool enabled) {
  std::shared_ptr<BgpRouter> router_a;
  std::shared_ptr<BgpRouter> router_b;
  {
    std::lock_guard lock(mutex_);
    auto it = links_.find(edgeKey(a, b));
    if (it == links_.end()) {
      throw std::runtime_error("Unknown link: " + a + " - " + b);
    }
    if (it->second.config.enabled == enabled) {
      return false;
    }
    it->second.config.enabled = enabled;
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
    router_a = routers_.at(a);
    router_b = routers_.at(b);
  }

  BmpLogManager::instance().recordTopologyEvent(enabled ? "link_up"
                                                       : "link_down",
                                                {{"a", a}, {"b", b}});
  if (enabled) {
    router_a->neighborUp(b);
    router_b->neighborUp(a);
  } else {
    router_a->neighborDown(b);
    router_b->neighborDown(a);
  }
  return true;
}

bool TopoManager::setRouterState(const std::string &router_id, bool enabled) {
  std::vector<std::shared_ptr<BgpRouter>> peer_routers;
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
    if (router->isActive() == enabled) {
      return false;
    }
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
    for (const auto &[_, link] : links_) {
      std::string peer;
      if (link.config.a == router_id) {
        peer = link.config.b;
      } else if (link.config.b == router_id) {
        peer = link.config.a;
      }
      if (!peer.empty()) {
        if (auto peer_router = routers_.find(peer);
            peer_router != routers_.end()) {
          peer_routers.push_back(peer_router->second);
        }
      }
    }
  }

  BmpLogManager::instance().recordTopologyEvent(enabled ? "router_up"
                                                       : "router_down",
                                                {{"router", router_id}});
  if (enabled) {
    router->start();
    for (const auto &peer_router : peer_routers) {
      peer_router->neighborUp(router_id);
    }
  } else {
    router->stop();
    for (const auto &peer_router : peer_routers) {
      peer_router->neighborDown(router_id);
    }
  }
  return true;
}

void TopoManager::originatePrefix(const std::string &router_id,
                                  const std::string &prefix) {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    const auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
  }
  BmpLogManager::instance().recordTopologyEvent(
      "manual_advertise", {{"router", router_id}, {"prefix", prefix}});
  router->originatePrefix(prefix);
}

void TopoManager::withdrawPrefix(const std::string &router_id,
                                 const std::string &prefix) {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    const auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
    last_convergence_activity_at_ = std::chrono::steady_clock::now();
  }
  BmpLogManager::instance().recordTopologyEvent(
      "manual_withdraw", {{"router", router_id}, {"prefix", prefix}});
  router->withdrawLocalPrefix(prefix);
}

bool TopoManager::waitForConvergence(std::chrono::milliseconds timeout) {
  {
    std::lock_guard lock(mutex_);
    if (!pool_) {
      return !running_;
    }
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::max(timeout, convergenceQuietPeriod());
  while (std::chrono::steady_clock::now() < deadline) {
    if (isConverged()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return isConverged();
}

bool TopoManager::isConverged() const {
  const auto quiet_period = convergenceQuietPeriod();
  std::chrono::steady_clock::time_point last_activity;
  bool has_pool = false;
  bool pool_idle = false;
  {
    std::lock_guard lock(mutex_);
    last_activity = last_convergence_activity_at_;
    has_pool = static_cast<bool>(pool_);
    if (pool_) {
      pool_idle = pool_->isIdleFor(quiet_period);
    }
  }
  const auto activity_is_quiet =
      last_activity.time_since_epoch().count() == 0 ||
      last_activity + quiet_period <= std::chrono::steady_clock::now();
  return has_pool && pool_idle && activity_is_quiet;
}

void TopoManager::setBestPathObserver(BestPathObserver observer) {
  std::lock_guard lock(observer_mutex_);
  best_path_observer_ = std::move(observer);
}

void TopoManager::notifyBestPathChanges(
    const std::string &router_id, const std::vector<std::string> &prefixes) {
  BestPathObserver observer;
  {
    std::lock_guard lock(observer_mutex_);
    observer = best_path_observer_;
  }
  if (!observer) {
    return;
  }
  for (const auto &prefix : prefixes) {
    observer(router_id, prefix);
  }
}

void TopoManager::publishCurrentBestPaths() const {
  BestPathObserver observer;
  std::vector<std::pair<std::string, std::shared_ptr<BgpRouter>>> routers;
  {
    std::lock_guard observer_lock(observer_mutex_);
    observer = best_path_observer_;
  }
  if (!observer) {
    return;
  }

  {
    std::lock_guard lock(mutex_);
    routers.reserve(routers_.size());
    for (const auto &[router_id, router] : routers_) {
      routers.emplace_back(router_id, router);
    }
  }

  for (const auto &[router_id, router] : routers) {
    for (const auto &route : router->ribSnapshot().loc_rib) {
      observer(router_id, route.prefix);
    }
  }
}

TopoManager::BestPathSnapshot
TopoManager::bestPathSnapshot(const std::string &router_id,
                               const std::string &prefix) const {
  BestPathSnapshot result;
  result.router = router_id;
  result.prefix = prefix;

  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    const auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      return result;
    }
    router = it->second;
  }

  const auto rib = router->ribSnapshot();
  const auto route_it =
      std::find_if(rib.loc_rib.begin(), rib.loc_rib.end(),
                   [&](const auto &route) { return route.prefix == prefix; });
  if (route_it == rib.loc_rib.end()) {
    return result;
  }

  result.valid = true;
  result.route = *route_it;
  return result;
}

std::chrono::steady_clock::time_point
TopoManager::lastMessageProcessedAt() const {
  std::lock_guard lock(mutex_);
  return last_message_processed_at_;
}

std::chrono::steady_clock::time_point
TopoManager::lastConvergenceActivityAt() const {
  std::lock_guard lock(mutex_);
  return last_convergence_activity_at_;
}

std::vector<RouterSnapshot> TopoManager::routersSnapshot() const {
  std::vector<std::pair<std::string, std::shared_ptr<BgpRouter>>> routers;
  {
    std::lock_guard lock(mutex_);
    routers.reserve(routers_.size());
    for (const auto &[id, router] : routers_) {
      routers.emplace_back(id, router);
    }
  }

  std::vector<RouterSnapshot> result;
  result.reserve(routers.size());
  for (const auto &[id, router] : routers) {
    result.push_back({
        .id = id,
        .router_id = router->routerId(),
        .asn = router->asn(),
        .active = router->isActive(),
        .has_rr_clients = router->isRouteReflector(),
    });
  }
  return result;
}

RibSnapshot TopoManager::ribSnapshot(const std::string &router_id) const {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    const auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
  }
  return router->ribSnapshot();
}

std::vector<PeerSnapshot>
TopoManager::peersSnapshot(const std::string &router_id) const {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    const auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
  }
  return router->peerSnapshot();
}

std::filesystem::path TopoManager::logFile() const {
  std::lock_guard lock(mutex_);
  return bmp_log_file_;
}

std::filesystem::path TopoManager::databaseFile() const {
  std::lock_guard lock(mutex_);
  return bmp_database_file_;
}

std::string TopoManager::edgeKey(const std::string &a, const std::string &b) {
  return a < b ? a + "|" + b : b + "|" + a;
}

std::string TopoManager::directedKey(const std::string &from,
                                     const std::string &to) {
  return from + ">" + to;
}

void TopoManager::validateConfig() const {
  std::unordered_set<std::string> router_ids;
  std::unordered_set<std::string> bgp_router_ids;
  std::unordered_map<std::string, const RouterConfig *> routers_by_id;
  for (const auto &router : config_.routers) {
    if (router.id.empty()) {
      throw std::runtime_error("Router id cannot be empty");
    }
    if (router.id.find('|') != std::string::npos ||
        router.id.find('>') != std::string::npos) {
      throw std::runtime_error("Router id cannot contain '|' or '>': " +
                               router.id);
    }
    if (!router_ids.insert(router.id).second) {
      throw std::runtime_error("Duplicate router id: " + router.id);
    }
    routers_by_id.emplace(router.id, &router);
    if (!isValidBgpRouterId(router.router_id)) {
      throw std::runtime_error(
          "Router " + router.id +
          " has an invalid BGP router id. Expected dotted decimal x.x.x.x "
          "with each octet in 0..255, excluding 0.0.0.0");
    }
    if (!bgp_router_ids.insert(router.router_id).second) {
      throw std::runtime_error("Duplicate BGP router id: " + router.router_id);
    }
    if (router.asn == 0) {
      throw std::runtime_error("Router " + router.id + " has invalid ASN 0");
    }
    for (const auto &prefix : router.originated_prefixes) {
      if (!isValidIpv4Cidr(prefix)) {
        throw std::runtime_error("Router " + router.id +
                                 " has invalid originated prefix: " + prefix);
      }
    }
  }

  std::unordered_set<std::string> link_keys;
  for (const auto &link : config_.links) {
    if (link.a.empty() || link.b.empty()) {
      throw std::runtime_error("Link endpoints cannot be empty");
    }
    if (link.a == link.b) {
      throw std::runtime_error("Link cannot connect router to itself: " +
                               link.a);
    }
    if (!router_ids.contains(link.a)) {
      throw std::runtime_error("Link references unknown router: " + link.a);
    }
    if (!router_ids.contains(link.b)) {
      throw std::runtime_error("Link references unknown router: " + link.b);
    }
    const auto key = edgeKey(link.a, link.b);
    if (!link_keys.insert(key).second) {
      throw std::runtime_error("Duplicate link: " + link.a + " - " + link.b);
    }
  }

  for (const auto &router : config_.routers) {
    std::unordered_set<std::string> neighbor_ids;
    for (const auto &neighbor : router.neighbors) {
      if (neighbor.id.empty()) {
        throw std::runtime_error("Router " + router.id +
                                 " has an empty neighbor id");
      }
      if (neighbor.id == router.id) {
        throw std::runtime_error("Router " + router.id +
                                 " cannot peer with itself");
      }
      const auto peer_it = routers_by_id.find(neighbor.id);
      if (peer_it == routers_by_id.end()) {
        throw std::runtime_error("Router " + router.id +
                                 " references unknown neighbor: " +
                                 neighbor.id);
      }
      if (!neighbor_ids.insert(neighbor.id).second) {
        throw std::runtime_error("Router " + router.id +
                                 " has duplicate neighbor: " + neighbor.id);
      }
      if (!link_keys.contains(edgeKey(router.id, neighbor.id))) {
        throw std::runtime_error("Router " + router.id + " neighbor " +
                                 neighbor.id + " has no backing link");
      }
      const auto &peer = *peer_it->second;
      if (neighbor.remote_asn != peer.asn) {
        throw std::runtime_error("Router " + router.id + " neighbor " +
                                 neighbor.id + " has remote_asn mismatch");
      }
      const auto expected_session =
          router.asn == peer.asn ? SessionType::Ibgp : SessionType::Ebgp;
      if (neighbor.session_type != expected_session) {
        throw std::runtime_error("Router " + router.id + " neighbor " +
                                 neighbor.id + " has session_type mismatch");
      }
    }
  }
}

void TopoManager::buildRouters() {
  for (const auto &router_config : config_.routers) {
    if (router_config.id.empty()) {
      throw std::runtime_error("Router id cannot be empty");
    }
    auto router = std::make_shared<BgpRouter>(router_config);
    router->attachManager(this);
    routers_[router_config.id] = std::move(router);
  }
}

void TopoManager::normalizeNeighborsFromLinks() {
  auto find_config = [this](const std::string &id) -> const RouterConfig & {
    const auto it = std::find_if(
        config_.routers.begin(), config_.routers.end(),
        [&](const RouterConfig &router) { return router.id == id; });
    if (it == config_.routers.end()) {
      throw std::runtime_error("Link references unknown router: " + id);
    }
    return *it;
  };

  for (const auto &link : config_.links) {
    const auto &a_config = find_config(link.a);
    const auto &b_config = find_config(link.b);
    auto a_router = routers_.at(link.a);
    auto b_router = routers_.at(link.b);

    if (!a_router->neighbor(link.b)) {
      a_router->addOrUpdateNeighbor({
          .id = link.b,
          .remote_asn = b_config.asn,
          .session_type = a_config.asn == b_config.asn ? SessionType::Ibgp
                                                       : SessionType::Ebgp,
          .rr_client = link.rr_client_from_a,
          .enabled = true,
          .hold_time_seconds = 90,
          .mrai_ms = link.mrai_ms_from_a,
      });
    }
    if (!b_router->neighbor(link.a)) {
      b_router->addOrUpdateNeighbor({
          .id = link.a,
          .remote_asn = a_config.asn,
          .session_type = a_config.asn == b_config.asn ? SessionType::Ibgp
                                                       : SessionType::Ebgp,
          .rr_client = link.rr_client_from_b,
          .enabled = true,
          .hold_time_seconds = 90,
          .mrai_ms = link.mrai_ms_from_b,
      });
    }
  }
}

bool TopoManager::messageStillDeliverable(const std::string &from,
                                          const std::string &to) const {
  std::shared_ptr<BgpRouter> source;
  std::shared_ptr<BgpRouter> destination;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
      return false;
    }
    auto src_it = routers_.find(from);
    auto dst_it = routers_.find(to);
    if (src_it == routers_.end() || dst_it == routers_.end()) {
      return false;
    }
    const auto link = linkFor(from, to);
    if (!link || !link->config.enabled) {
      return false;
    }
    source = src_it->second;
    destination = dst_it->second;
  }
  return source->isActive() && destination->isActive();
}

std::optional<TopoManager::LinkRuntime>
TopoManager::linkFor(const std::string &a, const std::string &b) const {
  const auto it = links_.find(edgeKey(a, b));
  if (it == links_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::chrono::milliseconds TopoManager::convergenceQuietPeriod() const {
  std::uint64_t max_mrai_ms = 0;
  for (const auto &router : config_.routers) {
    for (const auto &neighbor : router.neighbors) {
      max_mrai_ms = std::max(max_mrai_ms,
                             static_cast<std::uint64_t>(neighbor.mrai_ms));
    }
  }

  const auto configured_quiet_ms =
      static_cast<std::uint64_t>(config_.simulation.convergence_quiet_ms);
  const auto mrai_quiet_ms = (max_mrai_ms * 3 + 1) / 2;
  return std::chrono::milliseconds(
      std::max(configured_quiet_ms, mrai_quiet_ms));
}

std::filesystem::path TopoManager::makeRunDirectory() const {
  std::filesystem::path base =
      config_.simulation.log_dir.empty()
          ? std::filesystem::path{"tmp"}
          : std::filesystem::path{config_.simulation.log_dir};
  auto run_dir = base / (config_.simulation.name + "_" + runTimestamp());
  run_dir += "_" + std::to_string(++run_counter);
  std::filesystem::create_directories(run_dir);
  return run_dir;
}

void TopoManager::markConvergenceActivity() {
  std::lock_guard lock(mutex_);
  last_convergence_activity_at_ = std::chrono::steady_clock::now();
}

} // namespace toposim
