#pragma once

#include "plugin/RouterPlugin.hpp"

namespace bgptester
{

class StandardBgpRouterNode final : public RouterNode
{
public:
    explicit StandardBgpRouterNode(RouterNodeContext context, QObject* parent = nullptr);

    [[nodiscard]] RouteEntry createOriginatedRoute(const QString& prefix) override;
    [[nodiscard]] std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes,
                                                        const NeighborConfig& fromPeer) override;
    [[nodiscard]] std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                                            const std::optional<RouteEntry>& currentBest) override;
    [[nodiscard]] std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) override;

private:
    [[nodiscard]] bool hasRouteReflectorClients() const;
};

class StandardBgpRouterPlugin final : public RouterNodePlugin
{
public:
    [[nodiscard]] RouterPluginMetadata metadata() const override;
    [[nodiscard]] RouterNode* createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) override;
};

} // namespace bgptester
