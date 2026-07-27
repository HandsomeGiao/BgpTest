#pragma once

#include "core/Types.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bgptester
{

enum class TopologyLoadStage
{
    ReadingRouters,
    ReadingLinks,
    Validating
};

struct TopologyLoadProgress
{
    TopologyLoadStage stage = TopologyLoadStage::ReadingRouters;
    std::int64_t bytesProcessed = 0;
    std::int64_t totalBytes = 0;
    std::size_t routersLoaded = 0;
    std::size_t linksLoaded = 0;
};

using TopologyLoadProgressCallback = std::function<bool(const TopologyLoadProgress&)>;

bool isCanonicalIpv4Address(std::string_view text, bool allowZero = true);
bool isCanonicalIpv4Prefix(std::string_view text);

class Topology
{
public:
    SimulationSettings simulation;
    RouterMap routers;
    std::vector<LinkConfig> links;

    Json toJson() const;
    static Topology starter();
    static std::optional<Topology> fromJson(const Json& object, std::string* error = nullptr);
    static std::optional<Topology> load(const std::filesystem::path& path, std::string* error = nullptr,
                                        const TopologyLoadProgressCallback& progress = {});
    bool save(const std::filesystem::path& path, std::string* error = nullptr) const;

    std::vector<std::string> validate() const;
    NeighborIndex buildNeighborIndex() const;
    std::vector<NeighborConfig> neighborsFor(const std::string& routerId) const;
    const LinkConfig* findLink(const std::string& a, const std::string& b) const;
    LinkConfig* findLink(const std::string& a, const std::string& b);
    std::string nextRouterName() const;
    std::string nextBgpRouterId() const;
    static std::string routerIdFromIndex(int oneBasedIndex);
    static std::string edgeKey(const std::string& a, const std::string& b);
};

} // namespace bgptester
