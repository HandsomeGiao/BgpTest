#include "router_plugins/StandardBgpRouterPlugin.hpp"

#include "plugin/RouterPluginRegistry.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace bgptester
{
namespace
{

constexpr quint32 ProviderLocalPreference = 50;
constexpr quint32 DefaultLocalPreference = 100;
constexpr quint32 PeerLocalPreference = DefaultLocalPreference;
constexpr quint32 CustomerLocalPreference = 200;

bool primaryBetter(const RouteEntry& lhs, const RouteEntry& rhs)
{
    if (lhs.localOrigin != rhs.localOrigin)
    {
        return lhs.localOrigin;
    }
    if (lhs.attributes.localPref != rhs.attributes.localPref)
    {
        return lhs.attributes.localPref > rhs.attributes.localPref;
    }
    if (lhs.attributes.asPath.size() != rhs.attributes.asPath.size())
    {
        return lhs.attributes.asPath.size() < rhs.attributes.asPath.size();
    }
    if (lhs.attributes.med != rhs.attributes.med)
    {
        return lhs.attributes.med < rhs.attributes.med;
    }
    if (lhs.sourceSession != rhs.sourceSession)
    {
        return lhs.sourceSession == SessionType::Ebgp;
    }
    return false;
}

bool samePrimaryPreference(const RouteEntry& lhs, const RouteEntry& rhs)
{
    return lhs.localOrigin == rhs.localOrigin && lhs.attributes.localPref == rhs.attributes.localPref &&
           lhs.attributes.asPath.size() == rhs.attributes.asPath.size() && lhs.attributes.med == rhs.attributes.med &&
           lhs.sourceSession == rhs.sourceSession;
}

bool deterministicBetter(const RouteEntry& lhs, const RouteEntry& rhs)
{
    return std::tie(lhs.attributes.nextHop, lhs.learnedFrom) < std::tie(rhs.attributes.nextHop, rhs.learnedFrom);
}

RouteSource routeSourceFor(NeighborRelationship relationship)
{
    switch (relationship)
    {
        case NeighborRelationship::Unspecified:
            return RouteSource::Unspecified;
        case NeighborRelationship::Peer:
            return RouteSource::Peer;
        case NeighborRelationship::Provider:
            return RouteSource::Provider;
        case NeighborRelationship::Customer:
            return RouteSource::Customer;
    }
    return RouteSource::Unspecified;
}

bool businessExportAllowed(RouteSource source, NeighborRelationship destination)
{
    switch (destination)
    {
        case NeighborRelationship::Unspecified:
        case NeighborRelationship::Customer:
            return true;
        case NeighborRelationship::Peer:
        case NeighborRelationship::Provider:
            return source != RouteSource::Peer && source != RouteSource::Provider;
    }
    return true;
}

} // namespace

StandardBgpRouterNode::StandardBgpRouterNode(RouterNodeContext context, QObject* parent) : RouterNode(std::move(context), parent)
{
}

RouteEntry StandardBgpRouterNode::createOriginatedRoute(const QString&)
{
    RouteEntry route;
    route.attributes.nextHop = context().config.routerId;
    route.learnedFrom = context().config.id;
    route.localOrigin = true;
    route.source = RouteSource::Local;
    return route;
}

std::optional<RouteEntry> StandardBgpRouterNode::importRoute(const QString&, const PathAttributes& attributes,
                                                             const NeighborConfig& fromPeer)
{
    if (attributes.asPath.contains(context().config.asn))
    {
        return std::nullopt;
    }
    if (!attributes.originatorId.isEmpty() && attributes.originatorId == context().config.routerId)
    {
        return std::nullopt;
    }
    const auto clusterId = context().config.clusterId.isEmpty() ? context().config.routerId : context().config.clusterId;
    if (attributes.clusterList.contains(clusterId))
    {
        return std::nullopt;
    }

    RouteEntry route;
    route.attributes = attributes;
    route.learnedFrom = fromPeer.id;
    route.sourceSession = fromPeer.sessionType;
    route.localOrigin = false;
    return route;
}

std::optional<RouteEntry> StandardBgpRouterNode::importAdvertisedRoute(const QString& prefix, const RouteEntry& advertisedRoute,
                                                                       const NeighborConfig& fromPeer)
{
    // Keep this call virtual so derived standard-router plugins can continue
    // observing and extending the attributes-only import path.
    auto imported = importRoute(prefix, advertisedRoute.attributes, fromPeer);
    if (!imported)
    {
        return std::nullopt;
    }

    if (fromPeer.sessionType == SessionType::Ibgp)
    {
        imported->source = advertisedRoute.source;
        return imported;
    }

    imported->source = routeSourceFor(fromPeer.relationship);
    switch (fromPeer.relationship)
    {
        case NeighborRelationship::Customer:
            imported->attributes.localPref = CustomerLocalPreference;
            break;
        case NeighborRelationship::Peer:
            imported->attributes.localPref = PeerLocalPreference;
            break;
        case NeighborRelationship::Provider:
            imported->attributes.localPref = ProviderLocalPreference;
            break;
        case NeighborRelationship::Unspecified:
            break;
    }
    return imported;
}

std::optional<RouteEntry> StandardBgpRouterNode::selectBestRoute(const QString&, const QVector<RouteEntry>& candidates,
                                                                 const std::optional<RouteEntry>& currentBest)
{
    if (candidates.isEmpty())
    {
        return std::nullopt;
    }

    RouteEntry primaryBest = candidates.front();
    for (const auto& candidate : candidates)
    {
        if (primaryBetter(candidate, primaryBest))
        {
            primaryBest = candidate;
        }
    }

    if (currentBest && samePrimaryPreference(*currentBest, primaryBest) &&
        std::find(candidates.cbegin(), candidates.cend(), *currentBest) != candidates.cend())
    {
        return currentBest;
    }

    RouteEntry result = primaryBest;
    for (const auto& candidate : candidates)
    {
        if (samePrimaryPreference(candidate, primaryBest) && deterministicBetter(candidate, result))
        {
            result = candidate;
        }
    }
    return result;
}

std::optional<RouteEntry> StandardBgpRouterNode::exportRoute(const RouteEntry& route, const NeighborConfig& toPeer)
{
    if (route.learnedFrom == toPeer.id)
    {
        return std::nullopt;
    }

    bool allowed = toPeer.sessionType == SessionType::Ebgp || route.localOrigin || route.sourceSession == SessionType::Ebgp;
    if (!allowed && hasRouteReflectorClients())
    {
        const auto learnedPeer = context().neighbors.constFind(route.learnedFrom);
        const auto learnedFromClient = learnedPeer != context().neighbors.cend() && learnedPeer->rrClient;
        allowed = learnedFromClient || toPeer.rrClient;
    }
    if (allowed && toPeer.sessionType == SessionType::Ebgp)
    {
        allowed = businessExportAllowed(route.source, toPeer.relationship);
    }
    if (!allowed)
    {
        return std::nullopt;
    }

    RouteEntry result = route;
    result.learnedFrom = context().config.id;
    result.localOrigin = false;
    result.sourceSession = toPeer.sessionType;
    if (toPeer.sessionType == SessionType::Ebgp)
    {
        result.attributes.asPath.prepend(context().config.asn);
        result.attributes.nextHop = context().config.routerId;
        result.attributes.localPref = DefaultLocalPreference;
        result.attributes.originatorId.clear();
        result.attributes.clusterList.clear();
        result.source = RouteSource::Unspecified;
        return result;
    }

    if (result.attributes.nextHop.isEmpty())
    {
        result.attributes.nextHop = context().config.routerId;
    }
    if (route.sourceSession == SessionType::Ibgp && !route.localOrigin && hasRouteReflectorClients())
    {
        if (result.attributes.originatorId.isEmpty())
        {
            const auto learned = context().topologyRouters.constFind(route.learnedFrom);
            result.attributes.originatorId = learned == context().topologyRouters.cend() ? route.attributes.nextHop : learned->routerId;
        }
        const auto clusterId = context().config.clusterId.isEmpty() ? context().config.routerId : context().config.clusterId;
        if (!result.attributes.clusterList.contains(clusterId))
        {
            result.attributes.clusterList.append(clusterId);
        }
    }
    return result;
}

bool StandardBgpRouterNode::hasRouteReflectorClients() const
{
    return std::any_of(context().neighbors.cbegin(), context().neighbors.cend(), [](const auto& peer) { return peer.rrClient; });
}

RouterPluginMetadata StandardBgpRouterPlugin::metadata() const
{
    return RouterPluginMetadata{
        .id = StandardRouterPluginId,
        .displayName = QStringLiteral("标准 BGP 路由器"),
        .version = QStringLiteral("1.1.0"),
        .description = QStringLiteral("内置 RFC 风格 BGP 节点，支持 EBGP、IBGP、路由反射、MRAI 与商业关系策略。"),
        .apiVersion = RouterPluginApiVersion,
        .defaultSettings = {},
    };
}

RouterNode* StandardBgpRouterPlugin::createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error)
{
    if (error)
    {
        error->clear();
    }
    return new StandardBgpRouterNode(context, parent);
}

} // namespace bgptester

BGPTESTER_REGISTER_ROUTER_PLUGIN(bgptester::StandardBgpRouterPlugin)
