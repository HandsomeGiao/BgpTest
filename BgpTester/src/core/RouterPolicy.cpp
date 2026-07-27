#include "core/RouterPolicy.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <tuple>
#include <utility>

namespace bgptester
{
namespace
{

constexpr std::uint32_t ProviderLocalPreference = 50;
constexpr std::uint32_t DefaultLocalPreference = 100;
constexpr std::uint32_t PeerLocalPreference = DefaultLocalPreference;
constexpr std::uint32_t CustomerLocalPreference = 200;
constexpr double MaxExactJsonInteger = 9007199254740991.0;

std::string trimAscii(std::string value)
{
    const auto isSpace = [](unsigned char character)
    {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n' || character == '\f' ||
               character == '\v';
    };
    const auto first = std::find_if_not(value.begin(), value.end(), [&](char character) { return isSpace(character); });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [&](char character) { return isSpace(character); }).base();
    if (first >= last)
    {
        return {};
    }
    return std::string(first, last);
}

const Json* setting(const Json& settings, std::string_view key)
{
    if (!settings.is_object())
    {
        return nullptr;
    }
    const auto it = settings.find(std::string(key));
    return it == settings.end() ? nullptr : &*it;
}

bool booleanSetting(const Json& settings, std::string_view key, bool fallback)
{
    const auto* value = setting(settings, key);
    return value && value->is_boolean() ? value->get<bool>() : fallback;
}

std::optional<std::uint64_t> versionFromJson(const Json* value)
{
    if (!value || value->is_null())
    {
        return 0;
    }
    try
    {
        if (value->is_string())
        {
            const auto text = trimAscii(value->get<std::string>());
            std::uint64_t parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            return error == std::errc{} && end == text.data() + text.size() ? std::optional<std::uint64_t>(parsed) : std::nullopt;
        }
        if (value->is_number_unsigned())
        {
            return value->get<std::uint64_t>();
        }
        if (value->is_number_integer())
        {
            const auto number = value->get<std::int64_t>();
            return number >= 0 ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(number)) : std::nullopt;
        }
        if (value->is_number_float())
        {
            const auto number = value->get<double>();
            if (number >= 0.0 && number <= MaxExactJsonInteger && std::floor(number) == number)
            {
                return static_cast<std::uint64_t>(number);
            }
        }
    }
    catch (const std::exception&)
    {
    }
    return std::nullopt;
}

std::string configuredEntityId(const RouterPolicyContext& context)
{
    const auto* configured = setting(context.config.pluginSettings, "entity_id");
    if (configured && configured->is_string())
    {
        const auto result = trimAscii(configured->get<std::string>());
        if (!result.empty())
        {
            return result;
        }
    }
    return context.config.routerId;
}

bool vectorContains(const std::vector<std::uint32_t>& values, std::uint32_t value)
{
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
}

