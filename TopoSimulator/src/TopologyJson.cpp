#include "toposim/TopologyJson.hpp"

#include <fstream>
#include <stdexcept>

namespace toposim {
namespace {

template <typename T>
void getIfPresent(const nlohmann::json &j, const char *key, T &value) {
  if (j.contains(key) && !j.at(key).is_null()) {
    j.at(key).get_to(value);
  }
}

} // namespace

void to_json(nlohmann::json &j, const PathAttributes &attrs) {
  j = nlohmann::json{
      {"origin", attrs.origin},     {"as_path", attrs.as_path},
      {"next_hop", attrs.next_hop}, {"local_pref", attrs.local_pref},
      {"med", attrs.med},           {"communities", attrs.communities},
  };
  if (attrs.originator_id) {
    j["originator_id"] = *attrs.originator_id;
  }
  if (attrs.cluster_list) {
    j["cluster_list"] = *attrs.cluster_list;
  }
}

void from_json(const nlohmann::json &j, PathAttributes &attrs) {
  getIfPresent(j, "origin", attrs.origin);
  getIfPresent(j, "as_path", attrs.as_path);
  getIfPresent(j, "next_hop", attrs.next_hop);
  getIfPresent(j, "local_pref", attrs.local_pref);
  getIfPresent(j, "med", attrs.med);
  getIfPresent(j, "communities", attrs.communities);
  if (j.contains("originator_id") && !j.at("originator_id").is_null()) {
    attrs.originator_id = j.at("originator_id").get<std::string>();
  }
  if (j.contains("cluster_list") && !j.at("cluster_list").is_null()) {
    attrs.cluster_list = j.at("cluster_list").get<std::string>();
  }
}

void to_json(nlohmann::json &j, const BgpOpenPayload &payload) {
  j = nlohmann::json{
      {"version", payload.version},
      {"asn", payload.asn},
      {"hold_time_seconds", payload.hold_time_seconds},
      {"router_id", payload.router_id},
  };
}

void from_json(const nlohmann::json &j, BgpOpenPayload &payload) {
  getIfPresent(j, "version", payload.version);
  getIfPresent(j, "asn", payload.asn);
  getIfPresent(j, "hold_time_seconds", payload.hold_time_seconds);
  getIfPresent(j, "router_id", payload.router_id);
}

void to_json(nlohmann::json &j, const BgpUpdatePayload &payload) {
  j = nlohmann::json{
      {"withdrawn_routes", payload.withdrawn_routes},
      {"nlri", payload.nlri},
      {"path_attributes", payload.path_attributes},
  };
}

void from_json(const nlohmann::json &j, BgpUpdatePayload &payload) {
  getIfPresent(j, "withdrawn_routes", payload.withdrawn_routes);
  getIfPresent(j, "nlri", payload.nlri);
  if (j.contains("path_attributes") && !j.at("path_attributes").is_null()) {
    payload.path_attributes = j.at("path_attributes").get<PathAttributes>();
  }
}

void to_json(nlohmann::json &j, const BgpNotificationPayload &payload) {
  j = nlohmann::json{
      {"error_code", payload.error_code},
      {"error_subcode", payload.error_subcode},
      {"data", payload.data},
  };
}

void from_json(const nlohmann::json &j, BgpNotificationPayload &payload) {
  getIfPresent(j, "error_code", payload.error_code);
  getIfPresent(j, "error_subcode", payload.error_subcode);
  getIfPresent(j, "data", payload.data);
}

void to_json(nlohmann::json &j, const BgpMessage &message) {
  j = nlohmann::json{
      {"type", toString(message.type)},
      {"from", message.from},
      {"to", message.to},
      {"sequence", message.sequence},
  };
  if (message.open) {
    j["open"] = *message.open;
  }
  if (message.update) {
    j["update"] = *message.update;
  }
  if (message.notification) {
    j["notification"] = *message.notification;
  }
}

void from_json(const nlohmann::json &j, BgpMessage &message) {
  if (j.contains("type")) {
    message.type = bgpMessageTypeFromString(j.at("type").get<std::string>());
  }
  getIfPresent(j, "from", message.from);
  getIfPresent(j, "to", message.to);
  getIfPresent(j, "sequence", message.sequence);
  if (j.contains("open") && !j.at("open").is_null()) {
    message.open = j.at("open").get<BgpOpenPayload>();
  }
  if (j.contains("update") && !j.at("update").is_null()) {
    message.update = j.at("update").get<BgpUpdatePayload>();
  }
  if (j.contains("notification") && !j.at("notification").is_null()) {
    message.notification = j.at("notification").get<BgpNotificationPayload>();
  }
}

void to_json(nlohmann::json &j, const NeighborConfig &config) {
  j = nlohmann::json{
      {"id", config.id},
      {"remote_asn", config.remote_asn},
      {"session_type", toString(config.session_type)},
      {"rr_client", config.rr_client},
      {"enabled", config.enabled},
      {"hold_time_seconds", config.hold_time_seconds},
      {"mrai_ms", config.mrai_ms},
  };
}

void from_json(const nlohmann::json &j, NeighborConfig &config) {
  j.at("id").get_to(config.id);
  getIfPresent(j, "remote_asn", config.remote_asn);
  if (j.contains("session_type")) {
    config.session_type =
        sessionTypeFromString(j.at("session_type").get<std::string>());
  }
  getIfPresent(j, "rr_client", config.rr_client);
  getIfPresent(j, "enabled", config.enabled);
  getIfPresent(j, "hold_time_seconds", config.hold_time_seconds);
  getIfPresent(j, "mrai_ms", config.mrai_ms);
}

void to_json(nlohmann::json &j, const RouterConfig &config) {
  j = nlohmann::json{
      {"id", config.id},
      {"router_id", config.router_id},
      {"asn", config.asn},
      {"cluster_id", config.cluster_id},
      {"originated_prefixes", config.originated_prefixes},
      {"neighbors", config.neighbors},
  };
}

void from_json(const nlohmann::json &j, RouterConfig &config) {
  j.at("id").get_to(config.id);
  j.at("router_id").get_to(config.router_id);
  j.at("asn").get_to(config.asn);
  getIfPresent(j, "cluster_id", config.cluster_id);
  getIfPresent(j, "originated_prefixes", config.originated_prefixes);
  getIfPresent(j, "neighbors", config.neighbors);
}

void to_json(nlohmann::json &j, const LinkConfig &config) {
  j = nlohmann::json{
      {"a", config.a},
      {"b", config.b},
      {"enabled", config.enabled},
      {"delay_ms", config.delay_ms},
      {"rr_client_from_a", config.rr_client_from_a},
      {"rr_client_from_b", config.rr_client_from_b},
      {"mrai_ms_from_a", config.mrai_ms_from_a},
      {"mrai_ms_from_b", config.mrai_ms_from_b},
  };
}

void from_json(const nlohmann::json &j, LinkConfig &config) {
  j.at("a").get_to(config.a);
  j.at("b").get_to(config.b);
  getIfPresent(j, "enabled", config.enabled);
  getIfPresent(j, "delay_ms", config.delay_ms);
  getIfPresent(j, "rr_client_from_a", config.rr_client_from_a);
  getIfPresent(j, "rr_client_from_b", config.rr_client_from_b);
  getIfPresent(j, "mrai_ms_from_a", config.mrai_ms_from_a);
  getIfPresent(j, "mrai_ms_from_b", config.mrai_ms_from_b);
}

void to_json(nlohmann::json &j, const SimulationConfig &config) {
  j = nlohmann::json{
      {"name", config.name},
      {"log_dir", config.log_dir},
      {"worker_threads", config.worker_threads},
      {"convergence_quiet_ms", config.convergence_quiet_ms},
      {"router_class", config.router_class},
  };
}

void from_json(const nlohmann::json &j, SimulationConfig &config) {
  getIfPresent(j, "name", config.name);
  getIfPresent(j, "log_dir", config.log_dir);
  getIfPresent(j, "worker_threads", config.worker_threads);
  getIfPresent(j, "convergence_quiet_ms", config.convergence_quiet_ms);
  getIfPresent(j, "router_class", config.router_class);
}

void to_json(nlohmann::json &j, const TopologyConfig &config) {
  j = nlohmann::json{
      {"simulation", config.simulation},
      {"routers", config.routers},
      {"links", config.links},
  };
}

void from_json(const nlohmann::json &j, TopologyConfig &config) {
  getIfPresent(j, "simulation", config.simulation);
  j.at("routers").get_to(config.routers);
  getIfPresent(j, "links", config.links);
}

void to_json(nlohmann::json &j, const RouteEntry &route) {
  j = nlohmann::json{
      {"prefix", route.prefix},
      {"path_attributes", route.attributes},
      {"learned_from", route.learned_from},
      {"source_session", toString(route.source_session)},
      {"local_origin", route.local_origin},
  };
}

void to_json(nlohmann::json &j, const RouterSnapshot &snapshot) {
  j = nlohmann::json{
      {"id", snapshot.id},
      {"router_id", snapshot.router_id},
      {"asn", snapshot.asn},
      {"active", snapshot.active},
      {"has_rr_clients", snapshot.has_rr_clients},
  };
}

void to_json(nlohmann::json &j, const PeerSnapshot &snapshot) {
  j = nlohmann::json{
      {"id", snapshot.id},
      {"remote_asn", snapshot.remote_asn},
      {"session_type", toString(snapshot.session_type)},
      {"rr_client", snapshot.rr_client},
      {"mrai_ms", snapshot.mrai_ms},
      {"enabled", snapshot.enabled},
      {"state", toString(snapshot.state)},
  };
}

void to_json(nlohmann::json &j, const RibSnapshot &snapshot) {
  j = nlohmann::json{
      {"router", snapshot.router},
      {"local_routes", snapshot.local_routes},
      {"loc_rib", snapshot.loc_rib},
      {"adj_rib_in", snapshot.adj_rib_in},
      {"adj_rib_out", snapshot.adj_rib_out},
  };
}

TopologyConfig loadTopologyConfig(const std::filesystem::path &topology_file) {
  std::ifstream in(topology_file);
  if (!in) {
    throw std::runtime_error("Unable to open topology file: " +
                             topology_file.string());
  }
  nlohmann::json j;
  try {
    in >> j;
    return j.get<TopologyConfig>();
  } catch (const nlohmann::json::exception &ex) {
    throw std::runtime_error("Failed to parse topology file " +
                             topology_file.string() + ": " + ex.what());
  }
}

nlohmann::json toJson(const std::vector<RouterSnapshot> &snapshot) {
  return nlohmann::json(snapshot);
}

nlohmann::json toJson(const TopologyConfig &config) {
  return nlohmann::json(config);
}

nlohmann::json toJson(const RibSnapshot &snapshot) {
  return nlohmann::json(snapshot);
}

nlohmann::json toJson(const std::vector<PeerSnapshot> &snapshot) {
  return nlohmann::json(snapshot);
}

} // namespace toposim
