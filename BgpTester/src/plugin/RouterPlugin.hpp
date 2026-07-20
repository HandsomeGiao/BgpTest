#pragma once

#include "engine/BgpTypes.hpp"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <utility>

namespace bgptester
{

inline constexpr int RouterPluginApiVersion = 5;

struct RouterPluginMetadata
{
    QString id;
    QString displayName;
    QString version;
    QString description;
    int apiVersion = RouterPluginApiVersion;
    QJsonObject defaultSettings;
};

// This is an immutable view of the topology when a simulation starts. A
// RouterNode instance is created for every RouterConfig in the topology.
struct RouterNodeContext
{
    RouterConfig config;
    QMap<QString, RouterConfig> topologyRouters;
    QMap<QString, NeighborConfig> neighbors;
};

// RouterNode owns the protocol policy for one router. SimulationEngine keeps
// responsibility for the event queue, links, BGP session transport and RIB
// storage; plugins control route creation, import, selection and export.
class RouterNode : public QObject
{
public:
    explicit RouterNode(RouterNodeContext context, QObject* parent = nullptr) : QObject(parent), context_(std::move(context))
    {
    }
    ~RouterNode() override = default;

    RouterNode(const RouterNode&) = delete;
    RouterNode& operator=(const RouterNode&) = delete;

    const RouterNodeContext& context() const
    {
        return context_;
    }

    virtual QStringList validateConfiguration() const
    {
        return {};
    }
    virtual void simulationStarted()
    {
    }
    virtual void simulationStopped()
    {
    }
    virtual void routerStateChanged(bool)
    {
    }
    virtual void convergenceStateChanged(bool)
    {
    }
    virtual void peerStateChanged(const NeighborConfig&, PeerState)
    {
    }

    virtual RouteEntry createOriginatedRoute(const QString& prefix) = 0;

    virtual std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes,
                                                  const NeighborConfig& fromPeer) = 0;

    // The full advertised route carries simulator-internal metadata that is
    // not encoded as a BGP path attribute. Attributes-only plugins retain
    // their existing behavior unless they opt into this hook.
    virtual std::optional<RouteEntry> importAdvertisedRoute(const QString& prefix, const RouteEntry& advertisedRoute,
                                                            const NeighborConfig& fromPeer)
    {
        return importRoute(prefix, advertisedRoute.attributes, fromPeer);
    }

    // Called before a route from this peer is removed from Adj-RIB-In.  The
    // default keeps source plugins that do not use withdrawal attributes
    // equivalent to ordinary BGP behavior.
    virtual void importWithdrawal(const QString&, const PathAttributes&, const NeighborConfig&)
    {
    }

    // Notification for a locally originated route being removed.  Stateful
    // plugins can advance per-prefix control-plane state before decision and
    // dissemination run.
    virtual void localRouteWithdrawn(const QString&)
    {
    }

    virtual std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                                      const std::optional<RouteEntry>& currentBest) = 0;

    // A stateful plugin can request dissemination even when its selected
    // RouteEntry is unchanged (for example, to flush prefix-local triggers).
    virtual bool requiresDissemination(const QString&) const
    {
        return false;
    }
    virtual void decisionCompleted(const QString&)
    {
    }

    // Returning std::nullopt filters (or withdraws) the route for this peer.
    virtual std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) = 0;

    // Prefix-aware export hook.  Existing plugins only need exportRoute();
    // plugins with per-prefix attributes can override this method.
    virtual std::optional<RouteEntry> exportRouteForPrefix(const QString&, const RouteEntry& route, const NeighborConfig& toPeer)
    {
        return exportRoute(route, toPeer);
    }

    // Attributes for a withdrawal caused by the local selected route becoming
    // unavailable.  This is deliberately not called for an export-policy
    // filter while a selected route still exists.
    virtual PathAttributes exportWithdrawal(const QString&, const NeighborConfig&)
    {
        return {};
    }

private:
    RouterNodeContext context_;
};

class RouterNodePlugin
{
public:
    virtual ~RouterNodePlugin() = default;

    virtual RouterPluginMetadata metadata() const = 0;

    // The factory can be called from the simulation thread. Implementations
    // must not retain the parent or error pointer and must not throw across the
    // plugin boundary.
    virtual RouterNode* createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) = 0;
};

} // namespace bgptester
