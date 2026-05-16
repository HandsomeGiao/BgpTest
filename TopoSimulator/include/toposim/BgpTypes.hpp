#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace toposim {

enum class BgpMessageType { Open, Update, Notification, Keepalive };

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
};

struct BgpOpenPayload {
  std::uint32_t version = 4;
  std::uint32_t asn = 0;
  std::uint32_t hold_time_seconds = 90;
  std::string router_id;
};

struct BgpUpdatePayload {
  std::vector<std::string> withdrawn_routes;
  std::vector<std::string> nlri;
  PathAttributes path_attributes;
};

struct BgpNotificationPayload {
  std::uint32_t error_code = 0;
  std::uint32_t error_subcode = 0;
  std::string data;
};

struct BgpMessage {
  BgpMessageType type = BgpMessageType::Keepalive;
  std::string from;
  std::string to;
  std::uint64_t sequence = 0;
  std::optional<BgpOpenPayload> open;
  std::optional<BgpUpdatePayload> update;
  std::optional<BgpNotificationPayload> notification;
};

struct NeighborConfig {
  std::string id;
  std::uint32_t remote_asn = 0;
  SessionType session_type = SessionType::Ibgp;
  bool rr_client = false;
  bool enabled = true;
  std::uint32_t hold_time_seconds = 90;
  std::uint32_t mrai_ms = 0;
};

struct RouterConfig {
  std::string id;
  std::string router_id;
  std::uint32_t asn = 0;
  std::string cluster_id;
  std::vector<std::string> originated_prefixes;
  std::vector<NeighborConfig> neighbors;
};

struct LinkConfig {
  std::string a;
  std::string b;
  bool enabled = true;
  std::uint32_t delay_ms = 1;
};

struct SimulationConfig {
  std::string name = "bgp-simulation";
  std::string log_dir = "tmp";
  std::uint32_t worker_threads = 0;
  std::uint32_t convergence_quiet_ms = 300;
};

struct TopologyConfig {
  SimulationConfig simulation;
  std::vector<RouterConfig> routers;
  std::vector<LinkConfig> links;
};

struct RouteEntry {
  std::string prefix;
  PathAttributes attributes;
  std::string learned_from;
  SessionType source_session = SessionType::Ibgp;
  bool local_origin = false;
};

std::string toString(BgpMessageType type);
std::string toString(SessionType type);
std::string toString(PeerState state);
BgpMessageType bgpMessageTypeFromString(const std::string &value);
SessionType sessionTypeFromString(const std::string &value);

void to_json(nlohmann::json &j, const PathAttributes &attrs);
void from_json(const nlohmann::json &j, PathAttributes &attrs);
void to_json(nlohmann::json &j, const BgpOpenPayload &payload);
void from_json(const nlohmann::json &j, BgpOpenPayload &payload);
void to_json(nlohmann::json &j, const BgpUpdatePayload &payload);
void from_json(const nlohmann::json &j, BgpUpdatePayload &payload);
void to_json(nlohmann::json &j, const BgpNotificationPayload &payload);
void from_json(const nlohmann::json &j, BgpNotificationPayload &payload);
void to_json(nlohmann::json &j, const BgpMessage &message);
void from_json(const nlohmann::json &j, BgpMessage &message);
void to_json(nlohmann::json &j, const NeighborConfig &config);
void from_json(const nlohmann::json &j, NeighborConfig &config);
void to_json(nlohmann::json &j, const RouterConfig &config);
void from_json(const nlohmann::json &j, RouterConfig &config);
void to_json(nlohmann::json &j, const LinkConfig &config);
void from_json(const nlohmann::json &j, LinkConfig &config);
void to_json(nlohmann::json &j, const SimulationConfig &config);
void from_json(const nlohmann::json &j, SimulationConfig &config);
void to_json(nlohmann::json &j, const TopologyConfig &config);
void from_json(const nlohmann::json &j, TopologyConfig &config);
void to_json(nlohmann::json &j, const RouteEntry &route);

} // namespace toposim
