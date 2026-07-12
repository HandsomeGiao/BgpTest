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

inline constexpr int RouterPluginApiVersion = 1;

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
    virtual void peerStateChanged(const NeighborConfig&, PeerState)
    {
    }

    virtual RouteEntry createOriginatedRoute(const QString& prefix) = 0;

    virtual std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes,
                                                  const NeighborConfig& fromPeer) = 0;

    virtual std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                                      const std::optional<RouteEntry>& currentBest) = 0;

    // Returning std::nullopt filters (or withdraws) the route for this peer.
    virtual std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) = 0;

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
