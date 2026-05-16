#include "toposim/TopoManager.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

toposim::RouterConfig makeRouter(std::string id, std::string router_id,
                                  std::uint32_t asn) {
  return toposim::RouterConfig{
      .id = std::move(id),
      .router_id = std::move(router_id),
      .asn = asn,
  };
}

toposim::TopologyConfig twoRouterTopology(std::uint32_t r1_to_r2_mrai_ms) {
  auto r1 = makeRouter("R1", "1.1.1.1", 65000);
  auto r2 = makeRouter("R2", "2.2.2.2", 65000);
  r1.neighbors.push_back({
      .id = "R2",
      .remote_asn = 65000,
      .session_type = toposim::SessionType::Ibgp,
      .rr_client = false,
      .enabled = true,
      .hold_time_seconds = 90,
      .mrai_ms = r1_to_r2_mrai_ms,
  });

  return toposim::TopologyConfig{
      .simulation =
          {
              .name = "core-tests",
              .log_dir = "tmp/tests",
              .worker_threads = 2,
              .convergence_quiet_ms = 30,
          },
      .routers = {std::move(r1), std::move(r2)},
      .links = {{.a = "R1", .b = "R2", .enabled = true, .delay_ms = 1}},
  };
}

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Fn> void requireThrows(Fn &&fn, const std::string &message) {
  try {
    fn();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}

bool ribHasPrefix(const toposim::RibSnapshot &rib, const std::string &prefix) {
  return std::any_of(rib.loc_rib.begin(), rib.loc_rib.end(),
                     [&](const auto &route) { return route.prefix == prefix; });
}

void rejectsDuplicateRouterIds() {
  auto config = twoRouterTopology(0);
  config.routers.push_back(makeRouter("R1", "3.3.3.3", 65000));

  requireThrows([&] { toposim::TopoManager manager(config); },
                "duplicate router id should be rejected");
}

void rejectsInvalidBgpRouterIds() {
  for (const auto &router_id : {"256.1.1.1", "1.2.3", "R1", "0.0.0.0"}) {
    auto config = twoRouterTopology(0);
    config.routers.front().router_id = router_id;
    requireThrows([&] { toposim::TopoManager manager(config); },
                  std::string("invalid BGP router id should be rejected: ") +
                      router_id);
  }
}

void rejectsInvalidLinks() {
  auto unknown_router = twoRouterTopology(0);
  unknown_router.links = {{.a = "R1", .b = "R3"}};
  requireThrows([&] { toposim::TopoManager manager(unknown_router); },
                "link to unknown router should be rejected");

  auto duplicate_link = twoRouterTopology(0);
  duplicate_link.links.push_back({.a = "R2", .b = "R1"});
  requireThrows([&] { toposim::TopoManager manager(duplicate_link); },
                "duplicate link should be rejected");
}

void startupDoesNotEmitKeepalives() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "no-keepalive-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery query;
  query.actions = {"KEEPALIVE"};
  query.limit = 10;
  require(toposim::BmpLogManager::instance().queryHistory(query).empty(),
          "startup emitted KEEPALIVE messages");
  manager.stop();
}

void mraiAdvertisementDoesNotReviveWithdrawnRoute() {
  auto config = twoRouterTopology(250);
  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  const std::string prefix = "203.0.113.0/24";
  manager.originatePrefix("R1", prefix);
  std::this_thread::sleep_for(50ms);
  manager.withdrawPrefix("R1", prefix);
  std::this_thread::sleep_for(20ms);
  manager.originatePrefix("R1", prefix);
  std::this_thread::sleep_for(20ms);
  manager.withdrawPrefix("R1", prefix);

  require(manager.waitForConvergence(2s), "final convergence timed out");
  toposim::BmpLogManager::instance().flush();
  toposim::BmpLogQuery query;
  query.routers = {"R2"};
  query.limit = 20;
  require(!toposim::BmpLogManager::instance().queryHistory(query).empty(),
          "BMP SQLite history query returned no records for R2");
  toposim::BmpLogQuery withdraw_query;
  withdraw_query.routers = {"R2"};
  withdraw_query.actions = {"WITHDRAW"};
  withdraw_query.limit = 20;
  const auto withdrawals =
      toposim::BmpLogManager::instance().queryHistory(withdraw_query);
  require(!withdrawals.empty(),
          "BMP SQLite history query returned no withdrawals for R2");
  require(std::all_of(withdrawals.begin(), withdrawals.end(), [](const auto &r) {
            return r.action == "WITHDRAW";
          }),
          "BMP withdrawal query returned records not labeled WITHDRAW");
  require(!ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "delayed MRAI advertisement revived a withdrawn route");
  manager.stop();
}

void bmpHistorySupportsMessageFilterFields() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "bmp-query-tests";
  config.routers[1].asn = 65001;
  config.routers[0].neighbors[0].remote_asn = 65001;
  config.routers[0].neighbors[0].session_type = toposim::SessionType::Ebgp;

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  const std::string prefix = "198.51.100.0/24";
  manager.originatePrefix("R1", prefix);
  require(manager.waitForConvergence(2s), "prefix convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery query;
  query.routers = {"R2"};
  query.actions = {"UPDATE"};
  query.from_asns = {65000};
  query.to_asns = {65001};
  query.limit = 10;
  require(!toposim::BmpLogManager::instance().queryHistory(query).empty(),
          "combined BMP SQLite attribute query returned no records");
  manager.stop();
}

void ebgpRouteIsNotWithdrawnBackToOriginPeer() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "no-withdraw-bounce-tests";
  config.routers[0].originated_prefixes.push_back("1.1.1.0/24");
  config.routers[1].asn = 65001;
  config.routers[0].neighbors[0].remote_asn = 65001;
  config.routers[0].neighbors[0].session_type = toposim::SessionType::Ebgp;

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery query;
  query.to_routers = {"R1"};
  query.from_routers = {"R2"};
  query.actions = {"UPDATE"};
  query.limit = 10;
  require(toposim::BmpLogManager::instance().queryHistory(query).empty(),
          "R2 sent a needless withdrawal back to the origin peer R1");
  manager.stop();
}

} // namespace

int main() {
  try {
    rejectsDuplicateRouterIds();
    rejectsInvalidBgpRouterIds();
    rejectsInvalidLinks();
    startupDoesNotEmitKeepalives();
    mraiAdvertisementDoesNotReviveWithdrawnRoute();
    bmpHistorySupportsMessageFilterFields();
    ebgpRouteIsNotWithdrawnBackToOriginPeer();
  } catch (const std::exception &ex) {
    std::cerr << "FAILED: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "All core tests passed.\n";
  return 0;
}
