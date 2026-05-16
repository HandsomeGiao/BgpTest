#include "toposim/TopoManager.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

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

} // namespace

TopoManager::TopoManager(TopologyConfig config) : config_(std::move(config)) {
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
                              std::chrono::milliseconds extra_delay) {
  std::shared_ptr<BgpRouter> destination;
  std::uint32_t delay_ms = 0;
  {
    std::lock_guard lock(mutex_);
    if (!running_) {
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
    destination = dst_it->second;
    delay_ms = link->config.delay_ms;
    message.sequence = ++sequence_;
  }

  auto *collector = bmp_.get();
  pool_->enqueue([destination = std::move(destination), collector, delay_ms, extra_delay,
                  message = std::move(message)]() mutable {
    if (extra_delay.count() > 0) {
      std::this_thread::sleep_for(extra_delay);
    }
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    collector->recordReceive(message.to, message);
    destination->receiveMessage(message);
  });
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
  std::vector<std::string> peers;
  std::shared_ptr<BgpRouter> router;
  {
    std::lock_guard lock(mutex_);
    auto it = routers_.find(router_id);
    if (it == routers_.end()) {
      throw std::runtime_error("Unknown router: " + router_id);
    }
    router = it->second;
    for (const auto &[_, link] : links_) {
      if (link.config.a == router_id) {
        peers.push_back(link.config.b);
      } else if (link.config.b == router_id) {
        peers.push_back(link.config.a);
      }
    }
  }

  bmp_->recordTopologyEvent(enabled ? "router_up" : "router_down",
                            {{"router", router_id}});
  if (enabled) {
    router->start();
    for (const auto &peer : peers) {
      if (auto peer_router = routers_.find(peer);
          peer_router != routers_.end()) {
        peer_router->second->neighborUp(router_id);
      }
    }
  } else {
    router->stop();
    for (const auto &peer : peers) {
      if (auto peer_router = routers_.find(peer);
          peer_router != routers_.end()) {
        peer_router->second->neighborDown(router_id);
      }
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
  const auto quiet_period = convergenceQuietPeriod();
  return pool_->waitForIdle(std::max(timeout, quiet_period), quiet_period);
}

bool TopoManager::isConverged() const {
  return pool_ && pool_->isIdleFor(convergenceQuietPeriod());
}

std::vector<RouterSnapshot> TopoManager::routersSnapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<RouterSnapshot> result;
  result.reserve(routers_.size());
  for (const auto &[id, router] : routers_) {
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
