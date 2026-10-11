#pragma once

#include "router_plugins/StandardBgpRouterPlugin.hpp"
#include "router_plugins/versioned_path/DependencyGraph.hpp"
#include <QSet>

namespace bgptester
{

inline const QString VersionedPathRouterPluginId = QStringLiteral("org.bgptester.router.versioned-path");

// Freshness changes eligibility; classic BGP still ranks the remaining routes.
class VersionedPathRouterNode final : public StandardBgpRouterNode
{
public:
    explicit VersionedPathRouterNode(RouterNodeContext context, QObject* parent = nullptr);
    QStringList validateConfiguration() const override;
    bool usesVersionedPaths() const override { return true; }
    RouteEntry createOriginatedRoute(const QString& prefix) override;
    std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes,
                                         const NeighborConfig& peer) override;
    std::optional<RouteEntry> importAdvertisedRoute(const QString& prefix, const RouteEntry& route,
                                                   const NeighborConfig& peer) override;
    std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                             const std::optional<RouteEntry>& current) override;
    std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& peer) override;
    std::optional<RouteEntry> exportRouteForPrefix(const QString& prefix, const RouteEntry& route,
                                                  const NeighborConfig& peer) override;
    bool requiresDissemination(const QString& prefix) const override;
    void decisionCompleted(const QString& prefix) override;
    void routerStateChanged(bool enabled) override;
    void peerStateChanged(const NeighborConfig& peer, PeerState state) override;
    QSet<QString> observeStateRecords(const std::vector<StateRecord>& records, const NeighborConfig& peer) override;
    std::vector<StateRecord> withdrawalRecords(const QString& prefix) const override;
    std::vector<StateRecord> takeStateRecords(const NeighborConfig& peer) override;
    nlohmann::json diagnostics() const override;

private:
    struct PrefixState
    {
        VersionedDependencyGraph graph;
        std::uint64_t localVersion = 0;
        std::optional<RouteEntry> selected;
        bool changed = false;
        std::map<StateCreator, std::uint64_t> retirements;
        std::set<VersionedState> revocations;
        std::vector<StateRecord> pending;
        std::set<std::pair<std::string, VersionedState>> rejectedCandidates;
    };
    bool validCreator(const StateCreator& creator) const;
    bool observePath(const QString& prefix, const PathAttributes& attributes, const NeighborConfig& peer);
    void updateSelection(const std::string& prefix, const std::optional<RouteEntry>& route);
    bool learnRecord(const StateRecord& record);
    StateCreator creator_;
    std::string mode_;
    std::map<std::string, PrefixState, std::less<>> states_;
    // Per-session facts already sent. Retirements use maxima, exact revocations
    // use identity; reconnect clears this ledger and resynchronizes knowledge.
    std::map<std::string, std::map<std::pair<std::string, StateCreator>, std::uint64_t>> sentRetirements_;
    std::map<std::string, std::set<StateRecord>> sentRevocations_;
    std::uint64_t rejectedAttributes_ = 0;
    std::uint64_t learnedRecords_ = 0;
};

class VersionedPathRouterPlugin final : public RouterNodePlugin
{
public:
    RouterPluginMetadata metadata() const override;
    RouterNode* createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) override;
};

} // namespace bgptester
