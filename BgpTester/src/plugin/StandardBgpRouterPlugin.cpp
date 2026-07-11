#include "plugin/StandardBgpRouterPlugin.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace bgptester {
namespace {

bool primaryBetter(const RouteEntry &lhs, const RouteEntry &rhs) {
  if (lhs.localOrigin != rhs.localOrigin) {
    return lhs.localOrigin;
  }
  if (lhs.attributes.localPref != rhs.attributes.localPref) {
    return lhs.attributes.localPref > rhs.attributes.localPref;
  }
  if (lhs.attributes.asPath.size() != rhs.attributes.asPath.size()) {
    return lhs.attributes.asPath.size() < rhs.attributes.asPath.size();
  }
  if (lhs.attributes.med != rhs.attributes.med) {
    return lhs.attributes.med < rhs.attributes.med;
  }
  if (lhs.sourceSession != rhs.sourceSession) {
    return lhs.sourceSession == SessionType::Ebgp;
  }
  return false;
}

bool samePrimaryPreference(const RouteEntry &lhs, const RouteEntry &rhs) {
  return lhs.localOrigin == rhs.localOrigin &&
         lhs.attributes.localPref == rhs.attributes.localPref &&
         lhs.attributes.asPath.size() == rhs.attributes.asPath.size() &&
         lhs.attributes.med == rhs.attributes.med &&
         lhs.sourceSession == rhs.sourceSession;
}

bool deterministicBetter(const RouteEntry &lhs, const RouteEntry &rhs) {
  return std::tie(lhs.attributes.nextHop, lhs.learnedFrom) <
         std::tie(rhs.attributes.nextHop, rhs.learnedFrom);
}

} // namespace

StandardBgpRouterNode::StandardBgpRouterNode(RouterNodeContext context,
                                             QObject *parent)
    : RouterNode(std::move(context), parent) {}

RouteEntry
StandardBgpRouterNode::createOriginatedRoute(const QString &prefix) {
  RouteEntry route;
  route.prefix = prefix;
  route.attributes.nextHop = context().config.routerId;
  route.learnedFrom = context().config.id;
  route.localOrigin = true;
  return route;
}

std::optional<RouteEntry> StandardBgpRouterNode::importRoute(
    const QString &prefix, const PathAttributes &attributes,
    const NeighborConfig &fromPeer) {
  if (attributes.asPath.contains(context().config.asn)) {
    return std::nullopt;
  }
  if (!attributes.originatorId.isEmpty() &&
      attributes.originatorId == context().config.routerId) {
    return std::nullopt;
  }
  const auto clusterId = context().config.clusterId.isEmpty()
                             ? context().config.routerId
                             : context().config.clusterId;
  if (attributes.clusterList.contains(clusterId)) {
    return std::nullopt;
  }

  RouteEntry route;
  route.prefix = prefix;
  route.attributes = attributes;
  route.learnedFrom = fromPeer.id;
  route.sourceSession = fromPeer.sessionType;
  route.localOrigin = false;
  return route;
}

std::optional<RouteEntry> StandardBgpRouterNode::selectBestRoute(
    const QString &, const QVector<RouteEntry> &candidates,
    const std::optional<RouteEntry> &currentBest) {
  if (candidates.isEmpty()) {
    return std::nullopt;
  }

  RouteEntry primaryBest = candidates.front();
  for (const auto &candidate : candidates) {
    if (primaryBetter(candidate, primaryBest)) {
      primaryBest = candidate;
    }
  }

  if (currentBest && samePrimaryPreference(*currentBest, primaryBest) &&
      std::find(candidates.cbegin(), candidates.cend(), *currentBest) !=
          candidates.cend()) {
    return currentBest;
  }

  RouteEntry result = primaryBest;
  for (const auto &candidate : candidates) {
    if (samePrimaryPreference(candidate, primaryBest) &&
        deterministicBetter(candidate, result)) {
      result = candidate;
    }
  }
  return result;
}

std::optional<RouteEntry>
StandardBgpRouterNode::exportRoute(const RouteEntry &route,
                                   const NeighborConfig &toPeer) {
  if (route.learnedFrom == toPeer.id) {
    return std::nullopt;
  }

  bool allowed = toPeer.sessionType == SessionType::Ebgp ||
                 route.localOrigin ||
                 route.sourceSession == SessionType::Ebgp;
  if (!allowed && hasRouteReflectorClients()) {
    const auto learnedPeer = context().neighbors.constFind(route.learnedFrom);
    const auto learnedFromClient =
        learnedPeer != context().neighbors.cend() && learnedPeer->rrClient;
    allowed = learnedFromClient || toPeer.rrClient;
  }
  if (!allowed) {
    return std::nullopt;
  }

  RouteEntry result = route;
  result.learnedFrom = context().config.id;
  result.localOrigin = false;
  result.sourceSession = toPeer.sessionType;
  if (toPeer.sessionType == SessionType::Ebgp) {
    result.attributes.asPath.prepend(context().config.asn);
    result.attributes.nextHop = context().config.routerId;
    result.attributes.originatorId.clear();
    result.attributes.clusterList.clear();
    return result;
  }

  if (result.attributes.nextHop.isEmpty()) {
    result.attributes.nextHop = context().config.routerId;
  }
  if (route.sourceSession == SessionType::Ibgp && !route.localOrigin &&
      hasRouteReflectorClients()) {
    if (result.attributes.originatorId.isEmpty()) {
      const auto learned =
          context().topologyRouters.constFind(route.learnedFrom);
      result.attributes.originatorId =
          learned == context().topologyRouters.cend()
              ? route.attributes.nextHop
              : learned->routerId;
    }
    const auto clusterId = context().config.clusterId.isEmpty()
                               ? context().config.routerId
                               : context().config.clusterId;
    if (!result.attributes.clusterList.contains(clusterId)) {
      result.attributes.clusterList.append(clusterId);
    }
  }
  return result;
}

bool StandardBgpRouterNode::hasRouteReflectorClients() const {
  return std::any_of(context().neighbors.cbegin(), context().neighbors.cend(),
                     [](const auto &peer) { return peer.rrClient; });
}

RouterPluginMetadata StandardBgpRouterPlugin::metadata() const {
  return RouterPluginMetadata{
      .id = StandardRouterPluginId,
      .displayName = QStringLiteral("标准 BGP 路由器"),
      .version = QStringLiteral("1.0.0"),
      .description = QStringLiteral(
          "内置 RFC 风格 BGP 节点，支持 EBGP、IBGP、路由反射与 MRAI。"),
      .apiVersion = RouterPluginApiVersion,
      .defaultSettings = {},
  };
}

RouterNode *StandardBgpRouterPlugin::createRouterNode(
    const RouterNodeContext &context, QObject *parent, QString *error) {
  if (error) {
    error->clear();
  }
  return new StandardBgpRouterNode(context, parent);
}

} // namespace bgptester
