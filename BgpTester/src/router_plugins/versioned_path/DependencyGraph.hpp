#pragma once

#include "engine/VersionedPathAttributes.hpp"

namespace bgptester
{

// One observer, one IPv4-unicast prefix. Immutable historical successors and
// watermarks survive removal of the last current candidate. Reference counts
// describe only the current observation forest.
class VersionedDependencyGraph
{
public:
    bool observe(const std::vector<VersionedState>& path);
    bool advance(const StateCreator& creator, std::uint64_t version);
    bool revoke(const VersionedState& state);
    bool invalid(const VersionedState& state) const;
    void setCandidate(const std::string& peer, const std::vector<VersionedState>& path);
    void removeCandidate(const std::string& peer);
    void retainCandidates(const std::set<std::string>& peers);
    nlohmann::json snapshot() const;
    const std::map<StateCreator, std::uint64_t>& highestVersions() const { return highest_; }

private:
    struct Node
    {
        std::optional<VersionedState> successor;
        std::set<VersionedState> dependents;
        std::size_t references = 0;
        bool poisoned = false;
    };
    void poison(const VersionedState& state);
    std::map<VersionedState, Node> nodes_;
    std::map<StateCreator, std::set<std::uint64_t>> versions_;
    std::map<StateCreator, std::uint64_t> highest_;
    std::set<VersionedState> revoked_;
    std::map<std::string, std::vector<VersionedState>> candidates_;
    std::uint64_t poisonedCount_ = 0;
};

} // namespace bgptester
