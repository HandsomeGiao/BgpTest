#include "router_plugins/versioned_path/DependencyGraph.hpp"

#include <algorithm>
#include <stdexcept>

namespace bgptester
{

std::vector<VersionedState> declarationPath(const VersionedPath& info)
{
    auto path = info.path;
    if (path.empty() || path.front() != info.advertiser)
    {
        path.insert(path.begin(), info.advertiser);
    }
    return path;
}

nlohmann::json stateJson(const VersionedState& state)
{
    return {{"asn", state.creator.asn}, {"router_id", state.creator.routerId}, {"version", state.version}};
}

nlohmann::json versionedPathJson(const VersionedPath& info)
{
    auto path = nlohmann::json::array();
    for (const auto& state : info.path) path.push_back(stateJson(state));
    return {{"advertiser", stateJson(info.advertiser)}, {"path", path}};
}

nlohmann::json stateRecordJson(const StateRecord& record)
{
    return {{"afi", record.afi}, {"safi", record.safi}, {"prefix", record.prefix},
            {"operation", record.kind == StateRecordKind::RetireBefore ? "RETIRE_BEFORE" : "REVOKE_STATE"},
            {"state", stateJson(record.state)}};
}

nlohmann::json withdrawalBindingJson(const WithdrawalBinding& binding)
{
    return {{"advertiser", stateJson(binding.advertiser)},
            {"path_head", binding.pathHead ? stateJson(*binding.pathHead) : nlohmann::json(nullptr)}};
}

bool VersionedDependencyGraph::observe(const std::vector<VersionedState>& path)
{
    std::set<VersionedState> seen;
    // Validate the ENTIRE declaration before learning any version from it.
    for (std::size_t i = 0; i < path.size(); ++i)
    {
        const auto& state = path[i];
        if (state.creator.asn == 0 || state.creator.routerId.empty() || state.version == 0 ||
            !seen.insert(state).second)
        {
            return false;
        }
        const auto successor = i + 1 < path.size() ? std::optional(path[i + 1]) : std::nullopt;
        const auto old = nodes_.find(state);
        if (old != nodes_.end() && old->second.successor != successor) return false;
    }
    for (std::size_t i = path.size(); i > 0; --i)
    {
        const auto& state = path[i - 1];
        const auto successor = i < path.size() ? std::optional(path[i]) : std::nullopt;
        const auto [node, inserted] = nodes_.try_emplace(state, Node{successor, {}, 0, false});
        if (inserted)
        {
            versions_[state.creator].insert(state.version);
            if (successor) nodes_.at(*successor).dependents.insert(state);
        }
        if (revoked_.contains(state) ||
            (highest_.contains(state.creator) && state.version < highest_.at(state.creator)) ||
            (successor && nodes_.at(*successor).poisoned))
        {
            poison(state);
        }
        (void)node;
    }
    for (const auto& state : path) advance(state.creator, state.version);
    return true;
}

bool VersionedDependencyGraph::advance(const StateCreator& creator, std::uint64_t version)
{
    auto& highest = highest_[creator];
    if (version <= highest) return false;
    const auto previous = highest;
    highest = version;
    const auto found = versions_.find(creator);
    if (found != versions_.end())
    {
        for (auto old = found->second.lower_bound(previous); old != found->second.end(); ++old)
        {
            if (*old >= version) break;
            poison({creator, *old});
        }
    }
    return true;
}

bool VersionedDependencyGraph::revoke(const VersionedState& state)
{
    if (!revoked_.insert(state).second) return false;
    poison(state);
    return true;
}

void VersionedDependencyGraph::poison(const VersionedState& state)
{
    std::vector<VersionedState> pending{state};
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        const auto node = nodes_.find(current);
        if (node == nodes_.end() || node->second.poisoned) continue;
        node->second.poisoned = true;
        ++poisonedCount_;
        pending.insert(pending.end(), node->second.dependents.begin(), node->second.dependents.end());
    }
}

bool VersionedDependencyGraph::invalid(const VersionedState& state) const
{
    const auto node = nodes_.find(state);
    const auto highest = highest_.find(state.creator);
    return revoked_.contains(state) || (highest != highest_.end() && state.version < highest->second) ||
           (node != nodes_.end() && node->second.poisoned);
}

void VersionedDependencyGraph::setCandidate(const std::string& peer, const std::vector<VersionedState>& path)
{
    const auto old = candidates_.find(peer);
    if (old != candidates_.end() && old->second == path) return;
    removeCandidate(peer);
    for (const auto& state : path) ++nodes_.at(state).references;
    candidates_.emplace(peer, path);
}

void VersionedDependencyGraph::removeCandidate(const std::string& peer)
{
    const auto old = candidates_.find(peer);
    if (old == candidates_.end()) return;
    for (const auto& state : old->second) --nodes_.at(state).references;
    candidates_.erase(old);
}

void VersionedDependencyGraph::retainCandidates(const std::set<std::string>& peers)
{
    std::vector<std::string> removed;
    for (const auto& [peer, path] : candidates_)
    {
        (void)path;
        if (!peers.contains(peer)) removed.push_back(peer);
    }
    for (const auto& peer : removed) removeCandidate(peer);
}

nlohmann::json VersionedDependencyGraph::snapshot() const
{
    auto nodes = nlohmann::json::array();
    auto candidates = nlohmann::json::array();
    auto highest = nlohmann::json::array();
    std::size_t edges = 0;
    for (const auto& [state, node] : nodes_)
    {
        if (!node.references) continue;
        if (node.successor) ++edges;
        nodes.push_back({{"state", stateJson(state)}, {"references", node.references}, {"poisoned", node.poisoned},
                         {"successor", node.successor ? stateJson(*node.successor) : nlohmann::json(nullptr)}});
    }
    for (const auto& [peer, path] : candidates_)
    {
        candidates.push_back({{"peer", peer}, {"root", stateJson(path.front())},
                              {"eligible_by_version", !invalid(path.front())}});
    }
    for (const auto& [creator, version] : highest_) highest.push_back(stateJson({creator, version}));
    return {{"nodes", nodes}, {"candidates", candidates}, {"highest_versions", highest},
            {"active_nodes", nodes.size()}, {"active_edges", edges}, {"components", nodes.size() - edges},
            {"historical_nodes", nodes_.size()}, {"poisoned_total", poisonedCount_}, {"revoked_states", revoked_.size()}};
}

} // namespace bgptester
