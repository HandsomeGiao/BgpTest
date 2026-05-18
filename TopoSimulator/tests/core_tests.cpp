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

toposim::TopologyConfig threeRouterRelayTopology() {
  auto r1 = makeRouter("R1", "1.1.1.1", 65001);
  auto r2 = makeRouter("R2", "2.2.2.2", 65002);
  auto r3 = makeRouter("R3", "3.3.3.3", 65003);

  auto addNeighbor = [](toposim::RouterConfig &router, std::string id,
                        std::uint32_t remote_asn) {
    router.neighbors.push_back({
        .id = std::move(id),
        .remote_asn = remote_asn,
        .session_type = toposim::SessionType::Ebgp,
        .rr_client = false,
        .enabled = true,
        .hold_time_seconds = 90,
        .mrai_ms = 0,
    });
  };

  addNeighbor(r1, "R2", 65002);
  addNeighbor(r2, "R1", 65001);
  addNeighbor(r2, "R3", 65003);
  addNeighbor(r3, "R2", 65002);

  return toposim::TopologyConfig{
      .simulation =
          {
              .name = "batch-receive-tests",
              .log_dir = "tmp/tests",
              .worker_threads = 4,
              .convergence_quiet_ms = 30,
          },
      .routers = {std::move(r1), std::move(r2), std::move(r3)},
      .links =
          {
              {.a = "R1", .b = "R2", .enabled = true, .delay_ms = 0},
              {.a = "R2", .b = "R3", .enabled = true, .delay_ms = 0},
          },
  };
}

