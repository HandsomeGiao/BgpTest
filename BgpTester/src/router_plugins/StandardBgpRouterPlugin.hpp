#pragma once

#include "plugin/RouterPlugin.hpp"

namespace bgptester
{

// Baseline BGP policy shared by the standard router and routers that extend
// standard BGP behavior with additional path attributes or decision logic.
class StandardBgpRouterNode : public RouterNode
{
public:
    explicit StandardBgpRouterNode(RouterNodeContext context, QObject* parent = nullptr);

    RouteEntry createOriginatedRoute(const QString& prefix) override;
    std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer) override;
    std::optional<RouteEntry> importAdvertisedRoute(const QString& prefix, const RouteEntry& advertisedRoute,
                                                    const NeighborConfig& fromPeer) override;
    std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>& currentBest) override;
    std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) override;

protected:
    // Shared classic-BGP comparison primitives let derived protocols apply an
    // eligibility predicate without first copying eligible routes into a
    // temporary container.
    static bool primaryRouteBetter(const RouteEntry& lhs, const RouteEntry& rhs);
    static bool samePrimaryPreference(const RouteEntry& lhs, const RouteEntry& rhs);
    static bool deterministicRouteBetter(const RouteEntry& lhs, const RouteEntry& rhs);

private:
    bool hasRouteReflectorClients() const;
};

class StandardBgpRouterPlugin final : public RouterNodePlugin
{
public:
    RouterPluginMetadata metadata() const override;
    RouterNode* createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) override;
};

} // namespace bgptester
