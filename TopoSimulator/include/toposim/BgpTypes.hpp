#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace toposim {

enum class BgpMessageType { Open, Update, Notification };

enum class SessionType { Ibgp, Ebgp };

enum class PeerState { Idle, OpenSent, Established };

struct PathAttributes {
  std::string origin = "igp";
  std::vector<std::uint32_t> as_path;
  std::string next_hop;
  std::uint32_t local_pref = 100;
  std::uint32_t med = 0;
  std::optional<std::string> originator_id;
  std::optional<std::string> cluster_list;
  std::map<std::string, std::string> communities;

  bool operator==(const PathAttributes &) const = default;
};

struct BgpOpenPayload {
  std::uint32_t version = 4;
  std::uint32_t asn = 0;
  std::uint32_t hold_time_seconds = 90;
  std::string router_id;

  bool operator==(const BgpOpenPayload &) const = default;
};

struct BgpUpdatePayload {
  std::vector<std::string> withdrawn_routes;
  std::vector<std::string> nlri;
  PathAttributes path_attributes;

  bool operator==(const BgpUpdatePayload &) const = default;
};

struct BgpNotificationPayload {
  std::uint32_t error_code = 0;
  std::uint32_t error_subcode = 0;
  std::string data;

  bool operator==(const BgpNotificationPayload &) const = default;
};

struct BgpMessage {
  BgpMessageType type = BgpMessageType::Open;
  std::string from;
  std::string to;
  std::uint64_t sequence = 0;
  std::optional<BgpOpenPayload> open;
  std::optional<BgpUpdatePayload> update;
  std::optional<BgpNotificationPayload> notification;

  bool operator==(const BgpMessage &) const = default;
};

struct NeighborConfig {
  std::string id;
  std::uint32_t remote_asn = 0;
  SessionType session_type = SessionType::Ibgp;
  bool rr_client = false;
  bool enabled = true;
  std::uint32_t hold_time_seconds = 90;
  std::uint32_t mrai_ms = 0;

  bool operator==(const NeighborConfig &) const = default;
};

struct RouterConfig {
  std::string id;
  std::string router_id;
  std::uint32_t asn = 0;
  std::string cluster_id;
  std::vector<std::string> originated_prefixes;
  std::vector<NeighborConfig> neighbors;

  bool operator==(const RouterConfig &) const = default;
};

struct LinkConfig {
  std::string a;
  std::string b;
  bool enabled = true;
  std::uint32_t delay_ms = 1;
  bool rr_client_from_a = false;
  bool rr_client_from_b = false;
  std::uint32_t mrai_ms_from_a = 0;
  std::uint32_t mrai_ms_from_b = 0;

  bool operator==(const LinkConfig &) const = default;
};

struct SimulationConfig {
  std::string name = "bgp-simulation";
  std::string log_dir = "tmp";
  std::uint32_t worker_threads = 0;
  std::uint32_t convergence_quiet_ms = 300;

  bool operator==(const SimulationConfig &) const = default;
};

struct TopologyConfig {
  SimulationConfig simulation;
  std::vector<RouterConfig> routers;
  std::vector<LinkConfig> links;

  bool operator==(const TopologyConfig &) const = default;
};

struct RouteEntry {
  std::string prefix;
  PathAttributes attributes;
  std::string learned_from;
  SessionType source_session = SessionType::Ibgp;
  bool local_origin = false;

  bool operator==(const RouteEntry &) const = default;
};

struct RouterSnapshot {
  std::string id;
  std::string router_id;
  std::uint32_t asn = 0;
  bool active = false;
  bool has_rr_clients = false;
};

struct PeerSnapshot {
  std::string id;
  std::uint32_t remote_asn = 0;
  SessionType session_type = SessionType::Ibgp;
  bool rr_client = false;
  bool enabled = true;
  std::uint32_t mrai_ms = 0;
  PeerState state = PeerState::Idle;
};

struct RibSnapshot {
  std::string router;
  std::vector<RouteEntry> loc_rib;
  std::map<std::string, std::map<std::string, RouteEntry>> adj_rib_in;
  std::map<std::string, std::map<std::string, RouteEntry>> adj_rib_out;
};

std::string toString(BgpMessageType type);
std::string toString(SessionType type);
std::string toString(PeerState state);
BgpMessageType bgpMessageTypeFromString(const std::string &value);
SessionType sessionTypeFromString(const std::string &value);

} // namespace toposim