toposim::TopologyConfig fiveRouterTransientTopology(std::uint32_t mrai_ms) {
  auto r1 = makeRouter("R1", "1.1.1.1", 111);
  r1.originated_prefixes.push_back("1.1.1.0/24");
  auto r2 = makeRouter("R2", "2.2.2.2", 222);
  auto r3 = makeRouter("R3", "3.3.3.3", 333);
  auto r4 = makeRouter("R4", "4.4.4.4", 444);
  auto r5 = makeRouter("R5", "5.5.5.5", 555);

  auto addNeighbor = [mrai_ms](toposim::RouterConfig &router, std::string id,
                               std::uint32_t remote_asn) {
    router.neighbors.push_back({
        .id = std::move(id),
        .remote_asn = remote_asn,
        .session_type = toposim::SessionType::Ebgp,
        .rr_client = false,
        .enabled = true,
        .hold_time_seconds = 90,
        .mrai_ms = mrai_ms,
    });
  };

  addNeighbor(r1, "R2", 222);
  addNeighbor(r2, "R1", 111);
  addNeighbor(r2, "R3", 333);
  addNeighbor(r2, "R4", 444);
  addNeighbor(r3, "R2", 222);
  addNeighbor(r3, "R5", 555);
  addNeighbor(r4, "R2", 222);
  addNeighbor(r4, "R5", 555);
  addNeighbor(r5, "R3", 333);
  addNeighbor(r5, "R4", 444);

  return toposim::TopologyConfig{
      .simulation =
          {
              .name = "transient-withdraw-tests",
              .log_dir = "tmp/tests",
              .worker_threads = 4,
              .convergence_quiet_ms = 30,
          },
      .routers = {std::move(r2), std::move(r3), std::move(r4), std::move(r5),
                  std::move(r1)},
      .links =
          {
              {.a = "R1", .b = "R2", .enabled = true, .delay_ms = 10},
              {.a = "R2", .b = "R3", .enabled = true, .delay_ms = 5},
              {.a = "R3", .b = "R5", .enabled = true, .delay_ms = 4},
              {.a = "R2", .b = "R4", .enabled = true, .delay_ms = 0},
              {.a = "R4", .b = "R5", .enabled = true, .delay_ms = 0},
          },
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

std::size_t locRibPrefixCount(const toposim::RibSnapshot &rib) {
  return rib.loc_rib.size();
}

void sendingOpenDoesNotDowngradeEstablishedPeer() {
  auto config = makeRouter("R1", "1.1.1.1", 65000);
  config.neighbors.push_back({
      .id = "R2",
      .remote_asn = 65000,
      .session_type = toposim::SessionType::Ibgp,
      .rr_client = false,
      .enabled = true,
      .hold_time_seconds = 90,
      .mrai_ms = 0,
  });

  toposim::BgpRouter router(std::move(config));
  router.start(false);

  toposim::BgpMessage open;
  open.type = toposim::BgpMessageType::Open;
  open.from = "R2";
  open.to = "R1";
  open.open = toposim::BgpOpenPayload{
      .version = 4,
      .asn = 65000,
      .hold_time_seconds = 90,
      .router_id = "2.2.2.2",
  };
  router.receiveMessage(open);
  router.neighborUp("R2");

  const auto peers = router.peerSnapshot();
  require(peers.size() == 1 &&
              peers.front().state == toposim::PeerState::Established,
          "sending our OPEN after receiving peer OPEN downgraded the session");
  router.stop();
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

void rejectsInvalidNeighborConfig() {
  auto remote_asn_mismatch = twoRouterTopology(0);
  remote_asn_mismatch.routers[0].neighbors[0].remote_asn = 65001;
  requireThrows([&] { toposim::TopoManager manager(remote_asn_mismatch); },
                "remote_asn mismatch should be rejected");

  auto session_type_mismatch = twoRouterTopology(0);
  session_type_mismatch.routers[0].neighbors[0].session_type =
      toposim::SessionType::Ebgp;
  requireThrows([&] { toposim::TopoManager manager(session_type_mismatch); },
                "session_type mismatch should be rejected");

  auto missing_link = twoRouterTopology(0);
  missing_link.links.clear();
  requireThrows([&] { toposim::TopoManager manager(missing_link); },
                "explicit neighbor without a backing link should be rejected");
}

void rejectsDuplicateBgpRouterIdsAndInvalidPrefixes() {
  auto duplicate_bgp_id = twoRouterTopology(0);
  duplicate_bgp_id.routers[1].router_id = duplicate_bgp_id.routers[0].router_id;
  requireThrows([&] { toposim::TopoManager manager(duplicate_bgp_id); },
                "duplicate BGP router id should be rejected");

  auto invalid_prefix = twoRouterTopology(0);
  invalid_prefix.routers[0].originated_prefixes.push_back("not-a-prefix");
  requireThrows([&] { toposim::TopoManager manager(invalid_prefix); },
                "invalid originated prefix should be rejected");
}

void updateBeforeOpenIsRejected() {
  auto r1 = makeRouter("R1", "1.1.1.1", 65000);
  r1.neighbors.push_back({
      .id = "R2",
      .remote_asn = 65001,
      .session_type = toposim::SessionType::Ebgp,
      .rr_client = false,
      .enabled = true,
  });

  toposim::BgpRouter router(std::move(r1));
  router.start();

  toposim::BgpMessage update_message;
  update_message.type = toposim::BgpMessageType::Update;
  update_message.from = "R2";
  update_message.to = "R1";
  update_message.update = toposim::BgpUpdatePayload{
      .nlri = {"203.0.113.0/24"},
      .path_attributes =
          {
              .as_path = {65001},
              .next_hop = "2.2.2.2",
          },
  };

  router.receiveMessage(update_message);

  const auto peers = router.peerSnapshot();
  const auto peer = std::find_if(peers.begin(), peers.end(), [](const auto &p) {
    return p.id == "R2";
  });
  require(peer != peers.end(), "test peer should exist");
  require(peer->state != toposim::PeerState::Established,
          "UPDATE before OPEN established a session");
  require(!ribHasPrefix(router.ribSnapshot(), "203.0.113.0/24"),
          "UPDATE before OPEN polluted the Loc-RIB");
  router.stop();
}

void stoppedTopoManagerCannotRestart() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "restart-rejection-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");
  manager.stop();
  requireThrows([&] { manager.start(); },
                "TopoManager restart after stop should be rejected");
}

void bmpFlushDrainsInflightBatch() {
  auto &logger = toposim::BmpLogManager::instance();
  logger.initialize("tmp/tests/bmp-flush/bmp.log",
                    "tmp/tests/bmp-flush/bmp.sqlite", 2048);

  for (std::uint64_t i = 0; i < 1000; ++i) {
    logger.recordTopologyEvent("flush_test", {{"index", i}});
  }
  logger.flush();

  toposim::BmpLogQuery query;
  query.actions = {"flush_test"};
  query.limit = 1200;
  const auto records = logger.queryHistory(query);
  require(records.size() == 1000,
          "BMP flush returned before all records reached SQLite");
  logger.shutdown();
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

void startupActivatesRoutersBeforeSendingOpens() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "startup-open-order-tests";
  config.simulation.worker_threads = 8;
  config.links[0].delay_ms = 0;
  config.routers[0].originated_prefixes.push_back("203.0.113.0/24");

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "startup convergence timed out");

  const auto r1_peers = manager.peersSnapshot("R1");
  const auto r2_peers = manager.peersSnapshot("R2");
  require(r1_peers.size() == 1 &&
              r1_peers.front().state == toposim::PeerState::Established,
          "R1 did not establish its zero-delay startup session");
  require(r2_peers.size() == 1 &&
              r2_peers.front().state == toposim::PeerState::Established,
          "R2 did not establish its zero-delay startup session");
  require(ribHasPrefix(manager.ribSnapshot("R2"), "203.0.113.0/24"),
          "zero-delay startup dropped the initial route advertisement");
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
  require(std::all_of(withdrawals.begin(), withdrawals.end(), [](const auto &r) {
            return r.action == "WITHDRAW";
          }),
          "BMP withdrawal query returned records not labeled WITHDRAW");
  require(!ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "delayed MRAI advertisement revived a withdrawn route");
  manager.stop();
}

