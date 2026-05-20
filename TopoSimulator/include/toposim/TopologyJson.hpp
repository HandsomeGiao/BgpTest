#pragma once

#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "toposim/BgpTypes.hpp"

namespace toposim {

TopologyConfig loadTopologyConfig(const std::filesystem::path &topology_file);

nlohmann::json toJson(const TopologyConfig &config);
nlohmann::json toJson(const std::vector<RouterSnapshot> &snapshot);
nlohmann::json toJson(const RibSnapshot &snapshot);
nlohmann::json toJson(const std::vector<PeerSnapshot> &snapshot);

} // namespace toposim
