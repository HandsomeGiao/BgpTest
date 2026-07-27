#pragma once

#include "core/Types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bgptester
{

inline constexpr int RouterPolicyApiVersion = 5;
inline constexpr std::string_view StandardRouterPolicyId = "org.bgptester.router.standard-bgp";
inline constexpr std::string_view ConfigurableExportRouterPolicyId = "org.bgptester.example.configurable-export";
inline constexpr std::string_view TfpVersionRouterPolicyId = "org.bgptester.router.tfp-version";

struct RouterPolicyMetadata
{
    std::string id;
    std::string displayName;
    std::string version;
    std::string description;
    int apiVersion = RouterPolicyApiVersion;
    Json defaultSettings = Json::object();
};

using RouterConfigMap = RouterMap;

// Every policy receives one immutable topology view. Sharing the router map
// avoids copying a std::map once per router on large topologies.
struct RouterPolicyContext
{
    RouterConfig config;
    std::shared_ptr<const RouterConfigMap> topologyRouters;
    std::map<std::string, NeighborConfig> neighbors;
};

class RouterPolicy
{
public:
    explicit RouterPolicy(RouterPolicyContext context);
    virtual ~RouterPolicy() = default;

    RouterPolicy(const RouterPolicy&) = delete;
    RouterPolicy& operator=(const RouterPolicy&) = delete;

    [[nodiscard]] const RouterPolicyContext& context() const noexcept;

    [[nodiscard]] virtual std::vector<std::string> validateConfiguration() const;
    virtual void simulationStarted();
    virtual void simulationStopped();
    virtual void routerStateChanged(bool enabled);
    virtual void convergenceStateChanged(bool converged);
    virtual void peerStateChanged(const NeighborConfig& neighbor, PeerState state);

    [[nodiscard]] virtual RouteEntry createOriginatedRoute(std::string_view prefix) = 0;
    [[nodiscard]] virtual std::optional<RouteEntry> importRoute(std::string_view prefix, const PathAttributes& attributes,
                                                                const NeighborConfig& fromPeer) = 0;
    [[nodiscard]] virtual std::optional<RouteEntry> importAdvertisedRoute(std::string_view prefix,
                                                                          const RouteEntry& advertisedRoute,
                                                                          const NeighborConfig& fromPeer);
    virtual void importWithdrawal(std::string_view prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer);
    virtual void localRouteWithdrawn(std::string_view prefix);

    [[nodiscard]] virtual std::optional<RouteEntry> selectBestRoute(std::string_view prefix,
                                                                    const std::vector<RouteEntry>& candidates,
                                                                    const std::optional<RouteEntry>& currentBest) = 0;
    [[nodiscard]] virtual bool requiresDissemination(std::string_view prefix) const;
    virtual void decisionCompleted(std::string_view prefix);

    [[nodiscard]] virtual std::optional<RouteEntry> exportRoute(const RouteEntry& route,
                                                                 const NeighborConfig& toPeer) = 0;
    [[nodiscard]] virtual std::optional<RouteEntry> exportRouteForPrefix(std::string_view prefix, const RouteEntry& route,
                                                                          const NeighborConfig& toPeer);
    [[nodiscard]] virtual PathAttributes exportWithdrawal(std::string_view prefix, const NeighborConfig& toPeer);

private:
    RouterPolicyContext context_;
};

using RouterPolicyFactory = std::function<std::unique_ptr<RouterPolicy>(RouterPolicyContext, std::string*)>;

struct RegisteredRouterPolicy
{
    RouterPolicyMetadata metadata;
    std::string source;
};

class RouterPolicyRegistry final
{
public:
    static RouterPolicyRegistry& instance();

    RouterPolicyRegistry(const RouterPolicyRegistry&) = delete;
    RouterPolicyRegistry& operator=(const RouterPolicyRegistry&) = delete;

    [[nodiscard]] std::vector<RegisteredRouterPolicy> policies() const;
    [[nodiscard]] std::optional<RouterPolicyMetadata> metadata(std::string_view policyId) const;
    [[nodiscard]] bool contains(std::string_view policyId) const;
    [[nodiscard]] std::vector<std::string> registrationErrors() const;

    bool registerPolicy(RouterPolicyMetadata metadata, RouterPolicyFactory factory, std::string source,
                        std::string* error = nullptr);
    [[nodiscard]] std::unique_ptr<RouterPolicy> createRouterPolicy(RouterPolicyContext context,
                                                                   std::string* error = nullptr) const;

private:
    RouterPolicyRegistry();

    struct Entry
    {
        RouterPolicyMetadata metadata;
        RouterPolicyFactory factory;
        std::string source;
    };

    mutable std::shared_mutex mutex_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::vector<std::string> registrationErrors_;
};

} // namespace bgptester
