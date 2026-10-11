#include "router_plugins/VersionedPathRouterPlugin.hpp"
#include "plugin/RouterPluginRegistry.hpp"
#include "model/StrictIpv4.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace bgptester
{
using Json = nlohmann::json;
namespace
{
bool classicEquivalent(RouteEntry a, RouteEntry b)
{
    a.attributes.versionedPath.reset();
    b.attributes.versionedPath.reset();
    return a == b;
}
}

VersionedPathRouterNode::VersionedPathRouterNode(RouterNodeContext context, QObject* parent)
    : StandardBgpRouterNode(std::move(context), parent),
      creator_{this->context().config.asn, this->context().config.routerId.toStdString()}
{
    const auto value = this->context().config.pluginSettings.value(QStringLiteral("mode"));
    mode_ = value.isString() ? value.toString().toStdString() : "full";
}

QStringList VersionedPathRouterNode::validateConfiguration() const
{
    auto errors = StandardBgpRouterNode::validateConfiguration();
    const auto mode = context().config.pluginSettings.value(QStringLiteral("mode"));
    if ((!mode.isUndefined() && !mode.isString()) ||
        (mode_ != "full" && mode_ != "local" && mode_ != "path_only"))
        errors.append(QStringLiteral("versioned-path mode 必须为 full、local 或 path_only"));
    for (const auto& peer : context().neighbors)
        if (peer.rrClient) errors.append(QStringLiteral("版本化依赖图实验暂不支持路由反射器"));
    for (const auto& router : context().topologyRouters)
        if (router.pluginId != VersionedPathRouterPluginId)
        {
            errors.append(QStringLiteral("版本化依赖图要求全网部署 versioned-path 路由器"));
            break;
        }
    return errors;
}

bool VersionedPathRouterNode::validCreator(const StateCreator& creator) const
{
    if (creator.asn == 0 || !isCanonicalIpv4Address(QString::fromStdString(creator.routerId), false)) return false;
    return std::any_of(context().topologyRouters.cbegin(), context().topologyRouters.cend(), [&](const auto& router)
    {
        return router.asn == creator.asn && router.routerId.toStdString() == creator.routerId;
    });
}

bool VersionedPathRouterNode::observePath(const QString& prefix, const PathAttributes& attributes,
                                           const NeighborConfig& peer)
{
    const auto reject = [&] { ++rejectedAttributes_; return false; };
    if (!attributes.versionedPath) return reject();
    const auto& info = *attributes.versionedPath;
    std::vector<std::uint32_t> compressed;
    for (auto asn : attributes.asPath)
        if (compressed.empty() || compressed.back() != asn) compressed.push_back(asn);
    if (compressed.size() != info.path.size() || info.advertiser.creator.asn != peer.remoteAsn) return reject();
    if (!context().topologyRouters.isEmpty())
    {
        const auto remote = context().topologyRouters.constFind(peer.id);
        if (remote == context().topologyRouters.cend() ||
            remote->routerId.toStdString() != info.advertiser.creator.routerId) return reject();
    }
    if (peer.sessionType == SessionType::Ebgp &&
        (info.path.empty() || info.path.front() != info.advertiser)) return reject();
    for (std::size_t i = 0; i < compressed.size(); ++i)
        if (compressed[i] != info.path[i].creator.asn) return reject();
    const auto declaration = declarationPath(info);
    for (const auto& state : declaration)
    {
        if (!validCreator(state.creator) || !state.version) return reject();
        if (state.creator == creator_ && state.version > states_[prefix.toStdString()].localVersion) return reject();
    }
    if (!states_[prefix.toStdString()].graph.observe(declaration)) return reject();
    return true;
}

RouteEntry VersionedPathRouterNode::createOriginatedRoute(const QString& prefix)
{
    auto route = StandardBgpRouterNode::createOriginatedRoute(prefix);
    updateSelection(prefix.toStdString(), route);
    return route;
}

std::optional<RouteEntry> VersionedPathRouterNode::importRoute(const QString& prefix,
                                                                const PathAttributes& attributes,
                                                                const NeighborConfig& peer)
{
    if (!observePath(prefix, attributes, peer)) return std::nullopt;
    return StandardBgpRouterNode::importRoute(prefix, attributes, peer);
}

std::optional<RouteEntry> VersionedPathRouterNode::importAdvertisedRoute(const QString& prefix,
                                                                          const RouteEntry& route,
                                                                          const NeighborConfig& peer)
{
    // Learning facts precedes ordinary eligibility (including AS-loop checks).
    return StandardBgpRouterNode::importAdvertisedRoute(prefix, route, peer);
}

void VersionedPathRouterNode::updateSelection(const std::string& prefix, const std::optional<RouteEntry>& selected)
{
    auto& state = states_[prefix];
    if (selected == state.selected) return;
    if (state.localVersion == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("versioned-path counter exhausted; refusing to reuse an immutable state");
    ++state.localVersion;
    state.selected = selected;
    state.changed = true;
    state.graph.advance(creator_, state.localVersion);
    if (selected)
    {
        std::vector<VersionedState> declaration{{creator_, state.localVersion}};
        if (selected->attributes.versionedPath)
        {
            const auto& suffix = selected->attributes.versionedPath->path;
            declaration.insert(declaration.end(), suffix.begin(), suffix.end());
        }
        if (!state.graph.observe(declaration)) throw std::logic_error("local version suffix was reused");
    }
    if (mode_ != "path_only" && state.localVersion > 1)
        learnRecord({1, 1, prefix, StateRecordKind::RetireBefore, {creator_, state.localVersion}});
}

std::optional<RouteEntry> VersionedPathRouterNode::selectBestRoute(const QString& prefix,
    const QVector<RouteEntry>& candidates, const std::optional<RouteEntry>& current)
{
    auto& state = states_[prefix.toStdString()];
    QVector<RouteEntry> eligible;
    std::set<std::string> peers;
    std::optional<RouteEntry> refreshedCurrent;
    for (const auto& candidate : candidates)
    {
        if (!candidate.localOrigin)
        {
            if (!candidate.attributes.versionedPath) continue;
            const auto declaration = declarationPath(*candidate.attributes.versionedPath);
            peers.insert(candidate.learnedFrom.toStdString());
            state.graph.setCandidate(candidate.learnedFrom.toStdString(), declaration);
            if (state.graph.invalid(declaration.front()))
            {
                state.rejectedCandidates.emplace(candidate.learnedFrom.toStdString(), declaration.front());
                continue;
            }
        }
        eligible.push_back(candidate);
        if (current && classicEquivalent(candidate, *current)) refreshedCurrent = candidate;
    }
    state.graph.retainCandidates(peers);
    auto selected = StandardBgpRouterNode::selectBestRoute(prefix, eligible, refreshedCurrent);
    updateSelection(prefix.toStdString(), selected);
    return selected;
}

std::optional<RouteEntry> VersionedPathRouterNode::exportRoute(const RouteEntry& route, const NeighborConfig& peer)
{
    return StandardBgpRouterNode::exportRoute(route, peer);
}

std::optional<RouteEntry> VersionedPathRouterNode::exportRouteForPrefix(const QString& prefix,
                                                                        const RouteEntry& route,
                                                                        const NeighborConfig& peer)
{
    auto exported = StandardBgpRouterNode::exportRouteForPrefix(prefix, route, peer);
    if (!exported) return std::nullopt;
    auto& state = states_[prefix.toStdString()];
    if (!state.localVersion) updateSelection(prefix.toStdString(), route);
    VersionedPath info{{creator_, state.localVersion}, {}};
    if (route.attributes.versionedPath) info.path = route.attributes.versionedPath->path;
    if (peer.sessionType == SessionType::Ebgp) info.path.insert(info.path.begin(), info.advertiser);
    exported->attributes.versionedPath = std::move(info);
    return exported;
}

bool VersionedPathRouterNode::requiresDissemination(const QString& prefix) const
{
    const auto found = states_.find(prefix.toStdString());
    return found != states_.end() && found->second.changed;
}

void VersionedPathRouterNode::decisionCompleted(const QString& prefix)
{
    auto& state = states_[prefix.toStdString()];
    state.changed = false;
    state.pending.clear();
}

void VersionedPathRouterNode::routerStateChanged(bool enabled)
{
    if (enabled) return;
    for (auto& [prefix, state] : states_)
    {
        updateSelection(prefix, std::nullopt);
        state.graph.retainCandidates({});
    }
}

void VersionedPathRouterNode::peerStateChanged(const NeighborConfig& peer, PeerState state)
{
    if (state == PeerState::Established) return;
    sentRetirements_.erase(peer.id.toStdString());
    sentRevocations_.erase(peer.id.toStdString());
    for (auto& [prefix, stored] : states_)
    {
        (void)prefix;
        stored.graph.removeCandidate(peer.id.toStdString());
    }
}

bool VersionedPathRouterNode::learnRecord(const StateRecord& record)
{
    auto& state = states_[record.prefix];
    if (record.kind == StateRecordKind::RetireBefore)
    {
        auto& known = state.retirements[record.state.creator];
        // Dedup explicit facts separately from highest versions learned from
        // paths: knowing version 8 does NOT mean its event was disseminated.
        if (known >= record.state.version) return false;
        known = record.state.version;
        state.graph.advance(record.state.creator, record.state.version);
    }
    else
    {
        if (!state.revocations.insert(record.state).second) return false;
        state.graph.revoke(record.state);
    }
    ++learnedRecords_;
    state.pending.push_back(record);
    return true;
}

QSet<QString> VersionedPathRouterNode::observeStateRecords(const std::vector<StateRecord>& records,
                                                                    const NeighborConfig&)
{
    QSet<QString> changed;
    if (mode_ == "path_only") return changed;
    for (const auto& record : records)
    {
        if (record.afi != 1 || record.safi != 1 || !isCanonicalIpv4Prefix(QString::fromStdString(record.prefix)) ||
            !validCreator(record.state.creator) || record.state.version == 0 ||
            (record.state.creator == creator_ && record.state.version > states_[record.prefix].localVersion))
        {
            ++rejectedAttributes_;
            continue;
        }
        if (learnRecord(record)) changed.insert(QString::fromStdString(record.prefix));
    }
    return changed;
}

std::vector<StateRecord> VersionedPathRouterNode::withdrawalRecords(const QString& prefix) const
{
    const auto found = states_.find(prefix.toStdString());
    if (found == states_.end()) return {};
    auto records = found->second.pending;
    if (mode_ == "local")
        std::erase_if(records, [&](const auto& record) { return record.state.creator != creator_; });
    return records;
}

std::vector<StateRecord> VersionedPathRouterNode::takeStateRecords(const NeighborConfig& peer)
{
    std::vector<StateRecord> result;
    if (mode_ == "path_only") return result;
    for (const auto& [prefix, state] : states_)
    {
        for (const auto& [creator, version] : state.retirements)
        {
            if (mode_ == "local" && creator != creator_) continue;
            auto& sent = sentRetirements_[peer.id.toStdString()][{prefix, creator}];
            if (sent >= version) continue;
            sent = version;
            result.push_back({1, 1, prefix, StateRecordKind::RetireBefore, {creator, version}});
        }
        for (const auto& revoked : state.revocations)
        {
            if (mode_ == "local" && revoked.creator != creator_) continue;
            const StateRecord record{1, 1, prefix, StateRecordKind::Revoke, revoked};
            if (sentRevocations_[peer.id.toStdString()].insert(record).second) result.push_back(record);
        }
    }
    return result;
}

Json VersionedPathRouterNode::diagnostics() const
{
    Json prefixes = Json::object();
    for (const auto& [prefix, state] : states_)
    {
        auto graph = state.graph.snapshot();
        graph["local_version"] = state.localVersion;
        graph["has_route"] = state.selected.has_value();
        graph["rejected_candidate_instances"] = state.rejectedCandidates.size();
        prefixes[prefix] = std::move(graph);
    }
    return {{"kind", "versioned-path"}, {"mode", mode_}, {"prefixes", prefixes},
            {"rejected_attributes", rejectedAttributes_}, {"learned_records", learnedRecords_}};
}

RouterPluginMetadata VersionedPathRouterPlugin::metadata() const
{
    return {VersionedPathRouterPluginId, QStringLiteral("版本化路径依赖图路由器"), QStringLiteral("0.2.0"),
            QStringLiteral("有序版本路径、不可变依赖后缀、版本化撤销与退役事实传播。"),
            RouterPluginApiVersion, {{QStringLiteral("mode"), QStringLiteral("full")}}};
}

RouterNode* VersionedPathRouterPlugin::createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error)
{
    if (error) error->clear();
    return new VersionedPathRouterNode(context, parent);
}

} // namespace bgptester

BGPTESTER_REGISTER_ROUTER_PLUGIN(bgptester::VersionedPathRouterPlugin)