void firstMraiUpdateIsNotImmediateAfterStartup() {
  auto config = twoRouterTopology(1000);
  config.simulation.name = "initial-mrai-jitter-tests";
  const std::string prefix = "203.0.113.0/24";
  config.routers[0].originated_prefixes.push_back(prefix);

  toposim::TopoManager manager(std::move(config));
  manager.start();
  std::this_thread::sleep_for(200ms);
  require(!ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "first MRAI-protected UPDATE was sent immediately after startup");

  require(manager.waitForConvergence(4s),
          "initial randomized MRAI convergence timed out");
  require(ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "initial randomized MRAI UPDATE never reached R2");
  manager.stop();
}

void withdrawalsBypassMraiForSamePeerAndPrefix() {
  auto config = twoRouterTopology(1000);
  config.simulation.name = "mrai-withdrawal-bypass-tests";
  const std::string prefix = "203.0.113.0/24";
  config.routers[0].originated_prefixes.push_back(prefix);

  toposim::TopoManager manager(std::move(config));
  manager.start();

  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (!ribHasPrefix(manager.ribSnapshot("R2"), prefix) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  require(ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "initial advertisement did not reach R2");

  manager.withdrawPrefix("R1", prefix);
  const auto withdraw_deadline = std::chrono::steady_clock::now() + 250ms;
  while (ribHasPrefix(manager.ribSnapshot("R2"), prefix) &&
         std::chrono::steady_clock::now() < withdraw_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  require(!ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "withdrawal UPDATE waited for the per-peer MRAI timer");

  require(manager.waitForConvergence(4s),
          "immediate withdrawal convergence timed out");
  require(!ribHasPrefix(manager.ribSnapshot("R2"), prefix),
          "withdrawal did not remove the route from R2");
  manager.stop();
}

void mraiIsSharedByAllPrefixesForPeer() {
  auto config = twoRouterTopology(1000);
  config.simulation.name = "peer-mrai-spacing-tests";
  config.routers[0].originated_prefixes.push_back("198.51.100.0/24");
  config.routers[0].originated_prefixes.push_back("203.0.113.0/24");

  toposim::TopoManager manager(std::move(config));
  manager.start();
  std::this_thread::sleep_for(200ms);
  require(locRibPrefixCount(manager.ribSnapshot("R2")) == 0,
          "MRAI coalesced prefixes bypassed the initial jitter");

  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (locRibPrefixCount(manager.ribSnapshot("R2")) == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  const auto flush_deadline = std::chrono::steady_clock::now() + 250ms;
  while (locRibPrefixCount(manager.ribSnapshot("R2")) < 2 &&
         std::chrono::steady_clock::now() < flush_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  require(locRibPrefixCount(manager.ribSnapshot("R2")) == 2,
          "peer-level MRAI did not flush pending prefixes together");

  require(manager.waitForConvergence(4s),
          "peer-level MRAI convergence timed out");
  require(locRibPrefixCount(manager.ribSnapshot("R2")) == 2,
          "peer-level MRAI second prefix never reached R2");
  manager.stop();
}

void staleMraiFlushDoesNotDelayNextValidUpdate() {
  auto config = twoRouterTopology(1200);
  config.simulation.name = "stale-mrai-flush-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  const std::string stale_prefix = "198.51.100.0/24";
  const std::string valid_prefix = "203.0.113.0/24";
  manager.originatePrefix("R1", stale_prefix);
  std::this_thread::sleep_for(50ms);
  manager.withdrawPrefix("R1", stale_prefix);
  std::this_thread::sleep_for(1300ms);
  require(!ribHasPrefix(manager.ribSnapshot("R2"), stale_prefix),
          "stale MRAI update was delivered");

  manager.originatePrefix("R1", valid_prefix);
  const auto deadline = std::chrono::steady_clock::now() + 250ms;
  while (!ribHasPrefix(manager.ribSnapshot("R2"), valid_prefix) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  require(ribHasPrefix(manager.ribSnapshot("R2"), valid_prefix),
          "stale MRAI flush consumed the next valid update slot");
  manager.stop();
}

void mraiFlushBatchesWithdrawalsForPeer() {
  auto config = twoRouterTopology(500);
  config.simulation.name = "withdrawal-batching-tests";
  const std::string first_prefix = "198.51.100.0/24";
  const std::string second_prefix = "203.0.113.0/24";
  config.routers[0].originated_prefixes.push_back(first_prefix);
  config.routers[0].originated_prefixes.push_back(second_prefix);

  toposim::TopoManager manager(std::move(config));
  manager.start();
  const auto initial_deadline = std::chrono::steady_clock::now() + 1500ms;
  while (locRibPrefixCount(manager.ribSnapshot("R2")) < 2 &&
         std::chrono::steady_clock::now() < initial_deadline) {
    std::this_thread::sleep_for(10ms);
  }
  require(locRibPrefixCount(manager.ribSnapshot("R2")) == 2,
          "initial batched advertisements did not reach R2");

  manager.withdrawPrefix("R1", first_prefix);
  manager.withdrawPrefix("R1", second_prefix);

  require(manager.waitForConvergence(3s),
          "batched withdrawal convergence timed out");
  require(locRibPrefixCount(manager.ribSnapshot("R2")) == 0,
          "batched withdrawals did not remove both routes");

  toposim::BmpLogManager::instance().flush();
  toposim::BmpLogQuery query;
  query.from_routers = {"R1"};
  query.to_routers = {"R2"};
  query.actions = {"WITHDRAW"};
  query.limit = 10;
  const auto withdrawals =
      toposim::BmpLogManager::instance().queryHistory(query);
  require(withdrawals.size() == 2,
          "batched transport should still expose separate BMP withdrawals");
  const auto has_first = std::any_of(
      withdrawals.begin(), withdrawals.end(), [&](const auto &record) {
        return record.withdrawn.find(first_prefix) != std::string::npos;
      });
  const auto has_second = std::any_of(
      withdrawals.begin(), withdrawals.end(), [&](const auto &record) {
        return record.withdrawn.find(second_prefix) != std::string::npos;
      });
  require(has_first && has_second,
          "batched withdrawal BMP records did not include both prefixes");
  manager.stop();
}

void batchedReceiveRunsOneDecisionAfterAllMessages() {
  auto config = threeRouterRelayTopology();
  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  const std::string prefix = "203.0.113.0/24";
  auto makeUpdate = [&](std::vector<std::uint32_t> as_path) {
    toposim::BgpMessage message;
    message.type = toposim::BgpMessageType::Update;
    message.update = toposim::BgpUpdatePayload{
        .nlri = {prefix},
        .path_attributes =
            {
                .as_path = std::move(as_path),
                .next_hop = "1.1.1.1",
            },
    };
    return message;
  };

  std::vector<toposim::BgpMessage> updates;
  updates.push_back(makeUpdate({65001, 64512}));
  updates.push_back(makeUpdate({65001}));
  manager.sendMessages("R1", "R2", std::move(updates));

  require(manager.waitForConvergence(2s),
          "batched receive convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery query;
  query.from_routers = {"R2"};
  query.to_routers = {"R3"};
  query.actions = {"UPDATE"};
  query.limit = 20;
  const auto records =
      toposim::BmpLogManager::instance().queryHistory(query);

  bool saw_final_route = false;
  bool saw_intermediate_route = false;
  std::size_t update_count = 0;
  for (const auto &record : records) {
    if (record.prefixes != prefix) {
      continue;
    }
    ++update_count;
    saw_final_route = saw_final_route || record.as_path == "65002 65001";
    saw_intermediate_route =
        saw_intermediate_route || record.as_path == "65002 65001 64512";
  }

  require(saw_final_route,
          "receiver did not advertise the final route after the batch");
  require(!saw_intermediate_route,
          "receiver advertised an intermediate route before finishing batch");
  require(update_count == 1,
          "receiver emitted more than one UPDATE for one delivered batch");
  manager.stop();
}

void linkDirectionalMraiIsUsedForGeneratedNeighbors() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "link-mrai-normalization-tests";
  config.routers[0].neighbors.clear();
  config.links[0].mrai_ms_from_a = 1234;
  config.links[0].mrai_ms_from_b = 5678;

  toposim::TopoManager manager(std::move(config));
  const auto r1_peers = manager.peersSnapshot("R1");
  const auto r2_peers = manager.peersSnapshot("R2");

  require(r1_peers.size() == 1 && r1_peers.front().mrai_ms == 1234,
          "link A->B MRAI was not copied to the generated neighbor");
  require(r2_peers.size() == 1 && r2_peers.front().mrai_ms == 5678,
          "link B->A MRAI was not copied to the generated neighbor");
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

void redundantLinkStateChangeIsNoop() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "redundant-link-state-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  require(!manager.setLinkState("R1", "R2", true),
          "link up on an already enabled link should be a no-op");
  require(manager.setLinkState("R1", "R2", false),
          "link down on an enabled link should change state");
  require(!manager.setLinkState("R1", "R2", false),
          "link down on an already disabled link should be a no-op");
  manager.stop();
}

void redundantNodeStateChangeIsNoop() {
  auto config = twoRouterTopology(0);
  config.simulation.name = "redundant-node-state-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(2s), "initial convergence timed out");

  require(!manager.setRouterState("R1", true),
          "node up on an already active router should be a no-op");
  require(manager.setRouterState("R1", false),
          "node down on an active router should change state");
  require(!manager.setRouterState("R1", false),
          "node down on an already stopped router should be a no-op");
  manager.stop();
}

void canceledTransientAdvertisementDoesNotEmitWithdraw() {
  auto config = fiveRouterTransientTopology(250);
  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(6s),
          "five-router topology convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery query;
  query.from_routers = {"R3"};
  query.to_routers = {"R2"};
  query.limit = 100;
  auto records = toposim::BmpLogManager::instance().queryHistory(query);
  std::sort(records.begin(), records.end(),
            [](const auto &lhs, const auto &rhs) { return lhs.id < rhs.id; });

  bool delivered_advertisement = false;
  for (const auto &record : records) {
    if (record.action == "UPDATE" && record.prefixes == "1.1.1.0/24") {
      delivered_advertisement = true;
    }
    if (record.action == "WITHDRAW" && record.withdrawn == "1.1.1.0/24") {
      require(delivered_advertisement,
              "R3 withdrew a route from R2 before delivering it to R2");
    }
  }
  manager.stop();
}

void restartedRouterAdvertisesBestRouteToReestablishedPeer() {
  auto config = fiveRouterTransientTopology(1000);
  config.simulation.name = "restart-advertise-best-route-tests";

  toposim::TopoManager manager(std::move(config));
  manager.start();
  require(manager.waitForConvergence(6s),
          "five-router topology convergence timed out");

  require(manager.setRouterState("R4", false),
          "R4 down should change router state");
  require(manager.waitForConvergence(6s), "R4 down convergence timed out");
  require(manager.setRouterState("R4", true),
          "R4 up should change router state");
  require(manager.waitForConvergence(6s), "R4 restart convergence timed out");
  toposim::BmpLogManager::instance().flush();

  toposim::BmpLogQuery all_records_query;
  all_records_query.limit = 10000;
  const auto records =
      toposim::BmpLogManager::instance().queryHistory(all_records_query);

  std::uint64_t last_router_up_id = 0;
  for (const auto &record : records) {
    if (record.event == "router_up") {
      last_router_up_id = std::max(last_router_up_id, record.id);
    }
  }

  const auto advertised_to_r5 =
      std::any_of(records.begin(), records.end(), [&](const auto &record) {
        return record.id > last_router_up_id && record.from == "R4" &&
               record.to == "R5" && record.action == "UPDATE" &&
               record.prefixes == "1.1.1.0/24" &&
               record.as_path == "444 222 111";
      });
  require(advertised_to_r5,
          "R4 restart did not advertise the R2-learned best route to R5");

  const auto repeated_to_r3 =
      std::any_of(records.begin(), records.end(), [&](const auto &record) {
        return record.id > last_router_up_id && record.from == "R2" &&
               record.to == "R3" && record.action == "UPDATE" &&
               record.prefixes == "1.1.1.0/24";
      });
  require(!repeated_to_r3,
          "R4 restart caused R2 to repeat an unchanged UPDATE to R3");
  manager.stop();
}

} // namespace

int main() {
  try {
    sendingOpenDoesNotDowngradeEstablishedPeer();
    rejectsDuplicateRouterIds();
    rejectsInvalidBgpRouterIds();
    rejectsInvalidLinks();
    rejectsInvalidNeighborConfig();
    rejectsDuplicateBgpRouterIdsAndInvalidPrefixes();
    updateBeforeOpenIsRejected();
    stoppedTopoManagerCannotRestart();
    bmpFlushDrainsInflightBatch();
    startupDoesNotEmitKeepalives();
    startupActivatesRoutersBeforeSendingOpens();
    mraiAdvertisementDoesNotReviveWithdrawnRoute();
    firstMraiUpdateIsNotImmediateAfterStartup();
    withdrawalsBypassMraiForSamePeerAndPrefix();
    mraiIsSharedByAllPrefixesForPeer();
    staleMraiFlushDoesNotDelayNextValidUpdate();
    mraiFlushBatchesWithdrawalsForPeer();
    batchedReceiveRunsOneDecisionAfterAllMessages();
    linkDirectionalMraiIsUsedForGeneratedNeighbors();
    bmpHistorySupportsMessageFilterFields();
    ebgpRouteIsNotWithdrawnBackToOriginPeer();
    redundantLinkStateChangeIsNoop();
    redundantNodeStateChangeIsNoop();
    canceledTransientAdvertisementDoesNotEmitWithdraw();
    restartedRouterAdvertisesBestRouteToReestablishedPeer();
  } catch (const std::exception &ex) {
    std::cerr << "FAILED: " << ex.what() << '\n';
    return 1;
  }

  std::cout << "All core tests passed.\n";
  return 0;
}
