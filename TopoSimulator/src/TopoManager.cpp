#include "toposim/TopoManager.hpp"

#include <algorithm>
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
  std::tm tm{};
  localtime_s(&tm, &time);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
  return oss.str();
}

bool isValidBgpRouterId(const std::string &value) {
  if (value == "0.0.0.0") {
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
  bmp_ = std::make_unique<BmpCollector>(run_dir_ / "bmp_collector.log");
}

TopoManager::~TopoManager() { stop(); }

void TopoManager::start() {
  std::vector<std::shared_ptr<BgpRouter>> routers;
  {
    std::lock_guard lock(mutex_);
    running_ = true;
    for (const auto &[_, router] : routers_) {
      routers.push_back(router);
    }
  }

  bmp_->recordTopologyEvent(
      "simulation_started",
      {
          {"name", config_.simulation.name},
          {"router_count",
           BmpEventValue{static_cast<std::uint64_t>(routers.size())}},
          {"link_count",
           BmpEventValue{static_cast<std::uint64_t>(links_.size())}},
      });

  for (const auto &router : routers) {
    router->start();
  }
}

void TopoManager::stop() {
  std::vector<std::shared_ptr<BgpRouter>> routers;
  {
    std::lock_guard lock(mutex_);
    if (!running_ && !pool_) {
      return;
    }
    running_ = false;
    for (const auto &[_, router] : routers_) {
      routers.push_back(router);
    }
  }
  for (const auto &router : routers) {
    router->stop();
  }
  if (pool_) {
    pool_->waitForIdle(std::chrono::seconds(5), std::chrono::milliseconds(10));
    pool_->stop();
    pool_.reset();
  }
  if (bmp_) {
    bmp_->recordTopologyEvent("simulation_stopped", BmpEventDetail{});
  }
}

void TopoManager::sendMessage(const std::string &from, const std::string &to,
                              BgpMessage message,
                              std::chrono::milliseconds extra_delay,
                              std::function<bool()> delivery_guard) {
  {
    std::lock_guard lock(mutex_);
    if (!running_ || !pool_ || !bmp_) {
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
    const auto delay_ms = link->config.delay_ms;
    auto *collector = bmp_.get();
    message.sequence = ++sequence_;

    pool_->enqueue([this, destination = std::move(destination), collector,
                    delay_ms, extra_delay,
                    delivery_guard = std::move(delivery_guard),
                    message = std::move(message)]() mutable {
      if (extra_delay.count() > 0) {
        std::this_thread::sleep_for(extra_delay);
      }
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      if (delivery_guard && !delivery_guard()) {
        return;
      }
      if (!messageStillDeliverable(message.from, message.to)) {
        return;
      }
      collector->recordReceive(message.to, message);
      destination->receiveMessage(message);
    });
  }
}

void TopoManager::setLinkState(const std::string &a, const std::string &b,
                               bool enabled) {
  std::shared_ptr<BgpRouter> router_a;
  std::shared_ptr<BgpRouter> router_b;
  {
    std::lock_guard lock(mutex_);
    auto it = links_.find(edgeKey(a, b));
    if (it == links_.end()) {
      throw std::runtime_error("Unknown link: " + a + " - " + b);
    }
    it->second.config.enabled = enabled;
    router_a = routers_.at(a);
    router_b = routers_.at(b);
  }

  bmp_->recordTopologyEvent(enabled ? "link_up" : "link_down",
                            {{"a", a}, {"b", b}});
  if (enabled) {
    router_a->neighborUp(b);
    router_b->neighborUp(a);
  } else {
    router_a->neighborDown(b);
    router_b->neighborDown(a);
  }
}

void TopoManager::setRouterState(const std::string &router_id, bool enabled) {
  std::vector<std::shared_ptr<BgpRouter>> peer_routers;
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
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

  bmp_->recordTopologyEvent(enabled ? "router_up" : "router_down",
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
}

void TopoManager::originatePrefix(const std::string &router_id,
                                  const std::string &prefix) {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    router = routers_.at(router_id);
  }
  bmp_->recordTopologyEvent("manual_advertise",
                            {{"router", router_id}, {"prefix", prefix}});
  router->originatePrefix(prefix);
}

void TopoManager::withdrawPrefix(const std::string &router_id,
                                 const std::string &prefix) {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    router = routers_.at(router_id);
  }
  bmp_->recordTopologyEvent("manual_withdraw",
                            {{"router", router_id}, {"prefix", prefix}});
  router->withdrawLocalPrefix(prefix);
}

bool TopoManager::waitForConvergence(std::chrono::milliseconds timeout) {
  if (!pool_) {
    return !running_;
  }
  const auto quiet_period = convergenceQuietPeriod();
  return pool_->waitForIdle(std::max(timeout, quiet_period), quiet_period);
}

bool TopoManager::isConverged() const {
  return pool_ && pool_->isIdleFor(convergenceQuietPeriod());
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
    router = routers_.at(router_id);
  }
  return router->ribSnapshot();
}

std::vector<PeerSnapshot>
TopoManager::peersSnapshot(const std::string &router_id) const {
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    router = routers_.at(router_id);
  }
  return router->peerSnapshot();
}

const std::filesystem::path &TopoManager::logFile() const {
  return bmp_->logFile();
}

std::string TopoManager::edgeKey(const std::string &a, const std::string &b) {
  return a < b ? a + "|" + b : b + "|" + a;
}

void TopoManager::validateConfig() const {
  std::unordered_set<std::string> router_ids;
  for (const auto &router : config_.routers) {
    if (router.id.empty()) {
      throw std::runtime_error("Router id cannot be empty");
    }
    if (!router_ids.insert(router.id).second) {
      throw std::runtime_error("Duplicate router id: " + router.id);
    }
    if (!isValidBgpRouterId(router.router_id)) {
      throw std::runtime_error(
          "Router " + router.id +
          " has an invalid BGP router id. Expected dotted decimal x.x.x.x "
          "with each octet in 0..255, excluding 0.0.0.0");
    }
    if (router.asn == 0) {
      throw std::runtime_error("Router " + router.id + " has invalid ASN 0");
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
      if (!router_ids.contains(neighbor.id)) {
        throw std::runtime_error("Router " + router.id +
                                 " references unknown neighbor: " +
                                 neighbor.id);
      }
      if (!neighbor_ids.insert(neighbor.id).second) {
        throw std::runtime_error("Router " + router.id +
                                 " has duplicate neighbor: " + neighbor.id);
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
          .rr_client = false,
          .enabled = true,
      });
    }
    if (!b_router->neighbor(link.a)) {
      b_router->addOrUpdateNeighbor({
          .id = link.a,
          .remote_asn = a_config.asn,
          .session_type = a_config.asn == b_config.asn ? SessionType::Ibgp
                                                       : SessionType::Ebgp,
          .rr_client = false,
          .enabled = true,
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
  std::filesystem::create_directories(run_dir);
  return run_dir;
}

} // namespace toposim