bool stringVectorContains(const std::vector<std::string>& values, std::string_view value)
{
    return std::find(values.cbegin(), values.cend(), value) != values.cend();
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

class StandardRouterPolicy : public RouterPolicy
{
public:
    using RouterPolicy::RouterPolicy;

    RouteEntry createOriginatedRoute(std::string_view) override
    {
        RouteEntry route;
        route.attributes.nextHop = context().config.routerId;
        route.learnedFrom = context().config.id;
        route.localOrigin = true;
        route.source = RouteSource::Local;
        return route;
    }

    std::optional<RouteEntry> importRoute(std::string_view, const PathAttributes& attributes,
                                          const NeighborConfig& fromPeer) override
    {
        if (vectorContains(attributes.asPath, context().config.asn))
        {
            return std::nullopt;
        }
        if (!attributes.originatorId.empty() && attributes.originatorId == context().config.routerId)
        {
            return std::nullopt;
        }
        const auto& clusterId = context().config.clusterId.empty() ? context().config.routerId : context().config.clusterId;
        if (stringVectorContains(attributes.clusterList, clusterId))
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

    std::optional<RouteEntry> importAdvertisedRoute(std::string_view prefix, const RouteEntry& advertisedRoute,
                                                     const NeighborConfig& fromPeer) override
    {
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

    std::optional<RouteEntry> selectBestRoute(std::string_view, const std::vector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>& currentBest) override
    {
        if (candidates.empty())
        {
            return std::nullopt;
        }
        RouteEntry primaryBest = candidates.front();
        for (const auto& candidate : candidates)
        {
            if (primaryRouteBetter(candidate, primaryBest))
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
            if (samePrimaryPreference(candidate, primaryBest) && deterministicRouteBetter(candidate, result))
            {
                result = candidate;
            }
        }
        return result;
    }

    std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) override
    {
        if (route.learnedFrom == toPeer.id)
        {
            return std::nullopt;
        }
        bool allowed = toPeer.sessionType == SessionType::Ebgp || route.localOrigin || route.sourceSession == SessionType::Ebgp;
        if (!allowed && hasRouteReflectorClients())
        {
            const auto learnedPeer = context().neighbors.find(route.learnedFrom);
            const auto learnedFromClient = learnedPeer != context().neighbors.end() && learnedPeer->second.rrClient;
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

        auto result = route;
        result.learnedFrom = context().config.id;
        result.localOrigin = false;
        result.sourceSession = toPeer.sessionType;
        if (toPeer.sessionType == SessionType::Ebgp)
        {
            result.attributes.asPath.insert(result.attributes.asPath.begin(), context().config.asn);
            result.attributes.nextHop = context().config.routerId;
            result.attributes.localPref = DefaultLocalPreference;
            result.attributes.originatorId.clear();
            result.attributes.clusterList.clear();
            result.source = RouteSource::Unspecified;
            return result;
        }

        if (result.attributes.nextHop.empty())
        {
            result.attributes.nextHop = context().config.routerId;
        }
        if (route.sourceSession == SessionType::Ibgp && !route.localOrigin && hasRouteReflectorClients())
        {
            if (result.attributes.originatorId.empty())
            {
                const auto learned = context().topologyRouters ? context().topologyRouters->find(route.learnedFrom)
                                                               : RouterConfigMap::const_iterator{};
                result.attributes.originatorId = !context().topologyRouters || learned == context().topologyRouters->end()
                                                     ? route.attributes.nextHop
                                                     : learned->second.routerId;
            }
            const auto& clusterId = context().config.clusterId.empty() ? context().config.routerId : context().config.clusterId;
            if (!stringVectorContains(result.attributes.clusterList, clusterId))
            {
                result.attributes.clusterList.push_back(clusterId);
            }
        }
        return result;
    }

protected:
    static bool primaryRouteBetter(const RouteEntry& lhs, const RouteEntry& rhs)
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

    static bool samePrimaryPreference(const RouteEntry& lhs, const RouteEntry& rhs)
    {
        return lhs.localOrigin == rhs.localOrigin && lhs.attributes.localPref == rhs.attributes.localPref &&
               lhs.attributes.asPath.size() == rhs.attributes.asPath.size() && lhs.attributes.med == rhs.attributes.med &&
               lhs.sourceSession == rhs.sourceSession;
    }

    static bool deterministicRouteBetter(const RouteEntry& lhs, const RouteEntry& rhs)
    {
        return std::tie(lhs.attributes.nextHop, lhs.learnedFrom) < std::tie(rhs.attributes.nextHop, rhs.learnedFrom);
    }

private:
    bool hasRouteReflectorClients() const
    {
        return std::any_of(context().neighbors.cbegin(), context().neighbors.cend(),
                           [](const auto& entry) { return entry.second.rrClient; });
    }
};

class ConfigurableExportRouterPolicy final : public RouterPolicy
{
public:
    using RouterPolicy::RouterPolicy;

    std::vector<std::string> validateConfiguration() const override
    {
        std::vector<std::string> problems;
        const auto& settings = context().config.pluginSettings;
        if (const auto* value = setting(settings, "export_routes"); value && !value->is_boolean())
        {
            problems.emplace_back("export_routes 必须是布尔值");
        }
        if (const auto* value = setting(settings, "local_preference"))
        {
            bool valid = false;
            try
            {
                if (value->is_number_unsigned())
                {
                    valid = value->get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max();
                }
                else if (value->is_number_integer())
                {
                    const auto number = value->get<std::int64_t>();
                    valid = number >= 0 && static_cast<std::uint64_t>(number) <= std::numeric_limits<std::uint32_t>::max();
                }
                else if (value->is_number_float())
                {
                    const auto number = value->get<double>();
                    valid = number >= 0.0 && number <= std::numeric_limits<std::uint32_t>::max() && std::floor(number) == number;
                }
            }
            catch (const std::exception&)
            {
                valid = false;
            }
            if (!valid)
            {
                problems.emplace_back("local_preference 必须是 0..4294967295 的整数");
            }
        }
        return problems;
    }

    RouteEntry createOriginatedRoute(std::string_view) override
    {
        RouteEntry route;
        route.attributes.nextHop = context().config.routerId;
        if (const auto* value = setting(context().config.pluginSettings, "local_preference"))
        {
            try
            {
                route.attributes.localPref = value->get<std::uint32_t>();
            }
            catch (const std::exception&)
            {
                route.attributes.localPref = DefaultLocalPreference;
            }
        }
        route.learnedFrom = context().config.id;
        route.localOrigin = true;
        return route;
    }

    std::optional<RouteEntry> importRoute(std::string_view, const PathAttributes& attributes,
                                          const NeighborConfig& fromPeer) override
    {
        if (vectorContains(attributes.asPath, context().config.asn))
        {
            return std::nullopt;
        }
        RouteEntry route;
        route.attributes = attributes;
        route.learnedFrom = fromPeer.id;
        route.sourceSession = fromPeer.sessionType;
        return route;
    }

    std::optional<RouteEntry> selectBestRoute(std::string_view, const std::vector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>&) override
    {
        if (candidates.empty())
        {
            return std::nullopt;
        }
        return *std::min_element(candidates.cbegin(), candidates.cend(), [](const RouteEntry& lhs, const RouteEntry& rhs)
        {
            return std::tuple(!lhs.localOrigin, std::numeric_limits<std::uint32_t>::max() - lhs.attributes.localPref,
                              lhs.attributes.asPath.size(), lhs.learnedFrom) <
                   std::tuple(!rhs.localOrigin, std::numeric_limits<std::uint32_t>::max() - rhs.attributes.localPref,
                              rhs.attributes.asPath.size(), rhs.learnedFrom);
        });
    }

    std::optional<RouteEntry> exportRoute(const RouteEntry& route, const NeighborConfig& toPeer) override
    {
        if (!booleanSetting(context().config.pluginSettings, "export_routes", false) || route.learnedFrom == toPeer.id)
        {
            return std::nullopt;
        }
        if (toPeer.sessionType == SessionType::Ibgp && !route.localOrigin && route.sourceSession == SessionType::Ibgp)
        {
            return std::nullopt;
        }
        auto exported = route;
        exported.learnedFrom = context().config.id;
        exported.localOrigin = false;
        exported.sourceSession = toPeer.sessionType;
        if (toPeer.sessionType == SessionType::Ebgp)
        {
            exported.attributes.asPath.insert(exported.attributes.asPath.begin(), context().config.asn);
            exported.attributes.nextHop = context().config.routerId;
        }
        return exported;
    }
};

bool classicEquivalent(const RouteEntry& lhs, const RouteEntry& rhs)
{
    const auto& left = lhs.attributes;
    const auto& right = rhs.attributes;
    return lhs.learnedFrom == rhs.learnedFrom && lhs.sourceSession == rhs.sourceSession && lhs.localOrigin == rhs.localOrigin &&
           lhs.source == rhs.source && left.origin == right.origin && left.asPath == right.asPath && left.nextHop == right.nextHop &&
           left.localPref == right.localPref && left.med == right.med && left.originatorId == right.originatorId &&
           left.clusterList == right.clusterList && left.communities == right.communities;
}

bool triggerInvalidates(const RouteEntry& route, const TfpVersionVector& triggers)
{
    if (!route.attributes.tfpVersionInfo)
    {
        return false;
    }
    const auto& dependencies = route.attributes.tfpVersionInfo->dependencyVector;
    for (const auto& [entity, version] : triggers)
    {
        const auto dependency = dependencies.find(entity);
        if (dependency != dependencies.end() && dependency->second < version)
        {
            return true;
        }
    }
    return false;
}

struct PrefixVersionState
{
    struct TriggerKnowledge
    {
        std::uint64_t staleVersion = 0;
        std::uint64_t seenVersion = 0;
        bool triggerSeen = false;
    };

    std::uint64_t localVersion = 0;
    std::uint64_t localSeenTriggerVersion = 0;
    bool advancedForPendingDecision = false;
    bool staleCheckNeeded = false;
    std::map<TfpEntity, std::uint64_t> maxVersions;
    std::map<TfpEntity, TriggerKnowledge> triggerKnowledge;
    TfpVersionVector pendingTriggers;
};

class TfpVersionRouterPolicy final : public StandardRouterPolicy
{
public:
    explicit TfpVersionRouterPolicy(RouterPolicyContext context)
        : StandardRouterPolicy(std::move(context)), localEntity_{this->context().config.asn, configuredEntityId(this->context())},
          initialVersion_(versionFromJson(setting(this->context().config.pluginSettings, "initial_version")).value_or(0))
    {
    }

    std::vector<std::string> validateConfiguration() const override
    {
        auto problems = StandardRouterPolicy::validateConfiguration();
        const auto* entity = setting(context().config.pluginSettings, "entity_id");
        if (entity && !entity->is_string())
        {
            problems.emplace_back("entity_id 必须是字符串");
        }
        const auto parsed = versionFromJson(setting(context().config.pluginSettings, "initial_version"));
        if (!parsed || *parsed == std::numeric_limits<std::uint64_t>::max())
        {
            problems.emplace_back("initial_version 必须是小于 18446744073709551615 的非负整数或十进制字符串");
        }
        return problems;
    }

    void convergenceStateChanged(bool converged) override
    {
        if (converged)
        {
            bootstrapComplete_ = true;
        }
    }

    RouteEntry createOriginatedRoute(std::string_view prefix) override
    {
        auto route = StandardRouterPolicy::createOriginatedRoute(prefix);
        auto& state = stateFor(prefix);
        if (bootstrapComplete_)
        {
            advanceLocalVersion(state);
            state.advancedForPendingDecision = true;
        }
        return route;
    }

    std::optional<RouteEntry> importRoute(std::string_view prefix, const PathAttributes& attributes,
                                          const NeighborConfig& fromPeer) override
    {
        auto imported = StandardRouterPolicy::importRoute(prefix, attributes, fromPeer);
        if (imported)
        {
            observeVersionInfo(prefix, attributes);
            if (imported->attributes.tfpVersionInfo)
            {
                imported->attributes.tfpVersionInfo->triggerVector.clear();
            }
        }
        return imported;
    }

    void importWithdrawal(std::string_view prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer) override
    {
        StandardRouterPolicy::importWithdrawal(prefix, attributes, fromPeer);
        observeVersionInfo(prefix, attributes);
    }

    void localRouteWithdrawn(std::string_view prefix) override
    {
        StandardRouterPolicy::localRouteWithdrawn(prefix);
        auto& state = stateFor(prefix);
        if (bootstrapComplete_)
        {
            advanceLocalVersion(state);
            state.advancedForPendingDecision = true;
        }
    }

    std::optional<RouteEntry> selectBestRoute(std::string_view prefix, const std::vector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>& currentBest) override
    {
        auto& state = stateFor(prefix);
        decisionPrefix_ = prefix;
        decisionState_ = &state;
        decisionExportVersionInfo_.reset();
        const RouteEntry* primaryBest = nullptr;
        const RouteEntry* deterministicBest = nullptr;
        const RouteEntry* refreshedCurrent = nullptr;
        for (const auto& candidate : candidates)
        {
            if (state.staleCheckNeeded && isStale(candidate, state))
            {
                continue;
            }
            if (!primaryBest || primaryRouteBetter(candidate, *primaryBest))
            {
                primaryBest = &candidate;
                deterministicBest = &candidate;
            }
            else if (samePrimaryPreference(candidate, *primaryBest) && deterministicRouteBetter(candidate, *deterministicBest))
            {
                deterministicBest = &candidate;
            }
            if (currentBest && !refreshedCurrent && classicEquivalent(candidate, *currentBest))
            {
                refreshedCurrent = &candidate;
            }
        }
        const RouteEntry* selected = nullptr;
        bool preservesCurrent = false;
        if (primaryBest)
        {
            preservesCurrent = currentBest && refreshedCurrent && samePrimaryPreference(*refreshedCurrent, *primaryBest);
            selected = preservesCurrent ? refreshedCurrent : deterministicBest;
        }
        const auto changed = currentBest ? !preservesCurrent : selected != nullptr;
        if (bootstrapComplete_ && changed && !state.advancedForPendingDecision)
        {
            advanceLocalVersion(state, !currentBest || !triggerInvalidates(*currentBest, state.pendingTriggers));
        }
        return selected ? std::optional<RouteEntry>(*selected) : std::nullopt;
    }

    bool requiresDissemination(std::string_view prefix) const override
    {
        if (decisionState_ && decisionPrefix_ == prefix)
        {
            return !decisionState_->pendingTriggers.empty();
        }
        const auto state = states_.find(std::string(prefix));
        return state != states_.end() && !state->second.pendingTriggers.empty();
    }

    void decisionCompleted(std::string_view prefix) override
    {
        PrefixVersionState* state = nullptr;
        if (decisionState_ && decisionPrefix_ == prefix)
        {
            state = decisionState_;
        }
        else if (const auto stored = states_.find(std::string(prefix)); stored != states_.end())
        {
            state = &stored->second;
        }
        if (state)
        {
            state->advancedForPendingDecision = false;
            state->pendingTriggers.clear();
        }
        if (decisionPrefix_ == prefix)
        {
            decisionState_ = nullptr;
            decisionPrefix_.clear();
            decisionExportVersionInfo_.reset();
        }
    }

    std::optional<RouteEntry> exportRouteForPrefix(std::string_view prefix, const RouteEntry& route,
                                                    const NeighborConfig& toPeer) override
    {
        auto exported = StandardRouterPolicy::exportRouteForPrefix(prefix, route, toPeer);
        if (!exported)
        {
            return std::nullopt;
        }
        auto& state = decisionState_ && decisionPrefix_ == prefix ? *decisionState_ : stateFor(prefix);
        const auto buildVersionInfo = [&]
        {
            TfpVersionInfo info;
            if (route.attributes.tfpVersionInfo)
            {
                info.dependencyVector = route.attributes.tfpVersionInfo->dependencyVector;
            }
            info.dependencyVector.insert_or_assign(localEntity_, state.localVersion);
            info.triggerVector = state.pendingTriggers;
            return info;
        };
        if (decisionState_ && decisionPrefix_ == prefix)
        {
            if (!decisionExportVersionInfo_)
            {
                decisionExportVersionInfo_ = buildVersionInfo();
            }
            exported->attributes.tfpVersionInfo = *decisionExportVersionInfo_;
        }
        else
        {
            exported->attributes.tfpVersionInfo = buildVersionInfo();
        }
        return exported;
    }

    PathAttributes exportWithdrawal(std::string_view prefix, const NeighborConfig& toPeer) override
    {
        auto attributes = StandardRouterPolicy::exportWithdrawal(prefix, toPeer);
        auto& state = decisionState_ && decisionPrefix_ == prefix ? *decisionState_ : stateFor(prefix);
        if (!state.pendingTriggers.empty())
        {
            TfpVersionInfo info;
            info.triggerVector = state.pendingTriggers;
            attributes.tfpVersionInfo = std::move(info);
        }
        return attributes;
    }

private:
    PrefixVersionState& stateFor(std::string_view prefix)
    {
        if (cachedState_ && cachedPrefix_ == prefix)
        {
            return *cachedState_;
        }
        const auto [it, inserted] = states_.try_emplace(std::string(prefix));
        if (inserted)
        {
            it->second.localVersion = initialVersion_;
            it->second.localSeenTriggerVersion = initialVersion_;
            it->second.staleCheckNeeded = initialVersion_ > 0;
        }
        cachedPrefix_ = prefix;
        cachedState_ = &it->second;
        return *cachedState_;
    }

    bool isStale(const RouteEntry& route, const PrefixVersionState& state) const
    {
        if (!route.attributes.tfpVersionInfo)
        {
            return false;
        }
        const auto& dependencies = route.attributes.tfpVersionInfo->dependencyVector;
        for (const auto& [entity, knowledge] : state.triggerKnowledge)
        {
            if (knowledge.staleVersion == 0)
            {
                continue;
            }
            const auto dependency = dependencies.find(entity);
            if (dependency != dependencies.end() && dependency->second < knowledge.staleVersion)
            {
                return true;
            }
        }
        if (state.localVersion != 0)
        {
            const auto local = dependencies.find(localEntity_);
            if (local != dependencies.end() && local->second < state.localVersion)
            {
                return true;
            }
        }
        return false;
    }

    void observeVersionInfo(std::string_view prefix, const PathAttributes& attributes)
    {
        if (!attributes.tfpVersionInfo)
        {
            return;
        }
        auto& state = decisionState_ && decisionPrefix_ == prefix ? *decisionState_ : stateFor(prefix);
        const auto mergeKnown = [&](const TfpVersionVector& versions)
        {
            for (const auto& [entity, version] : versions)
            {
                if (entity.asn == 0 || entity.entityId.empty())
                {
                    continue;
                }
                if (entity == localEntity_)
                {
                    if (state.localVersion < version)
                    {
                        state.localVersion = version;
                        state.staleCheckNeeded = true;
                    }
                    continue;
                }
                const auto known = state.maxVersions.find(entity);
                if (known == state.maxVersions.end())
                {
                    if (version == 0)
                    {
                        continue;
                    }
                    state.maxVersions.emplace(entity, version);
                    auto& knowledge = state.triggerKnowledge[entity];
                    knowledge.staleVersion = std::max(knowledge.staleVersion, version);
                    state.staleCheckNeeded = true;
                }
                else if (known->second != version)
                {
                    const auto effective = std::max(known->second, version);
                    known->second = effective;
                    auto& knowledge = state.triggerKnowledge[entity];
                    knowledge.staleVersion = std::max(knowledge.staleVersion, effective);
                    state.staleCheckNeeded = true;
                }
            }
        };
        mergeKnown(attributes.tfpVersionInfo->dependencyVector);
        for (const auto& [entity, version] : attributes.tfpVersionInfo->triggerVector)
        {
            if (entity.asn == 0 || entity.entityId.empty())
            {
                continue;
            }
            if (entity == localEntity_)
            {
                if (state.localSeenTriggerVersion >= version)
                {
                    continue;
                }
                state.localVersion = std::max(state.localVersion, version);
                state.localSeenTriggerVersion = version;
                state.staleCheckNeeded = true;
                state.pendingTriggers.insert_or_assign(entity, version);
                continue;
            }
            auto& knowledge = state.triggerKnowledge[entity];
            if (knowledge.triggerSeen && knowledge.seenVersion >= version)
            {
                continue;
            }
            state.staleCheckNeeded = true;
            const auto known = state.maxVersions.find(entity);
            const auto effective = known == state.maxVersions.end() ? version : std::max(known->second, version);
            knowledge.staleVersion = std::max(knowledge.staleVersion, effective);
            knowledge.seenVersion = version;
            knowledge.triggerSeen = true;
            const auto pending = state.pendingTriggers.find(entity);
            if (pending == state.pendingTriggers.end() || pending->second < version)
            {
                state.pendingTriggers.insert_or_assign(entity, version);
            }
        }
    }

    void advanceLocalVersion(PrefixVersionState& state, bool publishTrigger = true)
    {
        if (state.localVersion == std::numeric_limits<std::uint64_t>::max())
        {
            return;
        }
        ++state.localVersion;
        state.staleCheckNeeded = true;
        if (publishTrigger)
        {
            state.localSeenTriggerVersion = state.localVersion;
            state.pendingTriggers.insert_or_assign(localEntity_, state.localVersion);
        }
    }

    TfpEntity localEntity_;
    std::uint64_t initialVersion_ = 0;
    bool bootstrapComplete_ = false;
    std::map<std::string, PrefixVersionState, std::less<>> states_;
    std::string decisionPrefix_;
    PrefixVersionState* decisionState_ = nullptr;
    std::optional<TfpVersionInfo> decisionExportVersionInfo_;
    std::string cachedPrefix_;
    PrefixVersionState* cachedState_ = nullptr;
};

RouterPolicyMetadata standardMetadata()
{
    return {std::string(StandardRouterPolicyId), "标准 BGP 路由器", "1.1.0",
            "内置 RFC 风格 BGP 节点，支持 EBGP、IBGP、路由反射、MRAI 与商业关系策略。", RouterPolicyApiVersion,
            Json::object()};
}

RouterPolicyMetadata configurableMetadata()
{
    return {std::string(ConfigurableExportRouterPolicyId), "示例：可配置出口路由器", "1.0.0",
            "演示自定义选路与出口控制；export_routes 决定是否发布路由。", RouterPolicyApiVersion,
            Json{{"export_routes", false}, {"local_preference", 100}}};
}

RouterPolicyMetadata tfpMetadata()
{
    return {std::string(TfpVersionRouterPolicyId), "TFP 路径版本路由器", "1.2.0",
            "基于路由器级实体、依赖向量和触发向量，提前排除必然失效的旧路径。", RouterPolicyApiVersion,
            Json{{"entity_id", ""}, {"initial_version", "0"}}};
}

} // namespace

RouterPolicy::RouterPolicy(RouterPolicyContext context) : context_(std::move(context))
{
}

const RouterPolicyContext& RouterPolicy::context() const noexcept
{
    return context_;
}

std::vector<std::string> RouterPolicy::validateConfiguration() const
{
    return {};
}

void RouterPolicy::simulationStarted()
{
}

void RouterPolicy::simulationStopped()
{
}

void RouterPolicy::routerStateChanged(bool)
{
}

void RouterPolicy::convergenceStateChanged(bool)
{
}

void RouterPolicy::peerStateChanged(const NeighborConfig&, PeerState)
{
}

std::optional<RouteEntry> RouterPolicy::importAdvertisedRoute(std::string_view prefix, const RouteEntry& advertisedRoute,
                                                               const NeighborConfig& fromPeer)
{
    return importRoute(prefix, advertisedRoute.attributes, fromPeer);
}

void RouterPolicy::importWithdrawal(std::string_view, const PathAttributes&, const NeighborConfig&)
{
}

void RouterPolicy::localRouteWithdrawn(std::string_view)
{
}

bool RouterPolicy::requiresDissemination(std::string_view) const
{
    return false;
}

void RouterPolicy::decisionCompleted(std::string_view)
{
}

std::optional<RouteEntry> RouterPolicy::exportRouteForPrefix(std::string_view, const RouteEntry& route,
                                                              const NeighborConfig& toPeer)
{
    return exportRoute(route, toPeer);
}

PathAttributes RouterPolicy::exportWithdrawal(std::string_view, const NeighborConfig&)
{
    return {};
}

RouterPolicyRegistry& RouterPolicyRegistry::instance()
{
    static RouterPolicyRegistry registry;
    return registry;
}

RouterPolicyRegistry::RouterPolicyRegistry()
{
    std::string error;
    if (!registerPolicy(standardMetadata(),
                        [](RouterPolicyContext context, std::string*)
                        { return std::make_unique<StandardRouterPolicy>(std::move(context)); },
                        "builtin:standard", &error))
    {
        registrationErrors_.push_back(error);
    }
    if (!registerPolicy(configurableMetadata(),
                        [](RouterPolicyContext context, std::string*)
                        { return std::make_unique<ConfigurableExportRouterPolicy>(std::move(context)); },
                        "builtin:configurable-export", &error))
    {
        registrationErrors_.push_back(error);
    }
    if (!registerPolicy(tfpMetadata(),
                        [](RouterPolicyContext context, std::string*)
                        { return std::make_unique<TfpVersionRouterPolicy>(std::move(context)); },
                        "builtin:tfp-version", &error))
    {
        registrationErrors_.push_back(error);
    }
}

std::vector<RegisteredRouterPolicy> RouterPolicyRegistry::policies() const
{
    std::shared_lock lock(mutex_);
    std::vector<RegisteredRouterPolicy> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_)
    {
        (void)id;
        result.push_back({entry.metadata, entry.source});
    }
    return result;
}

std::optional<RouterPolicyMetadata> RouterPolicyRegistry::metadata(std::string_view policyId) const
{
    std::shared_lock lock(mutex_);
    const auto it = entries_.find(policyId);
    return it == entries_.end() ? std::nullopt : std::optional<RouterPolicyMetadata>(it->second.metadata);
}

bool RouterPolicyRegistry::contains(std::string_view policyId) const
{
    std::shared_lock lock(mutex_);
    return entries_.contains(policyId);
}

std::vector<std::string> RouterPolicyRegistry::registrationErrors() const
{
    std::shared_lock lock(mutex_);
    return registrationErrors_;
}

bool RouterPolicyRegistry::registerPolicy(RouterPolicyMetadata metadata, RouterPolicyFactory factory, std::string source,
                                          std::string* error)
{
    if (error)
    {
        error->clear();
    }
    metadata.id = trimAscii(std::move(metadata.id));
    metadata.displayName = trimAscii(std::move(metadata.displayName));
    if (!factory)
    {
        if (error)
        {
            *error = "路由器策略工厂为空";
        }
        return false;
    }
    if (metadata.id.empty() || metadata.displayName.empty())
    {
        if (error)
        {
            *error = source + " 的策略 ID 或显示名称为空";
        }
        return false;
    }
    if (metadata.apiVersion != RouterPolicyApiVersion)
    {
        if (error)
        {
            *error = "策略 " + metadata.id + " 的 API 版本不兼容";
        }
        return false;
    }
    std::unique_lock lock(mutex_);
    if (entries_.contains(metadata.id))
    {
        if (error)
        {
            *error = "路由器策略 ID 重复：" + metadata.id;
        }
        return false;
    }
    const auto id = metadata.id;
    entries_.emplace(id, Entry{std::move(metadata), std::move(factory), std::move(source)});
    return true;
}

std::unique_ptr<RouterPolicy> RouterPolicyRegistry::createRouterPolicy(RouterPolicyContext context, std::string* error) const
{
    if (error)
    {
        error->clear();
    }
    RouterPolicyFactory factory;
    {
        std::shared_lock lock(mutex_);
        const auto it = entries_.find(context.config.pluginId);
        if (it == entries_.end())
        {
            if (error)
            {
                *error = "路由器 " + context.config.id + " 使用的策略未注册：" + context.config.pluginId;
            }
            return nullptr;
        }
        factory = it->second.factory;
    }
    try
    {
        auto policy = factory(std::move(context), error);
        if (!policy && error && error->empty())
        {
            *error = "路由器策略工厂未能创建实例";
        }
        return policy;
    }
    catch (const std::exception& exception)
    {
        if (error)
        {
            *error = std::string("创建路由器策略时发生异常：") + exception.what();
        }
    }
    catch (...)
    {
        if (error)
        {
            *error = "创建路由器策略时发生未知异常";
        }
    }
    return nullptr;
}

} // namespace bgptester
